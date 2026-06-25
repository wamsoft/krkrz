#pragma once

// メモリ stats 周期ダンプ + 終了時ダンプ。
// CLI フラグ駆動で TVPHeapDump 等を呼び出す。
//   -memstatinterval=N (秒): N > 0 で N 秒ごとに TVPHeapDump 実行。0 / 未指定で OFF
//   -memstatonexit=1       : 終了時に TVPHeapDump を 1 回呼び出す。0 / 未指定で OFF
//   -cachelistonexit=<m>   : 終了時に Storages cache 一覧をダンプ。
//                            1 / all → file + image、file → file のみ、
//                            image → image のみ、0 / none / 未指定で OFF
// (T4 leak dump は別途 atexit に登録済みで本機能とは独立)。

void TVPInitializeMemoryStatPeriodicDump();
