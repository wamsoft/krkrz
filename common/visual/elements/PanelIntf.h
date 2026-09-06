//---------------------------------------------------------------------------
//!@file TJS ElementsPanel クラスバインディング
//
// Elements の画面を **ホストのレイヤ**へ描くパネル。 overlay ダイアログ
// (`Dialog`) が常に最前面なのに対し、 こちらはただのレイヤの中身になるので
// z 順・トランジション・スクリーンショット・入力の帰属が吉里吉里の
// レイヤの仕組みに従う。
//
//   var lay = new Layer(win, win.primaryLayer);
//   lay.setImageSize(400, 300);
//   lay.setSizeToImageSize();
//   lay.type = ltAlpha;
//   lay.hitThreshold = 0;               // 全てのマウスメッセージを受ける
//   lay.visible = true;
//
//   var panel = new ElementsPanel(lay);
//   panel.onAction = function(id, payload) { ... };
//   panel.showFile("ui/hud.jsonc");
//
//   // レイヤの入力をパネルへ流す (スクリプト側の Layer 派生で 1 回書けばよい)
//   lay.onMouseDown = function(x, y, b, f) { panel.mouseDown(x, y, b, f); };
//
// イベント名と引数は `Dialog` と同じ (`onAction` / `onDrag` / `onVar` /
// `onClose`)。 既存の画面ドライバをそのまま載せられるようにするため。
//
// **`Dialog` とは別枠**なので、 `Agent.dialogs()` には現れず、
// ゲームの入力インターセプトやウィンドウクローズ抑止にも影響しない。
//---------------------------------------------------------------------------
#ifndef ELEMENTS_PANEL_INTF_H
#define ELEMENTS_PANEL_INTF_H

#include "tjsCommHead.h"
#include "tjsNative.h"
#include "DialogEventHandler.h"

#include <memory>
#include <vector>

class tTVPElementsLayerPanel;
class tTJSNI_BaseLayer;

class tTJSNI_ElementsPanel : public tTJSNativeInstance,
                             public iTVPDialogEventHandler
{
	typedef tTJSNativeInstance inherited;

public:
	tTJSNI_ElementsPanel();
	virtual ~tTJSNI_ElementsPanel();

	tjs_error TJS_INTF_METHOD Construct(
		tjs_int numparams, tTJSVariant** param, iTJSDispatch2* tjs_obj);
	void TJS_INTF_METHOD Invalidate();

	// --- iTVPDialogEventHandler ---------------------------------------
	void OnAction(const ttstr& id, const tTJSVariant& payload) override;
	void OnDrag(const tTJSVariant& payload) override;
	void OnVar(const ttstr& name, const ttstr& value) override;
	void OnClosed(const ttstr& action) override;
	bool WantsVarNotify(std::vector<ttstr>& out_names) override;

	// --- TJS から呼ばれる --------------------------------------------
	bool OpenFile(const ttstr& path);
	bool OpenJson(const ttstr& json_utf16);
	bool OpenDict(iTJSDispatch2* dict);
	void Close();
	bool IsOpen() const;

	void NotifyResized();

	void MouseDown(tjs_int x, tjs_int y, tjs_int button, tjs_uint32 flags);
	void MouseUp  (tjs_int x, tjs_int y, tjs_int button, tjs_uint32 flags);
	void MouseMove(tjs_int x, tjs_int y, tjs_uint32 flags);
	void MouseWheel(tjs_int delta, tjs_int x, tjs_int y, tjs_uint32 flags);
	void MouseLeave();
	bool KeyDown(tjs_uint key, tjs_uint32 shift);
	bool KeyUp  (tjs_uint key, tjs_uint32 shift);
	void TextInput(const ttstr& text);

	bool SetVar(const ttstr& name, const ttstr& value);
	bool GetVar(const ttstr& name, ttstr& out);
	iTJSDispatch2* ListVars();
	void RefreshVarWatch();
	void DoInvalidate();
	bool FocusById(const ttstr& id);
	bool ActivateById(const ttstr& id);
	ttstr FocusedId() const;

	//! `ElementsDialog.watchVars` と同じ意味。
	enum class VarWatch { Auto, Off, All, Names };
	VarWatch WatchMode = VarWatch::Auto;
	std::vector<ttstr> WatchNames;

private:
	//! 画面 JSON (utf-8) を開く。 `resource_base` は path から導く。
	bool OpenUtf8(const std::string& json_utf8,
	              const std::string& resource_base_utf8,
	              const tjs_char* api, const ttstr& target);

	iTJSDispatch2*        Owner = nullptr;   // TJS 側の自分自身
	tTJSNI_BaseLayer*     Layer = nullptr;   // 描画先 (コンストラクタで受ける)
	iTJSDispatch2*        LayerObj = nullptr; // Layer を生かしておくための参照
	std::unique_ptr<tTVPElementsLayerPanel> Panel;
};

class tTJSNC_ElementsPanel : public tTJSNativeClass
{
	typedef tTJSNativeClass inherited;

public:
	tTJSNC_ElementsPanel();
	static tjs_uint32 ClassID;

protected:
	tTJSNativeInstance* CreateNativeInstance();
};

extern tTJSNativeClass* TVPCreateNativeClass_ElementsPanel();

#endif
