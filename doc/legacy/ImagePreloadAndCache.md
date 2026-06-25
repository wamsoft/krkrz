# 画像プリロード・キャッシュ統合設計

「指定拡張子のファイルを背景スレッドで読み込んでキャッシュ」「指定拡張
子の画像ファイルは展開 (decode) まで進めて展開後の状態でキャッシュ」
「メインスレッドの画像ロード時は展開済みデータをコピーまたは共有する
だけ」を実現する統合設計案。実装着手前の合意用ドキュメント。

## 1. 現状 (実装されているもの)

### 1.1 ファイルキャッシュ層: StorageCache

- 入口 (TJS): `Storages.requestCache(path, minSize)` /
  `Storages.requestCacheFast(path)` / `Storages.addCacheTargetExtension(ext, minSize)`
- C++ 入口: `Application::CacheFileRequest()` → `tTVPStorageCacheThread::LoadRequest()`
  (`common/base/StorageCache.cpp:399`)
- 自動経由: `_TVPCreateStream()` (`common/base/StorageIntf.cpp:1726`)
  が `file://` かつ拡張子登録済みなら透過的に `TVPGetStorageCache()`
  を見に行く。`tTVPStreamHolder` 経由でも自動でキャッシュヒット
- ワーカー: `tTVPStorageCacheThread` (`ttpIdle` 優先度、Fast/Normal 2
  段キュー、`StorageCacheWaitTime=3` 秒の混雑制御付き)

### 1.2 画像デコード後キャッシュ層: TVPGraphicCache

- `common/visual/GraphicsLoaderIntf.cpp:1402` の
  `tTJSHashTable<tTVPGraphicsSearchData, tTVPGraphicImageHolder, ...>`
- キー: `(Name, KeyIdx, Mode, DesW, DesH)` の構造体
- 値: `tTVPGraphicImageData` — 1 枚の `tTVPBaseBitmap*` を保持
- 取り出し: `AssignToBitmap()` (`GraphicsLoaderIntf.cpp:1356`) は
  `TVPAllocGraphicCacheOnHeap` が `false` の通常ケースで
  `tTVPBaseBitmap::AssignBitmap()` を呼ぶ → 内部 `tTVPBitmap*` を
  **refcount で共有** (`LayerBitmapImpl.cpp:652`)
- LRU 駆逐: `TVPGraphicCacheLimit` 超過で末尾 chop。デフォルトは
  **BitmapAllocator pool の capacity** (default 1 GB、`-bitmappoolsize=N`
  で調整可)。`-bitmappoolsize=none` で BasicAllocator (raw malloc) に
  落としたときだけ従来の `physmem / 10` 段階値 + 512 MB cap に
  フォールバック (`SysInitImpl.cpp` の `TVPAfterSystemInit` / 同 generic 側)。
  詳細は §19.6 参照
- `TVPCompactEvent` (`level >= MINIMIZE`) で全クリア

### 1.3 非同期画像デコード層: tTVPAsyncImageLoader

- `common/visual/GraphicsLoadThread.cpp` 一本。単一スレッド (`ttpIdle`)
- 入口は **`Bitmap.loadAsync(name)` のみ** → `Application::LoadImageRequest()`
  → `tTVPAsyncImageLoader::LoadRequest()`
- `LoadImageFromCommand` (`GraphicsLoadThread.cpp:264`) で `tTVPStreamHolder`
  → ハンドラ (PNG/JPEG/TLG) で背景デコード → メインスレッドにポストして
  `HandleLoadedImage()` で `Bitmap` に attach + `TVPPushGraphicCache`
- 完了通知は `Bitmap` の `onLoaded` イベント

### 1.4 Bitmap の共有性 (重要な前提)

`tTVPBaseBitmap` は内部に `tTVPBitmap*` を 1 個だけ持ち、
- `AssignBitmap(rhs)`: ポインタ共有、refcount + 1
- `CopyFrom(rhs)`: 中身を新規確保して memcpy
- `Independ()`: refcount > 1 なら中身を新規確保してコピーして自分専用化
- `GetScanLineForWrite()` / 各種書込み API は内部で `Independ()` を呼ぶ

つまり**「展開済みキャッシュからのコピー」は実体としてコピーではなく
共有で済む**。書き換えがかかるまでメモリ実体は 1 枚。これは本案件の
鍵となる前提。

## 2. 不足している機能 (なぜ現状の async が活きないか)

ユーザー指摘の核心:

> 結局必要なタイミングになってから画像ロードするのでそこだけスレッド分け
> してもあまり意味がないことになってます

理由を分解:

1. **入口の偏り**: async は `Bitmap.loadAsync` だけ。実運用で多用される
   `Layer.loadImages(name, colorkey)` (`LayerIntf.cpp:6847`) は同期経路
   `TVPLoadGraphic` のみで、背景デコードを使えない
2. **拡張子で自動 decode preload するセマンティクスが無い**:
   `Storages.addCacheTargetExtension` は file 層止まり。「png は decode
   まで進めて」という指定ができない
3. **キャッシュキー不揃い**: async preload は固定キー
   `(name, glmNormal, 0, 0, TVP_clNone)`、同期は引数全部キー化。colorkey
   や desw/desh 指定があるとプリロード分はミスして再 decode
4. **進行中タスクの wait 機構なし**: 同期側がキャッシュ未ヒット時、
   「同名ファイルが async でデコード中」でも待たず自分で同期 decode
   開始 → 二重デコード or 無駄
5. **ワーカー並列度 1**: 複数枚を同時に背景処理できない (CPU 多コア機で
   勿体ない)
6. **メモリ予算が分離**: StorageCache (file bytes) と TVPGraphicCache
   (decoded) の上限は別管理。decode preload がフル稼働した時の合算上限
   が見えない

このうち **(1)〜(4) が "意味がない" 状態の主因**。(5)(6) は将来課題。

## 3. 設計目標

| # | 目標 |
|---|---|
| G1 | 拡張子で「decode preload する」を指定できる |
| G2 | `Storages.requestCache(path)` を呼ぶだけで画像系拡張子は decode まで進む |
| G3 | `Layer.loadImages` 同期経路がプリロード結果に**自動で**ヒットする |
| G4 | 同期ロード時、同名の async decode が進行中なら**待つ** (二重デコード回避) |
| G5 | キャッシュヒット時は **memcpy 0** で共有 (refcount + COW)。書込みで自動分離 |
| G6 | プリロード worker は並列度を選べる (デフォルト 1、上限は CPU - 1) |
| G7 | 既存 `Bitmap.loadAsync` / `Layer.loadImages` 互換、設定 OFF で従来動作 |
| G8 | `TVPCompactEvent` での駆逐は維持 |

非目標:

- 進行中 decode の途中キャンセル (キューからの取り下げのみ対応)
- TVPGraphicCacheLimit と StorageCache の合算予算ネゴシエーション
  (これは別案件の `MemoryBudgetNegotiation.md` 配下で扱う)
- 部分デコード (画像ストリーミング再生)

## 4. 全体アーキテクチャ

### 4.1 ロード経路図 (after)

```
TJS呼び出し
  ├─ Storages.addDecodeTargetExtension("png", opts)  ← 新規
  │     [decode prefetch 対象拡張子テーブル]
  │
  ├─ Storages.requestCache(path)
  │     ├─ TVPIsCacheTargetFile(path)?  → file 先読みキューへ
  │     └─ TVPIsDecodeTargetFile(path)? → decode 先読みキューへ  ← 新規
  │
  ├─ Bitmap.loadAsync(name)
  │     └─ 既存経路 (decode キューに入る)
  │
  └─ Layer.loadImages(name, colorkey)
        └─ TVPLoadGraphic (同期)
              ├─ TVPGraphicCache ヒット? → AssignToBitmap (共有)  ← 高速
              ├─ 進行中タスクあり? → wait (~10ms 単位ポーリング or event)  ← 新規
              └─ 同期 decode (現行通り) → cache push

[Worker pool: tTVPGraphicLoadPool (新規 or 既存 tTVPAsyncImageLoader 拡張)]
  ├─ Decode Queue (Fast / Normal の 2 段)
  ├─ Worker N スレッド (ttpIdle)
  └─ Decode 実行: tTVPStreamHolder → handler → tTVPTmpBitmapImage
                  → メインスレッドに完了通知 → TVPPushGraphicCache
```

### 4.2 関連データ構造

```cpp
// 新規: 進行中タスクテーブル
// 同期 LoadGraphic がここを覗いて「待つ」or「キックする」を決める
struct DecodeInFlight {
    ttstr             path;            // 正規化済み
    tjs_int32         keyidx;          // 通常 TVP_clNone
    tTVPGraphicLoadMode mode;          // 通常 glmNormal
    tjs_uint          desw, desh;      // 通常 0
    tTVPThreadEvent   completeEvent;   // 完了通知 (broadcast)
    std::atomic<bool> done;
    ttstr             errorMessage;    // エラー時
};
std::map<DecodeKey, std::shared_ptr<DecodeInFlight>> InFlightTable;
tTJSCriticalSection InFlightCS;

// DecodeKey は tTVPGraphicsSearchData と同じ構造 (キー揃え)

// 新規: 拡張子別 decode preload 設定
struct DecodeTargetOption {
    tjs_uint64 minSize = 0;     // この閾値以下はキャッシュしない
    bool       fast    = false; // 既定で fast キューに入れるか (false: normal)
    // 将来: 既定の colorkey 等 (現状は固定 TVP_clNone のみ)
};
std::map<ttstr, DecodeTargetOption> TVPDecodeTargetExtensions; // 拡張子→opt
```

## 5. API 設計

### 5.1 TJS 公開 API

```tjs
// 拡張子別 decode preload 登録 (既存 addCacheTargetExtension の上位)
Storages.addDecodeTargetExtension("png");
Storages.addDecodeTargetExtension("jpg", %[ minSize: 4096, fast: false ]);
Storages.removeDecodeTargetExtension("png");

// requestCache の挙動拡張: 画像拡張子なら decode キューへ自動投入
Storages.requestCache("image/title.png");      // decode preload を試みる
Storages.requestCacheFast("image/loading.png");// fast キューで先行

// 明示的 API (要望に応じて、最初は requestCache 経由のみでも可)
Storages.requestImageCache("image/foo.png");
Storages.cancelImageCache("image/foo.png");    // キュー上のみキャンセル

// 並列度設定
Storages.setImageDecodeWorkers(2);             // 既定 1。上限 CPU - 1
Storages.isImageCacheLoading();                // 進行中タスク有無
```

### 5.2 C++ 内部 API

```cpp
// StorageIntf.h 追加
void TVPAddDecodeTargetExtension(const ttstr &ext, tjs_uint64 minSize, bool fast);
void TVPRemoveDecodeTargetExtension(const ttstr &ext);
bool TVPIsDecodeTargetExtension(const ttstr &ext);
bool TVPIsDecodeTargetFile(const ttstr &name); // file:// + ext 両方判定

// GraphicsLoadThread.h 追加 / 拡張
class tTVPAsyncImageLoader {
public:
    // 既存: LoadRequest (Bitmap.loadAsync 用)
    void LoadRequest(iTJSDispatch2 *owner, tTJSNI_Bitmap *bmp, const ttstr &name);

    // 新規: prefetch (owner なし、結果は cache に積むだけ)
    void PrefetchRequest(const ttstr &name, bool fast = false);

    // 新規: 進行中タスクの公開 (同期側が wait に使う)
    bool HasInFlight(const DecodeKey &key) const;
    bool WaitForInFlight(const DecodeKey &key, tjs_int timeoutMs);

    // 新規: 並列度
    void SetWorkerCount(int n);
};

// GraphicsLoaderIntf.cpp の TVPLoadGraphic を改修:
// cache miss 時、進行中タスクがあれば WaitForInFlight して再度 cache を覗く
```

### 5.3 既存 API の扱い

- `Storages.addCacheTargetExtension`: そのまま (file 層用)
- `Storages.requestCache`: 内部で **両方** に分岐するように変更
  (file 層は従来通り、画像系拡張子なら decode キューにも積む)
- `Bitmap.loadAsync`: そのまま
- `Layer.loadImages`: 内部実装の `TVPLoadGraphic` だけ変更、TJS API は不変

## 6. キャッシュキーの揃え

`TVPGraphicCache` のキーは `(Name, KeyIdx, Mode, DesW, DesH)`。
プリロード経路は呼び出し時に colorkey や desw/desh を知らないので、
**既定値 `(name, glmNormal, 0, 0, TVP_clNone)` でしかキャッシュ登録
できない**。

これは仕様として割り切る:

> プリロードの恩恵を受けるのは `Layer.loadImages(name)` を colorkey
> 省略 + 既定モードで呼ぶ場合のみ。colorkey を指定された呼び出しは
> ミスして同期 decode に落ちる (= 従来通り)。

ただしプリロードした「素」のキャッシュエントリから colorkey 適用後の
ビットマップを派生させる最適化は将来あり得る (colorkey 適用が定数時間
で済むなら)。本案件では扱わない。

`addDecodeTargetExtension` のオプションで「**この拡張子はこの colorkey
で preload」を指定可能にする拡張は次フェーズ案件。

## 7. 同期ロード側の wait 戦略

`TVPLoadGraphic` の cache miss 直後、進行中タスクが居たら待つ:

```cpp
void TVPLoadGraphic(...) {
    // ... cache 検索 (既存)
    if (cache hit) return;

    // 新規: 進行中タスクをチェック
    DecodeKey key = makeKey(nname, keyidx, mode, desw, desh);
    auto inflight = AsyncImageLoader->FindInFlight(key);
    if (inflight) {
        // 完了 or タイムアウトまで待つ
        if (AsyncImageLoader->WaitForInFlight(key, kInFlightWaitMs)) {
            // 完了済み → cache 再検索でヒットするはず
            if (TVPCheckImageCache(...)) return;
        }
        // タイムアウト時はそのまま同期 decode へフォールスルー
    }

    // 同期 decode (既存)
}
```

**待ち戦略**:

- `tTVPThreadEvent` (broadcast) を使ったブロック wait
- タイムアウトは安全側で 5〜10 秒 (画像 1 枚のデコードが想定上限)
- メインスレッドが止まる懸念があるが、進行中ならどのみち decode 待ち
  なので**同期側で再 decode するより常に得**

**注意**: `WaitForInFlight` 中はメインスレッドがブロックするので、
完了通知は **decode 完了直後の worker スレッドから** broadcast する
(メインスレッドポストの前段)。`HandleLoadedImage` 経由だと wait が
解けない (デッドロック)。

## 8. ワーカープール化

### 8.1 構造

`tTVPAsyncImageLoader` を 1 → N スレッド化:

- `CommandQueue` / `LoadedQueue` は CS で共有 (現状通り)
- worker N 本が同じ `CommandQueue` から pop
- 完了は同じ `LoadedQueue` に push、メインへ単一 event 通知
  (`HandleLoadedImage` は既にループで複数捌く構造なので変更最小)

### 8.2 既定値

- `kDefaultWorkers = 1` (現状互換)
- `Storages.setImageDecodeWorkers(N)` で増やす
- 上限は `std::thread::hardware_concurrency() - 1` (UI/メイン分の確保)

### 8.3 リスク

- TLG 等 SIMD を使うデコーダは並列実行で CPU を食い合う → ttpIdle 優先
  度がメイン処理を阻害しないことを担保
- `tTVPBitmapBitsAlloc` が CS で全 alloc 直列化 (`BitmapBitsAlloc.cpp:165`)
  → 並列 decode 時のボトルネック懸念。ベンチで要計測

## 9. メモリ予算

- 既存 `TVPGraphicCacheLimit` (BitmapAllocator pool capacity = default
  1 GB、§19.6) を引き継ぐ
- decode preload で積極的に貯まる → 駆逐頻度が上がる可能性
- `Storages.setImageDecodeWorkers(N)` を上げると decode 速度 ↑、cache
  圧迫 ↑
- 本案件で **新たな上限値は導入しない**。`TVPGraphicCacheLimit` の調整
  で吸収する運用前提
- 将来: `MemoryBudgetNegotiation.md` の枠組みで file + decoded の合算
  予算管理に統合

## 10. キャンセル・エラー

### 10.1 キャンセル

- `Storages.cancelImageCache(path)`: キューから該当エントリを削除
- 既に worker が掴んでいる task はキャンセルできない (= 完走させる)
- これは現 `tTVPStorageCacheThread::CancelLoadQueue` と同じ方針

### 10.2 エラー

- 同期側 wait → タイムアウト or `inflight->errorMessage` が立っていれば
  通常の同期 decode にフォールスルー
- preload 経路のエラーは**ログ WARNING のみ** (TJS には出さない)。
  `Storages.requestCache` が裏でエラっても本番ロード時に再 decode で
  リトライされる方が安全

### 10.3 onLoaded イベント

- prefetch 経路は owner が居ないので onLoaded を発火しない
- `Bitmap.loadAsync` 経路は従来通り発火

## 11. スレッド安全性

| データ | 保護 |
|---|---|
| `CommandQueue` / `LoadedQueue` | `CommandQueueCS` / `ImageQueueCS` (既存) |
| `InFlightTable` | 新規 `InFlightCS` |
| `TVPGraphicCache` | `tTJSHashTable` 自体は単スレッド前提 → 既存実装は **メインスレッドからのみ触る**前提。worker からは触らない (HandleLoadedImage 内で触る) |
| `TVPDecodeTargetExtensions` | 新規 `TVPDecodeTargetExtensionsCS` |

**重要**: worker スレッドから `TVPGraphicCache` を直接更新しない。
完了通知でメインスレッドに渡してから push する (現実装と同じ)。

`InFlightTable` への登録は `LoadRequest` / `PrefetchRequest` 時 (任意
スレッド)、削除は完了通知のメインスレッドハンドラ。CS で守る。

## 12. 既存挙動への互換性

- `Storages.addCacheTargetExtension("png")` だけ呼んだ既存スクリプト:
  従来通り file キャッシュのみ。何も壊れない
- `Bitmap.loadAsync`: 完全に従来通り
- `Layer.loadImages`: cache hit すれば共有 (従来から実は共有経路だが
  hit する機会が少なかった)、miss なら同期 decode (従来通り)
- 新 API は opt-in。`addDecodeTargetExtension` を呼ばない限り decode
  preload は走らない

## 13. 段階的導入計画

| Phase | 内容 | 完了条件 |
|---|---|---|
| I1 | `tTVPAsyncImageLoader::PrefetchRequest` (owner なし版) を追加 | `Bitmap.loadAsync` から PrefetchRequest が呼べる、cache 登録される |
| I2 | `InFlightTable` 導入 + `WaitForInFlight` API + 同期 `TVPLoadGraphic` の wait 統合 | 同名 prefetch 中に `Layer.loadImages` を呼ぶと待って共有取得 |
| I3 | `TVPDecodeTargetExtensions` map + `TVPIsDecodeTargetFile` 判定 + `Storages.addDecodeTargetExtension` TJS API | TJS で拡張子を登録できる |
| I4 | `Storages.requestCache` を decode prefetch にも分岐 | requestCache だけで decode まで進む |
| I5 | `Storages.requestImageCache` / `cancelImageCache` の明示 API (任意) | TJS 側の明示制御パスが整う |
| I6 | ワーカープール化 (`SetWorkerCount`) | 並列 decode が動く、TLG 並列でクラッシュしない |
| I7 | キャッシュキー揃えの拡張 (`addDecodeTargetExtension` の colorkey オプション) | colorkey 付きでもプリロードヒット可 |

最小実用は **I1〜I4** で完結。I5 以降は需要に応じて。

実装順は P1 → P2 ではなく **I1 → I2 を 1 PR、I3 → I4 を別 PR** が
妥当 (テストしやすさ)。

## 14. テスト方針

- 機能: TJS スクリプトで `addDecodeTargetExtension` + `requestCache` の
  あと `Layer.loadImages` を呼んで cache hit ログが出ることを確認
- 性能: `Storages.requestCache(path)` 直後に `Layer.loadImages(path)`
  を呼ぶケースで、メインスレッド時間が `Layer.loadImages` 単独呼びより
  顕著に短いことを確認 (ベンチ計測)
- 競合: 同名 prefetch 進行中に `Layer.loadImages` を呼ぶケースで、
  二重 decode が発生しないこと (`TVPInternalLoadGraphic` 呼出回数を
  カウント)
- 並列: `setImageDecodeWorkers(2)` で 2 枚同時 prefetch → 完了時間が
  単スレッドより短くなること
- 互換: `addDecodeTargetExtension` を呼ばない既存スクリプトの挙動変化
  ゼロ

## 15. 未決事項

- **wait のタイムアウト値**: 5 秒? 10 秒? 平均 decode 時間 + α
- **fast キューを decode 側でも持つか**: I1 では持たず単一キュー、I6 で
  worker 増やすときに 2 段化検討
- **`Bitmap.loadAsync` と `requestImageCache` のエントリ統合**: 内部で
  同じ `CommandQueue` を使うが、`PrefetchRequest` は owner=NULL で完了
  通知不要にする方向。`tTVPImageLoadCommand` の owner_ を nullable
  扱い (現状 AddRef しているので破棄パスだけ要修正)
- **ファイルキャッシュとの順序**: `requestCache` で file → decode の
  二段にする? それとも decode が直接 `tTVPStreamHolder` で読む際に
  StorageCache 経由で透過ヒット (現状) のままで十分? **後者で十分**
  と思われる (StorageCache は `_TVPCreateStream` で自動経由するため、
  decode worker が `tTVPStreamHolder(name)` するだけで file キャッシュ
  からヒット)
- **TVPCompactEvent との連動**: prefetch 中にメモリ逼迫で `MINIMIZE`
  が来たら `TVPClearGraphicCache` で全消去される。これは既存の挙動を
  踏襲。prefetch キュー側もクリアすべきか? → してよいと思う
- **ワーカー優先度**: 現 `ttpIdle` を維持。`Storages.setImageDecodeWorkers`
  と一緒に優先度調整 API は出さない (need が出てから)

---

## 16. 関連ドキュメント

- `doc/legacy/MemoryBudgetNegotiation.md` — メモリ予算ネゴシエーション設計
  (本案件の駆逐頻度上昇問題は将来そこで解消)
- `common/visual/GraphicsLoadThread.cpp` — 既存 async loader
- `common/visual/GraphicsLoaderIntf.cpp:1402-1859` — 既存 cache + sync
  load
- `common/base/StorageCache.cpp` — 既存 file cache

---

ドキュメントの位置付け: 設計案 + 進捗記録。実装は I1 から段階導入。
各 phase 完了で動作確認 + ベンチ計測してから次に進む。

---

# 17. 進捗状況 (2026-05-07 現在)

## 17.1 完了済み (I1〜I4)

最小実用ライン I1〜I4 を一括実装し、ビルド (SDL/WIN 両変種) + 既存挙動の
非破壊スモーク確認まで通過済み。

| Phase | 状態 | 主要変更 |
|---|---|---|
| I1 `PrefetchRequest` (owner なし) | 完 | `GraphicsLoadThread.{h,cpp}`: `tTVPImageLoadCommand::prefetch_only_` フラグ追加、`tTVPAsyncImageLoader::PrefetchRequest`、`FinalizePrefetchOnWorker` (worker 側で 1x1 ダミー Bitmap → `SetSizeAndImageBuffer` → cache push)。worker から cache push できるよう `GraphicsLoaderIntf.cpp` 全 cache 操作を `TVPGraphicCacheCS` で保護 |
| I2 `InFlightTable` + sync wait | 完 | `GraphicsLoadThread.{h,cpp}`: `std::map<ttstr, shared_ptr<tTVPImagePrefetchInFlight>>` + `InFlightCS` + `tTVPThreadEvent` + `atomic<bool> done`。`GraphicsLoaderIntf.cpp:TVPLoadGraphic` で cache miss + 既定キー (`keyidx==TVP_clNone, mode==glmNormal, dw==0, dh==0`) の時 `TVPWaitForImagePrefetch(nname, 10秒)` → 完了で cache 再検索 → AssignBitmap で refcount 共有取得 |
| I3 `addDecodeTargetExtension` | 完 | `StorageIntf.{h,cpp}`: `TVPDecodeTargetExtensions` map + CS、`TVPAddDecodeTargetExtension` / `TVPRemoveDecodeTargetExtension` / `TVPIsDecodeTargetExtension` / `TVPIsDecodeTargetFile` / `TVPClearDecodeTargetExtensions`。TJS バインディング: `Storages.addDecodeTargetExtension(ext, minSize)` / `Storages.removeDecodeTargetExtension(ext)` |
| I4 `requestCache` を分岐 | 完 | `StorageIntf.cpp:requestCache` / `requestFastCache`: `TVPIsDecodeTargetFile` 判定で画像系拡張子なら `Application->LoadImagePrefetchRequest` も併発火 |
| 横断: Application API | 完 | `generic`/`win32` 両方の `Application.{h,cpp}` に `LoadImagePrefetchRequest(name)` と `GetImageLoadThread()` 追加 |
| 横断: CompactEvent 連動 | 完 | `tTVPClearGraphicCacheCallback::OnCompact` (MINIMIZE) で `TVPFlushImagePrefetchQueue` を撃ってから `TVPClearGraphicCache`。worker が掴み中の cmd は完走させる方針 |

**TJS 側の使用例**:

```tjs
Storages.addCacheTargetExtension('png');     // file 層キャッシュ (既存)
Storages.addDecodeTargetExtension('png');    // decode まで進める対象に追加
Storages.requestCache('image/foo.png');      // 両方発火
// ...シーン進行中に裏で decode 完了...
layer.loadImages('image/foo.png');           // cache hit、shared bitmap で 0-copy 取得
```

**確認できる効果の前提条件**:

- `Layer.loadImages` を **colorkey 省略 + 既定モード**で呼ぶ (colorkey 付き
  指定があるとキー不一致でミス → 同期 decode に落ちる)
- `Storages.addDecodeTargetExtension` で対象拡張子を事前登録
- `requestCache` 呼出から `loadImages` 呼出までに decode が完了していれば
  即取得、未了なら最大 10 秒待ち

## 17.1.1 非 file:// スキームへの対応 (2026-05-07 追補)

`Storages.requestCache(path)` を非 `file://` スキーム (psb://, psd:// 等
の MediaStorage プラグイン経由) でも decode prefetch が走るように調整:

- `TVPIsDecodeTargetFile`: `file://` 制約を撤廃、拡張子登録のみで判定
- `Storages.requestCache` / `requestFastCache`: file 層と decode 層を別判定
  - `TVPIsCacheTargetFile(nname)` (file:// のみ) → `CacheFileRequest`
  - `TVPIsDecodeTargetFile(nname)` (任意スキーム) → `LoadImagePrefetchRequest`
- 結果として psb://foo.png 等は **decode 層のみキャッシュ**され、底辺の
  file:// raw bytes との **二重キャッシュにならない**

レイヤの違い:

| Cache 層 | キー | 値 | 対象スキーム |
|---|---|---|---|
| `StorageCache` (file 層) | path | raw bytes | `file://` のみ |
| `TVPGraphicCache` (decode 層) | (path, keyidx, mode, dw, dh) | 展開後 `tTVPBitmap` (refcount + COW) | 任意 |

非 file:// が file:// を内部参照するケース (psb://archive.psb/foo.png →
file://archive.psb の派生) でも、decode 層は raw bytes と全く異なる
データ (展開済み bitmap) を保持するため、底辺の file:// 層キャッシュとの
重複は発生しない。

worker thread 側は `tTVPStreamHolder` 経由で各 MediaStorage の `Open()`
を呼ぶ。これは `_TVPCreateStream` 内で `TVPCreateStreamCS` の global lock
を取るため、プラグインの Open() が thread-safe でなくても安全
(worker から並列呼び出しはされない)。

## 17.2 保留 (実 app 挙動確認待ち)

実装したら必ず嬉しい、というほどの自信は無いものから順に保留。本番 app
で I1〜I4 を試してから判断する。

| Phase | 概要 | 着手判断条件 |
|---|---|---|
| I5 `requestImageCache` / `cancelImageCache` 明示 API | `Storages.requestCache` 経路では制御しきれないユースケースが出たら | 「ファイルキャッシュは要らずに decode キャッシュだけ欲しい」「シーン直前にピンポイントでキャンセルしたい」等の need が顕在化したとき。実装規模は小 (既存 `tTVPStorageCacheThread::CancelLoadQueue` と同パターンで `tTVPAsyncImageLoader` 側にも生やすだけ) |
| I6 worker プール化 (`Storages.setImageDecodeWorkers(N)`) | 複数枚同時 decode で総時間短縮を狙う | ベンチで「prefetch 1 本の連続 decode が予算内に収まらない」と判明したとき。**事前に**「TLG 等 SIMD デコーダの並列 CPU 食い合い」「`tTVPBitmapBitsAlloc::Alloc` の単一 CS (`BitmapBitsAlloc.cpp:165`) のボトルネック化」を実測で確認すること |
| I7 cache key 拡張 (colorkey オプション) | colorkey 付き呼出でも prefetch ヒットさせる | colorkey 多用シーンで「prefetch しているのに hit しない」が観測されたとき。`tTVPGraphicsSearchData` のキーを非既定値で prefetch 登録、拡張子別 option を構造体化して colorkey フィールド追加 |

## 17.3 残された未決事項 (実装時に再確認)

§15 で挙げた項目のうち、I1〜I4 実装時に確定したもの:

- ✅ wait タイムアウト = 10 秒で実装
- ✅ decode 側 fast/normal キューは単一 (I6 で再検討)
- ✅ `Bitmap.loadAsync` と `PrefetchRequest` は同じ `CommandQueue` 共有、`prefetch_only_` フラグで分岐
- ✅ ファイルキャッシュは `tTVPStreamHolder` → `_TVPCreateStream` 経由で透過ヒット (旧設計通り)
- ✅ `TVPCompactEvent (MINIMIZE)` で prefetch キューも flush
- ✅ worker 優先度は `ttpIdle` 維持

I5〜I7 着手時に再考する未決:

- **I6 着手前**: BitmapBitsAlloc の CS ボトルネック実測 / TLG 並列の CPU 食い合い実測
- **I7 着手時**: colorkey option を `addDecodeTargetExtension` の第 2 引数 (現
  `minSize`) と統合するか、option 辞書化するか
- **I7 着手時**: 同 path で colorkey 違いの複数 cache entry が並立した場合の
  メモリ予算扱い (cache 全体の `TVPGraphicCacheLimit` = BitmapAllocator pool
  capacity で吸収する想定で問題ないかは要確認)

## 17.4 撤退ライン

実 app で I1〜I4 を回した結果以下が起きたらロールバック検討:

- 同期 wait のタイムアウト 10 秒に頻繁に到達する (= prefetch が本来の load
  より遅く完了する想定外ケース)
- worker 経由の cache push に伴う `TVPGraphicCacheCS` 衝突でメインスレッド
  が体感できる stall を起こす
- `FinalizePrefetchOnWorker` の 1x1 ダミー Bitmap 構築が `TVPFontSystem` 等
  の初期化前に呼ばれてクラッシュ (init order の不備があれば顕在化)

これらが観測されたら issue を立てて、まずは `addDecodeTargetExtension` を
呼ばないことで実質無効化できる (I3〜I4 は opt-in のため)。コード自体の
ロールバックは II2 (worker cache push 経路) を `HandleLoadedImage` 経由に
戻すのが最小手段。

---

# 18. キャッシュ解放系の再設計 (2026-05-08〜)

I1〜I4 で「ロード/積み込み」側は揃った一方、**解放経路が層ごとに分散**して
いて運用しづらい状況になっている。本節は解放系の再設計と段階的移行計画。

## 18.1 解決したい課題

| # | 現状の問題 | 望む挙動 |
|---|---|---|
| Q1 | `Storages.clearCache(path)` が file 層しか触らない | path 単位 evict は両層 (file/decode) に効く |
| Q2 | decode 完了後も file 層 raw bytes が残り続ける | decode 層に積まれたら file 層は自動 drop |
| Q3 | 「タイトル戻り」用の一括 evict API が無い | pin されたものを残しつつ transient 全消し |
| Q4 | UI 等の常駐画像が LRU/Compact で消えうる | path 指定で **pin** できる |
| Q5 | IDLE/DEACTIVATE Compact が中途半端 (発火するが何もしない) | 意味付けを明確化 |
| Q6 | LRU 駆逐が「最古」しか見ず、実参照の有無を考慮しない | (将来) refcount==1 (cache 唯一参照) を優先駆逐 |

## 18.2 設計コンセプト

### A. エントリ "性質" の 3 値表現 (実装は P5 で)

| 性質 | 意味 | 駆逐順位 |
|---|---|---|
| `pinned` | 明示的に保護。UI 画像など | 駆逐対象外 (`MAX` Compact のみ消える) |
| `live`   | refcount > 1。Layer 等が掴んでいる、実参照あり | 通常 LRU の最後尾 (chop 候補だが優先度低) |
| `dormant`| refcount == 1。cache だけが持っている | 最優先 chop 候補。IDLE 時に整理対象 |

`tTVPGraphicImageData` は内部 `tTVPBaseBitmap*` を 1 個保持し、その
`tTVPBitmap*` の refcount を見れば live/dormant が判定できる
(`LayerBitmapImpl.cpp:652` の共有経路のため)。新しいフィールド追加は不要。

P2 で `pinned` フィールド + 判定 API、P5 で dormant 駆逐のロジックを
追加する 2 段構成。

### B. file→decode auto-drop

`TVPPushGraphicCache(nname, ...)` 完了直後に `TVPClearStorageCache(nname)`
を**無条件で**呼ぶ。decode 層に積まれた時点で raw bytes は不要 (再 decode
用に持つ理由がない)。

opt-out オプションは導入しない (実 app で「decode 後も raw bytes を保持
したい」需要が無いため)。後で必要になったら `addDecodeTargetExtension`
の option dict として追加。

### C. Compact レベルの意味付け再定義

| Level | 旧挙動 | 新挙動 |
|---|---|---|
| `IDLE` (5)        | 何もしない | 何もしない (P5 で dormant 整理を入れる予定) |
| `DEACTIVATE` (10) | 何もしない | 何もしない (バックグラウンド復帰即応性のため) |
| `MINIMIZE` (15)   | graphic cache 全消し + prefetch flush | **pin 以外を全消し** (= transient 全消し)。両層 |
| `MAX` (100)       | 全消し                                  | pin も含めて全消し (アプリ終了/OOM 相当)。両層 |

これで `System.doCompact(MINIMIZE)` が「使い終わった画像を解放、UI は残す」
という素直な意味になる。

### D. dormant 詰め直し (P5、将来案件)

「実参照のないキャッシュエントリは暇な時に詰め直し」の検討:

- **D1 (確実に効く)**: IDLE Compact で `dormant かつ最終 touch から N 秒以上`
  のエントリを chop。実装は LRU + `last_touch_tick` の小拡張
- **D2 (検討中)**: dormant エントリの `tTVPBitmap` を MemoryAllocator 上で
  別プールに移動 (defragment)。`MemoryBudgetNegotiation` 連携。実装大、
  効果は MemoryAllocator 実装依存
- **D3 (将来案件)**: dormant エントリを再エンコード (PNG/QOI) してメモリ
  削減。CPU 引換のメモリ削減。需要次第

P5 では D1 だけ着手し、D2/D3 は別案件とする。P2 で `IsDormant()` 判定 API
は仕込んでおき、P5 で再利用する。

## 18.3 API 提案

```tjs
// === pin (新規) ===
// pinCache は path を pin set に登録 + 既存 entry の pinned 化 +
// 自動的に load も開始する (= requestCache 相当の prefetch を発火)。
// 「ずっと持っていたいファイル」を 1 行で指定できる。
// path 正規化は norm (NormalizeStorageName) と placed (GetPlacedPath で
// autopath 解決後) の両方を pin set に登録するので、autopath 経由ファイル
// (= addAutoPath 配下に置いたファイル) でも file 層 cache に対して pin が
// 効く (decode 層は元から norm のみで一致)。
Storages.pinCache(path);              // 両層 sticky 化 + 自動 load
Storages.unpinCache(path);            // sticky 解除
Storages.isCachePinned(path);

// === path 単位 evict (改修) ===
Storages.clearCache(path);            // 既存 API。両層に効くよう改修 (pinned は無視)
Storages.clearCache();                // 全 transient evict (pinned 残る)
Storages.clearAllCaches();            // 新規。pinned 含めて全消し (= MAX Compact)

// === 一括 evict (新規) ===
Storages.clearTransientCaches();      // 両層 transient 全消し (タイトル戻り想定)
                                      // 内部的に MINIMIZE Compact と同じ

// === 既存 (変更なし) ===
Storages.requestCache(path, minSize);
Storages.addCacheTargetExtension(ext, minSize);
Storages.addDecodeTargetExtension(ext, minSize);
Storages.clearOldCache(keepTime, force);
```

呼び出し例:

```tjs
// 起動時 (addCache/addDecodeTargetExtension は事前に登録済み前提)
Storages.pinCache('ui/menu_bg.png');     // pin + 自動 load (両層)
Storages.pinCache('bgm/title.ogg');      // pin + 自動 load (file 層のみ)

// シーン進行中
Storages.requestCache('image/scene_01.png');  // transient (pin なし)

// タイトル戻り
Storages.clearTransientCaches();
// → menu_bg.png / title.ogg は残る、scene_01.png は両層から消える

// アプリ終了系
Storages.clearAllCaches();
```

## 18.4 段階的導入計画

| Phase | 内容 | 状態 |
|---|---|---|
| **P1** | `Storages.clearCache(path)` を両層対応 + `clearAllCaches()` / `clearTransientCaches()` 追加 | ✅ 完 (2026-05-08) |
| **P2** | pin 機構 (両層に `pinned` フィールド) + `Storages.pinCache` / `unpinCache` / `isCachePinned`。LRU chop / clearTransient / pressure callback で pinned スキップ | ✅ 完 |
| **P3** | file→decode auto-drop (`TVPPushGraphicCache` 末尾で無条件 `TVPClearStorageCache`) | ✅ 完 |
| **P4** | Compact level 意味付け再定義 + StorageCache 側にも CompactEventCallback 新設 + SDL `EVENT_LOW_MEMORY` の発火レベル見直し | ✅ 完 |
| **P5** (将来) | dormant 駆逐 (`IsDormant()` 判定 + IDLE 時 chop 等)。D2/D3 は更に先 | 保留 (別案件) |

P1〜P4 を 2026-05-08 に実装し、続いて §19-21 で観測 API / autopath 対応 /
書き込み連動 evict / cache key 統一 / 外部参照 invariant など周辺整備を完了
(〜2026-05-12)。

## 18.5 内部実装の要点

### StorageCache (`common/base/StorageCache.cpp`)

```cpp
struct StorageCacheEntry {
    std::shared_ptr<tTJSBinaryStreamBuffer> buffer;
    time_t lastaccess;
    int    usecount;
    bool   pinned = false;   // P2 で追加
};

// P1
void TVPClearStorageCacheEntry(const ttstr &name);   // 既存 TVPClearStorageCache のリネーム or 別名
void TVPClearAllStorageCacheForce();                 // pinned 含む全消し

// P2
void TVPPinStorageCache(const ttstr &name, bool pinned);
bool TVPIsStorageCachePinned(const ttstr &name);
void TVPClearTransientStorageCache();                // pinned 以外 erase

// P2: 既存 TVPClearOldStorageCache / IsOverMaxStorageCacheSize / pressure callback
//     のロジックで pinned をスキップ
```

### TVPGraphicCache (`common/visual/GraphicsLoaderIntf.cpp`)

```cpp
class tTVPGraphicImageData {
    bool pinned = false;                   // P2
    // last_touch_tick は P5 で追加
public:
    bool IsPinned() const { return pinned; }
    void SetPinned(bool v) { pinned = v; }
    // bool IsDormant() const;             // P5
};

// P1
void TVPClearGraphicCacheEntry(const ttstr &name);   // path 単位 evict

// P2
void TVPPinGraphicCache(const ttstr &name, bool pinned);
bool TVPIsGraphicCachePinned(const ttstr &name);
void TVPClearTransientGraphicCache();                // pinned 以外
// LRU chop は pinned をスキップ

// P4: tTVPClearGraphicCacheCallback::OnCompact のレベル分岐改修
```

### Storages TJS API (`common/base/StorageIntf.cpp`)

```cpp
// P1: clearCache(path) を改修。両層 evict
TJS_BEGIN_NATIVE_METHOD_DECL(clearCache) {
    ttstr path = numparams >= 1 ? *param[0] : TJS_W("");
    if (path.IsEmpty()) {
        // 全 transient
        TVPClearTransientStorageCache();
        TVPClearTransientGraphicCache();
    } else {
        ttstr nname = TVPNormalizeStorageName(path);
        TVPClearStorageCacheEntry(nname);
        TVPClearGraphicCacheEntry(nname);
    }
    return TJS_S_OK;
}

// P1: clearAllCaches / clearTransientCaches / pinCache / unpinCache / isCachePinned
//     を追加バインド
```

### file→decode auto-drop (`GraphicsLoaderIntf.cpp:TVPPushGraphicCache`)

```cpp
void TVPPushGraphicCache(const ttstr &nname, ...) {
    // ... 既存 push ロジック ...
    // P3: decode 層に積まれた時点で raw bytes は不要
    TVPClearStorageCache(nname);
    // (pinned は decode 層側に立つのでこれで OK。file 層 pin との
    //  整合は P2 で詰める)
}
```

## 18.6 互換性

- `Storages.clearCache(path)` の挙動が変わる (decode 層も消えるようになる) が、
  「両層消えてほしい」のが本来の意図と思われるため非破壊と判断
- `Storages.clearCache()` (引数なし) は新規。空文字列を渡すと現状は何も
  しないが、P1 以降は transient 全消しになる。**既存呼出があれば挙動変化**
- `System.doCompact(MINIMIZE)` の挙動変更 (graphic 全消し → transient 全消し)
  は P2 (pin 機構) を入れて初めて意味を持つ。pinCache 未使用なら従来と同じ
- `addDecodeTargetExtension` 未使用のスクリプトは P1〜P4 の影響を受けない

## 18.7 撤退ライン

- P3 (auto-drop) を入れた結果、再 decode 頻度が想定以上に増えるケースが
  あれば opt-out フラグを生やす。worst case は `TVPPushGraphicCache` 末尾の
  `TVPClearStorageCache` を消すだけで rollback 可
- pinned エントリが膨らみ `TVPGraphicCacheLimit` を恒常的に超過すると
  LRU が回らなくなる。pin 過多は警告ログを出す方向で抑止 (P2 で実装)
- Compact level 再定義 (P4) で既存 TJS スクリプトの `doCompact` 呼出
  の意味が変わる。検出された場合は app 側で旧挙動相当の API を別名で出す
  (`System.doCompactLegacy()` 等) — 必要が出てから判断

---

# 19. キャッシュ観測系 + 自動有効化 (2026-05-11)

§17-18 で揃ったロード/解放経路を「外から覗ける」観測 API と、cache が
default 無効になっていた長年の罠を直した一連の追加。

## 19.1 ImageCache の自動有効化 (重要修正)

`TVPGraphicCacheEnabled` は初期値 `false` で、`TVPSetGraphicCacheLimit(N)`
を **明示的に呼んだ時だけ** `true` になる仕様だった。これは TJS から
`System.graphicCacheLimit = N` で設定する想定だが、デフォルトでは誰も
呼ばないため起動直後は `Bitmap.load` も `requestCache` も decode 層
キャッシュに何も積まれない (TVPPushGraphicCache が空回り) 状態だった。

`TVPGraphicCacheSystemLimit` は `TVPAfterSystemInit()` で算出されているのに
(2026-05 当時は `physmem / 10` ベース、現在は §19.6 参照)、それを
`TVPGraphicCacheLimit` に反映するコードが無かった。

**修正 (`generic/base/SysInitImpl.cpp` / `win32/base/SysInitImpl.cpp`)**:
SystemLimit 計算直後に `TVPSetGraphicCacheLimit(TVPGraphicCacheSystemLimit)`
を呼んで自動有効化。INFO ログに以下の形で出力:

```
ImageCache enabled: limit=1024MB (physmem=65441MB, source=BitmapPool)
```

`source=` は `BitmapPool` (§19.6) / `auto` (BasicAllocator フォールバック時の
physmem 自動算出) / `-gclim` (CLI 明示指定) / `-gclim (capped by BitmapPool)`
のいずれか。`TVPGraphicCacheSystemLimit == 0` (極小メモリ機) の場合は
明示的に `disabled` ログを出す。

派生バグ修正: `SDL3Application::GetTotalPhysMemory()` が経由する
`getAvailableMemory()` の戻り値型が `long` (Windows MSVC では 32-bit) で、
`MEMORYSTATUSEX::ullTotalPhys` (uint64) を切り捨てて `tjs_uint64` に
sign-extend する形になっていた (例: `8446744073610653696` = 8 EB の値)。
`getTotalPhysMemoryBytes()` にリネームして戻り値型を `tjs_uint64` に統一。

## 19.2 キャッシュ列挙 API (TJS)

両層の現在エントリを取得する API。

```tjs
// File 層 (StorageCache, file:// raw bytes)
var fl = Storages.getFileCacheList();
// [%[ path:..., size:..., lastaccess:..., usecount:..., pinned:0/1 ], ...]

// decode 層 (TVPGraphicCache, 展開済み bitmap)
var il = Storages.getImageCacheList();
// [%[ path:..., keyidx:..., mode:..., dw:..., dh:...,
//     width:..., height:..., bytes:..., pinned:0/1 ], ...]

// ログに人間可読フォーマットで全件 dump (WARNING level)
Storages.dumpFileCacheList();
Storages.dumpImageCacheList();

// 画像 prefetch (Storages.requestCache の async path) が
// 進行中 (= InFlightTable 非空) かどうか。完了 polling 用
var loading = Storages.isImagePrefetchLoading();
```

C++ 側の対応: `TVPGetStorageCacheEntries` / `TVPGetGraphicCacheEntries`
(列挙) + `TVPGetStorageCacheCount` / `TVPGetGraphicCacheCount` (件数のみ)
+ `TVPDumpFileCacheList` / `TVPDumpImageCacheList` (TJS と REPL 共通実体)
+ `TVPIsImagePrefetchLoading` を追加。

## 19.3 REPL コマンド

`-repl` 起動時に以下が利用可能:

```
.filecache    -> Storages.dumpFileCacheList()
.imagecache   -> Storages.dumpImageCacheList()
```

## 19.4 件数表示の常時化

`TVPHeapDump` (= `System.dumpHeap` / 周期 dump / `.memdump`) と
MemoryOverlay 画面の両方に「FileCache: count=N pinned=P」「ImageCache:
count=N pinned=P」の 2 行を追加。これでオーバレイ表示中は常に件数が見える。

## 19.5 キャッシュ操作 DEBUG ログ

`-loglevel=debug` (大文字小文字どちらでも) で起動すると、以下の操作ごとに行が出る。
`MASTER` ビルドでは `DEBUG` レベルがコンパイル時に strip されるため出ない (詳細は
`doc/Logging.md` 参照)。Release/RelWithDebInfo は `-loglevel=debug` で実行時 ON 可。

| ログ名 | 出る場面 |
|---|---|
| `StorageCache:entry/get/clear` | file 層 cache の登録/取得/個別削除 (既存) |
| `StorageCache:clearOld/clearAll/clearTransient` | 一括解放 (件数 + dropped bytes 付き) |
| `StorageCache:pin` | pinned 状態が実際に変わったとき |
| `StorageCache:compact level=N -> ...` | Compact event hook 動作 |
| `ImageCache:lruChop` | TVPCheckGraphicCacheLimit が LRU 末尾を chop |
| `ImageCache:clearAll/clearEntry/clearTransient` | 一括解放 (件数 + dropped bytes 付き) |
| `ImageCache:pin` | pinned 状態が実際に変わったとき |
| `ImageCache:push` | TVPPushGraphicCache (decode 完了で cache 投入) |
| `ImageCache:hit` | TVPCheckImageCache が cache hit |
| `ImageCache:compact level=N -> ...` | Compact event hook 動作 |
| `Cache:pin/unpin` | TVPPinnedCachePaths 集合の更新 |

冗長になるので default の log level では出ない。バグ調査の際 `-loglevel=debug`
で全経路を追える。

## 19.6 SystemLimit を BitmapAllocator pool capacity に揃える

§19.1 の自動有効化と並行して、`TVPGraphicCacheSystemLimit` の決め方そのものも
見直した。これは 2026-05-11 の StorageCache の `MaxStorageCacheSize` を
FileAllocator capacity と揃えた変更 (`1e413695`) と同じ理屈の続き。

**旧仕様**: `physmem / 10` (上限 512 MB) を `TVPGraphicCacheSystemLimit`
として採用していた。これは 32-bit 時代のメモリ制約由来で、現在の TLSF
プール (§16, default 1 GB の `BitmapPool`) を入れた後は 512 MB cap が
ボトルネックになっていた。`-bitmappoolsize=2048` でプールを広げても
ImageCache 上限は 512 MB のままで、せっかく確保した pool の上半分が
ImageCache としては使われない構図。

**現仕様** (`win32/base/SysInitImpl.cpp` / `generic/base/SysInitImpl.cpp`):

```
poolCap = tTVPBitmapBitsAlloc::GetAllocator()->capacity()
if poolCap != SIZE_MAX:
    # BitmapAllocator が TLSF pool (= 通常運用)
    if -gclim auto: TVPGraphicCacheSystemLimit = poolCap
    if -gclim N:    TVPGraphicCacheSystemLimit = min(N, poolCap)
else:
    # BasicAllocator (raw malloc, -bitmappoolsize=none)
    # → 旧来の physmem / 10 + 512 MB cap
    ...
```

これにより:

- `-bitmappoolsize` でプールを広げると ImageCache も比例して広がる
- 駆逐不可な pinned/外部参照中エントリが pool 容量を圧迫する状況でも
  「pool に乗る分は cache 上限内」になり、`StorageCache` での 1e413695 と
  同じ構図 (refcount-aware eviction との整合) が成立
- `-gclim` の明示指定もこれまで通り尊重するが、pool capacity を超えた値は
  乗らないので頭打ちにする (ログに `-gclim (capped by BitmapPool)` と表示)
- 32-bit 機 / `-bitmappoolsize=none` 指定の **BasicAllocator 経路に限り**
  従来の `physmem / 10` + 512 MB cap が残る (=旧来挙動互換)

`TVPGraphicCacheLimit` (= TJS `System.graphicCacheLimit` の現在値) は
`TVPSetGraphicCacheLimit(N)` で SystemLimit でクランプして設定されるため、
TJS 側からの設定もこの新しい上限に従う。

## 19.7 動作テストスクリプト

`data/startup.tjs` に C / D / B の 3 キーバインドでテストを組み込み済:

| Key | 機能 |
|---|---|
| C | キャッシュ機能の状態機械テスト (async prefetch 完了待ち → dump → pin/clear → Compact) |
| D | `System.dumpHeap` |
| B | TLSF プールの stress test (40 個 alloc → ランダム順 free → 20 個再 alloc) |

C キーは `cacheTestState` 状態機械 + `onContinuous` 駆動で逐次進行する。
async path の完了は `Storages.isImagePrefetchLoading()` で polling。10 秒
タイムアウト監視あり。

## 19.8 動作確認時の見え方

実機ログ抜粋 (Win SDL build, key C 押下後):

```
[Step 1: Storages.requestCache x 5 (async, will wait)]
[After async prefetch (24ms): ImageCache=5 expected]
ImageCache: 5 entries (pinned=0, totalBytes=13139880)
        file://./.../star.png    206x195   bytes=160680
        file://./.../map07.png   1280x720  bytes=3686400
        file://./.../map01.png   1280x720  bytes=3686400
        file://./.../fg.png      1280x720  bytes=3686400
        file://./.../bg.jpg      800x600   bytes=1920000
[After clearTransientCaches: only pinned remain]
ImageCache: 2 entries (pinned=2, totalBytes=5606400)
  [pin] file://./.../map01.png 1280x720 bytes=3686400
  [pin] file://./.../bg.jpg    800x600  bytes=1920000
...
[After doCompact(100) MAX: all cleared incl. pinned]
ImageCache: 0 entries (pinned=0, totalBytes=0)
```

---

# 20. pinCache 拡張 + 書き込み連動 evict (2026-05-11)

§17-19 で揃ったロード/解放/観測の枠組みに、**pin 動作と stream 書き込みの
連動**を追加。日常運用で踏みやすかった「予防的 cache 全消し」と「pin が
効かない autopath ファイル」の罠を解消する一連の修正。

## 20.1 pinCache が pin 登録 + 自動 load まで担う

旧仕様 (P2 の段階):
- `Storages.pinCache(path)` は **pin set 登録 + 既存 entry の pinned 化のみ**
- ロードは別途 `Storages.requestCache(path)` で発火する必要があった
- 順序を間違えると「pin したつもりだが entry がまだ無いので effective には不発」

新仕様:
- `Storages.pinCache(path)` は内部で `Application->CacheFileRequest` /
  `Application->LoadImagePrefetchRequest` も発火する (= `requestCache` 相当)
- pin set 登録の方が先なので、cache 層の entry が作られるタイミングで
  pinned=true が初期化される
- 重複呼出は cache thread 側で skip されるので idempotent

```tjs
// 1 行で済む
Storages.pinCache('ui/menu_bg.png');   // pin + 両層 prefetch
Storages.pinCache('bgm/title.ogg');    // pin + file 層 prefetch
```

## 20.2 pin set の path 正規化を 2 系統に

旧仕様で踏んだバグ:
- `pinCache(path)` は `TVPNormalizeStorageName(path)` だけで pin set 登録
- decode 層 cache key (`TVPLoadGraphic` 内) も `TVPNormalizeStorageName` で
  揃うので一致 → pin が効く ✓
- 一方 file 層 cache (StorageCache, `_TVPCreateStream` 経由) は
  `TVPGetPlacedPath` (autopath 解決後の絶対 path) で entry を作る → 不一致
  → **autopath 配下のファイル (例: `bgm/bgm01.ogg`) は file 層で pin が効かない**

修正 (`common/base/StorageIntf.cpp:TVPPinCache/TVPUnpinCache`):
- pin set には `norm = TVPNormalizeStorageName(input)` と
  `placed = TVPGetPlacedPath(input)` の **両方** を登録 (異なれば 2 件)
- DEBUG ログ: `Cache:pin:<norm> (+placed:<placed>)`

これで decode 層 (norm 形式の cache key) と file 層 (placed 形式の cache key)
の **どちらでも** `TVPIsCachePathPinned` が hit するようになる。

## 20.3 書き込み stream open 時の自動 cache evict

旧仕様:
- `TVPSaveAsBMP/PNG/JPG` の冒頭で `TVPClearGraphicCache()` を呼んでいた
  (= decode 層全消し)。1 ファイル save するたび無関係な cache まで吹き飛ぶ
- 独自に `TVPCreateStream(path, TJS_BS_WRITE)` で書き込みストリームを開く
  ユーザコードでは、保存対象 path の cache を手動で `clearCache(path)` する
  必要があった。忘れると古い decode が残る

新仕様 (`common/base/StorageIntf.cpp:_TVPCreateStream`):
- `access != TJS_BS_READ` (= WRITE / APPEND / UPDATE) で開いた path に対して、
  自動的に `TVPClearStorageCache(name)` + `TVPClearGraphicCacheEntry(name)`
  を呼ぶ。両層から該当 path のみ駆逐
- 各 SaveHandler の冒頭にあった `TVPClearGraphicCache()` は撤去
- `tTVPGraphicHandlerType::Save` の per-path `TVPClearGraphicCacheEntry` も
  上記に集約されたので撤去

これで:
- 内部の Save API (BMP/PNG/JPG)
- TJS から `Bitmap.save` / `Layer.saveLayerImage`
- ユーザが直接 `TVPCreateStream(p, TJS_BS_WRITE)` で開く独自書き込み
- 任意の `iTJSBinaryStream` 派生で書き込む処理

すべての経路で、対象 path の cache が自動で無効化される。`ImageCache:clearAll`
ログが save のたびに出る現象は解消。

## 20.4 動作確認用テストへの追加

`data/startup.tjs` の C キーテスト case 4 に **OGG (autopath 経由) の pin 動作
確認**ステップを追加:

```tjs
case 4:
    Storages.clearAllCaches();
    Storages.unpinCache("map01.png");
    Storages.addCacheTargetExtension("ogg");
    Storages.pinCache("bgm01_dummy.ogg");  // pin + 自動 load
    cacheTest_step(5);
    break;
case 5:
    if (Storages.isCacheLoading()) return;  // file 層 load 完了待ち
    cacheTest_dumpAll("After pinCache + load: FileCache=1 ([pin] expected)");
    Storages.clearTransientCaches();
    cacheTest_dumpAll("After clearTransientCaches: bgm01_dummy.ogg は残るはず");
    ...
```

ファイルは `data/bgm/bgm01_dummy.ogg` (autopath 配下)。`pinCache` 単独で
load 開始 → `isCacheLoading()` 完了待ち → dump で `[pin]` 印を確認。

## 20.5 検証中に発見された関連 race condition バグ (副次修正)

§20.1 で pinCache から async prefetch が頻繁に走るようになり、main thread
の sync `Bitmap.load` と並列で動く機会が激増した結果、長年潜在していた
2 つの thread-safety 不具合が顕在化した。両方とも本案件の延長として修正済。

### 20.5.1 LoadJPEG: tjhandle が全スレッド共有されていた (`8736030e`)

`common/visual/LoadJPEG.cpp` 旧実装:

```cpp
static tjhandle jpegDecompressor = nullptr;  // ← 全スレッド共有
if (!jpegDecompressor) jpegDecompressor = tjInitDecompress();
```

libjpeg-turbo の `tjhandle` は **スレッドセーフでない** (1 ハンドルを複数
スレッドから並列で叩くと内部状態が破壊される)。pinCache → async prefetch
decode と sync `Bitmap.load` が同 JPEG を並走 → `decompress_onepass` 内で
memory access violation。

修正: `thread_local` + RAII デストラクタで各スレッド固有の handle に。
スレッド終了時に `tjDestroy` で自動解放されるので handle leak も無し。

PNG (libpng) は `png_create_read_struct` を呼出ごとにやる設計なのでこの
種の問題は元からない。

### 20.5.2 REPL log sink を mutex でシリアライズ (`c4cba399`)

`common/utils/REPL.cpp::TVPReplLogSink`: icline (`bbcode_t`) はスレッドセーフ
ではなく、複数 thread から `ic_printf` を並列で呼ぶと `bbcode_vprintf` 冒頭
の `assert(sbuf_len(bb->vout) == 0)` が失敗 (Debug ビルド) または出力破損
(Release) を起こす。

DEBUG ログ追加と pinCache 自動 load の組み合わせで、main thread の dump
出力と file cache thread / image load thread からの DEBUG ログが同時に
sink に来るパターンで顕在化。

修正: `g_repl_log_sink_mu` (std::mutex) で `TVPReplLogSink` 内 `ic_printf`
呼出をシリアライズ。

備考: REPL Execute (`ic_readline`) 自体の icline 呼出は依然 mutex 外。
ユーザのキー入力中の prompt 描画と log が同時に走った場合の race は別途
残るが、頻度が低く実害は少ないため対応保留。完全対策には icline 側に同期
機構を入れる必要がある。

### 20.5.3 教訓

「単一スレッドからしか呼ばれない前提」のコードは、async prefetch のような
新しい呼出経路を追加する際に**全部洗い直す必要がある**。本案件で踏んだ
パターン:

- C ライブラリの static handle (`tjhandle`、png_struct は OK だった例外)
- グローバル mutex 無しの I/O ライブラリ (icline)
- 静的バッファをまたいで使う 3rd party API

これらは sync 経路だけで使われていた間は気付けなかった。今後は async
プールに乗せる前に「呼び先が thread-safe か」を意識的に確認すること。

---

# 21. cache key 統一と「表 = 生存 buffer」 invariant 化 (2026-05-12)

§17-20 でロード/解放/観測経路は概ね揃ったが、デバッグ過程で「`tag[FileCache]
used` (allocator 視点) と `dumpFileCacheList totalBytes` (cache 表視点) が
最大 17 倍乖離する」「ファイル名の正規化ズレで二重 cache が起きる」「prefetch
の race で auto-drop が走らない経路がある」という 3 系統の問題が見つかった。
本節はそれらの修正と、結果として整った「StorageCache 表 = 生存 buffer の
indexable view」という invariant の説明。

## 21.1 cache key を autopath 解決後の物理 path に統一 (`a50f37ac`)

旧仕様:
- decode 層 cache (`TVPGraphicCache`) のキーは `TVPLoadGraphic` /
  `PrefetchRequest` で `TVPNormalizeStorageName` だけ通した name
  (= cwd-relative 論理 path)
- file 層 cache (`StorageCache`) のキーは `_TVPCreateStream` で
  `TVPGetPlacedPath` (autopath 解決後の絶対 path) を使う
- 同じ物理 file を異なる名前 (`bg.jpg` vs `image/bg.jpg`) でロードすると
  decode 層に **2 entry 並立** したり、`clearCache(path)` で名前形式が
  違うと hit しなかったり

修正: `TVPResolveCachePath(input)` ヘルパーを追加 (`StorageIntf.h`):
```cpp
// GetPlacedPath で autopath 解決を試み、失敗時 NormalizeStorageName fallback
ttstr TVPResolveCachePath(const ttstr &input);
```
- `TVPLoadGraphic` (sync 経路、cache key 計算)
- `tTVPAsyncImageLoader::PrefetchRequest` (async 経路、`cmd->path_`)
- `Storages.clearCache(path)` / `clearFastCache(path)` (path 単位 evict)

すべてこの helper で揃え、cache key は **常に autopath 解決後の物理 path**
になる。pinCache の path 集合は norm + placed 両方を入れる既存挙動を維持
(file 不在時の pin など edge case 用)。

## 21.2 prefetch / loadAsync の file 層 auto-drop 漏れ修正 (`f6d24b84`, `79ac82de`)

旧仕様の race:
1. main thread: `Bitmap.load("bg.jpg")` sync → file 層 cache 登録 → decode →
   push → `TVPClearStorageCache` で auto-drop
2. 並走中の prefetch worker: 同 path を `tTVPStreamHolder` で開く →
   main 側 auto-drop 後なので file 層 cache miss → 再 EntryStorageCache →
   別 buffer で再登録
3. worker decode 完了 → `TVPHasImageCache==true` (= main 側 push 済) →
   `TVPPushGraphicCache` を **skip** → **auto-drop も skip**
4. holder destructor で stream destruct → buffer refcount=1 (table のみ)
5. file 層 cache 表に entry 残存 → buffer 永続化 → リーク

`Bitmap.loadAsync` 経路 (`HandleLoadedImage`) も同型のリーク 2 件 (load 失敗
+ push skip) があった。

修正:
- `FinalizePrefetchOnWorker` (prefetch 経路): `TVPHasImageCache==true` 枝 +
  `cmd->result_` 非空枝で `TVPClearStorageCache(cmd->path_)` 明示呼び出し
- `HandleLoadedImage` (loadAsync 経路): 同様の 2 経路で明示駆逐
- `tTVPGraphicHandlerType::Save` の per-path clear は `_TVPCreateStream`
  WRITE 経路の自動 evict に集約 (重複削除)

## 21.3 外部参照中の buffer は表に残す (`59fa472d`, `1e413695`)

ユーザの観察「`tag[FileCache] used=173MB` だが `dumpFileCacheList totalBytes
=9.6MB`」の正体: font subsystem の `_fontlist` (`common/visual/FreeType.cpp`、
最大 10 entries) や `XP3ArchiveHandleCachePool` (`common/base/XP3Archive.cpp`、
最大 8 entries) が **stream を介して buffer の shared_ptr を保持**し続ける
ため、StorageCache 表からは clear で消えても buffer は alive なまま、という
状態。

```
[load 直後]                       [clearTransient 後]
StorageCache 表: [bg.jpg] [font.otf]    StorageCache 表: (空)
              ↓                           
       shared_ptr<buffer>            外部 holder (font 等):
              ↑                       shared_ptr<stream> → buffer (alive)
       他 holder (font 等):              
       shared_ptr<stream> → buffer    ← 表外で生きてる buffer
```

修正: 「StorageCache 表 = 生存 buffer の indexable view」 invariant にする。
`shared_ptr::use_count() > 1` (= cache 表以外に保持者あり) の entry は
clear をスキップ。

```cpp
static inline bool TVPStorageCacheEntryReferencedExternally(
    const StorageCacheEntry &e) {
    return e.buffer.use_count() > 1;
}
```

各 clear 系の挙動:
| API | 挙動 |
|---|---|
| `TVPClearStorageCache(name)` (default) | 参照中はスキップ |
| `TVPClearStorageCache(name, force=true)` | 必ず削除 (書き込み時 invalidate 用) |
| `TVPClearTransientStorageCache` | pinned + 参照中を残す |
| `TVPClearAllStorageCache` | 参照中は残す |
| `TVPClearOldStorageCache` | 参照中はスキップ |
| `_TVPCreateStream` 書き込み時 evict | `force=true` で強制削除 (stale 化) |

書き込み時だけ force=true にしているのは、新 reader が古い content を
読んでしまうのを防ぐため。stream を hold 中の reader は `shared_ptr` 経由で
古い snapshot を見続けるが、それは意図的な動作 (stream の所有を尊重)。

### 副作用: MaxStorageCacheSize の調整 (`1e413695`)

旧 `TVPNegotiateStorageCacheBudget` は `MaxStorageCacheSize = capacity * 0.5`
としていた (例: 256MB pool → 128MB cache 上限)。だが上記の invariant 化で
`CurrentStorageCacheSize` には外部参照中で駆逐不可な entry も積まれるので、
font + archive holds が上限を恒常的に超え、cache thread が
`StorageCache: over max size, wait 3 sec` で空振り停止するように。

修正: `MaxStorageCacheSize = capacity` に揃える。FileAllocator pool は実質
StorageCache 専用 (`tag[FileCache]` 1 種のみ経由) なので半分残す意味がない。
`over max` 判定は「pool が物理的に枯渇しそうな時だけ」発火する。

pressure callback (0.75 / 0.90 で `TVPClearOldStorageCache`) は引き続き
有効なので段階的駆逐は機能。「全部 pin で埋まる」場合の最終 safety net は
`TVPPooledAllocator` の system malloc fallback。

## 21.4 動作確認ログ

`-loglevel=debug` 起動時に、以下のログで挙動を追える:
- `StorageCache:entry:<path> size=<bytes>` (新規 cache 登録)

PooledAllocator の毎呼出ログ (`PoolAlloc:[FilePool] ...`/`PoolFree:[FilePool] ...`)
は通常見ない。leak 追跡時のみ `common/base/PooledAllocator.h` の
`TVP_POOL_VERBOSE_LOG` を `1` に変える (or cmake `-DTVP_POOL_VERBOSE_LOG=1`)
ことで有効化できる。default の `0` ではログ文字列フォーマット自体が
コンパイル時に消える。
- `StorageCache:get:<path>` (cache hit)
- `StorageCache:clear:<path>` / `clear:<path> (forced)` (実駆逐)
- `StorageCache:clearSkip:<path> (referenced, use_count=N)` (参照中で駆逐 skip)
- `StorageCache:clearTransient: dropped=N bytes=B kept(pinned)=K1 kept(referenced)=K2`

`tag[FileCache] used` と `dumpFileCacheList totalBytes` が一致していれば
invariant が成立。実機検証で:
```
FileAllocator used=137.66MB ... alloc_n=122 free_n=67  (outstanding 55)
FileCache: 55 entries (pinned=44, totalBytes=144345908)  // = 137.66MB
```
完全一致を確認済 (2026-05-12)。
