# krmovie 動画サブシステム モダン化 (DirectShow → Media Foundation)

WINVER モダン化ロードマップ [ModernizationRoadmap.md](ModernizationRoadmap.md) の **Track V** の
詳細設計 SSOT。DirectShow スタックを撤去し、全対応フォーマットを維持したまま
Media Foundation / movie-player / 軽量 MPEG-1 デコーダへ移行する。

## 背景・現状アーキテクチャ

krmovie は現状 `krmovie.dll`(tp_stub ベースのプラグイン構造)としてビルドされ、
エンジン (`win32/visual/VideoOvlImpl.cpp`) が `LoadLibrary`+`GetProcAddress` で 3 つの
エクスポートを呼ぶ。全プレイヤは共通 IF `iTVPVideoOverlay` を実装する。

| エクスポート | 実装 | 対応形式 | モダン度 |
|---|---|---|---|
| `GetVideoLayerObject`(レイヤ, `.webm`) | `tTVPWebpMovie` (webplayer.cpp + external/movie-player + miniaudio) | VP8/9・**α対応** | ✅ モダン・クロスPF |
| `GetVideoLayerObject`(レイヤ, その他) | `tTVPDSLayerVideo` (dslayerd + BufferRenderer) | .wmv/.mpg/.avi | ❌ DirectShow |
| `GetVideoOverlayObject`(オーバレイ既定) | `tTVPDSVideoOverlay` (dsoverlay + dsmovie) | .wmv/.mpg 等 | ❌ DirectShow |
| `GetMFVideoOverlayObject`(オーバレイ MF) | `tTVPMFPlayer` (MF MediaSession + EVR + 子ウィンドウ) | .mp4/H.264/HEVC | ✅ MF |

モード enum (`common/visual/voMode.h`): `vomOverlay` / `vomLayer` / `vomMixer`(旧VMR9,
現在は MF/EVR に再ルート) / `vomMFEVR`。既定は `vomOverlay`。

ストリームは kirikiri ストレージから供給される。MF 経路は `MFByteStream.cpp`
(`IMFByteStream` 実装) が担う。DirectShow 経路は `CDemuxSource`/`asyncrdr`/`asyncio` の
カスタム非同期リーダが担う。

## ユーザ要件 (2026-07-30 協議で確定)

- **レイヤ動画は多用されている**。α付き=`.webm`、**背景動画(αなし)=`.wmv`/`.mpg`(MPEG-1)**。
  → **全形式のサポート継続が必須**(レガシー .wmv/.mpg を切ってはいけない)。
- カスタム DS コーデック登録 API (`TVPRegisterDSVideoCodec` / `TVPGetDSFilterHandler`) は
  in-tree 未使用・実運用なし → **廃止 OK**。
- krmovie の DLL 分離は歴史的経緯のみ → **exe 統合してよい**(実装が簡単になるなら)。
- 性能で (B) GPU 経路が優れるならやってよい(ただし本命はレガシー確実撤去)。

## 重要な技術的制約

1. **MF は MPEG-1 をデコードできない**。DirectShow は quartz.dll に MPEG-1 スプリッタ+
   デコーダを常備(全 Windows)しているが、Media Foundation には MPEG-1 デコーダ MFT が
   無い(MF が持つのは H.264/HEVC/MPEG-4、MPEG-2 は Pro 版 DVD デコーダのみ)。
   → **`.mpg`(MPEG-1)は MF に載せられない。専用デコーダが必要。**
   → **`pl_mpeg`**(パブリックドメイン単一ヘッダ、MPEG-1 映像 + MP2 音声、MPEG-1 PS 対応)を
     vendoring して代替する(ユーザ承認済)。`.mpg` は MPEG-1 で確定(ユーザ確認済)。
2. **`.wmv`/`.asf` は MF SourceReader が OS 標準の Windows Media コンポーネントで
   ネイティブデコード可**。
3. **レイヤ合成器は CPU ビットマップ (`tTVPBaseBitmap`) ベース**。レイヤ動画のフレームは
   最終的に CPU の Layer バッファ (`SetVideoBuffer` ダブルバッファ) に届く必要がある。
   現行 BufferRenderer も既に CPU 配送なので、MF/pl_mpeg → CPU 配送はパリティ(退化でない)。
   → GPU テクスチャ経路 (B) の旨味はレイヤには出ない。(B) の本命は全画面合成の新モード。

## 移行後アーキテクチャ:フォーマット別ディスパッチ

`GetVideoLayerObject` / `GetVideoOverlayObject` を拡張子(または MF での型判定)で分岐:

| 形式 | 新レイヤ経路 | 新オーバレイ経路 |
|---|---|---|
| `.webm` | `tTVPWebpMovie` (現行維持) | 同左(必要なら) |
| `.wmv`/`.asf`/`.mp4`/`.m4v`/`.mov` | **新 `tTVPMFSourceReaderVideo`**(MF SourceReader → CPU フレーム) | 既存 `tTVPMFPlayer` (EVR) |
| `.mpg`/`.mpeg` (MPEG-1) | **新 `tTVPMpeg1Video`**(pl_mpeg → CPU フレーム) | 同 pl_mpeg をウィンドウへ present |
| `.avi` | MF SourceReader(内部コーデック次第、テストで判定) | 同 MF |

- **レイヤ**は全経路 CPU フレームを Layer バッファへ(現行 BufferRenderer と同契約)。
- **オーバレイ**は MF ネイティブ形式は既存 MFPlayer(EVR/子ウィンドウ)を流用。MPEG-1
  オーバレイは pl_mpeg フレームをウィンドウへ blit / D3D present。

## 撤去対象 (V-D)

**★DirectShow + EVR 撤去 = 2026-07-30 実施完了** (`external/baseclasses` submodule 含む)。
削除済ファイル:
- DirectShow: `dsmovie` / `dslayerd` / `dsoverlay` / `BufferRenderer` / `CDemuxSource` /
  `CDemuxOutputPin` / `CMediaSeekingProxy` / `asyncio` / `asyncrdr` / `CWMAllocator` /
  `CWMBuffer` / `CWMReader` / `CIStream` / `IBufferRenderer*` / `IRendererBuffer*` /
  `IDemuxReader` / `idl/` / `OptionInfo.h`
- EVR/MediaSession: `MFPlayer` / `MFByteStream` / `AsyncCB.h` / `PlayWindow` /
  `CDLLLoader.h` / `DShowException` (EVR 廃止の経緯は「オーバレイ・ルート統一 → 実施結果」)
- `external/baseclasses` submodule (`.gitmodules` から削除)
- CMakeLists.txt から baseclasses ライブラリ定義 + link + strmiids/quartz を除去

**残タスク (未実施)**: カスタムコーデック API `TVPRegisterDSVideoCodec` /
`TVPGetDSFilterHandler` (`TVPVideoOverlay.cpp`, exe 側にリンク) + tp_stub エクスポート
(`FuncStubs.cpp` / `tp_stub.h`)。in-tree 呼び出しは DS 削除で消滅済 (未使用) だが、
撤去には tp_stub 再生成 or 手動スタブ編集が要る (別途・要注意)。今は無害な未使用登録として残置。

## 段階 (推奨順)

- **V-B オーバレイ MF 既定化**: 既定 `vomOverlay` を MF ネイティブ形式について MFPlayer へ。
  `dsoverlay` + `dsmovie` のオーバレイ経路を退役。
- **V-C レイヤ MF/pl_mpeg 化(最重要・多用経路)**: `tTVPMFSourceReaderVideo`(wmv/mp4)と
  `tTVPMpeg1Video`(pl_mpeg for mpg)を新設し `GetVideoLayerObject` の非 webm 分岐を差し替え。
  `BufferRenderer`/`dslayerd`/`CDemux*` を退役。フレーム配送契約(`SetVideoBuffer`
  ダブルバッファ・ピクセル形式・上下方向・ピッチ)を現行と厳密一致させる。
- **V-D DirectShow 全撤去**: 上記ファイル群 + baseclasses + カスタムコーデック API を削除。
  **同時にオーバレイのルート整理を実施 (2026-07-30 ユーザ確定)** — 下記「オーバレイ・
  ルート統一」参照。DirectShow 撤去で avi の行き先と wmv→EVR 寄せが確定するので、
  ルート整理は単独ではなく V-D と同時に行う (中途半端な移行状態を避ける)。
- **V-A exe 統合 ✅完了 (2026-07-30)**: 減量後の krmovie(movie-player + MF SourceReader +
  pl_mpeg + D3D11 のみ)を **STATIC ライブラリ化して exe へ静的統合**。
  - `win32/movie/CMakeLists.txt`: `SHARED`→`STATIC`。`sound_alloc_stub.c` 撤去 (exe の
    `SoundAllocator.cpp` が sound_malloc 等を提供、静的リンクだと二重定義になるため)。
    `krmovie.rc`/`resource.h`/`krmovie.def` 撤去。
  - コーデック (ogg/vorbis/opus/vpx/yuv) は exe と同一 CMake ターゲットのため重複しない。
    `tp_stub.cpp` は `namespace krkrz_plugin` で exe の実 TVP* と衝突せず、krmovie 内に保持
    (V2Link で import pointer 初期化)。
  - `krmovie.h` の `EXPORT` を `krmovie_EXPORTS` (DLL 時のみ CMake 定義) でゲート → 静的時は
    dllexport しない通常関数。`krmovie.cpp` の `/EXPORT` pragma も同ゲート。DllMain/リソース
    版 About 撤去。
  - `VideoOvlImpl.cpp`: `tTVPVideoModule` に静的バインド用コンストラクタを追加
    (LoadLibrary/GetProcAddress を経ず krmovie のエクスポート関数を直接束ねる)。
    krflash.dll は従来通り DLL ロード経路を残置。exe の `target_link_libraries` に `krmovie`
    追加、`install(TARGETS krmovie)` 撤去。
  - **検証**: krmovie.dll を exe 隣から除去した状態で overlay 4形式 + layer 再生を確認
    (DLL 非依存 = 真の統合)。
- **V-A' de-プラグイン化 (tp_stub 境界撤去) ✅完了 (2026-08-02)**: V-A の STATIC ライブラリ +
  tp_stub 間接層は「exe 統合済みなのに engine を関数ポインタ表越しに呼ぶ」半端な状態だった
  ため、境界ごと撤去して engine の実シンボルを直接参照する形に整理。
  - `win32/movie/*.cpp` を独立 STATIC ターゲットから **`KRKRZ_SRC_WIN32` に畳み込み**
    (`sources.cmake`)、`win32/movie/CMakeLists.txt` を削除。動画デコーダの
    `external/movie-player` (webm) だけ static lib として取り込み `${EXENAME}` へ PRIVATE
    リンク (`pl_mpeg` はヘッダオンリで `KRKRZ_INC_WIN32` に追加)。`xaudio2` も exe へ直リンク
    (この時点では動画音声が XAudio2 のため。V-A'' で miniaudio 化し撤去)。
  - 各 `.cpp` の `#include "tp_stub.h"` を engine 実ヘッダ (`tjsCommHead.h` + `MsgIntf.h`) へ
    差し替え。krmovie が engine から使う API は実質 `TVPThrowExceptionMessage` のみで、
    リンクは exe 内実シンボルへ直接解決される。`tp_stub.cpp` / `V2Link` / `V2Unlink` /
    `TVPInitImportStub` / `GetAPIVersion` / `/EXPORT` pragma を全撤去。
  - `VideoOvlImpl.cpp`: `tTVPVideoModule` を「エントリ関数を束ねる薄いラッパ」に簡約
    (Holder/Handle/V2Link/version check/DLL ローダを撤去)。
  - **krflash.dll (旧 `.swf` 対応) を廃止**: DLL ロード用コンストラクタ・`.swf` 分岐・
    `TVPGetFlashVideoModule` を撤去。
  - **検証 (WINVER 実機)**: webm/mpg/mp4/wmv + alpha.webm を overlay(presenter/HW MediaEngine)
    / layer で captureScreen 目視、全形式で正常描画・エラー無しを確認。
- **V-A'' 動画音声を miniaudio へ統合 ✅完了 (2026-08-02)**: 境界撤去で `common/sound` の
  `TVPCreateAudioStream` / `iTVPAudioStream` を krmovie から直接使えるようになったため、
  WINVER の動画音声だけ残っていた **XAudio2 を廃止し miniaudio に統合** (generic/SDL/Android は
  既に統合済み)。engine 単一 miniaudio エンジンを共有 (マスタ音量 / `TVPSoundChannels` /
  SoundAllocator プール / 3D 定位と同じ土俵)。
  - **webplayer (webm, movie-player 駆動)**: `WebpXAudio2Sink` を廃止し、generic と同じ common の
    `tTVPMovieAudioSinkAdapter` (IAudioSink, borrow 意味論) を使用。`WebpXAudio2Sink.h` 削除。
  - **layer 経路 (MF SourceReader / pl_mpeg / webm-layer)**: `tTVPMovieAudioSink` を
    `iTVPAudioStream` 上に再実装 (`MovieAudioSink.h`)。デコーダが渡す PCM は借り物なので
    **内部コピー + フリーリスト再利用**し、`QueuedBuffers()` バックプレッシャと `GetPlayedMs()`
    マスタクロックを維持 (公開 API 不変 → LayerVideoBase/MF/Mpeg backend は無改造)。
  - `xaudio2` リンクを撤去。★アンダーラン時は `MiniAudioStream::ReadData` がゼロ埋め (無音) する
    ので停止/供給途切れで buzz は出ない。
  - **検証 (WINVER 実機)**: 全形式再生・エラー無し。layer 経路は音声クロックに映像を同期する
    ため「フレーム進行」で音声デバイス稼働を非可聴確認 (mp4/MediaEngine 再生の前後とも生存)。
    ★mp4/wmv **overlay は HW=IMFMediaEngine が音声も内部処理**するので本統合の対象外
    (このシンクを経由しない)。音の実聴 A/V 同期はユーザ確認事項。
- **V-E (任意) 新モード `vomD3D11`**: exe 統合後、`IMFMediaEngine` フレームサーバ +
  `TransferVideoFrame` で BasicDrawDevice の D3D11 へ直接 present(全画面合成・HDR・
  HEVC/AV1 の GPU 全経路)。(B) の本命。

## V-A / V-E: DrawDevice presenter インターフェース (2026-07-30 実装完了)

overlay 動画を子ウィンドウの独立 D3D11 デバイスではなく、エンジン (`BasicDrawDevice`)
の単一 D3D11 デバイス上でバックバッファへ **pull 型合成** する仕組み。mixer 追加画像の
復活も兼ねる。**スコープは WINVER のみ。Generic(SDL) は別計画** (末尾参照)。

### 2 インターフェース構成 (`win32/visual/VideoPresenter.h`)
- **`iTVPVideoPresenter`** … overlay 動画側 (`tTJSNI_VideoOverlay`) が実装する描画
  コールバック。`bool RenderVideoFrame(const tTVPVideoPresenterContext&)`。DrawDevice の
  `Show()`(描画スレッド) から毎フレーム呼ばれ、最新フレーム + mixer 追加画像を ctx の
  engine dev/ctx/RTV へ描く。
- **`iTVPVideoPresenterHost`** … DrawDevice 側 (`BasicDrawDevice`) が実装する登録口。
  `Add/RemoveVideoPresenter(iTVPVideoPresenter*)`。単一スロット (最後に登録した 1 つを
  保持。Remove は現在のスロットが自分の時だけクリア)。

### 能力判定 = TJS プロパティ経由 (C++ vtable を変えない=ABI 安全)
`iTVPDrawDevice` に仮想メソッドを足さず、**DrawDevice の TJS オブジェクトの読み取り専用
プロパティ `videoPresenterHost`** が host ポインタ (`static_cast` 済み) を `tjs_int64` で
返す (`tTJSNC_BasicDrawDevice`。既存 `interface` プロパティと同じ手口)。VideoOverlay は
`Window->GetDrawDeviceObject()` からこのプロパティを PropGet し、非 0 なら presenter 経路、
0/未定義 (OGL/Null/custom 等) なら従来の子ウィンドウ present にフォールバックする。

### pull 型 + スレッド境界 (設計の要)
`BasicDrawDevice` は単一 D3D11 デバイスで ImmediateContext は描画スレッド専用。デコード
スレッドから触ると壊れるので **「push」でなく「描画スレッド(`Show()`)が pull」**。VideoOverlay は
Play で host に登録するだけ。デコードスレッドは CPU バッファ (レイヤと同じ `SetVideoBuffer`
ダブルバッファ) へ書き、`Show()` 内の `RenderVideoFrame` が最新 front buffer を engine の
D3D11 テクスチャへ upload → 描画する。

### モード整理
- 実モードは `vomLayer` / `vomOverlay` の 2 つ。`vomMixer` / `vomMFEVR` は TJS 定数 (値 2/3) を
  互換のため残すが挙動は `vomOverlay` に統合 (旧 EVR 依存の `Mode != vomMFEVR` status 抑止を
  撤去、Open 分岐を collapse)。追加画像合成は「モード」でなく overlay の機能に格上げ。

### visible の扱い (全環境共通仕様)
`VideoOverlay.visible` は **映像表示のオン/オフのみ**を制御する (既定 **false**)。再生 (デコード + 音声)
は visible に依らず継続するので、**`visible=false` で `play()` すると「音は鳴るが映像は出ずゲーム画面のまま」**
になる (音も止めるなら `stop()`/`pause()`)。これは吉里吉里2/Z 以来の仕様で、旧 DirectShow/子ウィンドウ
時代 (`VideoOverlay->SetVisible(Visible)` で overlay 窓の表示制御) から一貫している。
- **overlay/mixer** (`vomOverlay`/`vomMixer`): WINVER は `RenderVideoFrame` 内で `!Visible` なら描画しない
  (ゲーム backbuffer に合成しない=ゲーム画面が見える)。**visible=true が必須**。
- **vomLayer**: `Visible` を対象 Layer(`layer1`/`layer2`) の visible へ伝播 (レイヤ自身の表示で制御)。
- **Generic(SDL/GL)** も同仕様に統一済 (下記「Generic(SDL) 側」参照)。

### 動作 (`tTJSNI_VideoOverlay`)
- **Open**: overlay かつ host 有り → `GetVideoLayerObject` (buffer 出力) + `Bitmap[0/1]` 確保 +
  `SetVideoBuffer` (レイヤと同経路)。host 無し → 従来 `GetVideoOverlayObject` (子ウィンドウ)。
- **Play/Stop**: `RegisterPresenter()` / `UnregisterPresenter()`。Close/Shutdown でも解除 + GPU
  リソース解放。
- **RenderVideoFrame**: front buffer (ボトムアップ。ScanLine0 と符号付きピッチで top-down 化) を
  BGRA テクスチャへ upload し、**ゲーム画面領域 `DestRect` 全面**へ `MovieAlpha` でアルファ合成
  (動画が画面を覆う前提=SDL 版 DrawDevice と同様、本体レイヤ描画はスキップ)。続けて mixer 画像
  (`setMixingLayer` のスナップショット) を screen 座標→client にスケールして上へ重ねる。
- ブリッタ = `win32/visual/VideoPresenterD3D.{h,cpp}` (`tTVPVideoPresenterD3D`)。engine デバイス上に
  遅延生成する BGRA クアッド + 全体アルファ定数 + SrcAlpha ブレンド。動画用/mixer 用に各 1 インスタンス。

### BasicDrawDevice
- `iTVPVideoPresenterHost` を多重継承。`Show()`: presenter 登録中は毎フレーム present を強制し、
  RTV を黒クリア (レターボックス余白は黒) → `RenderVideoPresenters()` が presenter を呼ぶ
  (本体レイヤ合成 `DrawCompositedFrame()` は呼ばない=動画が全画面)。`DrawCompositedFrame()` は
  `EndBitmapCompletion` の合成描画本体を切り出したもの (非動画時はこちら)。
- **検証補助**: presenter 稼働時は CPU シャドウに動画が載らないので、描画直後・Present 前に
  バックバッファを staging へ読み戻す `FulfillScreenCaptureFromBackBuffer()` を追加。これで
  `System.captureScreen` が overlay 込みの実画面を PNG 保存できる (swapchain=B8G8R8A8_UNORM)。

### 検証 (実機 REPL + captureScreen, 2026-07-30)
`videoPresenterHost` が非 0 を返すこと、overlay 4 形式 (wmv=MF SourceReader / mp4=H.264 MF /
webm=movie-player / mpg=pl_mpeg) が presenter 経路で色・向き・全画面正常、position が音声クロック
駆動で前進、close/reopen 反復でクラッシュ無し、mixer 追加画像 (半透明ボックス+文字) が動画上に
アルファ合成、を backbuffer キャプチャで確認。

### 既知の割り切り / 残
- presenter 経路の overlay は **全画面固定** (overlay の position/size は視覚に反映しない。動画が
  画面を覆う前提のため)。サブ矩形 overlay が要るなら将来 Rect マッピングを足す。
- **tp_stub 公開は保留**: 2 IF は D3D11 型を含み共有 tp_stub.h に載せると generic plugin に不都合、
  かつ現状 in-tree 消費者なし。custom DrawDevice/video plugin が参加するなら `VideoPresenter.h` を
  直接 include する運用 (host ポインタは TJS プロパティで取得可)。必要になれば WIN ガードで公開する。
- `voMode.h` はコメントのみ変更 (enum 値不変) なので tp_stub.h の同 enum は再生成不要 (値一致)。

### Generic(SDL) 側 (別計画として実施済)
SDL/OGL も同じ pull 型 presenter 構造へ統一済 (DrawDevice 直メソッド `UpdateVideo`/`ClearVideo` を
撤去し、`Show()` から presenter を pull)。presenter インターフェースは環境別
(SDL=`iTVPSDLVideoPresenter`(SDL_Renderer)、OGL=`iTVPGLVideoPresenter`(GLTextureDrawer))、
generic 側 VideoOverlay からは中立 IF (`common/visual/VideoOverlayPresenter.h`) 経由。overlay 動画は
YUV(I420) plane 直渡し + presenter 側 GPU YUV→RGB に対応 (**SDL** presenter=`SDL_UpdateYUVTexture` 内蔵、
**GL** presenter=自前 YUV→RGB シェーダ (BT.601, 3×R8 テクスチャ, `GLShaderUtil::CompileProgram`。既定 SDL
DrawDevice は GL 版なのでこれが実経路))。詳細は「Generic 動画 presenter 統一」計画。
- **coded 幅クロップ (緑帯回避)**: movie-player の I420 frame.width は coded (16 アライン padding) なので
  `generic/app/movie.cpp` の plane callback で `GetVideoFormat` の表示寸法へクロップ (plane stride は保持)。
  crop しないと右端に未定義 chroma 由来の緑帯が出る (GL/SDL 両 presenter 共通の中央修正)。
- **visible 尊重 (WINVER 整合, 2026-08-01)**: 統一初期の generic presenter は `visible` を見ておらず、
  未設定 (既定 false) でも overlay 動画が表示されていた (WINVER と乖離)。中立 IF に `SetVisible()`、
  環境別 IF (`iTVPSDLVideoPresenter`/`iTVPGLVideoPresenter`) に `IsVisible()` を追加し、各 DrawDevice の
  `ShowVideo()` が非表示なら false を返して**通常のゲーム描画へフォールバック**するようにした
  (WINVER が `RenderVideoFrame` 内で Visible を毎フレーム判定するのと等価。SDL は「presenter 登録中は
  動画が画面を占有」する設計なので、判定は presenter を pull する手前=`ShowVideo()` で行う)。対象 3 デバイス:
  `SDLOGLDrawDevice`(既定 GL)/`SDLDrawDevice`(SDL_Renderer)/`OGLDrawDevice`。`presenter->mVisible` は
  `std::atomic<bool>` (描画スレッドが読む)。generic `VideoOvlImpl` は presenter 生成時と `SetVisible` 時に
  `Visible` を presenter へ伝播する。**検証 (SDL x64-windows, 既定 GL, REPL+captureScreen)**: 再生中に
  `visible=false`→ゲーム画面 (音のみ)、`true`→動画全画面、再度 `false`→ゲーム復帰、のトグルが正常。

**MPEG-1 (`.mpg`/`.mpeg`) — generic/SDL でも内蔵対応**: `external/movie-player` は webm 専用のため、
WIN 版と同じ `pl_mpeg` (パブリックドメイン単一ヘッダ) で MPEG-1 を再生する
`generic/app/Mpeg1MoviePlayer.cpp` (`iTVPMoviePlayer` 実装) を新設。`generic/app/movie.cpp` の
`TVPCreateMoviePlayer` が拡張子で振り分ける (`.mpg`/`.mpeg`→pl_mpeg、それ以外→movie-player)。
pl_mpeg は I420 native なので YUV presenter 経路にそのまま載る (CPU 変換なし)。音声 (MP2) は
generic 共通の miniaudio シンクアダプタ (`tTVPMovieAudioSinkAdapter`) へ出力し、その再生済み
サンプル数を A/V 同期のマスタクロックに使う (WIN 版 `LayerVideoBase` と同方式。音声なしは
フレーム間 delta)。**検証 (SDL x64-windows, REPL+captureScreen)**: MPEG-1 PS を vomMixer 再生 →
YUV テクスチャで色・向き正常、position 前進 (実時間ペース)、stop でゲーム画面復帰。

**SDL 動画 mode を WINVER と整合 + mixer 追加画像の状況**:
- **mode 整合**: generic の `SetMode` は従来「非 vomLayer は全て vomMixer に丸める」だったが、WINVER の
  `isOverlay = (Mode != vomLayer)` に合わせ、`vomOverlay(0)`/`vomMixer(2)`/`vomMFEVR(3)` を全て overlay
  presenter 経路として扱う (SDL は HW/EVR 経路が無いので同一挙動。`vomMFEVR` は `vomOverlay` へ丸め)。
  `IsMixerPlaying()` と Open の decode 分岐の `Mode == vomMixer` 判定を `Mode != vomLayer` へ変更。既定は
  `vomOverlay`。これで `vomOverlay` 指定でも presenter で再生される (従来は vomMixer に矯正されていた)。
- **mixer 追加画像 (`setMixingLayer` 等) を generic/SDL でも実装済**: 中立 presenter IF
  (`VideoOverlayPresenter.h`) に `SetMixerImage`/`SetMixerAlpha`/`ClearMixerImage` を追加 (既定 no-op)。
  generic VideoOvlImpl の `SetMixingLayer` がレイヤ画像を BGRA スナップショット (プライマリ座標の矩形 +
  opacity) して presenter へ渡す (WINVER の presenter mixer と同構造)。presenter は動画描画後に mixer
  テクスチャを作り、プライマリ座標 → `DestRect` へマップして α 合成で上へ描く。**SDL presenter** は
  `SDL_RenderTexture`+`SDL_SetTextureAlphaModFloat`、**GL presenter** は mixer GLTexture (α を pixel×全体
  α で焼く)+`GLTextureDrawer::DrawTexture(blend=true)`。GL の描画先矩形は `ctx.DestRect`/`SrcSize` から
  clip 座標へ変換 (Y は本体描画と同じく反転、texture の上下も頂点 UV で整合)。context (SDL/GL とも)
  に `DestRect`/`SrcWidth`/`SrcHeight` を追加し DrawDevice が `GetSrcSize()` で埋める。**検証 (SDL 既定
  =GL presenter, REPL+captureScreen)**: vomMixer 動画の上に赤箱+黄文字レイヤが正位置・正立で α 合成表示、
  stop/close で mixer も消えてゲーム復帰。※既定 SDL DrawDevice は GL 版 (`glVideoPresenterHost`)。純
  SDL_Renderer 版 presenter (`sdlVideoPresenterHost`) にも同等実装 (起動時オプションで選択時に有効)。

### WINVER presenter の YUV(I420) 対応 (Phase 4c 部分)
overlay presenter 経路 (`BasicDrawDevice` の D3D11 バックバッファへ pull 合成) で、**I420 対応形式は
GPU で YUV→RGB する**ようにした (従来は全形式 CPU で BGRA 変換してから D3D11 DYNAMIC へ upload)。
- **ブリッタ** `tTVPVideoPresenterD3D` に `RenderI420()` を追加 (Y/U/V を R8 テクスチャ 3 枚へ upload +
  BT.601 limited-range YUV→RGB PS。VS/IL/VB/CB/Sampler/Blend は既存 BGRA 経路と共用)。
- **フレーム供給**: krmovie 内部 IF `iTVPVideoOverlay` に非 pure の `GetI420Frame()` を追加 (既定 false =
  従来 BGRA)。webm (`tTVPWebpMovie`) に `preferI420` モードを追加し、presenter 経路では I420(COLOR_NOCONV)
  でデコード→plane を内部 packed バッファに front/back 二重で保持 (描画スレッド安全)。新 factory
  `GetVideoPresenterObject` (krlmovie.cpp) が webm を preferI420 で開き、非対応形式は通常レイヤ(BGRA)へ委譲。
- **VideoOvlImpl**: presenter 経路は `GetVideoPresenterObject` を使い、`RenderVideoFrame` で `GetI420Frame`
  成功時は `RenderI420`、失敗時は従来の BGRA (`GetFrontBuffer`→`Render`) にフォールバック。
- **対象**: **webm + mpg**。webm=movie-player の I420 native (`tTVPWebpMovie` preferI420)。
  mpg=pl_mpeg の I420 native → `tTVPLayerVideoBase` に **preferI420 バッファモード**を追加
  (overlay/layer とは別に I420 を内部 front/back 二重保持し `GetI420Frame` で供給。バックエンドは
  `DecoderGetI420Planes` を実装、`tTVPMpeg1Video` が pl_mpeg の y/cb/cr を返す)。`GetVideoPresenterObject`
  が mpg も preferI420 で開く。**MF SourceReader(wmv/mp4 CPU) は YUV 未対応 (BGRA のまま)**、
  HW MediaEngine は元々 GPU なので YUV 化不要。
- **検証 (WINVER x64-windows-win, REPL+System.captureScreen)**: webm/mpg を `vomOverlay` で再生 →
  D3D11 YUV シェーダで色・向き正常にフルスクリーン描画、position 前進、BGRA 経路 (vomLayer) と無回帰。
  ※冒頭の黒フェードイン数秒は Y=16 の黒フレームで正常。

## HW 動画 (IMFMediaEngine フレームサーバ) — Track V-E 本命 (2026-07-30 実装完了)

overlay 動画の **真の HW デコード**。MF-native 形式 (mp4/H.264/HEVC/wmv/asf/…) を
`IMFMediaEngine` で HW デコードし、engine (BasicDrawDevice) の D3D11 デバイスへ
`TransferVideoFrame` で直接転送して present する。デコード・YUV→RGB・スケールを全て GPU で
行う (CPU デコーダ経路と対照的)。**MF-native 形式は既定でこの HW 経路**、`webm`(VP8/9) /
`mpg`(MPEG-1) は MF デコーダが無いので **CPU 経路のまま**。

### アーキテクチャ (デバイス共有方式)
- **engine の D3D11 デバイスを共有**する: `BasicDrawDevice::CreateD3DDevice` に
  `D3D11_CREATE_DEVICE_VIDEO_SUPPORT` を追加 (純加算)、生成直後に `ID3D11Multithread::
  SetMultithreadProtected(TRUE)` を有効化 (MediaEngine のデコードスレッドが共有デバイスを
  触るため)。別デバイス + 共有テクスチャ (keyed mutex) より単純で、既存描画への影響が小さい。
- MediaEngine の DXGI マネージャ (`MFCreateDXGIDeviceManager` + `ResetDevice`) を engine
  デバイスに束ねる → `TransferVideoFrame` は engine デバイス上のテクスチャ (RT+SRV) へ直接
  書ける。クロスデバイス共有は不要。
- engine デバイスは DrawDevice の TJS プロパティ **`d3d11Device`** (ID3D11Device* を
  `tjs_int64`) で公開 (`videoPresenterHost` と同手口)。VideoOverlay が Open 時に取得。

### 実装 (`win32/movie/MediaEngineVideo.{h,cpp}` = `tTVPMediaEngineVideo`)
- **`iTVPVideoOverlay` + `iTVPVideoPresenter` を多重実装**。tTJSNI_VideoOverlay は Open で
  HW オブジェクトを生成し、その presenter を host に登録する (CPU 経路の「自分を登録」と対照)。
- Open: `MFStartup` → DXGI マネージャ → `CLSID_MFMediaEngineClassFactory` (CoCreateInstance) →
  属性 (`MF_MEDIA_ENGINE_DXGI_MANAGER` / `_CALLBACK` / `_VIDEO_OUTPUT_FORMAT`=B8G8R8A8) →
  `CreateInstance`(フレームサーバ) → `MFCreateMFByteStreamOnStream`(kirikiri IStream) →
  `IMFMediaEngineEx::SetSourceFromByteStream`。非同期ロード、`LOADEDMETADATA` で
  `GetNativeVideoSize` → Ready。
- RenderVideoFrame(ctx): `OnVideoStreamTick` で新フレーム時に `TransferVideoFrame` → engine
  デバイス上の宛先テクスチャへ HW 転送 → `tTVPVideoPresenterD3D::RenderSRV` (CPU upload 無しで
  既存 GPU テクスチャの SRV を描く新メソッド) で全画面 present。
- 制御 (Play/Stop/Pause/position/SetPlayRate/volume) を MediaEngine API へマップ。**音声・A/V
  同期は MediaEngine が内部で担う** (CPU 経路の `MovieAudioSink` は使わない)。終端は
  `MF_MEDIA_ENGINE_EVENT_ENDED` → `EC_COMPLETE` 疑似通知でループ/停止判定。
- ファクトリ `GetMediaEngineVideoObject`(extern "C"、engine device を void* で受ける) を exe へ
  静的リンクし VideoOvlImpl.cpp から直呼び。krmovie に `__WINVER__` 定義 + common include を追加
  (VideoPresenter.h 参照のため)。lib は mfplat/mfuuid のみ (`mfmediaengine.lib` は存在しない、
  MediaEngine は CoCreateInstance 生成)。

### ディスパッチ (`tTJSNI_VideoOverlay::Open`)
overlay かつ **`Mode != vomMixer`** + host 有り + `TVPUseMediaEngine()` (既定 true) +
`TVPIsMediaEngineFormat(ext)` (mp4/m4v/mov/wmv/asf) + `d3d11Device` 取得可 → HW。生成失敗/
非対象 → CPU presenter (buffer 出力) or 子ウィンドウ。**CLI `-mediaengine=no`/`off`/`false`/`0`
で HW 無効化** → 全形式 CPU 経路へ。

### vomMixer = mixer 用 CPU 固定 (2026-07-30 ユーザ確定)
HW 経路は mixer 追加画像 (`setMixingLayer`) を**描画しない** (動画側オブジェクトが presenter を
持ち、mixer を合成する engine 側 `tTJSNI_VideoOverlay::RenderVideoFrame` が呼ばれないため)。
mixer が必要な場合は **`vomMixer` を明示指定**すると、HW を使わず必ず CPU presenter 経路になり
mixer が確実に合成される。この経路は音声も**自前処理 (MovieAudioSink → miniaudio)** なので
overlay の音量制御が engine 統合で効く。契約: `vomOverlay`(既定)=HW デコード優先 (mixer 無)、
`vomMixer`=CPU 合成 (mixer + 自前音声)。HW 経路への mixer 直接描画は将来検討 (中規模)。

### 検証 (WINVER REPL + captureScreen)
- mp4(H.264 1920x1080) / wmv を HW で色・向き・全画面正常、position 前進 (MediaEngine クロック
  駆動)、**過去の DXVA ハング再現なし** (共有デバイス + multithread 保護で open 即完了)。
- `-mediaengine=no` で mp4 が CPU (MF SourceReader、GetVideoSize 同期取得=1920x1080 即返し) へ
  フォールバックし正常再生。webm(CPU) 無回帰。
- **音の実聴・A/V 同期の詰めは要ユーザ確認** (MediaEngine 内部音声出力)。

### 割り切り / 残
- HW overlay も全画面固定 (CPU presenter と同じ)。frame 数/FPS は MediaEngine が非公開のため
  30fps 仮定の概算 (`onFrameUpdate` 相当のフレームイベントは HW では未発火。EC_COMPLETE のみ)。
- avi は MF コーデック次第で不安定なので HW 対象外 (CPU MF SourceReader)。HEVC/AV1/HDR は
  MediaEngine が OS デコーダを使うので理論上再生可 (要 OS コーデック)。

## オーバレイ・ルート統一 (V-D と同時実施予定・2026-07-30 ユーザ確定)

**課題**: 現状オーバレイは *mode 指定* で経路が分かれ、形式によって使えるモードが違う。
- `vomOverlay`(0) → `GetVideoOverlayObject` → webm/mpg=新D3D11、wmv=DirectShow、
  **mp4=不可** (`ParseVideoType` が mp4 を持たず "Unknown video format extension." を投げる。
  旧来から mp4 overlay は DirectShow 非対応で EVR 専用だった)。
- `vomMixer`(2)/`vomMFEVR`(3) → `GetMFVideoOverlayObject` → 全部 EVR(`tTVPMFPlayer`)。
  webm/mpg を EVR に渡すと再生不可 (MF はデコーダ非搭載)。

**あるべき姿 (ユーザ要望「指定はどちらでも統一ならそのまま動くように」)**:
mode に依らず **形式でルート**し、どのモードを指定しても全形式が再生できる。

**実施結果 (2026-07-30 完了。当初の EVR ハイブリッド案から変更)**:
統一ディスパッチャ `TVPGetOverlayVideoObject(callbackwin, stream, streamname, type,
size, out)` を新設し、形式でルート:
  - `.webm` → `tTVPWebpMovie(overlay)`        [movie-player + D3D11 YUV present]
  - `.mpg`/`.mpeg` → `tTVPMpeg1Video(overlay)` [pl_mpeg + D3D11 YUV present]
  - それ以外 (`.wmv`/`.asf`/`.mp4`/`.m4v`/`.mov`/`.avi`/未知) →
    `tTVPMFSourceReaderVideo(overlay)` [**MF SourceReader → BGRA → D3D11 present**]
`GetVideoOverlayObject`(mode0) と `GetMFVideoOverlayObject`(mode2/3) の両方をこの
ディスパッチャに委譲 → **mode 非依存**で全形式再生。

**★当初案 (mp4/wmv=EVR 維持) からの変更理由 = EVR 廃止**:
ルート統一で wmv を EVR(`tTVPMFPlayer`/MediaSession) に寄せたところ、**別動画 (webm 等)
の停止直後に開くと EVR の source 解決が間欠的に失敗** (`0xC00D36C4`
MF_E_UNSUPPORTED_BYTESTREAM_TYPE / `0x80070006` E_HANDLE) する teardown レースが判明。
EVR/MediaSession は他フォーマットの子ウィンドウ D3D11 present と別系統で脆かった。
そこで **overlay の wmv/mp4/avi も layer と同じ `tTVPMFSourceReaderVideo` (SourceReader
→ RGB32/BGRA CPU デコード) に寄せ、`tTVPD3D11OverlayWindow::PresentBGRA()` で present**
することにした (layer の SourceReader は実績があり安定)。結果:
- overlay は全形式が **単一の D3D11 子ウィンドウ present 経路**に統一 (webm/mpg=YUV
  シェーダ、wmv/mp4/avi=BGRA パススルーシェーダ)。teardown レース解消。
- **EVR(`tTVPMFPlayer`) / `MFByteStream` / `PlayWindow` / `DShowException` / baseclasses
  を全撤去可能に** (V-D step2b を同時達成)。`GetMFVideoOverlayObject` は krmovie.cpp で
  ディスパッチャに委譲するだけの薄いエクスポートとして残す (エンジンの mode2/3 互換)。

**検証 (実機)**: overlay 4形式 × mode 0/3 = 8/8 open+pos前進、webm→wmv 連続 6/6 (旧EVR
で頻発した teardown レース解消)、wmv overlay BGRA を dump で色/向き正常確認。layer 4形式
も回帰なし。

## V-B オーバレイ (overlay/mixer) の方針 (2026-07-30 ユーザ協議で確定)

**目標: 全形式 × 全方式** (webm/wmv/mp4/mpg × layer/overlay)。

> ⚠ **この節の「EVR ハイブリッド」案は最終的に不採用。** EVR の teardown レースにより
> overlay は全形式 CPU デコード + D3D11 子ウィンドウ present に統一した (上の
> 「オーバレイ・ルート統一 → 実施結果」参照)。以下は当初検討の記録。

**当初案 (不採用): overlay ハイブリッド — EVR が扱える mp4/wmv は EVR 維持、EVR が
扱えない webm/mpg だけ CPU 子ウィンドウ present。**

| overlay 形式 | 当初案の経路 | 最終 |
|---|---|---|
| mp4 / wmv / asf / m4v / mov | ~~MFPlayer (EVR)~~ | **MF SourceReader + D3D11 (BGRA)** |
| webm | CPU 子ウィンドウ present (movie-player) | movie-player + D3D11 (YUV) |
| mpg (MPEG-1) | CPU 子ウィンドウ present (pl_mpeg) | pl_mpeg + D3D11 (YUV) |
| legacy (.avi 等) | 当面 DirectShow | **MF SourceReader + D3D11 (BGRA)** |

overlay と layer の違いは出力先だけ (子ウィンドウ vs レイヤビットマップ) でデコードは
同じなので、CPU overlay は layer 用の共有バックエンドをそのまま使い、出力だけ
「子ウィンドウへ CPU フレーム present」にする。EVR は全廃せず mp4/wmv 用に残すので、
DirectShow overlay (dsoverlay/dsmovie) の退役が主目的 (wmv→EVR, mpg→CPU に移す)。

**重要な設計判断: DrawDevice 裏口 (mixer) は WINVER に入れない。**
- DrawDevice には `UpdateVideo(w,h,updator)` / `ClearVideo()` の "裏口" があり、SDL は
  **vomMixer** でこれを使う (`generic/visual/VideoOvlImpl.cpp` の `Mode==vomMixer` →
  `Window->UpdateVideo`)。active DrawDevice が動画を最前面合成する方式。
- しかし**自前 DrawDevice に差し替えたとき、その DrawDevice が UpdateVideo を実装して
  いないと overlay が消える** = 従来互換が無い。よって WINVER の `BasicDrawDevice` には
  裏口を実装せず、overlay は**子ウィンドウ方式 (独立最前面ウィンドウ、DrawDevice 非依存で
  robust)** を採用する。子ウィンドウは `tTVPD3D11OverlayWindow` (`D3D11OverlayWindow.h/cpp`)
  を新設 (生成/位置/表示 + マウスメッセージを game window へ drain)。present は
  **D3D11 + YUV シェーダ** (Y/U/V を R8_UNORM テクスチャに直接 upload、GPU で BT.601
  limited-range → RGB 変換 + スケール。CPU YUV→RGB を省き高速化。将来 vomD3D11 の土台)。

### 実装ステップ (V-B) — CPU overlay は webm/mpg のみ (mp4/wmv は EVR 維持)
1. 子ウィンドウ CPU present の presenter を用意 (`PlayWindow` 流用 + GDI StretchDIBits)。
   webm/mpg の両方で共有できる形にする。
2. 基底 `tTVPLayerVideoBase` に出力モード (layer / overlay) を追加。overlay 時は Open で
   自前ダブルバッファ確保 + 子ウィンドウ生成、present フックを「layer=エンジンへ通知 /
   overlay=子ウィンドウへ StretchDIBits」で分岐。SetWindow/SetRect/SetVisible は overlay
   時に presenter へ委譲。→ これで **pl_mpeg (mpg) の overlay** が動く。
3. webm (tTVPWebpMovie は push モデルで基底と別構造) に overlay 出力を追加
   (同じ presenter を使い、Update コールバックで子ウィンドウへ present)。
4. `GetVideoOverlayObject` を形式別ディスパッチに: mp4/wmv/asf → MFPlayer(EVR)、
   webm → 新CPU overlay、mpg → 新CPU overlay、その他 → 当面 DirectShow。
5. DirectShow overlay (dsoverlay/dsmovie) を退役 (V-D)。EVR(MFPlayer) は mp4/wmv 用に残す。

**検証の注意**: overlay は子ウィンドウ (D3D11 合成外) なので `System.captureScreen`
(BasicDrawDevice) には映らない → 目視確認はユーザ側 or PrintWindow 等で別途。
自己検証は `KRMOVIE_OVERLAY_DUMP` env (子ウィンドウの backbuffer を BMP 保存) で実施。

**実装状況 (2026-07-30): V-B の webm/mpg overlay = D3D11 YUV present で実装・検証済。**
- 経路: `GetVideoOverlayObject` で webm→`tTVPWebpMovie(overlay)`、mpg→`tTVPMpeg1Video(overlay)`、
  その他 (mp4/wmv/avi) は従来 `tTVPDSVideoOverlay` (DirectShow) のまま。
  mp4/wmv の EVR(`GetMFVideoOverlayObject`) 経路は据え置き (品質維持)。
- **coded 幅クロップ (重要)**: movie-player の `COLOR_NOCONV` は VP8/9 の *coded* plane
  (16 アライン padding 付き。例 表示 1920 → coded 1984) をそのまま返し、`VideoFrameInfo.width`
  は coded 幅になる。これを無クロップで present すると右端に未定義 chroma 由来の緑帯が出る。
  対策として `GetVideoFormat()` の表示寸法 (extractor 由来 = 1920) を取得し、plane stride は
  そのままに **width だけ表示幅へ縮めて upload** (movie-player CLAUDE.md の既知 issue
  「YUV texture pass-through may misbehave」に該当)。pl_mpeg 側は `f->width`(表示)と
  `f->y.width`(stride) が別なので元々クロップ済み。
- 色は BT.601 limited-range 固定 (現状の webm/mpg サンプルで layer 経路と一致確認済)。
  将来 BT.709 / full-range を扱う必要が出たら `VideoFrameInfo` に colorSpace/range を
  露出させてシェーダを切り替える (movie-player 側 API 追加が必要)。

**検証の注意**: overlay は子ウィンドウ (D3D11 合成外) なので `System.captureScreen`
(BasicDrawDevice) には映らない。overlay の目視確認はユーザ側 or PrintWindow 等で別途。

### F-3 (将来項目) DrawDevice 改定 — mixer/裏口の従来互換対応
DrawDevice 裏口 (mixer, `UpdateVideo`) を "自前 DrawDevice でも動く" ようにする改定。
案: iTVPDrawDevice のデフォルト実装で裏口未対応デバイスでも最低限描く仕組み、あるいは
overlay(子ウィンドウ) へ自動フォールバックする層を設ける等。優先度低・別途検討。

## 検証

- テスト動画: `D:\work\kirikiri\movietest\`(リポジトリ外)に各形式 1 本
  (`bg.mpg`=MPEG-1, `bg.wmv`, `alpha.webm`, `movie.mp4`, 任意 `test.avi`)。
- WINVER でも `System.captureScreen` / `Agent.captureScreen` が動作する
  (`BasicDrawDevice::FulfillScreenCapture[FromBackBuffer]` 実装済、overlay/動画込みは
  present 直前のバックバッファから読み戻し)。Agent 駆動 API 自体も WINVER 対応済
  (入力注入は `AgentInput` seam 経由)。それでも A/V 同期・体感は最終的にユーザ目視確認。
- 各段階で REPL(`-replfile`)による数値確認(status/position/frame 前進)+ スクショ。

## 実装テンプレート:`tTVPWebpMovie` (webplayer.cpp)

新規レイヤプレイヤ (`tTVPMFSourceReaderVideo` / `tTVPMpeg1Video`) は、既存のモダンな
レイヤプレイヤ `tTVPWebpMovie` (webplayer.cpp) を**雛形にクローン**するのが最短。
共通の iTVPVideoOverlay レイヤ契約を既に正しく実装しているため、**デコーダ部分だけ
差し替える**。共通部分は基底クラス化を推奨 (~40 の overlay 専用メソッドは no-op 共有)。

`tTVPWebpMovie` の要点 (再現すべきパターン):
- `SetVideoBuffer(buff1,buff2,size)` → `mBuffer = buff1` を保持 (buff2 不使用)。
- デコード完了コールバックで **ボトムアップ書き込み**: `d = mBuffer + (w*4)*(h-1)`,
  `dpitch = -w*4` で 1 フレームを埋め、`mUpdate = true`、
  `PostMessage(OwnerWindow, WM_GRAPHNOTIFY)` でエンジンを起こす。
- `GetEvent(evcode,p1,p2,got)` → `mUpdate` なら `evcode=EC_UPDATE`, `p1=現フレーム番号`,
  `got=true`, `mUpdate=false`。エンジンはこれを見て `GetFrontBuffer` → `AssignMainImage`。
- `GetFrontBuffer(&buff)` → `mBuffer` を返す (単一バッファ。ダブルバッファは下層/エンジン側)。
- `GetVideoSize` / `GetFPS` / `GetNumberOfFrame` / `GetTotalTime` / `Play`/`Stop`/`Pause` /
  `SetPosition`(seek) / `GetPosition` / `GetStatus` はデコーダへ委譲。
- overlay 専用 (`SetRect`/`SetWindow`/`SetVisible`/mixing/contrast/brightness/hue/
  saturation 等) は全て no-op。
- `EC_COMPLETE`(=0x0001) は再生終端で通知 (ループ/停止判定に使用)。`WM_GRAPHNOTIFY`
  でエンジンの WndProc が起き、`GetEvent` をドレインする。

新プレイヤの追加要素: デコードスレッド (自前のフレームタイミング) + MF SourceReader /
pl_mpeg のデコード → RGB32(BGRA) 変換 → mBuffer へボトムアップ書き込み。
pl_mpeg は `IStream` を `plm_buffer` のカスタム read コールバックへ橋渡し
(webplayer の `MovieStream` に相当)。

## フレーム配送契約 (V-C の要・現行 BufferRenderer 準拠)

`iTVPVideoOverlay::SetVideoBuffer(BYTE* buff1, BYTE* buff2, long size)` でダブルバッファを
受け取り、デコード完了フレームを書き込んで `GetFrontBuffer` で提示、コールバックウィンドウへ
更新通知。ピクセル形式・ストライド(負ピッチ=ボトムアップの可能性)・32bpp 前提を
現行実装から厳密に踏襲する(実装時に BufferRenderer.cpp / dslayerd.cpp を精読して一致させる)。
