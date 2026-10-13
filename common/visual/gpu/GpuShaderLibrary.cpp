#include "tjsCommHead.h"

#include "GpuShaderLibrary.h"
#include "StorageIntf.h"
#include "UtilStreams.h"
#include "SysInitIntf.h"
#include "DebugIntf.h"
#include "LogIntf.h"
#include "CharacterSet.h"
#include "GpuBuiltinShaders.inc"
#include "GpuBuiltinShadersDXIL.inc"   // D3D12 用 (gpushaderc.py builtin --target dxil)

#include <cstdio>

namespace TVPGpu {

//---------------------------------------------------------------------------
std::string NormalizeShaderText( const std::string& src )
{
	std::string s;
	s.reserve( src.size() );
	size_t i = 0;
	if( src.size() >= 3 && (uint8_t)src[0] == 0xEF && (uint8_t)src[1] == 0xBB && (uint8_t)src[2] == 0xBF )
		i = 3;
	for( ; i < src.size(); i++ ) {
		if( src[i] == '\r' && i + 1 < src.size() && src[i + 1] == '\n' ) continue;
		s.push_back( src[i] );
	}
	return s;
}
//---------------------------------------------------------------------------
uint64_t ShaderProgramKey( const std::string& vsUtf8, const std::string& fsUtf8 )
{
	uint64_t h = 0xCBF29CE484222325ULL;
	auto feed = [&h]( const std::string& s ) {
		for( unsigned char c : s ) {
			h ^= c;
			h *= 0x100000001B3ULL;
		}
	};
	feed( NormalizeShaderText( vsUtf8 ) );
	h ^= 0; h *= 0x100000001B3ULL;   // 区切りの "\0"
	feed( NormalizeShaderText( fsUtf8 ) );
	return h;
}
//---------------------------------------------------------------------------
ShaderLibrary& ShaderLibrary::Instance()
{
	static ShaderLibrary lib;
	static bool builtinLoaded = false;
	if( !builtinLoaded ) {
		builtinLoaded = true;
		lib.AddPack( TVPGpuBuiltinShaderPack, TVPGpuBuiltinShaderPackSize, "builtin" );
		lib.AddPack( TVPGpuBuiltinShaderPackDXIL, TVPGpuBuiltinShaderPackDXILSize, "builtin (dxil)" );
	}
	return lib;
}
//---------------------------------------------------------------------------
static bool ReadU32( const uint8_t*& p, const uint8_t* end, uint32_t& v )
{
	if( end - p < 4 ) return false;
	v = (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
	p += 4;
	return true;
}
//---------------------------------------------------------------------------
bool ShaderLibrary::AddPack( const uint8_t* data, size_t size, const char* label )
{
	const uint8_t* p = data;
	const uint8_t* end = data + size;
	if( size < 12 || memcmp( p, "KZGS", 4 ) != 0 ) {
		TVPLOG_ERROR( "GPU shader pack {}: not a shader pack", label );
		return false;
	}
	p += 4;
	uint32_t version = 0, format = 1, count = 0;
	ReadU32( p, end, version );
	if( version == 2 ) ReadU32( p, end, format );   // v1 は SPIR-V のみ
	ReadU32( p, end, count );
	if( version != 1 && version != 2 ) {
		TVPLOG_ERROR( "GPU shader pack {}: unsupported version {}", label, version );
		return false;
	}
	if( format != (uint32_t)ShaderFormat::Spirv && format != (uint32_t)ShaderFormat::Private &&
		format != (uint32_t)ShaderFormat::Dxil ) {
		TVPLOG_ERROR( "GPU shader pack {}: unknown shader format {}", label, format );
		return false;
	}
	for( uint32_t i = 0; i < count; i++ ) {
		uint32_t lo, hi;
		if( !ReadU32( p, end, lo ) || !ReadU32( p, end, hi ) ) goto broken;
		{
			uint64_t key = (uint64_t)lo | ((uint64_t)hi << 32);
			Entry e;
			e.format = (ShaderFormat)format;
			uint32_t n;
			if( !ReadU32( p, end, n ) || (size_t)(end - p) < n ) goto broken;
			e.vs = p; e.vsSize = n; p += n;
			if( !ReadU32( p, end, n ) || (size_t)(end - p) < n ) goto broken;
			e.fs = p; e.fsSize = n; p += n;
			if( !ReadU32( p, end, n ) || (size_t)(end - p) < n ) goto broken;
			e.refl = (const char*)p; e.reflSize = n; p += n;
			std::vector<Entry>& v = Entries[key];
			bool replaced = false;
			for( Entry& old : v ) if( old.format == e.format ) { old = e; replaced = true; }
			if( !replaced ) v.push_back( e );
		}
	}
	TVPLOG_INFO( "GPU shader pack {}: {} programs (format {})", label, count, format );
	return true;
broken:
	TVPLOG_ERROR( "GPU shader pack {}: broken", label );
	return false;
}
//---------------------------------------------------------------------------
const char* ShaderLibrary::BuiltinSource( const char* program, ShaderStage stage )
{
	static const struct { const char* name; const char* vs; const char* fs; } table[] = {
		{ "default",              TVPGpuBuiltinVS_default,              TVPGpuBuiltinFS_default },
		{ "fill",                 TVPGpuBuiltinVS_fill,                 TVPGpuBuiltinFS_fill },
		{ "transition_crossfade", TVPGpuBuiltinVS_transition_crossfade, TVPGpuBuiltinFS_transition_crossfade },
		{ "transition_universal", TVPGpuBuiltinVS_transition_universal, TVPGpuBuiltinFS_transition_universal },
		{ "clip_stencil",         TVPGpuBuiltinVS_clip_stencil,         TVPGpuBuiltinFS_clip_stencil },
		{ "clip_mask",            TVPGpuBuiltinVS_clip_mask,            TVPGpuBuiltinFS_clip_mask },
		{ "effect_pointwise",     TVPGpuBuiltinVS_effect_pointwise,     TVPGpuBuiltinFS_effect_pointwise },
		{ "effect_lut",           TVPGpuBuiltinVS_effect_lut,           TVPGpuBuiltinFS_effect_lut },
		{ "effect_blur",          TVPGpuBuiltinVS_effect_blur,          TVPGpuBuiltinFS_effect_blur },
	};
	for( auto& e : table ) {
		if( strcmp( e.name, program ) == 0 ) return stage == ShaderStage::Vertex ? e.vs : e.fs;
	}
	return nullptr;
}
//---------------------------------------------------------------------------
bool ShaderLibrary::Find( uint64_t key, ProgramBinary& out ) const
{
	auto it = Entries.find( key );
	if( it == Entries.end() ) return false;
	Device* dev = Device::Get();
	const Entry* found = nullptr;
	// 機種専用形式があればそれを優先する
	for( const Entry& c : it->second )
		if( c.format == ShaderFormat::Private && dev->SupportsShaderFormat( c.format ) ) found = &c;
	if( !found )
		for( const Entry& c : it->second )
			if( dev->SupportsShaderFormat( c.format ) ) { found = &c; break; }
	if( !found ) return false;
	const Entry& e = *found;
	out.format = e.format;
	out.vertexCode.assign( e.vs, e.vs + e.vsSize );
	out.fragmentCode.assign( e.fs, e.fs + e.fsSize );
	std::string err;
	if( !out.reflection.Parse( e.refl, e.reflSize, &err ) ) {
		TVPLOG_ERROR( "GPU shader {:016x}: broken reflection: {}", key, err );
		return false;
	}
	return true;
}
//---------------------------------------------------------------------------
// ユーザーのパック:
//   -gpushaderpack=<storage>[;<storage>...]  明示指定
//   既定: canvas_shaders.kzgs がストレージ (データ) にあれば読む。無ければ
//         exe と同じフォルダのものを読む (ビルドが exe の隣に置く場合)
//---------------------------------------------------------------------------
void ShaderLibrary::LoadUserPacks()
{
	if( UserPacksLoaded ) return;
	UserPacksLoaded = true;

	std::vector<ttstr> names;
	tTJSVariant val;
	if( TVPGetCommandLine( TJS_W("-gpushaderpack"), &val ) ) {
		ttstr s( val );
		const tjs_char* p = s.c_str();
		ttstr cur;
		for( ; *p; p++ ) {
			if( *p == TJS_W(';') ) { if( !cur.IsEmpty() ) names.push_back( cur ); cur.Clear(); }
			else cur += *p;
		}
		if( !cur.IsEmpty() ) names.push_back( cur );
	} else {
		static const ttstr def( TJS_W("canvas_shaders.kzgs") );
		if( TVPIsExistentStorage( def ) ) names.push_back( def );
		else {
			ttstr besideExe = TVPGetAppPath() + def;
			if( TVPIsExistentStorage( besideExe ) ) names.push_back( besideExe );
		}
	}
	for( const ttstr& name : names ) {
		try {
			ttstr placed = TVPGetPlacedPath( name );
			tTVPStreamHolder st( placed.IsEmpty() ? name : placed, TJS_BS_READ );
			tjs_uint64 size = st->GetSize();
			std::vector<uint8_t> buf( (size_t)size );
			if( size && st->Read( buf.data(), (tjs_uint)size ) != size ) {
				TVPAddLog( ttstr( TJS_W("GPU shader pack: read error: ") ) + name );
				continue;
			}
			std::string label;
			TVPUtf16ToUtf8( label, name.AsStdString() );
			OwnedPacks.push_back( std::move( buf ) );
			AddPack( OwnedPacks.back().data(), OwnedPacks.back().size(), label.c_str() );
		} catch( ... ) {
			TVPAddLog( ttstr( TJS_W("GPU shader pack: cannot open: ") ) + name );
		}
	}
}
//---------------------------------------------------------------------------
// 見つからなかったソースの収集: -gpushadercollect=<フォルダ>
//   <key>.vs / <key>.fs を書き出す。gpushaderc.py pack <フォルダ> でパックにできる
//---------------------------------------------------------------------------
std::string ShaderLibrary::CollectMissing( uint64_t key, const std::string& vs, const std::string& fs )
{
	tTJSVariant val;
	if( !TVPGetCommandLine( TJS_W("-gpushadercollect"), &val ) ) return std::string();
	ttstr dir( val );
	if( dir.IsEmpty() ) return std::string();
	if( dir.GetLastChar() != TJS_W('/') && dir.GetLastChar() != TJS_W('\\') ) dir += TJS_W("/");
	dir = TVPNormalizeStorageName( dir );
	TVPCreateFolders( TVPGetLocallyAccessibleName( dir ) );
	char keyname[32];
	snprintf( keyname, sizeof(keyname), "%016llx", (unsigned long long)key );
	ttstr base = dir + ttstr( keyname );
	try {
		{
			tTVPStreamHolder st( base + TJS_W(".vs"), TJS_BS_WRITE );
			st->Write( vs.data(), (tjs_uint)vs.size() );
		}
		{
			tTVPStreamHolder st( base + TJS_W(".fs"), TJS_BS_WRITE );
			st->Write( fs.data(), (tjs_uint)fs.size() );
		}
	} catch( ... ) {
		return std::string();
	}
	std::string out;
	TVPUtf16ToUtf8( out, base.AsStdString() );
	return out;
}

} // namespace TVPGpu
