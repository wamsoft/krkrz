// ---------------------------------------------------------------------------
// 描画後処理（ポストエフェクト）基盤 (GLEffect.h 参照)
//   gles プラグイン (GLESAdaptor) の GLEffect.cpp からの移植。
//   ncbind 依存を TJS ネイティブ API に置き換えている以外はロジック同一。
// ---------------------------------------------------------------------------
#include "tjsCommHead.h"
#include "GLEffect.h"
#include "GLShaderUtil.h"
#include "DebugIntf.h"
#include "tjsArray.h"

#include <string>
#include <cmath>

typedef unsigned char u8;

// ===========================================================================
// TJS 辞書/配列アクセスヘルパ (ncbPropAccessor 相当の最小機能)
// ===========================================================================
namespace {

bool DictGet(iTJSDispatch2 *obj, const tjs_char *name, tTJSVariant &out)
{
	if (!obj) return false;
	return TJS_SUCCEEDED(obj->PropGet(TJS_MEMBERMUSTEXIST, name, nullptr, &out, obj));
}

bool DictHas(iTJSDispatch2 *obj, const tjs_char *name)
{
	tTJSVariant v;
	return DictGet(obj, name, v) && v.Type() != tvtVoid;
}

double DictReal(iTJSDispatch2 *obj, const tjs_char *name, double def)
{
	tTJSVariant v;
	if (DictGet(obj, name, v) && v.Type() != tvtVoid) return (double)v.AsReal();
	return def;
}

tjs_int DictInt(iTJSDispatch2 *obj, const tjs_char *name, tjs_int def)
{
	tTJSVariant v;
	if (DictGet(obj, name, v) && v.Type() != tvtVoid) return (tjs_int)v.AsInteger();
	return def;
}

ttstr DictStr(iTJSDispatch2 *obj, const tjs_char *name)
{
	tTJSVariant v;
	if (DictGet(obj, name, v) && v.Type() != tvtVoid) return ttstr(v);
	return ttstr();
}

// Array インスタンスの Items を得る (Array でなければ nullptr)
tTJSArrayNI *GetArrayNI(const tTJSVariant &v)
{
	if (v.Type() != tvtObject) return nullptr;
	iTJSDispatch2 *obj = v.AsObjectNoAddRef();
	if (!obj) return nullptr;
	tTJSArrayNI *ni = nullptr;
	if (TJS_FAILED(obj->NativeInstanceSupport(TJS_NIS_GETINSTANCE,
			TJSGetArrayClassID(), (iTJSNativeInstance **)&ni))) return nullptr;
	return ni;
}

} // anonymous

// ===========================================================================
// GLFboPool
// ===========================================================================
GLFrameBufferObject *GLFboPool::acquire(int w, int h)
{
	for (size_t i = 0; i < mFree.size(); i++) {
		GLFrameBufferObject *fbo = mFree[i];
		if ((int)fbo->width() == w && (int)fbo->height() == h) {
			mFree.erase(mFree.begin() + i);
			return fbo;
		}
	}
	if (!mFree.empty()) {
		GLFrameBufferObject *fbo = mFree.back();
		mFree.pop_back();
		fbo->create(w, h, /*with_pbo=*/false);
		return fbo;
	}
	GLFrameBufferObject *fbo = new GLFrameBufferObject();
	fbo->create(w, h, /*with_pbo=*/false);
	mAll.push_back(fbo);
	return fbo;
}

void GLFboPool::release(GLFrameBufferObject *fbo)
{
	if (fbo) mFree.push_back(fbo);
}

void GLFboPool::clear()
{
	for (size_t i = 0; i < mAll.size(); i++) delete mAll[i];
	mAll.clear();
	mFree.clear();
}

// ===========================================================================
// シェーダ
// ===========================================================================

// 共通頂点シェーダ(フルスクリーン矩形)
static const char *kVsSource =
	"#version 300 es\n"
	"in vec2 a_position;\n"
	"in vec2 a_texCoord;\n"
	"out vec2 v_uv;\n"
	"void main() {\n"
	"  gl_Position = vec4(a_position, 0.0, 1.0);\n"
	"  v_uv = a_texCoord;\n"
	"}\n";

// 混合系点処理フラグメントシェーダ(オペコードループ)
//   オペコードは enum GLEffectOp と一致させること。
static std::string buildPointwiseFs()
{
	std::string n = std::to_string(GLEFFECT_MAX_OPS);
	std::string s;
	s += "#version 300 es\n";
	s += "precision highp float;\n";
	s += "uniform sampler2D s_tex;\n";
	s += "uniform int u_count;\n";
	s += "uniform int u_ops[" + n + "];\n";
	s += "uniform vec4 u_params[" + n + "];\n";
	s += "uniform float u_seed;\n";
	s += "uniform vec2 u_res;\n";
	s += "in vec2 v_uv;\n";
	s += "out vec4 fragColor;\n";

	// 座標ハッシュ(rand() の代替)
	s += "float hash12(vec2 p) {\n";
	s += "  vec3 p3 = fract(vec3(p.xyx) * 0.1031);\n";
	s += "  p3 += dot(p3, p3.yzx + 33.33);\n";
	s += "  return fract((p3.x + p3.y) * p3.z);\n";
	s += "}\n";

	// HSL <-> RGB (LayerExImage modulate と同じアルゴリズム)
	s += "float hue2rgb(float m1, float m2, float h) {\n";
	s += "  if (h < 0.0) h += 1.0; if (h > 1.0) h -= 1.0;\n";
	s += "  if (h < 1.0/6.0) return m1 + (m2-m1)*6.0*h;\n";
	s += "  if (h < 1.0/2.0) return m2;\n";
	s += "  if (h < 2.0/3.0) return m1 + (m2-m1)*(2.0/3.0-h)*6.0;\n";
	s += "  return m1;\n";
	s += "}\n";
	s += "vec3 hslToRgb(float h, float s, float l) {\n";
	s += "  if (s <= 0.0) return vec3(l);\n";
	s += "  float m2 = (l <= 0.5) ? l*(1.0+s) : l+s-l*s;\n";
	s += "  float m1 = 2.0*l - m2;\n";
	s += "  return vec3(hue2rgb(m1,m2,h+1.0/3.0), hue2rgb(m1,m2,h), hue2rgb(m1,m2,h-1.0/3.0));\n";
	s += "}\n";
	s += "vec3 rgbToHsl(vec3 c) {\n";
	s += "  float cMax = max(max(c.r,c.g),c.b);\n";
	s += "  float cMin = min(min(c.r,c.g),c.b);\n";
	s += "  float delta = cMax - cMin;\n";
	s += "  float add = cMax + cMin;\n";
	s += "  float l = add*0.5; float h = 0.0; float sat = 0.0;\n";
	s += "  if (delta > 0.0) {\n";
	s += "    sat = (l < 0.5) ? delta/add : delta/(2.0-add);\n";
	s += "    if (c.r == cMax) h = (c.g-c.b)/delta;\n";
	s += "    else if (c.g == cMax) h = 2.0 + (c.b-c.r)/delta;\n";
	s += "    else h = 4.0 + (c.r-c.g)/delta;\n";
	s += "    h /= 6.0; if (h < 0.0) h += 1.0;\n";
	s += "  }\n";
	s += "  return vec3(h, sat, l);\n";
	s += "}\n";

	// Photoshop 合成モード(吉里吉里 GPU描画の blendmode glsl 準拠)
	s += "vec3 bScreen(vec3 d, vec3 s) { return vec3(1.0) - (vec3(1.0)-d)*(vec3(1.0)-s); }\n";
	s += "float bOverlay(float a, float b) { return (a < 0.5) ? a*b*2.0 : 1.0 - (1.0-a)*(1.0-b)*2.0; }\n";
	s += "vec3 bOverlay(vec3 d, vec3 s) { return vec3(bOverlay(d.r,s.r), bOverlay(d.g,s.g), bOverlay(d.b,s.b)); }\n";
	s += "float bHard(float a, float b) { return (b < 0.5) ? a*b*2.0 : 1.0 - (1.0-a)*(1.0-b)*2.0; }\n";
	s += "vec3 bHard(vec3 d, vec3 s) { return vec3(bHard(d.r,s.r), bHard(d.g,s.g), bHard(d.b,s.b)); }\n";
	s += "float bSoft(float a, float b) { return (b < 0.5) ? (2.0*a*b + a*a*(1.0-2.0*b)) : (sqrt(a)*(2.0*b-1.0) + 2.0*a*(1.0-b)); }\n";
	s += "vec3 bSoft(vec3 d, vec3 s) { return vec3(bSoft(d.r,s.r), bSoft(d.g,s.g), bSoft(d.b,s.b)); }\n";
	// dest に src(定数色) を alpha・mode で合成。mode は tTVPLayerType 値。
	s += "vec3 applyOver(vec3 d, vec3 s, float a, int mode) {\n";
	s += "  if (mode == 1) return s * a;\n";                                              // opaque
	s += "  else if (mode == 3) return min(vec3(1.0), d + s);\n";                          // additive
	s += "  else if (mode == 4) return max(vec3(0.0), d + s - vec3(1.0));\n";              // subtractive
	s += "  else if (mode == 5) return d * s;\n";                                          // multiplicative
	s += "  else if (mode == 8) return min(vec3(1.0), d / (vec3(1.0) - s));\n";            // dodge
	s += "  else if (mode == 9) return min(d, s);\n";                                      // darken
	s += "  else if (mode == 10) return max(d, s);\n";                                     // lighten
	s += "  else if (mode == 11) return bScreen(d, s);\n";                                 // screen
	s += "  else if (mode == 12) return min(vec3(1.0), d*(1.0-a) + s);\n";                 // addalpha
	s += "  else if (mode == 14) return mix(d, min(vec3(1.0), d+s), a);\n";                // psAdditive
	s += "  else if (mode == 15) return mix(d, max(vec3(0.0), d+s-vec3(1.0)), a);\n";      // psSubtractive
	s += "  else if (mode == 16) return mix(d, d*s, a);\n";                                // psMultiplicative
	s += "  else if (mode == 17) return mix(d, bScreen(d,s), a);\n";                       // psScreen
	s += "  else if (mode == 18) return mix(d, bOverlay(d,s), a);\n";                      // psOverlay
	s += "  else if (mode == 19) return mix(d, bHard(d,s), a);\n";                         // psHardLight
	s += "  else if (mode == 20) return mix(d, bSoft(d,s), a);\n";                         // psSoftLight
	s += "  else if (mode == 21) return mix(d, min(vec3(1.0), d/(vec3(1.0)-s)), a);\n";    // psColorDodge
	s += "  else if (mode == 22) return min(vec3(1.0), d / (vec3(1.0) - s*a));\n";         // psColorDodge5
	s += "  else if (mode == 23) return mix(d, max(vec3(0.0), vec3(1.0)-(vec3(1.0)-d)/s), a);\n"; // psColorBurn
	s += "  else if (mode == 24) return mix(d, max(d,s), a);\n";                           // psLighten
	s += "  else if (mode == 25) return mix(d, min(d,s), a);\n";                           // psDarken
	s += "  else if (mode == 26) return mix(d, abs(d-s), a);\n";                           // psDifference
	s += "  else if (mode == 27) return abs(d - s*a);\n";                                  // psDifference5
	s += "  else if (mode == 28) return mix(d, d + s - 2.0*s*d, a);\n";                    // psExclusion
	s += "  return mix(d, s, a);\n";                                                       // alpha / psNormal(2,13) / 既定
	s += "}\n";

	s += "void main() {\n";
	s += "  vec4 c = texture(s_tex, v_uv);\n";
	s += "  vec2 px = v_uv * u_res;\n";
	s += "  for (int i = 0; i < u_count; i++) {\n";
	s += "    int op = u_ops[i];\n";
	s += "    vec4 p = u_params[i];\n";
	// overcolor: op >= base のとき mode=op-base、p=塗り色(rgba)
	s += "    if (op >= " + std::to_string(GLEFFECT_OVERCOLOR_BASE) + ") {\n";
	s += "      c.rgb = applyOver(c.rgb, p.rgb, p.a, op - " + std::to_string(GLEFFECT_OVERCOLOR_BASE) + ");\n";
	// grayscale: Y = (19*B + 183*G + 54*R) >> 8
	s += "    } else if (op == 1) {\n";
	s += "      vec3 s255 = c.rgb * 255.0;\n";
	s += "      float y = floor((s255.b*19.0 + s255.g*183.0 + s255.r*54.0) / 256.0);\n";
	s += "      c.rgb = vec3(y / 255.0);\n";
	// colorize: p = (hueN, satN, blend)
	s += "    } else if (op == 2) {\n";
	s += "      vec3 hsl = rgbToHsl(c.rgb);\n";
	s += "      vec3 rgb = hslToRgb(p.x, p.y, hsl.z);\n";
	s += "      c.rgb = mix(c.rgb, rgb, p.z);\n";
	// modulate: p = (h, s, l) 正規化加算
	s += "    } else if (op == 3) {\n";
	s += "      vec3 hsl = rgbToHsl(c.rgb);\n";
	s += "      float h = fract(hsl.x + p.x);\n";
	s += "      float ss = hsl.y; float ll = hsl.z;\n";
	s += "      ss += (p.y > 0.0) ? (1.0-ss)*p.y : ss*p.y;\n";
	s += "      ll += (p.z > 0.0) ? (1.0-ll)*p.z : ll*p.z;\n";
	s += "      c.rgb = hslToRgb(h, ss, ll);\n";
	// noise: p.x = level (0..255)
	s += "    } else if (op == 4) {\n";
	s += "      float lv = p.x / 255.0;\n";
	s += "      float n0 = (hash12(px + vec2(u_seed, 1.0)) - 0.5) * lv;\n";
	s += "      float n1 = (hash12(px + vec2(7.0, u_seed)) - 0.5) * lv;\n";
	s += "      float n2 = (hash12(px + vec2(u_seed, u_seed+3.0)) - 0.5) * lv;\n";
	s += "      c.rgb = clamp(c.rgb + vec3(n0,n1,n2), 0.0, 1.0);\n";
	// generateWhiteNoise
	s += "    } else if (op == 5) {\n";
	s += "      float nn = floor(hash12(px + vec2(u_seed, u_seed)) * 255.0) / 255.0;\n";
	s += "      c.rgb = vec3(nn);\n";
	s += "    }\n";
	s += "  }\n";
	s += "  fragColor = c;\n";
	s += "}\n";
	return s;
}

// per-channel LUT フラグメントシェーダ
static std::string buildLutFs()
{
	std::string s;
	s += "#version 300 es\n";
	s += "precision highp float;\n";
	s += "uniform sampler2D s_tex;\n";
	s += "uniform sampler2D s_lut;\n";   // 256x1 RGBA8 (NEAREST)
	s += "in vec2 v_uv;\n";
	s += "out vec4 fragColor;\n";
	s += "float idx(float v) { return (v*255.0 + 0.5) / 256.0; }\n";
	s += "void main() {\n";
	s += "  vec4 c = texture(s_tex, v_uv);\n";
	s += "  float r = texture(s_lut, vec2(idx(c.r), 0.5)).r;\n";
	s += "  float g = texture(s_lut, vec2(idx(c.g), 0.5)).g;\n";
	s += "  float b = texture(s_lut, vec2(idx(c.b), 0.5)).b;\n";
	s += "  fragColor = vec4(r, g, b, c.a);\n";
	s += "}\n";
	return s;
}

// 重み付き分離ブラーフラグメントシェーダ(box / gaussian 共通)
static std::string buildBlurFs()
{
	std::string m = std::to_string(GLEFFECT_MAX_BLUR);
	std::string len = std::to_string(2 * GLEFFECT_MAX_BLUR + 1);
	std::string s;
	s += "#version 300 es\n";
	s += "precision highp float;\n";
	s += "uniform sampler2D s_tex;\n";
	s += "uniform vec2 u_dir;\n";
	s += "uniform int u_radius;\n";
	s += "uniform float u_weights[" + len + "];\n";
	s += "in vec2 v_uv;\n";
	s += "out vec4 fragColor;\n";
	s += "void main() {\n";
	s += "  vec4 sum = vec4(0.0);\n";
	s += "  for (int i = -" + m + "; i <= " + m + "; i++) {\n";
	s += "    if (i < -u_radius || i > u_radius) continue;\n";
	s += "    sum += texture(s_tex, v_uv + u_dir * float(i)) * u_weights[i + u_radius];\n";
	s += "  }\n";
	s += "  fragColor = sum;\n";
	s += "}\n";
	return s;
}

// ===========================================================================
// GLEffectContext
// ===========================================================================
GLEffectContext::GLEffectContext()
	: mInited(false), mSeed(0.0f)
	, mPwProgram(0), mPwAttrPos(-1), mPwAttrUV(-1)
	, mPwUnifTex(-1), mPwUnifCount(-1), mPwUnifOps(-1), mPwUnifParams(-1), mPwUnifSeed(-1), mPwUnifRes(-1)
	, mLutProgram(0), mLutAttrPos(-1), mLutAttrUV(-1), mLutUnifTex(-1), mLutUnifLut(-1)
	, mBlurProgram(0), mBlurAttrPos(-1), mBlurAttrUV(-1)
	, mBlurUnifTex(-1), mBlurUnifDir(-1), mBlurUnifRadius(-1), mBlurUnifWeights(-1)
{
}

GLEffectContext::~GLEffectContext()
{
	done();
}

void GLEffectContext::init()
{
	if (mInited) return;

	mPwProgram = CompileProgram(kVsSource, buildPointwiseFs());
	if (mPwProgram) {
		mPwAttrPos    = glGetAttribLocation(mPwProgram, "a_position");
		mPwAttrUV     = glGetAttribLocation(mPwProgram, "a_texCoord");
		mPwUnifTex    = glGetUniformLocation(mPwProgram, "s_tex");
		mPwUnifCount  = glGetUniformLocation(mPwProgram, "u_count");
		mPwUnifOps    = glGetUniformLocation(mPwProgram, "u_ops");
		mPwUnifParams = glGetUniformLocation(mPwProgram, "u_params");
		mPwUnifSeed   = glGetUniformLocation(mPwProgram, "u_seed");
		mPwUnifRes    = glGetUniformLocation(mPwProgram, "u_res");
	}

	mLutProgram = CompileProgram(kVsSource, buildLutFs());
	if (mLutProgram) {
		mLutAttrPos = glGetAttribLocation(mLutProgram, "a_position");
		mLutAttrUV  = glGetAttribLocation(mLutProgram, "a_texCoord");
		mLutUnifTex = glGetUniformLocation(mLutProgram, "s_tex");
		mLutUnifLut = glGetUniformLocation(mLutProgram, "s_lut");
	}

	mBlurProgram = CompileProgram(kVsSource, buildBlurFs());
	if (mBlurProgram) {
		mBlurAttrPos     = glGetAttribLocation(mBlurProgram, "a_position");
		mBlurAttrUV      = glGetAttribLocation(mBlurProgram, "a_texCoord");
		mBlurUnifTex     = glGetUniformLocation(mBlurProgram, "s_tex");
		mBlurUnifDir     = glGetUniformLocation(mBlurProgram, "u_dir");
		mBlurUnifRadius  = glGetUniformLocation(mBlurProgram, "u_radius");
		mBlurUnifWeights = glGetUniformLocation(mBlurProgram, "u_weights");
	}

	mInited = true;
}

void GLEffectContext::done()
{
	if (mPwProgram)   { glDeleteProgram(mPwProgram);   mPwProgram = 0; }
	if (mLutProgram)  { glDeleteProgram(mLutProgram);  mLutProgram = 0; }
	if (mBlurProgram) { glDeleteProgram(mBlurProgram); mBlurProgram = 0; }
	mInited = false;
}

// フルスクリーン矩形(TRIANGLE_STRIP)。
//   uv(0,0)->ndc(-1,-1) の素直な対応。上下反転は導入しない
//   (向きを保存するので捕捉テクスチャの向き規約に依存しない)。
void GLEffectContext::drawQuad(GLint attrPos, GLint attrUV, const GLint *scissorBox)
{
	static const GLfloat position[8] = {
		-1.0f, -1.0f,  -1.0f,  1.0f,   1.0f, -1.0f,   1.0f,  1.0f,
	};
	static const GLfloat uv[8] = {
		 0.0f,  0.0f,   0.0f,  1.0f,   1.0f,  0.0f,   1.0f,  1.0f,
	};

	glDisable(GL_DEPTH_TEST);
	glDisable(GL_STENCIL_TEST);
	glDisable(GL_CULL_FACE);
	// 矩形クリップは最終合成時のみ指定される (チェーン中間パスは全面処理)
	if (scissorBox) {
		glEnable(GL_SCISSOR_TEST);
		glScissor(scissorBox[0], scissorBox[1], scissorBox[2], scissorBox[3]);
	} else {
		glDisable(GL_SCISSOR_TEST);
	}

	glEnableVertexAttribArray(attrPos);
	glEnableVertexAttribArray(attrUV);
	glVertexAttribPointer(attrPos, 2, GL_FLOAT, GL_FALSE, 0, (GLvoid *)position);
	glVertexAttribPointer(attrUV,  2, GL_FLOAT, GL_FALSE, 0, (GLvoid *)uv);
	glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
}

void GLEffectContext::drawPointwise(GLuint srcTex, const int *ops, const float *params, int count, int w, int h,
                                    const GLint *scissorBox)
{
	if (!mPwProgram) return;
	if (count > GLEFFECT_MAX_OPS) count = GLEFFECT_MAX_OPS;

	glUseProgram(mPwProgram);
	glUniform1i(mPwUnifTex, 0);
	glUniform1i(mPwUnifCount, count);
	glUniform1f(mPwUnifSeed, mSeed);
	glUniform2f(mPwUnifRes, (float)w, (float)h);
	if (count > 0) {
		glUniform1iv(mPwUnifOps, count, ops);
		glUniform4fv(mPwUnifParams, count, params);
	}
	glActiveTexture(GL_TEXTURE0);
	glBindTexture(GL_TEXTURE_2D, srcTex);
	drawQuad(mPwAttrPos, mPwAttrUV, scissorBox);
}

void GLEffectContext::drawLut(GLuint srcTex, GLuint lutTex)
{
	if (!mLutProgram) return;

	glUseProgram(mLutProgram);
	glUniform1i(mLutUnifTex, 0);
	glUniform1i(mLutUnifLut, 1);
	glActiveTexture(GL_TEXTURE1);
	glBindTexture(GL_TEXTURE_2D, lutTex);
	glActiveTexture(GL_TEXTURE0);
	glBindTexture(GL_TEXTURE_2D, srcTex);
	drawQuad(mLutAttrPos, mLutAttrUV);
}

void GLEffectContext::drawBlur(GLuint srcTex, float dx, float dy, int radius, const float *weights)
{
	if (!mBlurProgram) return;
	if (radius < 0) radius = 0;
	if (radius > GLEFFECT_MAX_BLUR) radius = GLEFFECT_MAX_BLUR;

	glUseProgram(mBlurProgram);
	glUniform1i(mBlurUnifTex, 0);
	glUniform2f(mBlurUnifDir, dx, dy);
	glUniform1i(mBlurUnifRadius, radius);
	glUniform1fv(mBlurUnifWeights, 2 * radius + 1, weights);
	glActiveTexture(GL_TEXTURE0);
	glBindTexture(GL_TEXTURE_2D, srcTex);
	drawQuad(mBlurAttrPos, mBlurAttrUV);
}

// ===========================================================================
// CPU 側テーブル生成(kirikiri / LayerExImage の演算を厳密移植)
// ===========================================================================

// TVPInitGammaAdjustTempData 相当(1チャンネル)
static void buildGammaTable(u8 out[256], double gamma, int floorv, int ceilv)
{
	if (gamma <= 0.0) gamma = 1.0;
	double amp = (double)(ceilv - floorv);
	double g = 1.0 / gamma;
	for (int i = 0; i < 256; i++) {
		int n;
		if (i == 0) {
			n = (int)(0.0 * amp + 0.5 + (double)floorv);
		} else {
			double rate = log((double)i / 255.0);
			n = (int)(exp(rate * g) * amp + 0.5 + (double)floorv);
		}
		if (n < 0) n = 0; else if (n > 255) n = 255;
		out[i] = (u8)n;
	}
}

// LayerExImage::light 相当(明度/コントラスト)
static void buildLightTable(u8 out[256], int brightness, int contrast)
{
	float c = (100 + contrast) / 100.0f;
	int b = brightness + 128;
	for (int i = 0; i < 256; i++) {
		int v = (int)((i - 128) * c + b);
		if (v < 0) v = 0; else if (v > 255) v = 255;
		out[i] = (u8)v;
	}
}

// gen_convolve_matrix の移植(正規化ガウス行列、長さを返す)
static int genConvolveMatrix(float radius, std::vector<float> &matrix)
{
	radius = (float)fabs(0.5 * radius) + 0.25f;
	float std_dev = radius;
	radius = std_dev * 2;

	int matrix_length = (int)(2 * ceil(radius - 0.5) + 1);
	if (matrix_length <= 0) matrix_length = 1;
	matrix.assign(matrix_length, 0.0f);

	for (int i = matrix_length / 2 + 1; i < matrix_length; i++) {
		float base_x = i - (float)floor((float)(matrix_length / 2)) - 0.5f;
		float sum = 0;
		for (int j = 1; j <= 50; j++) {
			if (base_x + 0.02 * j <= radius)
				sum += (float)exp(-(base_x + 0.02 * j) * (base_x + 0.02 * j) / (2 * std_dev * std_dev));
		}
		matrix[i] = sum / 50;
	}
	for (int i = 0; i <= matrix_length / 2; i++)
		matrix[i] = matrix[matrix_length - 1 - i];

	float sum = 0;
	for (int j = 0; j <= 50; j++)
		sum += (float)exp(-(0.5 + 0.02 * j) * (0.5 + 0.02 * j) / (2 * std_dev * std_dev));
	matrix[matrix_length / 2] = sum / 51;

	sum = 0;
	for (int i = 0; i < matrix_length; i++) sum += matrix[i];
	for (int i = 0; i < matrix_length; i++) matrix[i] /= sum;

	return matrix_length;
}

// ===========================================================================
// ステージ実装
// ===========================================================================

// --- LUT系(gamma / light / lut を1枚に合成) ----------------------------
class GLLutStage : public GLEffectStage {
public:
	GLLutStage() : mTex(0) {
		for (int i = 0; i < 256; i++) { mR[i] = mG[i] = mB[i] = (u8)i; }
	}
	~GLLutStage() {
		if (mTex) { glDeleteTextures(1, &mTex); mTex = 0; }
	}

	// 現在のテーブルに per-channel テーブルを合成(out = cmd(current))
	void compose(const u8 r[256], const u8 g[256], const u8 b[256]) {
		for (int i = 0; i < 256; i++) {
			mR[i] = r[mR[i]];
			mG[i] = g[mG[i]];
			mB[i] = b[mB[i]];
		}
	}
	void composeAll(const u8 t[256]) { compose(t, t, t); }

	virtual void render(GLuint srcTex, int w, int h,
	                    GLFrameBufferObject *dst, GLFboPool &pool, GLEffectContext &ctx) override {
		if (!mTex) buildTexture();
		dst->bindFramebuffer();
		glDisable(GL_BLEND);
		ctx.drawLut(srcTex, mTex);
	}

private:
	u8 mR[256], mG[256], mB[256];
	GLuint mTex;

	void buildTexture() {
		u8 data[256 * 4];
		for (int i = 0; i < 256; i++) {
			data[i * 4 + 0] = mR[i];
			data[i * 4 + 1] = mG[i];
			data[i * 4 + 2] = mB[i];
			data[i * 4 + 3] = 255;
		}
		glGenTextures(1, &mTex);
		glBindTexture(GL_TEXTURE_2D, mTex);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
		glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, 256, 1, 0, GL_RGBA, GL_UNSIGNED_BYTE, data);
		glBindTexture(GL_TEXTURE_2D, 0);
	}
};

// --- 混合系(オペコードループ1パス) -------------------------------------
class GLPointwiseStage : public GLEffectStage {
public:
	void add(int op, float p0, float p1, float p2, float p3) {
		if ((int)mOps.size() >= GLEFFECT_MAX_OPS) return;
		mOps.push_back(op);
		mParams.push_back(p0); mParams.push_back(p1);
		mParams.push_back(p2); mParams.push_back(p3);
	}
	bool full() const { return (int)mOps.size() >= GLEFFECT_MAX_OPS; }

	virtual void render(GLuint srcTex, int w, int h,
	                    GLFrameBufferObject *dst, GLFboPool &pool, GLEffectContext &ctx) override {
		dst->bindFramebuffer();
		glDisable(GL_BLEND);
		ctx.drawPointwise(srcTex, mOps.data(), mParams.data(), (int)mOps.size(), w, h);
	}

private:
	std::vector<int>   mOps;
	std::vector<float> mParams;
};

// --- 近傍系(重み付き分離畳み込み、box / gaussian) ----------------------
class GLWeightedBlurStage : public GLEffectStage {
public:
	GLWeightedBlurStage(int radius, int iterations, const std::vector<float> &weights)
		: mRadius(radius), mIter(iterations < 1 ? 1 : iterations), mWeights(weights) {}

	virtual void render(GLuint srcTex, int w, int h,
	                    GLFrameBufferObject *dst, GLFboPool &pool, GLEffectContext &ctx) override {
		float dx = (w > 0) ? 1.0f / (float)w : 0.0f;
		float dy = (h > 0) ? 1.0f / (float)h : 0.0f;
		const float *wp = mWeights.data();

		GLuint input = srcTex;
		GLFrameBufferObject *carried = nullptr;

		for (int it = 0; it < mIter; it++) {
			bool lastIter = (it == mIter - 1);

			GLFrameBufferObject *hbuf = pool.acquire(w, h);
			hbuf->bindFramebuffer();
			glDisable(GL_BLEND);
			ctx.drawBlur(input, dx, 0.0f, mRadius, wp);

			GLFrameBufferObject *vout = lastIter ? dst : pool.acquire(w, h);
			vout->bindFramebuffer();
			glDisable(GL_BLEND);
			ctx.drawBlur(hbuf->textureId(), 0.0f, dy, mRadius, wp);

			pool.release(hbuf);
			if (carried) pool.release(carried);

			if (!lastIter) { input = vout->textureId(); carried = vout; }
		}
	}

private:
	int mRadius;
	int mIter;
	std::vector<float> mWeights;
};

// ===========================================================================
// GLEffectChain
// ===========================================================================
void GLEffectChain::clear()
{
	for (size_t i = 0; i < mStages.size(); i++) delete mStages[i];
	mStages.clear();
}

void GLEffectChain::compile(const tTJSVariant &commandArray)
{
	clear();
	tTJSArrayNI *arr = GetArrayNI(commandArray);
	if (!arr) return;

	GLLutStage       *lut = nullptr;   // 連続するLUT系の合成先
	GLPointwiseStage *pw  = nullptr;   // 連続する混合系の融合先

	for (const tTJSVariant &elm : arr->Items) {
		if (elm.Type() != tvtObject) continue;
		iTJSDispatch2 *cmd = elm.AsObjectNoAddRef();
		if (!cmd) continue;
		ttstr name = DictStr(cmd, TJS_W("cmd"));

		// ---- LUT系 ----
		if (name == TJS_W("gamma") || name == TJS_W("adjustGamma")) {
			pw = nullptr;
			if (!lut) { lut = new GLLutStage(); mStages.push_back(lut); }
			u8 tr[256], tg[256], tb[256];
			if (DictHas(cmd, TJS_W("value"))) {
				double v = DictReal(cmd, TJS_W("value"), 1.0);
				buildGammaTable(tr, v, 0, 255);
				buildGammaTable(tg, v, 0, 255);
				buildGammaTable(tb, v, 0, 255);
			} else {
				buildGammaTable(tr, DictReal(cmd, TJS_W("rgamma"), 1.0),
				                DictInt(cmd, TJS_W("rfloor"), 0), DictInt(cmd, TJS_W("rceil"), 255));
				buildGammaTable(tg, DictReal(cmd, TJS_W("ggamma"), 1.0),
				                DictInt(cmd, TJS_W("gfloor"), 0), DictInt(cmd, TJS_W("gceil"), 255));
				buildGammaTable(tb, DictReal(cmd, TJS_W("bgamma"), 1.0),
				                DictInt(cmd, TJS_W("bfloor"), 0), DictInt(cmd, TJS_W("bceil"), 255));
			}
			lut->compose(tr, tg, tb);

		} else if (name == TJS_W("light")) {
			pw = nullptr;
			if (!lut) { lut = new GLLutStage(); mStages.push_back(lut); }
			u8 t[256];
			buildLightTable(t, DictInt(cmd, TJS_W("brightness"), 0), DictInt(cmd, TJS_W("contrast"), 0));
			lut->composeAll(t);

		} else if (name == TJS_W("lut")) {
			pw = nullptr;
			if (!lut) { lut = new GLLutStage(); mStages.push_back(lut); }
			u8 t[256];
			tTJSVariant tableVar;
			DictGet(cmd, TJS_W("table"), tableVar);
			tTJSArrayNI *tbl = GetArrayNI(tableVar);
			if (tbl) {
				int n = (int)tbl->Items.size();
				for (int k = 0; k < 256; k++) {
					int v = (k < n) ? (int)tbl->Items[k].AsInteger() : k;
					if (v < 0) v = 0; else if (v > 255) v = 255;
					t[k] = (u8)v;
				}
				lut->composeAll(t);
			}

		// ---- 混合系 ----
		} else if (name == TJS_W("grayscale")) {
			lut = nullptr;
			if (!pw || pw->full()) { pw = new GLPointwiseStage(); mStages.push_back(pw); }
			pw->add(GLEOP_GRAYSCALE, 0, 0, 0, 0);

		} else if (name == TJS_W("colorize")) {
			lut = nullptr;
			if (!pw || pw->full()) { pw = new GLPointwiseStage(); mStages.push_back(pw); }
			float hue = (float)DictInt(cmd, TJS_W("hue"), 0) / 255.0f;
			float sat = (float)DictInt(cmd, TJS_W("sat"), DictInt(cmd, TJS_W("saturation"), 0)) / 255.0f;
			float blend = (float)DictReal(cmd, TJS_W("blend"), 1.0);
			if (blend < 0.0f) blend = 0.0f;
			if (blend > 1.0f) blend = 1.0f;
			pw->add(GLEOP_COLORIZE, hue, sat, blend, 0);

		} else if (name == TJS_W("modulate")) {
			lut = nullptr;
			if (!pw || pw->full()) { pw = new GLPointwiseStage(); mStages.push_back(pw); }
			float h = (float)(DictInt(cmd, TJS_W("hue"), 0) / 360.0);
			float s = (float)(DictInt(cmd, TJS_W("sat"), DictInt(cmd, TJS_W("saturation"), 0)) / 100.0);
			float l = (float)(DictInt(cmd, TJS_W("lum"), DictInt(cmd, TJS_W("luminance"), 0)) / 100.0);
			pw->add(GLEOP_MODULATE, h, s, l, 0);

		} else if (name == TJS_W("noise")) {
			lut = nullptr;
			if (!pw || pw->full()) { pw = new GLPointwiseStage(); mStages.push_back(pw); }
			pw->add(GLEOP_NOISE, (float)DictInt(cmd, TJS_W("level"), 0), 0, 0, 0);

		} else if (name == TJS_W("generateWhiteNoise") || name == TJS_W("whiteNoise")) {
			lut = nullptr;
			if (!pw || pw->full()) { pw = new GLPointwiseStage(); mStages.push_back(pw); }
			pw->add(GLEOP_WHITENOISE, 0, 0, 0, 0);

		} else if (name == TJS_W("overcolor")) {
			// 指定色での全体塗りつぶし合成(吉里吉里 fillOperateRect 準拠)
			lut = nullptr;
			if (!pw || pw->full()) { pw = new GLPointwiseStage(); mStages.push_back(pw); }
			tjs_uint32 col = (tjs_uint32)DictInt(cmd, TJS_W("color"), 0);
			float a = (float)((col >> 24) & 0xff) / 255.0f;
			float r = (float)((col >> 16) & 0xff) / 255.0f;
			float g = (float)((col >>  8) & 0xff) / 255.0f;
			float b = (float)((col      ) & 0xff) / 255.0f;
			int opacity = DictInt(cmd, TJS_W("opacity"), 255);
			a *= (float)opacity / 255.0f;
			int type = DictInt(cmd, TJS_W("type"), 2);  // 既定 omAlpha(2)
			pw->add(GLEFFECT_OVERCOLOR_BASE + type, r, g, b, a);

		// ---- 近傍系 ----
		} else if (name == TJS_W("boxBlur")) {
			lut = nullptr; pw = nullptr;
			int radius = DictInt(cmd, TJS_W("area"), DictInt(cmd, TJS_W("radius"), 1));
			int iter   = DictInt(cmd, TJS_W("iter"), 1);
			if (radius < 0) radius = 0;
			if (radius > GLEFFECT_MAX_BLUR) radius = GLEFFECT_MAX_BLUR;
			std::vector<float> weights(2 * radius + 1, 1.0f / (float)(2 * radius + 1));
			mStages.push_back(new GLWeightedBlurStage(radius, iter, weights));

		} else if (name == TJS_W("gaussianBlur")) {
			lut = nullptr; pw = nullptr;
			float r = (float)DictReal(cmd, TJS_W("radius"), 1.0);
			std::vector<float> matrix;
			int mlen = genConvolveMatrix(r, matrix);
			int radius = mlen / 2;
			if (radius > GLEFFECT_MAX_BLUR) {
				// 行列が大きすぎる場合は中心を切り出してクランプ(裾を捨てる)
				TVPAddLog(TJS_W("GLEffect: gaussianBlur radius clamped"));
				int start = radius - GLEFFECT_MAX_BLUR;
				std::vector<float> trimmed(matrix.begin() + start, matrix.begin() + start + (2 * GLEFFECT_MAX_BLUR + 1));
				float s = 0; for (size_t k = 0; k < trimmed.size(); k++) s += trimmed[k];
				for (size_t k = 0; k < trimmed.size(); k++) trimmed[k] /= s;
				matrix.swap(trimmed);
				radius = GLEFFECT_MAX_BLUR;
			}
			mStages.push_back(new GLWeightedBlurStage(radius, 1, matrix));

		} else {
			TVPAddLog(TJS_W("GLEffect: unknown command: ") + name);
		}
	}
}

GLFrameBufferObject *GLEffectChain::apply(GLuint srcTex, int w, int h, GLFboPool &pool, GLEffectContext &ctx)
{
	if (mStages.empty()) return nullptr;

	GLuint input = srcTex;
	GLFrameBufferObject *prev = nullptr;

	for (size_t i = 0; i < mStages.size(); i++) {
		GLFrameBufferObject *out = pool.acquire(w, h);
		mStages[i]->render(input, w, h, out, pool, ctx);
		if (prev) pool.release(prev);
		prev = out;
		input = out->textureId();
	}
	return prev;
}
