#define NOMINMAX
#include "tjsCommHead.h"
#include "DrawDevice.h"
#include "SDLOGLDrawDevice.h"
#include "LayerIntf.h"
#include "MsgImpl.h"
#include "SysInitIntf.h"
#include "WindowIntf.h"
#include "LogIntf.h"
#include "ThreadIntf.h"
#include "ComplexRect.h"
#include "EventIntf.h"
#include "WindowImpl.h"
#include "WindowForm.h"

#include "BitmapInfomation.h"
#include "TextureIntf.h"
#include "Application.h"
#include "OpenGLContext.h"
#include "MemoryOverlayGL.h"
#include "PadOverlayGL.h"
#include "PostRenderCallback.h"
#ifdef KRKRZ_USE_REPL
#include "ScreenCapture.h"
#include <vector>
#endif

#include <algorithm>

#ifdef KRKRZ_HAS_ELEMENTS
#include "elements/ElementsDialogManager.h"
#include "OGLDialogRenderer.h"
#include <memory>
#endif

//---------------------------------------------------------------------------
tTVPSDLOGLDrawDevice::tTVPSDLOGLDrawDevice(iTJSDispatch2 *self)
 : Owner(nullptr)
 , Self(self)
 , GLContext(nullptr)
 , TextureClass(nullptr)
 , TextureInstance(nullptr)
 , SurfaceWidth(0)
 , SurfaceHeight(0)
 , VideoPresenter(nullptr)
{
	if (Self) Self->AddRef();

	// overlay 動画 presenter factory を登録 (VideoOverlay が pull 経路で使う)。冪等。
	TVPRegisterGLVideoOverlayPresenterFactory();

	// 描画位置指定 - いったん全画面
	_position[0] = -1.0f; _position[1] =  1.0f; // left top
	_position[2] = -1.0f; _position[3] = -1.0f; // left bottom
	_position[4] =  1.0f; _position[5] =  1.0f; // right top
	_position[6] =  1.0f; _position[7] = -1.0f; // right bottom
	// 動画の配置矩形は presenter (GLVideoPresenter.cpp) が毎フレーム算出する。

	// 内部 Texture native class (TJS 公開はしない、internal use)
	TextureClass = TVPCreateNativeClass_Texture();
}

//---------------------------------------------------------------------------
tTVPSDLOGLDrawDevice::~tTVPSDLOGLDrawDevice()
{
}

//---------------------------------------------------------------------------
void TJS_INTF_METHOD tTVPSDLOGLDrawDevice::Show()
{
	if (!GLContext) return;

	GLContext->MakeCurrent();
	InitPosition();

	if (ShowVideo()) {
		// 動画描画した場合はそれだけ
	} else if (TextureInstance) {
		// ビューポート余白の背景色でクリア。
		tjs_uint32 bg = GetViewportBgColor();
		glClearColor(((bg >> 16) & 0xff) / 255.0f, ((bg >> 8) & 0xff) / 255.0f,
		             (bg & 0xff) / 255.0f, ((bg >> 24) & 0xff) / 255.0f);
		glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

		// 壁紙があれば余白として描画 (ゲーム描画より前)。
		TVPDrawGLViewportWallpaper(TextureDrawer, mWallpaperCache, *this,
			SurfaceWidth, SurfaceHeight);

		TextureDrawer.DrawTexture(TextureInstance->GetTexture(),
			SurfaceWidth, SurfaceHeight, _position,
			TextureInstance->GetWidth(),
			TextureInstance->GetHeight());
	}

	// Layer 合成完了直後 + memoverlay の前に Elements ダイアログを上に貼る
	PresentDialogOverlay();

	// memoverlay の OpenGL 直接版描画 + 計測値更新 + log 出力。
	// SDL_Renderer 不在経路用に font 8x8 embed + shader/VBO で実装した独自描画。
	// SDLDrawDevice (SDL_Renderer 経路) は引き続き TVPRenderMemoryOverlay(renderer)。
	TVPRenderMemoryOverlayGL();
	TVPRenderPadOverlayGL();
	// プラグイン登録の post-render コールバック (仮想パッド等の GL ES 直描画)
	TVPDispatchPostRenderCallbacksGL();

#ifdef KRKRZ_USE_REPL
	// エージェント駆動: 保留中の画面キャプチャを Swap 直前に読み戻して保存。
	if (TVPHasPendingScreenCapture()) {
		tTVPScreenCaptureReq req;
		if (TVPTakeScreenCaptureRequest(req)) {
			int fw = SurfaceWidth, fh = SurfaceHeight;
			if (fw > 0 && fh > 0) {
				std::vector<unsigned char> buf((size_t)fw * fh * 4);
				glReadPixels(0, 0, fw, fh, GL_RGBA, GL_UNSIGNED_BYTE, buf.data());
				TVPSaveGLReadback(req, buf.data(), fw, fh);
			} else {
				TVPSetScreenCaptureResult(req.path, 0, 0, false);
			}
		}
	}
#endif // KRKRZ_USE_REPL

	GLContext->Swap();
}

//---------------------------------------------------------------------------
void tTVPSDLOGLDrawDevice::DestroyTexture()
{
	// invalidate Texture object
	if (TextureObject.Type() == tvtObject) {
		TextureObject.AsObjectClosureNoAddRef().Invalidate(0, nullptr, nullptr, TextureObject.AsObjectNoAddRef());
	}
	TextureObject.Clear();
	TextureInstance = nullptr;
}

//---------------------------------------------------------------------------
void tTVPSDLOGLDrawDevice::CreateTexture()
{
	if (!TextureInstance && TextureClass) {
		tjs_int w, h;
		GetSrcSize(w, h);
		if (w > 0 && h > 0) {
			iTJSDispatch2 *newobj = nullptr;
			try {
				tTJSVariant param_w = w;
				tTJSVariant param_h = h;
				tTJSVariant *pparam[2] = { &param_w, &param_h };
				if (TJS_FAILED(TextureClass->CreateNew(0, nullptr, nullptr, &newobj, 2, pparam, TextureClass)))
					TVPThrowExceptionMessage(TVPInternalError, TJS_W("tTJSNI_Texture::Construct"));
				TextureObject = tTJSVariant(newobj, newobj);
			} catch (...) {
				if (newobj) newobj->Release();
				throw;
			}
			if (newobj) newobj->Release();

			// extract interface
			if (TextureObject.Type() == tvtObject) {
				tTJSVariantClosure clo = TextureObject.AsObjectClosureNoAddRef();
				if (clo.Object) {
					if (TJS_FAILED(clo.Object->NativeInstanceSupport(TJS_NIS_GETINSTANCE,
							tTJSNC_Texture::ClassID, (iTJSNativeInstance**)&TextureInstance))) {
						TextureInstance = nullptr;
						TVPThrowExceptionMessage(TJS_W("Cannot retrive texture instance."));
					}
				}
			}

			mTextureUpdateRect.Resize(w, h);
			RequestInvalidation(tTVPRect(0, 0, DestRect.get_width(), DestRect.get_height()));
		}
	}
}

//---------------------------------------------------------------------------
void tTVPSDLOGLDrawDevice::InitPosition()
{
	int w, h;
	GLContext->GetSurfaceSize(&w, &h);

	if (SurfaceWidth != w || SurfaceHeight != h) {
		// 描画位置指定 destRect の領域
		int w2 = w / 2;
		int h2 = h / 2;
		float left   = (float)(DestRect.left   - w2) / w2;
		float bottom = (float)(DestRect.bottom - h2) / h2;
		float right  = (float)(DestRect.right  - w2) / w2;
		float top    = (float)(DestRect.top    - h2) / h2;

		_position[0] = left;  _position[1] = -bottom; // left top
		_position[2] = left;  _position[3] = -top;    // left bottom
		_position[4] = right; _position[5] = -bottom; // right top
		_position[6] = right; _position[7] = -top;    // right bottom

		SurfaceWidth = w;
		SurfaceHeight = h;
	}
}

//---------------------------------------------------------------------------
void tTVPSDLOGLDrawDevice::InitContext(void *nativeWindow)
{
	GLContext = iTVPGLContext::GetContext(nativeWindow);

	CreateTexture();
	TextureDrawer.Init();

#ifdef KRKRZ_HAS_ELEMENTS
	// GL context 生存中だけ存在する dialog renderer を DrawDevice 自身が所有し、
	// iTVPDialogRendererHost (this) として manager に登録 (DoneContext で解除)。
	DialogRenderer = std::make_unique<tTVPOGLDialogRenderer>(this);
	tTVPElementsDialogManager::Instance().RegisterDialogHost(this, this);
#endif

	// Context コンテキスト作成時コールバック
	if (Self) {
		static ttstr eventname(TJS_W("onInit"));
		TVPPostEvent(Self, Self, eventname, 0, TVP_EPT_IMMEDIATE, 0, nullptr);
	}
}

//---------------------------------------------------------------------------
void tTVPSDLOGLDrawDevice::DoneContext()
{
	if (!GLContext) return;

	GLContext->MakeCurrent();

	// Context コンテキスト破棄前コールバック
	if (Self) {
		static ttstr eventname(TJS_W("onDone"));
		TVPPostEvent(Self, Self, eventname, 0, TVP_EPT_IMMEDIATE, 0, nullptr);
	}

#ifdef KRKRZ_HAS_ELEMENTS
	// GL リソース付き dialog renderer を context 解放前に破棄する。host 登録解除
	// (この device のダイアログ teardown) を renderer 破棄前に行う。
	tTVPElementsDialogManager::Instance().UnregisterDialogHost(this);
	DialogRenderer.reset();
#endif

	DestroyTexture();
	// ビューポート壁紙テクスチャを context 解放前に破棄。
	mWallpaperCache.Release();
	TextureDrawer.Done();

	if (GLContext) {
		GLContext->Release();
		GLContext = nullptr;
	}
}

//---------------------------------------------------------------------------
void TJS_INTF_METHOD tTVPSDLOGLDrawDevice::Destruct()
{
	DoneContext();
	WindowObject.Clear();
	if (Owner) Owner->Release(); Owner = nullptr;
	if (Self) Self->Release(); Self = nullptr;
	if (TextureClass) TextureClass->Release(); TextureClass = nullptr;
	inherited::Destruct();
}

//---------------------------------------------------------------------------
void TJS_INTF_METHOD tTVPSDLOGLDrawDevice::SetWindowInterface(iTVPWindow * window)
{
	inherited::SetWindowInterface(window);
	if (Owner) Owner->Release();
	Owner = Window->GetWindowDispatch();
	WindowObject = tTJSVariant(Owner, Owner);

	// GLES 初期化用処理 - 初回接続時に初期化
	tTJSNI_Window *NIWindow;
	if (TJS_FAILED(Owner->NativeInstanceSupport(TJS_NIS_GETINSTANCE,
			tTJSNC_Window::ClassID, (iTJSNativeInstance**)&NIWindow))) {
		TVPThrowExceptionMessage(TVPSpecifyWindow);
	}

	void *nativeWindow = NIWindow->GetForm()->NativeWindowHandle();
	iTVPGLContext *context = iTVPGLContext::GetContext(nativeWindow);
	if (context) {
		context->MakeCurrent();
		InitGLES();
		context->Release();
	}
}

//---------------------------------------------------------------------------
void TJS_INTF_METHOD tTVPSDLOGLDrawDevice::AddLayerManager(iTVPLayerManager * manager)
{
	if (inherited::Managers.size() > 0) {
		// "SDLOGL" デバイスでは２つ以上のLayer Managerを登録できない
		TVPThrowExceptionMessage(TVPBasicDrawDeviceDoesNotSupporteLayerManagerMoreThanOne);
	}
	inherited::AddLayerManager(manager);

	manager->SetDesiredLayerType(ltOpaque); // ltOpaque な出力を受け取りたい
}

//---------------------------------------------------------------------------
void TJS_INTF_METHOD tTVPSDLOGLDrawDevice::SetTargetWindow(HWND wnd, bool is_main)
{
	if (!GLContext || (void*)wnd != GLContext->NativeWindow()) {
		DoneContext();
		if (wnd) {
			InitContext((void*)wnd);
		}
	}
}

//---------------------------------------------------------------------------
void TJS_INTF_METHOD tTVPSDLOGLDrawDevice::SetDestRectangle(const tTVPRect & rect)
{
	inherited::SetDestRectangle(rect);
	SurfaceWidth = 0;
	SurfaceHeight = 0;
}

//---------------------------------------------------------------------------
void TJS_INTF_METHOD tTVPSDLOGLDrawDevice::NotifyLayerResize(iTVPLayerManager * manager)
{
	inherited::NotifyLayerResize(manager);
	if (!GLContext) return;

	GLContext->MakeCurrent();
	if (TextureInstance) {
		tjs_int w, h;
		GetSrcSize(w, h);
		if (TextureInstance->Resize(w, h)) {
			mTextureUpdateRect.Resize(w, h);
		} else {
			DestroyTexture();
			CreateTexture();
		}
	} else {
		CreateTexture();
	}
}

//---------------------------------------------------------------------------
void TJS_INTF_METHOD tTVPSDLOGLDrawDevice::StartBitmapCompletion(iTVPLayerManager * manager)
{
	mTextureUpdateRect.Clear();
}

//---------------------------------------------------------------------------
void TJS_INTF_METHOD tTVPSDLOGLDrawDevice::NotifyBitmapCompleted(iTVPLayerManager * manager,
	tjs_int x, tjs_int y, const void * bits, const BITMAPINFO * bmpinfo,
	const tTVPRect &cliprect, tTVPLayerType type, tjs_int opacity)
{
	if (!TextureInstance) return;

	int _width  = bmpinfo->bmiHeader.biWidth;
	int _height = bmpinfo->bmiHeader.biHeight;
	int _pitch  = bmpinfo->bmiHeader.biSizeImage / _height;

	int src_w = cliprect.get_width();
	int src_h = cliprect.get_height();

	tjs_int w, h;
	GetSrcSize(w, h);
	if (!(x < 0 || y < 0 ||
			x + src_w > w ||
			y + src_h > h) &&
		!(cliprect.left < 0 || cliprect.top < 0 ||
			cliprect.right > _width ||
			cliprect.bottom > _height))
	{
		// bitmapinfo で表された cliprect の領域を x,y にコピーする
		long src_y = cliprect.top;
		long src_x = cliprect.left;
		const tjs_uint8 * src_p = (const tjs_uint8 *)bits;
		long src_pitch;

		if (_height < 0) {
			// bottom-down
			src_pitch = _pitch;
		} else {
			// bottom-up
			src_pitch = -_pitch;
			src_p += _pitch * (_height - 1);
		}

		mTextureUpdateRect.Update(TextureInstance, x, y, src_w, src_h, src_p, src_pitch, src_x, src_y);
	}
}

//---------------------------------------------------------------------------
void TJS_INTF_METHOD tTVPSDLOGLDrawDevice::EndBitmapCompletion(iTVPLayerManager * manager)
{
	if (TextureInstance) {
		mTextureUpdateRect.RenderToTexture(TextureInstance);
	}
}

//---------------------------------------------------------------------------
//  VideoOverlay Support
//---------------------------------------------------------------------------
// overlay 動画 presenter host (pull 型)。単一スロットで最後に登録した 1 つを保持する。
void TJS_INTF_METHOD tTVPSDLOGLDrawDevice::AddVideoPresenter( iTVPGLVideoPresenter * presenter )
{
	if( !presenter ) return;
	VideoPresenter = presenter;
}
void TJS_INTF_METHOD tTVPSDLOGLDrawDevice::RemoveVideoPresenter( iTVPGLVideoPresenter * presenter )
{
	if( VideoPresenter == presenter ) VideoPresenter = nullptr;
}

bool tTVPSDLOGLDrawDevice::ShowVideo()
{
	// presenter 稼働中は動画のみを描く。フレーム保持と GLTexture 管理は presenter 側
	// (GLVideoPresenter.cpp) が行い、ここでは描画スレッド (GL context current) から pull する。
	if( !VideoPresenter ) return false;
	glClearColor(0.0f, 0.0f, 0.0f, 0.0f);
	glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
	tTVPGLVideoPresenterContext ctx;
	ctx.TextureDrawer = &TextureDrawer;
	ctx.TargetWidth   = SurfaceWidth;
	ctx.TargetHeight  = SurfaceHeight;
	ctx.DestRect      = DestRect;
	{	tjs_int sw = 0, sh = 0; GetSrcSize( sw, sh ); ctx.SrcWidth = sw; ctx.SrcHeight = sh; }
	VideoPresenter->RenderVideoFrame(ctx);
	return true;
}

void tTVPSDLOGLDrawDevice::SetWaitVSync(bool enable)
{
	if (GLContext) {
		GLContext->SetWaitVSync(enable);
	}
}

//---------------------------------------------------------------------------
// tTJSNI_SDLOGLDrawDevice : SDLOGLDrawDevice TJS native class
//---------------------------------------------------------------------------
tjs_uint32 tTJSNC_SDLOGLDrawDevice::ClassID = (tjs_uint32)-1;
tTJSNC_SDLOGLDrawDevice::tTJSNC_SDLOGLDrawDevice() :
	tTJSNativeClass(TJS_W("SDLOGLDrawDevice"))
{
	// register native methods/properties
	TJS_BEGIN_NATIVE_MEMBERS(SDLOGLDrawDevice)
	TJS_DECL_EMPTY_FINALIZE_METHOD
//----------------------------------------------------------------------
// constructor/methods
//----------------------------------------------------------------------
TJS_BEGIN_NATIVE_CONSTRUCTOR_DECL(/*var.name*/_this, /*var.type*/tTJSNI_SDLOGLDrawDevice,
	/*TJS class name*/SDLOGLDrawDevice)
{
	return TJS_S_OK;
}
TJS_END_NATIVE_CONSTRUCTOR_DECL(/*TJS class name*/SDLOGLDrawDevice)
//----------------------------------------------------------------------

//----------------------------------------------------------------------
// properties
//----------------------------------------------------------------------
TJS_BEGIN_NATIVE_PROP_DECL(interface)
{
	TJS_BEGIN_NATIVE_PROP_GETTER
	{
		TJS_GET_NATIVE_INSTANCE(/*var. name*/_this, /*var. type*/tTJSNI_SDLOGLDrawDevice);
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
		TJS_GET_NATIVE_INSTANCE(/*var. name*/_this, /*var. type*/tTJSNI_SDLOGLDrawDevice);
		*result = _this->GetDevice()->GetWindowObject();
		return TJS_S_OK;
	}
	TJS_END_NATIVE_PROP_GETTER

	TJS_DENY_NATIVE_PROP_SETTER
}
TJS_END_NATIVE_PROP_DECL(window)
//----------------------------------------------------------------------
// overlay 動画 presenter の登録口 (iTVPGLVideoPresenterHost) をポインタ値として公開する
// (WINVER の videoPresenterHost / SDL の sdlVideoPresenterHost と同じ規約)。
TJS_BEGIN_NATIVE_PROP_DECL(glVideoPresenterHost)
{
	TJS_BEGIN_NATIVE_PROP_GETTER
	{
		TJS_GET_NATIVE_INSTANCE(/*var. name*/_this, /*var. type*/tTJSNI_SDLOGLDrawDevice);
		iTVPGLVideoPresenterHost * host = static_cast<iTVPGLVideoPresenterHost*>(_this->GetDevice());
		*result = reinterpret_cast<tjs_int64>(host);
		return TJS_S_OK;
	}
	TJS_END_NATIVE_PROP_GETTER

	TJS_DENY_NATIVE_PROP_SETTER
}
TJS_END_NATIVE_PROP_DECL(glVideoPresenterHost)
//----------------------------------------------------------------------
#ifdef KRKRZ_HAS_ELEMENTS
// Elements ダイアログ overlay の描画アダプタ提供口 (iTVPDialogRendererHost) を
// ポインタ値として公開する (videoPresenterHost と同じ規約)。GL context 未生成時は
// GetDialogRenderer() が nullptr を返す (host ポインタ自体は非 0)。
TJS_BEGIN_NATIVE_PROP_DECL(dialogRendererHost)
{
	TJS_BEGIN_NATIVE_PROP_GETTER
	{
		TJS_GET_NATIVE_INSTANCE(/*var. name*/_this, /*var. type*/tTJSNI_SDLOGLDrawDevice);
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
	TJS_END_NATIVE_MEMBERS
}
//---------------------------------------------------------------------------
iTJSNativeInstance *tTJSNC_SDLOGLDrawDevice::CreateNativeInstance()
{
	return new tTJSNI_SDLOGLDrawDevice();
}
//---------------------------------------------------------------------------

//---------------------------------------------------------------------------
tTJSNI_SDLOGLDrawDevice::tTJSNI_SDLOGLDrawDevice()
{
	Device = NULL;
}
//---------------------------------------------------------------------------
tTJSNI_SDLOGLDrawDevice::~tTJSNI_SDLOGLDrawDevice()
{
	if (Device) Device->Destruct(), Device = NULL;
}
//---------------------------------------------------------------------------
tjs_error TJS_INTF_METHOD
	tTJSNI_SDLOGLDrawDevice::Construct(tjs_int numparams, tTJSVariant **param,
		iTJSDispatch2 *tjs_obj)
{
	Device = new tTVPSDLOGLDrawDevice(tjs_obj);
	return TJS_S_OK;
}
//---------------------------------------------------------------------------
void TJS_INTF_METHOD tTJSNI_SDLOGLDrawDevice::Invalidate()
{
	if (Device) Device->Destruct(), Device = NULL;
}
//---------------------------------------------------------------------------
