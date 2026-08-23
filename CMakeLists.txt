cmake_minimum_required(VERSION 3.20)

# UTF-8 source
add_compile_options("$<$<AND:$<C_COMPILER_ID:MSVC>,$<COMPILE_LANGUAGE:C,CXX>>:/utf-8>")
add_compile_options("$<$<AND:$<CXX_COMPILER_ID:MSVC>,$<COMPILE_LANGUAGE:CXX>>:/Zc:__cplusplus>")
# ignore warnings
add_compile_options("$<$<AND:$<CXX_COMPILER_ID:MSVC>,$<COMPILE_LANGUAGE:C,CXX>>:/wd4819>")
add_compile_definitions("$<$<AND:$<CXX_COMPILER_ID:MSVC>,$<COMPILE_LANGUAGE:C,CXX>>:_CRT_SECURE_NO_WARNINGS>")
# force output PDB
add_link_options("$<$<AND:$<C_COMPILER_ID:MSVC>,$<COMPILE_LANGUAGE:C,CXX>>:/DEBUG>")


set(PROJECT_NAME krkrz)
set(PROJECT_VERSION 2.0.0)

# ----------------------------------------------------
# vcpkg マニフェスト統合
#
# トップの vcpkg.json を BASE として、サブモジュール側 (movie-player や
# libplugin など) の vcpkg.json をマージしたものを ${CMAKE_BINARY_DIR}/vcpkg.json
# に書き出し、VCPKG_MANIFEST_DIR を build dir に向ける。トップの vcpkg.json
# 自体は触らない (commit 済みファイルのまま)。
#
# 追加で取り込みたいフォルダがあれば -DKRKRZ_VCPKG_EXTRA_MANIFESTS="dir1;dir2"
# で指定する。各フォルダ直下の vcpkg.json が拾われる。
# ----------------------------------------------------
include(${CMAKE_CURRENT_SOURCE_DIR}/cmake/MergeVcpkgManifest.cmake)

set(KRKRZ_VCPKG_EXTRA_MANIFESTS "" CACHE STRING
    "Additional directories whose vcpkg.json should be merged into the build manifest")

# movie-player (VideoOverlay 実装) を組み込むか。OFF にすると external/movie-player
# を一切参照せず (manifest マージ対象からも外れる)、TVPCreateMoviePlayer は
# generic/app/movie_null.cpp のスタブになる。wasm など動画非対応環境向け。
# pre-project のマージ判定で使うためここで定義する。
option(KRKRZ_USE_MOVIE "build video overlay support via external/movie-player" ON)

# glyphware (統一フォントエンジン: FreeType + HarfBuzz + SheenBidi) を組み込むか。
option(KRKRZ_USE_GLYPHWARE "build the glyphware font engine (external/glyphware)" ON)

set(_KRKRZ_VCPKG_SUBDIRS "")
if(KRKRZ_USE_MOVIE)
    list(APPEND _KRKRZ_VCPKG_SUBDIRS "${CMAKE_CURRENT_SOURCE_DIR}/external/movie-player")
endif()
# glyphware は harfbuzz を必要とするが、umbrella (krkrz_dev) では
# cmake/CommonExternals.cmake が harfbuzz を FetchContent で一括提供し、各
# プラグインと ABI を揃える。そのため harfbuzz ターゲットが既にある場合は
# vcpkg マニフェストに glyphware/vcpkg.json を足さない (= 全体で harfbuzz を
# vcpkg 導入しない)。src/core / glyphware 単体ビルド時のみ vcpkg から取り込む。
if(KRKRZ_USE_GLYPHWARE AND NOT TARGET harfbuzz)
    list(APPEND _KRKRZ_VCPKG_SUBDIRS "${CMAKE_CURRENT_SOURCE_DIR}/external/glyphware")
endif()
if(KRKRZ_VCPKG_EXTRA_MANIFESTS)
    list(APPEND _KRKRZ_VCPKG_SUBDIRS ${KRKRZ_VCPKG_EXTRA_MANIFESTS})
endif()

# TVP_PLUGINS × TVP_PLUGIN_FOLDERS を pre-project でも解決して、各プラグイン
# 直下の vcpkg.json を統合対象に含める。本体側 foreach(PLUGIN ${TVP_PLUGINS})
# と同じ検索順 (フォルダリストの先頭で一致したものを採用)。
# TVP_PLUGINS / TVP_PLUGIN_FOLDERS は -D もしくは初期 CACHE で渡しておく
# 必要がある (project() 後に win32/custom.cmake 等で設定したものは
# pre-project マージには間に合わない)。
foreach(_plugin IN LISTS TVP_PLUGINS)
    foreach(_folder IN LISTS TVP_PLUGIN_FOLDERS)
        if(EXISTS "${_folder}/${_plugin}")
            list(APPEND _KRKRZ_VCPKG_SUBDIRS "${_folder}/${_plugin}")
            break()
        endif()
    endforeach()
endforeach()

krkrz_merge_vcpkg_manifest(
    BASE    "${CMAKE_CURRENT_SOURCE_DIR}/vcpkg.json"
    OUTPUT  "${CMAKE_BINARY_DIR}/vcpkg.json"
    SUBDIRS ${_KRKRZ_VCPKG_SUBDIRS}
    # movie-player の glew/glfw3 はサンプル用で krkrz のビルドには不要
    EXCLUDE
        "movie-player:glew"
        "movie-player:glfw3"
)
set(VCPKG_MANIFEST_DIR "${CMAKE_BINARY_DIR}" CACHE PATH
    "vcpkg manifest directory (merged manifest written by krkrz_merge_vcpkg_manifest)" FORCE)

project(${PROJECT_NAME} VERSION ${PROJECT_VERSION})

option(MASTER "build master version" OFF)

option(KRKRZ_USE_SJIS "use sjis" OFF)
option(KRKRZ_USE_OPENGL "use opengl" ON)
# wasm: リソースを wasm 本体へ埋め込む (ON) か、外部 (MEMFS /resource へ preload
# し file://./resource/ で読む, OFF) か。OFF だと wasm がリソース非依存になり
# 共有バイナリ化できる。案件別 charset は preload する config.cf で切替 (krkrz_web 側)。
option(KRKRZ_EMSCRIPTEN_EMBED_RESOURCE "embed resource into wasm (OFF=external MEMFS /resource via preload)" OFF)
set(KRKRZ_VARIANT "SDL" CACHE STRING "Build variant: SDL (default), WIN (Windows native), LIB (static library)")
option(KRKRZ_BUILD_TESTS "build SIMD parity tests" ON)

# Elements (汎用ダイアログ機構 / -userconf UI) を組み込むかどうか。
# SDL build でのみ意味があり、 OFF にすると:
#   - external/elements を add_subdirectory しない
#     (cycfi::elements / thorvg / elements_modal が要らなくなる。 elements_modal は
#      elements リポ配下 external/elements/external/elements_modal でビルドされる)
#   - common/visual/elements/* と sdl3/visual/SDL{Dialog,Elements*}.cpp /
#     common/visual/opengl/OGLDialogRenderer.cpp が build から外れる
#   - KRKRZ_HAS_ELEMENTS マクロが立たないので、 DrawDevice / Window / TJS の
#     dialog overlay / 入力転送 / `Dialog` ネイティブクラス / `-userconf` UI 経路が
#     #ifdef で全部 stub 化される
# WIN / LIB バリアントには元から Elements は組み込まれていないので、 この
# オプションは事実上 SDL ビルド専用の knob。
option(KRKRZ_USE_ELEMENTS "embed Elements dialog UI (cycfi::elements + thorvg + elements_modal, SDL build only)" ON)

# picojson の INT64 サポートを exe 全体で統一する。
# picojson はヘッダオンリーで value のメソッドが inline (COMDAT) 統合されるため、
# TU ごとに PICOJSON_USE_INT64 の有無が混在すると ODR 違反になる。 INT64 の有無で
# 型 enum の値が 1 ずれる (object=5↔6 等) ので、 リンカがどちらの実装を採用するか
# (= リンク順) 次第で get<>()/to_str() が type mismatch assert
# (throw runtime_error) を起こす。 従来は tjsDebuggerCore.h / DAPServer.h /
# FontSystem.cpp だけが TU ローカルに define しており、 NX リンクで実際に発火した。
# ディレクトリスコープで定義して elements_modal (add_subdirectory) 含む全 TU に適用する。
add_compile_definitions(PICOJSON_USE_INT64)

# デスクトップ判定 (Win/Mac/Linux)
# Android/iOS/Emscripten はデスクトップ機能を無効化
if(WIN32 OR (APPLE AND NOT CMAKE_SYSTEM_NAME STREQUAL "iOS") OR (UNIX AND NOT ANDROID AND NOT EMSCRIPTEN))
	set(KRKRZ_DESKTOP ON)
else()
	set(KRKRZ_DESKTOP OFF)
endif()

# デスクトップ判定はソース側でも使う ( 設定ファイル .cf/.cfu の探索規約を
# WINVER と揃えるかどうかの分岐。 generic/base/SysInitImpl.cpp )
if(KRKRZ_DESKTOP)
	add_compile_definitions(KRKRZ_DESKTOP=1)
endif()

# REPL はデスクトップでのみデフォルト有効
option(KRKRZ_REPL "enable interactive TJS REPL (requires icline)" ${KRKRZ_DESKTOP})
# 端末制御 (raw mode / termios / Win32 console API) を持たない環境向けに
# icline を切り、fgets ベースの簡易行入力に切り替えるオプション。
# 端末制御を持たない一部プラットフォームで有効にする想定。デフォルト ON (= icline を使う)。
option(KRKRZ_REPL_LINE_EDIT "use icline (line editing/history/color) for REPL" ON)
# ブラウザ REPL ビューワー (-replweb=<port> で HTTP+SSE サーバを立て、
# ブラウザから 上=ログ / 下=入力 で操作。端末非依存で選択/コピペ/スクロール可)。
# console REPL (KRKRZ_REPL) とは独立に有効化できる。端末 (標準入出力) を持たない
# 一部プラットフォームでは console REPL 無しで これ だけを有効にして使う。
option(KRKRZ_REPL_WEB "enable browser REPL viewer (HTTP+SSE, -replweb=<port>)" ${KRKRZ_REPL})
# 既定バインドアドレスを 0.0.0.0 (全インタフェース) にする。本体にローカル
# ブラウザが無く開発PCから LAN 越しに繋ぐ、端末を持たない一部プラットフォーム向け。
# OFF なら既定 127.0.0.1 (ローカル専用、デスクトップの安全側)。
option(KRKRZ_REPL_WEB_BIND_ANY "default -replweb bind address to 0.0.0.0 (for platforms without a local browser)" OFF)
# ファイル監視コマンドチャネル (-replfile=<dir>) と、その応答口を使う file-based
# modal (confirm / inputString / ファイル選択 を REPL 経由で応答)。ローカル
# ファイルシステムを使うデスクトップのエージェント駆動向け機能で、console REPL /
# web REPL とは独立。web REPL の modal はブラウザ側 + web I/F 側の別実装が必要で
# あり (未実装)、この file-based 実装とは無関係。既定は console REPL に追従。
option(KRKRZ_REPL_FILE "enable file command channel + file-based modal (-replfile, desktop agent driving)" ${KRKRZ_REPL})

# MASTER (量産) ビルドでは REPL / REPLWEB / file channel を完全無効化する。
# 端末 REPL もブラウザ REPL ビューワーも file channel も成果物に一切含めない。
if (MASTER)
	set(KRKRZ_REPL      OFF)
	set(KRKRZ_REPL_WEB  OFF)
	set(KRKRZ_REPL_FILE OFF)
endif()

# REPL コア = メインスレッド実行キュー / 画面キャプチャ / エントリポイント
# (TVPCreateREPL 等) と共有基盤。console REPL・web REPL・file channel の
# いずれか一つでも有効なら必要。console(icline/stdin) / web(HTTP+SSE) /
# file(-replfile) はそれぞれ独立に有効化でき、コア基盤はこの派生フラグで
# 一括制御する。file-based modal は KRKRZ_REPL_FILE 側 (コアには含めない)。
if (KRKRZ_REPL OR KRKRZ_REPL_WEB OR KRKRZ_REPL_FILE)
	set(KRKRZ_REPL_CORE ON)
else()
	set(KRKRZ_REPL_CORE OFF)
endif()

# LogCore はデスクトップでのみ有効 (リングバッファ、ファイル出力、TJS handler 等)
# モバイルでは SDL3 のネイティブログ出力を直接使用 (Android→logcat, iOS→os_log)
option(KRKRZ_USE_LOGCORE "enable LogCore (desktop logging features)" ${KRKRZ_DESKTOP})

# コンパイル時 TVPLOG_LEVEL の上書き。空文字列なら build type に応じて自動
# (MASTER=WARNING / NDEBUG=INFO / それ以外=DEBUG)。実行時 -loglevel=DEBUG を
# 効かせるためには Release/RelWithDebInfo (NDEBUG) でも DEBUG マクロが
# コンパイル時に展開される必要があるので、その場合 -DKRKRZ_LOG_LEVEL=DEBUG
# (= 1) を渡してビルドする。
# 値: VERBOSE / DEBUG / INFO / WARNING / ERROR / CRITICAL / OFF (大文字)
set(KRKRZ_LOG_LEVEL "" CACHE STRING "Compile-time log level override (VERBOSE/DEBUG/INFO/WARNING/ERROR/CRITICAL/OFF; empty = auto by build type)")
if(NOT KRKRZ_LOG_LEVEL STREQUAL "")
	set(_kz_log_level_map_VERBOSE  0)
	set(_kz_log_level_map_DEBUG    1)
	set(_kz_log_level_map_INFO     2)
	set(_kz_log_level_map_WARNING  3)
	set(_kz_log_level_map_ERROR    4)
	set(_kz_log_level_map_CRITICAL 5)
	set(_kz_log_level_map_OFF      6)
	if(DEFINED _kz_log_level_map_${KRKRZ_LOG_LEVEL})
		set(_kz_log_level_value ${_kz_log_level_map_${KRKRZ_LOG_LEVEL}})
		message(STATUS "KRKRZ_LOG_LEVEL=${KRKRZ_LOG_LEVEL} -> -DTVPLOG_LEVEL=${_kz_log_level_value}")
		add_compile_definitions(TVPLOG_LEVEL=${_kz_log_level_value})
	else()
		message(WARNING "KRKRZ_LOG_LEVEL '${KRKRZ_LOG_LEVEL}' is not recognized; ignoring")
	endif()
endif()

# VSCode Debug Adapter Protocol サーバ。デフォルト ON。OFF にすると DAP 関連の
# ソケット/JSON 処理が #ifdef で全部抜ける (組み込み等で不要な場合)。
option(KRKRZ_ENABLE_DAP "enable VSCode Debug Adapter Protocol server" ON)

# 動画プレイヤーをストリーム経由で開く (XP3 アーカイブ対応)。
# OFF にするとファイルパス直接指定で開く。
option(KRKRZ_MOVIE_STREAM "use stream-based movie player (XP3 archive support)" ON)
if(NOT KRKRZ_USE_MOVIE)
	# movie-player 非搭載時はストリーム再生経路も無効化する
	# (VideoOvlImpl の IMovieReadStream アダプタが movie-player のヘッダに依存)
	set(KRKRZ_MOVIE_STREAM OFF)
endif()

# DrawThreadPool (TVPBeginThreadTask 系) の利用統計を atomic カウンタで採取。
# memoverlay やログから「分散経路の利用率」「メイン spin 時間」等を観察できる。
# Switch 等で他コアが使われずメインだけ詰まる現象の調査用。
# OFF (デフォルト): カウンタ操作が完全に消えるので実 build に影響なし。
option(KRKRZ_DRAW_STATS "collect DrawThreadPool usage stats (atomic counters)" OFF)

# SDL3 内部の malloc/calloc/realloc/free を SDL_SetMemoryFunctions で
# 本体側に redirect し GlobalAllocStats の Sdl collector に集計する機能。
# OFF (デフォルト): SDL3 はデフォルトのアロケータをそのまま使う (SDL_SetMemoryFunctions
# は呼ばれない)。NX 等で SDL_SetMemoryFunctions による hook が問題を起こすため、
# 動作検証中はデフォルト OFF。観測したいプラットフォームでのみ ON にする。
option(KRKRZ_SDLMEMORY_STAT "hook SDL3 allocator via SDL_SetMemoryFunctions for GlobalAllocStats" OFF)

# GlobalAllocStats[Krkrz] の細粒度 breakdown (tag 別集計 / size histogram /
# AllocTagScope による event dispatch 計装) と TJS 言語側 Dictionary/Array
# instance tracking を有効化する。OFF にすると以下が完全に消える:
#   - TVPAllocTagScope ctor/dtor (inline 空クラス、thread-local 参照しない)
#   - operator new override 内の per-tag / size_hist 更新
#   - tTJSCustomObject ctor/dtor の counter inc/dec
#   - tTJSDictionaryObject/tTJSArrayObject の register/unregister (mutex+hashset)
#   - TVPDumpTJSObjectStats と Krkrz Dump の tag/size_hist 行
# 基本カウンタ (alloc_count / live_bytes) と per-allocator stats (File/
# Bitmap/Sound) は影響なし、常に有効。
# OFF (デフォルト): 量産ビルド向け、観測経路が完全に消える。
# 開発・調査時のみ ON にしてリーク種別を可視化。
option(KRKRZ_ENABLE_MEMSTAT_DETAIL "Enable per-tag / size_hist / TJSObjectStats diagnostics in Krkrz memory tracking" OFF)

# ----------------------------------------------------------------------------
# 統計機能の個別 gate (default ON、MASTER=ON 時は強制 OFF)
#
# 共通方針: TJS 層 (System.dumpHeap 等) と REPL コマンド (.mem / .memdump /
# .memoverlay 等) は OFF でもビルドされる。呼ばれた先の観測 API が空値 /
# "(disabled)" を返すだけで、機能としては何も起きない。
# ----------------------------------------------------------------------------

# GlobalAllocStats core: operator new override + TJS_malloc redirect + Krkrz
# pool (TLSF) + 基本カウンタ (alloc_count / live_bytes / peak_bytes)。
# OFF にすると operator new override / TJS_malloc redirect / 全観測 API が
# stub 化し、全 alloc は素 std::malloc 直行 (header なし / pool なし)。
option(KRKRZ_ENABLE_ALLOC_STATS "Enable global new/malloc tracking + Krkrz pool" ON)

# Per-allocator stats: File/Bitmap/Sound allocator が持つ
# tTVPMemoryAllocatorStatsCollector (size bin + tag 別) 集計。
# OFF にすると recordAlloc/recordFree が no-op、snapshot が空 Stats、
# used() が SIZE_MAX を返す ("不明" マーカー)。
option(KRKRZ_ENABLE_ALLOCATOR_STATS "Enable per-allocator (File/Bitmap/Sound) stats collector" ON)

# MemoryOverlay: 画面右上のリアルタイム折れ線グラフ (sampler thread + GL render)。
# OFF にすると Initialize/Finalize/SetEnabled/GetSnapshot が no-op、sampler
# thread が起動せず、GL 側の overlay 描画も呼ばれない。
option(KRKRZ_ENABLE_MEMORY_OVERLAY "Enable on-screen memory overlay (sampler thread + GL render)" ON)

# MemoryStatPeriodicDump: -memstatinterval=N で起動する周期 dump thread +
# -memstatonexit / -cachelistonexit による終了時ダンプ。
# OFF にすると Initialize が空となり cmdline 解析と thread 起動が抜ける。
option(KRKRZ_ENABLE_PERIODIC_DUMP "Enable -memstatinterval / -memstatonexit / -cachelistonexit" ON)

# MASTER ビルド時は統計機能をすべて強制 OFF (量産バイナリでは観測 overhead /
# code を排除)。MEMSTAT_DETAIL も同様に強制 OFF。
if (MASTER)
    set(KRKRZ_ENABLE_ALLOC_STATS      OFF CACHE BOOL "Forced OFF by MASTER" FORCE)
    set(KRKRZ_ENABLE_ALLOCATOR_STATS  OFF CACHE BOOL "Forced OFF by MASTER" FORCE)
    set(KRKRZ_ENABLE_MEMORY_OVERLAY   OFF CACHE BOOL "Forced OFF by MASTER" FORCE)
    set(KRKRZ_ENABLE_PERIODIC_DUMP    OFF CACHE BOOL "Forced OFF by MASTER" FORCE)
    set(KRKRZ_ENABLE_MEMSTAT_DETAIL   OFF CACHE BOOL "Forced OFF by MASTER" FORCE)
    set(KRKRZ_DRAW_STATS              OFF CACHE BOOL "Forced OFF by MASTER" FORCE)
    set(KRKRZ_SDLMEMORY_STAT          OFF CACHE BOOL "Forced OFF by MASTER" FORCE)
    message(STATUS "MASTER build: all stat features forced OFF")
endif()

# 描画分散判定 (GetAdaptiveThreadNum 等) の pixel 数閾値乗数。
# 各 callsite ごとの factor に対して "pixelNum >= factor * SCALE" で threading 化判定。
# 値を下げるほど小さい画像でも threading 化される。Switch (Tegra) 実機で 500 / 200 / 100
# を比較計測した結果 100 が sweet spot だったため 100 をデフォルト採用 (2026-05-08)。
# - 500: 1T 99% (= ほぼ分散経路に乗らない) → 旧仕様、PC では妥当だが他コアが活用されない
# - 200: 1T ~46%、Main -130ms/s、FPS +30%。Spin +30ms/s で sync コストはまだ余裕
# - 100: 1T ~30%、site:244 (Fill) もほぼ NT へ、Spin 増えず。これ以上下げても頭打ち
# PC 等で旧挙動を確認したい場合は -DKRKRZ_THREAD_PIXEL_SCALE=500 で復元可能。
set(KRKRZ_THREAD_PIXEL_SCALE "100" CACHE STRING "Pixel-count multiplier for adaptive thread dispatch (lower = more aggressive threading)")

if(KRKRZ_ENABLE_DAP)
	set(KRKRZ_DEFINES_DAP KRKRZ_ENABLE_DAP)
	if(WIN32)
		set(KRKRZ_LIB_DAP ws2_32)
	endif()
endif()

# LogCore 有効時: ソース追加 (マクロ定義は KRKRZ_DEFINES 初期化後に追加)
if(KRKRZ_USE_LOGCORE)
	list(APPEND KRKRZ_SRC_ADD common/utils/LogCore.cpp)
endif()

if(WIN32 AND (CMAKE_SIZEOF_VOID_P EQUAL 8))
set(WIN64 1)
endif()

if(WIN32)
if(WIN64)
set(EXENAME ${PROJECT_NAME}64)
set(LIBNAME lib${PROJECT_NAME})
set(PLUGIN_DIR "plugin64")
else()
set(EXENAME ${PROJECT_NAME})
set(LIBNAME lib${PROJECT_NAME}32)
set(PLUGIN_DIR "plugin")
endif()
else()
set(EXENAME ${PROJECT_NAME})
set(LIBNAME lib${PROJECT_NAME})
set(PLUGIN_DIR "plugin")
endif()

if(APPLE)
if(CMAKE_SYSTEM_NAME STREQUAL "iOS")
set(KRKRZ_APPLE_BUNDLE_RUNTIME_DIR "${EXENAME}.app")
else()
set(KRKRZ_APPLE_BUNDLE_RUNTIME_DIR "${EXENAME}.app/Contents/MacOS")
endif()
set(KRKRZ_PLUGIN_INSTALL_DIR "${KRKRZ_APPLE_BUNDLE_RUNTIME_DIR}")
else()
set(KRKRZ_PLUGIN_INSTALL_DIR "${PLUGIN_DIR}")
endif()

if (WIN32)
set(CMAKE_RC_FLAGS "")
endif()

set(KRKRZ_DEFINES
TJS_JP_LOCALIZED
TVP_ENABLE_EXECUTE_AT_EXCEPTION
TVP_LOG_TO_COMMANDLINE_CONSOLE
UNICODE
_UNICODE
# ファイル検索時はケース無視
TVP_AUTOPATH_IGNORECASE
# ローカルパス参照のcaseを強制参照
#TVP_LOCALFILE_FORCE_CASESENSITIVE
__CODEGUARD__
KRKRZ_VARIANT=${KRKRZ_VARIANT}
)

# ----------------------------------------------------
# バージョン情報の単一供給元
#
#   PROJECT_VERSION (先頭で定義) + KRKRZ_VERSION_BUILD を元に
#     - ${CMAKE_BINARY_DIR}/krkrz_version.h   … C++ 側 (generic の版情報取得)
#     - ${CMAKE_BINARY_DIR}/krkrz_version.rc  … Windows の VERSIONINFO
#   を生成する。 これ以前は WINVER が rc 手書きの 2.0.0.0、 SDL が
#   generic/base/PluginImpl.cpp 手書きの 1.0.0.1 と食い違っていた。
#
#   4 桁目 (build) は既定 0。 CI 等で振りたい場合のみ -DKRKRZ_VERSION_BUILD=N。
# ----------------------------------------------------
set(KRKRZ_VERSION_BUILD "0" CACHE STRING "4th version component (build number)")

if (KRKRZ_VARIANT STREQUAL "WIN")
	set(KRKRZ_PLATFORM_LABEL "Win32")
elseif (KRKRZ_VARIANT STREQUAL "LIB")
	set(KRKRZ_PLATFORM_LABEL "Library")
else()
	set(KRKRZ_PLATFORM_LABEL "SDL3")
endif()
set(KRKRZ_PRODUCT_NAME
    "TVP(KIRIKIRI) Z Core Wamsoft Edition / Scripting Platform for ${KRKRZ_PLATFORM_LABEL}")

configure_file(
    ${CMAKE_CURRENT_SOURCE_DIR}/cmake/krkrz_version.h.in
    ${CMAKE_BINARY_DIR}/krkrz_version.h
    @ONLY)

if (WIN32)
	configure_file(
	    ${CMAKE_CURRENT_SOURCE_DIR}/cmake/krkrz_version.rc.in
	    ${CMAKE_BINARY_DIR}/krkrz_version.rc
	    @ONLY)
	set(KRKRZ_VERSION_RC ${CMAKE_BINARY_DIR}/krkrz_version.rc)
else()
	set(KRKRZ_VERSION_RC "")
endif()

message(STATUS "krkrz version: ${PROJECT_VERSION}.${KRKRZ_VERSION_BUILD} (${KRKRZ_PRODUCT_NAME})")

if(NOT WIN32)
list(APPEND KRKRZ_DEFINES
TVP_NO_NORMALIZE_PATH
)
endif()

if (MASTER)
list(APPEND KRKRZ_DEFINES
MASTER
)
endif()

if (KRKRZ_REPL_CORE)
list(APPEND KRKRZ_DEFINES KRKRZ_USE_REPL)
# console REPL の icline 行編集を使うのは KRKRZ_REPL(console) 有効時のみ。
# web 専用ビルド (KRKRZ_REPL=OFF) や LINE_EDIT=OFF では fgets shim に切替。
if (NOT (KRKRZ_REPL AND KRKRZ_REPL_LINE_EDIT))
list(APPEND KRKRZ_DEFINES KRKRZ_REPL_NO_ICLINE)
endif()
endif()

# file 監視コマンドチャネル + file-based modal (confirm/inputString/選択の応答口)。
# ローカルファイルシステムを使うデスクトップ限定機能。web-only ビルドでは未定義に
# なり、modal 呼び出し元 (System/Storages) は native/既定へフォールバックする。
if (KRKRZ_REPL_FILE)
list(APPEND KRKRZ_DEFINES KRKRZ_USE_REPL_FILECHANNEL)
endif()

if (KRKRZ_USE_LOGCORE)
list(APPEND KRKRZ_DEFINES TVP_USE_LOGCORE)
endif()

if (KRKRZ_DRAW_STATS)
list(APPEND KRKRZ_DEFINES KRKRZ_DRAW_STATS)
endif()

if (KRKRZ_SDLMEMORY_STAT)
list(APPEND KRKRZ_DEFINES KRKRZ_SDLMEMORY_STAT)
endif()

if (KRKRZ_ENABLE_MEMSTAT_DETAIL)
list(APPEND KRKRZ_DEFINES KRKRZ_ENABLE_MEMSTAT_DETAIL)
endif()

if (KRKRZ_ENABLE_ALLOC_STATS)
list(APPEND KRKRZ_DEFINES KRKRZ_ENABLE_ALLOC_STATS)
endif()

if (KRKRZ_ENABLE_ALLOCATOR_STATS)
list(APPEND KRKRZ_DEFINES KRKRZ_ENABLE_ALLOCATOR_STATS)
endif()

if (KRKRZ_ENABLE_MEMORY_OVERLAY)
list(APPEND KRKRZ_DEFINES KRKRZ_ENABLE_MEMORY_OVERLAY)
endif()

if (KRKRZ_ENABLE_PERIODIC_DUMP)
list(APPEND KRKRZ_DEFINES KRKRZ_ENABLE_PERIODIC_DUMP)
endif()

# pixel 閾値乗数。500 (デフォルト値) のときも明示で渡しておく
# (header の #ifndef フォールバックと一致するので副作用なし)。
list(APPEND KRKRZ_DEFINES KRKRZ_THREAD_PIXEL_SCALE=${KRKRZ_THREAD_PIXEL_SCALE})

if (KRKRZ_USE_SJIS)
list(APPEND KRKRZ_DEFINES
# 標準をSJISにするかどうか
TVP_TEXT_READ_ANSI_MBCS
)
endif()

if (KRKRZ_MOVIE_STREAM)
list(APPEND KRKRZ_DEFINES
KRKRZ_MOVIE_STREAM
)
endif()

if (WIN32)
list(APPEND KRKRZ_DEFINES
TJS_TEXT_OUT_CRLF
)
endif()

include(sources.cmake)

if (EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/win32/custom.cmake")
message(STATUS "add custom system")
include(${CMAKE_CURRENT_SOURCE_DIR}/win32/custom.cmake)
endif()

# ----------------------------------------------------
# plugins
#
# TVP_PLUGIN_FOLDERS  プラグイン参照先フォルダのリスト
# TVP_PLUGINS         同時作成するプラグインのリスト
# TVP_PLUGINS_STATIC  同時作成するプラグインのうち吉里吉里本体に含めるプラグインのリスト
#
# STATIC 対象プラグインは TVP_STATIC_PLUGIN 定義時に krkr_plugin_XXX 関数をエクスポートする必要あり
# ----------------------------------------------------

# StaticPlugins.cpp 生成用の変数を初期化
set(STATIC_PLUGIN_EXTERNS "")
set(STATIC_PLUGIN_ATSTARTS "")
set(KRKRZ_DYNAMIC_PLUGINS "")

foreach(PLUGIN ${TVP_PLUGINS})
    if(${PLUGIN} IN_LIST TVP_PLUGINS_STATIC)
        set(TVP_STATIC_PLUGIN ON)
    else()
        set(TVP_STATIC_PLUGIN OFF)
    endif()

    # TVP_PLUGIN_FOLDERS から該当プラグインのフォルダを検索
    set(PLUGIN_SOURCE_DIR "")
    foreach(FOLDER ${TVP_PLUGIN_FOLDERS})
        if(EXISTS "${FOLDER}/${PLUGIN}")
            set(PLUGIN_SOURCE_DIR "${FOLDER}/${PLUGIN}")
            break()
        endif()
    endforeach()

    if(NOT PLUGIN_SOURCE_DIR)
        message(FATAL_ERROR "Plugin '${PLUGIN}' not found in any of: ${PLUGIN_FOLDERS}")
    endif()

    # krkrz_plugin() (tp_stub/krkrz.cmake) は生成ターゲットを GLOBAL プロパティ
    # KRKRZ_PLUGIN_TARGETS へ追記する。add_subdirectory 前後の差分を取ることで、
    # フォルダ名と異なるターゲット名 (gles → krkrgles) や 1 フォルダ複数プラグイン
    # (motion → krkrmotion + krkremote) を実ターゲット名で扱える。
    get_property(_PLUGIN_TARGETS_BEFORE GLOBAL PROPERTY KRKRZ_PLUGIN_TARGETS)
    add_subdirectory(${PLUGIN_SOURCE_DIR} ${CMAKE_CURRENT_BINARY_DIR}/plugins/${PLUGIN})
    get_property(PLUGIN_TARGETS GLOBAL PROPERTY KRKRZ_PLUGIN_TARGETS)
    if(_PLUGIN_TARGETS_BEFORE)
        list(REMOVE_ITEM PLUGIN_TARGETS ${_PLUGIN_TARGETS_BEFORE})
    endif()
    if(NOT PLUGIN_TARGETS)
        # krkrz_plugin() を使わない従来型 CMakeLists はフォルダ名=ターゲット名前提
        set(PLUGIN_TARGETS ${PLUGIN})
    endif()

    foreach(PLUGIN_TARGET ${PLUGIN_TARGETS})
    if(TVP_STATIC_PLUGIN)
        message(STATUS "static  plugin: ${PLUGIN} (target: ${PLUGIN_TARGET})")
        list(APPEND KRKRZ_LIB_ADD ${PLUGIN_TARGET})
        # StaticPlugins.cpp 用のデータを生成
        string(APPEND STATIC_PLUGIN_EXTERNS "extern \"C\" void krkrz_plugin_${PLUGIN_TARGET}();\n")
        string(APPEND STATIC_PLUGIN_ATSTARTS "static tTVPAtStart AtStart_${PLUGIN_TARGET}(TVP_ATSTART_PRI_PREPARE, [](){ krkrz_plugin_${PLUGIN_TARGET}(); });\n")
    else()
        message(STATUS "dynamic plugin: ${PLUGIN} (target: ${PLUGIN_TARGET})")
		list(APPEND KRKRZ_DYNAMIC_PLUGINS ${PLUGIN_TARGET})
		if(NOT APPLE)
        install(TARGETS ${PLUGIN_TARGET}
			LIBRARY DESTINATION ${KRKRZ_PLUGIN_INSTALL_DIR}
			RUNTIME DESTINATION ${KRKRZ_PLUGIN_INSTALL_DIR}
        )
		endif()
    endif()
    endforeach()
endforeach()

if(CMAKE_SYSTEM_NAME STREQUAL "iOS" AND KRKRZ_DYNAMIC_PLUGINS)
message(WARNING "iOS restricts runtime loading of bundled dynamic libraries; prefer TVP_PLUGINS_STATIC for app plugins")
endif()

# Static Pluginが存在する場合のみ StaticPlugins.cpp を生成してソースに追加
if(STATIC_PLUGIN_EXTERNS)
	set(STATIC_PLUGINS_CPP_CONTENT "#include \"tjsCommHead.h\"\n#include \"SysInitIntf.h\"\n${STATIC_PLUGIN_EXTERNS}${STATIC_PLUGIN_ATSTARTS}")
    file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/StaticPlugins.cpp" "${STATIC_PLUGINS_CPP_CONTENT}")
    list(APPEND KRKRZ_SRC_ADD "${CMAKE_CURRENT_BINARY_DIR}/StaticPlugins.cpp")
	message(STATUS "Generated StaticPlugins.cpp with static plugins")
	# 共通バインド部
	list(APPEND KRKRZ_INC ${TPSTUB_DIR})
	list(APPEND KRKRZ_SRC_ADD ${TPSTUB_DIR}/tp_stub.cpp)
endif()

# ----------------------------------------------------
# libraries
# ----------------------------------------------------

if (NOT TARGET ZLIB::ZLIB)
	find_package(ZLIB REQUIRED)
endif()
if (NOT TARGET freetype)
	find_package(freetype CONFIG REQUIRED)
endif()
# glyphware (統一フォントエンジン)。tests は本体ビルドでは無効化。harfbuzz は
# umbrella では CommonExternals が提供 (TARGET harfbuzz)、単体ビルドでは
# glyphware/vcpkg.json のマニフェストマージ経由 (上の NOT TARGET harfbuzz 参照)。
# glyphware 側 CMakeLists も if(NOT TARGET harfbuzz) で find_package をガードする。
set(KRKRZ_LIB_GLYPHWARE "")
if (KRKRZ_USE_GLYPHWARE)
	if (NOT TARGET glyphware)
		set(GLYPHWARE_BUILD_TESTS OFF CACHE BOOL "" FORCE)
		# 配布物に glyphware/SheenBidi の lib/include を混入させない
		set(GLYPHWARE_INSTALL OFF CACHE BOOL "" FORCE)
		add_subdirectory(external/glyphware)
	endif()
	set(KRKRZ_LIB_GLYPHWARE glyphware)
	add_compile_definitions(KRKRZ_USE_GLYPHWARE)
endif()
# libogg + libvorbis は krkrz/external/sound-codecs/ で FetchContent して
# 自前ビルドする。これにより _ogg_malloc/_ogg_calloc/_ogg_realloc/_ogg_free を
# SoundAllocator (sound_malloc 等) に差し替えられる。
# 既に親 CMakeLists で Ogg::ogg 等が定義されている場合 (LIB build や別途取込み)
# は重複構築しない。
if (NOT TARGET Ogg::ogg OR NOT TARGET Vorbis::vorbis)
	add_subdirectory(external/sound-codecs)
endif()
find_package(PNG MODULE)
find_package(Opus)
find_package(oniguruma CONFIG REQUIRED)
find_package(glm CONFIG REQUIRED)
find_package(libjpeg-turbo CONFIG)

# libjpeg-turbo::turbojpeg が見つからない場合
if (NOT libjpeg-turbo_FOUND)
	message("libjpeg-turbo not found, trying to find libjpeg")
	find_package(JPEG QUIET)
else()
	message("libjpeg-turbo found")
	set(JPEG_INCLUDE_DIR ${libjpeg-turbo_INCLUDE_DIRS})
	set(JPEG_LIBRARIES $<IF:$<TARGET_EXISTS:libjpeg-turbo::turbojpeg>,libjpeg-turbo::turbojpeg,libjpeg-turbo::turbojpeg-static>)
	set(JPEG_TURBO_FOUND TRUE)
	list(APPEND KRKRZ_DEFINES TVP_USE_TURBO_JPEG_API)
endif()

# opusfile は撤去 (OpusCodecDecoder.cpp が libopus 直叩きに移行)。
# decoder state は krkrz 側で sound_malloc 経由で確保するので、外部依存は
# libopus のみで十分。

find_path(PICOJSON_INCLUDE_DIRS "picojson/picojson.h")
# ----------------------------------------------------
# ログ整形は tvpfmt (common/utils/LogCore.cpp の自前実装) が担当するため
# fmtlib / <format> への依存は持たない。機能追加が必要なら LogIntf.h /
# LogCore.cpp の vformat 側に限定して行う。
# ----------------------------------------------------

# ----------------------------------------------------
# icline (portable readline replacement, isocline fork)
# ----------------------------------------------------
if (KRKRZ_REPL AND KRKRZ_REPL_LINE_EDIT AND NOT TARGET icline)
	include(FetchContent)
	FetchContent_Declare(
		icline
		GIT_REPOSITORY https://github.com/deths74r/icline.git
		GIT_TAG        main
		EXCLUDE_FROM_ALL
	)
	FetchContent_MakeAvailable(icline)
	if (MSVC)
		target_compile_options(icline PRIVATE /wd4996 /wd4244 /wd4267 /wd4101 /wd4018)
	else()
		target_compile_options(icline PRIVATE -Wno-unused-function -Wno-unused-variable)
	endif()
	list(APPEND KRKRZ_INC ${icline_SOURCE_DIR}/include)
	set(KRKRZ_LIB_REPL icline)
endif()

# ----------------------------------------------------
# ブラウザ REPL ビューワー (HTTP+SSE。-replweb=<port>)
# 依存追加なし (TCP ソケット + 埋め込み HTML)。Windows は Winsock をリンク。
# ----------------------------------------------------
if (KRKRZ_REPL_WEB)
	list(APPEND KRKRZ_DEFINES KRKRZ_REPL_WEB)
	if (KRKRZ_REPL_WEB_BIND_ANY)
		list(APPEND KRKRZ_DEFINES KRKRZ_REPL_WEB_BIND_ANY)
	endif()
	if (WIN32)
		list(APPEND KRKRZ_LIB_REPL ws2_32)
	endif()
endif()

# ----------------------------------------------------
# Elements (汎用ダイアログ機構, ThorVG ベース)
#
# WIN / SDL 両 variant で組み込む (LIB は window / renderer が無いため対象外)。
# host UI backend は variant で選ぶ (WIN→win32 / SDL→sdl)。 build 設定・overlay
# 経路 (overlay_session) は host 非依存。 各 variant の target には
# ${KRKRZ_LIB_ELEMENTS} をリンクし、 KRKRZ_HAS_ELEMENTS で consumer 側 (#ifdef) を
# 有効化する。 KRKRZ_USE_ELEMENTS=OFF で完全に除外。
set(KRKRZ_LIB_ELEMENTS "")
if (KRKRZ_USE_ELEMENTS AND NOT KRKRZ_VARIANT STREQUAL "LIB")
	if (NOT TARGET cycfi::elements)
		# host UI backend を variant で選ぶ。
		#   SDL variant → "sdl"  (krkrz-SDL は SDL3 ベース)
		#   WIN variant → "win32" (WINVER は Win32 window + D3D11。SDL3 を引き込まない
		#                 ため cycfi の native Win32 host を使う。overlay ダイアログは
		#                 embedded モード = host 非経由の ce::view(extent) で描く)。
		if (KRKRZ_VARIANT STREQUAL "WIN")
			set(ELEMENTS_HOST_UI_LIBRARY "win32" CACHE STRING "Elements host UI library" FORCE)
		else()
			set(ELEMENTS_HOST_UI_LIBRARY "sdl" CACHE STRING "Elements host UI library" FORCE)
		endif()
		# サンプル/テストはビルド対象外
		set(ELEMENTS_BUILD_EXAMPLES OFF CACHE BOOL "Build Elements examples" FORCE)
		# ThorVG は krkrz に静的リンクする
		set(TVG_BUILD_SHARED OFF CACHE BOOL "Build ThorVG as static" FORCE)
		set(TVG_STATIC_MODULES ON CACHE BOOL "Use static modules in ThorVG" FORCE)
		# 多言語テキストレンダリング: FreeType + HarfBuzz ローダを使う (TTF reader と
		# 相互排他)。Elements 側のデフォルトと同じだが、cache を旧値で持ち越して
		# しまうケースがあるため明示 force-set する。
		set(TVG_LOADER_TTF OFF CACHE BOOL "Disable built-in TTF loader" FORCE)
		if(KRKRZ_USE_GLYPHWARE)
			# glyphware ホストブリッジ経由 (thorvg 内の FreeType+HarfBuzz を使わない。
			# ブリッジ実装 = common/visual/elements/GlyphwareTvgBridge.cpp)
			set(TVG_LOADER_FT OFF CACHE BOOL "Disable in-tree FT+HarfBuzz text loader" FORCE)
			set(TVG_LOADER_GW ON  CACHE BOOL "Enable host-bridge (glyphware) text loader" FORCE)
		else()
			set(TVG_LOADER_FT ON  CACHE BOOL "Enable FT+HarfBuzz multilingual text loader" FORCE)
			set(TVG_LOADER_GW OFF CACHE BOOL "Disable host-bridge text loader" FORCE)
		endif()
		# Elements / ThorVG が直接ファイル I/O することを禁止する。 krkrz では
		# 全てのリソース読み込み (フォント / 画像 / Dialog JSON) を Storages 経由に
		# 統一する (XP3 / Switch サンドボックス対応)。 OFF で default resource_loader が
		# null になり、 common/visual/elements/StoragesResourceLoader.cpp が TVPReadStream
		# へ橋渡しする。
		set(ELEMENTS_FILE_IO_SUPPORT OFF CACHE BOOL "Allow elements to read resources from disk" FORCE)
		add_subdirectory(external/elements)

		# ThorVG は自前で global operator new/delete を override するが、 krkrz も
		# GlobalAllocStats で override するため LNK2005 で衝突する。 ThorVG 側の
		# ガードを有効化し ThorVG 内部の new も krkrz の統計プールへ通す。
		if (TARGET thorvg)
			target_compile_definitions(thorvg PRIVATE THORVG_NO_GLOBAL_NEW_OVERRIDE=1)
		endif()

		# HarfBuzz の hb.hh は pragma で警告を error 化し、 新しめの clang (emsdk 等)
		# で FetchContent ビルドが止まる。 ガードマクロで抑止。
		if (EMSCRIPTEN AND TARGET harfbuzz)
			target_compile_definitions(harfbuzz PRIVATE HB_NO_PRAGMA_GCC_DIAGNOSTIC_ERROR)
		endif()
	endif()

	# 各 variant の target にリンクする Elements ライブラリ群。 elements_modal は
	# cycfi::elements を PUBLIC 依存 (→ thorvg も推移) するので elements_modal を
	# リンクすれば cycfi / thorvg の include・defines も伝播する。 cycfi も明示。
	if (TARGET cycfi::elements)
		list(APPEND KRKRZ_LIB_ELEMENTS cycfi::elements)
	endif()
	if (TARGET elements_modal)
		list(APPEND KRKRZ_LIB_ELEMENTS elements_modal::elements_modal)
	endif()

	# Elements 機構が組み込まれていることを示すマクロ。 DrawDevice / Window / TJS の
	# 共通コードは #ifdef KRKRZ_HAS_ELEMENTS でガードする。
	list(APPEND KRKRZ_DEFINES KRKRZ_HAS_ELEMENTS)
endif()

if (KRKRZ_VARIANT STREQUAL "LIB")

# ----------------------------------------------------
# krkrz generic library
# ----------------------------------------------------

message("krkrz generic library")

add_library( ${LIBNAME} EXCLUDE_FROM_ALL STATIC
	${KRKRZ_SRC} 
	${KRKRZ_SRC_OPENGL} 
	${KRKRZ_SRC_GENERIC} 
	${KRKRZ_SRC_PLATFORM}
	${KRKRZ_SRC_ADD}
)

set_target_properties(${LIBNAME} PROPERTIES 
    PUBLIC_HEADER "${KRKRZ_PUBLIC_HEADER}"
#   DEBUG_POSTFIX "_d"
)

target_include_directories( ${LIBNAME} PUBLIC
	${KRKRZ_INC}
	${KRKRZ_INC_GENERIC}
	${KRKRZ_INC_PLATFORM}
)

target_include_directories( ${LIBNAME} PRIVATE 
	${JPEG_INCLUDE_DIR}
	${PICOJSON_INCLUDE_DIRS}
	${CMAKE_BINARY_DIR}          # 生成 krkrz_version.h
)

target_compile_definitions( ${LIBNAME} PUBLIC
	${KRKRZ_DEFINES}
	__GENERIC__
	TVP_DONT_AUTOLOAD_PROVINCE
	TVP_DONT_AUTOLOAD_MASK
	${KRKRZ_DEFINES_PLATFORM}
	${KRKRZ_DEFINES_OPENGL}
	${KRKRZ_DEFINES_DAP}
)

target_link_libraries( ${LIBNAME} PUBLIC
	freetype
	${KRKRZ_LIB_GLYPHWARE}
	${JPEG_LIBRARIES}
	Ogg::ogg
	Vorbis::vorbis
	Vorbis::vorbisenc
	Vorbis::vorbisfile
	ZLIB::ZLIB
	oniguruma::onig
	PNG::PNG
	glm::glm
	$<IF:$<TARGET_EXISTS:Opus::opus>,Opus::opus,>
	${KRKRZ_LIB_REPL}
	${KRKRZ_LIB_ADD}
	${KRKRZ_LIB_DAP}
)

elseif(KRKRZ_VARIANT STREQUAL "SDL") # !LIB

# ----------------------------------------------------
# krkrz SDL version
# ----------------------------------------------------

# ----------------------------------------------------
# SDL3

if (NOT TARGET SDL3::SDL3)
	include(FetchContent)
	FetchContent_Declare(
		SDL3
		GIT_REPOSITORY https://github.com/libsdl-org/SDL.git
		GIT_TAG main
	)

	set(SDL_SHARED ON CACHE BOOL "Build SDL shared library")
	set(SDL_STATIC OFF CACHE BOOL "Build SDL static library")

	FetchContent_MakeAvailable(SDL3)

endif()

# ----------------------------------------------------
# movieplayer

if(KRKRZ_USE_MOVIE AND NOT TARGET movieplayer)
option(MOVIEPLAYER_EXTERNAL_MINIAUDIO "use external miniaudio" ON)
add_subdirectory(external/movie-player)
endif()

# Elements の build / link / KRKRZ_HAS_ELEMENTS は variant chain の前 (icline の直後)
# で WIN / SDL 共通に処理済み (KRKRZ_LIB_ELEMENTS / KRKRZ_DEFINES)。 SDL target は
# 下の target_link_libraries で ${KRKRZ_LIB_ELEMENTS} をリンクする。

# ----------------------------------------------------
# resource

# リソースデータ一式
# 既定はエンジン同梱の resource/ だが、案件では NX の setup/nxresource や
# Android の asset に相当する「案件リソースフォルダ」を埋め込みたい。
# -DKRKRZ_RESOURCE_DIR=<案件の nxresource> で差し替え可能にしておく
# (config.cf/messages.json/SysInitScript.tjs/フォント一式が案件版に入れ替わる)。
set(KRKRZ_RESOURCE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/resource" CACHE PATH
    "リソースフォルダ (既定=エンジン同梱。案件の nxresource で上書き可)")
set(RESOURCE_DIR ${KRKRZ_RESOURCE_DIR})
file(GLOB RESOURCE_FILES "${RESOURCE_DIR}/*")

if (WIN32)

# `.rc` ファイルを生成
set(RC_FILE ${CMAKE_BINARY_DIR}/resources.rc)
file(WRITE ${RC_FILE} "")

foreach(RESOURCE ${RESOURCE_FILES})
    get_filename_component(NAME ${RESOURCE} NAME)
    file(APPEND ${RC_FILE} "${NAME} BINARY \"${RESOURCE}\"\n")
endforeach()

# アプリアイコン。 案件側から -DKRKRZ_WIN_ICON=<.ico の絶対パス> で渡す。
# 生成 .rc はこれまで BINARY リソースしか吐いておらず、 exe にアイコン
# リソースが 1 つも無かった。 そのため後段でアイコンを差し替える類のツール
# (IconReset 等) も「リソースが見つからない」で失敗する。 ここで埋めておけば
# ビルド直後から案件のアイコンが付く。 名前を MAINICON にしてあるのは既存の
# 差し替えツールがその名前を前提にしているため (後から差し替えも効く)。
if (KRKRZ_WIN_ICON)
    if (EXISTS "${KRKRZ_WIN_ICON}")
        file(APPEND ${RC_FILE} "MAINICON ICON \"${KRKRZ_WIN_ICON}\"\n")
        list(APPEND RESOURCE_FILES "${KRKRZ_WIN_ICON}")   # 下の OBJECT_DEPENDS へ
        message(STATUS "krkrz: app icon = ${KRKRZ_WIN_ICON}")
    else()
        message(WARNING "krkrz: KRKRZ_WIN_ICON not found: ${KRKRZ_WIN_ICON}")
    endif()
endif()

# .rc は参照先バイナリの「内容」を含めて .res へ埋め込むが、ビルドツールは
# .rc 自身のタイムスタンプしか見ない。resource/*.json 更新時に .res が再生成
# されず、旧 messages.json 埋め込み exe が MsgLoad 側 (採番数増) と食い違い
# 起動時 picojson OOB で落ちる事故が起きたため、内容依存を明示する。
set_source_files_properties(${RC_FILE} PROPERTIES OBJECT_DEPENDS "${RESOURCE_FILES}")

elseif(APPLE)

# iOS または macOS の場合、リソースをアプリバンドルに追加
foreach(RESOURCE ${RESOURCE_FILES})
	set_source_files_properties(${RESOURCE} PROPERTIES MACOSX_PACKAGE_LOCATION Resources)
endforeach()

elseif(ANDROID)

# install でファイルをアプリに一式コピーする
# LINUX誤認防止で先に定義

elseif(EMSCRIPTEN AND NOT KRKRZ_EMSCRIPTEN_EMBED_RESOURCE)

# wasm: リソースは wasm へ埋め込まない (既定)。krkrz_web 側が preload バンドルで
# MEMFS /resource に upfront 配置し、エンジンは file://./resource/ から同期読みする
# (sdl3/environ/app.cpp の __EMSCRIPTEN__ 分岐)。バイナリがリソース非依存になり
# 案件横断で共有できる。よって resources ライブラリは作らない。
set(RESOURCE_LIB "")

elseif(EMSCRIPTEN)

# wasm には objcopy が使えないため、build 時にリソースを C 配列ソースへ変換して
# 埋め込む (cmake/Bin2C.cmake)。resource_table を消費する側は UNIX と同じ
# generic/app/objres.cpp をそのまま使う (シンボル名も objcopy 版と揃える)。
# end シンボルは作れないので、configure 時に file(SIZE) で得たサイズを
# 「start + サイズ」の定数式としてレジストリに焼き込む。

set(RESOURCE_OBJ_DIR ${CMAKE_BINARY_DIR}/resource_obj)
file(MAKE_DIRECTORY ${RESOURCE_OBJ_DIR})

set(REGISTRY_SOURCE_CONTENT "/* Auto-generated resource registry source */
#include <stddef.h>

/* Resource data arrays generated by cmake/Bin2C.cmake */
")
set(REGISTRY_ENTRIES "")
set(REGISTRY_COUNT 0)

foreach(RESOURCE ${RESOURCE_FILES})
    get_filename_component(NAME ${RESOURCE} NAME)
    string(REGEX REPLACE "[^a-zA-Z0-9]" "_" SYMBOL_NAME "${NAME}")
    set(C_FILE ${RESOURCE_OBJ_DIR}/${SYMBOL_NAME}.c)

    add_custom_command(
        OUTPUT ${C_FILE}
        COMMAND ${CMAKE_COMMAND}
            -DINPUT=${RESOURCE}
            -DOUTPUT=${C_FILE}
            -DSYMBOL=_binary_${SYMBOL_NAME}_start
            -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/Bin2C.cmake
        DEPENDS ${RESOURCE} ${CMAKE_CURRENT_SOURCE_DIR}/cmake/Bin2C.cmake
        COMMENT "Embedding resource ${NAME} as C array"
    )
    list(APPEND RESOURCE_OBJECTS ${C_FILE})

    file(SIZE ${RESOURCE} RESOURCE_SIZE)
    set(REGISTRY_SOURCE_CONTENT "${REGISTRY_SOURCE_CONTENT}
extern const char _binary_${SYMBOL_NAME}_start[];
")
    set(REGISTRY_ENTRIES "${REGISTRY_ENTRIES}    { \"${NAME}\", _binary_${SYMBOL_NAME}_start, _binary_${SYMBOL_NAME}_start + ${RESOURCE_SIZE} },\n")
    math(EXPR REGISTRY_COUNT "${REGISTRY_COUNT} + 1")
endforeach()

# リソースレジストリソースファイルを出力
set(RESOURCE_REGISTRY_C ${RESOURCE_OBJ_DIR}/resource_registry.c)
set(REGISTRY_SOURCE_CONTENT "${REGISTRY_SOURCE_CONTENT}
/* Resource entry structure */
typedef struct {
    const char* name;
    const char* start;
    const char* end;
} ResourceEntry;

/* Resource table */
const ResourceEntry resource_table[] = {
${REGISTRY_ENTRIES}};

const size_t resource_count = ${REGISTRY_COUNT};

")
file(WRITE "${RESOURCE_REGISTRY_C}" "${REGISTRY_SOURCE_CONTENT}")
message(STATUS "Generated resource registry (emscripten): ${RESOURCE_REGISTRY_C}")

set(RESOURCE_LIB resources)

# リソースを含むライブラリを作成
add_library(${RESOURCE_LIB} STATIC ${RESOURCE_OBJECTS} ${RESOURCE_REGISTRY_C})
set_property(TARGET ${RESOURCE_LIB} PROPERTY LINKER_LANGUAGE C)

elseif(UNIX)

# objcopy でバイナリを埋め込む
# objcopy のコマンドを探す
find_program(OBJCOPY objcopy)

# リソースオブジェクト出力ディレクトリ
set(RESOURCE_OBJ_DIR ${CMAKE_BINARY_DIR}/resource_obj)
file(MAKE_DIRECTORY ${RESOURCE_OBJ_DIR})

# リソースレジストリソースファイルの内容を生成
set(REGISTRY_SOURCE_CONTENT "/* Auto-generated resource registry source */
#include <stddef.h>
#include <string.h>

/* External symbols generated by objcopy */
")
set(REGISTRY_ENTRIES "")
set(REGISTRY_COUNT 0)
# objcopy の BFD target を CPU アーキに合わせて選択する
# (-I binary で取り込んだ生データを ELF オブジェクトに変換する際の出力フォーマット)
# CACHE にしてしまうと configure をまたいでアーキの異なる値が焼き付いたまま残り
# (-O elf64-x86-64 なのに -B aarch64 のような食い違いが出る) ため、毎 configure で
# KRKRZ_TARGET_ARM / CMAKE_SIZEOF_VOID_P を見て再計算する。明示的に上書きしたい場合は
# -DOBJFORMAT=... / -DOBJARCH=... を指定する。
if (KRKRZ_TARGET_ARM)
    if (CMAKE_SIZEOF_VOID_P EQUAL 8)
        set(OBJFORMAT_DEFAULT elf64-littleaarch64)
        set(OBJARCH_DEFAULT aarch64)
    else()
        set(OBJFORMAT_DEFAULT elf32-littlearm)
        set(OBJARCH_DEFAULT arm)
    endif()
else()
    if (CMAKE_SIZEOF_VOID_P EQUAL 8)
        set(OBJFORMAT_DEFAULT elf64-x86-64)
        set(OBJARCH_DEFAULT i386:x86-64)
    else()
        set(OBJFORMAT_DEFAULT elf32-i386)
        set(OBJARCH_DEFAULT i386)
    endif()
endif()
if (NOT DEFINED OBJFORMAT)
    set(OBJFORMAT ${OBJFORMAT_DEFAULT})
endif()
if (NOT DEFINED OBJARCH)
    set(OBJARCH ${OBJARCH_DEFAULT})
endif()
message(STATUS "Resource objcopy: -O ${OBJFORMAT} -B ${OBJARCH}")

# 各バイナリをオブジェクトファイルに変換 & レジストリエントリ生成
foreach(RESOURCE ${RESOURCE_FILES})
    get_filename_component(NAME ${RESOURCE} NAME)
    set(OBJ_FILE ${RESOURCE_OBJ_DIR}/${NAME}.o)
    
    # ファイル名をシンボル名に変換（特殊文字を_に置換）
    string(REGEX REPLACE "[^a-zA-Z0-9]" "_" SYMBOL_NAME "${NAME}")
    string(REGEX REPLACE "[^a-zA-Z0-9]" "_" ORIG_NAME "${RESOURCE}")

    add_custom_command(
        OUTPUT ${OBJ_FILE}
        COMMAND ${OBJCOPY} -I binary -O ${OBJFORMAT} -B ${OBJARCH}
            --redefine-sym _binary_${ORIG_NAME}_start=_binary_${SYMBOL_NAME}_start
            --redefine-sym _binary_${ORIG_NAME}_end=_binary_${SYMBOL_NAME}_end
            --redefine-sym _binary_${ORIG_NAME}_size=_binary_${SYMBOL_NAME}_size
            ${RESOURCE} ${OBJ_FILE}
        DEPENDS ${RESOURCE}
        WORKING_DIRECTORY ${RESOURCE_DIR}
        COMMENT "Converting ${NAME} to object file"
    )

    list(APPEND RESOURCE_OBJECTS ${OBJ_FILE})

    # レジストリエントリを追加
    set(REGISTRY_SOURCE_CONTENT "${REGISTRY_SOURCE_CONTENT}
extern const char _binary_${SYMBOL_NAME}_start[];
extern const char _binary_${SYMBOL_NAME}_end[];
")
    set(REGISTRY_ENTRIES "${REGISTRY_ENTRIES}    { \"${NAME}\", _binary_${SYMBOL_NAME}_start, _binary_${SYMBOL_NAME}_end },\n")
    math(EXPR REGISTRY_COUNT "${REGISTRY_COUNT} + 1")
endforeach()

# リソースレジストリソースファイルを出力
set(RESOURCE_REGISTRY_C ${RESOURCE_OBJ_DIR}/resource_registry.c)
set(REGISTRY_SOURCE_CONTENT "${REGISTRY_SOURCE_CONTENT}
/* Resource entry structure */
typedef struct {
    const char* name;
    const char* start;
    const char* end;
} ResourceEntry;

/* Resource table */
const ResourceEntry resource_table[] = {
${REGISTRY_ENTRIES}};

const size_t resource_count = ${REGISTRY_COUNT};

")
file(WRITE "${RESOURCE_REGISTRY_C}" "${REGISTRY_SOURCE_CONTENT}")
message(STATUS "Generated resource registry: ${RESOURCE_REGISTRY_C}")

set(RESOURCE_LIB resources)

# リソースを含むライブラリを作成
add_library(${RESOURCE_LIB} STATIC ${RESOURCE_OBJECTS} ${RESOURCE_REGISTRY_C})
set_property(TARGET ${RESOURCE_LIB} PROPERTY LINKER_LANGUAGE C)

endif()

# ----------------------------------------------------
# executable

if(ANDROID)

# SDL applications need to be built as a shared library
function(add_executable TARGET)
	add_library(${TARGET} SHARED ${ARGN})
endfunction()

endif()

if(APPLE)

add_executable(${EXENAME} MACOSX_BUNDLE
	${KRKRZ_SRC} 
	${KRKRZ_SRC_OPENGL} 
	${KRKRZ_SRC_GENERIC} 
	${KRKRZ_SRC_SDL3}
	${KRKRZ_SRC_PLATFORM}
	${KRKRZ_SRC_ADD}
	${RESOURCE_FILES}
)

else()

if (WIN32)
	# Windows では GUI サブシステム化し、起動時に親シェルのコンソールに attach する
	# (TVPAttachWindowsConsole())。CONSOLE サブシステムのままだと Git Bash / mintty
	# 経由で起動した場合に std ハンドルが winpty のパイプとなり、icline が要求する
	# 本物の Win32 console ハンドルを得られず REPL のエコー等が壊れるため。
	set(KRKRZ_SDL_EXE_TYPE WIN32)
else()
	set(KRKRZ_SDL_EXE_TYPE "")
endif()

add_executable(${EXENAME} ${KRKRZ_SDL_EXE_TYPE}
	${KRKRZ_SRC}
	${KRKRZ_SRC_OPENGL}
	${KRKRZ_SRC_GENERIC}
	${KRKRZ_SRC_SDL3}
	${KRKRZ_SRC_PLATFORM}
	${KRKRZ_SRC_ADD}
	${RC_FILE}
	${KRKRZ_VERSION_RC}
)

endif()

target_include_directories(${EXENAME} PRIVATE
	${KRKRZ_INC}
	${KRKRZ_INC_GENERIC}
	${KRKRZ_INC_SDL3}
	${KRKRZ_INC_PLATFORM}
	${JPEG_INCLUDE_DIR}
	${PICOJSON_INCLUDE_DIRS}
	${CMAKE_BINARY_DIR}          # 生成 krkrz_version.h
)

target_compile_definitions(${EXENAME} PRIVATE
	${KRKRZ_DEFINES}
	__GENERIC__
	TVP_DONT_AUTOLOAD_PROVINCE
	TVP_DONT_AUTOLOAD_MASK
	${KRKRZ_DEFINES_PLATFORM}
	${KRKRZ_DEFINES_OPENGL}
	${KRKRZ_DEFINES_DAP}
)

target_link_libraries( ${EXENAME} PRIVATE
	freetype
	${KRKRZ_LIB_GLYPHWARE}
	${JPEG_LIBRARIES}
	Ogg::ogg
	Vorbis::vorbis
	Vorbis::vorbisenc
	Vorbis::vorbisfile
	ZLIB::ZLIB
	oniguruma::onig
	PNG::PNG
	glm::glm
	$<IF:$<TARGET_EXISTS:Opus::opus>,Opus::opus,>
	$<IF:$<TARGET_EXISTS:movieplayer>,movieplayer,>
	${KRKRZ_LIB_REPL}
	${KRKRZ_LIB_SDL3}
	${KRKRZ_LIB_ELEMENTS}
	${KRKRZ_LIB_ADD}
	${KRKRZ_LIB_PLATFORM}
	${KRKRZ_LIB_DAP}
	${RESOURCE_LIB}
	$<$<PLATFORM_ID:Windows>:shell32>   # TVPExecuteProgram: ShellExecuteW (App Paths 解決)
)

if(APPLE)
foreach(PLUGIN ${KRKRZ_DYNAMIC_PLUGINS})
	add_dependencies(${EXENAME} ${PLUGIN})
	add_custom_command(TARGET ${EXENAME} POST_BUILD
		COMMAND ${CMAKE_COMMAND} -E copy_if_different
			$<TARGET_FILE:${PLUGIN}>
			"$<TARGET_FILE_DIR:${EXENAME}>/"
	)
endforeach()
endif()

# ----------------------------------------------------
# install

if (ANDROID)

elseif(SDL_SHARED)

	# KRKRZ_LIB_SDL3 はリンク用と「ランタイムライブラリの配布」用に共用しているが、
	# cycfi::elements / elements_modal などの静的ライブラリ (.lib/.a) は exe に
	# 静的リンクされるので配布・install 対象にしない。 SHARED (DLL/.so) のみ拾う。
	set(KRKRZ_LIB_SDL3_RUNTIME)
	foreach(LIB ${KRKRZ_LIB_SDL3})
		set(_lib_target ${LIB})
		get_target_property(_aliased ${_lib_target} ALIASED_TARGET)
		if(_aliased)
			set(_lib_target ${_aliased})
		endif()
		get_target_property(_lib_type ${_lib_target} TYPE)
		if(_lib_type STREQUAL "SHARED_LIBRARY")
			list(APPEND KRKRZ_LIB_SDL3_RUNTIME ${LIB})
		endif()
	endforeach()

	# 実行ディレクトリの場所にコピー
	foreach(LIB ${KRKRZ_LIB_SDL3_RUNTIME})
		add_custom_command(TARGET ${EXENAME} POST_BUILD
			COMMAND ${CMAKE_COMMAND} -E copy_if_different
				$<TARGET_FILE:${LIB}>
				$<TARGET_FILE_DIR:${EXENAME}>
		)
	endforeach()

	install(TARGETS ${EXENAME}
		RUNTIME DESTINATION .
		LIBRARY DESTINATION .
		BUNDLE DESTINATION .
	)

	if (NOT APPLE)
		# SDL3 の DLL も一緒にインストール
		foreach(LIB ${KRKRZ_LIB_SDL3_RUNTIME})
			install(FILES $<TARGET_FILE:${LIB}> DESTINATION .)
		endforeach()
	endif()

else()

	install(TARGETS ${EXENAME}
			RUNTIME DESTINATION .
			BUNDLE DESTINATION .
	)

endif()

elseif(KRKRZ_VARIANT STREQUAL "WIN") # !SDL

# ----------------------------------------------------
# krkrz original win32 version
# ----------------------------------------------------

if (WIN32)
message("krkrz original based win32 executable")
else()
message(FATAL_ERROR "Unsupported platform")
endif()

# Track V-A: krmovie は exe へ直接統合 (win32/movie/*.cpp を KRKRZ_SRC_WIN32 に畳み込み、
# tp_stub 境界を撤去)。動画デコーダの movie-player (webm/VP8-9) だけ static lib として
# 取り込み PRIVATE リンクする (pl_mpeg はヘッダオンリで KRKRZ_INC_WIN32 に追加済み)。
if(NOT TARGET movieplayer)
	add_subdirectory(external/movie-player)
endif()

# krkrz executable

# for LogImp.cpp
find_package(plog CONFIG)

# リソース埋め込み (WINVER)。既存の win32/vcproj/tvpwin32.rc は IDR_* の
# SysInitScript / messages / license / optiondesc を埋め込むが、Elements dialog 用の
# フォント (resource/*.otf,*.ttf) 等は含まない。SDL ブランチと同様に resource/ 配下を
# "BINARY" 型リソースとして生成 rc で埋め込み、TVPRegisterElementsFontsFromWinResources
# が EnumResourceNamesW("BINARY") で列挙して register_font_buffer で登録できるようにする。
# (RESOURCE_DIR / RESOURCE_FILES は SDL ブランチ内ローカルなのでここで再定義する。)
set(KRKRZ_RESOURCE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/resource" CACHE PATH
    "リソースフォルダ (既定=エンジン同梱。案件の nxresource で上書き可)")
set(RESOURCE_DIR ${KRKRZ_RESOURCE_DIR})
file(GLOB RESOURCE_FILES "${RESOURCE_DIR}/*")
set(RC_FILE ${CMAKE_BINARY_DIR}/resources.rc)
file(WRITE ${RC_FILE} "")
foreach(RESOURCE ${RESOURCE_FILES})
    get_filename_component(NAME ${RESOURCE} NAME)
    file(APPEND ${RC_FILE} "${NAME} BINARY \"${RESOURCE}\"\n")
endforeach()
# resource 内容変更で .res を再生成させる (SDL ブランチ側と同趣旨)
set_source_files_properties(${RC_FILE} PROPERTIES OBJECT_DEPENDS "${RESOURCE_FILES}")

add_executable( ${EXENAME} WIN32
	${KRKRZ_SRC}
	${KRKRZ_SRC_OPENGL}
	${KRKRZ_SRC_WIN32}
	${KRKRZ_SRC_WIN32_SSE}
	${KRKRZ_SRC_WIN32_OPENGL}
	${KRKRZ_SRC_ADD}
	${RC_FILE}
	${KRKRZ_VERSION_RC}
)

target_include_directories( ${EXENAME} PRIVATE 
	${KRKRZ_INC}
	${KRKRZ_INC_WIN32} 
	${JPEG_INCLUDE_DIR}
	${PICOJSON_INCLUDE_DIRS}
	${CMAKE_BINARY_DIR}          # 生成 krkrz_version.h
)

target_compile_definitions( ${EXENAME} PRIVATE
	${KRKRZ_DEFINES}
	NO_STRICT
	__WINVER__
	TJS_DEBUG_DUMP_STRING
	TVP_REPORT_HW_EXCEPTION
	${KRKRZ_DEFINES_PLATFORM}
	${KRKRZ_DEFINES_OPENGL}
	${KRKRZ_DEFINES_DAP}
)

target_link_libraries( ${EXENAME} PRIVATE
	freetype
	${KRKRZ_LIB_GLYPHWARE}
	${JPEG_LIBRARIES}
	Ogg::ogg
	Opus::opus
	Vorbis::vorbis
	Vorbis::vorbisenc
	Vorbis::vorbisfile
	ZLIB::ZLIB
	oniguruma::onig
	PNG::PNG
	glm::glm
 	plog::plog
	d3d11
	dxgi
	d3dcompiler
	shlwapi
	shell32   # TVPExecuteProgram: ShellExecute (App Paths 解決)
	movieplayer
	${KRKRZ_LIB_REPL}
	${KRKRZ_LIB_ELEMENTS}
	${KRKRZ_LIB_ADD}
	${KRKRZ_LIB_DAP}
)

install(TARGETS ${EXENAME}
	DESTINATION .
)

endif() # KRKRZ_VARIANT

# ----------------------------------------------------
# ライセンス文の配布同梱
#   - LICENSE → license.txt (配布物ルート)
#   - licenses/manifest.json の全エントリ → licenses/<slug>.txt
#     (本体内蔵テーブルと同じ収録内容をテキストとしても同梱する。
#      仕組みは doc/LicenseSystem.md 参照)
# ----------------------------------------------------
install(FILES ${CMAKE_CURRENT_SOURCE_DIR}/LICENSE DESTINATION . RENAME license.txt)
file(READ ${CMAKE_CURRENT_SOURCE_DIR}/licenses/manifest.json _LIC_JSON)
string(JSON _LIC_COUNT LENGTH ${_LIC_JSON} entries)
math(EXPR _LIC_LAST "${_LIC_COUNT} - 1")
foreach(_LIC_I RANGE ${_LIC_LAST})
	string(JSON _LIC_NAME GET ${_LIC_JSON} entries ${_LIC_I} name)
	string(JSON _LIC_FILE GET ${_LIC_JSON} entries ${_LIC_I} file)
	string(TOLOWER "${_LIC_NAME}" _LIC_SLUG)
	string(REGEX REPLACE "[^a-z0-9.+_]+" "-" _LIC_SLUG "${_LIC_SLUG}")
	string(REGEX REPLACE "^-+" "" _LIC_SLUG "${_LIC_SLUG}")
	string(REGEX REPLACE "-+$" "" _LIC_SLUG "${_LIC_SLUG}")
	get_filename_component(_LIC_ABS "${CMAKE_CURRENT_SOURCE_DIR}/licenses/${_LIC_FILE}" ABSOLUTE)
	if(EXISTS "${_LIC_ABS}")
		install(FILES "${_LIC_ABS}" DESTINATION licenses RENAME "${_LIC_SLUG}.txt")
	else()
		message(WARNING "license text not found: ${_LIC_NAME} (${_LIC_ABS})")
	endif()
endforeach()

# ----------------------------------------------------
# tests (Phase 3: SIMD パリティテスト)
# ----------------------------------------------------
if (KRKRZ_BUILD_TESTS AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/tests/CMakeLists.txt")
    enable_testing()
    add_subdirectory(tests)
endif()
