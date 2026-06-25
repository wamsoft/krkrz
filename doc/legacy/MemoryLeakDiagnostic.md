# メモリリーク調査ガイド

ゲーム進行中のメモリ累積を **どの subsystem の何の用途か** まで特定するための
観測機能と、典型的な調査フローのまとめ。

基礎的なメモリ観測 (FileAllocator / BitmapAllocator / GlobalAllocStats の見方、
MemoryOverlay、周期 dump、CLI オプション等) は `doc/legacy/MemoryInspection.md` を
参照。本ドキュメントはその上に乗る **細粒度の breakdown + leak 種別特定**
機能を扱う。

---

## 1. 全体構造

メモリ消費は 4 つの allocator + 外部に分かれる:

```
Process RSS
  ├── FileAllocator      (StorageCache 用、file_malloc 経由)
  ├── BitmapAllocator    (tTVPBitmapBitsAlloc、Bitmap Bits)
  ├── SoundAllocator     (common/sound/ の PCM/リング/DSP)
  ├── GlobalAlloc[Krkrz] (operator new + TJS_malloc 経由)
  │     ├── tag[TJS2]           (TJS 実行中の確保 — event dispatch / executor)
  │     ├── tag[GraphicsLoader] (画像 decoder の作業バッファ / metadata)
  │     ├── tag[User]           (script から System.beginAllocTag で振った範囲)
  │     ├── tag[Unknown]        (engine 基盤、起動時 / 未計装領域)
  │     └── ...
  └── 外部                (ANGLE / miniaudio / opus / libpng 等の C ライブラリ
                            内部 malloc、GL texture、HeapAlloc 直叩き)
```

各 allocator が独立した stats を持ち、`.memdump` (= `System.dumpHeap()` = REPL
`.memdump`) で一括 dump される。

GlobalAlloc[Krkrz] には、さらに **TJS 言語側オブジェクトの追跡** が乗る:

- `TJSObjectStats: CustomObject total instances=N` (全 TJS object 数)
- `TJSObjectStats: Dictionary instances=N total_entries=S` (+ top-N 詳細 + bin 別 fingerprint)
- `TJSObjectStats: Array instances=N`

---

## 2. 有効化方法

細粒度 breakdown と TJSObjectStats は CMake option で gate されている:

```bash
# 観測機能ありでビルド (調査時)
cmake -B build/x64-windows -DKRKRZ_ENABLE_MEMSTAT_DETAIL=ON
cmake --build build/x64-windows --config Release

# 観測機能なしでビルド (量産)
cmake -B build/x64-windows -DKRKRZ_ENABLE_MEMSTAT_DETAIL=OFF
cmake --build build/x64-windows --config Release
```

**デフォルトは OFF**。量産ビルドではゼロオーバーヘッド、開発・調査時のみ ON。

### OFF にすると消えるもの

- `TVPAllocTagScope` の ctor/dtor (完全 inline 空クラス、thread-local 不参照)
- operator new override 内の **per-tag accounting + size histogram 更新**
- `tTJSCustomObject` ctor/dtor の counter inc/dec
- `tTJSDictionaryObject` / `tTJSArrayObject` の register/unregister (mutex+hashset 操作)
- Dump 出力の `size_hist:` / `tag[*]` / `TJSObjectStats:` セクション全部

### OFF でも残るもの

- per-allocator stats (FileAllocator / BitmapAllocator / SoundAllocator) → **常時有効**
- GlobalAlloc[Krkrz] の基本カウンタ (alloc_count / live_bytes / pool_used / etc.)
- Pool overflow 検知
- MemoryOverlay
- `TVPHeapDump` の基本セクション

つまり OFF でも「どの allocator が膨らんでるか」までは見える。`tag[TJS2]` 内訳や
「どの Dictionary が肥大」レベルが見えなくなる。

---

## 3. 観測 API

### TJS から

```tjs
// 完全 dump (ログへ)
System.dumpHeap();

// peak リセット (overlay の "(peak X.XX)" を「ここから先」基準に)
System.resetMemoryPeak();

// 用途別 tag を手動 push/pop (KRKRZ_ENABLE_MEMSTAT_DETAIL=ON 時のみ意味あり)
System.beginAllocTag("User");
loadChapter(3);
System.dumpHeap();  // tag[User] にこの間の確保が出る
System.endAllocTag();
```

tag 名は `TVPAllocTag` enum 名 (`Unknown` / `FileCache` / `BitmapBits` /
`GraphicsLoader` / `Texture` / `Sound` / `Movie` / `TJS2` / `User`)。一致しない
名前は `User` 扱い。スタック深さ 16。

### REPL から (`-replport=N` 起動時)

| コマンド | 内容 |
|---|---|
| `.mem` | 1 行サマリ |
| `.memdump` | 完全 dump (`TVPHeapDump`) |
| `.mempeakclear` | File/Bitmap/Sound/GlobalAlloc Krkrz の peak をリセット |
| `.memoverlay [on\|off]` | SDL3 build の画面 overlay 切替 |
| `.filecache` | StorageCache のエントリ一覧 |
| `.imagecache` | TVPGraphicCache (decode 済画像) のエントリ一覧 |

### CLI から

| flag | 内容 | 既定 |
|---|---|---|
| `-memstatinterval=N` | N 秒毎の周期 dump (0 で OFF) | OFF |
| `-memstatonexit=1` | 終了時 1 回 dump | OFF |
| `-bitmappoolsize=N` | BitmapAllocator pool 容量 (MB) | 1024 |
| `-filepoolsize=N` | FileAllocator pool 容量 (MB) | 512 |
| `-krkrzpoolsize=N` | GlobalAlloc[Krkrz] pool 容量 (MB) | 256 |

---

## 4. Dump 出力の読み方

`.memdump` 実行例 (`KRKRZ_ENABLE_MEMSTAT_DETAIL=ON` 時):

```
MemoryAllocator [FileAllocator] cap=512.00MB used=...  alloc_n=... free_n=...
  size_hist: <128=0 <1K=3 <16K=35 <256K=32 <4M=36 <64M=13 ...
  tag[FileCache] alloc=... free=... used=... total_alloc=... total_freed=...

MemoryAllocator [BitmapAllocator] cap=768.00MB used=...
  size_hist: ...
  tag[BitmapBits] alloc=... used=...

MemoryAllocator [SoundAllocator] cap=? used=...
  size_hist: ...
  tag[Sound] alloc=... used=...

GlobalAlloc[Krkrz] live=... peak=... total_alloc=... alloc_n=... free_n=...
GlobalAlloc[Krkrz] pool used=... peak=... cap=512.00MB fallback_n=0
GlobalAlloc[Krkrz]   size_hist: <128=... <1K=... <16K=... ...
GlobalAlloc[Krkrz]   tag[Unknown]        alloc=... used=...
GlobalAlloc[Krkrz]   tag[GraphicsLoader] alloc=... used=...
GlobalAlloc[Krkrz]   tag[TJS2]           alloc=... used=...

TJSObjectStats: CustomObject total instances=N peak=M
TJSObjectStats: Dictionary instances=N total_entries=S
TJSObjectStats: Array      instances=N
TJSObjectStats:   Dict[0] entries=991 ptr=0x... sample_keys=["key1", "key2", ...]
TJSObjectStats:   Dict[1] entries=794 ptr=0x... sample_keys=[...]
TJSObjectStats:   ... (top-N、最大 10 件)
TJSObjectStats:   Dict bin entries==0: count=N
TJSObjectStats:     fp[0] count=N keys=[(empty)]
TJSObjectStats:   Dict bin entries=1-3: count=N
TJSObjectStats:     fp[0] count=N keys=[keyA|keyB|keyC]
TJSObjectStats:     fp[1] count=N keys=[...]
TJSObjectStats:   Dict bin entries=4-10: ...
TJSObjectStats:   Dict bin entries=11-50: ...
TJSObjectStats:   Dict bin entries=51-200: ...
TJSObjectStats:   Dict bin entries=>200: ...

Process memory: rss=... peak_rss=... vsize=...
FileCache: count=... pinned=...
ImageCache: count=... pinned=...
```

### 重要な切り分け指標

| 増え続けるもの | 疑い |
|---|---|
| `FileAllocator: used` | StorageCache の駆逐が効いていない |
| `BitmapAllocator: used` | Layer / Bitmap 解放漏れ |
| `SoundAllocator: used` | サウンドバッファ滞留 (PCM / リング) |
| Krkrz `tag[TJS2]: used` | TJS 側参照保持 / 循環参照 / Dictionary entry 累積 |
| Krkrz `tag[GraphicsLoader]: used` | 画像 metadata cache 解放漏れ |
| Krkrz `tag[Unknown]: used` | 計装外領域 (起動時 / movie / plugin 等)。要追加 scope |
| `size_hist <128 / <1K` 爆発 | 小オブジェクト / 文字列大量生成 |
| `TJSObjectStats: Dictionary instances` 増 | Dict 自体が大量生成・解放漏れ |
| `Dict bin entries=4-10 fp[*]` 増 | bin 内の最頻パターンが正体 |

---

## 5. 典型的な調査フロー

### ステップ 1: per-allocator で部分系を切り分け

```
処理前: System.dumpHeap()
プレイ進行
処理後: System.dumpHeap()
```

それぞれの `used` の増分を比較:

```
              処理前   処理後   差分
File          162 MB   162 MB    ±0       ← 駆逐 OK
Bitmap        243 MB   348 MB   +105 MB   ← 一時スパイク? leak?
Sound         1.5 MB   1.5 MB    ±0       ← OK
Krkrz pool     60 MB    74 MB    +14 MB   ← じわじわ増 (本ケース)
RSS           688 MB   934 MB   +246 MB
```

差が大きい allocator が次の調査対象。**RSS 増分 - 上記 allocator 増分**
= 外部 (GL texture / 外部ライブラリ) なので、これも視野に。

### ステップ 2: Krkrz tag breakdown で TJS / engine 切り分け

```
tag[TJS2]   used=7.83MB → 59.97MB (+52MB, 99%)  ← TJS が原因
tag[Unknown] ±0                                  ← engine は安定
tag[GraphicsLoader] ±0                           ← 画像 decode 計装内も OK
```

TJS2 が支配的なら → ステップ 3。Unknown が支配的なら計装漏れ — 該当 entry point
に `TVPAllocTagScope` を追加。

### ステップ 3: TJSObjectStats で TJS 内訳

```
CustomObject total instances=22,863 → 73,249 (+50,386)
Dictionary instances=5,461 → 36,249 (+30,788)
Array     instances=3,365 → 22,391 (+19,026)
```

Dict + Array の増分が CustomObject の増分と一致するか確認。一致するなら **Dict
or Array 自体が大量に生まれて解放されていない**。

### ステップ 4: 大きい Dict (top-N) を見る

```
Dict[0] entries=991 sample_keys=["title_bgall_jp", ...]  ← 処理前後で entries 固定
Dict[1] entries=794 sample_keys=["envPlayerSceneReaded", ...]  ← 固定
...
```

top-N が固定なら「**大きい Dict に push してる訳ではない**」。次へ。

### ステップ 5: bin 別 fingerprint で正体特定

```
Dict bin entries=4-10: count=959 → 21,998 (+21,039)   ← 急増!
  fp[0] count=12,988 keys=[textlength|speechtext|text]   ← これ
  fp[1] count=1,778 keys=[selectInfos|selectTotalCount|性格値]
  fp[2] count=1,289 keys=[num|flags|scene]

Dict bin entries=11-50: count=254 → 6,601 (+6,347)
  fp[0] count=5,190 keys=[taglist|noframe|target]
```

最も増えてる fingerprint パターンが**犯人**。上記例なら
`[textlength|speechtext|text]` 12,988 個 = メッセージバックログレコード。

### ステップ 6: script 側で push 元を grep で特定

```bash
grep -rn "textlength" *.tjs *.ks
grep -rn "speechtext" *.tjs *.ks
```

該当の **Dict を作って Array/Dict に push する場所** を割り出す。
KAGEX/KAG3 系なら `MessageLayer` の `record` メソッド系のはず。

### ステップ 7: 対策

- バックログ上限を設ける (`if (count >= MAX) shift()` で循環バッファ化)
- 保持 key を減らす (装飾情報を抜く、本文だけ保存)
- シーン切替時に古い分を clear
- 循環参照を明示的に切る (タイトル戻り以外でも周期的に)

---

## 6. 実例: textlength|speechtext|text パターン特定 (2026-05-16)

実プロジェクトでの調査ログ:

```
処理前 (01:49:29):
  Krkrz pool used=60 MB
  tag[TJS2] used=8 MB
  Dictionary instances=5,461

処理後 (01:53:25, 4 分後):
  Krkrz pool used=151 MB  (+91 MB)
  tag[TJS2] used=84 MB    (+76 MB, 99%)
  Dictionary instances=36,474  (+30,788)

  bin entries=4-10:
    fp[0] count=12,988 keys=[textlength|speechtext|text]   ← +12,988 新規
```

→ 1 messages = 1 Dict 作って Array に push し続ける構造。タイトル戻りで
`backlog.clear()` 相当が走って一気に解放。208 個/秒のペースで累積。30 分プレイで
pool 上限 512 MB に到達する計算 (実測と一致)。

`textlength` で grep してメッセージレコード生成箇所を特定 → KAGEX の
`BookmarkLayer.dataList.push` 系を修正 (上限導入 or 軽量化)。

---

## 7. ファイル位置

| ファイル | 役割 |
|---|---|
| `common/utils/AllocTagScope.{h,cpp}` | thread-local tag stack + `TVPAllocTagScope` RAII |
| `common/utils/GlobalAllocStats.{h,cpp}` | operator new + TJS_malloc + SDL alloc 統合カウンタ + tag/size_hist breakdown |
| `common/tjs2/tjsObjectStats.{h,cpp}` | TJS Dictionary/Array instance tracking + bin/fingerprint 集計 |
| `common/base/SoundAllocator.{h,cpp}` | サウンド用専用 allocator (raw malloc + 16B header) |
| `common/base/FileAllocator.cpp` | File 用専用 allocator (範本) |
| `common/base/MemoryAllocatorStats.h` | StatsCollector + dump helpers (`TVPDumpAllocatorStats` 等) |
| `common/base/MemoryStatPeriodicDump.{h,cpp}` | CLI 駆動の周期 dump |
| `common/base/MemoryOverlay.{h,cpp}` | サンプラ thread + リング |
| `generic/base/SystemImpl.cpp` / `win32/base/SystemImpl.cpp` | `TVPHeapDump` + `System.dumpHeap` / `System.beginAllocTag` 等 TJS API |
| `common/utils/REPL.cpp` | `.mem` / `.memdump` / `.mempeakclear` ハンドラ |

### 計装ポイント (entry point)

| 場所 | tag |
|---|---|
| `common/tjs2/tjsScriptBlock.cpp` `ExecuteTopLevel` | `TJS2` |
| `common/base/EventIntf.cpp` `TVPDeliverAllEvents` | `TJS2` |
| `common/base/EventIntf.cpp` `TVPDeliverWindowUpdateEvents` | `TJS2` |
| `common/base/EventIntf.cpp` `TVPDeliverContinuousEvent` | `TJS2` |
| `common/base/EventIntf.cpp` `TVPDeliverCompactEvent` | `TJS2` |
| `common/visual/GraphicsLoaderIntf.cpp` `TVPLoadGraphic` | `GraphicsLoader` |

追加で計装したい場所があれば、該当関数の冒頭に:
```cpp
#include "AllocTagScope.h"
// ...
void SomeEntry() {
    TVPAllocTagScope _alloc_tag_scope("TJS2"); // or "Movie" / "User" etc.
    // ...
}
```

---

## 8. 関連ドキュメント

- `doc/legacy/MemoryInspection.md` — 観測機能の基礎 (per-allocator stats / pool / overlay / 周期 dump)
- `doc/legacy/MemoryBudgetNegotiation.md` — 容量ネゴシエーション + tag 設計
- `doc/legacy/GlobalAllocationStats.md` — operator new override の設計
- `doc/legacy/ImagePreloadAndCache.md` — 画像 cache 解放と観測
