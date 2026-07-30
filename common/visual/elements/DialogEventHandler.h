//---------------------------------------------------------------------------
//!@file Elements ダイアログのイベントハンドラ抽象
//---------------------------------------------------------------------------
#ifndef ELEMENTS_DIALOG_EVENT_HANDLER_H
#define ELEMENTS_DIALOG_EVENT_HANDLER_H

#include "tjsCommHead.h"
#include "tjsVariant.h"

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

	// インスタンスの teardown 完了時 (manager のリストから外れた後) に 1 回
	// 呼ばれる。 action は close_on_click / Esc で閉じた場合の button id
	// (Close() / ForceClose / Window close 等の外部要因は空文字)。 一度も
	// active にならなかった (show 失敗) インスタンスでは呼ばれない。 既定 no-op。
	// この中から manager の Show* / Close を呼んでも安全 (teardown 完了後発火)。
	virtual void OnClosed(const ttstr& /*action*/) {}
};

#endif
