//---------------------------------------------------------------------------
/*
	TVP2 ( T Visual Presenter 2 )  A script authoring tool
	Copyright (C) 2000 W.Dee <dee@kikyou.info> and contributors

	See details of license at "license.txt"
*/
//---------------------------------------------------------------------------
// krmovie.dll ( kirikiri movie playback support DLL ) interface
//---------------------------------------------------------------------------


#ifndef __KRMOVIE_H__
#define __KRMOVIE_H__

#define TVP_KRMOVIE_VER   0x0001000B

// krmovie を DLL としてビルドするときのみ dllexport (krmovie_EXPORTS は CMake が
// SHARED ターゲットに自動定義)。exe へ静的統合 (Track V-A) するときは通常の
// 関数として本体から直接リンクするので dllexport 不要。
#ifdef krmovie_EXPORTS
#define EXPORT(hr) extern "C" __declspec(dllexport) hr __stdcall
#else
#define EXPORT(hr) extern "C" hr __stdcall
#endif


//---------------------------------------------------------------------------
enum tTVPVideoStatus { vsStopped, vsPlaying, vsPaused, vsProcessing, vsEnded, vsReady };
//---------------------------------------------------------------------------

//---------------------------------------------------------------------------
// iTVPVideoOverlay
//---------------------------------------------------------------------------
class iTVPVideoOverlay // this is not a COM object
{
public:
	virtual void __stdcall AddRef() = 0;
	virtual void __stdcall Release() = 0;

	virtual void __stdcall SetWindow(HWND window) = 0;
	virtual void __stdcall SetMessageDrainWindow(HWND window) = 0;
	virtual void __stdcall SetRect(RECT *rect) = 0;
	virtual void __stdcall SetVisible(bool b) = 0;
	virtual void __stdcall Play() = 0;
	virtual void __stdcall Stop() = 0;
	virtual void __stdcall Pause() = 0;
	virtual void __stdcall SetPosition(unsigned __int64 tick) = 0;
	virtual void __stdcall GetPosition(unsigned __int64 *tick) = 0;
	virtual void __stdcall GetStatus(tTVPVideoStatus *status) = 0;
	virtual void __stdcall GetEvent(long *evcode, LONG_PTR *param1,
			LONG_PTR *param2, bool *got) = 0;

	virtual void __stdcall FreeEventParams(long evcode, LONG_PTR param1, LONG_PTR param2) = 0;

	virtual void __stdcall Rewind() = 0;
	virtual void __stdcall SetFrame( int f ) = 0;
	virtual void __stdcall GetFrame( int *f ) = 0;
	virtual void __stdcall GetFPS( double *f ) = 0;
	virtual void __stdcall GetNumberOfFrame( int *f ) = 0;
	virtual void __stdcall GetTotalTime( __int64 *t ) = 0;
	
	virtual void __stdcall GetVideoSize( long *width, long *height ) = 0;
	virtual void __stdcall GetFrontBuffer( BYTE **buff ) = 0;
	virtual void __stdcall SetVideoBuffer( BYTE *buff1, BYTE *buff2, long size ) = 0;

	// presenter が GPU で YUV→RGB する経路向けに、最新の I420(planar YUV420) フレームを
	// 直接取得する。I420 出力に対応した実装 (かつ I420 優先で開かれた場合) のみ true を返し
	// planes/strides/w/h を設定する。既定 false = 従来 BGRA(GetFrontBuffer) 経路。
	// plane データは実装が内部バッファに保持し、次フレーム更新まで有効。
	virtual bool __stdcall GetI420Frame( const BYTE** y, int* yStride, const BYTE** u, int* uStride,
		const BYTE** v, int* vStride, int* w, int* h ) { return false; }

	virtual void __stdcall SetStopFrame( int frame ) = 0;
	virtual void __stdcall GetStopFrame( int *frame ) = 0;
	virtual void __stdcall SetDefaultStopFrame() = 0;

	virtual void __stdcall SetPlayRate( double rate ) = 0;
	virtual void __stdcall GetPlayRate( double *rate ) = 0;

	virtual void __stdcall SetAudioBalance( long balance ) = 0;
	virtual void __stdcall GetAudioBalance( long *balance ) = 0;
	virtual void __stdcall SetAudioVolume( long volume ) = 0;
	virtual void __stdcall GetAudioVolume( long *volume ) = 0;

	virtual void __stdcall GetNumberOfAudioStream( unsigned long *streamCount ) = 0;
	virtual void __stdcall SelectAudioStream( unsigned long num ) = 0;
	virtual void __stdcall GetEnableAudioStreamNum( long *num ) = 0;
	virtual void __stdcall DisableAudioStream( void ) = 0;

	virtual void __stdcall GetNumberOfVideoStream( unsigned long *streamCount ) = 0;
	virtual void __stdcall SelectVideoStream( unsigned long num ) = 0;
	virtual void __stdcall GetEnableVideoStreamNum( long *num ) = 0;

	virtual void __stdcall SetMixingBitmap( HDC hdc, RECT *dest, float alpha ) = 0;
	virtual void __stdcall ResetMixingBitmap() = 0;

	virtual void __stdcall SetMixingMovieAlpha( float a ) = 0;
	virtual void __stdcall GetMixingMovieAlpha( float *a ) = 0;
	virtual void __stdcall SetMixingMovieBGColor( unsigned long col ) = 0;
	virtual void __stdcall GetMixingMovieBGColor( unsigned long *col ) = 0;

	virtual void __stdcall PresentVideoImage() = 0;

	virtual void __stdcall GetContrastRangeMin( float *v ) = 0;
	virtual void __stdcall GetContrastRangeMax( float *v ) = 0;
	virtual void __stdcall GetContrastDefaultValue( float *v ) = 0;
	virtual void __stdcall GetContrastStepSize( float *v ) = 0;
	virtual void __stdcall GetContrast( float *v ) = 0;
	virtual void __stdcall SetContrast( float v ) = 0;

	virtual void __stdcall GetBrightnessRangeMin( float *v ) = 0;
	virtual void __stdcall GetBrightnessRangeMax( float *v ) = 0;
	virtual void __stdcall GetBrightnessDefaultValue( float *v ) = 0;
	virtual void __stdcall GetBrightnessStepSize( float *v ) = 0;
	virtual void __stdcall GetBrightness( float *v ) = 0;
	virtual void __stdcall SetBrightness( float v ) = 0;

	virtual void __stdcall GetHueRangeMin( float *v ) = 0;
	virtual void __stdcall GetHueRangeMax( float *v ) = 0;
	virtual void __stdcall GetHueDefaultValue( float *v ) = 0;
	virtual void __stdcall GetHueStepSize( float *v ) = 0;
	virtual void __stdcall GetHue( float *v ) = 0;
	virtual void __stdcall SetHue( float v ) = 0;

	virtual void __stdcall GetSaturationRangeMin( float *v ) = 0;
	virtual void __stdcall GetSaturationRangeMax( float *v ) = 0;
	virtual void __stdcall GetSaturationDefaultValue( float *v ) = 0;
	virtual void __stdcall GetSaturationStepSize( float *v ) = 0;
	virtual void __stdcall GetSaturation( float *v ) = 0;
	virtual void __stdcall SetSaturation( float v ) = 0;
};
//---------------------------------------------------------------------------

//---------------------------------------------------------------------------
typedef void (__stdcall *tGetAPIVersion)(DWORD *version);
typedef void  (__stdcall *tGetVideoOverlayObject)(
	HWND callbackwin, IStream *stream, const wchar_t * streamname,
	const wchar_t *type, unsigned __int64 size, iTVPVideoOverlay **out);
//---------------------------------------------------------------------------

//---------------------------------------------------------------------------
#define WM_GRAPHNOTIFY  (WM_USER+15)
#define WM_CALLBACKCMD  (WM_USER+16)
#define EC_UPDATE		(0x8000/*EC_USER*/+1)
#define WM_STATE_CHANGE	(WM_USER+18)
//---------------------------------------------------------------------------


#endif


