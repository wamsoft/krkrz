#pragma once
#include <cstddef>

// TJS Dictionary / Array の生存インスタンス追跡。
// 大きい Dictionary に entry を push し続けるパターンのリーク調査用。
//
// 仕組み:
//   - tTJSDictionaryObject / tTJSArrayObject の ctor/dtor で
//     unordered_set にポインタを Register / Unregister
//   - dump 時に set を走査して各 instance の entry 数を取得
//   - 上位 N (= entry 数の多いもの) と entries 数別ヒストグラム +
//     fingerprint 集計を出力
//
// Dictionary の entry 数は tTJSCustomObject::Count (public) を直接読む。
// Array は要素を tTJSArrayNI::Items に持つので Object 単独からはサイズ取得
// できない。Phase 1 は instance 数のみ。entry top-N は Dictionary だけ。
//
// KRKRZ_ENABLE_MEMSTAT_DETAIL 未定義時は本ヘッダの全 API が inline 空に置換。
// ctor/dtor の hook 呼び出しは死コードとして除去される。

namespace TJS {
class tTJSDictionaryObject;
class tTJSArrayObject;
class tTJSCustomObject;
}

#ifdef KRKRZ_ENABLE_MEMSTAT_DETAIL

void TVPRegisterTJSDictionary(TJS::tTJSDictionaryObject *obj) noexcept;
void TVPUnregisterTJSDictionary(TJS::tTJSDictionaryObject *obj) noexcept;
void TVPRegisterTJSArray(TJS::tTJSArrayObject *obj) noexcept;
void TVPUnregisterTJSArray(TJS::tTJSArrayObject *obj) noexcept;

// tTJSCustomObject 全体 (Dictionary/Array/カスタムクラス含む) の総数 +
// 生存インスタンスの登録。 dump 時にクラス名別の内訳を出すために
// ポインタも覚える (どのクラスが増え続けているかを名指しするため)。
void TVPIncrementTJSCustomObjectCount(TJS::tTJSCustomObject *obj) noexcept;
void TVPDecrementTJSCustomObjectCount(TJS::tTJSCustomObject *obj) noexcept;

// 統計 dump (TVPHeapDump / System.dumpTJSObjectStats / REPL .tjsstats から呼ぶ)。
// INFO レベルで 1 行サマリ + 上位 N 件詳細を出力する。
void TVPDumpTJSObjectStats() noexcept;

#else // !KRKRZ_ENABLE_MEMSTAT_DETAIL

inline void TVPRegisterTJSDictionary(TJS::tTJSDictionaryObject *) noexcept {}
inline void TVPUnregisterTJSDictionary(TJS::tTJSDictionaryObject *) noexcept {}
inline void TVPRegisterTJSArray(TJS::tTJSArrayObject *) noexcept {}
inline void TVPUnregisterTJSArray(TJS::tTJSArrayObject *) noexcept {}
inline void TVPIncrementTJSCustomObjectCount(TJS::tTJSCustomObject *) noexcept {}
inline void TVPDecrementTJSCustomObjectCount(TJS::tTJSCustomObject *) noexcept {}
inline void TVPDumpTJSObjectStats() noexcept {}

#endif // KRKRZ_ENABLE_MEMSTAT_DETAIL
