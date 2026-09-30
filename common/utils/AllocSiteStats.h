#pragma once
#include <cstddef>
#include <cstdint>

// 呼び出し元 (スタック) 別の生存確保集計。GlobalAllocStats の診断拡張。
//
// Krkrz collector を通る確保 (operator new / TJS_malloc) ごとにスタックを
// 数段取り、同じスタックを 1 つの「サイト」にまとめて生存件数/バイトを数える。
// free 時は Header に残したサイト番号で差し引く。Dump でサイトを DbgHelp で
// シンボル化し、アロケータ内部 (operator new / std:: / TJS_malloc 等) を
// 飛ばした最初の関数ごと、および呼び出し連鎖ごとに生存バイトの多い順で出す。
//
// 有効化: KRKRZ_ENABLE_MEMSTAT_DETAIL ビルド (Windows のみ) で、起動オプション
//   -memstatsite=yes     (既定 16 段)
//   -memstatsite=<段数>  (1..32)
// を付けたとき。スタック取得は確保ごとに走るため起動は遅くなる。
// 出力: REPL `.memsites [件数] [関数名の一部]`、および .memdump (TVPHeapDump) の末尾。
//
// 未対応環境 / 無効時は全 API が何もしない (Capture は 0 を返す)。

namespace TVPAllocSiteStats {

#if defined(KRKRZ_ENABLE_MEMSTAT_DETAIL) && defined(_WIN32)

// GlobalAllocStats::Initialize から 1 回呼ぶ。起動オプションを読んで表を用意する。
void Initialize();
bool Enabled();
// 現在のスタックのサイト番号 (0 = 記録なし)。確保経路から呼ぶので確保はしない。
uint32_t Capture();
void OnAlloc(uint32_t site, size_t size);
void OnFree(uint32_t site, size_t size);
// 生存バイトの多い順に top 件をログへ出す。filter を渡すと、呼び出し連鎖に
// その文字列を含むサイトだけを 8 段の連鎖で出す。
void Dump(int top, const char *filter = nullptr);

#else

inline void Initialize() {}
inline bool Enabled() { return false; }
inline uint32_t Capture() { return 0; }
inline void OnAlloc(uint32_t, size_t) {}
inline void OnFree(uint32_t, size_t) {}
void Dump(int top, const char *filter = nullptr);

#endif

} // namespace TVPAllocSiteStats
