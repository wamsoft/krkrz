//---------------------------------------------------------------------------
// 画面 JSON から overlay_session を組み立てる共通部 (ElementsSessionBuild.h)
//---------------------------------------------------------------------------
#include "tjsCommHead.h"
#include "ElementsSessionBuild.h"

#include "DialogEventHandler.h"
#include "ElementsDialogManager.h"   // Dispatch{Action,Drag,Var} (通知キューは 1 本)
#include "CharacterSet.h"            // TVPUtf8ToUtf16 / TVPUtf16ToUtf8
#include "tjsDictionary.h"           // TJSCreateDictionaryObject

#include <set>
#include <type_traits>
#include <variant>
#include <vector>

namespace tvp_elements {

//---------------------------------------------------------------------------
ttstr Utf8ToTtstr(const std::string& utf8)
{
	tjs_string ts;
	TVPUtf8ToUtf16(ts, utf8);
	return ttstr(ts.c_str());
}

//---------------------------------------------------------------------------
std::string TtstrToUtf8(const ttstr& s)
{
	std::string out;
	tjs_string ts(s.c_str());
	TVPUtf16ToUtf8(out, ts);
	return out;
}

//---------------------------------------------------------------------------
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
			out = Utf8ToTtstr(val);
		}
	}, v);
	return out;
}

//---------------------------------------------------------------------------
tTJSVariant DragEventToDict(const elements_modal::drag_event& d)
{
	iTJSDispatch2* dict = TJSCreateDictionaryObject();
	if (!dict) return tTJSVariant();
	auto put = [dict](const tjs_char* key, const tTJSVariant& v) {
		tTJSVariant tmp = v;
		dict->PropSet(TJS_MEMBERENSURE, key, nullptr, &tmp, dict);
	};
	using phase = elements_modal::drag_event::phase;
	const tjs_char* ph = (d.ph == phase::begin) ? TJS_W("begin")
	                   : (d.ph == phase::move)  ? TJS_W("move")
	                                            : TJS_W("end");
	put(TJS_W("id"),        tTJSVariant(Utf8ToTtstr(d.id)));
	put(TJS_W("phase"),     tTJSVariant(ttstr(ph)));
	put(TJS_W("x"),         tTJSVariant(static_cast<tjs_real>(d.x)));
	put(TJS_W("y"),         tTJSVariant(static_cast<tjs_real>(d.y)));
	put(TJS_W("dx"),        tTJSVariant(static_cast<tjs_real>(d.dx)));
	put(TJS_W("dy"),        tTJSVariant(static_cast<tjs_real>(d.dy)));
	put(TJS_W("startX"),    tTJSVariant(static_cast<tjs_real>(d.start_x)));
	put(TJS_W("startY"),    tTJSVariant(static_cast<tjs_real>(d.start_y)));
	put(TJS_W("modifiers"), tTJSVariant(static_cast<tjs_int>(d.modifiers)));
	tTJSVariant out(dict, dict);
	dict->Release();
	return out;
}

//---------------------------------------------------------------------------
// iTVPDialogEventHandler に転送する event_callback を作る。
// button click は payload=void、 state widget は実値を渡す krkrz 慣習を維持。
// 直接 handler->OnAction は呼ばず manager の DispatchAction を経由する:
// session->update() の最中に発火した action は、 その場で配送するとコールバック
// 内のブロッキングモーダル (System.inputString 等) が window update の再入禁止に
// 阻まれて描画不能になるため、 update の外へ遅延させる必要がある。
//---------------------------------------------------------------------------
namespace {

elements_modal::event_callback MakeBridgeCallback(iTVPDialogEventHandler* handler)
{
	if (!handler) return {};
	return [handler](const std::string& id, bool is_button_click,
	                 const elements_modal::value_t& payload) {
		if (!handler) return;
		ttstr id_tt = Utf8ToTtstr(id);
		if (is_button_click) {
			tTJSVariant empty;
			tTVPElementsDialogManager::Instance().DispatchAction(handler, id_tt, empty);
		} else {
			tTJSVariant v = ValueToVariant(payload);
			tTVPElementsDialogManager::Instance().DispatchAction(handler, id_tt, v);
		}
	};
}

} // anonymous

//---------------------------------------------------------------------------
void ApplyVarWatch(elements_modal::overlay_session& session,
                   iTVPDialogEventHandler* handler)
{
	std::vector<ttstr> names;
	if (!handler || !handler->WantsVarNotify(names)) {
		session.set_var_watcher(elements_modal::overlay_session::var_watcher{});
		return;
	}
	auto filter = std::make_shared<std::set<std::string>>();
	for (auto const& n : names) filter->insert(TtstrToUtf8(n));
	// handler は «そのとき生きているインスタンス» の照合キーとしてのみ使う
	// (配送前に manager が存在確認するので、 detach 済みの handler へ配送
	//  されることはない)。
	session.set_var_watcher(
		[handler, filter](const std::string& name, const std::string& value) {
			if (!filter->empty() && !filter->count(name)) return;
			tTVPElementsDialogManager::Instance().DispatchVar(
				handler, Utf8ToTtstr(name), Utf8ToTtstr(value));
		});
}

//---------------------------------------------------------------------------
std::unique_ptr<elements_modal::overlay_session> BuildSession(
	const std::string& json_utf8,
	const SessionOptions& opt,
	iTVPDialogEventHandler* handler)
{
	if (opt.width <= 0 || opt.height <= 0) return nullptr;

	auto sess = std::make_unique<elements_modal::overlay_session>();
	// pixel_scale は 1.0 固定 — 実際の描画密度は render_to_buffer に渡す
	// buffer サイズから毎回導出される。
	if (!sess->start(json_utf8, opt.width, opt.height, 1.0f,
	                 MakeBridgeCallback(handler), opt.resource_base)) {
		return nullptr;
	}

	// ドラッグ通知 ("drag_events": true の widget)。 action と同じキューへ積む
	// ので、 update 中に発火しても安全で、 押下/移動/離すの順序も保たれる。
	if (handler) {
		sess->set_drag_callback([handler](const elements_modal::drag_event& d) {
			const bool coalesce =
				(d.ph == elements_modal::drag_event::phase::move);
			tTVPElementsDialogManager::Instance().DispatchDrag(
				handler, DragEventToDict(d), coalesce);
		});
	}

	// i18n: ホストが言語を決めているなら、 新しく開く画面もその言語で始める
	// (画面 JSON の "lang" 既定より優先)。 "strings" を持たない画面では no-op。
	if (!opt.language.empty()) sess->set_language(opt.language);

	// 変数観測 (OnVar)。 観測を望まない handler では watcher を張らない。
	ApplyVarWatch(*sess, handler);
	return sess;
}

} // namespace tvp_elements
