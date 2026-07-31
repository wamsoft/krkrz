#ifndef SDL_DRAW_DEVICE_H
#define SDL_DRAW_DEVICE_H

#include "DrawDevice.h"
#include "WindowImpl.h"
#include <SDL3/SDL.h>
#include "SDLTextureUpdateRect.h"
#include "WindowForm.h"
#include "SDLVideoPresenter.h"        // iTVPSDLVideoPresenter / iTVPSDLVideoPresenterHost
#ifdef KRKRZ_HAS_ELEMENTS
#include "elements/DialogRenderer.h"   // iTVPDialogRendererHost
#include <memory>
class tTVPSDLDialogRenderer;
#endif

//---------------------------------------------------------------------------
//! @brief		SDL3 Render APIを使用する描画デバイス
//---------------------------------------------------------------------------
class tTVPSDLDrawDevice : public tTVPDrawDevice
	, public iTVPSDLVideoPresenterHost   // overlay 動画を pull 型で受ける登録口
#ifdef KRKRZ_HAS_ELEMENTS
	, public iTVPDialogRendererHost   // manager→DrawDevice: renderer 提供
#endif
{
	typedef tTVPDrawDevice inherited;

	iTJSDispatch2 *Owner;
	iTJSDispatch2 *Self;
	tTJSVariant WindowObject;
	tTJSNI_Window *NIWindow;

public:
	const tTJSVariant & GetWindowObject() const { return WindowObject; }

	//! @brief Elements ダイアログレンダラ等から SDL_Renderer を借用するための getter
	SDL_Renderer * GetSDLRenderer() const { return mRenderer; }

	//! @brief Elements ダイアログレンダラ等から DestRect を借用するための accessor
	const tTVPRect & GetDestRectExt() const { return DestRect; }

#ifdef KRKRZ_HAS_ELEMENTS
	// iTVPDialogRendererHost — manager が具象型を知らずに renderer を取得する口。
	iTVPDialogRenderer * GetDialogRenderer() override;   // 実体は .cpp (renderer 完全型)
#endif

	tTVPSDLDrawDevice(iTJSDispatch2 *tjs_obj); //!< コンストラクタ

private:
	~tTVPSDLDrawDevice(); //!< デストラクタ

	void InitRenderer(SDL_Window *sdl_wnd);
	void DestroyRenderer();
	void CreateTexture();
	void DestroyTexture();

#ifdef KRKRZ_HAS_ELEMENTS
	//! この DrawDevice が所有する SDL dialog renderer (host = this)。
	std::unique_ptr<tTVPSDLDialogRenderer> DialogRenderer;
#endif

public:
	//---- オブジェクト生存期間制御
	virtual void TJS_INTF_METHOD Destruct();

	//---- window interface 関連
	virtual void TJS_INTF_METHOD SetWindowInterface(iTVPWindow * window);

//---- LayerManager の管理関連
	virtual void TJS_INTF_METHOD AddLayerManager(iTVPLayerManager * manager);

//---- 描画位置・サイズ関連
	virtual void TJS_INTF_METHOD SetTargetWindow(HWND wnd, bool is_main);
	virtual void TJS_INTF_METHOD NotifyLayerResize(iTVPLayerManager * manager);

//---- 再描画関連
	virtual void TJS_INTF_METHOD Show();

//---- LayerManager からの画像受け渡し関連
	virtual void TJS_INTF_METHOD StartBitmapCompletion(iTVPLayerManager * manager);
	virtual void TJS_INTF_METHOD NotifyBitmapCompleted(iTVPLayerManager * manager,
		tjs_int x, tjs_int y, const void * bits, const BITMAPINFO * bitmapinfo,
		const tTVPRect &cliprect, tTVPLayerType type, tjs_int opacity);
	virtual void TJS_INTF_METHOD EndBitmapCompletion(iTVPLayerManager * manager);

	// -----------------------------------------------------
	// VideoOverlay Support (pull 型: presenter host)
	// -----------------------------------------------------
	// 従来の push 型 (UpdateVideo/ClearVideo + 自前 mVideoBuffer) は廃止し、WINVER
	// (BasicDrawDevice) と同じく overlay 動画側 (iTVPSDLVideoPresenter) を登録して
	// Show() から pull する構造へ統一。base の UpdateVideo/ClearVideo は no-op を継承。

public:
	//---- iTVPSDLVideoPresenterHost
	virtual void TJS_INTF_METHOD AddVideoPresenter( iTVPSDLVideoPresenter * presenter );
	virtual void TJS_INTF_METHOD RemoveVideoPresenter( iTVPSDLVideoPresenter * presenter );
	//! 現在 presenter が稼働中か (稼働中はレイヤ描画を省き動画のみ描く)。
	bool HasActiveVideoPresenter() const { return VideoPresenter != nullptr; }

	virtual void SetWaitVSync(bool enable);

private:
	// 描画処理用
	SDL_Renderer *mRenderer;
	SDL_Texture* Texture;
	SDLTextureUpdateRect mTextureUpdateRect;

	// renderer 特性に応じた描画モード:
	//   - mUseFlipOnShow=true  : テクスチャは bottom-up DIB を保持し、Show 時に
	//                             SDL_RenderTextureRotated(SDL_FLIP_VERTICAL) で反転。
	//                             GPU renderer ではこれが最速 (テクスチャ転送に memcpy ゼロ)。
	//   - mUseFlipOnShow=false : テクスチャは top-down (表示順)。Show は SDL_RenderTexture。
	//                             SW renderer ではこちらでないと SW_RenderCopyEx の
	//                             中間サーフェス確保 (3 枚/frame) が走る。
	bool mUseFlipOnShow;
	// テクスチャ確保形式。framebuffer / GPU swapchain と一致させると SW renderer
	// fast path (SDL_StretchSurface 直叩き) に乗り、中間 tmp2 サーフェス確保が消える。
	SDL_PixelFormat mPreferredTextureFormat;

	// overlay 動画 presenter (単一スロット、最後に登録した 1 つを pull する)。
	// フレームバッファ / テクスチャは presenter 側 (SDLVideoPresenter.cpp) が持つ。
	iTVPSDLVideoPresenter *VideoPresenter;
	//! presenter 稼働中に動画を描く (Show() から)。何か描いたら true。
	bool ShowVideo();

	// ビューポート余白 (背景色 + 壁紙)。壁紙テクスチャは base の ViewportWallpaperGen
	// と mWallpaperGen を比較して遅延 (再)アップロードする。
	SDL_Texture *mWallpaperTexture;
	tjs_uint32 mWallpaperGen;
	int mWallpaperW, mWallpaperH;
	void DrawViewportBackground(SDL_Renderer *renderer, int sw, int sh);

	SDL_Texture *CreateTexture(SDL_PixelFormat format, SDL_TextureAccess access, int w, int h);
	void Render(std::function<void(SDL_Renderer *renserer)> func);
};
//---------------------------------------------------------------------------


//---------------------------------------------------------------------------
// tTJSNI_SDLDrawDevice
//---------------------------------------------------------------------------
class tTJSNI_SDLDrawDevice :
	public tTJSNativeInstance
{
	typedef tTJSNativeInstance inherited;

	tTVPSDLDrawDevice * Device;

public:
	tTJSNI_SDLDrawDevice();
	~tTJSNI_SDLDrawDevice();
	tjs_error TJS_INTF_METHOD
		Construct(tjs_int numparams, tTJSVariant **param,
			iTJSDispatch2 *tjs_obj);
	void TJS_INTF_METHOD Invalidate();

public:
	tTVPSDLDrawDevice * GetDevice() const { return Device; }

};
//---------------------------------------------------------------------------



//---------------------------------------------------------------------------
// tTJSNC_SDLDrawDevice
//---------------------------------------------------------------------------
class tTJSNC_SDLDrawDevice : public tTJSNativeClass
{
public:
	tTJSNC_SDLDrawDevice();

	static tjs_uint32 ClassID;

private:
	iTJSNativeInstance *CreateNativeInstance();
};
//---------------------------------------------------------------------------


#endif // SDL_DRAW_DEVICE_H