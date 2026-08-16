
#ifndef __VSYNC_TIMING_THREAD_H__
#define __VSYNC_TIMING_THREAD_H__

#include "ThreadIntf.h"
#include "NativeEventQueue.h"

//---------------------------------------------------------------------------
// VSync用のタイミングを発生させるためのスレッド
//---------------------------------------------------------------------------
class tTVPVSyncTimingThread : public tTVPThread
{
	DWORD SleepTime;
	tTVPThreadEvent Event;
	tTJSCriticalSection CS;
	DWORD VSyncInterval; //!< VSync の間隔(参考値)
	tjs_uint64 LastVBlankTick; //!< 最後の vblank の時間 (ms, TVPGetTickCount 基準)

	HANDLE TimerHandle; //!< high-resolution waitable timer (前眠り用)

	bool Enabled;

	//!< 直近の vblank 待ちの結果 (ワーカースレッドで取得し、メインスレッドの Proc が読む)
	tjs_int LastInVBlank;
	tjs_int LastDelayed;

	NativeEventQueue<tTVPVSyncTimingThread> EventQueue;

	class tTJSNI_Window* OwnerWindow;

	void PreciseSleep( DWORD ms ); //!< タイマ分解能に依存しない精密な短時間スリープ
public:
	tTVPVSyncTimingThread(class tTJSNI_Window* owner);
	~tTVPVSyncTimingThread();

protected:
	void Execute();
	void Proc( NativeEvent& ev );

public:
	void MeasureVSyncInterval(); // VSyncInterval を計測する
};
//---------------------------------------------------------------------------

#endif // __VSYNC_TIMING_THREAD_H__
