#pragma once

#include "krmovie.h"

// external/movie-player library から
#include "IMoviePlayer.h"
#include "WebpXAudio2Sink.h"

class tTVPD3D11OverlayWindow;

class tTVPWebpMovie : public iTVPVideoOverlay
{
public:
    //! overlayOutput=true で overlay 出力 (子ウィンドウ D3D11 YUV present)。
    tTVPWebpMovie(HWND owner, bool overlayOutput = false);
    virtual ~tTVPWebpMovie();

	bool Open(const char *path);
	bool Open(IStream *stream);

	void Update(int width, int height, IMoviePlayer::DestUpdater updater);
	void UpdatePlanes(const IMoviePlayer::VideoFrameInfo &frame); // overlay: YUV plane 経路
	void OnState(IMoviePlayer::State state);

	virtual void __stdcall AddRef();
	virtual void __stdcall Release();

	virtual void __stdcall SetWindow(HWND window);
	virtual void __stdcall SetMessageDrainWindow(HWND window);
	virtual void __stdcall SetRect(RECT *rect);
	virtual void __stdcall SetVisible(bool b);
	virtual void __stdcall Play();
	virtual void __stdcall Stop();
	virtual void __stdcall Pause();
	virtual void __stdcall SetPosition(unsigned __int64 tick);
	virtual void __stdcall GetPosition(unsigned __int64 *tick);
	virtual void __stdcall GetStatus(tTVPVideoStatus *status);
	virtual void __stdcall GetEvent(long *evcode, LONG_PTR *param1,
			LONG_PTR *param2, bool *got);

	virtual void __stdcall FreeEventParams(long evcode, LONG_PTR param1, LONG_PTR param2);

	virtual void __stdcall Rewind();
	virtual void __stdcall SetFrame( int f );
	virtual void __stdcall GetFrame( int *f );
	virtual void __stdcall GetFPS( double *f );
	virtual void __stdcall GetNumberOfFrame( int *f );
	virtual void __stdcall GetTotalTime( __int64 *t );
	
	virtual void __stdcall GetVideoSize( long *width, long *height );
	virtual void __stdcall GetFrontBuffer( BYTE **buff );
	virtual void __stdcall SetVideoBuffer( BYTE *buff1, BYTE *buff2, long size );

	virtual void __stdcall SetStopFrame( int frame );
	virtual void __stdcall GetStopFrame( int *frame );
	virtual void __stdcall SetDefaultStopFrame();

	virtual void __stdcall SetPlayRate( double rate );
	virtual void __stdcall GetPlayRate( double *rate );

	virtual void __stdcall SetAudioBalance( long balance );
	virtual void __stdcall GetAudioBalance( long *balance );
	virtual void __stdcall SetAudioVolume( long volume );
	virtual void __stdcall GetAudioVolume( long *volume );

	virtual void __stdcall GetNumberOfAudioStream( unsigned long *streamCount );
	virtual void __stdcall SelectAudioStream( unsigned long num );
	virtual void __stdcall GetEnableAudioStreamNum( long *num );
	virtual void __stdcall DisableAudioStream( void );

	virtual void __stdcall GetNumberOfVideoStream( unsigned long *streamCount );
	virtual void __stdcall SelectVideoStream( unsigned long num );
	virtual void __stdcall GetEnableVideoStreamNum( long *num );

	virtual void __stdcall SetMixingBitmap( HDC hdc, RECT *dest, float alpha );
	virtual void __stdcall ResetMixingBitmap();

	virtual void __stdcall SetMixingMovieAlpha( float a );
	virtual void __stdcall GetMixingMovieAlpha( float *a );
	virtual void __stdcall SetMixingMovieBGColor( unsigned long col );
	virtual void __stdcall GetMixingMovieBGColor( unsigned long *col );

	virtual void __stdcall PresentVideoImage();

	virtual void __stdcall GetContrastRangeMin( float *v );
	virtual void __stdcall GetContrastRangeMax( float *v );
	virtual void __stdcall GetContrastDefaultValue( float *v );
	virtual void __stdcall GetContrastStepSize( float *v );
	virtual void __stdcall GetContrast( float *v );
	virtual void __stdcall SetContrast( float v );

	virtual void __stdcall GetBrightnessRangeMin( float *v );
	virtual void __stdcall GetBrightnessRangeMax( float *v );
	virtual void __stdcall GetBrightnessDefaultValue( float *v );
	virtual void __stdcall GetBrightnessStepSize( float *v );
	virtual void __stdcall GetBrightness( float *v );
	virtual void __stdcall SetBrightness( float v );

	virtual void __stdcall GetHueRangeMin( float *v );
	virtual void __stdcall GetHueRangeMax( float *v );
	virtual void __stdcall GetHueDefaultValue( float *v );
	virtual void __stdcall GetHueStepSize( float *v );
	virtual void __stdcall GetHue( float *v );
	virtual void __stdcall SetHue( float v );

	virtual void __stdcall GetSaturationRangeMin( float *v );
	virtual void __stdcall GetSaturationRangeMax( float *v );
	virtual void __stdcall GetSaturationDefaultValue( float *v );
	virtual void __stdcall GetSaturationStepSize( float *v );
	virtual void __stdcall GetSaturation( float *v );
	virtual void __stdcall SetSaturation( float v );

protected:
	ULONG		RefCount;
	// AudioSink は Player より先に宣言 = 後に破棄。Player の decoder thread が
	// 停止 (Player のデストラクタ内) してから sink がクリーンアップされる。
	WebpXAudio2Sink AudioSink;
	IMoviePlayer *Player;
	HWND OwnerWindow;
	BYTE *mBuffer;
	bool mUpdate;

	bool OverlayMode;                 //!< true=overlay 出力
	tTVPD3D11OverlayWindow *Overlay;  //!< overlay 時の D3D11 present 先
	int DispW, DispH;                 //!< 表示サイズ (coded 幅 padding を除いたクロップ寸法。0=未取得)
};
