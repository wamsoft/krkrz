# MemoryAllocator 容量ネゴシエーション設計案

ファイル読み込みキャッシュ用アロケータ (`file_malloc` / `file_free`) と、
それを利用する側 (`StorageCache`) の間で、**容量上限・現在使用量・逼迫通知**
をやり取りできるようにするための設計メモ。実装はまだ行わない。

本ドキュメントの対象は主にファイルキャッシュ系統だが、`iTVPMemoryAllocator`
インタフェースは BitmapBits アロケータと共有しているため、ビットマップ系へ
の波及・流用可能性についても触れる。

## 1. 現状のおさらいと問題点

### 1.1 関係箇所

- `common/base/FileAllocator.cpp` — `file_malloc` / `file_free` と
  `BasicFileAllocator` (現状唯一の実装、内部は `::malloc` / `::free`)
- `common/base/BinaryStreamBuffer.h` — `tTJSBinaryStreamBuffer::create()`
  で `file_malloc` を経由
- `common/base/StorageCache.cpp` — キャッシュ本体、`MaxStorageCacheSize`
  (デフォルト 200 MB) と `CurrentStorageCacheSize` を内部管理
- `generic/environ/Application.h` / `win32/environ/Application.h` —
  `iTVPMemoryAllocator` 抽象 + `CreateFileAllocator()` 仮想関数

### 1.2 現状の構造的問題

`iTVPMemoryAllocator` は `allocate(size_t)` と `free(void*)` の 2 メソッド
しか持たない。よって以下が成立しない:

1. **キャッシュ側の上限値とアロケータ実体容量が独立**
   `MaxStorageCacheSize = 200MB` は固定の数値で、`Application` が差し替えた
   アロケータの実容量とは無関係。
2. **使用量問い合わせができない**
   `CurrentStorageCacheSize` はキャッシュ管理側のブックキーピングで、
   アロケータが他用途に消費した分は見えない。
3. **逼迫の通知がない**
   キャッシュが上限に達する前にアロケータが先に枯渇した場合、検知できる
   のは `allocate()` が `nullptr` を返した瞬間だけ (= 完全枯渇後)。
4. **失敗フローしかない**
   `file_malloc` の失敗時に `TVPClearOldStorageCache(0,false)` →
   `(0,true)` の二段リトライで吸収。`force=true` は使用中
   (`usecount > 0`) のキャッシュも巻き込むため、巻き添え I/O 再発の遠因。

このため、外部で固定サイズプール等を `CreateFileAllocator()` に刺すユース
ケース (組込み・モバイル等で物理メモリ上限を厳密に管理したいケース) を
サポートしようとするとキャッシュ側が常時暴走する。

## 2. 設計目標

| # | 目標 | 備考 |
|---|---|---|
| G1 | アロケータ実容量と使用量を `StorageCache` が問い合わせ可能 | 単純な getter |
| G2 | 逼迫の事前通知 (push) を受け取れる | reactive ではなく proactive |
| G3 | デフォルト実装 (raw `malloc`) では「容量不明」を素直に表現できる | `SIZE_MAX` で表現 |
| G4 | BitmapBits アロケータにも同じ枠組みを将来適用できる | 設計だけ揃える、即時実装は別 |
| G5 | スレッド安全 (背景の `tTVPStorageCacheThread` から呼ばれる) | コールバック側に責任 |

非目標:

- アロケータ毎の per-consumer 使用量トラッキング (StorageCache は自前で
  `CurrentStorageCacheSize` を持つので不要)
- 複数 consumer 間の予約・配分機構
- 細粒度のメモリプレッシャ階層 (Linux PSI 風)

## 3. インタフェース拡張案

### 3.1 `iTVPMemoryAllocator` への追加 (case A: 直接拡張)

```cpp
class iTVPMemoryAllocator {
public:
    virtual ~iTVPMemoryAllocator() {}
    virtual void* allocate(size_t size) = 0;
    virtual void  free(void* mem) = 0;

    // --- 以下、新規。すべて非純粋仮想 (default あり) で追加する ---

    // アロケータが管理する最大容量 (バイト)。
    // 不明・無制限 (例: 素の malloc) の場合は SIZE_MAX。
    virtual size_t capacity() const { return SIZE_MAX; }

    // 現在 allocate 済みでまだ free されていない総バイト数。
    // 把握していない実装は SIZE_MAX を返してよい
    // (= "この情報は信用するな" の意味)。
    virtual size_t used() const { return SIZE_MAX; }

    // 残り空きの目安。デフォルトは capacity - used を計算するが、
    // どちらかが SIZE_MAX なら SIZE_MAX を返す。
    virtual size_t available() const {
        size_t c = capacity(), u = used();
        if (c == SIZE_MAX || u == SIZE_MAX) return SIZE_MAX;
        return (c > u) ? (c - u) : 0;
    }

    // 逼迫通知の購読。コールバックは pressure ∈ [0.0, 1.0] を受ける
    // (1.0 = ほぼ枯渇)。通知タイミングはアロケータ実装の自由
    // (例: used / capacity が 0.8 を超えた時に 1 度だけ等)。
    // コールバックは任意のスレッドから呼ばれうる。再入禁止
    // (allocate / free 中から呼んでよいが、購読側で同期は必要)。
    using PressureCallback = std::function<void(float pressure)>;
    virtual void setPressureCallback(PressureCallback /*cb*/) {}

    // (compact() は P1 では入れない。プール実装が現れて必要になった時点で
    //  追加する。詳細は §8 参照)
};
```

**case A の利点**: dynamic_cast 不要、呼び出し側が単純。vtable レイアウト
変更を伴うが、`iTVPMemoryAllocator` を継承する箇所は in-tree のみ
(`BasicAllocator` / `GlobalAllocAllocator` / `HeapAllocAllocator` /
`ProcessHeapAllocAllocator` / `BasicFileAllocator` の 5 実装) で、
プラグイン側に派生クラスは無いため互換性懸念なし。

### 3.2 別インタフェースに切り出す (case B: capability 分離)

```cpp
class iTVPMemoryAllocator { /* 現状のまま */ };

class iTVPCapacityAware {
public:
    virtual ~iTVPCapacityAware() {}
    virtual size_t capacity() const = 0;
    virtual size_t used()     const = 0;
    virtual size_t available()const = 0;
    virtual void   setPressureCallback(PressureCallback cb) = 0;
};
```

実装側は多重継承、呼び出し側は `dynamic_cast<iTVPCapacityAware*>` で能力
チェック。

**case B の利点**: 既存 ABI を完全に温存。能力を持たないアロケータと持つ
アロケータを混在しやすい。
**case B の欠点**: dynamic_cast、多重継承、ファクトリ署名が分かれる。

### 3.3 推奨

**case A (直接拡張) を推奨**。理由:

- 既存 5 実装すべて in-tree で更新可能
- プラグイン側に派生クラスは無く、ABI 互換懸念なし
- すべて非純粋仮想 + 良性のデフォルト (`SIZE_MAX` = 不明) なので、追加
  メソッドを呼ぶ側が "不明扱い" にフォールバックできる

## 4. StorageCache 側の利用方針

### 4.1 起動時のネゴシエーション

```
TVPInitializeFileAllocator() で g_FileAllocator を生成した直後:

    size_t cap = g_FileAllocator->capacity();
    if (cap != SIZE_MAX) {
        // アロケータが上限を持つ → MaxStorageCacheSize はそれを超えない
        size_t budget = static_cast<size_t>(cap * kCacheShareRatio);
        if (MaxStorageCacheSize > budget) {
            MaxStorageCacheSize = budget;
        }
    }

    g_FileAllocator->setPressureCallback([](float p){
        if (p >= kPressureHardThreshold) {
            // 強駆逐: usecount 無視、最終アクセス順
            TVPClearOldStorageCache(0, /*force=*/true);
        } else if (p >= kPressureSoftThreshold) {
            TVPClearOldStorageCache(StorageCacheKeepTime, /*force=*/false);
        }
    });
```

定数 (P2 実装時に確定した値):

| 定数 | 値 | 役割 |
|---|---|---|
| `kCacheShareRatio` | **1.0** (実装) | アロケータ容量のうちキャッシュが使ってよい割合。当初案は 0.5 だったが、§21.3 の「外部参照中 entry は表に残す」invariant 化 (`1e413695`) と合わせて `1.0` に変更。FilePool は実質 StorageCache 専用なので半分残す意味がない |
| `kPressureSoftThreshold` | 0.75 | この pressure 以上で未使用エントリを駆逐 |
| `kPressureHardThreshold` | 0.90 | この pressure 以上で使用中含めて駆逐 |

`-filecacheratio` 等の上書きコマンドラインは需要が出ていないため未実装。
代わりに `-filepoolsize` でプール全体の容量を制御する形に集約。

### 4.2 ロード判定の差し替え

`tTVPStorageCacheThread::Execute` の `IsOverMaxStorageCacheSize()` 判定を
拡張:

```
bool IsOverMaxStorageCacheSize() {
    if (TVPGetStorageCacheSize() > MaxStorageCacheSize) return true;
    size_t avail = g_FileAllocator->available();
    if (avail != SIZE_MAX && avail < kReserveBytes) return true;
    return false;
}
```

`kReserveBytes` は単発の大きなファイル 1 個分のセーフマージン (例: 8 MB)。
キャッシュ管理上の上限と、アロケータ全体の残量、両方を満たす必要がある。

### 4.3 失敗フローの簡素化

`file_malloc` 内の二段リトライは残すが、callback で事前駆逐が入っている
ので **到達頻度が下がる**ことを期待する。`force=true` の最終手段として
の役割は維持。

`StorageCache.cpp:172-181` 周辺で `tTJSBinaryStreamBuffer::create()` が
`nullptr` を返すケースを **明示的に処理**する (現状は確認漏れ)。
具体的には:

- `entry.buffer = GetStreamBuffer(Stream);` が null `shared_ptr` を返したら
  キャッシュ登録せず、`Stream` をそのまま返す (=「キャッシュできなかった
  けど読み込みは続行できる」フォールバック)
- ログは WARNING レベル

これは本設計案の付随修正として一緒に扱う価値がある。

## 5. アロケータ実装側の責務

### 5.1 `BasicFileAllocator` (デフォルト, malloc 実装)

- `capacity()` は `SIZE_MAX` を返す (不明)
- `used()` は `SIZE_MAX` を返す (追跡しない)
- `setPressureCallback` は no-op

つまり**何も変わらない**。整合制御は将来の差し替え実装に対してのみ意味
を持つ。

### 5.2 想定する差し替え実装の例

組込み向けの "FixedPoolFileAllocator" を仮想:

- 起動時に `posix_memalign` 等で確保した N MB のプールから払い出し
- `capacity()` = N
- `used()` = 内部の払い出しカウンタ
- `allocate` 内で `used / capacity` が閾値を超えたら `pressureCallback`
  を**起動スレッドで** invoke (callback 実装側がスレッド安全に処理)

### 5.3 BitmapBits 系への流用

`tTVPBitmapBitsAlloc::Allocator` も同じ `iTVPMemoryAllocator` を使うので、
拡張メソッドを使えば「Bitmap ヒープが逼迫したら GraphicsCache を駆逐」と
いう似た挙動が組める。ただし Bitmap 系は `TVPDeliverCompactEvent` という
別系統の駆逐イベントが既にあるので、本案件のスコープから外し将来課題
とする (要件 G4)。

### 5.4 将来候補: GraphicsLoader / TJS2 ヒープ

P1〜P6 + T1〜T6 がすべて片付いた後の **更なる流用候補**。現時点では
スコープ外、本節は記録のみ。

#### 5.4.1 GraphicsLoader (画像デコード作業バッファ)

- **対象**: `common/visual/LoadPNG.cpp` の `PNG_malloc`/`PNG_free` を始め、
  libpng / libjpeg / TLG デコーダの作業バッファ
- **差し替え点**: libpng は `png_create_read_struct_2` の `user_malloc_fn`
  / `user_free_fn` 引数で差し替え可。LoadPNG.cpp:200-202 / 611-613 が
  既にコールバック化されており、現状はその中で素 `malloc` / `free` を
  呼んでいるだけ
- **新設アロケータ**: `Application::CreateGraphicsLoaderAllocator()`
  (tag = `GraphicsLoader`)
- **価値**: 容量制限としての意味は薄い (デコードは瞬間的・直列) が、
  T3 tag 別カウンタで「画像デコード中の瞬時メモリピーク」を観測できる
  と画像最適化の判断材料になる
- **コスト**: 小 (1 セッション程度。libjpeg / libpng / TLG / BMP の
  4 ローダで同じパターン)
- **優先度**: 低。T1〜T3 が入ってから「画像デコード中のピークが想像
  より大きい」と判明したら着手

#### 5.4.2 TJS2 ヒープ

- **対象**: TJS2 言語処理系全体のメモリ使用 (Variant / String / Object /
  Dictionary / Array / ScriptBlock / バイトコード等)
- **既存 indirection**: `common/tjs2/tjsConfig.h:91-93` の `TJS_malloc`
  / `TJS_free` / `TJS_realloc` マクロが既に存在 (現状は素 `malloc`
  に直結)。利用箇所は 6 ファイルのみ (`tjsInterCodeGen.cpp`,
  `tjsInterCodeExec.cpp`, `tjsLex.cpp`, `utils/ObjectList.h` 等)
- **問題**: TJS2 全体で見ると **`new` / `delete` 直接呼び出しが 30 ファ
  イル / 49+ 箇所**。マクロを差し替えても**コンパイル時産物 (AST /
  バイトコード) しか捕捉できず、実行時オブジェクトヒープ (8〜9 割) は
  見逃す**
- **半端実装の罠**:
  - 観測目的: 「TJS は 5 MB しか使ってません」表示で実態は 50 MB という
    ミスリードが起きる
  - 容量制限目的: 観測されないパスで OOM すると budget tracker から
    見れば「まだ余裕あります」状態のまま `bad_alloc` が走り、回収戦略が
    打てない (= 半端は無意味どころか有害)
- **本格対応に必要な作業**:
  1. tjsConfig.h マクロ → `iTVPMemoryAllocator` 経路化
  2. 主要型の `operator new` / `operator delete` オーバライド
     (`tTJSVariantString`, `tTJSDispatch` 系, `tTJSCustomObject`,
     `tTJSDictionaryObject`, `tTJSArrayObject`, `tTJSScriptBlock`,
     `tTJSInterCodeContext`, `tTJSRegExp`, `tTJSDate` 等 5〜10 型)
  3. 内部 STL コンテナ (`std::vector<tTJSVariant>` 等) の allocator
     差し替え (触る範囲が広いが個別ロジックは単純)
  4. TJS テストコード回帰検証
- **コスト**: 3〜5 セッション規模
- **優先度**: 最低。**T1〜T3 で BitmapBits + FileAllocator + Sound +
  GraphicsLoader の tag 別計測が出た後に、「TJS ヒープが想像より太い」
  と判明した場合のみ着手**。それまで動機なし
- **注意**: 半端で止めるなら **やらない方がマシ**。着手するなら最初から
  全パス補足を前提にする

#### 5.4.3 Sound デコード PCM バッファ (参考)

`WaveLoopManager` / `PhaseVocoderFilter` / `QueueSoundBufferImpl` の
`new []` 経由バッファ。同時多重再生時のピークが数十 MB に達しうる。
T3 tag 別計測で観測できるよう **`Sound` tag は §11.1 で予約済み**。
専用アロケータ化は本格対応 (T 系完了後) の判断待ち。

#### 5.4.4 標準関数 (malloc / new) 全体の捕捉

iTVPMemoryAllocator を経由しないグローバルヒープ allocation
(libpng / miniaudio / SDL3 等の C ライブラリ内部、kirikiri 本体内の
素 `new`、プラグイン DLL 内すべて) は本案件の P/T シリーズでは
捕捉できない。グローバル operator new/delete override / OS スナップ
ショット (HeapWalk) / mimalloc 化等の方策は **`doc/legacy/GlobalAllocationStats.md`**
に分離して管理。本体 exe 内の operator new + TJS_malloc + SDL3 内部 alloc
についてはそちら §6 (2026-05-15 実装) で TLSF プール化済み (CLI:
`-krkrzpoolsize=N` / `-sdlpoolsize=N`)。プラグイン DLL 内 / C ライブラリ
内部の素 malloc は依然対象外で、必要が顕在化したら案 B (mimalloc 全置換)
を検討する。

## 6. スレッド安全性

- `allocate` / `free` は複数スレッドから呼ばれる (メインスレッド +
  `tTVPStorageCacheThread`)
- `setPressureCallback` は起動時に 1 度だけ呼ぶ前提とし、ランタイム差し
  替えは想定しない (実装も atomic で十分、ロック不要)
- `pressureCallback` は **任意スレッドから呼ばれる**契約とする。実装側
  (StorageCache) が `TVPClearOldStorageCache` を呼ぶ際は内部 CS で守られて
  いるので問題なし
- `pressureCallback` 内から `allocate`/`free` を**呼ばないこと** (再入の
  危険) は API 契約として明文化

## 7. 段階的導入計画

| Phase | 内容 | 状態 |
|---|---|---|
| P1 | `iTVPMemoryAllocator` 拡張 (case A) + 既存 5 実装にデフォルト追加 | ✅ 完 |
| P2 | `StorageCache` 側でネゴシエーション処理 (4.1, 4.2) を実装 | ✅ 完 |
| P3 | `tTJSBinaryStreamBuffer::create()` null 戻り時のフォールバック修正 (4.3) | ✅ 完 |
| P4 | 容量制限のある実 allocator を投入 (本来は試験用 "FixedPool" の予定だった) | ✅ **TVPPooledAllocator として本番実装** (2026-05-11) |
| P5 | コマンドライン `-filepoolsize` / `-bitmappoolsize` 等の足し込み | ✅ 完 |
| P6 (任意) | BitmapBits 系への横展開 | ✅ **P4 と同時に実装** (BitmapAllocator も pool 経由) |

P4 は当初「tests / sample レベルの FixedPoolFileAllocator」だったが、最終
的には**本番デフォルトとして** TLSF プール allocator (`TVPPooledAllocator`)
を投入する形になった。詳細は §16 (新設) 参照。

## 8. 未決事項 / 要相談

- ~~**`kCacheShareRatio` のデフォルト値**~~ → **決着 (2026-05-12)**: `1.0` に
  変更 (`1e413695`)。詳細は §4.1 参照
- **pressure 閾値のヒステリシス**: 一度 0.75 を超えたら 0.7 を下回るまで
  再通知しない、等の制御を実装側 (アロケータ) に課すかどうか。現状は
  実装側に任せて未指定
- ~~**コマンドライン UI**~~ → **決着 (2026-05-11)**: `-filepoolsize` /
  `-bitmappoolsize` でプール全体の容量を制御する形に集約。比率指定や
  pressure 閾値の CLI 公開は需要待ち
- ~~**`compact()` を本当に入れるか**~~ → **決着 (2026-05-07)**: P1 では入れ
  ない。malloc 実装で書きようがなく、プール実装も未着手のため。実装が
  必要になった時点で追加する
- **マルチアロケータ将来構想**: 1 プロセス内に複数の `iTVPMemoryAllocator`
  が並立する未来 (例: ファイル用 + テクスチャ用) を見据えるなら、
  pressure callback の購読は単一 callback ではなく list にするか
  (現案は単一)。今回は単一でよさそう

## 9. 参考: 関連コミット / メモリ

- `common/base/FileAllocator.cpp:52-65` — 現行の二段リトライ
- `common/base/StorageCache.cpp:104-105, 282-291` — 容量管理の現状
- `common/base/StorageCache.cpp:363-368` — `IsOverMaxStorageCacheSize`
  時の `tTVPStorageCacheThread` の待機ロジック
- `win32/visual/BitmapBitsAlloc.cpp:163-193` — 別系統 (BitmapBits) の
  失敗時 GC + HeapCompact リトライ
- 過去メモ: `feedback_spsc_ring_capacity.md` (容量設計の整合不備が落とし
  穴になった先例) — 同じ性格の問題

---

ドキュメントの位置付け: 本ファイルは設計案であり、実装に着手する際は
P1 から進める。実装前にこのドキュメントを再レビューして合意を取ること。

---

# 拡張案: 使用状況テレメトリとリーク推定

§1-9 の容量ネゴシエーションは「現在状態」を扱うが、運用・デバッグの
ためには「累積・履歴・残存」の情報も欲しい。同じ `iTVPMemoryAllocator`
を出発点に、**コスト別にレイヤを分けて**乗せる。

## 10. レイヤ構成

| Layer | 内容 | 標準ビルドでの有効化 | デバッグ専用 |
|---|---|---|---|
| L0 | 容量ネゴシエーション (§3) | ON | — |
| L1 | 軽量カウンタ (alloc/free 回数, peak, 累積) | ON 推奨 | — |
| L2 | サイズヒストグラム (固定 8 段ビン) | ON 可 | — |
| L3 | tag 別カテゴリ集計 (enum tag) | ON 可 | — |
| L4 | tag 別リーク推定 (alloc-free 差) | ON 可 | — |
| L5 | per-block 追跡 (callsite/backtrace) | OFF | デバッグ/runtime opt-in |
| L6 | グローバル集約 + TJS API | ON | — |

**L1〜L4 は atomic 加算 1〜2 命令程度で済む安い層**。L5 だけが O(alloc) で
重いので分離。`MASTER` ビルドでも L1-L4 は残し、L5 は `_DEBUG` か
コマンドライン (`-memtrack=full`) でのみ有効化する想定。

## 11. インタフェース拡張案

### 11.1 tag (用途識別子)

```cpp
enum class TVPAllocTag : uint16_t {
    Unknown = 0,
    FileCache,        // StorageCache が使う file_malloc
    BitmapBits,       // tTVPBitmapBitsAlloc 経由
    GraphicsLoader,   // 画像デコード作業バッファ
    Texture,          // OpenGL テクスチャ
    Sound,            // AudioStream / decoder バッファ
    Movie,            // movie-player 経路
    TJS2,             // (対応する場合) TJS_malloc 経由
    User,             // プラグイン任意用途
    _Count
};
```

`allocate` に tag を渡せるオーバーロードを追加 (既存 `allocate(size_t)` は
`Unknown` 扱いで残す):

```cpp
class iTVPMemoryAllocator {
public:
    // 既存 + L0 の §3 拡張はそのまま

    // L3: タグ付き allocate。デフォルトは tag を捨てて allocate(size) を呼ぶ
    virtual void* allocate(size_t size, TVPAllocTag tag) {
        return allocate(size);
    }

    // L1-L2: 統計取得 (デフォルトは "未対応" を返す)
    struct Stats {
        size_t  current_used     = SIZE_MAX; // 現在使用 (= used())
        size_t  peak_used        = SIZE_MAX; // ピーク
        uint64_t total_allocated = 0;        // 累積 alloc バイト数
        uint64_t total_freed     = 0;        // 累積 free バイト数
        uint64_t alloc_count     = 0;
        uint64_t free_count      = 0;
        // L2: サイズビン (例: <128, <1K, <16K, <256K, <4M, <64M, <1G, ≥1G)
        std::array<uint64_t, 8> alloc_size_hist = {};
    };
    virtual Stats getStats() const { return Stats{}; }

    // L3-L4: tag 別の (current_used, alloc_count, free_count)
    struct TagStats {
        size_t   current_used   = 0;
        uint64_t alloc_count    = 0;
        uint64_t free_count     = 0;
        uint64_t total_allocated= 0;
    };
    virtual TagStats getTagStats(TVPAllocTag /*tag*/) const { return TagStats{}; }

    // L5: per-block 追跡の有効化 (デバッグ向け)
    enum class TrackingLevel { Off, Counts, Callsite, Backtrace };
    virtual void setTrackingLevel(TrackingLevel /*lv*/) {}
    virtual TrackingLevel getTrackingLevel() const { return TrackingLevel::Off; }

    // L5: 未解放ブロックダンプ (出力先はログ)。OFF ならカウントだけ報告
    virtual void dumpOutstanding() const {}
};
```

### 11.2 呼び出し側の使い分け

- `StorageCache` 経由: `file_malloc(size)` 内で `allocate(size, FileCache)`
- `BitmapBitsAlloc::Alloc`: `allocate(size, BitmapBits)`
- 画像ローダ (`LoadPNG.cpp` 等): デコード作業バッファは `GraphicsLoader`
- 音声系 (`AudioStream.cpp`): `Sound`

`file_malloc` は単一アロケータ前提なので呼び出し点が 1 箇所、tag は固定で
よい。BitmapBits も同様。tag を引数で渡せる API は他用途への拡張のため。

### 11.3 リーク推定 (L4)

L5 に頼らず L4 だけでも実用的な情報が得られる:

- アプリ終了直前 (`tTVPAtExit` の最終段) で全 tag の `getTagStats()` を
  集計し、`alloc_count != free_count` または `current_used > 0` の tag を
  WARNING ログに出す
- TJS シーン遷移後など中間タイミングでも呼べるようにする
  (リグレッション検出に有効)
- 「FileCache が 200MB 残ってる」「BitmapBits が 2MB 残ってる」等の粒度
  で出れば、原因特定の最初の絞り込みになる

L4 だけならアロケータ実装は内部で `unordered_map<tag, atomic<...>>` を
持つだけなので軽い。

### 11.4 per-block 追跡 (L5) の実装案

デバッグビルド・runtime opt-in。実装側がやることは:

- `allocate(size, tag)` 時に内部 map (block_ptr → record) に登録
  - record: size, tag, callsite (`_ReturnAddress()` / `__builtin_return_address(0)`)
  - `Backtrace` レベル時は `CaptureStackBackTrace` (Win32) / `backtrace` (POSIX)
- `free` 時に entry 削除。未登録ポインタの free は WARNING
- `dumpOutstanding()` で残存全件をログ出力 (size, tag, callsite/backtrace)

コスト試算: alloc/free 1 回ごとに mutex + map 操作で **数百 ns 〜 μs オー
ダー**増。ファイルキャッシュは秒に数十回程度なので無視できる。Bitmap/
Texture は秒に数百回ありうるので Backtrace は重い。よって:

- L5 デフォルトは `Off`
- `-memtrack=counts` で L4 相当 (実は何もしない)
- `-memtrack=callsite` で `_ReturnAddress()` 1 段だけ記録
- `-memtrack=backtrace` で 8〜16 段スタックトレース

## 12. グローバル集約 + TJS API (L6)

### 12.1 集約

```cpp
namespace TVPMemoryStat {
    struct Snapshot {
        // 各アロケータごと
        struct AllocatorEntry {
            std::string name;       // "FileAllocator", "BitmapBitsAlloc", ...
            size_t      capacity;
            size_t      used;
            size_t      peak;
            iTVPMemoryAllocator::Stats stats;
            std::map<TVPAllocTag, iTVPMemoryAllocator::TagStats> per_tag;
        };
        std::vector<AllocatorEntry> allocators;
        // OS 全体 (Windows なら HeapWalk 集計、Linux なら /proc/self/status 等)
        size_t process_rss = 0;
        size_t process_vsize = 0;
        // タイムスタンプ
        uint64_t tick_ms = 0;
    };
    Snapshot capture();
    void     dump(const Snapshot &snap);   // ログに整形出力
    void     dumpDelta(const Snapshot &before, const Snapshot &after);
}
```

`tTVPApplication` がアロケータを生成しているので、生成時に登録テーブル
へ追加する経路にして enumerate できるようにする。

### 12.2 TJS 公開

```tjs
// 一回限りのスナップショット
var snap = System.getMemoryStat();
Debug.message(snap.fileAllocator.used);
Debug.message(snap.fileAllocator.perTag["FileCache"].current);

// 差分計測
var t0 = System.getMemoryStat();
loadHeavyResource();
var t1 = System.getMemoryStat();
System.dumpMemoryStatDelta(t0, t1);
```

これにより、QA がシナリオ進行に応じてメモリ使用量推移を計測できる。
リグレッションテストの素材にもなる。

### 12.3 自動定期ダンプ

`-memstatinterval=10` で 10 秒ごとにログへ自動ダンプ、`-memdumponexit` で
終了時に全 snapshot + L4 リーク推定を出力、等のオプション。

## 13. テレメトリ層の段階的導入計画

容量ネゴシエーション側 (P1〜P6) と独立に並行可能:

| Phase | 内容 | 状態 |
|---|---|---|
| T1 | L1 軽量カウンタ + `getStats()` API + `BasicFileAllocator` 実装 | ✅ 完 |
| T2 | L2 サイズヒストグラム | ✅ 完 |
| T3 | L3 tag 引数 + 既存呼び出し点の差し替え | ✅ 完 |
| T4 | L4 tag 別リーク推定 + `tTVPAtExit` での自動ダンプ | ✅ 完 |
| T5 | L6 グローバル集約 + TJS API (`System.getMemoryStat`) | 保留 (需要次第) |
| T6 | L5 per-block 追跡 (`-memtrack=callsite/backtrace`) | 保留 (デバッグ用、需要次第) |

P1 → T1 → T2 → P2 → T3 → ... の順で実装、T1〜T4 と P1〜P6 は 2026-05-11 までに
完了。T5/T6 は需要が出てから判断 (M3/M4 の周期ダンプと M6 オーバレイで現状
カバーできているため、追加の TJS Snapshot API や per-block 追跡の即時 need が
顕在化していない)。

## 14. 既存仕組みとの統合

- **`win32/visual/BitmapBitsAlloc.cpp` の `tTVPLayerBitmapMemoryRecord`**:
  すでにブロック毎にレコードを持っているので、L5 (per-block 追跡) の
  Bitmap 版は実質ここに tag/callsite フィールドを足すだけ。新メカと
  二重管理にしない
- **`TVPHeapDump`** (`win32/base/SystemImpl.cpp`): Win32 ヒープウォーク
  の現行ロジックは L6 の `process_rss` 取得に統合
- **`_CrtDumpMemoryLeaks`**: CRT ヒープ部分のみ追跡しているので、
  L4/L5 が網羅的になれば後段に降格する (補助情報扱い)
- **過去メモリ `feedback_spsc_ring_capacity.md`** との関係: 容量設計の
  整合不備で「気づかない drop」が起きる事例。L4 の「カウント差ログ」
  はこの種の不整合検出にも有効

## 15. 未決事項 (テレメトリ追加分)

- ~~**tag を enum 固定値にするか拡張可能 ID にするか**~~ → **決着
  (2026-05-07)**: 当初は §11.1 の enum 固定で進める。プラグインから tag
  を増やしたい要求が出た時点で User+N 拡張を検討
- **マルチスレッド時の atomic コスト**: 1 alloc あたり 4〜5 個の atomic
  加算がベンチで影響するか要計測。BitmapBits は alloc 頻度が低いので
  許容、ファイルキャッシュも問題なし、画像ローダの細かい alloc が
  ホットスポットになる可能性
- **L5 の record map のロック粒度**: 単一 mutex は競合する。shard 化
  (ポインタ下位 4bit でバケット 16 分割) で十分の見込み
- **TJS API の安定化**: `System.getMemoryStat` は構造を変えにくい。
  最初は dictionary を返すだけにして、正式 API は実用例が溜まってから
- **シーン遷移時の自動チェックポイント**: `tTVPCompactEvent` 発火時に
  L4 ダンプを撃つフックを入れるか (リーク検出の精度向上)

---

# 16. TVPPooledAllocator (TLSF プール、2026-05-11)

P4 / P6 の本実装。**ファイルキャッシュ・ビットマップ処理の実メモリ利用域
を TJS2 ヒープなどスクリプト系から分離**して断片化を抑える、というのが
最大の動機。副次効果として §3-4 の容量ネゴシエーション (`capacity()` /
`available()` / pressure callback) が「数字を返さない」状態から「実値を
返す」状態に進化し、StorageCache の `TVPNegotiateStorageCacheBudget` も
finite cap で動くようになった。

## 16.1 構造

| 項目 | 値 |
|---|---|
| 実装 | `common/base/PooledAllocator.{h,cpp}` |
| アルゴリズム | TLSF (Two-Level Segregated Fit) |
| FL bin | 27 (2^5 〜 2^31) |
| SL bin | 32 (5-bit subdivision) |
| 小ブロック特例 | size < 1024B は FL=0 で線形 SL bucketing (= size/32) |
| min block | 32 byte (header 16 + free list ptrs 16) |
| align | 16 byte |
| backing | 起動時 1 回の `std::malloc(pool_size)` |
| fallback | pool 枯渇時は system malloc に逃がす (アドレス範囲で識別) |
| stats | 既存 `tTVPMemoryAllocatorStatsCollector` を Sized mode で使用 |
| capacity() | pool size を返す (= `TVPNegotiateStorageCacheBudget` が縮小判断する元値) |
| 並行性 | 単一 `std::mutex` で TLSF 操作を保護 (alloc 頻度が低いので問題なし) |

## 16.2 接続

| Allocator | デフォルト pool | CLI 指定 |
|---|---|---|
| FileAllocator | 256 MB | `-filepoolsize=N` (MB)。`none` / `0` / `off` で raw malloc に戻す |
| BitmapAllocator | 1024 MB (= 1 GB) | `-bitmappoolsize=N` (MB)。同上 |

WIN build (`win32/visual/BitmapBitsAlloc.cpp::CreateBitmapAllocator`) は
既存の `ProcessHeapAllocAllocator` 等を維持。SDL/generic build
(`generic/environ/Application.cpp::CreateBitmapAllocator`) と
`common/base/FileAllocator.cpp` がプール allocator に切り替わる。

## 16.3 fallback の動作

- pool に収まらない大きい alloc / pool 残量不足 → system malloc + 16 byte
  ヘッダ (size + tag) 前置で確保
- free 時はポインタが pool 範囲内 (`pool_buf_ <= p < pool_buf_ + pool_size_`)
  なら TLSF 経路、外なら fallback 経路
- fallback 統計は別カウンタ (`fallback_alloc_count_` / `fallback_free_count_`
  / `fallback_bytes_`) で集計、`stats_` には通常の `recordAlloc/Free` で
  混ぜて流す (= dump で見ると「合計 used」に fallback 分も乗る)

## 16.4 起動ログ例

```
[INFO] PooledAllocator [BitmapPool]: pool 1073741824 bytes (1024 MB) initialized at 000001F6A468C040
[INFO] PooledAllocator [FilePool]:   pool 268435456 bytes (256 MB) initialized at 000001F6E4696040
[INFO] StorageCache: budget set to FileAllocator capacity=256MB
[INFO] ImageCache enabled: limit=1024MB (physmem=65441MB, source=BitmapPool)
```

3 行目は §4 で設計した「`capacity` を見て StorageCache 上限を揃える」
が実動した結果 (旧 `capacity * 0.5` → `capacity` に変更、commit `1e413695`、
詳細は `doc/legacy/ImagePreloadAndCache.md §21.3`)。4 行目は ImageCache 側で
BitmapPool capacity を上限に採用する経路 (詳細は `doc/legacy/ImagePreloadAndCache.md §19`)。

## 16.5 効果

- **空間分離**: pool ごとに大ブロックを 1 個保持しているので、Process
  Explorer 等で見ると単一の常駐領域として識別できる。TJS2 ヒープや
  プラグイン DLL の alloc とは混ざらない
- **断片化抑制**: TLSF は O(1) で coalesce/split を行うので、長時間運用
  でも fragment が累積しにくい
- **finite capacity**: pool 容量を起点に各 cache 上限を再計算する。
  StorageCache は `TVPNegotiateStorageCacheBudget(g_FileAllocator)` で
  `MaxStorageCacheSize = FilePool.capacity()` (commit `1e413695` で
  従来の `capacity * 0.5` から変更; refcount-aware eviction との整合)。
  ImageCache (= `TVPGraphicCacheSystemLimit`) も同様に
  `BitmapPool.capacity()` を採用 (`SysInitImpl.cpp` の `TVPAfterSystemInit`、
  `BasicAllocator` フォールバック時のみ旧 `physmem / 10` + 512 MB cap)。
  pressure callback (§4) も動作
- **計測ノイズ低減**: `current_used` / `peak_used` が pool 内の値だけ
  カウントされるので、TJS2 / プラグインの一時 alloc に揺らされない
