# メモリ観測・リーク調査・キャッシュ運用ガイド

kirikiri Z のメモリアロケータ群 (FileAllocator / BitmapAllocator /
SoundAllocator / GlobalAllocStats) と画像キャッシュ (StorageCache /
ImageCache) について、**使い方** と **出力結果の読み方** をまとめた
ユーザ視点のガイド。

内部実装と設計の根拠は `doc/MemoryDesign.md` を参照。歴史的な検討記録
(案比較・段階導入計画・撤退ライン等) は `doc/legacy/` に退避してある。

---

## 1. 全体構造

メモリは 4 つの allocator + 外部に分かれる:

```
Process RSS
  ├── FileAllocator      (StorageCache 用、file_malloc 経由)
  ├── BitmapAllocator    (tTVPBitmapBitsAlloc、Bitmap Bits)
  ├── SoundAllocator     (sound_malloc 経由、TLSF プール)
  │     ├── common/sound/        (PCM / リング / DSP / クロスフェード一時)
  │     ├── miniaudio 内部        (ma_allocation_callbacks で hook)
  │     ├── libogg / libvorbis    (_ogg_* マクロを sound_malloc に redirect)
  │     └── libopus decoder state (opus_multistream_decoder_init を sound_malloc 上で)
  ├── GlobalAlloc[Krkrz] (operator new + TJS_malloc 経由、TLSF プール)
  │     ├── tag[TJS2]           (TJS 実行中の確保 — event dispatch / executor)
  │     ├── tag[GraphicsLoader] (画像 decoder の作業バッファ / metadata)
  │     ├── tag[User]           (script から System.beginAllocTag で振った範囲)
  │     └── tag[Unknown]        (engine 基盤、起動時 / 未計装領域)
  ├── GlobalAlloc[SDL]   (SDL_SetMemoryFunctions、KRKRZ_SDLMEMORY_STAT=ON 時のみ)
  └── 外部                (ANGLE / libpng / libjpeg / プラグイン DLL 内部 malloc、
                           GL texture、HeapAlloc 直叩き)
```

各 allocator が独立した stats を持ち、`System.dumpHeap()` / REPL `.memdump`
で一括 dump できる。さらに `KRKRZ_ENABLE_MEMSTAT_DETAIL=ON` でビルドすると
GlobalAlloc[Krkrz] に **TJS 言語側オブジェクトの追跡** が乗る:

- `TJSObjectStats: CustomObject total instances=N`
- `TJSObjectStats: Dictionary instances=N total_entries=S` (+ top-N + bin 別 fingerprint)
- `TJSObjectStats: Array instances=N`

---

## 2. TJS API

`System` クラス (全 build):

```tjs
// メモリ状態の詳細をログにダンプ
//   per-allocator stats + per-tag 内訳 + サイズビン + プロセス全体メモリ
System.dumpHeap();

// オーバレイ表示 (SDL3 build のみ実描画、WINVER は flag のみ)
System.setMemoryOverlay(true);   // 表示開始
System.setMemoryOverlay(false);  // 表示停止
System.setMemoryOverlay();       // toggle (戻り値は新しい状態 0/1)

// File/Bitmap/Sound allocator の peak_used を current_used に揃え直す
//   オーバレイの "(peak X.XX)" を「ここから先の最大」にリセットしたいとき
System.resetMemoryPeak();

// システムアロケータ情報を Dictionary で取得
//   コンソール機等のプラットフォーム固有実装を含む
//   (一般 OS では OS API 経由の値が、Switch 等ではプラットフォーム allocator
//    が返す値が入る)
var info = System.getSystemAllocatorInfo();
// %[
//   totalFreeSize:        ...,  // 空き領域合計 (GetTotalFreeSize 相当)
//   allocatableSize:      ...,  // 確保可能最大連続サイズ
//   processRss:           ...,  // RSS
//   processPeakRss:       ...,  // ピーク RSS
//   processVsize:         ...,  // virtual size
//   systemTotalPhysical:  ...,  // システム全体の物理メモリ
//   systemAvailPhysical:  ...,  // システム空き物理メモリ
//   usedSize:             ...,  // (プラットフォーム実装のみ)
//   peakUsedSize:         ...,  // (プラットフォーム実装のみ)
//   totalSize:            ...,  // (プラットフォーム実装のみ)
// ]
// 取得不可な項目はキー自体が dict に存在しない (`"xxx" in info` で判定可能)。
```

`Storages` クラス (キャッシュ一覧):

```tjs
// File キャッシュエントリ一覧 (戻り値は Array of Dict)
var list = Storages.getFileCacheList();
// 要素: %[ path: ..., size: ..., usecount: ..., last_use_tick: ... ]

// Image キャッシュエントリ一覧
var list = Storages.getImageCacheList();
// 要素: %[ path: ..., w: ..., h: ..., size: ..., pinned: ..., usecount: ... ]

// ログにダンプ (path/size/use 数まで)
Storages.dumpFileCacheList();
Storages.dumpImageCacheList();
```

---

## 3. REPL コマンド

| コマンド | 動作 |
|---|---|
| `.mem` | 1 行サマリ (各 allocator の live/peak/alloc_n + GlobalAlloc + システム空き) |
| `.memdump` | `TVPHeapDump()` 相当の詳細ダンプ |
| `.mempeakclear` | `System.resetMemoryPeak()` 相当 |
| `.memoverlay [on\|off]` | overlay toggle / 明示制御 |
| `.sysalloc` | システムアロケータの空き / 確保可能サイズ / プロセス RSS を 1 行表示 |
| `.filecache` | File キャッシュ一覧 |
| `.imagecache` | Image キャッシュ一覧 |

---

## 4. CLI オプション

CLI と `config.cf` の両方で指定可能。

| オプション | 動作 |
|---|---|
| `-memstatinterval=N` | N 秒ごとに `TVPHeapDump()` をログへ出力する常駐スレッドを起動 |
| `-memstatonexit=1` | 終了時に `TVPHeapDump()` を 1 回実行 |
| `-cachelistonexit=<m>` | 終了時にキャッシュ一覧をダンプ (`file`/`image`/`all`/`none`) |
| `-memoverlay=1` | 起動時から画面オーバレイ ON (SDL3 build のみ実描画) |
| `-krkrzpoolsize=N` | Krkrz pool 容量 (MB)。`none`/`off`/`0` で pool 無効化 (= stats のみ) |
| `-filepoolsize=N` | File pool 容量 (MB)。`none`/`off`/`0` で BasicFileAllocator (raw malloc) にフォールバック (既定 512MB) |
| `-bitmappoolsize=N` | Bitmap pool 容量 (MB)。`none`/`off`/`0` で raw malloc にフォールバック |
| `-soundpoolsize=N` | Sound pool 容量 (MB)。`none`/`off`/`0` で BasicSoundAllocator にフォールバック (既定 128MB) |
| `-sdlpoolsize=N` | SDL pool 容量 (MB)。`KRKRZ_SDLMEMORY_STAT=ON` ビルドのみ有効 |
| `-loglevel=info` | 周期ダンプの結果を console に出すなら必要 |

例:
```bash
# 5 秒ごとにヒープダンプ、終了時にキャッシュ一覧
krkrz64 -memstatinterval=5 -memstatonexit=1 -cachelistonexit=all -loglevel=info
```

---

## 5. CMake gate 一覧

すべて **デフォルト ON、`-DMASTER=ON` で強制 OFF**
(`KRKRZ_ENABLE_MEMSTAT_DETAIL` のみデフォルト OFF)。

| option | 効果 |
|---|---|
| `KRKRZ_ENABLE_ALLOC_STATS` | GlobalAllocStats core (operator new override / TJS_malloc redirect / Krkrz TLSF pool / 基本カウンタ) |
| `KRKRZ_ENABLE_ALLOCATOR_STATS` | File/Bitmap/Sound allocator の stats collector (sizeBin / tag 別) |
| `KRKRZ_ENABLE_MEMORY_OVERLAY` | 画面右上オーバレイ (sampler thread + SDL/GL 描画) |
| `KRKRZ_ENABLE_PERIODIC_DUMP` | `-memstatinterval` / `-memstatonexit` / `-cachelistonexit` の処理 |
| `KRKRZ_ENABLE_MEMSTAT_DETAIL` | per-tag / size_hist / TJSObjectStats (デフォルト OFF、調査時のみ) |
| `KRKRZ_SDLMEMORY_STAT` | SDL3 内部 alloc を SDL_SetMemoryFunctions で hook (デフォルト OFF) |
| `KRKRZ_DRAW_STATS` | DrawThreadPool 利用統計。詳細は `doc/DrawStats.md` (デフォルト OFF) |

OFF にしても TJS API / REPL コマンド / CLI オプション自体は残るが、
中身が空または `"(disabled)"` を返すだけ。

---

## 6. ダンプ出力の読み方

`System.dumpHeap()` (= REPL `.memdump`) の典型出力:

```
MemoryAllocator [FileAllocator] cap=200.00MB used=148.32MB peak=185.40MB
                                 total_alloc=2.30GB total_freed=2.15GB alloc_n=18432 free_n=18261
  size_hist: <128=12 <1K=423 <16K=8912 <256K=7831 <4M=1212 <64M=42 <1G=0 >=1G=0
  tag[FileCache] alloc=18432 free=18261 used=148.32MB total_alloc=2.30GB total_freed=2.15GB

MemoryAllocator [BitmapAllocator] cap=512.00MB used=312.45MB peak=410.23MB ...
MemoryAllocator [SoundAllocator]  cap=128.00MB used=12.34MB peak=18.20MB ...

GlobalAlloc[Krkrz] live=156.78MB peak=178.42MB total_alloc=12.34GB total_freed=12.18GB
                   alloc_n=8429183 free_n=8392812
GlobalAlloc[Krkrz] pool used=158.20MB peak=180.10MB cap=256.00MB fallback_n=0 fallback_live=0B

ProcessMemory: rss=812.34MB vsize=1.23GB peak_rss=945.20MB
```

### 6.1 各列の意味

- `cap` — allocator capacity (`-krkrzpoolsize` 等で指定、または `SIZE_MAX`)
- `used` — 現在の生存バイト (`current_used`)
- `peak` — `used` のピーク (`resetMemoryPeak` でクリア)
- `total_alloc` / `total_freed` — 累積 alloc/free バイト (peak の代わりに leak 判定に使う)
- `alloc_n` / `free_n` — alloc/free 回数
- `size_hist` — 確保サイズ分布 (8 ビン: `<128` / `<1K` / `<16K` / `<256K` / `<4M` / `<64M` / `<1G` / `>=1G`)
- `tag[NAME]` — タグ別内訳 (`KRKRZ_ENABLE_MEMSTAT_DETAIL=ON` のみ)
- `fallback_n` / `fallback_live` — pool 枯渇 → system malloc 経由した分

### 6.2 GlobalAlloc の意味

- `[Krkrz]` — 本体 exe の `operator new` / `TJS_malloc` 経由全部
- `[SDL]` — SDL3 内部 alloc (`KRKRZ_SDLMEMORY_STAT=ON` ビルドのみ)
- 捕捉外: libpng / libjpeg / プラグイン DLL / ANGLE 内部
  (`miniaudio` / `libogg` / `libvorbis` / `libopus` decoder state は
  SoundAllocator 配下に取り込み済み — §1 全体構造参照)

`live_bytes` が増え続け、`fallback_n > 0` になっている場合は pool 枯渇。
`-krkrzpoolsize` を増やすか、リーク調査 (§7) に進む。

---

## 7. リーク調査フロー

`KRKRZ_ENABLE_MEMSTAT_DETAIL=ON` でビルドすると `tag[*]` / `TJSObjectStats`
セクションが追加される。

```bash
cmake -B build/x64-windows -DKRKRZ_ENABLE_MEMSTAT_DETAIL=ON
cmake --build build/x64-windows --config Release
```

### 7.1 7 ステップ

1. **基準点を撮る** — `System.dumpHeap()` を「リーク前」の状態で呼ぶ
2. **症状を再現** — 該当のゲーム進行を行う
3. **再度ダンプ** — `System.dumpHeap()` を「リーク後」に呼ぶ
4. **どの allocator か特定** — `live` / `total_alloc - total_freed` の差分を見て、
   どの allocator が累積しているか
5. **どの tag か特定** — `tag[TJS2]` / `tag[GraphicsLoader]` 等で内訳を見る
6. **TJSObjectStats を読む** — Dict/Array instance 数の増分、top-N の entries、
   bin 別 fingerprint で 「同じ key 構造の Dict が大量生成されている」パターンを検出
7. **script を修正** — backlog 上限導入、シーン切替時 clear、保持 key の軽量化等

### 7.2 TJSObjectStats の読み方

```
TJSObjectStats: CustomObject total instances=58432 peak=62100
TJSObjectStats: Dictionary instances=42183 total_entries=189432
TJSObjectStats: Array      instances=21008
TJSObjectStats:   Dict[0] entries=10234 ptr=0x... sample_keys=["msg", "name", "voice", ...]
TJSObjectStats:   Dict[1] entries=8421 ptr=0x... sample_keys=["text", "wait"]
TJSObjectStats:   Dict bin entries=4-10: count=14523
TJSObjectStats:     fp[0] count=12988 keys=[textlength|speechtext|text]
TJSObjectStats:     fp[1] count=820   keys=[name|voice|wait]
```

- **CustomObject total** — 全 TJS object 数 (Dict + Array + native instance + class)
- **Dict top-N** — entries 数の多い Dict 上位。 _「大きい Dict にひたすら push されてる」リークの検出_
- **Dict bin** — entries 数別の分布 (`=0` / `1-3` / `4-10` / `11-50` / `51-200` / `>200`)
- **fp[N]** — 各 bin 内で同じ先頭 key 構造を持つ Dict の頻度。
  _「小さい Dict が大量に増えてる」リーク (例: メッセージバックログ) の検出_

### 7.3 実例: textlength|speechtext|text パターン (2026-05-16)

4 分プレイで Krkrz pool +91 MB の調査。

```
GlobalAlloc[Krkrz]   tag[TJS2] alloc=44964772 free=44912388 used=76.42MB ...

TJSObjectStats: Dictionary instances=42183 total_entries=189432
  (4 分前は instances=11395)
TJSObjectStats:   Dict bin entries=4-10: count=14523
TJSObjectStats:     fp[0] count=12988 keys=[textlength|speechtext|text]
```

→ entries=4-10 サイズの Dict が +30k 個増えていて、ほぼ全部が
`[textlength|speechtext|text]` パターン。これは KAGEX 系の
`dataList.push(%[text:..., speechtext:..., textlength:...])` 形式の
**メッセージバックログレコード**。backlog 上限 / シーン切替時 clear /
保持 key 軽量化 を script 側で対処する。

---

## 8. 画面オーバレイの読み方 (SDL3 build)

`System.setMemoryOverlay(true)` または `-memoverlay=1` で右上に表示:

```
FPS  60.0  (16.7ms)
File 148.32MB (peak 185.40MB)
Bmap 312.45MB (peak 410.23MB)
RSS  812.34MB
Alloc/s file=42 bmap=8
FileCache 1284 / pinned 312
ImageCache 421 / pinned 89
GblK 156.78MB / 256.00MB
GblS 12.34MB / 64.00MB                  ← KRKRZ_SDLMEMORY_STAT=ON のみ
SysFree: 7842.1M  Allocatable: 6273.7M  ← iTVPSystemAllocatorInfo 経由
DrwT NT=46% wait=2.1ms                  ← KRKRZ_DRAW_STATS=ON のみ
```

`SysFree` / `Allocatable` は `iTVPSystemAllocatorInfo` 経由で取得した
プラットフォームアロケータ / OS の空き情報。データ未取得時は `--` 表示。
コンソール機ではプラットフォーム allocator の正確な値、一般 OS では
システム空き物理メモリの近似値 (`Allocatable` は ×80%) が入る。

下半分には File / Bitmap / RSS の **3 系列折れ線グラフ** (64 秒分、4Hz サンプリング)。

- `peak` は `System.resetMemoryPeak()` でクリアできる
- ImageCache の `pinned` は `Storages.pinCache()` で固定された分

---

## 9. ImageCache の自動有効化

`SystemLimit` (= TJS グローバル `System.imageCacheLimit`) が **0 でない**
ときに ImageCache が有効化される。デフォルトは BitmapAllocator pool capacity
に揃えられる (例: 512 MB pool なら 512 MB)。

明示制御するには:

```tjs
// 制限を 256 MB に
System.imageCacheLimit = 256 * 1024 * 1024;

// 無効化 (= 既存挙動、毎回 decode し直し)
System.imageCacheLimit = 0;

// pin (load + 解放させない) / unpin
Storages.pinCache("path/to/image.png");
Storages.unpinCache("path/to/image.png");
```

書き込み stream open 時 (`Storages.open` で write モード) は対応する
cache エントリが自動 evict される。

---

## 10. トラブルシュート

### 10.1 `pool capacity exceeded` warning が出る

pool 容量を超えて素 malloc に fallback している。`-krkrzpoolsize` を増やすか、
リーク調査 (§7) で原因を突き止める。

### 10.2 `(disabled)` と表示される

CMake gate が OFF (例: `-DMASTER=ON` ビルドや明示 `-DKRKRZ_ENABLE_*=OFF`)。
診断ビルドが必要なら gate を ON にして再ビルド。

### 10.3 `tag[Unknown]` ばかりで内訳が見えない

`KRKRZ_ENABLE_MEMSTAT_DETAIL=OFF` のとき。リーク調査時は ON でビルドし直し。

### 10.4 メモリオーバレイが表示されない

WINVER ビルドでは表示されない (flag のみ立つ)。SDL3 ビルドで確認。
GL context が確立する前は何も出ない (起動直後の数フレーム)。

### 10.5 周期ダンプの結果が console に出ない

`-loglevel=info` も併用する。ログレベルが INFO 以下に絞られていると
`TVPLOG_INFO` が抑制されている。

---

## 11. 関連ドキュメント

- `doc/MemoryDesign.md` — 内部実装と設計の根拠
- `doc/DrawStats.md` — 描画スレッドプール統計 (`KRKRZ_DRAW_STATS`)
- `doc/REPL.md` — REPL の起動方法と一般的なコマンド
- `doc/Logging.md` — ログレベル / 出力先 / `-loglevel`
- `doc/legacy/` — 設計検討の履歴 (案比較、段階導入計画、撤退ライン等)
