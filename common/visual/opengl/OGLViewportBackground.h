//---------------------------------------------------------------------------
/*
	GL 系 DrawDevice (tTVPOGLDrawDevice / tTVPSDLOGLDrawDevice) 共用の
	ビューポート余白壁紙描画ヘルパ。

	glClear (背景色) 済みの状態で TVPDrawGLViewportWallpaper() を呼ぶと、
	壁紙ビットマップを surface 上にフィット配置して描画する (ゲーム描画より前)。

	壁紙テクスチャは tTVPGLWallpaperCache が世代カウンタ付きで保持し、
	tTVPDrawDevice::GetViewportWallpaperGen() の変化時のみ再アップロードする。
	GL 操作は render thread (Show) でのみ行うこと ([[feedback_gl_thread_boundary]])。
*/
//---------------------------------------------------------------------------
#ifndef OGLViewportBackgroundH
#define OGLViewportBackgroundH

#include "GLTexture.h"
#include "ViewportConfig.h"
#include "DrawDevice.h"  // tTVPDrawDevice (壁紙オブジェクトから画像取得)
#include <cstring>

//---------------------------------------------------------------------------
//! @brief	GL DrawDevice ごとの壁紙テクスチャキャッシュ
//---------------------------------------------------------------------------
struct tTVPGLWallpaperCache {
	GLTexture *texture;
	tjs_uint32 gen;
	int w, h;
	tTVPGLWallpaperCache() : texture(nullptr), gen(0), w(0), h(0) {}
	void Release() { if (texture) { delete texture; texture = nullptr; } w = h = 0; }
};

//---------------------------------------------------------------------------
//! @brief	ビューポート余白壁紙を描画する (glClear 後・ゲーム描画前に呼ぶ)
//! @param	drawer		GLTextureDrawer (描画デバイスの所有物)
//! @param	cache		壁紙テクスチャキャッシュ (描画デバイスの所有物)
//! @param	dev			壁紙オブジェクト/設定を保持する描画デバイス基底。
//!						壁紙イメージは imageWidth/mainImageBuffer 等のプロパティ経由で取得。
//! @param	surfaceW,surfaceH	外側 surface サイズ
//---------------------------------------------------------------------------
inline void TVPDrawGLViewportWallpaper(GLTextureDrawer &drawer,
	tTVPGLWallpaperCache &cache, const tTVPDrawDevice &dev,
	int surfaceW, int surfaceH)
{
	tjs_uint32 wpGen = dev.GetViewportWallpaperGen();
	if (wpGen != cache.gen) { cache.Release(); cache.gen = wpGen; }

	if (!cache.texture) {
		// 壁紙オブジェクト (Layer/Bitmap) からプロパティ経由で画像イメージを取得する。
		// PropGet を伴うので世代変化時 (= テクスチャ未作成時) のみ行う。
		tjs_int wpw, wph, srcpitch;
		const tjs_uint8 *buffer;
		if (!dev.GetViewportWallpaperImage(wpw, wph, srcpitch, buffer)) return;

		cache.texture = new GLTexture(wpw, wph); // GL_RGBA
		cache.w = wpw; cache.h = wph;
		// kirikiri bitmap は ARGB8888 (メモリ B,G,R,A) → GL_RGBA (R,G,B,A) へ swizzle。
		cache.texture->UpdateTexture(0, 0, wpw, wph,
			[buffer, wpw, wph, srcpitch](char *Dest, int pitch) {
				for (int y = 0; y < wph; ++y) {
					const tjs_uint8 *s = buffer + (size_t)y * srcpitch;
					tjs_uint8 *d = (tjs_uint8 *)Dest + (size_t)y * pitch;
					for (int x = 0; x < wpw; ++x) {
						d[x * 4 + 0] = s[x * 4 + 2];
						d[x * 4 + 1] = s[x * 4 + 1];
						d[x * 4 + 2] = s[x * 4 + 0];
						d[x * 4 + 3] = s[x * 4 + 3];
					}
				}
			});
	}
	if (!cache.texture) return;

	tTVPViewportConfig wcfg;
	wcfg.fit = dev.GetViewportWallpaperFit();
	wcfg.alignX = dev.GetViewportWpAlignX();
	wcfg.alignY = dev.GetViewportWpAlignY();
	tTVPRect r = TVPCalcViewportDestRect(wcfg, surfaceW, surfaceH, cache.w, cache.h);

	int w2 = surfaceW / 2, h2 = surfaceH / 2;
	if (w2 <= 0 || h2 <= 0) return;
	float left   = (float)(r.left   - w2) / w2;
	float right  = (float)(r.right  - w2) / w2;
	float bottom = (float)(r.bottom - h2) / h2;
	float top    = (float)(r.top    - h2) / h2;
	GLfloat pos[8] = {
		left,  -bottom, // left top
		left,  -top,    // left bottom
		right, -bottom, // right top
		right, -top,    // right bottom
	};
	drawer.DrawTexture(cache.texture, surfaceW, surfaceH, pos, cache.w, cache.h, false);
}

//---------------------------------------------------------------------------
#endif
