//---------------------------------------------------------------------------
// SDLGpuDrawDevice: SDLDrawDevice に Canvas (GPU 版) を載せた描画デバイス
//---------------------------------------------------------------------------
#include "tjsCommHead.h"

#include "SDLGpuDrawDevice.h"
#include "GpuDeviceSDL.h"
#include "GpuCanvasIntf.h"
#include "WindowIntf.h"
#include "MsgIntf.h"
#include "DebugIntf.h"
#include "LogIntf.h"
#include "ScriptMgnIntf.h"   // TVPScriptExceptionShownCount
#include <vector>

//---------------------------------------------------------------------------
tTVPSDLGpuDrawDevice::tTVPSDLGpuDrawDevice( iTJSDispatch2* tjs_obj )
	: inherited( tjs_obj ), SelfObj( tjs_obj )
{
	if( SelfObj ) SelfObj->AddRef();
}

tTVPSDLGpuDrawDevice::~tTVPSDLGpuDrawDevice()
{
}

void TJS_INTF_METHOD tTVPSDLGpuDrawDevice::Destruct()
{
	// base の Destruct が renderer を破棄する (OnRendererDestroying で後始末)
	inherited::Destruct();
	DestroyCanvas();
	DestroyLayerTexture();
	if( SelfObj ) SelfObj->Release();
	SelfObj = nullptr;
}

//---------------------------------------------------------------------------
// renderer は Canvas と同じ GPU デバイスで作る
//---------------------------------------------------------------------------
//
// renderer はウィンドウに付けたまま使い回す (描画デバイスを差し替えても作り直さない)。
// SDL_GPU のドライバによってはウィンドウの手放し (SDL_ReleaseWindowFromGPUDevice) が
// 無く、作り直すとウィンドウを握り直せないため。renderer はウィンドウの破棄時に SDL が
// 一緒に破棄する。
//---------------------------------------------------------------------------
static const char* const TVPGpuRendererProp = "krkrz.gpudrawdevice.renderer";

SDL_Renderer* tTVPSDLGpuDrawDevice::CreateSDLRenderer( SDL_Window* sdl_wnd, const char* requested )
{
	SDL_Renderer* cached = SDL_GetRenderer( sdl_wnd );
	if( cached && SDL_GetPointerProperty( SDL_GetWindowProperties( sdl_wnd ), TVPGpuRendererProp, nullptr ) == cached )
		return cached;
	SDL_GPUDevice* dev = nullptr;
	try {
		dev = (SDL_GPUDevice*)TVPGpu::Device::Get()->NativeHandle();
	} catch( ... ) {
		dev = nullptr;
	}
	if( dev ) {
		SDL_Renderer* r = SDL_CreateGPURenderer( dev, sdl_wnd );
		if( r ) {
			SDL_SetPointerProperty( SDL_GetWindowProperties( sdl_wnd ), TVPGpuRendererProp, r );
			return r;
		}
		TVPLOG_ERROR( "GPUDrawDevice: SDL_CreateGPURenderer failed: {}", SDL_GetError() );
	}
	TVPAddImportantLog( TJS_W("GPUDrawDevice: GPU device is not available, Canvas is disabled") );
	return inherited::CreateSDLRenderer( sdl_wnd, requested );
}

void tTVPSDLGpuDrawDevice::DestroySDLRenderer( SDL_Renderer* renderer )
{
	SDL_Window* wnd = SDL_GetRenderWindow( renderer );
	if( wnd && SDL_GetPointerProperty( SDL_GetWindowProperties( wnd ), TVPGpuRendererProp, nullptr ) == renderer )
		return;   // ウィンドウに付けたまま残す
	inherited::DestroySDLRenderer( renderer );
}

void tTVPSDLGpuDrawDevice::FireEvent( const tjs_char* name, bool immediate )
{
	if( !SelfObj ) return;
	ttstr ev( name );
	TVPPostEvent( SelfObj, SelfObj, ev, 0, immediate ? TVP_EPT_IMMEDIATE : TVP_EPT_POST, 0, nullptr );
}

void tTVPSDLGpuDrawDevice::OnRendererCreated()
{
	if( !TVPGpu::Device::GetIfCreated() ) return;
	CreateLayerTexture();
	if( DoCreateCanvas ) CreateCanvas();
	// renderer はウィンドウ更新の配送中 (Window::UpdateContent → SetTargetWindow) に
	// 遅延生成される。 ここで onInit を即時発火すると、 onInit のスクリプト例外で出る
	// エラーダイアログのモーダルループがイベント配送の内側で回り、 ウィンドウ更新も
	// タイマーも配送されずにダイアログもゲームも描画されないまま固まる。
	// 通常の post にして、 配送が一巡したあと (次の配送) で呼ぶ。
	FireEvent( TJS_W("onInit"), false );
}

void tTVPSDLGpuDrawDevice::OnRendererDestroying()
{
	FireEvent( TJS_W("onDone") );
	DestroyScreen();
}

//---------------------------------------------------------------------------
// Canvas
//---------------------------------------------------------------------------
void tTVPSDLGpuDrawDevice::RequestCreateCanvas()
{
	if( !mRenderer || !TVPGpu::Device::GetIfCreated() ) {
		DoCreateCanvas = true;
		return;
	}
	if( !CanvasInstance ) CreateCanvas();
}

void tTVPSDLGpuDrawDevice::CreateCanvas()
{
	DoCreateCanvas = false;
	iTJSDispatch2* cls = new tTJSNC_GpuCanvas();
	iTJSDispatch2* obj = nullptr;
	try {
		if( TJS_FAILED( cls->CreateNew( 0, nullptr, nullptr, &obj, 0, nullptr, cls ) ) )
			TVPThrowExceptionMessage( TVPInternalError, TJS_W("tTJSNI_GpuCanvas::Construct") );
		SetCanvasObject( tTJSVariant( obj, obj ) );
	} catch( ... ) {
		cls->Release();
		if( obj ) obj->Release();
		throw;
	}
	cls->Release();
	if( obj ) obj->Release();
	if( Window ) Window->RequestUpdate();
}

void tTVPSDLGpuDrawDevice::DestroyCanvas()
{
	SetCanvasObject( tTJSVariant() );
}

void tTVPSDLGpuDrawDevice::SetCanvasObject( const tTJSVariant& val )
{
	if( CanvasObject.Type() == tvtObject )
		CanvasObject.AsObjectClosureNoAddRef().Invalidate( 0, nullptr, nullptr, CanvasObject.AsObjectNoAddRef() );
	CanvasObject = val;
	CanvasInstance = nullptr;
	if( val.Type() == tvtObject && val.AsObjectNoAddRef() ) {
		CanvasInstance = (tTJSNI_GpuCanvas*)TJSGetNativeInstance( tTJSNC_GpuCanvas::ClassID, &CanvasObject );
		if( !CanvasInstance ) TVPThrowExceptionMessage( TVPCannotRetriveInstance, TJS_W("canvas") );
	}
}

//---------------------------------------------------------------------------
// レイヤー画像 (texture プロパティ)
//---------------------------------------------------------------------------
void tTVPSDLGpuDrawDevice::CreateLayerTexture()
{
	if( TextureInstance ) return;
	tjs_int w, h;
	GetSrcSize( w, h );
	if( w <= 0 || h <= 0 ) return;
	iTJSDispatch2* cls = new tTJSNC_GpuTexture();
	iTJSDispatch2* obj = nullptr;
	try {
		tTJSVariant pw = w, ph = h;
		tTJSVariant* pp[2] = { &pw, &ph };
		if( TJS_FAILED( cls->CreateNew( 0, nullptr, nullptr, &obj, 2, pp, cls ) ) )
			TVPThrowExceptionMessage( TVPInternalError, TJS_W("tTJSNI_GpuTexture::Construct") );
		TextureObject = tTJSVariant( obj, obj );
	} catch( ... ) {
		cls->Release();
		if( obj ) obj->Release();
		throw;
	}
	cls->Release();
	if( obj ) obj->Release();
	TextureInstance = (tTJSNI_GpuTexture*)TJSGetNativeInstance( tTJSNC_GpuTexture::ClassID, &TextureObject );
	// 全面を描き直させる
	RequestInvalidation( tTVPRect( 0, 0, w, h ) );
}

void tTVPSDLGpuDrawDevice::DestroyLayerTexture()
{
	if( TextureObject.Type() == tvtObject )
		TextureObject.AsObjectClosureNoAddRef().Invalidate( 0, nullptr, nullptr, TextureObject.AsObjectNoAddRef() );
	TextureObject.Clear();
	TextureInstance = nullptr;
}

void TJS_INTF_METHOD tTVPSDLGpuDrawDevice::NotifyLayerResize( iTVPLayerManager* manager )
{
	inherited::NotifyLayerResize( manager );
	if( !TVPGpu::Device::GetIfCreated() || !mRenderer ) return;
	tjs_int w, h;
	GetSrcSize( w, h );
	if( !TextureInstance || (tjs_int)TextureInstance->GetWidth() != w || (tjs_int)TextureInstance->GetHeight() != h ) {
		DestroyLayerTexture();
		CreateLayerTexture();
	}
}

void TJS_INTF_METHOD tTVPSDLGpuDrawDevice::NotifyBitmapCompleted( iTVPLayerManager* manager,
	tjs_int x, tjs_int y, const void* bits, const BITMAPINFO* bmpinfo,
	const tTVPRect& cliprect, tTVPLayerType type, tjs_int opacity )
{
	inherited::NotifyBitmapCompleted( manager, x, y, bits, bmpinfo, cliprect, type, opacity );
	if( !TextureInstance ) return;

	// DIB (上下どちら向きもありうる) の cliprect を、x,y の位置へ転送する
	const int bw = bmpinfo->bmiHeader.biWidth;
	const int bh = bmpinfo->bmiHeader.biHeight;
	const int ah = bh < 0 ? -bh : bh;
	const int pitch = bmpinfo->bmiHeader.biSizeImage / ah;
	const int w = cliprect.get_width(), h = cliprect.get_height();
	if( w <= 0 || h <= 0 || x < 0 || y < 0 ||
		x + w > (tjs_int)TextureInstance->GetMemoryWidth() || y + h > (tjs_int)TextureInstance->GetMemoryHeight() ||
		cliprect.left < 0 || cliprect.top < 0 || cliprect.right > bw || cliprect.bottom > ah ) return;
	std::vector<tjs_uint32> buf( (size_t)w * h );
	const tjs_uint8* base = (const tjs_uint8*)bits;
	for( int row = 0; row < h; row++ ) {
		const int sy = cliprect.top + row;
		const tjs_uint8* line = bh < 0 ? base + (size_t)pitch * sy : base + (size_t)pitch * ( ah - 1 - sy );
		memcpy( &buf[(size_t)w * row], line + cliprect.left * 4, (size_t)w * 4 );
	}
	TextureInstance->UpdateDirect( x, y, w, h, buf.data(), w * 4 );
}

//---------------------------------------------------------------------------
// 画面
//---------------------------------------------------------------------------
void tTVPSDLGpuDrawDevice::EnsureScreen( int w, int h )
{
	if( ScreenTex && ScreenW == w && ScreenH == h ) return;
	DestroyScreen();
	if( w <= 0 || h <= 0 ) return;
	TVPGpu::TextureDesc d;
	d.width = w; d.height = h;
	d.format = TVPGpu::TextureFormat::BGRA8;
	d.renderTarget = true;
	ScreenTex = TVPGpu::Device::Get()->CreateTexture( d );
	ScreenW = w; ScreenH = h;

	SDL_PropertiesID p = SDL_CreateProperties();
	SDL_SetNumberProperty( p, SDL_PROP_TEXTURE_CREATE_FORMAT_NUMBER, TVPGpu::GetSDLPixelFormat( ScreenTex.get() ) );
	SDL_SetNumberProperty( p, SDL_PROP_TEXTURE_CREATE_ACCESS_NUMBER, SDL_TEXTUREACCESS_STATIC );
	SDL_SetNumberProperty( p, SDL_PROP_TEXTURE_CREATE_WIDTH_NUMBER, w );
	SDL_SetNumberProperty( p, SDL_PROP_TEXTURE_CREATE_HEIGHT_NUMBER, h );
	SDL_SetPointerProperty( p, SDL_PROP_TEXTURE_CREATE_GPU_TEXTURE_POINTER, ScreenTex->NativeHandle() );
	ScreenWrap = SDL_CreateTextureWithProperties( mRenderer, p );
	SDL_DestroyProperties( p );
	if( !ScreenWrap ) {
		TVPLOG_ERROR( "GPUDrawDevice: cannot wrap the canvas texture: {}", SDL_GetError() );
		return;
	}
	SDL_SetTextureBlendMode( ScreenWrap, SDL_BLENDMODE_NONE );
	SDL_SetTextureScaleMode( ScreenWrap, SDL_SCALEMODE_LINEAR );
}

void tTVPSDLGpuDrawDevice::DestroyScreen()
{
	if( ScreenWrap ) SDL_DestroyTexture( ScreenWrap );
	ScreenWrap = nullptr;
	if( CanvasInstance ) CanvasInstance->SetScreenTarget( nullptr, 0, 0 );
	ScreenTex.reset();
	ScreenW = ScreenH = 0;
}

void TJS_INTF_METHOD tTVPSDLGpuDrawDevice::Show()
{
	if( CanvasInstance && mRenderer && NIWindow && TVPGpu::Device::GetIfCreated() && !IsVideoCoveringScreen() ) {
		// Canvas の座標系はレイヤー (プライマリレイヤー) と同じ。画面へはレイヤー画像と
		// 同じ矩形 (RenderContent の dst) へ拡縮して出す。ウィンドウの内側サイズとは
		// 限らない (PS5 は内側が 1920x1080 固定で、レイヤーが 1280x720 のことがある)
		tjs_int sw = 0, sh = 0;
		GetSrcSize( sw, sh );
		if( sw <= 0 || sh <= 0 ) {
			sw = NIWindow->GetInnerWidth();
			sh = NIWindow->GetInnerHeight();
		}
		EnsureScreen( sw, sh );
		if( ScreenTex ) {
			CanvasInstance->SetScreenTarget( ScreenTex.get(), sw, sh );
			CanvasInstance->BeginDrawing();
			try {
				// onDraw が毎フレーム例外を投げ続けるのを止める (OGLDrawDevice と同じ)
				if( !OnDrawDisabled ) {
					tjs_uint64 shown = TVPScriptExceptionShownCount;
					FireEvent( TJS_W("onDraw") );
					if( TVPScriptExceptionShownCount != shown ) {
						if( OnDrawExceptionGuard.OnException() ) {
							OnDrawDisabled = true;
							TVPAddImportantLog( TJS_W("GPUDrawDevice: onDraw suspended after repeated exceptions. Call drawDevice.resumeOnDraw() to resume.") );
						}
					} else {
						OnDrawExceptionGuard.OnSuccess();
					}
				}
			} catch( ... ) {
				CanvasInstance->EndDrawing();
				throw;
			}
			CanvasInstance->EndDrawing();
		}
	}
	inherited::Show();
}

void tTVPSDLGpuDrawDevice::RenderContent( SDL_Renderer* renderer, const SDL_FRect& dst )
{
	if( CanvasInstance && ScreenWrap ) {
		// Canvas はレイヤーと同じ座標系で描いているので、レイヤー画像と同じ矩形へ出す
		SDL_RenderTexture( renderer, ScreenWrap, nullptr, &dst );
		return;
	}
	inherited::RenderContent( renderer, dst );
}

//---------------------------------------------------------------------------
// tTJSNI_SDLGpuDrawDevice
//---------------------------------------------------------------------------
tTJSNI_SDLGpuDrawDevice::~tTJSNI_SDLGpuDrawDevice()
{
	if( Device ) Device->Destruct(), Device = nullptr;
}

tjs_error TJS_INTF_METHOD tTJSNI_SDLGpuDrawDevice::Construct( tjs_int numparams, tTJSVariant** param, iTJSDispatch2* tjs_obj )
{
	Device = new tTVPSDLGpuDrawDevice( tjs_obj );
	return TJS_S_OK;
}

void TJS_INTF_METHOD tTJSNI_SDLGpuDrawDevice::Invalidate()
{
	if( Device ) Device->Destruct(), Device = nullptr;
}

//---------------------------------------------------------------------------
// tTJSNC_SDLGpuDrawDevice
//---------------------------------------------------------------------------
tjs_uint32 tTJSNC_SDLGpuDrawDevice::ClassID = (tjs_uint32)-1;
tTJSNC_SDLGpuDrawDevice::tTJSNC_SDLGpuDrawDevice( const tjs_char* name ) : tTJSNativeClass( name )
{
	TJS_BEGIN_NATIVE_MEMBERS( GPUDrawDevice )
	TJS_DECL_EMPTY_FINALIZE_METHOD
	// OGLDrawDevice の名前でも登録するので、どちらの名前でも super.XXX() できるようにする
	TJS_BEGIN_NATIVE_CONSTRUCTOR_DECL( _this, tTJSNI_SDLGpuDrawDevice, GPUDrawDevice ) { return TJS_S_OK; }
	TJS_END_NATIVE_CONSTRUCTOR_DECL( GPUDrawDevice )
	TJS_BEGIN_NATIVE_CONSTRUCTOR_DECL( _this, tTJSNI_SDLGpuDrawDevice, OGLDrawDevice ) { return TJS_S_OK; }
	TJS_END_NATIVE_CONSTRUCTOR_DECL( OGLDrawDevice )

	TJS_BEGIN_NATIVE_METHOD_DECL( createCanvas )
	{
		TJS_GET_NATIVE_INSTANCE( _this, tTJSNI_SDLGpuDrawDevice );
		if( _this->GetDevice() ) _this->GetDevice()->RequestCreateCanvas();
		return TJS_S_OK;
	}
	TJS_END_NATIVE_METHOD_DECL( createCanvas )

	TJS_BEGIN_NATIVE_METHOD_DECL( resumeOnDraw )
	{
		TJS_GET_NATIVE_INSTANCE( _this, tTJSNI_SDLGpuDrawDevice );
		if( _this->GetDevice() ) _this->GetDevice()->ResumeOnDraw();
		return TJS_S_OK;
	}
	TJS_END_NATIVE_METHOD_DECL( resumeOnDraw )

	TJS_BEGIN_NATIVE_METHOD_DECL( recreate ) { return TJS_S_OK; }
	TJS_END_NATIVE_METHOD_DECL( recreate )

#define GPU_DD_RO_PROP( name, expr ) \
	TJS_BEGIN_NATIVE_PROP_DECL( name ) { \
		TJS_BEGIN_NATIVE_PROP_GETTER { \
			TJS_GET_NATIVE_INSTANCE( _this, tTJSNI_SDLGpuDrawDevice ); \
			if( !_this->GetDevice() ) return TJS_E_FAIL; \
			*result = expr; return TJS_S_OK; \
		} TJS_END_NATIVE_PROP_GETTER \
		TJS_DENY_NATIVE_PROP_SETTER \
	} TJS_END_NATIVE_PROP_DECL( name )

	// interface は Windows のヘッダでマクロ (struct) なので、GPU_DD_RO_PROP に通すと
	// 引数が展開されて別名で登録される。直接書く
	TJS_BEGIN_NATIVE_PROP_DECL( interface ) {
		TJS_BEGIN_NATIVE_PROP_GETTER {
			TJS_GET_NATIVE_INSTANCE( _this, tTJSNI_SDLGpuDrawDevice );
			*result = reinterpret_cast<tjs_int64>( _this->GetDevice() ); return TJS_S_OK;
		} TJS_END_NATIVE_PROP_GETTER
		TJS_DENY_NATIVE_PROP_SETTER
	} TJS_END_NATIVE_PROP_DECL( interface )
	GPU_DD_RO_PROP( window, _this->GetDevice()->GetWindowObject() )
	GPU_DD_RO_PROP( canvas, _this->GetDevice()->GetCanvasObject() )
	GPU_DD_RO_PROP( texture, _this->GetDevice()->GetTextureObject() )
	GPU_DD_RO_PROP( sdlVideoPresenterHost, reinterpret_cast<tjs_int64>( static_cast<iTVPSDLVideoPresenterHost*>( _this->GetDevice() ) ) )
	GPU_DD_RO_PROP( viewportBackgroundHost, reinterpret_cast<tjs_int64>( static_cast<iTVPViewportBackgroundHost*>( _this->GetDevice() ) ) )
#ifdef KRKRZ_HAS_ELEMENTS
	GPU_DD_RO_PROP( dialogRendererHost, reinterpret_cast<tjs_int64>( static_cast<iTVPDialogRendererHost*>( _this->GetDevice() ) ) )
#endif

	TJS_END_NATIVE_MEMBERS
}
