/**
 * GLCompositor クラス実装
 * 詳細は GLCompositorIntf.h のヘッダコメント参照。
 */
#include "tjsCommHead.h"

#include "GLCompositorIntf.h"
#include "CanvasIntf.h"
#include "OffscreenIntf.h"
#include "TextureIntf.h"
#include "Matrix32Intf.h"
#include "BitmapIntf.h"
#include "OpenGLContext.h"
#include "OpenGLHeader.h"
#include "LayerIntf.h"
#include "WindowIntf.h"
#include "WindowImpl.h"
// フォーム参照用 (OGLDrawDevice.cpp と同じガード)
#ifdef __WINVER__
#include "WindowFormUnit.h"
#else
#include "WindowForm.h"
#endif
#include "MsgIntf.h"

//---------------------------------------------------------------------------
tjs_uint32 tTJSNC_GLCompositor::ClassID = (tjs_uint32)-1;

//---------------------------------------------------------------------------
tTJSNI_GLCompositor::tTJSNI_GLCompositor()
	: NativeWindow(nullptr), Context(nullptr),
	  CanvasInstance(nullptr), CanvasMatrixInstance(nullptr),
	  TargetInstance(nullptr), TargetWidth(0), TargetHeight(0),
	  BitmapInstance(nullptr),
	  Width(32), Height(32),
	  Owner(nullptr)
{
}
//---------------------------------------------------------------------------
tTJSNI_GLCompositor::~tTJSNI_GLCompositor()
{
}
//---------------------------------------------------------------------------
tjs_error TJS_INTF_METHOD tTJSNI_GLCompositor::Construct(tjs_int numparams, tTJSVariant **param, iTJSDispatch2 *tjs_obj)
{
	if( numparams < 1 ) return TJS_E_BADPARAMCOUNT;

	// 自分自身の TJS オブジェクトを記録 (callback の objthis として使う)。非 AddRef。
	Owner = tjs_obj;

	// Window から native window ハンドルを得る (OGLDrawDevice と同じ手順)
	iTJSDispatch2 *winobj = param[0]->AsObjectNoAddRef();
	tTJSNI_Window *NIWindow = nullptr;
	if( winobj == nullptr ||
		TJS_FAILED(winobj->NativeInstanceSupport(TJS_NIS_GETINSTANCE,
			tTJSNC_Window::ClassID, (iTJSNativeInstance**)&NIWindow)) ) {
		TVPThrowExceptionMessage(TVPGLCompositor1stArgumentMustBeAWindow);
	}

#ifdef __WINVER__
	NativeWindow = NIWindow->GetForm()->GetHandle();
#else
	NativeWindow = NIWindow->GetForm()->NativeWindowHandle();
#endif

	// 描画デバイスに依存せず、ウィンドウ用の GL コンテキストを取得してカレントにする。
	Context = iTVPGLContext::GetContext(NativeWindow);
	if( !Context ) {
		TVPThrowExceptionMessage(TVPGLCompositorFailedToGetGLContext);
	}
	Context->MakeCurrent();
	InitGLES();

	// 内部 Canvas を生成 (シェーダコンパイル等が走るので context カレント後)
	CreateCanvas();

	return TJS_S_OK;
}
//---------------------------------------------------------------------------
void TJS_INTF_METHOD tTJSNI_GLCompositor::Invalidate()
{
	if( Context ) Context->MakeCurrent();

	if( BitmapObject.Type() == tvtObject )
		BitmapObject.AsObjectClosureNoAddRef().Invalidate( 0, NULL, NULL, BitmapObject.AsObjectNoAddRef() );
	BitmapObject.Clear();
	BitmapInstance = nullptr;

	if( TargetObject.Type() == tvtObject )
		TargetObject.AsObjectClosureNoAddRef().Invalidate( 0, NULL, NULL, TargetObject.AsObjectNoAddRef() );
	TargetObject.Clear();
	TargetInstance = nullptr;

	CanvasMatrixInstance = nullptr;
	if( CanvasObject.Type() == tvtObject )
		CanvasObject.AsObjectClosureNoAddRef().Invalidate( 0, NULL, NULL, CanvasObject.AsObjectNoAddRef() );
	CanvasObject.Clear();
	CanvasInstance = nullptr;

	if( Context ) {
		Context->Release();
		Context = nullptr;
	}
}
//---------------------------------------------------------------------------
void tTJSNI_GLCompositor::MakeCurrent()
{
	if( Context ) Context->MakeCurrent();
}
//---------------------------------------------------------------------------
void tTJSNI_GLCompositor::CreateCanvas()
{
	iTJSDispatch2 * cls = nullptr;
	iTJSDispatch2 * newobj = nullptr;
	try {
		cls = new tTJSNC_Canvas();
		if( TJS_FAILED( cls->CreateNew( 0, nullptr, nullptr, &newobj, 0, nullptr, cls ) ) )
			TVPThrowExceptionMessage( TVPInternalError, TJS_W("tTJSNI_Canvas::Construct") );
		CanvasObject = tTJSVariant( newobj, newobj );
		if( TJS_FAILED( newobj->NativeInstanceSupport( TJS_NIS_GETINSTANCE,
				tTJSNC_Canvas::ClassID, (iTJSNativeInstance**)&CanvasInstance ) ) )
			TVPThrowExceptionMessage( TVPInternalError, TJS_W("tTJSNI_Canvas instance") );
	} catch( ... ) {
		if( cls ) cls->Release();
		if( newobj ) newobj->Release();
		throw;
	}
	if( cls ) cls->Release();
	if( newobj ) newobj->Release();

	// drawLayer 用に Canvas の matrix インスタンスを取得しておく
	CanvasMatrixInstance = nullptr;
	const tTJSVariant &mo = CanvasInstance->GetMatrix32Object();
	if( mo.Type() == tvtObject ) {
		mo.AsObjectNoAddRef()->NativeInstanceSupport( TJS_NIS_GETINSTANCE,
			tTJSNC_Matrix32::ClassID, (iTJSNativeInstance**)&CanvasMatrixInstance );
	}
}
//---------------------------------------------------------------------------
void tTJSNI_GLCompositor::EnsureBitmap()
{
	if( BitmapInstance ) return;
	iTJSDispatch2 * cls = nullptr;
	iTJSDispatch2 * newobj = nullptr;
	try {
		cls = new tTJSNC_Bitmap();
		if( TJS_FAILED( cls->CreateNew( 0, nullptr, nullptr, &newobj, 0, nullptr, cls ) ) )
			TVPThrowExceptionMessage( TVPInternalError, TJS_W("tTJSNI_Bitmap::Construct") );
		BitmapObject = tTJSVariant( newobj, newobj );
		if( TJS_FAILED( newobj->NativeInstanceSupport( TJS_NIS_GETINSTANCE,
				tTJSNC_Bitmap::ClassID, (iTJSNativeInstance**)&BitmapInstance ) ) )
			TVPThrowExceptionMessage( TVPInternalError, TJS_W("tTJSNI_Bitmap instance") );
	} catch( ... ) {
		if( cls ) cls->Release();
		if( newobj ) newobj->Release();
		throw;
	}
	if( cls ) cls->Release();
	if( newobj ) newobj->Release();
}
//---------------------------------------------------------------------------
void tTJSNI_GLCompositor::DrawLayer(tTJSNI_BaseLayer* layer, float a, float b, float c, float d, float tx, float ty, int opacity)
{
	if( !layer || !CanvasInstance ) return;

	// レイヤのメインイメージを Bitmap へ (内部ビットマップ参照の移送で安価)。
	EnsureBitmap();
	layer->CopyFromMainImage( BitmapInstance );

	// Bitmap から Texture を構築する (drawTexture の正規経路。向き/UV/サイズが正しい)。
	iTJSDispatch2 * cls = nullptr;
	iTJSDispatch2 * texobj = nullptr;
	tTJSNI_Texture* tex = nullptr;
	try {
		cls = new tTJSNC_Texture();
		tTJSVariant tp0( BitmapObject );
		tTJSVariant *pparam[1] = { &tp0 };
		if( TJS_FAILED( cls->CreateNew( 0, nullptr, nullptr, &texobj, 1, pparam, cls ) ) )
			TVPThrowExceptionMessage( TVPInternalError, TJS_W("tTJSNI_Texture::Construct") );
		texobj->NativeInstanceSupport( TJS_NIS_GETINSTANCE,
			tTJSNC_Texture::ClassID, (iTJSNativeInstance**)&tex );

		if( tex ) {
			// アフィン変換を Canvas の matrix へ設定
			if( CanvasMatrixInstance ) CanvasMatrixInstance->Set( a, b, c, d, tx, ty );

			// 不透明度を既定シェーダの a_opacity へ設定
			const tTJSVariant &shobj = CanvasInstance->GetDefaultShader();
			bool shaderReady = ( shobj.Type() == tvtObject );
			static ttstr a_opacity( TJS_W("a_opacity") );
			if( shaderReady ) {
				tTJSVariant op( (tjs_real)opacity / 255.0 );
				iTJSDispatch2 *sh = shobj.AsObjectNoAddRef();
				sh->PropSet( TJS_MEMBERENSURE, a_opacity.c_str(), a_opacity.GetHint(), &op, sh );
			}

			CanvasInstance->DrawTexture( tex, nullptr );

			if( shaderReady ) {
				tTJSVariant one( (tjs_real)1.0 );
				iTJSDispatch2 *sh = shobj.AsObjectNoAddRef();
				sh->PropSet( TJS_MEMBERENSURE, a_opacity.c_str(), a_opacity.GetHint(), &one, sh );
			}
		}
	} catch( ... ) {
		if( cls ) cls->Release();
		if( texobj ) texobj->Release();
		throw;
	}
	// texobj は Invalidate してから解放 (GL リソースを即解放)
	if( texobj ) {
		texobj->Invalidate( 0, nullptr, nullptr, texobj );
		texobj->Release();
	}
	if( cls ) cls->Release();
}
//---------------------------------------------------------------------------
void tTJSNI_GLCompositor::CopyLayer(tTJSNI_BaseLayer* layer, int left, int top)
{
	DrawLayer( layer, 1.0f, 0.0f, 0.0f, 1.0f, (float)left, (float)top, 255 );
}
//---------------------------------------------------------------------------
void tTJSNI_GLCompositor::EnsureTarget(int w, int h)
{
	if( TargetInstance && TargetWidth == w && TargetHeight == h ) return;

	// 旧ターゲットを破棄
	if( TargetObject.Type() == tvtObject )
		TargetObject.AsObjectClosureNoAddRef().Invalidate( 0, NULL, NULL, TargetObject.AsObjectNoAddRef() );
	TargetObject.Clear();
	TargetInstance = nullptr;

	iTJSDispatch2 * cls = nullptr;
	iTJSDispatch2 * newobj = nullptr;
	try {
		cls = new tTJSNC_Offscreen();
		tTJSVariant p0((tjs_int)w), p1((tjs_int)h);
		tTJSVariant *pparam[2] = { &p0, &p1 };
		if( TJS_FAILED( cls->CreateNew( 0, nullptr, nullptr, &newobj, 2, pparam, cls ) ) )
			TVPThrowExceptionMessage( TVPInternalError, TJS_W("tTJSNI_Offscreen::Construct") );
		TargetObject = tTJSVariant( newobj, newobj );
		if( TJS_FAILED( newobj->NativeInstanceSupport( TJS_NIS_GETINSTANCE,
				tTJSNC_Offscreen::ClassID, (iTJSNativeInstance**)&TargetInstance ) ) )
			TVPThrowExceptionMessage( TVPInternalError, TJS_W("tTJSNI_Offscreen instance") );
		TargetWidth = w;
		TargetHeight = h;
	} catch( ... ) {
		if( cls ) cls->Release();
		if( newobj ) newobj->Release();
		throw;
	}
	if( cls ) cls->Release();
	if( newobj ) newobj->Release();
}
//---------------------------------------------------------------------------
void tTJSNI_GLCompositor::SetScreenSize(int width, int height)
{
	Width = width;
	Height = height;
}
//---------------------------------------------------------------------------
void tTJSNI_GLCompositor::Capture(tTJSNI_BaseLayer* layer, const tTJSVariant& callback, const tTJSVariant& param, tjs_uint32 color)
{
	if( !layer || !CanvasInstance ) return;

	MakeCurrent();

	tTVPBaseBitmap* bmp = layer->GetMainImage();
	int w = (int)bmp->GetWidth();
	int h = (int)bmp->GetHeight();
	if( w <= 0 || h <= 0 ) return;

	EnsureTarget( w, h );

	CanvasInstance->SetRenderTargetObject( TargetObject );
	CanvasInstance->SetSurfaceSize( w, h );
	CanvasInstance->SetClearColor( color );
	CanvasInstance->BeginDrawing();  // レンダーターゲット FBO をバインドする
	// BeginDrawing のクリアはバインド前 (=default FB) に対して行われるため、
	// バインド後のレンダーターゲット FBO をここで明示的にクリアする。
	// (ターゲットは EnsureTarget でサイズ一致時に再利用されるので前回内容が残る)
	CanvasInstance->Clear( color );

	if( callback.Type() == tvtObject ) {
		// GLESAdaptor 互換: callback(w, h, param) を objthis=このコンポジタ で呼ぶ。
		// コールバック内では this がこのコンポジタなので this.canvas / this.drawLayer 等が
		// 直接使える (canvas を引数で渡す必要はない)。
		tTJSVariant wv( (tjs_int)w );
		tTJSVariant hv( (tjs_int)h );
		tTJSVariant pv( param );
		tTJSVariant *args[3] = { &wv, &hv, &pv };
		callback.AsObjectClosureNoAddRef().FuncCall( 0, 0, 0, nullptr, 3, args, Owner );
	}

	// ターゲット FBO がバインドされているうちに読み戻す
	CanvasInstance->Capture( bmp, 0, 0, w, h );

	CanvasInstance->EndDrawing();

	layer->SetImageModified( true );
	layer->Update( false );
}
//---------------------------------------------------------------------------


//---------------------------------------------------------------------------
// tTJSNC_GLCompositor : TJS GLCompositor class
//---------------------------------------------------------------------------
tTJSNC_GLCompositor::tTJSNC_GLCompositor() : tTJSNativeClass(TJS_W("GLCompositor"))
{
	TJS_BEGIN_NATIVE_MEMBERS(GLCompositor)
	TJS_DECL_EMPTY_FINALIZE_METHOD
//----------------------------------------------------------------------
TJS_BEGIN_NATIVE_CONSTRUCTOR_DECL(/*var.name*/_this, /*var.type*/tTJSNI_GLCompositor, /*TJS class name*/GLCompositor)
{
	return TJS_S_OK;
}
TJS_END_NATIVE_CONSTRUCTOR_DECL(/*TJS class name*/GLCompositor)
//----------------------------------------------------------------------
TJS_BEGIN_NATIVE_METHOD_DECL(/*func. name*/setScreenSize)
{
	TJS_GET_NATIVE_INSTANCE(/*var. name*/_this, /*var. type*/tTJSNI_GLCompositor);
	if( numparams < 2 ) return TJS_E_BADPARAMCOUNT;
	_this->SetScreenSize( (tjs_int)*param[0], (tjs_int)*param[1] );
	return TJS_S_OK;
}
TJS_END_NATIVE_METHOD_DECL(/*func. name*/setScreenSize)
//----------------------------------------------------------------------
TJS_BEGIN_NATIVE_METHOD_DECL(/*func. name*/capture)
{
	TJS_GET_NATIVE_INSTANCE(/*var. name*/_this, /*var. type*/tTJSNI_GLCompositor);
	if( numparams < 1 ) return TJS_E_BADPARAMCOUNT;

	tTJSNI_BaseLayer* layer = (tTJSNI_BaseLayer*)TJSGetNativeInstance( tTJSNC_Layer::ClassID, param[0] );
	if( !layer ) return TJS_E_INVALIDPARAM;

	tTJSVariant callback = numparams > 1 ? *param[1] : tTJSVariant();
	tTJSVariant cbparam  = numparams > 2 ? *param[2] : tTJSVariant();
	tjs_uint32 color = numparams > 3 ? (tjs_uint32)(tjs_int64)*param[3] : 0;

	_this->Capture( layer, callback, cbparam, color );
	return TJS_S_OK;
}
TJS_END_NATIVE_METHOD_DECL(/*func. name*/capture)
//----------------------------------------------------------------------
TJS_BEGIN_NATIVE_METHOD_DECL(/*func. name*/drawLayer)
{
	TJS_GET_NATIVE_INSTANCE(/*var. name*/_this, /*var. type*/tTJSNI_GLCompositor);
	if( numparams < 7 ) return TJS_E_BADPARAMCOUNT;

	tTJSNI_BaseLayer* layer = (tTJSNI_BaseLayer*)TJSGetNativeInstance( tTJSNC_Layer::ClassID, param[0] );
	if( !layer ) return TJS_E_INVALIDPARAM;

	float a  = (float)(tjs_real)*param[1];
	float b  = (float)(tjs_real)*param[2];
	float c  = (float)(tjs_real)*param[3];
	float d  = (float)(tjs_real)*param[4];
	float tx = (float)(tjs_real)*param[5];
	float ty = (float)(tjs_real)*param[6];
	int opacity = numparams > 7 ? (tjs_int)*param[7] : 255;

	_this->DrawLayer( layer, a, b, c, d, tx, ty, opacity );
	return TJS_S_OK;
}
TJS_END_NATIVE_METHOD_DECL(/*func. name*/drawLayer)
//----------------------------------------------------------------------
TJS_BEGIN_NATIVE_METHOD_DECL(/*func. name*/copyLayer)
{
	TJS_GET_NATIVE_INSTANCE(/*var. name*/_this, /*var. type*/tTJSNI_GLCompositor);
	if( numparams < 3 ) return TJS_E_BADPARAMCOUNT;

	tTJSNI_BaseLayer* layer = (tTJSNI_BaseLayer*)TJSGetNativeInstance( tTJSNC_Layer::ClassID, param[0] );
	if( !layer ) return TJS_E_INVALIDPARAM;

	_this->CopyLayer( layer, (tjs_int)*param[1], (tjs_int)*param[2] );
	return TJS_S_OK;
}
TJS_END_NATIVE_METHOD_DECL(/*func. name*/copyLayer)
//----------------------------------------------------------------------
TJS_BEGIN_NATIVE_METHOD_DECL(/*func. name*/makeCurrent)
{
	TJS_GET_NATIVE_INSTANCE(/*var. name*/_this, /*var. type*/tTJSNI_GLCompositor);
	_this->MakeCurrent();
	return TJS_S_OK;
}
TJS_END_NATIVE_METHOD_DECL(/*func. name*/makeCurrent)
//----------------------------------------------------------------------
TJS_BEGIN_NATIVE_PROP_DECL(canvas)
{
	TJS_BEGIN_NATIVE_PROP_GETTER
	{
		TJS_GET_NATIVE_INSTANCE(/*var. name*/_this, /*var. type*/tTJSNI_GLCompositor);
		*result = _this->GetCanvasObject();
		return TJS_S_OK;
	}
	TJS_END_NATIVE_PROP_GETTER
	TJS_DENY_NATIVE_PROP_SETTER
}
TJS_END_NATIVE_PROP_DECL(canvas)
//----------------------------------------------------------------------
TJS_BEGIN_NATIVE_PROP_DECL(blendMode)
{
	TJS_BEGIN_NATIVE_PROP_GETTER
	{
		TJS_GET_NATIVE_INSTANCE(/*var. name*/_this, /*var. type*/tTJSNI_GLCompositor);
		*result = (tjs_int)_this->GetCanvasInstance()->GetBlendMode();
		return TJS_S_OK;
	}
	TJS_END_NATIVE_PROP_GETTER
	TJS_BEGIN_NATIVE_PROP_SETTER
	{
		TJS_GET_NATIVE_INSTANCE(/*var. name*/_this, /*var. type*/tTJSNI_GLCompositor);
		_this->GetCanvasInstance()->SetBlendMode( (tTVPBlendMode)(tjs_int)*param );
		return TJS_S_OK;
	}
	TJS_END_NATIVE_PROP_SETTER
}
TJS_END_NATIVE_PROP_DECL(blendMode)
//----------------------------------------------------------------------
TJS_BEGIN_NATIVE_PROP_DECL(screenWidth)
{
	TJS_BEGIN_NATIVE_PROP_GETTER
	{
		TJS_GET_NATIVE_INSTANCE(/*var. name*/_this, /*var. type*/tTJSNI_GLCompositor);
		*result = (tjs_int)_this->GetScreenWidth();
		return TJS_S_OK;
	}
	TJS_END_NATIVE_PROP_GETTER
	TJS_BEGIN_NATIVE_PROP_SETTER
	{
		TJS_GET_NATIVE_INSTANCE(/*var. name*/_this, /*var. type*/tTJSNI_GLCompositor);
		_this->SetScreenSize( (tjs_int)*param, _this->GetScreenHeight() );
		return TJS_S_OK;
	}
	TJS_END_NATIVE_PROP_SETTER
}
TJS_END_NATIVE_PROP_DECL(screenWidth)
//----------------------------------------------------------------------
TJS_BEGIN_NATIVE_PROP_DECL(screenHeight)
{
	TJS_BEGIN_NATIVE_PROP_GETTER
	{
		TJS_GET_NATIVE_INSTANCE(/*var. name*/_this, /*var. type*/tTJSNI_GLCompositor);
		*result = (tjs_int)_this->GetScreenHeight();
		return TJS_S_OK;
	}
	TJS_END_NATIVE_PROP_GETTER
	TJS_BEGIN_NATIVE_PROP_SETTER
	{
		TJS_GET_NATIVE_INSTANCE(/*var. name*/_this, /*var. type*/tTJSNI_GLCompositor);
		_this->SetScreenSize( _this->GetScreenWidth(), (tjs_int)*param );
		return TJS_S_OK;
	}
	TJS_END_NATIVE_PROP_SETTER
}
TJS_END_NATIVE_PROP_DECL(screenHeight)
//----------------------------------------------------------------------
TJS_BEGIN_NATIVE_PROP_DECL(GLGetProcAddress)
{
	TJS_BEGIN_NATIVE_PROP_GETTER
	{
		TJS_GET_NATIVE_INSTANCE(/*var. name*/_this, /*var. type*/tTJSNI_GLCompositor);
		// 自前コンテキストをカレントにして、GL エントリポイント解決関数のポインタを返す。
		// (GLES 系プラグインの oglbase として利用: EffekseerDevice 等)
		_this->MakeCurrent();
		*result = (tjs_int64)(void*)(&TVPGLGetProcAddress);
		return TJS_S_OK;
	}
	TJS_END_NATIVE_PROP_GETTER
	TJS_DENY_NATIVE_PROP_SETTER
}
TJS_END_NATIVE_PROP_DECL(GLGetProcAddress)
//----------------------------------------------------------------------
	TJS_END_NATIVE_MEMBERS
}
//---------------------------------------------------------------------------
tTJSNativeClass * TVPCreateNativeClass_GLCompositor()
{
	tTJSNativeClass *cls = new tTJSNC_GLCompositor();
	return cls;
}
//---------------------------------------------------------------------------
