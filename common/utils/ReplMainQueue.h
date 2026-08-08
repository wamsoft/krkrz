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
#include <functional>

namespace TVPReplMainQueue {

//! @brief worker: 式を提出してメイン実行の結果を待つ (ブロック)。
//! @return true: 実行された (out/error 有効) / false: シャットダウン中。
bool Submit(const ttstr& script, tTJSVariant& out, ttstr& error);

//! @brief worker: 任意処理をメインスレッドで実行してもらい完了を待つ (ブロック)。
//!        script Submit と違い複数件を並行提出でき、Drain が予算内で順次処理する
//!        (web REPL のハンドラ dispatch 等)。fn 内の例外は握りつぶされるので、
//!        エラー通知が必要なら fn 側で捕捉して結果に反映すること。
//! @return true: 実行された / false: シャットダウン中 (未実行)。
bool SubmitTask(const std::function<void()>& fn);

//! @brief main thread: 保留中の提出を処理する (毎フレーム呼ぶ)。
//!        script 提出は最大 1 件 + タスクは予算内で複数件。
void Drain();

//! @brief ブロック中の全 submitter を起こす (シャットダウン)。
void Shutdown();

//! @brief terminating フラグをクリアして再利用可能にする ((再)起動時)。
void Reset();

} // namespace TVPReplMainQueue

#endif
