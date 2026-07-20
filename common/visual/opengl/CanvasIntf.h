/**
 * Canvas クラス
 * Intf/Impl で分ける方法ではなく、共通化して、ifdefかラッパーメソッドでの環境切り替えを
 * 前提に継承は避けたデザインとする。
 */

#ifndef CanvasIntfH
#define CanvasIntfH

#include "tjsNative.h"
#include "drawable.h"
#include "tjsHashSearch.h"
#include "ComplexRect.h"
#include "GLVertexBufferObject.h"
#include "GLEffect.h"
#include "GLClip.h"
#include <vector>
#include <memory>

enum class tTVPBlendMode : tjs_int {
	bmDisable = 0,
	bmOpaque = 1,
	bmAlpha = 2,
	bmAdd = 3,
	bmAddWithAlpha = 4,
	bmSubtract = 5,
	bmMultiply = 6,
	bmMin = 7,
	bmMax = 8,
	bmScreen = 9
};
struct tTVPCanvasState {
	static const tjs_uint32 FLAG_CLIP_RECT = 0x01 << 0;
	static const tjs_uint32 FLAG_CULLING = 0x01 << 1;

	float Matrix[6];
	tjs_int ClipRect[4];
	tjs_uint32 Flag;

	tTVPCanvasState( class tTJSNI_Matrix32* mat, class tTJSNI_Rect* clip, bool enableClip, bool enableCulling );
};

class tTJSNI_Canvas : public tTJSNativeInstance
{
	static const ttstr DefaultVertexShaderText;
	static const ttstr DefaultFragmentShaderText;
	static const ttstr DefaultFillVertexShaderText;
	static const ttstr DefaultFillFragmentShaderText;
	static const float DefaultUVs[];

	bool InDrawing;
	bool EnableClipRect;
	bool EnableCulling = false;
	tjs_uint32 ClearColor;
	tTVPBlendMode BlendMode;
	tTVPRect CurrentScissorRect;

	std::vector<std::unique_ptr<tTVPCanvasState> > StateStack;

	GLVertexBufferObject TextureVertexBuffer;

	// 直前のBeginDrawingで設定したViewportの幅と高さ
	tjs_int PrevViewportWidth;
	tjs_int PrevViewportHeight;

	tTJSVariant RenterTaretObject;
	class tTJSNI_Offscreen* RenderTargetInstance;

	tTJSVariant ClipRectObject;
	class tTJSNI_Rect* ClipRectInstance;

	tTJSVariant Matrix32Object;
	class tTJSNI_Matrix32* Matrix32Instance;

	tTJSVariant EmbeddedDefaultShaderObject;
	tTJSVariant DefaultShaderObject;
	class tTJSNI_ShaderProgram* DefaultShaderInstance;

	tTJSVariant EmbeddedDefaultFillShaderObject;
	tTJSVariant DefaultFillShaderObject;
	class tTJSNI_ShaderProgram* DefaultFillShaderInstance;
public:
	void SetRenderTargetObject( const tTJSVariant & val );
	const tTJSVariant& GetRenderTargetObject() const { return RenterTaretObject; }

	void SetClipRectObject( const tTJSVariant & val );
	const tTJSVariant& GetClipRectObject() const { return ClipRectObject; }

	void SetMatrix32Object( const tTJSVariant & val );
	const tTJSVariant& GetMatrix32Object() const { return Matrix32Object; }

	void SetDefaultShader( const tTJSVariant & val );
	const tTJSVariant& GetDefaultShader() const { return DefaultShaderObject; }

	void SetDefaultFillShader( const tTJSVariant & val );
	const tTJSVariant& GetDefaultFillShader() const { return DefaultFillShaderObject; }

private:
	// ポストエフェクト / 画像クリップ (gles プラグイン GLESAdaptor 由来の機構)
	GLFboPool EffectFboPool;
	GLEffectContext EffectCtx;
	GLClipContext ClipCtx;
	std::vector<GLuint> EffectTargetStack;                 // begin 時の退避ターゲット (FBO id)
	std::vector<GLFrameBufferObject*> EffectCaptureStack;  // 捕捉中の中間 FBO
	float EffectSeed = 0.0f;                               // noise 用シード (endEffect 毎に更新)
	bool StencilClipEnabled = false;
	GLint EffectScissorBox[4];                             // 合成時 scissor (GL 座標系、下記の戻り値バッファ)

	// enableClipRect 時の clipRect を GL 座標系 scissor 矩形にして返す (無効なら nullptr)
	const GLint* CurrentScissorBox();
	// begin/end の取りこぼしを解放する (EndDrawing 時の後始末)
	void UnwindEffects();
	// 合成パスが触った scissor 状態を Canvas の状態機械へ再同期する
	void RestoreClipState();

	void ApplyBlendMode();
	void ApplyClipRect();
	void DisableClipRect();
	static void SetCulling( bool b );
	void CreateDefaultShader();
	void CreateDefaultMatrix();
	void SetupEachDrawing();

	// 描画に必要な設定と1個目のテクスチャまで設定する
	void SetupTextureDrawing( class tTJSNI_ShaderProgram* shader, const class iTVPTextureInfoIntrface* tex, class tTJSNI_Matrix32* mat, const tTVPPoint& vpSize );

	// 描画領域の幅/高さ。レンダーターゲット指定している場合はそのサイズ、そうでない場合はクライアント領域(サーフェイス)のサイズ
	tjs_int GetCanvasWidth() const;
	tjs_int GetCanvasHeight() const;

	int SurfaceWidth;
	int SurfaceHeight;
	int CanvasWidth;
	int CanvasHeight;
	GLint DefaultFrameBufferId;

public:
	tTJSNI_Canvas();
	~tTJSNI_Canvas() override;
	tjs_error TJS_INTF_METHOD Construct(tjs_int numparams, tTJSVariant **param, iTJSDispatch2 *tjs_obj) override;
	void TJS_INTF_METHOD Invalidate() override;
	void TJS_INTF_METHOD Destruct() override;

	void SetSurfaceSize( int width, int height );

	void BeginDrawing();
	void EndDrawing();

	// method
	void Capture( class tTJSNI_Bitmap* bmp, int x, int y, int w, int h);
	void Capture( const class iTVPTextureInfoIntrface* texture, int x, int y, int w, int h);
	void Clear( tjs_uint32 color );

	void Fill( tjs_int width, tjs_int height, tjs_uint32 colors[4], class tTJSNI_ShaderProgram* shader = nullptr );
	void DrawTexture( const class iTVPTextureInfoIntrface* texture, class tTJSNI_ShaderProgram* shader = nullptr );
	void DrawTexture( const class iTVPTextureInfoIntrface* texture0, const class iTVPTextureInfoIntrface* texture1, class tTJSNI_ShaderProgram* shader );
	void DrawTexture( const class iTVPTextureInfoIntrface* texture0, const class iTVPTextureInfoIntrface* texture1, const class iTVPTextureInfoIntrface* texture2, class tTJSNI_ShaderProgram* shader );
	void DrawText( class tTJSNI_Font* font, tjs_int x, tjs_int y, const ttstr& text, tjs_uint32 color );
	void DrawTextureAtlas( const class tTJSNI_Rect* rect, const class iTVPTextureInfoIntrface* texture, class tTJSNI_ShaderProgram* shader = nullptr );

	void DrawMesh( class tTJSNI_ShaderProgram* shader, tjs_int primitiveType, tjs_int offset, tjs_int count );
	void DrawMesh( class tTJSNI_ShaderProgram* shader, tjs_int primitiveType, const class tTJSNI_VertexBinder* index, tjs_int count );

	/**
	 * 9patchを利用した描画
	 */
	void Draw9PatchTexture( class tTJSNI_Texture* tex, tjs_int width, tjs_int height, tTVPRect& margin, class tTJSNI_ShaderProgram* shader = nullptr );

	// 状態をセーブする
	void Save();
	// 状態を元に戻す
	void Restore();

	// ポストエフェクト: begin〜end で囲んだ描画を中間 FBO に捕捉し、コマンド
	// 配列の画像加工チェーンを GPU 上で適用してから blendMode で合成する。
	// コマンド仕様は gles プラグイン (GLESAdaptor.beginEffect/endEffect) 互換。
	// ネスト可。
	void BeginEffect();
	void EndEffect( const tTJSVariant & commands );

	// マスククリップ: begin〜end で囲んだ描画を捕捉し、マスク画像の α を
	// 乗算しながら合成する (マスク矩形の外側は α=0)。mask=nullptr で素通し。
	void BeginMaskClip();
	void EndMaskClip( const class iTVPTextureInfoIntrface* mask, float x, float y );

	// ステンシルクリップ: マスク α が閾値以上の領域をステンシルへ書き込み、
	// 以降の描画をその領域内に切り抜く。単純テクスチャ描画専用
	// (自前でステンシルを使う描画とは競合する)。
	void BeginStencilClip( const class iTVPTextureInfoIntrface* mask, float x, float y, tjs_int threshold );
	void EndStencilClip();

	// prop
	void SetClearColor(tjs_uint32 color) { ClearColor = color; }
	tjs_uint32 GetClearColor() const { return ClearColor; }
	void SetBlendMode( tTVPBlendMode bm );
	tTVPBlendMode GetBlendMode() const { return BlendMode; }
	tjs_uint GetWidth() const;
	tjs_uint GetHeight() const;
	void SetEnableClipRect( bool b );
	bool GetEnableClipRect() const { return EnableClipRect; }
	void SetEnableCulling( bool b );
	bool GetEnableCulling() const { return EnableCulling; }
};


//---------------------------------------------------------------------------
// tTJSNC_Canvas : TJS Canvas class
//---------------------------------------------------------------------------
class tTJSNC_Canvas : public tTJSNativeClass
{
public:
	tTJSNC_Canvas();
	static tjs_uint32 ClassID;

protected:
	tTJSNativeInstance *CreateNativeInstance() override { return new tTJSNI_Canvas(); }
};

extern tTJSNativeClass * TVPCreateNativeClass_Canvas();
#endif
