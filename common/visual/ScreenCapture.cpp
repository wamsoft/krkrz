//---------------------------------------------------------------------------
// 画面キャプチャ要求の受け渡し実装 (SDL3 ビルド)
//---------------------------------------------------------------------------
#include "tjsCommHead.h"
#include "ScreenCapture.h"
#include "LayerBitmapIntf.h"     // tTVPBaseBitmap
#include "GraphicsLoaderIntf.h"  // TVPSaveImage
#include "DebugIntf.h"

#include <mutex>
#include <cstring>
#include <vector>

namespace {
std::mutex g_cap_mu;
bool g_pending = false;
tTVPScreenCaptureReq g_req;

bool  g_last_ok = false;
ttstr g_last_path;
int   g_last_w = 0;
int   g_last_h = 0;
} // anonymous

void TVPRequestScreenCapture(const ttstr& path, int x, int y, int w, int h)
{
	std::lock_guard<std::mutex> lk(g_cap_mu);
	g_req.path = path;
	g_req.x = x; g_req.y = y; g_req.w = w; g_req.h = h;
	g_pending = true;
}

bool TVPHasPendingScreenCapture()
{
	std::lock_guard<std::mutex> lk(g_cap_mu);
	return g_pending;
}

bool TVPTakeScreenCaptureRequest(tTVPScreenCaptureReq& out)
{
	std::lock_guard<std::mutex> lk(g_cap_mu);
	if (!g_pending) return false;
	out = g_req;
	g_pending = false;
	return true;
}

bool TVPSaveCapturedImage(const ttstr& path, const void* pixels,
                          int w, int h, int pitch_bytes,
                          const ttstr& mode)
{
	if (!pixels || w <= 0 || h <= 0) return false;
	try {
		tTVPBaseBitmap bmp((tjs_uint)w, (tjs_uint)h, 32);
		const tjs_uint8* src = reinterpret_cast<const tjs_uint8*>(pixels);
		const int row_bytes = w * 4;
		for (int y = 0; y < h; ++y) {
			void* dst = bmp.GetScanLineForWrite((tjs_uint)y);
			if (!dst) return false;
			std::memcpy(dst, src + (size_t)y * pitch_bytes, row_bytes);
		}
		TVPSaveImage(path, mode, &bmp, nullptr);
		return true;
	} catch (...) {
		TVPAddImportantLog(ttstr(TJS_W("ScreenCapture: save failed: ")) + path);
		return false;
	}
}

bool TVPSaveGLReadback(const tTVPScreenCaptureReq& req,
                       const void* rgba_bottomup, int fullw, int fullh)
{
	if (!rgba_bottomup || fullw <= 0 || fullh <= 0) {
		TVPSetScreenCaptureResult(req.path, 0, 0, false);
		return false;
	}
	// クロップ矩形 (top-down 座標)。 w<=0 で全面。
	int cx = req.x, cy = req.y, cw = req.w, ch = req.h;
	if (cw <= 0 || ch <= 0) { cx = 0; cy = 0; cw = fullw; ch = fullh; }
	// サーフェスからはみ出さないように clamp。
	if (cx < 0) cx = 0; if (cy < 0) cy = 0;
	if (cx >= fullw || cy >= fullh) { TVPSetScreenCaptureResult(req.path, 0, 0, false); return false; }
	if (cx + cw > fullw) cw = fullw - cx;
	if (cy + ch > fullh) ch = fullh - cy;

	const tjs_uint8* src = reinterpret_cast<const tjs_uint8*>(rgba_bottomup);
	std::vector<tjs_uint8> argb((size_t)cw * ch * 4);
	for (int y = 0; y < ch; ++y) {
		// 出力 top-down 行 y → GL bottom-up 行 (fullh-1 - (cy + y))。
		int gly = fullh - 1 - (cy + y);
		const tjs_uint8* srow = src + (size_t)gly * fullw * 4 + (size_t)cx * 4;
		tjs_uint8* drow = argb.data() + (size_t)y * cw * 4;
		for (int x = 0; x < cw; ++x) {
			// src = R,G,B,A → dst (ARGB8888 メモリ) = B,G,R,A
			drow[x*4 + 0] = srow[x*4 + 2];
			drow[x*4 + 1] = srow[x*4 + 1];
			drow[x*4 + 2] = srow[x*4 + 0];
			drow[x*4 + 3] = srow[x*4 + 3];
		}
	}
	bool ok = TVPSaveCapturedImage(req.path, argb.data(), cw, ch, cw * 4,
	                               ttstr(TJS_W("png")));
	TVPSetScreenCaptureResult(req.path, cw, ch, ok);
	return ok;
}

void TVPSetScreenCaptureResult(const ttstr& path, int w, int h, bool ok)
{
	std::lock_guard<std::mutex> lk(g_cap_mu);
	g_last_path = path;
	g_last_w = w; g_last_h = h; g_last_ok = ok;
}

bool TVPGetLastScreenCapture(ttstr& path, int& w, int& h, bool& ok)
{
	std::lock_guard<std::mutex> lk(g_cap_mu);
	path = g_last_path;
	w = g_last_w; h = g_last_h; ok = g_last_ok;
	return !g_last_path.IsEmpty();
}
