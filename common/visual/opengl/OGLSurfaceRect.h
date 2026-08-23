#ifndef __OGL_SURFACE_RECT_H__
#define __OGL_SURFACE_RECT_H__

//---------------------------------------------------------------------------
// 論理サーフェス座標 → GL 実フレームバッファ座標
//
// DrawDevice が受け取る DestRect は「ウィンドウの inner サイズ (論理サーフェス)」
// 座標で来る。 一方 GL 経路は glViewport も Elements の dialog も
// iTVPGLContext::GetSurfaceSize (= 実フレームバッファ) 基準で描く。
//
// 常にフルスクリーンで実フレームバッファがディスプレイ解像度になる環境
// (PS5 等。 論理 1920x1080 に対し実 3840x2160) では両者が食い違うので、
// GL へ渡す前にここで実サイズ側へ移す。 一致する環境 (デスクトップ / NX)
// では比が 1 なので素通しになる。
//---------------------------------------------------------------------------
inline tTVPRect TVPScaleRectToSurface(const tTVPRect &r, int logW, int logH,
                                      int physW, int physH)
{
	if (logW <= 0 || logH <= 0 || physW <= 0 || physH <= 0) return r;
	if (logW == physW && logH == physH) return r;
	double sx = (double)physW / logW;
	double sy = (double)physH / logH;
	tTVPRect out;
	out.left   = (tjs_int)(r.left   * sx + 0.5);
	out.right  = (tjs_int)(r.right  * sx + 0.5);
	out.top    = (tjs_int)(r.top    * sy + 0.5);
	out.bottom = (tjs_int)(r.bottom * sy + 0.5);
	return out;
}

#endif // __OGL_SURFACE_RECT_H__
