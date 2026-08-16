
#include "tjsCommHead.h"

#include "VSyncTimingThread.h"
#include "WindowImpl.h"
#include "EventIntf.h"
#include "UserEvent.h"
#include "DebugIntf.h"
#include "MsgImpl.h"
#include "TickCount.h"

//---------------------------------------------------------------------------
tTVPVSyncTimingThread::tTVPVSyncTimingThread(tTJSNI_Window* owner)
	 : tTVPThread("VSyncTimingThread"), EventQueue(this,&tTVPVSyncTimingThread::Proc), OwnerWindow(owner)
{
	SleepTime = 1;
	LastVBlankTick = 0;
	VSyncInterval = 16; // 約60FPS
	Enabled = false;
	LastInVBlank = 0;
	LastDelayed = 0;

	// high-resolution waitable timer を用意する。これにより timeBeginPeriod で
	// システム全体のタイマ分解能を上げなくても、~0.5ms 精度の短時間スリープが
	// できる (Win10 1803+ で CREATE_WAITABLE_TIMER_HIGH_RESOLUTION が有効)。
	TimerHandle = ::CreateWaitableTimerExW(NULL, NULL,
		CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
	if(TimerHandle == NULL) // 古い環境向けフォールバック (通常到達しない)
		TimerHandle = ::CreateWaitableTimerExW(NULL, NULL, 0, TIMER_ALL_ACCESS);

	EventQueue.Allocate();
	MeasureVSyncInterval();
	StartThread();
}
//---------------------------------------------------------------------------


//---------------------------------------------------------------------------
tTVPVSyncTimingThread::~tTVPVSyncTimingThread()
{
	Terminate();
	Event.Set();
	WaitFor();
	EventQueue.Deallocate();
	if(TimerHandle) { ::CloseHandle(TimerHandle); TimerHandle = NULL; }
}
//---------------------------------------------------------------------------
void tTVPVSyncTimingThread::PreciseSleep( DWORD ms )
{
	// high-resolution waitable timer による精密スリープ。相対指定は 100ns 単位の
	// 負値で与える。失敗時は通常の Sleep にフォールバック。
	if(TimerHandle && ms > 0)
	{
		LARGE_INTEGER due;
		due.QuadPart = -(LONGLONG)ms * 10000LL; // ms → 100ns 単位・相対
		if(::SetWaitableTimer(TimerHandle, &due, 0, NULL, NULL, FALSE))
		{
			::WaitForSingleObject(TimerHandle, INFINITE);
			return;
		}
	}
	::Sleep(ms);
}
//---------------------------------------------------------------------------


//---------------------------------------------------------------------------
void tTVPVSyncTimingThread::Execute()
{
	while(!GetTerminated())
	{
		// SleepTime と LastVBlankTick を得る
		DWORD sleep_time;
		tjs_uint64 last_vblank_tick;
		{	// thread-protected
			tTJSCriticalSectionHolder holder(CS);
			sleep_time = SleepTime;
			last_vblank_tick = LastVBlankTick;
		}

		// SleepTime 分眠る
		// LastVBlankTick から起算し、SleepTime 分眠る
		tjs_uint64 sleep_start_tick = TVPGetTickCount();

		tjs_uint64 sleep_time_adj = sleep_start_tick - last_vblank_tick;

		if(sleep_time_adj < sleep_time)
		{
			PreciseSleep((DWORD)(sleep_time - sleep_time_adj));
		}
		else
		{
			// 普通、メインスレッド内で Event.Set() したならば、
			// タイムスライス(長くて10ms) が終わる頃は
			// ここに来ているはずである。
			// sleep_time は通常 10ms より長いので、
			// ここに来るってのは異常。
			// よほどシステムが重たい状態になってると考えられる。
			// そこで立て続けに イベントをポストするわけにはいかないので
			// 適当な時間(本当に適当) 眠る。
			PreciseSleep(5);
		}

		// vblank 待ちはこのワーカースレッドで行う。
		//
		//   以前はメインスレッド (下の Proc、 = ここから投函するメッセージの
		//   ウィンドウプロシージャ内) で WaitForVBlank していたが、これは
		//   IDXGIOutput::WaitForVBlank() が次の垂直帰線までブロックする同期待ち
		//   なので、メッセージ 1 件の処理に丸々 1 フレームかかることになる。
		//   Windows のメッセージ取り出し優先順位は「ポストされたメッセージ >
		//   キュー入力 (マウス/キー)」なので、ウィンドウが複数あって
		//   VSyncTimingThread が複数本走ると (例: モーダルウィンドウ表示中は
		//   本体 + ダイアログの 2 本) 、ポストメッセージの処理だけでメインスレッド
		//   が飽和し、マウス入力が永久にキューから取り出されなくなる。
		//   実際にモーダルダイアログがマウス操作を一切受け付けなくなる不具合が
		//   発生していた (WM_NCHITTEST は SendMessage なので届くが、
		//   WM_MOUSEMOVE / WM_LBUTTONDOWN が届かない、という症状)。
		//
		//   ブロックしてよいのはこのワーカーなので、待ちをこちらへ移す。
		//   メインスレッド側 (Proc) は結果を読んで画面更新するだけになる。
		{
			tjs_int in_vblank = 0;
			tjs_int delayed = 0;
			bool supportvwait = false;
			if( OwnerWindow ) supportvwait = OwnerWindow->WaitForVBlank( &in_vblank, &delayed );
			if( supportvwait == false ) {
				// VBlank 待ちはサポートされていないので、気にせずそのまま進行
				// (待ち時間はいい加減だが気にしないことにする)
				in_vblank = 0;
				delayed = 0;
			}
			tTJSCriticalSectionHolder holder(CS);
			LastInVBlank = in_vblank;
			LastDelayed = delayed;
		}
		if( GetTerminated() ) break;

		// イベントをポストする
		NativeEvent ev(TVP_EV_VSYNC_TIMING_THREAD);
		ev.LParam = (LPARAM)sleep_start_tick;
		EventQueue.PostEvent(ev);

		Event.WaitFor(0x7fffffff); // メインスレッドの描画完了まで待つ
	}
}
//---------------------------------------------------------------------------


//---------------------------------------------------------------------------
void tTVPVSyncTimingThread::Proc( NativeEvent& ev )
{
	if(ev.Message != TVP_EV_VSYNC_TIMING_THREAD) {
		EventQueue.HandlerDefault(ev);
		return;
	}
	if( OwnerWindow == NULL ) return;

	// tTVPVSyncTimingThread から投げられたメッセージ
	// vblank 待ち自体は Execute() (ワーカースレッド) で済んでいる。ここで待つと
	// メッセージポンプが 1 フレーム止まり、キュー入力が飢餓状態になる (Execute()
	// のコメント参照)。

	tjs_int in_vblank = 0;
	tjs_int delayed = 0;
	{
		tTJSCriticalSectionHolder holder(CS);
		in_vblank = LastInVBlank;
		delayed = LastDelayed;
	}

	// タイマの時間原点を設定する
	if(!delayed)
	{
		tTJSCriticalSectionHolder holder(CS);
		LastVBlankTick = TVPGetTickCount(); // これが次に眠る時間の起算点になる
	}
	else
	{
		tTJSCriticalSectionHolder holder(CS);
		LastVBlankTick += VSyncInterval; // これが次に眠る時間の起算点になる(おおざっぱ)
		if((tjs_int64) (TVPGetTickCount() - (LastVBlankTick + SleepTime)) <= 0)
		{
			// 眠った後、次に起きようとする時間がすでに過去なので眠れません
			LastVBlankTick = TVPGetTickCount(); // 強制的に今の時刻にします
		}
	}

	// 画面の更新を行う (DrawDeviceのShowメソッドを呼ぶ)
	OwnerWindow->DeliverDrawDeviceShow();

	// もし vsync 待ちを行う直前、すでに vblank に入っていた場合は、
	// 待つ時間が長すぎたと言うことである
	if(in_vblank)
	{
		// その場合は SleepTime を減らす
		tTJSCriticalSectionHolder holder(CS);
		if(SleepTime > 8) SleepTime --;
	}
	else
	{
		// vblank で無かった場合は二つの場合が考えられる
		// 1. vblank 前だった
		// 2. vblank 後だった
		// どっちかは分からないが
		// SleepTime を増やす。ただしこれが VSyncInterval を超えるはずはない。
		tTJSCriticalSectionHolder holder(CS);
		SleepTime ++;
		if(SleepTime > VSyncInterval) SleepTime = VSyncInterval;
	}

	// タイマを起動する
	Event.Set();

	// ContinuousHandler を呼ぶ
	// これは十分な時間をとれるよう、vsync 待ちの直後に呼ばれる
	TVPProcessContinuousHandlerEventFlag = true; // set flag to invoke continuous handler on next idle
}
//---------------------------------------------------------------------------


//---------------------------------------------------------------------------
void tTVPVSyncTimingThread::MeasureVSyncInterval()
{
	TVPEnsureDirect3DObject();

	DWORD vsync_interval = 10000;
	DWORD vsync_rate = 0;

	HDC dc = ::GetDC(0);
	vsync_rate = ::GetDeviceCaps(dc, VREFRESH);
	::ReleaseDC(0, dc);

	if(vsync_rate != 0)
		vsync_interval = 1000 / vsync_rate;
	else
		vsync_interval = 0;

	TVPAddLog( TVPFormatMessage(TVPRoughVsyncIntervalReadFromApi,ttstr((int)vsync_interval)) );

	// vsync 周期は適切っぽい？
	if(vsync_interval < 6 || vsync_interval > 66)
	{
		TVPAddLog( (const tjs_char*)TVPRoughVsyncIntervalStillSeemsWrong );
		vsync_interval = 16;
	}

	VSyncInterval = vsync_interval;
}
//---------------------------------------------------------------------------
