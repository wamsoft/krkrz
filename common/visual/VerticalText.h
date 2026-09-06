// VerticalText — 縦組みテキストをエンジンのビットマップへ描画する
//
// 統一フォントエンジン glyphware でシェイピングし (正立ランは TTB、横倒しラン
// は LTR + 90 度回転)、JLReq 水準の組版 (約物の詰め・禁則・追い込み/追い出し)
// を通してから合成する。横組みの drawShapedText 系 (GlyphwareText.h) と同じ
// フォント解決・同じ合成を使うので、同じ Font 指定で見た目が揃う。
//
// 現状の対応範囲は本文のみ。ルビ・縦中横・圏点・割注・字取り、段組、
// 下線/打ち消し線は未対応。
#pragma once
#ifdef KRKRZ_USE_GLYPHWARE

#include "tjsCommHead.h"
#include "GlyphwareText.h"   // tTVPShapedTextStyle

class tTVPBaseBitmap;

// 縦組みの組版オプション
struct tTVPVerticalTextOptions
{
	// 正立/横倒しの指定。0=mixed (和文は正立・欧文は横倒し)、1=すべて正立、
	// 2=すべて横倒し
	tjs_int orientation = 0;
	// 列の進む向き。false (既定) は vertical-rl = 右から左
	bool verticalLr = false;
	// 約物の詰め (JLReq のアキ量表を適用する)。false ならベタ組み
	bool punctuation = true;
	// 和欧間のアキを入れる
	bool latinGap = true;
	// 行末に来た句読点を版面外へ出す
	bool hanging = false;
	// 行末を揃える (グルーを伸ばして行長へ合わせる)。false でも溢れない分の
	// 詰めは行う
	bool justify = true;
	// 字間 (em 単位)
	float letterSpacing = 0.f;
	// 行間追加 px (負でもよい)。行送り = フォントサイズ + lineSpacing
	tjs_int lineSpacing = 0;
};

// 描画/計測の結果
struct tTVPVerticalTextResult
{
	int width = 0;        // 使った幅 px (列方向。矩形の右端/左端からの厚み)
	int lines = 0;        // 組んだ列数 (矩形に入りきらず捨てた列は数えない)
	int count = 0;        // 実際に描画したクラスタ数
	int totalCount = 0;   // count 制限が無いときのクラスタ総数
	                      // (クラスタ = 描画単位。欧文間隔は数えない)
};

// 矩形 (x, y, width, height) へ縦組みで描画する。
//
// 列の長さは height、列の送りは (フォントサイズ + lineSpacing)。vertical-rl
// では 1 列目が矩形の右端に来て左へ進む (vertical-lr は逆)。矩形からはみ出す
// 列は描画しない。描画は矩形内へクリップされる。
//
// `count` >= 0 で先頭 count クラスタのみ描画する (クラスタ = 描画単位。
// 行分割は全文で確定してから制限を掛けるので、逐次表示でリフローしない)。
// style.angle は無視する。
//
// 失敗時 (フォントが解決できない等) は false。
bool TVPGlyphwareDrawVerticalTextArea(tTVPBaseBitmap* dest, tjs_int x, tjs_int y,
                                      tjs_int width, tjs_int height,
                                      const ttstr& text, tjs_uint32 color,
                                      const tTVPShapedTextStyle& style,
                                      tjs_int count,
                                      const tTVPVerticalTextOptions& opts,
                                      tTVPVerticalTextResult& out);

// 同じ組版を描画せずに行うだけ。戻り値の意味は上と同じ。
bool TVPGlyphwareMeasureVerticalTextArea(tjs_int width, tjs_int height,
                                         const ttstr& text,
                                         const tTVPShapedTextStyle& style,
                                         tjs_int count,
                                         const tTVPVerticalTextOptions& opts,
                                         tTVPVerticalTextResult& out);

#endif // KRKRZ_USE_GLYPHWARE
