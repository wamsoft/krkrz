//---------------------------------------------------------------------------
/*
	TVP2 ( T Visual Presenter 2 )  A script authoring tool
	Copyright (C) 2000 W.Dee <dee@kikyou.info> and contributors

	See details of license at "license.txt"
*/
//---------------------------------------------------------------------------
// Utilities for Logging
//---------------------------------------------------------------------------
#ifndef LogIntfH
#define LogIntfH

#include "tjsNative.h"
#include "CharacterSet.h"

#include <cstdint>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

//---------------------------------------------------------------------------
// log functions
//---------------------------------------------------------------------------

/*[*/

// ---------------------------------------------------------------------------
// tvpfmt — ログ専用ミニマル書式整形
//
// 旧実装は C++20 <format> / fmtlib のどちらかに切替える薄いラッパだったが、
// C++20 環境によって fmtlib 9/10 がビルドできないケースが出たため、ログ用途に
// 必要な機能だけを自前実装して fmt 依存を完全に切り離している。
//
// サポートする書式:
//   {}            — 既定 (文字列/整数/ポインタ/真偽値)
//   {:d} {:i}     — 10 進整数
//   {:x} {:X}     — 16 進整数
//   {:o}          — 8 進整数
//   {:s}          — 文字列
//   {:0Nd} {:0Nx} — ゼロ埋め + 幅指定
//   {:Nd} {:Nx}   — 幅指定のみ
//   {{  }}        — 波括弧エスケープ
// 浮動小数点・位置指定・精度・アライン・type='b' は未対応 (ログ内で未使用)。
//
// tjs_char* / ttstr / tjs_string は整形境界で UTF-8 へ変換される。低層の
// TVPLog() は常に UTF-8 を受け取る前提。
// ---------------------------------------------------------------------------
namespace tvpfmt {

class format_error : public std::runtime_error {
public:
    explicit format_error(const std::string& w) : std::runtime_error(w) {}
    explicit format_error(const char* w) : std::runtime_error(w) {}
};

namespace detail {
struct FormatArg {
    enum Kind {
        K_NONE, K_S64, K_U64, K_CSTR, K_STR, K_PTR, K_BOOL, K_DBL
    } kind = K_NONE;
    int64_t     i64  = 0;
    uint64_t    u64  = 0;
    double      dbl  = 0.0;
    const char *cstr = nullptr;
    const void *ptr  = nullptr;
    std::string str;
};

inline FormatArg make_arg_impl(bool v)           { FormatArg a; a.kind=FormatArg::K_BOOL; a.u64=v?1:0; return a; }
inline FormatArg make_arg_impl(char v)           { FormatArg a; a.kind=FormatArg::K_S64;  a.i64=v; return a; }
inline FormatArg make_arg_impl(signed char v)    { FormatArg a; a.kind=FormatArg::K_S64;  a.i64=v; return a; }
inline FormatArg make_arg_impl(unsigned char v)  { FormatArg a; a.kind=FormatArg::K_U64;  a.u64=v; return a; }
inline FormatArg make_arg_impl(short v)          { FormatArg a; a.kind=FormatArg::K_S64;  a.i64=v; return a; }
inline FormatArg make_arg_impl(unsigned short v) { FormatArg a; a.kind=FormatArg::K_U64;  a.u64=v; return a; }
inline FormatArg make_arg_impl(int v)            { FormatArg a; a.kind=FormatArg::K_S64;  a.i64=v; return a; }
inline FormatArg make_arg_impl(unsigned int v)   { FormatArg a; a.kind=FormatArg::K_U64;  a.u64=v; return a; }
inline FormatArg make_arg_impl(long v)           { FormatArg a; a.kind=FormatArg::K_S64;  a.i64=v; return a; }
inline FormatArg make_arg_impl(unsigned long v)  { FormatArg a; a.kind=FormatArg::K_U64;  a.u64=v; return a; }
inline FormatArg make_arg_impl(long long v)      { FormatArg a; a.kind=FormatArg::K_S64;  a.i64=v; return a; }
inline FormatArg make_arg_impl(unsigned long long v){ FormatArg a; a.kind=FormatArg::K_U64; a.u64=v; return a; }
inline FormatArg make_arg_impl(float v)          { FormatArg a; a.kind=FormatArg::K_DBL;  a.dbl=v; return a; }
inline FormatArg make_arg_impl(double v)         { FormatArg a; a.kind=FormatArg::K_DBL;  a.dbl=v; return a; }
inline FormatArg make_arg_impl(long double v)    { FormatArg a; a.kind=FormatArg::K_DBL;  a.dbl=(double)v; return a; }

inline FormatArg make_arg_impl(const char *v) {
    FormatArg a; a.kind = FormatArg::K_CSTR; a.cstr = v ? v : "(null)"; return a;
}
inline FormatArg make_arg_impl(char *v) { return make_arg_impl(static_cast<const char*>(v)); }

inline FormatArg make_arg_impl(const std::string& v) {
    FormatArg a; a.kind = FormatArg::K_STR; a.str = v; return a;
}
inline FormatArg make_arg_impl(std::string&& v) {
    FormatArg a; a.kind = FormatArg::K_STR; a.str = std::move(v); return a;
}

// tjs_char*/ttstr/tjs_string は境界で UTF-8 へ変換
inline FormatArg make_arg_impl(const tjs_char *v) {
    FormatArg a; a.kind = FormatArg::K_STR;
    if (v) TVPUtf16ToUtf8(a.str, v);
    return a;
}
inline FormatArg make_arg_impl(tjs_char *v) { return make_arg_impl(static_cast<const tjs_char*>(v)); }

inline FormatArg make_arg_impl(const ttstr& v) {
    FormatArg a; a.kind = FormatArg::K_STR;
    if (v.c_str()) TVPUtf16ToUtf8(a.str, v.c_str());
    return a;
}
inline FormatArg make_arg_impl(const tjs_string& v) {
    FormatArg a; a.kind = FormatArg::K_STR;
    TVPUtf16ToUtf8(a.str, v);
    return a;
}

// 汎用ポインタ (上記の非テンプレート overload に該当しないもの)
template<typename T>
inline FormatArg make_arg_impl(T *v) {
    FormatArg a; a.kind = FormatArg::K_PTR; a.ptr = static_cast<const void*>(v); return a;
}
} // namespace detail

class format_args {
public:
    std::vector<detail::FormatArg> args;
};

template<typename... Args>
inline format_args make_format_args(Args&&... as) {
    format_args r;
    r.args.reserve(sizeof...(as));
    (r.args.emplace_back(detail::make_arg_impl(std::forward<Args>(as))), ...);
    return r;
}

std::string vformat(const char *fmt, const format_args& args);

template<typename... Args>
inline std::string format(const char *fmt_, Args&&... as) {
    return vformat(fmt_, make_format_args(std::forward<Args>(as)...));
}

} // namespace tvpfmt

enum TVPLogLevel {
    TVPLOG_LEVEL_VERBOSE = 0,
    TVPLOG_LEVEL_DEBUG = 1,
    TVPLOG_LEVEL_INFO = 2,
    TVPLOG_LEVEL_WARNING = 3,
    TVPLOG_LEVEL_ERROR = 4,
    TVPLOG_LEVEL_CRITICAL = 5,
    TVPLOG_LEVEL_OFF = 6
};

// コンパイル時に定義されるログレベル。
// このレベル未満のマクロは無条件で `do {} while(0)` に展開され、引数も
// 評価されない (cost ゼロ)。実行時の -loglevel フィルタはこの上で動く。
//
// - MASTER: WARNING (DEBUG/INFO/VERBOSE は完全 strip。リリース成果物用)
// - それ以外 (Release/RelWithDebInfo/Debug): DEBUG (VERBOSE のみ strip)
//   → -loglevel=DEBUG で実行時に DEBUG ログを表示可能
//
// もっと絞りたい/緩めたい場合は CMake `-DKRKRZ_LOG_LEVEL=...` で上書き
// (CMakeLists.txt 参照)。
#ifndef TVPLOG_LEVEL
#ifdef MASTER
#define TVPLOG_LEVEL TVPLOG_LEVEL_WARNING
#else
#define TVPLOG_LEVEL TVPLOG_LEVEL_DEBUG
#endif
#endif

/*]*/

void TVPLogInit(TVPLogLevel logLevel);
TJS_EXP_FUNC_DEF(void, TVPLogSetLevel, (TVPLogLevel logLevel));
TJS_EXP_FUNC_DEF(void, TVPLog, (TVPLogLevel logLevel, const char *file, int line, const char *func, const char *fmt, tvpfmt::format_args args));
TJS_EXP_FUNC_DEF(void, TVPLogMsg, (TVPLogLevel logLevel, const char *msg));

//---------------------------------------------------------------------------
// コンソール sink フック
//
// 整形済みの 1 行 (UTF-8) を受け取る差し替え可能な出力先。
// REPL など、行編集中の端末に割り込み出力したいサブシステムが登録する。
// フックが true を返した場合、LogCore は既定のコンソール書き出しを行わない。
//---------------------------------------------------------------------------
typedef bool (*TVPLogConsoleSinkFn)(TVPLogLevel level, const char *utf8_line);
TJS_EXP_FUNC_DEF(void, TVPLogSetConsoleSink, (TVPLogConsoleSinkFn hook));
TJS_EXP_FUNC_DEF(TVPLogConsoleSinkFn, TVPLogGetConsoleSink, ());

//---------------------------------------------------------------------------
// LogCore への統合ディスパッチ
//
// LogImpl (plog / SDL3) は platform 固有の整形を行ったあと、
// 1 行 UTF-8 として TVPLogDispatchLine を呼ぶ。LogCore はタイムスタンプ付与・
// リングバッファ追加・important cache 追加・TJS logging handler 呼び出し・
// ファイル出力・コンソール出力 (sink or 既定) をまとめて処理する。
//
// LogCore 側でタイムスタンプを一元付与するため、LogImpl 側は
// タイムスタンプを含めない「素の本文」を渡すこと。
//---------------------------------------------------------------------------
TJS_EXP_FUNC_DEF(void, TVPLogDispatchLine, (TVPLogLevel level, const char *utf8_line));

//---------------------------------------------------------------------------
// TJS logging handler registry (旧 DebugIntf.cpp から LogCore.cpp に移動)
//---------------------------------------------------------------------------
extern void TVPAddLoggingHandler(tTJSVariantClosure clo);
extern void TVPRemoveLoggingHandler(tTJSVariantClosure clo);

// handler の FuncCall は main thread 限定。非 main スレッド (REPL チャネル等) 発の
// ログ行は LogCore 内の保留キューに積まれ、main thread がこれを毎フレーム呼んで
// 配送する (イベントポンプ / TVPDrainREPL から呼ばれる)。
extern void TVPFlushQueuedLoggingEvents();

/*[*/

// ログマクロ実装のヘルパー
#define TVPLOG_IMPL(level, format, ...) do{ TVPLog(level, __FILE__, __LINE__, __FUNCTION__, format, tvpfmt::make_format_args(__VA_ARGS__)); } while(0)

// ログレベルに応じたマクロ定義
#if TVPLOG_LEVEL <= TVPLOG_LEVEL_VERBOSE
#define TVPLOG_VERBOSE(format, ...) TVPLOG_IMPL(TVPLOG_LEVEL_VERBOSE, format, __VA_ARGS__)
#else
#define TVPLOG_VERBOSE(...) do {} while(0)
#endif

#if TVPLOG_LEVEL <= TVPLOG_LEVEL_DEBUG
#define TVPLOG_DEBUG(format, ...) TVPLOG_IMPL(TVPLOG_LEVEL_DEBUG, format, __VA_ARGS__)
#else
#define TVPLOG_DEBUG(...) do {} while(0)
#endif

#if TVPLOG_LEVEL <= TVPLOG_LEVEL_INFO
#define TVPLOG_INFO(format, ...) TVPLOG_IMPL(TVPLOG_LEVEL_INFO, format, __VA_ARGS__)
#else
#define TVPLOG_INFO(...) do {} while(0)
#endif

#if TVPLOG_LEVEL <= TVPLOG_LEVEL_WARNING
#define TVPLOG_WARNING(format, ...) TVPLOG_IMPL(TVPLOG_LEVEL_WARNING, format, __VA_ARGS__)
#else
#define TVPLOG_WARNING(...) do {} while(0)
#endif

#if TVPLOG_LEVEL <= TVPLOG_LEVEL_ERROR
#define TVPLOG_ERROR(format, ...) TVPLOG_IMPL(TVPLOG_LEVEL_ERROR, format, __VA_ARGS__)
#else
#define TVPLOG_ERROR(...) do {} while(0)
#endif

#if TVPLOG_LEVEL <= TVPLOG_LEVEL_CRITICAL
#define TVPLOG_CRITICAL(format, ...) TVPLOG_IMPL(TVPLOG_LEVEL_CRITICAL, format, __VA_ARGS__)
#else
#define TVPLOG_CRITICAL(...) do {} while(0)
#endif

// 汎用ログマクロ（ログレベルを指定可能）
#define TJSLOG(level, format, ...) do { \
    if (level >= TVPLOG_LEVEL) { \
        TVPLOG_IMPL(level, format, __VA_ARGS__); \
    } \
} while(0)

/*]*/

#endif
//---------------------------------------------------------------------------
