//---------------------------------------------------------------------------
// Canvas (GPU 版) の描画デバイス抽象
//
// SDL_GPU の作り方 (パイプライン状態を前もってまとめる / 描画はレンダーパスの中 /
// uniform はブロックで渡す / シェーダーは事前変換したもの) に合わせた、Canvas が
// 使う範囲だけの薄い層。バックエンドは今のところ SDL_GPU のみ
// (sdl3/visual/GpuDeviceSDL.cpp)。設計は doc/CanvasGPU.md。
//
// 座標系は左上原点 (テクスチャ・描画先とも 1 行目が上)。GL のような上下反転は無い。
//---------------------------------------------------------------------------
#ifndef GpuDeviceH
#define GpuDeviceH

#include <cstdint>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

namespace TVPGpu {

//---------------------------------------------------------------------------
// 列挙
//---------------------------------------------------------------------------
enum class TextureFormat : uint8_t {
	BGRA8,      // krkrz の Bitmap と同じメモリ並び (B,G,R,A)。色テクスチャ・描画先の既定
	A8,         // α のみ (サンプル結果は (0,0,0,a))。GL の GL_ALPHA 相当
	D32F_S8,    // 深度・ステンシル (D24S8 は持たない機種があるので使わない)
};

enum class PrimitiveType : uint8_t { TriangleList, TriangleStrip, LineList, LineStrip, PointList };
enum class IndexType : uint8_t { U16, U32 };

enum class VertexFormat : uint8_t {
	Invalid,
	Float, Float2, Float3, Float4,
	Int, Int2, Int3, Int4,
	UInt, UInt2, UInt3, UInt4,
	Byte2, Byte4, UByte2, UByte4,
	Byte2Norm, Byte4Norm, UByte2Norm, UByte4Norm,
	Short2, Short4, UShort2, UShort4,
	Short2Norm, Short4Norm, UShort2Norm, UShort4Norm,
};

enum class BlendFactor : uint8_t {
	Zero, One, SrcColor, OneMinusSrcColor, DstColor, OneMinusDstColor,
	SrcAlpha, OneMinusSrcAlpha, DstAlpha, OneMinusDstAlpha,
};
enum class BlendOp : uint8_t { Add, Subtract, ReverseSubtract, Min, Max };
enum class CompareOp : uint8_t { Never, Less, Equal, LessOrEqual, Greater, NotEqual, GreaterOrEqual, Always };
enum class StencilOp : uint8_t { Keep, Zero, Replace, IncrementClamp, DecrementClamp, Invert, IncrementWrap, DecrementWrap };
enum class CullMode : uint8_t { None, Back, Front };
enum class Filter : uint8_t { Nearest, Linear };
enum class WrapMode : uint8_t { Repeat, ClampToEdge, MirroredRepeat };

//---------------------------------------------------------------------------
// 状態
//---------------------------------------------------------------------------
struct BlendState {
	bool enable = false;
	BlendFactor srcColor = BlendFactor::One, dstColor = BlendFactor::Zero;
	BlendFactor srcAlpha = BlendFactor::One, dstAlpha = BlendFactor::Zero;
	BlendOp colorOp = BlendOp::Add, alphaOp = BlendOp::Add;
	uint8_t writeMask = 0xf;   // bit0=R bit1=G bit2=B bit3=A
};

struct StencilState {
	bool enable = false;
	CompareOp compare = CompareOp::Always;
	StencilOp pass = StencilOp::Keep, fail = StencilOp::Keep, depthFail = StencilOp::Keep;
	uint8_t readMask = 0xff, writeMask = 0xff;
};

struct SamplerDesc {
	Filter minFilter = Filter::Linear, magFilter = Filter::Linear, mipFilter = Filter::Nearest;
	bool mipmaps = false;
	WrapMode wrapU = WrapMode::ClampToEdge, wrapV = WrapMode::ClampToEdge;
	bool operator==( const SamplerDesc& o ) const { return memcmp( this, &o, sizeof(*this) ) == 0; }
};

struct TextureDesc {
	uint32_t width = 0, height = 0;
	TextureFormat format = TextureFormat::BGRA8;
	uint32_t mipLevels = 1;
	bool renderTarget = false;   // 描画先にもする (Offscreen / 画面用)
};

//---------------------------------------------------------------------------
// リソース
//---------------------------------------------------------------------------
class Texture {
public:
	virtual ~Texture() {}
	uint32_t Width() const { return Desc.width; }
	uint32_t Height() const { return Desc.height; }
	TextureFormat Format() const { return Desc.format; }
	const TextureDesc& GetDesc() const { return Desc; }
	virtual void* NativeHandle() const = 0;   // SDL_GPU なら SDL_GPUTexture*
protected:
	TextureDesc Desc;
};

class Buffer {
public:
	virtual ~Buffer() {}
	uint32_t Size() const { return BufSize; }
	bool IsIndex() const { return Index; }
	virtual void* NativeHandle() const = 0;
protected:
	uint32_t BufSize = 0;
	bool Index = false;
};

//---------------------------------------------------------------------------
// シェーダーのリフレクション情報 (gpushaderc.py が作るテキストを読んだもの)
//---------------------------------------------------------------------------
enum class ShaderStage : uint8_t { Vertex = 0, Fragment = 1 };

enum class UniformType : uint8_t {
	Unknown,
	Float, Vec2, Vec3, Vec4,
	Int, IVec2, IVec3, IVec4,
	UInt, UVec2, UVec3, UVec4,
	Bool, BVec2, BVec3, BVec4,
	Mat2, Mat3, Mat4,
	Mat2x3, Mat2x4, Mat3x2, Mat3x4, Mat4x2, Mat4x3,
};

struct ShaderReflection {
	struct Uniform {
		std::string name;
		ShaderStage stage;
		uint32_t offset;
		UniformType type;
		uint32_t arrayCount;     // 0 = 配列でない
		uint32_t arrayStride;
		uint32_t matrixStride;
	};
	struct Sampler {
		std::string name;
		ShaderStage stage;
		uint32_t slot;
	};
	struct Attribute {
		std::string name;
		uint32_t location;
		std::string type;        // GLSL の型名 (float / vec2 ...)
	};
	std::vector<Uniform> uniforms;
	std::vector<Sampler> samplers;
	std::vector<Attribute> attributes;
	uint32_t blockSize[2] = { 0, 0 };     // stage ごとの uniform ブロックの大きさ
	uint32_t samplerCount[2] = { 0, 0 };  // stage ごとのテクスチャ数

	bool Parse( const char* text, size_t len, std::string* error );
	const Attribute* FindAttribute( const std::string& name ) const;
	const Sampler* FindSampler( const std::string& name ) const;   // フラグメント優先
};

UniformType ParseUniformType( const std::string& glslType );
uint32_t UniformComponentCount( UniformType t );   // mat4 なら 16

//! シェーダーの形式 (パックの値。SDL_GPU の SPIRV / PRIVATE / DXIL に対応)
enum class ShaderFormat : uint32_t { Spirv = 1, Private = 2, Dxil = 3 };

struct ProgramBinary {
	ShaderFormat format = ShaderFormat::Spirv;
	std::vector<uint8_t> vertexCode, fragmentCode;
	ShaderReflection reflection;
};

class Program {
public:
	virtual ~Program() {}
	const ShaderReflection& Reflection() const { return Refl; }
protected:
	ShaderReflection Refl;
};

//---------------------------------------------------------------------------
// 描画
//---------------------------------------------------------------------------
struct VertexAttribute {
	uint32_t location = 0;
	VertexFormat format = VertexFormat::Float2;
	uint32_t offset = 0;
	uint32_t slot = 0;          // 頂点バッファのスロット
};

static const uint32_t MaxVertexBuffers = 8;
static const uint32_t MaxTextures = 8;

struct PipelineState {
	Program* program = nullptr;
	std::vector<VertexAttribute> attributes;
	uint32_t strides[MaxVertexBuffers] = {};
	uint8_t instanceRate[MaxVertexBuffers] = {};   // 1 = インスタンスごとに進むスロット
	uint32_t vertexBufferCount = 0;
	PrimitiveType primitive = PrimitiveType::TriangleStrip;
	BlendState blend;
	StencilState stencil;
	CullMode cull = CullMode::None;
};

struct DrawCall {
	PipelineState state;
	Buffer* vertexBuffers[MaxVertexBuffers] = {};
	uint32_t vertexOffsets[MaxVertexBuffers] = {};
	Buffer* indexBuffer = nullptr;
	IndexType indexType = IndexType::U16;
	uint32_t indexOffset = 0;    // バイト
	Texture* textures[MaxTextures] = {};    // フラグメントのスロット順
	SamplerDesc samplers[MaxTextures];
	uint32_t textureCount = 0;
	const void* uniforms[2] = { nullptr, nullptr };   // stage ごとの uniform ブロック
	uint32_t uniformSize[2] = { 0, 0 };
	uint32_t count = 0;          // 頂点数 (indexBuffer 有りならインデックス数)
	uint32_t first = 0;          // 最初の頂点 / インデックス
	int32_t vertexOffset = 0;    // インデックス描画時に足す値
	uint32_t instanceCount = 1;
	uint32_t firstInstance = 0;
	uint8_t stencilReference = 0;
};

struct Rect { int32_t x, y, w, h; };

class Device {
public:
	virtual ~Device() {}

	//! 共有のデバイス (無ければ作る。作れなければ例外)
	static Device* Get();
	//! 作られていれば返す (作らない)
	static Device* GetIfCreated();
	//! 共有デバイスを破棄する (終了処理)
	static void Shutdown();

	virtual void* NativeHandle() const = 0;   // SDL_GPU なら SDL_GPUDevice*
	virtual bool SupportsShaderFormat( ShaderFormat f ) const = 0;

	// リソース
	virtual std::unique_ptr<Texture> CreateTexture( const TextureDesc& desc ) = 0;
	//! pixels は 1 行目が上。format が BGRA8 なら B,G,R,A、A8 なら 1 byte/px
	virtual void UploadTexture( Texture* tex, uint32_t level, uint32_t x, uint32_t y,
		uint32_t w, uint32_t h, const void* pixels, uint32_t pitch ) = 0;
	virtual void CopyTexture( Texture* src, uint32_t sx, uint32_t sy,
		Texture* dst, uint32_t dx, uint32_t dy, uint32_t w, uint32_t h ) = 0;
	virtual void GenerateMipmaps( Texture* tex ) = 0;
	//! 同期読み戻し (遅い)。dst には B,G,R,A で 1 行目が上
	virtual bool ReadPixels( Texture* tex, uint32_t x, uint32_t y, uint32_t w, uint32_t h,
		void* dst, uint32_t pitch ) = 0;

	virtual std::unique_ptr<Buffer> CreateBuffer( bool index, uint32_t size ) = 0;
	virtual void UpdateBuffer( Buffer* buf, uint32_t offset, const void* data, uint32_t size ) = 0;
	//! 一時的な頂点 / インデックスデータ (クライアント配列の代わり)。フレームが進むと無効
	virtual void AllocTransient( bool index, const void* data, uint32_t size,
		Buffer** buf, uint32_t* offset ) = 0;

	virtual std::unique_ptr<Program> CreateProgram( const ProgramBinary& bin, std::string* error ) = 0;

	// 描画。BeginPass〜EndPass の間に Draw を積む。パスの途中でテクスチャ更新などを
	// 呼んでもよい (デバイスがパスを切って順序を保つ)。
	virtual void BeginPass( Texture* target, Texture* depthStencil ) = 0;
	//! 描画先を塗りつぶす (scissor 無効時のみ有効。scissor 付きのクリアは描画で行う)
	virtual void ClearColor( const float rgba[4] ) = 0;
	virtual void ClearStencil( uint8_t value ) = 0;
	virtual void SetViewport( const Rect& r ) = 0;
	virtual void SetScissor( const Rect* r ) = 0;   // nullptr で無効
	virtual void Draw( const DrawCall& call ) = 0;
	virtual void EndPass() = 0;

	//! 積んだものをすべて GPU に送る (フレームの最後)
	virtual void Submit() = 0;
	//! 描画先として使ってよい色フォーマット (BGRA8 が描画先に使えない機種の確認用)
	virtual bool SupportsRenderTarget( TextureFormat f ) const = 0;
};

//! バックエンドが実装する生成関数 (sdl3/visual/GpuDeviceSDL.cpp)
Device* CreatePlatformDevice();

} // namespace TVPGpu

#endif
