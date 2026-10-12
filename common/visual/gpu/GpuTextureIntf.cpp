//---------------------------------------------------------------------------
// Texture / Offscreen (GPU 版)
//---------------------------------------------------------------------------
#include "tjsCommHead.h"

#include "tjsArray.h"
#include "GpuCanvasIntf.h"
#include "BitmapIntf.h"
#include "GraphicsLoaderIntf.h"
#include "LayerBitmapIntf.h"
#include "LayerIntf.h"
#include "MsgIntf.h"
#include "TVPColor.h"
#include "RectItf.h"
#include "DebugIntf.h"
#include "LogIntf.h"
#include <memory>
#include <algorithm>

using namespace TVPGpu;

//---------------------------------------------------------------------------
iTVPGpuDrawable* TVPGetGpuDrawable( const tTJSVariant* param )
{
	if( !param || param->Type() != tvtObject ) return nullptr;
	tTJSVariantClosure clo = param->AsObjectClosureNoAddRef();
	if( !clo.Object ) return nullptr;
	tTJSNI_GpuTexture* tex = nullptr;
	if( TJS_SUCCEEDED( clo.Object->NativeInstanceSupport( TJS_NIS_GETINSTANCE, tTJSNC_GpuTexture::ClassID, (iTJSNativeInstance**)&tex ) ) && tex )
		return tex;
	tTJSNI_GpuOffscreen* off = nullptr;
	if( TJS_SUCCEEDED( clo.Object->NativeInstanceSupport( TJS_NIS_GETINSTANCE, tTJSNC_GpuOffscreen::ClassID, (iTJSNativeInstance**)&off ) ) && off )
		return off;
	return nullptr;
}

static inline bool IsPow2( tjs_uint x ) { return x && ( x & ( x - 1 ) ) == 0; }
static inline tjs_uint ToPow2( tjs_uint x ) { tjs_uint r = 1; while( r < x ) r <<= 1; return r; }

//---------------------------------------------------------------------------
// tTJSNI_GpuTexture
//---------------------------------------------------------------------------
tTJSNI_GpuTexture::tTJSNI_GpuTexture() { TVPTempBitmapHolderAddRef(); }
tTJSNI_GpuTexture::~tTJSNI_GpuTexture() { TVPTempBitmapHolderRelease(); }

void tTJSNI_GpuTexture::Create( tjs_uint w, tjs_uint h, tTVPGpuTextureColor color, tjs_uint levels )
{
	if( w == 0 || h == 0 ) TVPThrowExceptionMessage( TJS_W("Texture: invalid size") );
	TextureDesc d;
	d.width = w; d.height = h;
	d.format = color == tTVPGpuTextureColor::Alpha ? TextureFormat::A8 : TextureFormat::BGRA8;
	d.mipLevels = levels;
	Tex = Device::Get()->CreateTexture( d );
	Color = color;
}

//---------------------------------------------------------------------------
// ビットマップから作る。1 行目が上 (GL 版と同じく上下反転しない)
//---------------------------------------------------------------------------
void tTJSNI_GpuTexture::LoadBitmap( const tTVPBaseBitmap* bmp, tTVPGpuTextureColor color, bool fromAlphaChannel )
{
	const tjs_uint w = bmp->GetWidth(), h = bmp->GetHeight();
	if( !w || !h ) return;
	if( color == tTVPGpuTextureColor::RGBA ) {
		Create( w, h, color );
		if( bmp->Is32BPP() ) {
			// 行ごとに連続なら 1 回で上げる
			const tjs_int pitch = bmp->GetPitchBytes();
			if( pitch == (tjs_int)w * 4 ) {
				Device::Get()->UploadTexture( Tex.get(), 0, 0, 0, w, h, bmp->GetScanLine( 0 ), w * 4 );
			} else {
				std::vector<tjs_uint32> buf( (size_t)w * h );
				for( tjs_uint y = 0; y < h; y++ ) memcpy( &buf[(size_t)w * y], bmp->GetScanLine( y ), w * 4 );
				Device::Get()->UploadTexture( Tex.get(), 0, 0, 0, w, h, buf.data(), w * 4 );
			}
		} else {
			// 8bpp → グレイの色
			std::vector<tjs_uint32> buf( (size_t)w * h );
			for( tjs_uint y = 0; y < h; y++ ) {
				const tjs_uint8* s = (const tjs_uint8*)bmp->GetScanLine( y );
				for( tjs_uint x = 0; x < w; x++ ) buf[(size_t)w * y + x] = 0xff000000 | ( s[x] << 16 ) | ( s[x] << 8 ) | s[x];
			}
			Device::Get()->UploadTexture( Tex.get(), 0, 0, 0, w, h, buf.data(), w * 4 );
		}
		return;
	}
	// Alpha: tcfAlphaChannel なら α、tcfAlpha なら輝度 (8bpp ならその値)
	Create( w, h, color );
	std::vector<tjs_uint8> buf( (size_t)w * h );
	for( tjs_uint y = 0; y < h; y++ ) {
		tjs_uint8* d = &buf[(size_t)w * y];
		if( bmp->Is8BPP() ) {
			memcpy( d, bmp->GetScanLine( y ), w );
		} else {
			const tjs_uint32* s = (const tjs_uint32*)bmp->GetScanLine( y );
			for( tjs_uint x = 0; x < w; x++ ) {
				if( fromAlphaChannel ) d[x] = (tjs_uint8)( s[x] >> 24 );
				else {
					const tjs_uint32 c = s[x];
					// GL 版の DoGrayScale と同じ係数 (R*19 + G*183 + B*54) / 256
					d[x] = (tjs_uint8)( ( ( ( c >> 16 ) & 0xff ) * 19 + ( ( c >> 8 ) & 0xff ) * 183 + ( c & 0xff ) * 54 ) >> 8 );
				}
			}
		}
	}
	Device::Get()->UploadTexture( Tex.get(), 0, 0, 0, w, h, buf.data(), w );
}

//---------------------------------------------------------------------------
// ミップマップ: sizeList は [w1,h1, w2,h2, ...] の順に小さくなる大きさ
//---------------------------------------------------------------------------
void tTJSNI_GpuTexture::LoadMipmap( const tTVPBaseBitmap* bmp, tTJSArrayNI* sizeList, tjs_int type, tjs_real typeopt )
{
	const tjs_uint count = (tjs_uint)( sizeList->Items.size() / 2 );
	if( count < 1 ) TVPThrowExceptionMessage( TJS_W("Texture: invalid mipmap size list") );
	const tjs_uint w = bmp->GetWidth(), h = bmp->GetHeight();
	Create( w, h, tTVPGpuTextureColor::RGBA, count + 1 );
	SrcWidth = w; SrcHeight = h;
	std::vector<tjs_uint32> buf( (size_t)w * h );
	for( tjs_uint y = 0; y < h; y++ ) memcpy( &buf[(size_t)w * y], bmp->GetScanLine( y ), w * 4 );
	Device::Get()->UploadTexture( Tex.get(), 0, 0, 0, w, h, buf.data(), w * 4 );
	tjs_int prew = w, preh = h;
	tTVPRect srcRect( 0, 0, w, h );
	for( tjs_uint i = 0; i < count; i++ ) {
		tjs_int sw = sizeList->Items[i * 2 + 0];
		tjs_int sh = sizeList->Items[i * 2 + 1];
		if( sw <= 0 || sh <= 0 || sw > prew || sh > preh ) TVPThrowExceptionMessage( TJS_W("Texture: invalid mipmap size list") );
		tTVPBaseBitmap dst( sw, sh, 32, true );
		tTVPRect dstRect( 0, 0, sw, sh );
		tTVPRect clip( dstRect );
		dst.StretchBlt( clip, dstRect, bmp, srcRect, bmCopy, 255, false, (tTVPBBStretchType)type, typeopt );
		std::vector<tjs_uint32> lb( (size_t)sw * sh );
		for( tjs_int y = 0; y < sh; y++ ) memcpy( &lb[(size_t)sw * y], dst.GetScanLine( y ), sw * 4 );
		Device::Get()->UploadTexture( Tex.get(), i + 1, 0, 0, sw, sh, lb.data(), sw * 4 );
		prew = sw; preh = sh;
	}
}

//---------------------------------------------------------------------------
void tTJSNI_GpuTexture::Read9Patch( std::unique_ptr<tTVPBaseBitmap>& holder, const tTVPBaseBitmap*& bmp )
{
	if( !bmp->Is32BPP() ) return;
	bmp->Read9PatchInfo( Scale9Patch, Margin9Patch );
	Scale9Patch.add_offsets( -1, -1 );
	Margin9Patch.add_offsets( -1, -1 );
	if( Margin9Patch.top >= 0 ) {
		if( Margin9Patch.bottom < 0 ) Margin9Patch.bottom = 0;
		if( Margin9Patch.right < 0 ) Margin9Patch.right = 0;
	}
	// 周囲 1 ピクセルを削る
	std::unique_ptr<tTVPBaseBitmap> b2( new tTVPBaseBitmap( bmp->GetWidth() - 2, bmp->GetHeight() - 2, 32, true ) );
	if( b2->CopyRect( 0, 0, bmp, tTVPRect( 1, 1, bmp->GetWidth() - 1, bmp->GetHeight() - 1 ) ) ) {
		holder.reset( b2.release() );
		bmp = holder.get();
	}
	if( Margin9Patch.left >= 0 && Margin9Patch.top >= 0 && Margin9Patch.right >= 0 && Margin9Patch.bottom >= 0 ) {
		iTJSDispatch2* ret = TVPCreateRectObject( Margin9Patch.left, Margin9Patch.top, Margin9Patch.right, Margin9Patch.bottom );
		MarginRectObject = tTJSVariant( ret, ret );
		if( ret ) ret->Release();
	}
}

//---------------------------------------------------------------------------
tjs_error TJS_INTF_METHOD tTJSNI_GpuTexture::Construct( tjs_int numparams, tTJSVariant** param, iTJSDispatch2* tjs_obj )
{
	if( numparams < 1 ) return TJS_E_BADPARAMCOUNT;

	// 色形式・9patch・2 の累乗化の引数 (ファイル名 / Bitmap の場合)
	auto readOptions = [&]( int base, tTVPGpuTextureColor& color, bool& fromAlpha, bool& is9patch, bool& pow2 ) {
		color = tTVPGpuTextureColor::RGBA; fromAlpha = false; is9patch = false; pow2 = false;
		if( numparams > base ) {
			const tjs_int fmt = (tjs_int)*param[base];
			if( fmt == TVP_GPU_TCF_ALPHA_CHANNEL ) { fromAlpha = true; color = tTVPGpuTextureColor::Alpha; }
			else color = (tTVPGpuTextureColor)fmt;
		}
		if( numparams > base + 1 ) is9patch = (tjs_int)*param[base + 1] != 0;
		if( numparams > base + 2 ) pow2 = (tjs_int)*param[base + 2] != 0;
	};
	auto loadFrom = [&]( const tTVPBaseBitmap* bmp, tTVPGpuTextureColor color, bool fromAlpha, bool is9patch, bool pow2 ) {
		std::unique_ptr<tTVPBaseBitmap> holder;
		if( is9patch ) Read9Patch( holder, bmp );
		SrcWidth = bmp->GetWidth();
		SrcHeight = bmp->GetHeight();
		if( pow2 && !fromAlpha && ( !IsPow2( SrcWidth ) || !IsPow2( SrcHeight ) ) ) {
			std::unique_ptr<tTVPBaseBitmap> b2( new tTVPBaseBitmap( *bmp ) );
			b2->SetSizeWithFill( ToPow2( SrcWidth ), ToPow2( SrcHeight ), 0 );
			holder.reset( b2.release() );
			bmp = holder.get();
		}
		LoadBitmap( bmp, color, fromAlpha );
	};

	if( param[0]->Type() == tvtString ) {
		tTVPGpuTextureColor color; bool fromAlpha, is9patch, pow2;
		readOptions( 1, color, fromAlpha, is9patch, pow2 );
		std::unique_ptr<tTVPBaseBitmap> bitmap( new tTVPBaseBitmap( TVPGetInitialBitmap() ) );
		tTVPGraphicLoadMode mode = ( color == tTVPGpuTextureColor::Alpha && !fromAlpha ) ? glmGrayscale : glmNormal;
		TVPLoadGraphic( bitmap.get(), *param[0], clNone, 0, 0, mode, nullptr, nullptr, true );
		loadFrom( bitmap.get(), color, fromAlpha, is9patch, pow2 );
		return TJS_S_OK;
	}
	if( param[0]->Type() == tvtObject ) {
		// Texture ならコピー
		tTJSNI_GpuTexture* src = (tTJSNI_GpuTexture*)TJSGetNativeInstance( tTJSNC_GpuTexture::ClassID, param[0] );
		if( src ) { CopyTexture( src ); return TJS_S_OK; }

		tTJSVariantClosure clo = param[0]->AsObjectClosureNoAddRef();
		tTJSNI_Bitmap* bmp = nullptr;
		if( clo.Object && TJS_SUCCEEDED( clo.Object->NativeInstanceSupport( TJS_NIS_GETINSTANCE, tTJSNC_Bitmap::ClassID, (iTJSNativeInstance**)&bmp ) ) && bmp ) {
			tTVPGpuTextureColor color; bool fromAlpha, is9patch, pow2;
			readOptions( 1, color, fromAlpha, is9patch, pow2 );
			loadFrom( bmp->GetBitmap(), color, fromAlpha, is9patch, pow2 );
			return TJS_S_OK;
		}
		// 配列 + ファイル名ならミップマップ
		tTJSArrayNI* sizeList = nullptr;
		if( clo.Object && numparams >= 2 && param[1]->Type() == tvtString &&
			TJS_SUCCEEDED( clo.Object->NativeInstanceSupport( TJS_NIS_GETINSTANCE, TJSGetArrayClassID(), (iTJSNativeInstance**)&sizeList ) ) && sizeList ) {
			std::unique_ptr<tTVPBaseBitmap> bitmap( new tTVPBaseBitmap( TVPGetInitialBitmap() ) );
			TVPLoadGraphic( bitmap.get(), *param[1], clNone, 0, 0, glmNormal, nullptr, nullptr, true );
			tTVPBBStretchType type = stFastAreaAvg;
			if( numparams >= 3 && param[2]->Type() != tvtVoid ) type = (tTVPBBStretchType)(tjs_int)*param[2];
			tjs_real typeopt = 0.0;
			if( numparams >= 4 ) typeopt = (tjs_real)*param[3];
			else if( type == stFastCubic || type == stCubic ) typeopt = -1.0;
			LoadMipmap( bitmap.get(), sizeList, type, typeopt );
			return TJS_S_OK;
		}
		TVPThrowExceptionMessage( TVPParameterRequireClassInstance, TJS_W("Bitmap") );
	}
	if( numparams >= 2 && param[0]->Type() == tvtInteger && param[1]->Type() == tvtInteger ) {
		tjs_int w = *param[0], h = *param[1];
		tTVPGpuTextureColor color = tTVPGpuTextureColor::RGBA;
		if( numparams > 2 ) {
			const tjs_int fmt = (tjs_int)*param[2];
			color = ( fmt == TVP_GPU_TCF_ALPHA_CHANNEL ) ? tTVPGpuTextureColor::Alpha : (tTVPGpuTextureColor)fmt;
		}
		Create( w, h, color );
		SrcWidth = w; SrcHeight = h;
		return TJS_S_OK;
	}
	return TJS_E_INVALIDPARAM;
}

void TJS_INTF_METHOD tTJSNI_GpuTexture::Invalidate()
{
	Tex.reset();
	if( MarginRectObject.Type() == tvtObject )
		MarginRectObject.AsObjectClosureNoAddRef().Invalidate( 0, NULL, NULL, MarginRectObject.AsObjectNoAddRef() );
	MarginRectObject.Clear();
}

bool tTJSNI_GpuTexture::IsPowerOfTwo() const
{
	return Tex && IsPow2( Tex->Width() ) && IsPow2( Tex->Height() );
}

SamplerDesc tTJSNI_GpuTexture::GetSamplerDesc() const
{
	SamplerDesc s;
	const Filter f = StretchType == TVP_GL_NEAREST ? Filter::Nearest : Filter::Linear;
	s.minFilter = s.magFilter = f;
	auto wrap = []( tjs_int v ) {
		if( v == TVP_GL_REPEAT ) return WrapMode::Repeat;
		if( v == TVP_GL_MIRRORED_REPEAT ) return WrapMode::MirroredRepeat;
		return WrapMode::ClampToEdge;
	};
	s.wrapU = wrap( WrapS );
	s.wrapV = wrap( WrapT );
	if( Tex && Tex->GetDesc().mipLevels > 1 ) { s.mipmaps = true; s.mipFilter = Filter::Linear; }
	return s;
}

//---------------------------------------------------------------------------
// ビットマップの範囲を転送する。範囲はビットマップとテクスチャの両方でクリップする
//---------------------------------------------------------------------------
bool tTJSNI_GpuTexture::CopyBitmap( tjs_int left, tjs_int top, const tTVPBaseBitmap* bmp, const tTVPRect& srcRect )
{
	if( !Tex || !bmp ) return false;
	tTVPRect clip( srcRect );
	if( clip.right > (tjs_int)bmp->GetWidth() ) clip.right = bmp->GetWidth();
	if( clip.bottom > (tjs_int)bmp->GetHeight() ) clip.bottom = bmp->GetHeight();
	if( clip.left < 0 ) { left -= clip.left; clip.left = 0; }
	if( clip.top < 0 ) { top -= clip.top; clip.top = 0; }
	if( left < 0 ) { clip.left -= left; left = 0; }
	if( top < 0 ) { clip.top -= top; top = 0; }
	if( (tjs_int)Tex->Width() < left + clip.get_width() ) clip.set_width( Tex->Width() - left );
	if( (tjs_int)Tex->Height() < top + clip.get_height() ) clip.set_height( Tex->Height() - top );
	const tjs_int w = clip.get_width(), h = clip.get_height();
	if( w <= 0 || h <= 0 ) return false;

	if( Color == tTVPGpuTextureColor::RGBA ) {
		if( !bmp->Is32BPP() ) { TVPAddLog( TJS_W("Texture.copyRect: unsupported format") ); return false; }
		std::vector<tjs_uint32> buf( (size_t)w * h );
		for( tjs_int y = 0; y < h; y++ )
			memcpy( &buf[(size_t)w * y], (const tjs_uint32*)bmp->GetScanLine( clip.top + y ) + clip.left, w * 4 );
		Device::Get()->UploadTexture( Tex.get(), 0, left, top, w, h, buf.data(), w * 4 );
	} else {
		std::vector<tjs_uint8> buf( (size_t)w * h );
		for( tjs_int y = 0; y < h; y++ ) {
			if( bmp->Is8BPP() ) {
				memcpy( &buf[(size_t)w * y], (const tjs_uint8*)bmp->GetScanLine( clip.top + y ) + clip.left, w );
			} else {
				const tjs_uint32* s = (const tjs_uint32*)bmp->GetScanLine( clip.top + y ) + clip.left;
				for( tjs_int x = 0; x < w; x++ ) buf[(size_t)w * y + x] = (tjs_uint8)( s[x] >> 24 );
			}
		}
		Device::Get()->UploadTexture( Tex.get(), 0, left, top, w, h, buf.data(), w );
	}
	return true;
}

void tTJSNI_GpuTexture::UpdateDirect( tjs_int x, tjs_int y, tjs_int w, tjs_int h, const void* bits, tjs_int pitch )
{
	if( !Tex || w <= 0 || h <= 0 ) return;
	Device::Get()->UploadTexture( Tex.get(), 0, x, y, w, h, bits, pitch );
}

void tTJSNI_GpuTexture::CopyTexture( const tTJSNI_GpuTexture* src )
{
	if( !src || src == this || !src->Tex ) return;
	TextureDesc d = src->Tex->GetDesc();
	d.mipLevels = 1;
	Tex = Device::Get()->CreateTexture( d );
	Device::Get()->CopyTexture( src->Tex.get(), 0, 0, Tex.get(), 0, 0, d.width, d.height );
	Color = src->Color;
	SrcWidth = d.width;
	SrcHeight = d.height;
	MarginRectObject.Clear();
}

bool tTJSNI_GpuTexture::Resize( tjs_int width, tjs_int height )
{
	if( Tex && width <= (tjs_int)Tex->Width() && height <= (tjs_int)Tex->Height() ) {
		SrcWidth = width;
		SrcHeight = height;
		return true;
	}
	return false;
}

//---------------------------------------------------------------------------
// tTJSNC_GpuTexture
//---------------------------------------------------------------------------
tjs_uint32 tTJSNC_GpuTexture::ClassID = -1;
tTJSNC_GpuTexture::tTJSNC_GpuTexture() : tTJSNativeClass( TJS_W("Texture") )
{
	TJS_BEGIN_NATIVE_MEMBERS( Texture )
	TJS_DECL_EMPTY_FINALIZE_METHOD
	TJS_BEGIN_NATIVE_CONSTRUCTOR_DECL( _this, tTJSNI_GpuTexture, Texture ) { return TJS_S_OK; }
	TJS_END_NATIVE_CONSTRUCTOR_DECL( Texture )

	TJS_BEGIN_NATIVE_METHOD_DECL( copyRect )
	{
		TJS_GET_NATIVE_INSTANCE( _this, tTJSNI_GpuTexture );
		if( numparams == 1 ) {
			tTJSNI_Bitmap* bitmap = (tTJSNI_Bitmap*)TJSGetNativeInstance( tTJSNC_Bitmap::ClassID, param[0] );
			if( !bitmap ) return TJS_E_INVALIDPARAM;
			const tTVPBaseBitmap* b = bitmap->GetBitmap();
			_this->CopyBitmap( 0, 0, b, tTVPRect( 0, 0, b->GetWidth(), b->GetHeight() ) );
			return TJS_S_OK;
		}
		if( numparams < 4 ) return TJS_E_BADPARAMCOUNT;
		tTJSNI_Bitmap* bitmap = (tTJSNI_Bitmap*)TJSGetNativeInstance( tTJSNC_Bitmap::ClassID, param[2] );
		if( !bitmap ) return TJS_E_INVALIDPARAM;
		tTJSNI_Rect* rect = (tTJSNI_Rect*)TJSGetNativeInstance( tTJSNC_Rect::ClassID, param[3] );
		tTVPRect srcRect;
		if( !rect ) {
			if( numparams < 7 ) return TJS_E_INVALIDPARAM;
			srcRect.left = *param[3];
			srcRect.top = *param[4];
			srcRect.right = (tjs_int)*param[5] + srcRect.left;
			srcRect.bottom = (tjs_int)*param[6] + srcRect.top;
		} else {
			srcRect = rect->Get();
		}
		_this->CopyBitmap( *param[0], *param[1], bitmap->GetBitmap(), srcRect );
		return TJS_S_OK;
	}
	TJS_END_NATIVE_METHOD_DECL( copyRect )

	TJS_BEGIN_NATIVE_METHOD_DECL( copyTexture )
	{
		TJS_GET_NATIVE_INSTANCE( _this, tTJSNI_GpuTexture );
		if( numparams >= 1 ) {
			tTJSNI_GpuTexture* src = (tTJSNI_GpuTexture*)TJSGetNativeInstance( tTJSNC_GpuTexture::ClassID, param[0] );
			if( !src ) return TJS_E_INVALIDPARAM;
			_this->CopyTexture( src );
		}
		return TJS_S_OK;
	}
	TJS_END_NATIVE_METHOD_DECL( copyTexture )

	TJS_BEGIN_NATIVE_METHOD_DECL( setDrawSize )
	{
		TJS_GET_NATIVE_INSTANCE( _this, tTJSNI_GpuTexture );
		if( numparams < 2 ) return TJS_E_BADPARAMCOUNT;
		_this->SetDrawSize( (tjs_uint)(tjs_int64)*param[0], (tjs_uint)(tjs_int64)*param[1] );
		return TJS_S_OK;
	}
	TJS_END_NATIVE_METHOD_DECL( setDrawSize )

#define GPU_TEX_RO_PROP( name, expr ) \
	TJS_BEGIN_NATIVE_PROP_DECL( name ) { \
		TJS_BEGIN_NATIVE_PROP_GETTER { \
			TJS_GET_NATIVE_INSTANCE( _this, tTJSNI_GpuTexture ); \
			*result = expr; return TJS_S_OK; \
		} TJS_END_NATIVE_PROP_GETTER \
		TJS_DENY_NATIVE_PROP_SETTER \
	} TJS_END_NATIVE_PROP_DECL( name )
#define GPU_TEX_RW_PROP( name, getter, setter ) \
	TJS_BEGIN_NATIVE_PROP_DECL( name ) { \
		TJS_BEGIN_NATIVE_PROP_GETTER { \
			TJS_GET_NATIVE_INSTANCE( _this, tTJSNI_GpuTexture ); \
			*result = (tjs_int)_this->getter(); return TJS_S_OK; \
		} TJS_END_NATIVE_PROP_GETTER \
		TJS_BEGIN_NATIVE_PROP_SETTER { \
			TJS_GET_NATIVE_INSTANCE( _this, tTJSNI_GpuTexture ); \
			_this->setter( (tjs_int)*param ); return TJS_S_OK; \
		} TJS_END_NATIVE_PROP_SETTER \
	} TJS_END_NATIVE_PROP_DECL( name )
#define GPU_STATIC_INT_PROP( name, value ) \
	TJS_BEGIN_NATIVE_PROP_DECL( name ) { \
		TJS_BEGIN_NATIVE_PROP_GETTER { *result = (tjs_int)( value ); return TJS_S_OK; } TJS_END_NATIVE_PROP_GETTER \
		TJS_DENY_NATIVE_PROP_SETTER \
	} TJS_END_NATIVE_STATIC_PROP_DECL( name )

	GPU_TEX_RO_PROP( width, (tjs_int64)_this->GetWidth() )
	GPU_TEX_RO_PROP( height, (tjs_int64)_this->GetHeight() )
	GPU_TEX_RO_PROP( memoryWidth, (tjs_int64)_this->GetMemoryWidth() )
	GPU_TEX_RO_PROP( memoryHeight, (tjs_int64)_this->GetMemoryHeight() )
	GPU_TEX_RO_PROP( isGray, (tjs_int)( _this->IsGray() ? 1 : 0 ) )
	GPU_TEX_RO_PROP( isPowerOfTwo, (tjs_int)( _this->IsPowerOfTwo() ? 1 : 0 ) )
	GPU_TEX_RO_PROP( nativeHandle, _this->GetNativeHandle() )
	GPU_TEX_RO_PROP( margin9Patch, _this->GetMarginRectObject() )
	GPU_TEX_RW_PROP( stretchType, GetStretchType, SetStretchType )
	GPU_TEX_RW_PROP( wrapModeHorizontal, GetWrapModeHorizontal, SetWrapModeHorizontal )
	GPU_TEX_RW_PROP( wrapModeVertical, GetWrapModeVertical, SetWrapModeVertical )
	GPU_STATIC_INT_PROP( stNearest, TVP_GL_NEAREST )
	GPU_STATIC_INT_PROP( stLinear, TVP_GL_LINEAR )
	GPU_STATIC_INT_PROP( wmRepeat, TVP_GL_REPEAT )
	GPU_STATIC_INT_PROP( wmClampToEdge, TVP_GL_CLAMP_TO_EDGE )
	GPU_STATIC_INT_PROP( wmMirror, TVP_GL_MIRRORED_REPEAT )

	TJS_END_NATIVE_MEMBERS
}
tTJSNativeClass* TVPCreateNativeClass_GpuTexture() { return new tTJSNC_GpuTexture(); }

//---------------------------------------------------------------------------
// tTJSNI_GpuOffscreen
//---------------------------------------------------------------------------
void tTJSNI_GpuOffscreen::Create( tjs_uint w, tjs_uint h )
{
	if( w == 0 || h == 0 ) TVPThrowExceptionMessage( TJS_W("Offscreen: invalid size") );
	TextureDesc d;
	d.width = w; d.height = h;
	d.format = TextureFormat::BGRA8;
	d.renderTarget = true;
	Color = Device::Get()->CreateTexture( d );
	Stencil.reset();
}

TVPGpu::Texture* tTJSNI_GpuOffscreen::GetStencilTexture()
{
	if( !Stencil && Color ) {
		TextureDesc d;
		d.width = Color->Width(); d.height = Color->Height();
		d.format = TextureFormat::D32F_S8;
		d.renderTarget = true;
		Stencil = Device::Get()->CreateTexture( d );
	}
	return Stencil.get();
}

tjs_error TJS_INTF_METHOD tTJSNI_GpuOffscreen::Construct( tjs_int numparams, tTJSVariant** param, iTJSDispatch2* tjs_obj )
{
	if( numparams < 2 ) return TJS_E_BADPARAMCOUNT;
	Create( (tjs_int)*param[0], (tjs_int)*param[1] );
	return TJS_S_OK;
}

void TJS_INTF_METHOD tTJSNI_GpuOffscreen::Invalidate()
{
	Color.reset();
	Stencil.reset();
}

void tTJSNI_GpuOffscreen::CopyFromBitmap( tjs_int left, tjs_int top, const tTVPBaseBitmap* bmp, const tTVPRect& srcRect )
{
	if( !Color || !bmp || !bmp->Is32BPP() ) return;
	tTVPRect clip( srcRect );
	if( clip.right > (tjs_int)bmp->GetWidth() ) clip.right = bmp->GetWidth();
	if( clip.bottom > (tjs_int)bmp->GetHeight() ) clip.bottom = bmp->GetHeight();
	if( clip.left < 0 ) clip.left = 0;
	if( clip.top < 0 ) clip.top = 0;
	if( left < 0 ) { clip.left -= left; left = 0; }
	if( top < 0 ) { clip.top -= top; top = 0; }
	if( (tjs_int)Color->Width() < left + clip.get_width() ) clip.set_width( Color->Width() - left );
	if( (tjs_int)Color->Height() < top + clip.get_height() ) clip.set_height( Color->Height() - top );
	const tjs_int w = clip.get_width(), h = clip.get_height();
	if( w <= 0 || h <= 0 ) return;
	std::vector<tjs_uint32> buf( (size_t)w * h );
	for( tjs_int y = 0; y < h; y++ )
		memcpy( &buf[(size_t)w * y], (const tjs_uint32*)bmp->GetScanLine( clip.top + y ) + clip.left, w * 4 );
	Device::Get()->UploadTexture( Color.get(), 0, left, top, w, h, buf.data(), w * 4 );
}

void tTJSNI_GpuOffscreen::ExchangeTexture( tTJSNI_GpuTexture* texture )
{
	// 寸法・形式が同じで、描画先にできるテクスチャ同士だけ交換できる
	if( !texture || !texture->Tex || !Color ||
		texture->Tex->Width() != Color->Width() || texture->Tex->Height() != Color->Height() ||
		texture->Color != tTVPGpuTextureColor::RGBA || !texture->Tex->GetDesc().renderTarget ) {
		// 交換できない場合は中身をコピーする (結果は同じに見える)
		if( texture && texture->Tex && Color && texture->Color == tTVPGpuTextureColor::RGBA ) {
			const uint32_t w = std::min( texture->Tex->Width(), Color->Width() );
			const uint32_t h = std::min( texture->Tex->Height(), Color->Height() );
			Device::Get()->CopyTexture( Color.get(), 0, 0, texture->Tex.get(), 0, 0, w, h );
			return;
		}
		TVPThrowExceptionMessage( TVPCannotExchangeTexture );
	}
	std::swap( texture->Tex, Color );
}

//---------------------------------------------------------------------------
// tTJSNC_GpuOffscreen
//---------------------------------------------------------------------------
tjs_uint32 tTJSNC_GpuOffscreen::ClassID = -1;
tTJSNC_GpuOffscreen::tTJSNC_GpuOffscreen() : tTJSNativeClass( TJS_W("Offscreen") )
{
	TJS_BEGIN_NATIVE_MEMBERS( Offscreen )
	TJS_DECL_EMPTY_FINALIZE_METHOD
	TJS_BEGIN_NATIVE_CONSTRUCTOR_DECL( _this, tTJSNI_GpuOffscreen, Offscreen ) { return TJS_S_OK; }
	TJS_END_NATIVE_CONSTRUCTOR_DECL( Offscreen )

	TJS_BEGIN_NATIVE_METHOD_DECL( copyRect )
	{
		TJS_GET_NATIVE_INSTANCE( _this, tTJSNI_GpuOffscreen );
		if( numparams == 1 ) {
			tTJSNI_Bitmap* bitmap = (tTJSNI_Bitmap*)TJSGetNativeInstance( tTJSNC_Bitmap::ClassID, param[0] );
			if( !bitmap ) return TJS_E_INVALIDPARAM;
			const tTVPBaseBitmap* b = bitmap->GetBitmap();
			_this->CopyFromBitmap( 0, 0, b, tTVPRect( 0, 0, b->GetWidth(), b->GetHeight() ) );
			return TJS_S_OK;
		}
		if( numparams < 4 ) return TJS_E_BADPARAMCOUNT;
		tTJSNI_Bitmap* bitmap = (tTJSNI_Bitmap*)TJSGetNativeInstance( tTJSNC_Bitmap::ClassID, param[2] );
		if( !bitmap ) return TJS_E_INVALIDPARAM;
		tTJSNI_Rect* rect = (tTJSNI_Rect*)TJSGetNativeInstance( tTJSNC_Rect::ClassID, param[3] );
		tTVPRect srcRect;
		if( !rect ) {
			if( numparams < 7 ) return TJS_E_INVALIDPARAM;
			srcRect.left = *param[3];
			srcRect.top = *param[4];
			srcRect.right = (tjs_int)*param[5] + srcRect.left;
			srcRect.bottom = (tjs_int)*param[6] + srcRect.top;
		} else {
			srcRect = rect->Get();
		}
		_this->CopyFromBitmap( *param[0], *param[1], bitmap->GetBitmap(), srcRect );
		return TJS_S_OK;
	}
	TJS_END_NATIVE_METHOD_DECL( copyRect )

	TJS_BEGIN_NATIVE_METHOD_DECL( exchangeTexture )
	{
		TJS_GET_NATIVE_INSTANCE( _this, tTJSNI_GpuOffscreen );
		if( numparams < 1 ) return TJS_E_BADPARAMCOUNT;
		tTJSNI_GpuTexture* tex = (tTJSNI_GpuTexture*)TJSGetNativeInstance( tTJSNC_GpuTexture::ClassID, param[0] );
		if( !tex ) return TJS_E_INVALIDPARAM;
		_this->ExchangeTexture( tex );
		return TJS_S_OK;
	}
	TJS_END_NATIVE_METHOD_DECL( exchangeTexture )

#define GPU_OFF_RO_PROP( name, expr ) \
	TJS_BEGIN_NATIVE_PROP_DECL( name ) { \
		TJS_BEGIN_NATIVE_PROP_GETTER { \
			TJS_GET_NATIVE_INSTANCE( _this, tTJSNI_GpuOffscreen ); \
			*result = expr; return TJS_S_OK; \
		} TJS_END_NATIVE_PROP_GETTER \
		TJS_DENY_NATIVE_PROP_SETTER \
	} TJS_END_NATIVE_PROP_DECL( name )

	GPU_OFF_RO_PROP( width, (tjs_int64)_this->GetWidth() )
	GPU_OFF_RO_PROP( height, (tjs_int64)_this->GetHeight() )
	GPU_OFF_RO_PROP( nativeHandle, _this->GetNativeHandle() )

	TJS_END_NATIVE_MEMBERS
}
tTJSNativeClass* TVPCreateNativeClass_GpuOffscreen() { return new tTJSNC_GpuOffscreen(); }
