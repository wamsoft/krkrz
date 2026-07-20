// ---------------------------------------------------------------------------
// クリッピング描画基盤 (GLClip.h 参照)
//   gles プラグイン (GLESAdaptor) の GLClip.cpp からの移植。
//   GLTexture* 引数を GL texture id + サイズに置き換えている以外は同一。
// ---------------------------------------------------------------------------
#include "tjsCommHead.h"
#include "GLClip.h"
#include "GLShaderUtil.h"

// ---------------------------------------------------------------------------
// シェーダー
// ---------------------------------------------------------------------------

static const char *kClipVsSource =
	"attribute vec2 a_position;"
	"attribute vec2 a_texCoord;"
	"varying vec2 v_texCoord;"
	"void main()"
	"{"
	"gl_Position = vec4(a_position, 0.0, 1.0);"
	"v_texCoord = a_texCoord;"
	"}";

// ステンシル書き込み: マスク α が閾値未満のピクセルを discard する。
// カラーは colorMask で落とすので値は何でもよい。
static const char *kStencilFsSource =
	"precision mediump float;"
	"varying vec2 v_texCoord;"
	"uniform sampler2D s_texture;"
	"uniform float u_threshold;"
	"void main()"
	"{"
	"if (texture2D(s_texture, v_texCoord).a < u_threshold) discard;"
	"gl_FragColor = vec4(0.0);"
	"}";

// マスク合成: 捕捉内容の α にマスク α を乗算して出力する。
//   u_res      : キャンバスサイズ
//   u_maskRect : マスクの配置 (x, y, w, h) キャンバス座標(左上原点)
//   u_maskUV   : マスク論理サイズ / テクスチャ実サイズ
//   u_useMask  : 0 なら素通し (矩形クリップのみの合成用)
//   捕捉テクスチャは v=0 が画面下端 (GL 座標) なので、
//   キャンバス座標への変換で上下を反転する。マスクテクスチャは
//   krkrz Texture の慣例どおり v=1 側が画像上端のため
//   こちらも 1-y でサンプリングする。
static const char *kMaskFsSource =
	"precision mediump float;"
	"varying vec2 v_texCoord;"
	"uniform sampler2D s_texture;"
	"uniform sampler2D s_mask;"
	"uniform vec2 u_res;"
	"uniform vec4 u_maskRect;"
	"uniform vec2 u_maskUV;"
	"uniform int u_useMask;"
	"void main()"
	"{"
	"vec4 color = texture2D(s_texture, v_texCoord);"
	"if (u_useMask != 0) {"
	"vec2 pixel = vec2(v_texCoord.x * u_res.x, (1.0 - v_texCoord.y) * u_res.y);"
	"vec2 m = (pixel - u_maskRect.xy) / u_maskRect.zw;"
	"float a = 0.0;"
	"if (m.x >= 0.0 && m.x <= 1.0 && m.y >= 0.0 && m.y <= 1.0) {"
	"a = texture2D(s_mask, vec2(m.x * u_maskUV.x, (1.0 - m.y) * u_maskUV.y)).a;"
	"}"
	"color.a *= a;"
	"}"
	"gl_FragColor = color;"
	"}";

// ---------------------------------------------------------------------------

GLClipContext::GLClipContext()
	: mInited(false)
	, mStProgram(0), mStAttrPos(-1), mStAttrUV(-1)
	, mStUnifTex(-1), mStUnifThreshold(-1)
	, mMaskProgram(0), mMaskAttrPos(-1), mMaskAttrUV(-1)
	, mMaskUnifTex(-1), mMaskUnifMask(-1), mMaskUnifRes(-1)
	, mMaskUnifRect(-1), mMaskUnifUV(-1), mMaskUnifUseMask(-1)
{
}

GLClipContext::~GLClipContext()
{
	done();
}

void GLClipContext::init()
{
	if (mInited) return;

	mStProgram = CompileProgram(kClipVsSource, kStencilFsSource);
	if (mStProgram) {
		mStAttrPos       = glGetAttribLocation(mStProgram, "a_position");
		mStAttrUV        = glGetAttribLocation(mStProgram, "a_texCoord");
		mStUnifTex       = glGetUniformLocation(mStProgram, "s_texture");
		mStUnifThreshold = glGetUniformLocation(mStProgram, "u_threshold");
	}

	mMaskProgram = CompileProgram(kClipVsSource, kMaskFsSource);
	if (mMaskProgram) {
		mMaskAttrPos     = glGetAttribLocation(mMaskProgram, "a_position");
		mMaskAttrUV      = glGetAttribLocation(mMaskProgram, "a_texCoord");
		mMaskUnifTex     = glGetUniformLocation(mMaskProgram, "s_texture");
		mMaskUnifMask    = glGetUniformLocation(mMaskProgram, "s_mask");
		mMaskUnifRes     = glGetUniformLocation(mMaskProgram, "u_res");
		mMaskUnifRect    = glGetUniformLocation(mMaskProgram, "u_maskRect");
		mMaskUnifUV      = glGetUniformLocation(mMaskProgram, "u_maskUV");
		mMaskUnifUseMask = glGetUniformLocation(mMaskProgram, "u_useMask");
	}

	mInited = true;
}

void GLClipContext::done()
{
	if (mStProgram)   { glDeleteProgram(mStProgram);   mStProgram = 0; }
	if (mMaskProgram) { glDeleteProgram(mMaskProgram); mMaskProgram = 0; }
	mInited = false;
}

// マスク画像 α をステンシルへ書き込む
void GLClipContext::drawStencilWrite(GLuint maskTex, float x, float y, float w, float h,
                                     float uScale, float vScale, int scrW, int scrH, float threshold)
{
	if (!mStProgram || !maskTex) return;

	// キャンバス座標 → NDC の変換。
	float w2 = scrW / 2.0f;
	float h2 = scrH / 2.0f;
	float tx = x - w2;
	float ty = y - h2;
	GLfloat position[8];
	position[0] =   tx / w2;        // left top
	position[1] = - ty / h2;
	position[2] =   tx / w2;        // left bottom
	position[3] = - (h + ty) / h2;
	position[4] =   (w + tx) / w2;  // right top
	position[5] = - ty / h2;
	position[6] =   (w + tx) / w2;  // right bottom
	position[7] = - (h + ty) / h2;

	// テクスチャは v=vScale 側が画像上端
	GLfloat uv[8];
	uv[0] = 0.0f;   uv[1] = vScale;  // left top
	uv[2] = 0.0f;   uv[3] = 0.0f;    // left bottom
	uv[4] = uScale; uv[5] = vScale;  // right top
	uv[6] = uScale; uv[7] = 0.0f;    // right bottom

	glViewport(0, 0, scrW, scrH);

	glDisable(GL_DEPTH_TEST);
	glDisable(GL_SCISSOR_TEST);
	glDisable(GL_CULL_FACE);

	// カラーへは書かず、描画できたピクセルのステンシルを 1 にする
	glColorMask(GL_FALSE, GL_FALSE, GL_FALSE, GL_FALSE);
	glEnable(GL_STENCIL_TEST);
	glStencilFunc(GL_ALWAYS, 1, 0xFF);
	glStencilOp(GL_KEEP, GL_KEEP, GL_REPLACE);
	glStencilMask(0xFF);

	glUseProgram(mStProgram);
	glEnableVertexAttribArray(mStAttrPos);
	glEnableVertexAttribArray(mStAttrUV);
	glUniform1i(mStUnifTex, 0);
	glUniform1f(mStUnifThreshold, threshold);
	glActiveTexture(GL_TEXTURE0);
	glBindTexture(GL_TEXTURE_2D, maskTex);
	glVertexAttribPointer(mStAttrPos, 2, GL_FLOAT, GL_FALSE, 0, (GLvoid *)position);
	glVertexAttribPointer(mStAttrUV,  2, GL_FLOAT, GL_FALSE, 0, (GLvoid *)uv);
	glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);

	// 以降の描画をステンシルで切り抜く状態にして返す
	glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
	glStencilFunc(GL_EQUAL, 1, 0xFF);
	glStencilOp(GL_KEEP, GL_KEEP, GL_KEEP);
}

// 捕捉テクスチャをマスク付きで全面合成
void GLClipContext::drawMaskComposite(GLuint srcTex, GLuint maskTex,
                                      float x, float y, float w, float h,
                                      float uScale, float vScale, int scrW, int scrH,
                                      const GLint *scissorBox)
{
	if (!mMaskProgram) return;

	// フルスクリーン矩形 (GLEffectContext::drawQuad と同じ対応)
	static const GLfloat position[8] = {
		-1.0f, -1.0f,  -1.0f,  1.0f,   1.0f, -1.0f,   1.0f,  1.0f,
	};
	static const GLfloat uv[8] = {
		 0.0f,  0.0f,   0.0f,  1.0f,   1.0f,  0.0f,   1.0f,  1.0f,
	};

	glViewport(0, 0, scrW, scrH);

	glDisable(GL_DEPTH_TEST);
	glDisable(GL_STENCIL_TEST);
	glDisable(GL_CULL_FACE);
	if (scissorBox) {
		glEnable(GL_SCISSOR_TEST);
		glScissor(scissorBox[0], scissorBox[1], scissorBox[2], scissorBox[3]);
	} else {
		glDisable(GL_SCISSOR_TEST);
	}

	glUseProgram(mMaskProgram);
	glEnableVertexAttribArray(mMaskAttrPos);
	glEnableVertexAttribArray(mMaskAttrUV);
	glUniform1i(mMaskUnifTex, 0);
	glUniform1i(mMaskUnifMask, 1);
	glUniform2f(mMaskUnifRes, (float)scrW, (float)scrH);
	if (maskTex) {
		glUniform4f(mMaskUnifRect, x, y, w, h);
		glUniform2f(mMaskUnifUV, uScale, vScale);
		glUniform1i(mMaskUnifUseMask, 1);
		glActiveTexture(GL_TEXTURE1);
		glBindTexture(GL_TEXTURE_2D, maskTex);
	} else {
		glUniform1i(mMaskUnifUseMask, 0);
	}
	glActiveTexture(GL_TEXTURE0);
	glBindTexture(GL_TEXTURE_2D, srcTex);
	glVertexAttribPointer(mMaskAttrPos, 2, GL_FLOAT, GL_FALSE, 0, (GLvoid *)position);
	glVertexAttribPointer(mMaskAttrUV,  2, GL_FLOAT, GL_FALSE, 0, (GLvoid *)uv);
	glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);

	if (scissorBox) {
		glDisable(GL_SCISSOR_TEST);
	}
}
