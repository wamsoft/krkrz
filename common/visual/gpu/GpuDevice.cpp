#include "tjsCommHead.h"

#include "GpuDevice.h"
#include "MsgIntf.h"
#include "LogIntf.h"

#include <sstream>

namespace TVPGpu {

//---------------------------------------------------------------------------
// 共有デバイス
//---------------------------------------------------------------------------
static Device* SharedDevice = nullptr;
static bool SharedDeviceFailed = false;

Device* Device::Get()
{
	if( SharedDevice ) return SharedDevice;
	if( SharedDeviceFailed )
		TVPThrowExceptionMessage( TJS_W("GPU device is not available") );
	SharedDevice = CreatePlatformDevice();
	if( !SharedDevice ) {
		SharedDeviceFailed = true;
		TVPThrowExceptionMessage( TJS_W("GPU device is not available") );
	}
	return SharedDevice;
}
Device* Device::GetIfCreated()
{
	return SharedDevice;
}
void Device::Shutdown()
{
	delete SharedDevice;
	SharedDevice = nullptr;
}

//---------------------------------------------------------------------------
// リフレクション
//---------------------------------------------------------------------------
UniformType ParseUniformType( const std::string& t )
{
	static const struct { const char* name; UniformType type; } table[] = {
		{ "float", UniformType::Float }, { "vec2", UniformType::Vec2 }, { "vec3", UniformType::Vec3 }, { "vec4", UniformType::Vec4 },
		{ "int", UniformType::Int }, { "ivec2", UniformType::IVec2 }, { "ivec3", UniformType::IVec3 }, { "ivec4", UniformType::IVec4 },
		{ "uint", UniformType::UInt }, { "uvec2", UniformType::UVec2 }, { "uvec3", UniformType::UVec3 }, { "uvec4", UniformType::UVec4 },
		{ "bool", UniformType::Bool }, { "bvec2", UniformType::BVec2 }, { "bvec3", UniformType::BVec3 }, { "bvec4", UniformType::BVec4 },
		{ "mat2", UniformType::Mat2 }, { "mat3", UniformType::Mat3 }, { "mat4", UniformType::Mat4 },
		{ "mat2x2", UniformType::Mat2 }, { "mat3x3", UniformType::Mat3 }, { "mat4x4", UniformType::Mat4 },
		{ "mat2x3", UniformType::Mat2x3 }, { "mat2x4", UniformType::Mat2x4 }, { "mat3x2", UniformType::Mat3x2 },
		{ "mat3x4", UniformType::Mat3x4 }, { "mat4x2", UniformType::Mat4x2 }, { "mat4x3", UniformType::Mat4x3 },
	};
	for( auto& e : table ) if( t == e.name ) return e.type;
	return UniformType::Unknown;
}
//---------------------------------------------------------------------------
uint32_t UniformComponentCount( UniformType t )
{
	switch( t ) {
	case UniformType::Float: case UniformType::Int: case UniformType::UInt: case UniformType::Bool: return 1;
	case UniformType::Vec2: case UniformType::IVec2: case UniformType::UVec2: case UniformType::BVec2: return 2;
	case UniformType::Vec3: case UniformType::IVec3: case UniformType::UVec3: case UniformType::BVec3: return 3;
	case UniformType::Vec4: case UniformType::IVec4: case UniformType::UVec4: case UniformType::BVec4: return 4;
	case UniformType::Mat2: return 4;
	case UniformType::Mat3: return 9;
	case UniformType::Mat4: return 16;
	case UniformType::Mat2x3: case UniformType::Mat3x2: return 6;
	case UniformType::Mat2x4: case UniformType::Mat4x2: return 8;
	case UniformType::Mat3x4: case UniformType::Mat4x3: return 12;
	default: return 0;
	}
}
//---------------------------------------------------------------------------
// 形式 (gpushaderc.py の reflection_text):
//   B <v|f> <block_size>
//   U <v|f> <name> <offset> <type> <array_count> <array_stride> <matrix_stride>
//   S <v|f> <name> <binding> <type>
//   A <name> <location> <type>
//---------------------------------------------------------------------------
bool ShaderReflection::Parse( const char* text, size_t len, std::string* error )
{
	uniforms.clear(); samplers.clear(); attributes.clear();
	blockSize[0] = blockSize[1] = 0;
	samplerCount[0] = samplerCount[1] = 0;
	std::istringstream in( std::string( text, len ) );
	std::string line;
	auto stageOf = []( const std::string& s, ShaderStage& st ) {
		if( s == "v" ) { st = ShaderStage::Vertex; return true; }
		if( s == "f" ) { st = ShaderStage::Fragment; return true; }
		return false;
	};
	while( std::getline( in, line ) ) {
		if( line.empty() ) continue;
		std::istringstream ls( line );
		std::string tag;
		ls >> tag;
		if( tag == "B" ) {
			std::string s; uint32_t size = 0; ShaderStage st;
			ls >> s >> size;
			if( !stageOf( s, st ) ) goto bad;
			blockSize[(int)st] = size;
		} else if( tag == "U" ) {
			Uniform u; std::string s, type;
			ls >> s >> u.name >> u.offset >> type >> u.arrayCount >> u.arrayStride >> u.matrixStride;
			if( !stageOf( s, u.stage ) || ls.fail() ) goto bad;
			u.type = ParseUniformType( type );
			if( u.type == UniformType::Unknown ) {
				if( error ) *error = "unsupported uniform type: " + type;
				return false;
			}
			uniforms.push_back( u );
		} else if( tag == "S" ) {
			Sampler sm; std::string s, type;
			ls >> s >> sm.name >> sm.slot >> type;
			if( !stageOf( s, sm.stage ) || ls.fail() ) goto bad;
			samplers.push_back( sm );
			if( sm.slot + 1 > samplerCount[(int)sm.stage] ) samplerCount[(int)sm.stage] = sm.slot + 1;
		} else if( tag == "A" ) {
			Attribute a;
			ls >> a.name >> a.location >> a.type;
			if( ls.fail() ) goto bad;
			attributes.push_back( a );
		} else {
			goto bad;
		}
		continue;
	bad:
		if( error ) *error = "bad line: " + line;
		return false;
	}
	return true;
}
//---------------------------------------------------------------------------
const ShaderReflection::Attribute* ShaderReflection::FindAttribute( const std::string& name ) const
{
	for( auto& a : attributes ) if( a.name == name ) return &a;
	return nullptr;
}
//---------------------------------------------------------------------------
const ShaderReflection::Sampler* ShaderReflection::FindSampler( const std::string& name ) const
{
	const Sampler* found = nullptr;
	for( auto& s : samplers ) {
		if( s.name != name ) continue;
		if( s.stage == ShaderStage::Fragment ) return &s;
		found = &s;
	}
	return found;
}

} // namespace TVPGpu
