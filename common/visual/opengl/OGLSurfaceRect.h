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
//
// 縦横は同じ倍率にして中央寄せする (SDL_Renderer 経路の
// SDL_LOGICAL_PRESENTATION_LETTERBOX と同じ見え方)。 縦横別倍率にすると
// 論理面とアスペクト比が違う画面 (iPad の 4:3、 縦長スマホ等) で引き伸ばされる。
// 余白は DrawDevice のクリア色 (ビューポート背景色) になる。
//---------------------------------------------------------------------------
inline tTVPRect TVPScaleRectToSurface(const tTVPRect &r, int logW, int logH,
                                      int physW, int physH)
{
	if (logW <= 0 || logH <= 0 || physW <= 0 || physH <= 0) return r;
	if (logW == physW && logH == physH) return r;
	double sx = (double)physW / logW;
	double sy = (double)physH / logH;
	double s  = sx < sy ? sx : sy;
	double ox = (physW - logW * s) * 0.5;
	double oy = (physH - logH * s) * 0.5;
	tTVPRect out;
	out.left   = (tjs_int)(r.left   * s + ox + 0.5);
	out.right  = (tjs_int)(r.right  * s + ox + 0.5);
	out.top    = (tjs_int)(r.top    * s + oy + 0.5);
	out.bottom = (tjs_int)(r.bottom * s + oy + 0.5);
	return out;
}

#endif // __OGL_SURFACE_RECT_H__
