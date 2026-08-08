//---------------------------------------------------------------------------
// REPL (Read-Eval-Print Loop) Interface
//---------------------------------------------------------------------------
#pragma once

#include "tjsNative.h"
#include "ThreadIntf.h"

#include <atomic>
#include <condition_variable>
#include <mutex>
#include <string>
#include <functional>

class tTVPReplThread : public tTVPThread
{
private:
	// --- request slot (worker → main) ---
	std::mutex req_mtx_;
	ttstr req_script_;
	bool req_pending_ = false;

	// --- response slot (main → worker) ---
	std::mutex resp_mtx_;
	std::condition_variable resp_cv_;
	tTJSVariant resp_result_;
	ttstr resp_error_;
	bool resp_ok_ = false;
	bool resp_ready_ = false;

	// shutdown
	std::atomic<bool> terminating_{false};

public:
	tTVPReplThread();
	~tTVPReplThread();

	// メインスレッドから毎 frame 呼ばれる drain。
	// pending リクエストがあれば 1 件だけ TVPExecuteExpression を実行して
	// 結果をレスポンススロットに詰め、worker を起こす。
	void DrainMain();

	static bool ShouldStartREPL();
	void Shutdown();

	// --- 行処理の共有 (icline / FTXUI split 双方から利用) ---
	// 出力レベル (フロント側で色付けに使う)
	enum LineLevel { LL_NORMAL = 0, LL_RESULT, LL_ERROR, LL_ECHO, LL_HELP };
	struct LineSink {
		// 出力 1 行 (utf8, 改行含まず) を level 付きでフロントへ
		std::function<void(int /*LineLevel*/, const std::string& /*utf8*/)> emit;
		// 確定した文を履歴へ (省略可)
		std::function<void(const std::string& /*utf8 stmt*/)> addHistory;
	};
	// 入力 1 行を処理する。multiline は継続入力の蓄積 (呼び出し側が保持)。
	// ドットコマンド/複数行継続/TJS 評価を扱い、出力は sink.emit へ。
	// exit/quit のとき false を返す (REPL 終了)。
	static bool ProcessLine(const std::string& input, std::string& multiline,
	                        const LineSink& sink);

	// 括弧/クォートのバランスで文が完結しているか (継続入力判定)
	static bool IsCompleteStatement(const std::string& script);

protected:
	void Execute();

private:
	// worker 側: 式を提出して結果が返るまで待つ
	bool SubmitAndWait(const ttstr& script, tTJSVariant& outResult, ttstr& outError);

	void PrintWelcome();
};

void TVPCreateREPL();
void TVPDestroyREPL();
void TVPDrainREPL();
