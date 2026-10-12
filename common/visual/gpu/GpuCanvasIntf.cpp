//---------------------------------------------------------------------------
// Canvas (GPU 版)
//
// GL 版 (common/visual/opengl/CanvasIntf.cpp) と同じ TJS API を TVPGpu::Device の上に作る。
//   - 座標は左上原点。Texture / Offscreen とも UV (0,0) が左上
//   - 組み込みシェーダーは shaders/ のもの (GL 版と同じ GLSL を事前変換)
//   - 未対応 (今後): beginEffect/endEffect、beginMaskClip/endMaskClip、
//     beginStencilClip/endStencilClip、drawText
//---------------------------------------------------------------------------
#include "tjsCommHead.h"

#include "tjsArray.h"
#include "GpuCanvasIntf.h"
#include "GpuShaderLibrary.h"
#include "Matrix32Intf.h"
#include "RectItf.h"
#include "BitmapIntf.h"
#include "LayerBitmapIntf.h"
#include "MsgIntf.h"
#include "CharacterSet.h"
#include "DebugIntf.h"
#include "LogIntf.h"
#include <memory>
#include <algorithm>

using namespace TVPGpu;

// GL の enum 値 (TJS の ptXXX / dtXXX と互換)
enum : tjs_int {
	GL_POINTS_ = 0, GL_LINES_ = 1, GL_LINE_LOOP_ = 2, GL_LINE_STRIP_ = 3,
	GL_TRIANGLES_ = 4, GL_TRIANGLE_STRIP_ = 5, GL_TRIANGLE_FAN_ = 6,
	GL_UNSIGNED_BYTE_ = 0x1401, GL_UNSIGNED_SHORT_ = 0x1403, GL_UNSIGNED_INT_ = 0x1405,
};

//---------------------------------------------------------------------------
// 未指定のサンプラー用の 1x1 白テクスチャ (SDL_GPU は全スロットの束縛が要る)
//---------------------------------------------------------------------------
static Texture* WhiteTexture()
{
	static std::unique_ptr<Texture> tex;
	if( !tex ) {
		TextureDesc d;
		d.width = d.height = 1;
		d.format = TextureFormat::BGRA8;
		tex = Device::Get()->CreateTexture( d );
		const tjs_uint32 white = 0xffffffff;
		Device::Get()->UploadTexture( tex.get(), 0, 0, 0, 1, 1, &white, 4 );
	}
	return tex.get();
}

//---------------------------------------------------------------------------
tTJSNI_GpuCanvas::tTJSNI_GpuCanvas() {}
tTJSNI_GpuCanvas::~tTJSNI_GpuCanvas() {}

//---------------------------------------------------------------------------
// TJS の ShaderProgram オブジェクトを C++ から作る
//---------------------------------------------------------------------------
tTJSVariant tTJSNI_GpuCanvas::CreateShaderObject( const char* vs, const char* fs, bool opacityOne )
{
	tjs_string wvs, wfs;
	TVPUtf8ToUtf16( wvs, std::string( vs ) );
	TVPUtf8ToUtf16( wfs, std::string( fs ) );
	iTJSDispatch2* cls = new tTJSNC_GpuShaderProgram();
	iTJSDispatch2* obj = nullptr;
	tTJSVariant result;
	try {
		tTJSVariant param[4] = { ttstr( wvs.c_str() ), ttstr( wfs.c_str() ), (tjs_int)0, (tjs_int)0 };
		tTJSVariant* pparam[4] = { param, param + 1, param + 2, param + 3 };
		if( TJS_FAILED( cls->CreateNew( 0, NULL, NULL, &obj, 4, pparam, cls ) ) )
			TVPThrowExceptionMessage( TVPInternalError, TJS_W("tTJSNI_GpuShaderProgram::Construct") );
		if( opacityOne ) {
			// a_opacity の既定値は 1.0 (未設定だと 0 = 完全透明で何も見えない)
			tTJSVariant opacity( 1.0 );
			static ttstr name( TJS_W("a_opacity") );
			obj->PropSet( TJS_MEMBERENSURE, name.c_str(), name.GetHint(), &opacity, obj );
		}
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

static tTJSNI_GpuShaderProgram* ShaderOf( const tTJSVariant& v )
{
	return (tTJSNI_GpuShaderProgram*)TJSGetNativeInstance( tTJSNC_GpuShaderProgram::ClassID, &v );
}

//---------------------------------------------------------------------------
tjs_error TJS_INTF_METHOD tTJSNI_GpuCanvas::Construct( tjs_int numparams, tTJSVariant** param, iTJSDispatch2* tjs_obj )
{
	// 既定の行列
	{
		iTJSDispatch2* cls = new tTJSNC_Matrix32();
		iTJSDispatch2* obj = nullptr;
		try {
			if( TJS_FAILED( cls->CreateNew( 0, NULL, NULL, &obj, 0, NULL, cls ) ) )
				TVPThrowExceptionMessage( TVPInternalError, TJS_W("tTJSNI_Matrix32::Construct") );
			SetMatrix32Object( tTJSVariant( obj, obj ) );
		} catch( ... ) {
			cls->Release();
			if( obj ) obj->Release();
			throw;
		}
		cls->Release();
		if( obj ) obj->Release();
	}
	// 既定のシェーダー
	EmbeddedDefaultShaderObject = CreateShaderObject(
		ShaderLibrary::BuiltinSource( "default", ShaderStage::Vertex ),
		ShaderLibrary::BuiltinSource( "default", ShaderStage::Fragment ), true );
	SetDefaultShader( EmbeddedDefaultShaderObject );
	EmbeddedDefaultFillShaderObject = CreateShaderObject(
		ShaderLibrary::BuiltinSource( "fill", ShaderStage::Vertex ),
		ShaderLibrary::BuiltinSource( "fill", ShaderStage::Fragment ), false );
	SetDefaultFillShader( EmbeddedDefaultFillShaderObject );
	return TJS_S_OK;
}

void TJS_INTF_METHOD tTJSNI_GpuCanvas::Invalidate()
{
	auto inval = []( tTJSVariant& v ) {
		if( v.Type() == tvtObject ) v.AsObjectClosureNoAddRef().Invalidate( 0, NULL, NULL, v.AsObjectNoAddRef() );
		v.Clear();
	};
	inval( ClipRectObject ); ClipRectInstance = nullptr;
	inval( Matrix32Object ); Matrix32Instance = nullptr;
	RenderTargetObject.Clear(); RenderTargetInstance = nullptr;
	DefaultShaderObject.Clear(); DefaultShaderInstance = nullptr;
	DefaultFillShaderObject.Clear(); DefaultFillShaderInstance = nullptr;
	inval( EmbeddedDefaultShaderObject );
	inval( EmbeddedDefaultFillShaderObject );
	for( int i = 0; i < 2; i++ ) { inval( TransitionShaderObject[i] ); TransitionShaderInstance[i] = nullptr; }
	ScreenTarget = nullptr;
	CurrentTarget = nullptr;
}

//---------------------------------------------------------------------------
// 描画先
//---------------------------------------------------------------------------
void tTJSNI_GpuCanvas::SetScreenTarget( Texture* tex, tjs_int width, tjs_int height )
{
	ScreenTarget = tex;
	SurfaceWidth = width;
	SurfaceHeight = height;
}

void tTJSNI_GpuCanvas::BindTarget()
{
	Texture* target = nullptr;
	if( RenderTargetInstance ) {
		target = RenderTargetInstance->GetGpuTexture();
		CanvasWidth = RenderTargetInstance->GetWidth();
		CanvasHeight = RenderTargetInstance->GetHeight();
	} else {
		target = ScreenTarget;
		CanvasWidth = SurfaceWidth;
		CanvasHeight = SurfaceHeight;
	}
	CurrentTarget = target;
	if( !target ) return;
	Device* dev = Device::Get();
	dev->BeginPass( target, nullptr );
	dev->SetViewport( { 0, 0, CanvasWidth, CanvasHeight } );
	ApplyScissor();
}

void tTJSNI_GpuCanvas::ApplyScissor()
{
	if( !CurrentTarget ) return;
	Device* dev = Device::Get();
	if( EnableClipRect && ClipRectInstance ) {
		tTVPRect r = ClipRectInstance->Get();
		// 描画先の範囲に収める (SDL_GPU は範囲外の scissor を許さない)
		r.left = std::max( 0, r.left ); r.top = std::max( 0, r.top );
		r.right = std::min( (tjs_int)CurrentTarget->Width(), r.right );
		r.bottom = std::min( (tjs_int)CurrentTarget->Height(), r.bottom );
		Rect sr = { r.left, r.top, std::max( 0, r.get_width() ), std::max( 0, r.get_height() ) };
		dev->SetScissor( &sr );
	} else {
		dev->SetScissor( nullptr );
	}
}

void tTJSNI_GpuCanvas::BeginDrawing()
{
	InDrawing = true;
	StateStack.clear();
	BindTarget();
	if( CurrentTarget ) {
		tjs_uint32 c = ClearColor;
		const float rgba[4] = { ( ( c >> 16 ) & 0xff ) / 255.0f, ( ( c >> 8 ) & 0xff ) / 255.0f, ( c & 0xff ) / 255.0f, ( ( c >> 24 ) & 0xff ) / 255.0f };
		Device::Get()->ClearColor( rgba );
	}
}

void tTJSNI_GpuCanvas::EndDrawing()
{
	StateStack.clear();
	InDrawing = false;
	if( CurrentTarget ) {
		Device::Get()->EndPass();
		Device::Get()->Submit();
	}
	CurrentTarget = nullptr;
}

//---------------------------------------------------------------------------
// プロパティ
//---------------------------------------------------------------------------
void tTJSNI_GpuCanvas::SetRenderTargetObject( const tTJSVariant& val )
{
	RenderTargetObject = val;
	RenderTargetInstance = nullptr;
	if( val.Type() == tvtObject && val.AsObjectNoAddRef() ) {
		RenderTargetInstance = (tTJSNI_GpuOffscreen*)TJSGetNativeInstance( tTJSNC_GpuOffscreen::ClassID, &val );
		if( !RenderTargetInstance ) TVPThrowExceptionMessage( TVPCannotRetriveInstance, TJS_W("Offscreen") );
	}
	// 描画中ならその場で描画先を切り替える
	if( InDrawing ) BindTarget();
}

void tTJSNI_GpuCanvas::SetClipRectObject( const tTJSVariant& val )
{
	ClipRectObject = val;
	ClipRectInstance = nullptr;
	if( val.Type() == tvtObject && val.AsObjectNoAddRef() ) {
		ClipRectInstance = (tTJSNI_Rect*)TJSGetNativeInstance( tTJSNC_Rect::ClassID, &val );
		if( !ClipRectInstance ) TVPThrowExceptionMessage( TVPCannotRetriveInstance, TJS_W("rect") );
	}
	if( InDrawing ) ApplyScissor();
}

void tTJSNI_GpuCanvas::SetMatrix32Object( const tTJSVariant& val )
{
	Matrix32Object = val;
	Matrix32Instance = nullptr;
	if( val.Type() == tvtObject && val.AsObjectNoAddRef() ) {
		Matrix32Instance = (tTJSNI_Matrix32*)TJSGetNativeInstance( tTJSNC_Matrix32::ClassID, &val );
		if( !Matrix32Instance ) TVPThrowExceptionMessage( TVPCannotRetriveInstance, TJS_W("matrix") );
	}
}

void tTJSNI_GpuCanvas::SetDefaultShader( const tTJSVariant& val )
{
	DefaultShaderObject = val.Type() == tvtVoid ? EmbeddedDefaultShaderObject : val;
	DefaultShaderInstance = nullptr;
	if( DefaultShaderObject.Type() == tvtObject ) {
		DefaultShaderInstance = ShaderOf( DefaultShaderObject );
		if( !DefaultShaderInstance ) TVPThrowExceptionMessage( TVPCannotRetriveInstance, TJS_W("shader") );
	}
}

void tTJSNI_GpuCanvas::SetDefaultFillShader( const tTJSVariant& val )
{
	DefaultFillShaderObject = val.Type() == tvtVoid ? EmbeddedDefaultFillShaderObject : val;
	DefaultFillShaderInstance = nullptr;
	if( DefaultFillShaderObject.Type() == tvtObject ) {
		DefaultFillShaderInstance = ShaderOf( DefaultFillShaderObject );
		if( !DefaultFillShaderInstance ) TVPThrowExceptionMessage( TVPCannotRetriveInstance, TJS_W("shader") );
	}
}

tTJSNI_GpuShaderProgram* tTJSNI_GpuCanvas::EnsureTransitionShader( bool universal )
{
	const int i = universal ? 1 : 0;
	if( !TransitionShaderInstance[i] ) {
		const char* name = universal ? "transition_universal" : "transition_crossfade";
		TransitionShaderObject[i] = CreateShaderObject(
			ShaderLibrary::BuiltinSource( name, ShaderStage::Vertex ),
			ShaderLibrary::BuiltinSource( name, ShaderStage::Fragment ), true );
		TransitionShaderInstance[i] = ShaderOf( TransitionShaderObject[i] );
	}
	return TransitionShaderInstance[i];
}

//---------------------------------------------------------------------------
// 描画の共通部分
//---------------------------------------------------------------------------
BlendState tTJSNI_GpuCanvas::MakeBlendState() const
{
	BlendState b;
	b.enable = BlendMode != tTVPGpuBlendMode::bmDisable;
	auto set = [&b]( BlendOp cop, BlendFactor sc, BlendFactor dc, BlendOp aop, BlendFactor sa, BlendFactor da ) {
		b.colorOp = cop; b.srcColor = sc; b.dstColor = dc;
		b.alphaOp = aop; b.srcAlpha = sa; b.dstAlpha = da;
	};
	switch( BlendMode ) {
	case tTVPGpuBlendMode::bmOpaque:
		set( BlendOp::Add, BlendFactor::One, BlendFactor::Zero, BlendOp::Add, BlendFactor::One, BlendFactor::Zero ); break;
	case tTVPGpuBlendMode::bmAdd:
		set( BlendOp::Add, BlendFactor::SrcAlpha, BlendFactor::One, BlendOp::Add, BlendFactor::Zero, BlendFactor::One ); break;
	case tTVPGpuBlendMode::bmAddWithAlpha:
		set( BlendOp::Add, BlendFactor::SrcAlpha, BlendFactor::One, BlendOp::Add, BlendFactor::SrcAlpha, BlendFactor::One ); break;
	case tTVPGpuBlendMode::bmSubtract:
		set( BlendOp::ReverseSubtract, BlendFactor::SrcAlpha, BlendFactor::One, BlendOp::Add, BlendFactor::Zero, BlendFactor::One ); break;
	case tTVPGpuBlendMode::bmMultiply:
		set( BlendOp::Add, BlendFactor::DstColor, BlendFactor::OneMinusSrcAlpha, BlendOp::Add, BlendFactor::Zero, BlendFactor::One ); break;
	case tTVPGpuBlendMode::bmMin:
		set( BlendOp::Min, BlendFactor::One, BlendFactor::One, BlendOp::Min, BlendFactor::One, BlendFactor::One ); break;
	case tTVPGpuBlendMode::bmMax:
		set( BlendOp::Max, BlendFactor::One, BlendFactor::One, BlendOp::Max, BlendFactor::One, BlendFactor::One ); break;
	case tTVPGpuBlendMode::bmScreen:
		set( BlendOp::Add, BlendFactor::OneMinusDstColor, BlendFactor::One, BlendOp::Add, BlendFactor::Zero, BlendFactor::One ); break;
	case tTVPGpuBlendMode::bmAlpha:
	default:
		set( BlendOp::Add, BlendFactor::SrcAlpha, BlendFactor::OneMinusSrcAlpha, BlendOp::Add, BlendFactor::One, BlendFactor::OneMinusSrcAlpha ); break;
	}
	return b;
}

void tTJSNI_GpuCanvas::SetStandardUniforms( tTJSNI_GpuShaderProgram* shader )
{
	static const float identity[16] = { 1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1 };
	shader->SetUniformFloats( "a_modelMat4", Matrix32Instance ? Matrix32Instance->GetMatrixArray16() : identity, 16 );
	const float size[2] = { (float)CanvasWidth, (float)CanvasHeight };
	shader->SetUniformFloats( "a_size", size, 2 );
}

//! uniform ブロック・サンプラー・ブレンドなど、描画 1 回ぶんの共通設定
void tTJSNI_GpuCanvas::FillCommon( DrawCall& dc, tTJSNI_GpuShaderProgram* shader )
{
	const ShaderReflection& r = shader->Reflection();
	dc.state.program = shader->GetProgram();
	dc.state.blend = MakeBlendState();
	dc.state.cull = EnableCulling ? CullMode::Back : CullMode::None;
	for( int s = 0; s < 2; s++ ) {
		const std::vector<uint8_t>& blk = shader->GetBlock( (ShaderStage)s );
		dc.uniforms[s] = blk.empty() ? nullptr : blk.data();
		dc.uniformSize[s] = (uint32_t)blk.size();
	}
	dc.textureCount = std::min<uint32_t>( r.samplerCount[1], MaxTextures );
	for( uint32_t i = 0; i < dc.textureCount; i++ ) {
		iTVPGpuDrawable* d = shader->GetSamplerValue( i );
		if( d && d->GetGpuTexture() ) {
			dc.textures[i] = d->GetGpuTexture();
			dc.samplers[i] = d->GetSamplerDesc();
		} else {
			dc.textures[i] = WhiteTexture();
		}
	}
}

//! 位置 (float2) + UV (float2) の頂点で描く
void tTJSNI_GpuCanvas::DrawTexturedQuads( tTJSNI_GpuShaderProgram* shader, const iTVPGpuDrawable* const* drawables, tjs_uint ndrawables,
	const float* verts, tjs_uint vertexCount, const uint16_t* indices, tjs_uint indexCount, PrimitiveType prim )
{
	if( !CurrentTarget ) return;
	const ShaderReflection& r = shader->Reflection();
	const ShaderReflection::Attribute* pos = r.FindAttribute( "a_pos" );
	if( !pos ) TVPThrowExceptionMessage( TVPNotFoundInShader, TJS_W("a_pos") );
	const ShaderReflection::Attribute* uv = r.FindAttribute( "a_texCoord" );
	if( !uv ) TVPThrowExceptionMessage( TVPNotFoundInShader, TJS_W("a_texCoord") );

	SetStandardUniforms( shader );
	DrawCall dc;
	FillCommon( dc, shader );
	static const char* names[3] = { "s_tex0", "s_tex1", "s_tex2" };
	for( tjs_uint i = 0; i < ndrawables && i < 3; i++ ) {
		const ShaderReflection::Sampler* s = r.FindSampler( names[i] );
		if( !s ) {
			ttstr n( names[i] );
			TVPThrowExceptionMessage( TVPNotFoundInShader, n );
		}
		if( s->stage != ShaderStage::Fragment || s->slot >= dc.textureCount ) continue;
		dc.textures[s->slot] = drawables[i]->GetGpuTexture();
		dc.samplers[s->slot] = drawables[i]->GetSamplerDesc();
	}
	dc.state.attributes.push_back( { pos->location, VertexFormat::Float2, 0, 0 } );
	dc.state.attributes.push_back( { uv->location, VertexFormat::Float2, 8, 0 } );
	dc.state.strides[0] = 16;
	dc.state.vertexBufferCount = 1;
	dc.state.primitive = prim;

	Device* dev = Device::Get();
	dev->AllocTransient( false, verts, vertexCount * 16, &dc.vertexBuffers[0], &dc.vertexOffsets[0] );
	if( indices ) {
		dev->AllocTransient( true, indices, indexCount * 2, &dc.indexBuffer, &dc.indexOffset );
		dc.indexType = IndexType::U16;
		dc.count = indexCount;
	} else {
		dc.count = vertexCount;
	}
	dev->Draw( dc );
}

//---------------------------------------------------------------------------
// 描画メソッド
//---------------------------------------------------------------------------
void tTJSNI_GpuCanvas::Clear( tjs_uint32 color )
{
	if( !CurrentTarget ) return;
	const float rgba[4] = { ( ( color >> 16 ) & 0xff ) / 255.0f, ( ( color >> 8 ) & 0xff ) / 255.0f, ( color & 0xff ) / 255.0f, ( ( color >> 24 ) & 0xff ) / 255.0f };
	if( !( EnableClipRect && ClipRectInstance ) ) {
		Device::Get()->ClearColor( rgba );
		return;
	}
	// 矩形クリップ中は GL の glClear と同じく範囲内だけを塗る (ブレンド無しの全面描画)
	tTJSNI_GpuShaderProgram* shader = ShaderOf( EmbeddedDefaultFillShaderObject );
	if( !shader ) return;
	const ShaderReflection::Attribute* pos = shader->Reflection().FindAttribute( "a_pos" );
	const ShaderReflection::Attribute* col = shader->Reflection().FindAttribute( "a_color" );
	if( !pos || !col ) return;
	static const float identity[16] = { 1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1 };
	shader->SetUniformFloats( "a_modelMat4", identity, 16 );
	const float size[2] = { (float)CanvasWidth, (float)CanvasHeight };
	shader->SetUniformFloats( "a_size", size, 2 );
	const float w = (float)CanvasWidth, h = (float)CanvasHeight;
	float v[4 * 6];
	const float xy[4][2] = { { 0, 0 }, { 0, h }, { w, 0 }, { w, h } };
	for( int i = 0; i < 4; i++ ) {
		v[i*6+0] = xy[i][0]; v[i*6+1] = xy[i][1];
		v[i*6+2] = rgba[0]; v[i*6+3] = rgba[1]; v[i*6+4] = rgba[2]; v[i*6+5] = rgba[3];
	}
	DrawCall dc;
	FillCommon( dc, shader );
	dc.state.blend = BlendState();   // ブレンド無し (書き込みのみ)
	dc.state.cull = CullMode::None;
	dc.state.attributes.push_back( { pos->location, VertexFormat::Float2, 0, 0 } );
	dc.state.attributes.push_back( { col->location, VertexFormat::Float4, 8, 0 } );
	dc.state.strides[0] = 24;
	dc.state.vertexBufferCount = 1;
	dc.state.primitive = PrimitiveType::TriangleStrip;
	Device::Get()->AllocTransient( false, v, sizeof(v), &dc.vertexBuffers[0], &dc.vertexOffsets[0] );
	dc.count = 4;
	Device::Get()->Draw( dc );
}

void tTJSNI_GpuCanvas::Fill( tjs_int width, tjs_int height, const tjs_uint32 colors[4], tTJSNI_GpuShaderProgram* shader )
{
	if( !CurrentTarget ) return;
	if( !shader ) shader = DefaultFillShaderInstance;
	if( !shader ) return;
	const ShaderReflection& r = shader->Reflection();
	const ShaderReflection::Attribute* pos = r.FindAttribute( "a_pos" );
	if( !pos ) TVPThrowExceptionMessage( TVPNotFoundInShader, TJS_W("a_pos") );
	const ShaderReflection::Attribute* col = r.FindAttribute( "a_color" );
	if( !col ) TVPThrowExceptionMessage( TVPNotFoundInShader, TJS_W("a_color") );

	const float w = (float)width, h = (float)height;
	const float xy[4][2] = { { 0, 0 }, { 0, h }, { w, 0 }, { w, h } };   // 左上・左下・右上・右下
	float v[4 * 6];
	for( int i = 0; i < 4; i++ ) {
		const tjs_uint32 c = colors[i];
		v[i*6+0] = xy[i][0]; v[i*6+1] = xy[i][1];
		v[i*6+2] = ( ( c >> 16 ) & 0xff ) / 255.0f;
		v[i*6+3] = ( ( c >> 8 ) & 0xff ) / 255.0f;
		v[i*6+4] = ( c & 0xff ) / 255.0f;
		v[i*6+5] = ( ( c >> 24 ) & 0xff ) / 255.0f;
	}
	SetStandardUniforms( shader );
	DrawCall dc;
	FillCommon( dc, shader );
	dc.state.attributes.push_back( { pos->location, VertexFormat::Float2, 0, 0 } );
	dc.state.attributes.push_back( { col->location, VertexFormat::Float4, 8, 0 } );
	dc.state.strides[0] = 24;
	dc.state.vertexBufferCount = 1;
	dc.state.primitive = PrimitiveType::TriangleStrip;
	Device::Get()->AllocTransient( false, v, sizeof(v), &dc.vertexBuffers[0], &dc.vertexOffsets[0] );
	dc.count = 4;
	Device::Get()->Draw( dc );
}

void tTJSNI_GpuCanvas::DrawTexture( const iTVPGpuDrawable* const* textures, tjs_uint count, tTJSNI_GpuShaderProgram* shader )
{
	if( !count || !textures[0] ) return;
	if( !shader ) shader = DefaultShaderInstance;
	if( !shader ) return;
	// 描画サイズ (Texture.setDrawSize) と実体の大きさの比で UV を決める
	const iTVPGpuDrawable* t = textures[0];
	const float w = (float)t->GetWidth(), h = (float)t->GetHeight();
	const float u1 = t->GetMemoryWidth() ? std::min( 1.0f, w / (float)t->GetMemoryWidth() ) : 1.0f;
	const float v1 = t->GetMemoryHeight() ? std::min( 1.0f, h / (float)t->GetMemoryHeight() ) : 1.0f;
	const float verts[] = {
		0, 0, 0,  0,
		0, h, 0,  v1,
		w, 0, u1, 0,
		w, h, u1, v1,
	};
	DrawTexturedQuads( shader, textures, count, verts, 4, nullptr, 0, PrimitiveType::TriangleStrip );
}

void tTJSNI_GpuCanvas::DrawTransition( const iTVPGpuDrawable* front, const iTVPGpuDrawable* back, tjs_real phase,
	const iTVPGpuDrawable* rule, tjs_int vague )
{
	const bool universal = rule != nullptr;
	tTJSNI_GpuShaderProgram* shader = EnsureTransitionShader( universal );
	if( !shader ) return;
	float p = (float)std::min( 1.0, std::max( 0.0, (double)phase ) );
	shader->SetUniformFloats( "a_phase", &p, 1 );
	const iTVPGpuDrawable* tex[3] = { front, back, rule };
	if( universal ) {
		vague = std::min<tjs_int>( 255, std::max<tjs_int>( 1, vague ) );
		float vv = vague / 255.0f;
		shader->SetUniformFloats( "a_vague", &vv, 1 );
		DrawTexture( tex, 3, shader );
	} else {
		DrawTexture( tex, 2, shader );
	}
}

void tTJSNI_GpuCanvas::DrawTextureAtlas( const tTVPRect& rect, const iTVPGpuDrawable* texture, tTJSNI_GpuShaderProgram* shader )
{
	if( !texture ) return;
	if( !shader ) shader = DefaultShaderInstance;
	if( !shader ) return;
	const float tw = (float)texture->GetMemoryWidth(), th = (float)texture->GetMemoryHeight();
	tTVPRect r = rect;
	if( r.top < 0 ) r.top = 0;
	if( r.left < 0 ) r.left = 0;
	if( r.right > (tjs_int)texture->GetWidth() ) r.right = (tjs_int)texture->GetWidth();
	if( r.bottom > (tjs_int)texture->GetHeight() ) r.bottom = (tjs_int)texture->GetHeight();
	const float w = (float)r.get_width(), h = (float)r.get_height();
	const float u0 = r.left / tw, u1 = r.right / tw, v0 = r.top / th, v1 = r.bottom / th;
	const float verts[] = {
		0, 0, u0, v0,
		0, h, u0, v1,
		w, 0, u1, v0,
		w, h, u1, v1,
	};
	const iTVPGpuDrawable* tex[1] = { texture };
	DrawTexturedQuads( shader, tex, 1, verts, 4, nullptr, 0, PrimitiveType::TriangleStrip );
}

//---------------------------------------------------------------------------
// 9patch: 3x3 の領域それぞれを別の四角形で描く (隣の領域を拾わないように UV を内側に寄せる)
//---------------------------------------------------------------------------
void tTJSNI_GpuCanvas::Draw9Patch( tTJSNI_GpuTexture* tex, tjs_int width, tjs_int height, tTVPRect& margin, tTJSNI_GpuShaderProgram* shader )
{
	if( !tex ) return;
	if( !shader ) shader = DefaultShaderInstance;
	if( !shader ) return;
	const float tw = (float)tex->GetMemoryWidth(), th = (float)tex->GetMemoryHeight();
	const float sw = (float)tex->GetWidth(), sh = (float)tex->GetHeight();
	const tTVPRect scale = tex->GetScale9Patch();
	margin = tex->GetMargin9Patch();
	// 列・行ごとの UV (領域の端 2 つずつ。GL 版と同じ寄せ方) と出力位置
	const float us[6] = { 0.0f, ( scale.left - 1 ) / tw, scale.left / tw, ( scale.right - 1 ) / tw, scale.right / tw, sw / tw };
	const float vs[6] = { 0.0f, ( scale.top - 1 ) / th, scale.top / th, ( scale.bottom - 1 ) / th, scale.bottom / th, sh / th };
	const float xs[6] = { 0.0f, (float)scale.left, (float)scale.left, width - ( sw - scale.right ), width - ( sw - scale.right ), (float)width };
	const float ys[6] = { 0.0f, (float)scale.top, (float)scale.top, height - ( sh - scale.bottom ), height - ( sh - scale.bottom ), (float)height };
	float verts[36 * 4];
	for( int c = 0; c < 6; c++ ) {
		for( int rr = 0; rr < 6; rr++ ) {
			float* v = &verts[( c * 6 + rr ) * 4];
			v[0] = xs[c]; v[1] = ys[rr]; v[2] = us[c]; v[3] = vs[rr];
		}
	}
	uint16_t idx[54];
	int n = 0;
	for( int cb = 0; cb < 3; cb++ ) {          // 列ブロック (左・中・右)
		for( int rb = 0; rb < 3; rb++ ) {      // 行ブロック (上・中・下)
			const uint16_t tl = (uint16_t)( ( cb * 2 ) * 6 + rb * 2 );
			const uint16_t bl = tl + 1;
			const uint16_t tr = (uint16_t)( ( cb * 2 + 1 ) * 6 + rb * 2 );
			const uint16_t br = tr + 1;
			idx[n++] = tl; idx[n++] = bl; idx[n++] = tr;
			idx[n++] = bl; idx[n++] = br; idx[n++] = tr;
		}
	}
	const iTVPGpuDrawable* t[1] = { tex };
	DrawTexturedQuads( shader, t, 1, verts, 36, idx, n, PrimitiveType::TriangleList );
}

//---------------------------------------------------------------------------
// drawMesh: 頂点属性 (VertexBinder) とテクスチャはシェーダーのプロパティから
//---------------------------------------------------------------------------
static bool ToPrimitive( tjs_int glType, PrimitiveType& out )
{
	switch( glType ) {
	case GL_POINTS_: out = PrimitiveType::PointList; return true;
	case GL_LINES_: out = PrimitiveType::LineList; return true;
	case GL_LINE_STRIP_: out = PrimitiveType::LineStrip; return true;
	case GL_TRIANGLES_: out = PrimitiveType::TriangleList; return true;
	case GL_TRIANGLE_STRIP_: out = PrimitiveType::TriangleStrip; return true;
	}
	return false;   // LINE_LOOP / TRIANGLE_FAN はインデックス展開する
}

//! LINE_LOOP → LINE_LIST、TRIANGLE_FAN → TRIANGLE_LIST に展開したインデックス
static void ExpandIndices( tjs_int glType, const std::vector<uint32_t>& src, std::vector<uint32_t>& out )
{
	out.clear();
	const size_t n = src.size();
	if( glType == GL_LINE_LOOP_ ) {
		if( n < 2 ) return;
		for( size_t i = 0; i < n; i++ ) { out.push_back( src[i] ); out.push_back( src[( i + 1 ) % n] ); }
	} else if( glType == GL_TRIANGLE_FAN_ ) {
		for( size_t i = 1; i + 1 < n; i++ ) { out.push_back( src[0] ); out.push_back( src[i] ); out.push_back( src[i + 1] ); }
	}
}

void tTJSNI_GpuCanvas::DrawMesh( tTJSNI_GpuShaderProgram* shader, tjs_int primitiveType, tjs_int offset, tjs_int count,
	const tTJSNI_GpuVertexBinder* index )
{
	if( !CurrentTarget || !shader || count <= 0 ) return;
	const ShaderReflection& r = shader->Reflection();
	SetStandardUniforms( shader );

	// 点の大きさが無い環境 (D3D12) 向けに gpushaderc.py が点を四角形に広げる形へ
	// 書き換えたシェーダーは、角の属性 _kz_corner と画面サイズ _kz_viewport を持つ。
	// 点のリストを描くときは、4 頂点の四角形を点の数だけインスタンス描画する
	// (スクリプトの属性はインスタンスごとに進める)。
	const ShaderReflection::Attribute* corner = r.FindAttribute( "_kz_corner" );
	const bool pointSprite = corner && primitiveType == GL_POINTS_ && !index;
	if( corner ) {
		const float vp[2] = { (float)CurrentTarget->Width(), (float)CurrentTarget->Height() };
		shader->SetUniformFloats( "_kz_viewport", vp, 2 );
	}

	DrawCall dc;
	FillCommon( dc, shader );

	// 頂点属性: binder ごとに 1 スロット
	for( const auto& a : r.attributes ) {
		if( corner && &a == corner ) continue;   // 角の属性はここで足す (下)
		const tTJSVariant* v = shader->GetAttributeValue( a.name );
		if( !v ) {
			ttstr n( a.name.c_str() );
			TVPThrowExceptionMessage( TVPNotFoundInShader, n );
		}
		const tTJSNI_GpuVertexBinder* b = (const tTJSNI_GpuVertexBinder*)TJSGetNativeInstance( tTJSNC_GpuVertexBinder::ClassID, v );
		if( !b || !b->GetVertexBuffer() || !b->GetVertexBuffer()->GetBuffer() ) TVPThrowExceptionMessage( TVPParameterRequireClassInstance, TJS_W("VertexBinder") );
		const VertexFormat fmt = b->GetVertexFormat();
		if( fmt == VertexFormat::Invalid ) TVPThrowExceptionMessage( TVPUnknownDataType );
		const uint32_t slot = dc.state.vertexBufferCount++;
		if( slot >= MaxVertexBuffers ) TVPThrowExceptionMessage( TJS_W("drawMesh: too many vertex attributes") );
		// stride 0 は詰めて並んでいる (GL と同じ)
		uint32_t elemSize = 4;
		switch( b->GetVertexBuffer()->GetDataType() ) {
		case 0x1400: case 0x1401: elemSize = 1; break;
		case 0x1402: case 0x1403: elemSize = 2; break;
		}
		dc.state.strides[slot] = b->GetStride() ? (uint32_t)b->GetStride() : elemSize * b->GetComponentCount();
		dc.state.attributes.push_back( { a.location, fmt, 0, slot } );
		dc.vertexBuffers[slot] = b->GetVertexBuffer()->GetBuffer();
		dc.vertexOffsets[slot] = b->GetOffset();
		dc.state.instanceRate[slot] = pointSprite ? 1 : 0;
	}

	Device* dev = Device::Get();
	if( corner ) {
		// 四角形の角 (三角形ストリップの順)。点として描くときは使われないが、
		// 頂点入力の定義を揃えるため常に結び付ける
		static const float corners[8] = { -1, -1,  1, -1,  -1, 1,  1, 1 };
		const uint32_t slot = dc.state.vertexBufferCount++;
		if( slot >= MaxVertexBuffers ) TVPThrowExceptionMessage( TJS_W("drawMesh: too many vertex attributes") );
		dc.state.strides[slot] = sizeof(float) * 2;
		dc.state.attributes.push_back( { corner->location, VertexFormat::Float2, 0, slot } );
		dev->AllocTransient( false, corners, sizeof(corners), &dc.vertexBuffers[slot], &dc.vertexOffsets[slot] );
	}
	if( pointSprite ) {
		dc.state.primitive = PrimitiveType::TriangleStrip;
		dc.count = 4;
		dc.instanceCount = (uint32_t)count;
		dc.firstInstance = (uint32_t)offset;
		dev->Draw( dc );
		return;
	}

	PrimitiveType prim;
	const bool direct = ToPrimitive( primitiveType, prim );
	if( !direct ) {
		if( primitiveType != GL_LINE_LOOP_ && primitiveType != GL_TRIANGLE_FAN_ ) TVPThrowExceptionMessage( TJS_W("drawMesh: unknown primitive type") );
		prim = primitiveType == GL_LINE_LOOP_ ? PrimitiveType::LineList : PrimitiveType::TriangleList;
	}
	dc.state.primitive = prim;

	if( index ) {
		const tTJSNI_GpuVertexBuffer* ib = index->GetVertexBuffer();
		if( !ib || !ib->GetBuffer() ) TVPThrowExceptionMessage( TVPParameterRequireClassInstance, TJS_W("VertexBinder") );
		const tjs_int type = ib->GetDataType();
		if( type != GL_UNSIGNED_BYTE_ && type != GL_UNSIGNED_SHORT_ && type != GL_UNSIGNED_INT_ ) TVPThrowExceptionMessage( TVPIndexTypeIsInvalid );
		// 8bit は 16bit で格納している (offset は GL の単位 = 要素数 x 1 byte)
		const bool u32 = type == GL_UNSIGNED_INT_;
		const uint32_t esize = u32 ? 4 : 2;
		const uint32_t glsize = type == GL_UNSIGNED_BYTE_ ? 1 : esize;
		const uint32_t firstIndex = index->GetOffset() / glsize;
		if( direct ) {
			dc.indexBuffer = ib->GetBuffer();
			dc.indexType = u32 ? IndexType::U32 : IndexType::U16;
			dc.indexOffset = firstIndex * esize;
			dc.count = count;
		} else {
			const std::vector<uint8_t>& sh = ib->GetShadow();
			std::vector<uint32_t> src, out;
			for( tjs_int i = 0; i < count; i++ ) {
				const size_t at = ( (size_t)firstIndex + i ) * esize;
				if( at + esize > sh.size() ) break;
				src.push_back( u32 ? *(const uint32_t*)&sh[at] : *(const uint16_t*)&sh[at] );
			}
			ExpandIndices( primitiveType, src, out );
			if( out.empty() ) return;
			dev->AllocTransient( true, out.data(), (uint32_t)( out.size() * 4 ), &dc.indexBuffer, &dc.indexOffset );
			dc.indexType = IndexType::U32;
			dc.count = (uint32_t)out.size();
		}
	} else if( direct ) {
		dc.first = offset;
		dc.count = count;
	} else {
		std::vector<uint32_t> src( count ), out;
		for( tjs_int i = 0; i < count; i++ ) src[i] = offset + i;
		ExpandIndices( primitiveType, src, out );
		if( out.empty() ) return;
		dev->AllocTransient( true, out.data(), (uint32_t)( out.size() * 4 ), &dc.indexBuffer, &dc.indexOffset );
		dc.indexType = IndexType::U32;
		dc.count = (uint32_t)out.size();
	}
	dev->Draw( dc );
}

//---------------------------------------------------------------------------
// 読み戻し
//---------------------------------------------------------------------------
void tTJSNI_GpuCanvas::Capture( tTVPBaseBitmap* bmp, int x, int y, int w, int h )
{
	if( !CurrentTarget ) TVPThrowExceptionMessage( TVPCaptureAreaOver );
	std::vector<tjs_uint32> buf( (size_t)w * h );
	if( !Device::Get()->ReadPixels( CurrentTarget, x, y, w, h, buf.data(), w * 4 ) ) return;
	for( int row = 0; row < h; row++ )
		memcpy( bmp->GetScanLineForWrite( row ), &buf[(size_t)w * row], w * 4 );
}

void tTJSNI_GpuCanvas::Capture( tTJSNI_GpuTexture* texture, int x, int y, int w, int h )
{
	if( !CurrentTarget || !texture || !texture->GetGpuTexture() ) return;
	Device::Get()->CopyTexture( CurrentTarget, x, y, texture->GetGpuTexture(), 0, 0, w, h );
}

//---------------------------------------------------------------------------
void tTJSNI_GpuCanvas::Save()
{
	State s;
	if( Matrix32Instance ) memcpy( s.Matrix, Matrix32Instance->GetMatrixArray(), sizeof(s.Matrix) );
	else { static const float id[6] = { 1, 0, 0, 1, 0, 0 }; memcpy( s.Matrix, id, sizeof(s.Matrix) ); }
	if( ClipRectInstance ) {
		const tTVPRect& r = ClipRectInstance->Get();
		s.ClipRect[0] = r.left; s.ClipRect[1] = r.top; s.ClipRect[2] = r.right; s.ClipRect[3] = r.bottom;
	} else {
		memset( s.ClipRect, 0, sizeof(s.ClipRect) );
	}
	s.EnableClip = EnableClipRect;
	s.EnableCulling = EnableCulling;
	StateStack.push_back( s );
}

void tTJSNI_GpuCanvas::Restore()
{
	if( StateStack.empty() ) return;
	State& s = StateStack.back();
	if( Matrix32Instance ) Matrix32Instance->Set( s.Matrix );
	if( ClipRectInstance ) ClipRectInstance->Set( s.ClipRect );
	EnableClipRect = s.EnableClip;
	EnableCulling = s.EnableCulling;
	StateStack.pop_back();
	if( InDrawing ) ApplyScissor();
}

//---------------------------------------------------------------------------
// tTJSNC_GpuCanvas
//---------------------------------------------------------------------------
static tTJSNI_GpuShaderProgram* GetShaderParam( tTJSVariant* v )
{
	return (tTJSNI_GpuShaderProgram*)TJSGetNativeInstance( tTJSNC_GpuShaderProgram::ClassID, v );
}

static void WarnUnsupportedOnce( const tjs_char* what )
{
	static std::vector<ttstr> warned;
	ttstr w( what );
	for( auto& s : warned ) if( s == w ) return;
	warned.push_back( w );
	TVPAddImportantLog( ttstr( TJS_W("Canvas (GPU): not supported yet: ") ) + w );
}

tjs_uint32 tTJSNC_GpuCanvas::ClassID = -1;
tTJSNC_GpuCanvas::tTJSNC_GpuCanvas() : tTJSNativeClass( TJS_W("Canvas") )
{
	TJS_BEGIN_NATIVE_MEMBERS( Canvas )
	TJS_DECL_EMPTY_FINALIZE_METHOD
	TJS_BEGIN_NATIVE_CONSTRUCTOR_DECL( _this, tTJSNI_GpuCanvas, Canvas ) { return TJS_S_OK; }
	TJS_END_NATIVE_CONSTRUCTOR_DECL( Canvas )

	// canvas.draw(target, action, ...)  target.action(canvas, ...) を描画区間の中で呼ぶ
	TJS_BEGIN_NATIVE_METHOD_DECL( draw )
	{
		TJS_GET_NATIVE_INSTANCE( _this, tTJSNI_GpuCanvas );
		if( numparams < 1 ) return TJS_E_BADPARAMCOUNT;
		tTJSVariantClosure owner = param[0]->AsObjectClosure();
		ttstr action;
		if( numparams >= 2 && param[1]->Type() != tvtVoid ) action = *param[1];
		if( owner.Object ) {
			tTJSVariant canvas( objthis, objthis );
			std::vector<tTJSVariant*> pp;
			pp.push_back( &canvas );
			for( int i = 2; i < numparams; i++ ) pp.push_back( param[i] );
			_this->BeginDrawing();
			try {
				owner.FuncCall( 0, action.IsEmpty() ? NULL : action.c_str(), action.IsEmpty() ? NULL : action.GetHint(),
					result, (tjs_int)pp.size(), &pp[0], NULL );
			} catch( ... ) {
				_this->EndDrawing();
				owner.Release();
				throw;
			}
			_this->EndDrawing();
		}
		owner.Release();
		return TJS_S_OK;
	}
	TJS_END_NATIVE_METHOD_DECL( draw )

	TJS_BEGIN_NATIVE_METHOD_DECL( clear )
	{
		TJS_GET_NATIVE_INSTANCE( _this, tTJSNI_GpuCanvas );
		_this->Clear( numparams > 0 ? (tjs_uint32)(tjs_int64)*param[0] : _this->GetClearColor() );
		return TJS_S_OK;
	}
	TJS_END_NATIVE_METHOD_DECL( clear )

	TJS_BEGIN_NATIVE_METHOD_DECL( capture )
	{
		TJS_GET_NATIVE_INSTANCE( _this, tTJSNI_GpuCanvas );
		if( numparams < 1 ) return TJS_E_BADPARAMCOUNT;
		int x = numparams > 1 ? (tjs_int)*param[1] : 0;
		int y = numparams > 2 ? (tjs_int)*param[2] : 0;
		int w = numparams > 3 ? (tjs_int)*param[3] : 0;
		int h = numparams > 4 ? (tjs_int)*param[4] : 0;
		tTJSNI_GpuTexture* tex = (tTJSNI_GpuTexture*)TJSGetNativeInstance( tTJSNC_GpuTexture::ClassID, param[0] );
		if( tex ) {
			tjs_int sw = std::min<tjs_int>( _this->GetCanvasWidth(), tex->GetWidth() );
			tjs_int sh = std::min<tjs_int>( _this->GetCanvasHeight(), tex->GetHeight() );
			if( w == 0 ) w = sw;
			if( h == 0 ) h = sh;
			if( x < 0 || y < 0 || x + w > sw || y + h > sh ) TVPThrowExceptionMessage( TVPCaptureAreaOver );
			_this->Capture( tex, x, y, w, h );
			return TJS_S_OK;
		}
		tTJSNI_Bitmap* bmp = (tTJSNI_Bitmap*)TJSGetNativeInstance( tTJSNC_Bitmap::ClassID, param[0] );
		if( !bmp ) return TJS_E_INVALIDPARAM;
		tjs_int sw = _this->GetCanvasWidth(), sh = _this->GetCanvasHeight();
		if( w == 0 ) w = sw;
		if( h == 0 ) h = sh;
		if( x < 0 || y < 0 || x + w > sw || y + h > sh ) TVPThrowExceptionMessage( TVPCaptureAreaOver );
		bmp->SetSize( w, h, false );
		_this->Capture( bmp->GetBitmap(), x, y, w, h );
		return TJS_S_OK;
	}
	TJS_END_NATIVE_METHOD_DECL( capture )

	TJS_BEGIN_NATIVE_METHOD_DECL( fill )
	{
		TJS_GET_NATIVE_INSTANCE( _this, tTJSNI_GpuCanvas );
		if( numparams < 2 ) return TJS_E_BADPARAMCOUNT;
		tjs_int width = *param[0], height = *param[1];
		tTJSNI_GpuShaderProgram* shader = numparams >= 4 ? GetShaderParam( param[3] ) : nullptr;
		tjs_uint32 colors[4] = { 0xffffffff, 0xffffffff, 0xffffffff, 0xffffffff };
		if( numparams >= 3 ) {
			if( param[2]->Type() == tvtObject ) {
				tTJSVariantClosure clo = param[2]->AsObjectClosureNoAddRef();
				if( clo.Object && TJSGetArrayElementCount( clo.Object ) >= 4 ) {
					tTJSVariant tmp;
					for( int i = 0; i < 4; i++ ) {
						if( TJS_FAILED( clo.Object->PropGetByNum( TJS_MEMBERMUSTEXIST, i, &tmp, clo.ObjThis ) ) )
							TVPThrowExceptionMessage( TVPInsufficientNumberOfArrays );
						colors[i] = (tjs_uint32)(tjs_int64)tmp;
					}
				}
			} else if( param[2]->Type() != tvtVoid ) {
				const tjs_uint32 c = (tjs_uint32)(tjs_int64)*param[2];
				for( int i = 0; i < 4; i++ ) colors[i] = c;
			}
		}
		_this->Fill( width, height, colors, shader );
		return TJS_S_OK;
	}
	TJS_END_NATIVE_METHOD_DECL( fill )

	TJS_BEGIN_NATIVE_METHOD_DECL( drawTexture )
	{
		TJS_GET_NATIVE_INSTANCE( _this, tTJSNI_GpuCanvas );
		if( numparams < 1 || numparams > 4 ) return TJS_E_BADPARAMCOUNT;
		const iTVPGpuDrawable* tex[3] = {};
		tjs_uint ntex = numparams == 1 ? 1 : numparams - 1;
		for( tjs_uint i = 0; i < ntex; i++ ) {
			tex[i] = TVPGetGpuDrawable( param[i] );
			if( !tex[i] ) return TJS_E_INVALIDPARAM;
		}
		tTJSNI_GpuShaderProgram* shader = nullptr;
		if( numparams >= 2 ) {
			shader = GetShaderParam( param[numparams - 1] );
			if( !shader ) return TJS_E_INVALIDPARAM;
		}
		_this->DrawTexture( tex, ntex, shader );
		return TJS_S_OK;
	}
	TJS_END_NATIVE_METHOD_DECL( drawTexture )

	TJS_BEGIN_NATIVE_METHOD_DECL( drawTransition )
	{
		TJS_GET_NATIVE_INSTANCE( _this, tTJSNI_GpuCanvas );
		if( numparams < 3 ) return TJS_E_BADPARAMCOUNT;
		const iTVPGpuDrawable* front = TVPGetGpuDrawable( param[0] );
		const iTVPGpuDrawable* back = TVPGetGpuDrawable( param[1] );
		if( !front || !back ) return TJS_E_INVALIDPARAM;
		const iTVPGpuDrawable* rule = nullptr;
		if( numparams >= 4 && param[3]->Type() == tvtObject && param[3]->AsObjectNoAddRef() ) {
			rule = TVPGetGpuDrawable( param[3] );
			if( !rule ) return TJS_E_INVALIDPARAM;
		}
		tjs_int vague = ( numparams >= 5 && param[4]->Type() != tvtVoid ) ? (tjs_int)*param[4] : 64;
		_this->DrawTransition( front, back, (tjs_real)*param[2], rule, vague );
		return TJS_S_OK;
	}
	TJS_END_NATIVE_METHOD_DECL( drawTransition )

	TJS_BEGIN_NATIVE_METHOD_DECL( drawTextureAtlas )
	{
		TJS_GET_NATIVE_INSTANCE( _this, tTJSNI_GpuCanvas );
		if( numparams < 2 ) return TJS_E_BADPARAMCOUNT;
		const tTJSNI_Rect* rect = (tTJSNI_Rect*)TJSGetNativeInstance( tTJSNC_Rect::ClassID, param[0] );
		const iTVPGpuDrawable* tex = TVPGetGpuDrawable( param[1] );
		if( !rect || !tex ) return TJS_E_INVALIDPARAM;
		tTJSNI_GpuShaderProgram* shader = nullptr;
		if( numparams >= 3 ) {
			shader = GetShaderParam( param[2] );
			if( !shader ) return TJS_E_INVALIDPARAM;
		}
		_this->DrawTextureAtlas( rect->Get(), tex, shader );
		return TJS_S_OK;
	}
	TJS_END_NATIVE_METHOD_DECL( drawTextureAtlas )

	TJS_BEGIN_NATIVE_METHOD_DECL( drawText )
	{
		WarnUnsupportedOnce( TJS_W("drawText") );
		return TJS_S_OK;
	}
	TJS_END_NATIVE_METHOD_DECL( drawText )

	// drawMesh(shader, count [, type=ptTriangles [, offset]])
	// drawMesh(shader, indexBinder, count [, type=ptTriangles])
	TJS_BEGIN_NATIVE_METHOD_DECL( drawMesh )
	{
		TJS_GET_NATIVE_INSTANCE( _this, tTJSNI_GpuCanvas );
		if( numparams < 2 ) return TJS_E_BADPARAMCOUNT;
		tTJSNI_GpuShaderProgram* shader = GetShaderParam( param[0] );
		if( !shader ) return TJS_E_INVALIDPARAM;
		if( param[1]->Type() == tvtObject ) {
			if( numparams < 3 ) return TJS_E_BADPARAMCOUNT;
			tTJSNI_GpuVertexBinder* index = (tTJSNI_GpuVertexBinder*)TJSGetNativeInstance( tTJSNC_GpuVertexBinder::ClassID, param[1] );
			if( !index ) return TJS_E_INVALIDPARAM;
			tjs_int type = numparams >= 4 ? (tjs_int)*param[3] : GL_TRIANGLES_;
			_this->DrawMesh( shader, type, 0, *param[2], index );
		} else {
			tjs_int type = numparams >= 3 ? (tjs_int)*param[2] : GL_TRIANGLES_;
			tjs_int offset = numparams >= 4 ? (tjs_int)*param[3] : 0;
			_this->DrawMesh( shader, type, offset, *param[1], nullptr );
		}
		return TJS_S_OK;
	}
	TJS_END_NATIVE_METHOD_DECL( drawMesh )

	TJS_BEGIN_NATIVE_METHOD_DECL( draw9Patch )
	{
		TJS_GET_NATIVE_INSTANCE( _this, tTJSNI_GpuCanvas );
		if( numparams < 3 ) return TJS_E_BADPARAMCOUNT;
		tTJSNI_GpuTexture* tex = (tTJSNI_GpuTexture*)TJSGetNativeInstance( tTJSNC_GpuTexture::ClassID, param[0] );
		if( !tex ) return TJS_E_INVALIDPARAM;
		tTJSNI_GpuShaderProgram* shader = nullptr;
		if( numparams >= 4 ) {
			shader = GetShaderParam( param[3] );
			if( !shader ) return TJS_E_INVALIDPARAM;
		}
		tTVPRect margin;
		_this->Draw9Patch( tex, *param[1], *param[2], margin, shader );
		if( result ) {
			iTJSDispatch2* ret = TVPCreateRectObject( margin.left, margin.top, margin.right, margin.bottom );
			*result = tTJSVariant( ret, ret );
			ret->Release();
		}
		return TJS_S_OK;
	}
	TJS_END_NATIVE_METHOD_DECL( draw9Patch )

	TJS_BEGIN_NATIVE_METHOD_DECL( flush ) { return TJS_S_OK; }
	TJS_END_NATIVE_METHOD_DECL( flush )

	TJS_BEGIN_NATIVE_METHOD_DECL( save )
	{
		TJS_GET_NATIVE_INSTANCE( _this, tTJSNI_GpuCanvas );
		_this->Save();
		return TJS_S_OK;
	}
	TJS_END_NATIVE_METHOD_DECL( save )

	TJS_BEGIN_NATIVE_METHOD_DECL( restore )
	{
		TJS_GET_NATIVE_INSTANCE( _this, tTJSNI_GpuCanvas );
		_this->Restore();
		return TJS_S_OK;
	}
	TJS_END_NATIVE_METHOD_DECL( restore )

	// 未対応 (今後): エフェクト・マスク・ステンシルクリップ。描画はそのまま素通しになる
	TJS_BEGIN_NATIVE_METHOD_DECL( beginEffect ) { WarnUnsupportedOnce( TJS_W("beginEffect/endEffect") ); return TJS_S_OK; }
	TJS_END_NATIVE_METHOD_DECL( beginEffect )
	TJS_BEGIN_NATIVE_METHOD_DECL( endEffect ) { return TJS_S_OK; }
	TJS_END_NATIVE_METHOD_DECL( endEffect )
	TJS_BEGIN_NATIVE_METHOD_DECL( beginMaskClip ) { WarnUnsupportedOnce( TJS_W("beginMaskClip/endMaskClip") ); return TJS_S_OK; }
	TJS_END_NATIVE_METHOD_DECL( beginMaskClip )
	TJS_BEGIN_NATIVE_METHOD_DECL( endMaskClip ) { return TJS_S_OK; }
	TJS_END_NATIVE_METHOD_DECL( endMaskClip )
	TJS_BEGIN_NATIVE_METHOD_DECL( beginStencilClip ) { WarnUnsupportedOnce( TJS_W("beginStencilClip/endStencilClip") ); return TJS_S_OK; }
	TJS_END_NATIVE_METHOD_DECL( beginStencilClip )
	TJS_BEGIN_NATIVE_METHOD_DECL( endStencilClip ) { return TJS_S_OK; }
	TJS_END_NATIVE_METHOD_DECL( endStencilClip )

	//-- properties
#define GPU_CV_PROP_OBJ( name, getter, setter ) \
	TJS_BEGIN_NATIVE_PROP_DECL( name ) { \
		TJS_BEGIN_NATIVE_PROP_GETTER { \
			TJS_GET_NATIVE_INSTANCE( _this, tTJSNI_GpuCanvas ); \
			*result = _this->getter(); return TJS_S_OK; \
		} TJS_END_NATIVE_PROP_GETTER \
		TJS_BEGIN_NATIVE_PROP_SETTER { \
			TJS_GET_NATIVE_INSTANCE( _this, tTJSNI_GpuCanvas ); \
			_this->setter( *param ); return TJS_S_OK; \
		} TJS_END_NATIVE_PROP_SETTER \
	} TJS_END_NATIVE_PROP_DECL( name )

	GPU_CV_PROP_OBJ( renderTarget, GetRenderTargetObject, SetRenderTargetObject )
	GPU_CV_PROP_OBJ( clipRect, GetClipRectObject, SetClipRectObject )
	GPU_CV_PROP_OBJ( matrix, GetMatrix32Object, SetMatrix32Object )
	GPU_CV_PROP_OBJ( defaultShader, GetDefaultShader, SetDefaultShader )
	GPU_CV_PROP_OBJ( defaultFillShader, GetDefaultFillShader, SetDefaultFillShader )

	TJS_BEGIN_NATIVE_PROP_DECL( clearColor ) {
		TJS_BEGIN_NATIVE_PROP_GETTER {
			TJS_GET_NATIVE_INSTANCE( _this, tTJSNI_GpuCanvas );
			*result = (tjs_int64)_this->GetClearColor(); return TJS_S_OK;
		} TJS_END_NATIVE_PROP_GETTER
		TJS_BEGIN_NATIVE_PROP_SETTER {
			TJS_GET_NATIVE_INSTANCE( _this, tTJSNI_GpuCanvas );
			_this->SetClearColor( (tjs_uint32)(tjs_int64)*param ); return TJS_S_OK;
		} TJS_END_NATIVE_PROP_SETTER
	} TJS_END_NATIVE_PROP_DECL( clearColor )

	TJS_BEGIN_NATIVE_PROP_DECL( blendMode ) {
		TJS_BEGIN_NATIVE_PROP_GETTER {
			TJS_GET_NATIVE_INSTANCE( _this, tTJSNI_GpuCanvas );
			*result = (tjs_int)_this->GetBlendMode(); return TJS_S_OK;
		} TJS_END_NATIVE_PROP_GETTER
		TJS_BEGIN_NATIVE_PROP_SETTER {
			TJS_GET_NATIVE_INSTANCE( _this, tTJSNI_GpuCanvas );
			_this->SetBlendMode( (tTVPGpuBlendMode)(tjs_int)*param ); return TJS_S_OK;
		} TJS_END_NATIVE_PROP_SETTER
	} TJS_END_NATIVE_PROP_DECL( blendMode )

	TJS_BEGIN_NATIVE_PROP_DECL( enableClipRect ) {
		TJS_BEGIN_NATIVE_PROP_GETTER {
			TJS_GET_NATIVE_INSTANCE( _this, tTJSNI_GpuCanvas );
			*result = (tjs_int)( _this->GetEnableClipRect() ? 1 : 0 ); return TJS_S_OK;
		} TJS_END_NATIVE_PROP_GETTER
		TJS_BEGIN_NATIVE_PROP_SETTER {
			TJS_GET_NATIVE_INSTANCE( _this, tTJSNI_GpuCanvas );
			_this->SetEnableClipRect( (tjs_int)*param != 0 ); return TJS_S_OK;
		} TJS_END_NATIVE_PROP_SETTER
	} TJS_END_NATIVE_PROP_DECL( enableClipRect )

	TJS_BEGIN_NATIVE_PROP_DECL( enableCulling ) {
		TJS_BEGIN_NATIVE_PROP_GETTER {
			TJS_GET_NATIVE_INSTANCE( _this, tTJSNI_GpuCanvas );
			*result = (tjs_int)( _this->GetEnableCulling() ? 1 : 0 ); return TJS_S_OK;
		} TJS_END_NATIVE_PROP_GETTER
		TJS_BEGIN_NATIVE_PROP_SETTER {
			TJS_GET_NATIVE_INSTANCE( _this, tTJSNI_GpuCanvas );
			_this->SetEnableCulling( (tjs_int)*param != 0 ); return TJS_S_OK;
		} TJS_END_NATIVE_PROP_SETTER
	} TJS_END_NATIVE_PROP_DECL( enableCulling )

	TJS_BEGIN_NATIVE_PROP_DECL( width ) {
		TJS_BEGIN_NATIVE_PROP_GETTER {
			TJS_GET_NATIVE_INSTANCE( _this, tTJSNI_GpuCanvas );
			*result = (tjs_int64)_this->GetWidth(); return TJS_S_OK;
		} TJS_END_NATIVE_PROP_GETTER
		TJS_DENY_NATIVE_PROP_SETTER
	} TJS_END_NATIVE_PROP_DECL( width )

	TJS_BEGIN_NATIVE_PROP_DECL( height ) {
		TJS_BEGIN_NATIVE_PROP_GETTER {
			TJS_GET_NATIVE_INSTANCE( _this, tTJSNI_GpuCanvas );
			*result = (tjs_int64)_this->GetHeight(); return TJS_S_OK;
		} TJS_END_NATIVE_PROP_GETTER
		TJS_DENY_NATIVE_PROP_SETTER
	} TJS_END_NATIVE_PROP_DECL( height )

	TJS_END_NATIVE_MEMBERS
}
tTJSNativeClass* TVPCreateNativeClass_GpuCanvas() { return new tTJSNC_GpuCanvas(); }
