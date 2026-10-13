//---------------------------------------------------------------------------
// GLCompositor の GPU 版 (GpuCompositorIntf.h 参照)
//   処理の流れは GL 版 (common/visual/opengl/GLCompositorIntf.cpp) と同じ。
//---------------------------------------------------------------------------
#include "tjsCommHead.h"

#include "GpuCompositorIntf.h"
#include "GpuCanvasIntf.h"
#include "Matrix32Intf.h"
#include "BitmapIntf.h"
#include "LayerIntf.h"
#include "LayerBitmapIntf.h"
#include "WindowIntf.h"
#include "MsgIntf.h"

using namespace TVPGpu;

tjs_uint32 tTJSNC_GpuCompositor::ClassID = (tjs_uint32)-1;

//---------------------------------------------------------------------------
tjs_error TJS_INTF_METHOD tTJSNI_GpuCompositor::Construct( tjs_int numparams, tTJSVariant** param, iTJSDispatch2* tjs_obj )
{
	if( numparams < 1 ) return TJS_E_BADPARAMCOUNT;
	Owner = tjs_obj;

	// GL 版と同じく第 1 引数は Window (GPU 版では使わない)
	iTJSDispatch2* winobj = param[0]->AsObjectNoAddRef();
	tTJSNI_Window* NIWindow = nullptr;
	if( winobj == nullptr ||
		TJS_FAILED( winobj->NativeInstanceSupport( TJS_NIS_GETINSTANCE,
			tTJSNC_Window::ClassID, (iTJSNativeInstance**)&NIWindow ) ) ) {
		TVPThrowExceptionMessage( TVPGLCompositor1stArgumentMustBeAWindow );
	}
	CreateCanvas();
	return TJS_S_OK;
}

void TJS_INTF_METHOD tTJSNI_GpuCompositor::Invalidate()
{
	auto inval = []( tTJSVariant& v ) {
		if( v.Type() == tvtObject ) v.AsObjectClosureNoAddRef().Invalidate( 0, NULL, NULL, v.AsObjectNoAddRef() );
		v.Clear();
	};
	inval( BitmapObject ); BitmapInstance = nullptr;
	CanvasMatrixInstance = nullptr;
	inval( CanvasObject ); CanvasInstance = nullptr;
	Target.reset();
}

//---------------------------------------------------------------------------
static tTJSVariant CreateObject( iTJSDispatch2* cls, tjs_int numparams, tTJSVariant** param )
{
	iTJSDispatch2* obj = nullptr;
	tTJSVariant result;
	try {
		if( TJS_FAILED( cls->CreateNew( 0, nullptr, nullptr, &obj, numparams, param, cls ) ) )
			TVPThrowExceptionMessage( TVPInternalError, TJS_W("tTJSNI_GpuCompositor") );
		result = tTJSVariant( obj, obj );
	} catch( ... ) {
		cls->Release();
		if( obj ) obj->Release();
		throw;
	}
	cls->Release();
	if( obj ) obj->Release();
	return result;
}

void tTJSNI_GpuCompositor::CreateCanvas()
{
	CanvasObject = CreateObject( new tTJSNC_GpuCanvas(), 0, nullptr );
	CanvasInstance = (tTJSNI_GpuCanvas*)TJSGetNativeInstance( tTJSNC_GpuCanvas::ClassID, &CanvasObject );
	if( !CanvasInstance ) TVPThrowExceptionMessage( TVPInternalError, TJS_W("tTJSNI_GpuCanvas instance") );
	// drawLayer 用に Canvas の matrix を取っておく
	const tTJSVariant& mo = CanvasInstance->GetMatrix32Object();
	CanvasMatrixInstance = (tTJSNI_Matrix32*)TJSGetNativeInstance( tTJSNC_Matrix32::ClassID, &mo );
}

void tTJSNI_GpuCompositor::EnsureBitmap()
{
	if( BitmapInstance ) return;
	BitmapObject = CreateObject( new tTJSNC_Bitmap(), 0, nullptr );
	BitmapInstance = (tTJSNI_Bitmap*)TJSGetNativeInstance( tTJSNC_Bitmap::ClassID, &BitmapObject );
	if( !BitmapInstance ) TVPThrowExceptionMessage( TVPInternalError, TJS_W("tTJSNI_Bitmap instance") );
}

void tTJSNI_GpuCompositor::EnsureTarget( int w, int h )
{
	if( Target && (int)Target->Width() == w && (int)Target->Height() == h ) return;
	TextureDesc d;
	d.width = w; d.height = h;
	d.format = TextureFormat::BGRA8;
	d.renderTarget = true;
	Target = Device::Get()->CreateTexture( d );
}

//---------------------------------------------------------------------------
void tTJSNI_GpuCompositor::DrawLayer( tTJSNI_BaseLayer* layer, float a, float b, float c, float d, float tx, float ty, int opacity )
{
	if( !layer || !CanvasInstance ) return;

	// レイヤーのメインイメージを Bitmap へ (内部ビットマップの共有で安価) → Texture
	EnsureBitmap();
	layer->CopyFromMainImage( BitmapInstance );
	tTJSVariant bmp( BitmapObject );
	tTJSVariant* pp[1] = { &bmp };
	tTJSVariant texobj = CreateObject( new tTJSNC_GpuTexture(), 1, pp );
	tTJSNI_GpuTexture* tex = (tTJSNI_GpuTexture*)TJSGetNativeInstance( tTJSNC_GpuTexture::ClassID, &texobj );
	if( tex ) {
		if( CanvasMatrixInstance ) CanvasMatrixInstance->Set( a, b, c, d, tx, ty );
		// 不透明度は既定シェーダーの a_opacity で
		const tTJSVariant& shobj = CanvasInstance->GetDefaultShader();
		const bool shaderReady = shobj.Type() == tvtObject;
		static ttstr a_opacity( TJS_W("a_opacity") );
		if( shaderReady ) {
			tTJSVariant op( (tjs_real)opacity / 255.0 );
			iTJSDispatch2* sh = shobj.AsObjectNoAddRef();
			sh->PropSet( TJS_MEMBERENSURE, a_opacity.c_str(), a_opacity.GetHint(), &op, sh );
		}
		const iTVPGpuDrawable* t[1] = { tex };
		CanvasInstance->DrawTexture( t, 1, nullptr );
		if( shaderReady ) {
			tTJSVariant one( (tjs_real)1.0 );
			iTJSDispatch2* sh = shobj.AsObjectNoAddRef();
			sh->PropSet( TJS_MEMBERENSURE, a_opacity.c_str(), a_opacity.GetHint(), &one, sh );
		}
	}
	// テクスチャはすぐ手放す (描画に使われた分はデバイスが送信まで保つ)
	if( texobj.Type() == tvtObject )
		texobj.AsObjectClosureNoAddRef().Invalidate( 0, nullptr, nullptr, texobj.AsObjectNoAddRef() );
}

void tTJSNI_GpuCompositor::CopyLayer( tTJSNI_BaseLayer* layer, int left, int top )
{
	DrawLayer( layer, 1.0f, 0.0f, 0.0f, 1.0f, (float)left, (float)top, 255 );
}

//---------------------------------------------------------------------------
void tTJSNI_GpuCompositor::Capture( tTJSNI_BaseLayer* layer, const tTJSVariant& callback, const tTJSVariant& param, tjs_uint32 color )
{
	if( !layer || !CanvasInstance ) return;
	tTVPBaseBitmap* bmp = layer->GetMainImage();
	const int w = (int)bmp->GetWidth();
	const int h = (int)bmp->GetHeight();
	if( w <= 0 || h <= 0 ) return;

	EnsureTarget( w, h );
	// 描画先は Canvas の「画面」として渡す (コールバックで renderTarget を戻してもここに戻る)
	CanvasInstance->SetScreenTarget( Target.get(), w, h );
	CanvasInstance->SetClearColor( color );
	CanvasInstance->BeginDrawing();   // ClearColor で塗られる

	if( callback.Type() == tvtObject ) {
		// GL 版と同じく callback(w, h, param) を objthis = このコンポジタで呼ぶ
		tTJSVariant wv( (tjs_int)w ), hv( (tjs_int)h ), pv( param );
		tTJSVariant* args[3] = { &wv, &hv, &pv };
		try {
			callback.AsObjectClosureNoAddRef().FuncCall( 0, 0, 0, nullptr, 3, args, Owner );
		} catch( ... ) {
			CanvasInstance->EndDrawing();
			throw;
		}
	}

	// 描画先を戻してから読み戻す (コールバックが renderTarget を変えていても画面側を読む)
	CanvasInstance->SetRenderTargetObject( tTJSVariant() );
	CanvasInstance->Capture( bmp, 0, 0, w, h );
	CanvasInstance->EndDrawing();

	// premultiplied-alpha を straight-alpha へ戻す (GL 版と同じ)
	if( Unpremultiply ) {
		for( int yy = 0; yy < h; yy++ ) {
			tjs_uint32* line = (tjs_uint32*)bmp->GetScanLineForWrite( yy );
			for( int xx = 0; xx < w; xx++ ) {
				tjs_uint32 p = line[xx];
				tjs_uint32 a = ( p >> 24 ) & 0xff;
				if( a == 0 ) {
					line[xx] = 0;
				} else if( a < 255 ) {
					tjs_uint32 r = ( ( p >> 16 ) & 0xff ) * 255u + a / 2;
					tjs_uint32 g = ( ( p >>  8 ) & 0xff ) * 255u + a / 2;
					tjs_uint32 b = ( ( p       ) & 0xff ) * 255u + a / 2;
					r /= a; g /= a; b /= a;
					if( r > 255u ) r = 255u;
					if( g > 255u ) g = 255u;
					if( b > 255u ) b = 255u;
					line[xx] = ( a << 24 ) | ( r << 16 ) | ( g << 8 ) | b;
				}
			}
		}
	}
	layer->SetImageModified( true );
	layer->Update( false );
}

//---------------------------------------------------------------------------
// tTJSNC_GpuCompositor (TJS のクラス名は GLCompositor)
//---------------------------------------------------------------------------
static tTJSNI_BaseLayer* LayerOf( tTJSVariant* v )
{
	return (tTJSNI_BaseLayer*)TJSGetNativeInstance( tTJSNC_Layer::ClassID, v );
}

tTJSNC_GpuCompositor::tTJSNC_GpuCompositor() : tTJSNativeClass( TJS_W("GLCompositor") )
{
	TJS_BEGIN_NATIVE_MEMBERS( GLCompositor )
	TJS_DECL_EMPTY_FINALIZE_METHOD
	TJS_BEGIN_NATIVE_CONSTRUCTOR_DECL( _this, tTJSNI_GpuCompositor, GLCompositor ) { return TJS_S_OK; }
	TJS_END_NATIVE_CONSTRUCTOR_DECL( GLCompositor )

	TJS_BEGIN_NATIVE_METHOD_DECL( setScreenSize )
	{
		TJS_GET_NATIVE_INSTANCE( _this, tTJSNI_GpuCompositor );
		if( numparams < 2 ) return TJS_E_BADPARAMCOUNT;
		_this->SetScreenSize( (tjs_int)*param[0], (tjs_int)*param[1] );
		return TJS_S_OK;
	}
	TJS_END_NATIVE_METHOD_DECL( setScreenSize )

	TJS_BEGIN_NATIVE_METHOD_DECL( capture )
	{
		TJS_GET_NATIVE_INSTANCE( _this, tTJSNI_GpuCompositor );
		if( numparams < 1 ) return TJS_E_BADPARAMCOUNT;
		tTJSNI_BaseLayer* layer = LayerOf( param[0] );
		if( !layer ) return TJS_E_INVALIDPARAM;
		tTJSVariant callback = numparams > 1 ? *param[1] : tTJSVariant();
		tTJSVariant cbparam = numparams > 2 ? *param[2] : tTJSVariant();
		tjs_uint32 color = numparams > 3 ? (tjs_uint32)(tjs_int64)*param[3] : 0;
		_this->Capture( layer, callback, cbparam, color );
		return TJS_S_OK;
	}
	TJS_END_NATIVE_METHOD_DECL( capture )

	TJS_BEGIN_NATIVE_METHOD_DECL( drawLayer )
	{
		TJS_GET_NATIVE_INSTANCE( _this, tTJSNI_GpuCompositor );
		if( numparams < 7 ) return TJS_E_BADPARAMCOUNT;
		tTJSNI_BaseLayer* layer = LayerOf( param[0] );
		if( !layer ) return TJS_E_INVALIDPARAM;
		_this->DrawLayer( layer, (float)(tjs_real)*param[1], (float)(tjs_real)*param[2], (float)(tjs_real)*param[3],
			(float)(tjs_real)*param[4], (float)(tjs_real)*param[5], (float)(tjs_real)*param[6],
			numparams > 7 ? (tjs_int)*param[7] : 255 );
		return TJS_S_OK;
	}
	TJS_END_NATIVE_METHOD_DECL( drawLayer )

	TJS_BEGIN_NATIVE_METHOD_DECL( copyLayer )
	{
		TJS_GET_NATIVE_INSTANCE( _this, tTJSNI_GpuCompositor );
		if( numparams < 3 ) return TJS_E_BADPARAMCOUNT;
		tTJSNI_BaseLayer* layer = LayerOf( param[0] );
		if( !layer ) return TJS_E_INVALIDPARAM;
		_this->CopyLayer( layer, (tjs_int)*param[1], (tjs_int)*param[2] );
		return TJS_S_OK;
	}
	TJS_END_NATIVE_METHOD_DECL( copyLayer )

	// GL のコンテキストは無いので何もしない
	TJS_BEGIN_NATIVE_METHOD_DECL( makeCurrent ) { return TJS_S_OK; }
	TJS_END_NATIVE_METHOD_DECL( makeCurrent )

	TJS_BEGIN_NATIVE_PROP_DECL( canvas )
	{
		TJS_BEGIN_NATIVE_PROP_GETTER
		{
			TJS_GET_NATIVE_INSTANCE( _this, tTJSNI_GpuCompositor );
			*result = _this->GetCanvasObject();
			return TJS_S_OK;
		}
		TJS_END_NATIVE_PROP_GETTER
		TJS_DENY_NATIVE_PROP_SETTER
	}
	TJS_END_NATIVE_PROP_DECL( canvas )

	TJS_BEGIN_NATIVE_PROP_DECL( blendMode )
	{
		TJS_BEGIN_NATIVE_PROP_GETTER
		{
			TJS_GET_NATIVE_INSTANCE( _this, tTJSNI_GpuCompositor );
			*result = (tjs_int)_this->GetCanvasInstance()->GetBlendMode();
			return TJS_S_OK;
		}
		TJS_END_NATIVE_PROP_GETTER
		TJS_BEGIN_NATIVE_PROP_SETTER
		{
			TJS_GET_NATIVE_INSTANCE( _this, tTJSNI_GpuCompositor );
			_this->GetCanvasInstance()->SetBlendMode( (tTVPGpuBlendMode)(tjs_int)*param );
			return TJS_S_OK;
		}
		TJS_END_NATIVE_PROP_SETTER
	}
	TJS_END_NATIVE_PROP_DECL( blendMode )

	TJS_BEGIN_NATIVE_PROP_DECL( unpremultiply )
	{
		TJS_BEGIN_NATIVE_PROP_GETTER
		{
			TJS_GET_NATIVE_INSTANCE( _this, tTJSNI_GpuCompositor );
			*result = (tjs_int)( _this->GetUnpremultiply() ? 1 : 0 );
			return TJS_S_OK;
		}
		TJS_END_NATIVE_PROP_GETTER
		TJS_BEGIN_NATIVE_PROP_SETTER
		{
			TJS_GET_NATIVE_INSTANCE( _this, tTJSNI_GpuCompositor );
			_this->SetUnpremultiply( (tjs_int)*param != 0 );
			return TJS_S_OK;
		}
		TJS_END_NATIVE_PROP_SETTER
	}
	TJS_END_NATIVE_PROP_DECL( unpremultiply )

	TJS_BEGIN_NATIVE_PROP_DECL( screenWidth )
	{
		TJS_BEGIN_NATIVE_PROP_GETTER
		{
			TJS_GET_NATIVE_INSTANCE( _this, tTJSNI_GpuCompositor );
			*result = (tjs_int)_this->GetScreenWidth();
			return TJS_S_OK;
		}
		TJS_END_NATIVE_PROP_GETTER
		TJS_BEGIN_NATIVE_PROP_SETTER
		{
			TJS_GET_NATIVE_INSTANCE( _this, tTJSNI_GpuCompositor );
			_this->SetScreenSize( (tjs_int)*param, _this->GetScreenHeight() );
			return TJS_S_OK;
		}
		TJS_END_NATIVE_PROP_SETTER
	}
	TJS_END_NATIVE_PROP_DECL( screenWidth )

	TJS_BEGIN_NATIVE_PROP_DECL( screenHeight )
	{
		TJS_BEGIN_NATIVE_PROP_GETTER
		{
			TJS_GET_NATIVE_INSTANCE( _this, tTJSNI_GpuCompositor );
			*result = (tjs_int)_this->GetScreenHeight();
			return TJS_S_OK;
		}
		TJS_END_NATIVE_PROP_GETTER
		TJS_BEGIN_NATIVE_PROP_SETTER
		{
			TJS_GET_NATIVE_INSTANCE( _this, tTJSNI_GpuCompositor );
			_this->SetScreenSize( _this->GetScreenWidth(), (tjs_int)*param );
			return TJS_S_OK;
		}
		TJS_END_NATIVE_PROP_SETTER
	}
	TJS_END_NATIVE_PROP_DECL( screenHeight )

	// GL の関数解決口は無い (GLES 系プラグインは GPU 版では使えない)
	TJS_BEGIN_NATIVE_PROP_DECL( GLGetProcAddress )
	{
		TJS_BEGIN_NATIVE_PROP_GETTER
		{
			*result = (tjs_int64)0;
			return TJS_S_OK;
		}
		TJS_END_NATIVE_PROP_GETTER
		TJS_DENY_NATIVE_PROP_SETTER
	}
	TJS_END_NATIVE_PROP_DECL( GLGetProcAddress )

	TJS_END_NATIVE_MEMBERS
}

tTJSNativeClass* TVPCreateNativeClass_GpuCompositor()
{
	return new tTJSNC_GpuCompositor();
}
