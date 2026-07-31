//---------------------------------------------------------------------------
// Agent 入力注入 seam — generic 実装 (SDL3 / LIB 両ビルドで使用)
//
// generic form (TTVPWindowForm) の SendMouseMessage/SendMessage 経由で、実入力と
// 同じ同期処理経路に流す。common/environ/AgentControlIntf.cpp から呼ばれる。
//---------------------------------------------------------------------------
#include "tjsCommHead.h"
#include "AgentInput.h"
#include "Application.h"
#include "WindowForm.h"
#include "WindowFormEvent.h"   // AM_KEY_DOWN / AM_MOUSE_DOWN 等

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
	form->SendMouseMessage(AM_MOUSE_MOVE, 0, shift, x, y);
	return true;
}

bool TVPAgentInjectMouseButton(bool down, int button, int shift, int x, int y)
{
	TTVPWindowForm* form = AgentMainForm();
	if (!form) return false;
	form->SendMouseMessage(down ? AM_MOUSE_DOWN : AM_MOUSE_UP, button, shift, x, y);
	return true;
}

bool TVPAgentInjectWheel(int delta, int shift, int x, int y)
{
	TTVPWindowForm* form = AgentMainForm();
	if (!form) return false;
	form->SendMouseMessage(AM_MOUSE_WHEEL, delta, shift, x, y);
	return true;
}

bool TVPAgentInjectKey(bool down, tjs_int64 vk, tjs_int64 shift)
{
	TTVPWindowForm* form = AgentMainForm();
	if (!form) return false;
	form->SendMessage(down ? AM_KEY_DOWN : AM_KEY_UP, vk, shift);
	return true;
}

void TVPAgentRequestRedraw()
{
	if (Application) Application->RequestUpdate();
}
