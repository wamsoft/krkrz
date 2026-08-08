//---------------------------------------------------------------------------
//!@file REPL モーダル応答チャネル
//
// REPL 駆動中、本体側でモーダル UI (confirm / ファイル選択 / inputString 等) が
// 必要になったとき、ネイティブのブロッキングダイアログを出す代わりに、REPL の
// 外部エージェントへ「要求」を渡し「応答」を待つための仕組み。
//
// 実行モデル: REPL コマンドはメインスレッド上で実行される (TVPReplMainQueue::Drain)。
// モーダル要求もメインスレッド上で発生し、ここでブロックしてポーリングする。応答は
// cmd/resp とは別の専用ファイル対 (modal / modalresp) を介し、外部エージェント
// (別プロセス) が直接書き込むため、メインスレッドの実行キューを介さずに解決できる
// (デッドロックしない)。
//
// プロトコル (ReplFileChannel の <dir> 配下):
//   1. 本体: 要求 JSON を `modal.tmp` に書き `modal` に rename。
//   2. エージェント: `modal` を検出→読取→応答 (プレーン文字列) を `modalresp` に書く。
//   3. 本体: `modalresp` の出現を待ち、読取→`modal`/`modalresp` 削除→応答を返す。
//
// 要求 JSON 例: { "type":"confirm", "caption":"確認", "text":"続行?" }
// 応答 (type 別のプレーン文字列):
//   confirm         : "yes" / "no"
//   selectFile/Dir  : 選択パス (空文字列 = キャンセル)
//---------------------------------------------------------------------------
#ifndef REPL_MODAL_H
#define REPL_MODAL_H

#include "tjs.h"   // ttstr

//! ReplFileChannel が起動時に監視ディレクトリを登録する (停止時は空文字でクリア)。
void TVPSetReplModalChannelDir(const ttstr &dir);

//! REPL 経由でモーダル応答を得られる状態か (TVPReplActive かつ チャネル登録済み)。
bool TVPReplModalActive();

//! 汎用: 要求 JSON を出し、応答文字列が来るまでブロック待ちする。
//! 応答を得たら true (responseOut に格納)。チャネル無し/中断時は false。
bool TVPReplRequestModal(const ttstr &requestJson, ttstr &responseOut);

//! confirm (Yes/No)。REPL が処理したら true を返し answer に結果を格納。
//! 未処理 (REPL チャネル無し) なら false (呼び側は既定応答へ)。
bool TVPReplConfirm(const ttstr &text, const ttstr &caption, bool &answer);

//! ファイル/フォルダ選択。REPL が処理したら true を返し、selected/pathOut を格納。
//! selected=false はキャンセル。未処理なら false (呼び側はネイティブダイアログへ)。
bool TVPReplSelectPath(bool isDir, const ttstr &name, const ttstr &title,
	bool save, ttstr &pathOut, bool &selected);

//! selectFile/selectDirectory バインディング用の高レベルヘルパ。
//! params (%[name,title,save]) を読み REPL 経由で選択し、選択時は params["name"]
//! に正規化パスを書き戻す。戻り値: -1 = 未処理 (呼び側はネイティブへ) /
//! 0 = キャンセル / 1 = 選択。
int TVPReplTrySelect(iTJSDispatch2 *params, bool isDir);

//! inputString。REPL が処理したら true。cancelled=true はキャンセル。
bool TVPReplInputString(const ttstr &caption, const ttstr &prompt, const ttstr &def,
	ttstr &result, bool &cancelled);

#endif
