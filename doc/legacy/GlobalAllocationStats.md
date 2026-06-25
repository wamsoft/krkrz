# 標準関数 (malloc / new) アロケーション統計取得 (設計 + 実装)

`iTVPMemoryAllocator` 経由ではない **CRT / グローバルヒープ上の
allocation** (素 `malloc` / `new` / 第三者ライブラリの内部確保等)
についても統計を取るための仕組み。

**ステータス (2026-05-15):** §1〜§5 は当初の設計検討メモ。**§6 は実装記録**で、
案 A (operator new override + TJS_malloc redirect + SDL_SetMemoryFunctions) を
ベースに **TVPPooledAllocator (TLSF) ベースの事前確保プール + 遅延 init** で
実装済 (PR #4 + 拡張)。CLI option は `-krkrzpoolsize=N` / `-sdlpoolsize=N` (MB)。
プラグイン DLL 内 / C ライブラリ内部の素 malloc は依然対象外で、必要が
顕在化したら案 B (mimalloc 全置換) を検討する。

T5/T6 (グローバル集約 TJS API / per-block 追跡) は別ライン継続検討。

## 1. 背景

`MemoryBudgetNegotiation` の T1〜T4 では `iTVPMemoryAllocator` を持つ
アロケータ (FileAllocator / BitmapBitsAlloc。将来的に GraphicsLoader /
Sound / TJS2 は §5.4 で議論中) について alloc/free 数・バイト数・tag 別集計が
取れる。ただし以下のパスは捕捉できない:

- **C ライブラリ内部の `malloc`/`free`** (libpng、libjpeg、miniaudio、
  SDL3、ICU、libwebp、cef、その他 vcpkg 経由のすべての依存)
- **kirikiri 本体内の `new` 直接呼び出しのうち、専用アロケータを
  通さないもの** (`tjsConfig.h` の `TJS_malloc` 経由でない大半の
  TJS2 オブジェクト、`new char[]` 系の補助バッファ全般、各サブ
  システムの local heap)
- **プラグイン DLL 内のすべての alloc** (kirikiri は `/MT` static CRT
  なので、各 plugin は独自 CRT を持ち、本体 exe からは到達不可能)

これらをどこまで掴むかは**目的次第**:

- **観測 (Process 全体の RSS と何が食ってるかの内訳)**
  → OS スナップショット系 (案 C) で十分
- **per-call 計測 (誰がどの size で alloc しているか)**
  → operator new override (案 A) で C++ 側のみ
- **完全捕捉 (libpng 等の C ライブラリも含めて全部)**
  → CRT malloc 置換 (案 B) しかない

## 2. アプローチ比較

### 案 A: グローバル `operator new` / `operator delete` オーバライド

C++ 標準の機能で、本体 exe 内の `::operator new(size_t)` /
`::operator delete(void*)` (および array / sized 版) を再定義。
内部の counter (atomic) で `Stats` を集計、専用 API で取得。

**捕捉対象**:
- 本体 exe 内の `new` / `new[]` 経由全 allocation
- `std::vector` 等の STL コンテナが `std::allocator` を経由した分

**捕捉外**:
- C 関数の `malloc`/`free` (libpng、miniaudio、第三者ライブラリ全般)
- プラグイン DLL 内の `new` (各 DLL の operator new は独立)
- VirtualAlloc/HeapAlloc 直呼び (Win32 直叩きの BitmapBitsAlloc 等)

**実装コスト**: 小〜中
- 1 cpp ファイルに 8 種の operator (`new` / `new[]` / `new(nothrow)` /
  `new[](nothrow)` + `delete` / `delete[]` + sized delete 2 種) を定義
- atomic counter 6 本 (T1 の Stats と同形)
- C++14 の sized `operator delete(void*, size_t)` を使うと free 時に
  size が引数で渡るので header 前置不要 (sized delete 未対応経路の
  fallback 用に header 前置を併設するのが安全)
- static 初期化順序: counter は zero-init されるので問題なし

**ABI / 副作用**:
- 既存の `_CrtDumpMemoryLeaks` (Debug CRT) と競合する可能性
  (Debug 時のみ off にする等の切り分け)
- Address Sanitizer 等のメモリツールと競合する可能性
- 自前 `operator new` を持つクラス (TJS2 で将来やる場合) は別経路
  になる — それは設計上 OK (per-allocator として別カウント)

**プラグイン**: `/MT` build なので各 plugin DLL は独立 CRT。本体の
override は本体内のみ。プラグイン側で計測したいなら plugin 側で
同じ override を入れる (= 本案件のスコープ外、別案件)。

### 案 B: CRT `malloc` 置換 (mimalloc / jemalloc)

`malloc`/`free` 自体を mimalloc 等で差し替える。`mi_override_malloc`
or static link で全 `malloc` 呼出を mimalloc に redirect。

**捕捉対象**:
- 本体 + (適切に link すれば) プラグイン側の **C/C++ 全 heap allocation**
- libpng / miniaudio / SDL3 / ICU 等の依存ライブラリも全部

**捕捉外**:
- `VirtualAlloc` / `HeapAlloc` 直叩き
- mmap / 巨大 alloc (mimalloc は内部で OS pages を直接使うが、それも
  mimalloc 内 stats に含まれる)

**実装コスト**: 中〜大
- mimalloc を vcpkg から取得 (既に vcpkg にある)
- CMakeLists で全 target に link、Windows は MSVCRT override が
  追加要 (`mimalloc-override.dll` の preload か static link)
- プラグイン側も同じ allocator を使うよう調整 (link order 注意)
- パフォーマンス回帰テストが必要 (mimalloc は通常良くなるが特定
  pattern で悪化することも)

**メリット**:
- mimalloc / jemalloc は **stats API が充実**
  (`mi_stats_print`, `mi_heap_visit_blocks`, `mallctl("stats.allocated")`)
- ピーク・断片化率・ページ使用率・サイズクラス別分布まで取れる
- 副次効果として **割り当て速度向上**が期待できる
- マルチスレッド時のロック競合が減る場合が多い

**副作用**:
- 動作が CRT malloc と微妙に違う (alignment, free 後の reuse pattern)
- デバッグツール (Visual Studio の heap profiler 等) の出力が変わる

### 案 C: OS スナップショット取得

既存の Win32 `HeapWalk` を流用 + Linux/macOS 相当 API を追加して、
**プロセス全体のヒープ使用量** を on-demand で取得。

**捕捉対象**:
- ProcessHeap 上の全 block サイズ + 個数 (= ほぼ全 malloc/new 結果)
- OS が認識する RSS / VirtualSize

**捕捉外**:
- 別 heap (HeapAllocAllocator が `HeapCreate` で作った heap)
- per-call の個別記録 (snapshot のみ)
- 累積 alloc/free 回数 (現在状態のみ)

**実装コスト**: 小
- Win32: 既存 `TVPHeapDump` + `GetProcessMemoryInfo`
- Linux: `/proc/self/status` (VmRSS, VmPeak, VmData) のパース
- macOS: `task_info(mach_task_self(), TASK_BASIC_INFO_64, ...)`
- Android: `/proc/self/status` (Linux と同じ) + `mallinfo2()`

**位置付け**: `doc/legacy/MemoryBudgetNegotiation.md` §12.1 の `process_rss` /
`process_vsize` (L6) に相当。MemoryBudgetNegotiation 本体の T6 完了後、
これを補強する形で OS 直取得を追加するのが自然。

**欠点**:
- **スナップショット限定** (連続 polling は重い)
- HeapWalk 中はヒープロックを取るので大量 block で停滞
- per-allocator stats と数字がずれる場合がある
  (HeapWalk は free 直後の空きブロックも見える)

### 案 D: ハイブリッド (推奨)

上記を組み合わせ:

| Layer | 手段 | 何が見えるか |
|---|---|---|
| Per-allocator | iTVPMemoryAllocator T1〜T4 | FileCache / BitmapBits / Sound / GraphicsLoader / TJS2 (将来) ごとの alloc/free 累積・peak・サイズ分布・tag |
| Global C++ | operator new override (案 A) | 本体内の `new` 経由全 allocation 統合カウンタ |
| Global C/C++ | OS snapshot (案 C) | RSS / VirtualSize / ProcessHeap 全 block |
| 完全 (option) | mimalloc 化 (案 B) | ライブラリ含む全 C/C++ allocation 詳細 |

優先度:
1. **案 C (OS snapshot)** が一番 ROI が高い (実装小、外形把握できる)
2. **案 A (operator new override)** で本体 C++ 側を per-call 計測
3. **案 B (mimalloc)** は性能改善目的と兼ねて検討、観測目的単独だと
   コスト過剰

## 3. 実装ステップ案 (当初計画)

> **注 (2026-05-15):** 本節は当初の段階分割案。実装は §6 にあり、結果として
> 以下のように対応した:
> - G1 (OS snapshot API): 別案件で `common/utils/ProcessMemory.{h,cpp}` として
>   既に実装済 (`TVPGetProcessMemoryInfo`)。GlobalAllocStats とは独立
> - G2 (TJS から取得): 未実装。`System.dumpHeap()` のログ出力には含まれる
> - **G3 (operator new override): §6 で実装済 (案 A ベース + TLSF プール拡張)**
> - G4 (`System.getMemoryStat().globalNew` 公開): 未実装。`Snapshot` 構造体は
>   公開済だが TJS バインドはなし
> - G5 (mimalloc 化): 未着手 (必要が出たら検討)

| Phase | 内容 | 完了条件 |
|---|---|---|
| G1 | OS snapshot API (案 C) を `TVPGetProcessMemorySnapshot()` として実装 | Win32/Linux/macOS/Android で RSS/VSize 取れる |
| G2 | snapshot を `System.getMemoryStat()` の `process_rss` / `process_vsize` に統合 | TJS から取れる |
| G3 | グローバル operator new/delete override (案 A) を導入 | 本体 C++ alloc が atomic 6 本でカウントされる |
| G4 | G3 の Stats を `System.getMemoryStat().globalNew` として公開 | 内訳が見える |
| G5 (任意) | mimalloc / jemalloc 化検討 | 性能ベンチ + stats 出力サンプル取得後に判断 |

G1〜G2 は独立に意味あり、G3〜G4 と並行可能。G5 は別案件として切り出す。

## 4. 未決事項

> **注 (2026-05-15):** §6 実装で結論が出た項目に **[結論]** を付記、
> 残課題はそのまま残す。

- **G3 の operator new override は debug build / release build で切り
  替えるか**: Debug build は `_CrtDumpMemoryLeaks` を残したい場合があり、
  競合するので `MASTER` ビルドのみ有効化等の運用が要相談
  - **[結論]** 全ビルドで有効化。`_CrtDumpMemoryLeaks` との競合は実用上
    出ていない (CRT debug heap は本機構の header より下層で動く)
- **per-call backtrace の有無**: doc/legacy/MemoryBudgetNegotiation.md §11.3
  の L5 (per-block 追跡) と同じ機構で `_ReturnAddress()` 1 段の callsite
  記録は可能。ただし C++ heap は alloc 頻度が高いのでオーバヘッド要計測
  - **[未着手]** 必要が出たら検討。現状 fallback_count / `live_bytes` で
    overflow 検知は十分カバーできている
- **プラグイン DLL の取り扱い**: `/MT` build なので独立 CRT。プラグイン
  側で同じ override を入れる仕組みを `tp_stub` 経由で提供するか、
  プラグイン分は OS snapshot (案 C) で間接観測するに留めるか
  - **[未着手]** 現状はプラグイン DLL 内 alloc は捕捉外。`Process memory`
    行 (RSS) との差分で間接観測。tp_stub 経由 override は将来課題
- **TJS2 ヒープとの関係**: doc/legacy/MemoryBudgetNegotiation.md §5.4.2 の
  TJS2 専用アロケータ (将来候補) が入った場合、operator new override
  との二重カウントを避ける設計が必要 (TJS2 のクラスが `operator new`
  を override すれば override を呼ばないので自然と分かれる)

## 5. 関連ドキュメント

- `doc/legacy/MemoryBudgetNegotiation.md` — 本案件の前提となる per-allocator
  容量ネゴ + テレメトリ設計
- 過去メモ: `feedback_spsc_ring_capacity.md` — 容量設計整合不備の事例
- `win32/base/SystemImpl.cpp` の `TVPHeapDump` — 既存の HeapWalk 実装

## 6. 実装 (2026-05-15)

PR #4 で**案 A (operator new override) ベースの最小実装**を入れたあと、
2026-05-15 に **TVPPooledAllocator (TLSF) ベースの事前確保プール** + **遅延 init
構造** に拡張した。実装ファイル:

- `common/utils/GlobalAllocStats.{h,cpp}` — 本体
- `common/tjs2/tjsConfig.h` — `TJS_malloc/free/realloc` を `TVPKrkrzMalloc/...` に redirect
- `sdl3/environ/main.cpp` SDL_AppInit — `SDL_SetMemoryFunctions` 設置 +
  `app->InitPath()` 後に `Initialize()`
- `win32/environ/Application.cpp` wWinMain — `Application = new ...` 直後に `Initialize()`
- `common/utils/REPL.cpp` — `.mem` / `.mempeakclear` への組み込み
- `{generic,win32}/base/SystemImpl.cpp` `TVPHeapDump` — `Dump()` 呼び出し追加

### 6.1 動作モード

二段階:
- **pre-init** (Initialize 未呼出): 全 alloc が素 `std::malloc`/`free` 直行。
  オーバーヘッドゼロ、stats なし、pool なし。CRT default と同等の振る舞い。
  この区間で確保されたポインタは header を持たない。
- **post-init** (Initialize 呼出後): 全 alloc に 16 byte header + magic を付与。
  pool が紐付いていれば pool 経由 (TLSF + 内部 fallback)、なければ素 `malloc` +
  header。free は magic で経路判別。pre-init で確保されたポインタは magic
  mismatch → 素 free に流れて整合する。

### 6.2 Magic 配置

```
[16-byte Header (size + magic)] [user payload]
                                 ^returned to user
```

magic は 2 種類:
- `kMagicRaw  = 0x4B524B5A4D454D31` — pool 未紐付け / pool 構築失敗で素 malloc
- `kMagicPool = 0x4B524B5A4D454D32` — TVPPooledAllocator::allocate 経由

### 6.3 Initialize() タイミング

`TVPGetCommandLine` が使えるようになった直後 (= Application 組み上げ +
InitPath 完了後)。これより前の SDL_Init / kirikiri config 読み出し時の
小規模 alloc は pre-init モードで素 malloc を通る。実 game runtime の
bulk alloc はすべて post-init で pool 経由になる。

### 6.4 CLI / .cf 設定

`-bitmapheapsize` / `-filepoolsize` と同じ慣習:
- `-krkrzpoolsize=N` (MB、`none`/`off`/`0` で pool 無効化、未指定で 256)
- `-sdlpoolsize=N` (MB、同上、未指定で 64)

`-sdlpoolsize` は `__GENERIC__` build (SDL3 / LIB) のみ有効。WINVER build は
SDL3 を使わないので `Initialize()` 内で `#ifdef __GENERIC__` 制御により SDL
pool 自体を構築しない (= 64MB の無駄確保を回避)。`SDL` collector は WINVER
ビルドでも存在するが pool=null + 統計値ゼロで動く。

### 6.5 Overflow 検知

各 alloc 前後で `TVPPooledAllocator::fallbackAllocCount()` を比較。差分が
出たら pool 容量を超えて system malloc に逃げた = overflow。Collector の
`warned_overflow_` atomic flag で 1 度だけ WARNING ログを発火:

```
GlobalAllocStats[Krkrz]: pool capacity exceeded (268435456 bytes);
subsequent allocs fall back to system malloc
```

`Snapshot::fallback_count` / `fallback_bytes` で累積回数 / 現在 outstanding
バイト数を取得可能。`:mem` / `Dump()` にも反映。

### 6.6 観測 API

```cpp
namespace TVPGlobalAllocStats {
    struct Snapshot {
        uint64_t alloc_count, alloc_bytes, free_count, free_bytes;
        uint64_t live_bytes, peak_bytes;       // user-level (header 抜き)
        uint64_t pool_capacity, pool_used, pool_peak;  // pool 紐付け時のみ
        uint64_t fallback_count, fallback_bytes;
    };
    Snapshot GetKrkrzStats();
    Snapshot GetSdlStats();
    void ResetKrkrzPeak();
    void ResetSdlPeak();
    std::string Summarize();    // REPL `.mem` 用 1 行
    void Dump();                // TVPHeapDump 用 INFO ログ出力
    void Initialize();          // pool 構築 + tracking on
}
```

REPL: `.mem` で `Summarize()` / `.memdump` (= `System.dumpHeap()`) で `Dump()`
が間接的に呼ばれる。`.mempeakclear` で File/Bitmap allocator + GlobalAlloc
両方の peak がリセットされる。

### 6.7 設計上のトレードオフ (採用判断)

| 軸 | 採用案 | 却下案 | 理由 |
|---|---|---|---|
| pool init タイミング | InitPath 後 (遅延) | SDL_AppInit 冒頭 (早期) | TVPGetCommandLine で .cf 対応のため |
| pre-init alloc | 素 malloc (untracked) | 全 header 付き raw 経路 | CRT default 同等のオーバーヘッドゼロ |
| pool 構造 | TVPPooledAllocator (TLSF) | 専用 bump arena | 既存 pool と統一 |
| overflow 通知 | atomic 1 度 WARNING | 毎回 / spdlog rate limit | 簡単・スパムしない |


