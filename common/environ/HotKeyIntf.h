//---------------------------------------------------------------------------
//!@file 最上位ホットキーフック (System.registerHotKey)
//
// プラットフォームのイベントポンプが、 通常の入力 dispatch (フォーカス中の
// ウィンドウ / テキスト入力 / Elements モーダル) より前に TVPProcessHotKey を
// 呼ぶことで、 フォーカス位置に依存しないアプリケーショングローバルな
// キー処理を提供する。 何を割り当てるかはスクリプト側が決める:
//
//   System.registerHotKey(VK_RETURN, ssAlt, function(key, shift) { ... });
//   System.unregisterHotKey(VK_RETURN, ssAlt);
//
// 登録されたキーは押下 (down) でコールバックが呼ばれ、 down / repeat /
// 対応する up とも通常の dispatch へは渡らない (up は key のみで照合し、
// 修飾キーが先に離されても片割れが入力レイヤへ漏れないようにする)。
// コールバックが false を返すとそのイベントは消費せず通常の dispatch へ
// 流す (メニュー表示中だけ素通しする等、 状況依存のホットキー用)。
//
// Elements 側の ElementsDialog.registerHotKey (ホストホットキー) とは別物:
// あちらは「Elements パネルに食わせず通常のゲーム入力へ流すキー」の宣言で
// モーダル表示中は無効。 こちらは全 dispatch より上流のコールバック起動。
//---------------------------------------------------------------------------
#ifndef HOTKEYINTF_H
#define HOTKEYINTF_H

#include "tjsVariant.h"

// mods / shift は TVP_SS_SHIFT / TVP_SS_ALT / TVP_SS_CTRL (tvpinputdefs.h) の
// 組み合わせ。 照合はこの 3 ビットのみで行う (NumLock 等の状態は無視)。
void TVPRegisterHotKey(tjs_uint key, tjs_uint32 mods, const tTJSVariantClosure& clo);
bool TVPUnregisterHotKey(tjs_uint key, tjs_uint32 mods);

// イベントポンプから呼ぶ。 true = 登録キーに一致 (イベントを消費すること)。
// コールバック呼び出しは down かつ非リピートのときのみ。
bool TVPProcessHotKey(tjs_uint key, tjs_uint32 shift, bool down, bool repeat);

#endif // HOTKEYINTF_H
