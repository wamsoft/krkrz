//---------------------------------------------------------------------------
// TJS Agent クラス実装 — エージェント / 自動テスト駆動用の制御 API
//
// SDL3 / WINVER 両対応。入力注入は AgentInput seam (TVPAgentInject*) 経由で実入力と
// 同じ経路を通すので、 ゲームにも Elements ダイアログにも届く (DrawDevice / Window の
// dialog intercept を経由)。seam 実装は sdl3/environ/AgentInput.cpp (SendMouseMessage/
// SendMessage) と win32/environ/AgentInput.cpp (OnMouse*/OnKey*)。ダイアログ制御・
// text・captureScreen は元から横断 (ElementsDialogManager / ScreenCapture は common)。
//---------------------------------------------------------------------------
#include "tjsCommHead.h"
#include "AgentControlIntf.h"
#include "tjsArray.h"
#include "tjsDictionary.h"
#include "DebugIntf.h"
#include "CharacterSet.h"
#include "AgentInput.h"        // TVPAgentInject* / TVPAgentRequestRedraw (platform seam)
#include "tvpinputdefs.h"      // mbLeft / mbRight / mbMiddle
#include "ScreenCapture.h"     // TVPRequestScreenCapture / TVPGetLastScreenCapture

#ifdef KRKRZ_HAS_ELEMENTS
#include "elements/ElementsDialogManager.h"
#endif

#include <string>

// shift フラグは TJS から渡された数値をそのまま seam へ渡す。
// button は krkrz の tTVPMouseButton (mbLeft=0 / mbRight=1 / mbMiddle=2)。

//---------------------------------------------------------------------------
// Agent クラス (インスタンス不要、 System と同様にクラスオブジェクトのメソッド)
//---------------------------------------------------------------------------
tjs_uint32 tTJSNC_Agent::ClassID = (tjs_uint32)-1;

tTJSNC_Agent::tTJSNC_Agent() : inherited(TJS_W("Agent"))
{
	TJS_BEGIN_NATIVE_MEMBERS(Agent)
	TJS_DECL_EMPTY_FINALIZE_METHOD
	//---------------------------------------------------------------------------
	TJS_BEGIN_NATIVE_CONSTRUCTOR_DECL(/*var.name*/_this, /*var.type*/tTJSNativeInstance, /*TJS class name*/Agent)
	{
		return TJS_S_OK;
	}
	TJS_END_NATIVE_CONSTRUCTOR_DECL(/*TJS class name*/Agent)

	//=== 入力: マウス =========================================================
	//---------------------------------------------------------------------------
	// mouseMove(x, y [, shift])
	TJS_BEGIN_NATIVE_METHOD_DECL(/*func. name*/mouseMove)
	{
		if (numparams < 2) return TJS_E_BADPARAMCOUNT;
		int x = (int)(tjs_int)*param[0];
		int y = (int)(tjs_int)*param[1];
		int shift = (numparams >= 3) ? (int)(tjs_int)*param[2] : 0;
		if (!TVPAgentInjectMouseMove(shift, x, y)) return TJS_E_FAIL;
		if (result) *result = (tjs_int)1;
		return TJS_S_OK;
	}
	TJS_END_NATIVE_METHOD_DECL(/*func. name*/mouseMove)
	//---------------------------------------------------------------------------
	// mouseDown(x, y [, button [, shift]])
	TJS_BEGIN_NATIVE_METHOD_DECL(/*func. name*/mouseDown)
	{
		if (numparams < 2) return TJS_E_BADPARAMCOUNT;
		int x = (int)(tjs_int)*param[0];
		int y = (int)(tjs_int)*param[1];
		int button = (numparams >= 3) ? (int)(tjs_int)*param[2] : (int)mbLeft;
		int shift  = (numparams >= 4) ? (int)(tjs_int)*param[3] : 0;
		if (!TVPAgentInjectMouseButton(true, button, shift, x, y)) return TJS_E_FAIL;
		if (result) *result = (tjs_int)1;
		return TJS_S_OK;
	}
	TJS_END_NATIVE_METHOD_DECL(/*func. name*/mouseDown)
	//---------------------------------------------------------------------------
	// mouseUp(x, y [, button [, shift]])
	TJS_BEGIN_NATIVE_METHOD_DECL(/*func. name*/mouseUp)
	{
		if (numparams < 2) return TJS_E_BADPARAMCOUNT;
		int x = (int)(tjs_int)*param[0];
		int y = (int)(tjs_int)*param[1];
		int button = (numparams >= 3) ? (int)(tjs_int)*param[2] : (int)mbLeft;
		int shift  = (numparams >= 4) ? (int)(tjs_int)*param[3] : 0;
		if (!TVPAgentInjectMouseButton(false, button, shift, x, y)) return TJS_E_FAIL;
		if (result) *result = (tjs_int)1;
		return TJS_S_OK;
	}
	TJS_END_NATIVE_METHOD_DECL(/*func. name*/mouseUp)
	//---------------------------------------------------------------------------
	// click(x, y [, button [, shift]])  — down + up
	TJS_BEGIN_NATIVE_METHOD_DECL(/*func. name*/click)
	{
		if (numparams < 2) return TJS_E_BADPARAMCOUNT;
		int x = (int)(tjs_int)*param[0];
		int y = (int)(tjs_int)*param[1];
		int button = (numparams >= 3) ? (int)(tjs_int)*param[2] : (int)mbLeft;
		int shift  = (numparams >= 4) ? (int)(tjs_int)*param[3] : 0;
		// hover を先に送ってから down/up (ホバー状態に依存する widget 対策)。
		if (!TVPAgentInjectMouseMove(shift, x, y)) return TJS_E_FAIL;
		TVPAgentInjectMouseButton(true,  button, shift, x, y);
		TVPAgentInjectMouseButton(false, button, shift, x, y);
		if (result) *result = (tjs_int)1;
		return TJS_S_OK;
	}
	TJS_END_NATIVE_METHOD_DECL(/*func. name*/click)
	//---------------------------------------------------------------------------
	// wheel(delta, x, y [, shift])  — delta は 120 単位 (1 ノッチ = 120)
	TJS_BEGIN_NATIVE_METHOD_DECL(/*func. name*/wheel)
	{
		if (numparams < 3) return TJS_E_BADPARAMCOUNT;
		int delta = (int)(tjs_int)*param[0];
		int x = (int)(tjs_int)*param[1];
		int y = (int)(tjs_int)*param[2];
		int shift = (numparams >= 4) ? (int)(tjs_int)*param[3] : 0;
		if (!TVPAgentInjectWheel(delta, shift, x, y)) return TJS_E_FAIL;
		if (result) *result = (tjs_int)1;
		return TJS_S_OK;
	}
	TJS_END_NATIVE_METHOD_DECL(/*func. name*/wheel)

	//=== 入力: キー ===========================================================
	//---------------------------------------------------------------------------
	// keyDown(vk [, shift])
	TJS_BEGIN_NATIVE_METHOD_DECL(/*func. name*/keyDown)
	{
		if (numparams < 1) return TJS_E_BADPARAMCOUNT;
		tjs_int64 vk = (tjs_int)*param[0];
		tjs_int64 shift = (numparams >= 2) ? (tjs_int)*param[1] : 0;
		if (!TVPAgentInjectKey(true, vk, shift)) return TJS_E_FAIL;
		if (result) *result = (tjs_int)1;
		return TJS_S_OK;
	}
	TJS_END_NATIVE_METHOD_DECL(/*func. name*/keyDown)
	//---------------------------------------------------------------------------
	// keyUp(vk [, shift])
	TJS_BEGIN_NATIVE_METHOD_DECL(/*func. name*/keyUp)
	{
		if (numparams < 1) return TJS_E_BADPARAMCOUNT;
		tjs_int64 vk = (tjs_int)*param[0];
		tjs_int64 shift = (numparams >= 2) ? (tjs_int)*param[1] : 0;
		if (!TVPAgentInjectKey(false, vk, shift)) return TJS_E_FAIL;
		if (result) *result = (tjs_int)1;
		return TJS_S_OK;
	}
	TJS_END_NATIVE_METHOD_DECL(/*func. name*/keyUp)
	//---------------------------------------------------------------------------
	// keyPress(vk [, shift])  — down + up
	TJS_BEGIN_NATIVE_METHOD_DECL(/*func. name*/keyPress)
	{
		if (numparams < 1) return TJS_E_BADPARAMCOUNT;
		tjs_int64 vk = (tjs_int)*param[0];
		tjs_int64 shift = (numparams >= 2) ? (tjs_int)*param[1] : 0;
		if (!TVPAgentInjectKey(true, vk, shift)) return TJS_E_FAIL;
		TVPAgentInjectKey(false, vk, shift);
		if (result) *result = (tjs_int)1;
		return TJS_S_OK;
	}
	TJS_END_NATIVE_METHOD_DECL(/*func. name*/keyPress)
	//---------------------------------------------------------------------------
	// text(str)  — テキスト入力を実入力と同じ経路で注入する (TEXT_INPUT /
	//              WM_CHAR 相当)。Elements ダイアログのテキスト欄に focus が
	//              あればそちらが消費し、無ければゲーム側 (Window.onTextInput /
	//              Layer.onKeyPress 互換配送) へ流れる。
	TJS_BEGIN_NATIVE_METHOD_DECL(/*func. name*/text)
	{
		if (numparams < 1) return TJS_E_BADPARAMCOUNT;
		ttstr s(*param[0]);
		bool ok = TVPAgentInjectText(s);
		if (result) *result = (tjs_int)(ok ? 1 : 0);
		return TJS_S_OK;
	}
	TJS_END_NATIVE_METHOD_DECL(/*func. name*/text)

	//=== Elements ダイアログ制御 ==============================================
#ifdef KRKRZ_HAS_ELEMENTS
	//---------------------------------------------------------------------------
	// dialogs()  — アクティブダイアログ記述の配列を返す。
	//   各要素 = %[ index, modal, active, screen, focused, x, y, w, h ]
	TJS_BEGIN_NATIVE_METHOD_DECL(/*func. name*/dialogs)
	{
		auto infos = tTVPElementsDialogManager::Instance().DescribeInstances();
		iTJSDispatch2* arr = TJSCreateArrayObject();
		if (!arr) return TJS_E_FAIL;
		for (tjs_int i = 0; i < (tjs_int)infos.size(); ++i) {
			const auto& info = infos[i];
			iTJSDispatch2* d = TJSCreateDictionaryObject();
			if (d) {
				auto setI = [&](const tjs_char* k, tjs_int64 v) {
					tTJSVariant vv((tjs_int64)v);
					d->PropSet(TJS_MEMBERENSURE, k, nullptr, &vv, d);
				};
				auto setB = [&](const tjs_char* k, bool v) {
					tTJSVariant vv(v);
					d->PropSet(TJS_MEMBERENSURE, k, nullptr, &vv, d);
				};
				auto setS = [&](const tjs_char* k, const ttstr& v) {
					tTJSVariant vv(v);
					d->PropSet(TJS_MEMBERENSURE, k, nullptr, &vv, d);
				};
				setI(TJS_W("index"), i);
				setB(TJS_W("modal"), info.modal);
				setB(TJS_W("active"), info.active);
				setS(TJS_W("screen"), info.screen);
				setS(TJS_W("focused"), info.focused);
				setI(TJS_W("x"), info.x);
				setI(TJS_W("y"), info.y);
				setI(TJS_W("w"), info.w);
				setI(TJS_W("h"), info.h);
				tTJSVariant dv(d, d);
				arr->PropSetByNum(TJS_MEMBERENSURE, i, &dv, arr);
				d->Release();
			}
		}
		if (result) *result = tTJSVariant(arr, arr);
		arr->Release();
		return TJS_S_OK;
	}
	TJS_END_NATIVE_METHOD_DECL(/*func. name*/dialogs)
	//---------------------------------------------------------------------------
	// closeDialog([index])  — index 省略で最前面を閉じる。
	TJS_BEGIN_NATIVE_METHOD_DECL(/*func. name*/closeDialog)
	{
		// index 指定版は今は最前面 close にフォールバック (per-index close は
		// handler 単位 API のみのため)。 詳細制御は closeAllDialogs / dialogs 併用。
		tTVPElementsDialogManager::Instance().Close();
		if (result) *result = (tjs_int)1;
		return TJS_S_OK;
	}
	TJS_END_NATIVE_METHOD_DECL(/*func. name*/closeDialog)
	//---------------------------------------------------------------------------
	// closeAllDialogs()  — 全インスタンスを即 teardown。
	TJS_BEGIN_NATIVE_METHOD_DECL(/*func. name*/closeAllDialogs)
	{
		tTVPElementsDialogManager::Instance().ForceClose();
		if (result) *result = (tjs_int)1;
		return TJS_S_OK;
	}
	TJS_END_NATIVE_METHOD_DECL(/*func. name*/closeAllDialogs)
	//---------------------------------------------------------------------------
	// dialogClick(index, id)  — id 指定の widget をフォーカス + 起動 (Enter 相当)。
	TJS_BEGIN_NATIVE_METHOD_DECL(/*func. name*/dialogClick)
	{
		if (numparams < 2) return TJS_E_BADPARAMCOUNT;
		int index = (int)(tjs_int)*param[0];
		ttstr id(*param[1]);
		bool ok = tTVPElementsDialogManager::Instance().ActivateWidgetById(index, id);
		if (result) *result = (tjs_int)(ok ? 1 : 0);
		return TJS_S_OK;
	}
	TJS_END_NATIVE_METHOD_DECL(/*func. name*/dialogClick)
	//---------------------------------------------------------------------------
	// dialogFocus(index, id)  — id 指定の widget へフォーカス移動。
	TJS_BEGIN_NATIVE_METHOD_DECL(/*func. name*/dialogFocus)
	{
		if (numparams < 2) return TJS_E_BADPARAMCOUNT;
		int index = (int)(tjs_int)*param[0];
		ttstr id(*param[1]);
		bool ok = tTVPElementsDialogManager::Instance().FocusWidgetById(index, id);
		if (result) *result = (tjs_int)(ok ? 1 : 0);
		return TJS_S_OK;
	}
	TJS_END_NATIVE_METHOD_DECL(/*func. name*/dialogFocus)
	//---------------------------------------------------------------------------
	// dialogTree(index)  — index 番目のダイアログの id 付き widget 一覧を返す。
	//   各要素 = %[ id, type, value ]  (value は state widget の現在値、 無ければ void)。
	//   widget の id が分かるので dialogClick(index, id) で座標不要に操作できる。
	TJS_BEGIN_NATIVE_METHOD_DECL(/*func. name*/dialogTree)
	{
		if (numparams < 1) return TJS_E_BADPARAMCOUNT;
		int index = (int)(tjs_int)*param[0];
		auto widgets = tTVPElementsDialogManager::Instance().DescribeWidgets(index);
		iTJSDispatch2* arr = TJSCreateArrayObject();
		if (!arr) return TJS_E_FAIL;
		for (tjs_int i = 0; i < (tjs_int)widgets.size(); ++i) {
			const auto& w = widgets[i];
			iTJSDispatch2* d = TJSCreateDictionaryObject();
			if (d) {
				{ tTJSVariant v(w.id);   d->PropSet(TJS_MEMBERENSURE, TJS_W("id"),   nullptr, &v, d); }
				{ tTJSVariant v(w.type); d->PropSet(TJS_MEMBERENSURE, TJS_W("type"), nullptr, &v, d); }
				if (w.has_value) {
					tTJSVariant v = w.value;
					d->PropSet(TJS_MEMBERENSURE, TJS_W("value"), nullptr, &v, d);
				}
				tTJSVariant dv(d, d);
				arr->PropSetByNum(TJS_MEMBERENSURE, i, &dv, arr);
				d->Release();
			}
		}
		if (result) *result = tTJSVariant(arr, arr);
		arr->Release();
		return TJS_S_OK;
	}
	TJS_END_NATIVE_METHOD_DECL(/*func. name*/dialogTree)
#endif // KRKRZ_HAS_ELEMENTS

	//=== 画面キャプチャ =======================================================
	//---------------------------------------------------------------------------
	// captureScreen(path [, x, y, w, h])
	//   overlay 込みの実画面を次フレームの present 直前に読み戻して PNG 保存する。
	//   即 return し、 実際の保存は ~1 フレーム後 (ファイルが出来たら完了)。
	//   x/y/w/h 省略で全画面。 戻り値は要求した path (文字列)。
	TJS_BEGIN_NATIVE_METHOD_DECL(/*func. name*/captureScreen)
	{
		if (numparams < 1) return TJS_E_BADPARAMCOUNT;
		ttstr path(*param[0]);
		int x = (numparams >= 2) ? (int)(tjs_int)*param[1] : 0;
		int y = (numparams >= 3) ? (int)(tjs_int)*param[2] : 0;
		int w = (numparams >= 4) ? (int)(tjs_int)*param[3] : 0;
		int h = (numparams >= 5) ? (int)(tjs_int)*param[4] : 0;
		TVPRequestScreenCapture(path, x, y, w, h);
		// アイドル時は Show() が呼ばれず要求が消化されないので、 再描画を促す。
		TVPAgentRequestRedraw();
		if (result) *result = path;
		return TJS_S_OK;
	}
	TJS_END_NATIVE_METHOD_DECL(/*func. name*/captureScreen)
	//---------------------------------------------------------------------------
	// lastCapture()  — 直近キャプチャの結果 %[ path, width, height, ok ]。
	//   captureScreen 後にこれで完了 / 寸法を確認できる (ok=true で保存済み)。
	TJS_BEGIN_NATIVE_METHOD_DECL(/*func. name*/lastCapture)
	{
		ttstr path; int w = 0, h = 0; bool ok = false;
		TVPGetLastScreenCapture(path, w, h, ok);
		iTJSDispatch2* d = TJSCreateDictionaryObject();
		if (!d) return TJS_E_FAIL;
		{ tTJSVariant v(path); d->PropSet(TJS_MEMBERENSURE, TJS_W("path"), nullptr, &v, d); }
		{ tTJSVariant v((tjs_int64)w); d->PropSet(TJS_MEMBERENSURE, TJS_W("width"), nullptr, &v, d); }
		{ tTJSVariant v((tjs_int64)h); d->PropSet(TJS_MEMBERENSURE, TJS_W("height"), nullptr, &v, d); }
		{ tTJSVariant v(ok); d->PropSet(TJS_MEMBERENSURE, TJS_W("ok"), nullptr, &v, d); }
		if (result) *result = tTJSVariant(d, d);
		d->Release();
		return TJS_S_OK;
	}
	TJS_END_NATIVE_METHOD_DECL(/*func. name*/lastCapture)

	//---------------------------------------------------------------------------
	TJS_END_NATIVE_MEMBERS
}

tTJSNativeInstance* tTJSNC_Agent::CreateNativeInstance()
{
	return nullptr;  // インスタンス状態は持たない
}

tTJSNativeClass* TVPCreateNativeClass_Agent()
{
	return new tTJSNC_Agent();
}
