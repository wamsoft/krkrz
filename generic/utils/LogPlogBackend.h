//---------------------------------------------------------------------------
//!@file plog バックエンドと tjs 側 (LogImpl.cpp) の間の host 非依存 seam
//
// このヘッダも実装 (LogPlogBackend.cpp) も **plog だけ** を include し、tjs /
// <windows.h> を一切引かない。
//
// 理由: WINVER では tjsCommHead.h が <windows.h> を引き、plog の <plog/WinApi.h>
// (plog/Util.h が _WIN32 で無条件 include) が RegCreateKeyExW / RegSetValueExW 等の
// Win32 API を namespace 内の extern "C" ブロックで再宣言する。 winreg.h の宣言と
// 実引数リストが (top-level const 差で) 食い違い、C++20 では C2116/C2733 になって
// 同一 TU に同居できない。 そこで plog を使う側 (backend) と <windows.h> を引く側
// (bridge = LogImpl.cpp) を別 translation unit に分け、seam の中立型だけで橋渡しする。
//---------------------------------------------------------------------------
#ifndef TVP_LOG_PLOG_BACKEND_H
#define TVP_LOG_PLOG_BACKEND_H

#include <string>

namespace tvplog {

//! 中立 severity (plog::Severity にも TVPLogLevel にも依存しない seam 型)。
enum class Sev { Verbose, Debug, Info, Warning, Error, Fatal, None };

// --- tjs 側 (LogImpl.cpp) → plog バックエンド ---

//! plog を初期化し、内部の dispatch appender を登録する。 先に PlogSetDispatch で
//! 配送先を登録しておくこと。
void PlogInit(Sev level);
//! ロガーの最大 severity を更新する。
void PlogSetLevel(Sev level);
//! 整形済み UTF-8 本文を file/line/func 付きで書き込む (整形は呼出側 = tvpfmt)。
void PlogWrite(Sev level, const char* file, int line, const char* func,
               const std::string& utf8msg);
//! file/line/func 無しの本文のみ書き込む。
void PlogWriteMsg(Sev level, const std::string& utf8msg);

// --- plog バックエンド → LogCore への整形済み 1 行配送 ---

//! plog で整形された 1 行 (末尾改行除去済み UTF-8) を LogCore へ渡すコールバック。
//! LogImpl.cpp が PlogInit の前に登録する。
typedef void (*DispatchFn)(Sev level, const char* utf8line);
void PlogSetDispatch(DispatchFn fn);

} // namespace tvplog

#endif // TVP_LOG_PLOG_BACKEND_H
