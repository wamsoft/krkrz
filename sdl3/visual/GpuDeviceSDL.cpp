//---------------------------------------------------------------------------
// Canvas (GPU 版) の描画デバイス: SDL_GPU バックエンド
//
// 描画は「区間」(segment) 単位で記録してからまとめて GPU に流す。区間は
// 1 つの描画先に対する一続きの描画で、次のときに閉じる (= 実際にレンダーパスを
// 組んで記録を流す):
//   - 描画先が変わる / EndPass
//   - テクスチャ・バッファの更新、コピー、読み戻し (レンダーパスの外でしか
//     できないので、順序を保つためにそこで区切る)
//   - Submit
// 区切ったあとの続きは描画先を LOAD で読み込んで再開する。
//---------------------------------------------------------------------------
#include "tjsCommHead.h"

#include "GpuDeviceSDL.h"
#include "LogIntf.h"
#include "MsgIntf.h"
#include "SysInitIntf.h"
#include "CharacterSet.h"

#include <SDL3/SDL.h>
#include <unordered_map>
#include <algorithm>

namespace TVPGpu {

namespace {

//---------------------------------------------------------------------------
// 変換表
//---------------------------------------------------------------------------
SDL_GPUVertexElementFormat ToSDL( VertexFormat f )
{
	switch( f ) {
	case VertexFormat::Float:       return SDL_GPU_VERTEXELEMENTFORMAT_FLOAT;
	case VertexFormat::Float2:      return SDL_GPU_VERTEXELEMENTFORMAT_FLOAT2;
	case VertexFormat::Float3:      return SDL_GPU_VERTEXELEMENTFORMAT_FLOAT3;
	case VertexFormat::Float4:      return SDL_GPU_VERTEXELEMENTFORMAT_FLOAT4;
	case VertexFormat::Int:         return SDL_GPU_VERTEXELEMENTFORMAT_INT;
	case VertexFormat::Int2:        return SDL_GPU_VERTEXELEMENTFORMAT_INT2;
	case VertexFormat::Int3:        return SDL_GPU_VERTEXELEMENTFORMAT_INT3;
	case VertexFormat::Int4:        return SDL_GPU_VERTEXELEMENTFORMAT_INT4;
	case VertexFormat::UInt:        return SDL_GPU_VERTEXELEMENTFORMAT_UINT;
	case VertexFormat::UInt2:       return SDL_GPU_VERTEXELEMENTFORMAT_UINT2;
	case VertexFormat::UInt3:       return SDL_GPU_VERTEXELEMENTFORMAT_UINT3;
	case VertexFormat::UInt4:       return SDL_GPU_VERTEXELEMENTFORMAT_UINT4;
	case VertexFormat::Byte2:       return SDL_GPU_VERTEXELEMENTFORMAT_BYTE2;
	case VertexFormat::Byte4:       return SDL_GPU_VERTEXELEMENTFORMAT_BYTE4;
	case VertexFormat::UByte2:      return SDL_GPU_VERTEXELEMENTFORMAT_UBYTE2;
	case VertexFormat::UByte4:      return SDL_GPU_VERTEXELEMENTFORMAT_UBYTE4;
	case VertexFormat::Byte2Norm:   return SDL_GPU_VERTEXELEMENTFORMAT_BYTE2_NORM;
	case VertexFormat::Byte4Norm:   return SDL_GPU_VERTEXELEMENTFORMAT_BYTE4_NORM;
	case VertexFormat::UByte2Norm:  return SDL_GPU_VERTEXELEMENTFORMAT_UBYTE2_NORM;
	case VertexFormat::UByte4Norm:  return SDL_GPU_VERTEXELEMENTFORMAT_UBYTE4_NORM;
	case VertexFormat::Short2:      return SDL_GPU_VERTEXELEMENTFORMAT_SHORT2;
	case VertexFormat::Short4:      return SDL_GPU_VERTEXELEMENTFORMAT_SHORT4;
	case VertexFormat::UShort2:     return SDL_GPU_VERTEXELEMENTFORMAT_USHORT2;
	case VertexFormat::UShort4:     return SDL_GPU_VERTEXELEMENTFORMAT_USHORT4;
	case VertexFormat::Short2Norm:  return SDL_GPU_VERTEXELEMENTFORMAT_SHORT2_NORM;
	case VertexFormat::Short4Norm:  return SDL_GPU_VERTEXELEMENTFORMAT_SHORT4_NORM;
	case VertexFormat::UShort2Norm: return SDL_GPU_VERTEXELEMENTFORMAT_USHORT2_NORM;
	case VertexFormat::UShort4Norm: return SDL_GPU_VERTEXELEMENTFORMAT_USHORT4_NORM;
	default:                        return SDL_GPU_VERTEXELEMENTFORMAT_INVALID;
	}
}
SDL_GPUBlendFactor ToSDL( BlendFactor f )
{
	switch( f ) {
	case BlendFactor::Zero:             return SDL_GPU_BLENDFACTOR_ZERO;
	case BlendFactor::One:              return SDL_GPU_BLENDFACTOR_ONE;
	case BlendFactor::SrcColor:         return SDL_GPU_BLENDFACTOR_SRC_COLOR;
	case BlendFactor::OneMinusSrcColor: return SDL_GPU_BLENDFACTOR_ONE_MINUS_SRC_COLOR;
	case BlendFactor::DstColor:         return SDL_GPU_BLENDFACTOR_DST_COLOR;
	case BlendFactor::OneMinusDstColor: return SDL_GPU_BLENDFACTOR_ONE_MINUS_DST_COLOR;
	case BlendFactor::SrcAlpha:         return SDL_GPU_BLENDFACTOR_SRC_ALPHA;
	case BlendFactor::OneMinusSrcAlpha: return SDL_GPU_BLENDFACTOR_ONE_MINUS_SRC_ALPHA;
	case BlendFactor::DstAlpha:         return SDL_GPU_BLENDFACTOR_DST_ALPHA;
	case BlendFactor::OneMinusDstAlpha: return SDL_GPU_BLENDFACTOR_ONE_MINUS_DST_ALPHA;
	}
	return SDL_GPU_BLENDFACTOR_ONE;
}
SDL_GPUBlendOp ToSDL( BlendOp o )
{
	switch( o ) {
	case BlendOp::Add:             return SDL_GPU_BLENDOP_ADD;
	case BlendOp::Subtract:        return SDL_GPU_BLENDOP_SUBTRACT;
	case BlendOp::ReverseSubtract: return SDL_GPU_BLENDOP_REVERSE_SUBTRACT;
	case BlendOp::Min:             return SDL_GPU_BLENDOP_MIN;
	case BlendOp::Max:             return SDL_GPU_BLENDOP_MAX;
	}
	return SDL_GPU_BLENDOP_ADD;
}
SDL_GPUCompareOp ToSDL( CompareOp o )
{
	switch( o ) {
	case CompareOp::Never:          return SDL_GPU_COMPAREOP_NEVER;
	case CompareOp::Less:           return SDL_GPU_COMPAREOP_LESS;
	case CompareOp::Equal:          return SDL_GPU_COMPAREOP_EQUAL;
	case CompareOp::LessOrEqual:    return SDL_GPU_COMPAREOP_LESS_OR_EQUAL;
	case CompareOp::Greater:        return SDL_GPU_COMPAREOP_GREATER;
	case CompareOp::NotEqual:       return SDL_GPU_COMPAREOP_NOT_EQUAL;
	case CompareOp::GreaterOrEqual: return SDL_GPU_COMPAREOP_GREATER_OR_EQUAL;
	case CompareOp::Always:         return SDL_GPU_COMPAREOP_ALWAYS;
	}
	return SDL_GPU_COMPAREOP_ALWAYS;
}
SDL_GPUStencilOp ToSDL( StencilOp o )
{
	switch( o ) {
	case StencilOp::Keep:           return SDL_GPU_STENCILOP_KEEP;
	case StencilOp::Zero:           return SDL_GPU_STENCILOP_ZERO;
	case StencilOp::Replace:        return SDL_GPU_STENCILOP_REPLACE;
	case StencilOp::IncrementClamp: return SDL_GPU_STENCILOP_INCREMENT_AND_CLAMP;
	case StencilOp::DecrementClamp: return SDL_GPU_STENCILOP_DECREMENT_AND_CLAMP;
	case StencilOp::Invert:         return SDL_GPU_STENCILOP_INVERT;
	case StencilOp::IncrementWrap:  return SDL_GPU_STENCILOP_INCREMENT_AND_WRAP;
	case StencilOp::DecrementWrap:  return SDL_GPU_STENCILOP_DECREMENT_AND_WRAP;
	}
	return SDL_GPU_STENCILOP_KEEP;
}
SDL_GPUPrimitiveType ToSDL( PrimitiveType p )
{
	switch( p ) {
	case PrimitiveType::TriangleList:  return SDL_GPU_PRIMITIVETYPE_TRIANGLELIST;
	case PrimitiveType::TriangleStrip: return SDL_GPU_PRIMITIVETYPE_TRIANGLESTRIP;
	case PrimitiveType::LineList:      return SDL_GPU_PRIMITIVETYPE_LINELIST;
	case PrimitiveType::LineStrip:     return SDL_GPU_PRIMITIVETYPE_LINESTRIP;
	case PrimitiveType::PointList:     return SDL_GPU_PRIMITIVETYPE_POINTLIST;
	}
	return SDL_GPU_PRIMITIVETYPE_TRIANGLELIST;
}
SDL_GPUSamplerAddressMode ToSDL( WrapMode w )
{
	switch( w ) {
	case WrapMode::Repeat:         return SDL_GPU_SAMPLERADDRESSMODE_REPEAT;
	case WrapMode::ClampToEdge:    return SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE;
	case WrapMode::MirroredRepeat: return SDL_GPU_SAMPLERADDRESSMODE_MIRRORED_REPEAT;
	}
	return SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE;
}

//---------------------------------------------------------------------------
// FNV-1a で状態をハッシュする
//---------------------------------------------------------------------------
struct Hasher {
	uint64_t h = 0xCBF29CE484222325ULL;
	void Bytes( const void* p, size_t n ) {
		const uint8_t* b = (const uint8_t*)p;
		for( size_t i = 0; i < n; i++ ) { h ^= b[i]; h *= 0x100000001B3ULL; }
	}
	template<typename T> void Value( const T& v ) { Bytes( &v, sizeof(v) ); }
};

//---------------------------------------------------------------------------
// リソース
//---------------------------------------------------------------------------
// 描画は記録して区間の終わりにまとめて積むので、記録した描画が使うリソースを
// 先に手放すと積むときに解放済みを触る。手放す前に記録分を積ませる (SDL 側は
// 積んだコマンドが終わるまで実体の解放を待つ)
static void FlushBeforeRelease( const void* res );

class SDLTexture : public Texture {
public:
	SDL_GPUDevice* Dev;
	SDL_GPUTexture* Tex;
	SDL_GPUTextureFormat GpuFormat;
	bool SwapRB;       // BGRA8 を RGBA8 で持っている (BGRA8 非対応機種)
	bool ExpandAlpha;  // A8 を 4ch で持っている (A8 非対応機種)
	SDLTexture( SDL_GPUDevice* dev, SDL_GPUTexture* tex, const TextureDesc& d, SDL_GPUTextureFormat f, bool swap, bool expand )
		: Dev( dev ), Tex( tex ), GpuFormat( f ), SwapRB( swap ), ExpandAlpha( expand ) { Desc = d; }
	~SDLTexture() override { FlushBeforeRelease( this ); if( Tex ) SDL_ReleaseGPUTexture( Dev, Tex ); }
	void* NativeHandle() const override { return Tex; }
};

class SDLBuffer : public Buffer {
public:
	SDL_GPUDevice* Dev;
	SDL_GPUBuffer* Buf;
	bool Owned;
	SDLBuffer( SDL_GPUDevice* dev, SDL_GPUBuffer* b, uint32_t size, bool index, bool owned )
		: Dev( dev ), Buf( b ), Owned( owned ) { BufSize = size; Index = index; }
	~SDLBuffer() override { if( Owned && Buf ) { FlushBeforeRelease( this ); SDL_ReleaseGPUBuffer( Dev, Buf ); } }
	void* NativeHandle() const override { return Buf; }
};

class SDLProgram : public Program {
public:
	SDL_GPUDevice* Dev;
	SDL_GPUShader* VS = nullptr;
	SDL_GPUShader* FS = nullptr;
	uint64_t Id;
	SDLProgram( SDL_GPUDevice* dev, uint64_t id, const ShaderReflection& r ) : Dev( dev ), Id( id ) { Refl = r; }
	~SDLProgram() override {
		FlushBeforeRelease( this );
		if( VS ) SDL_ReleaseGPUShader( Dev, VS );
		if( FS ) SDL_ReleaseGPUShader( Dev, FS );
	}
};

//---------------------------------------------------------------------------
// 記録した描画
//---------------------------------------------------------------------------
struct RecordedDraw {
	DrawCall call;
	uint32_t uniformOffset[2];   // UniformStorage 内の位置
	bool transientVB[MaxVertexBuffers];
	bool transientIB;
	Rect viewport;
	bool scissor;
	Rect scissorRect;
};

//---------------------------------------------------------------------------
class SDLDevice : public Device {
public:
	SDL_GPUDevice* Dev;
	bool OwnDevice;
	bool Spirv;
	bool Private;
	bool Dxil;
	bool HasBGRA;
	bool HasA8;

	SDL_GPUCommandBuffer* Cmd = nullptr;

	// 現在の描画先と記録中の区間
	SDLTexture* Target = nullptr;
	SDLTexture* DepthStencil = nullptr;
	bool PendingClear = false;
	float ClearRGBA[4] = { 0, 0, 0, 0 };
	bool PendingStencilClear = false;
	uint8_t StencilClearValue = 0;
	Rect Viewport = { 0, 0, 0, 0 };
	bool ScissorOn = false;
	Rect ScissorRect = { 0, 0, 0, 0 };
	std::vector<RecordedDraw> Draws;
	std::vector<uint8_t> UniformStorage;

	// 一時頂点・インデックスデータ (区間ごとに GPU へ上げる)
	std::vector<uint8_t> TransientVB, TransientIB;
	SDLBuffer* TransientVBHandle = nullptr;   // 区間ごとに作り直すダミーのハンドル
	SDLBuffer* TransientIBHandle = nullptr;
	SDL_GPUBuffer* TransientVBGpu = nullptr; uint32_t TransientVBGpuSize = 0;
	SDL_GPUBuffer* TransientIBGpu = nullptr; uint32_t TransientIBGpuSize = 0;
	SDL_GPUTransferBuffer* TransientTB = nullptr; uint32_t TransientTBSize = 0;

	static SDLDevice* TheDevice;

	//! リソースを手放す前に呼ばれる (FlushBeforeRelease)
	void FlushForRelease( const void* res )
	{
		if( !Draws.empty() || PendingClear || PendingStencilClear ) CloseSegment();
		if( Target == res ) Target = nullptr;
		if( DepthStencil == res ) DepthStencil = nullptr;
	}

	std::unordered_map<uint64_t, SDL_GPUGraphicsPipeline*> Pipelines;
	std::unordered_map<uint64_t, SDL_GPUSampler*> Samplers;
	uint64_t NextProgramId = 1;

	SDLDevice( SDL_GPUDevice* dev, bool own ) : Dev( dev ), OwnDevice( own )
	{
		Spirv = ( SDL_GetGPUShaderFormats( Dev ) & SDL_GPU_SHADERFORMAT_SPIRV ) != 0;
		Private = ( SDL_GetGPUShaderFormats( Dev ) & SDL_GPU_SHADERFORMAT_PRIVATE ) != 0;
		Dxil = ( SDL_GetGPUShaderFormats( Dev ) & SDL_GPU_SHADERFORMAT_DXIL ) != 0;
		const SDL_GPUTextureUsageFlags rt = SDL_GPU_TEXTUREUSAGE_SAMPLER | SDL_GPU_TEXTUREUSAGE_COLOR_TARGET;
		HasBGRA = SDL_GPUTextureSupportsFormat( Dev, SDL_GPU_TEXTUREFORMAT_B8G8R8A8_UNORM, SDL_GPU_TEXTURETYPE_2D, rt );
		HasA8 = SDL_GPUTextureSupportsFormat( Dev, SDL_GPU_TEXTUREFORMAT_A8_UNORM, SDL_GPU_TEXTURETYPE_2D, SDL_GPU_TEXTUREUSAGE_SAMPLER );
		// 一時バッファのハンドル (中身の SDL_GPUBuffer は区間ごとに差し替える)
		TransientVBHandle = new SDLBuffer( Dev, nullptr, 0, false, false );
		TransientIBHandle = new SDLBuffer( Dev, nullptr, 0, true, false );
		TheDevice = this;
		TVPLOG_INFO( "GPU device: driver={} spirv={} private={} dxil={} bgra8={} a8={}",
			SDL_GetGPUDeviceDriver( Dev ), Spirv, Private, Dxil, HasBGRA, HasA8 );
	}
	~SDLDevice() override
	{
		TheDevice = nullptr;
		Submit();
		SDL_WaitForGPUIdle( Dev );
		for( auto& p : Pipelines ) SDL_ReleaseGPUGraphicsPipeline( Dev, p.second );
		for( auto& s : Samplers ) SDL_ReleaseGPUSampler( Dev, s.second );
		if( TransientVBGpu ) SDL_ReleaseGPUBuffer( Dev, TransientVBGpu );
		if( TransientIBGpu ) SDL_ReleaseGPUBuffer( Dev, TransientIBGpu );
		if( TransientTB ) SDL_ReleaseGPUTransferBuffer( Dev, TransientTB );
		delete TransientVBHandle;
		delete TransientIBHandle;
		if( OwnDevice ) SDL_DestroyGPUDevice( Dev );
	}

	void* NativeHandle() const override { return Dev; }
	bool SupportsShaderFormat( ShaderFormat f ) const override {
		return f == ShaderFormat::Spirv ? Spirv : f == ShaderFormat::Private ? Private :
		       f == ShaderFormat::Dxil ? Dxil : false;
	}
	bool SupportsRenderTarget( TextureFormat f ) const override {
		return f == TextureFormat::BGRA8 || f == TextureFormat::D32F_S8;
	}

	SDL_GPUCommandBuffer* GetCmd()
	{
		if( !Cmd ) {
			Cmd = SDL_AcquireGPUCommandBuffer( Dev );
			if( !Cmd ) TVPThrowExceptionMessage( TJS_W("GPU: cannot acquire command buffer: %1"), ttstr( SDL_GetError() ) );
		}
		return Cmd;
	}

	//-----------------------------------------------------------------------
	// テクスチャ
	//-----------------------------------------------------------------------
	std::unique_ptr<Texture> CreateTexture( const TextureDesc& desc ) override
	{
		SDL_GPUTextureCreateInfo ci = {};
		ci.type = SDL_GPU_TEXTURETYPE_2D;
		ci.width = desc.width;
		ci.height = desc.height;
		ci.layer_count_or_depth = 1;
		ci.num_levels = desc.mipLevels ? desc.mipLevels : 1;
		ci.sample_count = SDL_GPU_SAMPLECOUNT_1;
		bool swap = false, expand = false;
		switch( desc.format ) {
		case TextureFormat::BGRA8:
			ci.format = HasBGRA ? SDL_GPU_TEXTUREFORMAT_B8G8R8A8_UNORM : SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM;
			swap = !HasBGRA;
			ci.usage = SDL_GPU_TEXTUREUSAGE_SAMPLER;
			// ミップマップ生成には COLOR_TARGET が要る
			if( desc.renderTarget || ci.num_levels > 1 ) ci.usage |= SDL_GPU_TEXTUREUSAGE_COLOR_TARGET;
			break;
		case TextureFormat::A8:
			if( HasA8 && !desc.renderTarget && ci.num_levels == 1 ) {
				ci.format = SDL_GPU_TEXTUREFORMAT_A8_UNORM;
			} else {
				ci.format = HasBGRA ? SDL_GPU_TEXTUREFORMAT_B8G8R8A8_UNORM : SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM;
				expand = true;
			}
			ci.usage = SDL_GPU_TEXTUREUSAGE_SAMPLER;
			if( desc.renderTarget || ci.num_levels > 1 ) ci.usage |= SDL_GPU_TEXTUREUSAGE_COLOR_TARGET;
			break;
		case TextureFormat::D32F_S8:
			ci.format = SDL_GPU_TEXTUREFORMAT_D32_FLOAT_S8_UINT;
			ci.usage = SDL_GPU_TEXTUREUSAGE_DEPTH_STENCIL_TARGET;
			break;
		}
		SDL_GPUTexture* t = SDL_CreateGPUTexture( Dev, &ci );
		if( !t ) {
			ttstr size = ttstr( (tjs_int)desc.width ) + TJS_W("x") + ttstr( (tjs_int)desc.height );
			TVPThrowExceptionMessage( TJS_W("GPU: cannot create texture (%1): %2"), size, ttstr( SDL_GetError() ) );
		}
		return std::unique_ptr<Texture>( new SDLTexture( Dev, t, desc, ci.format, swap, expand ) );
	}

	void UploadTexture( Texture* tex, uint32_t level, uint32_t x, uint32_t y,
		uint32_t w, uint32_t h, const void* pixels, uint32_t pitch ) override
	{
		if( !w || !h ) return;
		SDLTexture* t = static_cast<SDLTexture*>( tex );
		CloseSegment();
		const bool fourCh = t->Format() == TextureFormat::BGRA8 || t->ExpandAlpha;
		const uint32_t bpp = fourCh ? 4 : 1;
		const uint32_t size = w * h * bpp;
		SDL_GPUTransferBufferCreateInfo tci = {};
		tci.usage = SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD;
		tci.size = size;
		SDL_GPUTransferBuffer* tb = SDL_CreateGPUTransferBuffer( Dev, &tci );
		if( !tb ) TVPThrowExceptionMessage( TJS_W("GPU: cannot create transfer buffer: %1"), ttstr( SDL_GetError() ) );
		uint8_t* dst = (uint8_t*)SDL_MapGPUTransferBuffer( Dev, tb, false );
		const uint8_t* src = (const uint8_t*)pixels;
		for( uint32_t row = 0; row < h; row++ ) {
			const uint8_t* s = src + (size_t)pitch * row;
			uint8_t* d = dst + (size_t)w * bpp * row;
			if( t->Format() == TextureFormat::A8 && t->ExpandAlpha ) {
				for( uint32_t i = 0; i < w; i++ ) { d[i*4+0] = 0; d[i*4+1] = 0; d[i*4+2] = 0; d[i*4+3] = s[i]; }
			} else if( t->SwapRB ) {
				for( uint32_t i = 0; i < w; i++ ) { d[i*4+0] = s[i*4+2]; d[i*4+1] = s[i*4+1]; d[i*4+2] = s[i*4+0]; d[i*4+3] = s[i*4+3]; }
			} else {
				memcpy( d, s, (size_t)w * bpp );
			}
		}
		SDL_UnmapGPUTransferBuffer( Dev, tb );

		SDL_GPUCopyPass* cp = SDL_BeginGPUCopyPass( GetCmd() );
		SDL_GPUTextureTransferInfo src_info = {};
		src_info.transfer_buffer = tb;
		src_info.pixels_per_row = w;
		src_info.rows_per_layer = h;
		SDL_GPUTextureRegion region = {};
		region.texture = t->Tex;
		region.mip_level = level;
		region.x = x; region.y = y; region.w = w; region.h = h; region.d = 1;
		SDL_UploadToGPUTexture( cp, &src_info, &region, false );
		SDL_EndGPUCopyPass( cp );
		SDL_ReleaseGPUTransferBuffer( Dev, tb );   // GPU 側の処理が終わるまで SDL が保持する
	}

	void CopyTexture( Texture* src, uint32_t sx, uint32_t sy,
		Texture* dst, uint32_t dx, uint32_t dy, uint32_t w, uint32_t h ) override
	{
		if( !w || !h ) return;
		CloseSegment();
		SDL_GPUCopyPass* cp = SDL_BeginGPUCopyPass( GetCmd() );
		SDL_GPUTextureLocation s = {}, d = {};
		s.texture = static_cast<SDLTexture*>( src )->Tex; s.x = sx; s.y = sy;
		d.texture = static_cast<SDLTexture*>( dst )->Tex; d.x = dx; d.y = dy;
		SDL_CopyGPUTextureToTexture( cp, &s, &d, w, h, 1, false );
		SDL_EndGPUCopyPass( cp );
	}

	void GenerateMipmaps( Texture* tex ) override
	{
		if( tex->GetDesc().mipLevels <= 1 ) return;
		CloseSegment();
		SDL_GenerateMipmapsForGPUTexture( GetCmd(), static_cast<SDLTexture*>( tex )->Tex );
	}

	bool ReadPixels( Texture* tex, uint32_t x, uint32_t y, uint32_t w, uint32_t h,
		void* dst, uint32_t pitch ) override
	{
		if( !w || !h ) return true;
		SDLTexture* t = static_cast<SDLTexture*>( tex );
		if( t->Format() != TextureFormat::BGRA8 ) return false;
		CloseSegment();
		const uint32_t size = w * h * 4;
		SDL_GPUTransferBufferCreateInfo tci = {};
		tci.usage = SDL_GPU_TRANSFERBUFFERUSAGE_DOWNLOAD;
		tci.size = size;
		SDL_GPUTransferBuffer* tb = SDL_CreateGPUTransferBuffer( Dev, &tci );
		if( !tb ) return false;
		SDL_GPUCopyPass* cp = SDL_BeginGPUCopyPass( GetCmd() );
		SDL_GPUTextureRegion region = {};
		region.texture = t->Tex;
		region.x = x; region.y = y; region.w = w; region.h = h; region.d = 1;
		SDL_GPUTextureTransferInfo dst_info = {};
		dst_info.transfer_buffer = tb;
		dst_info.pixels_per_row = w;
		dst_info.rows_per_layer = h;
		SDL_DownloadFromGPUTexture( cp, &region, &dst_info );
		SDL_EndGPUCopyPass( cp );
		SDL_GPUFence* fence = SDL_SubmitGPUCommandBufferAndAcquireFence( Cmd );
		Cmd = nullptr;
		bool ok = false;
		if( fence ) {
			SDL_WaitForGPUFences( Dev, true, &fence, 1 );
			SDL_ReleaseGPUFence( Dev, fence );
			const uint8_t* s = (const uint8_t*)SDL_MapGPUTransferBuffer( Dev, tb, false );
			if( s ) {
				for( uint32_t row = 0; row < h; row++ ) {
					const uint8_t* sr = s + (size_t)w * 4 * row;
					uint8_t* dr = (uint8_t*)dst + (size_t)pitch * row;
					if( t->SwapRB ) {
						for( uint32_t i = 0; i < w; i++ ) { dr[i*4+0] = sr[i*4+2]; dr[i*4+1] = sr[i*4+1]; dr[i*4+2] = sr[i*4+0]; dr[i*4+3] = sr[i*4+3]; }
					} else {
						memcpy( dr, sr, (size_t)w * 4 );
					}
				}
				SDL_UnmapGPUTransferBuffer( Dev, tb );
				ok = true;
			}
		}
		SDL_ReleaseGPUTransferBuffer( Dev, tb );
		return ok;
	}

	//-----------------------------------------------------------------------
	// バッファ
	//-----------------------------------------------------------------------
	std::unique_ptr<Buffer> CreateBuffer( bool index, uint32_t size ) override
	{
		SDL_GPUBufferCreateInfo ci = {};
		ci.usage = index ? SDL_GPU_BUFFERUSAGE_INDEX : SDL_GPU_BUFFERUSAGE_VERTEX;
		ci.size = size ? size : 4;
		SDL_GPUBuffer* b = SDL_CreateGPUBuffer( Dev, &ci );
		if( !b ) TVPThrowExceptionMessage( TJS_W("GPU: cannot create buffer: %1"), ttstr( SDL_GetError() ) );
		return std::unique_ptr<Buffer>( new SDLBuffer( Dev, b, ci.size, index, true ) );
	}

	void UpdateBuffer( Buffer* buf, uint32_t offset, const void* data, uint32_t size ) override
	{
		if( !size ) return;
		CloseSegment();
		SDL_GPUTransferBufferCreateInfo tci = {};
		tci.usage = SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD;
		tci.size = size;
		SDL_GPUTransferBuffer* tb = SDL_CreateGPUTransferBuffer( Dev, &tci );
		if( !tb ) TVPThrowExceptionMessage( TJS_W("GPU: cannot create transfer buffer: %1"), ttstr( SDL_GetError() ) );
		void* p = SDL_MapGPUTransferBuffer( Dev, tb, false );
		memcpy( p, data, size );
		SDL_UnmapGPUTransferBuffer( Dev, tb );
		SDL_GPUCopyPass* cp = SDL_BeginGPUCopyPass( GetCmd() );
		SDL_GPUTransferBufferLocation src = {};
		src.transfer_buffer = tb;
		SDL_GPUBufferRegion dst = {};
		dst.buffer = static_cast<SDLBuffer*>( buf )->Buf;
		dst.offset = offset;
		dst.size = size;
		SDL_UploadToGPUBuffer( cp, &src, &dst, false );
		SDL_EndGPUCopyPass( cp );
		SDL_ReleaseGPUTransferBuffer( Dev, tb );
	}

	void AllocTransient( bool index, const void* data, uint32_t size, Buffer** buf, uint32_t* offset ) override
	{
		std::vector<uint8_t>& v = index ? TransientIB : TransientVB;
		// 4 バイト境界にそろえる
		size_t off = ( v.size() + 3 ) & ~(size_t)3;
		v.resize( off + size );
		memcpy( v.data() + off, data, size );
		*buf = index ? TransientIBHandle : TransientVBHandle;
		*offset = (uint32_t)off;
	}

	//-----------------------------------------------------------------------
	// シェーダー
	//-----------------------------------------------------------------------
	std::unique_ptr<Program> CreateProgram( const ProgramBinary& bin, std::string* error ) override
	{
		if( !SupportsShaderFormat( bin.format ) ) {
			if( error ) *error = "GPU device does not accept this shader format";
			return nullptr;
		}
		std::unique_ptr<SDLProgram> p( new SDLProgram( Dev, NextProgramId++, bin.reflection ) );
		const ShaderReflection& r = bin.reflection;
		SDL_GPUShaderCreateInfo ci = {};
		ci.format = bin.format == ShaderFormat::Private ? SDL_GPU_SHADERFORMAT_PRIVATE :
		            bin.format == ShaderFormat::Dxil ? SDL_GPU_SHADERFORMAT_DXIL : SDL_GPU_SHADERFORMAT_SPIRV;
		ci.entrypoint = "main";

		ci.code = bin.vertexCode.data();
		ci.code_size = bin.vertexCode.size();
		ci.stage = SDL_GPU_SHADERSTAGE_VERTEX;
		ci.num_samplers = r.samplerCount[0];
		ci.num_uniform_buffers = r.blockSize[0] ? 1 : 0;
		p->VS = SDL_CreateGPUShader( Dev, &ci );
		if( !p->VS ) {
			if( error ) *error = std::string( "vertex shader: " ) + SDL_GetError();
			return nullptr;
		}
		ci.code = bin.fragmentCode.data();
		ci.code_size = bin.fragmentCode.size();
		ci.stage = SDL_GPU_SHADERSTAGE_FRAGMENT;
		ci.num_samplers = r.samplerCount[1];
		ci.num_uniform_buffers = r.blockSize[1] ? 1 : 0;
		p->FS = SDL_CreateGPUShader( Dev, &ci );
		if( !p->FS ) {
			if( error ) *error = std::string( "fragment shader: " ) + SDL_GetError();
			return nullptr;
		}
		return std::unique_ptr<Program>( p.release() );
	}

	//-----------------------------------------------------------------------
	// 描画
	//-----------------------------------------------------------------------
	void BeginPass( Texture* target, Texture* depthStencil ) override
	{
		if( Target != target || DepthStencil != depthStencil ) CloseSegment();
		Target = static_cast<SDLTexture*>( target );
		DepthStencil = static_cast<SDLTexture*>( depthStencil );
		Viewport = { 0, 0, (int32_t)target->Width(), (int32_t)target->Height() };
		ScissorOn = false;
	}

	void ClearColor( const float rgba[4] ) override
	{
		// 区間の途中なら、そこまでを流してから次の区間の頭でクリアする
		if( !Draws.empty() ) CloseSegment();
		PendingClear = true;
		memcpy( ClearRGBA, rgba, sizeof(ClearRGBA) );
	}

	void ClearStencil( uint8_t value ) override
	{
		if( !DepthStencil ) return;
		if( !Draws.empty() ) CloseSegment();
		PendingStencilClear = true;
		StencilClearValue = value;
	}

	void SetViewport( const Rect& r ) override { Viewport = r; }
	void SetScissor( const Rect* r ) override
	{
		ScissorOn = r != nullptr;
		if( r ) ScissorRect = *r;
	}

	void Draw( const DrawCall& call ) override
	{
		if( !Target ) return;
		RecordedDraw d;
		d.call = call;
		for( int s = 0; s < 2; s++ ) {
			d.uniformOffset[s] = 0;
			d.call.uniforms[s] = nullptr;
			if( call.uniformSize[s] && call.uniforms[s] ) {
				size_t off = ( UniformStorage.size() + 15 ) & ~(size_t)15;
				UniformStorage.resize( off + call.uniformSize[s] );
				memcpy( UniformStorage.data() + off, call.uniforms[s], call.uniformSize[s] );
				d.uniformOffset[s] = (uint32_t)off;
			}
		}
		for( uint32_t i = 0; i < MaxVertexBuffers; i++ )
			d.transientVB[i] = call.vertexBuffers[i] == TransientVBHandle;
		d.transientIB = call.indexBuffer == TransientIBHandle;
		d.viewport = Viewport;
		d.scissor = ScissorOn;
		d.scissorRect = ScissorRect;
		Draws.push_back( std::move( d ) );
	}

	void EndPass() override
	{
		CloseSegment();
		Target = nullptr;
		DepthStencil = nullptr;
	}

	void Submit() override
	{
		CloseSegment();
		if( Cmd ) {
			SDL_SubmitGPUCommandBuffer( Cmd );
			Cmd = nullptr;
		}
		TransientVB.clear();
		TransientIB.clear();
	}

	//-----------------------------------------------------------------------
	// 区間を閉じる: 記録した描画をレンダーパスにして流す
	//-----------------------------------------------------------------------
	void CloseSegment()
	{
		if( !Target ) {
			Draws.clear();
			UniformStorage.clear();
			return;
		}
		if( Draws.empty() && !PendingClear && !PendingStencilClear ) return;
		SDL_GPUCommandBuffer* cmd = GetCmd();

		// 一時データを GPU へ
		UploadTransient( cmd );

		SDL_GPUColorTargetInfo ct = {};
		ct.texture = Target->Tex;
		if( PendingClear ) {
			ct.load_op = SDL_GPU_LOADOP_CLEAR;
			ct.clear_color = { ClearRGBA[0], ClearRGBA[1], ClearRGBA[2], ClearRGBA[3] };
			ct.cycle = true;     // 全面を書き直すので前フレームの使用中なら別の実体を使ってよい
		} else {
			ct.load_op = SDL_GPU_LOADOP_LOAD;
		}
		ct.store_op = SDL_GPU_STOREOP_STORE;

		SDL_GPUDepthStencilTargetInfo ds = {};
		if( DepthStencil ) {
			ds.texture = DepthStencil->Tex;
			ds.load_op = SDL_GPU_LOADOP_DONT_CARE;
			ds.store_op = SDL_GPU_STOREOP_DONT_CARE;
			ds.stencil_load_op = PendingStencilClear ? SDL_GPU_LOADOP_CLEAR : SDL_GPU_LOADOP_LOAD;
			ds.stencil_store_op = SDL_GPU_STOREOP_STORE;
			ds.clear_stencil = StencilClearValue;
		}
		SDL_GPURenderPass* rp = SDL_BeginGPURenderPass( cmd, &ct, 1, DepthStencil ? &ds : nullptr );
		PendingClear = false;
		PendingStencilClear = false;
		if( !rp ) {
			TVPLOG_ERROR( "GPU: cannot begin render pass: {}", SDL_GetError() );
			Draws.clear();
			UniformStorage.clear();
			return;
		}

		const SDL_GPUTextureFormat colorFmt = Target->GpuFormat;
		for( RecordedDraw& d : Draws ) {
			SDL_GPUGraphicsPipeline* pipe = GetPipeline( d.call.state, colorFmt, DepthStencil != nullptr );
			if( !pipe ) continue;
			SDL_GPUViewport vp = { (float)d.viewport.x, (float)d.viewport.y, (float)d.viewport.w, (float)d.viewport.h, 0.0f, 1.0f };
			SDL_SetGPUViewport( rp, &vp );
			SDL_Rect sc;
			if( d.scissor ) sc = { d.scissorRect.x, d.scissorRect.y, d.scissorRect.w, d.scissorRect.h };
			else sc = { 0, 0, (int)Target->Width(), (int)Target->Height() };
			SDL_SetGPUScissor( rp, &sc );
			SDL_BindGPUGraphicsPipeline( rp, pipe );
			if( d.call.state.stencil.enable ) SDL_SetGPUStencilReference( rp, d.call.stencilReference );

			SDL_GPUBufferBinding vb[MaxVertexBuffers] = {};
			uint32_t nvb = d.call.state.vertexBufferCount;
			for( uint32_t i = 0; i < nvb; i++ ) {
				vb[i].buffer = d.transientVB[i] ? TransientVBGpu
					: ( d.call.vertexBuffers[i] ? static_cast<SDLBuffer*>( d.call.vertexBuffers[i] )->Buf : nullptr );
				vb[i].offset = d.call.vertexOffsets[i];
			}
			if( nvb ) SDL_BindGPUVertexBuffers( rp, 0, vb, nvb );

			SDL_GPUTextureSamplerBinding tsb[MaxTextures] = {};
			for( uint32_t i = 0; i < d.call.textureCount; i++ ) {
				SDLTexture* t = static_cast<SDLTexture*>( d.call.textures[i] );
				tsb[i].texture = t ? t->Tex : nullptr;
				tsb[i].sampler = GetSampler( d.call.samplers[i] );
			}
			if( d.call.textureCount ) SDL_BindGPUFragmentSamplers( rp, 0, tsb, d.call.textureCount );

			if( d.call.uniformSize[0] ) SDL_PushGPUVertexUniformData( cmd, 0, UniformStorage.data() + d.uniformOffset[0], d.call.uniformSize[0] );
			if( d.call.uniformSize[1] ) SDL_PushGPUFragmentUniformData( cmd, 0, UniformStorage.data() + d.uniformOffset[1], d.call.uniformSize[1] );

			if( d.call.indexBuffer ) {
				SDL_GPUBufferBinding ib = {};
				ib.buffer = d.transientIB ? TransientIBGpu : static_cast<SDLBuffer*>( d.call.indexBuffer )->Buf;
				ib.offset = d.call.indexOffset;
				SDL_BindGPUIndexBuffer( rp, &ib, d.call.indexType == IndexType::U16
					? SDL_GPU_INDEXELEMENTSIZE_16BIT : SDL_GPU_INDEXELEMENTSIZE_32BIT );
				SDL_DrawGPUIndexedPrimitives( rp, d.call.count, d.call.instanceCount, d.call.first, d.call.vertexOffset, d.call.firstInstance );
			} else {
				SDL_DrawGPUPrimitives( rp, d.call.count, d.call.instanceCount, d.call.first, d.call.firstInstance );
			}
		}
		SDL_EndGPURenderPass( rp );
		Draws.clear();
		UniformStorage.clear();
	}

	void UploadTransient( SDL_GPUCommandBuffer* cmd )
	{
		const uint32_t vbSize = (uint32_t)TransientVB.size();
		const uint32_t ibSize = (uint32_t)TransientIB.size();
		if( !vbSize && !ibSize ) return;
		EnsureBuffer( TransientVBGpu, TransientVBGpuSize, vbSize, SDL_GPU_BUFFERUSAGE_VERTEX );
		EnsureBuffer( TransientIBGpu, TransientIBGpuSize, ibSize, SDL_GPU_BUFFERUSAGE_INDEX );
		const uint32_t total = vbSize + ibSize;
		if( TransientTBSize < total ) {
			if( TransientTB ) SDL_ReleaseGPUTransferBuffer( Dev, TransientTB );
			TransientTBSize = std::max<uint32_t>( total, TransientTBSize * 2 );
			SDL_GPUTransferBufferCreateInfo tci = {};
			tci.usage = SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD;
			tci.size = TransientTBSize;
			TransientTB = SDL_CreateGPUTransferBuffer( Dev, &tci );
		}
		uint8_t* p = (uint8_t*)SDL_MapGPUTransferBuffer( Dev, TransientTB, true );   // cycle: 前の区間がまだ使っていても可
		if( vbSize ) memcpy( p, TransientVB.data(), vbSize );
		if( ibSize ) memcpy( p + vbSize, TransientIB.data(), ibSize );
		SDL_UnmapGPUTransferBuffer( Dev, TransientTB );
		SDL_GPUCopyPass* cp = SDL_BeginGPUCopyPass( cmd );
		if( vbSize ) {
			SDL_GPUTransferBufferLocation src = { TransientTB, 0 };
			SDL_GPUBufferRegion dst = { TransientVBGpu, 0, vbSize };
			SDL_UploadToGPUBuffer( cp, &src, &dst, true );
		}
		if( ibSize ) {
			SDL_GPUTransferBufferLocation src = { TransientTB, vbSize };
			SDL_GPUBufferRegion dst = { TransientIBGpu, 0, ibSize };
			SDL_UploadToGPUBuffer( cp, &src, &dst, true );
		}
		SDL_EndGPUCopyPass( cp );
		TransientVB.clear();
		TransientIB.clear();
	}

	void EnsureBuffer( SDL_GPUBuffer*& buf, uint32_t& cap, uint32_t need, SDL_GPUBufferUsageFlags usage )
	{
		if( !need || cap >= need ) return;
		if( buf ) SDL_ReleaseGPUBuffer( Dev, buf );
		cap = std::max<uint32_t>( need, std::max<uint32_t>( cap * 2, 64 * 1024 ) );
		SDL_GPUBufferCreateInfo ci = {};
		ci.usage = usage;
		ci.size = cap;
		buf = SDL_CreateGPUBuffer( Dev, &ci );
	}

	SDL_GPUSampler* GetSampler( const SamplerDesc& s )
	{
		Hasher h; h.Value( s );
		auto it = Samplers.find( h.h );
		if( it != Samplers.end() ) return it->second;
		SDL_GPUSamplerCreateInfo ci = {};
		ci.min_filter = s.minFilter == Filter::Linear ? SDL_GPU_FILTER_LINEAR : SDL_GPU_FILTER_NEAREST;
		ci.mag_filter = s.magFilter == Filter::Linear ? SDL_GPU_FILTER_LINEAR : SDL_GPU_FILTER_NEAREST;
		ci.mipmap_mode = s.mipFilter == Filter::Linear ? SDL_GPU_SAMPLERMIPMAPMODE_LINEAR : SDL_GPU_SAMPLERMIPMAPMODE_NEAREST;
		ci.address_mode_u = ToSDL( s.wrapU );
		ci.address_mode_v = ToSDL( s.wrapV );
		ci.address_mode_w = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE;
		ci.min_lod = 0.0f;
		ci.max_lod = s.mipmaps ? 1000.0f : 0.0f;
		SDL_GPUSampler* smp = SDL_CreateGPUSampler( Dev, &ci );
		Samplers[h.h] = smp;
		return smp;
	}

	SDL_GPUGraphicsPipeline* GetPipeline( const PipelineState& st, SDL_GPUTextureFormat colorFmt, bool hasDS )
	{
		SDLProgram* prog = static_cast<SDLProgram*>( st.program );
		Hasher h;
		h.Value( prog->Id );
		for( auto& a : st.attributes ) { h.Value( a.location ); h.Value( a.format ); h.Value( a.offset ); h.Value( a.slot ); }
		h.Value( st.strides ); h.Value( st.instanceRate ); h.Value( st.vertexBufferCount );
		h.Value( st.primitive ); h.Value( st.blend ); h.Value( st.stencil ); h.Value( st.cull );
		h.Value( colorFmt ); h.Value( hasDS );
		auto it = Pipelines.find( h.h );
		if( it != Pipelines.end() ) return it->second;

		SDL_GPUGraphicsPipelineCreateInfo ci = {};
		ci.vertex_shader = prog->VS;
		ci.fragment_shader = prog->FS;
		SDL_GPUVertexBufferDescription vbd[MaxVertexBuffers] = {};
		for( uint32_t i = 0; i < st.vertexBufferCount; i++ ) {
			vbd[i].slot = i;
			vbd[i].pitch = st.strides[i];
			vbd[i].input_rate = st.instanceRate[i] ? SDL_GPU_VERTEXINPUTRATE_INSTANCE : SDL_GPU_VERTEXINPUTRATE_VERTEX;
		}
		std::vector<SDL_GPUVertexAttribute> va( st.attributes.size() );
		for( size_t i = 0; i < st.attributes.size(); i++ ) {
			va[i].location = st.attributes[i].location;
			va[i].buffer_slot = st.attributes[i].slot;
			va[i].format = ToSDL( st.attributes[i].format );
			va[i].offset = st.attributes[i].offset;
		}
		ci.vertex_input_state.vertex_buffer_descriptions = vbd;
		ci.vertex_input_state.num_vertex_buffers = st.vertexBufferCount;
		ci.vertex_input_state.vertex_attributes = va.data();
		ci.vertex_input_state.num_vertex_attributes = (Uint32)va.size();
		ci.primitive_type = ToSDL( st.primitive );
		ci.rasterizer_state.fill_mode = SDL_GPU_FILLMODE_FILL;
		ci.rasterizer_state.cull_mode = st.cull == CullMode::Back ? SDL_GPU_CULLMODE_BACK
			: st.cull == CullMode::Front ? SDL_GPU_CULLMODE_FRONT : SDL_GPU_CULLMODE_NONE;
		// GL の既定 (反時計回りが表) に合わせたいが、SDL_GPU が回り方をどの座標系で
		// 判定するか (NDC か描画先の座標か) は未確認。enableCulling を使うときに要確認
		// (TODO: 実機で裏返しの描画が消えるか確かめる)
		ci.rasterizer_state.front_face = SDL_GPU_FRONTFACE_CLOCKWISE;

		SDL_GPUColorTargetDescription ctd = {};
		ctd.format = colorFmt;
		const BlendState& b = st.blend;
		ctd.blend_state.enable_blend = b.enable;
		ctd.blend_state.src_color_blendfactor = ToSDL( b.srcColor );
		ctd.blend_state.dst_color_blendfactor = ToSDL( b.dstColor );
		ctd.blend_state.color_blend_op = ToSDL( b.colorOp );
		ctd.blend_state.src_alpha_blendfactor = ToSDL( b.srcAlpha );
		ctd.blend_state.dst_alpha_blendfactor = ToSDL( b.dstAlpha );
		ctd.blend_state.alpha_blend_op = ToSDL( b.alphaOp );
		ctd.blend_state.color_write_mask = b.writeMask;
		ctd.blend_state.enable_color_write_mask = true;
		ci.target_info.color_target_descriptions = &ctd;
		ci.target_info.num_color_targets = 1;
		if( hasDS ) {
			ci.target_info.has_depth_stencil_target = true;
			ci.target_info.depth_stencil_format = SDL_GPU_TEXTUREFORMAT_D32_FLOAT_S8_UINT;
			const StencilState& s = st.stencil;
			ci.depth_stencil_state.enable_stencil_test = s.enable;
			SDL_GPUStencilOpState so = {};
			so.compare_op = ToSDL( s.compare );
			so.pass_op = ToSDL( s.pass );
			so.fail_op = ToSDL( s.fail );
			so.depth_fail_op = ToSDL( s.depthFail );
			ci.depth_stencil_state.front_stencil_state = so;
			ci.depth_stencil_state.back_stencil_state = so;
			ci.depth_stencil_state.compare_mask = s.readMask;
			ci.depth_stencil_state.write_mask = s.writeMask;
		}
		SDL_GPUGraphicsPipeline* p = SDL_CreateGPUGraphicsPipeline( Dev, &ci );
		if( !p ) TVPLOG_ERROR( "GPU: cannot create pipeline: {}", SDL_GetError() );
		Pipelines[h.h] = p;
		return p;
	}
};

SDLDevice* SDLDevice::TheDevice = nullptr;

static void FlushBeforeRelease( const void* res )
{
	if( SDLDevice::TheDevice ) SDLDevice::TheDevice->FlushForRelease( res );
}

} // anonymous namespace

//---------------------------------------------------------------------------
SDL_PixelFormat GetSDLPixelFormat( Texture* tex )
{
	SDLTexture* t = static_cast<SDLTexture*>( tex );
	return SDL_GetPixelFormatFromGPUTextureFormat( t->GpuFormat );
}

//---------------------------------------------------------------------------
// 共有デバイスの生成。
//   SDL_Renderer (GPU 版) と同じデバイスを使うので、SDL の GPU レンダラーが
//   使えるシェーダー形式も含めて要求する。Canvas 用のシェーダーは SPIR-V。
//   -gpudriver=<name> で SDL_GPU のドライバを指定できる (vulkan / direct3d12 / ...)。
//---------------------------------------------------------------------------
Device* CreatePlatformDevice()
{
	if( !SDL_WasInit( SDL_INIT_VIDEO ) ) SDL_InitSubSystem( SDL_INIT_VIDEO );
	const char* driver = nullptr;
	std::string driverName;
	tTJSVariant val;
	if( TVPGetCommandLine( TJS_W("-gpudriver"), &val ) ) {
		ttstr s( val );
		TVPUtf16ToUtf8( driverName, s.AsStdString() );
		driver = driverName.c_str();
	}
	SDL_PropertiesID props = SDL_CreateProperties();
	SDL_SetBooleanProperty( props, SDL_PROP_GPU_DEVICE_CREATE_SHADERS_SPIRV_BOOLEAN, true );
	SDL_SetBooleanProperty( props, SDL_PROP_GPU_DEVICE_CREATE_SHADERS_PRIVATE_BOOLEAN, true );
	// DXIL (D3D12) は Xbox と、-gpudriver=direct3d12 で明示したときだけ要求する。
	// SDL は D3D12 を Vulkan より先に試すので、常に要求すると Windows の既定が
	// D3D12 に変わり、SPIR-V だけのシェーダーパックを持つ案件が動かなくなる。
	bool wantDxil = driver && SDL_strcasecmp( driver, "direct3d12" ) == 0;
#if defined(SDL_PLATFORM_XBOXONE) || defined(SDL_PLATFORM_XBOXSERIES)
	wantDxil = true;
#endif
	if( wantDxil ) SDL_SetBooleanProperty( props, SDL_PROP_GPU_DEVICE_CREATE_SHADERS_DXIL_BOOLEAN, true );
#ifdef _DEBUG
	SDL_SetBooleanProperty( props, SDL_PROP_GPU_DEVICE_CREATE_DEBUGMODE_BOOLEAN, true );
#endif
	if( driver ) SDL_SetStringProperty( props, SDL_PROP_GPU_DEVICE_CREATE_NAME_STRING, driver );
	SDL_GPUDevice* dev = SDL_CreateGPUDeviceWithProperties( props );
	SDL_DestroyProperties( props );
	if( !dev ) {
		TVPLOG_ERROR( "GPU: SDL_CreateGPUDevice failed: {}", SDL_GetError() );
		return nullptr;
	}
	return new SDLDevice( dev, true );
}

} // namespace TVPGpu
