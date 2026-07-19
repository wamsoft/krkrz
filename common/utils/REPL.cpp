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
#include "ReplFileChannel.h"
#include "ReplSocketChannel.h"
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

		if (input.empty() && multiline_input.empty()) continue;

		if (multiline_input.empty()) {
			if (input == "exit" || input == "quit") {
				TVPTerminateAsync(0);
				break;
			}
			if (input == ".help") {
				ic_printf("Available commands:\n");
				ic_printf("  exit, quit       - Exit the REPL\n");
				ic_printf("  .help            - Show this help\n");
				ic_printf("  .clear           - Clear multiline input\n");
				ic_printf("  .depth [N]       - Show/set pretty-print depth (current: %d)\n", g_repl_pp_depth);
				ic_printf("  .compact [on|off]- Show/toggle compact mode (current: %s)\n",
					g_repl_pp_compact ? "on" : "off");
				ic_printf("  .mem             - Show one-line memory summary\n");
				ic_printf("  .memdump         - Dump full memory stats to log (TVPHeapDump)\n");
				ic_printf("  .sysalloc        - Show system allocator info (free/allocatable)\n");
				ic_printf("  .memoverlay [on|off] - Toggle on-screen memory graph (SDL3 build only)\n");
				ic_printf("  .padoverlay [on|off] - Toggle on-screen gamepad button matrix (SDL3 build only)\n");
				ic_printf("  .mempeakclear    - Reset peak_used on File/Bitmap/Sound allocators\n");
				ic_printf("  .filecache       - Dump StorageCache (file cache) entries to log\n");
				ic_printf("  .imagecache      - Dump TVPGraphicCache (decoded image cache) entries to log\n");
				ic_printf("  .cap [path]      - Capture screen (overlay incl.) to PNG (Agent.captureScreen)\n");
				ic_printf("  .dlg             - List active Elements dialogs (Agent.dialogs)\n");
				ic_printf("  .dlgclose        - Close all Elements dialogs (Agent.closeAllDialogs)\n");
				ic_printf("  .click X Y       - Inject a mouse click at (X,Y) (Agent.click)\n");
				ic_printf("\nEnter TJS expressions or statements to evaluate.\n");
				continue;
			}
			if (input == ".mem") {
				ic_printf("%s\n", TVPSummarizeAllocator("File", TVPGetFileAllocator()).c_str());
				ic_printf("%s\n", TVPSummarizeAllocator("Bitmap", tTVPBitmapBitsAlloc::GetAllocator()).c_str());
				ic_printf("%s\n", TVPSummarizeAllocator("Sound", TVPGetSoundAllocator()).c_str());
				ic_printf("%s\n", TVPGlobalAllocStats::Summarize().c_str());
				ic_printf("%s\n", TVPSummarizeProcessMemory().c_str());
				ic_printf("%s\n", TVPSummarizeSystemAllocatorInfo().c_str());
				continue;
			}
			if (input == ".sysalloc") {
				ic_printf("%s\n", TVPSummarizeSystemAllocatorInfo().c_str());
				continue;
			}
			if (input == ".memdump") {
				TVPHeapDump();
				ic_printf("(memory stats dumped to log)\n");
				continue;
			}
			if (input == ".mempeakclear") {
				if (auto *fa = TVPGetFileAllocator())            fa->resetPeak();
				if (auto *ba = tTVPBitmapBitsAlloc::GetAllocator()) ba->resetPeak();
				if (auto *sa = TVPGetSoundAllocator())           sa->resetPeak();
				TVPGlobalAllocStats::ResetKrkrzPeak();
				TVPGlobalAllocStats::ResetSdlPeak();
				ic_printf("(File/Bitmap/Sound/GlobalAlloc peak_used reset)\n");
				continue;
			}
			if (input == ".filecache") {
				TVPDumpFileCacheList();
				ic_printf("(file cache list dumped to log)\n");
				continue;
			}
			if (input == ".imagecache") {
				TVPDumpImageCacheList();
				ic_printf("(image cache list dumped to log)\n");
				continue;
			}
			if (input.rfind(".memoverlay", 0) == 0) {
				std::string arg = input.size() > 11 ? input.substr(11) : "";
				while (!arg.empty() && arg.front() == ' ') arg.erase(arg.begin());
				if (arg.empty()) {
					TVPMemoryOverlay::SetEnabled(!TVPMemoryOverlay::IsEnabled());
				} else if (arg == "on" || arg == "true" || arg == "1") {
					TVPMemoryOverlay::SetEnabled(true);
				} else if (arg == "off" || arg == "false" || arg == "0") {
					TVPMemoryOverlay::SetEnabled(false);
				}
				ic_printf("memoverlay = %s\n", TVPMemoryOverlay::IsEnabled() ? "on" : "off");
				continue;
			}
			if (input.rfind(".padoverlay", 0) == 0) {
				std::string arg = input.size() > 11 ? input.substr(11) : "";
				while (!arg.empty() && arg.front() == ' ') arg.erase(arg.begin());
				if (arg.empty()) {
					TVPPadOverlay::SetEnabled(!TVPPadOverlay::IsEnabled());
				} else if (arg == "on" || arg == "true" || arg == "1") {
					TVPPadOverlay::SetEnabled(true);
				} else if (arg == "off" || arg == "false" || arg == "0") {
					TVPPadOverlay::SetEnabled(false);
				}
				ic_printf("padoverlay = %s\n", TVPPadOverlay::IsEnabled() ? "on" : "off");
				continue;
			}
			if (input == ".clear") { multiline_input.clear(); continue; }
			if (input.rfind(".depth", 0) == 0) {
				std::string arg = input.size() > 6 ? input.substr(6) : "";
				while (!arg.empty() && arg.front() == ' ') arg.erase(arg.begin());
				if (arg.empty()) {
					ic_printf("depth = %d\n", g_repl_pp_depth);
				} else {
					int n = atoi(arg.c_str());
					if (n < 0) n = 0;
					g_repl_pp_depth = n;
					ic_printf("depth = %d\n", g_repl_pp_depth);
				}
				continue;
			}
			if (input.rfind(".compact", 0) == 0) {
				std::string arg = input.size() > 8 ? input.substr(8) : "";
				while (!arg.empty() && arg.front() == ' ') arg.erase(arg.begin());
				if (arg.empty()) {
					g_repl_pp_compact = !g_repl_pp_compact;
				} else if (arg == "on" || arg == "true" || arg == "1") {
					g_repl_pp_compact = true;
				} else if (arg == "off" || arg == "false" || arg == "0") {
					g_repl_pp_compact = false;
				}
				ic_printf("compact = %s\n", g_repl_pp_compact ? "on" : "off");
				continue;
			}
			// --- エージェント駆動ショートカット (Agent.* に変換して評価) ---
			// continue せず input を書き換えて通常の式評価フローに流す。
			{
				auto trim = [](std::string s) {
					while (!s.empty() && (s.front()==' '||s.front()=='\t')) s.erase(s.begin());
					while (!s.empty() && (s.back()==' '||s.back()=='\t')) s.pop_back();
					return s;
				};
				if (input.rfind(".cap", 0) == 0 &&
				    (input.size() == 4 || input[4] == ' ')) {
					std::string arg = trim(input.substr(4));
					if (arg.empty()) arg = "agent_cap.png";
					input = "Agent.captureScreen(\"" + arg + "\")";
				} else if (input == ".dlg") {
					input = "Agent.dialogs()";
				} else if (input == ".dlgclose") {
					input = "Agent.closeAllDialogs()";
				} else if (input.rfind(".click", 0) == 0 &&
				           (input.size() == 6 || input[6] == ' ')) {
					// ".click X Y" → "Agent.click(X,Y)" (空白区切りをカンマに)
					std::string arg = trim(input.substr(6));
					for (char& c : arg) if (c == ' ' || c == '\t') c = ',';
					input = "Agent.click(" + arg + ")";
				}
			}
		}

		if (!multiline_input.empty()) {
			multiline_input += "\n";
			multiline_input += input;
		} else {
			multiline_input = input;
		}

		if (!IsCompleteStatement(multiline_input)) continue;

		tjs_string script_u16;
		TVPUtf8ToUtf16(script_u16, multiline_input);

		ic_history_add(multiline_input.c_str());

		tTJSVariant result;
		ttstr error;
		bool ok = SubmitAndWait(ttstr(script_u16.c_str()), result, error);

		if (terminating_.load(std::memory_order_acquire)) break;

		if (ok) {
			ttstr resultStr = TVPPrettyPrint(result, g_repl_pp_depth, g_repl_pp_compact);
			std::string resultUTF8;
			TVPUtf16ToUtf8(resultUTF8, resultStr.AsStdString());
			ic_printf("[green]=>[/] %s\n", BBEscape(resultUTF8).c_str());
		} else {
			std::string errorUTF8;
			TVPUtf16ToUtf8(errorUTF8, error.AsStdString());
			ic_printf("[red]%s[/]\n", BBEscape(errorUTF8).c_str());
		}

		multiline_input.clear();
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
// REPL 起動時に console が無ければ (アプリ起動時の TVPAttachWindowsConsole で
// 親プロセスに attach できなかったケース) 最後の砦として AllocConsole を試みる。
// stdio を CONIN$/CONOUT$ に張り直すのは icline が C stdin で isatty を見るため。
static void ReplEnsureWindowsConsole()
{
	if (::GetConsoleWindow() == NULL) {
		if (!::AttachConsole(ATTACH_PARENT_PROCESS)) {
			if (!::AllocConsole()) return;
		}
	}
	FILE *dummy;
	freopen_s(&dummy, "CONIN$",  "r", stdin);
	freopen_s(&dummy, "CONOUT$", "w", stdout);
	freopen_s(&dummy, "CONOUT$", "w", stderr);
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
static tTVPReplFileChannel *TVPReplFileChan = nullptr;
static tTVPReplSocketChannel *TVPReplSocketChan = nullptr;

void TVPCreateREPL()
{
	// console REPL (-repl) / file channel (-replfile) / socket channel
	// (-replsocket または env KRKRZ_REPL_SOCKET) は独立に起動できる。
	// いずれか有効ならメインスレッド実行キューを使うので Reset する。
	bool wantConsole = (TVPScriptREPL == nullptr) && tTVPReplThread::ShouldStartREPL();
	bool wantFile    = (TVPReplFileChan == nullptr) && tTVPReplFileChannel::ShouldStart();
	bool wantSocket  = (TVPReplSocketChan == nullptr) && tTVPReplSocketChannel::ShouldStart();
	if (!wantConsole && !wantFile && !wantSocket) return;

	TVPReplMainQueue::Reset();

	if (wantConsole) {
		ReplEnsureWindowsConsole();
		TVPScriptREPL = new tTVPReplThread();
	}
	if (wantFile) {
		TVPReplFileChan = new tTVPReplFileChannel();
	}
	if (wantSocket) {
		TVPReplSocketChan = new tTVPReplSocketChannel();
	}

	// 例外で即終了しない / inform・MessageDlg を REPL コンソールへ流すための
	// グローバルフラグを立てる ([[project_dap_pause_blocks_app]] 系の agent 駆動)。
	TVPReplActive = true;
}

void TVPDestroyREPL()
{
	// 先にキューを shutdown して、 ブロック中の worker / channel を起こす。
	TVPReplMainQueue::Shutdown();
	if (TVPReplSocketChan) {
		delete TVPReplSocketChan;
		TVPReplSocketChan = nullptr;
	}
	if (TVPReplFileChan) {
		delete TVPReplFileChan;
		TVPReplFileChan = nullptr;
	}
	if (TVPScriptREPL) {
		delete TVPScriptREPL;
		TVPScriptREPL = nullptr;
	}
	TVPReplActive = false;
}

void TVPDrainREPL()
{
	// console / file channel いずれの提出も共有キューが処理する。
	TVPReplMainQueue::Drain();
}
