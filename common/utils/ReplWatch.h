//---------------------------------------------------------------------------
//!@file 監視式 (watch expressions) — 吉里吉里2 デバッグ窓「監視式」の復活
//
// 吉里吉里2 の `environ/win32/WatchFormUnit.cpp` が持っていた「式のリストを
// 保持して、 まとめて評価し、 式と値を並べて見せる」機能をプラットフォーム
// 非依存な形で持つコア。 UI (REPL の `.watch` コマンド / `-replweb` の Watch
// パネル) は薄い前段に徹する。
//
// 原典と同じく **評価コンテキストは global 固定**で、 評価中の例外は捕まえて
// `(error) <message>` を値にする (窓 / REPL を死なせない)。
//
// スレッド安全性:
//   - リスト操作 (Add / Remove / Edit / Clear / List / Set|GetInterval) は
//     mutex 保護。 worker / HTTP スレッドから呼んでよい。
//   - **評価 (EvaluateAll / Drain) はメインスレッド専用**。 worker からは
//     EvaluateAllOnMain() を使う (TVPReplMainQueue::SubmitTask 経由で
//     メインスレッドへ運び、 完了まで待つ)。
//
// 設計 SSOT: doc/DebugToolsRevival.md
//---------------------------------------------------------------------------
#ifndef REPL_WATCH_H
#define REPL_WATCH_H

#include "tjsNative.h"

#include <string>
#include <vector>

namespace TVPReplWatch {

//! @brief 監視式 1 件のスナップショット。
struct Entry {
	int   id = 0;        //!< 追加順に振られる一意な id (削除しても再利用しない)
	ttstr expr;          //!< 式そのもの
	ttstr value;         //!< 直近の評価結果 (未評価なら空)
	bool  error = false; //!< 直近の評価が例外で終わったか (value は "(error) ...")
	bool  evaluated = false; //!< 一度でも評価したか
};

//! @brief 自動更新の既定間隔 (ms)。 原典の「リアルタイム」= 毎フレーム評価は
//!        重い式で描画に影響するので、 既定はこの値。
constexpr int kDefaultIntervalMs = 500;

//! @brief 自動更新間隔の下限 (ms)。 これ未満は 0 (毎フレーム) を除いて
//!        切り上げる。
constexpr int kMinIntervalMs = 100;

//! @brief 自動更新オフを表す間隔値。
constexpr int kIntervalOff = -1;

//! @brief 保存ファイルの既定名 (カレントディレクトリ)。 REPL 履歴の
//!        `.krkrz_history` と同じ流儀。
constexpr const char* kDefaultStateFile = ".krkrz_watch";

//! @brief 保存先を設定して読み込む (TVPCreateREPL がメインスレッドから呼ぶ)。
//!        `-replwatchfile=<path>` で差し替え、 `=no` で永続化を切る。
//!        以後、 式の追加 / 削除 / 編集 / 間隔変更のたびに書き戻す。
//!        原典 (吉里吉里2) は environ profile の `[watch]` に式一覧と間隔を
//!        持っていた。 窓位置・列幅はブラウザ側の話なので持たない。
void InitPersistence();

//! @brief 現在の内容を保存先へ書く (永続化が無効なら何もしない)。
//!        通常は変更のたびに自動で呼ばれるので、 明示呼出は不要。
void Save();

//! @brief メインスレッドを記録する (TVPCreateREPL がメインスレッドから呼ぶ)。
//!        EvaluateAllOnMain() が「その場で評価するか、 キューへ運ぶか」を
//!        決めるのに使う。 記録前は常にキュー経由へ倒す (自分待ちで固まる
//!        より、 一手多い方が安全)。
void NoteMainThread();

//! @brief 式を追加する。 @return 振られた id。
int  Add(const ttstr& expr);

//! @brief id の式を削除する。 @return 消したか (未知の id なら false)。
bool Remove(int id);

//! @brief id の式を差し替える。 値は «未評価» に戻る。
bool Edit(int id, const ttstr& expr);

//! @brief 全件削除。
void Clear();

//! @brief 現在のリストのスナップショット (どのスレッドからでも安全)。
std::vector<Entry> List();

//! @brief 登録件数。
size_t Count();

//! @brief **メインスレッド専用**。 全件を評価して値を更新する。
//!        変化があれば `-replweb` の "watch" チャネルへ push する。
void EvaluateAll();

//! @brief worker スレッドから全件評価を依頼して完了まで待つ。
//!        メインスレッドから呼んだ場合はその場で EvaluateAll() する。
//! @return 評価できたか (シャットダウン中なら false)。
bool EvaluateAllOnMain();

//! @brief 自動更新の間隔を設定する。 0 = 毎フレーム、 負値 = オフ。
//!        1〜kMinIntervalMs-1 は kMinIntervalMs へ切り上げる。
void SetInterval(int ms);

//! @brief 現在の自動更新間隔 (0 = 毎フレーム / 負値 = オフ)。
int  GetInterval();

//! @brief **メインスレッド専用**。 TVPDrainREPL から毎フレーム呼ぶ。
//!        自動更新が有効で間隔を過ぎていれば EvaluateAll() する。
//! @param now_ms 現在時刻 (TVPGetTickCount() 相当)。
void Drain(tjs_uint64 now_ms);

//! @brief 現在の状態を `-replweb` の "watch" チャネルへ push する
//!        (変化の有無を問わない)。 評価を伴わない変更 — 削除 / 全消し /
//!        間隔変更 — は EvaluateAll() の «変化したら push» に載らないので、
//!        ここから明示的に流す。 どのスレッドから呼んでもよい。
void BroadcastState();

//! @brief 値の変化を外部 (Web UI 等) へ push するときの JSON。
//!        `{"interval":500,"entries":[{"id":1,"expr":"...","value":"...",
//!          "error":false},...]}` (UTF-8)。 P2 の `/watch` でも使う。
std::string ToJson();

} // namespace TVPReplWatch

#endif
