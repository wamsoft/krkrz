# ttstr データ保持の棚卸し（スレッド安全性調査）

> ステータス: **キャッシュ系（H1-H5 / H8-H9）の tjs_string 化を実装済み (2026-06-15)。**
> 起点コミット: `d5292429`（AutoPath キャッシュの暫定対処）
> 調査日: 2026-06-15 / 実装日: 2026-06-15

## 0. スレッド実態と「本当に新しく壊れた箇所」（実装前の再評価）

> 当初 §3 は「ワーカースレッドが触るか」で機械的にリスク分類したが、実装に先立ち
> イベント配送のスレッド実態を精査した結果、**サウンド/タイマー/イベント由来は
> 旧来からの安全な設計**であり、**実際に新しく壊れたのは後付けされたキャッシュ系
> スレッドが ttstr を越境共有している箇所**であることが判明した。

**正しい設計（かつての吉里吉里から不変）**: ワーカースレッドはイベントを直接
投げず、OS/SDL のイベント機構でメインスレッドを起こすだけ。実際の `TVPPostEvent`
とイベント配送・ラベル発火はメインスレッドで行う。

- WINVER: `EventQueue.PostEvent( NativeEvent(TVP_EV_xxx) )`（Window メッセージ）→
  窓プロシージャ (`Proc`/`UtilWndProc`) でメインスレッド処理。
- Generic(SDL): `Application->SendAppEvent( TVP_EV_xxx, ... )` → `Dispatch()`
  (AppEventInterface) でメインスレッド処理。

実コードでの確認:

- `common/sound/SoundEventThread.cpp`: worker の `Execute()` は wake 通知のみ。
  `HandleWake()`（コメントに「メインスレッド」明記）で
  `FireLabelEvents...→InvokeLabelEvent→TVPPostEvent` を呼ぶ。
- `common/visual/GraphicsLoadThread.cpp`: worker の `LoadingThread()` は
  `SendToLoadFinish()` で marshal。`HandleLoadedImage()`（`Proc`/`Dispatch` 経由
  ＝メインスレッド）が `TVPPostEvent` を呼ぶ。
- `common/utils/TimerIntf.cpp`: TimerThread は `Trigger`→pending 登録 + wake のみ。
  `Fire→TVPPostEvent` はメインスレッド (`HandleWake`)。

**TVPPostEvent / TVPEventQueue 監査結果（2026-06-15）**: 全ての `TVPPostEvent`
呼び出しと `TVPEventQueue`/`TVPInputEventQueue` への push/erase は**メインスレッド
からのみ**実行される。ワーカースレッドから直接イベントキューを触っている不正箇所
は**無し**（唯一 `tTVPVSyncTimingThread` が `EventQueue.PostEvent` を使うが、これは
thread-safe な Win32 `PostMessage` 経由で問題なし）。したがって **H10
（イベントキュー）は実害なし**、対応不要。

### 当初リスク分類の訂正

| # | 当初 | 実態 | 対応 |
|---|---|---|---|
| H1-H5 | 🔴 | 🔴（新規・キャッシュ系で確定） | **tjs_string 化 実装済み** |
| H8-H9 | 🔴 | 🔴（新規・キャッシュ系で確定） | **tjs_string 化 実装済み** |
| H6/H7 サウンドラベル | 🔴 | 🟡 LabelEventQueue は decode⇄メインだが RefCount 操作は `BufferCS` 下で直列化。旧来設計 | 今回は対象外（旧来から動作。要すれば別途 tjs_string 化） |
| H10 イベントキュー | 🔴 | 🟢 投入・配送ともメインスレッド限定（上記監査） | 対応不要 |
| M2 拡張子マップ | 🟡 | 🟢 キー RefCount 変更は登録/削除(メイン)のみ。worker は CS 下 `find`(比較のみ) | 対応不要 |
| M3 PinnedCachePaths | 🟡 | 🟢 同上（worker は `find` のみ） | 対応不要 |

---


## 1. 問題の本質

`ttstr`（= `tTJSString`、実体は `tTJSVariantString` を参照カウント付き COW
バッファで共有する文字列クラス）の **参照カウント `RefCount` の増減は非
atomic** である。

- `common/tjs2/tjsVariantString.h:84` — `AddRef()` は `RefCount++`
- `Release()` も同様に非 atomic なデクリメント

ttstr のコピーは COW（copy-on-write）であり、コピー時には新しいバッファを
確保せず、同一の `tTJSVariantString` を共有して `RefCount` を増やすだけ。
このため、**COW 共有された同一バッファを複数スレッドが同時に touch
（コピー・破棄・`c_str()` 経由の参照）すると、`RefCount` の増減が競合して
二重解放 → メモリ破壊を起こす。**

クリティカルセクション／mutex でコンテナ構造（サイズ・イテレータ）を保護
していても、**要素として格納された ttstr の `RefCount` 操作自体は保護され
ない**点に注意。ロック外で取り出した ttstr を別スレッドが破棄すれば、共有
バッファの二重解放が起きうる。

### 既存の対処（コミット `d5292429`）

`common/base/StorageIntf.cpp` の AutoPath キャッシュ（`TVPAutoPathCache`,
`TVPGetPlacedPath`）で、`TVPMakeIndependentString()` を導入：

```cpp
static inline ttstr TVPMakeIndependentString(const ttstr & s)
{
    if(s.IsEmpty()) return ttstr();
    return ttstr(s.c_str());   // c_str() から作り直し、独立した VS を持たせる
}
```

`c_str()` から作り直すことで COW 共有を切り、スレッド間でバッファを共有させ
ない。これは「暫定対処」であり、根本方針は次節。

## 2. 方針

> **ttstr は「TJS 処理内の一時利用」に留める。データ保持（長寿命の保持・
> コンテナ格納・スレッドをまたぐ受け渡し）には `tjs_string`
> （= `std::u16string` / `std::wstring`、`common/tjs2/tjsTypes.h:164,176`）を
> 使う。**

`tjs_string` は `std::basic_string<tjs_char>` であり COW 共有を行わない（C++11
以降は各インスタンスが独立バッファを持つ）ため、スレッド間で値コピーしても
`RefCount` 競合は起きない。

`TVPMakeIndependentString()` のような「独立化」コピーは暫定策としては有効だが、
データ保持の型そのものを `tjs_string` にするのが本筋。

## 3. 棚卸し結果

リスクは「そのデータ構造をワーカースレッドが触るか」で評価する。本コードベー
スの主なワーカースレッド：

- `tTVPAsyncImageLoader`（画像ロード／prefetch — `GraphicsLoadThread`）
- `tTVPStorageCacheThread`（ストレージキャッシュ — `StorageCache`）
- `SoundDecodeThread` / `SoundEventThread`（サウンド）
- `TimerThread`、`DAPServer`、`REPL`（デバッグ・タイマー）
- イベントキュー（`TVPPostEvent` は各スレッドから呼ばれ、メインスレッドで配信）

凡例: 🔴 高（ワーカースレッドと確実に共有）／🟡 中（条件付きでクロススレッド
の可能性）／🟢 低（基本メインスレッドのみ。方針上は移行対象だが緊急性低）

---

### 3.1 🔴 高リスク（実ファイルで確認済み・最優先）

| # | 箇所 | 型 / 格納先 | クロススレッド根拠 |
|---|---|---|---|
| H1 | `common/visual/GraphicsLoadThread.h:29,31` | `tTVPImageLoadCommand::path_`, `result_`（ttstr メンバ） | `std::queue<tTVPImageLoadCommand*> CommandQueue` / `LoadedQueue`（同 .h:59,61）でメイン⇄ロードワーカー間を受け渡し |
| H2 | `common/visual/GraphicsLoadThread.h:39` | `tTVPImagePrefetchInFlight::path`（ttstr メンバ） | `std::map<ttstr, std::shared_ptr<...>> InFlightTable`（同 .h:67）。メイン（`FindInFlight`/`PrefetchRequest`）とワーカー（完了処理）が共有 |
| H3 | `common/visual/GraphicsLoadThread.h:67` | `InFlightTable` の **キー** が ttstr | 同上。キーの ttstr が COW 共有されたまま別スレッドに渡る |
| H4 | `common/visual/GraphicsLoaderIntf.cpp:1275` | `tTVPGraphicsSearchData::Name`（ttstr メンバ、画像キャッシュキー） | `tTVPGraphicCache`（同 .cpp:1418）。prefetch worker から `TVPGraphicCacheCS` 下で操作されるが、キー ttstr の RefCount は非保護 |
| H5 | `common/visual/GraphicsLoaderIntf.cpp:1313` | `tTVPGraphicImageData::ProvinceName`（ttstr メンバ、キャッシュ値側） | 同上、画像キャッシュのエントリ内 |
| H6 | `common/sound/WaveSegmentQueue.h:44` | `tTVPWaveLabel::Name`（ttstr メンバ） | デコードスレッドが `LabelEventQueue` に push、イベントスレッドが pop して `InvokeLabelEvent`、メインへ伝播。`WaveIntf.cpp:977` に既に `c_str() to avoid race condition for ttstr` のコメントあり（＝既知の競合） |
| H7 | `common/sound/QueueSoundBufferImpl.h:80` | `LabelEventQueue`（`std::vector<tTVPWaveLabel>`） | デコードスレッド書込み（`QueueSoundBufferImpl.cpp:254`）⇄イベントスレッド読出し（同 :365）。`BufferCS` はコンテナのみ保護、要素 ttstr は非保護 |
| H8 | `common/base/StorageCache.h:18` | `LoadRequestItem::name`（ttstr メンバ） | `std::deque<LoadRequestItem> RequestQueue` / `RequestQueueFast`（同 .h:27,29）。メインが `LoadRequest` で投入、キャッシュスレッドが取り出す |
| H9 | `common/base/StorageCache.cpp:131`* | `StorageCacheTable` の **キー** が ttstr（`std::map<ttstr, ...>`） | メインとキャッシュスレッドが同時アクセス。投入された name をそのままキーに使う |
| H10 | `common/base/EventIntf.cpp:34` | `tTVPEvent::EventName`（ttstr メンバ） | `std::vector<tTVPEvent*> TVPEventQueue`（同 :177）。`TVPPostEvent`（同 :231）は各ワーカースレッドから呼ばれ、メインで配信。投入される `eventname` と引数 `tTJSVariant`（ラベル名等の ttstr を内包）が COW 共有されたままキューに残る |

\* H9 の行番号はエージェント報告値。実装時に再確認すること。

---

### 3.2 🟡 中リスク（条件付きクロススレッド・要精査）

| # | 箇所 | 型 / 格納先 | 備考 |
|---|---|---|---|
| M1 | `common/base/EventIntf.h:314` | `tTJSNI_AsyncTrigger::ActionName`（ttstr メンバ） | `Trigger()`（`EventIntf.cpp:1346`）でアクション発火時に参照。非同期発火経路があればクロススレッド |
| M2 | `common/base/StorageIntf.cpp:88,92` | `TVPCacheTargetExtensions` / `TVPDecodeTargetExtensions`（`std::map<ttstr, tjs_uint64>`、キー ttstr） | `d5292429` で AutoPathCache は対処済みだが、この 2 つの拡張子マップは生の ttstr キーのまま |
| M3 | `common/base/StorageIntf.cpp:163` | `TVPPinnedCachePaths`（`std::set<ttstr>`） | `TVPPinnedCachePathsCS` で保護。`find()` 結果を CS 外で使うと共有リスク |
| M4 | `common/base/StorageIntf.cpp:66` | `TVPCurrentMedia`（グローバル ttstr） | `SetCurrentDirectory`（同 :999）で書込み、`NormalizeStorageName`（同 :655）で読出し。CS 保護なし、read/write race の可能性 |
| M5 | `common/base/SysInitIntf.cpp:27,28` | `TVPProjectDir` / `TVPDataPath`（グローバル ttstr） | 初期化フェーズで設定→全体から読出し。読出しでも `c_str()` 経由で RefCount に触れる |
| M6 | `common/base/XP3Archive.h:94,98` | `tTVPXP3Archive::Name` / `tArchiveItem::Name`（ttstr メンバ） | アーカイブはキャッシュ保持され複数スレッドから参照されうる。アクセサが ttstr を返す経路に注意 |
| M7 | `common/base/UtilStreams.h:82,83`（要確認） | `tTVPLocalTempStorageHolder::LocalName` / `LocalFolder` | 通常はローカル生成だが、非同期処理から触る経路があれば中 |
| M8 | `generic/base/PluginImpl.h:119,120` | `tTVPExceptionDesc::type` / `message`（ttstr メンバ） | 例外がスレッド間で伝播する経路があれば危険 |
| M9 | `common/utils/REPL.h:19,26` | `tTVPReplThread::req_script_` / `resp_error_`（ttstr メンバ） | ワーカーが書込み、メインが `DrainMain` で読出し。`req_mtx_`/`resp_mtx_` 保護下でも ttstr の代入で RefCount が動く |
| M10 | `common/utils/TimerIntf.h:43` | `tTJSNI_Timer::ActionName`（ttstr メンバ） | `OnTimer` 発火時に参照（`TimerIntf.cpp:174`）。TimerThread 経路でクロススレッドの可能性 |
| M11 | `common/utils/LogCore.cpp:56-63,71,72` | `tTVPLogItem::Log`/`Time`、`TVPImportantLogs`、`TVPLogLocation`、`TVPLogDeque` | ログは各スレッドから呼ばれる。`TVPLogDispatchLine` 内の `static ttstr prevtimebuf`（同 :371）も複数スレッドから更新されうる |
| M12 | `sdl3/utils/LogImpl.cpp:176` | `TVPLogLocation`（グローバル ttstr） | LogCore との同期が不明確 |
| M13 | `common/visual/LayerBitmapImpl.cpp:133` | `TVPPrerenderedFonts`（`tTJSHashTable<ttstr, tTVPPrerenderedFont*>`、キー ttstr） | フォント追加／描画時にアクセス。描画がワーカー経路を持つなら中 |
| M14 | `common/visual/LayerIntf.h:529` ほか | `tTJSNI_BaseLayer::Hint`、各種イベント構造体（`WindowIntf.h:681` `tTVPOnHintChangeInputEvent::HintMessage` 等） | イベント配信でスレッド境界をまたぐ可能性 |

---

### 3.3 🟢 低リスク（基本メインスレッドのみ・方針上の移行対象）

ワーカースレッドからのアクセスは現状なさそうだが、「データ保持に ttstr を使
っている」という方針上は `tjs_string` 化の対象。緊急性は低い。

| 箇所 | 型 |
|---|---|
| `common/visual/LayerIntf.h:221` | `tTJSNI_BaseLayer::Name`（レイヤ名メンバ） |
| `common/visual/tvpfontstruc.h:26` | `tTVPFont::Face`（フォント名メンバ） |
| `common/visual/PrerenderedFont.h:29` | `tTVPPrerenderedFont::Storage`（メンバ） |
| `common/visual/LayerBitmapImpl.h:231` | `tTVPNativeBaseBitmap::CachedText`（描画テキスト一時キャッシュ） |
| `common/visual/ShaderProgramIntf.h:93` | `tTVPShaderParameter::TjsName`（メンバ） |
| `common/visual/GraphicsLoaderIntf.cpp:154` | `tTJSHashTable<ttstr, tTVPGraphicHandlerType> Hash`（初期化後は読出し専用） |
| `common/visual/TransIntf.cpp:303` | `TVPTransHandlerProviders`（`tTJSHashTable<ttstr, ...>`、登録は初期化時） |
| `generic/visual/WindowImpl.h:59` | `tTJSNI_Window::mCaption`（メンバ） |
| `generic/base/StorageImpl.h:41` | `tTVPPluginHolder::LocalPath`（メンバ） |
| `common/base/TextStream.cpp:28,30` | `DefaultReadEncoding`（static、初期化後読出し専用） |
| `common/base/EventIntf.cpp:907` | `TVPActionName`（グローバル、初期化後読出し専用） |
| 各所の `static ttstr eventname(TJS_W("onXxx"))` | イベント名 static（`LayerIntf.cpp`/`WindowIntf.cpp`/`SoundBufferBaseIntf.cpp:89,112,190`/`WaveIntf.cpp:847` 等多数） |

> 補足: イベント名 static（`static ttstr eventname(...)`）は文字列内容こそ
> 不変だが、`TVPPostEvent` に渡されてキュー要素にコピーされる際、**static の
> VS の `RefCount`** が投入スレッドとメインスレッドで非 atomic に増減する。
> 厳密にはこれも H10 の競合源の一部。低リスク扱いだが、移行時はイベント名
> 配送経路（H10）とセットで検討するのが望ましい。

## 4. 推奨対応の方向性（実装はしない）

優先度順の方針メモ。具体実装は別タスク。

1. **🔴 最優先 — ワーカースレッド共有構造の型を `tjs_string` に変更** ← **実装済み (2026-06-15)**
   - H1〜H3 ✅: `tTVPImageLoadCommand::path_/result_` / `tTVPImagePrefetchInFlight::path`
     / `InFlightTable` のキーを `tjs_string` 化。worker/main 各々で ttstr API 用の
     一時 ttstr に変換。
   - H4〜H5 ✅: `tTVPGraphicsSearchData::Name`（キャッシュキー）/
     `tTVPGraphicImageData::ProvinceName` を `tjs_string` 化。ハッシュは
     `tTJSHashFunc<tjs_char *>::Make(Name.c_str())` で同一アルゴリズムを維持。
   - H8〜H9 ✅: `LoadRequestItem::name`、`StorageCacheTable` のキーを `tjs_string` 化。
   - H6〜H7: `tTVPWaveLabel::Name`。実態は `BufferCS` 下で RefCount 直列化済みの
     旧来設計（§0）。今回は**対象外**（必要なら同様に `tjs_string` 化可能）。
   - H10: §0 の監査どおりイベントキューはメインスレッド限定のため**対応不要**。

2. **🟡 中 — 精査の上で `tjs_string` 化 or CS 保護の追加**
   - M1〜M14。特に M4（`TVPCurrentMedia`）は CS 保護がそもそも無いため、
     型変更と同期の両面で検討。

3. **🟢 低 — 機を見て `tjs_string` へ統一**
   - 3.3 の各メンバ。TJS 値とのやり取り境界でのみ ttstr へ変換する。

### 変換時の注意

- TJS との境界（`tTJSVariant` ⇄ 文字列、プロパティ get/set、イベント引数）
  では引き続き ttstr が必要。`tjs_string` ⇄ ttstr の変換は
  `ttstr(s.c_str())` / `ttstr::AsStdString()` 等の明示変換で行い、保持側は
  `tjs_string` に統一する。
- 暫定策としての `TVPMakeIndependentString()`（独立 VS 化）は型変更が困難な
  箇所のホットフィックスとして有効だが、恒久対策は型を `tjs_string` にする
  こと。

## 5. 関連

- `krkrz/CLAUDE.md` — *Sound codecs and allocator hook*、ストレージ／キャッシュ
- `doc/MemoryDesign.md` — メモリ設計
- コミット `d5292429`（AutoPath キャッシュ暫定対処）、`67d6f1d5`
  （storage/autopath cache synchronization）

## 6. 実装記録 (2026-06-15)

キャッシュ系（後付けスレッドの越境共有）を `tjs_string` 化。境界変換は
`ttstr::AsStdString()`（ttstr→tjs_string）/ `tTJSString(const tjs_string&)` ·
`tjs_string::c_str()`（tjs_string→ttstr）で行い、保持側は独立バッファの
`tjs_string` に統一した。SDL/generic・WINVER 両 variant でビルド＋リンク確認済み。

### H8-H9: ストレージキャッシュ — `common/base/StorageCache.{h,cpp}`
- `LoadRequestItem::name` を `tjs_string` 化。投入時 `name.AsStdString()` で独立化、
  キャッシュスレッドは pop 後に独立 `ttstr` を 1 つ作って各 API へ渡す。
- `StorageCacheTable` を `std::map<tjs_string, StorageCacheEntry>` 化。
  `operator[]`/`find` 引数を `AsStdString()` に。観測用 `TVPStorageCacheEntryInfo::name`
  は ttstr のまま（`ttstr(it.first)` で変換）。`CancelLoadQueue` の比較も tjs_string 化。

### H1-H3: 画像ロード/prefetch — `common/visual/GraphicsLoadThread.{h,cpp}`
- `tTVPImageLoadCommand::path_/result_`、`tTVPImagePrefetchInFlight::path`、
  `InFlightTable` のキーを `tjs_string` 化。
- worker(`LoadImageFromCommand`/`FinalizePrefetchOnWorker`)・main(`HandleLoadedImage`)
  は各々先頭で `ttstr path(cmd->path_)` を作り ttstr API へ渡す。`result_` は
  `.c_str()` 経由で TJSVariant/代入。`InFlightTable.find` はメンバ tjs_string 直で照合。

### H4-H5: 画像デコードキャッシュ — `common/visual/GraphicsLoaderIntf.{h,cpp}`
- キー構造体 `tTVPGraphicsSearchData::Name`、値 `tTVPGraphicImageData::ProvinceName`
  を `tjs_string` 化。`operator==` は std::wstring 同型比較。ハッシュは
  `tTJSHashFunc<tjs_char *>::Make(Name.c_str())`（ttstr 版と同一・空文字は 0）。
- `searchdata.Name = nname.AsStdString()`、ループ内比較は事前に `tjs_string nn` を
  作って照合、観測用 `TVPGraphicCacheEntryInfo::name` は ttstr のまま（`ttstr(k.Name)`）。

### 対象外（実態として安全 — §0 参照）
- H6/H7 サウンドラベル（`BufferCS` 下で RefCount 直列化済みの旧来設計）
- H10 イベントキュー（投入・配送ともメインスレッド限定）
- M2 拡張子マップ / M3 PinnedCachePaths（worker は CS 下 `find` のみ、RefCount 不変）

### 補足: なぜ「型変更」か（独立化との比較）
ttstr 同士のコピーは COW で VS を共有するため、`TVPMakeIndependentString` で
一時的に独立化しても、map/queue に**格納したキー/要素自体**が次の格納元 ttstr と
VS を共有し続け、越境 RefCount 競合が残る。保持型を RefCount を持たない
`tjs_string` にすることで、格納物そのものが独立し競合源が消える（d5292429 の
独立化は AutoPath キャッシュへのホットフィックス、本コミットが恒久対策）。
