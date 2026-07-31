//---------------------------------------------------------------------------
/*
	Kirikiri Z
	See details of license at "LICENSE"
*/
//---------------------------------------------------------------------------
// Video Overlay support implementation
//---------------------------------------------------------------------------
#ifndef VideoOvlImplH
#define VideoOvlImplH
//---------------------------------------------------------------------------
#include "tjsNative.h"
#include "WindowIntf.h"
#include "VideoOvlIntf.h"
#include "voMode.h"
#include "tjsUtils.h"

#include "AudioStream.h"
#include "MoviePlayer.h"

//---------------------------------------------------------------------------
// tTJSNI_VideoOverlay : VideoOverlay Native Instance
//---------------------------------------------------------------------------
class iTVPVideoOverlay;
class tTJSNI_VideoOverlay : public tTJSNI_BaseVideoOverlay
{
	typedef tTJSNI_BaseVideoOverlay inherited;

	class iTVPMoviePlayer* mPlayer;

	bool Visible;

	class tTJSNI_BaseLayer	*Layer1;
	class tTJSNI_BaseLayer	*Layer2;
	tTVPVideoOverlayMode	Mode;	//!< Modeの動的な変更は出来ない。open前にセットしておくこと
	class tTVPBaseBitmap	*Bitmap[2];	//!< Layer描画用バッファ用Bitmap

	int currentSurface;
	tTJSCriticalSection surfaceLock;
	bool updateSurface;

	// overlay 動画 (非 layer) を DrawDevice へ pull 型で渡す presenter。現在の DrawDevice が
	// 対応 host を公開していれば登録して使い (SDL 等)、無ければ従来 push (UpdateVideo) へ
	// フォールバックする。実体は環境別 (sdl3/visual/SDLVideoPresenter.cpp 等)。
	class iTVPVideoOverlayPresenter* Presenter;
	bool PresenterRegistered;
	bool mUseYUV;               // YUV plane 経路で開いたか (presenter が YUV 対応 && backend が供給)

	// mixer 追加画像 (setMixingLayer の後継。presenter へ渡して動画上へ α 合成)
	tTVPRect mMixerRect;        // プライマリレイヤ座標での配置矩形
	tjs_real mMixerAlpha;       // 全体アルファ (0..1)
	tjs_uint mMixerBGColor;     // 保持のみ (背景色。現状 presenter 未使用)
	void PreparePresenter();    // Open 時: presenter を生成し device へ bind、YUV 対応を判定
	void TryRegisterPresenter();// Play 時: presenter の pull を開始 (Activate)
	void ReleasePresenter();

public:
	//! presenter を登録解除する (mixer 停止で Window から呼ばれ、ゲーム画面へ復帰させる)。
	//! object は保持するので replay で再登録できる。
	void UnregisterPresenter();

	tTJSNI_VideoOverlay();
	~tTJSNI_VideoOverlay();

	tjs_error TJS_INTF_METHOD Construct(tjs_int numparams, tTJSVariant **param, iTJSDispatch2 *tjs_obj);
	void TJS_INTF_METHOD Invalidate();

	void CheckUpdate();

	// 動画更新通知
	void Update();

	bool IsMixerPlaying() const;

public:
	void Open(const ttstr &name);
	void Close();
	void Shutdown();
	void Disconnect(); // tTJSNI_BaseVideoOverlay::Disconnect override

	void Play();
	void Stop();
	void Pause();
	void Rewind();
	void Prepare();

	void SetSegmentLoop( int comeFrame, int goFrame );
	void CancelSegmentLoop() {}
	void SetPeriodEvent( int eventFrame );

	void SetStopFrame( tjs_int f );
	void SetDefaultStopFrame();
	tjs_int GetStopFrame();

	void SetRectangleToVideoOverlay();

	void SetPosition(tjs_int left, tjs_int top);
	void SetSize(tjs_int width, tjs_int height);
	void SetBounds(const tTVPRect & rect);

	void SetLeft(tjs_int l);
	tjs_int GetLeft() const { return 0; }
	void SetTop(tjs_int t);
	tjs_int GetTop() const { return 0; }
	void SetWidth(tjs_int w);
	tjs_int GetWidth() const { return 0; }
	void SetHeight(tjs_int h);
	tjs_int GetHeight() const { return 0; }

	void SetVisible(bool b);
	bool GetVisible() const;

	void SetTimePosition( tjs_uint64 p );
	tjs_uint64 GetTimePosition();

	void SetFrame( tjs_int f );
	tjs_int GetFrame();

	tjs_real GetFPS();
	tjs_int GetNumberOfFrame();
	tjs_int64 GetTotalTime();

	void SetLoop( bool b );
	bool GetLoop() const { return false; }

	void SetLayer1( tTJSNI_BaseLayer *l );
	tTJSNI_BaseLayer *GetLayer1() { return Layer1; }
	void SetLayer2( tTJSNI_BaseLayer *l );
	tTJSNI_BaseLayer *GetLayer2() { return Layer2; }

	void SetMode( tTVPVideoOverlayMode m );
	tTVPVideoOverlayMode GetMode() { return Mode; }

	tjs_real GetPlayRate();
	void SetPlayRate(tjs_real r);

	tjs_int GetSegmentLoopStartFrame() { return 0; }
	tjs_int GetSegmentLoopEndFrame() { return 0; }
	tjs_int GetPeriodEventFrame() { return 0; }

	tjs_int GetAudioBalance();
	void SetAudioBalance(tjs_int b);
	tjs_int GetAudioVolume();
	void SetAudioVolume(tjs_int v);

	tjs_uint GetNumberOfAudioStream();
	void SelectAudioStream(tjs_uint n);
	tjs_int GetEnabledAudioStream();
	void DisableAudioStream();

	tjs_uint GetNumberOfVideoStream();
	void SelectVideoStream(tjs_uint n);
	tjs_int GetEnabledVideoStream();
	void SetMixingLayer( tTJSNI_BaseLayer *l );
	void ResetMixingBitmap();

	void SetMixingMovieAlpha( tjs_real a );
	tjs_real GetMixingMovieAlpha();
	void SetMixingMovieBGColor( tjs_uint col );
	tjs_uint GetMixingMovieBGColor();


	tjs_real GetContrastRangeMin();
	tjs_real GetContrastRangeMax();
	tjs_real GetContrastDefaultValue();
	tjs_real GetContrastStepSize();
	tjs_real GetContrast();
	void SetContrast( tjs_real v );

	tjs_real GetBrightnessRangeMin();
	tjs_real GetBrightnessRangeMax();
	tjs_real GetBrightnessDefaultValue();
	tjs_real GetBrightnessStepSize();
	tjs_real GetBrightness();
	void SetBrightness( tjs_real v );

	tjs_real GetHueRangeMin();
	tjs_real GetHueRangeMax();
	tjs_real GetHueDefaultValue();
	tjs_real GetHueStepSize();
	tjs_real GetHue();
	void SetHue( tjs_real v );

	tjs_real GetSaturationRangeMin();
	tjs_real GetSaturationRangeMax();
	tjs_real GetSaturationDefaultValue();
	tjs_real GetSaturationStepSize();
	tjs_real GetSaturation();
	void SetSaturation( tjs_real v );

	tjs_int GetOriginalWidth();
	tjs_int GetOriginalHeight();

	void ResetOverlayParams();
	void SetRectOffset(tjs_int ofsx, tjs_int ofsy);
	void DetachVideoOverlay();
};
//---------------------------------------------------------------------------

#endif
