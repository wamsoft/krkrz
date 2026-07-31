//---------------------------------------------------------------------------
/*
	Kirikiri Z
	See details of license at "LICENSE"
*/
//---------------------------------------------------------------------------
// Video Overlay support implementation
//---------------------------------------------------------------------------


#include "tjsCommHead.h"
#include "CharacterSet.h"

#include <algorithm>
#include "VideoOvlImpl.h"
#include "DrawDevice.h"
#include "VideoOverlayPresenter.h"
#include "Application.h"
#include "StorageIntf.h"
#include "LayerIntf.h"
#include "LayerBitmapIntf.h"
#include "MsgImpl.h"
#include "LogIntf.h"
#ifdef KRKRZ_MOVIE_STREAM
#include "IMoviePlayer.h"

//---------------------------------------------------------------------------
// iTJSBinaryStream → IMovieReadStream アダプタ
//---------------------------------------------------------------------------
class TJSMovieReadStream : public IMovieReadStream {
	iTJSBinaryStream *mStream;
	int mRefCount;
public:
	TJSMovieReadStream(iTJSBinaryStream *s) : mStream(s), mRefCount(1) {}
	~TJSMovieReadStream() { if (mStream) mStream->Destruct(); }
	int AddRef() override { return ++mRefCount; }
	int Release() override {
		int r = --mRefCount;
		if (r <= 0) delete this;
		return r;
	}
	size_t Read(void *buf, size_t size) override {
		return mStream->Read(buf, (tjs_uint)size);
	}
	int64_t Tell() const override {
		return (int64_t)const_cast<iTJSBinaryStream*>(mStream)->GetPosition();
	}
	void Seek(int64_t offset, int origin) override {
		mStream->Seek(offset, origin);
	}
	size_t Size() const override {
		return (size_t)const_cast<iTJSBinaryStream*>(mStream)->GetSize();
	}
};
#endif // KRKRZ_MOVIE_STREAM

//---------------------------------------------------------------------------
// tTJSNI_VideoOverlay
//---------------------------------------------------------------------------
tTJSNI_VideoOverlay::tTJSNI_VideoOverlay() 
: mPlayer(nullptr)
, Layer1(nullptr)
, Layer2(nullptr)
, currentSurface(0)
, updateSurface(false)
, Presenter(nullptr)
, PresenterRegistered(false)
, mUseYUV(false)
, mMixerAlpha(1.0)
, mMixerBGColor(0)
{
	Mode = vomOverlay;
	Visible = false;
	mMixerRect.left = mMixerRect.top = mMixerRect.right = mMixerRect.bottom = 0;

	Bitmap[0] = nullptr;
	Bitmap[1] = nullptr;
}

tTJSNI_VideoOverlay::~tTJSNI_VideoOverlay()
{
	Close();
}

//---------------------------------------------------------------------------
tjs_error TJS_INTF_METHOD tTJSNI_VideoOverlay::Construct(tjs_int numparams, tTJSVariant **param, iTJSDispatch2 *tjs_obj)
{
	tjs_error hr = inherited::Construct(numparams, param, tjs_obj);
	if(TJS_FAILED(hr)) return hr;
	return TJS_S_OK;
}
//---------------------------------------------------------------------------
void TJS_INTF_METHOD tTJSNI_VideoOverlay::Invalidate()
{
	Close();
	inherited::Invalidate();
}

bool tTJSNI_VideoOverlay::IsMixerPlaying() const
{
	// overlay presenter モード (vomLayer 以外) で再生中か。WINVER の isOverlay=(Mode!=vomLayer)
	// と揃える (vomOverlay/vomMixer/vomMFEVR は SDL では同じ presenter 経路)。
	return Mode != vomLayer && mPlayer && mPlayer->IsPlaying();
}

void
tTJSNI_VideoOverlay::CheckUpdate()
{
	if (Status == tTVPVideoOverlayStatus::Play || Status == tTVPVideoOverlayStatus::Pause) {
		SetStatusAsync( mPlayer->IsPlaying() ? tTVPVideoOverlayStatus::Play : tTVPVideoOverlayStatus::Stop );
	}
}

void
tTJSNI_VideoOverlay::Update()
{
	// デコーダスレッドを持たない実装 (wasm <video>) のフレーム引き取り。
	// 通常実装では no-op
	if (mPlayer) mPlayer->Pump();

	if (Mode == vomLayer && updateSurface) {

		tTJSCriticalSectionHolder cs(surfaceLock);
		
		tTVPBaseBitmap *bmp = Bitmap[1-currentSurface];
		if (!bmp) return;

		int width = bmp->GetWidth();
		int height = bmp->GetHeight();

		tTJSNI_BaseLayer	*l1 = Layer1;
		tTJSNI_BaseLayer	*l2 = Layer2;

		if( l1 != NULL )
		{
			if( (long)l1->GetImageWidth() != width || (long)l1->GetImageHeight() != height )
				l1->SetImageSize( width, height );
			if( (long)l1->GetWidth() != width || (long)l1->GetHeight() != height )
				l1->SetSize( width, height );
			l1->AssignMainImage( bmp );
			l1->Update();
		}
		if( l2 != NULL )
		{
			if( (long)l2->GetImageWidth() != width || (long)l2->GetImageHeight() != height )
				l2->SetImageSize( width, height );
			if( (long)l2->GetWidth() != width || (long)l2->GetHeight() != height )
				l2->SetSize( width, height );
			l2->AssignMainImage( bmp );
			l2->Update();
		}
		updateSurface = false;
		// XXX フレーム番号がとれるのが理想
		FireFrameUpdateEvent(0);
	}
}

//---------------------------------------------------------------------------
void tTJSNI_VideoOverlay::Open(const ttstr &name)
{
	Close();

	ttstr path;
	ttstr newpath = TVPGetPlacedPath(name);
	if( newpath.IsEmpty() ) {
		path = TVPNormalizeStorageName(name);
	} else {
		path = newpath;
	}

	// presenter を先に用意 (Open 時に bind)。YUV 対応 presenter (SDL 等) なら movie player を
	// YUV plane 出力で開き、GPU 側で YUV→RGB する (CPU libyuv 変換を削減)。
	PreparePresenter();
	bool preferYUV = mUseYUV;

#ifdef KRKRZ_MOVIE_STREAM
	// 吉里吉里のストレージ層からストリームを取得（XP3アーカイブ対応）
	iTJSBinaryStream *tjsStream = TVPCreateStream(path);
	if (!tjsStream) {
		SetStatus(tTVPVideoOverlayStatus::LoadError);
		return;
	}
	IMovieReadStream *movieStream = new TJSMovieReadStream(tjsStream);
	std::string utf8name;
	TVPUtf16ToUtf8(utf8name, path.c_str());
	mPlayer = TVPCreateMoviePlayer(movieStream, utf8name.c_str(), preferYUV);
	if (!mPlayer) {
		movieStream->Release();
	}
#elif defined(__EMSCRIPTEN__)
	// wasm: 正規化ストレージ名のまま渡す (web:// 等は localname を持たない。
	// URL 解決/ストレージ読みは WebMoviePlayer 側で行う)
	mPlayer = TVPCreateMoviePlayer(path.c_str(), preferYUV);
#else
	// ファイルパス直接指定で開く
	TVPGetLocalName(path);
	mPlayer = TVPCreateMoviePlayer(path.c_str(), preferYUV);
#endif
	// 実際に YUV plane を供給できるか (backend 依存) で最終判定。
	mUseYUV = mUseYUV && mPlayer && mPlayer->SupportsPlanes();
	if (mPlayer && mUseYUV) {
		// YUV plane 経路: presenter へ plane を渡し、GPU で YUV→RGB。
		mPlayer->SetOnVideoDecodedPlanes([this](const iTVPMoviePlayer::VideoPlaneFrame &frame) {
			if (Presenter) Presenter->UpdateFrameYUV(frame);
			SetStatusAsync( mPlayer->IsPlaying() ? tTVPVideoOverlayStatus::Play : tTVPVideoOverlayStatus::Stop );
		});
	}
	if (mPlayer && !mUseYUV) {
		mPlayer->SetLayerMode(Mode == vomLayer);
		mPlayer->SetOnVideoDecoded([this](int w, int h, iTVPMoviePlayer::DestUpdater updater) {
			if (Mode != vomLayer) {
				// overlay presenter モード (vomOverlay/vomMixer/vomMFEVR): pull 型 presenter へ
				// ARGB フレームを渡す (WINVER の isOverlay 経路に相当)。
				if (Presenter) {
					Presenter->UpdateFrame(w, h, updater);
				}
			} else if (Mode == vomLayer) {
				// フロー制御: 直前のフレームをまだ consumer (Update) が取り込んで
				// いない (updateSurface==true) 間は、この新フレームを破棄する。
				//  - updater() (= YUV→RGB 変換) を呼ばないので、表示されずに上書き
				//    されるだけの中間フレームの変換コストを丸ごと省ける。
				//  - consumer が AssignMainImage で参照中のバッファを producer が
				//    上書きしないので、合成/アップロード途中の差し替え (ティアリング)
				//    も防げる。バッファは 2 枚、書き込み先 (currentSurface) は常に
				//    consumer が保持していない方になる。
				bool produce;
				{
					tTJSCriticalSectionHolder cs(surfaceLock);
					produce = !updateSurface;
				}
				if (produce) {
					if (!Bitmap[currentSurface]) {
						Bitmap[currentSurface] = new tTVPBaseBitmap(w, h, 32);
					} else {
						if (Bitmap[currentSurface]->GetWidth() != w || Bitmap[currentSurface]->GetHeight() != h)
							// Just set the size without changing the buffer
							Bitmap[currentSurface]->SetSize(w, h);
					}
					tTVPBitmap *bitmap = Bitmap[currentSurface]->GetBitmap();
					tjs_int dest_pitch = bitmap->GetPitch();
					char *destp = static_cast<char*>(bitmap->GetScanLine(0));
					updater(destp, dest_pitch);
					{
						tTJSCriticalSectionHolder cs(surfaceLock);
						updateSurface = true;
						currentSurface = (currentSurface + 1) % 2; // Toggle between 0 and 1
					}
				}
				// else: 前フレーム未消費につき drop (変換せず)
			}
			SetStatusAsync( mPlayer->IsPlaying() ? tTVPVideoOverlayStatus::Play : tTVPVideoOverlayStatus::Stop );
		});
	}
	if (!mPlayer) {
		SetStatus( tTVPVideoOverlayStatus::LoadError );
	}
}
//---------------------------------------------------------------------------
void tTJSNI_VideoOverlay::Close()
{
	// pull 経路を先に解放 (DrawDevice の登録解除 + フレーム/テクスチャ破棄)。
	ReleasePresenter();
	if (mPlayer) {
		Window->DelVideoOverlay(this);
		delete mPlayer;
		mPlayer = nullptr;
	}
	if( Bitmap[0] ) {
		delete Bitmap[0];
		Bitmap[0] = nullptr;
	}
	if( Bitmap[1] ) {
		delete Bitmap[1];		
		Bitmap[1] = nullptr;
	}
	SetStatus(tTVPVideoOverlayStatus::Unload);
}
//---------------------------------------------------------------------------
void tTJSNI_VideoOverlay::Shutdown() 
{
	Close();
}
//---------------------------------------------------------------------------
void tTJSNI_VideoOverlay::Disconnect() 
{
	Shutdown();
}
//---------------------------------------------------------------------------
//---------------------------------------------------------------------------
void tTJSNI_VideoOverlay::PreparePresenter()
{
	// Open 時: 非 layer モードのみ、登録済み factory (SDL / GL) を試して現行 DrawDevice に
	// 合う presenter を生成し bind する (pull はまだ開始しない)。presenter が YUV 対応なら
	// movie player を YUV plane 出力で開ける (mUseYUV)。
	mUseYUV = false;
	if( Mode == vomLayer || !Window ) return;
	if( !Presenter )
		Presenter = TVPCreateBoundVideoOverlayPresenter( Window->GetDrawDeviceObject() );
	if( Presenter ) mUseYUV = Presenter->SupportsYUV();
}
//---------------------------------------------------------------------------
void tTJSNI_VideoOverlay::TryRegisterPresenter()
{
	// Play 時: bind 済み presenter の pull を開始 (Activate)。未 bind (Prepare 失敗) なら何もしない。
	if( PresenterRegistered ) return;
	if( Presenter ) { Presenter->Activate(); PresenterRegistered = true; }
}
//---------------------------------------------------------------------------
void tTJSNI_VideoOverlay::UnregisterPresenter()
{
	// object / bind は保持 (replay で再 Activate)。pull を止めて DrawDevice をゲーム描画へ戻す。
	if( Presenter && PresenterRegistered ) Presenter->Deactivate();
	PresenterRegistered = false;
}
//---------------------------------------------------------------------------
void tTJSNI_VideoOverlay::ReleasePresenter()
{
	if( Presenter ) {
		Presenter->Deactivate();
		Presenter->ClearFrame();
		delete Presenter;
		Presenter = nullptr;
	}
	PresenterRegistered = false;
	mUseYUV = false;
}
//---------------------------------------------------------------------------
void tTJSNI_VideoOverlay::Play() {
	if (mPlayer) {
		// pull 経路 (presenter) を先に確保してから再生開始 (decode コールバックが即来ても
		// 最初のフレームから presenter へ渡せるように)。
		TryRegisterPresenter();
		mPlayer->Play();
		Window->AddVideoOverlay(this);
		// フレームコールバックが来ない実装 (wasm mixer = DOM 表示) でも
		// Status を Play にして CheckUpdate の終了検知を有効化する。
		// SetStatusAsync は変化時のみ発火するので通常実装では実害なし
		SetStatusAsync( tTVPVideoOverlayStatus::Play );
	}
}
//---------------------------------------------------------------------------
void tTJSNI_VideoOverlay::Stop() {
	if (mPlayer) {
		mPlayer->Stop();
	}
}
//---------------------------------------------------------------------------
void tTJSNI_VideoOverlay::Pause() {
	if (mPlayer) {
		mPlayer->Pause();
	}
}
//---------------------------------------------------------------------------
void tTJSNI_VideoOverlay::Rewind() {
	if (mPlayer) {
		mPlayer->Seek( 0 );
	}
}
//---------------------------------------------------------------------------
void tTJSNI_VideoOverlay::Prepare() {}
//---------------------------------------------------------------------------
void tTJSNI_VideoOverlay::SetSegmentLoop( int comeFrame, int goFrame ) {}
//---------------------------------------------------------------------------
void tTJSNI_VideoOverlay::SetPeriodEvent( int eventFrame ) {}
//---------------------------------------------------------------------------
void tTJSNI_VideoOverlay::SetRectangleToVideoOverlay() {}
//---------------------------------------------------------------------------
void tTJSNI_VideoOverlay::SetPosition(tjs_int left, tjs_int top) {
	if( Mode == vomLayer )
	{
		if( Layer1 != NULL ) Layer1->SetPosition( left, top );
		if( Layer2 != NULL ) Layer2->SetPosition( left, top );
	}
	else
	{
		// XXX
	}
}
//---------------------------------------------------------------------------
void tTJSNI_VideoOverlay::SetSize(tjs_int width, tjs_int height) {
}
//---------------------------------------------------------------------------
void tTJSNI_VideoOverlay::SetBounds(const tTVPRect & rect) {
}
//---------------------------------------------------------------------------
void tTJSNI_VideoOverlay::SetLeft(tjs_int l) {
	if( Mode == vomLayer )
	{
		if( Layer1 != NULL ) Layer1->SetLeft( l );
		if( Layer2 != NULL ) Layer2->SetLeft( l );
	}
}
//---------------------------------------------------------------------------
void tTJSNI_VideoOverlay::SetTop(tjs_int t) 
{
	if( Mode == vomLayer )
	{
		if( Layer1 != NULL ) Layer1->SetTop( t );
		if( Layer2 != NULL ) Layer2->SetTop( t );
	}

}
//---------------------------------------------------------------------------
void tTJSNI_VideoOverlay::SetWidth(tjs_int w) {}
//---------------------------------------------------------------------------
void tTJSNI_VideoOverlay::SetHeight(tjs_int h) {}
//---------------------------------------------------------------------------
void tTJSNI_VideoOverlay::SetVisible(bool b) {
	Visible = b;
	if( Mode == vomLayer )
	{
		if( Layer1 != NULL ) Layer1->SetVisible( Visible );
		if( Layer2 != NULL ) Layer2->SetVisible( Visible );
	} else {
		// 自前表示を持つ実装 (wasm <video>) の表示制御。通常実装では no-op
		if( mPlayer ) mPlayer->SetOverlayVisible( Visible );
	}
}
//---------------------------------------------------------------------------
bool tTJSNI_VideoOverlay::GetVisible() const {
	return Visible; 
}
//---------------------------------------------------------------------------
void tTJSNI_VideoOverlay::ResetOverlayParams() {}
//---------------------------------------------------------------------------
void tTJSNI_VideoOverlay::DetachVideoOverlay() {}
//---------------------------------------------------------------------------
void tTJSNI_VideoOverlay::SetRectOffset(tjs_int ofsx, tjs_int ofsy) {}
//---------------------------------------------------------------------------
void tTJSNI_VideoOverlay::SetTimePosition( tjs_uint64 p ) {
	if (mPlayer) {
		mPlayer->Seek( p * 1000 );
	}
}
//---------------------------------------------------------------------------
tjs_uint64 tTJSNI_VideoOverlay::GetTimePosition() {
	if (mPlayer) {
		return mPlayer->Position() / 1000;
	}
	return 0;
}
//---------------------------------------------------------------------------
void tTJSNI_VideoOverlay::SetFrame( tjs_int f ) {}
//---------------------------------------------------------------------------
tjs_int tTJSNI_VideoOverlay::GetFrame() {
	return 0;
}
//---------------------------------------------------------------------------
void tTJSNI_VideoOverlay::SetStopFrame( tjs_int f ) {}
//---------------------------------------------------------------------------
void tTJSNI_VideoOverlay::SetDefaultStopFrame() {}
//---------------------------------------------------------------------------
tjs_int tTJSNI_VideoOverlay::GetStopFrame() {
	return 0;
}
//---------------------------------------------------------------------------
tjs_real tTJSNI_VideoOverlay::GetFPS() {
	return 0.0;
}
//---------------------------------------------------------------------------
tjs_int tTJSNI_VideoOverlay::GetNumberOfFrame() {
	return 0;
}
//---------------------------------------------------------------------------
tjs_int64 tTJSNI_VideoOverlay::GetTotalTime() {
	if( mPlayer ) {
		return mPlayer->Duration() / 1000;
	}
	return 0;
}
//---------------------------------------------------------------------------
void tTJSNI_VideoOverlay::SetLoop( bool b ) {}
//---------------------------------------------------------------------------
void tTJSNI_VideoOverlay::SetLayer1( tTJSNI_BaseLayer *l ) { Layer1 = l; }
//---------------------------------------------------------------------------
void tTJSNI_VideoOverlay::SetLayer2( tTJSNI_BaseLayer *l ) { Layer2 = l; }
//---------------------------------------------------------------------------
void tTJSNI_VideoOverlay::SetMode( tTVPVideoOverlayMode m ) {
	// ビデオオープン後のモード変更は禁止
	if( !mPlayer )
	{
		// WINVER と揃える: 実モードは vomLayer(レイヤ描画) か、それ以外=overlay presenter。
		// generic/SDL は HW(MF/EVR)経路が無いので vomOverlay/vomMixer/vomMFEVR は全て
		// presenter 合成で同一挙動。vomMFEVR(EVR 廃止済)は vomOverlay に丸める。
		// (WINVER では vomMixer だけ HW 抑止だが SDL は元々 presenter=CPU/GPU 固定)。
		if( m == vomMFEVR ) m = vomOverlay;
		Mode = m;
	}
}
//---------------------------------------------------------------------------
tjs_real tTJSNI_VideoOverlay::GetPlayRate()
{
	return 0.0;
}
//---------------------------------------------------------------------------
void tTJSNI_VideoOverlay::SetPlayRate(tjs_real r) {}
//---------------------------------------------------------------------------
tjs_int tTJSNI_VideoOverlay::GetAudioBalance()
{
	return 0;
}
//---------------------------------------------------------------------------
void tTJSNI_VideoOverlay::SetAudioBalance(tjs_int b) {}
//---------------------------------------------------------------------------
tjs_int tTJSNI_VideoOverlay::GetAudioVolume() {
	if (mPlayer) {
		float volume = mPlayer->Volume();
		return (tjs_int)(volume * 100000);
	}
	return 0;
}
//---------------------------------------------------------------------------
void tTJSNI_VideoOverlay::SetAudioVolume(tjs_int b) {
	if (mPlayer) {
		if( b < 0 ) b = 0;
		if( b > 100000 ) b = 100000;
		float volume = (float)b / 100000.0f;
		mPlayer->SetVolume ( volume );
	}
}
//---------------------------------------------------------------------------
tjs_uint tTJSNI_VideoOverlay::GetNumberOfAudioStream()
{
	return 0;
}
//---------------------------------------------------------------------------
void tTJSNI_VideoOverlay::SelectAudioStream(tjs_uint n) {}
//---------------------------------------------------------------------------
tjs_int tTJSNI_VideoOverlay::GetEnabledAudioStream()
{
	return 0;
}
//---------------------------------------------------------------------------
void tTJSNI_VideoOverlay::DisableAudioStream() {}
//---------------------------------------------------------------------------
tjs_uint tTJSNI_VideoOverlay::GetNumberOfVideoStream()
{
	return 0;
}
//---------------------------------------------------------------------------
void tTJSNI_VideoOverlay::SelectVideoStream(tjs_uint n) {}
//---------------------------------------------------------------------------
tjs_int tTJSNI_VideoOverlay::GetEnabledVideoStream()
{
	return 0;
}
//---------------------------------------------------------------------------
void tTJSNI_VideoOverlay::SetMixingLayer( tTJSNI_BaseLayer *l )
{
	// presenter 経路 (overlay/mixer) のみ有効。レイヤ画像のスナップショットを presenter へ渡し、
	// 動画の上へ α 合成させる (WINVER の presenter mixer と同構造)。vomLayer では意味を持たない。
	if( !Presenter ) return;
	if( l && l->GetVisible() )
	{
		tTVPBaseBitmap *src = l->GetMainImage();
		tTVPBitmap *raw = src ? src->GetBitmap() : nullptr;
		if( raw )
		{
			int w = (int)raw->GetWidth();
			int h = (int)raw->GetHeight();
			if( w > 0 && h > 0 )
			{
				// レイヤバッファはボトムアップ格納。視覚的 top 行 (ScanLine 0) + 符号付きピッチ。
				const void *top = raw->GetScanLine(0);
				int pitch = w * 4;
				if( h > 1 )
					pitch = (int)( (const tjs_uint8*)raw->GetScanLine(1) - (const tjs_uint8*)top );
				mMixerRect.left   = l->GetLeft() + l->GetImageLeft();
				mMixerRect.top    = l->GetTop()  + l->GetImageTop();
				mMixerRect.right  = mMixerRect.left + (tjs_int)l->GetImageWidth();
				mMixerRect.bottom = mMixerRect.top  + (tjs_int)l->GetImageHeight();
				mMixerAlpha = (tjs_real)l->GetOpacity() / 255.0;
				Presenter->SetMixerImage( top, w, h, pitch, mMixerRect, (float)mMixerAlpha );
				return;
			}
		}
	}
	// 非表示 / 画像無し → mixer 画像をクリア
	Presenter->ClearMixerImage();
}
//---------------------------------------------------------------------------
void tTJSNI_VideoOverlay::ResetMixingBitmap()
{
	if( Presenter ) Presenter->ClearMixerImage();
}
//---------------------------------------------------------------------------
void tTJSNI_VideoOverlay::SetMixingMovieAlpha( tjs_real a )
{
	mMixerAlpha = a;
	if( Presenter ) Presenter->SetMixerAlpha( (float)a );
}
//---------------------------------------------------------------------------
tjs_real tTJSNI_VideoOverlay::GetMixingMovieAlpha()
{
	return mMixerAlpha;
}
//---------------------------------------------------------------------------
void tTJSNI_VideoOverlay::SetMixingMovieBGColor( tjs_uint col )
{
	mMixerBGColor = col; // 保持のみ (presenter は現状未使用)
}
//---------------------------------------------------------------------------
tjs_uint tTJSNI_VideoOverlay::GetMixingMovieBGColor()
{
	return mMixerBGColor;
}
//---------------------------------------------------------------------------
tjs_real tTJSNI_VideoOverlay::GetContrastRangeMin()
{
	TJS_eTJSError(TJSNotImplemented);
	return 0.0f;
}
//---------------------------------------------------------------------------
tjs_real tTJSNI_VideoOverlay::GetContrastRangeMax()
{
	TJS_eTJSError(TJSNotImplemented);
	return 0.0f;
}
//---------------------------------------------------------------------------
tjs_real tTJSNI_VideoOverlay::GetContrastDefaultValue()
{
	TJS_eTJSError(TJSNotImplemented);
	return 0.0f;
}
//---------------------------------------------------------------------------
tjs_real tTJSNI_VideoOverlay::GetContrastStepSize()
{
	TJS_eTJSError(TJSNotImplemented);
	return 0.0f;
}
//---------------------------------------------------------------------------
tjs_real tTJSNI_VideoOverlay::GetContrast()
{
	TJS_eTJSError(TJSNotImplemented);
	return 0.0f;
}
//---------------------------------------------------------------------------
void tTJSNI_VideoOverlay::SetContrast( tjs_real v )
{
	TJS_eTJSError(TJSNotImplemented);
}
//---------------------------------------------------------------------------
tjs_real tTJSNI_VideoOverlay::GetBrightnessRangeMin()
{
	TJS_eTJSError(TJSNotImplemented);
	return 0.0f;
}
//---------------------------------------------------------------------------
tjs_real tTJSNI_VideoOverlay::GetBrightnessRangeMax()
{
	TJS_eTJSError(TJSNotImplemented);
	return 0.0f;
}
//---------------------------------------------------------------------------
tjs_real tTJSNI_VideoOverlay::GetBrightnessDefaultValue()
{
	TJS_eTJSError(TJSNotImplemented);
	return 0.0f;
}
//---------------------------------------------------------------------------
tjs_real tTJSNI_VideoOverlay::GetBrightnessStepSize()
{
	TJS_eTJSError(TJSNotImplemented);
	return 0.0f;
}
//---------------------------------------------------------------------------
tjs_real tTJSNI_VideoOverlay::GetBrightness()
{
	TJS_eTJSError(TJSNotImplemented);
	return 0.0f;
}
//---------------------------------------------------------------------------
void tTJSNI_VideoOverlay::SetBrightness( tjs_real v )
{
	TJS_eTJSError(TJSNotImplemented);
}
//---------------------------------------------------------------------------
tjs_real tTJSNI_VideoOverlay::GetHueRangeMin()
{
	TJS_eTJSError(TJSNotImplemented);
	return 0.0f;
}
//---------------------------------------------------------------------------
tjs_real tTJSNI_VideoOverlay::GetHueRangeMax()
{
	TJS_eTJSError(TJSNotImplemented);
	return 0.0f;
}
//---------------------------------------------------------------------------
tjs_real tTJSNI_VideoOverlay::GetHueDefaultValue()
{
	TJS_eTJSError(TJSNotImplemented);
	return 0.0f;
}
//---------------------------------------------------------------------------
tjs_real tTJSNI_VideoOverlay::GetHueStepSize()
{
	TJS_eTJSError(TJSNotImplemented);
	return 0.0f;
}
//---------------------------------------------------------------------------
tjs_real tTJSNI_VideoOverlay::GetHue()
{
	TJS_eTJSError(TJSNotImplemented);
	return 0.0f;
}
//---------------------------------------------------------------------------
void tTJSNI_VideoOverlay::SetHue( tjs_real v )
{
	TJS_eTJSError(TJSNotImplemented);
}
//---------------------------------------------------------------------------
tjs_real tTJSNI_VideoOverlay::GetSaturationRangeMin()
{
	TJS_eTJSError(TJSNotImplemented);
	return 0.0f;
}
//---------------------------------------------------------------------------
tjs_real tTJSNI_VideoOverlay::GetSaturationRangeMax()
{
	TJS_eTJSError(TJSNotImplemented);
	return 0.0f;
}
//---------------------------------------------------------------------------
tjs_real tTJSNI_VideoOverlay::GetSaturationDefaultValue()
{
	TJS_eTJSError(TJSNotImplemented);
	return 0.0f;
}
//---------------------------------------------------------------------------
tjs_real tTJSNI_VideoOverlay::GetSaturationStepSize()
{
	TJS_eTJSError(TJSNotImplemented);
	return 0.0f;
}
//---------------------------------------------------------------------------
tjs_real tTJSNI_VideoOverlay::GetSaturation()
{
	TJS_eTJSError(TJSNotImplemented);
	return 0.0f;
}
//---------------------------------------------------------------------------
void tTJSNI_VideoOverlay::SetSaturation( tjs_real v )
{
	TJS_eTJSError(TJSNotImplemented);
}
//---------------------------------------------------------------------------
tjs_int tTJSNI_VideoOverlay::GetOriginalWidth()
{
	return 0;
}
//---------------------------------------------------------------------------
tjs_int tTJSNI_VideoOverlay::GetOriginalHeight()
{
	return 0;
}

//---------------------------------------------------------------------------
// tTJSNC_VideoOverlay::CreateNativeInstance : returns proper instance object
//---------------------------------------------------------------------------
tTJSNativeInstance *tTJSNC_VideoOverlay::CreateNativeInstance()
{
	return new tTJSNI_VideoOverlay();
}
//---------------------------------------------------------------------------


//---------------------------------------------------------------------------
// TVPCreateNativeClass_VideoOverlay
//---------------------------------------------------------------------------
tTJSNativeClass * TVPCreateNativeClass_VideoOverlay()
{
	return new tTJSNC_VideoOverlay();
}
//---------------------------------------------------------------------------

