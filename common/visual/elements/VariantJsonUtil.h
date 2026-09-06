//---------------------------------------------------------------------------
//!@file TJS Variant → JSON テキスト (UTF-8) シリアライザ
//
// ElementsDialog.showDict / showModalDict 等で、 TJS の Dictionary / Array で書いた
// レイアウトを elements_modal が要求する JSON テキストへ変換する。
//
// 対応型:
//   void        → null
//   Integer     → 整数リテラル
//   Real        → 数値リテラル (往復可能な最短表現)
//   String      → JSON 文字列 (escape + UTF-8 化)
//   Dictionary  → object (hidden member は除外)
//   Array       → array
//
// Octet / 上記以外のオブジェクト (関数・クラスインスタンス等) / 循環参照 /
// 非有限 Real (inf, nan) は eTJSError を投げる。
//---------------------------------------------------------------------------
#ifndef ELEMENTS_VARIANT_JSON_UTIL_H
#define ELEMENTS_VARIANT_JSON_UTIL_H

#include <string>

namespace TJS { class tTJSVariant; }

// v を JSON テキストにシリアライズして out へ追記する
void TVPVariantToJsonUtf8(const TJS::tTJSVariant& v, std::string& out);

#endif
