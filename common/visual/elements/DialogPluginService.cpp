//---------------------------------------------------------------------------
//!@file SDL 拡張プラグイン向け Elements ダイアログサービス実装
//
// tp_dialog_service.h の C ABI を tTVPElementsDialogManager に橋渡しする。
// handle = PluginDialogHandler* (iTVPDialogEventHandler 実装)。 生存管理は
// g_live_handlers レジストリで行い、 teardown (OnClosed) 後の handle を
// 渡されても未定義動作にならないようにする。
//---------------------------------------------------------------------------
#include "tjsCommHead.h"
#include "tp_dialog_service.h"
#include "ElementsDialogManager.h"
#include "DialogEventHandler.h"
#include "CharacterSet.h"

#include <set>
#include <string>

namespace {

ttstr Utf8ToTtstr(const char* utf8)
{
	if (!utf8) return ttstr();
	tjs_string ts;
	TVPUtf8ToUtf16(ts, utf8);
	return ttstr(ts.c_str());
}

std::string TtstrToUtf8(const ttstr& s)
{
	std::string out;
	tjs_string ts(s.c_str());
	TVPUtf16ToUtf8(out, ts);
	return out;
}

class PluginDialogHandler : public iTVPDialogEventHandler
{
public:
	TVPDialogActionCallback on_action = nullptr;
	TVPDialogCloseCallback  on_close  = nullptr;
	void* user = nullptr;

	void OnAction(const ttstr& id, const tTJSVariant& payload) override
	{
		if (!on_action) return;
		std::string id8 = TtstrToUtf8(id);
		if (payload.Type() == tvtVoid) {
			on_action(user, id8.c_str(), nullptr);
		} else {
			std::string payload8 = TtstrToUtf8(ttstr(payload));
			on_action(user, id8.c_str(), payload8.c_str());
		}
	}

	void OnClosed(const ttstr& action) override;
};

// 生きている handle のレジストリ。 全 API 関数が membership を確認してから
// 触るので、 teardown 済 handle を渡されても失敗扱いで済む。
std::set<PluginDialogHandler*>& LiveHandlers()
{
	static std::set<PluginDialogHandler*> s;
	return s;
}

void PluginDialogHandler::OnClosed(const ttstr& action)
{
	// レジストリから外してから通知 → callback 中に自分の handle を使った
	// API 呼出があっても「死んだ handle」として安全に失敗する。
	LiveHandlers().erase(this);
	if (on_close) {
		std::string action8 = TtstrToUtf8(action);
		on_close(user, action8.c_str());
	}
	delete this;
}

PluginDialogHandler* ResolveLive(TVPDialogHandle handle)
{
	auto* h = static_cast<PluginDialogHandler*>(handle);
	if (!h || LiveHandlers().count(h) == 0) return nullptr;
	return h;
}

//---------------------------------------------------------------------------
// API 実装
//---------------------------------------------------------------------------
TVPDialogHandle ShowOverlayJson(const char* json_utf8, int modal, int grab_focus,
                                TVPDialogActionCallback on_action,
                                TVPDialogCloseCallback on_close, void* user)
{
	if (!json_utf8) return nullptr;
	auto* handler = new PluginDialogHandler();
	handler->on_action = on_action;
	handler->on_close  = on_close;
	handler->user      = user;

	auto& mgr = tTVPElementsDialogManager::Instance();
	if (!mgr.ShowFromJsonString(std::string(json_utf8), handler,
	                            /*hostDevice=*/nullptr,
	                            modal != 0, grab_focus != 0)) {
		// show 失敗時は OnClosed が発火しない (ever_active ガード) ので
		// ここで直接破棄する。
		delete handler;
		return nullptr;
	}
	LiveHandlers().insert(handler);
	return handler;
}

void CloseDialog(TVPDialogHandle handle)
{
	if (auto* h = ResolveLive(handle)) {
		tTVPElementsDialogManager::Instance().Close(h);
	}
}

int IsActive(TVPDialogHandle handle)
{
	auto* h = ResolveLive(handle);
	if (!h) return 0;
	return tTVPElementsDialogManager::Instance().IsHandlerActive(h) ? 1 : 0;
}

int SetVar(TVPDialogHandle handle, const char* name, const char* value_utf8)
{
	auto* h = ResolveLive(handle);
	if (!h || !name) return 0;
	return tTVPElementsDialogManager::Instance().SetVar(
		h, Utf8ToTtstr(name), Utf8ToTtstr(value_utf8)) ? 1 : 0;
}

const TVPSDLDialogAPI_v1 g_api_v1 = {
	TVP_SDL_DIALOG_API_VERSION,
	ShowOverlayJson,
	CloseDialog,
	IsActive,
	SetVar,
};

} // anonymous

extern "C" const TVPSDLDialogAPI_v1* TVPGetSDLDialogAPI(uint32_t version)
{
	if (version != TVP_SDL_DIALOG_API_VERSION) return nullptr;
	return &g_api_v1;
}
