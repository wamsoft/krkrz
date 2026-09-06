/****************************************************************************/
/*! @file
@brief overlay 動画 presenter 用 BGRA→RTV ブリッタの実装 (Track V-E)
*****************************************************************************/
#include "tjsCommHead.h"
#include "VideoPresenterD3D.h"
#include <d3dcompiler.h>

//---------------------------------------------------------------------------
namespace {
struct tVertex { float x, y, u, v; };

// 単純なテクスチャ付きクアッド。定数バッファの全体アルファをフレームのアルファに乗算する。
static const char PresenterShaderHLSL[] =
	"Texture2D tex : register(t0);\n"
	"SamplerState smp : register(s0);\n"
	"cbuffer CB : register(b0) { float4 gColor; };\n"
	"struct VSIn  { float2 pos : POSITION; float2 uv : TEXCOORD; };\n"
	"struct VSOut { float4 pos : SV_POSITION; float2 uv : TEXCOORD; };\n"
	"VSOut VSMain(VSIn i){ VSOut o; o.pos = float4(i.pos, 0.0f, 1.0f); o.uv = i.uv; return o; }\n"
	"float4 PSMain(VSOut i) : SV_TARGET {\n"
	"    float4 c = tex.Sample(smp, i.uv);\n"
	"    return c * gColor;\n"   // rgb もアルファも gColor で乗算 (premultiply でないので rgb は視覚的に同等)
	"}\n";

// I420(planar YUV420, BT.601 limited range)→RGB PS。VS は上の VSMain を共用するので
// VSOut のシグネチャ (SV_POSITION + TEXCOORD) を一致させる。全体アルファは gColor.a。
static const char PresenterYUVShaderHLSL[] =
	"Texture2D texY : register(t0);\n"
	"Texture2D texU : register(t1);\n"
	"Texture2D texV : register(t2);\n"
	"SamplerState smp : register(s0);\n"
	"cbuffer CB : register(b0) { float4 gColor; };\n"
	"struct VSOut { float4 pos : SV_POSITION; float2 uv : TEXCOORD; };\n"
	"float4 PSYUV(VSOut i) : SV_TARGET {\n"
	"    float Y = texY.Sample(smp, i.uv).r;\n"
	"    float U = texU.Sample(smp, i.uv).r;\n"
	"    float V = texV.Sample(smp, i.uv).r;\n"
	"    float y = (Y - 0.0627451) * 1.164383;\n"
	"    float u = U - 0.5019608;\n"
	"    float v = V - 0.5019608;\n"
	"    float r = y + 1.596027 * v;\n"
	"    float g = y - 0.391762 * u - 0.812968 * v;\n"
	"    float b = y + 2.017232 * u;\n"
	"    return float4(saturate(r), saturate(g), saturate(b), gColor.a);\n"
	"}\n";

template<class T> static inline void SafeRelease( T *&p ) { if(p) { p->Release(); p = 0; } }
}
//---------------------------------------------------------------------------
tTVPVideoPresenterD3D::tTVPVideoPresenterD3D()
: Dev(0), VS(0), PS(0), IL(0), VB(0), CB(0), Samp(0), Blend(0), BlendPremul(0), Tex(0), Srv(0), TexW(0), TexH(0)
, PSYUV(0), TexY(0), TexU(0), TexV(0), SrvY(0), SrvU(0), SrvV(0), PlaneW(0), PlaneH(0)
{
}
//---------------------------------------------------------------------------
tTVPVideoPresenterD3D::~tTVPVideoPresenterD3D()
{
	Release();
}
//---------------------------------------------------------------------------
void tTVPVideoPresenterD3D::Release()
{
	SafeRelease(SrvY); SafeRelease(SrvU); SafeRelease(SrvV);
	SafeRelease(TexY); SafeRelease(TexU); SafeRelease(TexV);
	SafeRelease(PSYUV);
	SafeRelease(Srv);
	SafeRelease(Tex);
	SafeRelease(Blend);
	SafeRelease(BlendPremul);
	SafeRelease(Samp);
	SafeRelease(CB);
	SafeRelease(VB);
	SafeRelease(IL);
	SafeRelease(PS);
	SafeRelease(VS);
	Dev = 0;
	TexW = TexH = 0;
	PlaneW = PlaneH = 0;
}
//---------------------------------------------------------------------------
bool tTVPVideoPresenterD3D::EnsurePipeline( ID3D11Device * dev )
{
	if( Dev == dev && VS && PS && IL && VB && CB && Samp && Blend ) return true;

	// デバイスが変わった (デバイスロスト等) → 全部作り直す
	Release();
	Dev = dev;

	HRESULT hr;
	ID3DBlob *vsBlob = 0, *psBlob = 0, *errBlob = 0;
	UINT flags = 0;
#ifdef _DEBUG
	flags |= D3DCOMPILE_DEBUG | D3DCOMPILE_SKIP_OPTIMIZATION;
#endif

	hr = D3DCompile(PresenterShaderHLSL, sizeof(PresenterShaderHLSL)-1, "vpres", NULL, NULL,
		"VSMain", "vs_4_0", flags, 0, &vsBlob, &errBlob);
	if( FAILED(hr) ) { SafeRelease(errBlob); Release(); return false; }
	SafeRelease(errBlob);

	hr = D3DCompile(PresenterShaderHLSL, sizeof(PresenterShaderHLSL)-1, "vpres", NULL, NULL,
		"PSMain", "ps_4_0", flags, 0, &psBlob, &errBlob);
	if( FAILED(hr) ) { SafeRelease(errBlob); SafeRelease(vsBlob); Release(); return false; }
	SafeRelease(errBlob);

	hr = dev->CreateVertexShader(vsBlob->GetBufferPointer(), vsBlob->GetBufferSize(), NULL, &VS);
	if( FAILED(hr) ) { SafeRelease(psBlob); SafeRelease(vsBlob); Release(); return false; }
	hr = dev->CreatePixelShader(psBlob->GetBufferPointer(), psBlob->GetBufferSize(), NULL, &PS);
	SafeRelease(psBlob);
	if( FAILED(hr) ) { SafeRelease(vsBlob); Release(); return false; }

	D3D11_INPUT_ELEMENT_DESC ied[] = {
		{ "POSITION", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 0,                            D3D11_INPUT_PER_VERTEX_DATA, 0 },
		{ "TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, D3D11_APPEND_ALIGNED_ELEMENT, D3D11_INPUT_PER_VERTEX_DATA, 0 },
	};
	hr = dev->CreateInputLayout(ied, 2, vsBlob->GetBufferPointer(), vsBlob->GetBufferSize(), &IL);
	SafeRelease(vsBlob);
	if( FAILED(hr) ) { Release(); return false; }

	D3D11_BUFFER_DESC bd; ZeroMemory(&bd, sizeof(bd));
	bd.ByteWidth = sizeof(tVertex) * 4;
	bd.Usage = D3D11_USAGE_DYNAMIC;
	bd.BindFlags = D3D11_BIND_VERTEX_BUFFER;
	bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
	hr = dev->CreateBuffer(&bd, NULL, &VB);
	if( FAILED(hr) ) { Release(); return false; }

	D3D11_BUFFER_DESC cbd; ZeroMemory(&cbd, sizeof(cbd));
	cbd.ByteWidth = 16; // float4
	cbd.Usage = D3D11_USAGE_DYNAMIC;
	cbd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
	cbd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
	hr = dev->CreateBuffer(&cbd, NULL, &CB);
	if( FAILED(hr) ) { Release(); return false; }

	D3D11_SAMPLER_DESC sd; ZeroMemory(&sd, sizeof(sd));
	sd.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
	sd.AddressU = sd.AddressV = sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
	sd.ComparisonFunc = D3D11_COMPARISON_NEVER;
	sd.MaxLOD = D3D11_FLOAT32_MAX;
	hr = dev->CreateSamplerState(&sd, &Samp);
	if( FAILED(hr) ) { Release(); return false; }

	D3D11_BLEND_DESC bl; ZeroMemory(&bl, sizeof(bl));
	bl.RenderTarget[0].BlendEnable = TRUE;
	bl.RenderTarget[0].SrcBlend = D3D11_BLEND_SRC_ALPHA;
	bl.RenderTarget[0].DestBlend = D3D11_BLEND_INV_SRC_ALPHA;
	bl.RenderTarget[0].BlendOp = D3D11_BLEND_OP_ADD;
	bl.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_ONE;
	bl.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_INV_SRC_ALPHA;
	bl.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_ADD;
	bl.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
	hr = dev->CreateBlendState(&bl, &Blend);
	if( FAILED(hr) ) { Release(); return false; }

	// premultiplied 用: src の RGB には既にアルファが掛かっているので SrcBlend
	// は ONE。 Elements の overlay (ThorVG の ColorSpace::ARGB8888 出力) 用。
	bl.RenderTarget[0].SrcBlend = D3D11_BLEND_ONE;
	hr = dev->CreateBlendState(&bl, &BlendPremul);
	if( FAILED(hr) ) { Release(); return false; }

	return true;
}
//---------------------------------------------------------------------------
bool tTVPVideoPresenterD3D::EnsureTexture( ID3D11Device * dev, int w, int h )
{
	if( Tex && Srv && TexW == w && TexH == h ) return true;
	SafeRelease(Srv);
	SafeRelease(Tex);
	TexW = TexH = 0;
	if( w <= 0 || h <= 0 ) return false;

	D3D11_TEXTURE2D_DESC td; ZeroMemory(&td, sizeof(td));
	td.Width = w; td.Height = h;
	td.MipLevels = 1; td.ArraySize = 1;
	td.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
	td.SampleDesc.Count = 1;
	td.Usage = D3D11_USAGE_DYNAMIC;
	td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
	td.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
	HRESULT hr = dev->CreateTexture2D(&td, NULL, &Tex);
	if( FAILED(hr) ) return false;

	hr = dev->CreateShaderResourceView(Tex, NULL, &Srv);
	if( FAILED(hr) ) { SafeRelease(Tex); return false; }

	TexW = w; TexH = h;
	return true;
}
//---------------------------------------------------------------------------
bool tTVPVideoPresenterD3D::Render( const tTVPVideoPresenterContext & ctx,
	const void * topRow, int pitch, int w, int h,
	const tTVPRect & dst, float alpha )
{
	if( !ctx.Device || !ctx.Context || !topRow || w <= 0 || h <= 0 ) return false;
	if( !EnsurePipeline(ctx.Device) ) return false;
	if( !EnsureTexture(ctx.Device, w, h) ) return false;

	ID3D11DeviceContext * ictx = ctx.Context;

	// -- フレームをテクスチャへアップロード (top-down)
	D3D11_MAPPED_SUBRESOURCE m;
	if( FAILED(ictx->Map(Tex, 0, D3D11_MAP_WRITE_DISCARD, 0, &m)) ) return false;
	const BYTE * src = (const BYTE*)topRow;
	BYTE * dstp = (BYTE*)m.pData;
	int rowBytes = w * 4;
	for( int y = 0; y < h; ++y ) {
		memcpy( dstp + (size_t)y * m.RowPitch, src + (ptrdiff_t)y * pitch, rowBytes );
	}
	ictx->Unmap(Tex, 0);

	DrawQuad( ictx, &Srv, 1, PS, ctx, dst, alpha );
	return true;
}
//---------------------------------------------------------------------------
bool tTVPVideoPresenterD3D::RenderSRV( const tTVPVideoPresenterContext & ctx,
	ID3D11ShaderResourceView * srv, int w, int h,
	const tTVPRect & dst, float alpha, bool premultiplied )
{
	if( !ctx.Device || !ctx.Context || !srv || w <= 0 || h <= 0 ) return false;
	if( !EnsurePipeline(ctx.Device) ) return false;
	DrawQuad( ctx.Context, &srv, 1, PS, ctx, dst, alpha, premultiplied );
	return true;
}
//---------------------------------------------------------------------------
bool tTVPVideoPresenterD3D::EnsureYUVShader( ID3D11Device * dev )
{
	if( PSYUV ) return true;
	HRESULT hr;
	ID3DBlob *psBlob = 0, *errBlob = 0;
	UINT flags = 0;
#ifdef _DEBUG
	flags |= D3DCOMPILE_DEBUG | D3DCOMPILE_SKIP_OPTIMIZATION;
#endif
	hr = D3DCompile(PresenterYUVShaderHLSL, sizeof(PresenterYUVShaderHLSL)-1, "vpresyuv", NULL, NULL,
		"PSYUV", "ps_4_0", flags, 0, &psBlob, &errBlob);
	if( FAILED(hr) ) { SafeRelease(errBlob); return false; }
	SafeRelease(errBlob);
	hr = dev->CreatePixelShader(psBlob->GetBufferPointer(), psBlob->GetBufferSize(), NULL, &PSYUV);
	SafeRelease(psBlob);
	return SUCCEEDED(hr);
}
//---------------------------------------------------------------------------
bool tTVPVideoPresenterD3D::EnsurePlaneTextures( ID3D11Device * dev, int w, int h )
{
	if( TexY && TexU && TexV && PlaneW == w && PlaneH == h ) return true;
	SafeRelease(SrvY); SafeRelease(SrvU); SafeRelease(SrvV);
	SafeRelease(TexY); SafeRelease(TexU); SafeRelease(TexV);
	PlaneW = PlaneH = 0;
	if( w <= 0 || h <= 0 ) return false;

	struct { ID3D11Texture2D** t; ID3D11ShaderResourceView** s; int pw, ph; } planes[3] = {
		{ &TexY, &SrvY, w,      h      },
		{ &TexU, &SrvU, (w+1)/2, (h+1)/2 },
		{ &TexV, &SrvV, (w+1)/2, (h+1)/2 },
	};
	for( int i = 0; i < 3; ++i ) {
		D3D11_TEXTURE2D_DESC td; ZeroMemory(&td, sizeof(td));
		td.Width = planes[i].pw; td.Height = planes[i].ph;
		td.MipLevels = 1; td.ArraySize = 1;
		td.Format = DXGI_FORMAT_R8_UNORM;
		td.SampleDesc.Count = 1;
		td.Usage = D3D11_USAGE_DYNAMIC;
		td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
		td.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
		if( FAILED(dev->CreateTexture2D(&td, NULL, planes[i].t)) ) { SafeRelease(SrvY);SafeRelease(SrvU);SafeRelease(SrvV);SafeRelease(TexY);SafeRelease(TexU);SafeRelease(TexV); return false; }
		if( FAILED(dev->CreateShaderResourceView(*planes[i].t, NULL, planes[i].s)) ) { SafeRelease(SrvY);SafeRelease(SrvU);SafeRelease(SrvV);SafeRelease(TexY);SafeRelease(TexU);SafeRelease(TexV); return false; }
	}
	PlaneW = w; PlaneH = h;
	return true;
}
//---------------------------------------------------------------------------
bool tTVPVideoPresenterD3D::RenderI420( const tTVPVideoPresenterContext & ctx,
	const void * y, int yStride, const void * u, int uStride,
	const void * v, int vStride, int w, int h,
	const tTVPRect & dst, float alpha )
{
	if( !ctx.Device || !ctx.Context || !y || !u || !v || w <= 0 || h <= 0 ) return false;
	if( !EnsurePipeline(ctx.Device) ) return false;
	if( !EnsureYUVShader(ctx.Device) ) return false;
	if( !EnsurePlaneTextures(ctx.Device, w, h) ) return false;

	ID3D11DeviceContext * ictx = ctx.Context;
	int cw = (w+1)/2, ch = (h+1)/2;
	// Y / U / V を各テクスチャへアップロード (行ごと。src stride と dst RowPitch が異なりうる)
	struct { ID3D11Texture2D* t; const BYTE* src; int stride, pw, ph; } up[3] = {
		{ TexY, (const BYTE*)y, yStride, w,  h  },
		{ TexU, (const BYTE*)u, uStride, cw, ch },
		{ TexV, (const BYTE*)v, vStride, cw, ch },
	};
	for( int i = 0; i < 3; ++i ) {
		D3D11_MAPPED_SUBRESOURCE m;
		if( FAILED(ictx->Map(up[i].t, 0, D3D11_MAP_WRITE_DISCARD, 0, &m)) ) return false;
		BYTE* dstp = (BYTE*)m.pData;
		for( int row = 0; row < up[i].ph; ++row )
			memcpy( dstp + (size_t)row * m.RowPitch, up[i].src + (size_t)row * up[i].stride, up[i].pw );
		ictx->Unmap(up[i].t, 0);
	}

	ID3D11ShaderResourceView * srvs[3] = { SrvY, SrvU, SrvV };
	DrawQuad( ictx, srvs, 3, PSYUV, ctx, dst, alpha );
	return true;
}
//---------------------------------------------------------------------------
void tTVPVideoPresenterD3D::DrawQuad( ID3D11DeviceContext * ictx,
	ID3D11ShaderResourceView * const * srv, int srvCount, ID3D11PixelShader * ps,
	const tTVPVideoPresenterContext & ctx, const tTVPRect & dst, float alpha,
	bool premultiplied )
{
	// -- 定数バッファ (全体アルファ)
	D3D11_MAPPED_SUBRESOURCE cm;
	if( SUCCEEDED(ictx->Map(CB, 0, D3D11_MAP_WRITE_DISCARD, 0, &cm)) ) {
		float col[4] = { 1.0f, 1.0f, 1.0f, alpha };
		memcpy(cm.pData, col, sizeof(col));
		ictx->Unmap(CB, 0);
	}

	// -- dest ピクセル → NDC
	float sw = (float)(ctx.TargetWidth  ? ctx.TargetWidth  : 1);
	float sh = (float)(ctx.TargetHeight ? ctx.TargetHeight : 1);
	float nl = (float)dst.left   * 2.0f / sw - 1.0f;
	float nr = (float)dst.right  * 2.0f / sw - 1.0f;
	float nt = 1.0f - (float)dst.top    * 2.0f / sh;
	float nb = 1.0f - (float)dst.bottom * 2.0f / sh;

	tVertex verts[4] = {
		{ nl, nt, 0.0f, 0.0f },
		{ nr, nt, 1.0f, 0.0f },
		{ nl, nb, 0.0f, 1.0f },
		{ nr, nb, 1.0f, 1.0f },
	};
	D3D11_MAPPED_SUBRESOURCE vm;
	if( FAILED(ictx->Map(VB, 0, D3D11_MAP_WRITE_DISCARD, 0, &vm)) ) return;
	memcpy(vm.pData, verts, sizeof(verts));
	ictx->Unmap(VB, 0);

	// -- 描画 (RTV/viewport は呼び出し側 = RenderVideoPresenters が設定済み)
	UINT stride = sizeof(tVertex), offset = 0;
	ictx->IASetInputLayout(IL);
	ictx->IASetVertexBuffers(0, 1, &VB, &stride, &offset);
	ictx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);
	ictx->VSSetShader(VS, NULL, 0);
	ictx->PSSetShader(ps, NULL, 0);
	ictx->PSSetConstantBuffers(0, 1, &CB);
	ictx->PSSetShaderResources(0, srvCount, srv);
	ictx->PSSetSamplers(0, 1, &Samp);
	const float bf[4] = { 0, 0, 0, 0 };
	ictx->OMSetBlendState((premultiplied && BlendPremul) ? BlendPremul : Blend,
	                      bf, 0xffffffff);
	ictx->Draw(4, 0);

	// SRV を外す (次フレーム Map 時の hazard 回避)
	ID3D11ShaderResourceView * nullsrv[3] = { NULL, NULL, NULL };
	ictx->PSSetShaderResources(0, srvCount, nullsrv);
}
//---------------------------------------------------------------------------
