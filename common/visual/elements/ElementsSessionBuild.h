//---------------------------------------------------------------------------
//!@file 画面 JSON から overlay_session を組み立てる共通部
//
// 画面 JSON → `elements_modal::overlay_session` の «組み立て» は、 出力先が
// overlay (画面へ提示) でもホストのレイヤでも同じ:
//
//   1. `overlay_session::start()`
//   2. action / drag のコールバックを `iTVPDialogEventHandler` へ橋渡し
//   3. 表示言語の適用
//   4. 変数観測 (`onVar`) の張り直し
//
// 出力先ごとに違うのは «どこに描いて、 どう提示するか» と «入力をどう分配
// するか» だけなので、 その手前までをここに集約する。
//
// 利用側:
//   - ElementsDialogManager  … overlay ダイアログ (`Impl::BeginScreen`)
//   - ElementsLayerPanel     … ホストのレイヤに描くパネル
//
// 通知 (OnAction / OnDrag / OnVar) は **manager の Dispatch* を経由**する。
// キューを 1 本に保って、 ダイアログとパネルの通知順序が入れ替わらないように
// するため (詳細は ElementsDialogManager の QueueOrDispatchAction のコメント)。
//
// 内部ヘッダ。 elements を使う翻訳単位からのみ include すること。
//---------------------------------------------------------------------------
#ifndef ELEMENTS_SESSION_BUILD_H
#define ELEMENTS_SESSION_BUILD_H

#include "tjsCommHead.h"
#include "tjsVariant.h"

#include <elements_modal/modal.h>

#include <memory>
#include <string>

class iTVPDialogEventHandler;

namespace tvp_elements {

//---------------------------------------------------------------------------
// 文字列 / 値の変換 (utf-8 ⇄ ttstr、 elements の値 → TJS)
//---------------------------------------------------------------------------

//! @brief utf-8 → ttstr。
ttstr Utf8ToTtstr(const std::string& utf8);

//! @brief ttstr → utf-8。
std::string TtstrToUtf8(const ttstr& s);

//! @brief elements の widget 値 (variant) → TJS の値。
tTJSVariant ValueToVariant(const elements_modal::value_t& v);

//! @brief `drag_event` → TJS Dictionary。
//!
//! TJS Dictionary には bool が無いので `phase` は文字列
//! (`"begin"` / `"move"` / `"end"`) にする (switch でそのまま書ける)。
tTJSVariant DragEventToDict(const elements_modal::drag_event& d);

//---------------------------------------------------------------------------
// session の組み立て
//---------------------------------------------------------------------------

//! @brief `BuildSession` の入力。
struct SessionOptions
{
	//! view の logical サイズ。 ここに描かれる (overlay は present で拡縮、
	//! レイヤ出力は Layer のサイズと一致させる)。
	int width  = 0;
	int height = 0;

	//! 画面 JSON 内の相対資材パスの解決基準ディレクトリ (utf-8)。 空可。
	std::string resource_base;

	//! 表示言語 (画面 JSON の `"strings"` を引く言語)。 空なら JSON の既定。
	std::string language;
};

//! @brief 画面 JSON から session を作り、 通知を handler へ橋渡しする。
//!
//! 失敗 (JSON の parse / build が通らない) なら nullptr。
//! 成功したら `ApplyVarWatch` も済んでいる。
std::unique_ptr<elements_modal::overlay_session> BuildSession(
	const std::string& json_utf8,
	const SessionOptions& opt,
	iTVPDialogEventHandler* handler);

//! @brief 変数観測 (`OnVar`) を張り直す。
//!
//! handler の `WantsVarNotify()` を問い合わせ、 false なら watcher を外す。
//! 画面ごとに session が作り直されるので、 遷移先でも張り直す必要がある。
void ApplyVarWatch(elements_modal::overlay_session& session,
                   iTVPDialogEventHandler* handler);

} // namespace tvp_elements

#endif
