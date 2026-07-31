//---------------------------------------------------------------------------
// ログの tjs 側ブリッジ (tjsCommHead / LogIntf 側 = <windows.h> を引く TU)
//
// plog 本体は LogPlogBackend.cpp 側に隔離した。 WINVER では tjsCommHead.h が
// <windows.h> を引き、plog の <plog/WinApi.h> が同名の Win32 API (RegCreateKeyExW
// 等) を extern "C" で再宣言するため同一 TU に同居できない (C++20 で C2116/C2733)。
// そこで本 TU は plog を一切 include せず、seam (tvplog::) 経由で backend を呼ぶ。
//---------------------------------------------------------------------------
#include "tjsCommHead.h"
#include "LogIntf.h"
#include "LogPlogBackend.h"

//---------------------------------------------------------------------------
// TVPLogLevel ⇔ seam の中立 severity 変換
//---------------------------------------------------------------------------
namespace {

tvplog::Sev ToSev(TVPLogLevel logLevel)
{
	switch (logLevel) {
		case TVPLOG_LEVEL_VERBOSE:  return tvplog::Sev::Verbose;
		case TVPLOG_LEVEL_DEBUG:    return tvplog::Sev::Debug;
		case TVPLOG_LEVEL_INFO:     return tvplog::Sev::Info;
		case TVPLOG_LEVEL_WARNING:  return tvplog::Sev::Warning;
		case TVPLOG_LEVEL_ERROR:    return tvplog::Sev::Error;
		case TVPLOG_LEVEL_CRITICAL: return tvplog::Sev::Fatal;
		default:                    return tvplog::Sev::None;
	}
}

TVPLogLevel FromSev(tvplog::Sev s)
{
	switch (s) {
		case tvplog::Sev::Verbose: return TVPLOG_LEVEL_VERBOSE;
		case tvplog::Sev::Debug:   return TVPLOG_LEVEL_DEBUG;
		case tvplog::Sev::Info:    return TVPLOG_LEVEL_INFO;
		case tvplog::Sev::Warning: return TVPLOG_LEVEL_WARNING;
		case tvplog::Sev::Error:   return TVPLOG_LEVEL_ERROR;
		case tvplog::Sev::Fatal:   return TVPLOG_LEVEL_CRITICAL;
		default:                   return TVPLOG_LEVEL_OFF;
	}
}

// plog バックエンド → LogCore への配送 (seam DispatchFn)。 backend が整形した
// UTF-8 1 行をそのまま TVPLogDispatchLine へ渡す。
void DispatchToCore(tvplog::Sev sev, const char* utf8line)
{
	TVPLogDispatchLine(FromSev(sev), utf8line);
}

} // anonymous

void TVPLogSetLevel(TVPLogLevel logLevel)
{
	tvplog::PlogSetLevel(ToSev(logLevel));
}

void TVPLog(TVPLogLevel logLevel, const char *file, int line, const char *func, const char *format, tvpfmt::format_args args)
{
	// 整形は tjs 側の tvpfmt で行い、 結果の UTF-8 文字列だけを backend に渡す。
	std::string msg;
	try {
		msg = tvpfmt::vformat(format, args);
	} catch (const tvpfmt::format_error& e) {
		msg = "Log Format error: " + std::string(e.what());
	}
	tvplog::PlogWrite(ToSev(logLevel), file, line, func, msg);
}

void TVPLogMsg(TVPLogLevel logLevel, const char *msg)
{
	tvplog::PlogWriteMsg(ToSev(logLevel), msg ? msg : "");
}

void TVPLogInit(TVPLogLevel logLevel)
{
	// backend が整形行を LogCore へ返せるよう、 初期化前に配送先を登録する。
	tvplog::PlogSetDispatch(&DispatchToCore);
	tvplog::PlogInit(ToSev(logLevel));
}
