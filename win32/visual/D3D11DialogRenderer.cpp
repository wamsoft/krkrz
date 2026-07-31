//---------------------------------------------------------------------------
// D3D11 用 Elements ダイアログ描画アダプタ実装 (WINVER / BasicDrawDevice)
//---------------------------------------------------------------------------
#include "tjsCommHead.h"
#include "D3D11DialogRenderer.h"
#include "VideoPresenterD3D.h"   // tTVPVideoPresenterD3D
#include "VideoPresenter.h"      // tTVPVideoPresenterContext / tTVPRect
#include <d3d11.h>

//---------------------------------------------------------------------------
tTVPD3D11DialogRenderer::tTVPD3D11DialogRenderer(iTVPD3D11DialogHost * host)
	: _host(host)
{
}
//---------------------------------------------------------------------------
tTVPD3D11DialogRenderer::~tTVPD3D11DialogRenderer()
{
	for (auto & kv : _layers) DestroyLayer(kv.second);
	_layers.clear();
}
//---------------------------------------------------------------------------
void tTVPD3D11DialogRenderer::DestroyLayer(Layer & L)
{
	if (L.presenter) {
		L.presenter->Release();   // GPU リソース解放 (device 生存に依らず安全)
		delete L.presenter;
		L.presenter = nullptr;
	}
	L.staging.clear();
	L.staging.shrink_to_fit();
	L.w = 0;
	L.h = 0;
}
//---------------------------------------------------------------------------
void tTVPD3D11DialogRenderer::GetSurfaceSize(int & w, int & h)
{
	w = 0; h = 0;
	if (!_host) return;
	ID3D11Device * dev = nullptr; ID3D11DeviceContext * ctx = nullptr;
	ID3D11RenderTargetView * rtv = nullptr;
	int tw = 0, th = 0;
	if (_host->DialogHost_GetD3D(dev, ctx, rtv, tw, th)) { w = tw; h = th; }
}
//---------------------------------------------------------------------------
void tTVPD3D11DialogRenderer::GetDestRect(int & x, int & y, int & w, int & h)
{
	x = 0; y = 0; w = 0; h = 0;
	if (!_host) return;
	_host->DialogHost_GetDestRect(x, y, w, h);
	// DestRect が未設定 (0 サイズ) なら surface 全体にフォールバック。
	if (w <= 0 || h <= 0) {
		GetSurfaceSize(w, h);
		x = 0; y = 0;
	}
}
//---------------------------------------------------------------------------
std::uint32_t * tTVPD3D11DialogRenderer::AcquireBuffer(const void* layer, int w, int h)
{
	if (!_host || !layer || w <= 0 || h <= 0) return nullptr;
	Layer & L = _layers[layer];
	if (L.w != w || L.h != h) {
		// サイズ変化時は staging を作り直す。presenter のテクスチャは Render 内の
		// EnsureTexture がサイズ変化を吸収するので破棄不要。
		L.staging.assign(static_cast<size_t>(w) * static_cast<size_t>(h), 0u);
		L.w = w;
		L.h = h;
	}
	// Elements が描き込むのはステージング。pitch は常に w*4 で連続。
	return L.staging.data();
}
//---------------------------------------------------------------------------
void tTVPD3D11DialogRenderer::ReleaseBuffer(const void* /*layer*/)
{
	// アップロード (CPU staging → DYNAMIC tex) は PresentOverlay の Render 内で
	// 行うため、ここでは何もしない。
}
//---------------------------------------------------------------------------
void tTVPD3D11DialogRenderer::PresentOverlay(const void* layer, int x, int y, int w, int h)
{
	if (!_host) return;
	auto it = _layers.find(layer);
	if (it == _layers.end()) return;
	Layer & L = it->second;
	if (L.staging.empty() || L.w <= 0 || L.h <= 0) return;

	ID3D11Device * dev = nullptr; ID3D11DeviceContext * dctx = nullptr;
	ID3D11RenderTargetView * rtv = nullptr; int tw = 0, th = 0;
	if (!_host->DialogHost_GetD3D(dev, dctx, rtv, tw, th)) return;
	if (!dev || !dctx || !rtv || tw <= 0 || th <= 0) return;

	if (!L.presenter) L.presenter = new tTVPVideoPresenterD3D();

	// 動画 presenter 経路と違い、dialog overlay 経路は RTV/viewport が未設定なので
	// ここで self-contained に設定する (game frame の上に重ねるので clear はしない)。
	ID3D11RenderTargetView * rtvs[1] = { rtv };
	dctx->OMSetRenderTargets(1, rtvs, nullptr);
	D3D11_VIEWPORT vp; ZeroMemory(&vp, sizeof(vp));
	vp.Width = static_cast<float>(tw);
	vp.Height = static_cast<float>(th);
	vp.MinDepth = 0.0f; vp.MaxDepth = 1.0f;
	dctx->RSSetViewports(1, &vp);

	tTVPVideoPresenterContext vctx;
	vctx.Device       = dev;
	vctx.Context      = dctx;
	vctx.RenderTarget = rtv;
	vctx.TargetWidth  = static_cast<tjs_uint>(tw);
	vctx.TargetHeight = static_cast<tjs_uint>(th);
	// DrawQuad は TargetWidth/Height と dst 矩形しか使わないが、一応埋めておく。
	vctx.DestRect = tTVPRect(0, 0, tw, th);
	vctx.ClipRect = tTVPRect(0, 0, tw, th);
	vctx.SrcWidth = tw;
	vctx.SrcHeight = th;

	// staging (0xAARRGGBB = メモリ上 BGRA、top-down、pitch = L.w*4) を dst 矩形へ
	// α 合成描画。alpha=1.0 (レイヤ内容自体の α で合成される)。
	tTVPRect dst(x, y, x + w, y + h);
	L.presenter->Render(vctx, L.staging.data(), L.w * 4, L.w, L.h, dst, 1.0f);
}
//---------------------------------------------------------------------------
void tTVPD3D11DialogRenderer::ReleaseLayer(const void* layer)
{
	auto it = _layers.find(layer);
	if (it == _layers.end()) return;
	DestroyLayer(it->second);
	_layers.erase(it);
}
//---------------------------------------------------------------------------
