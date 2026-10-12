//---------------------------------------------------------------------------
// ShaderProgram (GPU 版)
//
// new ShaderProgram( vs, fs [, vsIsFile=true, fsIsFile=true] )
//   ソース (ファイルならその中身) のハッシュで事前変換済みのシェーダーを引く
//   (GpuShaderLibrary)。無ければ -gpushadercollect のフォルダへソースを書き出して例外。
//
// uniform / テクスチャ / 頂点属性はプロパティ代入で与える (GL 版と同じ):
//   shader.u_time = 1.0;  shader.s_tex1 = texture;  shader.a_pos = vertexBinder;
// uniform は stage ごとの std140 ブロック (影バッファ) に書き、描画時に送る。
//---------------------------------------------------------------------------
#include "tjsCommHead.h"

#include "tjsArray.h"
#include "GpuCanvasIntf.h"
#include "GpuShaderLibrary.h"
#include "MsgIntf.h"
#include "CharacterSet.h"
#include "StorageIntf.h"
#include "UtilStreams.h"
#include "DebugIntf.h"
#include "LogIntf.h"
#include <cstdio>
#include <algorithm>

using namespace TVPGpu;

//---------------------------------------------------------------------------
static std::string ReadShaderSource( const ttstr& src, bool isFile )
{
	std::string out;
	if( isFile ) {
		tTVPStreamHolder st( TVPNormalizeStorageName( src ), TJS_BS_READ );
		tjs_uint size = (tjs_uint)st->GetSize();
		out.resize( size );
		if( size && st->Read( &out[0], size ) != size ) TJS_eTJSError( TJSReadError );
	} else {
		if( !TVPUtf16ToUtf8( out, src.AsStdString() ) ) TVPThrowExceptionMessage( TVPInvalidUTF16ToUTF8 );
	}
	return out;
}

//---------------------------------------------------------------------------
tjs_error TJS_INTF_METHOD tTJSNI_GpuShaderProgram::Construct( tjs_int numparams, tTJSVariant** param, iTJSDispatch2* tjs_obj )
{
	if( numparams < 2 ) return TJS_E_BADPARAMCOUNT;
	bool isVsFile = numparams > 2 ? (tjs_int)*param[2] != 0 : true;
	bool isFsFile = numparams > 3 ? (tjs_int)*param[3] != 0 : true;
	std::string vs = ReadShaderSource( *param[0], isVsFile );
	std::string fs = ReadShaderSource( *param[1], isFsFile );

	ShaderLibrary& lib = ShaderLibrary::Instance();
	lib.LoadUserPacks();
	const uint64_t key = ShaderProgramKey( vs, fs );
	ProgramBinary bin;
	if( !lib.Find( key, bin ) ) {
		std::string where = lib.CollectMissing( key, vs, fs );
		char keyname[32];
		snprintf( keyname, sizeof(keyname), "%016llx", (unsigned long long)key );
		ttstr msg = ttstr( TJS_W("ShaderProgram: no precompiled shader for this source (key=") ) + ttstr( keyname ) + TJS_W(")");
		if( !where.empty() ) msg += ttstr( TJS_W(", source written to ") ) + ttstr( where.c_str() );
		else msg += TJS_W(". Run with -gpushadercollect=<folder> and convert it with gpushaderc.py");
		TVPThrowExceptionMessage( msg.c_str() );
	}
	std::string err;
	Prog = Device::Get()->CreateProgram( bin, &err );
	if( !Prog ) {
		TVPAddLog( ttstr( TJS_W("ShaderProgram: ") ) + ttstr( err.c_str() ) );
		TVPThrowExceptionMessage( TVPShaderCompileError );
	}
	const ShaderReflection& r = Prog->Reflection();
	Block[0].assign( r.blockSize[0], 0 );
	Block[1].assign( r.blockSize[1], 0 );
	SamplerValues.resize( r.samplerCount[1] );
	return TJS_S_OK;
}

void TJS_INTF_METHOD tTJSNI_GpuShaderProgram::Invalidate()
{
	Prog.reset();
	SamplerValues.clear();
	AttributeValues.clear();
}

//---------------------------------------------------------------------------
// TJS の値を uniform の型に合わせてブロックへ書く (std140)
//---------------------------------------------------------------------------
static void ReadArray( const tTJSVariant* param, std::vector<tTJSVariant>& out, tjs_uint need )
{
	tTJSVariantClosure clo = param->AsObjectClosureNoAddRef();
	if( !clo.Object ) TJS_eTJSError( TJSSpecifyArray );
	tjs_int count = TJSGetArrayElementCount( clo.Object );
	if( count < (tjs_int)need ) TVPThrowExceptionMessage( TVPInsufficientNumberOfArrays );
	out.resize( count );
	for( tjs_int i = 0; i < count; i++ ) {
		if( TJS_FAILED( clo.Object->PropGetByNum( TJS_MEMBERMUSTEXIST, i, &out[i], clo.ObjThis ) ) )
			TVPThrowExceptionMessage( TVPInsufficientNumberOfArrays );
	}
}

static bool IsIntType( UniformType t )
{
	return t == UniformType::Int || t == UniformType::IVec2 || t == UniformType::IVec3 || t == UniformType::IVec4;
}
static bool IsUIntType( UniformType t )
{
	return t == UniformType::UInt || t == UniformType::UVec2 || t == UniformType::UVec3 || t == UniformType::UVec4
		|| t == UniformType::Bool || t == UniformType::BVec2 || t == UniformType::BVec3 || t == UniformType::BVec4;
}
//! 行列の列数・行数 (GLSL の matCxR: C 列 R 行)
static bool MatrixShape( UniformType t, tjs_uint& cols, tjs_uint& rows )
{
	switch( t ) {
	case UniformType::Mat2: cols = 2; rows = 2; return true;
	case UniformType::Mat3: cols = 3; rows = 3; return true;
	case UniformType::Mat4: cols = 4; rows = 4; return true;
	case UniformType::Mat2x3: cols = 2; rows = 3; return true;
	case UniformType::Mat2x4: cols = 2; rows = 4; return true;
	case UniformType::Mat3x2: cols = 3; rows = 2; return true;
	case UniformType::Mat3x4: cols = 3; rows = 4; return true;
	case UniformType::Mat4x2: cols = 4; rows = 2; return true;
	case UniformType::Mat4x3: cols = 4; rows = 3; return true;
	default: return false;
	}
}

//! 要素 (配列なら 1 要素) を書く。values[pos..] を消費する
static void WriteElement( uint8_t* dst, const ShaderReflection::Uniform& u, const tTJSVariant* values, tjs_uint& pos )
{
	tjs_uint cols, rows;
	if( MatrixShape( u.type, cols, rows ) ) {
		const tjs_uint stride = u.matrixStride ? u.matrixStride : 16;
		for( tjs_uint c = 0; c < cols; c++ ) {
			float* col = (float*)( dst + stride * c );
			for( tjs_uint r = 0; r < rows; r++ ) col[r] = (float)(tjs_real)values[pos++];
		}
		return;
	}
	const tjs_uint n = UniformComponentCount( u.type );
	for( tjs_uint i = 0; i < n; i++ ) {
		const tTJSVariant& v = values[pos++];
		if( IsIntType( u.type ) ) ( (int32_t*)dst )[i] = (int32_t)(tjs_int)v;
		else if( IsUIntType( u.type ) ) ( (uint32_t*)dst )[i] = (uint32_t)(tjs_int64)v;
		else ( (float*)dst )[i] = (float)(tjs_real)v;
	}
}

void tTJSNI_GpuShaderProgram::WriteUniform( const ShaderReflection::Uniform& u, const tTJSVariant* param )
{
	std::vector<uint8_t>& blk = Block[(int)u.stage];
	const tjs_uint comps = UniformComponentCount( u.type );
	const tjs_uint elems = u.arrayCount ? u.arrayCount : 1;
	std::vector<tTJSVariant> values;
	if( comps == 1 && elems == 1 && param->Type() != tvtObject ) {
		values.push_back( *param );
	} else {
		ReadArray( param, values, comps * ( u.arrayCount ? 1 : elems ) );
	}
	const tjs_uint available = (tjs_uint)values.size() / comps;
	const tjs_uint n = std::min( elems, available );
	tjs_uint pos = 0;
	for( tjs_uint e = 0; e < n; e++ ) {
		const size_t off = u.offset + (size_t)e * ( u.arrayStride ? u.arrayStride : 0 );
		if( off >= blk.size() ) break;
		WriteElement( blk.data() + off, u, values.data(), pos );
	}
}

//---------------------------------------------------------------------------
tjs_error tTJSNI_GpuShaderProgram::SetShaderParam( const ttstr& name, const tTJSVariant* param )
{
	if( !Prog || !param ) return TJS_E_MEMBERNOTFOUND;
	std::string n;
	TVPUtf16ToUtf8( n, name.AsStdString() );
	const ShaderReflection& r = Prog->Reflection();
	bool found = false;
	for( const auto& u : r.uniforms ) {
		if( u.name != n ) continue;
		WriteUniform( u, param );
		found = true;
	}
	for( const auto& s : r.samplers ) {
		if( s.name != n || s.stage != ShaderStage::Fragment ) continue;
		if( s.slot < SamplerValues.size() ) SamplerValues[s.slot] = *param;
		found = true;
	}
	if( r.FindAttribute( n ) ) {
		bool replaced = false;
		for( auto& a : AttributeValues ) if( a.name == n ) { a.value = *param; replaced = true; }
		if( !replaced ) AttributeValues.push_back( { n, *param } );
		found = true;
	}
	return found ? TJS_S_OK : TJS_E_MEMBERNOTFOUND;
}

bool tTJSNI_GpuShaderProgram::SetUniformFloats( const char* name, const float* v, tjs_uint count )
{
	if( !Prog ) return false;
	bool found = false;
	for( const auto& u : Prog->Reflection().uniforms ) {
		if( u.name != name ) continue;
		std::vector<uint8_t>& blk = Block[(int)u.stage];
		tjs_uint cols, rows;
		if( MatrixShape( u.type, cols, rows ) ) {
			const tjs_uint stride = u.matrixStride ? u.matrixStride : 16;
			for( tjs_uint c = 0; c < cols; c++ )
				for( tjs_uint r = 0; r < rows && c * rows + r < count; r++ )
					if( u.offset + stride * c + r * 4 + 4 <= blk.size() )
						memcpy( blk.data() + u.offset + stride * c + r * 4, &v[c * rows + r], 4 );
		} else {
			const tjs_uint n = std::min( count, UniformComponentCount( u.type ) );
			if( u.offset + n * 4 <= blk.size() ) memcpy( blk.data() + u.offset, v, n * 4 );
		}
		found = true;
	}
	return found;
}

bool tTJSNI_GpuShaderProgram::HasUniform( const char* name ) const
{
	if( !Prog ) return false;
	for( const auto& u : Prog->Reflection().uniforms ) if( u.name == name ) return true;
	return false;
}

iTVPGpuDrawable* tTJSNI_GpuShaderProgram::GetSamplerValue( tjs_uint slot ) const
{
	if( slot >= SamplerValues.size() ) return nullptr;
	return TVPGetGpuDrawable( &SamplerValues[slot] );
}

const tTJSVariant* tTJSNI_GpuShaderProgram::GetAttributeValue( const std::string& name ) const
{
	for( auto& a : AttributeValues ) if( a.name == name ) return &a.value;
	return nullptr;
}

//---------------------------------------------------------------------------
// tTJSNC_GpuShaderProgram
//---------------------------------------------------------------------------
tjs_uint32 tTJSNC_GpuShaderProgram::ClassID = -1;
tTJSNC_GpuShaderProgram::tTJSNC_GpuShaderProgram() : tTJSNativeClass( TJS_W("ShaderProgram") )
{
	TJS_BEGIN_NATIVE_MEMBERS( ShaderProgram )
	TJS_DECL_EMPTY_FINALIZE_METHOD
	TJS_BEGIN_NATIVE_CONSTRUCTOR_DECL( _this, tTJSNI_GpuShaderProgram, ShaderProgram ) { return TJS_S_OK; }
	TJS_END_NATIVE_CONSTRUCTOR_DECL( ShaderProgram )
	TJS_END_NATIVE_MEMBERS
}

//! プロパティ代入を横取りしてシェーダーのパラメータにも反映する (値はプロパティとしても残る)
class tTJSGpuShaderProgramObject : public tTJSCustomObject {
	static void Forward( iTJSDispatch2* objthis, const ttstr& name, const tTJSVariant* param ) {
		tTJSNI_GpuShaderProgram* ni = nullptr;
		if( objthis && TJS_SUCCEEDED( objthis->NativeInstanceSupport( TJS_NIS_GETINSTANCE, tTJSNC_GpuShaderProgram::ClassID, (iTJSNativeInstance**)&ni ) ) && ni )
			ni->SetShaderParam( name, param );
	}
public:
	tjs_error TJS_INTF_METHOD PropSet( tjs_uint32 flag, const tjs_char* membername, tjs_uint32* hint, const tTJSVariant* param, iTJSDispatch2* objthis ) override {
		if( membername && param ) Forward( objthis, ttstr( membername ), param );
		return tTJSCustomObject::PropSet( flag, membername, hint, param, objthis );
	}
	tjs_error TJS_INTF_METHOD PropSetByVS( tjs_uint32 flag, tTJSVariantString* membername, const tTJSVariant* param, iTJSDispatch2* objthis ) override {
		if( membername && param ) Forward( objthis, ttstr( membername ), param );
		return tTJSCustomObject::PropSetByVS( flag, membername, param, objthis );
	}
};

iTJSDispatch2* tTJSNC_GpuShaderProgram::CreateBaseTJSObject() { return new tTJSGpuShaderProgramObject(); }
tTJSNativeClass* TVPCreateNativeClass_GpuShaderProgram() { return new tTJSNC_GpuShaderProgram(); }
