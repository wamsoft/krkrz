//---------------------------------------------------------------------------
//!@file 独立 SDL_Window でモーダル Elements ダイアログを実行 (Phase 6c)
//
// 既存の `tTVPElementsDialogManager` (DrawDevice overlay) とは別経路で、
// 独立した SDL_Window を立ててブロッキングモーダル UI を表示する。
//
// 主用途:
//   - TJS `Dialog.showModalJson` / `showModalFile` (DialogIntf.cpp)
//   - `-userconf` 起動時の UserConfig UI (SDLElementsUserConfig.cpp)
//
// 戻り値の仕組み:
//   - 内部ハンドラが button click を「閉じるアクション」、 state widget の
//     値変更を「id → value 記録」として振り分ける。
//   - button の payload は void、 checkbox/toggle/slide_switch は bool、
//     input_box は string で来るので tTJSVariant::Type() で判別する。
//---------------------------------------------------------------------------
#ifndef SDL_ELEMENTS_MODAL_RUNNER_H
#define SDL_ELEMENTS_MODAL_RUNNER_H

#include "tjsCommHead.h"
#include "tjsVariant.h"

#include <map>
#include <string>

//! @brief モーダル実行結果。
struct tTVPElementsModalResult
{
	//! 閉じるトリガとなった button の id。 Esc / × 閉じは空文字。
	ttstr Action;
	//! id を持つ state widget (checkbox / toggle_button / slide_switch /
	//! input_box) の最終値マップ。
	std::map<ttstr, tTJSVariant> Values;
};

class iTVPDialogEventHandler;

//! @brief 独立 SDL_Window を立ててモーダルダイアログを実行する。
//!        ダイアログが閉じるまでブロックする。
//! @param json_utf8   JSON テキスト (UTF-8)。 top-level の "size" は無視され、
//!                    実際のウィンドウサイズは引数 width/height で決まる。
//! @param title       ウィンドウタイトル (UTF-16)
//! @param width       ウィンドウ幅 (ピクセル)
//! @param height      ウィンドウ高 (ピクセル)
//! @param handler     button click / 値変化で OnAction が呼ばれるハンドラ
//!                    (nullptr 可)。 "close_on_click": true な button click 後に
//!                    モーダルが閉じる前にも 1 回 OnAction が来る。
//! @param out_result  結果格納先 (action + state widget 値マップ)
//! @return true: 正常実行 (out_result に値が入る) / false: 起動失敗
bool TVPRunElementsModalWindow(
	const std::string& json_utf8,
	const ttstr& title,
	int width, int height,
	iTVPDialogEventHandler* handler,
	tTVPElementsModalResult& out_result);

//! @brief 既存ゲーム window 上にオーバーレイ表示するモーダルダイアログ。
//!        既存 `tTVPElementsDialogManager::ShowFromJsonString` を起動して、
//!        ダイアログが閉じるまで自前 SDL_PollEvent ループを nested で回す。
//!        描画は通常通り DrawDevice::Show() 経由で PaintOverlay が呼ばれる。
//!
//!        独立 window モードと違い、 ゲーム window と独立した OS モーダル
//!        にはならない (z-order や入力ブロックの保証はないが、 オーバーレイ
//!        中は DrawDevice の入力 intercept で全入力がダイアログに吸われる)。
//!
//! @param json_utf8   JSON テキスト (UTF-8)
//! @param handler     button click / 値変化で OnAction が呼ばれるハンドラ
//!                    (nullptr 可)。 ShowFromJsonString と同じ意味論。
//! @param out_result  結果格納先
//! @return true: 正常実行 / false: 起動失敗 (他のダイアログが active 等)
bool TVPRunElementsModalOverlay(
	const std::string& json_utf8,
	iTVPDialogEventHandler* handler,
	tTVPElementsModalResult& out_result);

//! @brief navigator フロー (複数画面遷移) をオーバーレイでブロッキング実行する。
//!        TVPRunElementsModalOverlay と同じ nested pump で、 フローのスタックが
//!        空になる (どこかの画面が <exit> 相当の遷移をする) まで回す。 各画面間の
//!        push/pop/replace は manager 内の navigator が JSON "transitions" を見て
//!        解決し、 画面切替時に handler->OnScreenEnter / OnScreenLeave が呼ばれる。
//! @param manifest_path  app.jsonc 等のマニフェスト (krkrz storage パス, UTF-16)
//! @param handler        各画面の OnAction / OnScreenEnter / OnScreenLeave 通知先
//! @param out_result     最後に閉じた画面の action + state widget 値マップ
//! @return true: 正常実行 / false: 起動失敗 (既に active / manifest 不正等)
bool TVPRunElementsFlowOverlayManifest(
	const ttstr& manifest_path,
	iTVPDialogEventHandler* handler,
	tTVPElementsModalResult& out_result);

//! @brief インライン画面マップ版のフロー実行。 ファイル I/O を介さず、 画面名 →
//!        JSON (UTF-8) のマップと起点画面名でフローを駆動する。 その他は
//!        TVPRunElementsFlowOverlayManifest と同じ。
//! @param screens  画面名 (UTF-8) → JSON (UTF-8)
//! @param entry    起点画面名 (UTF-8)
bool TVPRunElementsFlowOverlayScreens(
	const std::map<std::string, std::string>& screens,
	const std::string& entry,
	iTVPDialogEventHandler* handler,
	tTVPElementsModalResult& out_result);

#endif
