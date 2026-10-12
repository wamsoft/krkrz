//---------------------------------------------------------------------------
// VertexBuffer / VertexBinder (GPU 版)
//
// TJS から見える型・用途・プリミティブの定数は GL 版と同じ GL の enum 値。
// SDL_GPU に無い形式は格納時に変換する:
//   - dtFixed (16.16 固定小数) は float にして持つ
//   - 8bit のインデックスは 16bit にして持つ
//---------------------------------------------------------------------------
#include "tjsCommHead.h"

#include "tjsArray.h"
#include "GpuCanvasIntf.h"
#include "MsgIntf.h"
#include "DebugIntf.h"
#include <memory>

using namespace TVPGpu;

// GL の enum 値 (TJS の定数と互換)
enum : tjs_int {
	GL_BYTE_ = 0x1400, GL_UNSIGNED_BYTE_ = 0x1401, GL_SHORT_ = 0x1402, GL_UNSIGNED_SHORT_ = 0x1403,
	GL_INT_ = 0x1404, GL_UNSIGNED_INT_ = 0x1405, GL_FLOAT_ = 0x1406, GL_FIXED_ = 0x140C,
	GL_STREAM_DRAW_ = 0x88E0, GL_STATIC_DRAW_ = 0x88E4, GL_DYNAMIC_DRAW_ = 0x88E8,
	GL_POINTS_ = 0, GL_LINES_ = 1, GL_LINE_LOOP_ = 2, GL_LINE_STRIP_ = 3,
	GL_TRIANGLES_ = 4, GL_TRIANGLE_STRIP_ = 5, GL_TRIANGLE_FAN_ = 6,
};

//! 格納時の 1 要素の大きさ
static tjs_uint StoredElementSize( tjs_int dataType, bool index )
{
	switch( dataType ) {
	case GL_BYTE_: case GL_UNSIGNED_BYTE_: return index ? 2 : 1;
	case GL_SHORT_: case GL_UNSIGNED_SHORT_: return 2;
	case GL_INT_: case GL_UNSIGNED_INT_: case GL_FLOAT_: case GL_FIXED_: return 4;
	}
	return 0;
}

//! TJS の値 1 つを格納形式で書く
static void StoreElement( uint8_t* dst, tjs_int dataType, bool index, const tTJSVariant& v )
{
	switch( dataType ) {
	case GL_BYTE_:
		if( index ) *(uint16_t*)dst = (uint16_t)(uint8_t)(tjs_int)v; else *(int8_t*)dst = (int8_t)(tjs_int)v;
		break;
	case GL_UNSIGNED_BYTE_:
		if( index ) *(uint16_t*)dst = (uint16_t)(uint8_t)(tjs_int)v; else *(uint8_t*)dst = (uint8_t)(tjs_int)v;
		break;
	case GL_SHORT_: *(int16_t*)dst = (int16_t)(tjs_int)v; break;
	case GL_UNSIGNED_SHORT_: *(uint16_t*)dst = (uint16_t)(tjs_int)v; break;
	case GL_INT_: *(int32_t*)dst = (int32_t)(tjs_int)v; break;
	case GL_UNSIGNED_INT_: *(uint32_t*)dst = (uint32_t)(tjs_int64)v; break;
	case GL_FLOAT_: *(float*)dst = (float)(tjs_real)v; break;
	case GL_FIXED_: *(float*)dst = (float)( (tjs_int)v ) / 65536.0f; break;
	}
}

//---------------------------------------------------------------------------
// tTJSNI_GpuVertexBuffer
//---------------------------------------------------------------------------
tjs_error TJS_INTF_METHOD tTJSNI_GpuVertexBuffer::Construct( tjs_int numparams, tTJSVariant** param, iTJSDispatch2* tjs_obj )
{
	if( numparams < 3 ) return TJS_E_BADPARAMCOUNT;
	DataType = (tjs_int)*param[1];
	UpdateType = (tjs_int)*param[2];
	Index = numparams >= 4 && (tjs_int)*param[3] != 0;
	const tjs_uint esize = StoredElementSize( DataType, Index );
	if( !esize ) TVPThrowExceptionMessage( TVPUnknownDataType );

	if( param[0]->Type() == tvtObject ) {
		tTJSVariantClosure clo = param[0]->AsObjectClosureNoAddRef();
		if( !clo.Object ) return TJS_E_INVALIDPARAM;
		tjs_int count = TJSGetArrayElementCount( clo.Object );
		if( count == 0 ) TVPThrowExceptionMessage( TVPArrayIsZero );
		Shadow.resize( (size_t)count * esize );
		tTJSVariant tmp;
		for( tjs_int i = 0; i < count; i++ ) {
			if( TJS_FAILED( clo.Object->PropGetByNum( TJS_MEMBERMUSTEXIST, i, &tmp, clo.ObjThis ) ) )
				TVPThrowExceptionMessage( TVPInsufficientNumberOfArrays );
			StoreElement( &Shadow[(size_t)i * esize], DataType, Index, tmp );
		}
	} else {
		// バイト数指定 (GL の格納単位)。格納形式が大きくなる型はそのぶん増やす
		tjs_int bytes = *param[0];
		const tjs_uint glsize = ( DataType == GL_BYTE_ || DataType == GL_UNSIGNED_BYTE_ ) ? 1
			: ( DataType == GL_SHORT_ || DataType == GL_UNSIGNED_SHORT_ ) ? 2 : 4;
		Shadow.assign( (size_t)( bytes / glsize ) * esize, 0 );
	}
	Buf = Device::Get()->CreateBuffer( Index, (uint32_t)Shadow.size() );
	Device::Get()->UpdateBuffer( Buf.get(), 0, Shadow.data(), (uint32_t)Shadow.size() );
	return TJS_S_OK;
}

void TJS_INTF_METHOD tTJSNI_GpuVertexBuffer::Invalidate()
{
	Buf.reset();
	Shadow.clear();
}

void* tTJSNI_GpuVertexBuffer::Lock()
{
	Locked = true;
	return Shadow.data();
}

void tTJSNI_GpuVertexBuffer::Unlock()
{
	if( !Locked ) return;
	Locked = false;
	if( Buf ) Device::Get()->UpdateBuffer( Buf.get(), 0, Shadow.data(), (uint32_t)Shadow.size() );
}

void tTJSNI_GpuVertexBuffer::SetVertex( const tTJSVariant* param, tjs_int offset )
{
	if( UpdateType == GL_STATIC_DRAW_ ) TVPThrowExceptionMessage( TVPThisVertexBufferIsConstant );
	tTJSVariantClosure clo = param->AsObjectClosureNoAddRef();
	if( !clo.Object ) return;
	tjs_int count = TJSGetArrayElementCount( clo.Object );
	if( count <= 0 ) return;
	const tjs_uint esize = StoredElementSize( DataType, Index );
	if( offset < 0 || ( (size_t)offset + count ) * esize > Shadow.size() ) TVPThrowExceptionMessage( TJSRangeError );
	tTJSVariant tmp;
	for( tjs_int i = 0; i < count; i++ ) {
		if( TJS_FAILED( clo.Object->PropGetByNum( TJS_MEMBERMUSTEXIST, i, &tmp, clo.ObjThis ) ) )
			TVPThrowExceptionMessage( TVPInsufficientNumberOfArrays );
		StoreElement( &Shadow[( (size_t)offset + i ) * esize], DataType, Index, tmp );
	}
	if( Buf ) Device::Get()->UpdateBuffer( Buf.get(), (uint32_t)( offset * esize ), &Shadow[(size_t)offset * esize], (uint32_t)( count * esize ) );
}

//---------------------------------------------------------------------------
// tTJSNC_GpuVertexBuffer
//---------------------------------------------------------------------------
#define GPU_STATIC_INT_PROP( name, value ) \
	TJS_BEGIN_NATIVE_PROP_DECL( name ) { \
		TJS_BEGIN_NATIVE_PROP_GETTER { *result = (tjs_int)( value ); return TJS_S_OK; } TJS_END_NATIVE_PROP_GETTER \
		TJS_DENY_NATIVE_PROP_SETTER \
	} TJS_END_NATIVE_STATIC_PROP_DECL( name )

tjs_uint32 tTJSNC_GpuVertexBuffer::ClassID = -1;
tTJSNC_GpuVertexBuffer::tTJSNC_GpuVertexBuffer() : tTJSNativeClass( TJS_W("VertexBuffer") )
{
	TJS_BEGIN_NATIVE_MEMBERS( VertexBuffer )
	TJS_DECL_EMPTY_FINALIZE_METHOD
	TJS_BEGIN_NATIVE_CONSTRUCTOR_DECL( _this, tTJSNI_GpuVertexBuffer, VertexBuffer ) { return TJS_S_OK; }
	TJS_END_NATIVE_CONSTRUCTOR_DECL( VertexBuffer )

	TJS_BEGIN_NATIVE_METHOD_DECL( setVertex )
	{
		TJS_GET_NATIVE_INSTANCE( _this, tTJSNI_GpuVertexBuffer );
		if( numparams < 1 ) return TJS_E_BADPARAMCOUNT;
		tjs_int offset = numparams >= 2 ? (tjs_int)*param[1] : 0;
		_this->SetVertex( param[0], offset );
		return TJS_S_OK;
	}
	TJS_END_NATIVE_METHOD_DECL( setVertex )

	TJS_BEGIN_NATIVE_METHOD_DECL( lock )
	{
		TJS_GET_NATIVE_INSTANCE( _this, tTJSNI_GpuVertexBuffer );
		if( result ) *result = (tjs_int64)(intptr_t)_this->Lock();
		return TJS_S_OK;
	}
	TJS_END_NATIVE_METHOD_DECL( lock )

	TJS_BEGIN_NATIVE_METHOD_DECL( unlock )
	{
		TJS_GET_NATIVE_INSTANCE( _this, tTJSNI_GpuVertexBuffer );
		_this->Unlock();
		return TJS_S_OK;
	}
	TJS_END_NATIVE_METHOD_DECL( unlock )

#define GPU_VB_RO_PROP( name, expr ) \
	TJS_BEGIN_NATIVE_PROP_DECL( name ) { \
		TJS_BEGIN_NATIVE_PROP_GETTER { \
			TJS_GET_NATIVE_INSTANCE( _this, tTJSNI_GpuVertexBuffer ); \
			*result = expr; return TJS_S_OK; \
		} TJS_END_NATIVE_PROP_GETTER \
		TJS_DENY_NATIVE_PROP_SETTER \
	} TJS_END_NATIVE_PROP_DECL( name )

	GPU_VB_RO_PROP( size, _this->GetBufferSize() )
	GPU_VB_RO_PROP( dataType, _this->GetDataType() )
	GPU_VB_RO_PROP( updateType, _this->GetUpdateType() )
	GPU_VB_RO_PROP( isIndex, (tjs_int)( _this->IsIndex() ? 1 : 0 ) )
	GPU_VB_RO_PROP( nativeHandle, _this->GetNativeHandle() )

	GPU_STATIC_INT_PROP( dtByte, GL_BYTE_ )
	GPU_STATIC_INT_PROP( dtUByte, GL_UNSIGNED_BYTE_ )
	GPU_STATIC_INT_PROP( dtShort, GL_SHORT_ )
	GPU_STATIC_INT_PROP( dtUShort, GL_UNSIGNED_SHORT_ )
	GPU_STATIC_INT_PROP( dtInt, GL_INT_ )
	GPU_STATIC_INT_PROP( dtUInt, GL_UNSIGNED_INT_ )
	GPU_STATIC_INT_PROP( dtFixed, GL_FIXED_ )
	GPU_STATIC_INT_PROP( dtFloat, GL_FLOAT_ )
	GPU_STATIC_INT_PROP( utStream, GL_STREAM_DRAW_ )
	GPU_STATIC_INT_PROP( utStatic, GL_STATIC_DRAW_ )
	GPU_STATIC_INT_PROP( utDynamic, GL_DYNAMIC_DRAW_ )
	GPU_STATIC_INT_PROP( ptPoints, GL_POINTS_ )
	GPU_STATIC_INT_PROP( ptLineStrip, GL_LINE_STRIP_ )
	GPU_STATIC_INT_PROP( ptLineLoop, GL_LINE_LOOP_ )
	GPU_STATIC_INT_PROP( ptLines, GL_LINES_ )
	GPU_STATIC_INT_PROP( ptTriangleStrip, GL_TRIANGLE_STRIP_ )
	GPU_STATIC_INT_PROP( ptTriangleFan, GL_TRIANGLE_FAN_ )
	GPU_STATIC_INT_PROP( ptTriangles, GL_TRIANGLES_ )

	TJS_END_NATIVE_MEMBERS
}
tTJSNativeClass* TVPCreateNativeClass_GpuVertexBuffer() { return new tTJSNC_GpuVertexBuffer(); }

//---------------------------------------------------------------------------
// tTJSNI_GpuVertexBinder
//   new VertexBinder( vertexBuffer, stride, componentCount, offset, normalize )
//---------------------------------------------------------------------------
void tTJSNI_GpuVertexBinder::SetVertexBufferObject( const tTJSVariant& val )
{
	VertexBufferObject = val;
	VertexBufferInstance = nullptr;
	if( VertexBufferObject.Type() == tvtObject ) {
		VertexBufferInstance = (tTJSNI_GpuVertexBuffer*)TJSGetNativeInstance( tTJSNC_GpuVertexBuffer::ClassID, &VertexBufferObject );
		if( !VertexBufferInstance ) TVPThrowExceptionMessage( TVPCannotRetriveInstance, TJS_W("VertexBuffer") );
	}
}

tjs_error TJS_INTF_METHOD tTJSNI_GpuVertexBinder::Construct( tjs_int numparams, tTJSVariant** param, iTJSDispatch2* tjs_obj )
{
	if( numparams >= 1 ) SetVertexBufferObject( *param[0] );
	if( numparams >= 2 ) Stride = *param[1];
	if( numparams >= 3 ) ComponentCount = (tjs_int)*param[2];
	if( numparams >= 4 ) Offset = (tjs_int)*param[3];
	if( numparams >= 5 ) Normalize = (tjs_int)*param[4] != 0;
	return TJS_S_OK;
}

void TJS_INTF_METHOD tTJSNI_GpuVertexBinder::Invalidate()
{
	VertexBufferObject.Clear();
	VertexBufferInstance = nullptr;
}

VertexFormat tTJSNI_GpuVertexBinder::GetVertexFormat() const
{
	if( !VertexBufferInstance ) return VertexFormat::Invalid;
	const tjs_uint n = ComponentCount;
	switch( VertexBufferInstance->GetDataType() ) {
	case GL_FLOAT_:
	case GL_FIXED_:
		return n == 1 ? VertexFormat::Float : n == 2 ? VertexFormat::Float2 : n == 3 ? VertexFormat::Float3 : n == 4 ? VertexFormat::Float4 : VertexFormat::Invalid;
	case GL_INT_:
		return n == 1 ? VertexFormat::Int : n == 2 ? VertexFormat::Int2 : n == 3 ? VertexFormat::Int3 : n == 4 ? VertexFormat::Int4 : VertexFormat::Invalid;
	case GL_UNSIGNED_INT_:
		return n == 1 ? VertexFormat::UInt : n == 2 ? VertexFormat::UInt2 : n == 3 ? VertexFormat::UInt3 : n == 4 ? VertexFormat::UInt4 : VertexFormat::Invalid;
	case GL_BYTE_:
		if( n == 2 ) return Normalize ? VertexFormat::Byte2Norm : VertexFormat::Byte2;
		if( n == 4 ) return Normalize ? VertexFormat::Byte4Norm : VertexFormat::Byte4;
		return VertexFormat::Invalid;
	case GL_UNSIGNED_BYTE_:
		if( n == 2 ) return Normalize ? VertexFormat::UByte2Norm : VertexFormat::UByte2;
		if( n == 4 ) return Normalize ? VertexFormat::UByte4Norm : VertexFormat::UByte4;
		return VertexFormat::Invalid;
	case GL_SHORT_:
		if( n == 2 ) return Normalize ? VertexFormat::Short2Norm : VertexFormat::Short2;
		if( n == 4 ) return Normalize ? VertexFormat::Short4Norm : VertexFormat::Short4;
		return VertexFormat::Invalid;
	case GL_UNSIGNED_SHORT_:
		if( n == 2 ) return Normalize ? VertexFormat::UShort2Norm : VertexFormat::UShort2;
		if( n == 4 ) return Normalize ? VertexFormat::UShort4Norm : VertexFormat::UShort4;
		return VertexFormat::Invalid;
	}
	return VertexFormat::Invalid;
}

//---------------------------------------------------------------------------
// tTJSNC_GpuVertexBinder
//---------------------------------------------------------------------------
tjs_uint32 tTJSNC_GpuVertexBinder::ClassID = -1;
tTJSNC_GpuVertexBinder::tTJSNC_GpuVertexBinder() : tTJSNativeClass( TJS_W("VertexBinder") )
{
	TJS_BEGIN_NATIVE_MEMBERS( VertexBinder )
	TJS_DECL_EMPTY_FINALIZE_METHOD
	TJS_BEGIN_NATIVE_CONSTRUCTOR_DECL( _this, tTJSNI_GpuVertexBinder, VertexBinder ) { return TJS_S_OK; }
	TJS_END_NATIVE_CONSTRUCTOR_DECL( VertexBinder )

	TJS_BEGIN_NATIVE_PROP_DECL( vertexBuffer ) {
		TJS_BEGIN_NATIVE_PROP_GETTER {
			TJS_GET_NATIVE_INSTANCE( _this, tTJSNI_GpuVertexBinder );
			*result = _this->GetVertexBufferObject(); return TJS_S_OK;
		} TJS_END_NATIVE_PROP_GETTER
		TJS_DENY_NATIVE_PROP_SETTER
	} TJS_END_NATIVE_PROP_DECL( vertexBuffer )

#define GPU_VBD_RW_PROP( name, getter, setter, type ) \
	TJS_BEGIN_NATIVE_PROP_DECL( name ) { \
		TJS_BEGIN_NATIVE_PROP_GETTER { \
			TJS_GET_NATIVE_INSTANCE( _this, tTJSNI_GpuVertexBinder ); \
			*result = (tjs_int)_this->getter(); return TJS_S_OK; \
		} TJS_END_NATIVE_PROP_GETTER \
		TJS_BEGIN_NATIVE_PROP_SETTER { \
			TJS_GET_NATIVE_INSTANCE( _this, tTJSNI_GpuVertexBinder ); \
			_this->setter( (type)(tjs_int)*param ); return TJS_S_OK; \
		} TJS_END_NATIVE_PROP_SETTER \
	} TJS_END_NATIVE_PROP_DECL( name )

	GPU_VBD_RW_PROP( stride, GetStride, SetStride, tjs_int )
	GPU_VBD_RW_PROP( componentCount, GetComponentCount, SetComponentCount, tjs_uint )
	GPU_VBD_RW_PROP( offset, GetOffset, SetOffset, tjs_uint )
	GPU_VBD_RW_PROP( normalize, GetNormalize, SetNormalize, bool )

	TJS_END_NATIVE_MEMBERS
}
tTJSNativeClass* TVPCreateNativeClass_GpuVertexBinder() { return new tTJSNC_GpuVertexBinder(); }
