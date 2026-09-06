//---------------------------------------------------------------------------
// Elements の画面をホストのレイヤへ描くパネル (ElementsLayerPanel.h)
//---------------------------------------------------------------------------
#include "tjsCommHead.h"
#include "ElementsLayerPanel.h"

#include "DialogEventHandler.h"
#include "ElementsDialogManager.h"   // EnsureRuntimeInitialized / 通知キュー共用
#include "ElementsInputMap.h"        // VK / マウス → cycfi 中立入力型
#include "ElementsSessionBuild.h"    // BuildSession / ApplyVarWatch / 文字列変換
#include "LayerIntf.h"               // tTJSNI_BaseLayer
#include "tvpgl.h"                   // TVPConvertAdditiveAlphaToAlpha
#include "DebugIntf.h"

#include <elements_modal/modal.h>

#include <algorithm>
#include <cstring>   // std::memcpy

namespace {

using namespace tvp_elements;
using namespace tvp_elements_input;

//! 開いているパネル。 プロセス全体の fan-out (言語 / 再描画要求) だけに使う。
//! z 順も入力の調停も持たない (それは吉里吉里のレイヤツリーの仕事)。
std::vector<tTVPElementsLayerPanel*>& PanelRegistry()
{
	static std::vector<tTVPElementsLayerPanel*> reg;
	return reg;
}

} // anonymous

//---------------------------------------------------------------------------
tTVPElementsLayerPanel::tTVPElementsLayerPanel(
	tTJSNI_BaseLayer* layer, iTVPDialogEventHandler* handler)
	: Layer(layer), Handler(handler)
{
}

//---------------------------------------------------------------------------
tTVPElementsLayerPanel::~tTVPElementsLayerPanel()
{
	Close();
}

//---------------------------------------------------------------------------
void tTVPElementsLayerPanel::ReleaseBuffer()
{
	Staging.clear();
	Staging.shrink_to_fit();
	BufW = BufH = 0;
}

//---------------------------------------------------------------------------
bool tTVPElementsLayerPanel::EnsureBuffer()
{
	if (!Layer) return false;
	const int w = static_cast<int>(Layer->GetImageWidth());
	const int h = static_cast<int>(Layer->GetImageHeight());
	if (w <= 0 || h <= 0) return false;
	if (w == BufW && h == BufH && !Staging.empty()) return true;
	// サイズが変わったら作り直す。 部分再描画は «前回描画が残っている» ことを
	// 前提にするので、 作り直した直後のフレームは session 側が全面へ落ちる
	// (buffer サイズの変化を見て自動でフォールバックする)。
	Staging.assign(static_cast<std::size_t>(w) * static_cast<std::size_t>(h), 0u);
	BufW = w;
	BufH = h;
	return true;
}

//---------------------------------------------------------------------------
bool tTVPElementsLayerPanel::Open(const std::string& json_utf8,
                                  const std::string& resource_base_utf8)
{
	Close();
	if (!Layer) return false;

	// ThorVG / フォントの初期化はプロセス全体で 1 回。 overlay と共有する。
	tTVPElementsDialogManager::Instance().EnsureRuntimeInitialized();

	if (!EnsureBuffer()) return false;

	tvp_elements::SessionOptions opt;
	opt.width         = BufW;
	opt.height        = BufH;
	opt.resource_base = resource_base_utf8;
	// 表示言語は overlay と同じものを既定にする (プロセス全体の設定)。
	opt.language      = TtstrToUtf8(
		tTVPElementsDialogManager::Instance().GetLanguage());

	Session = tvp_elements::BuildSession(json_utf8, opt, Handler);
	if (!Session) {
		ReleaseBuffer();
		return false;
	}
	ResourceBase = resource_base_utf8;

	// 通知キューは overlay と 1 本を共用するので、 配送前の生存確認を通す
	// ために handler を登録しておく。
	tTVPElementsDialogManager::Instance().RegisterExternalHandler(Handler);

	// 毎フレームの駆動。 継続イベントは window update の外で呼ばれるので、
	// ここから TJS を走らせても再入の問題が無い。
	if (!HookRegistered) {
		TVPAddContinuousEventHook(this);
		HookRegistered = true;
	}
	LastTick = 0;

	PanelRegistry().push_back(this);
	return true;
}

//---------------------------------------------------------------------------
void tTVPElementsLayerPanel::Close()
{
	if (HookRegistered) {
		TVPRemoveContinuousEventHook(this);
		HookRegistered = false;
	}
	auto& reg = PanelRegistry();
	reg.erase(std::remove(reg.begin(), reg.end(), this), reg.end());

	if (Session) {
		tTVPElementsDialogManager::Instance().UnregisterExternalHandler(Handler);
		Session.reset();
	}
	ReleaseBuffer();
}

//---------------------------------------------------------------------------
void tTVPElementsLayerPanel::NotifyLayerResized()
{
	if (!Session || !Layer) return;
	const int w = static_cast<int>(Layer->GetImageWidth());
	const int h = static_cast<int>(Layer->GetImageHeight());
	if (w <= 0 || h <= 0) return;
	if (w == BufW && h == BufH) return;
	if (!EnsureBuffer()) return;
	Session->notify_view_resize(BufW, BufH);
	Session->invalidate();
}

//---------------------------------------------------------------------------
// staging の矩形をレイヤの画像バッファへ転送する。
//
// elements は **premultiplied な 0xAARRGGBB** を書く (ThorVG の
// ColorSpace::ARGB8888)。 吉里吉里のレイヤは
//   ltAddAlpha … 加算アルファ = premultiplied なのでそのまま
//   それ以外   … straight alpha なので行ごとに un-premultiply する
// レイヤのビットマップは pitch が w*4 と一致しないことがあるので行単位で写す。
//---------------------------------------------------------------------------
void tTVPElementsLayerPanel::BlitToLayer(int x, int y, int w, int h)
{
	if (!Layer || Staging.empty()) return;
	if (w <= 0 || h <= 0) return;

	// staging の範囲へクリップ
	if (x < 0) { w += x; x = 0; }
	if (y < 0) { h += y; y = 0; }
	if (x + w > BufW) w = BufW - x;
	if (y + h > BufH) h = BufH - y;
	if (w <= 0 || h <= 0) return;

	tjs_uint8* base = static_cast<tjs_uint8*>(Layer->GetMainImagePixelBufferForWrite());
	if (!base) return;
	const tjs_int pitch = Layer->GetMainImagePixelBufferPitch();
	const bool premul = (Layer->GetType() == ltAddAlpha);

	for (int row = 0; row < h; ++row) {
		const tjs_uint32* src = Staging.data() +
			static_cast<std::size_t>(y + row) * static_cast<std::size_t>(BufW) + x;
		tjs_uint32* dst = reinterpret_cast<tjs_uint32*>(base + (y + row) * pitch) + x;
		std::memcpy(dst, src, static_cast<std::size_t>(w) * 4);
		if (!premul) TVPConvertAdditiveAlphaToAlpha(dst, w);
	}

	// レイヤ座標へ直して update (画像がレイヤ内でオフセットしている場合を考慮)。
	const tjs_int ox = Layer->GetImageLeft();
	const tjs_int oy = Layer->GetImageTop();
	Layer->Update(tTVPRect(x + ox, y + oy, x + ox + w, y + oy + h));
}

//---------------------------------------------------------------------------
void TJS_INTF_METHOD
tTVPElementsLayerPanel::OnContinuousCallback(tjs_uint64 tick)
{
	if (!Session || !Layer) return;
	if (InUpdate) return;   // 念のため (通知から再入した場合)

	// レイヤの画像サイズが外から変えられていたら追随する。
	if (static_cast<int>(Layer->GetImageWidth())  != BufW ||
	    static_cast<int>(Layer->GetImageHeight()) != BufH) {
		NotifyLayerResized();
		if (!Session) return;
	}

	// 経過 ms。 継続イベントの間隔はフレームと一致しないので実測差分を使う。
	// 初回と、 長く止まっていた後 (別画面から戻った等) は 1 フレーム相当に丸める
	// (演出が一気に飛ぶのを防ぐ)。
	float dt_ms = 16.0f;
	if (LastTick != 0 && tick > LastTick) {
		const tjs_uint64 d = tick - LastTick;
		dt_ms = (d > 200) ? 16.0f : static_cast<float>(d);
	}
	LastTick = tick;

	bool need_render = false;
	{
		// update() の最中に発火した通知は即時配送させない (manager の paint
		// 深度と同じ扱い)。 コールバックから session を触り直せてしまうため。
		tTVPElementsDialogManager::Instance().PushDeferScope();
		InUpdate = true;
		need_render = Session->update();
		InUpdate = false;
		tTVPElementsDialogManager::Instance().PopDeferScope();
	}
	if (!Session) return;   // update 中に閉じられた場合

	if (!need_render) return;

	elements_modal::overlay_session::render_rect rect{};
	elements_modal::overlay_session::render_rect updated{};
	// surface に 0,0 を渡して session 内部のアンカー配置を無効化する
	// (out_rect = (0,0,コンテンツ実寸))。 これで **レイヤ local 座標 =
	// view local 座標**になり、 入力の座標変換が要らなくなる。
	const bool ok = Session->render_to_buffer_partial(
		Staging.data(), BufW, BufH, 0, 0, rect, updated);
	if (!ok) return;   // 終了済み (finish 後は描画しない)

	if (updated.w <= 0 || updated.h <= 0) {
		BlitToLayer(0, 0, BufW, BufH);
	} else {
		BlitToLayer(updated.x, updated.y, updated.w, updated.h);
	}
}

//---------------------------------------------------------------------------
// 入力 (座標はレイヤ local のまま session へ)
//---------------------------------------------------------------------------
void tTVPElementsLayerPanel::MouseDown(
	tjs_int x, tjs_int y, tTVPMouseButton mb, tjs_uint32 flags)
{
	if (!Session) return;
	Session->on_mouse_down(static_cast<float>(x), static_cast<float>(y),
		MouseButtonToElements(mb), FlagsToElementsMods(flags));
}

void tTVPElementsLayerPanel::MouseUp(
	tjs_int x, tjs_int y, tTVPMouseButton mb, tjs_uint32 flags)
{
	if (!Session) return;
	Session->on_mouse_up(static_cast<float>(x), static_cast<float>(y),
		MouseButtonToElements(mb), FlagsToElementsMods(flags));
}

void tTVPElementsLayerPanel::MouseMove(tjs_int x, tjs_int y, tjs_uint32 flags)
{
	if (!Session) return;
	Session->on_mouse_move(static_cast<float>(x), static_cast<float>(y),
		FlagsToElementsMods(flags));
}

void tTVPElementsLayerPanel::MouseWheel(
	tjs_int delta, tjs_int x, tjs_int y, tjs_uint32 flags)
{
	if (!Session) return;
	(void)flags;
	// 吉里吉里のホイールは 120 単位。 elements 側は「ノッチ数」で受ける。
	const float dy = static_cast<float>(delta) / 120.0f;
	Session->on_mouse_wheel(0.0f, dy,
		static_cast<float>(x), static_cast<float>(y));
}

void tTVPElementsLayerPanel::MouseLeave()
{
	if (!Session) return;
	Session->on_mouse_leave();
}

//---------------------------------------------------------------------------
bool tTVPElementsLayerPanel::KeyDown(tjs_uint key, tjs_uint32 shift)
{
	if (!Session) return false;
	auto r = RouteVk(key);
	switch (r.k) {
		case vk_routing::kind::key:
			return Session->on_key_down(r.key,
				FlagsToElementsMods(shift) | r.extra_mods);
		case vk_routing::kind::pad_button:
			return Session->on_pad_button(r.pad, true);
		case vk_routing::kind::none:
		default:
			return false;
	}
}

bool tTVPElementsLayerPanel::KeyUp(tjs_uint key, tjs_uint32 shift)
{
	if (!Session) return false;
	auto r = RouteVk(key);
	switch (r.k) {
		case vk_routing::kind::key:
			return Session->on_key_up(r.key,
				FlagsToElementsMods(shift) | r.extra_mods);
		case vk_routing::kind::pad_button:
			return Session->on_pad_button(r.pad, false);
		case vk_routing::kind::none:
		default:
			return false;
	}
}

void tTVPElementsLayerPanel::TextInput(const char* utf8_text)
{
	if (!Session || !utf8_text) return;
	Session->on_text_input(utf8_text);
}

//---------------------------------------------------------------------------
// 状態
//---------------------------------------------------------------------------
bool tTVPElementsLayerPanel::SetVar(const ttstr& name, const ttstr& value)
{
	if (!Session) return false;
	Session->set_var(TtstrToUtf8(name), TtstrToUtf8(value));
	return true;
}

bool tTVPElementsLayerPanel::GetVar(const ttstr& name, ttstr& out) const
{
	if (!Session) return false;
	std::string v;
	if (!Session->get_var(TtstrToUtf8(name), v)) return false;
	out = Utf8ToTtstr(v);
	return true;
}

std::vector<tTVPElementsLayerPanel::VarInfo>
tTVPElementsLayerPanel::DescribeVars() const
{
	std::vector<VarInfo> out;
	if (!Session) return out;
	for (auto const& d : Session->list_vars()) {
		VarInfo vi;
		vi.name  = Utf8ToTtstr(d.name);
		vi.value = Utf8ToTtstr(d.value);
		for (auto const& u : d.used_by) {
			vi.used_by.emplace_back(Utf8ToTtstr(u.first), Utf8ToTtstr(u.second));
		}
		out.push_back(std::move(vi));
	}
	return out;
}

void tTVPElementsLayerPanel::RefreshVarWatch()
{
	if (!Session) return;
	tvp_elements::ApplyVarWatch(*Session, Handler);
}

void tTVPElementsLayerPanel::SetLanguage(const std::string& lang)
{
	if (!Session) return;
	Session->set_language(lang);
}

void tTVPElementsLayerPanel::Invalidate()
{
	if (!Session) return;
	Session->invalidate();
}

bool tTVPElementsLayerPanel::FocusById(const ttstr& id)
{
	if (!Session) return false;
	Session->focus_by_id(TtstrToUtf8(id));
	return true;
}

bool tTVPElementsLayerPanel::ActivateById(const ttstr& id)
{
	if (!Session) return false;
	return Session->activate_by_id(TtstrToUtf8(id));
}

ttstr tTVPElementsLayerPanel::FocusedId() const
{
	if (!Session) return ttstr();
	return Utf8ToTtstr(Session->focused_id());
}

bool tTVPElementsLayerPanel::IsFinished() const
{
	if (!Session) return true;
	return Session->finished();
}

//---------------------------------------------------------------------------
// プロセス全体の fan-out
//---------------------------------------------------------------------------
void tTVPElementsLayerPanel::InvalidateAll()
{
	// 反復中に閉じられても壊れないよう複製してから回す。
	auto snapshot = PanelRegistry();
	for (auto* p : snapshot) p->Invalidate();
}

void tTVPElementsLayerPanel::SetLanguageAll(const std::string& lang)
{
	auto snapshot = PanelRegistry();
	for (auto* p : snapshot) p->SetLanguage(lang);
}
