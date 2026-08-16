//---------------------------------------------------------------------------
/*
	TVP2 ( T Visual Presenter 2 )  A script authoring tool
	Copyright (C) 2000 W.Dee <dee@kikyou.info> and contributors

	See details of license at "license.txt"
*/
//---------------------------------------------------------------------------
// Timer Object Implementation
//---------------------------------------------------------------------------
#include "tjsCommHead.h"

#include <algorithm>
#include "EventIntf.h"
#include "TickCount.h"
#include "SysInitIntf.h"
#include "ThreadIntf.h"
#include "MsgIntf.h"
#include "DebugIntf.h"

#include "UserEvent.h"

#include "TimerThread.h"
#include "TimerIntf.h"

//---------------------------------------------------------------------------

// TVP Timer class gives ability of triggering event on punctual interval.
// a large quantity of event at once may easily cause freeze to system,
// so we must trigger only porocess-able quantity of the event.
#define TVP_LEAST_TIMER_INTERVAL 3

#define TVP_TIME_INFINITE 0

static tTVPTimerThread * TVPTimerThread = nullptr;
//---------------------------------------------------------------------------
tTVPTimerThread::tTVPTimerThread() : tTVPThread("TimerThread")
#ifdef __WINVER__
	, EventQueue(this,&tTVPTimerThread::Proc)
#endif
{
	PendingEventsAvailable = false;
	SetPriority(TVPLimitTimerCapacity ? ttpNormal : ttpHighest);
#ifdef __WINVER__
	EventQueue.Allocate();
#else
	Application->addEventHandler(this);
#endif
	StartThread();
}
//---------------------------------------------------------------------------
tTVPTimerThread::~tTVPTimerThread()
{
	Terminate();
	Event.Set();
	WaitFor();
#ifdef __WINVER__
	EventQueue.Deallocate();
#else
	Application->removeEventHandler(this);
#endif
}
//---------------------------------------------------------------------------
void tTVPTimerThread::Execute()
{
	while(!GetTerminated())
	{
		tjs_uint64 step_next = (tjs_uint64)(tjs_int64)-1L; // invalid value
		tjs_uint64 curtick = TVPGetTickCount() << TVP_SUBMILLI_FRAC_BITS;
		tjs_uint sleeptime;

		{	// thread-protected
			tTJSCriticalSectionHolder holder(TVPTimerCS);

			bool any_triggered = false;

			std::vector<tTVPTimerBase*>::iterator i;
			for(i = List.begin(); i!=List.end(); i ++)
			{
				tTVPTimerBase * item = *i;

				if(!item->GetEnabled() || item->GetInterval() == 0) continue;

				if(item->GetNextTick() < curtick)
				{
					tjs_uint n = static_cast<tjs_uint>( (curtick - item->GetNextTick()) / item->GetInterval() );
					n++;
					if(n > 40)
					{
						// too large amount of event at once; discard rest
						item->Trigger(1);
						any_triggered = true;
						item->SetNextTick(curtick + item->GetInterval());
					}
					else
					{
						item->Trigger(n);
						any_triggered = true;
						item->SetNextTick(item->GetNextTick() +
							n * item->GetInterval());
					}
				}


				tjs_uint64 to_next = item->GetNextTick() - curtick;

				if(step_next == (tjs_uint64)(tjs_int64)-1L)
				{
					step_next = to_next;
				}
				else
				{
					if(step_next > to_next) step_next = to_next;
				}
			}


			if(step_next != (tjs_uint64)(tjs_int64)-1L)
			{
				// too large step_next must be diminished to size of tjs_uint.
				if(step_next >= 0x80000000)
					sleeptime = 0x7fffffff; // smaller value than step_next is OK
				else {
					if( step_next == 0 ) {
						step_next = 1;
					}
					sleeptime = static_cast<tjs_uint>( step_next );
				}
			}
			else
			{
				sleeptime = TVP_TIME_INFINITE;
			}

			if( List.size() == 0 ) {
				sleeptime = TVP_TIME_INFINITE;
			}

			if(any_triggered)
			{
				// triggered; post notification message to the main thread
				if(!PendingEventsAvailable)
				{
#ifdef __WINVER__
					// ラッチは投函に成功したときだけ立てる。 PostMessage は
					// メッセージキューが上限に達すると失敗するので、失敗しても
					// 立ててしまうと「起こされないのに投函済み扱い」になり、
					// 以後タイマーが永久に動かなくなる。
					if( EventQueue.PostEvent( NativeEvent(TVP_EV_TIMER_THREAD) ) )
						PendingEventsAvailable = true;
#else
					// SendAppEvent は失敗時にリトライキューへ積むので取りこぼさない
					PendingEventsAvailable = true;
					Application->SendAppEvent( TVP_EV_TIMER_THREAD, 0, 0 );
#endif
				}
			}

		}	// end-of-thread-protected

		// now, sleeptime has sub-milliseconds precision but we need millisecond
		// precision time.
		if(sleeptime != TVP_TIME_INFINITE)
			sleeptime = (sleeptime >> TVP_SUBMILLI_FRAC_BITS) + (sleeptime & ((1<<TVP_SUBMILLI_FRAC_BITS)-1) ? 1: 0); // round up

		// clamp to TVP_LEAST_TIMER_INTERVAL ...
		if(sleeptime != TVP_TIME_INFINITE && sleeptime < TVP_LEAST_TIMER_INTERVAL)
			sleeptime = TVP_LEAST_TIMER_INTERVAL;

		Event.WaitFor(sleeptime); // wait until sleeptime is elapsed or
									// Event->SetEvent() is executed.
	}
}
//---------------------------------------------------------------------------
// wake メッセージを受けたときの本体処理 (メインスレッド)。
void tTVPTimerThread::HandleWake()
{
	// pending events occur
	//
	// Fire は必ず「ロックを手放し、かつ PendingEventsAvailable を下ろしてから」
	// 呼ぶこと。 Fire の先ではタイマーハンドラ (tTVPTimer なら直接、TJS の
	// Timer ならイベント配送経由) が走り、そこから Window.showModal のような
	// ネストしたメッセージループへ入ると HandleWake が戻らなくなる。
	//
	// 以前は TVPTimerCS を Fire の間ずっと握り、PendingEventsAvailable を最後に
	// false へ戻していたため、その状態になると
	//   - タイマースレッドが CS 待ちで停止する
	//   - PendingEventsAvailable が true のままなので wake も二度と投函されない
	// となり、モーダルウィンドウ表示中は TJS の Timer も
	// tTVPSystemControl の 50ms 監視タイマー (= イベント配送の駆動源) も
	// 完全に止まっていた (実測: モーダル中に仕掛けた Timer が一度も発火しない)。
	//
	// ProcWork をメンバからローカルへ移したのは、ネストループ内で HandleWake が
	// 再入しても壊れないようにするため。
	std::vector<tTVPTimerBase *> work;
	{
		tTJSCriticalSectionHolder holder(TVPTimerCS); // protect the object
		work.swap( Pending );
		PendingEventsAvailable = false;
	}

	for( auto i = work.begin(); i != work.end(); i++ ) {
		tTVPTimerBase *item = *i;
		tjs_int count = 0;
		{
			// Fire の中で他のタイマーが破棄されることがあるので、毎回
			// List に残っているか確認してからペンディング数を取り出す。
			tTJSCriticalSectionHolder holder(TVPTimerCS);
			if( std::find( List.begin(), List.end(), item ) == List.end() ) continue;
			count = item->TakePendingCount();
		}
		item->FirePendingEvents( count );
	}
}
//---------------------------------------------------------------------------
#ifdef __WINVER__
void tTVPTimerThread::Proc( NativeEvent& ev )
{
	// Window procedure of UtilWindow
	if( ev.Message == TVP_EV_TIMER_THREAD && !GetTerminated())
		HandleWake();
	else
		EventQueue.HandlerDefault(ev);
}
#else
bool tTVPTimerThread::Dispatch( tjs_int message, tjs_int64 /*wparam*/, tjs_int64 /*lparam*/ )
{
	if( message == TVP_EV_TIMER_THREAD && !GetTerminated() ) {
		HandleWake();
		return true;
	}
	return false;
}
#endif
//---------------------------------------------------------------------------
void tTVPTimerThread::AddItem(tTVPTimerBase * item)
{
	tTJSCriticalSectionHolder holder(TVPTimerCS);

	if(std::find(List.begin(), List.end(), item) == List.end())
		List.push_back(item);
}
//---------------------------------------------------------------------------
bool tTVPTimerThread::RemoveItem(tTVPTimerBase *item)
{
	tTJSCriticalSectionHolder holder(TVPTimerCS);

	// remove from the List
	for( auto i = List.begin(); i != List.end(); /**/)
	{
		if(*i == item) i = List.erase(i); else i++;
	}

	// also remove from the Pending list
	RemoveFromPendingItem(item);

	return List.size() != 0;
}
//---------------------------------------------------------------------------
void tTVPTimerThread::RemoveFromPendingItem(tTVPTimerBase *item)
{
	// remove item from pending list
	for( auto i = Pending.begin(); i != Pending.end(); /**/)
	{
		if(*i == item) i = Pending.erase(i); else i++;
	}
	item->ZeroPendingCount();
}
//---------------------------------------------------------------------------
void tTVPTimerThread::RegisterToPendingItem(tTVPTimerBase *item)
{
	// register item to the pending list
	Pending.push_back(item);
}
//---------------------------------------------------------------------------
void tTVPTimerThread::SetEnabled(tTVPTimerBase *item, bool enabled)
{
	{ // thread-protected
		tTJSCriticalSectionHolder holder(TVPTimerCS);

		item->InternalSetEnabled(enabled);
		if(enabled)
		{
			item->SetNextTick((TVPGetTickCount()  << TVP_SUBMILLI_FRAC_BITS) + item->GetInterval());
		}
		else
		{
			item->CancelEvents();
			item->ZeroPendingCount();
		}
	} // end-of-thread-protected

	if(enabled) Event.Set();
}
//---------------------------------------------------------------------------
void tTVPTimerThread::SetInterval(tTVPTimerBase *item, tjs_uint64 interval)
{
	{ // thread-protected
		tTJSCriticalSectionHolder holder(TVPTimerCS);

		item->InternalSetInterval(interval);
		if(item->GetEnabled())
		{
			item->CancelEvents();
			item->ZeroPendingCount();
			item->SetNextTick((TVPGetTickCount()  << TVP_SUBMILLI_FRAC_BITS) + item->GetInterval());
		}
	} // end-of-thread-protected

	if(item->GetEnabled()) Event.Set();

}
//---------------------------------------------------------------------------
void tTVPTimerThread::Init()
{
	if(!TVPTimerThread)
	{
		TVPStartTickCount(); // in TickCount.cpp
		TVPTimerThread = new tTVPTimerThread();
	}
}
//---------------------------------------------------------------------------
void tTVPTimerThread::Uninit()
{
	if(TVPTimerThread)
	{
		delete TVPTimerThread;
		TVPTimerThread = nullptr;
	}
}
//---------------------------------------------------------------------------
static tTVPAtExit TVPTimerThreadUninitAtExit(TVP_ATEXIT_PRI_SHUTDOWN,
	tTVPTimerThread::Uninit);
//---------------------------------------------------------------------------
void tTVPTimerThread::Add(tTVPTimerBase * item)
{
	// at this point, item->GetEnebled() must be false.

	Init();

	TVPTimerThread->AddItem(item);
}
//---------------------------------------------------------------------------
void tTVPTimerThread::Remove(tTVPTimerBase *item)
{
	if(TVPTimerThread)
	{
		if(!TVPTimerThread->RemoveItem(item)) Uninit();
	}
}
//---------------------------------------------------------------------------
void tTVPTimerThread::RemoveFromPending(tTVPTimerBase *item)
{
	if(TVPTimerThread)
	{
		TVPTimerThread->RemoveFromPendingItem(item);
	}
}
//---------------------------------------------------------------------------
void tTVPTimerThread::RegisterToPending(tTVPTimerBase *item)
{
	if(TVPTimerThread)
	{
		TVPTimerThread->RegisterToPendingItem(item);
	}
}
//---------------------------------------------------------------------------


//---------------------------------------------------------------------------
tTVPTimerBase::tTVPTimerBase()
 : NextTick(0), Interval(1000 << TVP_SUBMILLI_FRAC_BITS ), PendingCount(0), Enabled(false)
{
}
//---------------------------------------------------------------------------
void tTVPTimerBase::SetEnabled(bool b)
{
	TVPTimerThread->SetEnabled(this, b);
}
//---------------------------------------------------------------------------
void tTVPTimerBase::SetInterval(tjs_uint64 n)
{
	TVPTimerThread->SetInterval(this, n);
}
//---------------------------------------------------------------------------
void tTVPTimerBase::Trigger(tjs_uint n)
{
	// this function is called by sub-thread.
	if(PendingCount == 0) tTVPTimerThread::RegisterToPending(this);
	PendingCount += n;
}
//---------------------------------------------------------------------------
void tTVPTimerBase::FirePendingEventsAndClear()
{
	// fire all pending events and clear the pending event count
	if(PendingCount)
	{
		Fire(PendingCount);
		ZeroPendingCount();
	}
}

