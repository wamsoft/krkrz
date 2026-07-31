/****************************************************************************/
/*! @file
@brief レイヤ再生用ビデオプレイヤ共通基底の実装
*****************************************************************************/
#include <windows.h>
#include "tp_stub.h"
#include "LayerVideoBase.h"
#include "MovieAudioSink.h"
#include "D3D11OverlayWindow.h"
#include <chrono>
#include <cmath>

// EC_COMPLETE は DirectShow の定数だが、ここでは疑似イベントコードとして使う
// (エンジン側の EC_COMPLETE ハンドラと合わせる)。webplayer.cpp と同様に局所定義。
#ifndef EC_COMPLETE
#define EC_COMPLETE 0x0001
#endif

//---------------------------------------------------------------------------
tTVPLayerVideoBase::tTVPLayerVideoBase( HWND owner, bool overlayOutput, bool preferI420 )
: VideoWidth(0), VideoHeight(0), VideoFPS(0.0), DurationMs(0)
, State(stStopped), Terminate(false), DoSeek(false), SeekMs(0)
, OwnerWindow(owner), FrontIdx(0), BufferSize(0)
, Updated(false), Completed(false), CurPtsMs(0)
, ClockValid(false), AudioEpochMs(0), PtsEpochMs(0)
, RefCount(1), Loop(false), AudioVolMB(0), OverlayMode(overlayOutput), Overlay(nullptr)
, PreferI420(preferI420 && !overlayOutput)   // overlayOutput が優先
, I420W(0), I420H(0), I420Valid(false), I420Dirty(false)
, Audio(nullptr)
{
	Buf[0] = nullptr;
	Buf[1] = nullptr;
}
//---------------------------------------------------------------------------
tTVPLayerVideoBase::~tTVPLayerVideoBase()
{
	// 安全網: 派生デストラクタで StopThread() 済みのはずだが、念のため。
	// (DecoderClose() はここでは呼べない = 純粋仮想呼び出しになるため、派生側で行う)
	StopThread();
	// スレッド停止後に音声シンク / overlay を破棄 (デコードスレッドが使うため順序重要)
	if( Audio ) { delete Audio; Audio = nullptr; }
	if( Overlay ) { delete Overlay; Overlay = nullptr; }
}
//---------------------------------------------------------------------------
void tTVPLayerVideoBase::StopThread()
{
	Terminate = true;
	{
		std::lock_guard<std::mutex> lk(Mtx);
		Cond.notify_all();
	}
	if( DecodeThread.joinable() ) DecodeThread.join();
}
//---------------------------------------------------------------------------
bool tTVPLayerVideoBase::Open( IStream *stream, const tjs_char *streamname, const tjs_char *type, unsigned __int64 size )
{
	if( !DecoderOpen( stream, type, size ) ) return false;

	long w = 0, h = 0; double fps = 0.0; __int64 dur = 0;
	if( !DecoderGetInfo( w, h, fps, dur ) ) { DecoderClose(); return false; }
	VideoWidth  = w;
	VideoHeight = h;
	VideoFPS    = (fps > 0.0) ? fps : 30.0;
	DurationMs  = dur;

	// デコードスレッド開始 (Play まではアイドル)
	Terminate = false;
	State = stStopped;
	DecodeThread = std::thread( &tTVPLayerVideoBase::ThreadMain, this );
	return true;
}
//---------------------------------------------------------------------------
void tTVPLayerVideoBase::ThreadMain()
{
	const bool hasAudio = DecoderHasAudio();
	__int64 prevPts = 0;
	bool havePrev = false;

	auto interrupted = [&]{ return Terminate.load() || DoSeek.load() || State.load() != stPlaying; };

	for(;;)
	{
		// 再生指示待ち
		{
			std::unique_lock<std::mutex> lk(Mtx);
			Cond.wait( lk, [&]{ return Terminate.load() || DoSeek.load() || State.load() == stPlaying; } );
		}
		if( Terminate ) break;

		if( DoSeek.exchange(false) )
		{
			DecoderSeek( SeekMs.load() );
			if( Audio ) Audio->Flush();
			CurPtsMs = SeekMs.load();
			havePrev = false;
			ClockValid = false;   // 同期クロック基準を取り直す
			continue;
		}
		if( State.load() != stPlaying ) continue;

		// 出力先が未準備なら少し待って再試行 (layer=バッファ / overlay=子ウィンドウ /
		// preferI420=内部保持なので寸法のみ確認)
		bool notReady = PreferI420  ? ( VideoWidth <= 0 || VideoHeight <= 0 )
		              : OverlayMode ? ( Overlay == nullptr )
		                            : ( !Buf[0] || VideoWidth <= 0 || VideoHeight <= 0 );
		if( notReady )
		{
			std::unique_lock<std::mutex> lk(Mtx);
			Cond.wait_for( lk, std::chrono::milliseconds(16), [&]{ return interrupted(); } );
			continue;
		}

		// 音声を先に供給してシンクを満たす (A/V 同期のマスタクロック源)
		if( hasAudio ) { DecoderPumpAudio(); if( Audio ) Audio->DrainConsumed(); }

		// 1 フレームデコード (overlay=内部保持 / layer=裏バッファへボトムアップ書き込み)
		__int64 pts = 0;
		bool eos = false;
		bool got = false;
		int back = 0;
		if( OverlayMode || PreferI420 )
		{
			// overlay / presenter(I420): デコードして内部保持 (present/buffer は sync 後)
			got = DecoderDecodeOverlay( pts, eos );
		}
		else
		{
			back = 1 - FrontIdx.load();
			long rowBytes = VideoWidth * 4;
			BYTE *dst = Buf[back] + (size_t)( VideoHeight - 1 ) * rowBytes; // 最終行 (ボトムアップ)
			got = DecoderReadFrame( dst, -rowBytes, pts, eos );
		}

		if( !got || eos )
		{
			Completed = true;
			State = stEnded;
			if( OwnerWindow ) ::PostMessage( OwnerWindow, WM_GRAPHNOTIFY, 0, 0 );
			continue;
		}

		if( hasAudio && Audio )
		{
			// 音声マスタクロックに同期。基準が無ければこのフレームで確立。
			if( !ClockValid )
			{
				AudioEpochMs = Audio->GetPlayedMs();
				PtsEpochMs   = pts;
				ClockValid   = true;
			}
			// 音声クロックがこのフレームの pts に追いつくまで、音声を供給しつつ待つ
			for(;;)
			{
				if( interrupted() ) break;
				DecoderPumpAudio();
				if( Audio ) Audio->DrainConsumed();
				int64_t clk = AudioClockMs();
				if( clk < 0 || clk >= pts ) break;
				int64_t wait = pts - clk;
				if( wait > 100 ) wait = 100;
				std::unique_lock<std::mutex> lk(Mtx);
				Cond.wait_for( lk, std::chrono::milliseconds(wait), [&]{ return interrupted(); } );
			}
		}
		else
		{
			// 音声なし: フレーム間 delta で提示ペースを作る
			__int64 delta = havePrev ? ( pts - prevPts ) : 0;
			prevPts = pts;
			havePrev = true;
			if( delta > 0 )
			{
				if( delta > 1000 ) delta = 1000; // 安全クランプ
				std::unique_lock<std::mutex> lk(Mtx);
				Cond.wait_for( lk, std::chrono::milliseconds(delta), [&]{ return interrupted(); } );
			}
		}
		if( Terminate ) break;

		// 提示
		if( PreferI420 )
		{
			// presenter 経路: 保持 I420 を内部バッファへ copy (engine が GetI420Frame で pull)
			BufferI420FromDecoder();
		}
		else if( OverlayMode )
		{
			// 保持フレームを子ウィンドウへ present (D3D11 YUV)
			if( Overlay )
			{
				DecoderPresentOverlay( Overlay );
				// デバッグ: KRMOVIE_OVERLAY_DUMP にパスがあれば ~1s 地点の 1 フレームを
				// BMP 保存 (子ウィンドウは captureScreen 非対応のため自己検証用)。未設定なら無害。
				static bool dumped = false;
				if( !dumped && pts > 1000 )
				{
					const char *dp = getenv( "KRMOVIE_OVERLAY_DUMP" );
					if( dp && *dp )
					{
						wchar_t wp[1024]; wp[0] = 0;
						MultiByteToWideChar( CP_ACP, 0, dp, -1, wp, 1024 );
						Overlay->DebugSaveLastFrame( wp );
						dumped = true;
					}
				}
			}
		}
		else
		{
			// 書き終えた裏バッファを表に切替 (エンジンが GetFrontBuffer で新フレームを
			// 得てその Bitmap を表示。次デコードは旧表=新裏へ書き表示中を上書きしない)
			FrontIdx.store( back );
		}
		CurPtsMs = pts;
		Updated = true;
		if( OwnerWindow ) ::PostMessage( OwnerWindow, WM_GRAPHNOTIFY, 0, 0 );
	}
}
//---------------------------------------------------------------------------
int64_t tTVPLayerVideoBase::AudioClockMs()
{
	if( !Audio || !ClockValid ) return -1;
	int64_t played = Audio->GetPlayedMs();
	if( played < 0 ) return -1;
	return PtsEpochMs + ( played - AudioEpochMs );
}
//---------------------------------------------------------------------------
bool tTVPLayerVideoBase::CreateAudioSink( int channels, int sampleRate, int bitsPerSample, bool isFloat )
{
	if( Audio ) return true;
	tTVPMovieAudioSink *sink = new tTVPMovieAudioSink();
	if( !sink->IsAvailable() || !sink->Setup( channels, sampleRate, bitsPerSample, isFloat ) )
	{
		delete sink;
		return false;
	}
	Audio = sink;
	return true;
}
//---------------------------------------------------------------------------
void tTVPLayerVideoBase::DecoderSetVolume( float v )
{
	if( Audio ) Audio->SetVolume( v );
}
//---------------------------------------------------------------------------
void __stdcall tTVPLayerVideoBase::AddRef() { RefCount++; }
void __stdcall tTVPLayerVideoBase::Release() { if( RefCount == 1 ) delete this; else RefCount--; }
//---------------------------------------------------------------------------
void __stdcall tTVPLayerVideoBase::Play()
{
	std::lock_guard<std::mutex> lk(Mtx);
	bool fresh = ( State == stStopped || State == stEnded );
	if( State == stEnded ) { DoSeek = true; SeekMs = 0; }
	if( fresh ) ClockValid = false; // 新規再生はクロック基準を取り直す (pause 復帰は維持)
	State = stPlaying;
	Completed = false;
	if( Audio ) Audio->Start();
	Cond.notify_all();
}
//---------------------------------------------------------------------------
void __stdcall tTVPLayerVideoBase::Stop()
{
	std::lock_guard<std::mutex> lk(Mtx);
	State = stStopped;
	ClockValid = false;
	if( Audio ) Audio->Stop();
	Cond.notify_all();
}
//---------------------------------------------------------------------------
void __stdcall tTVPLayerVideoBase::Pause()
{
	std::lock_guard<std::mutex> lk(Mtx);
	if( State == stPlaying )
	{
		State = stPaused;
		if( Audio ) Audio->Stop(); // 一時停止 (SamplesPlayed が凍結し復帰後に継続)
	}
	Cond.notify_all();
}
//---------------------------------------------------------------------------
void __stdcall tTVPLayerVideoBase::SetPosition( unsigned __int64 tick )
{
	std::lock_guard<std::mutex> lk(Mtx);
	SeekMs = (__int64)tick;
	DoSeek = true;
	Cond.notify_all();
}
//---------------------------------------------------------------------------
void __stdcall tTVPLayerVideoBase::GetPosition( unsigned __int64 *tick )
{
	if( tick ) *tick = (unsigned __int64)CurPtsMs.load();
}
//---------------------------------------------------------------------------
void __stdcall tTVPLayerVideoBase::GetStatus( tTVPVideoStatus *status )
{
	if( !status ) return;
	switch( State.load() )
	{
	case stPlaying: *status = vsPlaying; break;
	case stPaused:  *status = vsPaused;  break;
	case stEnded:   *status = vsEnded;   break;
	case stStopped:
	default:        *status = vsStopped; break;
	}
}
//---------------------------------------------------------------------------
void __stdcall tTVPLayerVideoBase::GetEvent( long *evcode, LONG_PTR *param1, LONG_PTR *param2, bool *got )
{
	*got = false;
	if( param2 ) *param2 = 0;
	// 直近フレームを EC_UPDATE、終端を EC_COMPLETE として疑似的に通知
	if( Updated.exchange(false) )
	{
		int frame = 0; GetFrame( &frame );
		*evcode = EC_UPDATE;
		if( param1 ) *param1 = frame;
		*got = true;
	}
	else if( Completed.exchange(false) )
	{
		*evcode = EC_COMPLETE;
		if( param1 ) *param1 = 0;
		*got = true;
	}
}
//---------------------------------------------------------------------------
void __stdcall tTVPLayerVideoBase::FreeEventParams( long evcode, LONG_PTR param1, LONG_PTR param2 ) {}
//---------------------------------------------------------------------------
void __stdcall tTVPLayerVideoBase::Rewind()
{
	std::lock_guard<std::mutex> lk(Mtx);
	SeekMs = 0;
	DoSeek = true;
	Cond.notify_all();
}
//---------------------------------------------------------------------------
void __stdcall tTVPLayerVideoBase::SetFrame( int f )
{
	__int64 ms = ( VideoFPS > 0.0 ) ? (__int64)( f * 1000.0 / VideoFPS ) : 0;
	SetPosition( (unsigned __int64)ms );
}
//---------------------------------------------------------------------------
void __stdcall tTVPLayerVideoBase::GetFrame( int *f )
{
	if( f ) *f = (int)( CurPtsMs.load() * VideoFPS / 1000.0 );
}
//---------------------------------------------------------------------------
void __stdcall tTVPLayerVideoBase::GetFPS( double *f ) { if( f ) *f = VideoFPS; }
//---------------------------------------------------------------------------
void __stdcall tTVPLayerVideoBase::GetNumberOfFrame( int *f )
{
	if( f ) *f = (int)( DurationMs * VideoFPS / 1000.0 );
}
//---------------------------------------------------------------------------
void __stdcall tTVPLayerVideoBase::GetTotalTime( __int64 *t ) { if( t ) *t = DurationMs; }
//---------------------------------------------------------------------------
void __stdcall tTVPLayerVideoBase::GetVideoSize( long *width, long *height )
{
	if( width )  *width  = VideoWidth;
	if( height ) *height = VideoHeight;
}
//---------------------------------------------------------------------------
void __stdcall tTVPLayerVideoBase::GetFrontBuffer( BYTE **buff )
{
	if( buff ) *buff = Buf[FrontIdx.load()];
}
//---------------------------------------------------------------------------
// preferI420: 直前 DecoderDecodeOverlay の I420 を I420Back へ packed copy する
// (デコードスレッドから呼ばれる)。plane データは次デコードで無効化されるため同期 copy。
//---------------------------------------------------------------------------
void tTVPLayerVideoBase::BufferI420FromDecoder()
{
	const BYTE *y=nullptr,*u=nullptr,*v=nullptr; int ys=0,us=0,vs=0,w=0,h=0;
	if( !DecoderGetI420Planes( &y, &ys, &u, &us, &v, &vs, &w, &h ) ) return;
	if( !y || !u || !v || w<=0 || h<=0 ) return;
	int cw = (w+1)/2, ch = (h+1)/2;
	std::lock_guard<std::mutex> lk(I420Mtx);
	I420Back.resize( (size_t)w*h + 2*(size_t)cw*ch );
	BYTE *d = I420Back.data();
	for( int r=0; r<h;  ++r ) memcpy( d + (size_t)r*w, y + (size_t)r*ys, w );
	BYTE *du = d + (size_t)w*h;
	for( int r=0; r<ch; ++r ) memcpy( du + (size_t)r*cw, u + (size_t)r*us, cw );
	BYTE *dv = du + (size_t)cw*ch;
	for( int r=0; r<ch; ++r ) memcpy( dv + (size_t)r*cw, v + (size_t)r*vs, cw );
	I420W = w; I420H = h; I420Valid = true; I420Dirty = true;
}
//---------------------------------------------------------------------------
// 描画スレッドから最新 I420 を取得。ロック下に Back→Front 複製し Front を返す
// (upload 中にデコードスレッドが Back を上書きしても安全)。
//---------------------------------------------------------------------------
bool __stdcall tTVPLayerVideoBase::GetI420Frame( const BYTE **y, int *yStride, const BYTE **u, int *uStride,
	const BYTE **v, int *vStride, int *w, int *h )
{
	std::lock_guard<std::mutex> lk(I420Mtx);
	if( !I420Valid || I420W<=0 || I420H<=0 ) return false;
	if( I420Dirty || I420Front.size()!=I420Back.size() ) { I420Front = I420Back; I420Dirty = false; }
	int vw = I420W, vh = I420H, cw = (vw+1)/2, ch = (vh+1)/2;
	const BYTE *base = I420Front.data();
	if(y)*y=base; if(yStride)*yStride=vw;
	if(u)*u=base+(size_t)vw*vh; if(uStride)*uStride=cw;
	if(v)*v=base+(size_t)vw*vh+(size_t)cw*ch; if(vStride)*vStride=cw;
	if(w)*w=vw; if(h)*h=vh;
	return true;
}
//---------------------------------------------------------------------------
// overlay モード: 子ウィンドウ present へ委譲。layer モードでは no-op。
//---------------------------------------------------------------------------
void __stdcall tTVPLayerVideoBase::SetWindow( HWND window )
{
	if( !OverlayMode ) return;
	std::lock_guard<std::mutex> lk(Mtx);
	if( window )
	{
		if( !Overlay )
		{
			Overlay = new tTVPD3D11OverlayWindow();
			if( !Overlay->Create( window ) ) { delete Overlay; Overlay = nullptr; }
		}
	}
	else
	{
		if( Overlay ) { delete Overlay; Overlay = nullptr; }
	}
}
//---------------------------------------------------------------------------
void __stdcall tTVPLayerVideoBase::SetMessageDrainWindow( HWND window )
{
	if( OverlayMode && Overlay ) Overlay->SetMessageDrainWindow( window );
}
//---------------------------------------------------------------------------
void __stdcall tTVPLayerVideoBase::SetRect( RECT *rect )
{
	if( OverlayMode && Overlay && rect ) Overlay->SetRect( *rect );
}
//---------------------------------------------------------------------------
void __stdcall tTVPLayerVideoBase::SetVisible( bool b )
{
	if( OverlayMode && Overlay ) Overlay->SetVisible( b );
}
//---------------------------------------------------------------------------
void __stdcall tTVPLayerVideoBase::SetVideoBuffer( BYTE *buff1, BYTE *buff2, long size )
{
	if( buff1 == nullptr )
		TVPThrowExceptionMessage( TJS_W("SetVideoBuffer Parameter Error") );
	// buff1/buff2 の 2 枚でダブルバッファ (エンジンが表を表示中に裏へ書いて
	// ティアリングを避ける)。buff2 が無ければ単一バッファにフォールバック。
	Buf[0] = buff1;
	Buf[1] = buff2 ? buff2 : buff1;
	FrontIdx = 0;
	BufferSize = size;
}
//---------------------------------------------------------------------------
void __stdcall tTVPLayerVideoBase::SetAudioVolume( long b )
{
	if( b < -10000 ) b = -10000;
	if( b > 0 ) b = 0;
	AudioVolMB = b; // 読み戻し用に保持 (でないと GetAudioVolume が常に最大を返す)
	float volume = std::pow( 10.0f, b / 2000.0f );
	DecoderSetVolume( volume );
}
//---------------------------------------------------------------------------
void __stdcall tTVPLayerVideoBase::GetAudioVolume( long *volume )
{
	if( volume ) *volume = AudioVolMB;
}
//---------------------------------------------------------------------------
