# メモリアロケータ・観測機構・画像キャッシュ 内部設計

kirikiri Z のメモリ周りの **設計の核心** をまとめた開発者向けドキュメント。
- per-allocator 容量ネゴ + テレメトリ (`iTVPMemoryAllocator`)
- TLSF プール (`TVPPooledAllocator`)
- 各 allocator (File / Bitmap / Sound)
- GlobalAllocStats (`operator new` override + 経路振り分け)
- システムアロケータ情報抽象 (`iTVPSystemAllocatorInfo`)
- AllocTagScope (thread-local tag stack)
- TJSObjectStats (Dict/Array tracking)
- ImageCache の構造 (key 正規化 / 自動解放 / pin)

ユーザ視点の使い方は `doc/MemoryGuide.md`。歴史的な検討記録 (案 A/B/C 比較、
段階導入計画 P1-P5 / I1-I7、撤退ライン等) は `doc/legacy/` に退避してある。

---

## 1. 全体アーキテクチャ

```
[script]
    │
[TJS_malloc / new]──→ TVPKrkrzMalloc ──┐
                          │             │
[SDL_*]──→ TVPGlobalAllocStats::SdlMalloc (KRKRZ_SDLMEMORY_STAT=ON)
                          │             │
                          ▼             │ header (16B: size+magic+tag)
                  ┌──────────────┐      │
                  │  TVPPooledAllocator  │ (TLSF + system malloc fallback)
                  └──────────────┘
                          │
[FileAllocator / BitmapAllocator / SoundAllocator]
        ↑ iTVPMemoryAllocator (allocate/free + Stats + TagStats + capacity)
        │ 個別 pool (TVPPooledAllocator) を内包
[script via StorageCache / Bitmap / Sound]
```

すべての allocator は `iTVPMemoryAllocator` を実装し、`Application` 経由で
差し替え可能 (組込みや LIB build 用)。GlobalAlloc は `operator new` を上書きする
ため interface ではなく直接 hook 形式。

---

## 2. iTVPMemoryAllocator

`win32/environ/Application.h` / `generic/environ/Application.h` の共通インタフェース。

```cpp
class iTVPMemoryAllocator {
public:
    virtual void *allocate(size_t size) = 0;
    virtual void *allocate(size_t size, TVPAllocTag tag) = 0;
    virtual void  free(void *p) = 0;

    // realloc 相当 (libogg の _ogg_realloc 等のフック先)。
    // デフォルトは getAllocatedSize() で旧サイズを取って allocate+memcpy+free。
    // PS5 の mspace ベース実装は sceLibcMspaceRealloc で in-place 拡張を上書き。
    virtual void *reallocate(void *old, size_t new_size, TVPAllocTag tag);
    virtual size_t getAllocatedSize(void *p) const { return 0; } // 0 = 不明

    // 容量ネゴ
    virtual size_t capacity() const = 0;  // SIZE_MAX = 上限なし
    virtual size_t used() const     = 0;  // SIZE_MAX = 不明 (Unsized mode)

    // テレメトリ
    struct Stats {
        size_t   current_used    = SIZE_MAX; // SIZE_MAX = 不明
        size_t   peak_used       = SIZE_MAX;
        uint64_t total_allocated = 0;
        uint64_t total_freed     = 0;
        uint64_t alloc_count     = 0;
        uint64_t free_count      = 0;
        std::array<uint64_t, 8> alloc_size_hist = {}; // sizeBin
    };
    struct TagStats {
        uint64_t alloc_count     = 0;
        uint64_t total_allocated = 0;
        uint64_t free_count      = 0;
        size_t   current_used    = 0;   // Sized + tag-aware free のみ
        uint64_t total_freed     = 0;
    };
    virtual Stats    getStats() const = 0;
    virtual TagStats getTagStats(TVPAllocTag tag) const = 0;
    virtual void     resetPeak() = 0;
};
```

`TVPAllocTag` は alloc 元の用途ラベル:
`Unknown / FileCache / BitmapBits / GraphicsLoader / Texture / Sound / Movie / TJS2 / User`。

### 2.1 設計目標

| # | 目標 |
|---|---|
| G1 | 容量と使用量を caller 側から問い合わせ可能 |
| G2 | 不明値は `SIZE_MAX` で素直に表現 |
| G3 | デフォルト実装 (raw malloc 経由) も同一 interface で動く |
| G4 | テレメトリは Stats / TagStats / sizeBin の 3 段 |
| G5 | tag は alloc 経路ではなく **caller-side thread-local stack** で振る |

### 2.2 sizeBin

8 段固定: `<128` / `<1K` / `<16K` / `<256K` / `<4M` / `<64M` / `<1G` / `>=1G`。
分布の特徴 (微小 alloc が暴走している vs 巨大 alloc が散発)を一目で判定。

### 2.3 統計集計の共有実装

`common/base/MemoryAllocatorStats.h` の `tTVPMemoryAllocatorStatsCollector`
が atomic ベースで `recordAlloc` / `recordFree` / `snapshot` / `resetPeak` を提供。
`KRKRZ_ENABLE_ALLOCATOR_STATS=OFF` で全 record が no-op、snapshot は
SIZE_MAX を返す (不明マーカー)。Sized mode (BasicFileAllocator のような header 前置型)
と Unsized mode (size 不明) の 2 種。

---

## 3. TVPPooledAllocator (TLSF プール)

`common/base/PooledAllocator.{h,cpp}`。

### 3.1 構造

- 起動時に `VirtualAlloc` (Win) / `mmap` (POSIX) で大きな連続領域を確保
- 内部 TLSF (Two-Level Segregated Fit) で O(1) alloc/free
- 容量を超えたら system `std::malloc` に fallback (`fallbackAllocCount` で計数)
- header に size + magic + tag (16 byte 揃え) を前置し、free 時の経路振り分けに使う

### 3.2 接続先

| caller | pool | デフォルト容量 (64-bit / 32-bit) |
|---|---|---|
| BasicFileAllocator | 専用 pool | `-filepoolsize` (256MB / 64MB) |
| tTVPBitmapBitsAlloc | 専用 pool | `-bitmappoolsize` (512MB / 128MB) |
| TVPSoundAllocator | 専用 pool | `-soundpoolsize` (16MB) |
| GlobalAlloc[Krkrz] | 専用 pool | `-krkrzpoolsize` (256MB / 64MB) |
| GlobalAlloc[SDL] | 専用 pool | `-sdlpoolsize` (64MB、KRKRZ_SDLMEMORY_STAT=ON 時のみ) |

各 pool は独立。pool に紐付けない設定 (`-krkrzpoolsize=none`) では
`std::malloc` 直行 + stats 計上のみ (header + magic を付けるので overhead 小)。

**32-bit プロセスのデフォルト縮小**: 32-bit はユーザアドレス空間が 2GB
(LAA + 64-bit OS 上で最大 4GB) しかなく、全 pool 合計 ~1GB を起動時に
先取りすると、汎用ヒープ (TJS / GL / 画像デコード / プラグイン) が細って
負荷時に OOM 即死する。そのため 32-bit では bitmap/file/krkrz の各デフォルトを
1/4 に落としてある (`sizeof(void*)` で分岐)。足りない案件は個別に
`-bitmappoolsize=N` 等で増やす。**32-bit ビルドは `/LARGEADDRESSAWARE` を
必ず付ける** (CMakeLists.txt、WIN32 かつ非 WIN64 かつ MSVC)。付けないと
2GB 制限のまま同じ枯渇を起こす (旧 32-bit ビルドは LAA 付きだった)。

### 3.3 fallback

pool 容量を超えると system `std::malloc` に fallback、`fallbackAllocCount` を
インクリメント、初回は WARNING ログを出す。fallback 経由のポインタも header magic
で識別 (`kMagicRaw`)、free 時に正しい経路を選ぶ。

### 3.3.1 破損検知と縮退 (fail-safe)

`allocate` / `free` の pool 経路の入口で `validateBlock()` によりヘッダを
安価に検査する (範囲・16-align・payload 末尾が pool 内・FREE flag と free
リンクの整合)。破損を検知したら `markCorrupted()` で CRITICAL ログを出して
`pool_corrupted_` を立て、以降は **pool 経路を使わず fallback (system malloc)
へ縮退**、pool 内ポインタの free はリークさせる (プロセスは落とさない)。
merge 相手 (prev/next 隣接ブロック) も merge 前に検査する。

この防御を入れた背景: メモリ枯渇の局面でヘッダが壊れたまま `removeFree` /
`nextPhys` に進むと 0 番地近傍への書き込み (即死 AV) になり、しかも
**その AV が「例外メッセージをログ出力するための確保」の中で起きる**ため、
エラー表示もログも残さずプロセスが即死する (デバッガ無しでは原因不明の
「フリーズして落ちる」に見える)。縮退により、枯渇時でも fallback で確保が
続き、原因が CRITICAL ログに残る。

### 3.4 起動ログ

```
TVPPooledAllocator[Krkrz] cap=256MB block_size=64KB
TVPPooledAllocator[Bitmap] cap=512MB block_size=64KB
GlobalAllocStats: tracking activated (Krkrz pool=268435456 bytes, SDL pool=disabled)
```

---

## 4. 各 allocator 実装

### 4.1 BasicFileAllocator

`common/base/FileAllocator.cpp`。`iTVPMemoryAllocator` 実装 + `TVPPooledAllocator`
内包。`tTJSBinaryStreamBuffer::create()` (`BinaryStreamBuffer.h`) と
`StorageCache` から経由される。

Sized mode (alloc 時に size を header に書き込み、free 時に回収)。

### 4.2 tTVPBitmapBitsAlloc

`win32/visual/BitmapBitsAlloc.cpp` / `generic/visual/BitmapBitsAlloc.cpp`。
Bitmap pixel buffer の専用 allocator。BitmapPool (再利用) と組み合わせて
**zero-fill 必須** (TLG decoder が pitch padding を埋めないため、
allocator 再利用で前画像残骸が出る; `memory/feedback_bitmap_zero_fill.md` 参照)。

Sized mode。`Alloc` 時に `memset(p, 0, size)` が必須。

### 4.3 TVPSoundAllocator

`common/base/SoundAllocator.{h,cpp}`。`tag = TVPAllocTag::Sound` 専用。
`Application::CreateSoundAllocator()` がデフォルトでは `TVPPooledAllocator`
(TLSF、`-soundpoolsize=N`、既定 128MB) を返し、`-soundpoolsize=none/off/0`
のときだけ `BasicSoundAllocator` (raw malloc + 16B header) にフォールバック。

C 互換 API として `sound_malloc / sound_calloc / sound_realloc / sound_free` を
`SoundAllocator.h` から extern "C" でエクスポートする (`KRKRZ_VARIANT=LIB` も含め
全 build で利用可)。`sound_realloc` は `iTVPMemoryAllocator::reallocate()` を
呼び、デフォルト実装は `getAllocatedSize() → allocate → memcpy → free` で
内容コピーを保証する。`BasicSoundAllocator` / `TVPPooledAllocator` は
header / TLSF block から旧サイズを返せるので、libogg の `_ogg_realloc` 等の
フック先としてそのまま使える。

集計対象は:
- 既存の SoundSamples / RingBuffer / PhaseVocoder / WaveLoopManager
- AudioStream.cpp の `ma_engine` 本体 + miniaudio 内部 alloc
  (`ma_engine_config.allocationCallbacks` に sound_malloc/realloc/free を注入)
- libogg / libvorbis 内部 (FetchContent ビルド + `-include sound_alloc_hook.h`
  注入で `_ogg_malloc` 系を sound_malloc に redirect; §4.4 参照)
- libopus の decoder state (`OpusCodecDecoder.cpp` が
  `opus_multistream_decoder_get_size` + `_init` で sound_malloc 確保した
  buffer に init; §4.5 参照)

PS5 では `PS5Application::CreateSoundAllocator()` を override して
Direct Memory + `sceLibcMspace("krkrz_sound", ...)` 上の専用ヒープを構築
(`ps5/src/app.cpp` `PS5SoundAllocator`)。`reallocate` も override して
`sceLibcMspaceRealloc` で in-place 拡張を試みる。

### 4.4 libogg / libvorbis (FetchContent + allocator hook)

`krkrz/external/sound-codecs/CMakeLists.txt` で `xiph/ogg` v1.3.6 と
`xiph/vorbis` v1.3.7 を FetchContent し、各ターゲットに

- `-include sound_alloc_hook.h` (MSVC は `/FI`) で強制 include
- `external/sound-codecs/hooks/sound_alloc_hook.h` が `<ogg/os_types.h>` を
  読み込んでから `_ogg_malloc / _ogg_calloc / _ogg_realloc / _ogg_free` を
  `#undef` + 再定義し、`sound_malloc / sound_calloc / sound_realloc / sound_free`
  に redirect する

を仕込む。アップストリームのソースには手を加えない。libvorbis は 200 箇所以上で
`_ogg_*` マクロを呼ぶが、すべて SoundAllocator 経由になる。

libvorbis 上流 CMakeLists 同梱の `FindOgg.cmake` (find_library で `OGG_LIBRARY`
を探す) は、`OGG_LIBRARY / OGG_INCLUDE_DIR` を CACHE に先置きすることで
find_library / find_path の検索を回避し、`Ogg::ogg` を ALIAS で先に提供して
IMPORTED target の重複生成を回避する。

### 4.5 libopus (vcpkg のまま + caller-allocates API)

libopus は vcpkg-installed バイナリをそのまま使う (allocator フックなし)。
代わりに `common/sound/OpusCodecDecoder.cpp` を opusfile 経由から **libopus
直叩き** に書き換え、`opus_multistream_decoder_get_size(streams, coupled)` で
返ったサイズを `sound_malloc` で確保した buffer に
`opus_multistream_decoder_init()` で in-place 初期化する。Ogg framing は
libogg の `ogg_sync_*` / `ogg_stream_*` API で自前実装。OpusHead / OpusTags は
RFC 7845 ベースで自前パース。シーク (granule_pos + pre_skip)・チェイン・
タグを含む opusfile 同等の機能をカバーする。

これにより:
- opusfile への依存撤去 (`vcpkg.json` から削除)
- libopus 自体は無改造のままで decoder state が SoundAllocator 配下になる
- libogg 内部 alloc (`_ogg_*` 経路) は §4.4 のフックで SoundAllocator 経由

---

## 5. GlobalAllocStats (`operator new` override + 経路振り分け)

`common/utils/GlobalAllocStats.{h,cpp}`。

### 5.1 採用方針 (案 A)

- 検討した 3 案: A) `operator new` override, B) mimalloc 全置換, C) OS スナップショット
- 採用は **案 A**: C++ 側のみだが、本体 exe 内の `new` / `TJS_malloc` / SDL3 alloc を
  捕捉。プラグイン DLL 内 / C ライブラリ内部の素 malloc は依然対象外
  (必要なら案 B mimalloc 全置換に進む)。案比較の詳細は
  `doc/legacy/GlobalAllocationStats.md` §2 参照。

### 5.2 動作モード

```cpp
// pre-init (Initialize 未呼出)
new / TJS_malloc / SDL_malloc → std::malloc 直行 (header なし、stats なし、pool なし)

// post-init (Initialize 呼出後)
new / TJS_malloc / SDL_malloc → header (16B) 前置
                                → pool に紐付いていれば pool 経由 (TLSF)
                                → pool 枯渇 / pool 無効化なら std::malloc fallback
                                → stats 計上 (tag = TVPCurrentAllocTag())
```

### 5.3 Header layout (16B、`KRKRZ_ENABLE_ALLOC_STATS=ON` のみ前置)

```
+0:  size_t   size       (8 byte)
+8:  uint32_t magic      (4 byte) — kMagicRaw / kMagicPool / それ以外は pre-init or 外部 alloc
+12: uint16_t tag        (2 byte) — TVPAllocTag
+14: uint16_t pad        (2 byte)
```

`free` 時は magic で経路振り分け、不一致 (pre-init / プラグイン由来等) は
素 `std::free` に流す。

### 5.4 Initialize タイミング

`TVPGetCommandLine` が使える状態になった直後 (Application 組上げ後、
InitPath 完了後)。それより前 (SDL_Init / static initializer 等) の alloc は
pre-init モードで素 malloc を通る (= 観測対象外、コスト 0)。

詳細は `memory/feedback_global_alloc_init_timing.md`。

### 5.5 注意

- **size==0 でも non-null** を返す必要あり (C++ 標準)。
  `new` wrapper の先頭で `size = 1` に丸める; `memory/feedback_operator_new_size_zero.md` 参照
- pre-init で確保 → post-init で free のシーケンスは magic mismatch で
  素 `std::free` に流れる (一貫性あり)

---

## 6. システムアロケータ情報 (iTVPSystemAllocatorInfo)

`common/utils/SystemAllocatorInfo.{h,cpp}`。

`iTVPMemoryAllocator` が **本体管理下のプール** (File / Bitmap / Sound /
GlobalAlloc) の情報を返すのに対し、`iTVPSystemAllocatorInfo` は **その外側**
- OS / プラットフォーム提供アロケータや、システム全体のメモリ状況 - を
抽象化する。コンソール機 (Switch 等) で `nn::os::GetTotalFreeSize()` 相当の
プラットフォーム固有 API を露出させるためのフックポイントが主目的。

### 6.1 インタフェース

```cpp
struct TVPSystemAllocatorStats {
    size_t total_size       = SIZE_MAX; // アロケータの総容量
    size_t total_free_size  = SIZE_MAX; // 空き領域合計 (GetTotalFreeSize 相当)
    size_t allocatable_size = SIZE_MAX; // 確保可能な最大連続サイズ
    size_t used_size        = SIZE_MAX; // 使用中
    size_t peak_used_size   = SIZE_MAX;

    // プロセスメモリ情報 (OS レベル)
    size_t process_rss      = SIZE_MAX;
    size_t process_peak_rss = SIZE_MAX;
    size_t process_vsize    = SIZE_MAX;

    // システム全体
    size_t system_total_physical = SIZE_MAX;
    size_t system_avail_physical = SIZE_MAX;
};

class iTVPSystemAllocatorInfo {
public:
    // プラットフォームアロケータ互換 API
    virtual size_t GetTotalFreeSize()   const noexcept = 0;
    virtual size_t GetAllocatableSize() const noexcept = 0;
    virtual void   Dump()               const         = 0;

    // 一括取得 / 1 行サマリ
    virtual TVPSystemAllocatorStats GetStats() const = 0;
    virtual std::string             GetSummary() const = 0;
};
```

`SIZE_MAX` = 「情報が取得できない / 未対応」を示す sentinel。

### 6.2 デフォルト実装 (`tTVPDefaultSystemAllocatorInfo`)

一般 OS (Windows / macOS / Linux / Android) で、

- Win: `GlobalMemoryStatusEx` でシステム物理メモリ総量 / 空きを取得
- macOS: `sysctl(HW_MEMSIZE)` で総量、`host_statistics64` で空きページ算出
- Linux: `sysinfo` + `/proc/meminfo` の `MemAvailable` で空きを取得
- `process_*` は `TVPGetProcessMemoryInfo` (RSS / peak / vsize) を流用

`GetTotalFreeSize()` / `GetAllocatableSize()` は一般 OS では malloc ヒープ
内の空きを正確に知ることができないため、システム空き物理メモリ
(`Allocatable` は保守的に ×80%) を近似値として返す。

### 6.3 差し替え (プラットフォーム固有実装)

`tTVPApplication::GetSystemAllocatorInfo()` は virtual。組込み向け派生
Application でオーバーライドして、そのプラットフォームの API を呼ぶ実装を
返す:

```cpp
class tTVPApplicationSwitch : public tTVPApplication {
    MySwitchSystemAllocatorInfo m_sysAlloc;
public:
    iTVPSystemAllocatorInfo *GetSystemAllocatorInfo() override {
        return &m_sysAlloc;
    }
};
```

`TVPGetSystemAllocatorInfo()` (free 関数) は Application 初期化前にも呼べる
よう、常にデフォルト実装の Meyers singleton を返す (`MemoryOverlay` の
sampler thread 等が用途)。

### 6.4 利用箇所

| 利用箇所 | 用途 |
|---|---|
| `TVPHeapDump` (`SystemImpl.cpp`) | 各 allocator dump の後段にシステム情報を 1 セクション追加 |
| REPL `.mem` / `.sysalloc` | `GetSummary()` を 1 行表示 |
| MemoryOverlay (sampler) | `sys_total_free` / `sys_allocatable` を毎フレーム取得し SysFree 行に表示 |
| TJS API `System.getSystemAllocatorInfo()` | Dictionary で全 stats を返す |

---

## 7. AllocTagScope (thread-local tag stack)

`common/utils/AllocTagScope.{h,cpp}`。

```cpp
{
    TVPAllocTagScope _scope("TJS2");  // 以降の new / TJS_malloc は tag=TJS2
    ExecuteSomeTJSCode();
}  // RAII で pop
```

- thread_local stack (深さ 16) で push/pop
- `TVPCurrentAllocTag()` が stack top を返す
- `KRKRZ_ENABLE_MEMSTAT_DETAIL=OFF` のときは inline 空クラス (thread-local 参照しない)

### 7.1 計装ポイント

| 場所 | tag |
|---|---|
| `common/tjs2/tjsScriptBlock.cpp` ExecuteTopLevel | TJS2 (起動時のみ) |
| `common/base/EventIntf.cpp` TVPDeliverAllEvents | TJS2 |
| `common/base/EventIntf.cpp` TVPDeliverWindowUpdateEvents | TJS2 |
| `common/base/EventIntf.cpp` TVPDeliverContinuousEvent | TJS2 |
| `common/base/EventIntf.cpp` TVPDeliverCompactEvent | TJS2 |
| `common/visual/GraphicsLoaderIntf.cpp` TVPLoadGraphic | GraphicsLoader |
| script から `System.beginAllocTag("...")` / `System.endAllocTag()` | User (任意名) |

---

## 8. TJSObjectStats (Dict/Array 追跡)

`common/tjs2/tjsObjectStats.{h,cpp}`。`KRKRZ_ENABLE_MEMSTAT_DETAIL=ON` のみ動作。

### 8.1 追跡方式

| 追跡対象 | 方式 |
|---|---|
| tTJSCustomObject 総数 | `std::atomic<uint64_t>` (ctor/dtor で inc/dec) |
| tTJSDictionaryObject | `std::unordered_set` + `std::mutex` |
| tTJSArrayObject | `std::unordered_set` + `std::mutex` |

### 8.2 Dict 分析 (TVPDumpTJSObjectStats)

```
1. instance 総数
2. top-N (entries 数の多い順) の sample_keys 表示
3. entries 数別 bin (= 0 / 1-3 / 4-10 / 11-50 / 51-200 / >200) に分類
4. 各 bin 内で fingerprint (先頭 3 key を "|" 連結) の頻度集計、上位 3 件
```

bin + fingerprint で「小さい Dict が大量に増えてる」リーク (`textlength|speechtext|text`
パターンのメッセージバックログ等) が一目で見える。

---

## 9. ImageCache の構造

### 9.1 2 層構造

```
script load
    │
    ▼
TVPGraphicCache (image 層) — decoded Bitmap を保持、key=path
    │ (cache miss)
    ▼
TVPLoadGraphic ──→ デコード
    │
    ▼
StorageCache (file 層) — 生 byte 列を保持、key=path
    │ (cache miss)
    ▼
file open
```

両層とも path を key にする。**autopath 解決後の物理 path** に正規化
(`a50f37ac`)。

### 9.2 pinCache

`Storages.pinCache(path)`:
- path をピン登録 (解放対象外)
- 未 cache なら自動 load (cache miss で decode し pin)
- 書き込み stream open 時に対応する pin/cache が evict される

pin set は **2 系統** で path 正規化 (`a50f37ac`):
- 入力 path 由来 (autopath 解決前)
- 物理 path 由来 (解決後)

両方で照合することで `image/foo.png` と `data.xp3/image/foo.png` が同じ pin を指す。

### 9.3 自動有効化

`System.imageCacheLimit != 0` で ImageCache 有効化。デフォルトは
BitmapAllocator の pool capacity に揃える (例: 512 MB pool なら 512 MB)。
`0` で無効化 (旧挙動: 毎回 decode し直し)。

### 9.4 表 = 生存 buffer invariant

cache table のエントリは「現存する buffer を指している」を invariant に維持
(`59fa472d`, `1e413695`)。

- 通常 evict: table から外す + buffer release
- 外部 (TJS layer) が buffer を参照中: pinned 扱いで table に残す
- buffer がもう参照されていない: 即座に table から削除

これにより table を走査するだけで current_used が正しく取れる。

### 9.5 prefetch / loadAsync

`Storages.prefetchImage(path)` / `Storages.loadAsync(path)`:
- 別 thread で decode し ImageCache に投入
- 完了通知は callback (TJS) または `getFromCache` の結果
- file 層 (`StorageCache`) も同様に auto-drop が漏れないよう、
  prefetch / loadAsync 経路で明示 release を呼ぶ (`f6d24b84`, `79ac82de`)

---

## 10. スレッド安全性

| 構成要素 | 同期方式 |
|---|---|
| GlobalAllocStats counters | `std::atomic` (memory_order_relaxed) |
| GlobalAllocStats pool | `TVPPooledAllocator` (内部 mutex) |
| AllocatorStats collector | `std::atomic` (memory_order_relaxed) |
| Header magic check | atomic load (g_tracking_active) |
| TJSObjectStats sets | `std::mutex` |
| TJSObjectStats counters | `std::atomic` |
| AllocTagScope stack | thread_local (no sync) |
| MemoryOverlay sampler | 専用 thread + `std::mutex` |
| SystemAllocatorInfo | stateless (毎回 OS API 問い合わせ) |
| StorageCache / ImageCache | `tTJSCriticalSection` |

レース時、peak / ピーク値は瞬間値を取りこぼし得るが用途的に問題なし。

---

## 11. CMake gate 一覧 (実装影響)

| option | OFF 時の効果 |
|---|---|
| `KRKRZ_ENABLE_ALLOC_STATS` | `operator new` override 消滅、`TVPKrkrz*` wrapper が素 malloc 直行、pool 構築せず |
| `KRKRZ_ENABLE_ALLOCATOR_STATS` | `recordAlloc/Free` no-op、snapshot は SIZE_MAX マーカー、used() は SIZE_MAX |
| `KRKRZ_ENABLE_MEMORY_OVERLAY` | sampler thread 起動せず、Render 関数空 stub、GL/SDL 描画コードは build される (LTCG で消える) |
| `KRKRZ_ENABLE_PERIODIC_DUMP` | `TVPInitializeMemoryStatPeriodicDump` 空、cmdline 解析されず |
| `KRKRZ_ENABLE_MEMSTAT_DETAIL` | size_hist / per-tag / TJSObjectStats が全削除、AllocTagScope は空クラス |
| `KRKRZ_SDLMEMORY_STAT` | `SDL_SetMemoryFunctions` を呼ばず、GlobalAlloc[SDL] collector 不使用 |

`MASTER=ON` ビルドで上記 6 つすべて FORCE OFF。

---

## 12. 関連ファイル

- `common/utils/GlobalAllocStats.{h,cpp}` — operator new override / Krkrz pool
- `common/utils/SystemAllocatorInfo.{h,cpp}` — iTVPSystemAllocatorInfo + デフォルト実装
- `common/utils/AllocTagScope.{h,cpp}` — thread-local tag stack
- `common/base/MemoryAllocatorStats.h` — iTVPMemoryAllocator 用 stats collector
- `common/base/PooledAllocator.{h,cpp}` — TLSF プール
- `common/base/FileAllocator.cpp` — BasicFileAllocator
- `win32/visual/BitmapBitsAlloc.cpp` / `generic/visual/BitmapBitsAlloc.cpp` — Bitmap
- `common/base/SoundAllocator.{h,cpp}` — Sound (`-soundpoolsize` で TLSF pool 化、sound_malloc/calloc/realloc/free を extern "C" で公開)
- `external/sound-codecs/CMakeLists.txt` — libogg + libvorbis を FetchContent + `-include sound_alloc_hook.h` で `_ogg_*` を SoundAllocator に redirect
- `external/sound-codecs/include/sound_alloc.h` — C 互換 declarations (libogg/libvorbis 注入用)
- `external/sound-codecs/hooks/sound_alloc_hook.h` — `<ogg/os_types.h>` 後付け override
- `common/sound/OpusCodecDecoder.cpp` — libopus 直叩き (opusfile 撤去)、decoder state は sound_malloc 経由
- `common/sound/AudioStream.cpp` — miniaudio に `ma_allocation_callbacks = {sound_malloc, sound_realloc, sound_free}` を注入
- `ps5/src/app.cpp` `PS5SoundAllocator` — Direct Memory + sceLibcMspace 上の専用ヒープ
- `common/base/MemoryOverlay.{h,cpp}` — sampler thread + snapshot
- `sdl3/visual/MemoryOverlayRender.cpp` — SDL_Renderer 描画
- `common/visual/opengl/MemoryOverlayGL.cpp` — OGL 直接描画
- `common/base/MemoryStatPeriodicDump.{h,cpp}` — 周期 dump
- `common/tjs2/tjsObjectStats.{h,cpp}` — TJSObjectStats
- `common/base/StorageCache.{h,cpp}` — file 層 cache
- `common/visual/GraphicsLoaderIntf.{h,cpp}` — image 層 cache (`TVPGraphicCache`)

---

## 13. 関連ドキュメント

- `doc/MemoryGuide.md` — ユーザ視点の使い方
- `doc/DrawStats.md` — 描画スレッドプール統計 (`KRKRZ_DRAW_STATS`)
- `doc/legacy/` — 設計検討の履歴
  - `GlobalAllocationStats.md` — 案 A/B/C 比較、当初計画
  - `MemoryBudgetNegotiation.md` — 容量ネゴ G1-G5 / テレメトリ T1-T4 / 段階導入 P1-P5
  - `MemoryInspection.md` — 旧ユーザマニュアル
  - `MemoryLeakDiagnostic.md` — 旧リーク調査ガイド
  - `ImagePreloadAndCache.md` — ImageCache 設計 / I1-I7 フェーズ / pinCache / cache key 統一の進捗記録
