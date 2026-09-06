//---------------------------------------------------------------------------
// 監視式 (watch expressions) 実装 — 詳細は ReplWatch.h / doc/DebugToolsRevival.md
//---------------------------------------------------------------------------
#include "tjsCommHead.h"

#include "ReplWatch.h"
#include "ReplMainQueue.h"
#include "ScriptMgnIntf.h"   // TVPExecuteExpression
#include "DebugIntf.h"       // TVPPrettyPrint
#include "CharacterSet.h"    // TVPUtf16ToUtf8
#include "tjsError.h"        // eTJS / eTJSScriptError
#ifdef KRKRZ_REPL_WEB
#include "ReplWebServer.h"   // TVPReplWeb::BroadcastChannel (自動更新の push)
#endif

#include "SysInitIntf.h"   // TVPGetCommandLine (-replwatchfile)

#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <string>
#include <thread>

namespace {

std::mutex g_mtx;
std::vector<TVPReplWatch::Entry> g_entries;
int g_next_id = 1;

// 自動更新。 既定はオフで、 `.watch auto` で明示的に入れる。
int g_interval_ms = TVPReplWatch::kIntervalOff;
tjs_uint64 g_last_eval_ms = 0;

// 評価をメインスレッドへ運ぶかの判定用 (TVPCreateREPL が記録する)。
std::thread::id g_main_tid;
bool g_main_tid_known = false;

// 永続化 (-replwatchfile)。 空 = 無効。 InitPersistence が決める。
std::string g_state_file;
bool g_loading = false;   // 読込中は保存し返さない

bool OnMainThread()
{
	// 未記録なら「メインではない」= キュー経由に倒す。 メインから呼ぶ経路
	// (Drain) は Note 済みの前提。 逆に倒すと自分で自分を待って固まる。
	if (!g_main_tid_known) return false;
	return std::this_thread::get_id() == g_main_tid;
}

//---------------------------------------------------------------------------
// pretty print の設定は REPL の .depth / .compact と共有したいが、 あちらは
// REPL.cpp のファイル static。 監視式は「1 行に収まる短い表示」が要件なので
// 独立に浅め / compact を既定にする。
//---------------------------------------------------------------------------
constexpr int  kWatchPPDepth   = 2;
constexpr bool kWatchPPCompact = true;

//! 1 件を評価して value / error を埋める (メインスレッド)。
void EvaluateEntry(TVPReplWatch::Entry& e)
{
	// 原典 (WatchFormUnit.cpp EvalExpression) と同じく、 例外は捕まえて
	// "(error) メッセージ" を値にする。 窓 / REPL は死なせない。
	try {
		tTJSVariant result;
		TVPExecuteExpression(e.expr, &result);
		e.value = TVPPrettyPrint(result, kWatchPPDepth, kWatchPPCompact);
		e.error = false;
	} catch (eTJSScriptError& ex) {
		e.value = ttstr(TJS_W("(error) ")) + ex.GetMessage();
		e.error = true;
	} catch (eTJS& ex) {
		e.value = ttstr(TJS_W("(error) ")) + ex.GetMessage();
		e.error = true;
	} catch (...) {
		e.value = ttstr(TJS_W("(error) unknown exception"));
		e.error = true;
	}
	e.evaluated = true;
}

//! JSON 文字列リテラルの中身へのエスケープ (本体に JSON ライブラリを増やさない)。
void JsonEscapeInto(std::string& out, const ttstr& s)
{
	std::string u8;
	TVPUtf16ToUtf8(u8, s.AsStdString());
	for (unsigned char c : u8) {
		switch (c) {
			case 0x22: out += "\\\""; break;   // "
			case 0x5c: out += "\\\\"; break;   // backslash
			case 0x08: out += "\\b";  break;
			case 0x0c: out += "\\f";  break;
			case 0x0a: out += "\\n";  break;
			case 0x0d: out += "\\r";  break;
			case 0x09: out += "\\t";  break;
			default:
				if (c < 0x20) {
					char buf[8];
					std::snprintf(buf, sizeof(buf), "\\u%04x", c);
					out += buf;
				} else {
					out += static_cast<char>(c);
				}
		}
	}
}

//! g_mtx を握った状態で JSON を組む。
std::string ToJsonLocked()
{
	std::string out = "{\"interval\":";
	out += std::to_string(g_interval_ms);
	out += ",\"entries\":[";
	bool first = true;
	for (const auto& e : g_entries) {
		if (!first) out += ',';
		first = false;
		out += "{\"id\":";
		out += std::to_string(e.id);
		out += ",\"expr\":\"";
		JsonEscapeInto(out, e.expr);
		out += "\",\"value\":\"";
		JsonEscapeInto(out, e.value);
		out += "\",\"error\":";
		out += e.error ? "true" : "false";
		out += '}';
	}
	out += "]}";
	return out;
}

//! JSON payload を "watch" チャネルへ流す (web REPL が無いビルドでは no-op)。
//! g_mtx を握ったまま呼ばないこと (Broadcast 側は別の mutex を取る)。
void PushWatchJson(const std::string& payload)
{
#ifdef KRKRZ_REPL_WEB
	tjs_string u16;
	TVPUtf8ToUtf16(u16, payload);
	TVPReplWeb::BroadcastChannel(ttstr(TJS_W("watch")), ttstr(u16.c_str()));
#else
	(void)payload;
#endif
}

//! 現在の状態を組んで push する (g_mtx は内部で取る)。
void PushCurrentState()
{
	std::string payload;
	{
		std::lock_guard<std::mutex> lk(g_mtx);
		payload = ToJsonLocked();
	}
	PushWatchJson(payload);
}

//---------------------------------------------------------------------------
// 保存 / 読込
//
// 原典 (吉里吉里2) は environ profile の [watch] に式一覧と間隔を持っていた。
// こちらは REPL 履歴 (.krkrz_history) と同じ流儀で **カレントディレクトリの
// テキスト 1 枚**にする。 窓位置・列幅はブラウザ側の話なので持たない。
//
// 書式 (UTF-8、 行指向):
//   # で始まる行 = コメント。 "# interval=<ms>" だけ意味を持つ
//   それ以外の非空行 = 式 1 本
// 式に改行は入らない (Add / Edit で潰している) ので、 行指向で足りる。
// JSON にしないのは、 読む側にパーサが要るのを避けるため。
//---------------------------------------------------------------------------
void SaveLocked()
{
	if (g_state_file.empty() || g_loading) return;
	std::FILE* fp = std::fopen(g_state_file.c_str(), "wb");
	if (!fp) return;   // 書けない場所なら黙って諦める (開発用の付加機能)
	std::fprintf(fp, "# krkrz watch expressions\n");
	std::fprintf(fp, "# interval=%d\n", g_interval_ms);
	for (const auto& e : g_entries) {
		std::string u8;
		TVPUtf16ToUtf8(u8, e.expr.AsStdString());
		std::fprintf(fp, "%s\n", u8.c_str());
	}
	std::fclose(fp);
}

//! 式から改行を潰す (行指向の保存形式を壊さないため)。 前後の空白も落とす。
ttstr NormalizeExpr(const ttstr& expr)
{
	std::string u8;
	TVPUtf16ToUtf8(u8, expr.AsStdString());
	for (auto& c : u8) if (c == '\n' || c == '\r') c = ' ';
	size_t b = 0, e = u8.size();
	while (b < e && (u8[b] == ' ' || u8[b] == '\t')) ++b;
	while (e > b && (u8[e - 1] == ' ' || u8[e - 1] == '\t')) --e;
	u8 = u8.substr(b, e - b);
	tjs_string w;
	TVPUtf8ToUtf16(w, u8);
	return ttstr(w.c_str());
}

} // anonymous namespace

namespace TVPReplWatch {

void NoteMainThread()
{
	g_main_tid = std::this_thread::get_id();
	g_main_tid_known = true;
}

void Save()
{
	std::lock_guard<std::mutex> lk(g_mtx);
	SaveLocked();
}

void InitPersistence()
{
	// -replwatchfile=<path> で保存先を差し替え、 =no で永続化を切る。
	g_state_file = kDefaultStateFile;
	tTJSVariant v;
	if (TVPGetCommandLine(TJS_W("-replwatchfile"), &v)) {
		ttstr o(v);
		if (o == TJS_W("no") || o == TJS_W("off") || o == TJS_W("false") ||
		    o == TJS_W("0")) {
			g_state_file.clear();
		} else if (!o.IsEmpty() && o != TJS_W("yes") && o != TJS_W("on") &&
		           o != TJS_W("true")) {
			std::string u8;
			tjs_string t(o.c_str());
			TVPUtf16ToUtf8(u8, t);
			g_state_file = u8;
		}
	}
	if (g_state_file.empty()) return;

	std::FILE* fp = std::fopen(g_state_file.c_str(), "rb");
	if (!fp) return;   // 初回起動などで無いのは普通
	std::string body;
	{
		char buf[4096];
		size_t n;
		while ((n = std::fread(buf, 1, sizeof(buf), fp)) > 0) body.append(buf, n);
	}
	std::fclose(fp);

	std::lock_guard<std::mutex> lk(g_mtx);
	// 読込中は書き戻さない (1 行ごとに保存し直すのは無駄なうえ、 途中で
	// 落ちるとファイルを削り取ってしまう)。
	g_loading = true;
	size_t pos = 0, restored = 0;
	while (pos <= body.size()) {
		size_t nl = body.find('\n', pos);
		std::string line = (nl == std::string::npos)
			? body.substr(pos) : body.substr(pos, nl - pos);
		if (!line.empty() && line.back() == '\r') line.pop_back();
		if (!line.empty()) {
			if (line[0] == '#') {
				// "# interval=<ms>" だけ意味を持つ
				size_t k = line.find("interval=");
				if (k != std::string::npos)
					g_interval_ms = std::atoi(line.c_str() + k + 9);
			} else {
				tjs_string w;
				TVPUtf8ToUtf16(w, line);
				Entry e;
				e.id = g_next_id++;
				e.expr = ttstr(w.c_str());
				g_entries.push_back(e);
				++restored;
			}
		}
		if (nl == std::string::npos) break;
		pos = nl + 1;
	}
	g_loading = false;
	if (restored) {
		tjs_string fw;
		TVPUtf8ToUtf16(fw, g_state_file);
		TVPAddLog(ttstr(TJS_W("ReplWatch: restored ")) +
			ttstr((tjs_int)restored) + ttstr(TJS_W(" expression(s) from ")) +
			ttstr(fw.c_str()));
	}
}

int Add(const ttstr& expr)
{
	std::lock_guard<std::mutex> lk(g_mtx);
	Entry e;
	e.id = g_next_id++;
	e.expr = NormalizeExpr(expr);
	g_entries.push_back(e);
	SaveLocked();
	return e.id;
}

bool Remove(int id)
{
	{
		std::lock_guard<std::mutex> lk(g_mtx);
		bool found = false;
		for (auto it = g_entries.begin(); it != g_entries.end(); ++it) {
			if (it->id == id) { g_entries.erase(it); found = true; break; }
		}
		if (!found) return false;
		SaveLocked();
	}
	// 削除は評価を伴わないので EvaluateAll の push に載らない。明示的に流す。
	PushCurrentState();
	return true;
}

bool Edit(int id, const ttstr& expr)
{
	std::lock_guard<std::mutex> lk(g_mtx);
	for (auto& e : g_entries) {
		if (e.id != id) continue;
		e.expr = NormalizeExpr(expr);
		e.value.Clear();
		e.error = false;
		e.evaluated = false;
		SaveLocked();
		return true;
	}
	return false;
}

void Clear()
{
	{
		std::lock_guard<std::mutex> lk(g_mtx);
		if (g_entries.empty()) return;
		g_entries.clear();
		SaveLocked();
	}
	// 空になると EvaluateAll は即 return するので、ここで流さないと
	// ブラウザ側が «消える前の一覧» のままになる。
	PushCurrentState();
}

std::vector<Entry> List()
{
	std::lock_guard<std::mutex> lk(g_mtx);
	return g_entries;
}

size_t Count()
{
	std::lock_guard<std::mutex> lk(g_mtx);
	return g_entries.size();
}

void EvaluateAll()
{
	// 評価中は mutex を離す。 式が TJS を呼ぶので、 その中から REPL 経由で
	// リストが触られても固まらないようにするため。 «スナップショットを取る →
	// 評価する → id で書き戻す» の 3 段構え。
	std::vector<Entry> snapshot;
	{
		std::lock_guard<std::mutex> lk(g_mtx);
		snapshot = g_entries;
	}
	if (snapshot.empty()) return;

	for (auto& e : snapshot) EvaluateEntry(e);

	bool changed = false;
	std::string payload;
	{
		std::lock_guard<std::mutex> lk(g_mtx);
		for (const auto& s : snapshot) {
			for (auto& e : g_entries) {
				if (e.id != s.id) continue;
				if (e.value != s.value || e.error != s.error || !e.evaluated) {
					e.value = s.value;
					e.error = s.error;
					e.evaluated = true;
					changed = true;
				}
				break;
			}
		}
		// 値が変わったときだけ push する (無駄な配信を避ける)。
		if (changed) payload = ToJsonLocked();
	}

	if (changed) PushWatchJson(payload);
}

bool EvaluateAllOnMain()
{
	if (OnMainThread()) { EvaluateAll(); return true; }
	return TVPReplMainQueue::SubmitTask([]{ EvaluateAll(); });
}

void SetInterval(int ms)
{
	{
		std::lock_guard<std::mutex> lk(g_mtx);
		if (ms < 0)                   g_interval_ms = kIntervalOff;
		else if (ms == 0)             g_interval_ms = 0;   // 毎フレーム
		else if (ms < kMinIntervalMs) g_interval_ms = kMinIntervalMs;
		else                          g_interval_ms = ms;
		g_last_eval_ms = 0;   // 次の Drain で即 1 回評価する
		SaveLocked();
	}
	// 間隔は entries に出ないが payload には載る。UI のセレクタを追従させる。
	PushCurrentState();
}

int GetInterval()
{
	std::lock_guard<std::mutex> lk(g_mtx);
	return g_interval_ms;
}

void Drain(tjs_uint64 now_ms)
{
	int interval;
	bool empty;
	{
		std::lock_guard<std::mutex> lk(g_mtx);
		interval = g_interval_ms;
		empty = g_entries.empty();
		if (interval >= 0 && !empty) {
			if (g_last_eval_ms != 0 &&
			    now_ms - g_last_eval_ms < static_cast<tjs_uint64>(interval)) {
				return;   // まだ間隔に達していない
			}
			g_last_eval_ms = now_ms;
		}
	}
	if (interval < 0 || empty) return;
	EvaluateAll();
}

void BroadcastState()
{
	PushCurrentState();
}

std::string ToJson()
{
	std::lock_guard<std::mutex> lk(g_mtx);
	return ToJsonLocked();
}

} // namespace TVPReplWatch
