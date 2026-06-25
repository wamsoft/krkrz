//---------------------------------------------------------------------------
// krkrz wrap: 独立 SDL_Window モーダル (Phase 6c step1) はライブラリ
// elements_modal::run_modal にデリゲート、 オーバーレイモーダル (step2) は
// krkrz overlay 経路 (TVPElementsDialogManager) を nested pump で駆動する。
//---------------------------------------------------------------------------
#include "tjsCommHead.h"
#include "SDLElementsModalRunner.h"
#include "DialogEventHandler.h"
#include "ElementsDialogManager.h"
#include "DebugIntf.h"
#include "CharacterSet.h"
#include "Application.h"
#include "WindowForm.h"
#include "app.h"           // SDL3Application (overlay モードの nested iterate 用)

#include <elements_modal/modal.h>

#include <SDL3/SDL.h>

#include <string>

//---------------------------------------------------------------------------
// 内部: elements_modal::result → tTVPElementsModalResult への変換
//---------------------------------------------------------------------------
namespace {

tTVPElementsModalResult ToKrkrzResult(elements_modal::result&& src)
{
	tTVPElementsModalResult out;

	// Action: utf-8 → utf-16
	{
		tjs_string utf16;
		TVPUtf8ToUtf16(utf16, src.action);
		out.Action = ttstr(utf16.c_str());
	}

	// Values: elements_modal::value_t → tTJSVariant
	for (auto&& kv : src.values) {
		tjs_string utf16_id;
		TVPUtf8ToUtf16(utf16_id, kv.first);
		ttstr id(utf16_id.c_str());

		tTJSVariant v;
		std::visit([&](auto&& val) {
			using T = std::decay_t<decltype(val)>;
			if constexpr (std::is_same_v<T, bool>) {
				v = val;
			} else if constexpr (std::is_same_v<T, std::int64_t>) {
				v = static_cast<tjs_int64>(val);
			} else if constexpr (std::is_same_v<T, double>) {
				v = static_cast<tjs_real>(val);
			} else if constexpr (std::is_same_v<T, std::string>) {
				tjs_string u16;
				TVPUtf8ToUtf16(u16, val);
				v = ttstr(u16.c_str());
			}
		}, kv.second);
		out.Values.emplace(std::move(id), std::move(v));
	}
	return out;
}

} // anonymous

//---------------------------------------------------------------------------
// elements_modal::value_t → tTJSVariant 単発変換 + iTVPDialogEventHandler
// 橋渡し callback。 modal callback 経路用。
//---------------------------------------------------------------------------
namespace {
tTJSVariant ValueToVariant(const elements_modal::value_t& v)
{
	tTJSVariant out;
	std::visit([&](auto&& val) {
		using T = std::decay_t<decltype(val)>;
		if constexpr (std::is_same_v<T, bool>) {
			out = val;
		} else if constexpr (std::is_same_v<T, std::int64_t>) {
			out = static_cast<tjs_int64>(val);
		} else if constexpr (std::is_same_v<T, double>) {
			out = static_cast<tjs_real>(val);
		} else if constexpr (std::is_same_v<T, std::string>) {
			tjs_string u16;
			TVPUtf8ToUtf16(u16, val);
			out = ttstr(u16.c_str());
		}
	}, v);
	return out;
}

elements_modal::event_callback MakeHandlerBridge(iTVPDialogEventHandler* handler)
{
	if (!handler) return {};
	return [handler](const std::string& id, bool is_button_click,
	                 const elements_modal::value_t& payload) {
		tjs_string ts_id;
		TVPUtf8ToUtf16(ts_id, id);
		ttstr id_tt(ts_id.c_str());
		if (is_button_click) {
			tTJSVariant empty;
			handler->OnAction(id_tt, empty);
		} else {
			tTJSVariant v = ValueToVariant(payload);
			handler->OnAction(id_tt, v);
		}
	};
}
// 既存ゲームの iterate を nested で回し、 自分のインスタンス (handler が所有)
// が閉じるまで block する。 複数インスタンス共存に対応するため、 「何か
// アクティブか」ではなく「自分の handler のインスタンスがアクティブか」で
// 終了判定する (背景の非モーダル常駐 UI が居ても抜けられる)。 抜けたら結果を
// 取り出す。 overlay モーダル / フロー overlay の双方で共有。
void PumpModalLoop(SDL3Application* app, tTVPElementsDialogManager& mgr,
                   iTVPDialogEventHandler* handler,
                   tTVPElementsModalResult& out_result)
{
	while (mgr.IsHandlerActive(handler)) {
		SDL_PumpEvents();
		SDL_Event ev;
		while (SDL_PollEvent(&ev)) {
			if (ev.type == SDL_EVENT_QUIT) {
				mgr.Close(handler);
				SDL_PushEvent(&ev);
				break;
			}
			app->AppEvent(ev);
			if (!mgr.IsHandlerActive(handler)) break;
		}
		if (!mgr.IsHandlerActive(handler)) break;

		app->AppIterate();
		app->SendPadEvent();
		if (!app->IsInBackground()) app->RequestUpdate();
		app->Dispatch();
		SDL_Delay(8);
	}

	// "close_on_click" / フロー <exit> で finish した場合は mgr が結果を
	// スナップ済み。 Esc/外部 Close 等で結果が無い場合は空のまま返る。
	mgr.TakeLastModalResult(handler, out_result.Action, out_result.Values);
}

}  // anonymous

//---------------------------------------------------------------------------
// Phase 6c step1: 独立 SDL_Window モーダル — elements_modal::run_modal にデリゲート
//---------------------------------------------------------------------------
bool TVPRunElementsModalWindow(
	const std::string& json_utf8,
	const ttstr& title,
	int width, int height,
	iTVPDialogEventHandler* handler,
	tTVPElementsModalResult& out_result)
{
	if (width <= 0 || height <= 0) {
		TVPAddImportantLog(TJS_W("ElementsModal: invalid window size"));
		return false;
	}

	// ELEMENTS_FILE_IO_SUPPORT=OFF ビルドのため、 elements_modal::run_modal は
	// 自前で font 登録できない。 krkrz 側で resource_loader install + 既定
	// font directory のスキャンを先に走らせておく (idempotent)。
	tTVPElementsDialogManager::Instance().EnsureRuntimeInitialized();

	// title: utf-16 → utf-8
	std::string title_utf8;
	{
		tjs_string ts(title.c_str());
		TVPUtf16ToUtf8(title_utf8, ts);
	}
	if (title_utf8.empty()) title_utf8 = "Modal Dialog";

	// 親 SDL_Window: krkrz の main window form から
	SDL_Window* parent_window = nullptr;
	if (Application) {
		if (auto* form = Application->MainWindowForm()) {
			parent_window = static_cast<SDL_Window*>(form->NativeWindowHandle());
		}
	}

	elements_modal::config cfg;
	cfg.title_utf8 = std::move(title_utf8);
	cfg.width      = width;
	cfg.height     = height;
	cfg.parent     = parent_window;
	cfg.on_event   = MakeHandlerBridge(handler);
	// font_directory は空 — elements_modal 側の default 探索 (external/elements/...)
	// に任せる。 krkrz と同じ "data/font" を見たいときは別途指定する余地あり。

	elements_modal::result r;
	if (!elements_modal::run_modal(json_utf8, cfg, r)) {
		return false;
	}
	out_result = ToKrkrzResult(std::move(r));
	return true;
}

//---------------------------------------------------------------------------
// Phase 6c step2: オーバーレイモーダル — krkrz DialogManager 経路で起動して
// SDL_PollEvent を nested で回し、 閉じるまで block。 dialog の自動終了は
// overlay_session 側で "close_on_click": true な button を契機に行い、 manager
// が結果を保存する。 呼出側は handler に転送された onAction イベントの中で
// `dlg.close()` を呼んで終了させることもできる。
//---------------------------------------------------------------------------
bool TVPRunElementsModalOverlay(
	const std::string& json_utf8,
	iTVPDialogEventHandler* handler,
	tTVPElementsModalResult& out_result)
{
	auto& mgr = tTVPElementsDialogManager::Instance();
	if (mgr.IsHandlerActive(handler)) {
		TVPAddImportantLog(TJS_W("ElementsModal overlay: this handler already active"));
		return false;
	}

	SDL3Application* app = GetSDL3Application();
	if (!app) {
		TVPAddImportantLog(TJS_W("ElementsModal overlay: SDL3Application not available"));
		return false;
	}

	// 呼出側が用意した handler をそのまま渡す。 state widget 値変化と
	// 全 button click が OnAction に来る。 "close_on_click": true な button は
	// onAction 発火後に session が自動 finish → PaintOverlay が結果スナップして
	// mgr を teardown する。 modal=true で背景の非モーダル UI / ゲームへ入力を
	// 通さない。
	if (!mgr.ShowFromJsonString(json_utf8, handler, nullptr, /*modal=*/true)) {
		return false;
	}

	// 既存ゲームの iterate を nested で回し、 閉じたら結果を取り出す。
	PumpModalLoop(app, mgr, handler, out_result);
	return true;
}

//---------------------------------------------------------------------------
// navigator フロー (複数画面遷移) のブロッキング実行 — overlay 版
//---------------------------------------------------------------------------
bool TVPRunElementsFlowOverlayManifest(
	const ttstr& manifest_path,
	iTVPDialogEventHandler* handler,
	tTVPElementsModalResult& out_result)
{
	auto& mgr = tTVPElementsDialogManager::Instance();
	if (mgr.IsHandlerActive(handler)) {
		TVPAddImportantLog(TJS_W("ElementsFlow overlay: this handler already active"));
		return false;
	}
	SDL3Application* app = GetSDL3Application();
	if (!app) {
		TVPAddImportantLog(TJS_W("ElementsFlow overlay: SDL3Application not available"));
		return false;
	}
	// ブロッキングフローは modal=true (背景 UI / ゲームへ入力を通さない)。
	if (!mgr.StartFlowFromManifest(manifest_path, handler, nullptr, /*modal=*/true)) {
		return false;
	}
	PumpModalLoop(app, mgr, handler, out_result);
	return true;
}

bool TVPRunElementsFlowOverlayScreens(
	const std::map<std::string, std::string>& screens,
	const std::string& entry,
	iTVPDialogEventHandler* handler,
	tTVPElementsModalResult& out_result)
{
	auto& mgr = tTVPElementsDialogManager::Instance();
	if (mgr.IsHandlerActive(handler)) {
		TVPAddImportantLog(TJS_W("ElementsFlow overlay: this handler already active"));
		return false;
	}
	SDL3Application* app = GetSDL3Application();
	if (!app) {
		TVPAddImportantLog(TJS_W("ElementsFlow overlay: SDL3Application not available"));
		return false;
	}
	if (!mgr.StartFlowFromScreens(screens, entry, handler, nullptr, /*modal=*/true)) {
		return false;
	}
	PumpModalLoop(app, mgr, handler, out_result);
	return true;
}
