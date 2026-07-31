//---------------------------------------------------------------------------
// OpenGL ES 用 Elements ダイアログ描画アダプタ
//---------------------------------------------------------------------------
#include "tjsCommHead.h"
#include "OGLDialogRenderer.h"
#include "OpenGLContext.h"
#include "GLTexture.h"
#include "DebugIntf.h"
#include <cstring>

//---------------------------------------------------------------------------
tTVPOGLDialogRenderer::tTVPOGLDialogRenderer(iTVPGLDialogHost * host)
	: _host(host)
{
}

//---------------------------------------------------------------------------
tTVPOGLDialogRenderer::~tTVPOGLDialogRenderer()
{
	for (auto& kv : _layers) DestroyLayer(kv.second);
	_layers.clear();
}

//---------------------------------------------------------------------------
void tTVPOGLDialogRenderer::DestroyLayer(Layer& layer)
{
	if (layer.texture) {
		// GL context が生存しているうちにのみ glDeleteTextures を呼ぶ。
		// host が UnregisterDialogHost を DoneContext より前に呼んでいれば
		// ここで context を MakeCurrent して安全に破棄できる。
		iTVPGLContext * ctx = _host ? _host->DialogHost_GetGLContext() : nullptr;
		if (ctx) {
			ctx->MakeCurrent();
			delete layer.texture;
		} else {
			// context が既に解放されている: glDeleteTextures は UB なので
			// GL handle はリークさせる (process 寿命までで回収される)。
			TVPAddImportantLog(TJS_W("OGLDialogRenderer: GL context is gone; leaking texture handle"));
		}
		layer.texture = nullptr;
	}
	layer.staging.clear();
	layer.staging.shrink_to_fit();
	layer.w = 0;
	layer.h = 0;
	layer.dirty = false;
}

//---------------------------------------------------------------------------
void tTVPOGLDialogRenderer::GetSurfaceSize(int & w, int & h)
{
	w = 0; h = 0;
	if (!_host) return;
	iTVPGLContext * ctx = _host->DialogHost_GetGLContext();
	if (!ctx) return;
	ctx->GetSurfaceSize(&w, &h);
}

//---------------------------------------------------------------------------
void tTVPOGLDialogRenderer::GetDestRect(int & x, int & y, int & w, int & h)
{
	x = 0; y = 0; w = 0; h = 0;
	if (!_host) return;
	_host->DialogHost_GetDestRect(x, y, w, h);
	// DestRect 未確定なら surface 全体にフォールバック (旧来動作)
	if (w <= 0 || h <= 0) {
		GetSurfaceSize(w, h);
		x = 0;
		y = 0;
	}
}

//---------------------------------------------------------------------------
std::uint32_t * tTVPOGLDialogRenderer::AcquireBuffer(const void* layer, int w, int h)
{
	if (!_host || !layer || w <= 0 || h <= 0) return nullptr;

	Layer& L = _layers[layer];

	if (L.w != w || L.h != h) {
		// サイズ変更: 旧テクスチャを破棄して再確保する
		DestroyLayer(L);
	}
	if (L.staging.empty()) {
		L.staging.assign(static_cast<std::size_t>(w) * static_cast<std::size_t>(h), 0u);
		L.w = w;
		L.h = h;
	}
	L.dirty = false; // 呼出側がこれから書き込む
	return L.staging.data();
}

//---------------------------------------------------------------------------
void tTVPOGLDialogRenderer::ReleaseBuffer(const void* layer)
{
	auto it = _layers.find(layer);
	if (!_host || it == _layers.end() || it->second.staging.empty()) return;
	Layer& L = it->second;
	iTVPGLContext * ctx = _host->DialogHost_GetGLContext();
	if (!ctx) {
		// context 未確立: アップロードは PresentOverlay 時に試みる (現状は no-op)
		L.dirty = true;
		return;
	}
	ctx->MakeCurrent();

	if (!L.texture) {
		L.texture = new GLTexture(static_cast<GLuint>(L.w), static_cast<GLuint>(L.h));
	}
	if (!L.texture) {
		TVPAddImportantLog(TJS_W("OGLDialogRenderer: GLTexture allocation failed"));
		return;
	}

	// staging → GLTexture へ PBO 経由でアップロード。pitch は常に w*4 で連続。
	const std::uint32_t * src = L.staging.data();
	const int w = L.w;
	const int h = L.h;
	L.texture->UpdateTexture(0, 0, w, h, [src, w, h](char * dest, int pitch) {
		const int row_bytes = w * 4;
		if (pitch == row_bytes) {
			std::memcpy(dest, src, static_cast<std::size_t>(row_bytes) * h);
		} else {
			const char * sp = reinterpret_cast<const char *>(src);
			for (int y = 0; y < h; ++y) {
				std::memcpy(dest + y * pitch, sp + y * row_bytes, row_bytes);
			}
		}
	});
	L.dirty = false;
}

//---------------------------------------------------------------------------
void tTVPOGLDialogRenderer::PresentOverlay(const void* layer, int x, int y, int w, int h)
{
	if (!_host || w <= 0 || h <= 0) return;
	auto it = _layers.find(layer);
	if (it == _layers.end() || !it->second.texture) return;
	GLTexture * texture = it->second.texture;
	iTVPGLContext * ctx = _host->DialogHost_GetGLContext();
	GLTextureDrawer * drawer = _host->DialogHost_GetTextureDrawer();
	if (!ctx || !drawer) return;

	int sw = 0, sh = 0;
	ctx->GetSurfaceSize(&sw, &sh);
	if (sw <= 0 || sh <= 0) return;

	// pixel rect (x, y, w, h) → NDC quad に変換 (Y は画面下→上が +)
	// SDLOGLDrawDevice::InitPosition と同じ式に揃えてある。
	const float w2 = sw * 0.5f;
	const float h2 = sh * 0.5f;
	const float left   = (static_cast<float>(x)         - w2) / w2;
	const float right  = (static_cast<float>(x + w)     - w2) / w2;
	const float top    = (static_cast<float>(y)         - h2) / h2;
	const float bottom = (static_cast<float>(y + h)     - h2) / h2;

	float position[8];
	position[0] = left;  position[1] = -bottom; // left top  (NDC で top = -bottom)
	position[2] = left;  position[3] = -top;    // left bottom
	position[4] = right; position[5] = -bottom; // right top
	position[6] = right; position[7] = -top;    // right bottom

	// straight-alpha 合成で描画 (Elements の canvas は ThorVG の Sw raster で
	// 出力された ARGB8888 = メモリ上 BGRA byte order、 GLTexture の BGRA_EXT
	// / swizzle 経路で krkrz 本体と同じ色順で取り扱われる)。
	drawer->DrawTexture(texture, sw, sh, position,
	                    it->second.w, it->second.h, /*blend=*/true);
}

//---------------------------------------------------------------------------
void tTVPOGLDialogRenderer::ReleaseLayer(const void* layer)
{
	auto it = _layers.find(layer);
	if (it == _layers.end()) return;
	DestroyLayer(it->second);
	_layers.erase(it);
}
