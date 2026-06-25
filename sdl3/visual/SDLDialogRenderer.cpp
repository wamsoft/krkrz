//---------------------------------------------------------------------------
// SDL3 用 Elements ダイアログ描画アダプタ
//---------------------------------------------------------------------------
#include "tjsCommHead.h"
#include "SDLDialogRenderer.h"
#include "SDLDrawDevice.h"
#include "DebugIntf.h"

tTVPSDLDialogRenderer::tTVPSDLDialogRenderer(tTVPSDLDrawDevice * device)
	: _device(device)
{
}

tTVPSDLDialogRenderer::~tTVPSDLDialogRenderer()
{
	for (auto& kv : _layers) DestroyLayer(kv.second);
	_layers.clear();
}

void tTVPSDLDialogRenderer::GetSurfaceSize(int& w, int& h)
{
	w = 0; h = 0;
	if (!_device) return;
	SDL_Renderer * renderer = _device->GetSDLRenderer();
	if (!renderer) return;

	// SDL3 の SDL_GetRenderLogicalPresentation は logical 未設定でも true を
	// 返して w=h=0, mode=DISABLED をセットする。mode で分岐する必要あり。
	SDL_RendererLogicalPresentation mode = SDL_LOGICAL_PRESENTATION_DISABLED;
	int lw = 0, lh = 0;
	SDL_GetRenderLogicalPresentation(renderer, &lw, &lh, &mode);
	if (mode != SDL_LOGICAL_PRESENTATION_DISABLED && lw > 0 && lh > 0) {
		// logical 設定中: SDL_RenderTexture の dst rect も同じ logical 座標で
		// 解釈されるので、ここも logical サイズで返す。
		w = lw;
		h = lh;
	} else {
		// logical 未設定: dst rect は output (ピクセル) 座標で解釈される。
		SDL_GetCurrentRenderOutputSize(renderer, &w, &h);
	}
}

void tTVPSDLDialogRenderer::GetDestRect(int& x, int& y, int& w, int& h)
{
	x = 0; y = 0; w = 0; h = 0;
	if (!_device) return;
	const tTVPRect& dest = _device->GetDestRectExt();
	x = dest.left;
	y = dest.top;
	w = dest.get_width();
	h = dest.get_height();
	// DestRect が未設定 (0 サイズ) の場合は logical surface 全体にフォール
	// バック (= 旧来動作)
	if (w <= 0 || h <= 0) {
		GetSurfaceSize(w, h);
		x = 0;
		y = 0;
	}
}

void tTVPSDLDialogRenderer::DestroyLayer(Layer& layer)
{
	if (layer.texture) {
		SDL_DestroyTexture(layer.texture);
		layer.texture = nullptr;
	}
	layer.staging.clear();
	layer.staging.shrink_to_fit();
	layer.w = 0;
	layer.h = 0;
}

uint32_t * tTVPSDLDialogRenderer::AcquireBuffer(const void* layer, int w, int h)
{
	if (!_device || !layer) return nullptr;
	SDL_Renderer * renderer = _device->GetSDLRenderer();
	if (!renderer) return nullptr;

	Layer& L = _layers[layer];

	if (L.texture && (L.w != w || L.h != h)) {
		DestroyLayer(L);
	}
	if (!L.texture) {
		L.texture = SDL_CreateTexture(renderer,
			SDL_PIXELFORMAT_ARGB8888,
			SDL_TEXTUREACCESS_STREAMING,
			w, h);
		if (!L.texture) {
			TVPAddImportantLog(ttstr(TJS_W("ElementsDialog: SDL_CreateTexture failed: "))
				+ ttstr(SDL_GetError()));
			return nullptr;
		}
		SDL_SetTextureBlendMode(L.texture, SDL_BLENDMODE_BLEND);
		L.staging.assign(static_cast<size_t>(w) * static_cast<size_t>(h), 0u);
		L.w = w;
		L.h = h;
	}

	// Elements が描き込むのはステージングバッファ。pitch は常に w*4 で連続。
	return L.staging.data();
}

void tTVPSDLDialogRenderer::ReleaseBuffer(const void* layer)
{
	auto it = _layers.find(layer);
	if (it == _layers.end()) return;
	Layer& L = it->second;
	if (!L.texture || L.staging.empty()) return;
	// ステージング → SDL_Texture (pitch を SDL に任せる)
	if (!SDL_UpdateTexture(L.texture, nullptr, L.staging.data(), L.w * 4)) {
		TVPAddImportantLog(ttstr(TJS_W("ElementsDialog: SDL_UpdateTexture failed: "))
			+ ttstr(SDL_GetError()));
	}
}

void tTVPSDLDialogRenderer::PresentOverlay(const void* layer, int x, int y, int w, int h)
{
	if (!_device) return;
	auto it = _layers.find(layer);
	if (it == _layers.end() || !it->second.texture) return;
	SDL_Renderer * renderer = _device->GetSDLRenderer();
	if (!renderer) return;

	SDL_FRect dst{ static_cast<float>(x), static_cast<float>(y),
	               static_cast<float>(w), static_cast<float>(h) };
	SDL_RenderTexture(renderer, it->second.texture, nullptr, &dst);
}

void tTVPSDLDialogRenderer::ReleaseLayer(const void* layer)
{
	auto it = _layers.find(layer);
	if (it == _layers.end()) return;
	DestroyLayer(it->second);
	_layers.erase(it);
}
