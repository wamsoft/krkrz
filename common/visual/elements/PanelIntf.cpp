//---------------------------------------------------------------------------
// TJS ElementsPanel クラス実装 (PanelIntf.h)
//
// `iTVPDialogEventHandler` を直接 implement して、 通知が来たら TVPPostEvent で
// TJS の `onAction` / `onDrag` / `onVar` / `onClose` を起動する
// (`Dialog` と同じ名前・同じ引数)。
//---------------------------------------------------------------------------
#include "tjsCommHead.h"
#include "PanelIntf.h"

#include "ElementsLayerPanel.h"
#include "ElementsSessionBuild.h"   // Utf8ToTtstr / TtstrToUtf8
#include "LayerIntf.h"              // tTJSNI_BaseLayer / tTJSNC_Layer::ClassID
#include "EventIntf.h"              // TVPPostEvent
#include "StorageIntf.h"            // TVPReadStream
#include "CharacterSet.h"           // TVPUtf16ToUtf8
#include "DebugIntf.h"
#include "MsgIntf.h"                // TVPThrowExceptionMessage
#include "tjsError.h"               // eTJS
#include "tjsArray.h"               // TJSCreateArrayObject (listVars)
#include "tjsDictionary.h"          // TJSCreateDictionaryObject (listVars)
#include "VariantJsonUtil.h"        // TVPVariantToJsonUtf8 (showDict)

#include <string>

namespace {

using namespace tvp_elements;

//---------------------------------------------------------------------------
// storage パスのディレクトリ部 (末尾 '/' 込み)。 区切りが無ければ空。
//---------------------------------------------------------------------------
ttstr DirOfStoragePath(const ttstr& path)
{
	tjs_string s(path.c_str());
	auto pos = s.find_last_of(TJS_W("/\\"));
	if (pos == tjs_string::npos) return ttstr();
	return ttstr(s.substr(0, pos + 1).c_str());
}

//---------------------------------------------------------------------------
// 例外への文脈付加 (DialogIntf.cpp と同じ趣旨)。 elements 由来の例外は
// what() が短いので、 どの API のどの対象で起きたかを付けて TJS 例外へ。
//---------------------------------------------------------------------------
template<typename TFunc>
auto WithPanelExceptionContext(const tjs_char* api, const ttstr& target,
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
	return decltype(func()){};   // not reached
}

//! tjs_int のボタン番号 → tTVPMouseButton。 吉里吉里の onMouseDown が渡す
//! 値 (mbLeft = 0 …) をそのまま受ける。
tTVPMouseButton ToMouseButton(tjs_int b)
{
	switch (b) {
		case mbRight:  return mbRight;
		case mbMiddle: return mbMiddle;
		case mbX1:     return mbX1;
		case mbX2:     return mbX2;
		default:       return mbLeft;
	}
}

} // anonymous namespace

//---------------------------------------------------------------------------
// tTJSNI_ElementsPanel
//---------------------------------------------------------------------------
tTJSNI_ElementsPanel::tTJSNI_ElementsPanel() {}

tTJSNI_ElementsPanel::~tTJSNI_ElementsPanel()
{
	Panel.reset();
	if (LayerObj) { LayerObj->Release(); LayerObj = nullptr; }
}

tjs_error TJS_INTF_METHOD
tTJSNI_ElementsPanel::Construct(tjs_int numparams, tTJSVariant** param,
                                iTJSDispatch2* tjs_obj)
{
	Owner = tjs_obj;
	if (numparams < 1) return TJS_E_BADPARAMCOUNT;

	// 描画先の Layer を受ける。 Layer は TJS 側で生かしておくのが筋だが、
	// パネルが先に死ぬ保証は無いので参照を 1 つ持つ。
	tTJSVariantClosure clo = param[0]->AsObjectClosureNoAddRef();
	if (!clo.Object) return TJS_E_INVALIDPARAM;
	tTJSNI_BaseLayer* lay = nullptr;
	if (TJS_FAILED(clo.Object->NativeInstanceSupport(TJS_NIS_GETINSTANCE,
			tTJSNC_Layer::ClassID, (iTJSNativeInstance**)&lay)) || !lay) {
		TVPThrowExceptionMessage(
			TJS_W("ElementsPanel: 第 1 引数には Layer を渡してください"));
	}
	Layer = lay;
	LayerObj = clo.Object;
	LayerObj->AddRef();

	Panel = std::make_unique<tTVPElementsLayerPanel>(Layer, this);
	return TJS_S_OK;
}

void TJS_INTF_METHOD tTJSNI_ElementsPanel::Invalidate()
{
	// GC / invalidate の時点でパネルを畳む。 継続イベントフックが外れ、
	// 通知キューに残っている自分あての通知も捨てられる。
	Panel.reset();
	if (LayerObj) { LayerObj->Release(); LayerObj = nullptr; }
	Layer = nullptr;
	Owner = nullptr;
}

//---------------------------------------------------------------------------
// iTVPDialogEventHandler → TJS イベント (名前は Dialog と同じ)
//---------------------------------------------------------------------------
void tTJSNI_ElementsPanel::OnAction(const ttstr& id, const tTJSVariant& payload)
{
	if (!Owner) return;
	tTJSVariant args[2] = { id, payload };
	static ttstr eventname(TJS_W("onAction"));
	TVPPostEvent(Owner, Owner, eventname, 0, TVP_EPT_IMMEDIATE, 2, args);
}

void tTJSNI_ElementsPanel::OnDrag(const tTJSVariant& payload)
{
	if (!Owner) return;
	tTJSVariant args[1] = { payload };
	static ttstr eventname(TJS_W("onDrag"));
	TVPPostEvent(Owner, Owner, eventname, 0, TVP_EPT_IMMEDIATE, 1, args);
}

void tTJSNI_ElementsPanel::OnVar(const ttstr& name, const ttstr& value)
{
	if (!Owner) return;
	tTJSVariant args[2] = { name, value };
	static ttstr eventname(TJS_W("onVar"));
	TVPPostEvent(Owner, Owner, eventname, 0, TVP_EPT_IMMEDIATE, 2, args);
}

void tTJSNI_ElementsPanel::OnClosed(const ttstr& action)
{
	if (!Owner) return;
	tTJSVariant args[1] = { action };
	static ttstr eventname(TJS_W("onClose"));
	TVPPostEvent(Owner, Owner, eventname, 0, TVP_EPT_IMMEDIATE, 1, args);
}

bool tTJSNI_ElementsPanel::WantsVarNotify(std::vector<ttstr>& out_names)
{
	switch (WatchMode) {
	case VarWatch::Off:
		return false;
	case VarWatch::All:
		out_names.clear();
		return true;
	case VarWatch::Names:
		out_names = WatchNames;
		return !out_names.empty();
	case VarWatch::Auto:
	default:
		break;
	}
	// 既定: onVar を実装しているときだけ全変数を観測する (Dialog と同じ)。
	if (!Owner) return false;
	tTJSVariant v;
	if (TJS_FAILED(Owner->PropGet(0, TJS_W("onVar"), nullptr, &v, Owner)))
		return false;
	if (v.Type() == tvtVoid) return false;
	out_names.clear();
	return true;
}

//---------------------------------------------------------------------------
// 開閉
//---------------------------------------------------------------------------
bool tTJSNI_ElementsPanel::OpenUtf8(const std::string& json_utf8,
                                    const std::string& resource_base_utf8,
                                    const tjs_char* api, const ttstr& target)
{
	if (!Panel) return false;
	return WithPanelExceptionContext(api, target, [&]() -> bool {
		return Panel->Open(json_utf8, resource_base_utf8);
	});
}

bool tTJSNI_ElementsPanel::OpenFile(const ttstr& path)
{
	tjs_uint64 flen = 0;
	auto buf = TVPReadStream(path.c_str(), &flen);
	if (!buf || flen == 0) {
		TVPAddImportantLog(ttstr(TJS_W("ElementsPanel: cannot read: ")) + path);
		return false;
	}
	std::string json(reinterpret_cast<const char*>(buf.get()),
	                 static_cast<size_t>(flen));
	// 画面 JSON 内の相対資材パスは、 その JSON があるディレクトリ起点で解決する
	// (overlay ダイアログの単発 JSON と同じ規約)。
	return OpenUtf8(json, TtstrToUtf8(DirOfStoragePath(path)),
	                TJS_W("ElementsPanel.showFile"), path);
}

bool tTJSNI_ElementsPanel::OpenJson(const ttstr& json_utf16)
{
	std::string utf8;
	tjs_string ts(json_utf16.c_str());
	TVPUtf16ToUtf8(utf8, ts);
	return OpenUtf8(utf8, std::string(),
	                TJS_W("ElementsPanel.showJson"), ttstr());
}

bool tTJSNI_ElementsPanel::OpenDict(iTJSDispatch2* dict)
{
	if (!dict) return false;
	std::string utf8;
	tTJSVariant v(dict, dict);
	TVPVariantToJsonUtf8(v, utf8);
	return OpenUtf8(utf8, std::string(),
	                TJS_W("ElementsPanel.showDict"), ttstr());
}

void tTJSNI_ElementsPanel::Close()
{
	if (Panel) Panel->Close();
}

bool tTJSNI_ElementsPanel::IsOpen() const
{
	return Panel && Panel->IsOpen();
}

void tTJSNI_ElementsPanel::NotifyResized()
{
	if (Panel) Panel->NotifyLayerResized();
}

//---------------------------------------------------------------------------
// 入力 (座標はレイヤ local のまま)
//---------------------------------------------------------------------------
void tTJSNI_ElementsPanel::MouseDown(
	tjs_int x, tjs_int y, tjs_int button, tjs_uint32 flags)
{
	if (Panel) Panel->MouseDown(x, y, ToMouseButton(button), flags);
}

void tTJSNI_ElementsPanel::MouseUp(
	tjs_int x, tjs_int y, tjs_int button, tjs_uint32 flags)
{
	if (Panel) Panel->MouseUp(x, y, ToMouseButton(button), flags);
}

void tTJSNI_ElementsPanel::MouseMove(tjs_int x, tjs_int y, tjs_uint32 flags)
{
	if (Panel) Panel->MouseMove(x, y, flags);
}

void tTJSNI_ElementsPanel::MouseWheel(
	tjs_int delta, tjs_int x, tjs_int y, tjs_uint32 flags)
{
	if (Panel) Panel->MouseWheel(delta, x, y, flags);
}

void tTJSNI_ElementsPanel::MouseLeave()
{
	if (Panel) Panel->MouseLeave();
}

bool tTJSNI_ElementsPanel::KeyDown(tjs_uint key, tjs_uint32 shift)
{
	return Panel ? Panel->KeyDown(key, shift) : false;
}

bool tTJSNI_ElementsPanel::KeyUp(tjs_uint key, tjs_uint32 shift)
{
	return Panel ? Panel->KeyUp(key, shift) : false;
}

void tTJSNI_ElementsPanel::TextInput(const ttstr& text)
{
	if (!Panel) return;
	Panel->TextInput(TtstrToUtf8(text).c_str());
}

//---------------------------------------------------------------------------
// 状態
//---------------------------------------------------------------------------
bool tTJSNI_ElementsPanel::SetVar(const ttstr& name, const ttstr& value)
{
	return Panel ? Panel->SetVar(name, value) : false;
}

bool tTJSNI_ElementsPanel::GetVar(const ttstr& name, ttstr& out)
{
	return Panel ? Panel->GetVar(name, out) : false;
}

iTJSDispatch2* tTJSNI_ElementsPanel::ListVars()
{
	// ElementsDialog.listVars() と同じ形: [ %[name, value, usedBy: [%[id, kind], ...]] ]
	iTJSDispatch2* arr = TJSCreateArrayObject();
	if (!arr) return nullptr;
	if (!Panel) return arr;
	auto vars = Panel->DescribeVars();
	tjs_int idx = 0;
	for (auto const& v : vars) {
		iTJSDispatch2* d = TJSCreateDictionaryObject();
		if (!d) continue;
		auto put = [d](const tjs_char* k, const tTJSVariant& val) {
			tTJSVariant tmp = val;
			d->PropSet(TJS_MEMBERENSURE, k, nullptr, &tmp, d);
		};
		put(TJS_W("name"),  tTJSVariant(v.name));
		put(TJS_W("value"), tTJSVariant(v.value));
		iTJSDispatch2* ua = TJSCreateArrayObject();
		if (ua) {
			tjs_int ui = 0;
			for (auto const& u : v.used_by) {
				iTJSDispatch2* ud = TJSCreateDictionaryObject();
				if (!ud) continue;
				tTJSVariant tid(u.first), tkind(u.second);
				ud->PropSet(TJS_MEMBERENSURE, TJS_W("id"),   nullptr, &tid,   ud);
				ud->PropSet(TJS_MEMBERENSURE, TJS_W("kind"), nullptr, &tkind, ud);
				tTJSVariant uv(ud, ud);
				ua->PropSetByNum(TJS_MEMBERENSURE, ui++, &uv, ua);
				ud->Release();
			}
			put(TJS_W("usedBy"), tTJSVariant(ua, ua));
			ua->Release();
		}
		tTJSVariant dv(d, d);
		arr->PropSetByNum(TJS_MEMBERENSURE, idx++, &dv, arr);
		d->Release();
	}
	return arr;
}

void tTJSNI_ElementsPanel::RefreshVarWatch()
{
	if (Panel) Panel->RefreshVarWatch();
}

void tTJSNI_ElementsPanel::DoInvalidate()
{
	if (Panel) Panel->Invalidate();
}

bool tTJSNI_ElementsPanel::FocusById(const ttstr& id)
{
	return Panel ? Panel->FocusById(id) : false;
}

bool tTJSNI_ElementsPanel::ActivateById(const ttstr& id)
{
	return Panel ? Panel->ActivateById(id) : false;
}

ttstr tTJSNI_ElementsPanel::FocusedId() const
{
	return Panel ? Panel->FocusedId() : ttstr();
}

//---------------------------------------------------------------------------
// tTJSNC_ElementsPanel
//---------------------------------------------------------------------------
tjs_uint32 tTJSNC_ElementsPanel::ClassID = (tjs_uint32)-1;

tTJSNC_ElementsPanel::tTJSNC_ElementsPanel() : inherited(TJS_W("ElementsPanel"))
{
	TJS_BEGIN_NATIVE_MEMBERS(ElementsPanel)
	TJS_DECL_EMPTY_FINALIZE_METHOD
	//---------------------------------------------------------------------------
	TJS_BEGIN_NATIVE_CONSTRUCTOR_DECL(/*var.name*/_this,
		/*var.type*/tTJSNI_ElementsPanel, /*TJS class name*/ElementsPanel)
	{
		return TJS_S_OK;
	}
	TJS_END_NATIVE_CONSTRUCTOR_DECL(/*TJS class name*/ElementsPanel)
	//---------------------------------------------------------------------------
	// showFile(path) — 画面 JSON をファイルから開く。 相対資材パスは
	// その JSON があるディレクトリ起点で解決される。
	TJS_BEGIN_NATIVE_METHOD_DECL(/*func. name*/showFile)
	{
		TJS_GET_NATIVE_INSTANCE(/*var. name*/_this, /*var. type*/tTJSNI_ElementsPanel);
		if (numparams < 1) return TJS_E_BADPARAMCOUNT;
		bool ok = _this->OpenFile(ttstr(*param[0]));
		if (result) *result = ok;
		return TJS_S_OK;
	}
	TJS_END_NATIVE_METHOD_DECL(/*func. name*/showFile)
	//---------------------------------------------------------------------------
	// showJson(json) — 画面 JSON を文字列から開く。
	TJS_BEGIN_NATIVE_METHOD_DECL(/*func. name*/showJson)
	{
		TJS_GET_NATIVE_INSTANCE(/*var. name*/_this, /*var. type*/tTJSNI_ElementsPanel);
		if (numparams < 1) return TJS_E_BADPARAMCOUNT;
		bool ok = _this->OpenJson(ttstr(*param[0]));
		if (result) *result = ok;
		return TJS_S_OK;
	}
	TJS_END_NATIVE_METHOD_DECL(/*func. name*/showJson)
	//---------------------------------------------------------------------------
	// showDict(dict) — TJS の Dictionary / Array で書いたレイアウトを開く。
	TJS_BEGIN_NATIVE_METHOD_DECL(/*func. name*/showDict)
	{
		TJS_GET_NATIVE_INSTANCE(/*var. name*/_this, /*var. type*/tTJSNI_ElementsPanel);
		if (numparams < 1) return TJS_E_BADPARAMCOUNT;
		if (param[0]->Type() != tvtObject) return TJS_E_INVALIDPARAM;
		bool ok = _this->OpenDict(param[0]->AsObjectNoAddRef());
		if (result) *result = ok;
		return TJS_S_OK;
	}
	TJS_END_NATIVE_METHOD_DECL(/*func. name*/showDict)
	//---------------------------------------------------------------------------
	TJS_BEGIN_NATIVE_METHOD_DECL(/*func. name*/close)
	{
		TJS_GET_NATIVE_INSTANCE(/*var. name*/_this, /*var. type*/tTJSNI_ElementsPanel);
		_this->Close();
		return TJS_S_OK;
	}
	TJS_END_NATIVE_METHOD_DECL(/*func. name*/close)
	//---------------------------------------------------------------------------
	// notifyResized() — レイヤの画像サイズを変えた後に呼ぶ。
	TJS_BEGIN_NATIVE_METHOD_DECL(/*func. name*/notifyResized)
	{
		TJS_GET_NATIVE_INSTANCE(/*var. name*/_this, /*var. type*/tTJSNI_ElementsPanel);
		_this->NotifyResized();
		return TJS_S_OK;
	}
	TJS_END_NATIVE_METHOD_DECL(/*func. name*/notifyResized)
	//---------------------------------------------------------------------------
	// 入力。 座標は **レイヤ local** をそのまま渡す (Layer.onMouseDown の引数)。
	TJS_BEGIN_NATIVE_METHOD_DECL(/*func. name*/mouseDown)
	{
		TJS_GET_NATIVE_INSTANCE(/*var. name*/_this, /*var. type*/tTJSNI_ElementsPanel);
		if (numparams < 2) return TJS_E_BADPARAMCOUNT;
		tjs_int x = (tjs_int)*param[0], y = (tjs_int)*param[1];
		tjs_int b = (numparams >= 3 && param[2]->Type() != tvtVoid) ? (tjs_int)*param[2] : 0;
		tjs_uint32 f = (numparams >= 4 && param[3]->Type() != tvtVoid)
			? (tjs_uint32)(tjs_int)*param[3] : 0;
		_this->MouseDown(x, y, b, f);
		return TJS_S_OK;
	}
	TJS_END_NATIVE_METHOD_DECL(/*func. name*/mouseDown)
	//---------------------------------------------------------------------------
	TJS_BEGIN_NATIVE_METHOD_DECL(/*func. name*/mouseUp)
	{
		TJS_GET_NATIVE_INSTANCE(/*var. name*/_this, /*var. type*/tTJSNI_ElementsPanel);
		if (numparams < 2) return TJS_E_BADPARAMCOUNT;
		tjs_int x = (tjs_int)*param[0], y = (tjs_int)*param[1];
		tjs_int b = (numparams >= 3 && param[2]->Type() != tvtVoid) ? (tjs_int)*param[2] : 0;
		tjs_uint32 f = (numparams >= 4 && param[3]->Type() != tvtVoid)
			? (tjs_uint32)(tjs_int)*param[3] : 0;
		_this->MouseUp(x, y, b, f);
		return TJS_S_OK;
	}
	TJS_END_NATIVE_METHOD_DECL(/*func. name*/mouseUp)
	//---------------------------------------------------------------------------
	TJS_BEGIN_NATIVE_METHOD_DECL(/*func. name*/mouseMove)
	{
		TJS_GET_NATIVE_INSTANCE(/*var. name*/_this, /*var. type*/tTJSNI_ElementsPanel);
		if (numparams < 2) return TJS_E_BADPARAMCOUNT;
		tjs_int x = (tjs_int)*param[0], y = (tjs_int)*param[1];
		tjs_uint32 f = (numparams >= 3 && param[2]->Type() != tvtVoid)
			? (tjs_uint32)(tjs_int)*param[2] : 0;
		_this->MouseMove(x, y, f);
		return TJS_S_OK;
	}
	TJS_END_NATIVE_METHOD_DECL(/*func. name*/mouseMove)
	//---------------------------------------------------------------------------
	// mouseWheel(delta, x, y [, shift]) — delta は吉里吉里の 120 単位。
	TJS_BEGIN_NATIVE_METHOD_DECL(/*func. name*/mouseWheel)
	{
		TJS_GET_NATIVE_INSTANCE(/*var. name*/_this, /*var. type*/tTJSNI_ElementsPanel);
		if (numparams < 3) return TJS_E_BADPARAMCOUNT;
		tjs_int d = (tjs_int)*param[0];
		tjs_int x = (tjs_int)*param[1], y = (tjs_int)*param[2];
		tjs_uint32 f = (numparams >= 4 && param[3]->Type() != tvtVoid)
			? (tjs_uint32)(tjs_int)*param[3] : 0;
		_this->MouseWheel(d, x, y, f);
		return TJS_S_OK;
	}
	TJS_END_NATIVE_METHOD_DECL(/*func. name*/mouseWheel)
	//---------------------------------------------------------------------------
	TJS_BEGIN_NATIVE_METHOD_DECL(/*func. name*/mouseLeave)
	{
		TJS_GET_NATIVE_INSTANCE(/*var. name*/_this, /*var. type*/tTJSNI_ElementsPanel);
		_this->MouseLeave();
		return TJS_S_OK;
	}
	TJS_END_NATIVE_METHOD_DECL(/*func. name*/mouseLeave)
	//---------------------------------------------------------------------------
	// keyDown(key [, shift]) — パネルは既定でキーボードフォーカスを取らないので、
	// 鍵を効かせたい画面だけスクリプトから流す。 戻り値 = パネルが消費したか。
	TJS_BEGIN_NATIVE_METHOD_DECL(/*func. name*/keyDown)
	{
		TJS_GET_NATIVE_INSTANCE(/*var. name*/_this, /*var. type*/tTJSNI_ElementsPanel);
		if (numparams < 1) return TJS_E_BADPARAMCOUNT;
		tjs_uint k = (tjs_uint)(tjs_int)*param[0];
		tjs_uint32 f = (numparams >= 2 && param[1]->Type() != tvtVoid)
			? (tjs_uint32)(tjs_int)*param[1] : 0;
		bool ok = _this->KeyDown(k, f);
		if (result) *result = ok;
		return TJS_S_OK;
	}
	TJS_END_NATIVE_METHOD_DECL(/*func. name*/keyDown)
	//---------------------------------------------------------------------------
	TJS_BEGIN_NATIVE_METHOD_DECL(/*func. name*/keyUp)
	{
		TJS_GET_NATIVE_INSTANCE(/*var. name*/_this, /*var. type*/tTJSNI_ElementsPanel);
		if (numparams < 1) return TJS_E_BADPARAMCOUNT;
		tjs_uint k = (tjs_uint)(tjs_int)*param[0];
		tjs_uint32 f = (numparams >= 2 && param[1]->Type() != tvtVoid)
			? (tjs_uint32)(tjs_int)*param[1] : 0;
		bool ok = _this->KeyUp(k, f);
		if (result) *result = ok;
		return TJS_S_OK;
	}
	TJS_END_NATIVE_METHOD_DECL(/*func. name*/keyUp)
	//---------------------------------------------------------------------------
	// text(str) — input_box へのテキスト入力 (IME / 貼り付け相当)。
	TJS_BEGIN_NATIVE_METHOD_DECL(/*func. name*/text)
	{
		TJS_GET_NATIVE_INSTANCE(/*var. name*/_this, /*var. type*/tTJSNI_ElementsPanel);
		if (numparams < 1) return TJS_E_BADPARAMCOUNT;
		_this->TextInput(ttstr(*param[0]));
		return TJS_S_OK;
	}
	TJS_END_NATIVE_METHOD_DECL(/*func. name*/text)
	//---------------------------------------------------------------------------
	TJS_BEGIN_NATIVE_METHOD_DECL(/*func. name*/setVar)
	{
		TJS_GET_NATIVE_INSTANCE(/*var. name*/_this, /*var. type*/tTJSNI_ElementsPanel);
		if (numparams < 2) return TJS_E_BADPARAMCOUNT;
		bool ok = _this->SetVar(ttstr(*param[0]), ttstr(*param[1]));
		if (result) *result = ok;
		return TJS_S_OK;
	}
	TJS_END_NATIVE_METHOD_DECL(/*func. name*/setVar)
	//---------------------------------------------------------------------------
	// getVar(name) — 未知の変数 / 未オープンなら void。
	TJS_BEGIN_NATIVE_METHOD_DECL(/*func. name*/getVar)
	{
		TJS_GET_NATIVE_INSTANCE(/*var. name*/_this, /*var. type*/tTJSNI_ElementsPanel);
		if (numparams < 1) return TJS_E_BADPARAMCOUNT;
		ttstr out;
		if (_this->GetVar(ttstr(*param[0]), out)) {
			if (result) *result = out;
		} else {
			if (result) result->Clear();
		}
		return TJS_S_OK;
	}
	TJS_END_NATIVE_METHOD_DECL(/*func. name*/getVar)
	//---------------------------------------------------------------------------
	// listVars() — [ %[name, value, usedBy] ] (ElementsDialog.listVars と同じ形)。
	TJS_BEGIN_NATIVE_METHOD_DECL(/*func. name*/listVars)
	{
		TJS_GET_NATIVE_INSTANCE(/*var. name*/_this, /*var. type*/tTJSNI_ElementsPanel);
		iTJSDispatch2* arr = _this->ListVars();
		if (!arr) return TJS_E_FAIL;
		if (result) *result = tTJSVariant(arr, arr);
		arr->Release();
		return TJS_S_OK;
	}
	TJS_END_NATIVE_METHOD_DECL(/*func. name*/listVars)
	//---------------------------------------------------------------------------
	// invalidate() — 明示的な再描画要求 (registerImage の差替後など)。
	TJS_BEGIN_NATIVE_METHOD_DECL(/*func. name*/invalidate)
	{
		TJS_GET_NATIVE_INSTANCE(/*var. name*/_this, /*var. type*/tTJSNI_ElementsPanel);
		_this->DoInvalidate();
		return TJS_S_OK;
	}
	TJS_END_NATIVE_METHOD_DECL(/*func. name*/invalidate)
	//---------------------------------------------------------------------------
	TJS_BEGIN_NATIVE_METHOD_DECL(/*func. name*/focus)
	{
		TJS_GET_NATIVE_INSTANCE(/*var. name*/_this, /*var. type*/tTJSNI_ElementsPanel);
		if (numparams < 1) return TJS_E_BADPARAMCOUNT;
		bool ok = _this->FocusById(ttstr(*param[0]));
		if (result) *result = ok;
		return TJS_S_OK;
	}
	TJS_END_NATIVE_METHOD_DECL(/*func. name*/focus)
	//---------------------------------------------------------------------------
	TJS_BEGIN_NATIVE_METHOD_DECL(/*func. name*/activate)
	{
		TJS_GET_NATIVE_INSTANCE(/*var. name*/_this, /*var. type*/tTJSNI_ElementsPanel);
		if (numparams < 1) return TJS_E_BADPARAMCOUNT;
		bool ok = _this->ActivateById(ttstr(*param[0]));
		if (result) *result = ok;
		return TJS_S_OK;
	}
	TJS_END_NATIVE_METHOD_DECL(/*func. name*/activate)
	//---------------------------------------------------------------------------
	// active (getter) — 画面を開いているか。
	TJS_BEGIN_NATIVE_PROP_DECL(active)
	{
		TJS_BEGIN_NATIVE_PROP_GETTER
		{
			TJS_GET_NATIVE_INSTANCE(/*var. name*/_this, /*var. type*/tTJSNI_ElementsPanel);
			*result = _this->IsOpen();
			return TJS_S_OK;
		}
		TJS_END_NATIVE_PROP_GETTER
		TJS_DENY_NATIVE_PROP_SETTER
	}
	TJS_END_NATIVE_PROP_DECL(active)
	//---------------------------------------------------------------------------
	// focused (getter) — 現在フォーカス中の widget id (無ければ空文字)。
	TJS_BEGIN_NATIVE_PROP_DECL(focused)
	{
		TJS_BEGIN_NATIVE_PROP_GETTER
		{
			TJS_GET_NATIVE_INSTANCE(/*var. name*/_this, /*var. type*/tTJSNI_ElementsPanel);
			*result = _this->FocusedId();
			return TJS_S_OK;
		}
		TJS_END_NATIVE_PROP_GETTER
		TJS_DENY_NATIVE_PROP_SETTER
	}
	TJS_END_NATIVE_PROP_DECL(focused)
	//---------------------------------------------------------------------------
	// watchVars — どの変数の変化を onVar で受けるか (ElementsDialog.watchVars と同じ)。
	//   void / 未設定 … onVar を実装しているときだけ全変数
	//   "*"           … 全変数
	//   [] (空配列)   … 観測しない
	//   [名前, ...]   … その変数だけ
	TJS_BEGIN_NATIVE_PROP_DECL(watchVars)
	{
		TJS_BEGIN_NATIVE_PROP_GETTER
		{
			TJS_GET_NATIVE_INSTANCE(/*var. name*/_this, /*var. type*/tTJSNI_ElementsPanel);
			switch (_this->WatchMode) {
			case tTJSNI_ElementsPanel::VarWatch::All:
				*result = ttstr(TJS_W("*"));
				break;
			case tTJSNI_ElementsPanel::VarWatch::Off:
			case tTJSNI_ElementsPanel::VarWatch::Names: {
				iTJSDispatch2* arr = TJSCreateArrayObject();
				if (!arr) return TJS_E_FAIL;
				tjs_int i = 0;
				for (auto const& n : _this->WatchNames) {
					tTJSVariant v(n);
					arr->PropSetByNum(TJS_MEMBERENSURE, i++, &v, arr);
				}
				*result = tTJSVariant(arr, arr);
				arr->Release();
				break;
			}
			case tTJSNI_ElementsPanel::VarWatch::Auto:
			default:
				result->Clear();
				break;
			}
			return TJS_S_OK;
		}
		TJS_END_NATIVE_PROP_GETTER

		TJS_BEGIN_NATIVE_PROP_SETTER
		{
			TJS_GET_NATIVE_INSTANCE(/*var. name*/_this, /*var. type*/tTJSNI_ElementsPanel);
			_this->WatchNames.clear();
			if (param->Type() == tvtVoid) {
				_this->WatchMode = tTJSNI_ElementsPanel::VarWatch::Auto;
			} else if (param->Type() == tvtString) {
				ttstr s(*param);
				_this->WatchMode = (s == TJS_W("*"))
					? tTJSNI_ElementsPanel::VarWatch::All
					: tTJSNI_ElementsPanel::VarWatch::Off;
				if (_this->WatchMode == tTJSNI_ElementsPanel::VarWatch::Off &&
				    !s.IsEmpty()) {
					// 単一名の指定も受ける
					_this->WatchMode = tTJSNI_ElementsPanel::VarWatch::Names;
					_this->WatchNames.push_back(s);
				}
			} else if (param->Type() == tvtObject) {
				iTJSDispatch2* arr = param->AsObjectNoAddRef();
				tjs_int cnt = 0;
				tTJSVariant cv;
				if (arr && TJS_SUCCEEDED(arr->PropGet(0, TJS_W("count"),
						nullptr, &cv, arr))) {
					cnt = (tjs_int)cv;
				}
				for (tjs_int i = 0; i < cnt; ++i) {
					tTJSVariant v;
					if (TJS_SUCCEEDED(arr->PropGetByNum(0, i, &v, arr)))
						_this->WatchNames.push_back(ttstr(v));
				}
				_this->WatchMode = _this->WatchNames.empty()
					? tTJSNI_ElementsPanel::VarWatch::Off
					: tTJSNI_ElementsPanel::VarWatch::Names;
			} else {
				_this->WatchMode = tTJSNI_ElementsPanel::VarWatch::Auto;
			}
			// 表示中なら即座に張り直す。
			_this->RefreshVarWatch();
			return TJS_S_OK;
		}
		TJS_END_NATIVE_PROP_SETTER
	}
	TJS_END_NATIVE_PROP_DECL(watchVars)
	//---------------------------------------------------------------------------
	TJS_END_NATIVE_MEMBERS
}

tTJSNativeInstance* tTJSNC_ElementsPanel::CreateNativeInstance()
{
	return new tTJSNI_ElementsPanel();
}

tTJSNativeClass* TVPCreateNativeClass_ElementsPanel()
{
	return new tTJSNC_ElementsPanel();
}
