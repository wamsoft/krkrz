//---------------------------------------------------------------------------
//!@file Elements ダイアログのイベントハンドラ抽象
//---------------------------------------------------------------------------
#ifndef ELEMENTS_DIALOG_EVENT_HANDLER_H
#define ELEMENTS_DIALOG_EVENT_HANDLER_H

#include "tjsCommHead.h"
#include "tjsVariant.h"

#include <vector>

class iTVPDialogEventHandler
{
public:
	virtual ~iTVPDialogEventHandler() = default;

	// id="ok" のボタン押下、id="name" のテキスト変更、等。
	// payload は要素種別ごとに意味が異なる variant。
	virtual void OnAction(const ttstr& id, const tTJSVariant& payload) = 0;

	// === navigator フロー (複数画面遷移) 用の通知 ===
	// 単発ダイアログ (showJson / showModal*) では呼ばれない。 既定 no-op。
	//
	// OnScreenEnter: 新しい画面 (overlay_session) を start した直後に呼ばれる。
	// OnScreenLeave: 現画面が閉じて遷移する直前 (advance 前) に、 閉じた
	//                action id とともに呼ばれる。 action は閉じた button の id
	//                (Esc / B / 右クリック等は空文字)。
	virtual void OnScreenEnter(const ttstr& /*name*/) {}
	virtual void OnScreenLeave(const ttstr& /*name*/, const ttstr& /*action*/) {}

	// === ドラッグ通知 ===
	// 画面 JSON で "drag_events": true を書いた widget の 押下 → 移動 → 離す。
	// payload は Dictionary: %[id, phase("begin"/"move"/"end"), x, y, dx, dy,
	// startX, startY, modifiers]。 座標は画面 JSON に書いた座標系 (view logical)。
	// 既定 no-op。
	//
	// 注: «掴んだ絵をついてこさせる» だけなら、 widget 側の "drag_at_var" が
	// 位置を変数へ書き、 同じ変数を "at_var" に挿すだけで済む (C++ 内で完結し
	// フレーム同期)。 この通知は «どこで離したか» のような判断をする用。
	virtual void OnDrag(const tTJSVariant& /*payload*/) {}

	// === 変数変化通知 ===
	// 画面 JSON の変数 store (vars / text_var / vars_on_hover / vars_on_focus /
	// slider の value_var / drag_at_var 等がぶら下がる 1 本の store) の値が
	// 変わったときに呼ばれる。 ホスト自身の SetVar による変化も届く。
	// «絵はホスト側のレイヤ、 当たり判定だけダイアログ» のような構成で、
	// hover / focus / スライダの結果をホスト処理へ繋ぐのに使う。 既定 no-op。
	//
	// 通知は必ず 1 フレーム遅延して (window update の外で) 配送され、 同じ
	// 変数の連続変化は最新 1 件へ畳まれる。 «いまの値» が要るときは通知を
	// 待たず manager の GetVar で読むこと。
	virtual void OnVar(const ttstr& /*name*/, const ttstr& /*value*/) {}

	// 画面 (overlay_session) の開始時に manager が問い合わせる。 true を返した
	// ときだけ変数の観測が有効になり、 以後 OnVar が届く。 out_names が空なら
	// 全変数、 非空ならその名前の変数だけを通知する (hover 連動変数のような
	// 高頻度の書込を除外して負荷を抑える口)。 既定 false = 観測しない。
	virtual bool WantsVarNotify(std::vector<ttstr>& /*out_names*/) { return false; }

	// インスタンスの teardown 完了時 (manager のリストから外れた後) に 1 回
	// 呼ばれる。 action は close_on_click / Esc で閉じた場合の button id
	// (Close() / ForceClose / Window close 等の外部要因は空文字)。 一度も
	// active にならなかった (show 失敗) インスタンスでは呼ばれない。 既定 no-op。
	// この中から manager の Show* / Close を呼んでも安全 (teardown 完了後発火)。
	virtual void OnClosed(const ttstr& /*action*/) {}
};

#endif
