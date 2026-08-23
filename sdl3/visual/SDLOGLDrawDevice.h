#ifndef SDL_OGL_DRAW_DEVICE_H
#define SDL_OGL_DRAW_DEVICE_H

#include "DrawDevice.h"
#include "WindowImpl.h"
#include "OpenGLHeader.h"
#include "SDLOGLTextureUpdateRect.h"
#include "GLTexture.h"
#include "GLVideoPresenter.h"   // iTVPGLVideoPresenter / iTVPGLVideoPresenterHost
#include "OGLViewportBackground.h"
#include <SDL3/SDL.h>
#include <functional>
#include <mutex>
#include <memory>

#ifdef KRKRZ_HAS_ELEMENTS
#include "OGLDialogRenderer.h"
#endif

//---------------------------------------------------------------------------
//! @brief	SDL3 + OpenGL ES 直接版 DrawDevice
//
// SDLDrawDevice は SDL_Renderer (= SDL3 backend に依存) でテクスチャ更新を行うが、
// SDL_UpdateTexture per-rect が Switch 実機で main CPU を 200-400 ms/s 食う問題が
// あった (handoff_graphics_mt_redesign.md Phase 9 まで)。
//
// このクラスは backend が opengl のときに SDL_Renderer 経路を使わず、
// OpenGL ES 直接 (PBO 経由) でテクスチャを更新する DrawDevice。OGLDrawDevice
// (Canvas / Texture / Shader 等の TJS API を持つフル機能版) とは別物で、
// 「kirikiri DrawDevice + 動画再生用画面描画」のみのシンプル版として動作する。
//
// 実装は OGLDrawDevice の __GENERIC__ 経路をベースに、Canvas / Matrix32 等の
// TJS 公開ロジックを除いたもの。GL context は同じ SDL_Window 経由で
// iTVPGLContext::GetContext で取得 (= OGLDrawDevice と context 共有可)。
//---------------------------------------------------------------------------
class tTVPSDLOGLDrawDevice : public tTVPDrawDevice
	, public iTVPGLVideoPresenterHost   // overlay 動画を pull 型で受ける登録口
#ifdef KRKRZ_HAS_ELEMENTS
	, public iTVPGLDialogHost         // renderer→DrawDevice: GL リソース借用
	, public iTVPDialogRendererHost   // manager→DrawDevice: renderer 提供
#endif
{
	typedef tTVPDrawDevice inherited;

	iTJSDispatch2 *Owner;
	iTJSDispatch2 *Self;
	tTJSVariant WindowObject;
	//! 論理サーフェス (inner) サイズの問い合わせ先。 SetWindowInterface で取得。
	class tTJSNI_Window *NIWindow;

	class iTVPGLContext *GLContext;
	GLTextureDrawer TextureDrawer;

	// primaryLayer 描画用テクスチャ (OGL 経由、内部 Native instance だが TJS 公開しない)
	tTJSNativeClass *TextureClass;
	tTJSVariant TextureObject;
	class tTJSNI_Texture *TextureInstance;
	SDLOGLTextureUpdateRect mTextureUpdateRect;

	GLfloat _position[8];
	//! GL の実フレームバッファサイズ (glViewport / 読み戻しの基準)。
	//! 常にフルスクリーンの環境 (PS5 等) ではウィンドウの inner サイズ
	//! (論理サーフェス) と一致しないので、 DestRect との換算が要る。
	int SurfaceWidth;
	int SurfaceHeight;

	//! DestRect (論理サーフェス座標) を GL の実フレームバッファ座標へ移す。
	//! 論理と物理が同じ環境 (デスクトップ / NX) では素通し。
	tTVPRect ToPhysicalRect(const tTVPRect &r, int physW, int physH) const;

	~tTVPSDLOGLDrawDevice();

	void CreateTexture();
	void DestroyTexture();

	void InitPosition();

	void InitContext(void *nativeWindow);
	void DoneContext();

public:
	const tTJSVariant & GetWindowObject() const { return WindowObject; }
	const tTJSVariant & GetTextureObject() const { return TextureObject; }

public:
	tTVPSDLOGLDrawDevice(iTJSDispatch2 *tjs_obj);

	//---- オブジェクト生存期間制御
	virtual void TJS_INTF_METHOD Destruct();

	//---- window interface 関連
	virtual void TJS_INTF_METHOD SetWindowInterface(iTVPWindow * window);

	//---- LayerManager の管理関連
	virtual void TJS_INTF_METHOD AddLayerManager(iTVPLayerManager * manager);

	//---- 描画位置・サイズ関連
	virtual void TJS_INTF_METHOD SetTargetWindow(HWND wnd, bool is_main);
	virtual void TJS_INTF_METHOD SetDestRectangle(const tTVPRect & rect);
	virtual void TJS_INTF_METHOD NotifyLayerResize(iTVPLayerManager * manager);

	//---- 表示処理
	virtual void TJS_INTF_METHOD Show();

	//---- LayerManager からの画像受け渡し関連
	virtual void TJS_INTF_METHOD StartBitmapCompletion(iTVPLayerManager * manager);
	virtual void TJS_INTF_METHOD NotifyBitmapCompleted(iTVPLayerManager * manager,
		tjs_int x, tjs_int y, const void * bits, const BITMAPINFO * bitmapinfo,
		const tTVPRect &cliprect, tTVPLayerType type, tjs_int opacity);
	virtual void TJS_INTF_METHOD EndBitmapCompletion(iTVPLayerManager * manager);

	// -----------------------------------------------------
	// VideoOverlay Support (pull 型: presenter host、WINVER/SDL/OGL と統一)
	// -----------------------------------------------------
public:
	//---- iTVPGLVideoPresenterHost
	virtual void TJS_INTF_METHOD AddVideoPresenter( iTVPGLVideoPresenter * presenter );
	virtual void TJS_INTF_METHOD RemoveVideoPresenter( iTVPGLVideoPresenter * presenter );
	//! 現在 presenter が稼働中か (稼働中はレイヤ描画を省き動画のみ描く)。
	bool HasActiveVideoPresenter() const { return VideoPresenter != nullptr; }

	virtual void SetWaitVSync(bool enable);

private:
	// ビューポート余白の壁紙テクスチャキャッシュ (背景色は base が保持)
	tTVPGLWallpaperCache mWallpaperCache;

	// overlay 動画 presenter (単一スロット)。フレームバッファ / GLTexture は
	// presenter 側 (GLVideoPresenter.cpp) が持つ。
	iTVPGLVideoPresenter *VideoPresenter;
	bool ShowVideo();

#ifdef KRKRZ_HAS_ELEMENTS
public:
	// iTVPGLDialogHost — Elements ダイアログ overlay 用に GL リソース / DestRect
	// を借用させるための accessor。GLContext は InitContext 前 / DoneContext 後は
	// nullptr を返す。
	iTVPGLContext * DialogHost_GetGLContext() override { return GLContext; }
	GLTextureDrawer * DialogHost_GetTextureDrawer() override { return &TextureDrawer; }
	void DialogHost_GetDestRect(int & x, int & y, int & w, int & h) override
	{
		x = DestRect.left;
		y = DestRect.top;
		w = DestRect.get_width();
		h = DestRect.get_height();
	}

	// iTVPDialogRendererHost — manager が具象型を知らずに renderer を取得する口。
	iTVPDialogRenderer * GetDialogRenderer() override { return DialogRenderer.get(); }
private:
	//! この DrawDevice が所有する OGL dialog renderer (host = this)。InitContext で
	//! 生成、DoneContext で破棄 (GL context 寿命に一致)。
	std::unique_ptr<tTVPOGLDialogRenderer> DialogRenderer;
public:
#endif
};
//---------------------------------------------------------------------------


//---------------------------------------------------------------------------
// tTJSNI_SDLOGLDrawDevice
//---------------------------------------------------------------------------
class tTJSNI_SDLOGLDrawDevice :
	public tTJSNativeInstance
{
	typedef tTJSNativeInstance inherited;

	tTVPSDLOGLDrawDevice * Device;

public:
	tTJSNI_SDLOGLDrawDevice();
	~tTJSNI_SDLOGLDrawDevice();
	tjs_error TJS_INTF_METHOD
		Construct(tjs_int numparams, tTJSVariant **param,
			iTJSDispatch2 *tjs_obj);
	void TJS_INTF_METHOD Invalidate();

public:
	tTVPSDLOGLDrawDevice * GetDevice() const { return Device; }
};
//---------------------------------------------------------------------------


//---------------------------------------------------------------------------
// tTJSNC_SDLOGLDrawDevice
//---------------------------------------------------------------------------
class tTJSNC_SDLOGLDrawDevice : public tTJSNativeClass
{
public:
	tTJSNC_SDLOGLDrawDevice();

	static tjs_uint32 ClassID;

private:
	iTJSNativeInstance *CreateNativeInstance();
};
//---------------------------------------------------------------------------


#endif // SDL_OGL_DRAW_DEVICE_H
