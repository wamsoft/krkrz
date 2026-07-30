//---------------------------------------------------------------------------
// SimpleImageLoad — 内蔵デコーダで RGBA8 バッファを吐く簡易ラッパー実装
//---------------------------------------------------------------------------
#include "tjsCommHead.h"
#include "SimpleImageLoad.h"
#include "GraphicsLoaderIntf.h"
#include "UtilStreams.h"

#include <stdlib.h>
#include <new>

//---------------------------------------------------------------------------
void tTVPSimpleImage::Free()
{
	if (pixels) {
		free(pixels);
		pixels = nullptr;
	}
	width = height = 0;
}
//---------------------------------------------------------------------------

namespace {

	// デコーダの sink コールバックが書き込む先を提供するコンテキスト
	struct tSinkContext
	{
		tTVPSimpleImage *img;
		tjs_uint         stride;   // = width*4
		bool             ok;       // サイズ確定+確保に成功したか
	};

	// 画像サイズ通知: ここで RGBA8 バッファを確保する
	void SizeCallback(void *cbdata, tjs_uint w, tjs_uint h)
	{
		tSinkContext *ctx = reinterpret_cast<tSinkContext *>(cbdata);
		ctx->img->Free();
		if (w == 0 || h == 0) { ctx->ok = false; return; }
		ctx->stride = w * 4;
		tjs_uint8 *buf = reinterpret_cast<tjs_uint8 *>(malloc((size_t)ctx->stride * h));
		if (!buf) { ctx->ok = false; return; }
		ctx->img->width  = w;
		ctx->img->height = h;
		ctx->img->pixels = buf;
		ctx->ok = true;
	}

	// 各スキャンラインの書き込み先を返す。y=-1 は「前行の書き込み完了」通知で
	// NULL を返す約束。範囲外/未確保でも NULL を返して停止させる。
	void *ScanLineCallback(void *cbdata, tjs_int y)
	{
		tSinkContext *ctx = reinterpret_cast<tSinkContext *>(cbdata);
		if (!ctx->ok || y < 0) return nullptr;
		if ((tjs_uint)y >= ctx->img->height) return nullptr;
		return ctx->img->pixels + (size_t)ctx->stride * (tjs_uint)y;
	}

	// 先頭マジックでフォーマットを判定してデコード
	bool DecodeByMagic(TJS::iTJSBinaryStream *src, tTVPSimpleImage &out)
	{
		out.Free();

		tjs_uint8 magic[8] = {0};
		src->Seek(0, TJS_BS_SEEK_SET);
		tjs_uint n = src->Read(magic, sizeof(magic));
		src->Seek(0, TJS_BS_SEEK_SET);

		tSinkContext ctx = { &out, 0, false };

		try {
			if (n >= 8 &&
				magic[0] == 0x89 && magic[1] == 0x50 && magic[2] == 0x4E && magic[3] == 0x47 &&
				magic[4] == 0x0D && magic[5] == 0x0A && magic[6] == 0x1A && magic[7] == 0x0A) {
				// PNG
				TVPLoadPNG(nullptr, &ctx, SizeCallback, ScanLineCallback, nullptr, src, 0, glmNormalRGBA);
			} else if (n >= 3 && magic[0] == 0xFF && magic[1] == 0xD8 && magic[2] == 0xFF) {
				// JPEG
				TVPLoadJPEG(nullptr, &ctx, SizeCallback, ScanLineCallback, nullptr, src, 0, glmNormalRGBA);
			} else {
				return false;
			}
		} catch (...) {
			out.Free();
			return false;
		}

		if (!ctx.ok || out.pixels == nullptr) {
			out.Free();
			return false;
		}
		return true;
	}

} // namespace

//---------------------------------------------------------------------------
bool TVPLoadSimpleImage(TJS::iTJSBinaryStream *src, tTVPSimpleImage &out)
{
	if (!src) return false;
	return DecodeByMagic(src, out);
}
//---------------------------------------------------------------------------
bool TVPLoadSimpleImageFromMemory(const void *data, tjs_uint size, tTVPSimpleImage &out)
{
	out.Free();
	if (!data || size == 0) return false;
	tTVPMemoryStream stream(data, size);
	return DecodeByMagic(&stream, out);
}
//---------------------------------------------------------------------------
