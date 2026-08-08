//---------------------------------------------------------------------------
/*
	TJS2 Script Engine
	Copyright (C) 2000 W.Dee <dee@kikyou.info> and contributors

	See details of license at "license.txt"
*/
//---------------------------------------------------------------------------
// DAP DebuggerCore (Phase 2 minimal).
// シングルトン。VM hook 受信、DAP request dispatch、停止/再開 CV、
// ブレークポイント保管、現在停止位置の追跡を担当する。
//---------------------------------------------------------------------------
#pragma once

#ifdef KRKRZ_ENABLE_DAP

#include "tjsString.h"
#include "tjsVariant.h"
#include "Debugger.h"   // Breakpoints struct, DBGHOOK_PREV_*

#include <atomic>
#include <condition_variable>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

// PICOJSON_USE_INT64 は CMakeLists.txt でビルド全体に定義される (TU ローカルに
// define すると他 TU と ODR 不整合になるためここでは定義しない)
#include "picojson/picojson.h"

namespace TJS
{
	class tTJSInterCodeContext;
}

class tTVPDAPServerThread;

namespace TJS {

class DebuggerCore
{
public:
	// シングルトン管理
	static DebuggerCore* Instance() { return instance_; }
	static void Init(int port);
	static void Shutdown();

	// VM hook 受信 (メインスレッド)。exception は EXCEPT hook 時のみ非 NULL。
	void OnHook(tjs_int evtype, const tjs_char* filename, tjs_int lineno,
				tTJSInterCodeContext* ctx, tTJSVariant* exception = nullptr);

	// ログ受信 (メインスレッド)。DAP `output` event として転送する
	void OnLog(const ttstr& line, bool important);

	// メインループから毎フレーム呼ぶ。停止中でなければ受信メッセージを drain
	void DrainMain();

private:
	DebuggerCore(int port);
	~DebuggerCore();

	// DAP server からのメッセージ到着通知 (server worker thread から)
	void OnServerMessage();

	// 1 リクエストを処理し response を返す
	void ProcessRequest(const picojson::value& req);

	// 各 request handler
	void HandleInitialize(const picojson::value& req);
	void HandleAttach(const picojson::value& req);
	void HandleSetBreakpoints(const picojson::value& req);
	void HandleSetExceptionBreakpoints(const picojson::value& req);
	void HandleConfigurationDone(const picojson::value& req);
	void HandleThreads(const picojson::value& req);
	void HandleStackTrace(const picojson::value& req);
	void HandleScopes(const picojson::value& req);
	void HandleVariables(const picojson::value& req);
	void HandleEvaluate(const picojson::value& req);
	void HandleContinue(const picojson::value& req);
	void HandlePause(const picojson::value& req);
	void HandleNext(const picojson::value& req);
	void HandleStepIn(const picojson::value& req);
	void HandleStepOut(const picojson::value& req);
	void HandleExceptionInfo(const picojson::value& req);
	void HandleDisconnect(const picojson::value& req);

	// variablesReference は全て動的採番 (1 から)。停止毎にクリアされる。
	// kind で 3 種類のエントリを判別する。
	struct VarRefEntry {
		enum Kind { OBJECT_PROPS, FRAME_LOCALS, FRAME_THIS, FRAME_GLOBALS };
		Kind                   kind;
		tTJSVariant            value;     //!< OBJECT_PROPS のとき有効
		tTJSInterCodeContext*  ctx;       //!< FRAME_LOCALS / FRAME_THIS / FRAME_GLOBALS のとき有効
	};
	std::map<int64_t, VarRefEntry> var_ref_table_;
	int64_t                        next_var_ref_ = 1;

	// 直近停止位置の frame 一覧 (top → bottom 順、HandleStackTrace で更新)
	struct FrameInfo {
		tTJSInterCodeContext* ctx;
		tjs_string            filename;
		tjs_string            funcname;
		int                   line;
	};
	std::vector<FrameInfo> current_frames_;

	// helpers
	int64_t RegisterIfHasChildren(const tTJSVariant& v);  // OBJECT_PROPS、有子 obj のみ
	int64_t RegisterScope(VarRefEntry::Kind kind, tTJSInterCodeContext* ctx);
	void    ClearVarRefs();
	picojson::array BuildVariablesFromVariant(const tTJSVariant& v);
	picojson::array BuildVariablesFromFrame(VarRefEntry::Kind kind, tTJSInterCodeContext* ctx);
	picojson::object MakeVariableObject(const tjs_string& name, const tTJSVariant& v);

	// 条件付き BP / log point (Phase 5b)。ctx は BP の発火位置 (current_ctx_ は
	// この時点でまだ更新されていないので、明示的に渡す)。
	bool EvalBPCondition(const tjs_string& expr, tTJSInterCodeContext* ctx);
	void EmitLogPoint(const tjs_string& msg, tTJSInterCodeContext* ctx);

	// frame の local 変数を含む iTJSDispatch2 (Dictionary) を作って返す。
	// TVPExecuteExpression の context に渡すと、bare な i などが local 変数として
	// 解決される (TJS の bare 識別子は local→this.X→global の順で resolve される。
	// この dict が this になり、メンバとして注入された local が見える)。
	// 戻り値は Release() 必要。null なら何も注入できなかった (free 不要)。
	iTJSDispatch2* BuildEvalContext(tTJSInterCodeContext* frame_ctx);

	// 共通レスポンス送信ヘルパ
	void SendResponse(const picojson::value& req, bool success,
					  const picojson::object& body = {},
					  const std::string& message = "");
	void SendEvent(const std::string& event, const picojson::object& body = {});

	// 停止/再開機構
	void BlockUntilResume(tTJSInterCodeContext* ctx, const std::string& reason);

	static DebuggerCore* instance_;

	std::unique_ptr<tTVPDAPServerThread> server_;
	int port_;

	// BP 保管 (filename → BP 行集合)
	Breakpoints breakpoints_;

	// 例外ブレーク種別
	bool break_on_uncaught_exception_ = false;

	// 実行モード
	enum ExecMode {
		EXEC_RUN,         // 通常実行
		EXEC_PAUSE,       // 次の hook で停止
		EXEC_STEP_IN,     // 次の EXE_LINE で必ず停止 (関数内に入る)
		EXEC_STEP_OVER,   // depth <= baseline で停止 (同階層 or 戻った)
		EXEC_STEP_OUT,    // depth <  baseline で停止 (現関数から return)
	};
	std::atomic<ExecMode> exec_mode_{EXEC_RUN};

	// Step depth tracking: CALL で +1, RETURN で -1。
	// 絶対値は意味なく、step 開始時点との差分が意味を持つ。
	int step_depth_          = 0;
	int step_baseline_depth_ = 0;

	// 停止/再開
	std::mutex pause_mtx_;
	std::condition_variable pause_cv_;
	std::atomic<bool> should_resume_{false};
	std::atomic<bool> terminating_{false};

	// 直近停止位置 (stackTrace 応答に使う)
	tTJSInterCodeContext* current_ctx_ = nullptr;
	tjs_string             current_filename_;
	int                    current_lineno_ = -1;

	// 例外停止時に保存される値 (Phase 5b-e)。例外でない停止では空。
	tTJSVariant            current_exception_;
	bool                   has_current_exception_ = false;

	// REPL 評価中フラグ (Phase 5b-f)。停止中に TVPDrainREPL() を回す時、その間の
	// VM hook を skip して step tracking を乱さないようにする。
	bool                   in_repl_eval_ = false;

	// configurationDone を受信したかどうか
	std::atomic<bool> configuration_done_{false};

	// シーケンス番号 (server → client メッセージ用)
	std::atomic<int> seq_counter_{1};
};

} // namespace TJS

// ----- public wrapper API (REPL と同じ命名規則) -----
// 起動時 / 終了時 / 毎フレームに呼ぶ。-dap=<port> 解釈もここで行う。
void TVPCreateDAP();
void TVPDestroyDAP();
void TVPDrainDAP();

#endif // KRKRZ_ENABLE_DAP
