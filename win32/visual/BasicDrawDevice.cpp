
#define NOMINMAX
#include "tjsCommHead.h"
#include "DrawDevice.h"
#include "BasicDrawDevice.h"
#include "LayerIntf.h"
#include "MsgImpl.h"
#include "SysInitIntf.h"
#include "WindowIntf.h"
#include "DebugIntf.h"
#include "ThreadIntf.h"
#include "ComplexRect.h"
#include "EventIntf.h"
#include "WindowImpl.h"
#include "BitmapInfomation.h"
#ifdef KRKRZ_USE_REPL
#include "ScreenCapture.h"
#endif

#include <d3d11.h>
#include <d3d11_4.h>   // ID3D11Multithread (Track V-E: HW 動画のデバイス共有保護)
#include <dxgi1_2.h>
#include <d3dcompiler.h>
#include <algorithm>
#include <string.h>
#include <stdlib.h>

//---------------------------------------------------------------------------
// 補助
//---------------------------------------------------------------------------
template<class T> static inline void TVPSafeRelease(T *&p) { if(p) { p->Release(); p = nullptr; } }

//---------------------------------------------------------------------------
// オプション
//---------------------------------------------------------------------------
static tjs_int TVPBasicDrawDeviceOptionsGeneration = 0;
bool TVPZoomInterpolation = true;
//---------------------------------------------------------------------------
static void TVPInitBasicDrawDeviceOptions()
{
	if(TVPBasicDrawDeviceOptionsGeneration == TVPGetCommandLineArgumentGeneration()) return;
	TVPBasicDrawDeviceOptionsGeneration = TVPGetCommandLineArgumentGeneration();

	tTJSVariant val;
	TVPZoomInterpolation = true;
	if(TVPGetCommandLine(TJS_W("-smoothzoom"), &val))
	{
		ttstr str(val);
		if(str == TJS_W("no"))
			TVPZoomInterpolation = false;
		else
			TVPZoomInterpolation = true;
	}
}
//---------------------------------------------------------------------------
// D3D11 提示用の頂点。位置は NDC (クリップ空間)、uv はテクスチャ座標。
struct tTVPBDDVertex { float x, y; float u, v; };
//---------------------------------------------------------------------------
// passthrough シェーダ (実行時 D3DCompile)
static const char TVPBDDShaderHLSL[] =
	"struct VSIn  { float2 pos : POSITION; float2 uv : TEXCOORD0; };\n"
	"struct VSOut { float4 pos : SV_Position; float2 uv : TEXCOORD0; };\n"
	"VSOut VSMain(VSIn i){ VSOut o; o.pos = float4(i.pos, 0.0f, 1.0f); o.uv = i.uv; return o; }\n"
	"Texture2D    tex : register(t0);\n"
	"SamplerState smp : register(s0);\n"
	"float4 PSMain(VSOut i) : SV_Target { return float4(tex.Sample(smp, i.uv).rgb, 1.0f); }\n";
//---------------------------------------------------------------------------



//---------------------------------------------------------------------------
tTVPBasicDrawDevice::tTVPBasicDrawDevice()
{
	TVPInitBasicDrawDeviceOptions(); // read and initialize options
	TargetWindow = NULL;
	IsMainWindow = false;
	DrawUpdateRectangle = false;

	D3DDevice = NULL;
	D3DContext = NULL;
	SwapChain = NULL;
	DXGIOutput = NULL;
	BackBufferRTV = NULL;

	VertexShader = NULL;
	PixelShader = NULL;
	InputLayout = NULL;
	VertexBuffer = NULL;
	SamplerLinear = NULL;
	SamplerPoint = NULL;

	Texture = NULL;
	TextureSRV = NULL;
	TextureBuffer = NULL;
	TexturePitch = 0;
	TextureWidth = TextureHeight = 0;

	SwapWidth = SwapHeight = 0;
	ShouldShow = false;
	VsyncInterval = 16;

	VideoPresenter = NULL;
}
//---------------------------------------------------------------------------
tTVPBasicDrawDevice::~tTVPBasicDrawDevice()
{
	DestroyD3DDevice();
}
//---------------------------------------------------------------------------
void tTVPBasicDrawDevice::DestroyD3DDevice()
{
	DestroyTexture();
	DestroySwapChain();
	DestroyPresentPipeline();
	if(D3DContext) { D3DContext->ClearState(); }
	TVPSafeRelease(D3DContext);
	TVPSafeRelease(D3DDevice);
}
//---------------------------------------------------------------------------
void tTVPBasicDrawDevice::DestroySwapChain()
{
	TVPSafeRelease(BackBufferRTV);
	TVPSafeRelease(DXGIOutput);
	TVPSafeRelease(SwapChain);
	SwapWidth = SwapHeight = 0;
}
//---------------------------------------------------------------------------
void tTVPBasicDrawDevice::DestroyPresentPipeline()
{
	TVPSafeRelease(SamplerPoint);
	TVPSafeRelease(SamplerLinear);
	TVPSafeRelease(VertexBuffer);
	TVPSafeRelease(InputLayout);
	TVPSafeRelease(PixelShader);
	TVPSafeRelease(VertexShader);
}
//---------------------------------------------------------------------------
void tTVPBasicDrawDevice::DestroyTexture()
{
	TVPSafeRelease(TextureSRV);
	TVPSafeRelease(Texture);
	if(TextureBuffer) { free(TextureBuffer); TextureBuffer = NULL; }
	TexturePitch = 0;
	TextureWidth = TextureHeight = 0;
}
//---------------------------------------------------------------------------
void tTVPBasicDrawDevice::InvalidateAll()
{
	// レイヤ演算結果をすべてリクエストする
	// デバイスを再構築した際に内容を再構築する目的で用いる
	RequestInvalidation(tTVPRect(0, 0, DestRect.get_width(), DestRect.get_height()));
}
//---------------------------------------------------------------------------
bool tTVPBasicDrawDevice::IsTargetWindowActive() const
{
	if( TargetWindow == NULL ) return false;
	return ::GetForegroundWindow() == TargetWindow;
}
//---------------------------------------------------------------------------
bool tTVPBasicDrawDevice::GetClientSize( UINT &w, UINT &h ) const
{
	if(!TargetWindow) return false;
	RECT rc;
	if(!::GetClientRect(TargetWindow, &rc)) return false;
	LONG cw = rc.right - rc.left;
	LONG ch = rc.bottom - rc.top;
	if(cw < 1) cw = 1;
	if(ch < 1) ch = 1;
	w = (UINT)cw;
	h = (UINT)ch;
	return true;
}
//---------------------------------------------------------------------------
void tTVPBasicDrawDevice::ErrorToLog( HRESULT hr )
{
	TVPAddImportantLog( ttstr(TJS_W("(info) BasicDrawDevice(D3D11) HRESULT=0x")) + TJSInt32ToHex((tjs_uint32)hr, 8) );
}
//---------------------------------------------------------------------------
bool tTVPBasicDrawDevice::CreatePresentPipeline()
{
	DestroyPresentPipeline();

	HRESULT hr;
	ID3DBlob *vsBlob = NULL, *psBlob = NULL, *errBlob = NULL;
	UINT flags = 0;
#ifdef _DEBUG
	flags |= D3DCOMPILE_DEBUG | D3DCOMPILE_SKIP_OPTIMIZATION;
#endif

	// -- vertex shader
	hr = D3DCompile(TVPBDDShaderHLSL, sizeof(TVPBDDShaderHLSL)-1, "bdd", NULL, NULL,
		"VSMain", "vs_4_0", flags, 0, &vsBlob, &errBlob);
	if( FAILED(hr) ) { TVPSafeRelease(errBlob); ErrorToLog(hr); return false; }
	TVPSafeRelease(errBlob);

	// -- pixel shader
	hr = D3DCompile(TVPBDDShaderHLSL, sizeof(TVPBDDShaderHLSL)-1, "bdd", NULL, NULL,
		"PSMain", "ps_4_0", flags, 0, &psBlob, &errBlob);
	if( FAILED(hr) ) { TVPSafeRelease(errBlob); TVPSafeRelease(vsBlob); ErrorToLog(hr); return false; }
	TVPSafeRelease(errBlob);

	hr = D3DDevice->CreateVertexShader(vsBlob->GetBufferPointer(), vsBlob->GetBufferSize(), NULL, &VertexShader);
	if( FAILED(hr) ) { TVPSafeRelease(psBlob); TVPSafeRelease(vsBlob); ErrorToLog(hr); return false; }

	hr = D3DDevice->CreatePixelShader(psBlob->GetBufferPointer(), psBlob->GetBufferSize(), NULL, &PixelShader);
	TVPSafeRelease(psBlob);
	if( FAILED(hr) ) { TVPSafeRelease(vsBlob); ErrorToLog(hr); return false; }

	// -- input layout
	D3D11_INPUT_ELEMENT_DESC ied[] = {
		{ "POSITION", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 0,                            D3D11_INPUT_PER_VERTEX_DATA, 0 },
		{ "TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, D3D11_APPEND_ALIGNED_ELEMENT, D3D11_INPUT_PER_VERTEX_DATA, 0 },
	};
	hr = D3DDevice->CreateInputLayout(ied, 2, vsBlob->GetBufferPointer(), vsBlob->GetBufferSize(), &InputLayout);
	TVPSafeRelease(vsBlob);
	if( FAILED(hr) ) { ErrorToLog(hr); return false; }

	// -- dynamic vertex buffer (4 verts, TRIANGLESTRIP)
	D3D11_BUFFER_DESC bd; ZeroMemory(&bd, sizeof(bd));
	bd.ByteWidth = sizeof(tTVPBDDVertex) * 4;
	bd.Usage = D3D11_USAGE_DYNAMIC;
	bd.BindFlags = D3D11_BIND_VERTEX_BUFFER;
	bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
	hr = D3DDevice->CreateBuffer(&bd, NULL, &VertexBuffer);
	if( FAILED(hr) ) { ErrorToLog(hr); return false; }

	// -- samplers (linear / point, clamp)
	D3D11_SAMPLER_DESC sd; ZeroMemory(&sd, sizeof(sd));
	sd.AddressU = sd.AddressV = sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
	sd.ComparisonFunc = D3D11_COMPARISON_NEVER;
	sd.MaxLOD = D3D11_FLOAT32_MAX;
	sd.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
	hr = D3DDevice->CreateSamplerState(&sd, &SamplerLinear);
	if( FAILED(hr) ) { ErrorToLog(hr); return false; }
	sd.Filter = D3D11_FILTER_MIN_MAG_MIP_POINT;
	hr = D3DDevice->CreateSamplerState(&sd, &SamplerPoint);
	if( FAILED(hr) ) { ErrorToLog(hr); return false; }

	return true;
}
//---------------------------------------------------------------------------
bool tTVPBasicDrawDevice::EnsureBackBufferRTV()
{
	TVPSafeRelease(BackBufferRTV);
	if(!SwapChain) return false;
	ID3D11Texture2D *backbuf = NULL;
	HRESULT hr = SwapChain->GetBuffer(0, __uuidof(ID3D11Texture2D), (void**)&backbuf);
	if( FAILED(hr) ) { ErrorToLog(hr); return false; }
	hr = D3DDevice->CreateRenderTargetView(backbuf, NULL, &BackBufferRTV);
	backbuf->Release();
	if( FAILED(hr) ) { ErrorToLog(hr); return false; }
	return true;
}
//---------------------------------------------------------------------------
bool tTVPBasicDrawDevice::CreateSwapChain( UINT w, UINT h )
{
	DestroySwapChain();
	if( w < 1 ) w = 1;
	if( h < 1 ) h = 1;

	IDXGIDevice*  dxgiDevice  = NULL;
	IDXGIAdapter* dxgiAdapter = NULL;
	IDXGIFactory2* dxgiFactory = NULL;
	HRESULT hr;

	hr = D3DDevice->QueryInterface(__uuidof(IDXGIDevice), (void**)&dxgiDevice);
	if( FAILED(hr) ) { ErrorToLog(hr); return false; }
	hr = dxgiDevice->GetAdapter(&dxgiAdapter);
	if( FAILED(hr) ) { dxgiDevice->Release(); ErrorToLog(hr); return false; }
	hr = dxgiAdapter->GetParent(__uuidof(IDXGIFactory2), (void**)&dxgiFactory);
	dxgiDevice->Release();
	if( FAILED(hr) ) { dxgiAdapter->Release(); ErrorToLog(hr); return false; }

	DXGI_SWAP_CHAIN_DESC1 scd; ZeroMemory(&scd, sizeof(scd));
	scd.Width  = w;
	scd.Height = h;
	scd.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
	scd.SampleDesc.Count = 1;
	scd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
	scd.BufferCount = 2;
	scd.SwapEffect  = DXGI_SWAP_EFFECT_FLIP_DISCARD;
	scd.Scaling     = DXGI_SCALING_STRETCH;
	scd.AlphaMode   = DXGI_ALPHA_MODE_IGNORE;

	hr = dxgiFactory->CreateSwapChainForHwnd(D3DDevice, TargetWindow, &scd, NULL, NULL, &SwapChain);
	if( FAILED(hr) ) {
		// FLIP_DISCARD 非対応環境向けフォールバック
		scd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL;
		hr = dxgiFactory->CreateSwapChainForHwnd(D3DDevice, TargetWindow, &scd, NULL, NULL, &SwapChain);
	}
	if( SUCCEEDED(hr) ) {
		// DXGI の Alt+Enter 排他フルスクリーン切替を無効化 (エンジンが管理する)
		dxgiFactory->MakeWindowAssociation(TargetWindow, DXGI_MWA_NO_ALT_ENTER);
	}
	dxgiFactory->Release();
	dxgiAdapter->Release();
	if( FAILED(hr) ) { ErrorToLog(hr); return false; }

	SwapWidth = w;
	SwapHeight = h;

	// WaitForVBlank 用の output を取得 (失敗は許容)
	TVPSafeRelease(DXGIOutput);
	SwapChain->GetContainingOutput(&DXGIOutput);

	return EnsureBackBufferRTV();
}
//---------------------------------------------------------------------------
bool tTVPBasicDrawDevice::ResizeSwapChain( UINT w, UINT h )
{
	if( !SwapChain ) return false;
	if( w < 1 ) w = 1;
	if( h < 1 ) h = 1;
	if( w == SwapWidth && h == SwapHeight ) return true;

	// RTV を外してからリサイズ
	if( D3DContext ) {
		ID3D11RenderTargetView* nullrtv[1] = { NULL };
		D3DContext->OMSetRenderTargets(1, nullrtv, NULL);
	}
	TVPSafeRelease(BackBufferRTV);
	TVPSafeRelease(DXGIOutput);

	HRESULT hr = SwapChain->ResizeBuffers(0, w, h, DXGI_FORMAT_UNKNOWN, 0);
	if( FAILED(hr) ) {
		if( hr == DXGI_ERROR_DEVICE_REMOVED || hr == DXGI_ERROR_DEVICE_RESET ) {
			HandleDeviceLost();
			return false;
		}
		ErrorToLog(hr);
		return false;
	}
	SwapWidth = w;
	SwapHeight = h;
	SwapChain->GetContainingOutput(&DXGIOutput);
	return EnsureBackBufferRTV();
}
//---------------------------------------------------------------------------
bool tTVPBasicDrawDevice::CreateD3DDevice()
{
	DestroyD3DDevice();
	if( !TargetWindow ) return false;

	// VIDEO_SUPPORT は Track V-E の HW 動画 (IMFMediaEngine) がこのデバイスを共有して
	// HW デコードするために必要 (純加算的な capability。非 HW 描画には無影響)。
	UINT flags = D3D11_CREATE_DEVICE_BGRA_SUPPORT | D3D11_CREATE_DEVICE_VIDEO_SUPPORT;
#ifdef _DEBUG
	// デバッグレイヤは環境に SDK レイヤが無いと失敗するので任意
#endif
	const D3D_FEATURE_LEVEL levels[] = {
		D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0,
		D3D_FEATURE_LEVEL_10_1, D3D_FEATURE_LEVEL_10_0,
	};
	D3D_FEATURE_LEVEL got = D3D_FEATURE_LEVEL_11_0;
	HRESULT hr = D3D11CreateDevice( NULL, D3D_DRIVER_TYPE_HARDWARE, NULL, flags,
		levels, (UINT)(sizeof(levels)/sizeof(levels[0])), D3D11_SDK_VERSION,
		&D3DDevice, &got, &D3DContext );
	if( FAILED(hr) ) {
		// WARP ソフトウェアラスタライザにフォールバック
		hr = D3D11CreateDevice( NULL, D3D_DRIVER_TYPE_WARP, NULL, flags,
			levels, (UINT)(sizeof(levels)/sizeof(levels[0])), D3D11_SDK_VERSION,
			&D3DDevice, &got, &D3DContext );
	}
	if( FAILED(hr) ) {
		if( IsTargetWindowActive() ) ErrorToLog(hr);
		DestroyD3DDevice();
		return false;
	}

	// Track V-E: HW 動画 (IMFMediaEngine) のデコードスレッドがこのデバイス/コンテキストを
	// 共有して触るため、マルチスレッド保護を有効化する (ImmediateContext 呼び出しを直列化)。
	{
		ID3D11Multithread* mt = NULL;
		if( SUCCEEDED( D3DContext->QueryInterface( __uuidof(ID3D11Multithread), (void**)&mt ) ) && mt ) {
			mt->SetMultithreadProtected( TRUE );
			mt->Release();
		}
	}

	if( !CreatePresentPipeline() ) { DestroyD3DDevice(); return false; }

	UINT cw = 1, ch = 1;
	GetClientSize(cw, ch);
	if( !CreateSwapChain(cw, ch) ) { DestroyD3DDevice(); return false; }

	// リフレッシュレート概算 (GDI)
	HDC hdc = ::GetDC(TargetWindow);
	int refreshrate = hdc ? ::GetDeviceCaps(hdc, VREFRESH) : 60;
	if(hdc) ::ReleaseDC(TargetWindow, hdc);
	if( refreshrate <= 0 ) refreshrate = 60;
	VsyncInterval = 1000 / refreshrate;

	return true;
}
//---------------------------------------------------------------------------
bool tTVPBasicDrawDevice::CreateTexture()
{
	DestroyTexture();
	if( !D3DDevice ) return false;
	tjs_int w, h;
	GetSrcSize( w, h );
	if( !(TargetWindow && w > 0 && h > 0) ) return true; // 何もしないが失敗ではない

	TextureWidth  = w;
	TextureHeight = h;
	TexturePitch  = (long)w * 4;

	// 永続 CPU シャドウバッファ (差分更新の内容保持用)
	TextureBuffer = malloc((size_t)TexturePitch * h);
	if( !TextureBuffer ) { TextureWidth = TextureHeight = 0; TexturePitch = 0; return false; }
	memset(TextureBuffer, 0, (size_t)TexturePitch * h);

	// DEFAULT テクスチャ (UpdateSubresource で dirty 矩形のみ部分更新 → 内容保持)
	D3D11_TEXTURE2D_DESC td; ZeroMemory(&td, sizeof(td));
	td.Width = w;
	td.Height = h;
	td.MipLevels = 1;
	td.ArraySize = 1;
	td.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
	td.SampleDesc.Count = 1;
	td.Usage = D3D11_USAGE_DEFAULT;
	td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
	td.CPUAccessFlags = 0;

	HRESULT hr = D3DDevice->CreateTexture2D(&td, NULL, &Texture);
	if( FAILED(hr) ) {
		if( IsTargetWindowActive() ) ErrorToLog(hr);
		DestroyTexture();
		return false;
	}
	hr = D3DDevice->CreateShaderResourceView(Texture, NULL, &TextureSRV);
	if( FAILED(hr) ) { ErrorToLog(hr); DestroyTexture(); return false; }
	return true;
}
//---------------------------------------------------------------------------
void tTVPBasicDrawDevice::EnsureDevice()
{
	TVPInitBasicDrawDeviceOptions();
	if( !TargetWindow ) return;
	try {
		bool recreate = false;
		if( D3DDevice == NULL ) {
			if( CreateD3DDevice() == false ) return;
			recreate = true;
		} else {
			// クライアントサイズが変わっていれば swapchain をリサイズ
			UINT cw, ch;
			if( GetClientSize(cw, ch) && (cw != SwapWidth || ch != SwapHeight) ) {
				ResizeSwapChain(cw, ch);
			}
		}
		if( Texture == NULL ) {
			if( CreateTexture() == false ) return;
			recreate = true;
		}
		if( recreate ) InvalidateAll();
	} catch(const eTJS & e) {
		TVPAddImportantLog( ttstr(TJS_W("(info) BasicDrawDevice(D3D11) device creation failed: ")) + e.GetMessage() );
		DestroyD3DDevice();
	} catch(...) {
		TVPAddImportantLog( ttstr(TJS_W("(info) BasicDrawDevice(D3D11) device creation failed (unknown)")) );
		DestroyD3DDevice();
	}
}
//---------------------------------------------------------------------------
void tTVPBasicDrawDevice::HandleDeviceLost()
{
	// D3D11 は原則ロストしないが、TDR / ドライバ更新で DEVICE_REMOVED があり得る。
	// すべて破棄し、次の EnsureDevice で作り直す。
	DestroyD3DDevice();
	InvalidateAll();
}
//---------------------------------------------------------------------------
void TJS_INTF_METHOD tTVPBasicDrawDevice::AddLayerManager(iTVPLayerManager * manager)
{
	if(inherited::Managers.size() > 0)
	{
		// "Basic" デバイスでは２つ以上のLayer Managerを登録できない
		TVPThrowExceptionMessage(TVPBasicDrawDeviceDoesNotSupporteLayerManagerMoreThanOne);
	}
	inherited::AddLayerManager(manager);

	manager->SetDesiredLayerType(ltOpaque); // ltOpaque な出力を受け取りたい
}
//---------------------------------------------------------------------------
void TJS_INTF_METHOD tTVPBasicDrawDevice::SetTargetWindow(HWND wnd, bool is_main)
{
	TVPInitBasicDrawDeviceOptions();
	DestroyD3DDevice();
	TargetWindow = wnd;
	IsMainWindow = is_main;
}
//---------------------------------------------------------------------------
void TJS_INTF_METHOD tTVPBasicDrawDevice::SetDestRectangle(const tTVPRect & rect)
{
	inherited::SetDestRectangle(rect);
	try {
		EnsureDevice();
	} catch(const eTJS & e) {
		TVPAddImportantLog( ttstr(TJS_W("(info) BasicDrawDevice(D3D11) SetDestRectangle failed: ")) + e.GetMessage() );
		DestroyD3DDevice();
	} catch(...) {
		DestroyD3DDevice();
	}
}
//---------------------------------------------------------------------------
void TJS_INTF_METHOD tTVPBasicDrawDevice::NotifyLayerResize(iTVPLayerManager * manager)
{
	inherited::NotifyLayerResize(manager);
	// 元画像サイズが変わったのでテクスチャ(+シャドウ)を作り直す
	CreateTexture();
}
//---------------------------------------------------------------------------
void TJS_INTF_METHOD tTVPBasicDrawDevice::Show()
{
	if(!TargetWindow) return;
	if(!SwapChain) return;

	// 動画 presenter 稼働中は layer 更新が無くても毎フレーム present する
	// (動画はデコードスレッドで layer 更新と非同期に進むため)。
	bool videoActive = HasActiveVideoPresenter();

#ifdef KRKRZ_USE_REPL
	// 動画 presenter 非稼働時: キャプチャ要求は持続 CPU シャドウ (TextureBuffer) から
	// 充足する。ShouldShow の早期 return より前に処理すればアイドルでも消化できる。
	// 稼働時は動画が CPU シャドウに載らないので、描画直後にバックバッファから読み戻す (下)。
	if( !videoActive && TVPHasPendingScreenCapture() ) FulfillScreenCapture();
#endif

	if(!ShouldShow && !videoActive) return;

	ShouldShow = false;

	if( videoActive ) {
		// SDL 版 DrawDevice と同様、presenter 登録中は本体 (レイヤ) 描画は不要
		// (動画が全画面を覆う前提)。RenderVideoPresenters が RTV を黒クリアしてから
		// 各 presenter (動画 + mixer 追加画像) を描く。
		RenderVideoPresenters();
	}

	// Layer 合成完了直後・Present 直前に Elements ダイアログをオーバーレイ
	PresentDialogOverlay();

#ifdef KRKRZ_USE_REPL
	// 動画 presenter 稼働時: 描画後・Present 前にバックバッファを読み戻してキャプチャ。
	if( videoActive && TVPHasPendingScreenCapture() ) FulfillScreenCaptureFromBackBuffer();
#endif

	// flip model のベストプラクティス: Present 前に RTV を外す
	if( D3DContext ) {
		ID3D11RenderTargetView* nullrtv[1] = { NULL };
		D3DContext->OMSetRenderTargets(1, nullrtv, NULL);
	}

	// VSyncTimingThread がタイミングを制御するので即時 Present (SyncInterval=0)
	HRESULT hr = SwapChain->Present(0, 0);

	if( hr == DXGI_ERROR_DEVICE_REMOVED || hr == DXGI_ERROR_DEVICE_RESET ) {
		if( IsTargetWindowActive() ) ErrorToLog(hr);
		HandleDeviceLost();
	} else if( FAILED(hr) ) {
		ErrorToLog(hr);
	}
}
//---------------------------------------------------------------------------
#ifdef KRKRZ_USE_REPL
void tTVPBasicDrawDevice::FulfillScreenCapture()
{
	tTVPScreenCaptureReq req;
	if( !TVPTakeScreenCaptureRequest(req) ) return;

	// TextureBuffer は最後に合成したフレームを保持する持続 CPU シャドウ
	// (トップダウン・32bpp・メモリ上 BGRA・行バイト数 TexturePitch)。
	// TVPSaveCapturedImage が期待する ARGB8888(メモリ BGRA) と一致するので、
	// GPU 読み戻し無しで直接保存できる (present も不要、メインスレッドで安全)。
	if( !TextureBuffer || TextureWidth == 0 || TextureHeight == 0 ) {
		TVPSetScreenCaptureResult(req.path, 0, 0, false);
		TVPAddImportantLog(ttstr(TJS_W("ScreenCapture: no composited frame yet: ")) + req.path);
		return;
	}

	int fullw = (int)TextureWidth, fullh = (int)TextureHeight;
	int cx = req.x, cy = req.y, cw = req.w, ch = req.h;
	if( cw <= 0 || ch <= 0 ) { cx = 0; cy = 0; cw = fullw; ch = fullh; }
	if( cx < 0 ) cx = 0;
	if( cy < 0 ) cy = 0;
	if( cx > fullw ) cx = fullw;
	if( cy > fullh ) cy = fullh;
	if( cx + cw > fullw ) cw = fullw - cx;
	if( cy + ch > fullh ) ch = fullh - cy;

	bool ok = false;
	if( cw > 0 && ch > 0 ) {
		const BYTE* base = (const BYTE*)TextureBuffer
			+ (size_t)cy * (size_t)TexturePitch + (size_t)cx * 4;
		ok = TVPSaveCapturedImage(req.path, base, cw, ch, (int)TexturePitch, ttstr(TJS_W("png")));
	}

	TVPSetScreenCaptureResult(req.path, ok ? cw : 0, ok ? ch : 0, ok);
	if( ok ) TVPAddLog(ttstr(TJS_W("ScreenCapture: saved ")) + req.path);
	else     TVPAddImportantLog(ttstr(TJS_W("ScreenCapture: failed ")) + req.path);
}
//---------------------------------------------------------------------------
void tTVPBasicDrawDevice::FulfillScreenCaptureFromBackBuffer()
{
	tTVPScreenCaptureReq req;
	if( !TVPTakeScreenCaptureRequest(req) ) return;

	bool ok = false; int outw = 0, outh = 0;
	ID3D11Texture2D *back = NULL, *stg = NULL;
	do {
		if( !D3DDevice || !D3DContext || !SwapChain ) break;
		if( FAILED(SwapChain->GetBuffer(0, __uuidof(ID3D11Texture2D), (void**)&back)) || !back ) break;

		D3D11_TEXTURE2D_DESC bd; back->GetDesc(&bd);
		D3D11_TEXTURE2D_DESC sd = bd;
		sd.Usage = D3D11_USAGE_STAGING;
		sd.BindFlags = 0;
		sd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
		sd.MiscFlags = 0;
		if( FAILED(D3DDevice->CreateTexture2D(&sd, NULL, &stg)) || !stg ) break;

		D3DContext->CopyResource(stg, back);

		D3D11_MAPPED_SUBRESOURCE m;
		if( SUCCEEDED(D3DContext->Map(stg, 0, D3D11_MAP_READ, 0, &m)) ) {
			int fullw = (int)bd.Width, fullh = (int)bd.Height;
			int cx = req.x, cy = req.y, cw = req.w, ch = req.h;
			if( cw <= 0 || ch <= 0 ) { cx = 0; cy = 0; cw = fullw; ch = fullh; }
			if( cx < 0 ) cx = 0;
			if( cy < 0 ) cy = 0;
			if( cx > fullw ) cx = fullw;
			if( cy > fullh ) cy = fullh;
			if( cx + cw > fullw ) cw = fullw - cx;
			if( cy + ch > fullh ) ch = fullh - cy;
			if( cw > 0 && ch > 0 ) {
				// swapchain は B8G8R8A8_UNORM (メモリ上 BGRA・top-down) = 保存が期待する形式
				const BYTE* base = (const BYTE*)m.pData + (size_t)cy * m.RowPitch + (size_t)cx * 4;
				ok = TVPSaveCapturedImage(req.path, base, cw, ch, (int)m.RowPitch, ttstr(TJS_W("png")));
				outw = cw; outh = ch;
			}
			D3DContext->Unmap(stg, 0);
		}
	} while(0);
	if( stg )  stg->Release();
	if( back ) back->Release();

	TVPSetScreenCaptureResult(req.path, ok ? outw : 0, ok ? outh : 0, ok);
	if( ok ) TVPAddLog(ttstr(TJS_W("ScreenCapture(backbuffer): saved ")) + req.path);
	else     TVPAddImportantLog(ttstr(TJS_W("ScreenCapture(backbuffer): failed ")) + req.path);
}
#endif // KRKRZ_USE_REPL
//---------------------------------------------------------------------------
bool TJS_INTF_METHOD tTVPBasicDrawDevice::WaitForVBlank( tjs_int* in_vblank, tjs_int* delayed )
{
	if( !SwapChain ) return false;
	if( !DXGIOutput ) {
		// swapchain から取り直しを試みる
		SwapChain->GetContainingOutput(&DXGIOutput);
		if( !DXGIOutput ) return false;
	}

	// DXGI の WaitForVBlank は次の VBlank までブロックする。
	// 戻った直後は VBlank に入った状態なので in_vblank=1。
	HRESULT hr = DXGIOutput->WaitForVBlank();
	if( FAILED(hr) ) {
		TVPSafeRelease(DXGIOutput);
		*in_vblank = 0;
		*delayed = 0;
		return false;
	}
	*in_vblank = 1;
	*delayed = 0;
	return true;
}
//---------------------------------------------------------------------------
void TJS_INTF_METHOD tTVPBasicDrawDevice::StartBitmapCompletion(iTVPLayerManager * manager)
{
	EnsureDevice();
	// シャドウバッファ (TextureBuffer) は CreateTexture で確保済み。
	// 差分更新の受け皿として毎回同じバッファを使い、EndBitmapCompletion で
	// 変化した矩形のみ GPU テクスチャへ転送する。
	// (D3D11 DYNAMIC の Map(WRITE_DISCARD) は全破棄で差分更新に使えないため
	//  永続 CPU シャドウ + DEFAULT テクスチャの UpdateSubresource 方式を採る)
}
//---------------------------------------------------------------------------
void TJS_INTF_METHOD tTVPBasicDrawDevice::NotifyBitmapCompleted(iTVPLayerManager * manager,
	tjs_int x, tjs_int y, const void * bits, const BITMAPINFO * bmpinfo,
	const tTVPRect &cliprect, tTVPLayerType type, tjs_int opacity)
{
	int _width  = bmpinfo->bmiHeader.biWidth;
	int _height = bmpinfo->bmiHeader.biHeight;
	int _pitch  = bmpinfo->bmiHeader.biSizeImage / (_height < 0 ? -_height : _height);

	// bits, bitmapinfo で表されるビットマップの cliprect の領域を、x, y に描画する。
	// opacity と type は無視する (ltOpaque 前提)。
	tjs_int w, h;
	GetSrcSize( w, h );
	if( TextureBuffer && TargetWindow &&
		!(x < 0 || y < 0 ||
			x + cliprect.get_width() > w ||
			y + cliprect.get_height() > h) &&
		!(cliprect.left < 0 || cliprect.top < 0 ||
			cliprect.right > _width ||
			cliprect.bottom > (_height < 0 ? -_height : _height)))
	{
		// 範囲外の転送は無視してよい
		ShouldShow = true;

		long src_y       = cliprect.top;
		long src_y_limit = cliprect.bottom;
		long src_x       = cliprect.left;
		long width_bytes = cliprect.get_width() * 4; // 32bit
		long dest_y      = y;
		long dest_x      = x;
		const tjs_uint8 * src_p = (const tjs_uint8 *)bits;
		long src_pitch;

		if(_height < 0)
		{
			// bottom-down
			src_pitch = _pitch;
		}
		else
		{
			// bottom-up
			src_pitch = - _pitch;
			src_p += (long)_pitch * (_height - 1);
		}

		for(; src_y < src_y_limit; src_y ++, dest_y ++)
		{
			const void *srcp = src_p + src_pitch * src_y + src_x * 4;
			void *destp = (tjs_uint8*)TextureBuffer + TexturePitch * dest_y + dest_x * 4;
			memcpy(destp, srcp, width_bytes);
		}
	}
}
//---------------------------------------------------------------------------
void TJS_INTF_METHOD tTVPBasicDrawDevice::EndBitmapCompletion(iTVPLayerManager * manager)
{
	// 合成済みフレームをバックバッファへ描画。動画 presenter 稼働中は Show() 側でも
	// 毎フレーム再描画するため、この描画本体は DrawCompositedFrame に切り出してある。
	DrawCompositedFrame();
}
//---------------------------------------------------------------------------
void tTVPBasicDrawDevice::DrawCompositedFrame()
{
	if(!TargetWindow) return;
	if(!D3DDevice || !D3DContext) return;
	if(!Texture || !TextureSRV) return;
	if(!SwapChain || !BackBufferRTV) return;
	if(!TextureBuffer) return;

	// シャドウバッファ全体を GPU テクスチャへ反映 (DEFAULT テクスチャなので内容は保持)
	// NOTE: 差分矩形の union だけを転送する最適化余地あり。まずは全面転送で正確性優先。
	D3DContext->UpdateSubresource(Texture, 0, NULL, TextureBuffer, (UINT)TexturePitch, 0);

	// 転送先をクリッピング矩形に基づきクリッピング (クライアント=swapchain 座標)
	float dl = (float)( DestRect.left   < ClipRect.left   ? ClipRect.left   : DestRect.left );
	float dt = (float)( DestRect.top    < ClipRect.top    ? ClipRect.top    : DestRect.top );
	float dr = (float)( DestRect.right  > ClipRect.right  ? ClipRect.right  : DestRect.right );
	float db = (float)( DestRect.bottom > ClipRect.bottom ? ClipRect.bottom : DestRect.bottom );
	float dw = (float)DestRect.get_width();
	float dh = (float)DestRect.get_height();
	if( dw <= 0 || dh <= 0 ) return;

	// はみ出している幅
	float cl = dl - (float)DestRect.left;
	float ct = dt - (float)DestRect.top;
	float cr = (float)DestRect.right  - dr;
	float cb = (float)DestRect.bottom - db;

	// はみ出しを考慮した転送元テクスチャ座標 (0..1)
	tjs_int w, h;
	GetSrcSize( w, h );
	float sl = (float)(cl * w / dw) / (float)TextureWidth;
	float st = (float)(ct * h / dh) / (float)TextureHeight;
	float sr = (float)(w - (cr * w / dw)) / (float)TextureWidth;
	float sb = (float)(h - (cb * h / dh)) / (float)TextureHeight;

	// dest ピクセル座標 → NDC ( x: -1..1, y: 1..-1 )
	float sw = (float)(SwapWidth  ? SwapWidth  : 1);
	float sh = (float)(SwapHeight ? SwapHeight : 1);
	float nl = dl * 2.0f / sw - 1.0f;
	float nr = dr * 2.0f / sw - 1.0f;
	float nt = 1.0f - dt * 2.0f / sh;
	float nb = 1.0f - db * 2.0f / sh;

	// TRIANGLESTRIP の 4 頂点
	tTVPBDDVertex verts[4] = {
		{ nl, nt, sl, st },
		{ nr, nt, sr, st },
		{ nl, nb, sl, sb },
		{ nr, nb, sr, sb },
	};

	D3D11_MAPPED_SUBRESOURCE mapped;
	if( SUCCEEDED(D3DContext->Map(VertexBuffer, 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped)) ) {
		memcpy(mapped.pData, verts, sizeof(verts));
		D3DContext->Unmap(VertexBuffer, 0);
	} else {
		return;
	}

	// -- 描画
	D3D11_VIEWPORT vp; ZeroMemory(&vp, sizeof(vp));
	vp.Width = sw; vp.Height = sh; vp.MinDepth = 0.0f; vp.MaxDepth = 1.0f;
	D3DContext->RSSetViewports(1, &vp);

	ID3D11RenderTargetView* rtvs[1] = { BackBufferRTV };
	D3DContext->OMSetRenderTargets(1, rtvs, NULL);

	const float black[4] = { 0.0f, 0.0f, 0.0f, 1.0f };
	D3DContext->ClearRenderTargetView(BackBufferRTV, black);

	UINT stride = sizeof(tTVPBDDVertex), offset = 0;
	D3DContext->IASetInputLayout(InputLayout);
	D3DContext->IASetVertexBuffers(0, 1, &VertexBuffer, &stride, &offset);
	D3DContext->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);
	D3DContext->VSSetShader(VertexShader, NULL, 0);
	D3DContext->PSSetShader(PixelShader, NULL, 0);
	D3DContext->PSSetShaderResources(0, 1, &TextureSRV);
	ID3D11SamplerState* samp = TVPZoomInterpolation ? SamplerLinear : SamplerPoint;
	D3DContext->PSSetSamplers(0, 1, &samp);
	D3DContext->Draw(4, 0);

	// SRV を外しておく (次フレーム UpdateSubresource 時の hazard 回避)
	ID3D11ShaderResourceView* nullsrv[1] = { NULL };
	D3DContext->PSSetShaderResources(0, 1, nullsrv);
}
//---------------------------------------------------------------------------
// Track V-E: overlay 動画 presenter (pull 型)
//---------------------------------------------------------------------------
void TJS_INTF_METHOD tTVPBasicDrawDevice::AddVideoPresenter( iTVPVideoPresenter * presenter )
{
	// 単一スロット (最後に登録した 1 つを保持)。
	if( !presenter ) return;
	VideoPresenter = presenter;
}
//---------------------------------------------------------------------------
void TJS_INTF_METHOD tTVPBasicDrawDevice::RemoveVideoPresenter( iTVPVideoPresenter * presenter )
{
	// 現在のスロットが自分の時だけクリア (別の presenter に置き換わっていればスルー)。
	if( VideoPresenter == presenter ) VideoPresenter = NULL;
}
//---------------------------------------------------------------------------
void tTVPBasicDrawDevice::RenderVideoPresenters()
{
	if( !VideoPresenter ) return;
	if( !D3DDevice || !D3DContext || !BackBufferRTV ) return;

	tTVPVideoPresenterContext ctx;
	ctx.Device       = D3DDevice;
	ctx.Context      = D3DContext;
	ctx.RenderTarget = BackBufferRTV;
	ctx.TargetWidth  = SwapWidth;
	ctx.TargetHeight = SwapHeight;
	ctx.DestRect     = DestRect;
	ctx.ClipRect     = ClipRect;
	tjs_int sw = 0, sh = 0;
	GetSrcSize( sw, sh );
	ctx.SrcWidth  = sw;
	ctx.SrcHeight = sh;

	// RTV を束ねて黒でクリア (動画が全画面を覆う前提。レターボックス余白は黒)。
	ID3D11RenderTargetView* rtvs[1] = { BackBufferRTV };
	D3DContext->OMSetRenderTargets(1, rtvs, NULL);
	const float black[4] = { 0.0f, 0.0f, 0.0f, 1.0f };
	D3DContext->ClearRenderTargetView(BackBufferRTV, black);

	// ビューポートをバックバッファ全面に (DrawCompositedFrame を経ない present 経路のため)
	D3D11_VIEWPORT vp; ZeroMemory(&vp, sizeof(vp));
	vp.Width = (float)(SwapWidth ? SwapWidth : 1);
	vp.Height = (float)(SwapHeight ? SwapHeight : 1);
	vp.MinDepth = 0.0f; vp.MaxDepth = 1.0f;
	D3DContext->RSSetViewports(1, &vp);

	VideoPresenter->RenderVideoFrame( ctx );
}
//---------------------------------------------------------------------------
void TJS_INTF_METHOD tTVPBasicDrawDevice::SetShowUpdateRect(bool b)
{
	DrawUpdateRectangle = b;
}
//---------------------------------------------------------------------------
bool TJS_INTF_METHOD tTVPBasicDrawDevice::SwitchToFullScreen( HWND window, tjs_uint w, tjs_uint h, tjs_uint bpp, tjs_uint color, bool changeresolution )
{
	// フルスクリーン化は行わない (互換のためウィンドウを全画面化する運用)。
	// DXGI 排他フルスクリーンはモーダルウィンドウと相性が悪いため常にウィンドウモード。
	// swapchain はクライアントサイズ追従なので、次の描画で EnsureDevice がリサイズする。
	ShouldShow = true;
	return true;
}
//---------------------------------------------------------------------------
void TJS_INTF_METHOD tTVPBasicDrawDevice::RevertFromFullScreen( HWND window, tjs_uint w, tjs_uint h, tjs_uint bpp, tjs_uint color )
{
	ShouldShow = true;
}
//---------------------------------------------------------------------------




//---------------------------------------------------------------------------
// tTJSNI_BasicDrawDevice : BasicDrawDevice TJS native class
//---------------------------------------------------------------------------
tjs_uint32 tTJSNC_BasicDrawDevice::ClassID = (tjs_uint32)-1;
tTJSNC_BasicDrawDevice::tTJSNC_BasicDrawDevice() :
	tTJSNativeClass(TJS_W("BasicDrawDevice"))
{
	// register native methods/properties

	TJS_BEGIN_NATIVE_MEMBERS(BasicDrawDevice)
	TJS_DECL_EMPTY_FINALIZE_METHOD
//----------------------------------------------------------------------
// constructor/methods
//----------------------------------------------------------------------
TJS_BEGIN_NATIVE_CONSTRUCTOR_DECL(/*var.name*/_this, /*var.type*/tTJSNI_BasicDrawDevice,
	/*TJS class name*/BasicDrawDevice)
{
	return TJS_S_OK;
}
TJS_END_NATIVE_CONSTRUCTOR_DECL(/*TJS class name*/BasicDrawDevice)
//----------------------------------------------------------------------
TJS_BEGIN_NATIVE_METHOD_DECL(/*func. name*/recreate)
{
	TJS_GET_NATIVE_INSTANCE(/*var. name*/_this, /*var. type*/tTJSNI_BasicDrawDevice);
	_this->GetDevice()->SetToRecreateDrawer();
	_this->GetDevice()->EnsureDevice();
	return TJS_S_OK;
}
TJS_END_NATIVE_METHOD_DECL(/*func. name*/recreate)
//----------------------------------------------------------------------


//---------------------------------------------------------------------------
//----------------------------------------------------------------------
// properties
//----------------------------------------------------------------------
TJS_BEGIN_NATIVE_PROP_DECL(interface)
{
	TJS_BEGIN_NATIVE_PROP_GETTER
	{
		TJS_GET_NATIVE_INSTANCE(/*var. name*/_this, /*var. type*/tTJSNI_BasicDrawDevice);
		*result = reinterpret_cast<tjs_int64>(_this->GetDevice());
		return TJS_S_OK;
	}
	TJS_END_NATIVE_PROP_GETTER

	TJS_DENY_NATIVE_PROP_SETTER
}
TJS_END_NATIVE_PROP_DECL(interface)
//----------------------------------------------------------------------
// Track V-E: overlay 動画 presenter の登録口 (iTVPVideoPresenterHost) をポインタ値
// として公開する。VideoOverlay はこのプロパティを Window の DrawDevice TJS オブジェクト
// から読み、非 0 なら presenter を登録して pull 合成に載る (0/未定義なら子ウィンドウ
// present にフォールバック)。static_cast で多重継承のポインタ調整を済ませてから渡す。
TJS_BEGIN_NATIVE_PROP_DECL(videoPresenterHost)
{
	TJS_BEGIN_NATIVE_PROP_GETTER
	{
		TJS_GET_NATIVE_INSTANCE(/*var. name*/_this, /*var. type*/tTJSNI_BasicDrawDevice);
		iTVPVideoPresenterHost * host = static_cast<iTVPVideoPresenterHost*>(_this->GetDevice());
		*result = reinterpret_cast<tjs_int64>(host);
		return TJS_S_OK;
	}
	TJS_END_NATIVE_PROP_GETTER

	TJS_DENY_NATIVE_PROP_SETTER
}
TJS_END_NATIVE_PROP_DECL(videoPresenterHost)
//----------------------------------------------------------------------
// Track V-E: HW 動画 (IMFMediaEngine) が engine の D3D11 デバイスへ束ねて HW デコード
// するため、ID3D11Device ポインタを公開する (VIDEO_SUPPORT + multithread 保護済み)。
TJS_BEGIN_NATIVE_PROP_DECL(d3d11Device)
{
	TJS_BEGIN_NATIVE_PROP_GETTER
	{
		TJS_GET_NATIVE_INSTANCE(/*var. name*/_this, /*var. type*/tTJSNI_BasicDrawDevice);
		*result = reinterpret_cast<tjs_int64>( _this->GetDevice()->GetD3DDevice() );
		return TJS_S_OK;
	}
	TJS_END_NATIVE_PROP_GETTER

	TJS_DENY_NATIVE_PROP_SETTER
}
TJS_END_NATIVE_PROP_DECL(d3d11Device)
//----------------------------------------------------------------------
	TJS_END_NATIVE_MEMBERS
}
//---------------------------------------------------------------------------
iTJSNativeInstance *tTJSNC_BasicDrawDevice::CreateNativeInstance()
{
	return new tTJSNI_BasicDrawDevice();
}
//---------------------------------------------------------------------------




//---------------------------------------------------------------------------
tTJSNI_BasicDrawDevice::tTJSNI_BasicDrawDevice()
{
	Device = new tTVPBasicDrawDevice();
}
//---------------------------------------------------------------------------
tTJSNI_BasicDrawDevice::~tTJSNI_BasicDrawDevice()
{
	if(Device) Device->Destruct(), Device = NULL;
}
//---------------------------------------------------------------------------
tjs_error TJS_INTF_METHOD
	tTJSNI_BasicDrawDevice::Construct(tjs_int numparams, tTJSVariant **param,
		iTJSDispatch2 *tjs_obj)
{
	return TJS_S_OK;
}
//---------------------------------------------------------------------------
void TJS_INTF_METHOD tTJSNI_BasicDrawDevice::Invalidate()
{
	if(Device) Device->Destruct(), Device = NULL;
}
//---------------------------------------------------------------------------
