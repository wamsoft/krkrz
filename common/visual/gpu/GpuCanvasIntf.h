//---------------------------------------------------------------------------
// Canvas 系 TJS クラスの GPU 版 (描画デバイス抽象 TVPGpu::Device の上に実装)
//
// TJS から見たクラス名・API は GL 版 (common/visual/opengl/) と同じ:
//   Texture / Offscreen / ShaderProgram / VertexBuffer / VertexBinder / Canvas
// GL 版とは同時にビルドしない (KRKRZ_CANVAS_BACKEND で選ぶ)。
// Matrix32 / Matrix44 は GL に依存しないので GL 版のものをそのまま使う。
//
// 違い (doc/CanvasGPU.md):
//   - ShaderProgram は事前変換したシェーダーをソースのハッシュで引く
//   - nativeHandle は SDL_GPUTexture* などバックエンドのハンドル
//   - 座標は左上原点。Offscreen も Texture と同じ向き (GL 版の上下反転は無い)
//---------------------------------------------------------------------------
#ifndef GpuCanvasIntfH
#define GpuCanvasIntfH

#include "tjsNative.h"
#include "ComplexRect.h"
#include "GpuDevice.h"
#include <memory>
#include <string>
#include <vector>

// テクスチャの色形式 (TJS の tcfRGBA / tcfAlpha / tcfAlphaChannel)
enum class tTVPGpuTextureColor : tjs_int { RGBA = 0, Alpha = 1 };
static const tjs_int TVP_GPU_TCF_ALPHA_CHANNEL = 2;

// GL 版と互換の定数値 (TJS の stNearest / wmRepeat などは GL の enum 値のまま)
static const tjs_int TVP_GL_NEAREST = 0x2600;
static const tjs_int TVP_GL_LINEAR = 0x2601;
static const tjs_int TVP_GL_REPEAT = 0x2901;
static const tjs_int TVP_GL_CLAMP_TO_EDGE = 0x812F;
static const tjs_int TVP_GL_MIRRORED_REPEAT = 0x8370;

//---------------------------------------------------------------------------
// 描画元になれるもの (Texture / Offscreen)
//---------------------------------------------------------------------------
class iTVPGpuDrawable {
public:
	virtual ~iTVPGpuDrawable() {}
	virtual TVPGpu::Texture* GetGpuTexture() const = 0;
	//! 描画サイズ
	virtual tjs_uint GetWidth() const = 0;
	virtual tjs_uint GetHeight() const = 0;
	//! 実体の大きさ
	virtual tjs_uint GetMemoryWidth() const = 0;
	virtual tjs_uint GetMemoryHeight() const = 0;
	virtual TVPGpu::SamplerDesc GetSamplerDesc() const = 0;
	virtual tTVPGpuTextureColor GetColorFormat() const = 0;
};
//! TJS の値 (Texture / Offscreen オブジェクト) から取り出す。違えば nullptr
iTVPGpuDrawable* TVPGetGpuDrawable( const tTJSVariant* param );

//---------------------------------------------------------------------------
// Texture
//---------------------------------------------------------------------------
class tTJSNI_GpuTexture : public tTJSNativeInstance, public iTVPGpuDrawable {
	std::unique_ptr<TVPGpu::Texture> Tex;
	tjs_uint SrcWidth = 0, SrcHeight = 0;     // 描画サイズ
	tTVPGpuTextureColor Color = tTVPGpuTextureColor::RGBA;
	tjs_int StretchType = TVP_GL_LINEAR;
	tjs_int WrapS = TVP_GL_CLAMP_TO_EDGE, WrapT = TVP_GL_CLAMP_TO_EDGE;
	tTVPRect Scale9Patch = { -1, -1, -1, -1 };
	tTVPRect Margin9Patch = { -1, -1, -1, -1 };
	tTJSVariant MarginRectObject;

	void Create( tjs_uint w, tjs_uint h, tTVPGpuTextureColor color, tjs_uint levels = 1 );
	void LoadBitmap( const class tTVPBaseBitmap* bmp, tTVPGpuTextureColor color, bool fromAlphaChannel );
	void LoadMipmap( const class tTVPBaseBitmap* bmp, class tTJSArrayNI* sizeList, tjs_int type, tjs_real typeopt );   // type は tTVPBBStretchType
	void Read9Patch( std::unique_ptr<class tTVPBaseBitmap>& holder, const class tTVPBaseBitmap*& bmp );

public:
	tTJSNI_GpuTexture();
	~tTJSNI_GpuTexture() override;
	tjs_error TJS_INTF_METHOD Construct( tjs_int numparams, tTJSVariant** param, iTJSDispatch2* tjs_obj ) override;
	void TJS_INTF_METHOD Invalidate() override;

	//! w x h の空テクスチャを作る (描画デバイスのレイヤー画像用など)
	void CreateEmpty( tjs_uint w, tjs_uint h ) { Create( w, h, tTVPGpuTextureColor::RGBA ); SrcWidth = w; SrcHeight = h; }
	//! 範囲をビットマップから転送する (範囲はクリップされる)
	bool CopyBitmap( tjs_int left, tjs_int top, const class tTVPBaseBitmap* bmp, const tTVPRect& srcRect );
	//! 連続したメモリ (BGRA, 1 行目が上) から転送する
	void UpdateDirect( tjs_int x, tjs_int y, tjs_int w, tjs_int h, const void* bits, tjs_int pitch );
	void CopyTexture( const tTJSNI_GpuTexture* src );
	void SetDrawSize( tjs_uint w, tjs_uint h ) { SrcWidth = w; SrcHeight = h; }
	bool Resize( tjs_int width, tjs_int height );

	const tTJSVariant& GetMarginRectObject() const { return MarginRectObject; }
	const tTVPRect& GetScale9Patch() const { return Scale9Patch; }
	const tTVPRect& GetMargin9Patch() const { return Margin9Patch; }
	bool IsGray() const { return Color == tTVPGpuTextureColor::Alpha; }
	bool IsPowerOfTwo() const;
	tjs_int64 GetNativeHandle() const { return Tex ? (tjs_int64)(intptr_t)Tex->NativeHandle() : 0; }
	tjs_int GetStretchType() const { return StretchType; }
	void SetStretchType( tjs_int v ) { StretchType = v; }
	tjs_int GetWrapModeHorizontal() const { return WrapS; }
	void SetWrapModeHorizontal( tjs_int v ) { WrapS = v; }
	tjs_int GetWrapModeVertical() const { return WrapT; }
	void SetWrapModeVertical( tjs_int v ) { WrapT = v; }

	// iTVPGpuDrawable
	TVPGpu::Texture* GetGpuTexture() const override { return Tex.get(); }
	tjs_uint GetWidth() const override { return SrcWidth; }
	tjs_uint GetHeight() const override { return SrcHeight; }
	tjs_uint GetMemoryWidth() const override { return Tex ? Tex->Width() : 0; }
	tjs_uint GetMemoryHeight() const override { return Tex ? Tex->Height() : 0; }
	TVPGpu::SamplerDesc GetSamplerDesc() const override;
	tTVPGpuTextureColor GetColorFormat() const override { return Color; }

	friend class tTJSNI_GpuOffscreen;
};

class tTJSNC_GpuTexture : public tTJSNativeClass {
public:
	tTJSNC_GpuTexture();
	static tjs_uint32 ClassID;
protected:
	tTJSNativeInstance* CreateNativeInstance() override { return new tTJSNI_GpuTexture(); }
};

//---------------------------------------------------------------------------
// Offscreen (描画先になるテクスチャ + ステンシル)
//---------------------------------------------------------------------------
class tTJSNI_GpuOffscreen : public tTJSNativeInstance, public iTVPGpuDrawable {
	std::unique_ptr<TVPGpu::Texture> Color;
	std::unique_ptr<TVPGpu::Texture> Stencil;
public:
	tjs_error TJS_INTF_METHOD Construct( tjs_int numparams, tTJSVariant** param, iTJSDispatch2* tjs_obj ) override;
	void TJS_INTF_METHOD Invalidate() override;

	void Create( tjs_uint w, tjs_uint h );
	TVPGpu::Texture* GetStencilTexture();   // 必要になったときに作る
	void CopyFromBitmap( tjs_int left, tjs_int top, const class tTVPBaseBitmap* bmp, const tTVPRect& srcRect );
	void ExchangeTexture( tTJSNI_GpuTexture* texture );
	tjs_int64 GetNativeHandle() const { return Color ? (tjs_int64)(intptr_t)Color->NativeHandle() : 0; }

	// iTVPGpuDrawable
	TVPGpu::Texture* GetGpuTexture() const override { return Color.get(); }
	tjs_uint GetWidth() const override { return Color ? Color->Width() : 0; }
	tjs_uint GetHeight() const override { return Color ? Color->Height() : 0; }
	tjs_uint GetMemoryWidth() const override { return GetWidth(); }
	tjs_uint GetMemoryHeight() const override { return GetHeight(); }
	TVPGpu::SamplerDesc GetSamplerDesc() const override { return TVPGpu::SamplerDesc(); }
	tTVPGpuTextureColor GetColorFormat() const override { return tTVPGpuTextureColor::RGBA; }
};

class tTJSNC_GpuOffscreen : public tTJSNativeClass {
public:
	tTJSNC_GpuOffscreen();
	static tjs_uint32 ClassID;
protected:
	tTJSNativeInstance* CreateNativeInstance() override { return new tTJSNI_GpuOffscreen(); }
};

//---------------------------------------------------------------------------
// VertexBuffer / VertexBinder
//---------------------------------------------------------------------------
class tTJSNI_GpuVertexBuffer : public tTJSNativeInstance {
	std::unique_ptr<TVPGpu::Buffer> Buf;
	std::vector<uint8_t> Shadow;   // lock() 用の CPU 側の写し (GL の mapBuffer 相当)
	tjs_int DataType = 0;           // GL の型 enum (GL_FLOAT など)
	tjs_int UpdateType = 0;         // GL の usage enum
	bool Index = false;
	bool Locked = false;
public:
	tjs_error TJS_INTF_METHOD Construct( tjs_int numparams, tTJSVariant** param, iTJSDispatch2* tjs_obj ) override;
	void TJS_INTF_METHOD Invalidate() override;

	TVPGpu::Buffer* GetBuffer() const { return Buf.get(); }
	const std::vector<uint8_t>& GetShadow() const { return Shadow; }
	tjs_int64 GetNativeHandle() const { return Buf ? (tjs_int64)(intptr_t)Buf->NativeHandle() : 0; }
	bool IsIndex() const { return Index; }
	tjs_int GetUpdateType() const { return UpdateType; }
	tjs_int GetDataType() const { return DataType; }
	tjs_int GetBufferSize() const { return (tjs_int)Shadow.size(); }
	void* Lock();
	void Unlock();
	void SetVertex( const tTJSVariant* param, tjs_int offset );
};

class tTJSNC_GpuVertexBuffer : public tTJSNativeClass {
public:
	tTJSNC_GpuVertexBuffer();
	static tjs_uint32 ClassID;
protected:
	tTJSNativeInstance* CreateNativeInstance() override { return new tTJSNI_GpuVertexBuffer(); }
};

class tTJSNI_GpuVertexBinder : public tTJSNativeInstance {
	tTJSVariant VertexBufferObject;
	tTJSNI_GpuVertexBuffer* VertexBufferInstance = nullptr;
	tjs_int Stride = 0;
	tjs_uint ComponentCount = 4;
	tjs_uint Offset = 0;
	bool Normalize = false;
	void SetVertexBufferObject( const tTJSVariant& val );
public:
	tjs_error TJS_INTF_METHOD Construct( tjs_int numparams, tTJSVariant** param, iTJSDispatch2* tjs_obj ) override;
	void TJS_INTF_METHOD Invalidate() override;

	const tTJSVariant& GetVertexBufferObject() const { return VertexBufferObject; }
	tTJSNI_GpuVertexBuffer* GetVertexBuffer() const { return VertexBufferInstance; }
	tjs_int GetStride() const { return Stride; }
	void SetStride( tjs_int v ) { Stride = v; }
	tjs_uint GetComponentCount() const { return ComponentCount; }
	void SetComponentCount( tjs_uint v ) { ComponentCount = v; }
	tjs_uint GetOffset() const { return Offset; }
	void SetOffset( tjs_uint v ) { Offset = v; }
	bool GetNormalize() const { return Normalize; }
	void SetNormalize( bool v ) { Normalize = v; }
	//! 頂点属性の形式 (データ型・要素数・正規化から)。対応しない組み合わせは Invalid
	TVPGpu::VertexFormat GetVertexFormat() const;
};

class tTJSNC_GpuVertexBinder : public tTJSNativeClass {
public:
	tTJSNC_GpuVertexBinder();
	static tjs_uint32 ClassID;
protected:
	tTJSNativeInstance* CreateNativeInstance() override { return new tTJSNI_GpuVertexBinder(); }
};

//---------------------------------------------------------------------------
// ShaderProgram
//---------------------------------------------------------------------------
class tTJSNI_GpuShaderProgram : public tTJSNativeInstance {
	std::unique_ptr<TVPGpu::Program> Prog;
	std::vector<uint8_t> Block[2];         // stage ごとの uniform ブロック (std140)
	// プロパティで与えられたテクスチャ (フラグメントのスロット順) と頂点属性
	std::vector<tTJSVariant> SamplerValues;
	struct AttributeValue { std::string name; tTJSVariant value; };
	std::vector<AttributeValue> AttributeValues;

	void WriteUniform( const TVPGpu::ShaderReflection::Uniform& u, const tTJSVariant* param );
public:
	tjs_error TJS_INTF_METHOD Construct( tjs_int numparams, tTJSVariant** param, iTJSDispatch2* tjs_obj ) override;
	void TJS_INTF_METHOD Invalidate() override;

	TVPGpu::Program* GetProgram() const { return Prog.get(); }
	const TVPGpu::ShaderReflection& Reflection() const { return Prog->Reflection(); }
	const std::vector<uint8_t>& GetBlock( TVPGpu::ShaderStage s ) const { return Block[(int)s]; }

	//! TJS からのプロパティ代入。シェーダーに無い名前なら TJS_E_MEMBERNOTFOUND
	tjs_error SetShaderParam( const ttstr& name, const tTJSVariant* param );
	//! C++ から uniform に float を書く (無ければ false)
	bool SetUniformFloats( const char* name, const float* v, tjs_uint count );
	bool HasUniform( const char* name ) const;
	//! プロパティで与えられたテクスチャ (無ければ nullptr)
	iTVPGpuDrawable* GetSamplerValue( tjs_uint slot ) const;
	const tTJSVariant* GetAttributeValue( const std::string& name ) const;
};

class tTJSNC_GpuShaderProgram : public tTJSNativeClass {
public:
	tTJSNC_GpuShaderProgram();
	static tjs_uint32 ClassID;
protected:
	iTJSDispatch2* CreateBaseTJSObject() override;
	tTJSNativeInstance* CreateNativeInstance() override { return new tTJSNI_GpuShaderProgram(); }
};

//---------------------------------------------------------------------------
// Canvas
//---------------------------------------------------------------------------
enum class tTVPGpuBlendMode : tjs_int {
	bmDisable = 0, bmOpaque = 1, bmAlpha = 2, bmAdd = 3, bmAddWithAlpha = 4,
	bmSubtract = 5, bmMultiply = 6, bmMin = 7, bmMax = 8, bmScreen = 9
};

class tTJSNI_GpuCanvas : public tTJSNativeInstance {
	struct State {
		float Matrix[6];
		tjs_int ClipRect[4];
		bool EnableClip;
		bool EnableCulling;
	};
	bool InDrawing = false;
	bool EnableClipRect = false;
	bool EnableCulling = false;
	tjs_uint32 ClearColor = 0xff000000;
	tTVPGpuBlendMode BlendMode = tTVPGpuBlendMode::bmAlpha;
	std::vector<State> StateStack;

	// 描画先: renderTarget (Offscreen) が無ければ画面 (SetScreenTarget で渡される)
	tTJSVariant RenderTargetObject;
	tTJSNI_GpuOffscreen* RenderTargetInstance = nullptr;
	TVPGpu::Texture* ScreenTarget = nullptr;
	TVPGpu::Texture* CurrentTarget = nullptr;
	tjs_int CanvasWidth = 0, CanvasHeight = 0;
	tjs_int SurfaceWidth = 0, SurfaceHeight = 0;

	tTJSVariant ClipRectObject;
	class tTJSNI_Rect* ClipRectInstance = nullptr;
	tTJSVariant Matrix32Object;
	class tTJSNI_Matrix32* Matrix32Instance = nullptr;

	tTJSVariant EmbeddedDefaultShaderObject, DefaultShaderObject;
	tTJSNI_GpuShaderProgram* DefaultShaderInstance = nullptr;
	tTJSVariant EmbeddedDefaultFillShaderObject, DefaultFillShaderObject;
	tTJSNI_GpuShaderProgram* DefaultFillShaderInstance = nullptr;
	tTJSVariant TransitionShaderObject[2];
	tTJSNI_GpuShaderProgram* TransitionShaderInstance[2] = { nullptr, nullptr };

	tTJSVariant CreateShaderObject( const char* vs, const char* fs, bool opacityOne );
	tTJSNI_GpuShaderProgram* EnsureTransitionShader( bool universal );

	void BindTarget();
	void ApplyScissor();
	TVPGpu::BlendState MakeBlendState() const;
	void FillCommon( TVPGpu::DrawCall& dc, tTJSNI_GpuShaderProgram* shader );
	//! 位置 + UV (float2 + float2) の頂点で描く。drawables は s_tex0.. の順
	void DrawTexturedQuads( tTJSNI_GpuShaderProgram* shader, const iTVPGpuDrawable* const* drawables, tjs_uint ndrawables,
		const float* verts, tjs_uint vertexCount, const uint16_t* indices, tjs_uint indexCount, TVPGpu::PrimitiveType prim );
	void SetStandardUniforms( tTJSNI_GpuShaderProgram* shader );

public:
	tTJSNI_GpuCanvas();
	~tTJSNI_GpuCanvas() override;
	tjs_error TJS_INTF_METHOD Construct( tjs_int numparams, tTJSVariant** param, iTJSDispatch2* tjs_obj ) override;
	void TJS_INTF_METHOD Invalidate() override;

	//! 画面 (renderTarget 未指定時の描画先) を渡す
	void SetScreenTarget( TVPGpu::Texture* tex, tjs_int width, tjs_int height );
	void BeginDrawing();
	void EndDrawing();

	void SetRenderTargetObject( const tTJSVariant& val );
	const tTJSVariant& GetRenderTargetObject() const { return RenderTargetObject; }
	void SetClipRectObject( const tTJSVariant& val );
	const tTJSVariant& GetClipRectObject() const { return ClipRectObject; }
	void SetMatrix32Object( const tTJSVariant& val );
	const tTJSVariant& GetMatrix32Object() const { return Matrix32Object; }
	void SetDefaultShader( const tTJSVariant& val );
	const tTJSVariant& GetDefaultShader() const { return DefaultShaderObject; }
	void SetDefaultFillShader( const tTJSVariant& val );
	const tTJSVariant& GetDefaultFillShader() const { return DefaultFillShaderObject; }

	// method
	void Capture( class tTVPBaseBitmap* bmp, int x, int y, int w, int h );
	void Capture( tTJSNI_GpuTexture* texture, int x, int y, int w, int h );
	void Clear( tjs_uint32 color );
	void Fill( tjs_int width, tjs_int height, const tjs_uint32 colors[4], tTJSNI_GpuShaderProgram* shader );
	void DrawTexture( const iTVPGpuDrawable* const* textures, tjs_uint count, tTJSNI_GpuShaderProgram* shader );
	void DrawTransition( const iTVPGpuDrawable* front, const iTVPGpuDrawable* back, tjs_real phase,
		const iTVPGpuDrawable* rule, tjs_int vague );
	void DrawTextureAtlas( const tTVPRect& rect, const iTVPGpuDrawable* texture, tTJSNI_GpuShaderProgram* shader );
	void DrawMesh( tTJSNI_GpuShaderProgram* shader, tjs_int primitiveType, tjs_int offset, tjs_int count,
		const tTJSNI_GpuVertexBinder* index );
	void Draw9Patch( tTJSNI_GpuTexture* tex, tjs_int width, tjs_int height, tTVPRect& margin, tTJSNI_GpuShaderProgram* shader );
	void Save();
	void Restore();

	// prop
	void SetClearColor( tjs_uint32 c ) { ClearColor = c; }
	tjs_uint32 GetClearColor() const { return ClearColor; }
	void SetBlendMode( tTVPGpuBlendMode bm ) { BlendMode = bm; }
	tTVPGpuBlendMode GetBlendMode() const { return BlendMode; }
	//! width / height プロパティ (GL 版と同じく画面の大きさ)
	tjs_uint GetWidth() const { return (tjs_uint)SurfaceWidth; }
	tjs_uint GetHeight() const { return (tjs_uint)SurfaceHeight; }
	//! 現在の描画先の大きさ
	tjs_int GetCanvasWidth() const { return CanvasWidth; }
	tjs_int GetCanvasHeight() const { return CanvasHeight; }
	void SetEnableClipRect( bool b ) { EnableClipRect = b; if( InDrawing ) ApplyScissor(); }
	bool GetEnableClipRect() const { return EnableClipRect; }
	void SetEnableCulling( bool b ) { EnableCulling = b; }
	bool GetEnableCulling() const { return EnableCulling; }
};

class tTJSNC_GpuCanvas : public tTJSNativeClass {
public:
	tTJSNC_GpuCanvas();
	static tjs_uint32 ClassID;
protected:
	tTJSNativeInstance* CreateNativeInstance() override { return new tTJSNI_GpuCanvas(); }
};

//---------------------------------------------------------------------------
// 登録用
//---------------------------------------------------------------------------
extern tTJSNativeClass* TVPCreateNativeClass_GpuTexture();
extern tTJSNativeClass* TVPCreateNativeClass_GpuOffscreen();
extern tTJSNativeClass* TVPCreateNativeClass_GpuShaderProgram();
extern tTJSNativeClass* TVPCreateNativeClass_GpuVertexBuffer();
extern tTJSNativeClass* TVPCreateNativeClass_GpuVertexBinder();
extern tTJSNativeClass* TVPCreateNativeClass_GpuCanvas();

#endif
