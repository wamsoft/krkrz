//---------------------------------------------------------------------------
/*
	TVP2 ( T Visual Presenter 2 )  A script authoring tool
	Copyright (C) 2000 W.Dee <dee@kikyou.info> and contributors

	See details of license at "license.txt"
*/
//---------------------------------------------------------------------------
// Script Event Handling and Dispatching
//---------------------------------------------------------------------------
#include "tjsCommHead.h"

#include "EventIntf.h"
#include "ThreadIntf.h"
#include "TickCount.h"
#include "TimerIntf.h"
#include "SysInitIntf.h"
#include "DebugIntf.h"
#include "WindowImpl.h"

#include "Application.h"
#include "UserEvent.h"

//---------------------------------------------------------------------------
// TVPInvokeEvents for EventIntf.cpp
// 各プラットフォームで実装する。
// アプリケーションの準備が出来ている時、イベントの配信を確実にするために必要
// 吉里吉里のイベントがPostされた後にコールされる。
// Windowsではメッセージディスパッチャでイベントをハンドリングするために
// ダミーメッセージをメインウィンドウへ送っている(::PostMessage)
//---------------------------------------------------------------------------
void TVPInvokeEvents()
{
}
//---------------------------------------------------------------------------


//---------------------------------------------------------------------------
// TVPEventReceived for EventIntf.cpp
// 各プラットフォームで実装する。
// 吉里吉里のイベント配信後、次のイベントの準備のために呼び出される。
// Windowsでは特に何もしていない
//---------------------------------------------------------------------------
void TVPEventReceived()
{
}
//---------------------------------------------------------------------------

//---------------------------------------------------------------------------
// TVPCallDeliverAllEventsOnIdle for EventIntf.cpp
// 各プラットフォームで実装する。
// 一度OSに制御を戻し、その後TVPInvokeEvents（）を呼び出すように設定。
// Windowsではダミーメッセージをメインウィンドウへ送っている(::PostMessage)
//---------------------------------------------------------------------------
void TVPCallDeliverAllEventsOnIdle()
{
}
//---------------------------------------------------------------------------

//---------------------------------------------------------------------------
// TVPBreathe
// implement this in each platform
// to handle OS's message events
// this should be called when in a long time processing, something like a
// network access, to reply to user's Windowing message, repainting, etc...
// in TVPBreathe, TVP events must not be invoked. ( happened events over the
// long time processing are pending until next timing of message delivering. )
// 各プラットフォームで実装する。
// 長時間メインスレッドを止めてしまう時にGUI等が固まらないように定期的に呼び出す
// プラグイン用に提供されている
//---------------------------------------------------------------------------
static bool TVPBreathing = false;
void TVPBreathe()
{
	TVPEventDisabled = true; // not to call TVP events...
	TVPBreathing = true;
	try
	{
		Application->ProcessMessages(); // do Windows message pumping
	}
	catch(...)
	{
		TVPBreathing = false;
		TVPEventDisabled = false;
		throw;
	}

	TVPBreathing = false;
	TVPEventDisabled = false;
}

//---------------------------------------------------------------------------
// breathing処理中かどうか
//---------------------------------------------------------------------------
bool TVPGetBreathing()
{
	// return whether now is in event breathing
	return TVPBreathing;
}
//---------------------------------------------------------------------------

//---------------------------------------------------------------------------
// TVPSystemEventDisabledState
// sets whether system overall event handling is enabled.
// this works distinctly from TVPEventDisabled.
// イベントハンドリングの有効/無効を設定する
//
// WINVER は TVPSystemControl 経由でメッセージループ駆動の配信を止めるが、
// Generic(SDL) は SDL_AppIterate が毎フレーム配信を回す連続ループのため、
// 専用フラグを持ち Application::DeliverEvents() がこれを見て配信を止める。
// 共通層 (TVPPostEvent / TVPPostInputEvent) も discardable イベントの
// post 抑止にこのフラグを参照する (EventIntf.cpp)。
//---------------------------------------------------------------------------
static bool TVPSystemEventDisabled = false;
void TVPSetSystemEventDisabledState(bool en)
{
	TVPSystemEventDisabled = en;
	// 再有効化 (en==false) 時は溜まっていたイベントを掃き出す。
	// WINVER の SetEventEnabled(true) 直後の TVPDeliverAllEvents() に相当。
	if(!en) TVPDeliverAllEvents();
}
//---------------------------------------------------------------------------
bool TVPGetSystemEventDisabledState()
{
	return TVPSystemEventDisabled;
}
//---------------------------------------------------------------------------

//---------------------------------------------------------------------------
void TVPBeginContinuousEvent()
{
	Application->BeginContinuousEvent();
}
//---------------------------------------------------------------------------
void TVPEndContinuousEvent()
{
	Application->EndContinuousEvent();
}

