//---------------------------------------------------------------------------
// SimpleImageLoad
//   吉里吉里内蔵の画像デコーダ (LoadPNG / LoadJPEG) を薄くラップし、
//   tTVPBaseBitmap を介さずに「単純な RGBA8 バッファ」へ展開する簡易 IF。
//   ホスト側 (splash / 権利表記など) やプラグインが手軽に使える。
//
//   出力は glmNormalRGBA = メモリ上 R,G,B,A バイト順 (GPU ネイティブ)。
//   OpenGL の GL_RGBA / nn::oe の R8_G8_B8_A8 にそのまま渡せる。
//
//   フォーマットは先頭のマジックバイトで自動判定 (PNG / JPEG)。storage には
//   依存せず、任意の iTJSBinaryStream / メモリブロックから読める。
//---------------------------------------------------------------------------
#ifndef SimpleImageLoadH
#define SimpleImageLoadH

#include "tjsCommHead.h"

namespace TJS { class iTJSBinaryStream; }

//---------------------------------------------------------------------------
// デコード結果。pixels は width*height*4 バイトの RGBA8 (所有権は本構造体)。
struct tTVPSimpleImage
{
	tjs_uint   width  = 0;
	tjs_uint   height = 0;
	tjs_uint8 *pixels = nullptr; // RGBA8, width*height*4, malloc 確保

	tTVPSimpleImage() = default;
	~tTVPSimpleImage() { Free(); }

	// コピー禁止 (所有ポインタの二重解放防止)
	tTVPSimpleImage(const tTVPSimpleImage &) = delete;
	tTVPSimpleImage &operator=(const tTVPSimpleImage &) = delete;

	void Free();                     // pixels を解放し空に戻す
	tjs_uint8 *Detach() { tjs_uint8 *p = pixels; pixels = nullptr; width = height = 0; return p; }
};

//---------------------------------------------------------------------------
// src の現在位置以降を読み、RGBA8 へデコードして out に格納する。
// 対応形式は先頭マジックで判定 (PNG / JPEG)。失敗時 false (out は空)。
bool TVPLoadSimpleImage(TJS::iTJSBinaryStream *src, tTVPSimpleImage &out);

// メモリブロックから読む版 (data は関数内でのみ参照)。
bool TVPLoadSimpleImageFromMemory(const void *data, tjs_uint size, tTVPSimpleImage &out);

#endif
