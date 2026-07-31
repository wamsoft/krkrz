//---------------------------------------------------------------------------
// TJS Dialog クラス実装 (Phase 6b)
//
// `iTVPDialogEventHandler` を直接 implement して、manager から OnAction が
// 呼ばれたら TVPPostEvent で TJS の `onAction` メソッドを起動する。
//---------------------------------------------------------------------------
#include "tjsCommHead.h"
#include "DialogIntf.h"
#include "ElementsDialogManager.h"
#include "EventIntf.h"
#include "DebugIntf.h"
#include "CharacterSet.h"
#include "tjsDictionary.h"   // TJSCreateDictionaryObject
#include "StoragesResourceLoader.h"   // TVPRegisterElementsFont(Dir)
#include "VariantJsonUtil.h" // TVPVariantToJsonUtf8 (showDict / showModalDict)

// モーダル実行 API (host 非依存の宣言)。 実装は host 別: SDL host =
// SDLElementsModalRunner.cpp (独立 SDL_Window の nested pump)、 WINVER host =
// WinElementsModalRunner.cpp (現状スタブ、 showModal* は未対応で false を返す)。
#include "ElementsModalRunner.h"
#include "StorageIntf.h"     // TVPReadStream (ShowModalFile)

#include <string>

//---------------------------------------------------------------------------
// tTJSNI_Dialog
//---------------------------------------------------------------------------
tTJSNI_Dialog::tTJSNI_Dialog() : Owner(nullptr) {}
tTJSNI_Dialog::~tTJSNI_Dialog() {}

tjs_error TJS_INTF_METHOD
tTJSNI_Dialog::Construct(tjs_int /*numparams*/, tTJSVariant** /*param*/, iTJSDispatch2* tjs_obj)
{
	Owner = tjs_obj;
	return TJS_S_OK;
}

void TJS_INTF_METHOD tTJSNI_Dialog::Invalidate()
{
	if (Owner) {
		// この Dialog インスタンスが「現在アクティブなモーダル」のオーナー
		// (= mgr の active_handler が this) のときだけ閉じる。 別 Dialog が
		// 開いた modal を巻き込まないように。 modalTest/modalOverlayTest を
		// 連続実行したとき、 前者の dlg が GC される直前に後者の modal が走って
		// いると、 ここで Close を呼ぶと後者の modal が即終了してしまう。
		auto& mgr = tTVPElementsDialogManager::Instance();
		if (mgr.IsHandlerActive(this)) {
			mgr.Close(this);
		}
	}
	Owner = nullptr;
}

void tTJSNI_Dialog::OnAction(const ttstr& id, const tTJSVariant& payload)
{
	if (!Owner) return;
	tTJSVariant args[2] = { id, payload };
	static ttstr eventname(TJS_W("onAction"));
	TVPPostEvent(Owner, Owner, eventname, 0, TVP_EPT_IMMEDIATE, 2, args);
}

void tTJSNI_Dialog::OnScreenEnter(const ttstr& name)
{
	if (!Owner) return;
	tTJSVariant args[1] = { name };
	static ttstr eventname(TJS_W("onScreen"));
	TVPPostEvent(Owner, Owner, eventname, 0, TVP_EPT_IMMEDIATE, 1, args);
}

void tTJSNI_Dialog::OnScreenLeave(const ttstr& name, const ttstr& action)
{
	if (!Owner) return;
	tTJSVariant args[2] = { name, action };
	static ttstr eventname(TJS_W("onScreenLeave"));
	TVPPostEvent(Owner, Owner, eventname, 0, TVP_EPT_IMMEDIATE, 2, args);
}

void tTJSNI_Dialog::OnClosed(const ttstr& action)
{
	if (!Owner) return;
	tTJSVariant args[1] = { action };
	static ttstr eventname(TJS_W("onClose"));
	TVPPostEvent(Owner, Owner, eventname, 0, TVP_EPT_IMMEDIATE, 1, args);
}

bool tTJSNI_Dialog::ShowFile(const ttstr& path, bool grabFocus)
{
	auto& mgr = tTVPElementsDialogManager::Instance();
	// 非モーダルオーバーレイ。grabFocus=false なら未フォーカスで表示し、
	// 未処理キーをホストへ通す (wants_focus = modal || grabFocus のため modal も false に)。
	return mgr.ShowFromJsonFile(path, this, nullptr, /*modal=*/grabFocus, grabFocus);
}

bool tTJSNI_Dialog::ShowJson(const ttstr& json_utf16, bool grabFocus)
{
	std::string utf8;
	tjs_string ts(json_utf16.c_str());
	TVPUtf16ToUtf8(utf8, ts);
	auto& mgr = tTVPElementsDialogManager::Instance();
	return mgr.ShowFromJsonString(utf8, this, nullptr, /*modal=*/grabFocus, grabFocus);
}

bool tTJSNI_Dialog::ShowDict(iTJSDispatch2* dict, bool grabFocus)
{
	if (!dict) return false;
	std::string utf8;
	tTJSVariant v(dict, dict);
	TVPVariantToJsonUtf8(v, utf8);
	auto& mgr = tTVPElementsDialogManager::Instance();
	return mgr.ShowFromJsonString(utf8, this, nullptr, /*modal=*/grabFocus, grabFocus);
}

void tTJSNI_Dialog::Close()
{
	// 自分が開いたインスタンスだけを閉じる (Invalidate と同じ理由)。
	auto& mgr = tTVPElementsDialogManager::Instance();
	if (mgr.IsHandlerActive(this)) {
		mgr.Close(this);
	}
}

bool tTJSNI_Dialog::SetVar(const ttstr& name, const ttstr& value)
{
	return tTVPElementsDialogManager::Instance().SetVar(this, name, value);
}

//---------------------------------------------------------------------------
// Phase 6c: モーダルダイアログ (独立 SDL_Window 経由、ブロッキング)
//
// 戻り値の Dictionary フォーマット:
//   %[
//     action: <ttstr>,      // 閉じた button の id (Esc/× は "")
//     values: %[             // state widget の最終値マップ
//        <id>: <bool|ttstr>, // checkbox/toggle/slide → bool, input_box → ttstr
//        ...
//     ]
//   ]
//
// 注意: showModalJson 経路では onAction イベントは発火しない (内部 collector
// handler が全イベントを吸収するため)。 onAction 主体で書きたい呼出側は
// 従来の showJson / showFile (非同期) を使う。
//---------------------------------------------------------------------------
namespace {

iTJSDispatch2* BuildModalResultDict(const tTVPElementsModalResult& mr)
{
	iTJSDispatch2* dict = TJSCreateDictionaryObject();
	if (!dict) return nullptr;

	// action
	{
		tTJSVariant v(mr.Action);
		dict->PropSet(TJS_MEMBERENSURE, TJS_W("action"), nullptr, &v, dict);
	}

	// values (nested dict)
	iTJSDispatch2* values = TJSCreateDictionaryObject();
	if (values) {
		for (const auto& kv : mr.Values) {
			tTJSVariant v = kv.second;
			values->PropSet(TJS_MEMBERENSURE, kv.first.c_str(), nullptr, &v, values);
		}
		tTJSVariant vv(values, values);
		dict->PropSet(TJS_MEMBERENSURE, TJS_W("values"), nullptr, &vv, dict);
		values->Release();
	}
	return dict;
}

} // anonymous

iTJSDispatch2* tTJSNI_Dialog::ShowModalJson(const ttstr& json_utf16,
	const ttstr& title, int width, int height)
{
	std::string utf8;
	tjs_string ts(json_utf16.c_str());
	TVPUtf16ToUtf8(utf8, ts);

	tTVPElementsModalResult mr;
	// `this` を handler として渡し、 button click / 値変化が onAction にも来る。
	// "close_on_click": true な button だけがモーダルを閉じる。
	if (!TVPRunElementsModalWindow(utf8, title, width, height, this, mr)) {
		return nullptr;
	}
	return BuildModalResultDict(mr);
}

iTJSDispatch2* tTJSNI_Dialog::ShowModalFile(const ttstr& path,
	const ttstr& title, int width, int height)
{
	tjs_uint64 flen = 0;
	auto buf = TVPReadStream(path.c_str(), &flen);
	if (!buf || flen == 0) {
		TVPAddImportantLog(ttstr(TJS_W("Dialog.showModalFile: cannot read: "))
			+ path);
		return nullptr;
	}
	std::string utf8(reinterpret_cast<const char*>(buf.get()),
	                 static_cast<size_t>(flen));

	tTVPElementsModalResult mr;
	if (!TVPRunElementsModalWindow(utf8, title, width, height, this, mr)) {
		return nullptr;
	}
	return BuildModalResultDict(mr);
}

iTJSDispatch2* tTJSNI_Dialog::ShowModalOverlayJson(const ttstr& json_utf16)
{
	std::string utf8;
	tjs_string ts(json_utf16.c_str());
	TVPUtf16ToUtf8(utf8, ts);

	tTVPElementsModalResult mr;
	if (!TVPRunElementsModalOverlay(utf8, this, mr)) {
		return nullptr;
	}
	return BuildModalResultDict(mr);
}

iTJSDispatch2* tTJSNI_Dialog::ShowModalOverlayFile(const ttstr& path)
{
	tjs_uint64 flen = 0;
	auto buf = TVPReadStream(path.c_str(), &flen);
	if (!buf || flen == 0) {
		TVPAddImportantLog(ttstr(TJS_W("Dialog.showModalOverlayFile: cannot read: "))
			+ path);
		return nullptr;
	}
	std::string utf8(reinterpret_cast<const char*>(buf.get()),
	                 static_cast<size_t>(flen));

	tTVPElementsModalResult mr;
	if (!TVPRunElementsModalOverlay(utf8, this, mr)) {
		return nullptr;
	}
	return BuildModalResultDict(mr);
}

iTJSDispatch2* tTJSNI_Dialog::ShowModalDict(iTJSDispatch2* dict,
	const ttstr& title, int width, int height)
{
	if (!dict) return nullptr;
	std::string utf8;
	tTJSVariant v(dict, dict);
	TVPVariantToJsonUtf8(v, utf8);

	tTVPElementsModalResult mr;
	if (!TVPRunElementsModalWindow(utf8, title, width, height, this, mr)) {
		return nullptr;
	}
	return BuildModalResultDict(mr);
}

iTJSDispatch2* tTJSNI_Dialog::ShowModalOverlayDict(iTJSDispatch2* dict)
{
	if (!dict) return nullptr;
	std::string utf8;
	tTJSVariant v(dict, dict);
	TVPVariantToJsonUtf8(v, utf8);

	tTVPElementsModalResult mr;
	if (!TVPRunElementsModalOverlay(utf8, this, mr)) {
		return nullptr;
	}
	return BuildModalResultDict(mr);
}

//---------------------------------------------------------------------------
// navigator フロー (複数画面遷移)
//---------------------------------------------------------------------------
iTJSDispatch2* tTJSNI_Dialog::ShowFlow(const ttstr& manifest_path)
{
	tTVPElementsModalResult mr;
	if (!TVPRunElementsFlowOverlayManifest(manifest_path, this, mr)) {
		return nullptr;
	}
	return BuildModalResultDict(mr);
}

namespace {

// TJS Dictionary (画面名 → レイアウト) を std::map<utf8,utf8> に変換する
// EnumMembers コールバック。 param[0]=メンバ名, param[1]=flags, param[2]=値。
// 値は JSON 文字列、 または Dictionary / Array (内部で JSON 化、 混在可)。
// どちらでもないメンバは無視する。
struct ScreenEnumCaller : public tTJSDispatch
{
	std::map<std::string, std::string> screens;

	tjs_error TJS_INTF_METHOD FuncCall(tjs_uint32 /*flag*/,
		const tjs_char* /*membername*/, tjs_uint32* /*hint*/,
		tTJSVariant* result, tjs_int numparams,
		tTJSVariant** param, iTJSDispatch2* /*objthis*/)
	{
		if (numparams < 3) return TJS_E_BADPARAMCOUNT;
		tjs_uint32 flags = (tjs_int)*param[1];
		if (flags & TJS_HIDDENMEMBER) {
			if (result) *result = (tjs_int)1;
			return TJS_S_OK;
		}
		ttstr key = *param[0];
		std::string k, v;
		{ tjs_string ts(key.c_str()); TVPUtf16ToUtf8(k, ts); }
		if (param[2]->Type() == tvtObject) {
			TVPVariantToJsonUtf8(*param[2], v);   // Dictionary / Array レイアウト
		} else if (param[2]->Type() == tvtString) {
			ttstr val = *param[2];   // JSON 文字列として読む
			tjs_string ts(val.c_str()); TVPUtf16ToUtf8(v, ts);
		}
		if (!k.empty() && !v.empty()) screens.emplace(std::move(k), std::move(v));
		if (result) *result = (tjs_int)1;
		return TJS_S_OK;
	}
};

} // anonymous

iTJSDispatch2* tTJSNI_Dialog::ShowFlowScreens(iTJSDispatch2* screens_dict,
	const ttstr& entry)
{
	if (!screens_dict) return nullptr;

	ScreenEnumCaller caller;
	tTJSVariantClosure clo(&caller, nullptr);
	screens_dict->EnumMembers(TJS_IGNOREPROP, &clo, screens_dict);
	if (caller.screens.empty()) {
		TVPAddImportantLog(TJS_W("Dialog.showFlowScreens: no string screens in dict"));
		return nullptr;
	}

	std::string entry_utf8;
	{ tjs_string ts(entry.c_str()); TVPUtf16ToUtf8(entry_utf8, ts); }

	tTVPElementsModalResult mr;
	if (!TVPRunElementsFlowOverlayScreens(caller.screens, entry_utf8, this, mr)) {
		return nullptr;
	}
	return BuildModalResultDict(mr);
}

//---------------------------------------------------------------------------
// 非モーダル (非ブロッキング) フロー
//---------------------------------------------------------------------------
bool tTJSNI_Dialog::StartFlow(const ttstr& manifest_path, bool grabFocus)
{
	// 即 return。 以降の画面遷移 / イベントは DrawDevice の PaintOverlay が駆動し、
	// onScreen / onScreenLeave / onAction で通知される。 close() で閉じる。
	return tTVPElementsDialogManager::Instance()
		.StartFlowFromManifest(manifest_path, this, nullptr, /*modal=*/false, grabFocus);
}

bool tTJSNI_Dialog::StartFlowScreens(iTJSDispatch2* screens_dict, const ttstr& entry,
	bool grabFocus)
{
	if (!screens_dict) return false;
	ScreenEnumCaller caller;
	tTJSVariantClosure clo(&caller, nullptr);
	screens_dict->EnumMembers(TJS_IGNOREPROP, &clo, screens_dict);
	if (caller.screens.empty()) {
		TVPAddImportantLog(TJS_W("Dialog.startFlowScreens: no string screens in dict"));
		return false;
	}
	std::string entry_utf8;
	{ tjs_string ts(entry.c_str()); TVPUtf16ToUtf8(entry_utf8, ts); }
	return tTVPElementsDialogManager::Instance()
		.StartFlowFromScreens(caller.screens, entry_utf8, this, nullptr,
			/*modal=*/false, grabFocus);
}

//---------------------------------------------------------------------------
// tTJSNC_Dialog
//---------------------------------------------------------------------------
tjs_uint32 tTJSNC_Dialog::ClassID = (tjs_uint32)-1;

tTJSNC_Dialog::tTJSNC_Dialog() : inherited(TJS_W("Dialog"))
{
	TJS_BEGIN_NATIVE_MEMBERS(Dialog)
	TJS_DECL_EMPTY_FINALIZE_METHOD
	//---------------------------------------------------------------------------
	TJS_BEGIN_NATIVE_CONSTRUCTOR_DECL(/*var.name*/_this, /*var.type*/tTJSNI_Dialog, /*TJS class name*/Dialog)
	{
		return TJS_S_OK;
	}
	TJS_END_NATIVE_CONSTRUCTOR_DECL(/*TJS class name*/Dialog)
	//---------------------------------------------------------------------------
	TJS_BEGIN_NATIVE_METHOD_DECL(/*func. name*/showFile)
	{
		TJS_GET_NATIVE_INSTANCE(/*var. name*/_this, /*var. type*/tTJSNI_Dialog);
		if (numparams < 1) return TJS_E_BADPARAMCOUNT;
		ttstr path(*param[0]);
		bool grabFocus = (numparams >= 2 && param[1]->Type() != tvtVoid) ? (bool)(tjs_int)*param[1] : true;
		bool ok = _this->ShowFile(path, grabFocus);
		if (result) *result = ok;
		return TJS_S_OK;
	}
	TJS_END_NATIVE_METHOD_DECL(/*func. name*/showFile)
	//---------------------------------------------------------------------------
	TJS_BEGIN_NATIVE_METHOD_DECL(/*func. name*/showJson)
	{
		TJS_GET_NATIVE_INSTANCE(/*var. name*/_this, /*var. type*/tTJSNI_Dialog);
		if (numparams < 1) return TJS_E_BADPARAMCOUNT;
		ttstr json(*param[0]);
		bool grabFocus = (numparams >= 2 && param[1]->Type() != tvtVoid) ? (bool)(tjs_int)*param[1] : true;
		bool ok = _this->ShowJson(json, grabFocus);
		if (result) *result = ok;
		return TJS_S_OK;
	}
	TJS_END_NATIVE_METHOD_DECL(/*func. name*/showJson)
	//---------------------------------------------------------------------------
	TJS_BEGIN_NATIVE_METHOD_DECL(/*func. name*/close)
	{
		TJS_GET_NATIVE_INSTANCE(/*var. name*/_this, /*var. type*/tTJSNI_Dialog);
		_this->Close();
		return TJS_S_OK;
	}
	TJS_END_NATIVE_METHOD_DECL(/*func. name*/close)
	//---------------------------------------------------------------------------
	// showModalJson(json [, title [, width [, height]]])
	//   引数 1 個: 既存ゲーム window 上にオーバーレイ表示 (in-game modal)。
	//   引数 2 個以上: 独立 SDL_Window でモーダル表示。 title / width / height
	//                  を渡す形 (UserConfig 風)。
	//   どちらも閉じるまでブロックし、 Dictionary `%[action, values]` を返す。
	TJS_BEGIN_NATIVE_METHOD_DECL(/*func. name*/showModalJson)
	{
		TJS_GET_NATIVE_INSTANCE(/*var. name*/_this, /*var. type*/tTJSNI_Dialog);
		if (numparams < 1) return TJS_E_BADPARAMCOUNT;
		ttstr json(*param[0]);
		iTJSDispatch2* dict;
		if (numparams >= 2) {
			ttstr title = ttstr(*param[1]);
			int width   = (numparams >= 3) ? static_cast<int>((tjs_int)*param[2]) : 800;
			int height  = (numparams >= 4) ? static_cast<int>((tjs_int)*param[3]) : 600;
			dict = _this->ShowModalJson(json, title, width, height);
		} else {
			dict = _this->ShowModalOverlayJson(json);
		}
		if (!dict) return TJS_E_FAIL;
		if (result) *result = tTJSVariant(dict, dict);
		dict->Release();
		return TJS_S_OK;
	}
	TJS_END_NATIVE_METHOD_DECL(/*func. name*/showModalJson)
	//---------------------------------------------------------------------------
	// showModalFile(path [, title [, width [, height]]])
	//   path から JSON を読込んで showModalJson と同じ動作。 引数 1 個で overlay、
	//   2 個以上で独立 SDL_Window。
	TJS_BEGIN_NATIVE_METHOD_DECL(/*func. name*/showModalFile)
	{
		TJS_GET_NATIVE_INSTANCE(/*var. name*/_this, /*var. type*/tTJSNI_Dialog);
		if (numparams < 1) return TJS_E_BADPARAMCOUNT;
		ttstr path(*param[0]);
		iTJSDispatch2* dict;
		if (numparams >= 2) {
			ttstr title = ttstr(*param[1]);
			int width   = (numparams >= 3) ? static_cast<int>((tjs_int)*param[2]) : 800;
			int height  = (numparams >= 4) ? static_cast<int>((tjs_int)*param[3]) : 600;
			dict = _this->ShowModalFile(path, title, width, height);
		} else {
			dict = _this->ShowModalOverlayFile(path);
		}
		if (!dict) return TJS_E_FAIL;
		if (result) *result = tTJSVariant(dict, dict);
		dict->Release();
		return TJS_S_OK;
	}
	TJS_END_NATIVE_METHOD_DECL(/*func. name*/showModalFile)
	//---------------------------------------------------------------------------
	// showDict(dict)
	//   showJson の Dictionary 版。 レイアウトを TJS の Dictionary / Array で
	//   直接書ける (内部で JSON 文字列へ変換して同じ経路に流す)。 変換不能な
	//   値 (octet / Dictionary・Array 以外のオブジェクト / 循環参照) は例外。
	TJS_BEGIN_NATIVE_METHOD_DECL(/*func. name*/showDict)
	{
		TJS_GET_NATIVE_INSTANCE(/*var. name*/_this, /*var. type*/tTJSNI_Dialog);
		if (numparams < 1) return TJS_E_BADPARAMCOUNT;
		if (param[0]->Type() != tvtObject) return TJS_E_INVALIDPARAM;
		bool grabFocus = (numparams >= 2 && param[1]->Type() != tvtVoid) ? (bool)(tjs_int)*param[1] : true;
		bool ok = _this->ShowDict(param[0]->AsObjectNoAddRef(), grabFocus);
		if (result) *result = ok;
		return TJS_S_OK;
	}
	TJS_END_NATIVE_METHOD_DECL(/*func. name*/showDict)
	//---------------------------------------------------------------------------
	// showModalDict(dict [, title [, width [, height]]])
	//   showModalJson の Dictionary 版。 引数 1 個で overlay、 2 個以上で
	//   独立 SDL_Window。 戻り値仕様も showModalJson と同じ。
	TJS_BEGIN_NATIVE_METHOD_DECL(/*func. name*/showModalDict)
	{
		TJS_GET_NATIVE_INSTANCE(/*var. name*/_this, /*var. type*/tTJSNI_Dialog);
		if (numparams < 1) return TJS_E_BADPARAMCOUNT;
		if (param[0]->Type() != tvtObject) return TJS_E_INVALIDPARAM;
		iTJSDispatch2* dict_in = param[0]->AsObjectNoAddRef();
		iTJSDispatch2* dict;
		if (numparams >= 2) {
			ttstr title = ttstr(*param[1]);
			int width   = (numparams >= 3) ? static_cast<int>((tjs_int)*param[2]) : 800;
			int height  = (numparams >= 4) ? static_cast<int>((tjs_int)*param[3]) : 600;
			dict = _this->ShowModalDict(dict_in, title, width, height);
		} else {
			dict = _this->ShowModalOverlayDict(dict_in);
		}
		if (!dict) return TJS_E_FAIL;
		if (result) *result = tTJSVariant(dict, dict);
		dict->Release();
		return TJS_S_OK;
	}
	TJS_END_NATIVE_METHOD_DECL(/*func. name*/showModalDict)
	//---------------------------------------------------------------------------
	// dictToJson(value)
	//   showDict 等が内部で使う Dictionary/Array → JSON 変換をそのまま呼び出す
	//   ユーティリティ。 変換結果の JSON 文字列を返す (デバッグ / JSON 資材の
	//   書き出し用)。
	TJS_BEGIN_NATIVE_METHOD_DECL(/*func. name*/dictToJson)
	{
		if (numparams < 1) return TJS_E_BADPARAMCOUNT;
		std::string utf8;
		TVPVariantToJsonUtf8(*param[0], utf8);
		if (result) {
			tjs_string ws;
			TVPUtf8ToUtf16(ws, utf8);
			*result = ttstr(ws.c_str());
		}
		return TJS_S_OK;
	}
	TJS_END_NATIVE_METHOD_DECL(/*func. name*/dictToJson)
	//---------------------------------------------------------------------------
	// showFlow(manifestPath)
	//   app.jsonc マニフェスト (storage パス) 駆動の複数画面フローをオーバーレイ
	//   でブロッキング実行。 フロー終了まで block し、 最後に閉じた画面の
	//   Dictionary `%[action, values]` を返す。 画面遷移ごとに onScreen /
	//   onScreenLeave、 各 widget で onAction が発火する。
	TJS_BEGIN_NATIVE_METHOD_DECL(/*func. name*/showFlow)
	{
		TJS_GET_NATIVE_INSTANCE(/*var. name*/_this, /*var. type*/tTJSNI_Dialog);
		if (numparams < 1) return TJS_E_BADPARAMCOUNT;
		ttstr path(*param[0]);
		iTJSDispatch2* dict = _this->ShowFlow(path);
		if (!dict) return TJS_E_FAIL;
		if (result) *result = tTJSVariant(dict, dict);
		dict->Release();
		return TJS_S_OK;
	}
	TJS_END_NATIVE_METHOD_DECL(/*func. name*/showFlow)
	//---------------------------------------------------------------------------
	// showFlowScreens(screensDict, entry)
	//   ファイル I/O を介さないインライン版。 screensDict は画面名 → レイアウト
	//   (JSON 文字列 or Dictionary、 混在可) の Dictionary、 entry は起点画面名。
	//   その他は showFlow と同じ。
	TJS_BEGIN_NATIVE_METHOD_DECL(/*func. name*/showFlowScreens)
	{
		TJS_GET_NATIVE_INSTANCE(/*var. name*/_this, /*var. type*/tTJSNI_Dialog);
		if (numparams < 2) return TJS_E_BADPARAMCOUNT;
		if (param[0]->Type() != tvtObject) return TJS_E_INVALIDPARAM;
		iTJSDispatch2* dict_in = param[0]->AsObjectNoAddRef();
		ttstr entry(*param[1]);
		iTJSDispatch2* dict = _this->ShowFlowScreens(dict_in, entry);
		if (!dict) return TJS_E_FAIL;
		if (result) *result = tTJSVariant(dict, dict);
		dict->Release();
		return TJS_S_OK;
	}
	TJS_END_NATIVE_METHOD_DECL(/*func. name*/showFlowScreens)
	//---------------------------------------------------------------------------
	// startFlow(manifestPath)
	//   非モーダル (非ブロッキング) フローを開始して即 return (戻り値 = 起動成否)。
	//   ゲーム画面に常駐させたまま背景でサンプルを動かせる。 画面遷移は JSON の
	//   transitions、 サンプル起動等は close_on_click 無し button の onAction で。
	//   close() で閉じる。
	TJS_BEGIN_NATIVE_METHOD_DECL(/*func. name*/startFlow)
	{
		TJS_GET_NATIVE_INSTANCE(/*var. name*/_this, /*var. type*/tTJSNI_Dialog);
		if (numparams < 1) return TJS_E_BADPARAMCOUNT;
		ttstr path(*param[0]);
		bool grabFocus = (numparams >= 2) ? (bool)(tjs_int)*param[1] : true;
		bool ok = _this->StartFlow(path, grabFocus);
		if (result) *result = ok;
		return TJS_S_OK;
	}
	TJS_END_NATIVE_METHOD_DECL(/*func. name*/startFlow)
	//---------------------------------------------------------------------------
	// startFlowScreens(screensDict, entry)  — startFlow のインライン版。
	TJS_BEGIN_NATIVE_METHOD_DECL(/*func. name*/startFlowScreens)
	{
		TJS_GET_NATIVE_INSTANCE(/*var. name*/_this, /*var. type*/tTJSNI_Dialog);
		if (numparams < 2) return TJS_E_BADPARAMCOUNT;
		if (param[0]->Type() != tvtObject) return TJS_E_INVALIDPARAM;
		iTJSDispatch2* dict_in = param[0]->AsObjectNoAddRef();
		ttstr entry(*param[1]);
		bool grabFocus = (numparams >= 3) ? (bool)(tjs_int)*param[2] : true;
		bool ok = _this->StartFlowScreens(dict_in, entry, grabFocus);
		if (result) *result = ok;
		return TJS_S_OK;
	}
	TJS_END_NATIVE_METHOD_DECL(/*func. name*/startFlowScreens)
	//---------------------------------------------------------------------------
	// navigator フロー: 画面に入った / 出たときに呼ばれるイベント (TJS で override)
	TJS_BEGIN_NATIVE_METHOD_DECL(/*func. name*/onScreen)
	{
		// no-op default
		return TJS_S_OK;
	}
	TJS_END_NATIVE_METHOD_DECL(/*func. name*/onScreen)
	//---------------------------------------------------------------------------
	TJS_BEGIN_NATIVE_METHOD_DECL(/*func. name*/onScreenLeave)
	{
		// no-op default
		return TJS_S_OK;
	}
	TJS_END_NATIVE_METHOD_DECL(/*func. name*/onScreenLeave)
	//---------------------------------------------------------------------------
	// 既定で何もしない onAction イベントメソッド (TJS 側で override される)
	TJS_BEGIN_NATIVE_METHOD_DECL(/*func. name*/onAction)
	{
		// no-op default
		return TJS_S_OK;
	}
	TJS_END_NATIVE_METHOD_DECL(/*func. name*/onAction)
	//---------------------------------------------------------------------------
	// onClose(action) — インスタンス teardown 完了時に発火 (非ブロッキング経路)。
	// action は close_on_click / Esc で閉じた button id (close() 等は空文字)。
	TJS_BEGIN_NATIVE_METHOD_DECL(/*func. name*/onClose)
	{
		// no-op default
		return TJS_S_OK;
	}
	TJS_END_NATIVE_METHOD_DECL(/*func. name*/onClose)
	//---------------------------------------------------------------------------
	// setVar(name, value) — 表示中ダイアログの変数 store へ書込。 JSON で
	// "text_var": name を指定した label が次フレームで更新される。 自分の
	// インスタンスが非アクティブなら false。
	TJS_BEGIN_NATIVE_METHOD_DECL(/*func. name*/setVar)
	{
		TJS_GET_NATIVE_INSTANCE(/*var. name*/_this, /*var. type*/tTJSNI_Dialog);
		if (numparams < 2) return TJS_E_BADPARAMCOUNT;
		ttstr name(*param[0]);
		ttstr value(*param[1]);
		bool ok = _this->SetVar(name, value);
		if (result) *result = ok;
		return TJS_S_OK;
	}
	TJS_END_NATIVE_METHOD_DECL(/*func. name*/setVar)
	//---------------------------------------------------------------------------
	// registerFont(family, path [, weight [, slant [, stretch]]])
	//
	// Elements 用のフォントを krkrz Storages 経由でメモリ登録する。
	// `path` は krkrz storage パス。 weight/slant/stretch は font_constants の
	// 整数値 (StoragesResourceLoader.h コメント参照)。 内部で resource_loader
	// を install してから cycfi::elements::register_font を呼ぶ。 `this` は
	// 使わないので任意の Dialog インスタンス経由で呼んで構わない。
	TJS_BEGIN_NATIVE_METHOD_DECL(/*func. name*/registerFont)
	{
		if (numparams < 2) return TJS_E_BADPARAMCOUNT;
		ttstr family(*param[0]);
		ttstr path(*param[1]);
		int weight  = (numparams >= 3) ? static_cast<int>((tjs_int)*param[2]) : 40;
		int slant   = (numparams >= 4) ? static_cast<int>((tjs_int)*param[3]) : 0;
		int stretch = (numparams >= 5) ? static_cast<int>((tjs_int)*param[4]) : 50;
		// 戻り値: ThorVG が読み取った embedded family 名 (取れなければ空文字)。
		// スクリプトはこの戻り値を見て theme への組込みやログ表示に使える。
		ttstr canonical =
			TVPRegisterElementsFont(family, path, weight, slant, stretch);
		if (result) *result = canonical;
		return TJS_S_OK;
	}
	TJS_END_NATIVE_METHOD_DECL(/*func. name*/registerFont)
	//---------------------------------------------------------------------------
	// registerFontDir(dir)
	//
	// 指定ディレクトリ配下の .ttf / .otf を全て列挙して登録する (内部で
	// ファイル名から family / weight / slant / stretch を推定)。 dir は
	// krkrz storage パス、 XP3 内のディレクトリでも OK。
	TJS_BEGIN_NATIVE_METHOD_DECL(/*func. name*/registerFontDir)
	{
		if (numparams < 1) return TJS_E_BADPARAMCOUNT;
		ttstr dir(*param[0]);
		TVPRegisterElementsFontsFromStorageDir(dir);
		if (result) *result = true;
		return TJS_S_OK;
	}
	TJS_END_NATIVE_METHOD_DECL(/*func. name*/registerFontDir)
	//---------------------------------------------------------------------------
	// defaultFontFamily プロパティ:
	//   getter — 現在 theme.label_font 等に当てはまっている families 文字列
	//            (comma 区切り)。 未設定なら空文字。
	//   setter — その families を theme フォントに上書き設定する。
	// EnsureRuntimeInitialized 後に意味を持つ。
	TJS_BEGIN_NATIVE_PROP_DECL(defaultFontFamily)
	{
		TJS_BEGIN_NATIVE_PROP_GETTER
		{
			*result = TVPGetElementsDefaultFontFamily();
			return TJS_S_OK;
		}
		TJS_END_NATIVE_PROP_GETTER

		TJS_BEGIN_NATIVE_PROP_SETTER
		{
			ttstr families = ttstr(*param);
			TVPSetElementsDefaultFontFamily(families);
			return TJS_S_OK;
		}
		TJS_END_NATIVE_PROP_SETTER
	}
	TJS_END_NATIVE_PROP_DECL(defaultFontFamily)
	//---------------------------------------------------------------------------
	// active プロパティ (getter のみ):
	//   この Dialog インスタンスが今アクティブなダイアログ / フローのオーナーか。
	//   非モーダル startFlow を閉じた後、 manager の teardown 完了を待ってから
	//   次の modal を起動する、 といった判定に使う (close() は次フレーム teardown
	//   なので、 閉じた直後はまだ true)。
	TJS_BEGIN_NATIVE_PROP_DECL(active)
	{
		TJS_BEGIN_NATIVE_PROP_GETTER
		{
			TJS_GET_NATIVE_INSTANCE(/*var. name*/_this, /*var. type*/tTJSNI_Dialog);
			auto& mgr = tTVPElementsDialogManager::Instance();
			bool a = mgr.IsHandlerActive(_this);
			*result = a;
			return TJS_S_OK;
		}
		TJS_END_NATIVE_PROP_GETTER
		TJS_DENY_NATIVE_PROP_SETTER
	}
	TJS_END_NATIVE_PROP_DECL(active)
	//---------------------------------------------------------------------------
	TJS_END_NATIVE_MEMBERS
}

tTJSNativeInstance* tTJSNC_Dialog::CreateNativeInstance()
{
	return new tTJSNI_Dialog();
}

tTJSNativeClass* TVPCreateNativeClass_Dialog()
{
	// ClassID は tTJSNC_Dialog ctor 内の TJS_BEGIN_NATIVE_MEMBERS で
	// 自動的に登録・代入される。
	return new tTJSNC_Dialog();
}
