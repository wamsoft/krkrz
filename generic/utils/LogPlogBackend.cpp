//---------------------------------------------------------------------------
// plog バックエンド実装 (plog のみ、tjs / <windows.h> 非依存)
//
// seam は LogPlogBackend.h を参照。 この TU は plog の <plog/WinApi.h> を引くため、
// tjsCommHead.h (→ <windows.h>) を絶対に include しないこと。 tjs 側との橋渡しは
// LogImpl.cpp が seam (tvplog::) 経由で行う。
//---------------------------------------------------------------------------
#include "LogPlogBackend.h"

#include <plog/Log.h>
#include <plog/Init.h>
#include <plog/Formatters/MessageOnlyFormatter.h>
#include <plog/Appenders/IAppender.h>
#include <plog/Util.h>

namespace tvplog {
namespace {

plog::Severity ToPlog(Sev s)
{
	switch (s) {
		case Sev::Verbose: return plog::verbose;
		case Sev::Debug:   return plog::debug;
		case Sev::Info:    return plog::info;
		case Sev::Warning: return plog::warning;
		case Sev::Error:   return plog::error;
		case Sev::Fatal:   return plog::fatal;
		default:           return plog::none;
	}
}

Sev FromPlog(plog::Severity s)
{
	switch (s) {
		case plog::verbose: return Sev::Verbose;
		case plog::debug:   return Sev::Debug;
		case plog::info:    return Sev::Info;
		case plog::warning: return Sev::Warning;
		case plog::error:   return Sev::Error;
		case plog::fatal:   return Sev::Fatal;
		default:            return Sev::None;
	}
}

DispatchFn g_dispatch = nullptr;

// plog で整形された本文 (タイムスタンプ無し、MessageOnlyFormatter) を UTF-8 化して
// seam の DispatchFn へ渡す。 以降のコンソール/ファイル/キャッシュ/sink は LogCore
// が面倒を見る。
class DispatchAppender : public plog::IAppender
{
public:
	void write(const plog::Record& record) override
	{
		plog::util::nstring str = plog::MessageOnlyFormatter::format(record);
		plog::util::MutexLock lock(m_mutex);

		// nstring が wide (Win 既定) でも narrow でも、 一旦 wide 化してから UTF-8 に
		// 落とす (元 LogImpl.cpp の toWide → TVPUtf16ToUtf8 と同じ意味論を plog util
		// だけで実現)。
		const std::wstring& wstr = plog::util::toWide(str);
		std::string utf8 = plog::util::toNarrow(wstr, plog::codePage::kUTF8);

		// 末尾の改行は LogCore 側で処理されるので剥がしておく。
		while (!utf8.empty() && (utf8.back() == '\n' || utf8.back() == '\r'))
			utf8.pop_back();
		if (g_dispatch) g_dispatch(FromPlog(record.getSeverity()), utf8.c_str());
	}
protected:
	plog::util::Mutex m_mutex;
};

} // anonymous

void PlogSetDispatch(DispatchFn fn) { g_dispatch = fn; }

void PlogInit(Sev level)
{
	static DispatchAppender dispatchAppender;
	plog::init(ToPlog(level), &dispatchAppender);
}

void PlogSetLevel(Sev level)
{
	auto logger = plog::get();
	if (logger) logger->setMaxSeverity(ToPlog(level));
}

void PlogWrite(Sev level, const char* file, int line, const char* func,
               const std::string& utf8msg)
{
	auto logger = plog::get();
	if (logger) {
		plog::Record record(ToPlog(level), func, line, file, 0, 0);
		record << utf8msg;
		logger->write(record.ref());
	}
}

void PlogWriteMsg(Sev level, const std::string& utf8msg)
{
	auto logger = plog::get();
	if (logger) {
		plog::Record record(ToPlog(level), "", 0, "", 0, 0);
		record << utf8msg;
		logger->write(record.ref());
	}
}

} // namespace tvplog
