#include "tjsCommHead.h"

#include "Application.h"
#include "WindowForm.h"

// パッド入力の中身は共有の tTVPPadManager (common/environ/PadManager.cpp) に集約。
// ここは generic/SDL ビルドの tTVPApplication から manager を駆動し、生成された
// キーイベント (VK_PAD*) を MainWindowForm に流すグルーのみ。

// キー押し下げ状態取得（パッドのみ。論理 0 = 最後に操作したパッド基準）
bool
tTVPApplication::GetAsyncKeyState(tjs_uint keycode, bool getcurrent)
{
	if (getcurrent) { // false ならトグル状態取得 (未対応)
		return PadManager_.GetAsyncKeyState(keycode);
	}
	return false;
}

void
tTVPApplication::SendPadEvent()
{
	// 全物理パッドを走査し active (=論理0) を更新、キーイベントを生成する。
	PadManager_.Update();

	TTVPWindowForm *form = MainWindowForm();
	if (!form) return;

	const int shift = 0;
	for (int key : PadManager_.GetUppedKeys()) {
		form->SendMessage(AM_KEY_UP, key, shift);
	}
	for (int key : PadManager_.GetDownedKeys()) {
		form->SendMessage(AM_KEY_DOWN, key, shift);
	}
	for (int key : PadManager_.GetRepeatKeys()) {
		form->SendMessage(AM_KEY_DOWN, key, shift | TVP_SS_REPEAT);
	}
}
