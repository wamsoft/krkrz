#define NOMINMAX
#include "tjsCommHead.h"
#include "DrawDevice.h"
#include "SDLDrawDevice.h"
#include "LayerIntf.h"
#include "MsgImpl.h"
#include "SysInitIntf.h"
#include "WindowIntf.h"
#include "DebugIntf.h"
#include "ThreadIntf.h"
#include "ComplexRect.h"
#include "EventIntf.h"
#include "WindowImpl.h"
#include "LogIntf.h"
#include "MemoryOverlayRender.h"
#include "PadOverlayRender.h"
#include "PostRenderCallback.h"
#include "ViewportConfig.h"  // tTVPViewportConfig (壁紙の配置計算)
#ifdef KRKRZ_USE_REPL
#include "ScreenCapture.h"
#endif

#include <SDL3/SDL.h>
#include <algorithm>
#include <chrono>

#include "app.h"

#ifdef KRKRZ_HAS_ELEMENTS
#include "elements/ElementsDialogManager.h"
#include "SDLDialogRenderer.h"
#endif

// section 計測は ThreadIntf.h の TVPRenderStatsScopedTimer を経由する。

#ifdef KRKRZ_USE_REPL
//---------------------------------------------------------------------------
// エージェント駆動: 保留中の画面キャプチャ要求を、 全 overlay 合成後・
// SDL_RenderPresent 直前のタイミングで読み戻して保存する。
//---------------------------------------------------------------------------
static void FulfillScreenCapture(SDL_Renderer* renderer)
{
	if (!renderer || !TVPHasPendingScreenCapture()) return;
	tTVPScreenCaptureReq req;
	if (!TVPTakeScreenCaptureRequest(req)) return;

	SDL_Rect r;
	SDL_Rect* rp = nullptr;
	if (req.w > 0 && req.h > 0) {
		r.x = req.x; r.y = req.y; r.w = req.w; r.h = req.h;
		rp = &r;
	}

	SDL_Surface* surf = SDL_RenderReadPixels(renderer, rp);
	if (!surf) {
		TVPAddImportantLog(ttstr(TJS_W("ScreenCapture: SDL_RenderReadPixels failed: "))
			+ ttstr(SDL_GetError()));
		TVPSetScreenCaptureResult(req.path, 0, 0, false);
		return;
	}
	// krkrz bitmap は ARGB8888 (メモリ上 B,G,R,A) 前提なので合わせる。
	SDL_Surface* conv = (surf->format == SDL_PIXELFORMAT_ARGB8888)
		? surf : SDL_ConvertSurface(surf, SDL_PIXELFORMAT_ARGB8888);
	bool ok = false;
	if (conv) {
		ok = TVPSaveCapturedImage(req.path, conv->pixels, conv->w, conv->h,
		                          conv->pitch, ttstr(TJS_W("png")));
		TVPSetScreenCaptureResult(req.path, conv->w, conv->h, ok);
		if (conv != surf) SDL_DestroySurface(conv);
	} else {
		TVPSetScreenCaptureResult(req.path, 0, 0, false);
	}
	SDL_DestroySurface(surf);
	if (ok) TVPAddLog(ttstr(TJS_W("ScreenCapture: saved ")) + req.path);
}
#endif // KRKRZ_USE_REPL

//---------------------------------------------------------------------------
// オプション
//---------------------------------------------------------------------------
static tjs_int TVPSDLDrawDeviceOptionsGeneration = 0;
bool TVPZoomInterpolation = true;
// SDL3 renderer backend を `-renderer=...` で明示指定する場合の値 (空ならデフォルト)。
// 例: "opengl" / "opengles2" / "vulkan" / "direct3d11" / "direct3d12" / "metal" / "software"
// SDL_CreateRenderer の name 引数にそのまま渡される (= SDL_GetRendererName が返す名前)。
static std::string TVPSDLRendererName;
//---------------------------------------------------------------------------
static void TVPInitSDLDrawDeviceOptions()
{
	if(TVPSDLDrawDeviceOptionsGeneration == TVPGetCommandLineArgumentGeneration()) return;
	TVPSDLDrawDeviceOptionsGeneration = TVPGetCommandLineArgumentGeneration();

	tTJSVariant val;
	TVPZoomInterpolation = true;
	if(TVPGetCommandLine(TJS_W("-smoothzoom"), &val))
	{
		ttstr str(val);
		if(str == TJS_W("no"))
			TVPZoomInterpolation = false;
		else
			TVPZoomInterpolation = true;
	}

	TVPSDLRendererName.clear();
	if(TVPGetCommandLine(TJS_W("-renderer"), &val))
	{
		tjs_string s = val.GetString();
		TVPUtf16ToUtf8(TVPSDLRendererName, s.c_str());
	}
}
//---------------------------------------------------------------------------

//---------------------------------------------------------------------------
tTVPSDLDrawDevice::tTVPSDLDrawDevice(iTJSDispatch2 *self)
 : Owner(NULL)
 , Self(NULL)
 , NIWindow(NULL)
 , Texture(NULL)
 , mRenderer(nullptr)
 , mUseFlipOnShow(true)
 , mPreferredTextureFormat(SDL_PIXELFORMAT_XRGB8888)
 , VideoPresenter(nullptr)
 , mWallpaperTexture(nullptr)
 , mWallpaperGen(0)
 , mWallpaperW(0)
 , mWallpaperH(0)
{
	if (Self) Self->AddRef();
	TVPInitSDLDrawDeviceOptions();

	// overlay 動画 presenter factory を登録 (VideoOverlay が pull 経路で使う)。冪等。
	TVPRegisterSDLVideoOverlayPresenterFactory();

#ifdef KRKRZ_HAS_ELEMENTS
	// dialog renderer を DrawDevice 自身が所有し、iTVPDialogRendererHost (this) として
	// manager に登録する。renderer は host (this) から SDL_Renderer 等を借用する。
	DialogRenderer = std::make_unique<tTVPSDLDialogRenderer>(this);
	tTVPElementsDialogManager::Instance().RegisterDialogHost(this, this);
#endif
}
//---------------------------------------------------------------------------
tTVPSDLDrawDevice::~tTVPSDLDrawDevice()
{
#ifdef KRKRZ_HAS_ELEMENTS
	tTVPElementsDialogManager::Instance().UnregisterDialogHost(this);
#endif
	DestroyRenderer();
}
#ifdef KRKRZ_HAS_ELEMENTS
//---------------------------------------------------------------------------
iTVPDialogRenderer * tTVPSDLDrawDevice::GetDialogRenderer()
{
	return DialogRenderer.get();
}
#endif
//---------------------------------------------------------------------------
void tTVPSDLDrawDevice::InitRenderer(SDL_Window *sdl_wnd)
{
	// 起動オプション `-renderer=<name>` で SDL3 backend を明示指定可能。空ならデフォルト。
	const char *requested = TVPSDLRendererName.empty() ? NULL : TVPSDLRendererName.c_str();
	mRenderer = SDL_CreateRenderer(sdl_wnd, requested);
	if (!mRenderer) {
		const char *err = SDL_GetError();
		TVPLOG_ERROR("tTVPSDLDrawDevice::InitRenderer() failed (requested={}):{}",
			requested ? requested : "(default)", err);
		return;
	}
	const char *backend = SDL_GetRendererName(mRenderer);
	TVPLOG_INFO("tTVPSDLDrawDevice::InitRenderer renderer={} (requested={})",
		backend ? backend : "?",
		requested ? requested : "(default)");

	// --- sRGB 調査用診断 (PS5 AGC / NX Vulkan の washed-out / 白い 解析) ---
	// 症状は SDL_GPU backend 共通。原因切り分けの核心は「最終 swapchain が
	// 実際に _SRGB encode する format か否か」。これだけ実機ログで確定すれば
	// 正解パッチ (UNORM 全段 passthrough 化 / SRGB_LINEAR 対称化) が一意に決まる。
	// renderer プロパティ経由で GPU device と output colorspace を引き、
	// SDL_GetGPUSwapchainTextureFormat で実 swapchain format を出す。
	// MASTER でも見えるよう WARNING で出す (調査完了後に削除予定)。
	{
		SDL_PropertiesID rprops = SDL_GetRendererProperties(mRenderer);
		const SDL_Colorspace cs = (SDL_Colorspace)SDL_GetNumberProperty(
			rprops, SDL_PROP_RENDERER_OUTPUT_COLORSPACE_NUMBER, SDL_COLORSPACE_SRGB);
		SDL_GPUDevice *gpu = (SDL_GPUDevice *)SDL_GetPointerProperty(
			rprops, SDL_PROP_RENDERER_GPU_DEVICE_POINTER, NULL);
		if (gpu) {
			const SDL_GPUTextureFormat scfmt = SDL_GetGPUSwapchainTextureFormat(gpu, sdl_wnd);
			const bool sc_is_srgb =
				(scfmt == SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM_SRGB ||
				 scfmt == SDL_GPU_TEXTUREFORMAT_B8G8R8A8_UNORM_SRGB);
			TVPLOG_WARNING("[srgb-diag] GPU backend: output_colorspace=0x{:08x} swapchain_gpu_format={} srgb_encoding_swapchain={}",
				(unsigned)cs, (int)scfmt, sc_is_srgb ? "YES" : "no");
		} else {
			TVPLOG_WARNING("[srgb-diag] non-GPU backend: output_colorspace=0x{:08x}",
				(unsigned)cs);
		}
	}

	// renderer 特性に応じた描画モードを決定。
	// SW renderer は SDL_FLIP_VERTICAL が SW_RenderCopyEx 経由になり中間サーフェスを
	// 毎フレーム alloc するため、top-down 配置 + 反転なし表示にする。さらに texture format を
	// framebuffer 形式に揃えて SDL_BlitSurfaceScaled の fast path (SDL_StretchSurface 直叩き)
	// に乗せる。
	const bool is_software = backend && SDL_strcasecmp(backend, "software") == 0;
	mUseFlipOnShow = !is_software;

	// renderer が公開する texture format リストの先頭 32bit RGB(A) を採用。
	// SW renderer は framebuffer 形式を先頭に並べるので、これで自然に一致する。
	// GPU renderer 群は ARGB8888 (BGRA32) 系を優先的に並べるので DIB と byte order が一致。
	mPreferredTextureFormat = SDL_PIXELFORMAT_XRGB8888;
	SDL_PropertiesID props = SDL_GetRendererProperties(mRenderer);
	if (props) {
		const SDL_PixelFormat *fmts = (const SDL_PixelFormat *)SDL_GetPointerProperty(
			props, SDL_PROP_RENDERER_TEXTURE_FORMATS_POINTER, NULL);
		if (fmts) {
			for (int i = 0; fmts[i] != SDL_PIXELFORMAT_UNKNOWN; ++i) {
				const SDL_PixelFormat f = fmts[i];
				if (f == SDL_PIXELFORMAT_XRGB8888 || f == SDL_PIXELFORMAT_ARGB8888 ||
					f == SDL_PIXELFORMAT_XBGR8888 || f == SDL_PIXELFORMAT_ABGR8888) {
					mPreferredTextureFormat = f;
					break;
				}
			}
		}
	}
	TVPLOG_INFO("tTVPSDLDrawDevice: useFlipOnShow={} textureFormat={}",
		mUseFlipOnShow, SDL_GetPixelFormatName(mPreferredTextureFormat));

	CreateTexture();
}
//---------------------------------------------------------------------------
void tTVPSDLDrawDevice::DestroyRenderer()
{
	if (mRenderer) {
		// presenter が保持するテクスチャは renderer 破棄前に手放させる。
		VideoPresenter = nullptr;
		DestroyTexture();
		if (mWallpaperTexture) {
			SDL_DestroyTexture(mWallpaperTexture);
			mWallpaperTexture = nullptr;
		}
		SDL_DestroyRenderer(mRenderer);
		mRenderer = nullptr;
	}
}
//---------------------------------------------------------------------------
void tTVPSDLDrawDevice::DestroyTexture() 
{
	if(Texture) {
		SDL_DestroyTexture(Texture);
		Texture = NULL;
	}
}
//---------------------------------------------------------------------------
void tTVPSDLDrawDevice::CreateTexture() 
{
	if (!Texture) {
		tjs_int w, h;
		GetSrcSize( w, h );
		TVPLOG_INFO("tTVPSDLDrawDevice::CreateTexture() {}x{}", w, h);
		if (w > 0 && h > 0) {
			// 内部 bitmap は DIB と同じ BGRA byte order (memory: B,G,R,A)。
			// XRGB8888 / ARGB8888 (LE) は B,G,R,X / B,G,R,A で DIB と一致するため R/B swap 不要。
			// XBGR8888 / ABGR8888 (LE) は R,G,B,X / R,G,B,A で R/B swap が要る。
			// mPreferredTextureFormat は renderer の好む format で、SW renderer のときは
			// framebuffer (PS5 では XBGR8888) に揃うので fast path に乗る代わりに swap が要る。
			Texture = CreateTexture(mPreferredTextureFormat, SDL_TEXTUREACCESS_STREAMING, w, h);
			if( !Texture ) {
				const char *err = SDL_GetError();
				TVPLOG_ERROR("tTVPSDLDrawDevice::CreateTexture() failed:{}", err);
				TVPThrowExceptionMessage(TVPCannotAllocateSDLTexture);
				return;
			}
			void *textureBuffers;
			int texturePitch;
			if (SDL_LockTexture(Texture, NULL, &textureBuffers, &texturePitch)) {
				// 0xffffffff で塗りつぶし
				SDL_memset(textureBuffers, 0xff, texturePitch * h);
				SDL_UnlockTexture(Texture);
			} else {
				const char *err = SDL_GetError();
				TVPLOG_ERROR("tTVPSDLDrawDevice::CreateTexture() Lock failed:{}", err);
			}
			SDL_SetTextureScaleMode( Texture, TVPZoomInterpolation ? SDL_SCALEMODE_LINEAR : SDL_SCALEMODE_NEAREST);
			// テクスチャは「最終 framebuffer の中身そのもの」を保持しており、
			// alpha チャンネルは内部 bitmap 由来の任意の値 (compositor が更新しない場合 0)。
			// SDL3 で ARGB8888 など alpha を含む format を CreateTexture すると、
			// 既定 blend mode が BLEND になり alpha=0 の領域が透けて RenderClear した黒が
			// 出てくる (partial 更新で layer 領域が黒抜けする現象)。BLENDMODE_NONE で固定する。
			SDL_SetTextureBlendMode(Texture, SDL_BLENDMODE_NONE);
			mTextureUpdateRect.Configure(w, h, mUseFlipOnShow, mPreferredTextureFormat);
			// 全画面再描画要求
			RequestInvalidation(tTVPRect(0, 0, w, h));
		}
	}
}
//---------------------------------------------------------------------------
void TJS_INTF_METHOD tTVPSDLDrawDevice::Destruct()
{
	DestroyRenderer();
	WindowObject.Clear();
	if (Owner) Owner->Release(); Owner = nullptr;
	if (Self) Self->Release(); Self = nullptr;
	inherited::Destruct();
}

//---------------------------------------------------------------------------
void TJS_INTF_METHOD tTVPSDLDrawDevice::SetWindowInterface(iTVPWindow * window)
{
	inherited::SetWindowInterface(window);
	if (Owner) Owner->Release();
	Owner = Window->GetWindowDispatch();
	WindowObject = tTJSVariant(Owner, Owner);
	if (TJS_FAILED(Owner->NativeInstanceSupport(TJS_NIS_GETINSTANCE,
			tTJSNC_Window::ClassID, (iTJSNativeInstance**)&NIWindow))) {
		TVPThrowExceptionMessage(TVPSpecifyWindow);
	}
	if (!NIWindow) TVPThrowExceptionMessage(TVPSpecifyWindow);
}

//---------------------------------------------------------------------------
void TJS_INTF_METHOD tTVPSDLDrawDevice::AddLayerManager(iTVPLayerManager * manager)
{
	if(inherited::Managers.size() > 0)
	{
		// "SDL" デバイスでは２つ以上のLayer Managerを登録できない
		TVPThrowExceptionMessage(TVPBasicDrawDeviceDoesNotSupporteLayerManagerMoreThanOne);
	}
	inherited::AddLayerManager(manager);

	manager->SetDesiredLayerType(ltOpaque); // ltOpaque な出力を受け取りたい
}
//---------------------------------------------------------------------------
void TJS_INTF_METHOD tTVPSDLDrawDevice::SetTargetWindow(HWND wnd, bool is_main)
{
	// SDL環境では HWND の実体は SDL_Window
	SDL_Window *sdl_wnd = (SDL_Window*)wnd;
	if (!mRenderer || SDL_GetRenderWindow(mRenderer) != sdl_wnd) {
		DestroyRenderer();
		if (sdl_wnd) {
			InitRenderer(sdl_wnd);
		}
	}
}
//---------------------------------------------------------------------------
void TJS_INTF_METHOD tTVPSDLDrawDevice::NotifyLayerResize(iTVPLayerManager * manager)
{
	inherited::NotifyLayerResize(manager);
	if (!mRenderer) return;
	tjs_int w, h;
	GetSrcSize( w, h );
	if (!Texture || (Texture->w != w || Texture->h != h)) {
		DestroyTexture();
		CreateTexture();
	}
}
//---------------------------------------------------------------------------
void TJS_INTF_METHOD tTVPSDLDrawDevice::Show()
{
	KRKRZ_RENDER_STATS_SCOPE(TVPRenderStatsAddFrameShow);
	if (!NIWindow || !Texture) return;

	if (ShowVideo()) {
		return;
	}

	// 描画先の矩形を計算
	SDL_FRect dstRect;
	dstRect.x = (float)DestRect.left;
	dstRect.y = (float)DestRect.top;
	dstRect.w = (float)DestRect.get_width();
	dstRect.h = (float)DestRect.get_height();

	Render([&](SDL_Renderer *renderer) {
		SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_NONE);
		{
			KRKRZ_RENDER_STATS_SCOPE(TVPRenderStatsAddShowTex);
			if (mUseFlipOnShow) {
				// GPU renderer 経路: テクスチャは DIB のメモリ並び (bottom-up) を保持。
				// 表示時の上下反転は GPU 上のシェーダ/UV で実質ゼロコストなので、
				// SDLTextureUpdateRect::Update を memcpy なしの最短アップロードにしてここで反転。
				SDL_RenderTextureRotated(renderer, Texture, NULL, &dstRect, 0.0, NULL, SDL_FLIP_VERTICAL);
			} else {
				// SW renderer 経路: SDL_RenderTextureRotated は SW_RenderCopyEx を経由し、
				// 内部で final_rect サイズの中間サーフェスを毎フレーム alloc する。
				// SDL_RenderTexture (= SW_RenderCopy) に切り替えて中間 alloc を避ける。
				// テクスチャ側は Update で top-down に詰め直してある。
				SDL_RenderTexture(renderer, Texture, NULL, &dstRect);
			}
		}
		// Layer 合成完了後・SDL_RenderPresent 直前に Elements ダイアログをオーバーレイ。
		// (ゲーム描画より前に置くとそのあとの flip/copy で上書きされる)
		PresentDialogOverlay();
		{
			KRKRZ_RENDER_STATS_SCOPE(TVPRenderStatsAddShowOverlay);
			// メモリ状態オーバレイ (OFF 時は no-op)
			TVPRenderMemoryOverlay(renderer);
			// パッド状態オーバレイ (OFF 時は no-op)
			TVPRenderPadOverlay(renderer);
			// プラグイン登録の post-render コールバック (仮想パッド等)
			TVPDispatchPostRenderCallbacks(renderer);
		}
#ifdef KRKRZ_USE_REPL
		// エージェント駆動: 保留中の画面キャプチャを present 直前に処理。
		FulfillScreenCapture(renderer);
#endif
	});
}

//---------------------------------------------------------------------------
void TJS_INTF_METHOD tTVPSDLDrawDevice::StartBitmapCompletion(iTVPLayerManager * manager)
{
	mTextureUpdateRect.Clear();
}
//---------------------------------------------------------------------------
void TJS_INTF_METHOD tTVPSDLDrawDevice::NotifyBitmapCompleted(iTVPLayerManager * manager,
	tjs_int x, tjs_int y, const void * bits, const BITMAPINFO * bmpinfo,
	const tTVPRect &cliprect, tTVPLayerType type, tjs_int opacity)
{
	if (!Texture) return;

	int _width  = bmpinfo->bmiHeader.biWidth;
	int _height = bmpinfo->bmiHeader.biHeight;
	int _pitch  = bmpinfo->bmiHeader.biSizeImage / _height;

	int src_w = cliprect.get_width();
	int src_h = cliprect.get_height();

	// bits, bitmapinfo で表されるビットマップの cliprect の領域を、x, y に描画
	// する。
	// opacity と type は無視するしかないので無視する
	tjs_int w, h;
	GetSrcSize( w, h );

	if (!(x < 0 || y < 0 ||
			x + src_w > w ||
			y + src_h > h) &&
		!(cliprect.left < 0 || cliprect.top < 0 ||
			cliprect.right > _width ||
			cliprect.bottom > _height))
	{
		// bitmapinfo で表された cliprect の領域を x,y にコピーする
		long src_y       = cliprect.top;
		long src_y_limit = cliprect.bottom;
		long src_x       = cliprect.left;
		long width_bytes = src_w * 4; // 32bit
		const tjs_uint8 * src_p = (const tjs_uint8 *)bits;
		int src_pitch;

		if(_height < 0)
		{
			// bottom-down
			src_pitch = _pitch;
		}
		else
		{
			// bottom-up
			src_pitch = - _pitch;
			src_p += _pitch * (_height - 1);
		}

		mTextureUpdateRect.Update(Texture, x, y, src_w, src_h, src_p, src_pitch, src_x, src_y);
	}
}
//---------------------------------------------------------------------------
void TJS_INTF_METHOD tTVPSDLDrawDevice::EndBitmapCompletion(iTVPLayerManager * manager)
{
	if (Texture) {
		mTextureUpdateRect.RenderToTexture(Texture);
	}
}

//---------------------------------------------------------------------------

// 描画用テクスチャを生成
SDL_Texture *
tTVPSDLDrawDevice::CreateTexture(SDL_PixelFormat format, SDL_TextureAccess access, int w, int h)
{
	// 動画用テクスチャが無い場合は作成
	return mRenderer ? SDL_CreateTexture(mRenderer, format, access, w, h) : nullptr;
}

// 描画処理の呼び出し
//---------------------------------------------------------------------------
// ビューポート余白の壁紙描画 (背景色クリア後・ゲーム描画前に呼ぶ)。
// 壁紙テクスチャは base の世代カウンタと比較して遅延 (再)アップロードする。
//---------------------------------------------------------------------------
void
tTVPSDLDrawDevice::DrawViewportBackground(SDL_Renderer *renderer, int sw, int sh)
{
	tjs_uint32 gen = GetViewportWallpaperGen();
	if (gen != mWallpaperGen) {
		// 壁紙が差し替わった → 旧テクスチャ破棄して再アップロードさせる。
		if (mWallpaperTexture) { SDL_DestroyTexture(mWallpaperTexture); mWallpaperTexture = nullptr; }
		mWallpaperW = mWallpaperH = 0;
		mWallpaperGen = gen;
	}

	if (!mWallpaperTexture) {
		// 壁紙オブジェクト (Layer/Bitmap) からプロパティ経由で画像イメージを取得する。
		// PropGet を伴うので世代変化時 (= テクスチャ未作成時) のみ行う。
		tjs_int wpw, wph, pitch;
		const tjs_uint8 *buffer;
		if (!GetViewportWallpaperImage(wpw, wph, pitch, buffer)) return; // 壁紙なし → 背景色のみ

		// kirikiri bitmap は ARGB8888 (メモリ上 B,G,R,A)。全面アップロードなので
		// STATIC + SDL_UpdateTexture (backend 非依存) を使う。
		mWallpaperTexture = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_ARGB8888,
			SDL_TEXTUREACCESS_STATIC, wpw, wph);
		if (!mWallpaperTexture) return;
		mWallpaperW = wpw; mWallpaperH = wph;
		SDL_UpdateTexture(mWallpaperTexture, NULL, buffer, pitch);
		SDL_SetTextureBlendMode(mWallpaperTexture, SDL_BLENDMODE_NONE);
	}

	// 壁紙のフィット方式で配置矩形を計算 (cover でははみ出し clip)。
	tTVPViewportConfig wcfg;
	wcfg.fit = GetViewportWallpaperFit();
	wcfg.alignX = GetViewportWpAlignX();
	wcfg.alignY = GetViewportWpAlignY();
	tTVPRect r = TVPCalcViewportDestRect(wcfg, sw, sh, mWallpaperW, mWallpaperH);
	SDL_FRect dst;
	dst.x = (float)r.left; dst.y = (float)r.top;
	dst.w = (float)r.get_width(); dst.h = (float)r.get_height();
	SDL_RenderTexture(renderer, mWallpaperTexture, NULL, &dst);
}
//---------------------------------------------------------------------------
void
tTVPSDLDrawDevice::Render(std::function<void(SDL_Renderer *renserer)> func)
{
	if (mRenderer) {
		// SDL_SetRenderLogicalPresentation で logical (= mSurfaceWidth/Height) → physical
		// (= window 実フレームバッファ) のスケーリングを SDL に任せる。
		// 例えば PS5 では SDL_WINDOW_FULLSCREEN により window 実サイズが 4K になっても、
		// mSurfaceWidth/Height は form.cpp で渡した 1920x1080 のまま固定なので、
		// この設定がないと DestRect (logical 座標) が物理 1:1 として扱われ 4K 左上に張り付く。
		//
		// SW renderer 経路でも、本クラスの Show() は SDL_RenderTexture (= SW_RenderCopy)
		// を使い、テクスチャ format も renderer 推奨に揃えてあるので、ここでのスケーリングは
		// SDL_BlitSurfaceScaled → SDL_StretchSurface の fast path で処理される
		// (以前 tmp2 を毎フレーム alloc していたのは FLIP_VERTICAL → SW_RenderCopyEx +
		// SDLgfx_rotateSurface 経由 + format 不一致の組合せが原因で、A で解消済み)。
		int sw =  NIWindow->GetInnerWidth();
		int sh =  NIWindow->GetInnerHeight();
		SDL_SetRenderLogicalPresentation(mRenderer, sw, sh, SDL_LOGICAL_PRESENTATION_LETTERBOX);

		{
			KRKRZ_RENDER_STATS_SCOPE(TVPRenderStatsAddShowClear);
			// ビューポート余白の背景色でクリア。
			tjs_uint32 bg = GetViewportBgColor();
			SDL_SetRenderDrawColor(mRenderer,
				(Uint8)((bg >> 16) & 0xff), (Uint8)((bg >> 8) & 0xff),
				(Uint8)(bg & 0xff), (Uint8)((bg >> 24) & 0xff));
			SDL_RenderClear(mRenderer);
			// 壁紙があれば余白として全面に描画 (ゲーム描画より前)。
			DrawViewportBackground(mRenderer, sw, sh);
		}
		func(mRenderer);
		{
			KRKRZ_RENDER_STATS_SCOPE(TVPRenderStatsAddShowPresent);
			SDL_RenderPresent(mRenderer);
		}
	}
}

bool
tTVPSDLDrawDevice::ShowVideo()
{
	// presenter 稼働中は動画のみを描く (動画が画面を覆う前提)。フレームの保持と
	// テクスチャ管理は presenter 側 (SDLVideoPresenter.cpp) が行い、ここでは描画スレッドから
	// pull するだけ。Render() が logical presentation + 背景クリア + present を担う。
	// overlay の Visible=false (WINVER 仕様: 既定 false) の間は画面を占有せず、false を返して
	// 通常のゲーム描画へ戻す。WINVER が RenderVideoFrame 内で Visible を判定するのと等価
	// (SDL は presenter 登録中は画面占有なので、判定は presenter を pull する手前で行う)。
	if (!VideoPresenter || !NIWindow || !mRenderer) return false;
	if (!VideoPresenter->IsVisible()) return false;
	tTVPSDLVideoPresenterContext ctx;
	ctx.Renderer     = mRenderer;
	ctx.TargetWidth  = NIWindow->GetInnerWidth();
	ctx.TargetHeight = NIWindow->GetInnerHeight();
	ctx.DestRect     = DestRect;
	{	// mixer 画像のプライマリ座標→描画先変換に使うプライマリレイヤ寸法
		tjs_int sw = 0, sh = 0;
		GetSrcSize( sw, sh );
		ctx.SrcWidth  = sw;
		ctx.SrcHeight = sh;
	}
	Render([&](SDL_Renderer *renderer) {
		{
			KRKRZ_RENDER_STATS_SCOPE(TVPRenderStatsAddShowTex);
			VideoPresenter->RenderVideoFrame(ctx);
		}
		{
			KRKRZ_RENDER_STATS_SCOPE(TVPRenderStatsAddShowOverlay);
			// ムービー再生中も FPS/メモリを観測したいので Show() と同じく末尾で呼ぶ。
			TVPRenderMemoryOverlay(renderer);
			TVPRenderPadOverlay(renderer);
		}
	});
	return true;
}

// overlay 動画 presenter host (pull 型)。単一スロットで最後に登録した 1 つを保持する
// (WINVER BasicDrawDevice と同じ規約)。
void TJS_INTF_METHOD tTVPSDLDrawDevice::AddVideoPresenter( iTVPSDLVideoPresenter * presenter )
{
	if( !presenter ) return;
	VideoPresenter = presenter;
}
void TJS_INTF_METHOD tTVPSDLDrawDevice::RemoveVideoPresenter( iTVPSDLVideoPresenter * presenter )
{
	if( VideoPresenter == presenter ) VideoPresenter = nullptr;
}

void tTVPSDLDrawDevice::SetWaitVSync(bool enable)
{
	if (mRenderer) {
		SDL_SetRenderVSync(mRenderer, enable ? 1 : 0);
	}
}

//---------------------------------------------------------------------------
// tTJSNI_SDLDrawDevice : SDLDrawDevice TJS native class
//---------------------------------------------------------------------------
tjs_uint32 tTJSNC_SDLDrawDevice::ClassID = (tjs_uint32)-1;
tTJSNC_SDLDrawDevice::tTJSNC_SDLDrawDevice() :
	tTJSNativeClass(TJS_W("SDLDrawDevice"))
{
	// register native methods/properties
	TJS_BEGIN_NATIVE_MEMBERS(SDLDrawDevice)
	TJS_DECL_EMPTY_FINALIZE_METHOD
//----------------------------------------------------------------------
// constructor/methods
//----------------------------------------------------------------------
TJS_BEGIN_NATIVE_CONSTRUCTOR_DECL(/*var.name*/_this, /*var.type*/tTJSNI_SDLDrawDevice,
	/*TJS class name*/SDLDrawDevice)
{
	return TJS_S_OK;
}
TJS_END_NATIVE_CONSTRUCTOR_DECL(/*TJS class name*/SDLDrawDevice)
//----------------------------------------------------------------------

//----------------------------------------------------------------------
// properties
//----------------------------------------------------------------------
TJS_BEGIN_NATIVE_PROP_DECL(interface)
{
	TJS_BEGIN_NATIVE_PROP_GETTER
	{
		TJS_GET_NATIVE_INSTANCE(/*var. name*/_this, /*var. type*/tTJSNI_SDLDrawDevice);
		*result = reinterpret_cast<tjs_int64>(_this->GetDevice());
		return TJS_S_OK;
	}
	TJS_END_NATIVE_PROP_GETTER

	TJS_DENY_NATIVE_PROP_SETTER
}
TJS_END_NATIVE_PROP_DECL(interface)
//----------------------------------------------------------------------
TJS_BEGIN_NATIVE_PROP_DECL(window)
{
	TJS_BEGIN_NATIVE_PROP_GETTER
	{
		TJS_GET_NATIVE_INSTANCE(/*var. name*/_this, /*var. type*/tTJSNI_SDLDrawDevice);
		*result = _this->GetDevice()->GetWindowObject();
		return TJS_S_OK;
	}
	TJS_END_NATIVE_PROP_GETTER

	TJS_DENY_NATIVE_PROP_SETTER
}
TJS_END_NATIVE_PROP_DECL(window)
//----------------------------------------------------------------------
// overlay 動画 presenter の登録口 (iTVPSDLVideoPresenterHost) をポインタ値として公開する
// (WINVER の videoPresenterHost と同じ規約)。VideoOverlay はこのプロパティを Window の
// DrawDevice TJS オブジェクトから読み、非 0 なら presenter を登録して pull 合成に載る。
TJS_BEGIN_NATIVE_PROP_DECL(sdlVideoPresenterHost)
{
	TJS_BEGIN_NATIVE_PROP_GETTER
	{
		TJS_GET_NATIVE_INSTANCE(/*var. name*/_this, /*var. type*/tTJSNI_SDLDrawDevice);
		iTVPSDLVideoPresenterHost * host = static_cast<iTVPSDLVideoPresenterHost*>(_this->GetDevice());
		*result = reinterpret_cast<tjs_int64>(host);
		return TJS_S_OK;
	}
	TJS_END_NATIVE_PROP_GETTER

	TJS_DENY_NATIVE_PROP_SETTER
}
TJS_END_NATIVE_PROP_DECL(sdlVideoPresenterHost)
//----------------------------------------------------------------------
#ifdef KRKRZ_HAS_ELEMENTS
// Elements ダイアログ overlay の描画アダプタ提供口 (iTVPDialogRendererHost) を
// ポインタ値として公開する (videoPresenterHost と同じ規約)。 外部 (プラグイン等) が
// Window の DrawDevice TJS オブジェクトから読み host->GetDialogRenderer() で取得可能。
TJS_BEGIN_NATIVE_PROP_DECL(dialogRendererHost)
{
	TJS_BEGIN_NATIVE_PROP_GETTER
	{
		TJS_GET_NATIVE_INSTANCE(/*var. name*/_this, /*var. type*/tTJSNI_SDLDrawDevice);
		iTVPDialogRendererHost * host = static_cast<iTVPDialogRendererHost*>(_this->GetDevice());
		*result = reinterpret_cast<tjs_int64>(host);
		return TJS_S_OK;
	}
	TJS_END_NATIVE_PROP_GETTER

	TJS_DENY_NATIVE_PROP_SETTER
}
TJS_END_NATIVE_PROP_DECL(dialogRendererHost)
#endif
//----------------------------------------------------------------------
// SDL固有機能を追加想定
//----------------------------------------------------------------------
	TJS_END_NATIVE_MEMBERS
}
//---------------------------------------------------------------------------
iTJSNativeInstance *tTJSNC_SDLDrawDevice::CreateNativeInstance()
{
	return new tTJSNI_SDLDrawDevice();
}
//---------------------------------------------------------------------------

//---------------------------------------------------------------------------
tTJSNI_SDLDrawDevice::tTJSNI_SDLDrawDevice()
{
	Device = NULL;
}
//---------------------------------------------------------------------------
tTJSNI_SDLDrawDevice::~tTJSNI_SDLDrawDevice()
{
	if(Device) Device->Destruct(), Device = NULL;
}
//---------------------------------------------------------------------------
tjs_error TJS_INTF_METHOD
	tTJSNI_SDLDrawDevice::Construct(tjs_int numparams, tTJSVariant **param,
		iTJSDispatch2 *tjs_obj)
{
	Device = new tTVPSDLDrawDevice(tjs_obj);
	return TJS_S_OK;
}
//---------------------------------------------------------------------------
void TJS_INTF_METHOD tTJSNI_SDLDrawDevice::Invalidate()
{
	if(Device) Device->Destruct(), Device = NULL;
}
//---------------------------------------------------------------------------
