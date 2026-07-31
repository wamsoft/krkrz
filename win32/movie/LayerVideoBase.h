/****************************************************************************/
/*! @file
@brief レイヤ再生用ビデオプレイヤの共通基底

Track V (DirectShow → Media Foundation 移行) の一環。
既存のモダンなレイヤプレイヤ tTVPWebpMovie (webplayer.cpp) の iTVPVideoOverlay
レイヤ契約を共通化した基底クラス。overlay 専用メソッドは全て no-op、レイヤモードの
バッファ/イベント/再生状態/デコードスレッド管理をここで実装し、実際のデコードは
純粋仮想フック (DecoderXxx) にてバックエンド (MF SourceReader / pl_mpeg 等) へ委譲する。

フレーム配送契約 (現行 BufferRenderer / tTVPWebpMovie 準拠):
- SetVideoBuffer(buff1,buff2,size) で受け取った buff1 を保持 (32bpp BGRA)。
- デコードした 1 フレームを buff1 へ *ボトムアップ* で書き込み (レイヤバッファは
  ボトムアップ格納)。書き込み後 mUpdate=true, PostMessage(OwnerWindow,WM_GRAPHNOTIFY)。
- GetEvent が mUpdate を EC_UPDATE として、終端を EC_COMPLETE として返す。
- GetFrontBuffer は buff1 を返す (ダブルバッファはエンジン側)。
*****************************************************************************/
#ifndef __LAYER_VIDEO_BASE_H__
#define __LAYER_VIDEO_BASE_H__

#include <windows.h>
#include "krmovie.h"
#include <thread>
#include <mutex>
#include <condition_variable>
#include <atomic>
#include <vector>

class tTVPLayerVideoBase : public iTVPVideoOverlay
{
public:
	//! overlayOutput=true で overlay 出力 (子ウィンドウ D3D11 present)、false で layer 出力。
	//! preferI420=true で presenter 経路向けに I420 プレーンを内部保持し GetI420Frame で供給する
	//! (engine の D3D11 presenter が GPU で YUV→RGB。overlayOutput とは排他、overlayOutput 優先)。
	//! バックエンドが DecoderGetI420Planes を実装している場合のみ有効。
	tTVPLayerVideoBase( HWND owner, bool overlayOutput = false, bool preferI420 = false );
	virtual ~tTVPLayerVideoBase();

	//-- 開く (サブクラスの DecoderOpen を呼び、寸法/FPS 等を取得する)
	bool Open( IStream *stream, const tjs_char *streamname, const tjs_char *type, unsigned __int64 size );

protected:
	//======================================================================
	// デコーダバックエンドが実装する純粋仮想フック
	//======================================================================
	//! ストリームを開く。成功で true。
	virtual bool DecoderOpen( IStream *stream, const tjs_char *type, unsigned __int64 size ) = 0;
	//! 閉じる。
	virtual void DecoderClose() = 0;
	//! 映像情報を返す (幅/高さ/FPS/長さ[ms])。
	virtual bool DecoderGetInfo( long &width, long &height, double &fps, __int64 &durationMs ) = 0;
	//! 次フレームを dst へ書き込む。dst は書き込み先の先頭行ポインタ、pitch は行バイト数
	//! (ボトムアップなので負値)。outPtsMs に提示時刻(ms)、eos で終端を返す。
	//! フレームを書けたら true、終端/失敗で false。
	virtual bool DecoderReadFrame( BYTE *dst, long pitch, __int64 &outPtsMs, bool &eos ) = 0;
	//! ms 位置へシーク。
	virtual bool DecoderSeek( __int64 ms ) = 0;
	//! 音量 (0.0-1.0)。既定 no-op (音声ありのバックエンドは Audio へ委譲)。
	virtual void DecoderSetVolume( float v );

	//! このバックエンドが音声を持つか (DecoderOpen で CreateAudioSink 済みなら true)。
	virtual bool DecoderHasAudio() { return false; }
	//! 音声デコードを進めてシンク (Audio) へ供給する。デコードスレッドから呼ばれる。
	//! 既定 no-op。音声ありのバックエンドが override して Audio->Submit() する。
	virtual void DecoderPumpAudio() {}

	//! overlay モード用 (2 段): 次フレームをデコードして内部に保持する。pts(ms)/eos を
	//! 返す。フレームを得たら true。overlay 対応バックエンド (pl_mpeg 等) が override。
	virtual bool DecoderDecodeOverlay( __int64 &pts, bool &eos ) { return false; }
	//! overlay モード用: 直前に DecoderDecodeOverlay で保持したフレームを ov へ present。
	//! (sync 待ちの後に呼ばれる)。
	virtual void DecoderPresentOverlay( class tTVPD3D11OverlayWindow *ov ) {}
	//! preferI420 モード用: 直前に DecoderDecodeOverlay で保持した I420 プレーンを返す。
	//! (y/u/v は次デコードまで有効。基底が同期 copy する)。対応バックエンド (pl_mpeg) が override。
	virtual bool DecoderGetI420Planes( const BYTE **y, int *yStride, const BYTE **u, int *uStride,
		const BYTE **v, int *vStride, int *w, int *h ) { return false; }

	//! バックエンドが DecoderOpen 内で呼び、音声シンクを生成する。成功で true。
	bool CreateAudioSink( int channels, int sampleRate, int bitsPerSample, bool isFloat );
	//! 音声シンク (音声が無ければ nullptr)。バックエンドの PumpAudio が Submit する。
	class tTVPMovieAudioSink *Audio;

	//-- サブクラスから使う情報
	long   VideoWidth;
	long   VideoHeight;
	double VideoFPS;
	__int64 DurationMs;

	//! デコードスレッドを停止して join する (冪等)。派生デストラクタの先頭で呼び、
	//! その後 DecoderClose() すること (基底デストラクタからは純粋仮想を呼べないため)。
	void StopThread();

private:
	//-- スレッド
	void ThreadMain();
	std::thread DecodeThread;
	std::mutex  Mtx;
	std::condition_variable Cond;
	enum tState { stStopped, stPlaying, stPaused, stEnded };
	std::atomic<tState> State;
	std::atomic<bool> Terminate;
	std::atomic<bool> DoSeek;
	std::atomic<__int64> SeekMs;

	//-- バッファ / イベント
	HWND   OwnerWindow;
	BYTE  *Buf[2];              //!< SetVideoBuffer で受けた 2 枚 (ダブルバッファ。
	                           //!  裏に書き込み、書き終えたら表に切替=表示中バッファへの書込を避けティアリング防止)
	std::atomic<int> FrontIdx; //!< 現在の表バッファ index (エンジンが表示中)
	long   BufferSize;
	std::atomic<bool> Updated;    //!< 新フレーム有り (GetEvent が EC_UPDATE で消費)
	std::atomic<bool> Completed;  //!< 終端 (GetEvent が EC_COMPLETE で消費)
	std::atomic<__int64> CurPtsMs; //!< 直近提示フレームの pts

	//-- A/V 同期 (音声をマスタクロックにする)。再生開始/シープ時に基準を取り直す。
	bool    ClockValid;       //!< クロック基準が有効か
	int64_t AudioEpochMs;     //!< 基準時点の Audio->GetPlayedMs() (累積)
	int64_t PtsEpochMs;       //!< 基準時点に対応する pts(ms)
	//! 音声マスタの現在時刻(ms)。音声が無ければ -1。
	int64_t AudioClockMs();

	long   RefCount;
	bool   Loop; // (エンジン側がループ制御するため通常未使用)
	long   AudioVolMB; // 直近設定の音量 (DS 減衰 mB, -10000..0)。GetAudioVolume の読み戻し用

	//-- overlay 出力 (子ウィンドウ D3D11 present)。layer モードでは未使用。
	bool   OverlayMode;
	class tTVPD3D11OverlayWindow *Overlay;

	//-- preferI420 出力 (presenter 経路)。I420 プレーンを内部 packed に front/back 二重保持し
	//   GetI420Frame で描画スレッドへ供給 (engine D3D11 presenter が GPU で YUV→RGB)。
	bool   PreferI420;
	std::mutex I420Mtx;
	std::vector<uint8_t> I420Back, I420Front;
	int    I420W, I420H;
	bool   I420Valid, I420Dirty;
	//! 直前 DecoderDecodeOverlay の I420 を I420Back へ packed copy し更新通知する。
	void   BufferI420FromDecoder();

public:
	//======================================================================
	// iTVPVideoOverlay 実装
	//======================================================================
	virtual void __stdcall AddRef();
	virtual void __stdcall Release();

	virtual void __stdcall Play();
	virtual void __stdcall Stop();
	virtual void __stdcall Pause();
	virtual void __stdcall SetPosition( unsigned __int64 tick );
	virtual void __stdcall GetPosition( unsigned __int64 *tick );
	virtual void __stdcall GetStatus( tTVPVideoStatus *status );
	virtual void __stdcall GetEvent( long *evcode, LONG_PTR *param1, LONG_PTR *param2, bool *got );
	virtual void __stdcall FreeEventParams( long evcode, LONG_PTR param1, LONG_PTR param2 );

	virtual void __stdcall Rewind();
	virtual void __stdcall SetFrame( int f );
	virtual void __stdcall GetFrame( int *f );
	virtual void __stdcall GetFPS( double *f );
	virtual void __stdcall GetNumberOfFrame( int *f );
	virtual void __stdcall GetTotalTime( __int64 *t );

	virtual void __stdcall GetVideoSize( long *width, long *height );
	virtual void __stdcall GetFrontBuffer( BYTE **buff );
	virtual void __stdcall SetVideoBuffer( BYTE *buff1, BYTE *buff2, long size );
	virtual bool __stdcall GetI420Frame( const BYTE **y, int *yStride, const BYTE **u, int *uStride,
		const BYTE **v, int *vStride, int *w, int *h );

	virtual void __stdcall SetAudioVolume( long volume );
	virtual void __stdcall GetAudioVolume( long *volume );

	//-- overlay モード時は子ウィンドウ present へ委譲 / layer モードでは no-op ----
	virtual void __stdcall SetWindow( HWND window );
	virtual void __stdcall SetMessageDrainWindow( HWND window );
	virtual void __stdcall SetRect( RECT *rect );
	virtual void __stdcall SetVisible( bool b );
	virtual void __stdcall SetStopFrame( int frame ) {}
	virtual void __stdcall GetStopFrame( int *frame ) { if(frame) *frame = 0; }
	virtual void __stdcall SetDefaultStopFrame() {}
	virtual void __stdcall SetPlayRate( double rate ) {}
	virtual void __stdcall GetPlayRate( double *rate ) { if(rate) *rate = 1.0; }
	virtual void __stdcall SetAudioBalance( long balance ) {}
	virtual void __stdcall GetAudioBalance( long *balance ) { if(balance) *balance = 0; }
	virtual void __stdcall GetNumberOfAudioStream( unsigned long *c ) { if(c) *c = 0; }
	virtual void __stdcall SelectAudioStream( unsigned long num ) {}
	virtual void __stdcall GetEnableAudioStreamNum( long *num ) { if(num) *num = -1; }
	virtual void __stdcall DisableAudioStream( void ) {}
	virtual void __stdcall GetNumberOfVideoStream( unsigned long *c ) { if(c) *c = 1; }
	virtual void __stdcall SelectVideoStream( unsigned long num ) {}
	virtual void __stdcall GetEnableVideoStreamNum( long *num ) { if(num) *num = 0; }
	virtual void __stdcall SetMixingBitmap( HDC hdc, RECT *dest, float alpha ) {}
	virtual void __stdcall ResetMixingBitmap() {}
	virtual void __stdcall SetMixingMovieAlpha( float a ) {}
	virtual void __stdcall GetMixingMovieAlpha( float *a ) { if(a) *a = 0; }
	virtual void __stdcall SetMixingMovieBGColor( unsigned long col ) {}
	virtual void __stdcall GetMixingMovieBGColor( unsigned long *col ) { if(col) *col = 0; }
	virtual void __stdcall PresentVideoImage() {}
	virtual void __stdcall GetContrastRangeMin( float *v ) { if(v) *v = 0; }
	virtual void __stdcall GetContrastRangeMax( float *v ) { if(v) *v = 0; }
	virtual void __stdcall GetContrastDefaultValue( float *v ) { if(v) *v = 0; }
	virtual void __stdcall GetContrastStepSize( float *v ) { if(v) *v = 0; }
	virtual void __stdcall GetContrast( float *v ) { if(v) *v = 0; }
	virtual void __stdcall SetContrast( float v ) {}
	virtual void __stdcall GetBrightnessRangeMin( float *v ) { if(v) *v = 0; }
	virtual void __stdcall GetBrightnessRangeMax( float *v ) { if(v) *v = 0; }
	virtual void __stdcall GetBrightnessDefaultValue( float *v ) { if(v) *v = 0; }
	virtual void __stdcall GetBrightnessStepSize( float *v ) { if(v) *v = 0; }
	virtual void __stdcall GetBrightness( float *v ) { if(v) *v = 0; }
	virtual void __stdcall SetBrightness( float v ) {}
	virtual void __stdcall GetHueRangeMin( float *v ) { if(v) *v = 0; }
	virtual void __stdcall GetHueRangeMax( float *v ) { if(v) *v = 0; }
	virtual void __stdcall GetHueDefaultValue( float *v ) { if(v) *v = 0; }
	virtual void __stdcall GetHueStepSize( float *v ) { if(v) *v = 0; }
	virtual void __stdcall GetHue( float *v ) { if(v) *v = 0; }
	virtual void __stdcall SetHue( float v ) {}
	virtual void __stdcall GetSaturationRangeMin( float *v ) { if(v) *v = 0; }
	virtual void __stdcall GetSaturationRangeMax( float *v ) { if(v) *v = 0; }
	virtual void __stdcall GetSaturationDefaultValue( float *v ) { if(v) *v = 0; }
	virtual void __stdcall GetSaturationStepSize( float *v ) { if(v) *v = 0; }
	virtual void __stdcall GetSaturation( float *v ) { if(v) *v = 0; }
	virtual void __stdcall SetSaturation( float v ) {}
};

#endif // __LAYER_VIDEO_BASE_H__
