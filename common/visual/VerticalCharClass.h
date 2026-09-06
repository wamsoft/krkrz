//---------------------------------------------------------------------------
// VerticalCharClass — JLReq の文字クラスと、クラス間のアキ量表
//
// 「日本語組版処理の要件」附属書 A の文字クラスのうち、間隔処理 (表 3) と
// 禁則処理に必要なものを実装する。ルビ・割注・添え字関連 (cl-14〜cl-18) は
// 本文だけを扱う現状のスコープには無い。
//
// 和文組版は「1 文字 = 1em の箱を並べる」問題ではなく、**文字クラス間のアキ
// (グルー) とペナルティの列を解く**問題として扱う。箱を並べる方式では約物の
// 詰め・禁則・追い込み/追い出しが原理的に表現できないため。
//
// 分類は 1 コードポイント単位。クラスタ (結合文字列) の文字クラスは先頭の
// コードポイントで決める。
//---------------------------------------------------------------------------
#ifndef __VERTICAL_CHAR_CLASS_H__
#define __VERTICAL_CHAR_CLASS_H__

#include <cstdint>

namespace TVPVertical {

//---------------------------------------------------------------------------
// 文字クラス (コメントの cl-NN は JLReq の文字クラス番号)
//---------------------------------------------------------------------------
enum class CharClass : std::uint8_t {
	OpenBracket,        // cl-01 始め括弧類          「（〔［｛〈《【
	CloseBracket,       // cl-02 終わり括弧類        」）〕］｝〉》】
	Hyphen,             // cl-03 ハイフン類          ‐ 〜 ゠ –
	Dividing,           // cl-04 区切り約物          ！ ？ ‼ ⁇
	MiddleDot,          // cl-05 中点類              ・ ： ；
	FullStop,           // cl-06 句点類              。 ．
	Comma,              // cl-07 読点類              、 ，
	Inseparable,        // cl-08 分離禁止文字        — … ‥
	Iteration,          // cl-09 繰返し記号          々 〻 ゝゞヽヾ
	Prolonged,          // cl-10 長音記号            ー
	SmallKana,          // cl-11 小書きの仮名        ぁぃぅ… ァィゥ… っゃゅょ
	PrefixAbbr,         // cl-12 前置省略記号        ￥ ＄ £ ＃ €
	PostfixAbbr,        // cl-13 後置省略記号        ° ′ ″ ℃ ％ ‰
	Ideographic,        // cl-19 漢字等
	Hiragana,           // cl-20 平仮名
	Katakana,           // cl-21 片仮名
	IdeographicSpace,   // cl-24 和字間隔 (全角空白)
	Space,              // cl-26 欧文間隔
	Western,            // cl-27 欧文用文字
	Digit,              // cl-27 のうち算用数字
	Unknown,            // 上記に該当しないもの (和字扱い)
};

//---------------------------------------------------------------------------
// 仮想ボディ内での字面の寄り
//
// 約物は全角の仮想ボディの中で字面が半分に寄っている。詰めるとは「ボディを
// 半角にする」ことなので、字面がボディのどちら側にあるかでグリフの描画
// オフセットが決まる。
//---------------------------------------------------------------------------
enum class BodyAlign : std::uint8_t {
	Full,    // 全角ボディいっぱい (通常の和字・欧文)
	Start,   // 行の進む向きの手前側に寄る (句読点・終わり括弧)
	End,     // 奥側に寄る (始め括弧)
	Center,  // 中央に寄る (中点類)
};

CharClass TVPGetCharClass(char32_t cp);

// 半角に詰められる約物か (仮想ボディが全角、字面が半角のもの)
bool TVPIsHalfWidthPunctuation(CharClass c);

BodyAlign TVPGetBodyAlign(CharClass c);

// 行頭禁則 — この文字の直前では改行できない (終わり括弧類・句読点・中点類・
// 区切り約物・小書きの仮名・長音記号・繰返し記号・後置省略記号・ハイフン類)
bool TVPIsLineStartProhibited(CharClass c);

// 行末禁則 — この文字の直後では改行できない (始め括弧類・前置省略記号)
bool TVPIsLineEndProhibited(CharClass c);

// 和欧間アキの判定に使う
bool TVPIsJapanese(CharClass c);
bool TVPIsWestern(CharClass c);

// ぶら下げ可能な約物か (句点類・読点類)
bool TVPIsHangable(CharClass c);

//---------------------------------------------------------------------------
// 伸縮するアキ (em 単位)
//
// TeX の glue と同じ 3 値。この形にした時点で、追い込み・追い出し・両端揃えは
// 「グルーの伸縮でどう行長に合わせるか」という 1 つの問題に統一される。
//---------------------------------------------------------------------------
struct GlueSpec {
	float natural = 0.f;
	float stretch = 0.f;
	float shrink = 0.f;

	bool isZero() const { return natural == 0.f && stretch == 0.f && shrink == 0.f; }
};

// JLReq 表 3。隣接する 2 文字の間に入るアキ量。
// 約物そのものの仮想ボディは全角のままで、ここで扱うのは**文字と文字の間**の
// アキ。字面が半角に寄っている約物は、ボディ幅を半角にしてから前後にアキを
// 入れる形で組む (TVPGetBodyAlign を参照)。
GlueSpec TVPGetSpacing(CharClass before, CharClass after);

// 仮想ボディ幅 (em 単位)。字面が半角に寄る約物は 0.5em、それ以外の和字は
// 1.0em。欧文はここでは決まらない (シェイパーのアドバンスを使う) ので 0。
float TVPGetBodyWidth(CharClass c);

} // namespace TVPVertical

#endif // __VERTICAL_CHAR_CLASS_H__
