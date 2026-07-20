//---------------------------------------------------------------------------
// Elements ベース汎用ダイアログ管理 (R6: 複数インスタンス同時表示対応)
//
// 単一 session を z-order 付きの「インスタンスリスト」に拡張し、 複数の
// 非モーダル UI を同時に出しっぱなしにできるようにした。 各インスタンスは
//   - overlay_session (+ navigator フロー状態)
//   - iTVPDialogEventHandler ブリッジ
//   - host DrawDevice / 配置 / 描画レイヤ (renderer のテクスチャキー)
//   - modal フラグ (入力独占するか)
// を個別に持つ。 描画 (PaintOverlay) は z-order 奥→手前に全インスタンスを
// 合成描画し、 入力は最前面 modal が独占 / 非モーダルはヒットテストで配送する。
//
// 描画と入力ロジック自体は従来どおり overlay_session 側で完結する。
//---------------------------------------------------------------------------
#include "tjsCommHead.h"
#include "ElementsDialogManager.h"
#include "DialogEventHandler.h"
#include "DialogRenderer.h"
#include "DebugIntf.h"
#include "MsgIntf.h"
#include "CharacterSet.h"   // TVPUtf8ToUtf16
#include "StorageIntf.h"    // TVPReadStream
#include "Application.h"    // Application, MainWindowForm()
#include "WindowForm.h"     // TTVPWindowForm::NativeWindowHandle()
#include "StoragesResourceLoader.h"   // TVPInstallElementsResourceLoader / Fonts

#ifndef _WIN32
#include "VirtualKey.h"
#endif

#include <elements_modal/modal.h>
#include <elements_modal/navigator.h>   // フロー駆動 (画面遷移スタック)

#include <SDL3/SDL.h>        // SDL_BUTTON_* / SDL_KMOD_*

#include <cstdlib>           // std::strtol
#include <map>
#include <memory>
#include <string>
#include <type_traits>
#include <vector>

//---------------------------------------------------------------------------
// 内部: krkrz ttstr ⇔ utf-8、 value_t → tTJSVariant 変換 + handler ブリッジ
//---------------------------------------------------------------------------
namespace {

ttstr Utf8ToTtstr(const std::string& utf8)
{
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

// iTVPDialogEventHandler に転送する event_callback を作る。
// button click は payload=void、 state widget は実値を渡す krkrz 慣習を維持。
elements_modal::event_callback MakeBridgeCallback(iTVPDialogEventHandler* handler)
{
	if (!handler) return {};
	return [handler](const std::string& id, bool is_button_click,
	                 const elements_modal::value_t& payload) {
		if (!handler) return;
		ttstr id_tt = Utf8ToTtstr(id);
		if (is_button_click) {
			tTJSVariant empty;
			handler->OnAction(id_tt, empty);
		} else {
			tTJSVariant v = ValueToVariant(payload);
			handler->OnAction(id_tt, v);
		}
	};
}

// krkrz storage パスのディレクトリ部 (末尾 '/' 込み) を返す。 区切りが無ければ
// 空文字 (= カレント相当)。 '/' と '\\' の両方を見る。
ttstr DirOfStoragePath(const ttstr& path)
{
	tjs_string s(path.c_str());
	auto pos = s.find_last_of(TJS_W("/\\"));
	if (pos == tjs_string::npos) return ttstr();
	return ttstr(s.substr(0, pos + 1).c_str());
}

} // anonymous

//---------------------------------------------------------------------------
// 内部実装 (PIMPL)
//---------------------------------------------------------------------------
struct tTVPElementsDialogManager::Impl
{
	// Dialog の描画密度。 view は logical、 buffer は kRenderScale 倍の pixel。
	static constexpr float kRenderScale = 2.0f;

	//! @brief 1 つの overlay UI インスタンス。 z-order = instances 内の並び順
	//!        (先頭 = 最背面、 末尾 = 最前面)。
	struct Instance
	{
		std::unique_ptr<elements_modal::overlay_session> session;

		// このインスタンスに紐付く event handler (TJS Dialog 等)。 layer キーと
		// しても使う (renderer のテクスチャ識別)。 ただし複数の異なる layer 用に
		// Instance 自身のアドレスを layer キーにする (handler は共有され得ない
		// 前提だが、 安全のため Instance ポインタで一意化)。
		iTVPDialogEventHandler* handler = nullptr;

		iTVPDrawDevice* host_device = nullptr;

		bool modal = false;           // true: 入力独占 / false: ヒットテスト素通し
		// キーボード/パッドのフォーカスを持つか。 modal は常に true。 非モーダルは
		// grabFocus 指定 (常駐 HUD は false でゲームのホットキーを邪魔しない)。
		bool wants_focus = true;
		bool active = false;
		bool close_requested = false; // 次フレーム PaintOverlay で teardown

		// dialog 論理サイズ (JSON "size" → content フィット後)。
		int dialog_w = 400;
		int dialog_h = 220;

		// マウス座標変換用に直近の DestRect 原点を保持。
		int dest_offset_x = 0;
		int dest_offset_y = 0;

		// 直近 render_to_buffer の描画矩形 (surface logical 座標)。 ヒットテスト用。
		elements_modal::overlay_session::render_rect last_rect{};
		bool has_rect = false;
		bool cursor_inside = false;   // mouse enter/leave 追跡

		// === navigator フロー (複数画面遷移) ===
		std::unique_ptr<elements_modal::navigator> nav;
		std::map<std::string, std::string> screen_jsons;
		ttstr manifest_base;
		std::string flow_lang;

		// renderer のテクスチャ識別キー (Instance ごとに一意)。
		const void* LayerKey() const { return static_cast<const void*>(this); }
	};

	std::vector<std::unique_ptr<Instance>> instances;  // z-order (末尾=最前面)

	// DrawDevice ごとのレンダラ。 オーナーは manager。
	std::map<iTVPDrawDevice*, std::unique_ptr<iTVPDialogRenderer>> renderers;

	// 直近に PaintOverlay を呼んだ (= 現在フレームを提示している) DrawDevice。
	// GL デモ等で drawDevice が OGLDrawDevice に差し替わると、提示中のデバイスも
	// そちらへ移る。 host 未指定 (nullptr) の Show はこのデバイスを既定ホストに
	// 選び、アクティブなデバイス上にパネルが出るようにする (renderers.begin() は
	// map のポインタ順で決まり提示中のデバイスとは限らないため)。
	iTVPDrawDevice* active_device = nullptr;

	// インスタンスが finish したとき保存する結果。 ブロッキングモーダルの
	// pump ループが handler をキーに取り出す。
	struct ResultSnapshot
	{
		ttstr action;
		std::map<ttstr, tTJSVariant> values;
	};
	std::map<iTVPDialogEventHandler*, ResultSnapshot> pending_results;

	// --- helpers ---

	static SDL_Window* GetMainSDLWindow()
	{
		if (!Application) return nullptr;
		auto* form = Application->MainWindowForm();
		if (!form) return nullptr;
		return static_cast<SDL_Window*>(form->NativeWindowHandle());
	}

	iTVPDialogRenderer* FindRenderer(iTVPDrawDevice* dev) const
	{
		auto it = renderers.find(dev);
		return (it != renderers.end()) ? it->second.get() : nullptr;
	}

	bool AnyActive() const
	{
		for (auto const& inst : instances) if (inst->active) return true;
		return false;
	}

	bool AnyModalActive() const
	{
		for (auto const& inst : instances) if (inst->active && inst->modal) return true;
		return false;
	}

	Instance* TopmostActive() const
	{
		for (auto it = instances.rbegin(); it != instances.rend(); ++it) {
			if ((*it)->active) return it->get();
		}
		return nullptr;
	}

	// キーボード/パッドの送り先 = フォーカスを持つ最前面アクティブインスタンス。
	// modal または wants_focus のものだけが候補。 z-order 末尾(最前面)優先なので、
	// 後から開いた focus-grab ダイアログが自然に focus を持ち、 それが閉じると
	// 直前の focus-grab ダイアログへ戻る (スタック不要)。 誰も持たなければ nullptr
	// = キーはゲームへ素通し。
	Instance* TopmostKeyboardFocus() const
	{
		for (auto it = instances.rbegin(); it != instances.rend(); ++it) {
			Instance* inst = it->get();
			if (inst->active && (inst->modal || inst->wants_focus)) return inst;
		}
		return nullptr;
	}

	Instance* FindByHandler(iTVPDialogEventHandler* handler) const
	{
		if (!handler) return nullptr;
		for (auto const& inst : instances) {
			if (inst->handler == handler) return inst.get();
		}
		return nullptr;
	}

	// マウス座標 (image-area 系) → surface logical 座標。
	static float ToSurfaceX(const Instance& inst, tjs_int image_x)
	{
		return static_cast<float>(image_x + inst.dest_offset_x);
	}
	static float ToSurfaceY(const Instance& inst, tjs_int image_y)
	{
		return static_cast<float>(image_y + inst.dest_offset_y);
	}

	// 指定 surface 座標が inst の描画矩形内か。
	static bool RectContains(const Instance& inst, float sx, float sy)
	{
		if (!inst.has_rect) return false;
		const auto& r = inst.last_rect;
		return sx >= r.x && sx < (r.x + r.w) && sy >= r.y && sy < (r.y + r.h);
	}

	// このプラットフォームがオンスクリーンキーボード (Android / iOS 等) を持つか。
	// true の場合、 SDL_StartTextInput はソフトキーボードを画面に出す。 そのため
	// 「ダイアログを開いた瞬間に無条件で開始」ではなく、 テキスト欄に focus が
	// 入ったときだけ開始する focus 駆動に切り替える。 デスクトップ (false) は物理
	// キーボードなので従来どおり開いた時点で開始してよい (ポップアップは出ない)。
	static bool PlatformUsesScreenKeyboard()
	{
		return SDL_HasScreenKeyboardSupport();
	}

	// focus 駆動でソフトキーボードを出している最中か (portable のみ使用)。
	bool ime_focus_active = false;

	// テキスト入力受信の開始/停止 (ウィンドウ単位なので参照カウント的に扱う)。
	void StartTextInputIfNeeded()
	{
		// portable はここでは開始しない。 UpdateFocusDrivenTextInput() が
		// テキスト欄への focus を検出して開始/停止する。
		if (PlatformUsesScreenKeyboard()) return;
		if (auto* w = GetMainSDLWindow()) SDL_StartTextInput(w);
	}
	void StopTextInputIfNoInstances()
	{
		if (instances.empty()) {
			if (auto* w = GetMainSDLWindow()) SDL_StopTextInput(w);
			ime_focus_active = false;
		}
	}

	// portable 用: 最前面フォーカスインスタンスのテキスト欄 focus 状態に追従して
	// ソフトキーボードを出し入れする。 PaintOverlay 末尾から毎フレーム呼ぶ。
	// デスクトップでは no-op (開いた時点で開始済み・ポップアップも無い)。
	void UpdateFocusDrivenTextInput()
	{
		if (!PlatformUsesScreenKeyboard()) return;
		auto* w = GetMainSDLWindow();
		if (!w) return;

		Instance* owner = TopmostKeyboardFocus();
		bool want = owner && owner->active && owner->session &&
		            owner->session->focus_consumes_text();

		if (want && !ime_focus_active) {
			SDL_StartTextInput(w);        // テキスト欄に focus → IME 表示
			ime_focus_active = true;
		} else if (!want && ime_focus_active) {
			SDL_StopTextInput(w);         // focus が外れた / ダイアログ閉じ → IME 非表示
			ime_focus_active = false;
		}
	}

	// === インスタンス生成 / teardown ===

	// 新規 Instance を作って push し、 ポインタを返す (まだ session 無し)。
	Instance* PushInstance(iTVPDialogEventHandler* handler,
	                       iTVPDrawDevice* host, bool modal, bool grabFocus)
	{
		auto inst = std::make_unique<Instance>();
		inst->handler = handler;
		inst->host_device = host;
		inst->modal = modal;
		inst->wants_focus = modal || grabFocus;  // modal は常にフォーカス強制
		Instance* p = inst.get();
		instances.push_back(std::move(inst));
		return p;
	}

	// 指定インスタンスを破棄 (renderer のレイヤも解放)。
	void TeardownInstance(Instance* inst)
	{
		if (!inst) return;
		if (auto* r = FindRenderer(inst->host_device)) {
			r->ReleaseLayer(inst->LayerKey());
		}
		for (auto it = instances.begin(); it != instances.end(); ++it) {
			if (it->get() == inst) {
				instances.erase(it);
				break;
			}
		}
		StopTextInputIfNoInstances();
	}

	// 全インスタンスを即破棄。
	void TeardownAll()
	{
		for (auto& inst : instances) {
			if (auto* r = FindRenderer(inst->host_device)) {
				r->ReleaseLayer(inst->LayerKey());
			}
		}
		instances.clear();
		if (auto* w = GetMainSDLWindow()) SDL_StopTextInput(w);
		ime_focus_active = false;
	}

	// === session / フロー (Instance 単位) ===

	// JSON 文字列から overlay_session を作って fit する。 inst.handler から
	// bridge を生成し、 成功時 session を差し替え dialog_w/h を更新する。
	bool BeginScreen(Instance& inst, const std::string& json_utf8,
	                 const std::string& resource_base_utf8);

	bool LoadScreenJson(Instance& inst, const std::string& name,
	                    std::string& out_json, std::string& out_resource_base);

	bool StartCurrentScreen(Instance& inst);

	// session 完了時にフローを 1 ステップ進める。 終了したら true (teardown 予約済)。
	void AdvanceFlow(Instance& inst);

	// 単発インスタンスの finish 処理 (結果スナップ + teardown 予約)。
	void FinishSingle(Instance& inst);

	// finish した結果を pending_results に保存。
	void SnapshotResult(Instance& inst, const ttstr& action,
	                    const std::map<ttstr, tTJSVariant>& values)
	{
		if (!inst.handler) return;
		ResultSnapshot snap;
		snap.action = action;
		snap.values = values;
		pending_results[inst.handler] = std::move(snap);
	}

	// 1 インスタンス分の描画 (close/finish 処理は呼出側で済ませる)。
	void RenderInstance(Instance& inst, iTVPDrawDevice* device,
	                    iTVPDialogRenderer* renderer);
};

//---------------------------------------------------------------------------
// Impl: フロー / 描画メソッド (out-of-line)
//---------------------------------------------------------------------------
bool tTVPElementsDialogManager::Impl::BeginScreen(
	Instance& inst, const std::string& json_utf8,
	const std::string& resource_base_utf8)
{
	// JSON の top-level "size" を簡易 peek (上限サイズ。 fit で内容に詰める)。
	inst.dialog_w = 400;
	inst.dialog_h = 220;
	{
		auto pos = json_utf8.find("\"size\"");
		if (pos != std::string::npos) {
			auto lb = json_utf8.find('[', pos);
			auto rb = (lb != std::string::npos) ? json_utf8.find(']', lb)
			                                    : std::string::npos;
			if (lb != std::string::npos && rb != std::string::npos) {
				const char* p = json_utf8.c_str() + lb + 1;
				char* endp = nullptr;
				int w = (int)std::strtol(p, &endp, 10);
				int h = (endp && *endp == ',')
				            ? (int)std::strtol(endp + 1, nullptr, 10) : 0;
				if (w > 0 && h > 0) { inst.dialog_w = w; inst.dialog_h = h; }
			}
		}
	}

	auto sess = std::make_unique<elements_modal::overlay_session>();
	auto bridge = MakeBridgeCallback(inst.handler);
	if (!sess->start(json_utf8, inst.dialog_w, inst.dialog_h, kRenderScale,
	                 std::move(bridge), resource_base_utf8)) {
		return false;
	}

	// run_modal と同じく content の自然サイズへフィット (上側空欄対策)。
	{
		int mw = 0, mh = 0;
		if (sess->measure_content(mw, mh)) {
			int fit_w = (mw > 0 && mw < inst.dialog_w) ? mw : inst.dialog_w;
			int fit_h = (mh > 0 && mh < inst.dialog_h) ? mh : inst.dialog_h;
			if (fit_w != inst.dialog_w || fit_h != inst.dialog_h) {
				sess->notify_view_resize(fit_w, fit_h);
				inst.dialog_w = fit_w;
				inst.dialog_h = fit_h;
			}
		}
	}

	inst.session = std::move(sess);
	inst.has_rect = false;

	// テキスト入力受信を開始 (input_box の IME / 物理キー入力用)。
	StartTextInputIfNeeded();
	return true;
}

bool tTVPElementsDialogManager::Impl::LoadScreenJson(
	Instance& inst, const std::string& name,
	std::string& out_json, std::string& out_resource_base)
{
	out_json.clear();
	out_resource_base.clear();

	// インラインモード優先
	if (!inst.screen_jsons.empty()) {
		auto it = inst.screen_jsons.find(name);
		if (it == inst.screen_jsons.end()) {
			TVPAddImportantLog(ttstr(TJS_W("ElementsDialog flow: no inline screen: "))
				+ Utf8ToTtstr(name));
			return false;
		}
		out_json = it->second;
		return true;
	}

	// manifest モード: nav->screen_file(name) を base と結合して読む。
	if (!inst.nav) return false;
	std::string rel = inst.nav->screen_file(name);
	if (rel.empty()) {
		TVPAddImportantLog(ttstr(TJS_W("ElementsDialog flow: screen not in manifest: "))
			+ Utf8ToTtstr(name));
		return false;
	}
	ttstr path = inst.manifest_base + Utf8ToTtstr(rel);
	tjs_uint64 flen = 0;
	auto buf = TVPReadStream(path.c_str(), &flen);
	if (!buf || flen == 0) {
		TVPAddImportantLog(ttstr(TJS_W("ElementsDialog flow: cannot read screen: "))
			+ path);
		return false;
	}
	out_json.assign(reinterpret_cast<const char*>(buf.get()),
	                static_cast<size_t>(flen));
	out_resource_base = TtstrToUtf8(DirOfStoragePath(path));
	return true;
}

bool tTVPElementsDialogManager::Impl::StartCurrentScreen(Instance& inst)
{
	if (!inst.nav) return false;
	const std::string name = inst.nav->current();
	if (name.empty()) return false;

	std::string json, resource_base;
	if (!LoadScreenJson(inst, name, json, resource_base)) return false;
	if (!BeginScreen(inst, json, resource_base)) {
		TVPAddImportantLog(ttstr(TJS_W("ElementsDialog flow: start failed: "))
			+ Utf8ToTtstr(name));
		return false;
	}

	const std::string& fid = inst.nav->focus_to_restore(name);
	if (!fid.empty()) inst.session->focus_by_id(fid);

	if (!inst.flow_lang.empty()) inst.session->set_language(inst.flow_lang);

	inst.active = true;
	if (inst.handler) inst.handler->OnScreenEnter(Utf8ToTtstr(name));
	return true;
}

void tTVPElementsDialogManager::Impl::AdvanceFlow(Instance& inst)
{
	const std::string current = inst.nav ? inst.nav->current() : std::string();
	const auto& r = inst.session->get_result();

	if (inst.nav) {
		inst.nav->remember_focus(current, inst.session->focused_id());
		inst.flow_lang = inst.session->language();
		inst.nav->set_language(inst.flow_lang);
	}

	// 結果スナップ (フロー終了時に呼出側へ返す = 最後に閉じた画面の値)。
	ttstr action = Utf8ToTtstr(r.action);
	std::map<ttstr, tTJSVariant> values;
	for (auto const& kv : r.values) {
		values.emplace(Utf8ToTtstr(kv.first), ValueToVariant(kv.second));
	}

	// leave 通知 (advance より前)。
	if (inst.handler) {
		inst.handler->OnScreenLeave(Utf8ToTtstr(current), action);
	}

	if (inst.nav) inst.nav->advance(r.action, inst.session->transitions());

	inst.session.reset();

	if (!inst.nav || inst.nav->empty()) {
		SnapshotResult(inst, action, values);
		inst.active = false;
		inst.close_requested = true;
		return;
	}

	if (!StartCurrentScreen(inst)) {
		SnapshotResult(inst, action, values);
		inst.active = false;
		inst.close_requested = true;
	}
}

void tTVPElementsDialogManager::Impl::FinishSingle(Instance& inst)
{
	const auto& r = inst.session->get_result();
	ttstr action = Utf8ToTtstr(r.action);
	std::map<ttstr, tTJSVariant> values;
	for (auto const& kv : r.values) {
		values.emplace(Utf8ToTtstr(kv.first), ValueToVariant(kv.second));
	}
	SnapshotResult(inst, action, values);
	inst.active = false;
	inst.close_requested = true;
	TVPAddLog(TJS_W("ElementsDialog: session auto-finished (close_on_click)"));
}

void tTVPElementsDialogManager::Impl::RenderInstance(
	Instance& inst, iTVPDrawDevice* device, iTVPDialogRenderer* renderer)
{
	const float scale = kRenderScale;
	const int w_logical = inst.dialog_w;
	const int h_logical = inst.dialog_h;
	const int w_pixels  = static_cast<int>(w_logical * scale);
	const int h_pixels  = static_cast<int>(h_logical * scale);

	int sw = 0, sh = 0;
	renderer->GetSurfaceSize(sw, sh);
	int dx = 0, dy = 0, dw = 0, dh = 0;
	renderer->GetDestRect(dx, dy, dw, dh);
	if (sw <= 0 || sh <= 0) {
		sw = dx + dw;
		sh = dy + dh;
	}
	inst.dest_offset_x = dx;
	inst.dest_offset_y = dy;

	const void* layer = inst.LayerKey();
	uint32_t* buf = renderer->AcquireBuffer(layer, w_pixels, h_pixels);
	if (!buf) return;

	elements_modal::overlay_session::render_rect rect{};
	bool ok = inst.session->render_to_buffer(buf, w_pixels, h_pixels,
	                                         sw, sh, rect);
	renderer->ReleaseBuffer(layer);
	if (!ok) return;

	inst.last_rect = rect;
	inst.has_rect = true;

	renderer->PresentOverlay(layer, rect.x, rect.y, w_logical, h_logical);
}

//---------------------------------------------------------------------------
// シングルトン
//---------------------------------------------------------------------------
tTVPElementsDialogManager& tTVPElementsDialogManager::Instance()
{
	static tTVPElementsDialogManager instance;
	return instance;
}

tTVPElementsDialogManager::tTVPElementsDialogManager()
	: _impl(std::make_unique<Impl>())
{
}

tTVPElementsDialogManager::~tTVPElementsDialogManager() = default;

bool tTVPElementsDialogManager::IsModalActive() const
{
	return _impl->AnyActive();
}

bool tTVPElementsDialogManager::HasModalInstance() const
{
	return _impl->AnyModalActive();
}

bool tTVPElementsDialogManager::IsHandlerActive(iTVPDialogEventHandler* handler) const
{
	Impl::Instance* inst = _impl->FindByHandler(handler);
	return inst && inst->active;
}

void tTVPElementsDialogManager::EnsureRuntimeInitialized()
{
	// ELEMENTS_FILE_IO_SUPPORT=OFF でビルドしているため elements 側 default の
	// null_resource_loader が選ばれている。 最初に Storages-backed loader を
	// install する (idempotent)。 順序は install → ThorVG init → font register。
	TVPInstallElementsResourceLoader();

	elements_modal::init("", /*load_default_fonts=*/false);

	static bool s_fonts_loaded = false;
	if (!s_fonts_loaded && Application) {
		TVPRegisterElementsFontsFromStorageDir(ttstr(Application->ResourcePath().c_str()));
		TVPApplyRegisteredFontsToElementsTheme();
		s_fonts_loaded = true;
	}
}

//---------------------------------------------------------------------------
// host device 解決 (未指定なら登録済みの先頭)。 renderer 必須。
//---------------------------------------------------------------------------
iTVPDrawDevice* tTVPElementsDialogManager::ResolveHostDeviceForFlow(
	iTVPDrawDevice* requested)
{
	iTVPDrawDevice* host = requested;
	if (!host) {
		if (_impl->renderers.empty()) {
			TVPAddImportantLog(TJS_W("ElementsDialog: no DrawDevice registered"));
			return nullptr;
		}
		// 提示中のデバイス (直近 PaintOverlay 呼出元) を優先する。 これにより
		// GL デモ等で drawDevice が OGLDrawDevice に切り替わっていても、その上に
		// パネルが出る。 未確定 / レンダラ無しのときのみ map 先頭へフォールバック。
		if (_impl->active_device && _impl->FindRenderer(_impl->active_device)) {
			host = _impl->active_device;
		} else {
			host = _impl->renderers.begin()->first;
		}
	}
	if (!_impl->FindRenderer(host)) {
		TVPAddImportantLog(TJS_W("ElementsDialog: no renderer for given DrawDevice"));
		return nullptr;
	}
	return host;
}

//---------------------------------------------------------------------------
// JSON 経由の Show (公開)
//---------------------------------------------------------------------------
bool tTVPElementsDialogManager::ShowFromJsonString(
	const std::string& json,
	iTVPDialogEventHandler* handler,
	iTVPDrawDevice* hostDevice,
	bool modal,
	bool grabFocus)
{
	// JSON パース中に basic_input_box::ctor 等が font metrics を取りに行く
	// (feedback_elements_font_init_order)。 element 生成より前に font load を
	// 完了させておく。
	EnsureRuntimeInitialized();

	// 同一 handler が既にアクティブなら拒否 (1 Dialog = 1 インスタンス)。
	if (_impl->FindByHandler(handler) &&
	    _impl->FindByHandler(handler)->active) {
		TVPAddImportantLog(TJS_W("ElementsDialog: handler already active"));
		return false;
	}

	iTVPDrawDevice* host = ResolveHostDeviceForFlow(hostDevice);
	if (!host) return false;

	Impl::Instance* inst = _impl->PushInstance(handler, host, modal, grabFocus);
	if (!_impl->BeginScreen(*inst, json, std::string())) {
		_impl->TeardownInstance(inst);
		return false;
	}
	inst->active = true;

	TVPAddLog(TJS_W("ElementsDialog: shown (overlay_session)"));
	return true;
}

bool tTVPElementsDialogManager::ShowFromJsonFile(
	const ttstr& path,
	iTVPDialogEventHandler* handler,
	iTVPDrawDevice* hostDevice,
	bool modal,
	bool grabFocus)
{
	tjs_uint64 flen = 0;
	auto buf = TVPReadStream(path.c_str(), &flen);
	if (!buf || flen == 0) {
		TVPAddImportantLog(ttstr(TJS_W("ElementsDialog: cannot read: ")) + path);
		return false;
	}
	std::string json(reinterpret_cast<const char*>(buf.get()),
	                 static_cast<size_t>(flen));
	return ShowFromJsonString(json, handler, hostDevice, modal, grabFocus);
}

//---------------------------------------------------------------------------
// navigator フロー (複数画面遷移)
//---------------------------------------------------------------------------
bool tTVPElementsDialogManager::StartFlowFromManifest(
	const ttstr& manifestPath,
	iTVPDialogEventHandler* handler,
	iTVPDrawDevice* hostDevice,
	bool modal,
	bool grabFocus)
{
	EnsureRuntimeInitialized();
	if (_impl->FindByHandler(handler) &&
	    _impl->FindByHandler(handler)->active) {
		TVPAddImportantLog(TJS_W("ElementsDialog flow: handler already active"));
		return false;
	}

	iTVPDrawDevice* host = ResolveHostDeviceForFlow(hostDevice);
	if (!host) return false;

	tjs_uint64 flen = 0;
	auto buf = TVPReadStream(manifestPath.c_str(), &flen);
	if (!buf || flen == 0) {
		TVPAddImportantLog(ttstr(TJS_W("ElementsDialog flow: cannot read manifest: "))
			+ manifestPath);
		return false;
	}
	std::string manifest_json(reinterpret_cast<const char*>(buf.get()),
	                          static_cast<size_t>(flen));
	elements_modal::app_manifest manifest =
		elements_modal::parse_app_manifest(manifest_json);
	if (!manifest.ok || manifest.entry.empty()) {
		TVPAddImportantLog(ttstr(TJS_W("ElementsDialog flow: invalid manifest: "))
			+ manifestPath);
		return false;
	}

	Impl::Instance* inst = _impl->PushInstance(handler, host, modal, grabFocus);
	inst->nav = std::make_unique<elements_modal::navigator>(std::move(manifest));
	inst->manifest_base = DirOfStoragePath(manifestPath);

	inst->nav->reset_to();   // manifest.entry を起点に
	if (!_impl->StartCurrentScreen(*inst)) {
		_impl->TeardownInstance(inst);
		return false;
	}
	TVPAddLog(TJS_W("ElementsDialog flow: started (manifest)"));
	return true;
}

bool tTVPElementsDialogManager::StartFlowFromScreens(
	const std::map<std::string, std::string>& screens,
	const std::string& entry,
	iTVPDialogEventHandler* handler,
	iTVPDrawDevice* hostDevice,
	bool modal,
	bool grabFocus)
{
	EnsureRuntimeInitialized();
	if (_impl->FindByHandler(handler) &&
	    _impl->FindByHandler(handler)->active) {
		TVPAddImportantLog(TJS_W("ElementsDialog flow: handler already active"));
		return false;
	}
	if (screens.empty() || entry.empty() || screens.count(entry) == 0) {
		TVPAddImportantLog(TJS_W("ElementsDialog flow: empty screens or bad entry"));
		return false;
	}

	iTVPDrawDevice* host = ResolveHostDeviceForFlow(hostDevice);
	if (!host) return false;

	Impl::Instance* inst = _impl->PushInstance(handler, host, modal, grabFocus);
	inst->screen_jsons = screens;
	inst->nav = std::make_unique<elements_modal::navigator>();  // manifest なし

	inst->nav->reset_to(entry);
	if (!_impl->StartCurrentScreen(*inst)) {
		_impl->TeardownInstance(inst);
		return false;
	}
	TVPAddLog(TJS_W("ElementsDialog flow: started (inline)"));
	return true;
}

//---------------------------------------------------------------------------
// テストダイアログ
//---------------------------------------------------------------------------
namespace {
class TestDialogHandler : public iTVPDialogEventHandler
{
public:
	void OnAction(const ttstr& id, const tTJSVariant& /*payload*/) override
	{
		TVPAddLog(ttstr(TJS_W("ElementsDialog: action: ")) + id);
		if (id == ttstr(TJS_W("ok")) || id == ttstr(TJS_W("close"))) {
			tTVPElementsDialogManager::Instance().Close(this);
		}
	}
};
} // anonymous

void tTVPElementsDialogManager::ShowTestDialog(iTVPDrawDevice* hostDevice)
{
	static TestDialogHandler handler;
	static const char kTestJson[] = R"json({
		"size": [400, 220],
		"background": [40, 40, 50, 230],
		"content": {
			"type": "margin", "padding": 20,
			"child": {
				"type": "vtile",
				"children": [
					{ "type": "align_center",
					  "child": { "type": "label", "text": "Hello from Elements!" } },
					{ "type": "align_center",
					  "child": { "type": "hsize", "width": 120,
					             "child": { "type": "button", "id": "ok", "text": "OK" } } }
				]
			}
		}
	})json";
	ShowFromJsonString(kTestJson, &handler, hostDevice);
}

//---------------------------------------------------------------------------
// Close / ForceClose
//---------------------------------------------------------------------------
void tTVPElementsDialogManager::Close()
{
	Impl::Instance* top = _impl->TopmostActive();
	if (!top) return;
	top->active = false;
	top->close_requested = true;   // 次フレーム teardown (再入安全)
	TVPAddLog(TJS_W("ElementsDialog: closed (topmost)"));
}

void tTVPElementsDialogManager::Close(iTVPDialogEventHandler* handler)
{
	Impl::Instance* inst = _impl->FindByHandler(handler);
	if (!inst || !inst->active) return;
	inst->active = false;
	inst->close_requested = true;
	TVPAddLog(TJS_W("ElementsDialog: closed (by handler)"));
}

iTVPDialogEventHandler* tTVPElementsDialogManager::ActiveHandler() const
{
	Impl::Instance* top = _impl->TopmostActive();
	return top ? top->handler : nullptr;
}

std::vector<tTVPElementsDialogManager::InstanceInfo>
tTVPElementsDialogManager::DescribeInstances() const
{
	std::vector<InstanceInfo> out;
	out.reserve(_impl->instances.size());
	for (auto const& up : _impl->instances) {
		Impl::Instance* inst = up.get();
		InstanceInfo info;
		info.modal  = inst->modal;
		info.active = inst->active;
		if (inst->nav) info.screen = Utf8ToTtstr(inst->nav->current());
		if (inst->session) info.focused = Utf8ToTtstr(inst->session->focused_id());
		if (inst->has_rect) {
			info.x = inst->last_rect.x;
			info.y = inst->last_rect.y;
			info.w = inst->last_rect.w;
			info.h = inst->last_rect.h;
		}
		out.push_back(std::move(info));
	}
	return out;
}

bool tTVPElementsDialogManager::FocusWidgetById(int index, const ttstr& id)
{
	if (index < 0 || index >= (int)_impl->instances.size()) return false;
	Impl::Instance* inst = _impl->instances[index].get();
	if (!inst->active || !inst->session) return false;
	inst->session->focus_by_id(TtstrToUtf8(id));
	return true;
}

bool tTVPElementsDialogManager::ActivateWidgetById(int index, const ttstr& id)
{
	if (index < 0 || index >= (int)_impl->instances.size()) return false;
	Impl::Instance* inst = _impl->instances[index].get();
	if (!inst->active || !inst->session) return false;
	// activate_by_id は focus を即時適用 (view->poll) してから Enter を送るので、
	// focus_by_id + on_key_down (focus が遅延タスクで間に合わない) より確実。
	return inst->session->activate_by_id(TtstrToUtf8(id));
}

std::vector<tTVPElementsDialogManager::WidgetInfo>
tTVPElementsDialogManager::DescribeWidgets(int index) const
{
	std::vector<WidgetInfo> out;
	if (index < 0 || index >= (int)_impl->instances.size()) return out;
	Impl::Instance* inst = _impl->instances[index].get();
	if (!inst->session) return out;

	auto widgets = inst->session->list_widgets();
	const auto& res = inst->session->get_result();  // 現在値 (state widget)
	out.reserve(widgets.size());
	for (auto const& w : widgets) {
		WidgetInfo info;
		info.id = Utf8ToTtstr(w.id);
		info.type = Utf8ToTtstr(w.type);
		auto vit = res.values.find(w.id);
		if (vit != res.values.end()) {
			info.value = ValueToVariant(vit->second);
			info.has_value = true;
		}
		out.push_back(std::move(info));
	}
	return out;
}

bool tTVPElementsDialogManager::HasLastModalResult() const
{
	return !_impl->pending_results.empty();
}

bool tTVPElementsDialogManager::TakeLastModalResult(
	iTVPDialogEventHandler* handler,
	ttstr& out_action, std::map<ttstr, tTJSVariant>& out_values)
{
	auto it = _impl->pending_results.find(handler);
	if (it == _impl->pending_results.end()) return false;
	out_action = std::move(it->second.action);
	out_values = std::move(it->second.values);
	_impl->pending_results.erase(it);
	return true;
}

void tTVPElementsDialogManager::ForceClose()
{
	if (_impl->instances.empty()) return;
	_impl->TeardownAll();
	TVPAddLog(TJS_W("ElementsDialog: force-closed (all)"));
}

//---------------------------------------------------------------------------
// Renderer 登録
//---------------------------------------------------------------------------
void tTVPElementsDialogManager::RegisterRenderer(
	iTVPDrawDevice* device,
	std::unique_ptr<iTVPDialogRenderer> renderer)
{
	if (!device || !renderer) return;
	_impl->renderers[device] = std::move(renderer);
}

void tTVPElementsDialogManager::UnregisterRenderer(iTVPDrawDevice* device)
{
	// この device をホストとするインスタンスは renderer が消える前に teardown。
	// (renderer 破棄前に ReleaseLayer 相当を済ませる)
	std::vector<Impl::Instance*> doomed;
	for (auto& inst : _impl->instances) {
		if (inst->host_device == device) doomed.push_back(inst.get());
	}
	for (auto* inst : doomed) _impl->TeardownInstance(inst);

	_impl->renderers.erase(device);

	// 提示中デバイスが外れたら既定ホストの記録もクリア (次の PaintOverlay で
	// 現行デバイスへ更新される)。 GL 離脱時の OGLDrawDevice 破棄などで発生。
	if (_impl->active_device == device) _impl->active_device = nullptr;
}

//---------------------------------------------------------------------------
// PaintOverlay (DrawDevice::Show() 終端から)
//---------------------------------------------------------------------------
void tTVPElementsDialogManager::PaintOverlay(iTVPDrawDevice* device)
{
	// 提示デバイスが切り替わったら (GL デモの drawDevice 差し替え等)、既存の
	// パネルを現在提示中のデバイスへ移設する。 パネルは create() 内で GL 有効化
	// 直後に表示されることがあり、その時点では旧デバイス (menu を描いた SDL 等)
	// がまだ提示中なので旧デバイスにホストされてしまう。 提示デバイスが実際に
	// 切り替わったこのタイミングで host を追従させ、旧レンダラのレイヤは解放する。
	// element ツリーは ThorVG の CPU ラスタ出力を各レンダラへアップロードする
	// 方式でデバイス非依存なので、host 付け替え + 旧レイヤ解放だけで移設できる。
	if (_impl->active_device != device) {
		for (auto& up : _impl->instances) {
			Impl::Instance* inst = up.get();
			if (inst->host_device == device) continue;
			if (auto* oldR = _impl->FindRenderer(inst->host_device))
				oldR->ReleaseLayer(inst->LayerKey());
			inst->host_device = device;
		}
		_impl->active_device = device;
	}

	// 1) close 予約 / 自動 finish の処理 (この device のインスタンスのみ)。
	//    teardown でリスト要素が消えるので、 ポインタを集めてから処理する。
	{
		std::vector<Impl::Instance*> to_teardown;
		for (auto& up : _impl->instances) {
			Impl::Instance* inst = up.get();
			if (inst->host_device != device) continue;

			if (inst->close_requested) {
				inst->close_requested = false;
				to_teardown.push_back(inst);
				continue;
			}
			if (!inst->active || !inst->session) continue;

			// "close_on_click" / Esc 等で session が自動 finish したとき。
			if (inst->session->finished()) {
				if (inst->nav) {
					_impl->AdvanceFlow(*inst);
				} else {
					_impl->FinishSingle(*inst);
				}
				// AdvanceFlow / FinishSingle が close_requested を立てた場合は
				// 次フレームで teardown される (再入安全のため今は破棄しない)。
			}
		}
		for (auto* inst : to_teardown) _impl->TeardownInstance(inst);
	}

	// 2) 描画 (z-order 奥→手前 = instances 先頭→末尾)。
	iTVPDialogRenderer* renderer = _impl->FindRenderer(device);
	if (!renderer) return;
	for (auto& up : _impl->instances) {
		Impl::Instance* inst = up.get();
		if (inst->host_device != device) continue;
		if (!inst->active || !inst->session) continue;
		_impl->RenderInstance(*inst, device, renderer);
	}

	// 3) portable: テキスト欄への focus 状態に追従してソフトキーボードを出し入れ。
	_impl->UpdateFocusDrivenTextInput();
}

//---------------------------------------------------------------------------
// 入力フォワード
//---------------------------------------------------------------------------
namespace {

int MouseButtonToSDL(tTVPMouseButton mb)
{
	switch (mb) {
		case mbLeft:   return SDL_BUTTON_LEFT;
		case mbMiddle: return SDL_BUTTON_MIDDLE;
		case mbRight:  return SDL_BUTTON_RIGHT;
		default:       return SDL_BUTTON_LEFT;
	}
}

int FlagsToSDLMods(tjs_uint32 flags)
{
	int mods = 0;
	if (flags & TVP_SS_SHIFT) mods |= SDL_KMOD_SHIFT;
	if (flags & TVP_SS_CTRL)  mods |= SDL_KMOD_CTRL;
	if (flags & TVP_SS_ALT)   mods |= SDL_KMOD_ALT;
	return mods;
}

//---------------------------------------------------------------------------
// Windows VK code → (SDL_Keycode | SDL_GAMEPAD_BUTTON) 振り分け。
//---------------------------------------------------------------------------
struct vk_routing {
	enum class kind { none, key, pad_button };
	kind k         = kind::none;
	int  sdl_key   = 0;
	int  extra_mods = 0;
	int  sdl_pad   = 0;
};

vk_routing RouteVk(tjs_uint vk)
{
	using K = vk_routing::kind;
	auto key  = [](int sdl, int m = 0) { return vk_routing{K::key, sdl, m, 0}; };
	auto pad  = [](int gp) { return vk_routing{K::pad_button, 0, 0, gp}; };

	switch (vk) {
		case VK_RETURN: return key(SDLK_RETURN);
		case VK_TAB:    return key(SDLK_TAB);
		case VK_ESCAPE: return key(SDLK_ESCAPE);
		case VK_BACK:   return key(SDLK_BACKSPACE);
		case VK_DELETE: return key(SDLK_DELETE);
		case VK_INSERT: return key(SDLK_INSERT);
		case VK_HOME:   return key(SDLK_HOME);
		case VK_END:    return key(SDLK_END);
		case VK_PRIOR:  return key(SDLK_PAGEUP);
		case VK_NEXT:   return key(SDLK_PAGEDOWN);
		case VK_SPACE:  return key(SDLK_SPACE);
		case VK_LEFT:   return key(SDLK_LEFT);
		case VK_UP:     return key(SDLK_UP);
		case VK_RIGHT:  return key(SDLK_RIGHT);
		case VK_DOWN:   return key(SDLK_DOWN);

		case 0x1C0: return pad(SDL_GAMEPAD_BUTTON_SOUTH);          // VK_PAD1  (A)
		case 0x1C1: return pad(SDL_GAMEPAD_BUTTON_EAST);           // VK_PAD2  (B)
		case 0x1C2: return pad(SDL_GAMEPAD_BUTTON_WEST);           // VK_PAD3  (X)
		case 0x1C3: return pad(SDL_GAMEPAD_BUTTON_NORTH);          // VK_PAD4  (Y)
		case 0x1C4: return pad(SDL_GAMEPAD_BUTTON_LEFT_SHOULDER);  // VK_PAD5  (LB)
		case 0x1C5: return pad(SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER); // VK_PAD6  (RB)
		case 0x1C8: return pad(SDL_GAMEPAD_BUTTON_BACK);           // VK_PAD9  (Back)
		case 0x1C9: return pad(SDL_GAMEPAD_BUTTON_START);          // VK_PAD10 (Start)
		case 0x1CA: return pad(SDL_GAMEPAD_BUTTON_LEFT_STICK);     // VK_PAD11 (L3)
		case 0x1CB: return pad(SDL_GAMEPAD_BUTTON_RIGHT_STICK);    // VK_PAD12 (R3)

		case 0x1B5: case 0x1CC: case 0x1D0:
			return pad(SDL_GAMEPAD_BUTTON_DPAD_LEFT);
		case 0x1B6: case 0x1CD: case 0x1D1:
			return pad(SDL_GAMEPAD_BUTTON_DPAD_UP);
		case 0x1B7: case 0x1CE: case 0x1D2:
			return pad(SDL_GAMEPAD_BUTTON_DPAD_RIGHT);
		case 0x1B8: case 0x1CF: case 0x1D3:
			return pad(SDL_GAMEPAD_BUTTON_DPAD_DOWN);

		case 0x1B9: return pad(SDL_GAMEPAD_BUTTON_SOUTH);

		default:
			if (vk >= '0' && vk <= '9') return key(static_cast<int>(vk));
			if (vk >= 'A' && vk <= 'Z') return key(static_cast<int>(vk - 'A' + 'a'));
			return vk_routing{};
	}
}

} // anonymous

//---------------------------------------------------------------------------
// 入力ルーティングの中核。
//
// マウス系: 最前面から順に走査し、 modal に当たればそこで消費 (下へ通さない)、
//   非モーダルは描画矩形ヒット時だけ消費。 どれにも当たらなければ非消費
//   (= ゲームへ素通し)。 戻り値はヒットした Instance (なければ nullptr)。
// キー / パッド / テキスト: 最前面アクティブインスタンスへ無条件で送って消費。
//---------------------------------------------------------------------------
bool tTVPElementsDialogManager::ForwardMouseDown(
	tjs_int x, tjs_int y, tTVPMouseButton mb, tjs_uint32 flags)
{
	for (auto it = _impl->instances.rbegin(); it != _impl->instances.rend(); ++it) {
		Impl::Instance* inst = it->get();
		if (!inst->active || !inst->session) continue;
		float sx = Impl::ToSurfaceX(*inst, x);
		float sy = Impl::ToSurfaceY(*inst, y);
		if (inst->modal || Impl::RectContains(*inst, sx, sy)) {
			inst->session->on_mouse_down(sx, sy,
				MouseButtonToSDL(mb), FlagsToSDLMods(flags));
			return true;
		}
	}
	return false;
}

bool tTVPElementsDialogManager::ForwardMouseUp(
	tjs_int x, tjs_int y, tTVPMouseButton mb, tjs_uint32 flags)
{
	// mouse up はドラッグ継続中のインスタンスにも届けたいので、 down と同様に
	// 最前面ヒット (または modal) へ送る。 釦が押されたインスタンスを覚えるより
	// 単純なヒットテストで十分 (overlay_session 内部で capture 管理される)。
	for (auto it = _impl->instances.rbegin(); it != _impl->instances.rend(); ++it) {
		Impl::Instance* inst = it->get();
		if (!inst->active || !inst->session) continue;
		float sx = Impl::ToSurfaceX(*inst, x);
		float sy = Impl::ToSurfaceY(*inst, y);
		if (inst->modal || Impl::RectContains(*inst, sx, sy)) {
			inst->session->on_mouse_up(sx, sy,
				MouseButtonToSDL(mb), FlagsToSDLMods(flags));
			return true;
		}
	}
	return false;
}

bool tTVPElementsDialogManager::ForwardMouseMove(
	tjs_int x, tjs_int y, tjs_uint32 flags)
{
	bool consumed = false;
	Impl::Instance* hit = nullptr;
	// 最前面から: modal なら独占、 非モーダルはヒット判定。
	for (auto it = _impl->instances.rbegin(); it != _impl->instances.rend(); ++it) {
		Impl::Instance* inst = it->get();
		if (!inst->active || !inst->session) continue;
		float sx = Impl::ToSurfaceX(*inst, x);
		float sy = Impl::ToSurfaceY(*inst, y);
		if (!hit && (inst->modal || Impl::RectContains(*inst, sx, sy))) {
			inst->session->on_mouse_move(sx, sy, FlagsToSDLMods(flags));
			inst->cursor_inside = true;
			hit = inst;
			consumed = true;
			if (inst->modal) {
				// modal 配下のインスタンスにはホバーを送らない (leave 通知だけ)
			}
		} else if (inst->cursor_inside) {
			// 以前カーソルがあったが今は外れたインスタンスへ leave。
			inst->session->on_mouse_leave();
			inst->cursor_inside = false;
		}
	}
	return consumed;
}

bool tTVPElementsDialogManager::ForwardMouseWheel(
	tjs_uint32 /*shift*/, tjs_int delta, tjs_int x, tjs_int y)
{
	float dy = static_cast<float>(delta) / 120.0f;
	for (auto it = _impl->instances.rbegin(); it != _impl->instances.rend(); ++it) {
		Impl::Instance* inst = it->get();
		if (!inst->active || !inst->session) continue;
		float sx = Impl::ToSurfaceX(*inst, x);
		float sy = Impl::ToSurfaceY(*inst, y);
		if (inst->modal || Impl::RectContains(*inst, sx, sy)) {
			inst->session->on_mouse_wheel(0.0f, dy, sx, sy);
			return true;
		}
	}
	return false;
}

bool tTVPElementsDialogManager::ForwardClick(tjs_int /*x*/, tjs_int /*y*/) { return false; }
bool tTVPElementsDialogManager::ForwardDoubleClick(tjs_int /*x*/, tjs_int /*y*/) { return false; }
bool tTVPElementsDialogManager::ForwardReleaseCapture() { return false; }

bool tTVPElementsDialogManager::ForwardMouseOutOfWindow()
{
	bool any = false;
	for (auto& up : _impl->instances) {
		Impl::Instance* inst = up.get();
		if (inst->active && inst->session && inst->cursor_inside) {
			inst->session->on_mouse_leave();
			inst->cursor_inside = false;
			any = true;
		}
	}
	// ウィンドウから出ただけなので消費はしない (素通し)。
	(void)any;
	return false;
}

// キー / パッド / テキストは「キーボードフォーカスを持つ」インスタンスへ送る
// (最前面とは別概念)。 フォーカスが無ければ非消費 = ゲームへ素通し。
// 消費判定: modal は無条件消費 (下にもゲームにも通さない)。 非モーダルは
// overlay_session が実際に処理したキーだけ消費し、 未処理キー (ゲームのホット
// キー等) はゲームへ通す (handled pass-through)。
bool tTVPElementsDialogManager::ForwardKeyDown(tjs_uint key, tjs_uint32 shift)
{
	Impl::Instance* f = _impl->TopmostKeyboardFocus();
	if (!f || !f->session) return false;
	auto r = RouteVk(key);
	bool handled = false;
	switch (r.k) {
		case vk_routing::kind::key: {
			int mods = FlagsToSDLMods(shift) | r.extra_mods;
			handled = f->session->on_key_down(r.sdl_key, mods);
			break;
		}
		case vk_routing::kind::pad_button:
			handled = f->session->on_pad_button(r.sdl_pad, /*down=*/true);
			break;
		case vk_routing::kind::none:
			handled = false;
			break;
	}
	// modal は未処理でも消費 (ゲーム/下に漏らさない)。 非モーダルは handled のみ。
	return f->modal ? true : handled;
}

bool tTVPElementsDialogManager::ForwardKeyUp(tjs_uint key, tjs_uint32 shift)
{
	Impl::Instance* f = _impl->TopmostKeyboardFocus();
	if (!f || !f->session) return false;
	auto r = RouteVk(key);
	bool handled = false;
	switch (r.k) {
		case vk_routing::kind::key: {
			int mods = FlagsToSDLMods(shift) | r.extra_mods;
			handled = f->session->on_key_up(r.sdl_key, mods);
			break;
		}
		case vk_routing::kind::pad_button:
			handled = f->session->on_pad_button(r.sdl_pad, /*down=*/false);
			break;
		case vk_routing::kind::none:
			handled = false;
			break;
	}
	return f->modal ? true : handled;
}

bool tTVPElementsDialogManager::ForwardKeyPress(tjs_char key)
{
	Impl::Instance* f = _impl->TopmostKeyboardFocus();
	if (!f || !f->session) return false;
	char buf[8] = {0};
	tjs_uint32 cp = static_cast<tjs_uint32>(key);
	if (cp < 0x80) {
		buf[0] = (char)cp;
	} else if (cp < 0x800) {
		buf[0] = (char)(0xC0 | (cp >> 6));
		buf[1] = (char)(0x80 | (cp & 0x3F));
	} else {
		buf[0] = (char)(0xE0 | (cp >> 12));
		buf[1] = (char)(0x80 | ((cp >> 6) & 0x3F));
		buf[2] = (char)(0x80 | (cp & 0x3F));
	}
	f->session->on_text_input(buf);
	return true;
}

bool tTVPElementsDialogManager::ForwardText(const char* utf8_text)
{
	// テキスト入力 (IME / 文字) はフォーカス中インスタンスの input_box 向け。
	// フォーカスが無ければ素通し。 あれば消費 (文字入力は意図的操作)。
	Impl::Instance* f = _impl->TopmostKeyboardFocus();
	if (!f || !f->session || !utf8_text) return false;
	f->session->on_text_input(utf8_text);
	return true;
}

// タッチ系は MVP では noop
bool tTVPElementsDialogManager::ForwardTouchDown(tjs_real, tjs_real, tjs_real, tjs_real, tjs_uint32) { return false; }
bool tTVPElementsDialogManager::ForwardTouchUp(tjs_real, tjs_real, tjs_real, tjs_real, tjs_uint32) { return false; }
bool tTVPElementsDialogManager::ForwardTouchMove(tjs_real, tjs_real, tjs_real, tjs_real, tjs_uint32) { return false; }

void tTVPElementsDialogManager::ShowTestDialog()
{
	if (_impl->renderers.empty()) {
		TVPAddImportantLog(TJS_W("ElementsDialog: no registered DrawDevice; cannot show test dialog"));
		return;
	}
	ShowTestDialog(_impl->renderers.begin()->first);
}

//---------------------------------------------------------------------------
// デバッグ用ヘルパ (F12 キー等から呼ぶ)
//---------------------------------------------------------------------------
void TVPShowElementsTestDialog()
{
	auto& mgr = tTVPElementsDialogManager::Instance();
	if (mgr.IsModalActive()) {
		mgr.Close();
	} else {
		mgr.ShowTestDialog();
	}
}
