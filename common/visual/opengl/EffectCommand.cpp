// ---------------------------------------------------------------------------
// ポストエフェクトのコマンド配列の解釈 (EffectCommand.h 参照)
//   gles プラグイン (GLESAdaptor) の GLEffect.cpp から、描画に依らない部分を分けたもの。
// ---------------------------------------------------------------------------
#include "tjsCommHead.h"
#include "EffectCommand.h"
#include "DebugIntf.h"
#include "tjsArray.h"

#include <cmath>
#include <cstring>

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

// ===========================================================================
// CPU 側テーブル生成(kirikiri / LayerExImage の演算を厳密移植)
// ===========================================================================

// TVPInitGammaAdjustTempData 相当(1チャンネル)
void buildGammaTable(u8 out[256], double gamma, int floorv, int ceilv)
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
void buildLightTable(u8 out[256], int brightness, int contrast)
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
int genConvolveMatrix(float radius, std::vector<float> &matrix)
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

// 現在の表に per-channel 表を合成 (out = cmd(current))
void composeLut(tTVPEffectStage &st, const u8 r[256], const u8 g[256], const u8 b[256])
{
	for (int i = 0; i < 256; i++) {
		st.lut[0][i] = r[st.lut[0][i]];
		st.lut[1][i] = g[st.lut[1][i]];
		st.lut[2][i] = b[st.lut[2][i]];
	}
}

void addOp(tTVPEffectStage &st, int op, float p0, float p1, float p2, float p3)
{
	st.ops.push_back(op);
	st.params.push_back(p0); st.params.push_back(p1);
	st.params.push_back(p2); st.params.push_back(p3);
}

} // anonymous

tTVPEffectStage::tTVPEffectStage(Kind k) : kind(k)
{
	for (int c = 0; c < 3; c++)
		for (int i = 0; i < 256; i++) lut[c][i] = (tjs_uint8)i;
}

// ===========================================================================
// コマンド配列 → 段の列
// ===========================================================================
void TVPCompileEffectCommands(const tTJSVariant &commands, std::vector<tTVPEffectStage> &stages)
{
	stages.clear();
	tTJSArrayNI *arr = GetArrayNI(commands);
	if (!arr) return;

	// 連続する LUT 系 / 混合系をまとめる先 (stages の添字。-1 で無し)
	int lut = -1, pw = -1;
	auto lutStage = [&]() -> tTVPEffectStage & {
		pw = -1;
		if (lut < 0) { stages.emplace_back(tTVPEffectStage::Lut); lut = (int)stages.size() - 1; }
		return stages[lut];
	};
	auto pwStage = [&]() -> tTVPEffectStage & {
		lut = -1;
		if (pw < 0 || (int)stages[pw].ops.size() >= TVP_EFFECT_MAX_OPS) {
			stages.emplace_back(tTVPEffectStage::Pointwise);
			pw = (int)stages.size() - 1;
		}
		return stages[pw];
	};

	for (const tTJSVariant &elm : arr->Items) {
		if (elm.Type() != tvtObject) continue;
		iTJSDispatch2 *cmd = elm.AsObjectNoAddRef();
		if (!cmd) continue;
		ttstr name = DictStr(cmd, TJS_W("cmd"));

		// ---- LUT系 ----
		if (name == TJS_W("gamma") || name == TJS_W("adjustGamma")) {
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
			composeLut(lutStage(), tr, tg, tb);

		} else if (name == TJS_W("light")) {
			u8 t[256];
			buildLightTable(t, DictInt(cmd, TJS_W("brightness"), 0), DictInt(cmd, TJS_W("contrast"), 0));
			composeLut(lutStage(), t, t, t);

		} else if (name == TJS_W("lut")) {
			tTVPEffectStage &st = lutStage();
			tTJSVariant tableVar;
			DictGet(cmd, TJS_W("table"), tableVar);
			tTJSArrayNI *tbl = GetArrayNI(tableVar);
			if (tbl) {
				u8 t[256];
				int n = (int)tbl->Items.size();
				for (int k = 0; k < 256; k++) {
					int v = (k < n) ? (int)tbl->Items[k].AsInteger() : k;
					if (v < 0) v = 0; else if (v > 255) v = 255;
					t[k] = (u8)v;
				}
				composeLut(st, t, t, t);
			}

		// ---- 混合系 ----
		} else if (name == TJS_W("grayscale")) {
			addOp(pwStage(), tveopGrayscale, 0, 0, 0, 0);

		} else if (name == TJS_W("colorize")) {
			float hue = (float)DictInt(cmd, TJS_W("hue"), 0) / 255.0f;
			float sat = (float)DictInt(cmd, TJS_W("sat"), DictInt(cmd, TJS_W("saturation"), 0)) / 255.0f;
			float blend = (float)DictReal(cmd, TJS_W("blend"), 1.0);
			if (blend < 0.0f) blend = 0.0f;
			if (blend > 1.0f) blend = 1.0f;
			addOp(pwStage(), tveopColorize, hue, sat, blend, 0);

		} else if (name == TJS_W("modulate")) {
			float h = (float)(DictInt(cmd, TJS_W("hue"), 0) / 360.0);
			float s = (float)(DictInt(cmd, TJS_W("sat"), DictInt(cmd, TJS_W("saturation"), 0)) / 100.0);
			float l = (float)(DictInt(cmd, TJS_W("lum"), DictInt(cmd, TJS_W("luminance"), 0)) / 100.0);
			addOp(pwStage(), tveopModulate, h, s, l, 0);

		} else if (name == TJS_W("noise")) {
			addOp(pwStage(), tveopNoise, (float)DictInt(cmd, TJS_W("level"), 0), 0, 0, 0);

		} else if (name == TJS_W("generateWhiteNoise") || name == TJS_W("whiteNoise")) {
			addOp(pwStage(), tveopWhiteNoise, 0, 0, 0, 0);

		} else if (name == TJS_W("overcolor")) {
			// 指定色での全体塗りつぶし合成(吉里吉里 fillOperateRect 準拠)
			tjs_uint32 col = (tjs_uint32)DictInt(cmd, TJS_W("color"), 0);
			float a = (float)((col >> 24) & 0xff) / 255.0f;
			float r = (float)((col >> 16) & 0xff) / 255.0f;
			float g = (float)((col >>  8) & 0xff) / 255.0f;
			float b = (float)((col      ) & 0xff) / 255.0f;
			int opacity = DictInt(cmd, TJS_W("opacity"), 255);
			a *= (float)opacity / 255.0f;
			int type = DictInt(cmd, TJS_W("type"), 2);  // 既定 omAlpha(2)
			addOp(pwStage(), TVP_EFFECT_OVERCOLOR_BASE + type, r, g, b, a);

		// ---- 近傍系 ----
		} else if (name == TJS_W("boxBlur")) {
			lut = -1; pw = -1;
			int radius = DictInt(cmd, TJS_W("area"), DictInt(cmd, TJS_W("radius"), 1));
			int iter   = DictInt(cmd, TJS_W("iter"), 1);
			if (radius < 0) radius = 0;
			if (radius > TVP_EFFECT_MAX_BLUR) radius = TVP_EFFECT_MAX_BLUR;
			stages.emplace_back(tTVPEffectStage::Blur);
			tTVPEffectStage &st = stages.back();
			st.radius = radius;
			st.iterations = iter < 1 ? 1 : iter;
			st.weights.assign(2 * radius + 1, 1.0f / (float)(2 * radius + 1));

		} else if (name == TJS_W("gaussianBlur")) {
			lut = -1; pw = -1;
			float r = (float)DictReal(cmd, TJS_W("radius"), 1.0);
			std::vector<float> matrix;
			int mlen = genConvolveMatrix(r, matrix);
			int radius = mlen / 2;
			if (radius > TVP_EFFECT_MAX_BLUR) {
				// 行列が大きすぎる場合は中心を切り出してクランプ(裾を捨てる)
				TVPAddLog(TJS_W("Canvas effect: gaussianBlur radius clamped"));
				int start = radius - TVP_EFFECT_MAX_BLUR;
				std::vector<float> trimmed(matrix.begin() + start, matrix.begin() + start + (2 * TVP_EFFECT_MAX_BLUR + 1));
				float s = 0; for (size_t k = 0; k < trimmed.size(); k++) s += trimmed[k];
				for (size_t k = 0; k < trimmed.size(); k++) trimmed[k] /= s;
				matrix.swap(trimmed);
				radius = TVP_EFFECT_MAX_BLUR;
			}
			stages.emplace_back(tTVPEffectStage::Blur);
			tTVPEffectStage &st = stages.back();
			st.radius = radius;
			st.iterations = 1;
			st.weights.swap(matrix);

		} else {
			TVPAddLog(TJS_W("Canvas effect: unknown command: ") + name);
		}
	}
}
