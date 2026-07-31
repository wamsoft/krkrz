//---------------------------------------------------------------------------
// Agent 入力注入 seam — WINVER 実装
//
// win32 form (TTVPWindowForm) の OnMouse*/OnKey* 経由で、WndProc からの実入力と
// 同じハンドラに流す (TVPPostInputEvent → DrawDevice / Elements ダイアログ intercept
// → ゲーム)。common/environ/AgentControlIntf.cpp から呼ばれる。
//
// shift は Agent から渡された値をそのまま OnMouse*/OnKey* へ渡す (実入力と同じく
// TShiftState として扱われ内部で uint32 へ変換される)。x/y はウィンドウ座標。
//---------------------------------------------------------------------------
#include "tjsCommHead.h"
#include "AgentInput.h"
#include "Application.h"
#include "WindowFormUnit.h"

namespace {
TTVPWindowForm* AgentMainForm()
{
	if (!Application) return nullptr;
	return Application->MainWindowForm();
}
} // anonymous

bool TVPAgentInjectMouseMove(int shift, int x, int y)
{
	TTVPWindowForm* form = AgentMainForm();
	if (!form) return false;
	form->OnMouseMove(shift, x, y);
	return true;
}

bool TVPAgentInjectMouseButton(bool down, int button, int shift, int x, int y)
{
	TTVPWindowForm* form = AgentMainForm();
	if (!form) return false;
	if (down) form->OnMouseDown(button, shift, x, y);
	else      form->OnMouseUp(button, shift, x, y);
	return true;
}

bool TVPAgentInjectWheel(int delta, int shift, int x, int y)
{
	TTVPWindowForm* form = AgentMainForm();
	if (!form) return false;
	form->OnMouseWheel(delta, shift, x, y);
	return true;
}

bool TVPAgentInjectKey(bool down, tjs_int64 vk, tjs_int64 shift)
{
	TTVPWindowForm* form = AgentMainForm();
	if (!form) return false;
	if (down) form->OnKeyDown((WORD)vk, (int)shift, 0, false);
	else      form->OnKeyUp((WORD)vk, (int)shift);
	return true;
}

void TVPAgentRequestRedraw()
{
	// WINVER は VSyncTimingThread が Show() を継続的に駆動するのでキャプチャ要求は
	// 自然に消化されるが、アイドル確実化のためメインウィンドウを invalidate する。
	if (!Application) return;
	HWND hwnd = Application->GetMainWindowHandle();
	if (hwnd) ::InvalidateRect(hwnd, NULL, FALSE);
}
