//---------------------------------------------------------------------------
//!@file TJS Dialog クラスバインディング (Phase 6b)
//
// TJS スクリプトから `new Dialog()` で生成し、JSON レイアウトを使って
// Elements ベースの汎用ダイアログを表示するためのネイティブクラス。
//
//   var dlg = new Dialog();
//   dlg.onAction = function(id, payload) {
//       if (id == "ok") System.inform("OK!");
//   };
//   dlg.showFile("ui/confirm.json");
//   // または
//   dlg.showJson('{"size":[400,200], "content":{...}}');
//   dlg.close();
//---------------------------------------------------------------------------
#ifndef ELEMENTS_DIALOG_INTF_H
#define ELEMENTS_DIALOG_INTF_H

#include "tjsCommHead.h"
#include "tjsNative.h"
#include "DialogEventHandler.h"

class tTJSNI_Dialog : public tTJSNativeInstance, public iTVPDialogEventHandler
{
	typedef tTJSNativeInstance inherited;

public:
	tTJSNI_Dialog();
	virtual ~tTJSNI_Dialog();

	tjs_error TJS_INTF_METHOD Construct(
		tjs_int numparams, tTJSVariant** param, iTJSDispatch2* tjs_obj);
	void TJS_INTF_METHOD Invalidate();

	// iTVPDialogEventHandler override — manager が button click 等で呼ぶ
	void OnAction(const ttstr& id, const tTJSVariant& payload) override;
	// navigator フロー: 画面遷移時に TJS の onScreen / onScreenLeave を起動
	void OnScreenEnter(const ttstr& name) override;
	void OnScreenLeave(const ttstr& name, const ttstr& action) override;
	// teardown 完了時に TJS の onClose(action) を起動 (非ブロッキング経路用)
	void OnClosed(const ttstr& action) override;

	// TJS から呼ばれる
	bool ShowFile(const ttstr& path, bool grabFocus = true);
	bool ShowJson(const ttstr& json_utf16, bool grabFocus = true);
	// ShowJson の Dictionary 版: TJS の Dictionary / Array で書いたレイアウトを
	// JSON 文字列へ変換して同じ経路に流す (elements_modal 側は JSON のまま)。
	// grabFocus=false のときはキーボードフォーカスを取らず、未処理キーは
	// ホスト (Window) へ通る (常駐 HUD / 独自ホットキー併用向け)。
	bool ShowDict(iTJSDispatch2* dict, bool grabFocus = true);
	void Close();
	// 変数 store へ書込 ("text_var" label の動的更新)。 非アクティブなら false。
	bool SetVar(const ttstr& name, const ttstr& value);

	// Phase 6c: 独立 SDL_Window 経由のブロッキングモーダル。
	// 戻り値は Dictionary `%[ action: ttstr, values: %[id: value, ...] ]`。
	// 失敗時は nullptr (TJS では void 扱いに)。
	iTJSDispatch2* ShowModalJson(const ttstr& json_utf16,
		const ttstr& title, int width, int height);
	iTJSDispatch2* ShowModalFile(const ttstr& path,
		const ttstr& title, int width, int height);

	// Phase 6c step2: 既存ゲーム window 上にオーバーレイ表示するモーダル。
	// 戻り値仕様は ShowModalJson / ShowModalFile と同じ。
	iTJSDispatch2* ShowModalOverlayJson(const ttstr& json_utf16);
	iTJSDispatch2* ShowModalOverlayFile(const ttstr& path);

	// ShowModal(Overlay)Json の Dictionary 版。 戻り値仕様は同じ。
	iTJSDispatch2* ShowModalDict(iTJSDispatch2* dict,
		const ttstr& title, int width, int height);
	iTJSDispatch2* ShowModalOverlayDict(iTJSDispatch2* dict);

	// navigator フロー (複数画面遷移) をオーバーレイでブロッキング実行。
	// 戻り値は最後に閉じた画面の `%[ action, values ]` (ShowModal* と同形式)。
	// ShowFlow:        app.jsonc マニフェスト (storage パス) 駆動。
	// ShowFlowScreens: 画面名→レイアウトの TJS Dictionary + 起点画面名。
	//                  レイアウト値は JSON 文字列 / Dictionary のどちらでも可
	//                  (Dictionary は内部で JSON 化、 混在も可)。
	iTJSDispatch2* ShowFlow(const ttstr& manifest_path);
	iTJSDispatch2* ShowFlowScreens(iTJSDispatch2* screens_dict,
		const ttstr& entry);

	// 非モーダル (非ブロッキング) フロー: 即座に return し、 画面遷移は
	// DrawDevice の PaintOverlay が駆動する。 ゲーム画面に常駐させたままに
	// できるので、 背景でサンプルを動かしつつメニューを出しっぱなしにする
	// 用途に使う。 イベントは onScreen / onScreenLeave / onAction で受ける。
	// 画面遷移 (push/pop) は JSON の transitions + close_on_click が、 サンプル
	// 起動等の「閉じない動作」は close_on_click 無しの button の onAction が担う。
	// close() で閉じる。 戻り値は起動成否 (bool)。
	// grabFocus: キーボード/パッドのフォーカスを取得するか (既定 true)。 false に
	// すると未フォーカス常駐になり、 ダイアログが使わないキーはゲームへ素通しする
	// (常駐 HUD 向け)。 操作はマウス、 またはフォーカスを持つ別ダイアログ経由。
	bool StartFlow(const ttstr& manifest_path, bool grabFocus = true);
	bool StartFlowScreens(iTJSDispatch2* screens_dict, const ttstr& entry,
		bool grabFocus = true);

private:
	iTJSDispatch2* Owner;  // TJS 側の自分自身
};

class tTJSNC_Dialog : public tTJSNativeClass
{
	typedef tTJSNativeClass inherited;

public:
	tTJSNC_Dialog();
	static tjs_uint32 ClassID;

protected:
	tTJSNativeInstance* CreateNativeInstance();
};

extern tTJSNativeClass* TVPCreateNativeClass_Dialog();

#endif
