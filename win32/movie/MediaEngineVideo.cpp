/****************************************************************************/
/*! @file
@brief HW 動画プレイヤ (IMFMediaEngine フレームサーバ) 実装 — Track V-E
*****************************************************************************/
#include "MediaEngineVideo.h"
#include "VideoPresenterD3D.h"
#include <mfapi.h>
#include <mferror.h>
#include <new>
#include <math.h>

// IMFMediaEngine は CoCreateInstance で生成 (直接エクスポート無し=専用 .lib 不要)。
// CLSID/IID/MF_MEDIA_ENGINE_* 定数は mfuuid.lib、MFStartup 等は mfplat.lib。
#pragma comment(lib, "mfplat.lib")
#pragma comment(lib, "mfuuid.lib")

#ifndef EC_COMPLETE
#define EC_COMPLETE 0x0001
#endif
#ifndef WM_GRAPHNOTIFY
#define WM_GRAPHNOTIFY  (WM_USER+15)
#endif

//---------------------------------------------------------------------------
// IMFMediaEngineNotify 実装 (イベントを tTVPMediaEngineVideo へ転送)
//---------------------------------------------------------------------------
class tTVPMediaEngineNotify : public IMFMediaEngineNotify
{
	long RefCount;
	tTVPMediaEngineVideo* Owner; //!< 非所有 (Owner が本オブジェクトを所有)
public:
	tTVPMediaEngineNotify( tTVPMediaEngineVideo* owner ) : RefCount(1), Owner(owner) {}
	void Detach() { Owner = NULL; }

	STDMETHODIMP QueryInterface( REFIID riid, void** ppv )
	{
		if( !ppv ) return E_POINTER;
		if( riid == IID_IUnknown || riid == __uuidof(IMFMediaEngineNotify) ) {
			*ppv = static_cast<IMFMediaEngineNotify*>(this);
			AddRef();
			return S_OK;
		}
		*ppv = NULL;
		return E_NOINTERFACE;
	}
	STDMETHODIMP_(ULONG) AddRef() { return InterlockedIncrement(&RefCount); }
	STDMETHODIMP_(ULONG) Release()
	{
		long c = InterlockedDecrement(&RefCount);
		if( c == 0 ) delete this;
		return c;
	}
	STDMETHODIMP EventNotify( DWORD meEvent, DWORD_PTR param1, DWORD param2 )
	{
		if( Owner ) Owner->OnMediaEngineEvent( meEvent, param1, param2 );
		return S_OK;
	}
};
//---------------------------------------------------------------------------
tTVPMediaEngineVideo::tTVPMediaEngineVideo( HWND owner, ID3D11Device* engineDevice )
: RefCount(1), OwnerWindow(owner), EngineDevice(engineDevice)
, MFStarted(false), MediaEngine(NULL), Notify(NULL), DXGIManager(NULL), DXGIResetToken(0)
, HeldStream(NULL), ByteStream(NULL)
, Ready(false), Ended(false), VideoWidth(0), VideoHeight(0)
, Visible(false), MovieAlpha(1.0f), Playing(false)
, DestTex(NULL), DestSrv(NULL), DestW(0), DestH(0), HasFrame(false), Blit(NULL)
{
	if( EngineDevice ) EngineDevice->AddRef();
}
//---------------------------------------------------------------------------
tTVPMediaEngineVideo::~tTVPMediaEngineVideo()
{
	Shutdown();
	if( EngineDevice ) { EngineDevice->Release(); EngineDevice = NULL; }
}
//---------------------------------------------------------------------------
void tTVPMediaEngineVideo::Shutdown()
{
	if( MediaEngine ) {
		MediaEngine->Shutdown();
		MediaEngine->Release();
		MediaEngine = NULL;
	}
	if( Notify ) {
		static_cast<tTVPMediaEngineNotify*>(Notify)->Detach();
		Notify->Release();
		Notify = NULL;
	}
	if( ByteStream ) { ByteStream->Release(); ByteStream = NULL; }
	if( HeldStream ) { HeldStream->Release(); HeldStream = NULL; }
	if( DXGIManager ) { DXGIManager->Release(); DXGIManager = NULL; }
	ReleaseDestTexture();
	if( Blit ) { delete Blit; Blit = NULL; }
	if( MFStarted ) { MFShutdown(); MFStarted = false; }
}
//---------------------------------------------------------------------------
bool tTVPMediaEngineVideo::Open( IStream* stream, const tjs_char* /*type*/, unsigned __int64 /*size*/ )
{
	if( !EngineDevice || !stream ) return false;
	HRESULT hr;

	hr = MFStartup( MF_VERSION, MFSTARTUP_FULL );
	if( FAILED(hr) ) return false;
	MFStarted = true;

	// DXGI デバイスマネージャを engine デバイスに束ねる
	hr = MFCreateDXGIDeviceManager( &DXGIResetToken, &DXGIManager );
	if( FAILED(hr) ) return false;
	hr = DXGIManager->ResetDevice( EngineDevice, DXGIResetToken );
	if( FAILED(hr) ) return false;

	// class factory (COM)
	IMFMediaEngineClassFactory* factory = NULL;
	hr = CoCreateInstance( CLSID_MFMediaEngineClassFactory, NULL, CLSCTX_INPROC_SERVER,
		IID_PPV_ARGS(&factory) );
	if( FAILED(hr) || !factory ) return false;

	// Notify + attributes
	Notify = new (std::nothrow) tTVPMediaEngineNotify( this );
	if( !Notify ) { factory->Release(); return false; }

	IMFAttributes* attr = NULL;
	hr = MFCreateAttributes( &attr, 4 );
	if( SUCCEEDED(hr) ) {
		attr->SetUnknown( MF_MEDIA_ENGINE_DXGI_MANAGER, DXGIManager );
		attr->SetUnknown( MF_MEDIA_ENGINE_CALLBACK, Notify );
		attr->SetUINT32( MF_MEDIA_ENGINE_VIDEO_OUTPUT_FORMAT, DXGI_FORMAT_B8G8R8A8_UNORM );
		// フレームサーバモード (再生ウィンドウを指定しない)
		hr = factory->CreateInstance( 0, attr, &MediaEngine );
		attr->Release();
	}
	factory->Release();
	if( FAILED(hr) || !MediaEngine ) return false;

	// byte stream (kirikiri IStream をラップ) + ソース設定
	HeldStream = stream;
	HeldStream->AddRef();
	hr = MFCreateMFByteStreamOnStream( stream, &ByteStream );
	if( FAILED(hr) || !ByteStream ) return false;

	IMFMediaEngineEx* mex = NULL;
	hr = MediaEngine->QueryInterface( IID_PPV_ARGS(&mex) );
	if( FAILED(hr) || !mex ) return false;
	BSTR url = SysAllocString( L"kirikiri://movie" );
	hr = mex->SetSourceFromByteStream( ByteStream, url );
	SysFreeString( url );
	mex->Release();
	if( FAILED(hr) ) return false;

	return true; // 非同期ロード。Ready は LOADEDMETADATA イベントで立つ
}
//---------------------------------------------------------------------------
void tTVPMediaEngineVideo::OnMediaEngineEvent( DWORD meEvent, DWORD_PTR /*param1*/, DWORD /*param2*/ )
{
	switch( meEvent )
	{
	case MF_MEDIA_ENGINE_EVENT_LOADEDMETADATA:
	{
		DWORD w = 0, h = 0;
		if( MediaEngine && SUCCEEDED( MediaEngine->GetNativeVideoSize( &w, &h ) ) ) {
			VideoWidth = (long)w;
			VideoHeight = (long)h;
		}
		Ready = true;
		break;
	}
	case MF_MEDIA_ENGINE_EVENT_ENDED:
		Ended = true;
		Playing = false;
		if( OwnerWindow ) ::PostMessage( OwnerWindow, WM_GRAPHNOTIFY, 0, 0 );
		break;
	case MF_MEDIA_ENGINE_EVENT_ERROR:
		Ended = true;
		Playing = false;
		if( OwnerWindow ) ::PostMessage( OwnerWindow, WM_GRAPHNOTIFY, 0, 0 );
		break;
	default:
		break;
	}
}
//---------------------------------------------------------------------------
bool tTVPMediaEngineVideo::EnsureDestTexture( ID3D11Device* dev, int w, int h )
{
	if( DestTex && DestSrv && DestW == w && DestH == h ) return true;
	ReleaseDestTexture();
	if( w <= 0 || h <= 0 ) return false;

	D3D11_TEXTURE2D_DESC td; ZeroMemory( &td, sizeof(td) );
	td.Width = w; td.Height = h;
	td.MipLevels = 1; td.ArraySize = 1;
	td.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
	td.SampleDesc.Count = 1;
	td.Usage = D3D11_USAGE_DEFAULT;
	td.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
	HRESULT hr = dev->CreateTexture2D( &td, NULL, &DestTex );
	if( FAILED(hr) ) return false;
	hr = dev->CreateShaderResourceView( DestTex, NULL, &DestSrv );
	if( FAILED(hr) ) { ReleaseDestTexture(); return false; }
	DestW = w; DestH = h;
	return true;
}
//---------------------------------------------------------------------------
void tTVPMediaEngineVideo::ReleaseDestTexture()
{
	if( DestSrv ) { DestSrv->Release(); DestSrv = NULL; }
	if( DestTex ) { DestTex->Release(); DestTex = NULL; }
	DestW = DestH = 0;
}
//---------------------------------------------------------------------------
bool TJS_INTF_METHOD tTVPMediaEngineVideo::RenderVideoFrame( const tTVPVideoPresenterContext & ctx )
{
	if( !Ready || !MediaEngine || !Visible ) return false;
	int w = (int)VideoWidth.load();
	int h = (int)VideoHeight.load();
	if( w <= 0 || h <= 0 ) return false;

	// 新フレームがあれば engine デバイス上の宛先テクスチャへ HW 転送
	LONGLONG pts = 0;
	if( MediaEngine->OnVideoStreamTick( &pts ) == S_OK ) {
		if( EnsureDestTexture( ctx.Device, w, h ) ) {
			MFVideoNormalizedRect srcNorm = { 0.0f, 0.0f, 1.0f, 1.0f };
			RECT dstRect = { 0, 0, w, h };
			MFARGB border = { 0, 0, 0, 255 };
			if( SUCCEEDED( MediaEngine->TransferVideoFrame( DestTex, &srcNorm, &dstRect, &border ) ) )
				HasFrame = true;
		}
	}
	if( !HasFrame || !DestSrv ) return false;

	if( !Blit ) Blit = new tTVPVideoPresenterD3D();
	Blit->RenderSRV( ctx, DestSrv, w, h, ctx.DestRect, MovieAlpha );
	return true;
}
//---------------------------------------------------------------------------
// iTVPVideoOverlay
//---------------------------------------------------------------------------
void __stdcall tTVPMediaEngineVideo::AddRef() { InterlockedIncrement(&RefCount); }
void __stdcall tTVPMediaEngineVideo::Release()
{
	if( InterlockedDecrement(&RefCount) == 0 ) delete this;
}
//---------------------------------------------------------------------------
void __stdcall tTVPMediaEngineVideo::Play()
{
	if( MediaEngine ) {
		Ended = false;
		MediaEngine->Play();
		Playing = true;
	}
}
void __stdcall tTVPMediaEngineVideo::Stop()
{
	if( MediaEngine ) {
		MediaEngine->Pause();
		MediaEngine->SetCurrentTime( 0.0 );
		Playing = false;
		HasFrame = false;
	}
}
void __stdcall tTVPMediaEngineVideo::Pause()
{
	if( MediaEngine ) { MediaEngine->Pause(); Playing = false; }
}
void __stdcall tTVPMediaEngineVideo::Rewind()
{
	if( MediaEngine ) MediaEngine->SetCurrentTime( 0.0 );
	Ended = false;
}
void __stdcall tTVPMediaEngineVideo::SetPosition( unsigned __int64 tick )
{
	if( MediaEngine ) MediaEngine->SetCurrentTime( (double)tick / 1000.0 );
}
void __stdcall tTVPMediaEngineVideo::GetPosition( unsigned __int64 *tick )
{
	if( tick ) *tick = MediaEngine ? (unsigned __int64)( MediaEngine->GetCurrentTime() * 1000.0 ) : 0;
}
void __stdcall tTVPMediaEngineVideo::GetStatus( tTVPVideoStatus *status )
{
	if( !status ) return;
	if( Ended ) *status = vsEnded;
	else if( Playing ) *status = vsPlaying;
	else *status = vsStopped;
}
void __stdcall tTVPMediaEngineVideo::GetEvent( long *evcode, LONG_PTR *param1, LONG_PTR *param2, bool *got )
{
	// 終端を EC_COMPLETE として疑似通知 (ループ/停止判定に使う)
	if( Ended.exchange(false) ) {
		if( evcode ) *evcode = EC_COMPLETE;
		if( param1 ) *param1 = 0;
		if( param2 ) *param2 = 0;
		if( got ) *got = true;
		return;
	}
	if( got ) *got = false;
}
void __stdcall tTVPMediaEngineVideo::FreeEventParams( long, LONG_PTR, LONG_PTR ) {}
//---------------------------------------------------------------------------
void __stdcall tTVPMediaEngineVideo::SetFrame( int f )
{
	// フレーム番号→時刻。fps 不明なので概算 (30fps 仮定)。主用途はシーク。
	if( MediaEngine ) MediaEngine->SetCurrentTime( (double)f / 30.0 );
}
void __stdcall tTVPMediaEngineVideo::GetFrame( int *f )
{
	if( f ) *f = MediaEngine ? (int)( MediaEngine->GetCurrentTime() * 30.0 ) : 0;
}
void __stdcall tTVPMediaEngineVideo::GetFPS( double *f ) { if(f) *f = 30.0; }
void __stdcall tTVPMediaEngineVideo::GetNumberOfFrame( int *f )
{
	if( f ) *f = MediaEngine ? (int)( MediaEngine->GetDuration() * 30.0 ) : 0;
}
void __stdcall tTVPMediaEngineVideo::GetTotalTime( __int64 *t )
{
	if( t ) *t = MediaEngine ? (__int64)( MediaEngine->GetDuration() * 1000.0 ) : 0;
}
void __stdcall tTVPMediaEngineVideo::GetVideoSize( long *width, long *height )
{
	if( width ) *width = VideoWidth.load();
	if( height ) *height = VideoHeight.load();
}
void __stdcall tTVPMediaEngineVideo::GetFrontBuffer( BYTE **buff ) { if(buff) *buff = NULL; }
void __stdcall tTVPMediaEngineVideo::SetVideoBuffer( BYTE*, BYTE*, long ) {}
void __stdcall tTVPMediaEngineVideo::SetPlayRate( double rate )
{
	if( MediaEngine ) MediaEngine->SetPlaybackRate( rate );
}
void __stdcall tTVPMediaEngineVideo::GetPlayRate( double *rate )
{
	if( rate ) *rate = MediaEngine ? MediaEngine->GetPlaybackRate() : 1.0;
}
void __stdcall tTVPMediaEngineVideo::SetAudioVolume( long volume )
{
	// kirikiri: DirectSound 減衰 (mB, -10000..0)。MediaEngine: 0.0..1.0。
	if( MediaEngine ) {
		double v = ( volume <= -10000 ) ? 0.0 : pow( 10.0, (double)volume / 2000.0 );
		if( v > 1.0 ) v = 1.0; if( v < 0.0 ) v = 0.0;
		MediaEngine->SetVolume( v );
	}
}
void __stdcall tTVPMediaEngineVideo::GetAudioVolume( long *volume )
{
	if( !volume ) return;
	double v = MediaEngine ? MediaEngine->GetVolume() : 1.0;
	*volume = ( v <= 0.0 ) ? -10000 : (long)( 2000.0 * log10( v ) );
}
//---------------------------------------------------------------------------
// ファクトリ (exe へ静的統合。VideoOvlImpl.cpp から extern 直呼び)。engineDevice は
// ID3D11Device* を void* で受ける (このヘッダに d3d11 型を出さないため)。成功で *out に
// iTVPVideoOverlay*、*outPresenter に描画用 iTVPVideoPresenter* を返す。失敗で *out=NULL。
extern "C" void __stdcall GetMediaEngineVideoObject(
	HWND callbackwin, IStream* stream, const wchar_t* /*streamname*/,
	const wchar_t* type, unsigned __int64 size, void* engineDevice,
	iTVPVideoOverlay** out, iTVPVideoPresenter** outPresenter )
{
	if( out ) *out = NULL;
	if( outPresenter ) *outPresenter = NULL;
	if( !out || !engineDevice ) return;

	tTVPMediaEngineVideo* obj =
		new (std::nothrow) tTVPMediaEngineVideo( callbackwin, (ID3D11Device*)engineDevice );
	if( !obj ) return;
	if( !obj->Open( stream, type, size ) ) { obj->Release(); return; }
	*out = static_cast<iTVPVideoOverlay*>( obj );
	if( outPresenter ) *outPresenter = obj->GetPresenter();
}
//---------------------------------------------------------------------------
