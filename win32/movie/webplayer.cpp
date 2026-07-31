#include <windows.h>
#include "tp_stub.h"
#include "webplayer.h"
#include "IMoviePlayer.h"
#include "D3D11OverlayWindow.h"

#ifndef EC_COMPLETE
#define EC_COMPLETE 0x0001
#endif

// IStreamから変換するためのクラス
class MovieStream : public IMovieReadStream {
public:
	MovieStream(IStream *stream) : mStream(stream){}

    virtual int AddRef(void) { 
		if (mStream) {
			return mStream->AddRef();
		}
		return 0;
	}

    virtual int Release(void) { 
		int ret = mStream->Release();
		if (ret == 0) {
			mStream = nullptr;
			delete this;
		}
		return ret;
	}

    virtual size_t Read(void *buf, size_t size) {
		if (mStream) {
			ULONG readed;
			if (SUCCEEDED(mStream->Read(buf, size, &readed))) {
				return readed;
			}
		}
		return 0;
	};

    virtual int64_t Tell() const {
		if (mStream) {
			LARGE_INTEGER offset;
			offset.QuadPart = 0;
			ULARGE_INTEGER pos;
			mStream->Seek(offset, SEEK_CUR, &pos);
			return pos.QuadPart;
		}
		return 0;
	}
    virtual void Seek(int64_t offset, int origin) {
		if (mStream) {
			LARGE_INTEGER _offset;
			_offset.QuadPart = offset;
			mStream->Seek(_offset, origin, nullptr);
		}
	}

    virtual size_t Size() const {
	    if (mStream) {
			STATSTG stat;
			mStream->Stat(&stat, STATFLAG_NONAME);
			return (size_t)(stat.cbSize.QuadPart);
		}
		return 0;
	}
private:
	IStream *mStream;
};


tTVPWebpMovie::tTVPWebpMovie(HWND owner, bool overlayOutput, bool preferI420)
: RefCount(1)
, Player(nullptr)
, OwnerWindow(owner)
, mBuffer(nullptr)
, mUpdate(false)
, OverlayMode(overlayOutput)
, Overlay(nullptr)
, DispW(0), DispH(0)
, PreferI420(preferI420 && !overlayOutput)   // overlayOutput が優先
, I420W(0), I420H(0), I420Valid(false), I420Dirty(false)
{
}

tTVPWebpMovie::~tTVPWebpMovie()
{
	mBuffer = nullptr;
	if (Player) {
		delete Player;   // decoder thread 停止 (以後 callback は来ない)
		Player = nullptr;
	}
	if (Overlay) {       // Player 停止後に破棄
		delete Overlay;
		Overlay = nullptr;
	}
}

bool
tTVPWebpMovie::Open(const char *path)
{
    IMoviePlayer::InitParam param;
    param.Init();
    param.videoColorFormat = (OverlayMode || PreferI420) ? IMoviePlayer::ColorFormat::COLOR_NOCONV
                                                         : IMoviePlayer::ColorFormat::COLOR_BGRA;
	param.audioSink        = &AudioSink;
	Player = IMoviePlayer::CreateMoviePlayer(path, param);

	if (Player) {
		if (OverlayMode) {
			Player->SetOnVideoDecodedPlanes([this](const IMoviePlayer::VideoFrameInfo &frame) {
				UpdatePlanes(frame);
			});
		} else if (PreferI420) {
			// presenter 経路: I420 を内部保持し GetI420Frame で供給 (engine の D3D11 presenter が GPU で YUV→RGB)。
			Player->SetOnVideoDecodedPlanes([this](const IMoviePlayer::VideoFrameInfo &frame) {
				BufferI420(frame);
			});
		} else {
			Player->SetOnVideoDecoded([this](int w, int h, IMoviePlayer::DestUpdater updater) {
				Update(w, h, updater);
			});
		}
		Player->SetOnState([](void *userData, IMoviePlayer::State state){
			tTVPWebpMovie *self = (tTVPWebpMovie *)userData;
			self->OnState(state);
			return 0;
		}, this);
	}

	return Player != nullptr;
}


bool
tTVPWebpMovie::Open(IStream *stream)
{
    IMoviePlayer::InitParam param;
    param.Init();
    param.videoColorFormat = (OverlayMode || PreferI420) ? IMoviePlayer::ColorFormat::COLOR_NOCONV
                                                         : IMoviePlayer::ColorFormat::COLOR_BGRA;
	param.audioSink        = &AudioSink;
	Player = IMoviePlayer::CreateMoviePlayer(new MovieStream(stream), param);

	if (Player) {
		if (OverlayMode) {
			Player->SetOnVideoDecodedPlanes([this](const IMoviePlayer::VideoFrameInfo &frame) {
				UpdatePlanes(frame);
			});
		} else if (PreferI420) {
			// presenter 経路: I420 を内部保持し GetI420Frame で供給 (engine の D3D11 presenter が GPU で YUV→RGB)。
			Player->SetOnVideoDecodedPlanes([this](const IMoviePlayer::VideoFrameInfo &frame) {
				BufferI420(frame);
			});
		} else {
			Player->SetOnVideoDecoded([this](int w, int h, IMoviePlayer::DestUpdater updater) {
				Update(w, h, updater);
			});
		}
		Player->SetOnState([](void *userData, IMoviePlayer::State state){
			tTVPWebpMovie *self = (tTVPWebpMovie *)userData;
			self->OnState(state);
			return 0;
		}, this);
	}

	return Player != nullptr;
}

void 
tTVPWebpMovie::Update(int w, int h, IMoviePlayer::DestUpdater updater)
{
	if (mBuffer) {
		// わたされた先のBitmapは最後の行
		uint8_t *d = mBuffer + (w*4) * (h-1);
		int dpitch = -w * 4;
		updater((char *)d, dpitch);
		mUpdate = true;
		if( OwnerWindow != NULL) {
			::PostMessage( OwnerWindow, WM_GRAPHNOTIFY, 0, 0 );
		}
	}
}
//----------------------------------------------------------------------------
//! @brief overlay モード: YUV(I420) plane を D3D11 子ウィンドウへ present する。
//----------------------------------------------------------------------------
void tTVPWebpMovie::UpdatePlanes(const IMoviePlayer::VideoFrameInfo &frame)
{
	if (!Overlay) return;
	// VP8/9 は I420 (planeCount=3)。alpha 付き webm でも overlay では色のみ (不透明表示)。
	if (frame.planeCount >= 3) {
		// frame.width は coded 幅 (16 アライン padding 付き) なので、表示幅にクロップする。
		// (crop しないと右端に未定義 chroma 由来の緑帯が出る。GetVideoFormat は
		//  extractor 由来の表示寸法を返す。plane stride はそのままで width だけ縮める)
		if (DispW == 0 && Player) {
			IMoviePlayer::VideoFormat fmt{};
			Player->GetVideoFormat(&fmt);
			DispW = (fmt.width  > 0 && fmt.width  <= frame.width ) ? fmt.width  : frame.width;
			DispH = (fmt.height > 0 && fmt.height <= frame.height) ? fmt.height : frame.height;
		}
		int vw = (DispW > 0 && DispW <= frame.width ) ? DispW : frame.width;
		int vh = (DispH > 0 && DispH <= frame.height) ? DispH : frame.height;
		Overlay->PresentI420(
			frame.planes[0].data, frame.planes[0].stride,
			frame.planes[1].data, frame.planes[1].stride,
			frame.planes[2].data, frame.planes[2].stride,
			vw, vh );
		mUpdate = true;
		if (OwnerWindow) ::PostMessage(OwnerWindow, WM_GRAPHNOTIFY, 0, 0);
		// デバッグ: KRMOVIE_OVERLAY_DUMP があれば ~100 フレーム目を BMP 保存 (冒頭の
		// 黒フェードを避けるため少し進めてから。自己検証用)
		static int frameCount = 0;
		static bool dumped = false;
		if (!dumped && ++frameCount >= 100) {
			const char *dp = getenv("KRMOVIE_OVERLAY_DUMP");
			if (dp && *dp) {
				wchar_t wp[1024]; wp[0]=0;
				MultiByteToWideChar(CP_ACP, 0, dp, -1, wp, 1024);
				Overlay->DebugSaveLastFrame(wp);
				dumped = true;
			}
		}
	}
}

//----------------------------------------------------------------------------
//! @brief presenter 経路: I420(planar YUV420) を内部バッファへ packed 保持する。
//!   デコードスレッドから呼ばれる。plane データは呼び出し後に無効化されるので同期 copy する。
//!   engine の D3D11 presenter が GetI420Frame で取り出し GPU で YUV→RGB する。
//----------------------------------------------------------------------------
void tTVPWebpMovie::BufferI420(const IMoviePlayer::VideoFrameInfo &frame)
{
	if (frame.planeCount < 3) return;
	// coded 幅 (16 アライン padding 付き) を表示寸法へクロップ (右端の緑帯回避。UpdatePlanes 同様)。
	if (DispW == 0 && Player) {
		IMoviePlayer::VideoFormat fmt{};
		Player->GetVideoFormat(&fmt);
		DispW = (fmt.width  > 0 && fmt.width  <= frame.width ) ? fmt.width  : frame.width;
		DispH = (fmt.height > 0 && fmt.height <= frame.height) ? fmt.height : frame.height;
	}
	int vw = (DispW > 0 && DispW <= frame.width ) ? DispW : frame.width;
	int vh = (DispH > 0 && DispH <= frame.height) ? DispH : frame.height;
	if (vw <= 0 || vh <= 0) return;
	int cw = (vw + 1) / 2, ch = (vh + 1) / 2;

	std::lock_guard<std::mutex> lk(I420Mtx);
	I420Back.resize((size_t)vw * vh + 2 * (size_t)cw * ch);
	uint8_t *dst = I420Back.data();
	// Y
	for (int y = 0; y < vh; ++y)
		memcpy(dst + (size_t)y * vw, frame.planes[0].data + (size_t)y * frame.planes[0].stride, vw);
	// U
	uint8_t *du = dst + (size_t)vw * vh;
	for (int y = 0; y < ch; ++y)
		memcpy(du + (size_t)y * cw, frame.planes[1].data + (size_t)y * frame.planes[1].stride, cw);
	// V
	uint8_t *dv = du + (size_t)cw * ch;
	for (int y = 0; y < ch; ++y)
		memcpy(dv + (size_t)y * cw, frame.planes[2].data + (size_t)y * frame.planes[2].stride, cw);
	I420W = vw; I420H = vh;
	I420Valid = true; I420Dirty = true;

	mUpdate = true;
	if (OwnerWindow) ::PostMessage(OwnerWindow, WM_GRAPHNOTIFY, 0, 0);
}
//----------------------------------------------------------------------------
//! @brief 描画スレッドから最新 I420 フレームを取得する。ロック下に Back→Front へ複製し、
//!   Front のポインタを返す (以後 Front はこのスレッド専有 = presenter の upload 中に
//!   デコードスレッドが Back を上書きしても安全)。
//----------------------------------------------------------------------------
bool __stdcall tTVPWebpMovie::GetI420Frame( const BYTE** y, int* yStride, const BYTE** u, int* uStride,
	const BYTE** v, int* vStride, int* w, int* h )
{
	std::lock_guard<std::mutex> lk(I420Mtx);
	if (!I420Valid || I420W <= 0 || I420H <= 0) return false;
	if (I420Dirty || I420Front.size() != I420Back.size()) {
		I420Front = I420Back;   // ロック下に複製
		I420Dirty = false;
	}
	int vw = I420W, vh = I420H, cw = (vw + 1) / 2, ch = (vh + 1) / 2;
	const uint8_t *base = I420Front.data();
	if (y) *y = base;
	if (yStride) *yStride = vw;
	if (u) *u = base + (size_t)vw * vh;
	if (uStride) *uStride = cw;
	if (v) *v = base + (size_t)vw * vh + (size_t)cw * ch;
	if (vStride) *vStride = cw;
	if (w) *w = vw;
	if (h) *h = vh;
	return true;
}
//----------------------------------------------------------------------------
void
tTVPWebpMovie::OnState(IMoviePlayer::State state)
{
	switch (state) {
    case IMoviePlayer::STATE_PLAY:
		if( OwnerWindow != NULL) {
			::PostMessage( OwnerWindow, WM_STATE_CHANGE, vsPlaying, 0 );
		}
		break;
    case IMoviePlayer::STATE_PAUSE:
		if( OwnerWindow != NULL) {
			::PostMessage( OwnerWindow, WM_STATE_CHANGE, vsPaused, 0 );
		}
		break;
	case IMoviePlayer::STATE_STOP:
		if( OwnerWindow != NULL) {
			::PostMessage( OwnerWindow, WM_STATE_CHANGE, vsStopped, 0 );
		}
		break;
	case IMoviePlayer::STATE_FINISH:
		if( OwnerWindow != NULL) {
			::PostMessage( OwnerWindow, WM_STATE_CHANGE, vsEnded, 0 );
		}
		break;
	}
}

//----------------------------------------------------------------------------
//! @brief	  	参照カウンタのインクリメント
//----------------------------------------------------------------------------
void __stdcall tTVPWebpMovie::AddRef()
{
	RefCount++;
}
//----------------------------------------------------------------------------
//! @brief	  	参照カウンタのデクリメント。1ならdelete。
//----------------------------------------------------------------------------
void __stdcall tTVPWebpMovie::Release()
{
	if(RefCount == 1)
		delete this;
	else
		RefCount--;
}
//----------------------------------------------------------------------------
//! @brief	  	ビデオを再生する
//----------------------------------------------------------------------------
void __stdcall tTVPWebpMovie::Play()
{
	if (!Player) return;
	if (Player->GetState() == IMoviePlayer::STATE_PAUSE) {
		Player->Resume();
	} else {
		Player->Seek(0);
 		Player->Play();
	}
}
//----------------------------------------------------------------------------
//! @brief	  	ビデオを停止する
//----------------------------------------------------------------------------
void __stdcall tTVPWebpMovie::Stop()
{
	if (!Player) return;
	Player->Stop();
}
//----------------------------------------------------------------------------
//! @brief	  	ビデオを一時停止する
//----------------------------------------------------------------------------
void __stdcall tTVPWebpMovie::Pause()
{
	if (!Player) return;
	Player->Pause();
}
//----------------------------------------------------------------------------
//! @brief	  	現在のムービー時間を設定する
//! @param 		tick : 設定する現在の時間
//----------------------------------------------------------------------------
void __stdcall tTVPWebpMovie::SetPosition( unsigned __int64 tick )
{
	if (!Player) return;
	Player->Seek(tick*1000.0);
}
//----------------------------------------------------------------------------
//! @brief	  	現在のムービー時間を取得する
//! @param 		tick : 現在の時間を返す変数
//----------------------------------------------------------------------------
void __stdcall tTVPWebpMovie::GetPosition( unsigned __int64 *tick )
{
	if (!Player) return;
	if (tick) {
		*tick = Player->Position()/1000.0;
	}
}
//----------------------------------------------------------------------------
//! @brief	  	現在のムービーの状態を取得する
//! @param 		status : 現在の状態を返す変数
//----------------------------------------------------------------------------
void __stdcall tTVPWebpMovie::GetStatus(tTVPVideoStatus *status)
{
	if (!Player) return;
	switch (Player->GetState()) {
	case IMoviePlayer::STATE_PLAY:
	    *status = vsPlaying;
		break;
	case IMoviePlayer::STATE_PAUSE:
	    *status = vsPaused;
		break;
	case IMoviePlayer::STATE_STOP:
	    *status = vsStopped; 
		break;
	case IMoviePlayer::STATE_FINISH:
	    *status = vsEnded;
		break;
	default:
    	*status = vsProcessing;
		break;
	}
}
//----------------------------------------------------------------------------
//! @brief	  	A sample has been delivered. Copy it to the texture.
//! @param 		evcode : イベントコード
//! @param 		param1 : パラメータ1。内容はイベントコードにより異なる。
//! @param 		param2 : パラメータ2。内容はイベントコードにより異なる。
//! @param 		got : 取得の正否
//----------------------------------------------------------------------------
void __stdcall tTVPWebpMovie::GetEvent( long *evcode, LONG_PTR *param1, LONG_PTR *param2, bool *got )
{
	if (!Player) return;
	// 更新イベントを疑似処理で対応する
	if (mUpdate) {
		*evcode = EC_UPDATE;
		tjs_int	frame = 0;
		GetFrame(&frame);
		*param1 = frame;
		*got = true;
		mUpdate = false;
	} else {
		*got = false;
	}
}
//----------------------------------------------------------------------------
//! @brief	  	イベントを解放する
//! 
//! GetEventでイベントを得て、処理した後、このメソッドによってイベントを解放すること
//! @param 		evcode : 解放するイベントコード
//! @param 		param1 : 解放するパラメータ1。内容はイベントコードにより異なる。
//! @param 		param2 : 解放するパラメータ2。内容はイベントコードにより異なる。
//----------------------------------------------------------------------------
void __stdcall tTVPWebpMovie::FreeEventParams(long evcode, LONG_PTR param1, LONG_PTR param2)
{
	if (!Player) return;
	//Event()->FreeEventParams(evcode, param1, param2);
	return;
}
//----------------------------------------------------------------------------
//! @brief	  	ムービーを最初の位置まで巻き戻す
//! @note		IMediaPositionは非推奨のようだが、サンプルでは使用されていたので、
//! 			同じままにしておく。
//----------------------------------------------------------------------------
void __stdcall tTVPWebpMovie::Rewind()
{
	if (!Player) return;
	//Player->Seek(0);
}
//----------------------------------------------------------------------------
//! @brief	  	指定されたフレームへ移動する
//! 
//! このメソッドによって設定された位置は、指定したフレームと完全に一致するわけではない。
//! フレームは、指定したフレームに最も近いキーフレームの位置に設定される。
//! @param		f : 移動するフレーム
//----------------------------------------------------------------------------
void __stdcall tTVPWebpMovie::SetFrame( int f )
{
	if (!Player) return;
	IMoviePlayer::VideoFormat format;
	Player->GetVideoFormat(&format);
	int64_t pos = f * 1000000.0 / format.frameRate;
	Player->Seek(pos);
}
//----------------------------------------------------------------------------
//! @brief	  	現在のフレームを取得する
//! @param		f : 現在のフレームを入れる変数へのポインタ
//----------------------------------------------------------------------------
void __stdcall tTVPWebpMovie::GetFrame( int *f )
{
	if (!Player || !f) return;
	IMoviePlayer::VideoFormat format;
	Player->GetVideoFormat(&format);
	*f = Player->Position() / 1000000.0 * format.frameRate;
}

//----------------------------------------------------------------------------
//! @brief	  	指定されたフレームで再生を停止させる
//! @param		f : 再生を停止させるフレーム
//----------------------------------------------------------------------------
void __stdcall tTVPWebpMovie::SetStopFrame( int f )
{
	if (!Player) return;
	// XXX
}
//----------------------------------------------------------------------------
//! @brief	  	現在の再生が停止するフレームを取得する
//! @param		f : 現在の再生が停止するフレームを入れる変数へのポインタ
//----------------------------------------------------------------------------
void __stdcall tTVPWebpMovie::GetStopFrame( int *f )
{
	if (!Player) return;
	// XXX
}
//----------------------------------------------------------------------------
//! @brief	  	再生を停止するフレームを初期状態に戻す。
//----------------------------------------------------------------------------
void __stdcall tTVPWebpMovie::SetDefaultStopFrame()
{
	if (!Player) return;
	// XXX
}
//----------------------------------------------------------------------------
//! @brief	  	FPSを取得する
//! @param		f : FPSを入れる変数へのポインタ
//----------------------------------------------------------------------------
void __stdcall tTVPWebpMovie::GetFPS( double *f )
{
	if (!Player || !f) return;
	IMoviePlayer::VideoFormat format;
	Player->GetVideoFormat(&format);
	*f = (double)format.frameRate;
}
//----------------------------------------------------------------------------
//! @brief	  	全フレーム数を取得する
//! @param		f : 全フレーム数を入れる変数へのポインタ
//----------------------------------------------------------------------------
void __stdcall tTVPWebpMovie::GetNumberOfFrame( int *f )
{
	if (!Player || !f) return;
	IMoviePlayer::VideoFormat format;
	Player->GetVideoFormat(&format);
	*f = Player->Duration() / 1000000.0 * format.frameRate;
}
//----------------------------------------------------------------------------
//! @brief	  	ムービーの長さ(msec)を取得する
//! @param		f : ムービーの長さを入れる変数へのポインタ
//----------------------------------------------------------------------------
void __stdcall tTVPWebpMovie::GetTotalTime( __int64 *t )
{
	if (!Player) return;
	*t = Player->Duration() / 1000.0;
}
//----------------------------------------------------------------------------
//! @brief	  	ビデオの画像サイズを取得する
//! @param		width : 幅
//! @param		height : 高さ
//----------------------------------------------------------------------------
void __stdcall tTVPWebpMovie::GetVideoSize( long *width, long *height )
{
	if (!Player) return;
	IMoviePlayer::VideoFormat format;
	Player->GetVideoFormat(&format);
	if( width != NULL ) {
		*width = format.width;
    }
	if( height != NULL ) {
		*height = format.height;
    }
}
//----------------------------------------------------------------------------
//! @brief	  	描画中のバッファを返す（低層がダブるバッファしてるので固定）
//----------------------------------------------------------------------------
void __stdcall tTVPWebpMovie::GetFrontBuffer( BYTE **buff )
{
	*buff = mBuffer;
	return;
}
//----------------------------------------------------------------------------
//! @brief	  	描画用ビデオバッファの設定
//----------------------------------------------------------------------------
void __stdcall tTVPWebpMovie::SetVideoBuffer( BYTE *buff1, BYTE *buff2, long size )
{
	if( buff1 == NULL)
		TVPThrowExceptionMessage(TJS_W("SetVideoBuffer Parameter Error"));

	mBuffer = buff1;
	// buff2は使わない
}

//----------------------------------------------------------------------------
//! @brief	  	何もしない。
//----------------------------------------------------------------------------
void __stdcall tTVPWebpMovie::SetWindow( HWND window )
{
	if (!OverlayMode) return;
	if (window) {
		if (!Overlay) {
			Overlay = new tTVPD3D11OverlayWindow();
			if (!Overlay->Create(window)) { delete Overlay; Overlay = nullptr; }
		}
	} else {
		if (Overlay) { delete Overlay; Overlay = nullptr; }
	}
}
//----------------------------------------------------------------------------
void __stdcall tTVPWebpMovie::SetMessageDrainWindow( HWND window )
{
	if (OverlayMode && Overlay) Overlay->SetMessageDrainWindow(window);
}
//----------------------------------------------------------------------------
void __stdcall tTVPWebpMovie::SetRect( RECT *rect )
{
	if (OverlayMode && Overlay && rect) Overlay->SetRect(*rect);
}
//----------------------------------------------------------------------------
void __stdcall tTVPWebpMovie::SetVisible( bool b )
{
	if (OverlayMode && Overlay) Overlay->SetVisible(b);
}
//----------------------------------------------------------------------------
//! @brief	  	再生速度を設定する
//! @param	rate : 再生レート。1.0が等速。
//----------------------------------------------------------------------------
void __stdcall tTVPWebpMovie::SetPlayRate( double rate )
{
	if (!Player) return;

	if( rate > 0.0 )
	{
	}
}
//----------------------------------------------------------------------------
//! @brief	  	再生速度を取得する
//! @param	*rate : 再生レート。1.0が等速。
//----------------------------------------------------------------------------
void __stdcall tTVPWebpMovie::GetPlayRate( double *rate )
{
	if (!Player) { if (rate) *rate = 1.0; return; }

	if( rate != NULL )
	{
	}
}
//----------------------------------------------------------------------------
//! @brief	  	オーディオバランスを設定する
//! @param	balance : バランスを指定する。値は -10,000 ～ 10,000 の範囲で指定できる。
//! 値が -10,000 の場合、右チャンネルは 100 dB 減衰され、無音となることを意味している。
//! 値が 10,000 の場合、左チャンネルが無音であることを意味している。
//! 真中の値は 0 で、これは両方のチャンネルがフル ボリュームであることを意味している。
//! 一方のチャンネルが減衰されても、もう一方のチャンネルはフル ボリュームのままである。 
//----------------------------------------------------------------------------
void __stdcall tTVPWebpMovie::SetAudioBalance( long balance )
{
	if (!Player) return;
}
//----------------------------------------------------------------------------
//! @brief	  	オーディオバランスを取得する
//! @param	*balance : バランスの範囲は -10,000 ～ 10,000までである。
//! 値が -10,000 の場合、右チャンネルは 100 dB 減衰され、無音となることを意味している。
//! 値が 10,000 の場合、左チャンネルが無音であることを意味している。
//! 真中の値は 0 で、これは両方のチャンネルがフル ボリュームであることを意味している。
//! 一方のチャンネルが減衰されても、もう一方のチャンネルはフル ボリュームのままである。 
//----------------------------------------------------------------------------
void __stdcall tTVPWebpMovie::GetAudioBalance( long *balance )
{
	if (!Player) { if (balance) *balance = 0; return; }
}
//----------------------------------------------------------------------------
//! @brief	  	オーディオボリュームを設定する
//! @param volume : ボリュームを -10,000 ～ 0 の数値で指定する。
//! 最大ボリュームは 0、無音は -10,000。
//! 必要なデシベル値を 100 倍する。たとえば、-10,000 = -100 dB。 
//----------------------------------------------------------------------------
void __stdcall tTVPWebpMovie::SetAudioVolume( long b )
{
	if (!Player) return;
	if( b < -10000 ) b = -10000;
	if( b > 0 ) b = 0;
	float volume = pow(10.0, b/2000.0f);
	Player->SetVolume ( volume );
}
//----------------------------------------------------------------------------
//! @brief	  	オーディオボリュームを設定する
//! @param volume : ボリュームを -10,000 ～ 0 の数値で指定する。
//! 最大ボリュームは 0、無音は -10,000。
//! 必要なデシベル値を 100 倍する。たとえば、-10,000 = -100 dB。 
//----------------------------------------------------------------------------
void __stdcall tTVPWebpMovie::GetAudioVolume( long *b )
{
	// ★修正: 旧コードは `if(!Player){*b=0;} return;` で return が無条件だったため、
	// Player 再生中は *b が未設定 (garbage) で以降が dead code だった。
	if( !b ) return;
	if( !Player ) { *b = 0; return; }
	float volume = Player->Volume();
	if( volume <= 0.0f ) {
		*b = -10000;
	} else {
		*b = (tjs_int)( 20 * log10f(volume) * 100 );
	}
}
//----------------------------------------------------------------------------
//! @brief	  	オーディオストリーム数を取得する
//! @param streamCount : オーディオストリーム数を入れる変数へのポインタ
//----------------------------------------------------------------------------
void __stdcall tTVPWebpMovie::GetNumberOfAudioStream( unsigned long *streamCount )
{
	if (!Player) return;

	if( streamCount != NULL )
		*streamCount = Player->IsAudioAvailable() ? 1: 0;
}
//----------------------------------------------------------------------------
//! @brief	  	指定したオーディオストリーム番号のストリームを有効にする
//! @param num : 有効にするオーディオストリーム番号
//----------------------------------------------------------------------------
void __stdcall tTVPWebpMovie::SelectAudioStream( unsigned long num )
{
	if (!Player) return;

}
//----------------------------------------------------------------------------
// @brief		有効なオーディオストリーム番号を得る
// 一番初めに見つかった有効なストリーム番号を返す。
// グループ内のすべてのストリームが有効である可能性もあるが、tTVPWebpMovie::SelectAudioStreamを使用した場合、グループ内で1つだけか有効になる。
//----------------------------------------------------------------------------
void __stdcall tTVPWebpMovie::GetEnableAudioStreamNum( long *num )
{
	if (!Player) return;

}
//----------------------------------------------------------------------------
//! @brief	  	ビデオストリーム数を取得する
//! @param streamCount : ビデオストリーム数を入れる変数へのポインタ
//----------------------------------------------------------------------------
void __stdcall tTVPWebpMovie::GetNumberOfVideoStream( unsigned long *streamCount )
{
	if (!Player) return;

	if( streamCount != NULL )
		*streamCount = Player->IsVideoAvailable() ? 1: 0;
}
//----------------------------------------------------------------------------
//! @brief	  	指定したビデオストリーム番号のストリームを有効にする
//! @param num : 有効にするビデオストリーム番号
//----------------------------------------------------------------------------
void __stdcall tTVPWebpMovie::SelectVideoStream( unsigned long num )
{
	if (!Player) return;

}
//----------------------------------------------------------------------------
// @brief		有効なビデオストリーム番号を得る
// 一番初めに見つかった有効なストリーム番号を返す。
// グループ内のすべてのストリームが有効である可能性もあるが、tTVPWebpMovie::SelectAudioStreamを使用した場合、グループ内で1つだけか有効になる。
//----------------------------------------------------------------------------
void __stdcall tTVPWebpMovie::GetEnableVideoStreamNum( long *num )
{
	if (!Player) return;

}

//----------------------------------------------------------------------------
// @brief		オーディオストリームを無効にする
// MPEG Iの時、この操作は出来ない
//----------------------------------------------------------------------------
void __stdcall tTVPWebpMovie::DisableAudioStream( void )
{
	if (!Player) return;
}
//----------------------------------------------------------------------------
//! @brief	  	何もしない。
//----------------------------------------------------------------------------
void __stdcall tTVPWebpMovie::SetMixingBitmap( HDC hdc, RECT *dest, float alpha )
{
}
//----------------------------------------------------------------------------
//! @brief	  	何もしない。
//----------------------------------------------------------------------------
void __stdcall tTVPWebpMovie::ResetMixingBitmap()
{
}
//----------------------------------------------------------------------------
//! @brief	  	何もしない。
//----------------------------------------------------------------------------
void __stdcall tTVPWebpMovie::SetMixingMovieAlpha( float a )
{
}
//----------------------------------------------------------------------------
//! @brief	  	何もしない。
//----------------------------------------------------------------------------
void __stdcall tTVPWebpMovie::GetMixingMovieAlpha( float *a )
{
}
//----------------------------------------------------------------------------
//! @brief	  	何もしない。
//----------------------------------------------------------------------------
void __stdcall tTVPWebpMovie::SetMixingMovieBGColor( unsigned long col )
{
}
//----------------------------------------------------------------------------
//! @brief	  	何もしない。
//----------------------------------------------------------------------------
void __stdcall tTVPWebpMovie::GetMixingMovieBGColor( unsigned long *col )
{
}
//----------------------------------------------------------------------------
//! @brief	  	何もしない。
//----------------------------------------------------------------------------
void __stdcall tTVPWebpMovie::PresentVideoImage()
{
}
//----------------------------------------------------------------------------
//! @brief	  	何もしない。
//----------------------------------------------------------------------------
void __stdcall tTVPWebpMovie::GetContrastRangeMin( float *v )
{
}
//----------------------------------------------------------------------------
//! @brief	  	何もしない。
//----------------------------------------------------------------------------
void __stdcall tTVPWebpMovie::GetContrastRangeMax( float *v )
{
}
//----------------------------------------------------------------------------
//! @brief	  	何もしない。
//----------------------------------------------------------------------------
void __stdcall tTVPWebpMovie::GetContrastDefaultValue( float *v )
{
}
//----------------------------------------------------------------------------
//! @brief	  	何もしない。
//----------------------------------------------------------------------------
void __stdcall tTVPWebpMovie::GetContrastStepSize( float *v )
{
}
//----------------------------------------------------------------------------
//! @brief	  	何もしない。
//----------------------------------------------------------------------------
void __stdcall tTVPWebpMovie::GetContrast( float *v )
{
}
//----------------------------------------------------------------------------
//! @brief	  	何もしない。
//----------------------------------------------------------------------------
void __stdcall tTVPWebpMovie::SetContrast( float v )
{
}
//----------------------------------------------------------------------------
//! @brief	  	何もしない。
//----------------------------------------------------------------------------
void __stdcall tTVPWebpMovie::GetBrightnessRangeMin( float *v )
{
}
//----------------------------------------------------------------------------
//! @brief	  	何もしない。
//----------------------------------------------------------------------------
void __stdcall tTVPWebpMovie::GetBrightnessRangeMax( float *v )
{
}
//----------------------------------------------------------------------------
//! @brief	  	何もしない。
//----------------------------------------------------------------------------
void __stdcall tTVPWebpMovie::GetBrightnessDefaultValue( float *v )
{
}
//----------------------------------------------------------------------------
//! @brief	  	何もしない。
//----------------------------------------------------------------------------
void __stdcall tTVPWebpMovie::GetBrightnessStepSize( float *v )
{
}
//----------------------------------------------------------------------------
//! @brief	  	何もしない。
//----------------------------------------------------------------------------
void __stdcall tTVPWebpMovie::GetBrightness( float *v )
{
}
//----------------------------------------------------------------------------
//! @brief	  	何もしない。
//----------------------------------------------------------------------------
void __stdcall tTVPWebpMovie::SetBrightness( float v )
{
}
//----------------------------------------------------------------------------
//! @brief	  	何もしない。
//----------------------------------------------------------------------------
void __stdcall tTVPWebpMovie::GetHueRangeMin( float *v )
{
}
//----------------------------------------------------------------------------
//! @brief	  	何もしない。
//----------------------------------------------------------------------------
void __stdcall tTVPWebpMovie::GetHueRangeMax( float *v )
{
}
//----------------------------------------------------------------------------
//! @brief	  	何もしない。
//----------------------------------------------------------------------------
void __stdcall tTVPWebpMovie::GetHueDefaultValue( float *v )
{
}
//----------------------------------------------------------------------------
//! @brief	  	何もしない。
//----------------------------------------------------------------------------
void __stdcall tTVPWebpMovie::GetHueStepSize( float *v )
{
}
//----------------------------------------------------------------------------
//! @brief	  	何もしない。
//----------------------------------------------------------------------------
void __stdcall tTVPWebpMovie::GetHue( float *v )
{
}
//----------------------------------------------------------------------------
//! @brief	  	何もしない。
//----------------------------------------------------------------------------
void __stdcall tTVPWebpMovie::SetHue( float v )
{
}
//----------------------------------------------------------------------------
//! @brief	  	何もしない。
//----------------------------------------------------------------------------
void __stdcall tTVPWebpMovie::GetSaturationRangeMin( float *v )
{
}
//----------------------------------------------------------------------------
//! @brief	  	何もしない。
//----------------------------------------------------------------------------
void __stdcall tTVPWebpMovie::GetSaturationRangeMax( float *v )
{
}
//----------------------------------------------------------------------------
//! @brief	  	何もしない。
//----------------------------------------------------------------------------
void __stdcall tTVPWebpMovie::GetSaturationDefaultValue( float *v )
{
}
//----------------------------------------------------------------------------
//! @brief	  	何もしない。
//----------------------------------------------------------------------------
void __stdcall tTVPWebpMovie::GetSaturationStepSize( float *v )
{
}
//----------------------------------------------------------------------------
//! @brief	  	何もしない。
//----------------------------------------------------------------------------
void __stdcall tTVPWebpMovie::GetSaturation( float *v )
{
}
//----------------------------------------------------------------------------
//! @brief	  	何もしない。
//----------------------------------------------------------------------------
void __stdcall tTVPWebpMovie::SetSaturation( float v )
{
}
