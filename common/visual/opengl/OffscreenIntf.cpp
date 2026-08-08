
#include "tjsCommHead.h"

#include "OffscreenIntf.h"
#include "MsgIntf.h"	// TVPThrowExceptionMessage
#include "BitmapIntf.h"
#include "LayerBitmapIntf.h"
#include "tvpgl.h"
#include "DebugIntf.h"
#include "RectItf.h"
#include "TextureIntf.h"
#include <memory>

extern bool TVPCopyBitmapToTexture( const iTVPTextureInfoIntrface* texture, tjs_int left, tjs_int top, const tTVPBaseBitmap* bitmap, const tTVPRect& srcRect );
//---------------------------------------------------------------------------
tTJSNI_Offscreen::tTJSNI_Offscreen() {
}
//---------------------------------------------------------------------------
tTJSNI_Offscreen::~tTJSNI_Offscreen() {
}
//---------------------------------------------------------------------------
tjs_error TJS_INTF_METHOD tTJSNI_Offscreen::Construct(tjs_int numparams, tTJSVariant **param, iTJSDispatch2 *tjs_obj) {
	if( numparams < 2 ) return TJS_E_BADPARAMCOUNT;
	tjs_int width = *param[0];
	tjs_int height = *param[1];

	bool result = FrameBuffer.create( width, height);
	if( result == false ) {
		TVPThrowExceptionMessage(TVPFBOCreateError );
	}
	return TJS_S_OK;
}
//---------------------------------------------------------------------------
void TJS_INTF_METHOD tTJSNI_Offscreen::Invalidate() {
	FrameBuffer.destory();
}
//---------------------------------------------------------------------------
void tTJSNI_Offscreen::CopyFromBitmap( tjs_int left, tjs_int top, const tTVPBaseBitmap* bmp, const tTVPRect& srcRect ) {
	TVPCopyBitmapToTexture( this, left, top, bmp, srcRect );
}
//---------------------------------------------------------------------------
tjs_int64 tTJSNI_Offscreen::GetVBOHandle() const {
	if( VertexBuffer.isCreated() ) {
		return VertexBuffer.id();
	} else {
		const float w = (float)FrameBuffer.width();
		const float h = (float)FrameBuffer.height();
		const GLfloat vertices[] = {
			0.0f, 0.0f,	// 左上
			0.0f,    h,	// 左下
			   w, 0.0f,	// 右上
			   w,    h,	// 右下
		};
		GLVertexBufferObject& vbo = const_cast<GLVertexBufferObject&>( VertexBuffer );
		vbo.createStaticVertex( vertices, sizeof(vertices) );
		return VertexBuffer.id();
	}
}
//---------------------------------------------------------------------------
void tTJSNI_Offscreen::ExchangeTexture( tTJSNI_Texture* texture ) {
	// 制約: 寸法・フォーマットが一致していることを要求する。
	// width/height/format/glformat は両側で同値なので handle のみ swap で済む。
	if( FrameBuffer.width() == texture->GetMemoryWidth() && FrameBuffer.height() == texture->GetMemoryHeight() && texture->format() == tTVPTextureColorFormat::RGBA) {
		GLuint oldFboTex = FrameBuffer.textureId();
		GLuint newFboTex = (GLuint)texture->GetNativeHandle();
		bool result = FrameBuffer.exchangeTexture( newFboTex );
		if( !result ) {
			TVPThrowExceptionMessage(TVPCannotExchangeTexture );
		} else {
			// 所有権の交換:
			//   旧 FBO 側 attachment (oldFboTex) → Texture が以後管理
			//   旧 Texture 側 handle (newFboTex) → FBO が以後管理
			// どちらも将来の destory() で各々 glDeleteTextures される。
			// pbo は両側そのまま据え置き (upload 用途で texture-independent)。
			texture->GetTexture()->AdoptTextureId( oldFboTex );
		}
	} else {
		TVPThrowExceptionMessage(TVPIncompatibleTexture );
	}
}
//---------------------------------------------------------------------------
void tTJSNI_Offscreen::BindFrameBuffer() {
	FrameBuffer.bindFramebuffer();
}
//---------------------------------------------------------------------------
// tTJSNC_Offscreen : TJS Offscreen class
//---------------------------------------------------------------------------
tjs_uint32 tTJSNC_Offscreen::ClassID = -1;
tTJSNC_Offscreen::tTJSNC_Offscreen() : tTJSNativeClass(TJS_W("Offscreen"))
{
	// registration of native members

	TJS_BEGIN_NATIVE_MEMBERS(Offscreen) // constructor
	TJS_DECL_EMPTY_FINALIZE_METHOD
//----------------------------------------------------------------------
TJS_BEGIN_NATIVE_CONSTRUCTOR_DECL(/*var.name*/_this, /*var.type*/tTJSNI_Offscreen, /*TJS class name*/Offscreen)
{
	return TJS_S_OK;
}
TJS_END_NATIVE_CONSTRUCTOR_DECL(/*TJS class name*/Offscreen)
//----------------------------------------------------------------------

//-- methods

//----------------------------------------------------------------------
TJS_BEGIN_NATIVE_METHOD_DECL(/*func. name*/copyRect)
{
	TJS_GET_NATIVE_INSTANCE(/*var. name*/_this, /*var. type*/tTJSNI_Offscreen);
	if( numparams < 4 ) return TJS_E_BADPARAMCOUNT;

	tTJSNI_Bitmap* bitmap = (tTJSNI_Bitmap*)TJSGetNativeInstance( tTJSNC_Bitmap::ClassID, param[2] );
	if( !bitmap ) return TJS_E_INVALIDPARAM;
	tTJSNI_Rect* rect = (tTJSNI_Rect*)TJSGetNativeInstance( tTJSNC_Rect::ClassID, param[3] );
	tTVPRect srcRect;
	if( !rect ) {
		if( numparams >= 7 ) {
			srcRect.left = *param[3];
			srcRect.top = *param[4];
			srcRect.right = (tjs_int)*param[5] + srcRect.left;
			srcRect.bottom = (tjs_int)*param[6] + srcRect.top;
		} else {
			return TJS_E_INVALIDPARAM;
		}
	} else {
		srcRect = rect->Get();
	}
	tjs_int left  = *param[0];
	tjs_int top   = *param[1];
	const tTVPBaseBitmap* bmp = bitmap->GetBitmap();
	_this->CopyFromBitmap( left, top, bmp, srcRect );

	return TJS_S_OK;
}
TJS_END_NATIVE_METHOD_DECL(/*func. name*/copyRect)
//----------------------------------------------------------------------
TJS_BEGIN_NATIVE_METHOD_DECL(/*func. name*/exchangeTexture )
{
	TJS_GET_NATIVE_INSTANCE(/*var. name*/_this, /*var. type*/tTJSNI_Offscreen );
	if( numparams < 1 ) return TJS_E_BADPARAMCOUNT;

	tTJSNI_Texture* texture = (tTJSNI_Texture*)TJSGetNativeInstance( tTJSNC_Texture::ClassID, param[0] );
	if( !texture ) TVPThrowExceptionMessage(TVPParameterRequireClassInstance, TJS_W("Texture"));

	_this->ExchangeTexture( texture );

	return TJS_S_OK;
}
TJS_END_NATIVE_METHOD_DECL(/*func. name*/exchangeTexture )
//----------------------------------------------------------------------


//----------------------------------------------------------------------

//-- properties

//----------------------------------------------------------------------
TJS_BEGIN_NATIVE_PROP_DECL(width)
{
	TJS_BEGIN_NATIVE_PROP_GETTER
	{
		TJS_GET_NATIVE_INSTANCE(/*var. name*/_this, /*var. type*/tTJSNI_Offscreen);
		*result = (tjs_int64)_this->GetWidth();
		return TJS_S_OK;
	}
	TJS_END_NATIVE_PROP_GETTER

	TJS_DENY_NATIVE_PROP_SETTER
}
TJS_END_NATIVE_PROP_DECL(width)
//----------------------------------------------------------------------
TJS_BEGIN_NATIVE_PROP_DECL(height)
{
	TJS_BEGIN_NATIVE_PROP_GETTER
	{
		TJS_GET_NATIVE_INSTANCE(/*var. name*/_this, /*var. type*/tTJSNI_Offscreen);
		*result = (tjs_int64)_this->GetHeight();
		return TJS_S_OK;
	}
	TJS_END_NATIVE_PROP_GETTER

	TJS_DENY_NATIVE_PROP_SETTER
}
TJS_END_NATIVE_PROP_DECL(height)
//----------------------------------------------------------------------
TJS_BEGIN_NATIVE_PROP_DECL(nativeHandle)
{
	TJS_BEGIN_NATIVE_PROP_GETTER
	{
		TJS_GET_NATIVE_INSTANCE(/*var. name*/_this, /*var. type*/tTJSNI_Offscreen);
		*result = _this->GetNativeHandle();
		return TJS_S_OK;
	}
	TJS_END_NATIVE_PROP_GETTER

	TJS_DENY_NATIVE_PROP_SETTER
}
TJS_END_NATIVE_PROP_DECL(nativeHandle)
//----------------------------------------------------------------------

//----------------------------------------------------------------------

	TJS_END_NATIVE_MEMBERS
}

//---------------------------------------------------------------------------
tTJSNativeClass * TVPCreateNativeClass_Offscreen()
{
	tTJSNativeClass *cls = new tTJSNC_Offscreen();
	return cls;
}
//---------------------------------------------------------------------------
