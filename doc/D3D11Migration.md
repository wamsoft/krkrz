# WINVER 描画の D3D9 → D3D11 移行

WINVER ビルド (`KRKRZ_VARIANT=WIN`) の Direct3D9 依存を撤去し、描画本体を
D3D11/DXGI ベースへ全面置換する作業計画。SDL / generic 変種は元々 D3D9 非依存
(SDL_Renderer / OpenGL ES) なので対象外。

方針は「置換可能なものは D3D11/DXGI/Win32 で置換、廃止して問題ないものは廃止、
`#ifdef` 併存は残さずまるごと置換」。

## 現状の D3D9 インベントリ (調査結果)

| # | 箇所 | 実体 | 対応 |
|---|---|---|---|
| 1 | `win32/visual/BasicDrawDevice.{h,cpp}` | WINVER 既定描画デバイス。動的テクスチャに合成フレームを memcpy → 全画面テクスチャ付きクアッドを `DrawPrimitiveUP` → `Present`。`GetRasterStatus` で vblank ポーリング | **D3D11 へ全面書換** (P1) |
| 2 | `win32/visual/WindowImpl.cpp` の共有 `IDirect3D9` (`TVPDirect3D` static) | (a) 解像度列挙 `TVPEnumerateAllDisplayModes` (b) ドライバ情報 `TVPDumpDirect3DDriverInformation` (c) 16bpp 555/565 判定 `TVPGetDisplayColorFormat` (d) `BasicDrawDevice` のデバイス生成 | (a) 既存の Win32 GDI 経路 (`EnumDisplaySettings`) へ一本化 (b) DXGI `IDXGIAdapter::GetDesc` (c) **撤去** (常時 32bpp) (d) は P1 で消滅 (P2) |
| 3 | `win32/visual/VSyncTimingThread.cpp` | D3D9 直接依存なし (リフレッシュレートは GDI `VREFRESH`)。`BasicDrawDevice::WaitForVBlank` の `GetRasterStatus` スピンだけが D3D9 | P1 で `WaitForVBlank` を DXGI 化すれば解消。スレッドの自己調整制御は簡素化可 (P1/P2) |
| 4 | 公開 ABI `TVPEnsureDirect3DObject` / `TVPGetDirect3DObjectNoAddRef` (`WindowImpl.h`, `__WINVER__` 限定) | tp_stub 経由でプラグインへ公開。**同梱プラグインの利用は 0 件**。シグネチャ文字列に `IDirect3D9*` が埋まる | **名前・シグネチャ維持の互換スタブ化** (Ensure=no-op, Get=NULL)。d3d9.dll 動的ロードを撤去 (P2) |
| 5 | `win32/movie/` krmovie.dll の vomMixer (`dsmixer.{h,cpp}` + `CVMRCustomAllocatorPresenter9.{h,cpp}`) | VMR9 レンダーレス。独立した `IDirect3DDevice9` を動的ロードで生成。非既定モード、本体 D3D9 と疎結合 | **vomMixer を撤去** し、ミキシング用途を vomMFEVR (Media Foundation/EVR) へ誘導 (P3) |
| 6 | `external/baseclasses/ddmm.cpp` の DirectDraw | ベンダ取込 (DirectShow BaseClasses) の残骸。自コード不使用 | 放置 |

### 重要な前提 (調査で判明)
- **d3d9.lib は元々リンクしていない**。D3D9 は全経路 `LoadLibrary("d3d9.dll")` +
  `GetProcAddress("Direct3DCreate9")` の動的ロード。`d3d9.h` は型定義のためだけ。
  → リンカからの `.lib` 除去作業は不要。
- `d3d9.h` を include するのは 5 ファイルのみ (WindowImpl.cpp / BasicDrawDevice.h /
  BasicDrawDevice.cpp / movie の dsmixer.h / CVMRCustomAllocatorPresenter9.h)。
- **同梱プラグインで D3D9 / 共有オブジェクトを使うものは 0 件**。影響は本体 WINVER
  変種と、公開 ABI 文字列を参照し得る外部 (非同梱) プラグインのみ。
- 本体は D3D11/DXGI を直接リンクしていなかった (ANGLE 経由のみ) → 新規に
  `d3d11.lib` / `dxgi.lib` / `d3dcompiler.lib` をリンク追加する (P0、実施済み)。

## D3D9 → D3D11/DXGI/Win32 置換対応表

| 現行 D3D9 | 用途 | 置換 |
|---|---|---|
| `Direct3DCreate9` | ファクトリ | `CreateDXGIFactory1` (列挙) / `D3D11CreateDevice` (描画) |
| `IDirect3D9::CreateDevice(HAL)` | デバイス+スワップチェーン | `D3D11CreateDevice` + `IDXGIFactory2::CreateSwapChainForHwnd` (flip model) |
| `IDirect3DTexture9(D3DUSAGE_DYNAMIC, X8R8G8B8)` + `LockRect` | 合成フレーム転送 | `ID3D11Texture2D(DYNAMIC, B8G8R8A8_UNORM)` + `Map(WRITE_DISCARD)` |
| `DrawPrimitiveUP(XYZRHW, FVF)` 全画面 quad | スケール描画 | RTV=backbuffer + 極小 VS/PS で quad。dest 矩形→NDC、UV は既存 sl/st/sr/sb 流用。半テクセル `-0.5` は D3D11 では不要 (撤去) |
| `Present(srect,drect,hwnd)` (`SWAPEFFECT_COPY`, oversized backbuffer) | 提示 | `IDXGISwapChain::Present`。**クライアント実サイズ swapchain へ直接スケール描画し全面 Present** (oversized backbuffer/COPY sub-rect を廃止し単純化) |
| `GetRasterStatus` スピン | vblank 待ち | `IDXGIOutput::WaitForVBlank` (ブロッキング) or `Present(1,…)` の vsync |
| `TestCooperativeLevel`/`Reset` ロスト処理 | デバイスロスト | 原則不要。`DXGI_ERROR_DEVICE_REMOVED`/`RESET` を Present/`GetDeviceRemovedReason` で検出し再生成。リサイズは `ResizeBuffers` |
| `GetAdapterDisplayMode().Format` 555/565 | 色形式 | 撤去 (常時 `B8G8R8A8_UNORM`) |
| `GetAdapterModeCount`/`EnumAdapterModes` | 解像度列挙 | Win32 `EnumDisplaySettings` (既存フォールバック) に一本化 |
| `GetAdapterIdentifier` | ドライバ情報 | `IDXGIAdapter::GetDesc` (Description/Vendor/Device/LUID)。WHQL 項目は削減 |
| `D3DCAPS9` (pow2/square/filter) | テクスチャ制約 | D3D11 は NPOT 標準対応、判定不要 (撤去) |

## フェーズ計画

- **P0 ビルド** ✅ — WIN ターゲットに `d3d11` / `dxgi` / `d3dcompiler` を
  `target_link_libraries` へ追加 (`src/core/CMakeLists.txt`)。
- **P1 描画デバイス (本丸)** ✅ — `tTVPBasicDrawDevice` を D3D11 実装へ書換。
  **WINVER ビルド (`x64-windows-win` プリセット) 成功**、実起動キャプチャで
  色/合成/スケール/テキスト/fps 正常を確認 (2026-07-29)。**差分更新は「永続
  CPU シャドウバッファ + DEFAULT テクスチャへ `UpdateSubresource`」方式**
  (D3D11 DYNAMIC の Map(WRITE_DISCARD) は全破棄で差分更新に使えないため)。
  注意: **WINVER 変種は `x64-windows-win`**。`x64-windows` は SDL 変種で
  win32/ の描画コードはコンパイルされない (検証時は必ず `-win` を使う)。
  - TJS クラス名 `BasicDrawDevice` と `tTJSNC_BasicDrawDevice`/`tTJSNI_BasicDrawDevice`
    は互換維持 (スクリプト側 `new BasicDrawDevice()` を壊さない)。
  - D3D11 device + `IDXGISwapChain1` (flip、クライアントサイズ) + 動的テクスチャ +
    passthrough VS/PS (`d3dcompiler` で実行時コンパイル) + RTV + sampler。
  - `NotifyBitmapCompleted` の memcpy (bottom-up 対応) は `Map(WRITE_DISCARD)` 上で維持。
  - `Show()` は `PresentDialogOverlay()` フック→ `Present`。Elements overlay の
    将来 Phase 5 はここに 2 枚目 quad を足す構造。
  - `WaitForVBlank` を `IDXGIOutput::WaitForVBlank` 化。
  - `SwitchToFullScreen`/`RevertFromFullScreen` は従来どおり実質 no-op
    (ボーダレス windowed。DXGI 排他フルスクリーンは使わない)。
- **P2 共有オブジェクト / 補助** ✅ (WINVER ビルド成功・実起動確認) — `WindowImpl.cpp`:
  - `TVPEnumerateAllDisplayModes` を GDI `EnumDisplaySettings` 経路に一本化 (D3D9 分岐削除)。
  - `TVPDumpDirect3DDriverInformation` を DXGI `IDXGIAdapter::GetDesc` へ。
  - `TVPGetDisplayColorFormat` (555/565) を撤去し 32bpp 固定。
  - `TVPEnsureDirect3DObject`/`TVPGetDirect3DObjectNoAddRef` を互換スタブ化
    (d3d9.dll ロード撤去、後者は NULL 返し)。tp_stub の再生成は不要
    (シグネチャ不変のため)。
  - `WindowImpl.cpp:850` 付近の壊れた未使用 `GetMonitorNumber` 様コードを除去。
- **P3 movie vomMixer 撤去** ✅ (WINVER ビルド成功・krmovie.dll 依存に d3d9.dll
  無しを確認) — `dsmixer.{h,cpp}` + `CVMRCustomAllocatorPresenter9.{h,cpp}` +
  `krmmovie.cpp` を削除、`GetMixingVideoOverlayObject` の export (def/pragma) と
  宣言・wrapper・GetProcAddress を撤去、win32 の vomMixer 生成 dispatch を
  `GetMFVideoOverlayObject` へ振替。movie CMake の該当ソース登録も削除。
  **enum `vomMixer` は残す** (generic/SDL ビルドでは D3D9 と無関係の既定モード=
  `UpdateVideo` 直接更新で多用)。
  - **重要な但し書き**: `vomMFEVR` は Media Foundation + 標準 EVR で、**EVR は
    OS 内部で D3D9 世代を使う**。P3 は krmovie の**自前 D3D9 直接依存の除去**で
    あり、映像の実描画を D3D11 化するものではない。真の D3D11 映像経路
    (`IMFMediaEngine` フレームサーバで本体 D3D11 テクスチャへ HW デコード転送) は
    別計画 (下記)。なお vomMixer の αビットマップ合成/ProcAmp は **元の D3D9(VMR9)
    経路でも no-op** (基底 `tTVPDSMovie` が空実装で dsmixer も override せず) だった
    ため、vomMFEVR への振替で**失われる稼働機能は無い**。win32 の vomMixer と
    vomOverlay の実際の差は「VMR9 で子ウィンドウに D3D9 描画 (キャプチャ/z順可)」対
    「旧 VideoRenderer のハードウェアオーバーレイ」であって、どちらもレイヤツリー外の
    別サーフェスに映像を出すだけ。**レイヤ合成に対応するのは vomLayer のみ**。
- **P4 検証** ✅ — WINVER (`x64-windows-win`) ビルド成功、実起動 PrintWindow
  (PW_RENDERFULLCONTENT) キャプチャで色/合成/スケール/テキスト/fps 正常を確認。
  **vsync 検証済** (一時計測): `IDXGISwapChain1::GetContainingOutput` 成功、
  `IDXGIOutput::WaitForVBlank` が毎回 ~16ms (1 リフレッシュ@60Hz、時折 31ms=2 フレーム)
  ブロックすることを確認 → vsync ペーシングは機能。Present は新フレーム時のみ
  (静的シーンでは present≈0) で正しくゲートされる。画面の fps オーバーレイ値
  (例 259) は **Present レートではなく**エンジン内部の idle/コンポジタ計数であり、
  旧 D3D9 の VSyncTimingThread 機構と等価に機能している (回帰なし)。
  **follow-up 実挙動確認済** (2026-07-29): ウィンドウ リサイズ 640×400 / 1280×800 で
  クラッシュ無し・正しく再描画 (swapchain `ResizeBuffers` 経路 OK、内容がサイズに追従して
  再合成される)。ウィンドウ移動 OK。全画面は排他非使用 (borderless windowed) のため
  Alt+Enter は no-op (`DXGI_MWA_NO_ALT_ENTER` + エンジン未割当。設計どおり); エンジンの
  `SwitchToFullScreen` はクライアントサイズ変化として同じ `ResizeSwapChain` 経路で処理される。
  マルチモニタ移動は単一モニタ環境のため未検証。TDR/デバイス削除は runtime 再現が困難で
  未実行だが、コード経路 (Present `DXGI_ERROR_DEVICE_REMOVED` → `HandleDeviceLost` →
  `DestroyD3DDevice` + `InvalidateAll` → 次 `EnsureDevice` で再生成) は実装済。

## 別計画 (本計画のスコープ外)

- **movie の Media Foundation 近代化** — vomMixer 撤去後の受け皿として MF/EVR 経路
  (`MFPlayer.cpp` 系) を精査し、`IMFMediaEngine` フレームサーバ (`TransferVideoFrame`
  で D3D11 テクスチャへ直接転送) / HW デコード / HDR 対応などモダン化を評価・実装する。
  D3D11 描画デバイス (P1) と統合すれば HW デコード映像を Layer 合成に載せられる。
  → 別 doc に分離 (movie 近代化計画)。

## メモ

- 共有 `IDirect3D9` は列挙 (IDXGIFactory) と描画 (ID3D11Device) の 2 系統に役割分割
  される。現状 1 個が列挙・情報・色形式・デバイス生成の 4 役を兼務していた。
- vsync は現状 `GetRasterStatus` スピン + `INTERVAL_IMMEDIATE` Present で、タイミング
  制御を `VSyncTimingThread` が担っていた。DXGI 化で `WaitForVBlank`/`Present(1,…)` に
  寄せられ、自己調整ロジックは縮小できる (挙動互換を優先して段階的に)。

## 後日談: Track V で movie モダン化完了 + device 拡張 (2026-07-30)

上記「movie の Media Foundation 近代化」計画は **Track V として完了**した (SSOT:
`doc/MovieMFMigration.md`)。本 D3D11 移行を前提に:

- **DirectShow / EVR / baseclasses を全撤去**し、layer/overlay とも CPU デコード
  (webm=movie-player / mpg=pl_mpeg / 他=MF SourceReader) + D3D11 present に統一。
  krmovie は exe へ静的統合。EVR (vomMFEVR) 経路も撤去 → `vomMFEVR` は `vomOverlay`
  互換エイリアスに。
- **`vomMixer` は「mixer 用 CPU 固定」経路として復活** (上の P3 で「vomMFEVR へ誘導」
  としたが、Track V-E で mixer 追加画像を確実に描く指定として再定義)。
- **真の HW デコード** = `IMFMediaEngine` フレームサーバ (`TransferVideoFrame`) を実装
  (`win32/movie/MediaEngineVideo.cpp`、MF-native 形式で既定・`-mediaengine=no` で無効化)。
- これに伴い **`BasicDrawDevice::CreateD3DDevice` の生成フラグに `D3D11_CREATE_DEVICE_VIDEO_SUPPORT`
  を追加し、生成直後に `ID3D11Multithread::SetMultithreadProtected(TRUE)` を有効化**
  (MediaEngine のデコードスレッドが engine の D3D11 デバイスを共有するため)。overlay は
  presenter (`iTVPVideoPresenter`/`iTVPVideoPresenterHost`) で D3D11 バックバッファへ合成。
  詳細は `CLAUDE.md` の Rendering 節と `doc/MovieMFMigration.md` を参照。
