# WINVER モダン化ロードマップ

Win10/11 を前提に、WINVER 実装のレガシー構成をモダン API へ寄せる作業の全体計画。
D3D9→D3D11 移行 (完了) に続く一連のワークストリームを、依存関係を踏まえて順序化する。

## ★全完了サマリ (2026-07-30)

本ロードマップの全フェーズ + Track V を完了。WINVER (`x64-windows-win`) 実機検証・
`krkrz_develop.git master` へ push 済 (umbrella `krkrz_dev` develop の `src/core` ポインタも更新済)。

| 区分 | 内容 | SSOT |
|---|---|---|
| D3D11 移行 | Direct3D9 → D3D11/DXGI 描画 (BasicDrawDevice) | [D3D11Migration.md](D3D11Migration.md) |
| Phase 0 | Win10 ターゲット化・マニフェスト・CompatibleNativeFuncs 撤去 | 本書 |
| Phase 1 | 音声マルチ ch (エンドポイント追従) + WaveSoundBuffer→miniaudio + DirectSound 撤去 | 本書 |
| Phase 2 | DLL ロード硬化 + DPI (GetDpiForWindow / WM_DPICHANGED) | 本書 |
| Phase 3 | 排他フルスクリーン→ボーダレス + GDI 色形式探り撤去 + ddraw.h 撤去 | 本書 |
| Phase 4 | QPC/GetTickCount64 + waitable timer + SHGetKnownFolderPath + Win11 判定 + JPEG XR 撤去 | 本書 |
| Track V | DirectShow/EVR/baseclasses 全撤去 → CPU/HW デコード + D3D11 present 統一。krmovie exe 統合。overlay presenter (D3D11 pull 合成) + IMFMediaEngine HW デコード (既定) + vomMixer (mixer 用 CPU 固定) + 音量読み戻し修正 | [MovieMFMigration.md](MovieMFMigration.md) |

**ユーザ可視の主な変更**: 動画 overlay は既定でハードウェアデコード (mp4/wmv 等、`-mediaengine=no`
で無効化)、追加画像合成は `vomMixer` 指定時のみ、JPEG XR (.jxr) サポート撤去、音声 5.1/マルチ ch
対応、DPI 追従。将来項目: Generic(SDL) の presenter 統一 / HW 経路への mixer 直描画 / F-1 3D 音声 /
F-2 long-path。

- **完了済**: D3D9 → D3D11/DXGI 描画移行 (SSOT: `doc/D3D11Migration.md`)。
- 監査ソース: レガシー Win32 API / メディア・描画・DPI / マニフェスト・バージョン分岐
  の 3 系統調査 (2026-07-29)。

## 原則

- **最低ターゲットを Win10 に引き上げる**。旧 OS (XP/Vista/7/8) 向けフォールバックは撤去。
- **サブシステム単位でまとめる** — 各フェーズでビルド + 実機スモーク (WINVER=`x64-windows-win`、
  PrintWindow キャプチャ / 音声・動画は実再生)。
- **退化させない順序**。特に音声は「マルチ ch 化 → 移行 → DirectSound 撤去」の順。
- 大きく独立した krmovie の MF 化は**並行トラック**として扱う (D3D11 描画完了が前提)。

## フェーズ (推奨実行順)

### Phase 0 — 基盤引き上げ 【小・機械的・他を解禁】 ✅ 完了 (commit f88c0f78)
WINVER ビルド + 実起動描画スモーク確認済。
| # | 項目 | ファイル | 依存 |
|---|---|---|---|
| 0-1 | `_WIN32_WINNT`/`WINVER` を `0x0601`(Win7)→`0x0A00`(Win10) | `win32/vcproj/targetver.h` | — |
| 0-2 | マニフェストに `supportedOS`(Win10 GUID) + `requestedExecutionLevel asInvoker` 明示 | `win32/vcproj/dpi.manifest` | — |
| 0-3 | `CompatibleNativeFuncs` 撤去 → touch/gesture API を直リンク。`SetThreadDescription` も直リンク化 | `win32/environ/CompatibleNativeFuncs.{h,cpp}`, `TVPWindow.cpp`, `WindowFormUnit.cpp`, `SystemImpl.cpp`, `ThreadImpl.cpp` | 0-1 |

検証: 全プリセットビルド + タッチ/起動スモーク。

### Phase 1 — 音声マルチ ch 化 + DirectSound 撤去 【ユーザ優先・自己完結】 ✅ 完了
1-1 (f81cd059) / 1-2 (e8c5f11e) / 1-3 (80a4ec45)。WINVER・SDL 両ビルド + WINVER
REPL 実機 (WaveSoundBuffer 再生・ma_engine WASAPI result=0・endpoint 追従) 確認済。
**discrete 5.1 の最終確認は 5.1 環境で** (現環境=RDP はステレオのため 2ch 追従を確認)。
| # | 項目 | ファイル | 依存 |
|---|---|---|---|
| 1-1 | **miniaudio 経路のマルチ ch 対応**: engine channels をエンドポイント/デバイスミックスフォーマットに追従、channel map 設定、ソースのチャンネル数維持 (現状 `TVPSoundChannels=2` ハードコード撤廃)。**全ビルド共通** (SDL も現状 stereo) | `common/sound/AudioStream.cpp` ほか | — |
| 1-2 | WINVER `WaveSoundBuffer` を DirectSound → miniaudio へ (1-1 の上で 5.1 退化なし) | `win32/sound/WaveImpl.cpp`, `common/sound/SoundBufferBaseImpl.cpp` | 1-1 |
| 1-3 | **DirectSound 撤去**: DS コード / `dsound.dll` 動的ロード / `GetSpeakerConfig`+primary buffer の名残 (Vista+ で実効薄) を削除 | `win32/sound/WaveImpl.cpp` | 1-2 |

検証: stereo + **5.1 コンテンツ**再生 (エンドポイント 5.1 構成)、音量カーブ (`-wsvolfactor`)、ループ/シーク。
補足: DirectSound の 5.1 は Vista+ では primary buffer 設定が実効薄く、実質エンドポイント頼み。
移行を機に**エンドポイント追従の正しい WASAPI(共有モード)マルチ ch 対応**に作り直すのが本筋。

**1-4 (追補): オーディオデバイス起動時先行初期化** ✅ — WINVER では miniaudio 自身が
WASAPI デバイスを開くため、遅延初期化のままだと初回サウンド再生時のデバイスオープンで
再生開始が遅れ音の頭が欠けることがある。`TVPPreInitAudioDevice()`
(`common/sound/AudioStream.cpp`) を追加し、`SysInitImpl.cpp` のサウンドアロケータ初期化
直後に呼んでデバイスオープンを起動時へ前倒し。`-wspreinit=no` で従来の遅延初期化に戻せる
(既定 ON)。SDL 版は `InitAudioSystem()` が起動時に `InitMiniAudio()` を呼ぶため元から不要。
WINVER 実機で起動時初期化 (startup.tjs 実行前) + 再生時の再初期化なし + `-wspreinit=no`
での抑止を確認済。

### Phase 2 — DPI & DLL セキュリティ 【高価値】 ✅ 完了
2-1 (c269a635) / 2-2・2-3 (51530709)。WINVER ビルド + 起動スモーク + REPL 実機
(200% 表示機で `win.displayDensity=192` の per-monitor DPI 取得を確認)。
libEGL 実ロードは当環境に ANGLE 非配置のため未検証、WM_DPICHANGED の実発火は
単一モニタのため未 (リサイズ経路は D3D11 検証でカバー)。
| # | 項目 | ファイル | 依存 |
|---|---|---|---|
| 2-1 | DLL ロード硬化: `SetDefaultDllDirectories(SEARCH_SYSTEM32/DEFAULT)` + 非システム DLL (libEGL/dinput) をフルパス/`LoadLibraryEx`。※dsound は Phase 1 で消滅 | `win32/environ/Application.cpp`, `win32/visual/OpenGLPlatform.cpp`, `win32/visual/DInputMgn.cpp` | — |
| 2-2 | `GetDensity()` → `GetDpiForWindow(hwnd)` + `GetDC(0)` の DC リーク解消 【小・明確なバグ】 | `win32/environ/Application.cpp:898` | 0-1 |
| 2-3 | `WM_DPICHANGED` ハンドリング (OS 提案矩形へ `SetWindowPos`) + 高 DPI 時のウィンドウ枠/レイアウト追従 | `win32/visual/WindowImpl.cpp` | 0-1 |

検証: 混在 DPI マルチモニタ (可能なら)、ウィンドウ移動、キャプチャ。
現状: マニフェストは `PerMonitorV2` 宣言済 (良好) だが実行時ハンドリングが未追従。

### Phase 3 — ディスプレイ/フルスクリーン 【✅完了 2026-07-30】
| # | 項目 | ファイル | 依存 |
|---|---|---|---|
| 3-1 ✅ | 排他フルスクリーン `ChangeDisplaySettings` 撤去 → ボーダレスウィンドウ (drawdevice の `SwitchToFullScreen` は D3D11 移行時に既にモード変更を止めており WS_POPUP でモニタ全体を覆う運用。残っていた activation/deactivation/exit ヘルパの `ChangeDisplaySettings` を撤去) | `win32/visual/WindowImpl.cpp` | 2-3 |
| 3-2 ✅ | GDI `SetPixel/GetPixel` 色形式探り撤去 (Win10/11 常時 32bpp、768 回ループ無意味) → `TVPGetDisplayColorFormat` は常に 0 (=32bit) を返す | `win32/visual/WindowImpl.cpp` | — |
| 3-3 ✅ | 死んだ `#include <ddraw.h>` 削除 (WindowFormUnit.cpp。WindowImpl.cpp のコメントアウト残骸も除去) | `win32/environ/WindowFormUnit.cpp` | — |

実装メモ:
- 3-1: `TVPMinimizeFullScreenWindowAtInactivation` / `TVPRestoreFullScreenWindowAtActivation` を no-op 化 (排他モードを持たないので活性化/非活性化での画面モード復元・再適用は不要。特に活性化時の `ChangeDisplaySettings(...,CDS_FULLSCREEN)` はボーダレスでは有害だった)。参照されなくなった `TVPFullScreenWindow` (常に NULL だった) と到達不能な D3D9 初期化ブロックも撤去。mode 候補列挙は表示ウィンドウのサイズ/ズーム決定に引き続き使用。
- 検証: WINVER (x64-windows-win) ビルド OK + 起動スモーク済。**フルスクリーン切替も REPL ファイルチャネル (`-replfile`) で実機確認済** — `win.fullScreen=true` で `innerWidth/Height` がモニタ native 解像度 (3840x2160) のボーダレスウィンドウになり (排他モード変更なし)、`false` で復帰、往復ともクラッシュ無し。※WINVER にも REPL 本体+`-replfile` チャネルは有効 (`KRKRZ_REPL=KRKRZ_DESKTOP` 既定)。`System.captureScreen` は WINVER でも動作する (`BasicDrawDevice::FulfillScreenCapture` 実装済、overlay 込みは present 直前のバックバッファから)。Elements ダイアログ (非モーダル/overlay モーダル/フロー/テキスト入力) も WINVER 対応済。**Agent クラスの駆動 API (入力注入 / captureScreen / dialogs 制御) も WINVER 対応済** — 本体は `common/environ/AgentControlIntf.cpp` に共通化し、入力注入だけ `AgentInput` seam (`generic/environ/AgentInput.cpp` = SendMouseMessage/SendMessage、`win32/environ/AgentInput.cpp` = OnMouse*/OnKey*) でプラットフォーム分離。SDL 専用として残るのは起動時 UserConfig UI (`-userconf`、ゲーム窓生成前の独立 OS ウィンドウが必須なため overlay 代替不可) のみ。

### Phase 4 — タイミング & 掃除 【✅完了 2026-07-30】
| # | 項目 | ファイル | 依存 | 状態 |
|---|---|---|---|---|
| 4-1 | tick 源 `timeGetTime` → **QueryPerformanceCounter** (32bit オーバフロー監視スレッド撤去) | `win32/utils/TickCountImpl.cpp`, `common/utils/TickCount.cpp` | — | ✅ |
| 4-2 | `timeBeginPeriod` 廃止 (システム全体のタイマ分解能変更を回避) + VSyncTimingThread の `timeGetTime`/`Sleep` を **high-resolution waitable timer** に置換 + winmm 依存除去 | `win32/base/SysInitImpl.cpp`, `win32/visual/VSyncTimingThread.{h,cpp}`, `win32/utils/TickCountImpl.cpp` | — | ✅ |
| 4-3 | `SHGetSpecialFolderPath/CSIDL` → `SHGetKnownFolderPath/FOLDERID` | `win32/base/SystemImpl.cpp`, `common/utils/ApplicationSpecialPath.h` | — | ✅ |
| 4-4 | `TVPGetOSName` の Win11 判定修正 (build 番号ベース) + 旧 OS 分岐掃除 | `win32/base/SystemImpl.cpp` | 0-1 | ✅ |
| 4-5 | 軽微: **JPEG XR (.jxr) 全撤去** (当初は死 `#else` のみの予定だったが、ユーザ判断で WINVER 専用・移植不可・旧式の JXR サポートごと撤去) / `ClipboardImpl` CF_TEXT 冗長削減 (※`GlobalAlloc` はクリップボード所有権の正しい用法なので維持) | `win32/visual/LoadJXR.cpp`(削除), `common/visual/GraphicsLoaderIntf.*`, `sources.cmake`, `CMakeLists.txt`, `win32/utils/ClipboardImpl.cpp` | — | ✅ |

実装メモ (4-1/4-3/4-4 = commit 未定, 2026-07-30):
- 4-1: `common/utils/TickCount.cpp` は KRKRZ_SRC_WIN32 (WINVER 専用) 割当なので他バリアント非影響。`TVPGetTickCount` は `TVPGetRoughTickCount64()` を直接返す形にし、32bit カウンタの桁溢れ監視スレッド + CS + バイアス加算を撤去。`TVPStartTickCount` は呼び出し元互換で no-op 残置。取得元は当初 GetTickCount64 にしたが、4-2 で timeBeginPeriod を廃止する関係で **QPC (QueryPerformanceCounter) に変更**した (下記 4-2 参照)。SDL は自前 `sdl3/utils/TickCount.cpp` (SDL_GetTicks) で不変。
- 4-3: 内部ヘルパを `TVPGetKnownFolderPath(REFKNOWNFOLDERID)` / `ApplicationSpecialPath::GetKnownFolderPath` に置換 (CSIDL_PERSONAL→FOLDERID_Documents, CSIDL_APPDATA→FOLDERID_RoamingAppData)。外部 API 名 (TVPGetPersonalPath 等) は不変。
- 4-4: `switch(dwPlatformId)` の Win9x/NT5/NT6 分岐を撤去し `dwMajorVersion==10` に集約。**Win11 は major=10 のままなので build>=22000 で区別**。Server も build 番号で 2016/2019/2022/2025 判定。
- 検証: WINVER + SDL 両ビルド OK。WINVER REPL 実機: `System.osName`=「Windows 11 10.0.26200 …」(build 26200 で Win11 判定)、`System.getTickCount()` 単調増加、`personalPath`=OneDrive リダイレクト先「ドキュメント」を正しく解決、`appDataPath`=Roaming 解決。
- 4-5 (JXR 全撤去 / clipboard): `.jxr` の登録 (`#ifdef __WINVER__`) と 4 宣言 + `LoadJXR.cpp`(780行) を削除、`sources.cmake` から除外。LoadJXR の WIC ヘッダが推移的に引いていた `shlwapi.lib` が外れて `PathFileExistsW`/`PathIsDirectoryW` が未解決になったため `CMakeLists.txt` の WIN target に `shlwapi` を明示追加。clipboard は `TVPClipboardSetText` を CF_UNICODETEXT のみに (CF_TEXT は Windows が自動合成、ANSI 版は非 ANSI 欠落もあり冗長)。WINVER+SDL 両ビルド + 起動スモーク OK。umbrella `doc/`(Files/GraphicSystem/fileformat)の JXR 記載も削除。
- 4-2 (timeBeginPeriod 廃止 / winmm 除去): 調査で **`-timerprec` の既定 prectick=1 → `timeBeginPeriod(1)` が既定で常時呼ばれていた**ことが判明 (システム/自プロセスの消費電力を上げるレガシー挙動)。これを廃止し、精度が要る 2 経路を局所対応: ① tick 源 `TVPGetRoughTickCount32/64` を timeGetTime → **QPC** に (timeBeginPeriod 廃止で ~15.6ms に退化するのを防ぎ、SDL の SDL_GetTicks 相当の高分解能を副作用なく得る)。② `VSyncTimingThread` の前眠り `::Sleep` を **`CreateWaitableTimerExW(CREATE_WAITABLE_TIMER_HIGH_RESOLUTION)`** による精密スリープに、`timeGetTime` を `TVPGetTickCount` に置換。`SysInitImpl.cpp` から `-timerprec`/`timeBeginPeriod`/`timeEndPeriod`/`timeGetDevCaps`/`TVPHighTimerPeriod`/`TVPTimeBeginPeriodRes` と `<mmsystem.h>` を撤去 (winmm 依存ほぼ消滅)。**フレームペーシング実機検証**: WINVER REPL で vsync 駆動の連続ハンドラ発火レートを計測 = **61.7fps** (60Hz vsync に正常ロック)、timeBeginPeriod 無しでも精度維持を確認。備考: 非 VSync の汎用 `Sleep`/`WaitFor` 待ち (TimerThread 等) の粒度は既定 ~15.6ms になるが、SDL ビルドは元々 timeBeginPeriod を呼ばずこの粒度で出荷済み = 許容範囲。

検証: 起動/タイミング/パス解決スモーク。

### Track V — krmovie 動画モダン化 (DirectShow → Media Foundation) 【大・独立トラック】

> **★実装は当初計画から変更・完了済 (2026-07-30)。詳細 SSOT = [MovieMFMigration.md](MovieMFMigration.md)。**
> DirectShow/EVR/baseclasses を全撤去し、layer/overlay とも CPU デコード (webm=movie-player /
> mpg=pl_mpeg / 他=MF SourceReader) + D3D11 present に統一。krmovie は exe へ静的統合 (V-A)。
> **V-E (presenter)** = overlay を `iTVPVideoPresenter`/`iTVPVideoPresenterHost` の 2 IF で
> `BasicDrawDevice` の D3D11 バックバッファへ pull 型合成 + mixer 追加画像復活 + モード整理
> (`vomMixer`/`vomMFEVR`→`vomOverlay`) を実装・実機検証済。以下の表は当初計画 (歴史的参考)。

上記 Phase と**並行可** (movie サブシステムで独立)。D3D11 描画デバイス (完了) を前提に:

| # | 項目 | ファイル | 依存 |
|---|---|---|---|
| V-1 | `IMFMediaEngine` フレームサーバプレイヤ新設: `TransferVideoFrame` で本体 D3D11 テクスチャへ HW デコード転送 → Layer 合成 (新 mode 例 `vomD3D11`)。HEVC/AV1・HDR の素地。`tTVPMFByteStream` 流用 | `win32/movie/` 新規 + `win32/visual/VideoOvlImpl.cpp` + D3D11 描画側にテクスチャ受け口 | D3D11 (済) |
| V-2 | `vomOverlay`(CLSID_VideoRenderer)/`vomLayer`(BufferRenderer) を MF/Media Engine 経路へ移行 | `win32/movie/dsoverlay.cpp`, `dslayerd.cpp` | V-1 |
| V-3 | DirectShow スタック撤去 (`dsoverlay`/`dslayerd`/`dsmovie`/`baseclasses`) | `win32/movie/`, `external/baseclasses/` | V-2 |
| V-4 | (任意) generic の `vomMixer`→`vomOverlay` 命名整理 (挙動不変・enum 温存)。詳細メモリ参照 | `generic/visual/VideoOvlImpl.cpp` | — |

検証: 各 mode の再生、mp4/HEVC、シーク/ループ、D3D11 合成、web:// ムービー。
補足: 既存 `MFPlayer.cpp`(MediaSession+標準 EVR)は素直な再生まで完成度高。V-1 の Media Engine 経路は
それとは別立てで新設 (詳細は別途 movie MF 計画 doc に展開)。

## 依存関係グラフ (要点)

```
Phase 0 (基盤) ──┬─ 0-1 → 0-3, 2-2, 2-3, 4-4
                 └─ 前提として先行

Phase 1 (音声):   1-1 → 1-2 → 1-3        (退化防止の直列)
Phase 2 (DPI/DLL): 2-1 / 2-2 / 2-3       (相互独立、0-1 後)
Phase 3 (表示):    2-3 → 3-1、3-2/3-3 独立
Phase 4 (掃除):    概ね独立
Track V (動画):    D3D11(済) → V-1 → V-2 → V-3、他 Phase と並行可
```

- **DirectSound (Phase 1) と DirectShow (Track V) は別物** — 独立に進められる。
- Phase 0 は多くの後続の前提 (Win10 直リンク化)。最初に。

## 推奨実行順

1. **Phase 0** ✅完了(基盤)
2. **Phase 1** ✅完了(音声 = マルチch化 → DirectSound 撤去)
3. **Phase 2** ✅完了(DPI/DLL)
4. **Phase 3** ✅完了(表示/フルスクリーン = D3D9 撤去の続き)
5. **Phase 4** ✅完了(掃除 = tick源QPC/DPI後始末/KnownFolder/Win11判定/JXR撤去/timeBeginPeriod廃止)
6. **Track V**(動画 MF化 = 最大。Phase 0 完了後いつでも並行開始可。V-1 は D3D11 の成果を早く見せられる) ← 次

各フェーズはビルド + 実機スモークを挟んで独立コミット。

## 将来項目 (モダン化完了後 / 新機能)

本ロードマップ (Phase 0-4 + Track V) のモダン化が一段落した後に着手する新機能。

### F-1 WaveSoundBuffer 3D 定位 API 新設 (miniaudio spatializer) 【優先度: 中〜高】
- **背景**: 旧吉里吉里の 3D モード (`-wsuse3d` / DS3D の `IDirectSound3DListener` + 3D バッファ)
  は「器」だけで **TJS から音源位置/速度/コーン/リスナーを設定する API が無く実質未使用**
  だった (かつ DS3D は Vista 以降 HW 無効)。Phase1-3 で撤去済。よって**旧 3D モードとの
  機能互換は不要** (合わせるべき既存 API が空)。
- **方針**: **miniaudio 内蔵スペーシャライザ (`ma_spatializer` / `ma_sound_set_position` /
  `set_velocity` / `set_cone` / 距離減衰 / ドップラー + `ma_engine_listener_*`) に直接
  マッピングしたクリーンな TJS 3D API を新規設計**。座標系・単位・既定値はモダンに再定義
  (DS3D の慣習は引きずらない)。Phase1 で `NO_SPATIALIZATION` にしている経路に対し、
  3D 指定時のみ spatialization を有効化する形。
- **段階**: 内蔵スペーシャライザ = 位置/距離/ドップラー/コーン/スピーカパンニング (**外部
  ライブラリ不要**)。本格 HRTF バイノーラル・遮蔽/反射・Atmos オブジェクト出力は更に上
  (段階2、Steam Audio / Windows Spatial Sound `ISpatialAudioClient` 等の別ライブラリ領域)。
- **位置づけ**: Phase 1 のパススルー再生とは独立の新機能。ゲームでの実需あり。
  **モダン化 (Phase 2-4/Track V) 完了後**に着手予定。

### F-2 long-path (>260字, MAX_PATH 超) 対応 【コア実装済 2026-07-30・残は edge のみ】
- **背景**: WINVER コードに `MAX_PATH`/`_MAX_PATH` 固定バッファが約40箇所残存。マニフェストに
  `longPathAware` が無く、OS が Win32 API 層で全パスを 260 字に強制していた。
- **実装済 (2026-07-30)**:
  1. **マニフェスト** `dpi.manifest` に `<longPathAware>true</longPathAware>` 追加。
  2. **実ファイル I/O の `\\?\` 拡張長プレフィックス**: `StorageImpl.cpp` に `TVPToExtendedLengthPath()`
     を新設し、CreateFile / FindFirstFile / GetFileAttributes(存在確認) に適用。ローカル名は既に
     絶対バックスラッシュ (`X:\...` / `\\server\...`) なので `\\?\` / `\\?\UNC\` を付けるだけで
     **レジストリ `LongPathsEnabled` ポリシー非依存**に 260 制限を回避。回帰回避のため長いパス
     (>= MAX_PATH-12) のみ変換し短いパスは従来通り。
  3. **正規化バッファの動的化**: `FilePathUtil.h` の `_wfullpath`(固定 `_MAX_PATH`→`_wfullpath(NULL,..,0)`
     で malloc 確保) / `StorageImpl.cpp` `GetTempPath`(長さ probe) / `WindowFormUnit.cpp` `DragQueryFile`
     (D&D ファイル名を長さ問い合わせで動的確保)。
  - **実機検証**: 327字パスの `isExistentStorage`=true、329字の深い .tjs を `execStorage` で読込・実行成功
    (`\\?\` 無しの素の Win32 では失敗する長さ)。短パスの通常起動も無回帰。
- **残 (edge・低優先)**: `SysInitImpl.cpp` の exe/モジュール/datapath/GetFullPathName 解決チェーンと
  診断/クラッシュハンドラ系 (HW 例外モジュール名, クラッシュダンプ) の MAX_PATH 固定は未変換。
  実害は「exe 自体を >260 の深さに設置」「datapath を >260 に設定」等の稀ケースに限られ、通常の
  コンテンツ/セーブの読み書きは上記 I/O 層の `\\?\` で機能する。必要時に個別対応。

### F-3 入力 (DirectInput) モダン化 【優先度: 中・将来調査/検証】
- **背景**: WINVER は `win32/visual/DInputMgn.cpp` で **DirectInput** を使用。ゲームパッド
  (`DIDEVTYPE_JOYSTICK`) と **マウス** (`GUID_SysMouse`) の両方を担う。DInput は legacy
  (DirectX 8/9 世代・非推奨) だが Win10/11 でも動作はする。
- **検討 (2026-07-30)**: 「DInput → XInput 全面載せ替え」は**不適**。① XInput はゲームパッド
  専用でマウスを扱えない ② XInput は XInput 互換 (Xbox 系) のみ = 汎用 HID パッド/アケコン/
  フライト/レーシング系が全滅 ③ ボタン/軸マッピングの意味が変わり既存ゲーム互換に影響。
- **妥当な方向**: **マウス = Raw Input (WM_INPUT)** / **ゲームパッド = Windows.Gaming.Input**
  (WinRT `Gamepad`/`RawGameController`。Win10+ で DInput/XInput 双方を置換する現行推奨 API。
  Xbox + 汎用両対応・トリガ分離・振動・バッテリ)。SDL3 ビルドは既に `SDL_Gamepad` (標準
  レイアウト) を使用しており、WINVER を Windows.Gaming.Input に寄せると挙動が揃う。
- **段取り (未着手)**: DInput 依存箇所の棚卸し (gamepad 列挙 / マウス取得 / force feedback /
  TJS の Pad・マウス API 契約) → 「標準ゲームパッド抽象」新設 + 既存 API 対応表で互換維持 →
  実機検証。**今回のモダン化スコープ外。将来、調査して検証する** (ユーザ判断 2026-07-30)。
