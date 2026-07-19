//---------------------------------------------------------------------------
//!@file REPL ソケットコマンドチャネル (Android / Linux、エージェント / adb 駆動用)
//
// `tTVPReplFileChannel` の socket 版。CLI 引数の無い Android から adb 経由で
// 対話 REPL を叩けるようにするための、abstract namespace の Unix domain socket
// チャネル。console (CONIN$/TTY) を介さずに `ReplMainQueue` へ提出する。
//
// 有効化: `-replsocket=<name>` (TVPGetCommandLine) または環境変数
//         `KRKRZ_REPL_SOCKET=<name>` (no/off/false/0/空は無効)。
//
// プロトコル (行ベース、lockstep):
//   1. クライアント: TJS 式 (UTF-8) を 1 行 + '\n' で送る。
//   2. チャネル:     1 行を `ReplMainQueue` でメイン実行し、結果 JSON を 1 行
//                    + '\n' で返す。 { "ok": bool, "result": "...", "error": "..." }
//
// PC 側の使い方 (例):
//   adb forward tcp:9099 localabstract:krkrz-repl
//   nc 127.0.0.1 9099        # 1 行入力して Enter、JSON が返る
//
// abstract socket なので Linux 専用 (Android を含む)。他プラットフォームでは
// ShouldStart() が常に false で、スレッドも起動しない。
//---------------------------------------------------------------------------
#ifndef REPL_SOCKET_CHANNEL_H
#define REPL_SOCKET_CHANNEL_H

#include "ThreadIntf.h"
#include <string>

class tTVPReplSocketChannel : public tTVPThread
{
public:
	tTVPReplSocketChannel();
	~tTVPReplSocketChannel();

	//! -replsocket=<name> か env KRKRZ_REPL_SOCKET が有効か (Linux 以外は常に false)。
	static bool ShouldStart();

protected:
	void Execute() override;

private:
	std::string name_;       //!< abstract socket 名 (先頭 NUL は含めない)
	int listen_fd_;          //!< listen ソケット (-1 = 未確保)
	int pp_depth_;           //!< 結果整形の深さ (.depth、既定 4)
	bool pp_compact_;        //!< 結果整形の compact (.compact、既定 false)

	static std::string GetNameFromConfig();

	//! 1 コマンド (dot コマンド または TJS スクリプト) を処理して応答 JSON を返す。
	//! allowDot=true かつ先頭 '.' のときだけ dot コマンド扱い (複数行時は false)。
	std::string ProcessCommand(const std::string& script, bool allowDot);
};

#endif
