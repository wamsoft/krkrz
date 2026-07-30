//---------------------------------------------------------------------------
//!@file SDL 拡張プラグイン向け Elements ダイアログサービス (C ABI)
//
// 静的リンクプラグイン (tp_stub ベース) から Elements overlay ダイアログ機構
// (tTVPElementsDialogManager) を使うための C インタフェース。 tp_stub は
// 本体と別系統の TJS 型定義を持つため、 本体ヘッダ (tjsCommHead.h /
// ElementsDialogManager.h) をプラグイン TU に include できない。 そこで
// 文字列は UTF-8 の char*、 通知は関数ポインタ + user データ、 インスタンス
// 識別は opaque handle に落とした ABI を切る。
//
// 取得方法 (静的リンクなので直接シンボル解決):
//   #include "tp_dialog_service.h"
//   const TVPSDLDialogAPI_v1* api = TVPGetSDLDialogAPI(TVP_SDL_DIALOG_API_VERSION);
//   if (!api) { /* Elements 無効ビルド or バージョン不一致 */ }
//
// 契約:
//   - 全関数はメインスレッド (TJS / エンジンスレッド) からのみ呼ぶこと。
//   - handle は on_close コールバックが返るまで有効。 それ以降は無効
//     (指しているものは破棄済み)。 各 API は生存確認をするので、 死んだ
//     handle を渡しても未定義動作にはならず失敗扱い (no-op / 0) になる。
//   - on_action / on_close は overlay の駆動中 (PaintOverlay / 入力
//     フォワード) に同スレッドで直接呼ばれる。 その中から show / close /
//     set_var を呼んでも安全。
//
// 将来の拡張 (SDL_Renderer への overlay 描画 hook 等) は version を上げた
// 別 struct (TVPSDLDialogAPI_v2 ...) を同じ entry point から返す。
//---------------------------------------------------------------------------
#ifndef TP_DIALOG_SERVICE_H
#define TP_DIALOG_SERVICE_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define TVP_SDL_DIALOG_API_VERSION 1u

//! overlay ダイアログインスタンスの opaque handle。
typedef void* TVPDialogHandle;

//! button click / state widget 値変化の通知。 payload_text は値の文字列表現
//! (UTF-8)。 button click 等 payload が無いイベントでは NULL。 文字列は
//! コールバック中のみ有効 (保持するならコピーすること)。
typedef void (*TVPDialogActionCallback)(void* user, const char* id,
                                        const char* payload_text);

//! インスタンス teardown 完了通知 (1 回だけ)。 action は close_on_click /
//! Esc で閉じた button の id (close() 呼出や Window close 等の外部要因は
//! 空文字列)。 この呼出が返った時点で handle は無効になる。
typedef void (*TVPDialogCloseCallback)(void* user, const char* action);

typedef struct TVPSDLDialogAPI_v1 {
	uint32_t version;   //!< = TVP_SDL_DIALOG_API_VERSION

	//! JSON (UTF-8) から overlay ダイアログを表示する (非ブロッキング)。
	//! modal:      非 0 で入力独占 (下のゲーム / UI に入力を通さない)。
	//! grab_focus: 非 0 でキーボード/パッドフォーカスを取得 (modal 時は
	//!             常に取得され、 この引数は無視される)。
	//! 失敗 (JSON パースエラー等) は NULL。 コールバックは main thread。
	TVPDialogHandle (*show_overlay_json)(const char* json_utf8,
	                                     int modal, int grab_focus,
	                                     TVPDialogActionCallback on_action,
	                                     TVPDialogCloseCallback on_close,
	                                     void* user);

	//! ダイアログを閉じる (次フレームで teardown → on_close 発火)。
	void (*close)(TVPDialogHandle handle);

	//! handle が表示中 (teardown 前) なら非 0。
	int (*is_active)(TVPDialogHandle handle);

	//! 変数 store へ書込。 JSON で "text_var": name を指定した label が
	//! 次フレームで自動更新される。 非アクティブ handle は 0 (失敗)。
	int (*set_var)(TVPDialogHandle handle,
	               const char* name, const char* value_utf8);
} TVPSDLDialogAPI_v1;

//! サービス取得。 対応しない version、 または Elements 無効ビルド
//! (KRKRZ_USE_ELEMENTS=OFF) では NULL を返す…ことにしたいが、 静的リンクでは
//! OFF 時にシンボル自体が存在しない。 プラグイン側はリンク成立 = Elements
//! 有効を前提としてよい (プラグインの組み込み自体を CMake 側で
//! KRKRZ_USE_ELEMENTS にゲートする)。
const TVPSDLDialogAPI_v1* TVPGetSDLDialogAPI(uint32_t version);

#ifdef __cplusplus
}
#endif

#endif
