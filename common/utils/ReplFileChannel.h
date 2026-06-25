//---------------------------------------------------------------------------
//!@file REPL ファイル監視コマンドチャネル (エージェント駆動用)
//
// `-replfile=<dir>` 指定時に起動。 外部エージェントが console (CONIN$) を
// 介さずに REPL を駆動するための、 ファイルベースのコマンドチャネル。
//
// プロトコル (<dir> 配下):
//   1. エージェント: コマンド (UTF-8 TJS) を `cmd.tmp` に書き、 `cmd` に rename。
//   2. チャネル:     `cmd` を検出→読取→削除→メイン実行→結果 JSON を
//                    `resp.tmp` に書き `resp` に rename。
//   3. エージェント: `resp` の出現を待ち、 読取→削除。 次コマンドへ。
//
// 結果 JSON: { "ok": bool, "result": "<pretty-printed>", "error": "<msg>" }
//---------------------------------------------------------------------------
#ifndef REPL_FILE_CHANNEL_H
#define REPL_FILE_CHANNEL_H

#include "ThreadIntf.h"
#include <string>

class tTVPReplFileChannel : public tTVPThread
{
public:
	tTVPReplFileChannel();
	~tTVPReplFileChannel();

	//! -replfile=<dir> が指定されているか (no/off/false/0 は無効)。
	static bool ShouldStart();

protected:
	void Execute() override;

private:
	std::string dir_utf8_;   //!< 監視ディレクトリ (UTF-8)

	static std::string GetDirFromCommandLine();
};

#endif
