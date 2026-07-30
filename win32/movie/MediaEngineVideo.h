/****************************************************************************/
/*! @file
@brief HW 動画プレイヤ (IMFMediaEngine フレームサーバ) — Track V-E

MF-native 形式 (mp4/H.264/HEVC/wmv/asf/…) を IMFMediaEngine で HW デコードし、
エンジン (BasicDrawDevice) の D3D11 デバイスへ TransferVideoFrame で直接転送して
present する。CPU デコーダ (MFSourceReaderVideo/pl_mpeg/movie-player) と違い、デコードも
YUV→RGB もスケールも GPU で行う (HEVC/AV1/HDR の素地)。

- iTVPVideoOverlay: Play/Stop/Pause/position/GetVideoSize 等の制御を MediaEngine へマップ。
  音声・A/V 同期は MediaEngine が内部で担う (CPU 経路の MovieAudioSink は使わない)。
- iTVPVideoPresenter: DrawDevice の Show() から描画スレッドで RenderVideoFrame が呼ばれ、
  OnVideoStreamTick で新フレーム時に TransferVideoFrame → 全画面 quad で present。

MediaEngine の DXGI マネージャはエンジンの D3D11 デバイス (VIDEO_SUPPORT + multithread
保護済み) に束ねる。よって TransferVideoFrame は engine デバイス上のテクスチャへ直接
書け、クロスデバイス共有は不要。webm(VP8/9)/mpg(MPEG-1) は MF デコーダが無いので対象外
(CPU 経路のまま)。
*****************************************************************************/
#ifndef __MEDIA_ENGINE_VIDEO_H__
#define __MEDIA_ENGINE_VIDEO_H__

#include <windows.h>
#include <d3d11.h>
#include <mfmediaengine.h>
#include <atomic>
#include <mutex>
#include "krmovie.h"          // iTVPVideoOverlay / tTVPVideoStatus
#include "VideoPresenter.h"   // iTVPVideoPresenter / tTVPVideoPresenterContext

class tTVPVideoPresenterD3D;

class tTVPMediaEngineVideo : public iTVPVideoOverlay, public iTVPVideoPresenter
{
public:
	//! owner=イベント通知先ウィンドウ、engineDevice=BasicDrawDevice の D3D11 デバイス。
	tTVPMediaEngineVideo( HWND owner, ID3D11Device* engineDevice );
	virtual ~tTVPMediaEngineVideo();

	//! ストリームを開く (非同期ロード。準備完了は LOADEDMETADATA イベントで Ready=true)。
	bool Open( IStream* stream, const tjs_char* type, unsigned __int64 size );

	//! tTJSNI_VideoOverlay が host へ登録する presenter。
	iTVPVideoPresenter* GetPresenter() { return static_cast<iTVPVideoPresenter*>(this); }

	//! MediaEngine のイベント (Notify から呼ばれる。MF スレッド)。
	void OnMediaEngineEvent( DWORD meEvent, DWORD_PTR param1, DWORD param2 );

	//======================================================================
	// iTVPVideoPresenter
	//======================================================================
	virtual bool TJS_INTF_METHOD RenderVideoFrame( const tTVPVideoPresenterContext & ctx );

	//======================================================================
	// iTVPVideoOverlay
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

	virtual void __stdcall SetAudioVolume( long volume );
	virtual void __stdcall GetAudioVolume( long *volume );

	//-- overlay 専用 / 未使用は no-op ----------------------------------------
	virtual void __stdcall SetWindow( HWND window ) {}
	virtual void __stdcall SetMessageDrainWindow( HWND window ) {}
	virtual void __stdcall SetRect( RECT *rect ) {}
	virtual void __stdcall SetVisible( bool b ) { Visible = b; }
	virtual void __stdcall SetStopFrame( int frame ) {}
	virtual void __stdcall GetStopFrame( int *frame ) { if(frame) *frame = 0; }
	virtual void __stdcall SetDefaultStopFrame() {}
	virtual void __stdcall SetPlayRate( double rate );
	virtual void __stdcall GetPlayRate( double *rate );
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
	virtual void __stdcall SetMixingMovieAlpha( float a ) { MovieAlpha = a; }
	virtual void __stdcall GetMixingMovieAlpha( float *a ) { if(a) *a = MovieAlpha; }
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

private:
	bool EnsureDestTexture( ID3D11Device* dev, int w, int h );
	void ReleaseDestTexture();
	void Shutdown();

	long   RefCount;
	HWND   OwnerWindow;
	ID3D11Device* EngineDevice;    //!< 非所有 (BasicDrawDevice の device)

	bool   MFStarted;
	IMFMediaEngine*        MediaEngine;
	IMFMediaEngineNotify*  Notify;        //!< イベントコールバック (COM)
	IMFDXGIDeviceManager*  DXGIManager;
	UINT                   DXGIResetToken;
	IStream*               HeldStream;    //!< byte stream が参照する間保持
	struct IMFByteStream*  ByteStream;

	std::atomic<bool> Ready;       //!< LOADEDMETADATA 済み
	std::atomic<bool> Ended;       //!< 終端 (GetEvent が EC_COMPLETE で消費)
	std::atomic<long> VideoWidth;
	std::atomic<long> VideoHeight;
	bool   Visible;
	float  MovieAlpha;
	bool   Playing;

	//-- present 用 (engine デバイス上)
	ID3D11Texture2D*          DestTex;   //!< TransferVideoFrame の宛先 (RT+SRV)
	ID3D11ShaderResourceView* DestSrv;
	int    DestW, DestH;
	bool   HasFrame;
	tTVPVideoPresenterD3D*    Blit;
};

#endif // __MEDIA_ENGINE_VIDEO_H__
