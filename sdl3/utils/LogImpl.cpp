#include "tjsCommHead.h"
#include "CharacterSet.h"
#include "LogIntf.h"

#include <SDL3/SDL.h>
#include <string>

//---------------------------------------------------------------------------
// TVP_USE_LOGCORE は CMake で定義される
// - デスクトップ (Windows, Linux, macOS): LogCore 経由でリングバッファ等を処理
// - モバイル (Android, iOS): SDL3 のネイティブログ出力を直接使用
//   (Android → logcat, iOS → os_log)
//---------------------------------------------------------------------------

// TVPLogLevelからSDL_LogPriorityへの変換テーブル
static SDL_LogPriority TVPLogLevelToSDLPriority(TVPLogLevel level)
{
    switch (level) {
        case TVPLOG_LEVEL_VERBOSE:  return SDL_LOG_PRIORITY_VERBOSE;
        case TVPLOG_LEVEL_DEBUG:    return SDL_LOG_PRIORITY_DEBUG;
        case TVPLOG_LEVEL_INFO:     return SDL_LOG_PRIORITY_INFO;
        case TVPLOG_LEVEL_WARNING:  return SDL_LOG_PRIORITY_WARN;
        case TVPLOG_LEVEL_ERROR:    return SDL_LOG_PRIORITY_ERROR;
        case TVPLOG_LEVEL_CRITICAL: return SDL_LOG_PRIORITY_CRITICAL;
        case TVPLOG_LEVEL_OFF:
        default:                    return SDL_LOG_PRIORITY_CRITICAL;
    }
}

// SDL_LogPriority → TVPLogLevel。LogCore 経路 (TVPSDLLogOutput) と、非 LogCore の
// コンソール sink 転送 (TVPSDLLogOutputSink, REPLWEB 時のみ) の双方で使う。
#if defined(TVP_USE_LOGCORE) || defined(KRKRZ_REPL_WEB)
static TVPLogLevel TVPSDLPriorityToLogLevel(SDL_LogPriority pri)
{
    switch (pri) {
        case SDL_LOG_PRIORITY_VERBOSE:  return TVPLOG_LEVEL_VERBOSE;
        case SDL_LOG_PRIORITY_DEBUG:    return TVPLOG_LEVEL_DEBUG;
        case SDL_LOG_PRIORITY_INFO:     return TVPLOG_LEVEL_INFO;
        case SDL_LOG_PRIORITY_WARN:     return TVPLOG_LEVEL_WARNING;
        case SDL_LOG_PRIORITY_ERROR:    return TVPLOG_LEVEL_ERROR;
        case SDL_LOG_PRIORITY_CRITICAL: return TVPLOG_LEVEL_CRITICAL;
        default:                        return TVPLOG_LEVEL_INFO;
    }
}
#endif

void TVPLogSetLevel(TVPLogLevel logLevel)
{
    SDL_LogPriority priority = TVPLogLevelToSDLPriority(logLevel);
    SDL_SetLogPriority(SDL_LOG_CATEGORY_APPLICATION, priority);
    SDL_SetLogPriority(SDL_LOG_CATEGORY_SYSTEM, priority);
}

#ifdef TVP_USE_LOGCORE
//---------------------------------------------------------------------------
// SDL のログ出力関数を差し替え、受けとった UTF-8 本文 (タイムスタンプ無し)
// をそのまま LogCore の TVPLogDispatchLine に渡す。コンソール/ファイル/
// キャッシュ/REPL sink は LogCore が処理する。
// デスクトップ環境のみ有効。
//---------------------------------------------------------------------------
static void SDLCALL TVPSDLLogOutput(void * /*userdata*/, int /*category*/, SDL_LogPriority priority, const char *message)
{
    if (!message) return;
    TVPLogDispatchLine(TVPSDLPriorityToLogLevel(priority), message);
}
#endif

//---------------------------------------------------------------------------
// Windows 環境での最低限のコンソール出力
// TVP_USE_LOGCORE が無効でも、Windows ではコンソールにログを出力できるようにする。
// SDL3 のデフォルトログ出力は Windows GUI アプリでは stderr が接続されていないため
// 何も表示されない。親コンソールにアタッチして WriteConsole/WriteFile で出力する。
//---------------------------------------------------------------------------
#if defined(_WIN32) && !defined(TVP_USE_LOGCORE)
#include <windows.h>
#include <vector>

static bool g_ConsoleAttached = false;

static void TVPAttachConsoleIfNeeded()
{
    if (g_ConsoleAttached) return;
    // 親プロセスのコンソールにアタッチを試みる
    if (AttachConsole(ATTACH_PARENT_PROCESS)) {
        g_ConsoleAttached = true;
    }
}

static void SDLCALL TVPSDLLogOutputWin32(void * /*userdata*/, int /*category*/, SDL_LogPriority /*priority*/, const char *message)
{
    if (!message) return;
    TVPAttachConsoleIfNeeded();

    HANDLE hStdOutput = ::GetStdHandle(STD_OUTPUT_HANDLE);
    if (hStdOutput == INVALID_HANDLE_VALUE) return;

    DWORD mode;
    size_t len = strlen(message);
    if (GetConsoleMode(hStdOutput, &mode)) {
        // 真の Win32 コンソール: UTF-8 → UTF-16 変換して WriteConsoleW
        int wlen = MultiByteToWideChar(CP_UTF8, 0, message, (int)len, NULL, 0);
        if (wlen > 0) {
            std::vector<wchar_t> wbuf(wlen + 1);
            MultiByteToWideChar(CP_UTF8, 0, message, (int)len, wbuf.data(), wlen);
            ::WriteConsoleW(hStdOutput, wbuf.data(), wlen, NULL, NULL);
            ::WriteConsoleW(hStdOutput, L"\r\n", 2, NULL, NULL);
        }
    } else {
        // パイプ等にリダイレクトされている場合: UTF-8 のまま WriteFile
        ::WriteFile(hStdOutput, message, (DWORD)len, NULL, NULL);
        ::WriteFile(hStdOutput, "\n", 1, NULL, NULL);
    }
}
#endif

void TVPLogInit(TVPLogLevel logLevel)
{
#ifdef TVP_USE_LOGCORE
    // デスクトップ環境: LogCore 経由でリングバッファ/ファイル出力/TJS handler 等を処理
    SDL_SetLogOutputFunction(TVPSDLLogOutput, nullptr);
#elif defined(_WIN32)
    // Windows + LogCore 無効: SDL のログを Win32 コンソールに出力
    SDL_SetLogOutputFunction(TVPSDLLogOutputWin32, nullptr);
#endif
    // モバイル環境 (Android/iOS): SDL3 のネイティブログ出力をそのまま使用
    TVPLogSetLevel(logLevel);
    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "TVP Log system initialized with level: %d", logLevel);
}

void TVPLog(TVPLogLevel logLevel, const char *file, int line, const char *func, const char *format, tvpfmt::format_args args)
{
    SDL_LogPriority priority = TVPLogLevelToSDLPriority(logLevel);

    std::string msg;
    try {
        msg = tvpfmt::vformat(format, args);
    } catch (const tvpfmt::format_error& e) {
        msg = "Log Format error: " + std::string(e.what());
    }
    if (file && func) {
        const char* fileName = file;
        const char* lastSlash = strrchr(file, '/');
        const char* lastBackslash = strrchr(file, '\\');
        if (lastSlash != nullptr || lastBackslash != nullptr) {
            if (lastSlash < lastBackslash || lastSlash == nullptr) {
                fileName = lastBackslash + 1;
            } else {
                fileName = lastSlash + 1;
            }
        }
        SDL_LogMessage(SDL_LOG_CATEGORY_APPLICATION, priority, "[%s:%s:%d] %s", fileName, func, line, msg.c_str());
    } else {
        SDL_LogMessage(SDL_LOG_CATEGORY_APPLICATION, priority, "%s", msg.c_str());
    }
}

void TVPLogMsg(TVPLogLevel logLevel, const char *msg)
{
    SDL_LogPriority priority = TVPLogLevelToSDLPriority(logLevel);
    SDL_LogMessage(SDL_LOG_CATEGORY_APPLICATION, priority, "%s", msg);
}

//---------------------------------------------------------------------------
// モバイル環境用スタブ実装
// LogCore.cpp が除外されるため、他のコードから呼ばれる関数のスタブを提供する。
// デスクトップ向けの機能（リングバッファ、ファイル出力、TJS handler等）は
// モバイルでは不要なため、最小限の実装または空実装とする。
//---------------------------------------------------------------------------
#ifndef TVP_USE_LOGCORE

#include "DebugIntf.h"

// グローバル変数 (DebugIntf.h で extern 宣言)
bool TVPAutoLogToFileOnError = false;
bool TVPAutoClearLogOnError = false;
bool TVPLoggingToFile = false;
tjs_uint TVPLogMaxLines = 0;
ttstr TVPLogLocation;
tjs_char TVPNativeLogLocation[MAX_PATH] = {0};

// TVPAddLog / TVPAddImportantLog - SDL_Log に転送
void TVPAddLog(const ttstr &line)
{
    std::string u8;
    TVPUtf16ToUtf8(u8, line.AsStdString());
    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "%s", u8.c_str());
}

void TVPAddImportantLog(const ttstr &line)
{
    std::string u8;
    TVPUtf16ToUtf8(u8, line.AsStdString());
    SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "%s", u8.c_str());
}

// LogCore への統合ディスパッチ - モバイルでは不使用だがリンクエラー回避のため定義
void TVPLogDispatchLine(TVPLogLevel /*level*/, const char * /*utf8_line*/)
{
    // モバイル環境では SDL_SetLogOutputFunction を差し替えないため呼ばれない
}

//---------------------------------------------------------------------------
// コンソール sink
//
// LogCore を持たない環境 (端末を持たない一部プラットフォーム等) では全ログが
// SDL_Log* 直行で、LogCore の g_console_sink dispatch を通らない。そのため -replweb
// のブラウザ REPL にログが届かない。REPLWEB 有効時のみ SDL のログ出力関数を差し替え
// て sink へ転送し、既定の SDL 出力 (プラットフォーム標準のログ出力等) へは引き続き
// チェーンする (コンソール ⇄
// ブラウザの両方に出る)。sink が実際にセットされたとき (replweb 起動時) だけ
// 差し替えるので、通常時のログ挙動には影響しない。
// REPLWEB 無効ビルド (MASTER 等) では従来どおり no-op スタブ。
//---------------------------------------------------------------------------
#ifdef KRKRZ_REPL_WEB

#include <atomic>

static std::atomic<TVPLogConsoleSinkFn> g_console_sink{nullptr};
static SDL_LogOutputFunction           g_prev_log_output = nullptr;
static void                           *g_prev_log_userdata = nullptr;
static std::atomic<bool>               g_sink_installed{false};

static void SDLCALL TVPSDLLogOutputSink(void *userdata, int category,
                                        SDL_LogPriority priority, const char *message)
{
    if (message) {
        if (auto hook = g_console_sink.load(std::memory_order_acquire)) {
            // 戻り値は無視: 既定出力もそのまま継続させる (コンソール ⇄ web ミラー)。
            hook(TVPSDLPriorityToLogLevel(priority), message);
        }
    }
    // 差し替え前の SDL 既定出力 (プラットフォーム標準のログ出力等) へチェーン。
    if (g_prev_log_output) {
        g_prev_log_output(g_prev_log_userdata, category, priority, message);
    }
}

void TVPLogSetConsoleSink(TVPLogConsoleSinkFn hook)
{
    g_console_sink.store(hook, std::memory_order_release);
    // 初回の非 null hook セット時 (=replweb 起動時) にのみ SDL 出力関数を差し替える。
    // 差し替え前の出力を退避して TVPSDLLogOutputSink からチェーン呼び出しする。
    if (hook && !g_sink_installed.load(std::memory_order_acquire)) {
        SDL_LogOutputFunction cur = nullptr; void *ud = nullptr;
        SDL_GetLogOutputFunction(&cur, &ud);
        if (cur != TVPSDLLogOutputSink) {
            g_prev_log_output   = cur;
            g_prev_log_userdata = ud;
        }
        SDL_SetLogOutputFunction(TVPSDLLogOutputSink, nullptr);
        g_sink_installed.store(true, std::memory_order_release);
    }
}

TVPLogConsoleSinkFn TVPLogGetConsoleSink()
{
    return g_console_sink.load(std::memory_order_acquire);
}

#else  // !KRKRZ_REPL_WEB : コンソール sink 消費者が無いので no-op スタブ

void TVPLogSetConsoleSink(TVPLogConsoleSinkFn /*hook*/) {}
TVPLogConsoleSinkFn TVPLogGetConsoleSink() { return nullptr; }

#endif // KRKRZ_REPL_WEB

// TJS logging handler - モバイルでは不使用
void TVPAddLoggingHandler(tTJSVariantClosure /*clo*/) {}
void TVPRemoveLoggingHandler(tTJSVariantClosure /*clo*/) {}

// ログ取得 - モバイルではリングバッファなし
ttstr TVPGetLastLog(tjs_uint /*n*/) { return ttstr(); }
ttstr TVPGetImportantLog() { return ttstr(); }

// ファイル出力 - モバイルでは不使用
void TVPStartLogToFile(bool /*clear*/) {}
void TVPSetLogLocation(const ttstr & /*loc*/) {}

// エラー時ログフラッシュ - モバイルでは不使用
void TVPOnError() {}

#endif // !TVP_USE_LOGCORE
