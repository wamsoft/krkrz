# メモリ状態監視・観測機能

kirikiri Z 内部のメモリアロケータ (FileAllocator / BitmapAllocator /
SoundAllocator) および OS から見たプロセス全体のメモリ使用量を、
ログ出力 / 画面オーバレイで観測するための機能群のまとめ。

**リーク調査向けの細粒度 breakdown (Krkrz tag scope / TJSObjectStats / bin
別 fingerprint) は `doc/legacy/MemoryLeakDiagnostic.md` を参照** — 本ドキュメントの
基礎観測の上に乗る形で、用途別 / Dict 種別までの特定方法をまとめている。

設計の根拠と背景は `doc/legacy/MemoryBudgetNegotiation.md` (per-allocator
容量ネゴシエーション + テレメトリ設計) を参照。本ドキュメントはユーザ
視点の使い方と、出力結果の読み方を扱う。

本体 exe 内の `::operator new` / `TJS_malloc` / SDL3 内部 alloc を一元捕捉
する仕組み (GlobalAllocStats) も実装済 (2026-05-15)。設計と詳細は
`doc/legacy/GlobalAllocationStats.md` §6 (実装) を参照。CLI option
(`-krkrzpoolsize` / `-sdlpoolsize`) は本ドキュメント §2.3 の表に記載。
プラグイン DLL 内 / C ライブラリ内部の素 malloc は依然対象外。

---

## 1. 概要

| 機能 | エントリ | 出力先 | OS / Build |
|---|---|---|---|
| ヒープダンプ (詳細) | `System.dumpHeap()` / REPL `.memdump` / `TVPHeapDump()` | ログ | 全 build |
| 1 行サマリ | REPL `.mem` | REPL コンソール | 全 build |
| 周期ダンプ | CLI `-memstatinterval=N` | ログ | 全 build |
| 終了時サマリ | CLI `-memstatonexit=1` | ログ | 全 build |
| リーク推定 | atexit (T4) 自動 | ログ (WARNING) | 全 build |
| 画面オーバレイ | `System.setMemoryOverlay()` / REPL `.memoverlay` / CLI `-memoverlay=1` | 画面右上 | **SDL3 build のみ** |
| Peak リセット | `System.resetMemoryPeak()` / REPL `.mempeakclear` | (overlay/dump 表示) | 全 build |
| キャッシュ件数 | (heap dump / overlay 内に内蔵) | ログ + 画面 | 全 build |
| File キャッシュ一覧 | `Storages.getFileCacheList()` / `Storages.dumpFileCacheList()` / REPL `.filecache` | 戻り値 (Array) または ログ | 全 build |
| Image キャッシュ一覧 | `Storages.getImageCacheList()` / `Storages.dumpImageCacheList()` / REPL `.imagecache` | 戻り値 (Array) または ログ | 全 build |

すべての出力は最終的に同じ TVPHeapDump / TVPDumpAllocatorStats 経由で
集計されているので、見え方は統一されている。

---

## 2. 使い方

### 2.1 TJS API

`System` クラスに 3 メソッド (両 build で利用可能):

```tjs
// メモリ状態の詳細をログにダンプ
// (per-allocator stats + per-tag 内訳 + サイズビン + プロセス全体メモリ)
System.dumpHeap();

// オーバレイ表示制御 (SDL3 build のみ実描画。WINVER は flag だけ)
System.setMemoryOverlay(true);   // 表示開始
System.setMemoryOverlay(false);  // 表示停止
System.setMemoryOverlay();       // toggle (戻り値は新しい状態 0/1)

// File/Bitmap allocator の peak_used を current_used に揃え直す。
// オーバレイの "(peak X.XX)" 表示を「ここから先の最大」にリセットしたい
// ときに使う (例: 序盤シーンの ramp-up が落ち着いた後)。
System.resetMemoryPeak();
```

`Storages` クラスにキャッシュ一覧取得 / ダンプの API:

```tjs
// File キャッシュ (file://) と decode 後 Image キャッシュをそれぞれ
// Array<Dictionary> で取得。要素のキーは下記。
var filelist  = Storages.getFileCacheList();
// %[ path:..., size:..., lastaccess:..., usecount:..., pinned:0/1 ]

var imagelist = Storages.getImageCacheList();
// %[ path:..., keyidx:..., mode:..., dw:..., dh:...,
//    width:..., height:..., bytes:..., pinned:0/1 ]

// それぞれ WARNING ログに人間可読フォーマットで全件ダンプ。
// MASTER ビルドでも出力される (調査用途)。
Storages.dumpFileCacheList();
Storages.dumpImageCacheList();
```

pin / clear 系 (詳細は `doc/legacy/ImagePreloadAndCache.md §18-20`):

```tjs
// pin: pin set 登録 + 既存 entry の pinned 化 + 自動 load 開始 (両層)。
// path 正規化は norm + placed (autopath 解決後) の両方を登録するので
// autopath 経由ファイル (例: addAutoPath('bgm/') 配下の bgm01.ogg) でも
// 正しく pin が効く。
Storages.pinCache('ui/bg.png');    // 画像系: file + decode 両層で pin
Storages.pinCache('bgm/title.ogg'); // 音声系: file 層のみで pin

Storages.unpinCache(path);
Storages.isCachePinned(path);

// 解放系
Storages.clearCache(path);          // path 単位 (両層、pinned 無視)
Storages.clearTransientCaches();    // pinned 残して両層全消し
Storages.clearAllCaches();          // pinned 含めて全消し
```

書き込み連動 evict: `TVPCreateStream(path, TJS_BS_WRITE/APPEND/UPDATE)` で
ファイルを書き込みオープンすると、対象 path の cache が両層から自動で
駆逐される。`Bitmap.save` / `Layer.saveLayerImage` / 独自書き込み処理
すべてで動作 (詳細は §20.3)。

`System.setMemoryOverlay()` は SDL3 build でのみ画面に描画される。
WINVER build や OGL DrawDevice (TJS スクリプトで `Window.drawDevice` を
切り替えた場合) では flag は立つが描画は行われない。

オーバレイ + heap dump にはキャッシュエントリ件数 (file 層 / decode 層) が
2 行分常時表示される。pinned 数は内訳。詳細な path 一覧は
`dumpFileCacheList` / `dumpImageCacheList` (または REPL の対応コマンド) で出す。

### 2.2 REPL コマンド

`-repl` 付きで起動した場合:

```
krkrz> .mem
File: used=12.34MB peak=45.67MB alloc_n=4567 free_n=4501
Bitmap: used=? peak=? alloc_n=234 free_n=200
Process: rss=234.56MB peak=512.00MB vsize=1.23GB

krkrz> .memdump
(memory stats dumped to log)

krkrz> .memoverlay on
memoverlay = on

krkrz> .memoverlay off
memoverlay = off

krkrz> .memoverlay
memoverlay = on   # 引数なしで toggle

krkrz> .mempeakclear
(File/Bitmap allocator peak_used reset)

krkrz> .filecache
(file cache list dumped to log)

krkrz> .imagecache
(image cache list dumped to log)
```

`.help` で利用可能コマンド一覧を確認できる。

### 2.3 コマンドラインフラグ

```bash
# 5 秒ごとに TVPHeapDump をログへ出力する常駐スレッドを起動
krkrz64.exe data/ -memstatinterval=5

# 終了時に 1 回 TVPHeapDump を実行
krkrz64.exe data/ -memstatonexit=1

# 終了時に File + Image キャッシュ一覧をログへダンプ
# (個々のエントリのパス/サイズ/use 数まで出る。リーク調査向け)
krkrz64.exe data/ -cachelistonexit=all

# 起動時から画面オーバレイを ON にする (SDL3 build のみ実描画)
krkrz64.exe data/ -memoverlay=1

# 全部併用 (常時 ON 観測モード)
krkrz64.exe data/ -memstatinterval=10 -memstatonexit=1 -memoverlay=1

# (周期ダンプの結果を console に出すなら -loglevel=info も指定)
krkrz64.exe data/ -memstatinterval=5 -loglevel=info
```

| フラグ | 意味 | デフォルト |
|---|---|---|
| `-memstatinterval=N` | N 秒ごとに `TVPHeapDump` (0 で OFF) | OFF |
| `-memstatonexit=1`   | 終了時に 1 回 `TVPHeapDump` | OFF |
| `-cachelistonexit=<m>` | 終了時にキャッシュ一覧をダンプ。`1`/`all` で File+Image、`file`/`image` で片方のみ、`0`/`none` で OFF | OFF |
| `-memoverlay=1`      | 起動時から画面オーバレイを ON | OFF |
| `-filepoolsize=N`    | FileAllocator のプール (TLSF) サイズ (MB)。`none` で従来 raw malloc | 256 |
| `-bitmappoolsize=N`  | BitmapAllocator のプール (TLSF) サイズ (MB)。`none` で従来 raw malloc | 1024 |
| `-krkrzpoolsize=N`   | GlobalAllocStats Krkrz プール (TLSF) サイズ (MB)。`none` で stats のみ (pool 無効) | 256 |
| `-sdlpoolsize=N`     | GlobalAllocStats SDL プール (TLSF) サイズ (MB)。`none` で stats のみ (pool 無効) | 64 |
| `-gclim=N`           | ImageCache (decode 層) の上限 (MB)。`auto` で physmem ベース自動算出 | auto |

`-filepoolsize` / `-bitmappoolsize` は起動時に 1 個の大きい malloc バッファを
確保し、そこから TLSF (Two-Level Segregated Fit) で切り出す独自 allocator を
有効化する。目的は **ファイルキャッシュ・ビットマップの実メモリ利用域を
TJS2 ヒープなどと分離して断片化を防ぐ**こと。プール枯渇時は system malloc
への fallback で動作継続。`-filepoolsize=none` (または `0` / `off`) で旧
`BasicFileAllocator` (raw malloc + 統計ヘッダ) に戻る。

設計詳細は `doc/legacy/MemoryBudgetNegotiation.md §16` 参照。

`-krkrzpoolsize` / `-sdlpoolsize` は **本体 exe 内の `::operator new` / `TJS_malloc` /
SDL3 内部 alloc** を pool 経由 (TLSF + 内部 fallback) で受けるためのもの。
StorageCache / BitmapAllocator とは独立した「OS から見た常駐ブロック」になり、
環境間でのメモリ確保パターン比較がしやすくなる。pool 容量を超えた alloc は
system malloc に fallback しつつ WARNING を 1 度発火 (`-krkrzpoolsize=128` 等で
ぎりぎりの境界を試せる)。`none` で pool を無効化すると **stats のみ収集** モード
(= 旧来挙動)。本機構自体が遅延 init で、`Initialize()` が呼ばれるまでの初期
alloc は素 malloc 直行 (オーバーヘッドゼロ)。詳細は `doc/legacy/GlobalAllocationStats.md`
参照。

`-gclim` は decode 後画像キャッシュ (`TVPGraphicCache`) の最大バイト数。
`auto` 既定では **BitmapAllocator pool capacity** (= `-bitmappoolsize` 値、
default 1024 MB) を採用する。`-bitmappoolsize=none` で BasicAllocator
(raw malloc) に落とした場合のみ、レガシーの `physmem / 10` + 512MB cap に
フォールバック。**`TVPAfterSystemInit()` で自動有効化される**ので、TJS から
`System.graphicCacheLimit = N` を呼ばなくても起動直後から有効。実際の値は
INFO ログに出る:

```
[INFO] ImageCache enabled: limit=1024MB (physmem=65441MB, source=BitmapPool)
```

`source=` は `BitmapPool` / `auto` (BasicAllocator フォールバック時の physmem
自動算出) / `-gclim` (CLI 明示指定) / `-gclim (capped by BitmapPool)` のいずれか。

(2026-05-11 以前は default `false` で、明示的に有効化しないと cache push が
空回りする罠があった。修正済。詳細は `doc/legacy/ImagePreloadAndCache.md §19.1`)

dump スレッドは最低優先度 (ttpIdle) で動くので通常負荷への影響は小さい。

---

## 3. ダンプ出力の読み方

`System.dumpHeap()` または `.memdump` を実行すると、ログに次のような
複数行が出力される (例):

```
[INFO] MemoryAllocator [FileAllocator] cap=? used=12.34MB peak=45.67MB total_alloc=234.56MB total_freed=222.22MB alloc_n=4567 free_n=4501
[INFO]   size_hist: <128=0 <1K=12 <16K=234 <256K=1234 <4M=2345 <64M=678 <1G=4 >=1G=0
[INFO]   tag[FileCache] alloc=4567 free=4501 used=12.34MB total_alloc=234.56MB total_freed=222.22MB
[INFO] MemoryAllocator [BitmapAllocator] cap=? used=89.10MB peak=120.50MB total_alloc=789.00MB total_freed=699.90MB alloc_n=234 free_n=200
[INFO]   size_hist: <128=0 <1K=2 <16K=12 <256K=120 <4M=80 <64M=20 <1G=0 >=1G=0
[INFO]   tag[BitmapBits] alloc=234 free=200 used=89.10MB total_alloc=789.00MB total_freed=699.90MB
[INFO] MemoryAllocator [SoundAllocator] cap=? used=4.30MB peak=5.20MB total_alloc=12.40MB total_freed=8.10MB alloc_n=42 free_n=30
[INFO]   size_hist: <128=0 <1K=0 <16K=4 <256K=30 <4M=8 <64M=0 <1G=0 >=1G=0
[INFO]   tag[Sound] alloc=42 free=30 used=4.30MB total_alloc=12.40MB total_freed=8.10MB
[INFO] GlobalAlloc[Krkrz] live=383.01KB peak=407.51KB total_alloc=1.29MB total_freed=939.44KB alloc_n=15184 free_n=10187
[INFO] GlobalAlloc[Krkrz] pool used=487.34KB peak=555.88KB cap=256.00MB fallback_n=0 fallback_live=0B
[INFO] GlobalAlloc[Krkrz]   size_hist: <128=8234 <1K=4523 <16K=1234 <256K=98 <4M=12 <64M=2 <1G=0 >=1G=0
[INFO] GlobalAlloc[Krkrz]   tag[Unknown]        alloc=234   free=200  used=2.10MB total_alloc=... total_freed=...
[INFO] GlobalAlloc[Krkrz]   tag[GraphicsLoader] alloc=1234  free=1200 used=8.40MB total_alloc=... total_freed=...
[INFO] GlobalAlloc[Krkrz]   tag[TJS2]           alloc=12500 free=11800 used=98.20MB total_alloc=... total_freed=...
[INFO] GlobalAlloc[SDL]   live=30.76KB peak=31.08KB total_alloc=44.83KB total_freed=14.07KB alloc_n=197 free_n=108
[INFO] GlobalAlloc[SDL]   pool used=32.75KB peak=33.02KB cap=64.00MB fallback_n=0 fallback_live=0B
[INFO] Process memory: rss=234.56MB peak_rss=512.00MB vsize=1.23GB
(WINVER のみ)
[INFO] The process has 5 heaps.
[INFO] #1 type: LFH [default]
[INFO]   Allocated: 1234, size: 56789012, overhead: 123456
[INFO]   Uncommitted: 0, size: 0, overhead: 0
[INFO]   Unused: 12, size: 345678, overhead: 1234
... (各 heap の HeapWalk 結果)
```

### 3.1 ヘッダ行 (`MemoryAllocator [Name] ...`)

| フィールド | 意味 |
|---|---|
| `cap`        | アロケータが申告する最大容量。`?` は未対応 (= 上限なし、または不明) |
| `used`       | 現在使用中のバイト数。Sized mode のみ正確。Unsized は `?` |
| `peak`       | 起動以降のピーク使用量 |
| `total_alloc`| 累積 alloc バイト数 (alloc 履歴の合計) |
| `total_freed`| 累積 free バイト数。Unsized mode は `0B` で「未対応」を意味する |
| `alloc_n`    | alloc 呼出回数 |
| `free_n`     | free 呼出回数 |

**Sized mode** = アロケータが free 時に size を回収できるタイプ。
- `BasicFileAllocator` は alloc 時にヘッダ (size + tag) を前置して、
  free(void*) のときに引き出す方式
- `Bitmap*Allocator` 群は上位 `tTVPBitmapBitsAlloc::Free` が
  `record->size` を持っているので、`iTVPMemoryAllocator::free(void*, size_t)`
  でサイズを下流に渡す方式 (アロケータ側に追加ヘッダなし)

**Unsized mode** = どちらの方法も取らないタイプ。`used` / `peak` /
`total_freed` が `?` のまま (= 未対応)。本ツリーには現状ないが、外部
プラグインが独自 allocator を差した場合に該当しうる。

Unsized mode は `total_alloc` (累積) と `alloc_n - free_n` (生存ブロック数)
を見る。

`peak` は起動以降の最大値を保持するが、`System.resetMemoryPeak()`
または REPL `.mempeakclear` で `peak = used` に戻せる。
序盤シーンの ramp-up が落ち着いた後に「ここから先の最大」を測りたい
ときに使う。

### 3.2 サイズヒストグラム行 (`size_hist:`)

8 段ビンで alloc 1 回ごとのサイズ分布を集計したもの。free 後も値は
減らない (累積 alloc 履歴)。

| ビン | 意味 |
|---|---|
| `<128`   | 128 byte 未満 |
| `<1K`    | 1 KB 未満 |
| `<16K`   | 16 KB 未満 |
| `<256K`  | 256 KB 未満 |
| `<4M`    | 4 MB 未満 |
| `<64M`   | 64 MB 未満 |
| `<1G`    | 1 GB 未満 |
| `>=1G`   | 1 GB 以上 |

**読み方**:
- `<256K` 以下が大量にある = 細かい alloc が頻発。FileAllocator なら
  小さいファイルを大量にキャッシュしている、BitmapAllocator なら
  小さい Layer が多い兆候
- `<4M` 以上が膨らむ = 大きな画像のキャッシュ、巨大 Layer 等
- ビンが偏ると断片化や上限設計の見直しが必要

### 3.3 tag 別行 (`tag[Name] ...`)

`TVPAllocTag` enum で分類された用途別の集計。per-allocator (FileAllocator
/ BitmapAllocator / SoundAllocator) では原則 1 tag 固定。GlobalAlloc[Krkrz]
は thread-local tag scope (§3.5) によって `TJS2` / `GraphicsLoader` /
`User` / `Unknown` 等に振り分けられる。

| フィールド | 意味 |
|---|---|
| `alloc`       | この tag で alloc した回数 |
| `free`        | この tag で free した回数 (Sized mode のみ正確) |
| `used`        | この tag が現在保持しているバイト数 (Sized mode のみ) |
| `total_alloc` | この tag の累積 alloc バイト数 |
| `total_freed` | この tag の累積 free バイト数 (Sized mode のみ) |

**読み方**:
- `alloc - free` が増えていく一方なら **リーク傾向**
- `used` が想定値より多ければ「キャッシュ駆逐が効いていない」
  「TJS 側で参照を保持しっぱなし」等の疑い

T4 のリーク推定 (atexit) はこの値を見て自動的に WARNING を出している。
通常ログレベルでは黙っているが、リーク疑いがあれば終了時に:

```
[WARN] MemoryAllocator leak [FileAllocator] tag=FileCache alloc=4567 free=4500 current=128.00KB total_alloc=... total_freed=...
```

の形で記録される。

### 3.5 GlobalAlloc[Krkrz] tag scope (用途別 breakdown)

GlobalAlloc[Krkrz] は単一カウンタではなく、確保時の **thread-local tag
stack の top** を使って tag 別に集計される。alloc 時の tag は Header に
保存され、free 時に同じ TagSlot から減算される。

主要 entry point は C++ 側で `TVPAllocTagScope` RAII を仕込み済:

| Entry point | tag | 場所 |
|---|---|---|
| TJS 実行 (top-level / global FuncCall) | `TJS2` | `common/tjs2/tjsScriptBlock.cpp` `ExecuteTopLevel` |
| 画像 decode 経路 | `GraphicsLoader` | `common/visual/GraphicsLoaderIntf.cpp` `TVPLoadGraphic` |
| その他 | `Unknown` | 起動初期 / engine main / 未計装領域 |

スクリプトからは TJS API で手動 push/pop:

```tjs
System.beginAllocTag("User");        // "User" tag を push
loadChapter(3);                       // この間の確保は User tag
System.endAllocTag();                 // 元の tag (=TJS2) に戻る
```

tag 名は TVPAllocTag enum 名 (`Unknown` / `FileCache` / `BitmapBits` /
`GraphicsLoader` / `Texture` / `Sound` / `Movie` / `TJS2` / `User`)。
一致しない名前は `User` 扱い。スタック深さは 16、overflow は警告 1 回。

**判別:**

| 増え続けるもの | 疑い |
|---|---|
| `tag[TJS2]` の `used` | TJS 側で参照を保持しっぱなし (循環参照、Dictionary に置きっぱなし、配列に push し続け 等) |
| `tag[GraphicsLoader]` の `used` | 画像 metadata cache / decoder 作業バッファの解放漏れ |
| `tag[Unknown]` の `used` | scope 未計装の領域。要追加計装ポイント |
| `size_hist <128 / <1K` の `alloc_n` 爆発 | 文字列 / 小オブジェクト大量生成 |
| `size_hist <256K / <4M` の `alloc_n` 爆発 | metadata / 中サイズワーク大量 |

### 3.6 GlobalAlloc 行 (`GlobalAlloc[Krkrz|SDL] ...`)

本体 exe 内の `::operator new` / `TJS_malloc` / SDL3 内部 alloc を集計した
GlobalAllocStats の出力。Krkrz / SDL の 2 collector で各 1〜2 行。

**1 行目 (counters)**:

| フィールド | 意味 |
|---|---|
| `live`        | 現在生存しているバイト数 (alloc - free) |
| `peak`        | live のピーク |
| `total_alloc` | 累積 alloc バイト数 |
| `total_freed` | 累積 free バイト数 |
| `alloc_n`     | alloc 呼出回数 |
| `free_n`      | free 呼出回数 |

**2 行目 (pool 紐付け時のみ)**:

| フィールド | 意味 |
|---|---|
| `pool used` | pool 内 (TLSF block + 16B GlobalAllocHeader 込み) の使用バイト |
| `pool peak` | pool 内ピーク |
| `cap`       | pool 容量 (`-krkrzpoolsize` / `-sdlpoolsize` での指定値) |
| `fallback_n`    | pool 容量超過で system malloc に逃げた alloc 回数 (累積) |
| `fallback_live` | fallback 経由で現在 outstanding なバイト |

**読み方**:
- `live` < `cap` で `fallback_n=0` なら pool 内に収まっている (健康)
- `fallback_n>0` なら pool 容量を超えた alloc が出た。最初の超過時に
  WARNING も発火 (`GlobalAllocStats[X]: pool capacity exceeded ...`)
- `pool used` は user `live` より少し大きい (TLSF block header + Header の
  16B/alloc オーバヘッド分)
- `cap=0` だと pool 行は出ない (= `-...poolsize=none` 指定時の stats のみモード)

**捕捉外** (= `Process memory` 行で見る):
- libpng / libjpeg / miniaudio / ICU の素 `malloc`
- プラグイン DLL 内部 (独立 CRT)
- ANGLE (`libEGL.dll` / `libGLESv2.dll`) 内部
- HeapAlloc/VirtualAlloc 直叩き (BitmapPool / FilePool の backing 等)

WINVER ビルドでは SDL pool は構築されない (`SDL pool=disabled`)。SDL 行は
出るが `pool` 行は出ない。

詳細は `doc/legacy/GlobalAllocationStats.md`。

### 3.5 プロセス全体メモリ行 (`Process memory:`)

OS から見たプロセス全体の使用量 (per-allocator では捕捉できない
libpng / miniaudio / SDL3 / プラグイン DLL / TJS の素 `new` 等を含む)。

| フィールド | 意味 | OS |
|---|---|---|
| `rss`      | Resident Set Size (物理常駐) | Win32 WorkingSetSize / Linux VmRSS / macOS resident_size |
| `peak_rss` | RSS のピーク | Win32 PeakWorkingSetSize / Linux VmHWM / macOS resident_size_max |
| `vsize`    | Virtual size (commit + reserved) | Win32 PrivateUsage / Linux VmSize / macOS virtual_size |

**読み方**:
- per-allocator の `used` 合計 + テクスチャ + その他 ≒ `rss` に近づく
  はずで、大きく乖離するなら捕捉できていない alloc 経路がある
  (`doc/legacy/GlobalAllocationStats.md` の議論)
- `peak_rss` がメモリ予算に近づいていたら危険水域

### 3.6 (WINVER のみ) HeapWalk 出力

ProcessHeap を `HeapWalk` で全走査した結果。各 heap ごとに:

| フィールド | 意味 |
|---|---|
| `Allocated`   | 使用中ブロック数 / 合計サイズ / オーバヘッド |
| `Uncommitted` | コミット済みだが未使用な領域 |
| `Unused`      | freelist 上の解放済みブロック |

per-allocator stats とは別系統で OS heap 全体を見ている。FileAllocator
が独自 heap を使っていない場合、`tag[FileCache] used` と heap の
`Allocated` 一部が重なる。

---

## 4. オーバレイ表示の読み方 (SDL3 build)

`System.setMemoryOverlay(true)` または REPL `.memoverlay on` で
画面右上に半透明パネル (320 x 176 px、`KRKRZ_DRAW_STATS=ON` ビルドでは
320 x 260 px) が表示される。SDL_SetRenderScale で 1.5 倍拡大して描画される。

```
+------------------------------------------+
| FPS:        59.9                         |
| File:    12.34 MB (peak  45.67)          |  <- 赤
| Bitmap:  89.10 MB (peak 120.50)          |  <- 緑
| RSS:     234.56 MB                       |  <- 青
| Alloc/s  File:   42  Bitmap:    8        |  <- 黄
| FileCache: count=42 pinned=4             |
| ImageCache: count=12 pinned=2            |
+------------------------------------------+
|  ___                          _____      |
| /   \____                ____/     \     |  <- 折れ線 3 系列
|/         \______________/           \__  |     共通スケール
+------------------------------------------+
```

### 4.1 ヘッダ部 (テキスト 7 行)

| 行 | 色 | 内容 |
|---|---|---|
| FPS    | 白              | 表示更新レート (500ms 平均) |
| File   | 赤 (255,96,96)  | FileAllocator の **`current_used`** / `peak_used` |
| Bitmap | 緑 (96,255,96)  | BitmapAllocator の **`current_used`** / `peak_used` |
| RSS    | 青 (96,160,255) | プロセス RSS |
| Alloc/s| 黄 (255,220,96) | 直近 1 サンプル分の alloc 回数 → 1 秒換算 (alloc rate) |
| FileCache  | 白          | StorageCache (file 層) のエントリ数 / pin 数 |
| ImageCache | 白          | TVPGraphicCache (decode 層) のエントリ数 / pin 数 |

`KRKRZ_DRAW_STATS=ON` ビルドではこの下に Draw / Wkr-Main-Spin /
TexUp-TexRen MB/s / Show / Frame / Layer / LayerEx の合計 7 行が追加される
(詳細は `doc/DrawStats.md`)。

`peak` は `System.resetMemoryPeak()` または REPL `.mempeakclear` で
`current_used` に揃え直せる。シーン切替の前後で「ここから先の最大」を
測りたいときに使う。

### 4.2 グラフ部 (折れ線 3 系列)

- 横軸: 時間 (左が古い、右が最新)
- 縦軸: バイト数 (3 系列共通スケール、最大値はバッファ内最大値で自動)
- サンプル: **4 Hz** (250 ms 間隔)、**256 件**保持 = 約 64 秒の履歴
- 系列はヘッダと同色

### 4.3 グラフの読み方

- **File 線が階段状に上がる**: ロード処理でキャッシュが増加
- **File 線が一気に下がる**: `TVPClearOldStorageCache` が走った
  (容量逼迫 / pressure callback / シーン遷移等)
- **Bitmap 線が階段状に上下する**: Layer/Bitmap が増減している。
  グラフは `current_used` (Sized mode 化済) で File 線と同じ縦軸の意味
- **RSS が File / Bitmap の合計より大きい**: TJS / ライブラリ /
  テクスチャ等が消費している分。乖離が大きいほど per-allocator で
  捕捉できていない量が多い
- **Alloc/s が常時高い**: 大量の細かい alloc が走っている。負荷源
  の特定には `.memdump` でサイズビン (`size_hist`) を確認

### 4.4 制限事項

- **OGL DrawDevice (`tTVPOGLDrawDevice`) 切替時も表示される** (`common/visual/opengl/MemoryOverlayGL.{h,cpp}` で
  自前 8x8 bitmap font + shader/VBO の GLES 直接描画版を実装、`OGLDrawDevice` /
  `SDLOGLDrawDevice` のどちらの Show 末尾からも呼び出し)
- **WINVER build (`BasicDrawDevice` = D3D9) では描画されない**。flag は
  `setMemoryOverlay` で立つが、その経路には memoverlay の描画フックが入っていない
- パネルサイズより小さい window では (320+8 px 未満) 表示を抑止

### 4.5 オーバヘッド

- サンプラ thread: 4 Hz で getStats (atomic load 数本) + getProcessMemoryInfo
  (Win32 API 1 回 / Linux: /proc/self/status fread) のみ。負荷無視可能
- 描画: フレームごとに ~750 line + 4 text。SDL_Renderer ネイティブ
  描画なので 1ms 未満
- OFF 時はサンプリングも停止 (collect しない、リングバッファクリア)

---

## 5. 内部仕様の概要

実装は段階的に積み上がっている (詳細は `doc/legacy/MemoryBudgetNegotiation.md`):

### 5.1 アロケータ拡張 (P1〜P3 / T1〜T4)

```
iTVPMemoryAllocator (Application.h)
├── 既存: allocate(size) / free(void*)
├── サイズ付き free: free(void*, size_t) (default: free(void*) に転送)
│   └── tTVPBitmapBitsAlloc::Free → Bitmap allocator が Sized mode で
│       current_used を集計するために size を下流へ運ぶ経路
├── 容量ネゴ (P1): capacity() / used() / available() / setPressureCallback()
├── テレメトリ (T1〜T4):
│   ├── allocate(size, TVPAllocTag)
│   ├── getStats() : Stats { current_used, peak_used, total_*, *_count, alloc_size_hist[8] }
│   └── getTagStats(tag) : TagStats { current_used, alloc_count, free_count, total_* }
├── peak 操作: resetPeak() (default: no-op、Sized mode の collector で実装)
└── すべて非純粋仮想 + デフォルト実装 (= 未対応値) で ABI 互換
```

`tTVPMemoryAllocatorStatsCollector` (`common/base/MemoryAllocatorStats.h`)
が 共通の集計ロジックを提供 (Sized / Unsized 二モード)。各 allocator
派生は composition で持って `recordAlloc` / `recordFree` を呼ぶだけ。

### 5.2 観測経路

```
ユーザ操作 / CLI
    ↓
TVPHeapDump() ── per-build entry (win32/base/SystemImpl.cpp / generic/base/SystemImpl.cpp)
    ├── TVPDumpAllocatorStats("FileAllocator", ...)   <- File alloc stats
    ├── TVPDumpAllocatorStats("BitmapAllocator", ...) <- Bitmap alloc stats
    ├── TVPDumpAllocatorStats("SoundAllocator", ...)  <- Sound alloc stats
    ├── TVPGlobalAllocStats::Dump()                    <- Krkrz / SDL collector + pool 状態
    ├── TVPDumpProcessMemoryInfo()                     <- RSS / VSize
    └── (WINVER のみ) HeapWalk 出力
```

### 5.3 周期ダンプ (M4)

`common/base/MemoryStatPeriodicDump.{h,cpp}`:
- 起動時 `TVPGetCommandLine` で `-memstatinterval` / `-memstatonexit` /
  `-cachelistonexit` を解析
- N > 0 なら `tTVPThread` (ttpIdle) を起動して N 秒スリープ + `TVPHeapDump`
- `TVP_ATEXIT_PRI_CLEANUP - 2` で thread 停止 + 終了時 dump (順序: HeapDump
  → FileCacheList → ImageCacheList。HeapDump の合計値と一覧の照合を
  しやすくするため一覧を後に出す)

### 5.4 オーバレイ (M6)

```
common/base/MemoryOverlay.{h,cpp}    <- OS 共通
├── サンプラ thread (4 Hz, ttpIdle)
├── std::deque<Sample> リングバッファ (256 件、mutex 保護)
└── SetEnabled / IsEnabled / GetSnapshot

sdl3/visual/MemoryOverlayRender.{h,cpp}    <- SDL_Renderer 経路
├── 半透明パネル + テキスト + 折れ線描画
└── tTVPSDLDrawDevice::Show() の Render lambda 末尾で呼出

common/visual/opengl/MemoryOverlayGL.{h,cpp}    <- GLES 直接描画版
├── 自前 8x8 bitmap font + shader/VBO で描画 (SDL 非依存)
└── SDLOGLDrawDevice / OGLDrawDevice (WINVER+SDL3 共通) の Show 末尾から呼出
```

### 5.5 GlobalAllocStats (operator new + TJS_malloc + SDL alloc 一元捕捉)

```
common/utils/GlobalAllocStats.{h,cpp}
├── pre-init: 全 alloc が素 std::malloc/free 直行 (オーバーヘッドゼロ)
├── post-init: 16B header (size + magic) + 経路振り分け
│   ├── kMagicPool: TVPPooledAllocator (TLSF) 経由 → pool->free
│   ├── kMagicRaw : 素 std::malloc + header → std::free(h)
│   └── magic mismatch: pre-init / 別アロケータ由来 → std::free(p)
├── operator new/delete (4+4 forms = 8 over-rides) → g_krkrz collector
├── extern "C" TVPKrkrzMalloc/Calloc/Realloc/Free → tjsConfig.h で TJS_malloc に redirect
└── SdlMalloc/Calloc/Realloc/Free → SDL_SetMemoryFunctions で SDL3 から呼ばれる
```

`Initialize()` は SDL build / WINVER build で異なる位置から呼ばれる:
- SDL3 (`sdl3/environ/main.cpp` SDL_AppInit): `app->InitPath()` 完了直後
- WINVER (`win32/environ/Application.cpp` wWinMain): `Application = new ...` 直後

`Initialize()` 内で `TVPGetCommandLine` から `-krkrzpoolsize` (default 256MB) /
`-sdlpoolsize` (default 64MB、`#ifdef __GENERIC__` で SDL build のみ) を読み、
TVPPooledAllocator を構築 → tracking flag を on。これより前の SDL_Init /
config 読込時の alloc は pre-init 経路で素 malloc を通る。

詳細・設計判断は `doc/legacy/GlobalAllocationStats.md` §6。

### 5.6 主要ファイル

| ファイル | 役割 |
|---|---|
| `common/base/MemoryAllocatorStats.h` | StatsCollector + format helpers (`TVPFormatBytes` / `TVPDumpAllocatorStats` / `TVPSummarizeAllocator` 等) |
| `common/base/FileAllocator.cpp` | BasicFileAllocator (Sized mode) + 容量ネゴ呼出 |
| `common/base/SoundAllocator.{h,cpp}` | BasicSoundAllocator (Sized mode, raw malloc + 16B header) — common/sound/ の PCM/リング/DSP 一時バッファを TVPAllocTag::Sound で集計 |
| `common/utils/AllocTagScope.{h,cpp}` | thread-local tag stack + RAII (`TVPAllocTagScope`)。GlobalAlloc[Krkrz] の tag 別 breakdown 入口 |
| `common/base/PooledAllocator.{h,cpp}` | TLSF プール (FilePool / BitmapPool / GlobalKrkrz / GlobalSdl 共通) |
| `common/base/StorageCache.cpp` | `TVPNegotiateStorageCacheBudget` |
| `common/utils/ProcessMemory.{h,cpp}` | OS 別 RSS/VSize 取得 |
| `common/utils/GlobalAllocStats.{h,cpp}` | operator new / TJS_malloc / SDL alloc 一元捕捉 + プール |
| `common/base/MemoryStatPeriodicDump.{h,cpp}` | CLI 駆動の周期 / 終了時 dump |
| `common/base/MemoryOverlay.{h,cpp}` | サンプラ thread + リングバッファ |
| `sdl3/visual/MemoryOverlayRender.{h,cpp}` | SDL_Renderer グラフ描画 |
| `common/visual/opengl/MemoryOverlayGL.{h,cpp}` | GLES 直接版グラフ描画 (SDL 非依存) |
| `win32/visual/BitmapBitsAlloc.cpp` | Bitmap 系 4 実装 (Unsized mode) |
| `generic/environ/Application.cpp` | generic 版 BasicAllocator (Unsized) |
| `win32/base/SystemImpl.cpp` / `generic/base/SystemImpl.cpp` | `TVPHeapDump` 実装 + `System.dumpHeap` / `setMemoryOverlay` TJS メソッド |
| `common/utils/REPL.cpp` | `.mem` / `.memdump` / `.memoverlay` |

---

## 6. 制限と将来課題

| 項目 | 制限 | 将来案 |
|---|---|---|
| BitmapAllocator current_used | Sized mode 化済 (T2 で free(void*, size_t) 経由) | — |
| グローバル `malloc`/`new` 捕捉 | 本体 exe 内 + SDL3 内部のみ実装済 (GlobalAllocStats) | `doc/legacy/GlobalAllocationStats.md`。残: C ライブラリ内部 / プラグイン DLL は捕捉外、必要なら案 B (mimalloc 全置換) |
| WINVER (`BasicDrawDevice` = D3D11) 上のメモリ/パッドオーバレイ | 非対応 (描画フックなし) | 必要が出てから検討 |
| プラグイン DLL 内の alloc | 各 DLL が独立 CRT のため捕捉外 (GlobalAllocStats でも到達不可能) | tp_stub 経由でプラグイン側にも override を撒く案を `doc/legacy/GlobalAllocationStats.md` で議論中 |
| TJS2 ヒープ | per-allocator 化していない | `MemoryBudgetNegotiation.md §5.4.2` (将来候補、半端実装は罠) |
| GraphicsLoader (libpng etc.) | per-allocator 化していない | `MemoryBudgetNegotiation.md §5.4.1` |
| Sound PCM バッファ | per-allocator 化していない | `MemoryBudgetNegotiation.md §5.4.3` |
| 周期ダンプの formatter 切替 | 全部固定フォーマット | JSON 出力等は需要次第 |
| L5 per-block 追跡 | 未実装 | doc §11.4 (T6 で扱う想定、デバッグビルド限定の重い機能) |
| L6 グローバル集約 + TJS Snapshot | 未実装 | doc §12 (T5、Snapshot 構造体 + `System.getMemoryStat()`) |

T5 / T6 は別ラインで継続検討 (本ドキュメントの対象外)。

---

## 7. トラブルシュート

### 7.1 周期ダンプがログに出ない

- `-loglevel=info` を指定しているか確認 (デフォルトは `MASTER` build で
  WARNING 以上、それ以外は INFO)
- リダイレクト先がコンソール / ファイルそれぞれで違う可能性 (
  `doc/Logging.md` 参照)

### 7.2 `Bitmap used=?` が常に表示される

- これは仕様。BitmapAllocator は Unsized mode (free 時 size 不明)
  なので `current_used` は出ない。代わりに `total_allocated` (累積) を
  見ること

### 7.3 オーバレイが出ない

- SDL3 build か確認 (`-DKRKRZ_VARIANT=SDL` でビルドされた krkrz64.exe)。
  WINVER (`BasicDrawDevice` = D3D9) では描画されない
- `Window.drawDevice` が `BasicDrawDevice` 等 SDL3/OGL 系以外に切り替わっていないか確認
  (SDLDrawDevice / SDLOGLDrawDevice / OGLDrawDevice はいずれも対応済)
- ウィンドウ幅が 320+8 px 未満だと自動抑止される
- `System.setMemoryOverlay(true)` の戻り値が `1` であることを確認

### 7.4 リーク警告が誤検知に見える

- アプリ終了直前 (CLEANUP-1 priority) で取っているので、その時点でまだ
  完全に解放されていないキャッシュ等は警告される
- 通常運用では「`tag[FileCache] alloc != free`」で残りキャッシュ件数が
  alloc-free 差として出るが、これは正常 (`TVPFinalizeFileAllocator` が
  CLEANUP で free するため警告時点では生存)
- 真のリーク疑いは「ピーク時より大幅に増えている」「ゼロにならない」
  パターン
