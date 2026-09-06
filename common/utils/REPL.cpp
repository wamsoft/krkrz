//---------------------------------------------------------------------------
// REPL (Read-Eval-Print Loop) Implementation
//---------------------------------------------------------------------------
#include "tjsCommHead.h"

#include <cstdio>
#include <cstdlib>
#include <string>
#include <algorithm>
#include <mutex>

#ifndef _WIN32
#include <unistd.h>
#endif

#include "REPL.h"
#include "ReplMainQueue.h"
#ifdef KRKRZ_USE_REPL_FILECHANNEL
#include "ReplFileChannel.h"    // -replfile= file channel + file-based modal (KRKRZ_REPL_FILE 時のみ実体)
#endif
#include "ReplSocketChannel.h"
#include "ReplWebServer.h"      // -replweb=<port> (KRKRZ_REPL_WEB 時のみ実体)
#include "ScriptMgnIntf.h"
#include "SysInitIntf.h"
#include "DebugIntf.h"
#include "CharacterSet.h"
#include "LogIntf.h"
#include "BinaryStreamBuffer.h"     // TVPGetFileAllocator
#include "SoundAllocator.h"         // TVPGetSoundAllocator
#include "BitmapBitsAlloc.h"        // tTVPBitmapBitsAlloc::GetAllocator
#include "MemoryAllocatorStats.h"   // TVPSummarizeAllocator
#include "ProcessMemory.h"          // TVPSummarizeProcessMemory
#include "SystemAllocatorInfo.h"    // TVPSummarizeSystemAllocatorInfo
#include "GlobalAllocStats.h"       // TVPGlobalAllocStats::Summarize
#include "SystemImpl.h"             // TVPHeapDump
#include "MemoryOverlay.h"          // TVPMemoryOverlay::SetEnabled / IsEnabled
#include "PadOverlay.h"             // TVPPadOverlay::SetEnabled / IsEnabled
#include "StorageCache.h"           // TVPDumpFileCacheList
#include "GraphicsLoaderIntf.h"     // TVPDumpImageCacheList
#include "ReplWatch.h"              // 監視式 (.watch)
#include "EventIntf.h"              // TVP(Get|Set)SystemEventDisabledState (.event)
#include "TickCount.h"              // TVPGetTickCount (監視式の自動更新)

// pretty print 設定 (REPL の .depth / .compact コマンドから変更)
static int g_repl_pp_depth = 3;
static bool g_repl_pp_compact = false;

// icline の bbcode 解釈から逃すための簡易エスケープ
// ('[' でタグが始まり、'\' がエスケープ文字)
static std::string BBEscape(const std::string &s)
{
	std::string out;
	out.reserve(s.size() + 8);
	for (char c : s) {
		if (c == '\\' || c == '[') out.push_back('\\');
		out.push_back(c);
	}
	return out;
}

#ifndef KRKRZ_REPL_NO_ICLINE
#include "icline.h"
#else
//---------------------------------------------------------------------------
// 端末制御 (raw mode / termios / Win32 console API) を持たない環境向けの
// 最小 shim。fgets ベースの行入力で、行編集・履歴・色出力は持たない。
// CMake で KRKRZ_REPL_LINE_EDIT=OFF にすると有効。
//
// 制約: ic_async_stop() は no-op。worker は fgets でブロックするため、
// クリーンな終了には別途 stdin を閉じる必要がある (組込み環境では
// プロセス終了時に OS が回収するので実用上問題ない想定)。
//---------------------------------------------------------------------------
#include <cstdarg>
#include <cstring>

// "[tag]...[/]" / "[/]" 形式の bbcode タグを除去 + "\[" "\\" のエスケープを
// 解除して dst (cap バイト) に書き出す。
static void ic_strip_bbcode(char *dst, const char *src, size_t cap) {
	size_t j = 0;
	bool in_tag = false;
	for (size_t i = 0; src[i] && j + 1 < cap; ++i) {
		char c = src[i];
		if (in_tag) {
			if (c == ']') in_tag = false;
			continue;
		}
		if (c == '\\' && (src[i + 1] == '[' || src[i + 1] == '\\')) {
			dst[j++] = src[i + 1];
			++i;
			continue;
		}
		if (c == '[') { in_tag = true; continue; }
		dst[j++] = c;
	}
	dst[j] = 0;
}

static inline void ic_set_history(const char *, long) {}
static inline void ic_enable_multiline(bool) {}
static inline void ic_enable_color(bool) {}
static inline void ic_enable_history_duplicates(bool) {}
static inline void ic_enable_brace_matching(bool) {}
static inline void ic_enable_brace_insertion(bool) {}
static inline void ic_history_add(const char *) {}
static inline void ic_async_stop(void) {}
static inline void ic_free(void *p) { free(p); }

static inline void ic_printf(const char *fmt, ...) {
	char buf[4096];
	char stripped[4096];
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(buf, sizeof(buf), fmt, ap);
	va_end(ap);
	ic_strip_bbcode(stripped, buf, sizeof(stripped));
	fputs(stripped, stdout);
	fflush(stdout);
}

static inline void ic_println(const char *s) {
	char stripped[4096];
	ic_strip_bbcode(stripped, s ? s : "", sizeof(stripped));
	fputs(stripped, stdout);
	fputc('\n', stdout);
	fflush(stdout);
}

// EOF / エラーで NULL を返す。返値は ic_free で解放する契約。
static inline char *ic_readline(const char *prompt) {
	if (prompt) {
		fputs(prompt, stdout);
		fputs("> ", stdout);
		fflush(stdout);
	}
	char buf[4096];
	if (!fgets(buf, sizeof(buf), stdin)) return nullptr;
	size_t n = strlen(buf);
	while (n > 0 && (buf[n - 1] == '\n' || buf[n - 1] == '\r')) buf[--n] = 0;
	char *out = static_cast<char *>(malloc(n + 1));
	if (!out) return nullptr;
	memcpy(out, buf, n + 1);
	return out;
}
#endif // KRKRZ_REPL_NO_ICLINE

//---------------------------------------------------------------------------
// メインスレッド起床 (Win32 は PostThreadMessage で WaitMessage を割り込む。
// SDL3 版は SDL_AppIterate が連続呼び出しされるため wake 不要)
//---------------------------------------------------------------------------
#ifdef _WIN32
#include <windows.h>
static DWORD g_repl_main_tid = 0;
static void ReplWakeMain() {
	if (g_repl_main_tid)
		::PostThreadMessageW(g_repl_main_tid, WM_NULL, 0, 0);
}
static void ReplCaptureMainThread() {
	g_repl_main_tid = ::GetCurrentThreadId();
}
#else
static void ReplWakeMain() {}
static void ReplCaptureMainThread() {}
#endif

//---------------------------------------------------------------------------
// ログ sink: LogImpl 側から整形済み UTF-8 行を受け取り、
// icline の bbcode でレベル別に色付けしてプロンプト上に差し込む。
//---------------------------------------------------------------------------
// icline (bbcode) はスレッドセーフでない。bbcode_t の vout バッファが
// 単一インスタンスで mutex 保護も無いため、複数スレッドから ic_printf を
// 並行で呼ぶと assert 失敗 (Debug ビルド) または出力破損 (Release) を起こす。
// 例: cache 操作 DEBUG ログ (file cache thread / image load thread) と main
// 側の dump 系ログが同時に sink に来る。本 mutex でシリアライズする。
static std::mutex g_repl_log_sink_mu;

static bool TVPReplLogSink(TVPLogLevel level, const char *utf8_line)
{
	const char *style = nullptr;
	switch (level) {
		case TVPLOG_LEVEL_VERBOSE:  style = "gray";          break;
		case TVPLOG_LEVEL_DEBUG:    style = "cyan";          break;
		case TVPLOG_LEVEL_INFO:     style = nullptr;         break;
		case TVPLOG_LEVEL_WARNING:  style = "yellow";        break;
		case TVPLOG_LEVEL_ERROR:    style = "red";           break;
		case TVPLOG_LEVEL_CRITICAL: style = "b red";         break;
		default:                    style = nullptr;         break;
	}
	std::string escaped = BBEscape(utf8_line);
	std::lock_guard<std::mutex> lk(g_repl_log_sink_mu);
	if (style) {
		ic_printf("[%s]%s[/]\n", style, escaped.c_str());
	} else {
		ic_println(escaped.c_str());
	}
	return true;
}

//---------------------------------------------------------------------------
tTVPReplThread::tTVPReplThread()
	: tTVPThread("ReplThread")
{
	ReplCaptureMainThread();
	TVPLogSetConsoleSink(TVPReplLogSink);
	StartThread();
}

tTVPReplThread::~tTVPReplThread()
{
	Shutdown();
	WaitFor();
	TVPLogSetConsoleSink(nullptr);
}

void tTVPReplThread::Shutdown()
{
	terminating_.store(true, std::memory_order_release);
	Terminate();
	ic_async_stop();
	resp_cv_.notify_all();
}

void tTVPReplThread::PrintWelcome()
{
	ic_printf("Kirikiri Z Interactive Shell\n");
	ic_printf("Type 'exit' or 'quit' to exit, '.help' for help\n\n");
}

//---------------------------------------------------------------------------
// worker 側: リクエスト提出と応答待ち
//---------------------------------------------------------------------------
bool tTVPReplThread::SubmitAndWait(const ttstr& script, tTJSVariant& outResult, ttstr& outError)
{
	// メインスレッド実行は共有キューに委譲 (console / file channel 共通)。
	// Win32 では WaitMessage を割り込む必要があるので起こしておく。
	ReplWakeMain();
	return TVPReplMainQueue::Submit(script, outResult, outError);
}

//---------------------------------------------------------------------------
// main 側 drain: 共有キューに委譲 (TVPDrainREPL からも呼ばれる)
//---------------------------------------------------------------------------
void tTVPReplThread::DrainMain()
{
	TVPReplMainQueue::Drain();
}

//---------------------------------------------------------------------------
// printf 形式 → std::string (dot コマンド出力の整形用)
static std::string ReplFmt(const char *fmt, ...)
{
	char buf[1024];
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(buf, sizeof(buf), fmt, ap);
	va_end(ap);
	return std::string(buf);
}

//---------------------------------------------------------------------------
// 入力 1 行の共有処理 (icline / FTXUI split 双方から呼ばれる)。
// ドットコマンド / 複数行継続 / TJS 評価を扱い、出力は sink.emit へ渡す。
// フロント固有の表示 (色付け・入力エコー・プロンプト) は呼び出し側の責務。
//---------------------------------------------------------------------------
bool tTVPReplThread::ProcessLine(const std::string& input_in,
	std::string& multiline_input, const LineSink& sink)
{
	std::string input = input_in;
	auto emit = [&](int lv, const std::string& s) { if (sink.emit) sink.emit(lv, s); };

	if (input.empty() && multiline_input.empty()) return true;

	if (multiline_input.empty()) {
		if (input == "exit" || input == "quit") return false;
		if (input == ".help") {
			emit(LL_HELP, "Available commands:");
			emit(LL_HELP, "  exit, quit       - Exit the REPL");
			emit(LL_HELP, "  .help            - Show this help");
			emit(LL_HELP, "  .clear           - Clear multiline input");
			emit(LL_HELP, ReplFmt("  .depth [N]       - Show/set pretty-print depth (current: %d)", g_repl_pp_depth));
			emit(LL_HELP, ReplFmt("  .compact [on|off]- Show/toggle compact mode (current: %s)", g_repl_pp_compact ? "on" : "off"));
			emit(LL_HELP, "  .mem             - Show one-line memory summary");
			emit(LL_HELP, "  .memdump         - Dump full memory stats to log (TVPHeapDump)");
			emit(LL_HELP, "  .sysalloc        - Show system allocator info (free/allocatable)");
			emit(LL_HELP, "  .memoverlay [on|off] - Toggle on-screen memory graph (drawn by OGL/SDL draw devices)");
			emit(LL_HELP, "  .padoverlay [on|off] - Toggle on-screen gamepad button matrix (drawn by OGL/SDL draw devices)");
			emit(LL_HELP, "  .mempeakclear    - Reset peak_used on File/Bitmap/Sound allocators");
			emit(LL_HELP, "  .filecache       - Dump StorageCache (file cache) entries to log");
			emit(LL_HELP, "  .imagecache      - Dump TVPGraphicCache (decoded image cache) entries to log");
			emit(LL_HELP, "  .cap [path]      - Capture screen (overlay incl.) to PNG (Agent.captureScreen)");
			emit(LL_HELP, "  .dlg             - List active Elements dialogs (Agent.dialogs)");
			emit(LL_HELP, "  .dlgclose        - Close all Elements dialogs (Agent.closeAllDialogs)");
			emit(LL_HELP, "  .click X Y       - Inject a mouse click at (X,Y) (Agent.click)");
			emit(LL_HELP, "  .watch           - List watch expressions (evaluates first)");
			emit(LL_HELP, "  .watch add EXPR  - Add a watch expression");
			emit(LL_HELP, "  .watch rm ID|all - Remove a watch expression (or all)");
			emit(LL_HELP, "  .watch edit ID EXPR - Replace a watch expression");
			emit(LL_HELP, ReplFmt("  .watch auto [ms|on|off] - Show/set auto-update interval (current: %s)",
				TVPReplWatch::GetInterval() < 0 ? "off" : "on"));
			emit(LL_HELP, "  .event [on|off]  - Show/toggle System.eventDisabled");
			emit(LL_HELP, "Enter TJS expressions or statements to evaluate.");
			return true;
		}
		if (input == ".mem") {
			emit(LL_NORMAL, TVPSummarizeAllocator("File", TVPGetFileAllocator()));
			emit(LL_NORMAL, TVPSummarizeAllocator("Bitmap", tTVPBitmapBitsAlloc::GetAllocator()));
			emit(LL_NORMAL, TVPSummarizeAllocator("Sound", TVPGetSoundAllocator()));
			emit(LL_NORMAL, TVPGlobalAllocStats::Summarize());
			emit(LL_NORMAL, TVPSummarizeProcessMemory());
			emit(LL_NORMAL, TVPSummarizeSystemAllocatorInfo());
			return true;
		}
		if (input == ".sysalloc") {
			emit(LL_NORMAL, TVPSummarizeSystemAllocatorInfo());
			return true;
		}
		if (input == ".memdump") {
			TVPHeapDump();
			emit(LL_NORMAL, "(memory stats dumped to log)");
			return true;
		}
		if (input == ".mempeakclear") {
			if (auto *fa = TVPGetFileAllocator())               fa->resetPeak();
			if (auto *ba = tTVPBitmapBitsAlloc::GetAllocator()) ba->resetPeak();
			if (auto *sa = TVPGetSoundAllocator())              sa->resetPeak();
			TVPGlobalAllocStats::ResetKrkrzPeak();
			TVPGlobalAllocStats::ResetSdlPeak();
			emit(LL_NORMAL, "(File/Bitmap/Sound/GlobalAlloc peak_used reset)");
			return true;
		}
		if (input == ".filecache") {
			TVPDumpFileCacheList();
			emit(LL_NORMAL, "(file cache list dumped to log)");
			return true;
		}
		if (input == ".imagecache") {
			TVPDumpImageCacheList();
			emit(LL_NORMAL, "(image cache list dumped to log)");
			return true;
		}
		if (input.rfind(".memoverlay", 0) == 0) {
			std::string arg = input.size() > 11 ? input.substr(11) : "";
			while (!arg.empty() && arg.front() == ' ') arg.erase(arg.begin());
			if (arg.empty())                                    TVPMemoryOverlay::SetEnabled(!TVPMemoryOverlay::IsEnabled());
			else if (arg == "on" || arg == "true" || arg == "1")  TVPMemoryOverlay::SetEnabled(true);
			else if (arg == "off" || arg == "false" || arg == "0") TVPMemoryOverlay::SetEnabled(false);
			emit(LL_NORMAL, ReplFmt("memoverlay = %s", TVPMemoryOverlay::IsEnabled() ? "on" : "off"));
			return true;
		}
		if (input.rfind(".padoverlay", 0) == 0) {
			std::string arg = input.size() > 11 ? input.substr(11) : "";
			while (!arg.empty() && arg.front() == ' ') arg.erase(arg.begin());
			if (arg.empty())                                    TVPPadOverlay::SetEnabled(!TVPPadOverlay::IsEnabled());
			else if (arg == "on" || arg == "true" || arg == "1")  TVPPadOverlay::SetEnabled(true);
			else if (arg == "off" || arg == "false" || arg == "0") TVPPadOverlay::SetEnabled(false);
			emit(LL_NORMAL, ReplFmt("padoverlay = %s", TVPPadOverlay::IsEnabled() ? "on" : "off"));
			return true;
		}
		if (input == ".clear") { multiline_input.clear(); return true; }
		//-------------------------------------------------------------------
		// 監視式 (.watch) — 吉里吉里2 デバッグ窓「監視式」の復活。
		// 評価は必ずメインスレッドで走らせる (ここは worker スレッド)。
		//-------------------------------------------------------------------
		if (input.rfind(".watch", 0) == 0 && (input.size() == 6 || input[6] == ' ')) {
			auto wtrim = [](std::string t) {
				while (!t.empty() && (t.front() == ' ' || t.front() == '\t')) t.erase(t.begin());
				while (!t.empty() && (t.back()  == ' ' || t.back()  == '\t')) t.pop_back();
				return t;
			};
			// 先頭語 (サブコマンド) と残り (引数) に割る。 式は空白を含みうるので
			// 残りはそのまま渡す。
			std::string rest = wtrim(input.size() > 6 ? input.substr(6) : "");
			std::string sub;
			{
				size_t sp = rest.find_first_of(" \t");
				if (sp == std::string::npos) { sub = rest; rest.clear(); }
				else { sub = rest.substr(0, sp); rest = wtrim(rest.substr(sp)); }
			}

			auto dumpList = [&]() {
				auto entries = TVPReplWatch::List();
				if (entries.empty()) {
					emit(LL_NORMAL, "(no watch expressions)");
					return;
				}
				for (const auto& e : entries) {
					std::string expr8, val8;
					TVPUtf16ToUtf8(expr8, e.expr.AsStdString());
					TVPUtf16ToUtf8(val8,  e.value.AsStdString());
					// 例外を投げる式も «(error) ... という値» として並べる
					// (原典と同じ。 LL_ERROR はコマンド自体の失敗に取っておく —
					// ファイルチャネルはそこで ok=false を立てるので、 混ぜると
					// 一覧の順序が result / error に割れてしまう)。
					emit(LL_NORMAL,
					     ReplFmt("%d: ", e.id) + expr8 + " = " +
					     (e.evaluated ? val8 : std::string("(not evaluated)")));
				}
			};

			if (sub.empty()) {
				// 一覧 (表示前に全件評価する = 原典の Update ボタン相当)
				TVPReplWatch::EvaluateAllOnMain();
				dumpList();
				return true;
			}
			if (sub == "add") {
				if (rest.empty()) { emit(LL_ERROR, "usage: .watch add <expression>"); return true; }
				tjs_string expr16;
				TVPUtf8ToUtf16(expr16, rest);
				int id = TVPReplWatch::Add(ttstr(expr16.c_str()));
				TVPReplWatch::EvaluateAllOnMain();
				// 追加した 1 件だけ返す
				for (const auto& e : TVPReplWatch::List()) {
					if (e.id != id) continue;
					std::string val8;
					TVPUtf16ToUtf8(val8, e.value.AsStdString());
					emit(LL_NORMAL, ReplFmt("%d: ", e.id) + rest + " = " + val8);
					break;
				}
				return true;
			}
			if (sub == "rm" || sub == "remove" || sub == "del") {
				if (rest == "all") {
					TVPReplWatch::Clear();
					emit(LL_NORMAL, "(all watch expressions removed)");
					return true;
				}
				int id = atoi(rest.c_str());
				if (id <= 0 || !TVPReplWatch::Remove(id)) {
					emit(LL_ERROR, "usage: .watch rm <id>|all");
				} else {
					emit(LL_NORMAL, ReplFmt("(removed #%d)", id));
				}
				return true;
			}
			if (sub == "edit") {
				size_t sp = rest.find_first_of(" \t");
				std::string idstr = (sp == std::string::npos) ? rest : rest.substr(0, sp);
				std::string expr  = (sp == std::string::npos) ? "" : wtrim(rest.substr(sp));
				int id = atoi(idstr.c_str());
				if (id <= 0 || expr.empty()) {
					emit(LL_ERROR, "usage: .watch edit <id> <expression>");
					return true;
				}
				tjs_string expr16;
				TVPUtf8ToUtf16(expr16, expr);
				if (!TVPReplWatch::Edit(id, ttstr(expr16.c_str()))) {
					emit(LL_ERROR, ReplFmt("(no such watch id: %d)", id));
					return true;
				}
				TVPReplWatch::EvaluateAllOnMain();
				dumpList();
				return true;
			}
			if (sub == "auto") {
				if (!rest.empty()) {
					if (rest == "off" || rest == "false")
						TVPReplWatch::SetInterval(TVPReplWatch::kIntervalOff);
					else if (rest == "on" || rest == "true")
						TVPReplWatch::SetInterval(TVPReplWatch::kDefaultIntervalMs);
					else
						TVPReplWatch::SetInterval(atoi(rest.c_str()));
				}
				int iv = TVPReplWatch::GetInterval();
				if (iv < 0)       emit(LL_NORMAL, "watch auto = off");
				else if (iv == 0) emit(LL_NORMAL, "watch auto = every frame");
				else              emit(LL_NORMAL, ReplFmt("watch auto = %d ms", iv));
				return true;
			}
			emit(LL_ERROR, "usage: .watch [add EXPR | rm ID|all | edit ID EXPR | auto [ms|on|off]]");
			return true;
		}
		//-------------------------------------------------------------------
		// .event — System.eventDisabled の表示 / 切替 (吉里吉里2 コントローラの
		// Event ボタン相当)。 再有効化で TVPDeliverAllEvents() が走るので、
		// 読み書きともメインスレッドへ運ぶ。
		//-------------------------------------------------------------------
		if (input.rfind(".event", 0) == 0 && (input.size() == 6 || input[6] == ' ')) {
			std::string arg = input.size() > 6 ? input.substr(6) : "";
			while (!arg.empty() && (arg.front() == ' ' || arg.front() == '\t')) arg.erase(arg.begin());
			while (!arg.empty() && (arg.back()  == ' ' || arg.back()  == '\t')) arg.pop_back();
			if (!arg.empty() && arg != "on" && arg != "true" && arg != "1" &&
			    arg != "off" && arg != "false" && arg != "0" && arg != "toggle") {
				emit(LL_ERROR, "usage: .event [on|off|toggle]");
				return true;
			}
			bool state = false;
			bool ran = TVPReplMainQueue::SubmitTask([&arg, &state]{
				if (arg == "on" || arg == "true" || arg == "1")
					TVPSetSystemEventDisabledState(true);
				else if (arg == "off" || arg == "false" || arg == "0")
					TVPSetSystemEventDisabledState(false);
				else if (arg == "toggle")
					TVPSetSystemEventDisabledState(!TVPGetSystemEventDisabledState());
				// 引数なし = 表示のみ。 «.event と打っただけでイベントが止まる»
				// のは事故のもとなので、 空はトグルにしない。
				state = TVPGetSystemEventDisabledState();
			});
			if (!ran) emit(LL_ERROR, "(shutting down)");
			else      emit(LL_NORMAL, ReplFmt("eventDisabled = %s", state ? "on" : "off"));
			return true;
		}
		if (input.rfind(".depth", 0) == 0) {
			std::string arg = input.size() > 6 ? input.substr(6) : "";
			while (!arg.empty() && arg.front() == ' ') arg.erase(arg.begin());
			if (!arg.empty()) { int n = atoi(arg.c_str()); g_repl_pp_depth = n < 0 ? 0 : n; }
			emit(LL_NORMAL, ReplFmt("depth = %d", g_repl_pp_depth));
			return true;
		}
		if (input.rfind(".compact", 0) == 0) {
			std::string arg = input.size() > 8 ? input.substr(8) : "";
			while (!arg.empty() && arg.front() == ' ') arg.erase(arg.begin());
			if (arg.empty())                                    g_repl_pp_compact = !g_repl_pp_compact;
			else if (arg == "on" || arg == "true" || arg == "1")  g_repl_pp_compact = true;
			else if (arg == "off" || arg == "false" || arg == "0") g_repl_pp_compact = false;
			emit(LL_NORMAL, ReplFmt("compact = %s", g_repl_pp_compact ? "on" : "off"));
			return true;
		}
		// エージェント駆動ショートカット (Agent.* に変換して評価フローへ)
		{
			auto trim = [](std::string s) {
				while (!s.empty() && (s.front() == ' ' || s.front() == '\t')) s.erase(s.begin());
				while (!s.empty() && (s.back()  == ' ' || s.back()  == '\t')) s.pop_back();
				return s;
			};
			if (input.rfind(".cap", 0) == 0 && (input.size() == 4 || input[4] == ' ')) {
				std::string arg = trim(input.substr(4));
				if (arg.empty()) arg = "agent_cap.png";
				input = "Agent.captureScreen(\"" + arg + "\")";
			} else if (input == ".dlg") {
				input = "Agent.dialogs()";
			} else if (input == ".dlgclose") {
				input = "Agent.closeAllDialogs()";
			} else if (input.rfind(".click", 0) == 0 && (input.size() == 6 || input[6] == ' ')) {
				std::string arg = trim(input.substr(6));
				for (char& c : arg) if (c == ' ' || c == '\t') c = ',';
				input = "Agent.click(" + arg + ")";
			}
		}
	}

	if (!multiline_input.empty()) { multiline_input += "\n"; multiline_input += input; }
	else                          multiline_input = input;

	if (!IsCompleteStatement(multiline_input)) return true; // 継続入力

	if (sink.addHistory) sink.addHistory(multiline_input);

	tjs_string script_u16;
	TVPUtf8ToUtf16(script_u16, multiline_input);
	tTJSVariant result;
	ttstr error;
	bool ok = TVPReplMainQueue::Submit(ttstr(script_u16.c_str()), result, error);
	if (ok) {
		ttstr resultStr = TVPPrettyPrint(result, g_repl_pp_depth, g_repl_pp_compact);
		std::string u8;
		TVPUtf16ToUtf8(u8, resultStr.AsStdString());
		emit(LL_RESULT, "=> " + u8);
	} else {
		std::string u8;
		TVPUtf16ToUtf8(u8, error.AsStdString());
		emit(LL_ERROR, u8);
	}
	multiline_input.clear();
	return true;
}

//---------------------------------------------------------------------------
// worker thread 本体
//---------------------------------------------------------------------------
void tTVPReplThread::Execute()
{
	try {
		ic_set_history(".krkrz_history", 500);
		ic_enable_multiline(true);
		ic_enable_color(true);
		ic_enable_history_duplicates(false);
		ic_enable_brace_matching(true);
		ic_enable_brace_insertion(false);

		PrintWelcome();
	} catch (...) {
		return;
	}

	// icline 用 sink: bbcode で色付け、履歴は ic_history_add。
	LineSink sink;
	sink.emit = [](int level, const std::string& s) {
		std::string esc = BBEscape(s);
		switch (level) {
			case LL_RESULT: ic_printf("[green]%s[/]\n", esc.c_str()); break;
			case LL_ERROR:  ic_printf("[red]%s[/]\n",   esc.c_str()); break;
			default:        ic_printf("%s\n",           esc.c_str()); break;
		}
	};
	sink.addHistory = [](const std::string& s) { ic_history_add(s.c_str()); };

	std::string multiline_input;

	while (!GetTerminated()) {

		const char *prompt = multiline_input.empty() ? "krkrz" : "  ...";
		char *line = ic_readline(prompt);

		if (!line) {
			if (GetTerminated()) break;
			printf("\n");
			TVPTerminateAsync(0);
			break;
		}

		std::string input(line);
		ic_free(line);

		// 行処理は共有の ProcessLine へ (ドットコマンド/複数行/評価)。
		// 出力は sink 経由で bbcode 色付けされる。
		bool cont = ProcessLine(input, multiline_input, sink);
		if (terminating_.load(std::memory_order_acquire)) break;
		if (!cont) { TVPTerminateAsync(0); break; }
	}
}

bool tTVPReplThread::ShouldStartREPL()
{
	// -repl のみを起動条件とする。
	// 明示的に -repl=no / -repl=off / -repl=false が指定された場合は抑止。
	tTJSVariant val;
	if (!TVPGetCommandLine(TJS_W("-repl"), &val)) return false;

	ttstr s(val);
	if (s == TJS_W("no") || s == TJS_W("off") || s == TJS_W("false") || s == TJS_W("0"))
		return false;
	return true;
}

//---------------------------------------------------------------------------
// Windows GUI サブシステムで親プロセスから継承した / 持っていない console に
// きちんと stdio をつなぎ直す。-repl 指定時にだけ呼ばれる。
//---------------------------------------------------------------------------
#ifdef _WIN32
// -repl=new / -repl=window 指定時は、親や継承した既存コンソールに attach せず
// 必ず新規コンソールウィンドウを開く。ターミナル(や別アプリ)から GUI を起動しつつ
// REPL を起動元のコンソールに食い込ませたくない場合に使う。
static bool ReplWantNewConsole()
{
	tTJSVariant val;
	if (!TVPGetCommandLine(TJS_W("-repl"), &val)) return false;
	ttstr s(val);
	// new/window/separate はいずれも新規コンソールを強制する
	// (起動元端末=CLI/シェルに食い込ませない)。
	return (s == TJS_W("new") || s == TJS_W("window") || s == TJS_W("separate"));
}

// REPL 起動時に console が無ければ (アプリ起動時の TVPAttachWindowsConsole で
// 親プロセスに attach できなかったケース) 最後の砦として AllocConsole を試みる。
// stdio を CONIN$/CONOUT$ に張り直すのは icline が C stdin で isatty を見るため。
static void ReplEnsureWindowsConsole()
{
	bool allocated = false; // 新規コンソールを作った(=conhost の可能性大)か
	if (ReplWantNewConsole()) {
		// 既存(親/継承)コンソールから切り離してから新規を割り当てる。
		// FreeConsole はコンソール未所持なら no-op。起動元コンソールは他プロセス
		// (シェル/CLI)所有のままなので影響しない。
		::FreeConsole();
		if (!::AllocConsole()) return;
		allocated = true;
	} else if (::GetConsoleWindow() == NULL) {
		// 親(端末: WT の擬似コンソール等)に attach できればそれを使う。
		// attach できない(端末から起動されていない)ときのみ新規確保。
		if (!::AttachConsole(ATTACH_PARENT_PROCESS)) {
			if (!::AllocConsole()) return;
			allocated = true;
		}
	}
	FILE *dummy;
	freopen_s(&dummy, "CONIN$",  "r", stdin);
	freopen_s(&dummy, "CONOUT$", "w", stdout);
	freopen_s(&dummy, "CONOUT$", "w", stderr);

	// 新規 AllocConsole したコンソールは既定で VT(ANSI) 処理が無効なため、
	// isocline の色付け・入力行の最下部固定(カーソル制御)がデグレする
	// (色が出ない / プロンプトが上に流れる)。VT を明示的に有効化し、
	// さらに COLORTERM を立てて isocline に高カラーパレット=VT 経路を使わせる。
#ifndef ENABLE_VIRTUAL_TERMINAL_PROCESSING
#define ENABLE_VIRTUAL_TERMINAL_PROCESSING 0x0004
#endif
	HANDLE hOut = ::GetStdHandle(STD_OUTPUT_HANDLE);
	if (hOut != NULL && hOut != INVALID_HANDLE_VALUE) {
		DWORD m = 0;
		if (::GetConsoleMode(hOut, &m))
			::SetConsoleMode(hOut, m | ENABLE_VIRTUAL_TERMINAL_PROCESSING);
	}
	HANDLE hErr = ::GetStdHandle(STD_ERROR_HANDLE);
	if (hErr != NULL && hErr != INVALID_HANDLE_VALUE) {
		DWORD m = 0;
		if (::GetConsoleMode(hErr, &m))
			::SetConsoleMode(hErr, m | ENABLE_VIRTUAL_TERMINAL_PROCESSING);
	}
	// isocline は COLORTERM を getenv(CRT) で読む。未設定なら truecolor を与えて
	// VT パレット経路(色 + カーソル制御)を選ばせる。既存値は尊重する。
	// (SetEnvironmentVariable は CRT の getenv キャッシュに載らないため _putenv_s)
	if (::getenv("COLORTERM") == nullptr)
		::_putenv_s("COLORTERM", "truecolor");

	// 新規コンソール(split/new)は既定でスクロールバック用に表示窓より大きい
	// スクリーンバッファを持ち、レガシー conhost だとスクロール바ーが出て
	// 最下行(入力欄)が隠れ、座標(キャレット)もずれる。バッファを表示窓サイズへ
	// 合わせてスクロールバックを無くし、窓==バッファにする。
	// (attach 継承コンソール=WT 等は相手の設定を壊さないよう対象外。新規確保時のみ)
	if (allocated && hOut != NULL && hOut != INVALID_HANDLE_VALUE) {
		CONSOLE_SCREEN_BUFFER_INFO info;
		if (::GetConsoleScreenBufferInfo(hOut, &info)) {
			SHORT winW = (SHORT)(info.srWindow.Right  - info.srWindow.Left + 1);
			SHORT winH = (SHORT)(info.srWindow.Bottom - info.srWindow.Top  + 1);
			COORD size = { winW, winH };
			// まず窓を左上原点へ寄せてからバッファを縮める (縮小はこの順でないと失敗する)
			SMALL_RECT rect = { 0, 0, (SHORT)(winW - 1), (SHORT)(winH - 1) };
			::SetConsoleWindowInfo(hOut, TRUE, &rect);
			::SetConsoleScreenBufferSize(hOut, size);
			::SetConsoleWindowInfo(hOut, TRUE, &rect);
		}
	}
}
#else
static void ReplEnsureWindowsConsole() {}
#endif

// Simple bracket/quote balance check to decide if input is complete.
bool tTVPReplThread::IsCompleteStatement(const std::string& script)
{
	int paren = 0, brace = 0, bracket = 0;
	bool in_single = false, in_double = false;
	bool in_line_comment = false, in_block_comment = false;
	bool last_backslash_line = false;
	size_t i = 0;
	const size_t n = script.size();
	while (i < n) {
		char c = script[i];
		char next = (i + 1 < n) ? script[i + 1] : 0;

		if (in_line_comment) {
			if (c == '\n') in_line_comment = false;
			++i; continue;
		}
		if (in_block_comment) {
			if (c == '*' && next == '/') { in_block_comment = false; i += 2; continue; }
			++i; continue;
		}
		if (in_single) {
			if (c == '\\' && next != 0) { i += 2; continue; }
			if (c == '\'') in_single = false;
			++i; continue;
		}
		if (in_double) {
			if (c == '\\' && next != 0) { i += 2; continue; }
			if (c == '"') in_double = false;
			++i; continue;
		}
		if (c == '/' && next == '/') { in_line_comment = true; i += 2; continue; }
		if (c == '/' && next == '*') { in_block_comment = true; i += 2; continue; }
		if (c == '\'') { in_single = true; ++i; continue; }
		if (c == '"')  { in_double = true; ++i; continue; }
		if (c == '(') ++paren;
		else if (c == ')') --paren;
		else if (c == '{') ++brace;
		else if (c == '}') --brace;
		else if (c == '[') ++bracket;
		else if (c == ']') --bracket;
		++i;
	}

	if (n > 0 && script[n - 1] == '\\') last_backslash_line = true;

	if (in_single || in_double || in_block_comment) return false;
	if (paren > 0 || brace > 0 || bracket > 0) return false;
	if (last_backslash_line) return false;
	return true;
}

//---------------------------------------------------------------------------
// Global functions
//---------------------------------------------------------------------------

static tTVPReplThread *TVPScriptREPL = nullptr;
#ifdef KRKRZ_USE_REPL_FILECHANNEL
static tTVPReplFileChannel *TVPReplFileChan = nullptr;
#endif
static tTVPReplSocketChannel *TVPReplSocketChan = nullptr;

void TVPCreateREPL()
{
	// console REPL (-repl) / file channel (-replfile) / socket channel
	// (-replsocket または env KRKRZ_REPL_SOCKET) は独立に起動できる。
	// いずれか有効ならメインスレッド実行キューを使うので Reset する。
	bool wantConsole = (TVPScriptREPL == nullptr) && tTVPReplThread::ShouldStartREPL();
	bool wantFile    = false;
#ifdef KRKRZ_USE_REPL_FILECHANNEL
	wantFile = (TVPReplFileChan == nullptr) && tTVPReplFileChannel::ShouldStart();
#endif
	bool wantSocket  = (TVPReplSocketChan == nullptr) && tTVPReplSocketChannel::ShouldStart();
	bool wantWeb     = false;
#ifdef KRKRZ_REPL_WEB
	wantWeb = TVPReplWeb::Wanted();
#endif
	if (!wantConsole && !wantFile && !wantSocket && !wantWeb) return;

	TVPReplMainQueue::Reset();

	if (wantConsole) {
		ReplEnsureWindowsConsole();
		TVPScriptREPL = new tTVPReplThread();
	}
#ifdef KRKRZ_USE_REPL_FILECHANNEL
	if (wantFile) {
		TVPReplFileChan = new tTVPReplFileChannel();
	}
#endif
	if (wantSocket) {
		TVPReplSocketChan = new tTVPReplSocketChannel();
	}
#ifdef KRKRZ_REPL_WEB
	if (wantWeb) {
		TVPReplWeb::Start();   // HTTP+SSE ブラウザビューワー (-replweb=<port>)
	}
#endif

	// 監視式 (.watch) の «メインスレッドはどれか» を記録する。 TVPCreateREPL は
	// メインスレッドから呼ばれるので、 ここが唯一確実な機会。
	TVPReplWatch::NoteMainThread();
	// 前回の式リストを読み戻す (-replwatchfile=no で無効)。 以後は変更のたびに
	// 自動保存される。
	TVPReplWatch::InitPersistence();

	// 例外で即終了しない / inform・MessageDlg を REPL コンソールへ流すための
	// グローバルフラグを立てる ([[project_dap_pause_blocks_app]] 系の agent 駆動)。
	TVPReplActive = true;
}

void TVPDestroyREPL()
{
	// 先にキューを shutdown して、 ブロック中の worker / channel を起こす。
	TVPReplMainQueue::Shutdown();
#ifdef KRKRZ_REPL_WEB
	TVPReplWeb::Stop();   // HTTP+SSE サーバ停止 (accept スレッド join / SSE 切断)
	// 登録ハンドラの TJS クロージャをスクリプトエンジン終了前に解放する。
	// サーバ未起動でも WebServer.register による登録は存在しうるので Stop とは別に呼ぶ。
	TVPReplWeb::ClearHandlers();
#endif
	if (TVPReplSocketChan) {
		delete TVPReplSocketChan;
		TVPReplSocketChan = nullptr;
	}
#ifdef KRKRZ_USE_REPL_FILECHANNEL
	if (TVPReplFileChan) {
		delete TVPReplFileChan;
		TVPReplFileChan = nullptr;
	}
#endif
	if (TVPScriptREPL) {
		delete TVPScriptREPL;
		TVPScriptREPL = nullptr;
	}
	TVPReplActive = false;
}

void TVPDrainREPL()
{
#ifdef TVP_USE_LOGCORE
	// REPL がイベントポンプ外で main を回している間 (modal 等) も、
	// チャネルスレッド発ログの TJS logging handler 配送を止めない。
	// 投げた handler は TVPDeliverLoggingEvent 側で登録解除済みなので握りつぶしてよい。
	try { TVPFlushQueuedLoggingEvents(); } catch(...) {}
#endif
	// console / file channel いずれの提出も共有キューが処理する。
	TVPReplMainQueue::Drain();
	// 監視式の自動更新 (.watch auto)。 評価はメインスレッドで行う契約なので
	// ここが定位置。 自動更新オフ / 式ゼロなら即 return する。
	TVPReplWatch::Drain(TVPGetTickCount());
#ifdef KRKRZ_REPL_WEB
	// ブラウザが全部閉じたらアプリも終わる (-replwebidle=<秒>)。未指定なら no-op。
	TVPReplWeb::CheckIdleShutdown();
	// コントローラ (/state) の表示を追従させる。変化が無ければ何もしない。
	TVPReplWeb::PublishStateIfChanged();
#endif
}
