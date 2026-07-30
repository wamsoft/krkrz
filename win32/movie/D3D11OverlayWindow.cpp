/****************************************************************************/
/*! @file
@brief overlay 動画用の D3D11 YUV レンダラ 実装
*****************************************************************************/
#include <windows.h>
#include "D3D11OverlayWindow.h"
#include <dxgi.h>
#include <d3dcompiler.h>
#include <cstdint>
#include <cstdio>

#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "d3dcompiler.lib")

ATOM tTVPD3D11OverlayWindow::ClassAtom = 0;

namespace {
template<class T> void SafeRel( T*& p ) { if( p ) { p->Release(); p = nullptr; } }

// 頂点: NDC 位置(x,y) + テクスチャ座標(u,v)
struct Vertex { float x, y, u, v; };

// パススルー VS + I420→RGB(BT.601 limited range) PS
const char* g_Shader =
"Texture2D texY : register(t0);\n"
"Texture2D texU : register(t1);\n"
"Texture2D texV : register(t2);\n"
"SamplerState smp : register(s0);\n"
"struct VSIn  { float2 pos:POSITION; float2 uv:TEXCOORD; };\n"
"struct VSOut { float4 pos:SV_POSITION; float2 uv:TEXCOORD; };\n"
"VSOut VSMain(VSIn i){ VSOut o; o.pos=float4(i.pos,0,1); o.uv=i.uv; return o; }\n"
"float4 PSMain(VSOut i):SV_TARGET{\n"
"  float Y = texY.Sample(smp,i.uv).r;\n"
"  float U = texU.Sample(smp,i.uv).r;\n"
"  float V = texV.Sample(smp,i.uv).r;\n"
"  float y = (Y - 0.0627451) * 1.164383;\n"   // (Y-16/255)*1.164383
"  float u = U - 0.5019608;\n"                 // U-128/255
"  float v = V - 0.5019608;\n"
"  float r = y + 1.596027 * v;\n"
"  float g = y - 0.391762 * u - 0.812968 * v;\n"
"  float b = y + 2.017232 * u;\n"
"  return float4(saturate(r), saturate(g), saturate(b), 1.0);\n"
"}\n"
// packed BGRA パススルー PS (texY を color テクスチャとして t0 に流用)
"float4 PSRgba(VSOut i):SV_TARGET{\n"
"  return float4(texY.Sample(smp,i.uv).rgb, 1.0);\n"
"}\n";
} // namespace

//---------------------------------------------------------------------------
tTVPD3D11OverlayWindow::tTVPD3D11OverlayWindow()
: ChildWnd(NULL), OwnerWnd(NULL), MessageDrainWindow(NULL), Visible(false)
, Dev(nullptr), Ctx(nullptr), Swap(nullptr), RTV(nullptr), SwapW(0), SwapH(0)
, VS(nullptr), PS(nullptr), PSRgba(nullptr), IL(nullptr), VB(nullptr), Samp(nullptr)
, TexY(nullptr), TexU(nullptr), TexV(nullptr), SrvY(nullptr), SrvU(nullptr), SrvV(nullptr)
, PlaneW(0), PlaneH(0), TexBGRA(nullptr), SrvBGRA(nullptr), BgraW(0), BgraH(0), LastW(0), LastH(0)
{
	DesiredRect.left = DesiredRect.top = 0;
	DesiredRect.right = DesiredRect.bottom = 0;
}
//---------------------------------------------------------------------------
tTVPD3D11OverlayWindow::~tTVPD3D11OverlayWindow()
{
	Destroy();
}
//---------------------------------------------------------------------------
LRESULT CALLBACK tTVPD3D11OverlayWindow::WndProcThunk( HWND h, UINT m, WPARAM w, LPARAM l )
{
	tTVPD3D11OverlayWindow* self = (tTVPD3D11OverlayWindow*)GetWindowLongPtr( h, GWLP_USERDATA );
	if( self ) return self->WndProc( h, m, w, l );
	return DefWindowProc( h, m, w, l );
}
//---------------------------------------------------------------------------
LRESULT tTVPD3D11OverlayWindow::WndProc( HWND h, UINT m, WPARAM w, LPARAM l )
{
	// マウスは game window へ流す (overlay 上でもゲーム操作を止めない)
	if( m >= WM_MOUSEFIRST && m <= WM_MOUSELAST && MessageDrainWindow )
		return ::SendMessage( MessageDrainWindow, m, w, l );
	return DefWindowProc( h, m, w, l );
}
//---------------------------------------------------------------------------
bool tTVPD3D11OverlayWindow::Create( HWND owner )
{
	if( ChildWnd ) return true;
	if( !owner ) return false;
	OwnerWnd = owner;

	HINSTANCE hInst = ::GetModuleHandle( NULL );
	if( ClassAtom == 0 )
	{
		WNDCLASSEX wc; ZeroMemory( &wc, sizeof(wc) );
		wc.cbSize = sizeof(wc);
		wc.style = CS_HREDRAW | CS_VREDRAW;
		wc.lpfnWndProc = WndProcThunk;
		wc.hInstance = hInst;
		wc.hCursor = ::LoadCursor( NULL, IDC_ARROW );
		wc.lpszClassName = L"krmovie D3D11 Overlay Window";
		ClassAtom = ::RegisterClassEx( &wc );
		if( ClassAtom == 0 ) return false;
	}

	int x = DesiredRect.left, y = DesiredRect.top;
	int w = DesiredRect.right - DesiredRect.left;
	int h = DesiredRect.bottom - DesiredRect.top;
	if( w <= 0 || h <= 0 ) { x = 0; y = 0; w = 320; h = 240; }

	ChildWnd = ::CreateWindowEx( 0, MAKEINTATOM(ClassAtom), L"Video",
		WS_CHILD, x, y, w, h, OwnerWnd, NULL, hInst, NULL );
	if( !ChildWnd ) return false;
	::SetWindowLongPtr( ChildWnd, GWLP_USERDATA, (LONG_PTR)this );

	if( !CreateDevice() ) { Destroy(); return false; }
	if( !CreateSwapChain( (UINT)w, (UINT)h ) ) { Destroy(); return false; }
	if( !CreatePipeline() ) { Destroy(); return false; }
	return true;
}
//---------------------------------------------------------------------------
bool tTVPD3D11OverlayWindow::CreateDevice()
{
	UINT flags = 0;
	D3D_FEATURE_LEVEL fl;
	HRESULT hr = D3D11CreateDevice( NULL, D3D_DRIVER_TYPE_HARDWARE, NULL, flags,
		NULL, 0, D3D11_SDK_VERSION, &Dev, &fl, &Ctx );
	if( FAILED(hr) )
		hr = D3D11CreateDevice( NULL, D3D_DRIVER_TYPE_WARP, NULL, flags,
			NULL, 0, D3D11_SDK_VERSION, &Dev, &fl, &Ctx );
	return SUCCEEDED(hr);
}
//---------------------------------------------------------------------------
bool tTVPD3D11OverlayWindow::CreateSwapChain( UINT w, UINT h )
{
	SafeRel( RTV );
	SafeRel( Swap );
	if( !Dev ) return false;

	IDXGIDevice*  dxgiDev = nullptr;
	IDXGIAdapter* adapter = nullptr;
	IDXGIFactory* factory = nullptr;
	if( FAILED( Dev->QueryInterface( __uuidof(IDXGIDevice), (void**)&dxgiDev ) ) ) return false;
	dxgiDev->GetAdapter( &adapter );
	dxgiDev->Release();
	if( !adapter ) return false;
	adapter->GetParent( __uuidof(IDXGIFactory), (void**)&factory );
	adapter->Release();
	if( !factory ) return false;

	DXGI_SWAP_CHAIN_DESC scd; ZeroMemory( &scd, sizeof(scd) );
	scd.BufferCount = 2;
	scd.BufferDesc.Width = w;
	scd.BufferDesc.Height = h;
	scd.BufferDesc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
	scd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
	scd.OutputWindow = ChildWnd;
	scd.SampleDesc.Count = 1;
	scd.Windowed = TRUE;
	scd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
	HRESULT hr = factory->CreateSwapChain( Dev, &scd, &Swap );
	if( FAILED(hr) )
	{
		scd.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;
		scd.BufferCount = 1;
		hr = factory->CreateSwapChain( Dev, &scd, &Swap );
	}
	factory->Release();
	if( FAILED(hr) ) return false;
	SwapW = w; SwapH = h;
	return EnsureBackBufferRTV();
}
//---------------------------------------------------------------------------
bool tTVPD3D11OverlayWindow::EnsureBackBufferRTV()
{
	SafeRel( RTV );
	if( !Swap ) return false;
	ID3D11Texture2D* bb = nullptr;
	if( FAILED( Swap->GetBuffer( 0, __uuidof(ID3D11Texture2D), (void**)&bb ) ) ) return false;
	HRESULT hr = Dev->CreateRenderTargetView( bb, NULL, &RTV );
	bb->Release();
	return SUCCEEDED(hr);
}
//---------------------------------------------------------------------------
bool tTVPD3D11OverlayWindow::CreatePipeline()
{
	ID3DBlob* vsb = nullptr; ID3DBlob* psb = nullptr; ID3DBlob* err = nullptr;
	if( FAILED( D3DCompile( g_Shader, strlen(g_Shader), NULL, NULL, NULL,
		"VSMain", "vs_4_0", 0, 0, &vsb, &err ) ) ) { SafeRel(err); return false; }
	if( FAILED( D3DCompile( g_Shader, strlen(g_Shader), NULL, NULL, NULL,
		"PSMain", "ps_4_0", 0, 0, &psb, &err ) ) ) { SafeRel(err); SafeRel(vsb); return false; }

	bool ok = SUCCEEDED( Dev->CreateVertexShader( vsb->GetBufferPointer(), vsb->GetBufferSize(), NULL, &VS ) )
		&& SUCCEEDED( Dev->CreatePixelShader( psb->GetBufferPointer(), psb->GetBufferSize(), NULL, &PS ) );

	// packed BGRA パススルー PS
	ID3DBlob* psbRgba = nullptr;
	if( ok && SUCCEEDED( D3DCompile( g_Shader, strlen(g_Shader), NULL, NULL, NULL,
		"PSRgba", "ps_4_0", 0, 0, &psbRgba, &err ) ) )
		ok = SUCCEEDED( Dev->CreatePixelShader( psbRgba->GetBufferPointer(), psbRgba->GetBufferSize(), NULL, &PSRgba ) );
	else
		{ SafeRel(err); ok = false; }
	SafeRel( psbRgba );

	D3D11_INPUT_ELEMENT_DESC ied[] = {
		{ "POSITION", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 0,  D3D11_INPUT_PER_VERTEX_DATA, 0 },
		{ "TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 8,  D3D11_INPUT_PER_VERTEX_DATA, 0 },
	};
	if( ok ) ok = SUCCEEDED( Dev->CreateInputLayout( ied, 2, vsb->GetBufferPointer(), vsb->GetBufferSize(), &IL ) );
	SafeRel( vsb ); SafeRel( psb );
	if( !ok ) return false;

	// フルスクリーンquad (triangle strip)。uv は上下そのまま (映像は top-down で上げる)
	Vertex verts[4] = {
		{ -1.f, +1.f, 0.f, 0.f },
		{ +1.f, +1.f, 1.f, 0.f },
		{ -1.f, -1.f, 0.f, 1.f },
		{ +1.f, -1.f, 1.f, 1.f },
	};
	D3D11_BUFFER_DESC bd; ZeroMemory( &bd, sizeof(bd) );
	bd.ByteWidth = sizeof(verts);
	bd.Usage = D3D11_USAGE_IMMUTABLE;
	bd.BindFlags = D3D11_BIND_VERTEX_BUFFER;
	D3D11_SUBRESOURCE_DATA srd; ZeroMemory( &srd, sizeof(srd) ); srd.pSysMem = verts;
	if( FAILED( Dev->CreateBuffer( &bd, &srd, &VB ) ) ) return false;

	D3D11_SAMPLER_DESC sd; ZeroMemory( &sd, sizeof(sd) );
	sd.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
	sd.AddressU = sd.AddressV = sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
	sd.MaxLOD = D3D11_FLOAT32_MAX;
	if( FAILED( Dev->CreateSamplerState( &sd, &Samp ) ) ) return false;
	return true;
}
//---------------------------------------------------------------------------
static ID3D11Texture2D* CreatePlaneTex( ID3D11Device* dev, int w, int h, ID3D11ShaderResourceView** srv )
{
	D3D11_TEXTURE2D_DESC td; ZeroMemory( &td, sizeof(td) );
	td.Width = w; td.Height = h; td.MipLevels = 1; td.ArraySize = 1;
	td.Format = DXGI_FORMAT_R8_UNORM;
	td.SampleDesc.Count = 1;
	td.Usage = D3D11_USAGE_DYNAMIC;
	td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
	td.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
	ID3D11Texture2D* tex = nullptr;
	if( FAILED( dev->CreateTexture2D( &td, NULL, &tex ) ) ) return nullptr;
	if( srv && FAILED( dev->CreateShaderResourceView( tex, NULL, srv ) ) ) { tex->Release(); return nullptr; }
	return tex;
}
//---------------------------------------------------------------------------
bool tTVPD3D11OverlayWindow::EnsurePlaneTextures( int w, int h )
{
	if( TexY && PlaneW == w && PlaneH == h ) return true;
	ReleasePlaneTextures();
	TexY = CreatePlaneTex( Dev, w, h, &SrvY );
	TexU = CreatePlaneTex( Dev, w/2, h/2, &SrvU );
	TexV = CreatePlaneTex( Dev, w/2, h/2, &SrvV );
	if( !TexY || !TexU || !TexV ) { ReleasePlaneTextures(); return false; }
	PlaneW = w; PlaneH = h;
	return true;
}
//---------------------------------------------------------------------------
void tTVPD3D11OverlayWindow::ReleasePlaneTextures()
{
	SafeRel( SrvY ); SafeRel( SrvU ); SafeRel( SrvV );
	SafeRel( TexY ); SafeRel( TexU ); SafeRel( TexV );
	PlaneW = PlaneH = 0;
}
//---------------------------------------------------------------------------
bool tTVPD3D11OverlayWindow::EnsureBGRATexture( int w, int h )
{
	if( TexBGRA && BgraW == w && BgraH == h ) return true;
	ReleaseBGRATexture();
	D3D11_TEXTURE2D_DESC td; ZeroMemory( &td, sizeof(td) );
	td.Width = w; td.Height = h; td.MipLevels = 1; td.ArraySize = 1;
	td.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
	td.SampleDesc.Count = 1;
	td.Usage = D3D11_USAGE_DYNAMIC;
	td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
	td.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
	if( FAILED( Dev->CreateTexture2D( &td, NULL, &TexBGRA ) ) ) return false;
	if( FAILED( Dev->CreateShaderResourceView( TexBGRA, NULL, &SrvBGRA ) ) ) { ReleaseBGRATexture(); return false; }
	BgraW = w; BgraH = h;
	return true;
}
//---------------------------------------------------------------------------
void tTVPD3D11OverlayWindow::ReleaseBGRATexture()
{
	SafeRel( SrvBGRA ); SafeRel( TexBGRA );
	BgraW = BgraH = 0;
}
//---------------------------------------------------------------------------
void tTVPD3D11OverlayWindow::PresentBGRA( const uint8_t *data, int stride, int w, int h )
{
	std::lock_guard<std::mutex> lk( Mtx );
	if( !Dev || !Ctx || !Swap || !RTV ) return;
	if( w <= 0 || h <= 0 || !data ) return;
	if( !EnsureBGRATexture( w, h ) ) return;

	// アップロード (行ごと。src stride と dst RowPitch が異なりうる)
	D3D11_MAPPED_SUBRESOURCE m;
	if( SUCCEEDED( Ctx->Map( TexBGRA, 0, D3D11_MAP_WRITE_DISCARD, 0, &m ) ) )
	{
		uint8_t* dst = (uint8_t*)m.pData;
		int rowBytes = w * 4;
		for( int y = 0; y < h; y++ )
			memcpy( dst + (size_t)y * m.RowPitch, data + (size_t)y * stride, rowBytes );
		Ctx->Unmap( TexBGRA, 0 );
	}
	LastW = w; LastH = h;

	D3D11_VIEWPORT vp; ZeroMemory( &vp, sizeof(vp) );
	vp.Width = (float)SwapW; vp.Height = (float)SwapH; vp.MaxDepth = 1.f;
	Ctx->RSSetViewports( 1, &vp );
	ID3D11RenderTargetView* rtvs[1] = { RTV };
	Ctx->OMSetRenderTargets( 1, rtvs, NULL );
	const float black[4] = { 0, 0, 0, 1 };
	Ctx->ClearRenderTargetView( RTV, black );

	UINT stride2 = sizeof(Vertex), offset = 0;
	Ctx->IASetInputLayout( IL );
	Ctx->IASetVertexBuffers( 0, 1, &VB, &stride2, &offset );
	Ctx->IASetPrimitiveTopology( D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP );
	Ctx->VSSetShader( VS, NULL, 0 );
	Ctx->PSSetShader( PSRgba, NULL, 0 );
	ID3D11ShaderResourceView* srvs[1] = { SrvBGRA };
	Ctx->PSSetShaderResources( 0, 1, srvs );
	Ctx->PSSetSamplers( 0, 1, &Samp );
	Ctx->Draw( 4, 0 );

	ID3D11RenderTargetView* nullrtv[1] = { NULL };
	Ctx->OMSetRenderTargets( 1, nullrtv, NULL );
	Swap->Present( 0, 0 );
}
//---------------------------------------------------------------------------
static void UploadPlane( ID3D11DeviceContext* ctx, ID3D11Texture2D* tex,
	const uint8_t* src, int srcStride, int w, int h )
{
	D3D11_MAPPED_SUBRESOURCE m;
	if( FAILED( ctx->Map( tex, 0, D3D11_MAP_WRITE_DISCARD, 0, &m ) ) ) return;
	uint8_t* dst = (uint8_t*)m.pData;
	for( int y = 0; y < h; y++ )
		memcpy( dst + (size_t)y * m.RowPitch, src + (size_t)y * srcStride, w );
	ctx->Unmap( tex, 0 );
}
//---------------------------------------------------------------------------
void tTVPD3D11OverlayWindow::PresentI420( const uint8_t *y, int yStride,
	const uint8_t *u, int uStride, const uint8_t *v, int vStride, int w, int h )
{
	std::lock_guard<std::mutex> lk( Mtx );
	if( !Dev || !Ctx || !Swap || !RTV ) return;
	if( w <= 0 || h <= 0 ) return;
	if( !EnsurePlaneTextures( w, h ) ) return;

	UploadPlane( Ctx, TexY, y, yStride, w, h );
	UploadPlane( Ctx, TexU, u, uStride, w/2, h/2 );
	UploadPlane( Ctx, TexV, v, vStride, w/2, h/2 );
	LastW = w; LastH = h;

	// 描画
	D3D11_VIEWPORT vp; ZeroMemory( &vp, sizeof(vp) );
	vp.Width = (float)SwapW; vp.Height = (float)SwapH; vp.MaxDepth = 1.f;
	Ctx->RSSetViewports( 1, &vp );
	ID3D11RenderTargetView* rtvs[1] = { RTV };
	Ctx->OMSetRenderTargets( 1, rtvs, NULL );
	const float black[4] = { 0, 0, 0, 1 };
	Ctx->ClearRenderTargetView( RTV, black );

	UINT stride = sizeof(Vertex), offset = 0;
	Ctx->IASetInputLayout( IL );
	Ctx->IASetVertexBuffers( 0, 1, &VB, &stride, &offset );
	Ctx->IASetPrimitiveTopology( D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP );
	Ctx->VSSetShader( VS, NULL, 0 );
	Ctx->PSSetShader( PS, NULL, 0 );
	ID3D11ShaderResourceView* srvs[3] = { SrvY, SrvU, SrvV };
	Ctx->PSSetShaderResources( 0, 3, srvs );
	Ctx->PSSetSamplers( 0, 1, &Samp );
	Ctx->Draw( 4, 0 );

	// flip 前に RTV/SRV を外す
	ID3D11RenderTargetView* nullrtv[1] = { NULL };
	Ctx->OMSetRenderTargets( 1, nullrtv, NULL );
	Swap->Present( 0, 0 );
}
//---------------------------------------------------------------------------
void tTVPD3D11OverlayWindow::ResizeToClient()
{
	if( !Swap || !ChildWnd ) return;
	RECT rc; ::GetClientRect( ChildWnd, &rc );
	UINT w = rc.right - rc.left, h = rc.bottom - rc.top;
	if( w == 0 || h == 0 || ( w == SwapW && h == SwapH ) ) return;
	SafeRel( RTV );
	if( SUCCEEDED( Swap->ResizeBuffers( 0, w, h, DXGI_FORMAT_UNKNOWN, 0 ) ) )
	{
		SwapW = w; SwapH = h;
		EnsureBackBufferRTV();
	}
}
//---------------------------------------------------------------------------
void tTVPD3D11OverlayWindow::SetRect( const RECT &rect )
{
	std::lock_guard<std::mutex> lk( Mtx );
	DesiredRect = rect;
	if( ChildWnd )
	{
		int w = rect.right - rect.left, h = rect.bottom - rect.top;
		if( w > 0 && h > 0 )
		{
			::MoveWindow( ChildWnd, rect.left, rect.top, w, h, TRUE );
			ResizeToClient();
		}
	}
}
//---------------------------------------------------------------------------
void tTVPD3D11OverlayWindow::SetVisible( bool visible )
{
	Visible = visible;
	if( ChildWnd )
		::ShowWindow( ChildWnd, visible ? SW_SHOW : SW_HIDE );
}
//---------------------------------------------------------------------------
bool tTVPD3D11OverlayWindow::DebugSaveLastFrame( const wchar_t *path )
{
	std::lock_guard<std::mutex> lk( Mtx );
	if( !Dev || !Ctx || !Swap ) return false;
	ID3D11Texture2D* bb = nullptr;
	if( FAILED( Swap->GetBuffer( 0, __uuidof(ID3D11Texture2D), (void**)&bb ) ) ) return false;
	D3D11_TEXTURE2D_DESC d; bb->GetDesc( &d );
	D3D11_TEXTURE2D_DESC sd = d;
	sd.Usage = D3D11_USAGE_STAGING; sd.BindFlags = 0;
	sd.CPUAccessFlags = D3D11_CPU_ACCESS_READ; sd.MiscFlags = 0;
	ID3D11Texture2D* stg = nullptr;
	bool ok = false;
	if( SUCCEEDED( Dev->CreateTexture2D( &sd, NULL, &stg ) ) )
	{
		Ctx->CopyResource( stg, bb );
		D3D11_MAPPED_SUBRESOURCE m;
		if( SUCCEEDED( Ctx->Map( stg, 0, D3D11_MAP_READ, 0, &m ) ) )
		{
			int w = (int)d.Width, h = (int)d.Height;
			// 32bit BMP (BGRA, ボトムアップ) で保存
			BITMAPFILEHEADER fh; ZeroMemory( &fh, sizeof(fh) );
			BITMAPINFOHEADER ih; ZeroMemory( &ih, sizeof(ih) );
			int rowBytes = w * 4;
			fh.bfType = 0x4D42;
			fh.bfOffBits = sizeof(fh) + sizeof(ih);
			fh.bfSize = fh.bfOffBits + rowBytes * h;
			ih.biSize = sizeof(ih); ih.biWidth = w; ih.biHeight = h;
			ih.biPlanes = 1; ih.biBitCount = 32; ih.biCompression = BI_RGB;
			FILE* fp = _wfopen( path, L"wb" );
			if( fp )
			{
				fwrite( &fh, sizeof(fh), 1, fp );
				fwrite( &ih, sizeof(ih), 1, fp );
				for( int yy = h - 1; yy >= 0; yy-- )
					fwrite( (uint8_t*)m.pData + (size_t)yy * m.RowPitch, rowBytes, 1, fp );
				fclose( fp );
				ok = true;
			}
			Ctx->Unmap( stg, 0 );
		}
		stg->Release();
	}
	bb->Release();
	return ok;
}
//---------------------------------------------------------------------------
void tTVPD3D11OverlayWindow::Destroy()
{
	std::lock_guard<std::mutex> lk( Mtx );
	ReleasePlaneTextures();
	ReleaseBGRATexture();
	SafeRel( Samp ); SafeRel( VB ); SafeRel( IL ); SafeRel( PSRgba ); SafeRel( PS ); SafeRel( VS );
	SafeRel( RTV ); SafeRel( Swap ); SafeRel( Ctx ); SafeRel( Dev );
	if( ChildWnd ) { ::SetWindowLongPtr( ChildWnd, GWLP_USERDATA, 0 ); ::DestroyWindow( ChildWnd ); ChildWnd = nullptr; }
}
//---------------------------------------------------------------------------
