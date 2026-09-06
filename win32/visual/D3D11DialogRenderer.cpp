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
void tTVPD3D11DialogRenderer::DestroyTexture(Layer & L)
{
	if (L.srv) { L.srv->Release(); L.srv = nullptr; }
	if (L.tex) { L.tex->Release(); L.tex = nullptr; }
	L.texDev = nullptr;
	L.uploaded = false;
}
//---------------------------------------------------------------------------
void tTVPD3D11DialogRenderer::DestroyLayer(Layer & L)
{
	DestroyTexture(L);
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
bool tTVPD3D11DialogRenderer::EnsureTexture(Layer & L, ID3D11Device * dev)
{
	if (!dev || L.w <= 0 || L.h <= 0) return false;
	if (L.tex && L.srv && L.texDev == dev) return true;
	DestroyTexture(L);

	D3D11_TEXTURE2D_DESC td; ZeroMemory(&td, sizeof(td));
	td.Width  = static_cast<UINT>(L.w);
	td.Height = static_cast<UINT>(L.h);
	td.MipLevels = 1; td.ArraySize = 1;
	td.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
	td.SampleDesc.Count = 1;
	// DEFAULT + UpdateSubresource(box) が部分更新できる唯一の組み合わせ
	// (DYNAMIC の Map は WRITE_DISCARD しか許されず全面転送になる)。
	td.Usage = D3D11_USAGE_DEFAULT;
	td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
	td.CPUAccessFlags = 0;
	if (FAILED(dev->CreateTexture2D(&td, NULL, &L.tex))) return false;
	if (FAILED(dev->CreateShaderResourceView(L.tex, NULL, &L.srv))) {
		L.tex->Release(); L.tex = nullptr;
		return false;
	}
	L.texDev = dev;
	L.uploaded = false;   // 作り直した直後は中身が不定なので全面転送が要る
	return true;
}
//---------------------------------------------------------------------------
void tTVPD3D11DialogRenderer::UploadRect(const void* layer, int x, int y, int w, int h)
{
	if (!_host) return;
	auto it = _layers.find(layer);
	if (it == _layers.end()) return;
	Layer & L = it->second;
	if (L.staging.empty() || L.w <= 0 || L.h <= 0) return;

	ID3D11Device * dev = nullptr; ID3D11DeviceContext * dctx = nullptr;
	ID3D11RenderTargetView * rtv = nullptr; int tw = 0, th = 0;
	if (!_host->DialogHost_GetD3D(dev, dctx, rtv, tw, th)) return;
	if (!dev || !dctx) return;
	if (!EnsureTexture(L, dev)) return;

	// 全面指定、 または未だ 1 度も全面を上げていない (テクスチャ内容が不定) 間は全面。
	if (w <= 0 || h <= 0 || !L.uploaded) { x = 0; y = 0; w = L.w; h = L.h; }

	// staging 範囲へクランプ (呼出側の外側丸めで 1px はみ出ることがある)
	if (x < 0) { w += x; x = 0; }
	if (y < 0) { h += y; y = 0; }
	if (x + w > L.w) w = L.w - x;
	if (y + h > L.h) h = L.h - y;
	if (w <= 0 || h <= 0) return;

	D3D11_BOX box;
	box.left = static_cast<UINT>(x);
	box.top = static_cast<UINT>(y);
	box.front = 0;
	box.right = static_cast<UINT>(x + w);
	box.bottom = static_cast<UINT>(y + h);
	box.back = 1;
	// staging は全面を保持したままなので、 先頭を (x, y) へずらして
	// pitch = L.w*4 のまま渡す (D3D 側が行間を読み飛ばす)。
	const std::uint32_t * src = L.staging.data() + static_cast<size_t>(y) * L.w + x;
	dctx->UpdateSubresource(L.tex, 0, &box, src,
	                        static_cast<UINT>(L.w) * 4, 0);
	L.uploaded = true;
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
		// サイズ変化時は staging と GPU テクスチャを作り直す。
		L.staging.assign(static_cast<size_t>(w) * static_cast<size_t>(h), 0u);
		L.w = w;
		L.h = h;
		DestroyTexture(L);
	}
	// Elements が描き込むのはステージング。pitch は常に w*4 で連続。
	return L.staging.data();
}
//---------------------------------------------------------------------------
void tTVPD3D11DialogRenderer::ReleaseBuffer(const void* layer)
{
	UploadRect(layer, 0, 0, 0, 0);   // 全面
}
//---------------------------------------------------------------------------
void tTVPD3D11DialogRenderer::ReleaseBufferRect(const void* layer, int x, int y, int w, int h)
{
	UploadRect(layer, x, y, w, h);
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

	// テクスチャは ReleaseBuffer(Rect) で更新済みのはず。デバイス作り直し等で
	// 失われていたらここで全面を上げ直す (描画スレッド内なので安全)。
	if (!L.tex || !L.srv || L.texDev != dev || !L.uploaded) {
		UploadRect(layer, 0, 0, 0, 0);
		if (!L.srv) return;
	}

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

	// アップロード済みテクスチャ (0xAARRGGBB = メモリ上 BGRA = B8G8R8A8_UNORM)
	// を dst 矩形へ α 合成描画。alpha=1.0 (レイヤ内容自体の α で合成される)。
	//
	// **premultiplied 合成**にする。 Elements の canvas は ThorVG の Sw raster が
	// `ColorSpace::ARGB8888` (= alpha-premultiplied) で出力したもの。 straight
	// alpha で合成するとアルファが二重に掛かり、 文字のアンチエイリアスや
	// 半透明の下地が薄く出る。
	tTVPRect dst(x, y, x + w, y + h);
	L.presenter->RenderSRV(vctx, L.srv, L.w, L.h, dst, 1.0f,
	                       /*premultiplied=*/true);
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
