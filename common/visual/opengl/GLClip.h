#ifndef GLClipH
#define GLClipH

// ---------------------------------------------------------------------------
// クリッピング描画基盤
//
//   AffineLayer の clip / clipImage 相当を Canvas の GL 描画経路で実現する
//   ためのシェーダーパスを保持する。gles プラグイン (GLESAdaptor) の
//   GLClip.{h,cpp} からの移植。
//
//   - 矩形クリップ (clip)      : scissor で実現。状態管理は Canvas 側
//                                (clipRect / enableClipRect)。
//   - 画像クリップ (clipImage) : 二方式
//       stencil 方式 : マスク画像の α を現ターゲットのステンシルへ書き込み、
//                      以降の描画を stencil test (EQUAL 1) で切り抜く。
//                      描画モジュールが自前でステンシルを使う場合 (Emote 等)
//                      は競合するため、単純テクスチャ描画ソース専用。
//       mask 方式    : 描画内容を中間FBOに捕捉した後、マスク画像の α を
//                      乗算しながら直前ターゲットへ合成する。
//                      CPU 版 Layer.clipAlphaRect と同じく、マスク矩形の
//                      外側は α=0 (完全クリップ) になる。
// ---------------------------------------------------------------------------

#include "OpenGLHeader.h"

class GLClipContext {
public:
	GLClipContext();
	~GLClipContext();

	void init();
	void done();
	bool ready() const { return mInited; }

	/**
	 * マスク画像の α をステンシルバッファへ書き込む。
	 *   呼び出し側でステンシルクリア済みであること。
	 *   カラーバッファへは書き込まない (colorMask を落として描画)。
	 * @param maskTex マスクテクスチャ (GL texture id)
	 * @param x,y  マスク配置位置 (キャンバス座標、左上原点)
	 * @param w,h  マスク論理サイズ (ピクセル)
	 * @param uScale,vScale テクスチャ実サイズに対する論理サイズの比
	 * @param scrW,scrH 描画先サイズ
	 * @param threshold ステンシルを立てる α 閾値 (0.0～1.0)
	 */
	void drawStencilWrite(GLuint maskTex, float x, float y, float w, float h,
	                      float uScale, float vScale, int scrW, int scrH, float threshold);

	/**
	 * 捕捉テクスチャをマスク付きで全面合成する。
	 *   ブレンド状態は呼び出し側が設定済みであること。
	 * @param srcTex 捕捉内容 (キャンバスサイズのテクスチャ)
	 * @param maskTex マスクテクスチャ (0 なら素通し合成)
	 * @param x,y  マスク配置位置 (キャンバス座標、左上原点)
	 * @param w,h  マスク論理サイズ (ピクセル)
	 * @param uScale,vScale テクスチャ実サイズに対する論理サイズの比
	 * @param scrW,scrH 描画先サイズ
	 * @param scissorBox 矩形クリップ (GL 座標系4要素、nullptr で無効)
	 */
	void drawMaskComposite(GLuint srcTex, GLuint maskTex,
	                       float x, float y, float w, float h,
	                       float uScale, float vScale, int scrW, int scrH,
	                       const GLint *scissorBox);

private:
	bool mInited;

	// ステンシル書き込みプログラム
	GLuint mStProgram;
	GLint  mStAttrPos, mStAttrUV;
	GLint  mStUnifTex, mStUnifThreshold;

	// マスク合成プログラム
	GLuint mMaskProgram;
	GLint  mMaskAttrPos, mMaskAttrUV;
	GLint  mMaskUnifTex, mMaskUnifMask, mMaskUnifRes, mMaskUnifRect, mMaskUnifUV, mMaskUnifUseMask;
};

#endif // GLClipH
