//---------------------------------------------------------------------------
//!@file REPL メインスレッド実行キュー
//
// console REPL スレッドと -replfile チャネルスレッドの両方が、 TJS 式を
// メインスレッドで実行してもらうために使う共有キュー。 Submit() は drain
// されるまでブロックする。 提出はシリアライズされる (同時に 1 件だけ in-flight)。
//---------------------------------------------------------------------------
#ifndef REPL_MAIN_QUEUE_H
#define REPL_MAIN_QUEUE_H

#include "tjsNative.h"

namespace TVPReplMainQueue {

//! @brief worker: 式を提出してメイン実行の結果を待つ (ブロック)。
//! @return true: 実行された (out/error 有効) / false: シャットダウン中。
bool Submit(const ttstr& script, tTJSVariant& out, ttstr& error);

//! @brief main thread: 保留中の提出を最大 1 件実行する (毎フレーム呼ぶ)。
void Drain();

//! @brief ブロック中の全 submitter を起こす (シャットダウン)。
void Shutdown();

//! @brief terminating フラグをクリアして再利用可能にする ((再)起動時)。
void Reset();

} // namespace TVPReplMainQueue

#endif
