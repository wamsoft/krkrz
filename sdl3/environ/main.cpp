#include "tjsCommHead.h"
#include "CharacterSet.h"
#include "EventIntf.h"
#include "LogIntf.h"
#include "SysInitIntf.h"
#include "SystemIntf.h"
#ifdef KRKRZ_USE_REPL
#include "REPL.h"
#endif
// tjsDebuggerCore.h は DAP 関数 (TVPCreateDAP 等) も宣言する。 DAP は REPL とは
// 独立に有効化できる (KRKRZ_ENABLE_DAP) ので、 REPL=OFF + DAP=ON でも include する。
#if defined(KRKRZ_USE_REPL) || defined(KRKRZ_ENABLE_DAP)
#include "tjsDebuggerCore.h"
#endif
#include "WinConsole.h"
#include "GlobalAllocStats.h"   // SDL_SetMemoryFunctions wrapper の入口
#include "app.h"

#ifdef KRKRZ_HAS_ELEMENTS
#include "elements/ElementsUserConfig.h"
#endif

#define SDL_MAIN_USE_CALLBACKS
#include <SDL3/SDL_main.h>

// 最後に押されたパッドをメインパッドにする
// 0: 無効 (旧挙動: 最初に認識されたパッドを保持、それが切断されない限り別パッドに
//          切り替わらない)
// 1: 有効 (現在の既定。ボタン or タッチパッド DOWN が来たパッドを以後のメインとする)
//
// 複数パッド対応は別課題。本機能はあくまで「1 枚のメインパッドを最後に触ったものに
// 追従させる」だけ。ボタン入力ベースなのでスティックドリフトでは切り替わらない。
#ifndef USE_LAST_PUSHDOWN_PAD
#define USE_LAST_PUSHDOWN_PAD 1
#endif

SDL_Joystick *joystick = NULL;
SDL_Gamepad *gamepad = NULL;
int gamepad_count = 0;

tjs_string SDL3Application::GetJoypadType(int no)
{
    if (gamepad == NULL) {
        return TJS_W("");
    }
    tjs_string name;
    TVPUtf8ToUtf16(name, SDL_GetGamepadName(gamepad));
    return name;
}

bool SDL3Application::RumbleGamepad(int no, int low, int high, int duration_ms)
{
    if (gamepad == NULL) {
        return false;
    }
    // 0〜255 を 0〜0xFFFF にスケール
    Uint16 low16 = (Uint16)((low * 0xFFFF) / 255);
    Uint16 high16 = (Uint16)((high * 0xFFFF) / 255);
    return SDL_RumbleGamepad(gamepad, low16, high16, (Uint32)duration_ms);
}

bool SDL3Application::StopRumbleGamepad(int no)
{
    if (gamepad == NULL) {
        return false;
    }
    return SDL_RumbleGamepad(gamepad, 0, 0, 0);
}

tjs_int SDL3Application::GetJoypadCount()
{
    int count = 0;
    SDL_JoystickID *ids = SDL_GetGamepads(&count);
    if (ids) {
        SDL_free(ids);
    }
    return count;
}

bool SDL3Application::HasJoypad(int no)
{
    // 現状は no=0 のメインゲームパッドのみ対応
    if (no == 0) {
        return gamepad != NULL;
    }
    return false;
}

static void ShowGamepadInfo(const char *state)
{
    SDL_JoystickID *ids = SDL_GetGamepads(&gamepad_count);
    if (gamepad_count == 0) {
        SDL_Log("%s:No gamepads connected", state);
    } else {
        SDL_Log("%s:Connected gamepads:", state);
        for (int i = 0; i < gamepad_count; ++i) {
            SDL_Gamepad *gp = SDL_OpenGamepad(ids[i]);
            if (gp) {
                SDL_Log("Gamepad ID %u: %s", ids[i], SDL_GetGamepadName(gp));
                SDL_CloseGamepad(gp);
            } else {
                SDL_Log("Failed to open gamepad ID %u: %s", ids[i], SDL_GetError());
            }
        }
        SDL_free(ids);
    }
}

SDL_AppResult SDL_AppEvent(void *appstate, SDL_Event *event)
{
    switch (event->type) {
    case SDL_EVENT_QUIT:
        return SDL_APP_SUCCESS; // アプリを正常終了
    
    case SDL_EVENT_WILL_ENTER_BACKGROUND:
        // バックグラウンドに入る前に描画を停止
        {
            SDL3Application *app = static_cast<SDL3Application *>(appstate);
            if (app) {
                SDL_Log("App will enter background");
                app->SetInBackground(true);
            }
        }
        break;

    case SDL_EVENT_DID_ENTER_BACKGROUND:
        // バックグラウンドに完全に入った（リソース解放などに使う）
        SDL_Log("App did enter background");
        break;

    case SDL_EVENT_WILL_ENTER_FOREGROUND:
        // フォアグラウンドに戻る直前（リソース再取得準備）
        SDL_Log("App will enter foreground");
        break;

    case SDL_EVENT_DID_ENTER_FOREGROUND:
        // フォアグラウンドに戻ったら描画を再開
        {
            SDL3Application *app = static_cast<SDL3Application *>(appstate);
            if (app) {
                SDL_Log("App did enter foreground");
                app->SetInBackground(false);
            }
        }
        break;

    case SDL_EVENT_LOW_MEMORY:
        // メモリ不足警告 - キャッシュ解放等を行う。
        // P4 で MAX → MINIMIZE に降格 (doc/legacy/ImagePreloadAndCache.md §18.2 C)。
        // pinned (UI 等) は残し transient のみ全消しする。
        // OOM 一歩手前なので解放はするが、復帰時の UI 再ロードコストは避けたい。
        SDL_Log("Low memory warning - releasing transient caches");
        TVPDeliverCompactEvent(TVP_COMPACT_LEVEL_MINIMIZE);
        break;

    case SDL_EVENT_LOCALE_CHANGED:
        // ロケール変更
        SDL_Log("Locale changed");
        break;

    case SDL_EVENT_SYSTEM_THEME_CHANGED:
        // システムテーマ変更（ダーク/ライトモード）
        SDL_Log("System theme changed");
        break;

    case SDL_EVENT_TERMINATING:
        // アプリ終了処理
        {
            TVPFireOnApplicationTerminating();
            SDL3Application *app = static_cast<SDL3Application *>(appstate);
            if (app) {
                app->OnTerminatingEnd();
            }
        }
        break;

    case SDL_EVENT_WINDOW_SHOWN:
        // ウィンドウが表示されたときの処理
        {
            SDL_Window *window = SDL_GetWindowFromEvent(event);
            if (window) {
                SDL_Log("Window shown: %s", SDL_GetWindowTitle(window));
            }
            ShowGamepadInfo("window show"); // Show connected gamepads info
        }
        break;
    
    case SDL_EVENT_JOYSTICK_ADDED:
        /* this event is sent for each hotplugged stick, but also each already-connected joystick during SDL_Init(). */
        if (joystick == NULL) {  /* we don't have a stick yet and one was added, open it! */
            joystick = SDL_OpenJoystick(event->jdevice.which);
            if (!joystick) {
                SDL_Log("Failed to open joystick ID %u: %s", (unsigned int) event->jdevice.which, SDL_GetError());
            }
        } else {
            SDL_Log("Joystick already opened, ignoring additional add event for ID %u", (unsigned int) event->jdevice.which);
        }
        break;
    
    case SDL_EVENT_JOYSTICK_REMOVED:
        if (joystick && (SDL_GetJoystickID(joystick) == event->jdevice.which)) {
            SDL_CloseJoystick(joystick);  /* our joystick was unplugged. */
            joystick = NULL;
        }
        break;
    
    case SDL_EVENT_GAMEPAD_ADDED:
        /* this event is sent for each hotplugged stick, but also each already-connected joystick during SDL_Init(). */
        if (gamepad == NULL) {  /* we don't have a stick yet and one was added, open it! */
            gamepad = SDL_OpenGamepad(event->gdevice.which);
            if (!gamepad) {
                SDL_Log("Failed to open gamepad ID %u: %s", (unsigned int) event->gdevice.which, SDL_GetError());
            } else {
                tjs_string name;
                TVPUtf8ToUtf16(name, SDL_GetGamepadName(gamepad));
                TVPFireOnJoypadChange(0, name.c_str());
            }
        } else {
            SDL_Log("Gamepad already opened, ignoring additional add event for ID %u", (unsigned int) event->gdevice.which);
        }
        ShowGamepadInfo("added"); // Show connected gamepads info
        break;

    case SDL_EVENT_GAMEPAD_REMOVED:
        if (gamepad && (SDL_GetGamepadID(gamepad) == event->gdevice.which)) {
            SDL_CloseGamepad(gamepad);  /* our joystick was unplugged. */
            gamepad = NULL;
            TVPFireOnJoypadChange(0, TJS_W(""));
        }
        ShowGamepadInfo("removed"); // Show connected gamepads info
        break;

#if USE_LAST_PUSHDOWN_PAD

    case SDL_EVENT_GAMEPAD_BUTTON_DOWN:
    case SDL_EVENT_GAMEPAD_TOUCHPAD_DOWN:
        // 最後にボタンを押したパッドをメインパッドにする処理
        if (!gamepad || (gamepad && (SDL_GetGamepadID(gamepad) != event->gbutton.which))) {
            auto newgamepad = SDL_OpenGamepad(event->gdevice.which);
            if (newgamepad) {
                if (gamepad) {
                    SDL_CloseGamepad(gamepad);
                }
                gamepad = newgamepad;
                SDL_Log("change main gamepad ID %u", (unsigned int) event->gdevice.which);
                // TJS 側に「メインパッドが切り替わった」を通知 (ADDED / REMOVED と
                // 同経路)。これがないとパッド種別表示が古いままになる。
                tjs_string name;
                TVPUtf8ToUtf16(name, SDL_GetGamepadName(gamepad));
                TVPFireOnJoypadChange(0, name.c_str());
            } else {
                SDL_Log("Failed to open gamepad ID %u: %s", (unsigned int) event->gdevice.which, SDL_GetError());
            }
        }
        break;
#endif


    }
	SDL3Application *app = static_cast<SDL3Application *>(appstate);	
	if (app) {
		return app->AppEvent(*event); // イベントを処理
	}
	return SDL_APP_CONTINUE; // イベントを無視
}

extern void InitAudioSystem();
extern void DoneAudioSystem();

extern void DoneStorageSystem();

// 機種別グローバル初期化処理（実体は各プラットフォーム別の app ファイルで定義）
extern void TVPAppInit(int argc, char *argv[]);

SDL_AppResult SDL_AppInit(void **appstate, int argc, char *argv[])
{
#ifdef KRKRZ_SDLMEMORY_STAT
    // SDL3 内部の malloc/calloc/realloc/free を本体の Sdl collector に redirect。
    // SDL_Init / SDL_LogInfo 等あらゆる SDL 関数より前で差し替える必要がある
    // (それ以降に呼ばれた SDL alloc を全部捕捉するため)。失敗しても致命傷
    // ではないので戻り値は無視。fallback path も用意してあるので、抜け漏れ
    // が一部あっても crash はしない。
    //
    // KRKRZ_SDLMEMORY_STAT=OFF (デフォルト) では SDL のデフォルトアロケータを
    // そのまま使う。NX 等で SDL_SetMemoryFunctions が動作しない/問題を起こす
    // 環境への対応。
    SDL_SetMemoryFunctions(&TVPGlobalAllocStats::SdlMalloc,
                           &TVPGlobalAllocStats::SdlCalloc,
                           &TVPGlobalAllocStats::SdlRealloc,
                           &TVPGlobalAllocStats::SdlFree);
#endif

    // Windows で GUI サブシステム化した場合、親シェルのコンソールに attach して
    // stdout/stderr (ログ出力) と REPL の stdin を可視化する。
    // 非 Windows / 既にコンソールを持っている場合は no-op。
    TVPAttachWindowsConsole();

    // ログレベル設定
#ifdef MASTER
    TVPLogInit(TVPLOG_LEVEL_WARNING);
#else
    {
#ifdef NDEBUG
        TVPLogLevel logLevel = TVPLOG_LEVEL_INFO;
#else
        TVPLogLevel logLevel = TVPLOG_LEVEL_DEBUG;
#endif
        // -loglevel=<level> をパース。値は大文字小文字どちらでも受け付ける。
        // 注意: NDEBUG ビルドではコンパイル時 TVPLOG_LEVEL=INFO で固定され、
        // TVPLOG_DEBUG() マクロが no-op に展開されるため、実行時に
        // logLevel を DEBUG にしても DEBUG ログは出ない。
        // DEBUG ログを出すには Debug ビルドにするか、cmake 構成時に
        // -DTVPLOG_LEVEL=1 (=DEBUG) を渡してビルドする必要がある。
        const char *prefix = "-loglevel=";
        const size_t prefix_len = strlen(prefix); // 10
        auto eq_icmp = [](const char *a, const char *b) {
            for (; *a && *b; ++a, ++b) {
                char ca = (*a >= 'a' && *a <= 'z') ? (*a - 'a' + 'A') : *a;
                char cb = (*b >= 'a' && *b <= 'z') ? (*b - 'a' + 'A') : *b;
                if (ca != cb) return false;
            }
            return *a == '\0' && *b == '\0';
        };
        for (int i = 1; i < argc; i++) {
            if (strncmp(argv[i], prefix, prefix_len) != 0) continue;
            const char *level = argv[i] + prefix_len;
            if (eq_icmp(level, "ERROR")) {
                logLevel = TVPLOG_LEVEL_ERROR;
            } else if (eq_icmp(level, "WARNING")) {
                logLevel = TVPLOG_LEVEL_WARNING;
            } else if (eq_icmp(level, "INFO")) {
                logLevel = TVPLOG_LEVEL_INFO;
            } else if (eq_icmp(level, "DEBUG")) {
                logLevel = TVPLOG_LEVEL_DEBUG;
            } else if (eq_icmp(level, "VERBOSE")) {
                logLevel = TVPLOG_LEVEL_VERBOSE;
            }
            break;
        }
        TVPLogInit(logLevel);
    }
#endif

    // 各機種別のグローバル初期化処理を呼び出す SDL_Init より前のタイミング
    TVPAppInit(argc, argv);

    if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_GAMEPAD | SDL_INIT_AUDIO)) {
        SDL_Log("SDL_Init failed: %s", SDL_GetError());
        return SDL_APP_FAILURE;
	}
    //SDL_AddGamepadMappingsFromFile("gamecontrollerdb.txt");

#ifdef TVP_USE_OPENGL
	// OpenGLESコンテキストの設定
	SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_ES);
	SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
	SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 2);
    SDL_GL_SetAttribute(SDL_GL_RED_SIZE, 8);
    SDL_GL_SetAttribute(SDL_GL_GREEN_SIZE, 8);
    SDL_GL_SetAttribute(SDL_GL_BLUE_SIZE, 8);
    SDL_GL_SetAttribute(SDL_GL_ALPHA_SIZE, 8);
    SDL_GL_SetAttribute(SDL_GL_STENCIL_SIZE, 8);
#endif

    // resource:// などの追加ストレージシステム初期化のため先に呼び出す
    TVPStartup();

    SDL3Application *app = GetSDL3Application();
    app->SetTitle(TJS_W("krkrz"));
	app->InitArgs(argc, argv);

    if (!app->InitPath()) {
        TVPLOG_ERROR("Failed to initialize paths");
        delete app;
        return SDL_APP_FAILURE;
    }

    app->AppInit();

	InitAudioSystem();

	// アプリ初期化
	if (!app->InitializeApplication()) {
#ifdef KRKRZ_HAS_ELEMENTS
		// `-userconf` フローが正常終了した場合は graceful exit。
		if (TVPGetUserConfigExitFlag()) {
			delete app;
			DoneAudioSystem();
			return SDL_APP_SUCCESS;
		}
#endif
		TVPLOG_ERROR("failed to initialize application");
        delete app;
        DoneAudioSystem();
        return SDL_APP_FAILURE;
	}

    // GlobalAllocStats プール初期化。Application + InitPath が済んで
    // TVPGetCommandLine が使える状態になったので、ここで `-krkrzpoolsize` /
    // `-sdlpoolsize` (CLI / .cf) を読み取って pool 構築 + tracking flag を on。
    // これより前の SDL_Init / config 読み出し時の alloc は全部素 malloc 直行
    // (オーバーヘッドゼロ、stats 対象外) で動く。
    TVPGlobalAllocStats::Initialize();

#ifdef KRKRZ_ENABLE_DAP
	// DAP server 起動 (-dap=<port> 指定時のみ)。
	// マルチフレーム stack trace のため、StackTracer をスクリプト起動より
	// 前にセットアップしておく必要がある (起動後だと最初の関数呼び出し
	// frame が記録されない)。
	TVPCreateDAP();
#endif

	// スクリプト起動開始
	app->Startup();

#ifdef KRKRZ_USE_REPL
	// REPL thread 起動 (TTY 有効時または -repl 指定時のみ)
	TVPCreateREPL();
#endif

    app->AppInitDone();
    app->OnTerminatingStart();

    *appstate = app;
    return SDL_APP_CONTINUE;
}

void SDL_AppQuit(void *appstate, SDL_AppResult result) 
{
    if (joystick) {
        SDL_CloseJoystick(joystick);
        joystick = NULL;
    }

#ifdef KRKRZ_USE_REPL
    TVPDestroyREPL();
#endif
#ifdef KRKRZ_ENABLE_DAP
    TVPDestroyDAP();
#endif

    SDL3Application *app = static_cast<SDL3Application *>(appstate);
    if (app) {
        TVPSystemUninit();
        app->AppQuit();
        delete app;
    }

    DoneAudioSystem();
    DoneStorageSystem();

    TVPDetachWindowsConsole();
}

SDL_AppResult SDL_AppIterate(void *appstate)
{
#ifdef KRKRZ_USE_REPL
    // REPL ワーカーからのリクエストを 1 件ドレインしてから通常処理へ
    TVPDrainREPL();
#endif
#ifdef KRKRZ_ENABLE_DAP
    TVPDrainDAP();
#endif

    SDL3Application *app = static_cast<SDL3Application *>(appstate);
    if (app) {
        app->AppIterate();
        app->SendPadEvent();
		if (!app->IsInBackground()) {
			app->RequestUpdate(); // 画面再描画用に常に更新要求を送る
		}
		app->Dispatch();
		if (app->IsTerminated()) {
			return app->TerminateCode() ? SDL_APP_FAILURE : SDL_APP_SUCCESS;
		}
    }
    return SDL_APP_CONTINUE;
}
