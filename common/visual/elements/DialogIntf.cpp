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
#include "MsgIntf.h"         // TVPThrowExceptionMessage (例外への文脈付加)
#include "tjsError.h"        // eTJS (TJS 例外の透過判定)

#include <elements/element/pad_icon.hpp>   // set_pad_icon_base_dir / set_pad_theme
#include <elements_modal/modal.h>          // set_focus_ring_enabled (Dialog.focusRing)

#include <string>

namespace {

//---------------------------------------------------------------------------
// 例外への文脈付加
//
// elements / elements_modal / host 別 modal runner 由来の C++ 例外は what() が
// 短く (例: "File does not exist.")、 どの Dialog API のどのファイルで起きたか
// が例外メッセージから分からない。 さらに std::exception 以外の型だと VM 側で
// メッセージ無しの例外になる。 全 API 入口でここを通し、 API 名と対象
// (path / entry) を付けて TJS 例外へ変換する。 eTJS 系 (TVPReadStream の
// storage エラー等) は元々十分な情報を持つのでそのまま透過させる。
//---------------------------------------------------------------------------
template<typename TFunc>
auto WithDialogExceptionContext(const tjs_char* api, const ttstr& target,
	TFunc&& func) -> decltype(func())
{
	try {
		return func();
	} catch (const eTJS&) {
		throw;
	} catch (const std::exception& e) {
		std::string w8 = e.what() ? e.what() : "";
		tjs_string ws;
		TVPUtf8ToUtf16(ws, w8);
		ttstr msg(api);
		if (!target.IsEmpty()) msg += ttstr(TJS_W("(")) + target + TJS_W(")");
		msg += ttstr(TJS_W(": ")) + ttstr(ws.c_str());
		TVPThrowExceptionMessage(TJS_W("%1"), msg);
	} catch (...) {
		ttstr msg(api);
		if (!target.IsEmpty()) msg += ttstr(TJS_W("(")) + target + TJS_W(")");
		msg += TJS_W(": unknown C++ exception");
		TVPThrowExceptionMessage(TJS_W("%1"), msg);
	}
	return decltype(func()){};   // not reached (TVPThrowExceptionMessage は必ず throw)
}

} // anonymous namespace

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
	// この native インスタンスを handler として参照する全インスタンスから
	// 参照を切る (自分のインスタンスだけが対象なので、 別 Dialog が開いた
	// modal を巻き込むことはない)。 単に Close() するだけだと、 モーダル終了
	// 直後 (active=false, teardown は次フレーム) に GC された場合に参照が
	// 残ったままとなり、 遅延 teardown の OnClosed が解放済み this への仮想
	// 呼び出しになる (Release ビルドで AV)。
	tTVPElementsDialogManager::Instance().DetachHandler(this);
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

// modal 引数の解決: -1 (省略) は後方互換で grabFocus に追従。 grabFocus=true +
// modal=0 が「非モーダル+フォーカスあり」= キー/パッドはダイアログへ届き、
// 未処理分はホストへ素通し (handled pass-through) の中間状態になる。
static bool ResolveShowModal(bool grabFocus, int modal)
{
	return (modal < 0) ? grabFocus : (modal != 0);
}

bool tTJSNI_Dialog::ShowFile(const ttstr& path, bool grabFocus, int modal)
{
	return WithDialogExceptionContext(TJS_W("Dialog.showFile"), path, [&]() -> bool {
		auto& mgr = tTVPElementsDialogManager::Instance();
		return mgr.ShowFromJsonFile(path, this, nullptr,
			ResolveShowModal(grabFocus, modal), grabFocus);
	});
}

bool tTJSNI_Dialog::ShowJson(const ttstr& json_utf16, bool grabFocus, int modal)
{
	return WithDialogExceptionContext(TJS_W("Dialog.showJson"), ttstr(), [&]() -> bool {
		std::string utf8;
		tjs_string ts(json_utf16.c_str());
		TVPUtf16ToUtf8(utf8, ts);
		auto& mgr = tTVPElementsDialogManager::Instance();
		return mgr.ShowFromJsonString(utf8, this, nullptr,
			ResolveShowModal(grabFocus, modal), grabFocus);
	});
}

bool tTJSNI_Dialog::ShowDict(iTJSDispatch2* dict, bool grabFocus, int modal)
{
	if (!dict) return false;
	return WithDialogExceptionContext(TJS_W("Dialog.showDict"), ttstr(), [&]() -> bool {
		std::string utf8;
		tTJSVariant v(dict, dict);
		TVPVariantToJsonUtf8(v, utf8);
		auto& mgr = tTVPElementsDialogManager::Instance();
		return mgr.ShowFromJsonString(utf8, this, nullptr,
			ResolveShowModal(grabFocus, modal), grabFocus);
	});
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

// showModal*(x, %[vars]) の 2 番目引数 (TJS Dictionary) を
// std::map<ttstr,ttstr> へ変換する EnumMembers コールバック。 値は文字列 /
// 数値等を ttstr 化して格納 (VariableStore は文字列ベース)。
struct VarsEnumCaller : public tTJSDispatch
{
	std::map<ttstr, ttstr> vars;

	tjs_error TJS_INTF_METHOD FuncCall(tjs_uint32 /*flag*/,
		const tjs_char* /*membername*/, tjs_uint32* /*hint*/,
		tTJSVariant* result, tjs_int numparams,
		tTJSVariant** param, iTJSDispatch2* /*objthis*/)
	{
		if (numparams < 3) return TJS_E_BADPARAMCOUNT;
		tjs_uint32 flags = (tjs_int)*param[1];
		if (!(flags & TJS_HIDDENMEMBER)) {
			ttstr key = *param[0];
			tTJSVariantType t = param[2]->Type();
			if (t == tvtString || t == tvtInteger || t == tvtReal) {
				vars[key] = ttstr(*param[2]);
			}
		}
		if (result) *result = (tjs_int)1;
		return TJS_S_OK;
	}
};

// TJS Dictionary → vars map。 dict が null なら空 map。
static std::map<ttstr, ttstr> DictToVarsMap(iTJSDispatch2* dict)
{
	std::map<ttstr, ttstr> out;
	if (!dict) return out;
	VarsEnumCaller caller;
	tTJSVariantClosure clo(&caller, nullptr);
	dict->EnumMembers(TJS_IGNOREPROP, &clo, dict);
	out = std::move(caller.vars);
	return out;
}

} // anonymous

//---------------------------------------------------------------------------
// System.inputString の Elements 実装 (SDL host)。
//---------------------------------------------------------------------------
static std::string InputJsonEscape(const std::string& s)
{
	std::string out;
	out.reserve(s.size() + 8);
	for (unsigned char c : s) {
		switch (c) {
			case '"':  out += "\\\""; break;
			case '\\': out += "\\\\"; break;
			case '\n': out += "\\n";  break;
			case '\r': out += "\\r";  break;
			case '\t': out += "\\t";  break;
			default:
				if (c < 0x20) { char b[8]; snprintf(b, sizeof(b), "\\u%04x", c); out += b; }
				else out += (char)c;
		}
	}
	return out;
}

bool TVPInputStringElements(const ttstr& caption, const ttstr& prompt,
	const ttstr& def, ttstr& result)
{
	std::string p8, d8;
	{ tjs_string t(prompt.c_str()); TVPUtf16ToUtf8(p8, t); }
	{ tjs_string t(def.c_str());    TVPUtf16ToUtf8(d8, t); }

	std::string json =
		"{\"background\":[40,40,40,245],\"content\":{"
		"\"type\":\"margin\",\"padding\":16,\"child\":{"
		"\"type\":\"vtile\",\"children\":["
		"{\"type\":\"label\",\"text\":\"" + InputJsonEscape(p8) + "\"},"
		"{\"type\":\"vspacer\",\"height\":8},"
		"{\"type\":\"input_box\",\"id\":\"value\",\"text\":\"" + InputJsonEscape(d8) + "\",\"initial_focus\":true},"
		"{\"type\":\"vspacer\",\"height\":12},"
		"{\"type\":\"htile\",\"children\":["
		"{\"type\":\"button\",\"id\":\"ok\",\"text\":\"OK\",\"close_on_click\":true},"
		"{\"type\":\"hspacer\",\"width\":8},"
		"{\"type\":\"button\",\"id\":\"cancel\",\"text\":\"\xE3\x82\xAD\xE3\x83\xA3\xE3\x83\xB3\xE3\x82\xBB\xE3\x83\xAB\",\"close_on_click\":true}"
		"]}"
		"]}}}";

	tTVPElementsModalResult mr;
	if (!TVPRunElementsModalWindow(json, caption, 380, 150, nullptr, mr))
		return false; // 起動失敗 (WINVER 独立 window modal 未対応など)

	if (mr.Action == ttstr(TJS_W("ok"))) {
		auto it = mr.Values.find(ttstr(TJS_W("value")));
		if (it != mr.Values.end() && it->second.Type() == tvtString)
			result = ttstr(it->second);
		else
			result = def; // 未入力は既定値
		return true;
	}
	return false; // cancel / Esc
}
//---------------------------------------------------------------------------
iTJSDispatch2* tTJSNI_Dialog::ShowModalJson(const ttstr& json_utf16,
	const ttstr& title, int width, int height)
{
	return WithDialogExceptionContext(TJS_W("Dialog.showModalJson"), ttstr(),
		[&]() -> iTJSDispatch2* {
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
	});
}

iTJSDispatch2* tTJSNI_Dialog::ShowModalFile(const ttstr& path,
	const ttstr& title, int width, int height)
{
	return WithDialogExceptionContext(TJS_W("Dialog.showModalFile"), path,
		[&]() -> iTJSDispatch2* {
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
	});
}

iTJSDispatch2* tTJSNI_Dialog::ShowModalOverlayJson(const ttstr& json_utf16,
	const std::map<ttstr, ttstr>* initialVars)
{
	return WithDialogExceptionContext(TJS_W("Dialog.showModalOverlayJson"), ttstr(),
		[&]() -> iTJSDispatch2* {
		std::string utf8;
		tjs_string ts(json_utf16.c_str());
		TVPUtf16ToUtf8(utf8, ts);

		tTVPElementsModalResult mr;
		if (!TVPRunElementsModalOverlay(utf8, this, mr, initialVars)) {
			return nullptr;
		}
		return BuildModalResultDict(mr);
	});
}

iTJSDispatch2* tTJSNI_Dialog::ShowModalOverlayFile(const ttstr& path,
	const std::map<ttstr, ttstr>* initialVars)
{
	return WithDialogExceptionContext(TJS_W("Dialog.showModalOverlayFile"), path,
		[&]() -> iTJSDispatch2* {
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
		if (!TVPRunElementsModalOverlay(utf8, this, mr, initialVars)) {
			return nullptr;
		}
		return BuildModalResultDict(mr);
	});
}

iTJSDispatch2* tTJSNI_Dialog::ShowModalDict(iTJSDispatch2* dict,
	const ttstr& title, int width, int height)
{
	if (!dict) return nullptr;
	return WithDialogExceptionContext(TJS_W("Dialog.showModalDict"), ttstr(),
		[&]() -> iTJSDispatch2* {
		std::string utf8;
		tTJSVariant v(dict, dict);
		TVPVariantToJsonUtf8(v, utf8);

		tTVPElementsModalResult mr;
		if (!TVPRunElementsModalWindow(utf8, title, width, height, this, mr)) {
			return nullptr;
		}
		return BuildModalResultDict(mr);
	});
}

iTJSDispatch2* tTJSNI_Dialog::ShowModalOverlayDict(iTJSDispatch2* dict,
	const std::map<ttstr, ttstr>* initialVars)
{
	if (!dict) return nullptr;
	return WithDialogExceptionContext(TJS_W("Dialog.showModalOverlayDict"), ttstr(),
		[&]() -> iTJSDispatch2* {
		std::string utf8;
		tTJSVariant v(dict, dict);
		TVPVariantToJsonUtf8(v, utf8);

		tTVPElementsModalResult mr;
		if (!TVPRunElementsModalOverlay(utf8, this, mr, initialVars)) {
			return nullptr;
		}
		return BuildModalResultDict(mr);
	});
}

//---------------------------------------------------------------------------
// navigator フロー (複数画面遷移)
//---------------------------------------------------------------------------
iTJSDispatch2* tTJSNI_Dialog::ShowFlow(const ttstr& manifest_path)
{
	return WithDialogExceptionContext(TJS_W("Dialog.showFlow"), manifest_path,
		[&]() -> iTJSDispatch2* {
		tTVPElementsModalResult mr;
		if (!TVPRunElementsFlowOverlayManifest(manifest_path, this, mr)) {
			return nullptr;
		}
		return BuildModalResultDict(mr);
	});
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

	return WithDialogExceptionContext(TJS_W("Dialog.showFlowScreens"), entry,
		[&]() -> iTJSDispatch2* {
		std::string entry_utf8;
		{ tjs_string ts(entry.c_str()); TVPUtf16ToUtf8(entry_utf8, ts); }

		tTVPElementsModalResult mr;
		if (!TVPRunElementsFlowOverlayScreens(caller.screens, entry_utf8, this, mr)) {
			return nullptr;
		}
		return BuildModalResultDict(mr);
	});
}

//---------------------------------------------------------------------------
// 非モーダル (非ブロッキング) フロー
//---------------------------------------------------------------------------
bool tTJSNI_Dialog::StartFlow(const ttstr& manifest_path, bool grabFocus)
{
	// 即 return。 以降の画面遷移 / イベントは DrawDevice の PaintOverlay が駆動し、
	// onScreen / onScreenLeave / onAction で通知される。 close() で閉じる。
	return WithDialogExceptionContext(TJS_W("Dialog.startFlow"), manifest_path,
		[&]() -> bool {
		return tTVPElementsDialogManager::Instance()
			.StartFlowFromManifest(manifest_path, this, nullptr, /*modal=*/false, grabFocus);
	});
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
	return WithDialogExceptionContext(TJS_W("Dialog.startFlowScreens"), entry,
		[&]() -> bool {
		std::string entry_utf8;
		{ tjs_string ts(entry.c_str()); TVPUtf16ToUtf8(entry_utf8, ts); }
		return tTVPElementsDialogManager::Instance()
			.StartFlowFromScreens(caller.screens, entry_utf8, this, nullptr,
				/*modal=*/false, grabFocus);
	});
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
		int modal = (numparams >= 3 && param[2]->Type() != tvtVoid) ? ((tjs_int)*param[2] ? 1 : 0) : -1;
		bool ok = _this->ShowFile(path, grabFocus, modal);
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
		int modal = (numparams >= 3 && param[2]->Type() != tvtVoid) ? ((tjs_int)*param[2] ? 1 : 0) : -1;
		bool ok = _this->ShowJson(json, grabFocus, modal);
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
		if (numparams >= 2 && param[1]->Type() == tvtObject) {
			// showModalJson(json, %[vars]) — overlay + 初期変数注入
			auto vars = DictToVarsMap(param[1]->AsObjectNoAddRef());
			dict = _this->ShowModalOverlayJson(json, &vars);
		} else if (numparams >= 2 && param[1]->Type() != tvtVoid) {
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
	//   showModalFile(path, %[vars]) — overlay + 初期変数注入 (build 直後・
	//   pump 前に setVar 相当。 index_var/enabled_var/selected_var 連動 widget
	//   へ動的初期値を流し込む)。
	TJS_BEGIN_NATIVE_METHOD_DECL(/*func. name*/showModalFile)
	{
		TJS_GET_NATIVE_INSTANCE(/*var. name*/_this, /*var. type*/tTJSNI_Dialog);
		if (numparams < 1) return TJS_E_BADPARAMCOUNT;
		ttstr path(*param[0]);
		iTJSDispatch2* dict;
		if (numparams >= 2 && param[1]->Type() == tvtObject) {
			auto vars = DictToVarsMap(param[1]->AsObjectNoAddRef());
			dict = _this->ShowModalOverlayFile(path, &vars);
		} else if (numparams >= 2 && param[1]->Type() != tvtVoid) {
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
		int modal = (numparams >= 3 && param[2]->Type() != tvtVoid) ? ((tjs_int)*param[2] ? 1 : 0) : -1;
		bool ok = _this->ShowDict(param[0]->AsObjectNoAddRef(), grabFocus, modal);
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
		if (numparams >= 2 && param[1]->Type() == tvtObject) {
			// showModalDict(dict, %[vars]) — overlay + 初期変数注入
			auto vars = DictToVarsMap(param[1]->AsObjectNoAddRef());
			dict = _this->ShowModalOverlayDict(dict_in, &vars);
		} else if (numparams >= 2 && param[1]->Type() != tvtVoid) {
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
	// registerHotKey(key [, shift = 0 [, duringTextInput = false]])
	//
	// ホストホットキー登録 (インスタンス不要のユーティリティ)。 登録したキー /
	// マウスボタン (VK_LBUTTON 等) / パッドボタン (VK_PAD*) は、 モーダルが
	// 表示されていない限り Elements ダイアログへ渡さず、 通常のゲーム入力経路
	// (Window.onKeyDown / onMouseDown) へそのまま流れる。 配送優先順位:
	//   モーダル > ホットキー > フォーカスパネル (未処理素通し) > ゲーム。
	// shift は ssShift|ssAlt|ssCtrl の組合せ (down は完全一致 / up は key のみ)。
	// duringTextInput=false (既定) はテキスト入力ウィジェット focus 中は抑止
	// (入力欄と衝突するキーを奪わない)。 印字キーの登録は非推奨 (onKeyPress の
	// 文字イベントまでは抑止しない)。
	TJS_BEGIN_NATIVE_METHOD_DECL(/*func. name*/registerHotKey)
	{
		if (numparams < 1) return TJS_E_BADPARAMCOUNT;
		tjs_uint key = static_cast<tjs_uint>((tjs_int)*param[0]);
		tjs_uint32 shift = (numparams >= 2 && param[1]->Type() != tvtVoid)
			? static_cast<tjs_uint32>((tjs_int)*param[1]) : 0;
		bool duringTextInput = (numparams >= 3 && param[2]->Type() != tvtVoid)
			? (bool)(tjs_int)*param[2] : false;
		tTVPElementsDialogManager::Instance().RegisterHostHotkey(
			key, shift, duringTextInput);
		return TJS_S_OK;
	}
	TJS_END_NATIVE_METHOD_DECL(/*func. name*/registerHotKey)
	//---------------------------------------------------------------------------
	// unregisterHotKey(key [, shift = 0]) — registerHotKey の解除 (key+shift 一致)
	TJS_BEGIN_NATIVE_METHOD_DECL(/*func. name*/unregisterHotKey)
	{
		if (numparams < 1) return TJS_E_BADPARAMCOUNT;
		tjs_uint key = static_cast<tjs_uint>((tjs_int)*param[0]);
		tjs_uint32 shift = (numparams >= 2 && param[1]->Type() != tvtVoid)
			? static_cast<tjs_uint32>((tjs_int)*param[1]) : 0;
		tTVPElementsDialogManager::Instance().UnregisterHostHotkey(key, shift);
		return TJS_S_OK;
	}
	TJS_END_NATIVE_METHOD_DECL(/*func. name*/unregisterHotKey)
	//---------------------------------------------------------------------------
	// clearHotKeys() — ホストホットキーを全解除
	TJS_BEGIN_NATIVE_METHOD_DECL(/*func. name*/clearHotKeys)
	{
		tTVPElementsDialogManager::Instance().ClearHostHotkeys();
		return TJS_S_OK;
	}
	TJS_END_NATIVE_METHOD_DECL(/*func. name*/clearHotKeys)
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
	// language プロパティ (static 相当):
	//   i18n の表示言語。 画面 JSON の top-level "strings" (textID → 言語別
	//   文字列) を引くときのキーで、 "ja" / "en" / "tc" / "sc" 等の任意文字列。
	//   setter — 表示中の全ダイアログへ即時適用 (text_id / text_list_id /
	//            options_id を持つ widget が再解決されてその場で表示が変わる。
	//            画面の開き直しは不要) + 以後に開く画面の既定にもなる。
	//   getter — 設定済みの言語。 未設定なら空文字 (= 各画面 JSON の "lang" 任せ)。
	//   "strings" を持たない画面では何も起きない。
	TJS_BEGIN_NATIVE_PROP_DECL(language)
	{
		TJS_BEGIN_NATIVE_PROP_GETTER
		{
			*result = tTVPElementsDialogManager::Instance().GetLanguage();
			return TJS_S_OK;
		}
		TJS_END_NATIVE_PROP_GETTER

		TJS_BEGIN_NATIVE_PROP_SETTER
		{
			tTVPElementsDialogManager::Instance().SetLanguage(ttstr(*param));
			return TJS_S_OK;
		}
		TJS_END_NATIVE_PROP_SETTER
	}
	TJS_END_NATIVE_PROP_DECL(language)
	//---------------------------------------------------------------------------
	// virtualKeyboard プロパティ (static 相当):
	//   テキスト欄に focus が入ったとき、 OS のソフトキーボード (NX の swkbd
	//   アプレット / PS5 の IME ダイアログ) の代わりに Elements 内蔵の英数
	//   キーボードを出すかどうか。
	//     "auto"   … 既定。 物理キーボードが接続されていないときだけ出す
	//     "always" … 物理キーボードがあっても常に出す (テスト用)
	//     "never"  … 出さない (OS 側に任せる)。 表示中なら閉じる
	//   初期値は環境変数 KRKRZ_FORCE_VIRTUAL_KEYBOARD=1 なら "always"。
	TJS_BEGIN_NATIVE_PROP_DECL(virtualKeyboard)
	{
		TJS_BEGIN_NATIVE_PROP_GETTER
		{
			*result = tTVPElementsDialogManager::Instance().GetVirtualKeyboardMode();
			return TJS_S_OK;
		}
		TJS_END_NATIVE_PROP_GETTER

		TJS_BEGIN_NATIVE_PROP_SETTER
		{
			tTVPElementsDialogManager::Instance().SetVirtualKeyboardMode(ttstr(*param));
			return TJS_S_OK;
		}
		TJS_END_NATIVE_PROP_SETTER
	}
	TJS_END_NATIVE_PROP_DECL(virtualKeyboard)
	//---------------------------------------------------------------------------
	// hasPhysicalKeyboard プロパティ (static 相当・読み取り専用):
	//   物理 (ハードウェア) キーボードが接続されているか。 デスクトップは常に真。
	//   NX / PS5 は USB キーボードの接続状態。 ゲーム側が独自ソフトキーボードを
	//   出すかどうかの判断に使う。
	TJS_BEGIN_NATIVE_PROP_DECL(hasPhysicalKeyboard)
	{
		TJS_BEGIN_NATIVE_PROP_GETTER
		{
			*result = (tjs_int)(tTVPElementsDialogManager::Instance()
				.HasPhysicalKeyboard() ? 1 : 0);
			return TJS_S_OK;
		}
		TJS_END_NATIVE_PROP_GETTER

		TJS_DENY_NATIVE_PROP_SETTER
	}
	TJS_END_NATIVE_PROP_DECL(hasPhysicalKeyboard)
	//---------------------------------------------------------------------------
	// renderScale プロパティ (static 相当):
	//   overlay の描画密度モード。
	//     0 (既定) = auto: 最終 present サイズで直接ラスタライズ (authored が
	//                surface より大きい画面は縮小率ぶん小さい buffer で描く)
	//     >0       = authored 論理サイズ × この倍率で描き、 present 時に拡縮
	//                (1.0 = 原寸レンダ→拡縮表示、 2.0 = 旧 supersampling 相当)
	//   表示中の画面にも次フレームから反映される (描画品質/負荷の比較用)。
	TJS_BEGIN_NATIVE_PROP_DECL(renderScale)
	{
		TJS_BEGIN_NATIVE_PROP_GETTER
		{
			*result = (tjs_real)tTVPElementsDialogManager::Instance().GetRenderScale();
			return TJS_S_OK;
		}
		TJS_END_NATIVE_PROP_GETTER

		TJS_BEGIN_NATIVE_PROP_SETTER
		{
			tTVPElementsDialogManager::Instance().SetRenderScale((float)(tjs_real)*param);
			return TJS_S_OK;
		}
		TJS_END_NATIVE_PROP_SETTER
	}
	TJS_END_NATIVE_PROP_DECL(renderScale)
	//---------------------------------------------------------------------------
	// renderCache プロパティ (static 相当):
	//   overlay の再ラスタライズ抑止。 true (既定) なら変化の無いフレームは
	//   ThorVG (CPU) の再ラスタライズとテクスチャ再アップロードを省略し、
	//   前回の描画結果をそのまま提示する (アイドル時 CPU 負荷の削減)。
	//   false で従来どおり毎フレーム再描画 (負荷 A/B 比較・問題切り分け用)。
	TJS_BEGIN_NATIVE_PROP_DECL(renderCache)
	{
		TJS_BEGIN_NATIVE_PROP_GETTER
		{
			*result = tTVPElementsDialogManager::Instance().GetRenderCache();
			return TJS_S_OK;
		}
		TJS_END_NATIVE_PROP_GETTER

		TJS_BEGIN_NATIVE_PROP_SETTER
		{
			tTVPElementsDialogManager::Instance().SetRenderCache(
				(bool)(tjs_int)*param);
			return TJS_S_OK;
		}
		TJS_END_NATIVE_PROP_SETTER
	}
	TJS_END_NATIVE_PROP_DECL(renderCache)
	//---------------------------------------------------------------------------
	// focusRing プロパティ (static 相当・アプリ全体設定):
	//   フォーカス中の要素に elements が描く汎用の枠。 既定 true。
	//   状態別の絵を自前で持つ画像 UI (PSD 由来の atlas_button / atlas_toggle 等)
	//   では枠が素材に重なって邪魔なので false にする。 画面単位ではなく
	//   タイトル全体の見た目方針なのでグローバルテーマのフラグ。
	//   フォーカス自体は生きているのでキー/パッド操作の挙動は変わらない。
	TJS_BEGIN_NATIVE_PROP_DECL(focusRing)
	{
		TJS_BEGIN_NATIVE_PROP_GETTER
		{
			*result = elements_modal::focus_ring_enabled();
			return TJS_S_OK;
		}
		TJS_END_NATIVE_PROP_GETTER

		TJS_BEGIN_NATIVE_PROP_SETTER
		{
			elements_modal::set_focus_ring_enabled((bool)(tjs_int)*param);
			// テーマ変更はセッションから観測できないので明示的に再描画させる
			tTVPElementsDialogManager::Instance().InvalidateOverlays();
			return TJS_S_OK;
		}
		TJS_END_NATIVE_PROP_SETTER
	}
	TJS_END_NATIVE_PROP_DECL(focusRing)
	//---------------------------------------------------------------------------
	// partialRedraw プロパティ (static 相当):
	//   overlay の部分再描画。 true (既定) なら、 ダーティが矩形で特定できる
	//   変化 (テキスト欄キャレットの点滅等) はその矩形だけをクリア + クリップ
	//   付きで再ラスタライズし、 テクスチャへも部分転送する。 renderCache が
	//   有効なときのみ機能する (前回フレームが残っていることが前提)。
	//   false で変化フレームは常に全面再描画 (A/B 比較・問題切り分け用)。
	TJS_BEGIN_NATIVE_PROP_DECL(partialRedraw)
	{
		TJS_BEGIN_NATIVE_PROP_GETTER
		{
			*result = tTVPElementsDialogManager::Instance().GetPartialRedraw();
			return TJS_S_OK;
		}
		TJS_END_NATIVE_PROP_GETTER

		TJS_BEGIN_NATIVE_PROP_SETTER
		{
			tTVPElementsDialogManager::Instance().SetPartialRedraw(
				(bool)(tjs_int)*param);
			return TJS_S_OK;
		}
		TJS_END_NATIVE_PROP_SETTER
	}
	TJS_END_NATIVE_PROP_DECL(partialRedraw)
	//---------------------------------------------------------------------------
	// renderCount プロパティ (static 相当、 読取専用):
	//   実際にラスタライズ (render_to_buffer) した累計回数。 アイドル時に増えて
	//   いないか (renderCache が効いているか) の確認・負荷比較用カウンタ。
	TJS_BEGIN_NATIVE_PROP_DECL(renderCount)
	{
		TJS_BEGIN_NATIVE_PROP_GETTER
		{
			*result = (tjs_int64)tTVPElementsDialogManager::Instance().GetRenderCount();
			return TJS_S_OK;
		}
		TJS_END_NATIVE_PROP_GETTER

		TJS_DENY_NATIVE_PROP_SETTER
	}
	TJS_END_NATIVE_PROP_DECL(renderCount)
	//---------------------------------------------------------------------------
	// renderStats プロパティ (static 相当、 読取専用):
	//   overlay 描画パイプラインの区間計測を辞書で返す。 すべて累積値
	//   (renderStatsReset() で 0 クリア)、 時間は microsecond。
	//   %[ frames (PaintOverlay 呼出 = 提示フレーム数),
	//      updates / rasters / cachedPresents / presents (回数),
	//      totalUs (PaintOverlay 全体), updateUs (状態更新),
	//      rasterUs (ThorVG ラスタ), acquireUs (バッファ確保/lock 待ち),
	//      uploadUs (テクスチャ転送), presentUs (提示) ]
	//   2 回読んで差分を取り、 経過実時間との比で負荷割合を出す (負荷計測用)。
	TJS_BEGIN_NATIVE_PROP_DECL(renderStats)
	{
		TJS_BEGIN_NATIVE_PROP_GETTER
		{
			tTVPElementsRenderStats s;
			tTVPElementsDialogManager::Instance().GetRenderStats(s);
			iTJSDispatch2* dict = TJSCreateDictionaryObject();
			tTJSVariant tv;
			tv = (tjs_int64)s.frames;         dict->PropSet(TJS_MEMBERENSURE, TJS_W("frames"),         nullptr, &tv, dict);
			tv = (tjs_int64)s.updates;        dict->PropSet(TJS_MEMBERENSURE, TJS_W("updates"),        nullptr, &tv, dict);
			tv = (tjs_int64)s.rasters;        dict->PropSet(TJS_MEMBERENSURE, TJS_W("rasters"),        nullptr, &tv, dict);
			tv = (tjs_int64)s.partials;       dict->PropSet(TJS_MEMBERENSURE, TJS_W("partials"),       nullptr, &tv, dict);
			tv = (tjs_int64)s.cachedPresents; dict->PropSet(TJS_MEMBERENSURE, TJS_W("cachedPresents"), nullptr, &tv, dict);
			tv = (tjs_int64)s.presents;       dict->PropSet(TJS_MEMBERENSURE, TJS_W("presents"),       nullptr, &tv, dict);
			tv = (tjs_int64)s.totalUs;        dict->PropSet(TJS_MEMBERENSURE, TJS_W("totalUs"),        nullptr, &tv, dict);
			tv = (tjs_int64)s.updateUs;       dict->PropSet(TJS_MEMBERENSURE, TJS_W("updateUs"),       nullptr, &tv, dict);
			tv = (tjs_int64)s.rasterUs;       dict->PropSet(TJS_MEMBERENSURE, TJS_W("rasterUs"),       nullptr, &tv, dict);
			tv = (tjs_int64)s.acquireUs;      dict->PropSet(TJS_MEMBERENSURE, TJS_W("acquireUs"),     nullptr, &tv, dict);
			tv = (tjs_int64)s.uploadUs;       dict->PropSet(TJS_MEMBERENSURE, TJS_W("uploadUs"),       nullptr, &tv, dict);
			tv = (tjs_int64)s.presentUs;      dict->PropSet(TJS_MEMBERENSURE, TJS_W("presentUs"),      nullptr, &tv, dict);
			*result = tTJSVariant(dict, dict);
			dict->Release();
			return TJS_S_OK;
		}
		TJS_END_NATIVE_PROP_GETTER

		TJS_DENY_NATIVE_PROP_SETTER
	}
	TJS_END_NATIVE_PROP_DECL(renderStats)
	//---------------------------------------------------------------------------
	// renderStatsReset()
	//   renderStats の累積カウンタを 0 クリアする (計測区間の開始に呼ぶ)。
	TJS_BEGIN_NATIVE_METHOD_DECL(/*func. name*/renderStatsReset)
	{
		tTVPElementsDialogManager::Instance().ResetRenderStats();
		return TJS_S_OK;
	}
	TJS_END_NATIVE_METHOD_DECL(renderStatsReset)
	//---------------------------------------------------------------------------
	// setPadIconBase(dir)
	//
	// pad_icon (Kenney input prompts) のベースディレクトリを設定する。 dir は
	// krkrz storage パス (XP3 内でも OK。 SVG 読込は Storages-backed
	// resource_loader 経由)。 配下に xbox/ps/switch/keyboard の各ディレクトリ +
	// vector/*.svg がある構成 (uisample の resources/kenny_input_prompts と同じ)。
	// 未設定のままだと pad_icon は灰色プレースホルダになる。
	TJS_BEGIN_NATIVE_METHOD_DECL(/*func. name*/setPadIconBase)
	{
		if (numparams < 1) return TJS_E_BADPARAMCOUNT;
		ttstr dir(*param[0]);
		std::string utf8;
		tjs_string ts(dir.c_str());
		TVPUtf16ToUtf8(utf8, ts);
		cycfi::elements::set_pad_icon_base_dir(utf8);
		if (result) *result = true;
		return TJS_S_OK;
	}
	TJS_END_NATIVE_METHOD_DECL(/*func. name*/setPadIconBase)
	//---------------------------------------------------------------------------
	// setPadTheme(name)
	//
	// pad_icon の全体テーマ ("xbox"/"ps"/"switch"/"keyboard"/"none") を設定する。
	// 画面 JSON の top-level "pad_theme" が指定されていればそちらが優先される
	// (build 時に上書き)。 戻り値は名前を解釈できたかどうか。
	TJS_BEGIN_NATIVE_METHOD_DECL(/*func. name*/setPadTheme)
	{
		if (numparams < 1) return TJS_E_BADPARAMCOUNT;
		ttstr name(*param[0]);
		std::string utf8;
		tjs_string ts(name.c_str());
		TVPUtf16ToUtf8(utf8, ts);
		auto t = cycfi::elements::parse_pad_theme(utf8);
		bool ok = (t != cycfi::elements::pad_theme::none) || (utf8 == "none");
		if (ok) cycfi::elements::set_pad_theme(t);
		if (result) *result = ok;
		return TJS_S_OK;
	}
	TJS_END_NATIVE_METHOD_DECL(/*func. name*/setPadTheme)
	//---------------------------------------------------------------------------
	// registerImage(name, path)
	//
	// 実行時画像ストアへ、 storage パス path のファイルを name で登録する。
	// jsonc の image ウィジェット等からは "mem://<name>" で参照する。 セーブ
	// サムネイル等、 実行時に変わる画像を Elements へ渡すための仕組み。
	// pixmap は画面 build 時に読み直されるので、 再登録 → 画面再オープンで表示が
	// 更新される。 戻り値 = 読込・登録に成功したか。
	TJS_BEGIN_NATIVE_METHOD_DECL(/*func. name*/registerImage)
	{
		if (numparams < 2) return TJS_E_BADPARAMCOUNT;
		ttstr name(*param[0]);
		ttstr path(*param[1]);
		bool ok = TVPRegisterElementsImageFile(name, path);
		if (result) *result = ok;
		return TJS_S_OK;
	}
	TJS_END_NATIVE_METHOD_DECL(/*func. name*/registerImage)
	//---------------------------------------------------------------------------
	// unregisterImage(name) — 実行時画像ストアから name を削除。
	TJS_BEGIN_NATIVE_METHOD_DECL(/*func. name*/unregisterImage)
	{
		if (numparams < 1) return TJS_E_BADPARAMCOUNT;
		ttstr name(*param[0]);
		TVPUnregisterElementsImage(name);
		if (result) *result = true;
		return TJS_S_OK;
	}
	TJS_END_NATIVE_METHOD_DECL(/*func. name*/unregisterImage)
	//---------------------------------------------------------------------------
	// clearImages() — 実行時画像ストアを全消去。
	TJS_BEGIN_NATIVE_METHOD_DECL(/*func. name*/clearImages)
	{
		TVPClearElementsImages();
		if (result) *result = true;
		return TJS_S_OK;
	}
	TJS_END_NATIVE_METHOD_DECL(/*func. name*/clearImages)
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
