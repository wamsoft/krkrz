//---------------------------------------------------------------------------
//!@file TJS ElementsDialog クラスバインディング (Phase 6b)
//
// TJS スクリプトから `new ElementsDialog()` で生成し、JSON レイアウトを使って
// Elements ベースの汎用ダイアログを表示するためのネイティブクラス。
//
//   var dlg = new ElementsDialog();
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

#include <map>
#include <vector>

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
	void OnDrag(const tTJSVariant& payload) override;
	void OnClosed(const ttstr& action) override;
	// 変数 store の変化で TJS の onVar(name, value) を起動する。 観測するか
	// どうかは WantsVarNotify が答える (watchVars / onVar 実装の有無で決まる)。
	void OnVar(const ttstr& name, const ttstr& value) override;
	bool WantsVarNotify(std::vector<ttstr>& out_names) override;

	// TJS から呼ばれる
	// modal: -1 = 省略 (後方互換で grabFocus に追従) / 0 = 非モーダル / 1 = モーダル。
	// grabFocus=true + modal=0 が「非モーダル+フォーカスあり」の中間状態で、
	// キー/パッドはダイアログへ届き、未処理分はホスト (Window) へ素通しする。
	bool ShowFile(const ttstr& path, bool grabFocus = true, int modal = -1);
	bool ShowJson(const ttstr& json_utf16, bool grabFocus = true, int modal = -1);
	// ShowJson の Dictionary 版: TJS の Dictionary / Array で書いたレイアウトを
	// JSON 文字列へ変換して同じ経路に流す (elements_modal 側は JSON のまま)。
	// grabFocus=false のときはキーボードフォーカスを取らず、未処理キーは
	// ホスト (Window) へ通る (常駐 HUD / 独自ホットキー併用向け)。
	bool ShowDict(iTJSDispatch2* dict, bool grabFocus = true, int modal = -1);
	void Close();
	// 変数 store へ書込 ("text_var" label の動的更新)。 非アクティブなら false。
	bool SetVar(const ttstr& name, const ttstr& value);
	// 変数 store から読出。 未知の変数 / 非アクティブなら false。
	bool GetVar(const ttstr& name, ttstr& out);
	// id 指定の widget へフォーカスを移す (Agent.dialogFocus の instance 版)。
	// input_box は編集フォーカス (キャレット + text 受理) になる。
	bool FocusWidget(const ttstr& id);
	bool ActivateWidget(const ttstr& id);

	//! ElementsDialog.watchVars の状態。 どの変数の変化を onVar で受けるか。
	//!  - Auto  … 既定。 onVar を実装しているときだけ「全変数」を観測する
	//!  - Off   … 観測しない (watchVars = [] を明示指定)
	//!  - All   … 全変数を観測 (watchVars = "*")
	//!  - Names … WatchNames の変数だけ観測 (watchVars = [名前, ...])
	enum class VarWatch { Auto, Off, All, Names };
	VarWatch WatchMode = VarWatch::Auto;
	std::vector<ttstr> WatchNames;

	// Phase 6c: 独立 SDL_Window 経由のブロッキングモーダル。
	// 戻り値は Dictionary `%[ action: ttstr, values: %[id: value, ...] ]`。
	// 失敗時は nullptr (TJS では void 扱いに)。
	iTJSDispatch2* ShowModalJson(const ttstr& json_utf16,
		const ttstr& title, int width, int height);
	iTJSDispatch2* ShowModalFile(const ttstr& path,
		const ttstr& title, int width, int height);

	// Phase 6c step2: 既存ゲーム window 上にオーバーレイ表示するモーダル。
	// 戻り値仕様は ShowModalJson / ShowModalFile と同じ。
	// initialVars (任意): build 直後・pump 前に変数 store へ流し込む初期値。
	// index_var / enabled_var / selected_var 等の subscribe 済 widget が
	// 反映する (静的 JSON への動的初期値注入。 TJS 側は
	// showModalFile(path, %[name => value, ...]) の形で渡す)。
	iTJSDispatch2* ShowModalOverlayJson(const ttstr& json_utf16,
		const std::map<ttstr, ttstr>* initialVars = nullptr);
	iTJSDispatch2* ShowModalOverlayFile(const ttstr& path,
		const std::map<ttstr, ttstr>* initialVars = nullptr);

	// ShowModal(Overlay)Json の Dictionary 版。 戻り値仕様は同じ。
	iTJSDispatch2* ShowModalDict(iTJSDispatch2* dict,
		const ttstr& title, int width, int height);
	iTJSDispatch2* ShowModalOverlayDict(iTJSDispatch2* dict,
		const std::map<ttstr, ttstr>* initialVars = nullptr);

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
