//---------------------------------------------------------------------------
//!@file バリアブルフォント (可変軸) 指定文字列の正規化とパース
//
// tTVPFont::Variations ("wght=700,wdth=87.5") の正規化と、描画側が使う
// (tag, value) 列への展開。glyphware 非依存の純粋な文字列処理なので、
// KRKRZ_USE_GLYPHWARE 無効ビルドでも Font.variations の保持・正規化は動く
// (描画時に無視されるだけ)。適用側は GlyphwareHost.h の
// TVPGlyphwareFaceWithVariations / TVPGlyphwareApplyVariationsToChain。
//---------------------------------------------------------------------------
#ifndef __FONT_VARIATIONS_H__
#define __FONT_VARIATIONS_H__

#include "tjsCommHead.h"
#include <utility>
#include <vector>

struct tTVPFont;

// 可変軸 1 項目: (OpenType 軸タグ big-endian pack, デザイン値)
typedef std::pair<tjs_uint32, float> tTVPFontAxisCoord;

//! @brief 軸指定文字列を正規化する。
//!   - 書式: "tag=value" をカンマ区切り (空白許容)。value は 10 進数値
//!   - タグ: 1〜4 文字の ASCII。小文字化して保持 (パック時は空白右詰め)
//!   - タグ昇順に並べ、重複は後勝ち
//!   - 量子化: wght = 1 刻み、その他の軸 = 0.5 刻み
//! 不正なトークンがあると TVPThrowExceptionMessage で例外を投げる。
//! 空文字列は空文字列のまま返す。
ttstr TVPNormalizeFontVariations(const ttstr& spec);

//! @brief 正規化済み (または任意の valid な) 軸指定文字列を (tag, value) 列へ
//!        展開する。不正トークンは黙って読み飛ばす。
void TVPParseFontVariations(const ttstr& spec, std::vector<tTVPFontAxisCoord>& out);

//! @brief tTVPFont の実効軸座標を得る: Variations を展開し、Font.Weight が
//!        指定されていて Variations に wght が無ければ wght として合流する。
//!        (Font.defaultUseVarStyle の bold/italic 自動マッピングはここでは
//!        行わない — 軸の有無が face 依存のため、適用側が chain 構築後に足す)
void TVPFontGetEffectiveVarCoords(const tTVPFont& font,
                                  std::vector<tTVPFontAxisCoord>& out);

//! @brief 同上の (weight, variations) 直接指定版。
void TVPFontGetEffectiveVarCoords(tjs_int weight, const ttstr& variations,
                                  std::vector<tTVPFontAxisCoord>& out);

//! @brief 4 文字タグのパック ("wght" → 0x77676874)。4 文字未満は空白右詰め。
tjs_uint32 TVPFontVarPackTag(const char* tag, size_t len);

//! @brief Font.defaultUseVarStyle (静的)。true のとき bold/italic を可変軸
//!        (wght=700 / slnt=-10 / ital=1) で表現できる face ではそちらを使い、
//!        合成ボールド/イタリックを無効化する。既定 false (既存の見た目を
//!        変えないため)。適用は glyphware 経路のみ
//!        (TVPGlyphwareAutoStyleCoords)。
extern bool TVPFontDefaultUseVarStyle;

#endif // __FONT_VARIATIONS_H__
