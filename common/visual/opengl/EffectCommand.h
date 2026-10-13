#ifndef EffectCommandH
#define EffectCommandH

// ---------------------------------------------------------------------------
// ポストエフェクトのコマンド配列 (Canvas.endEffect の引数) の解釈
//
//   描画手段に依らない「段」の列に直す。GL 版 (GLEffect.cpp) と GPU 版
//   (gpu/GpuCanvasIntf.cpp) が同じものを使い、それぞれの方法で描く。
//     - Lut       : gamma / light / lut。連続するものは 1 枚の表に合成する
//     - Pointwise : grayscale / colorize / modulate / noise / whiteNoise / overcolor。
//                   連続するものを 1 パスに融合する (最大 TVP_EFFECT_MAX_OPS 個)
//     - Blur      : boxBlur / gaussianBlur。重み付きの分離畳み込み (横 → 縦)
//   各コマンドの演算は kirikiri 本体と LayerExImage プラグインに合わせる (GLEffect.h 参照)。
// ---------------------------------------------------------------------------

#include "tjsNative.h"
#include <vector>

// 混合系を 1 パスに融合できる最大数
#define TVP_EFFECT_MAX_OPS 32
// 分離ブラーの最大半径 (片側)
#define TVP_EFFECT_MAX_BLUR 64
// overcolor は op = TVP_EFFECT_OVERCOLOR_BASE + 合成モード (tTVPLayerType の値)
#define TVP_EFFECT_OVERCOLOR_BASE 100

// 混合系のオペコード (シェーダー内の分岐と一致させること)
enum tTVPEffectOp {
	tveopNone       = 0,
	tveopGrayscale  = 1,
	tveopColorize   = 2,
	tveopModulate   = 3,
	tveopNoise      = 4,
	tveopWhiteNoise = 5,
};

struct tTVPEffectStage {
	enum Kind { Lut, Pointwise, Blur } kind;
	// Lut: チャンネルごとの表 (0=R 1=G 2=B)
	tjs_uint8 lut[3][256];
	// Pointwise: ops.size() 個。params は 1 個あたり 4 要素
	std::vector<int> ops;
	std::vector<float> params;
	// Blur: 重みは 2*radius+1 個
	int radius = 0;
	int iterations = 1;
	std::vector<float> weights;

	explicit tTVPEffectStage( Kind k );
};

//! コマンド配列 (Array of Dictionary) を段の列に直す。知らないコマンドはログを出して飛ばす
void TVPCompileEffectCommands( const tTJSVariant& commands, std::vector<tTVPEffectStage>& stages );

#endif
