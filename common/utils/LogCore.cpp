//---------------------------------------------------------------------------
// LogCore
//
// ログパイプラインのハブ。旧 DebugIntf.cpp にあった以下の状態と処理を集約:
//  - リングバッファ (TVPLogDeque)
//  - important log 文字列キャッシュ (TVPImportantLogs)
//  - ファイル出力 (tTVPLogStreamHolder → krkr.console.log, UTF-16 LE + BOM)
//  - TJS logging handler (TVPAddLoggingHandler / TVPRemoveLoggingHandler)
//  - コンソール sink フック (REPL 連携)
//
// 入口は TVPLogDispatchLine(level, utf8_line)。LogImpl (plog / SDL3) が
// 整形済みの素の本文を UTF-8 で渡してきて、LogCore がタイムスタンプ付与以降
// 下流のすべてを面倒見る。
//---------------------------------------------------------------------------
#include "tjsCommHead.h"

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <deque>
#include <mutex>
#include <thread>
#include <vector>
#include <string>
#include <time.h>

#include "LogIntf.h"
#include "DebugIntf.h"
#include "CharacterSet.h"
#include "MsgIntf.h"
#include "StorageIntf.h"
#include "SysInitIntf.h"
#include "SysInitImpl.h"
#include "NativeFile.h"

#include "tjsDebug.h"
#include "tjsDebuggerHook.h"

//---------------------------------------------------------------------------
// コンソール sink 保管
//---------------------------------------------------------------------------
static std::atomic<TVPLogConsoleSinkFn> g_console_sink{nullptr};

void TVPLogSetConsoleSink(TVPLogConsoleSinkFn hook)
{
	g_console_sink.store(hook, std::memory_order_release);
}

TVPLogConsoleSinkFn TVPLogGetConsoleSink()
{
	return g_console_sink.load(std::memory_order_acquire);
}

//---------------------------------------------------------------------------
// リングバッファと important log キャッシュ
//---------------------------------------------------------------------------
struct tTVPLogItem
{
	ttstr Log;      // 本文 (タイムスタンプを含まない)
	ttstr Time;     // "HH:MM:SS"
	TVPLogLevel Level;
	tTVPLogItem(const ttstr &log, const ttstr &time, TVPLogLevel lv)
		: Log(log), Time(time), Level(lv) {}
};
static std::deque<tTVPLogItem> *TVPLogDeque = NULL;
tjs_uint TVPLogMaxLines = 2048;

bool TVPAutoLogToFileOnError = true;
bool TVPAutoClearLogOnError = false;
bool TVPLoggingToFile = false;
static tjs_uint TVPLogToFileRollBack = 100;
static ttstr *TVPImportantLogs = NULL;
ttstr TVPLogLocation;
tjs_char TVPNativeLogLocation[MAX_PATH];

// ログ状態 (リングバッファ / important cache / タイムスタンプキャッシュ / ファイル出力)
// の保護。TVPLogDispatchLine は REPL チャネルスレッド等の非 main スレッドからも
// 呼ばれる (doc/TtstrDataRetention.md M11)。recursive なのは handler 実行中の再入対策。
static std::recursive_mutex TVPLogStateMutex;

static bool TVPLogObjectsInitialized = false;
static void TVPEnsureLogObjects()
{
	if(TVPLogObjectsInitialized) return;
	TVPLogObjectsInitialized = true;
	TVPLogDeque = new std::deque<tTVPLogItem>();
	TVPImportantLogs = new ttstr();
}
static void TVPDestroyPendingLogLines();
static void TVPDestroyLogObjects()
{
	std::lock_guard<std::recursive_mutex> lock(TVPLogStateMutex);
	if(TVPLogDeque) { delete TVPLogDeque; TVPLogDeque = NULL; }
	if(TVPImportantLogs) { delete TVPImportantLogs; TVPImportantLogs = NULL; }
	TVPDestroyPendingLogLines();
}
static tTVPAtExit TVPDestroyLogObjectsAtExit(TVP_ATEXIT_PRI_CLEANUP, TVPDestroyLogObjects);

//---------------------------------------------------------------------------
// TJS logging handler 列
//
// handler の FuncCall は TJS VM を共有する main thread 限定。ベクタの登録/解除は
// TJS (Debug.addLoggingHandler) 経由 = main thread からのみ行われる前提で、
// 非 main スレッド発のログ行は TVPPendingLogLines に保留し、main thread が
// TVPFlushQueuedLoggingEvents() (イベントポンプ毎フレーム) で配送する。
// 直接 FuncCall するとチャネルスレッド上で VM が走りメインスレッドと競合する
// (-replfile 起動時クラッシュの原因)。
//---------------------------------------------------------------------------
static std::vector<tTJSVariantClosure> TVPLoggingHandlerVector; // main thread のみが触る
static bool TVPInDeliverLoggingEvent = false;
static std::atomic<bool> TVPHasLoggingHandlers{false};       // 有効 handler の有無 (他スレッド早期判定用)
static std::atomic<std::thread::id> TVPLoggingMainThreadId{}; // handler 登録スレッド = main
static std::mutex TVPPendingLogLinesMutex;
static std::deque<ttstr> *TVPPendingLogLines = NULL;         // 非 main 発の配送保留行 (stamped 済)
static const size_t TVPPendingLogLinesMax = 1024;            // 溢れたら古い行から捨てる

static void TVPDestroyPendingLogLines()
{
	std::lock_guard<std::mutex> lock(TVPPendingLogLinesMutex);
	if(TVPPendingLogLines) { delete TVPPendingLogLines; TVPPendingLogLines = NULL; }
}

static void TVPUpdateHasLoggingHandlers()
{
	bool has = false;
	for(std::vector<tTJSVariantClosure>::const_iterator i = TVPLoggingHandlerVector.begin();
		i != TVPLoggingHandlerVector.end(); ++i)
	{
		if(i->Object) { has = true; break; }
	}
	TVPHasLoggingHandlers.store(has, std::memory_order_release);
}

static void TVPCleanupLoggingHandlerVector()
{
	std::vector<tTJSVariantClosure>::iterator i;
	for(i = TVPLoggingHandlerVector.begin(); i != TVPLoggingHandlerVector.end(); )
	{
		if(!i->Object)
		{
			i->Release();
			i = TVPLoggingHandlerVector.erase(i);
		}
		else
		{
			++i;
		}
	}
}

static void TVPDestroyLoggingHandlerVector()
{
	std::vector<tTJSVariantClosure>::iterator i;
	for(i = TVPLoggingHandlerVector.begin(); i != TVPLoggingHandlerVector.end(); ++i)
	{
		i->Release();
	}
	TVPLoggingHandlerVector.clear();
	TVPHasLoggingHandlers.store(false, std::memory_order_release);
}
static tTVPAtExit TVPDestroyLoggingHandlerAtExit
	(TVP_ATEXIT_PRI_PREPARE, TVPDestroyLoggingHandlerVector);

void TVPAddLoggingHandler(tTJSVariantClosure clo)
{
	std::vector<tTJSVariantClosure>::iterator i;
	i = std::find(TVPLoggingHandlerVector.begin(),
		TVPLoggingHandlerVector.end(), clo);
	if(i == TVPLoggingHandlerVector.end())
	{
		clo.AddRef();
		TVPLoggingHandlerVector.push_back(clo);
	}
	// 登録は TJS 経由 = main thread からのみ来る前提。ここで main を覚える。
	TVPLoggingMainThreadId.store(std::this_thread::get_id(), std::memory_order_release);
	TVPUpdateHasLoggingHandlers();
}

void TVPRemoveLoggingHandler(tTJSVariantClosure clo)
{
	std::vector<tTJSVariantClosure>::iterator i;
	i = std::find(TVPLoggingHandlerVector.begin(),
		TVPLoggingHandlerVector.end(), clo);
	if(i != TVPLoggingHandlerVector.end())
	{
		i->Release();
		i->Object = i->ObjThis = NULL;
	}
	if(!TVPInDeliverLoggingEvent)
	{
		TVPCleanupLoggingHandlerVector();
	}
	TVPUpdateHasLoggingHandlers();
}

static void TVPDeliverLoggingEvent(const ttstr &timestampedLine)
{
	if(TVPInDeliverLoggingEvent) return;
	if(TVPLoggingHandlerVector.empty()) return;
	TVPInDeliverLoggingEvent = true;
	try
	{
		bool emptyflag = false;
		tTJSVariant vline(timestampedLine);
		tTJSVariant *pvline[] = { &vline };
		for(tjs_uint i = 0; i < TVPLoggingHandlerVector.size(); i++)
		{
			if(TVPLoggingHandlerVector[i].Object)
			{
				tjs_error er;
				try
				{
					er = TVPLoggingHandlerVector[i].FuncCall(
						0, NULL, NULL, NULL, 1, pvline, NULL);
				}
				catch(...)
				{
					TVPLoggingHandlerVector[i].Release();
					TVPLoggingHandlerVector[i].Object =
					TVPLoggingHandlerVector[i].ObjThis = NULL;
					throw;
				}
				if(TJS_FAILED(er))
				{
					TVPLoggingHandlerVector[i].Release();
					TVPLoggingHandlerVector[i].Object =
					TVPLoggingHandlerVector[i].ObjThis = NULL;
					emptyflag = true;
				}
			}
			else
			{
				emptyflag = true;
			}
		}
		if(emptyflag) TVPCleanupLoggingHandlerVector();
	}
	catch(...)
	{
		TVPInDeliverLoggingEvent = false;
		TVPUpdateHasLoggingHandlers();
		throw;
	}
	TVPInDeliverLoggingEvent = false;
	TVPUpdateHasLoggingHandlers();
}

//---------------------------------------------------------------------------
// 非 main スレッド発の行を保留 (main thread が TVPFlushQueuedLoggingEvents で配送)
//---------------------------------------------------------------------------
static void TVPQueueLoggingEvent(const ttstr &stamped)
{
	std::lock_guard<std::mutex> lock(TVPPendingLogLinesMutex);
	if(!TVPPendingLogLines) TVPPendingLogLines = new std::deque<ttstr>();
	if(TVPPendingLogLines->size() >= TVPPendingLogLinesMax)
		TVPPendingLogLines->pop_front();
	TVPPendingLogLines->push_back(stamped);
}

//---------------------------------------------------------------------------
// main thread: 保留分の配送。イベントポンプ (TVPDeliverAllEvents) と
// TVPDrainREPL から毎フレーム呼ばれる。
//---------------------------------------------------------------------------
void TVPFlushQueuedLoggingEvents()
{
	if(TVPInDeliverLoggingEvent) return; // handler 実行中の再入は不可
	if(TVPHasLoggingHandlers.load(std::memory_order_acquire) &&
	   TVPLoggingMainThreadId.load(std::memory_order_acquire) != std::this_thread::get_id())
		return; // 保険: main 以外からは配送しない
	for(;;)
	{
		ttstr line;
		{
			std::lock_guard<std::mutex> lock(TVPPendingLogLinesMutex);
			if(!TVPPendingLogLines || TVPPendingLogLines->empty()) return;
			if(!TVPHasLoggingHandlers.load(std::memory_order_acquire))
			{
				TVPPendingLogLines->clear(); // handler 全解除後の残骸は破棄
				return;
			}
			line = TVPPendingLogLines->front();
			TVPPendingLogLines->pop_front();
		}
		TVPDeliverLoggingEvent(line);
	}
}

//---------------------------------------------------------------------------
// dispatch からの handler 配送入口。FuncCall は main thread 限定 —
// 非 main スレッド (REPL チャネル等) 発の行は保留キューへ回す。
//---------------------------------------------------------------------------
static void TVPDeliverLoggingEventThreadSafe(const ttstr &stamped)
{
	if(!TVPHasLoggingHandlers.load(std::memory_order_acquire)) return;
	if(TVPLoggingMainThreadId.load(std::memory_order_acquire) != std::this_thread::get_id())
	{
		TVPQueueLoggingEvent(stamped);
		return;
	}
	TVPFlushQueuedLoggingEvents(); // 保留分を先に流して順序を保つ
	TVPDeliverLoggingEvent(stamped);
}

//---------------------------------------------------------------------------
// ファイル出力
//---------------------------------------------------------------------------
class tTVPLogStreamHolder
{
	NativeFile Stream;
	bool Alive;
	bool OpenFailed;

public:
	tTVPLogStreamHolder() { Alive = true; OpenFailed = false; }
	~tTVPLogStreamHolder() { Stream.Close(); Alive = false; }

private:
	void Open(const tjs_char *mode);

public:
	void Clear();
	void Log(const ttstr &text);
	void Reopen() { Stream.Close(); Alive = false; OpenFailed = false; }
} static TVPLogStreamHolder;

static const tjs_char *WDAY[] = {
	TJS_W("Sunday"), TJS_W("Monday"), TJS_W("Tuesday"), TJS_W("Wednesday"),
	TJS_W("Thursday"), TJS_W("Friday"), TJS_W("Saturday")
};
static const tjs_char *MDAY[] = {
	TJS_W("January"), TJS_W("February"), TJS_W("March"), TJS_W("April"),
	TJS_W("May"), TJS_W("June"), TJS_W("July"), TJS_W("August"),
	TJS_W("September"), TJS_W("October"), TJS_W("November"), TJS_W("December")
};

void tTVPLogStreamHolder::Open(const tjs_char *mode)
{
	if(OpenFailed) return;
	try
	{
		tjs_char filename[MAX_PATH];
		if(TVPLogLocation.IsEmpty())
		{
			Stream.Close();
			OpenFailed = true;
		}
		else
		{
			TJS_strcpy(filename, TVPNativeLogLocation);
			TJS_strcat(filename, TJS_W("/krkr.console.log"));
			TVPEnsureDataPathDirectory();
			Stream.Open(filename, mode);
			if(!Stream.IsOpen()) OpenFailed = true;
		}
		if(Stream.IsOpen())
		{
			Stream.Seek(0, SEEK_END);
			if(Stream.Tell() == 0)
			{
				Stream.Write(TJS_N("\xff\xfe"), 2);
			}
#ifdef TJS_TEXT_OUT_CRLF
			ttstr separator(TVPSeparatorCRLF);
#else
			ttstr separator(TVPSeparatorCR);
#endif
			Log(separator);
			static tjs_char timebuf[80];
			{
				time_t timer; timer = time(&timer);
				tm* t = localtime(&timer);
				TJS_snprintf(timebuf, 79,
					TJS_W("%s, %s %02d, %04d %02d:%02d:%02d"),
					WDAY[t->tm_wday], MDAY[t->tm_mon], t->tm_mday,
					t->tm_year+1900, t->tm_hour, t->tm_min, t->tm_sec);
			}
			Log(ttstr(TJS_W("Logging to ")) + ttstr(filename)
				+ TJS_W(" started on ") + timebuf);
		}
	}
	catch(...) { OpenFailed = true; }
}

void tTVPLogStreamHolder::Clear()
{
	Stream.Close();
	Open(TJS_W("wb"));
}

void tTVPLogStreamHolder::Log(const ttstr &text)
{
	if(!Stream.IsOpen()) Open(TJS_W("ab"));
	try
	{
		if(Stream.IsOpen())
		{
			size_t len = text.GetLen() * sizeof(tjs_char);
			if(len != Stream.Write(text.c_str(), len))
			{
				Stream.Close();
				OpenFailed = true;
				return;
			}
#ifdef TJS_TEXT_OUT_CRLF
			Stream.Write(TJS_W("\r\n"), 2 * sizeof(tjs_char));
#else
			Stream.Write(TJS_W("\n"), 1 * sizeof(tjs_char));
#endif
			Stream.Flush();
		}
	}
	catch(...)
	{
		try { Stream.Close(); } catch(...) {}
		OpenFailed = true;
	}
}

//---------------------------------------------------------------------------
// コンソール既定書き出し (sink が無いとき / sink が false を返したとき)
//---------------------------------------------------------------------------
static void TVPLogConsoleDefaultWrite(const ttstr &timestampedLine)
{
#ifdef _WIN32
	HANDLE hStdOutput = ::GetStdHandle(STD_OUTPUT_HANDLE);
	if(hStdOutput == INVALID_HANDLE_VALUE) return;
	DWORD mode;
	const tjs_char *mes = timestampedLine.c_str();
	tjs_int len = (tjs_int)timestampedLine.GetLen();
	if(GetConsoleMode(hStdOutput, &mode))
	{
		::WriteConsoleW(hStdOutput, mes, len, NULL, NULL);
		::WriteConsoleW(hStdOutput, L"\r\n", 2, NULL, NULL);
	}
	else
	{
		static std::vector<char> cache(256);
		tjs_int u8len = TVPWideCharToUtf8String(mes, len, nullptr) + 2;
		if((tjs_int)cache.size() < u8len) cache.resize(u8len);
		tjs_int written = TVPWideCharToUtf8String(mes, len, &cache[0]);
		cache[written++] = '\n';
		::WriteFile(hStdOutput, &cache[0], written, NULL, NULL);
	}
#else
	std::string u8;
	TVPUtf16ToUtf8(u8, timestampedLine.AsStdString());
	fwrite(u8.c_str(), 1, u8.size(), stderr);
	fputc('\n', stderr);
	fflush(stderr);
#endif
}

//---------------------------------------------------------------------------
// TVPLogDispatchLine (中央ディスパッチャ)
//---------------------------------------------------------------------------
void TVPLogDispatchLine(TVPLogLevel level, const char *utf8_line)
{
	if(!utf8_line) return;

	// UTF-8 → UTF-16 (以降の処理は全部 ttstr ベース)
	tjs_string wide;
	TVPUtf8ToUtf16(wide, utf8_line);
	// 末尾の改行は LogCore 側で付与するので剥がす
	while(!wide.empty() && (wide.back() == TJS_W('\n') || wide.back() == TJS_W('\r')))
		wide.pop_back();
	ttstr line(wide.c_str());

	bool important = (level >= TVPLOG_LEVEL_WARNING);
	ttstr stamped;
	{
		// 非 main スレッドからも呼ばれる: 共有状態 (タイムスタンプキャッシュ /
		// リングバッファ / important cache) はロック下で触る
		std::lock_guard<std::recursive_mutex> lock(TVPLogStateMutex);

		TVPEnsureLogObjects();

		// タイムスタンプ (1 秒単位でキャッシュ)
		static time_t prevlogtime = 0;
		static ttstr prevtimebuf;
		static tjs_char timebuf[40];
		time_t timer = time(NULL);
		if(prevlogtime != timer)
		{
			tm *struct_tm = localtime(&timer);
			TJS_snprintf(timebuf, 39, TJS_W("%02d:%02d:%02d"),
				struct_tm->tm_hour, struct_tm->tm_min, struct_tm->tm_sec);
			prevlogtime = timer;
			prevtimebuf = timebuf;
		}

		// リングバッファ
		if(TVPLogDeque)
		{
			TVPLogDeque->push_back(tTVPLogItem(line, prevtimebuf, level));
			while(TVPLogDeque->size() >= TVPLogMaxLines + 100)
			{
				std::deque<tTVPLogItem>::iterator i = TVPLogDeque->begin();
				TVPLogDeque->erase(i, i + 100);
			}
		}

		// "HH:MM:SS [marker ]本文" 形式に組み立て
		stamped = prevtimebuf + TJS_W(" ");
		if(important) stamped += TJS_W("! ");
		stamped += line;

		// important log cache
		if(important && TVPImportantLogs)
		{
#ifdef TJS_TEXT_OUT_CRLF
			*TVPImportantLogs += stamped + TJS_W("\r\n");
#else
			*TVPImportantLogs += stamped + TJS_W("\n");
#endif
		}
	}

	// TJS logging handlers (FuncCall は main thread 限定 — 非 main 発は保留キューへ)
	TVPDeliverLoggingEventThreadSafe(stamped);

	// debugger 接続中はログを DAP `output` event として転送する (Phase 1: stub)
	if(TJS::TVPDebuggerWantsHook()) TJS::TJSDebuggerLog(stamped, important);

	// ファイル出力
	{
		std::lock_guard<std::recursive_mutex> lock(TVPLogStateMutex);
		if(TVPLoggingToFile) TVPLogStreamHolder.Log(stamped);
	}

	// コンソール出力: sink (REPL) 優先、無ければ既定書き出し
	if(auto hook = g_console_sink.load(std::memory_order_acquire))
	{
		// sink には「タイムスタンプ付きの行」を UTF-8 で渡す
		std::string u8;
		TVPUtf16ToUtf8(u8, stamped.AsStdString());
		if(hook(level, u8.c_str())) return;
	}
	TVPLogConsoleDefaultWrite(stamped);
}

//---------------------------------------------------------------------------
// ttstr 版 (旧 TVPAddLog/TVPAddImportantLog 経路) - 内部経由用
//---------------------------------------------------------------------------
static void TVPLogDispatchWide(TVPLogLevel level, const ttstr &line)
{
	std::string u8;
	TVPUtf16ToUtf8(u8, line.AsStdString());
	TVPLogDispatchLine(level, u8.c_str());
}

//---------------------------------------------------------------------------
// TVPAddLog / TVPAddImportantLog
//
// 旧 API。呼び元互換のため残すが、中身は TVPLogMsg 経由に統一。
//---------------------------------------------------------------------------
void TVPAddLog(const ttstr &line)
{
	TVPLogDispatchWide(TVPLOG_LEVEL_INFO, line);
}

void TVPAddImportantLog(const ttstr &line)
{
	TVPLogDispatchWide(TVPLOG_LEVEL_WARNING, line);
}

//---------------------------------------------------------------------------
// 以下は旧 DebugIntf.cpp の公開 API (状態をここに移動したので定義もここ)
//---------------------------------------------------------------------------
ttstr TVPGetImportantLog()
{
	std::lock_guard<std::recursive_mutex> lock(TVPLogStateMutex);
	if(!TVPImportantLogs) return ttstr();
	return *TVPImportantLogs;
}

ttstr TVPGetLastLog(tjs_uint n)
{
	std::lock_guard<std::recursive_mutex> lock(TVPLogStateMutex);
	TVPEnsureLogObjects();
	if(!TVPLogDeque) return TJS_W("");

	tjs_uint size = (tjs_uint)TVPLogDeque->size();
	if(n > size) n = size;
	if(n == 0) return ttstr();

	tjs_uint len = 0;
	std::deque<tTVPLogItem>::iterator i = TVPLogDeque->end();
	i -= n;
	for(tjs_uint c = 0; c < n; ++c, ++i)
	{
#ifdef TJS_TEXT_OUT_CRLF
		len += i->Time.GetLen() + 1 + i->Log.GetLen() + 2;
#else
		len += i->Time.GetLen() + 1 + i->Log.GetLen() + 1;
#endif
	}

	ttstr buf((tTJSStringBufferLength)len);
	tjs_char *p = buf.Independ();

	i = TVPLogDeque->end();
	i -= n;
	for(tjs_uint c = 0; c < n; ++c, ++i)
	{
		TJS_strcpy(p, i->Time.c_str());
		p += i->Time.GetLen();
		*p++ = TJS_W(' ');
		TJS_strcpy(p, i->Log.c_str());
		p += i->Log.GetLen();
#ifdef TJS_TEXT_OUT_CRLF
		*p++ = TJS_W('\r');
		*p++ = TJS_W('\n');
#else
		*p++ = TJS_W('\n');
#endif
	}
	return buf;
}

void TVPStartLogToFile(bool clear)
{
	std::lock_guard<std::recursive_mutex> lock(TVPLogStateMutex);
	TVPEnsureLogObjects();
	if(!TVPImportantLogs) return;
	if(TVPLoggingToFile) return;
	if(clear) TVPLogStreamHolder.Clear();

	// important log を先頭にダンプ
	TVPLogStreamHolder.Log(*TVPImportantLogs);

#ifdef TJS_TEXT_OUT_CRLF
	ttstr separator(TJS_W("\r\n")
		TJS_W("------------------------------------------------------------------------------\r\n"));
#else
	ttstr separator(TJS_W("\n")
		TJS_W("------------------------------------------------------------------------------\n"));
#endif
	TVPLogStreamHolder.Log(separator);

	ttstr content = TVPGetLastLog(TVPLogToFileRollBack);
	TVPLogStreamHolder.Log(content);

	TVPLoggingToFile = true;
}

void TVPSetLogLocation(const ttstr &loc)
{
	TVPLogLocation = TVPNormalizeStorageName(loc);
	if(loc.IsEmpty())
	{
		TVPNativeLogLocation[0] = 0;
		TVPLogLocation.Clear();
	}
	else
	{
		ttstr native = TVPGetLocallyAccessibleName(TVPLogLocation);
		if(native.IsEmpty())
		{
			TVPNativeLogLocation[0] = 0;
			TVPLogLocation.Clear();
		}
		else
		{
			TJS_strcpy(TVPNativeLogLocation, native.AsStdString().c_str());
			if(TVPNativeLogLocation[TJS_strlen(TVPNativeLogLocation)-1] != TJS_W('/'))
				TJS_strcat(TVPNativeLogLocation, TJS_W("/"));
		}
	}

	TVPLogStreamHolder.Reopen();

	tTJSVariant val;
	if(TVPGetCommandLine(TJS_W("-forcelog"), &val))
	{
		ttstr str(val);
		if(str == TJS_W("yes"))
		{
			TVPLoggingToFile = false;
			TVPStartLogToFile(false);
		}
		else if(str == TJS_W("clear"))
		{
			TVPLoggingToFile = false;
			TVPStartLogToFile(true);
		}
	}
	if(TVPGetCommandLine(TJS_W("-logerror"), &val))
	{
		ttstr str(val);
		if(str == TJS_W("no"))
		{
			TVPAutoLogToFileOnError = false;
		}
		else if(str == TJS_W("clear"))
		{
			TVPAutoClearLogOnError = true;
			TVPAutoLogToFileOnError = true;
		}
	}
}

//---------------------------------------------------------------------------
// TVPOnError はログのファイルフラッシュ専用。旧 DebugIntf にあったが
// ログ系状態がこちらに移ったため一緒に持ってくる。
//---------------------------------------------------------------------------
void TVPOnError()
{
	if(TVPAutoLogToFileOnError) TVPStartLogToFile(TVPAutoClearLogOnError);
}
