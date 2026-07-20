#ifndef GLEffectH
#define GLEffectH

// ---------------------------------------------------------------------------
// 描画後処理（ポストエフェクト）基盤
//
//   Canvas.beginEffect() ～ endEffect(commands) で囲んだ描画結果を中間 FBO に
//   捕捉し、コマンド配列で指定した画像加工チェーンを GPU 上で適用してから
//   元のターゲットへ合成する。gles プラグイン (GLESAdaptor) の
//   ポストエフェクト機構の本体取り込み版で、コマンド仕様は互換。
//
//   処理は次のように分類して扱う:
//     - LUT系点処理   : gamma / light / lut。per-channel 256段ルックアップに帰着。
//                       連続するものは CPU 側で1枚のLUTに合成し1パスで適用する。
//     - 混合系点処理  : grayscale / colorize / modulate / noise 等。
//                       チャンネル混合や乱数を含む。オペコードループ1パスに融合。
//     - 近傍処理      : boxBlur / gaussianBlur。分離可能な重み付き畳み込み。
//                       H/V の2パスを中間FBOで ping-pong する。
//
//   各コマンドの演算は kirikiri 本体(TVPDoGrayScale / TVPAdjustGamma)および
//   LayerExImage プラグインの実装に厳密に合わせる。
//   ただし以下は原理的に完全一致しない(視覚的同等を狙う):
//     - noise / generateWhiteNoise : CPU rand() 列は再現不能。座標ハッシュで代替。
//     - blur の最外周        : CPU はエッジで重み再正規化。GPU は CLAMP_TO_EDGE。
//     - colorize             : CPU は整数HSL丸め。GPU は浮動小数で同アルゴリズム。
//
//   シェーダは GLSL ES 3.00 (#version 300 es)。GLES2 フォールバック環境では
//   コンパイルに失敗しエフェクトは素通しになる (ログに出る)。
// ---------------------------------------------------------------------------

#include "tjsNative.h"
#include "OpenGLHeader.h"
#include "GLFrameBufferObject.h"

#include <vector>

// 混合系点処理を1パスに融合できる最大オペレーション数
#define GLEFFECT_MAX_OPS 32
// 分離ブラーの最大半径(片側)
#define GLEFFECT_MAX_BLUR 64

// 混合系点処理オペコード（シェーダ内の分岐と一致させること）
enum GLEffectOp {
	GLEOP_NONE       = 0,
	GLEOP_GRAYSCALE  = 1,
	GLEOP_COLORIZE   = 2,
	GLEOP_MODULATE   = 3,
	GLEOP_NOISE      = 4,
	GLEOP_WHITENOISE = 5,
};

// overcolor(指定色塗りつぶし合成) はオペコードに合成モードを載せる。
//   op = GLEFFECT_OVERCOLOR_BASE + type
//   type は吉里吉里 tTVPBlendOperationMode(=tTVPLayerType) の値(1..28)。
//   param は塗り色 (r,g,b,a) を 0..1 で格納(a は color.a * opacity)。
#define GLEFFECT_OVERCOLOR_BASE 100

// ---------------------------------------------------------------------------
// 中間FBOプール
//   キャンバスサイズのレンダーターゲットを貸し出す。ping-pong / 多段処理用。
//   GL オブジェクトを保持するので、破棄はコンテキスト有効時に clear() で行う。
// ---------------------------------------------------------------------------
class GLFboPool {
public:
	GLFboPool() {}
	~GLFboPool() { clear(); }

	GLFrameBufferObject *acquire(int w, int h);
	void release(GLFrameBufferObject *fbo);
	void clear();

private:
	std::vector<GLFrameBufferObject *> mFree;
	std::vector<GLFrameBufferObject *> mAll;
};

// ---------------------------------------------------------------------------
// エフェクト共有コンテキスト
//   各シェーダプログラムとフルスクリーン矩形描画を保持する。
//   描画プリミティブは「出力FBO・ビューポート・ブレンド状態は呼び出し側が設定済み」
//   を前提とする(ステージ側で管理する)。
// ---------------------------------------------------------------------------
class GLEffectContext {
public:
	GLEffectContext();
	~GLEffectContext();

	void init();
	void done();
	bool ready() const { return mInited; }

	// noise 用のシード(フレーム毎に変えると毎フレーム異なるノイズになる)
	void setSeed(float seed) { mSeed = seed; }

	// 混合系点処理を1パス適用。ops/params は count 個(0なら素通し=合成用)。
	// scissorBox は合成時の矩形クリップ (GL 座標系4要素、nullptr で無効)。
	void drawPointwise(GLuint srcTex, const int *ops, const float *params, int count, int w, int h,
	                   const GLint *scissorBox = nullptr);
	// per-channel LUT を1パス適用。lutTex は 256x1 の RGBA8(NEAREST)。
	void drawLut(GLuint srcTex, GLuint lutTex);
	// 重み付き分離ブラー1方向ぶんを適用。dx,dy はテクセル単位の1ステップ。
	void drawBlur(GLuint srcTex, float dx, float dy, int radius, const float *weights);

private:
	bool  mInited;
	float mSeed;

	// 混合系点処理プログラム
	GLuint mPwProgram;
	GLint  mPwAttrPos, mPwAttrUV;
	GLint  mPwUnifTex, mPwUnifCount, mPwUnifOps, mPwUnifParams, mPwUnifSeed, mPwUnifRes;

	// LUTプログラム
	GLuint mLutProgram;
	GLint  mLutAttrPos, mLutAttrUV;
	GLint  mLutUnifTex, mLutUnifLut;

	// ブラープログラム(重み付き分離)
	GLuint mBlurProgram;
	GLint  mBlurAttrPos, mBlurAttrUV;
	GLint  mBlurUnifTex, mBlurUnifDir, mBlurUnifRadius, mBlurUnifWeights;

	void drawQuad(GLint attrPos, GLint attrUV, const GLint *scissorBox = nullptr);
};

// ---------------------------------------------------------------------------
// エフェクト1ステージ: 入力テクスチャ -> 出力FBO
// ---------------------------------------------------------------------------
class GLEffectStage {
public:
	virtual ~GLEffectStage() {}
	virtual void render(GLuint srcTex, int w, int h,
	                    GLFrameBufferObject *dst, GLFboPool &pool, GLEffectContext &ctx) = 0;
};

// ---------------------------------------------------------------------------
// エフェクトチェーン
//   コマンド配列(TJS)からステージ列をコンパイルし、テクスチャに順次適用する。
// ---------------------------------------------------------------------------
class GLEffectChain {
public:
	GLEffectChain() {}
	~GLEffectChain() { clear(); }

	void clear();
	bool empty() const { return mStages.empty(); }

	// コマンド配列(Array of Dictionary)からステージ列を構築。
	void compile(const tTJSVariant &commandArray);

	// srcTex に全ステージを適用し、結果を保持するプールFBOを返す。
	// 呼び出し側は結果FBOを pool.release() で返却すること。
	// ステージが無い場合は nullptr を返す(=加工なし)。
	GLFrameBufferObject *apply(GLuint srcTex, int w, int h, GLFboPool &pool, GLEffectContext &ctx);

private:
	std::vector<GLEffectStage *> mStages;
};

#endif // GLEffectH
