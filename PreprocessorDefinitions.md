# デバッグオプション一覧
作成中。  
順次列挙予定。

### TVP\_TEXT\_READ\_ANSI\_MBCS
テキストをShift_JISとして読み込みます。
デフォルトは未定義でUTF-8として読み込まれます。

### TVP\_TOUCH\_DISABLE
Window.enableTouchをfalseとします。
デフォルトは未定義でタッチデバイスがありマルチタッチが有効な環境では、trueとなります。

### TVP_START_UP_SCRIPT_NAME
初期読み込みスクリプト名を指定します。
デフォルトは未定義でstartup.tjsを読み込みます。

### TJS\_64BIT\_OS
64bit環境かどうかを切り分けるために内部で使用されています。

### KRKRZ\_ENABLE\_DAP (Phase 2 以降で導入予定)
VSCode Debug Adapter Protocol サーバを内蔵してビルドします。
デフォルトは ON。`-dap=<port>` コマンドラインオプションでサーバを起動。

旧 `ENABLE_DEBUGGER` (Win32 専用 `WM_COPYDATA` ベースの独自プロトコル) は
2026-04-25 に廃止されました。代わりに上記 DAP 経由で VSCode から
ブレークポイント・ステップ実行・変数 inspect が可能になります。

### TVP\_USE\_LOGCORE
LogCore (`common/utils/LogCore.cpp`) を有効にしてビルドします。
CMake オプション `KRKRZ_USE_LOGCORE` で制御され、デスクトップ環境ではデフォルト ON、
モバイル環境 (Android / iOS) ではデフォルト OFF です。

有効時は以下の機能が利用可能:
- ログのリングバッファ (`Debug.getLastLog()`)
- ファイル出力 (`krkr.console.log`)
- TJS logging handler (`Debug.addLoggingHandler()`)
- REPL コンソール sink

無効時は SDL3 のネイティブログ出力をそのまま使用し、上記機能はスタブ実装となります。
詳細は `doc/Logging.md` を参照してください。

    // 以下未整理
    TJS_TEXT_OUT_CRLF
    TJS_SUPPORT_VCL
    TJS_HOST_IS_BIG_ENDIAN
    TJS_DEBUG_DUMP_STRING
    TJS_DEBUG_TRACE
    TJS_DEBUG_PROFILE_TIMETJS_DEBUG_UNRELEASED_STRING
    TJS_DEBUG_CHECK_STRING_HEAP_INTEGRITY
    TJS_DEBUG_CHECK_STRING_HEAP_INTEGRITY
    TJS_HS_DEBUG_CHAIN
    TJS_STRICT_ERROR_CODE_CHECK
    TJS_NO_REGEXP
    TJS_VS_USE_SYSTEM_NEW
    TJS_NO_CONSTANT_FOLDING
    TJS_NO_MASK_MATHERR
    TJS_WITH_IS_NOT_RESERVED_WORD
    TVP_ENABLE_EXECUTE_AT_EXCEPTION
    TVP_NO_CHECK_WIDE_CHAR_SIZE
    TVP_SUPPORT_KPI
    TVP_SUPPORT_OLD_WAVEUNPACKER
    TVP_REPORT_HW_EXCEPTION
    TVP_DISABLE_SELECT_XP3_OR_FOLDER
    TVP_IGNORE_LOAD_TPM_PLUGIN
    TVP_LOG_TO_COMMANDLINE_CONSOLE
    TVP_IN_LOOP_TUNER
    TVP_IN_PLUGIN_STUB
    TVP_FORCE_BILINEAR
    TVP_TRANS_SHOW_FPS
    
    // 以下リリースビルド時有効なもの
    TJS_TEXT_OUT_CRLF
    TJS_JP_LOCALIZED
    TJS_DEBUG_DUMP_STRING
    TVP_LOG_TO_COMMANDLINE_CONSOLE
    DISABLE_EMBEDDED_GAME_PAD
    TVP_REPORT_HW_EXCEPTION
    TVP_ENABLE_EXECUTE_AT_EXCEPTION

    TVP_NO_NORMALIZE_PATH               パス取得時にノーマライズしない
    TVP_AUTOPATH_IGNORECASE             自動パス検索時に case を無視する
    TVP_LOCALFILE_FORCE_CASESENSITIVE   ローカルパス検索時に caseを強制参照（WIN用）
