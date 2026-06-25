
#define NOMINMAX
#include "tjsCommHead.h"
#include "DrawDevice.h"
#include "OGLDrawDevice.h"
#include "LayerIntf.h"
#include "MsgImpl.h"
#include "SysInitIntf.h"
#include "WindowIntf.h"
#include "LogIntf.h"
#include "ThreadIntf.h"
#include "ComplexRect.h"
#include "EventIntf.h"
#include "WindowImpl.h"

// フォーム参照用
#ifdef __WINVER__
#include "WindowFormUnit.h"
#else
#include "WindowForm.h"
#endif


#include "BitmapInfomation.h"

#include "TextureIntf.h"
#include "Matrix32Intf.h"
#include "CanvasIntf.h"
#include <algorithm>

#include "Application.h"
#include "OpenGLContext.h"
#include "MemoryOverlayGL.h"
#include "PadOverlayGL.h"

#ifdef KRKRZ_HAS_ELEMENTS
#include "elements/ElementsDialogManager.h"
#include "OGLDialogRenderer.h"
#include <memory>
#endif
#ifdef KRKRZ_USE_REPL
#include "ScreenCapture.h"
#include <vector>
#endif

//---------------------------------------------------------------------------
tTVPOGLDrawDevice::tTVPOGLDrawDevice(iTJSDispatch2 *self)
 : SurfaceWidth(0)
 , SurfaceHeight(0)
 , CanvasInstance(nullptr)
 , Owner(nullptr)
 , Self(self)
 , TextureInstance(nullptr)
 , MatrixInstance(nullptr)
 , GLContext(nullptr)
 , DoCreateCanvas(false)
#ifndef __WINVER__
 , _video_texture(0)
 , mVideoBuffer(0)
 , mVideoBufferDirty(false)
 , mVideoWidth(0)
 , mVideoHeight(0)
#endif

 {
	if (Self) Self->AddRef();

    // 描画位置指定・いったん全画面
    _position[0] = -1.0f; // left top
    _position[1] =  1.0f;
    _position[2] = -1.0f; // left bottom
    _position[3] = -1.0f;
    _position[4] =  1.0f; // right top
    _position[5] =  1.0f;
    _position[6] =  1.0f; // right bottom
    _position[7] = -1.0f;

	// TextureClass
	TextureClass = TVPCreateNativeClass_Texture();

	// Matrix
	{
		tTJSNativeClass *MatrixClass = TVPCreateNativeClass_Matrix32();
		iTJSDispatch2 * newobj = NULL;
		try
		{
			if( TJS_FAILED( MatrixClass->CreateNew( 0, nullptr, nullptr, &newobj, 0, nullptr, MatrixClass ) ) )
				TVPThrowExceptionMessage( TVPInternalError, TJS_W( "tTJSNI_Matrix32::Construct" ) );
			MatrixObject = tTJSVariant( newobj, newobj );
		} catch( ... )
		{
			if( newobj ) newobj->Release();
			throw;
		}
		if( newobj ) newobj->Release();

		// extract interface
		if (MatrixObject.Type() == tvtObject) {
			tTJSVariantClosure clo = MatrixObject.AsObjectClosureNoAddRef();
			if( clo.Object ) {
				if(TJS_FAILED(clo.Object->NativeInstanceSupport(TJS_NIS_GETINSTANCE, tTJSNC_Matrix32::ClassID, (iTJSNativeInstance**)&MatrixInstance)))
				{
					MatrixInstance = nullptr;
					TVPThrowExceptionMessage(TJS_W("Cannot retrive matrix instance."));
				}
			}
		}
		MatrixClass->Release();
	}

#ifndef __WINVER__
    // 描画位置指定・いったん全画面対象
    _video_position[0] = -1.0f; // left top
    _video_position[1] =  1.0f;
    _video_position[2] = -1.0f; // left bottom
    _video_position[3] = -1.0f;
    _video_position[4] =  1.0f; // right top
    _video_position[5] =  1.0f;
    _video_position[6] =  1.0f; // right bottom
    _video_position[7] = -1.0f;
#endif

}

//---------------------------------------------------------------------------
tTVPOGLDrawDevice::~tTVPOGLDrawDevice()
{
}

//---------------------------------------------------------------------------
void tTVPOGLDrawDevice::CreateCanvas()
{
	// Canvas 描画システムで初期化する
	iTJSDispatch2 * cls = NULL;
	iTJSDispatch2 * newobj = NULL;
	try
	{
		cls = new tTJSNC_Canvas();
		// Windowクラスをパラメータに渡す(保持はせずWindowハンドルを得るだけ、AndroidではSurfaceハンドル)
		tTJSVariant param[1] = { Owner };
		tTJSVariant *pparam[1] = { param };
		if( TJS_FAILED( cls->CreateNew( 0, nullptr, nullptr, &newobj, 1, pparam, cls ) ) )
			TVPThrowExceptionMessage( TVPInternalError, TJS_W( "tTJSNI_Canvas::Construct" ) );
		SetCanvasObject( tTJSVariant( newobj, newobj ) );
	} catch( ... )
	{
		if( cls ) cls->Release();
		if( newobj ) newobj->Release();
		throw;
	}
	if( cls ) cls->Release();
	if( newobj ) newobj->Release();

	Window->RequestUpdate();
}

void tTVPOGLDrawDevice::DestroyCanvas()
{
	SetCanvasObject(tTJSVariant());
}

void tTVPOGLDrawDevice::RequestCreateCanvas()
{
	if (!GLContext) {
		DoCreateCanvas = true;
		return;
	}
	if (!CanvasInstance) {
		CreateCanvas();
	}
}

//---------------------------------------------------------------------------
void tTVPOGLDrawDevice::SetCanvasObject(const tTJSVariant & val)
{
	if( CanvasObject.Type() == tvtObject )
		CanvasObject.AsObjectClosureNoAddRef().Invalidate(0, nullptr, nullptr, CanvasObject.AsObjectNoAddRef());

	CanvasObject = val;
	CanvasInstance = nullptr;

	// extract interface
	if(CanvasObject.Type() == tvtObject)
	{
		tTJSVariantClosure clo = CanvasObject.AsObjectClosureNoAddRef();
		if( clo.Object ) {
			if(TJS_FAILED(clo.Object->NativeInstanceSupport(TJS_NIS_GETINSTANCE, tTJSNC_Canvas::ClassID, (iTJSNativeInstance**)&CanvasInstance)))
			{
				CanvasInstance = nullptr;
				TVPThrowExceptionMessage(TJS_W("Cannot retrive canvas instance."));
			}
		}
	}
}


//---------------------------------------------------------------------------
void TJS_INTF_METHOD tTVPOGLDrawDevice::Show()
{
	if (!GLContext) return;

	GLContext->MakeCurrent();

	InitPosition();

#ifndef __WINVER__
	if (ShowVideo()) {
	} else
#endif

	if (!CanvasInstance) {

#ifdef __GENERIC__
        // ビューポート余白の背景色でクリア。
        tjs_uint32 bg = GetViewportBgColor();
        glClearColor(((bg >> 16) & 0xff) / 255.0f, ((bg >> 8) & 0xff) / 255.0f,
                     (bg & 0xff) / 255.0f, ((bg >> 24) & 0xff) / 255.0f);
        glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
        // 壁紙があれば余白として描画 (ゲーム描画より前)。
        TVPDrawGLViewportWallpaper(TextureDrawer, mWallpaperCache, *this,
            SurfaceWidth, SurfaceHeight);
#else
        glClearColor(0.0f, 0.0f, 0.0f, 0.0f);
        glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
#endif

		if (TextureInstance) {
			TextureDrawer.DrawTexture(TextureInstance->GetTexture(), SurfaceWidth, SurfaceHeight, _position,
				TextureInstance->GetWidth(),
				TextureInstance->GetHeight());
		}

	} else if (CanvasInstance) {
		CanvasInstance->SetSurfaceSize(SurfaceWidth, SurfaceHeight);
		CanvasInstance->BeginDrawing();
		try {
			if( Self )
			{
				static ttstr eventname( TJS_W( "onDraw" ) );
				TVPPostEvent( Self, Self, eventname, 0, TVP_EPT_IMMEDIATE, 0, nullptr );
			}
		} catch( ... ) {
			CanvasInstance->EndDrawing();
			GLContext->Swap();
			throw;
		}
		CanvasInstance->EndDrawing();
	}

	// Layer 合成完了直後・Swap 直前に Elements ダイアログをオーバーレイ
	PresentDialogOverlay();

	// memoverlay 描画 (OFF 時は即 return、SDL_Renderer 不要)。SDLOGLDrawDevice
	// と共通の OGL 直接版 (common/visual/opengl/MemoryOverlayGL.h)。
	TVPRenderMemoryOverlayGL();
	TVPRenderPadOverlayGL();

#ifdef KRKRZ_USE_REPL
	// エージェント駆動: 保留中の画面キャプチャを Swap 直前に読み戻して保存。
	// (ScreenCapture は KRKRZ_REPL=ON の SDL ビルドのみリンクされる)
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
#endif

	GLContext->Swap();
}

//---------------------------------------------------------------------------
void tTVPOGLDrawDevice::DestroyTexture() 
{
	// invalidate Texture object
	if (TextureObject.Type() == tvtObject) {
		TextureObject.AsObjectClosureNoAddRef().Invalidate(0, nullptr, nullptr, TextureObject.AsObjectNoAddRef());
	}
	TextureObject.Clear();
	TextureInstance = nullptr;
}

//------------------------------------------------------g---------------------
void tTVPOGLDrawDevice::CreateTexture() {

	if (!TextureInstance && TextureClass) {
		tjs_int w, h;
		GetSrcSize( w, h );
		if (w > 0 && h > 0) {

			iTJSDispatch2 * newobj = NULL;
			try
			{
				tTJSVariant param_w = w;
				tTJSVariant param_h = h;
				tTJSVariant *pparam[2] = { &param_w,  &param_h };
				if( TJS_FAILED( TextureClass->CreateNew( 0, nullptr, nullptr, &newobj, 2, pparam, TextureClass ) ) )
					TVPThrowExceptionMessage( TVPInternalError, TJS_W( "tTJSNI_Texture::Construct" ) );
				TextureObject = tTJSVariant( newobj, newobj );
			} catch( ... )
			{
				if( newobj ) newobj->Release();
				throw;
			}
			if( newobj ) newobj->Release();

			// extract interface
			if (TextureObject.Type() == tvtObject) {
				tTJSVariantClosure clo = TextureObject.AsObjectClosureNoAddRef();
				if( clo.Object ) {
					if(TJS_FAILED(clo.Object->NativeInstanceSupport(TJS_NIS_GETINSTANCE, tTJSNC_Texture::ClassID, (iTJSNativeInstance**)&TextureInstance)))
					{
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

void 
tTVPOGLDrawDevice::UpdateTexture(int x, int y, int w, int h, std::function<void(char *dest, int pitch)> updator)
{
	if (TextureInstance) {
		TextureInstance->UpdateTexture(x, y, w, h, updator);
	}
}

//---------------------------------------------------------------------------
void tTVPOGLDrawDevice::InitPosition() 
{
	int w, h;
	GLContext->GetSurfaceSize(&w, &h);

	if (SurfaceWidth != w || SurfaceHeight != h) {

		// 描画位置指定 destRect の領域
		int w2 = w/2;
		int h2 = h/2;
		float left    = (float)(DestRect.left   - w2) / w2;
		float bottom  = (float)(DestRect.bottom - h2) / h2;
		float right   = (float)(DestRect.right  - w2) / w2;
		float top     = (float)(DestRect.top    - h2) / h2;

		TVPLOG_VERBOSE("drawdevice destrect: {},{},{},{}", DestRect.left, DestRect.top, DestRect.right, DestRect.bottom);
		TVPLOG_VERBOSE("drawdevice dest: {},{},{},{}", left, top, right, bottom);

		_position[0] = left; // left top
		_position[1] = -bottom;
		_position[2] = left; // left bottom
		_position[3] = -top;
		_position[4] = right; // right top
		_position[5] = -bottom;
		_position[6] = right; // right bottom
		_position[7] = -top;

		SurfaceWidth = w;
		SurfaceHeight = h;

		InitMatrix();
	}
}

void tTVPOGLDrawDevice::InitMatrix()
{
	if (MatrixInstance && TextureInstance) {
		tjs_real sx = (tjs_real)DestRect.get_width() / TextureInstance->GetWidth();
		tjs_real sy = (tjs_real)DestRect.get_height() / TextureInstance->GetHeight();
		MatrixInstance->SetIdentity();
		MatrixInstance->SetTranslate(DestRect.left, DestRect.top);
		MatrixInstance->SetScale(sx, sy);
	}
}

void tTVPOGLDrawDevice::InitUV() 
{
	if (TextureInstance) {
		InitMatrix();
	}
}

void tTVPOGLDrawDevice::InitContext(void *nativeWindow)
{
	GLContext = iTVPGLContext::GetContext(nativeWindow);

	if (DoCreateCanvas) {
		CreateCanvas();
	}
	CreateTexture();
	InitUV();
	TextureDrawer.Init();

#ifdef KRKRZ_HAS_ELEMENTS
	// GL context 生存中だけ存在する dialog renderer を登録 (DoneContext で解除)
	tTVPElementsDialogManager::Instance().RegisterRenderer(
		this, std::make_unique<tTVPOGLDialogRenderer>(this));
#endif

	// Context コンテキスト作成時コールバック
	if( Self ) {
		static ttstr eventname( TJS_W( "onInit" ) );
		TVPPostEvent( Self, Self, eventname, 0, TVP_EPT_IMMEDIATE, 0, nullptr );
	}
}

void tTVPOGLDrawDevice::DoneContext()
{
	// Context コンテキスト作成時コールバック
	if (!GLContext) return;

	GLContext->MakeCurrent();

	// Context コンテキスト破棄前コールバック
	if ( Self ) {
		static ttstr eventname( TJS_W( "onDone" ) );
		TVPPostEvent( Self, Self, eventname, 0, TVP_EPT_IMMEDIATE, 0, nullptr );
	}

#ifdef KRKRZ_HAS_ELEMENTS
	// GL リソース付き dialog renderer を context 解放前に破棄する
	tTVPElementsDialogManager::Instance().UnregisterRenderer(this);
#endif

	DestroyTexture();
	DestroyCanvas();
	TextureDrawer.Done();

#ifdef __GENERIC__
	// 動画用 GLTexture / mVideoBuffer は ClearVideo でのみ解放される。
	// DrawDevice 破棄時に呼ばないとリークするため、GL context を解放する
	// 直前で必ず一度呼ぶ (_video_texture の glDeleteTextures が context 必須)。
	ClearVideo();
	// ビューポート壁紙テクスチャも context 解放前に破棄。
	mWallpaperCache.Release();
#endif

	if (GLContext) {
		GLContext->Release();
		GLContext = nullptr;
	}
}

//---------------------------------------------------------------------------
void TJS_INTF_METHOD tTVPOGLDrawDevice::Destruct()
{
	DoneContext();
	WindowObject.Clear();
	if (Owner) Owner->Release(); Owner = nullptr;
	if (Self) Self->Release(); Self = nullptr;
	if (TextureClass) TextureClass->Release(); TextureClass = nullptr;
	inherited::Destruct();
}


//---------------------------------------------------------------------------
void TJS_INTF_METHOD tTVPOGLDrawDevice::SetWindowInterface(iTVPWindow * window)
{
	inherited::SetWindowInterface(window);
	if (Owner) Owner->Release();
	Owner = Window->GetWindowDispatch();
	WindowObject = tTJSVariant(Owner, Owner);

	// GLES 初期化用処理
	// OGLApplication とは限らないので初回接続時に初期化
	tTJSNI_Window *NIWindow;
	if (TJS_FAILED(Owner->NativeInstanceSupport(TJS_NIS_GETINSTANCE,
			tTJSNC_Window::ClassID, (iTJSNativeInstance**)&NIWindow))) {
		TVPThrowExceptionMessage(TVPSpecifyWindow);
	}

#ifdef __WINVER__
	void *nativeWindow = NIWindow->GetForm()->GetHandle();
#else
	void *nativeWindow = NIWindow->GetForm()->NativeWindowHandle();
#endif
	iTVPGLContext *context = iTVPGLContext::GetContext(nativeWindow);
	if (context) {
		context->MakeCurrent();
		InitGLES();
		context->Release();
	}
}

//---------------------------------------------------------------------------
void TJS_INTF_METHOD tTVPOGLDrawDevice::AddLayerManager(iTVPLayerManager * manager)
{
	if(inherited::Managers.size() > 0)
	{
		// "Basic" デバイスでは２つ以上のLayer Managerを登録できない
		TVPThrowExceptionMessage(TVPBasicDrawDeviceDoesNotSupporteLayerManagerMoreThanOne);
	}
	inherited::AddLayerManager(manager);

	manager->SetDesiredLayerType(ltOpaque); // ltOpaque な出力を受け取りたい
}

void TJS_INTF_METHOD tTVPOGLDrawDevice::SetTargetWindow(HWND wnd, bool is_main)
{
	if (!GLContext || (void*)wnd != GLContext->NativeWindow()) {
		DoneContext();
		if (wnd) {
			InitContext((void*)wnd);
		}
	}
}

void TJS_INTF_METHOD tTVPOGLDrawDevice::SetDestRectangle(const tTVPRect & rect)
{
	inherited::SetDestRectangle(rect);
	SurfaceWidth = 0;
	SurfaceHeight = 0;
}

//---------------------------------------------------------------------------
void TJS_INTF_METHOD tTVPOGLDrawDevice::NotifyLayerResize(iTVPLayerManager * manager)
{
	inherited::NotifyLayerResize(manager);
	if (!GLContext) return;

	GLContext->MakeCurrent();
	if (TextureInstance) {
		tjs_int w, h;
		GetSrcSize( w, h );
		if (TextureInstance->Resize(w, h)) {
			mTextureUpdateRect.Resize(w, h);
		} else {
			DestroyTexture();
			CreateTexture();
		}
	} else {
		CreateTexture();
	}
	InitUV();
}

void TJS_INTF_METHOD tTVPOGLDrawDevice::StartBitmapCompletion(iTVPLayerManager * manager)
{
	mTextureUpdateRect.Clear();
}
//---------------------------------------------------------------------------
void TJS_INTF_METHOD tTVPOGLDrawDevice::NotifyBitmapCompleted(iTVPLayerManager * manager,
	tjs_int x, tjs_int y, const void * bits, const BITMAPINFO * bmpinfo,
	const tTVPRect &cliprect, tTVPLayerType type, tjs_int opacity)
{
	if (!TextureInstance) return;

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
		long width_bytes   = cliprect.get_width() * 4; // 32bit
		const tjs_uint8 * src_p = (const tjs_uint8 *)bits;
		long src_pitch;

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

		mTextureUpdateRect.Update(TextureInstance, x, y, src_w, src_h, src_p, src_pitch, src_x, src_y);
	}
}
//---------------------------------------------------------------------------
void TJS_INTF_METHOD tTVPOGLDrawDevice::EndBitmapCompletion(iTVPLayerManager * manager)
{
	if (TextureInstance) {
		mTextureUpdateRect.RenderToTexture(TextureInstance);
	}
}

#ifdef __GENERIC__	

//---------------------------------------------------------------------------
//  VideoOverlay Support
//---------------------------------------------------------------------------

void 
tTVPOGLDrawDevice::UpdateVideoPosition(int w, int h)
{
    // 配置座標計算 (内接表示で補正)
    int sw =  SurfaceWidth;
    int sh =  SurfaceHeight;
    if (sw > 0 && sh > 0) {
        // 描画位置
        double scale = std::min((double)sw/w, (double)sh/h);
        int nw = w * scale;
        int nh = h * scale;
        int offx = (sw-nw)/2;
        int offy = (sh-nh)/2;

        // 描画位置を position換算
        int w2 = sw/2;
        int h2 = sh/2;
        float left    = (float)(offx      - w2) / w2;
        float top     = (float)(offy      - h2) / h2;
        float right   = (float)(offx + nw - w2) / w2;
        float bottom  = (float)(offy + nh - h2) / h2;

        _video_position[0] = left; // left top
        _video_position[1] = top;
        _video_position[2] = left; // left bottom
        _video_position[3] = bottom;
        _video_position[4] = right; // right top
        _video_position[5] = top;
        _video_position[6] = right; // right bottom
        _video_position[7] = bottom;
    }
}

void
tTVPOGLDrawDevice::UpdateVideo(int w, int h, std::function<void(char *dest, int pitch)> updator)
{
    std::lock_guard<std::mutex> lock( videooverlay_mutex_ );
	if (!mVideoBuffer) {
		// 動画バッファが無い場合は初期化
		mVideoBuffer = new char[w * h * 4]; // ARGB8888
		mVideoWidth = w;
		mVideoHeight = h;
	} else if (mVideoWidth != w || mVideoHeight != h) {
		// サイズが変わった場合は CPU 側バッファを再初期化。
		// ここはデコーダスレッドなので GL 操作 (delete _video_texture) は不可。
		// 旧サイズの _video_texture / 中の PBO は ShowVideo 側でサイズ不一致を
		// 検出して破棄・再生成する (そうしないと旧 PBO 容量を超える
		// glMapBufferRange / updator 書き込みが起きて GL メモリ側で overrun する)。
		delete[] mVideoBuffer;
		mVideoBuffer = new char[w * h * 4]; // ARGB8888
		mVideoWidth = w;
		mVideoHeight = h;
	}
	if (mVideoBuffer) {
		updator((char *)mVideoBuffer, w*4);
		UpdateVideoPosition(w, h);
		mVideoBufferDirty = true;
	}
}

bool
tTVPOGLDrawDevice::ShowVideo()
{
    if (mVideoBuffer) {
        if (mVideoBufferDirty) {
        	std::lock_guard<std::mutex> lock( videooverlay_mutex_ );
            // UpdateVideo 側で動画解像度が変わった場合、_video_texture と
            // 内部 PBO は旧サイズのままなので、ここで破棄して再生成する。
            // (GL 操作なので必ず render thread = ShowVideo 側で行う)
            if (_video_texture &&
                ((int)_video_texture->width()  != mVideoWidth ||
                 (int)_video_texture->height() != mVideoHeight)) {
                delete _video_texture;
                _video_texture = nullptr;
            }
            if (!_video_texture) {
                _video_texture = new GLTexture(mVideoWidth, mVideoHeight);
            }
            if (_video_texture) {

                int w = mVideoWidth;
                int h = mVideoHeight;
                char* src = (char*)mVideoBuffer;
                int spitch = mVideoWidth * 4;
                _video_texture->UpdateTexture(0, 0, w, h, [w,h,src,spitch](char *Dest, int pitch) {
                    if (pitch == spitch) {
                        // ピッチが正しい場合はデータをコピー
                        memcpy(Dest, src, pitch * h);
                    } else {
                        // ピッチが異なる場合は行ごとにコピー
                        for (int y = 0; y < h; ++y) {
                            memcpy((char *)Dest + y * pitch, (char*)src + y * spitch, spitch);
                        }
                    }
                });
            }
            mVideoBufferDirty = false;
        }
        if (_video_texture) {
            glClearColor(0.0f, 0.0f, 0.0f, 0.0f);
            glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
		    TextureDrawer.DrawTexture(_video_texture, SurfaceWidth, SurfaceHeight, _video_position);
        }

		return true;
	}
	return false;
}

void 
tTVPOGLDrawDevice::ClearVideo()
{
	std::lock_guard<std::mutex> lock( videooverlay_mutex_ );
    if (_video_texture) {
        delete _video_texture;
        _video_texture = 0;
    }
	if (mVideoBuffer) {
		delete[] mVideoBuffer;
		mVideoBuffer = nullptr;
	}
}

void 
tTVPOGLDrawDevice::SetWaitVSync(bool enable)
{
    if (GLContext) {
        GLContext->SetWaitVSync(enable);
    }
}

#endif








//---------------------------------------------------------------------------
// tTJSNI_OGLDrawDevice : OGLDrawDevice TJS native class
//---------------------------------------------------------------------------
tjs_uint32 tTJSNC_OGLDrawDevice::ClassID = (tjs_uint32)-1;
tTJSNC_OGLDrawDevice::tTJSNC_OGLDrawDevice() :
	tTJSNativeClass(TJS_W("OGLDrawDevice"))
{
	// register native methods/properties
	TJS_BEGIN_NATIVE_MEMBERS(OGLDrawDevice)
	TJS_DECL_EMPTY_FINALIZE_METHOD
//----------------------------------------------------------------------
// constructor/methods
//----------------------------------------------------------------------
TJS_BEGIN_NATIVE_CONSTRUCTOR_DECL(/*var.name*/_this, /*var.type*/tTJSNI_OGLDrawDevice,
	/*TJS class name*/OGLDrawDevice)
{
	return TJS_S_OK;
}
TJS_END_NATIVE_CONSTRUCTOR_DECL(/*TJS class name*/OGLDrawDevice)
//----------------------------------------------------------------------
TJS_BEGIN_NATIVE_METHOD_DECL(/*func. name*/recreate)
{
	TJS_GET_NATIVE_INSTANCE(/*var. name*/_this, /*var. type*/tTJSNI_OGLDrawDevice);
	// Surfaceつくりなおし対応？ XXX
	return TJS_S_OK;
}
TJS_END_NATIVE_METHOD_DECL(/*func. name*/recreate)
//----------------------------------------------------------------------
TJS_BEGIN_NATIVE_METHOD_DECL(/*func. name*/createCanvas)
{
	TJS_GET_NATIVE_INSTANCE(/*var. name*/_this, /*var. type*/tTJSNI_OGLDrawDevice);
	_this->GetDevice()->RequestCreateCanvas();
	return TJS_S_OK;
}
TJS_END_NATIVE_METHOD_DECL(/*func. name*/createCanvas)

//----------------------------------------------------------------------


//---------------------------------------------------------------------------
//----------------------------------------------------------------------
// properties
//----------------------------------------------------------------------
TJS_BEGIN_NATIVE_PROP_DECL(interface)
{
	TJS_BEGIN_NATIVE_PROP_GETTER
	{
		TJS_GET_NATIVE_INSTANCE(/*var. name*/_this, /*var. type*/tTJSNI_OGLDrawDevice);
		*result = reinterpret_cast<tjs_int64>(_this->GetDevice());
		return TJS_S_OK;
	}
	TJS_END_NATIVE_PROP_GETTER

	TJS_DENY_NATIVE_PROP_SETTER
}
TJS_END_NATIVE_PROP_DECL(interface)
//---------------------------------------------------------------------------
TJS_BEGIN_NATIVE_PROP_DECL(window)
{
	TJS_BEGIN_NATIVE_PROP_GETTER
	{
		TJS_GET_NATIVE_INSTANCE(/*var. name*/_this, /*var. type*/tTJSNI_OGLDrawDevice);
		*result = _this->GetDevice()->GetWindowObject();
		return TJS_S_OK;
	}
	TJS_END_NATIVE_PROP_GETTER

	TJS_DENY_NATIVE_PROP_SETTER
}
TJS_END_NATIVE_PROP_DECL(window)
//---------------------------------------------------------------------------
TJS_BEGIN_NATIVE_PROP_DECL(canvas)
{
	TJS_BEGIN_NATIVE_PROP_GETTER
	{
		TJS_GET_NATIVE_INSTANCE(/*var. name*/_this, /*var. type*/tTJSNI_OGLDrawDevice);
		*result = _this->GetDevice()->GetCanvasObject();
		return TJS_S_OK;
	}
	TJS_END_NATIVE_PROP_GETTER

	TJS_DENY_NATIVE_PROP_SETTER
}
TJS_END_NATIVE_PROP_DECL(canvas)
//---------------------------------------------------------------------------
TJS_BEGIN_NATIVE_PROP_DECL(texture)
{
	TJS_BEGIN_NATIVE_PROP_GETTER
	{
		TJS_GET_NATIVE_INSTANCE(/*var. name*/_this, /*var. type*/tTJSNI_OGLDrawDevice);
		*result = _this->GetDevice()->GetTextureObject();
		return TJS_S_OK;
	}
	TJS_END_NATIVE_PROP_GETTER

	TJS_DENY_NATIVE_PROP_SETTER
}
TJS_END_NATIVE_PROP_DECL(texture)
//---------------------------------------------------------------------------
TJS_BEGIN_NATIVE_PROP_DECL(matrix)
{
	TJS_BEGIN_NATIVE_PROP_GETTER
	{
		TJS_GET_NATIVE_INSTANCE(/*var. name*/_this, /*var. type*/tTJSNI_OGLDrawDevice);
		*result = _this->GetDevice()->GetMatrixObject();
		return TJS_S_OK;
	}
	TJS_END_NATIVE_PROP_GETTER

	TJS_DENY_NATIVE_PROP_SETTER
}
TJS_END_NATIVE_PROP_DECL(matrix)
//----------------------------------------------------------------------
	TJS_END_NATIVE_MEMBERS
}
//---------------------------------------------------------------------------
iTJSNativeInstance *tTJSNC_OGLDrawDevice::CreateNativeInstance()
{
	return new tTJSNI_OGLDrawDevice();
}
//---------------------------------------------------------------------------
tTJSNI_OGLDrawDevice::tTJSNI_OGLDrawDevice()
{
	Device = nullptr;
}
//---------------------------------------------------------------------------
tTJSNI_OGLDrawDevice::~tTJSNI_OGLDrawDevice()
{
	if(Device) Device->Destruct(), Device = NULL;
}
//---------------------------------------------------------------------------
tjs_error TJS_INTF_METHOD
	tTJSNI_OGLDrawDevice::Construct(tjs_int numparams, tTJSVariant **param,
		iTJSDispatch2 *tjs_obj)
{
	Device = new tTVPOGLDrawDevice(tjs_obj);
	return TJS_S_OK;
}
//---------------------------------------------------------------------------
void TJS_INTF_METHOD tTJSNI_OGLDrawDevice::Invalidate()
{
	if(Device) Device->Destruct(), Device = NULL;
}
//---------------------------------------------------------------------------

