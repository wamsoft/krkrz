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
#include "Application.h"    // Application, MainWindowForm() / ResourcePath()
#include "WindowIntf.h"     // iTVPWindow (cursor-warp: SetCursorPos) / mcs enum
#include "WindowImpl.h"     // tTJSNI_Window::SetMouseCursorState (cursor-warp hide)
#ifndef __WINVER__
#include "WindowForm.h"     // TTVPWindowForm::NativeWindowHandle() (SDL/generic host)
#endif
#include "StoragesResourceLoader.h"   // TVPInstallElementsResourceLoader / Fonts
#include "GraphicsLoaderIntf.h"       // TVPLoadGraphic (universal rule 画像)
#include "LayerBitmapIntf.h"          // tTVPBaseBitmap (rule 画像の 8bpp 展開)
#include "TickCount.h"                // TVPGetRoughTickCount32 (遷移エフェクト計時)

#ifndef _WIN32
#include "VirtualKey.h"
#endif

#include <elements/element/pad_icon.hpp>   // set_pad_theme / parse_pad_theme
#include <elements_modal/modal.h>
#include <elements_modal/navigator.h>   // フロー駆動 (画面遷移スタック)
#include <elements_modal/effects.h>     // 画面切替エフェクト (fade / universal ブレンド)

#include <chrono>   // renderStats 区間計測 (steady_clock)
#include <elements/base_view.hpp>        // cycfi 中立入力型 (mouse_button / key_code / mod_*)
#include <elements/element/gamepad.hpp>  // cycfi 中立入力型 (pad_button)

// テキスト入力の開始/停止 (ソフトキーボード制御) は host 依存。 WINVER は Win32 の
// WM_CHAR 経由 (ForwardText) で扱うため SDL は不要。 SDL host のみ SDL3 を引く。
#ifndef __WINVER__
#include <SDL3/SDL.h>        // SDL host: SDL_StartTextInput / SDL_HasScreenKeyboardSupport 等
#endif

#include <algorithm>         // std::remove_if (ホストホットキー解除)
#include <cstdlib>           // std::strtol
#include <cstring>           // std::memcpy (部分更新時の last_frame 複製)
#include <map>
#include <memory>
#include <string>
#include <type_traits>
#include <vector>

//---------------------------------------------------------------------------
// 内部: krkrz ttstr ⇔ utf-8、 value_t → tTJSVariant 変換 + handler ブリッジ
//---------------------------------------------------------------------------
namespace {

// renderStats 用: t0 からの経過 microsecond
tjs_uint64 ElapsedUs(std::chrono::steady_clock::time_point t0)
{
	return static_cast<tjs_uint64>(
		std::chrono::duration_cast<std::chrono::microseconds>(
			std::chrono::steady_clock::now() - t0).count());
}

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

//---------------------------------------------------------------------------
// host 依存のテキスト入力制御 (ソフトキーボード / IME イベント有効化)。
//
//  - SDL host: SDL_StartTextInput / SDL_StopTextInput でウィンドウ単位に text
//    入力を制御し、 Android/iOS では SDL_HasScreenKeyboardSupport() が true =
//    オンスクリーンキーボードが出る。
//  - WINVER host: テキストは Win32 の WM_CHAR → ForwardText で常時届くため明示的な
//    開始/停止は不要。 デスクトップなのでソフトキーボードも無い。 全て no-op。
//---------------------------------------------------------------------------
#ifdef __WINVER__

inline bool HostHasScreenKeyboard() { return false; }
inline void HostStartTextInput() {}
inline void HostStopTextInput()  {}

#else

inline bool HostHasScreenKeyboard() { return SDL_HasScreenKeyboardSupport(); }

inline SDL_Window* HostMainWindow()
{
	if (!Application) return nullptr;
	auto* form = Application->MainWindowForm();
	if (!form) return nullptr;
	return static_cast<SDL_Window*>(form->NativeWindowHandle());
}
inline void HostStartTextInput() { if (auto* w = HostMainWindow()) SDL_StartTextInput(w); }
inline void HostStopTextInput()  { if (auto* w = HostMainWindow()) SDL_StopTextInput(w); }

#endif

//---------------------------------------------------------------------------
// 物理 (ハードウェア) キーボードが接続されているか。
//
//  接続されていれば OS のソフトキーボードも内蔵仮想キーボードも不要で、
//  そのままキー / 文字イベントが届く。 判定は SDL_HasKeyboard() で、 各
//  プラットフォームの video ドライバが SDL_AddKeyboard/SDL_RemoveKeyboard を
//  呼んでいることが前提 (NX / PS5 は対応済み)。
//---------------------------------------------------------------------------
#ifdef __WINVER__

inline bool HostHasPhysicalKeyboard() { return true; }
inline const char* HostGetEnv(const char* name) { return getenv(name); }

#else

inline bool HostHasPhysicalKeyboard() { return SDL_HasKeyboard(); }
inline const char* HostGetEnv(const char* name) { return SDL_getenv(name); }

#endif

//! デスクトップでも内蔵仮想キーボードを出す (動作確認用)。
//!   KRKRZ_FORCE_VIRTUAL_KEYBOARD=1
inline bool ForceVirtualKeyboard()
{
	static const bool forced = [] {
		const char* v = HostGetEnv("KRKRZ_FORCE_VIRTUAL_KEYBOARD");
		return v && *v && *v != '0';
	}();
	return forced;
}

//---------------------------------------------------------------------------
// 内蔵仮想キーボードのレイアウト JSON。
//
//  OS のソフトキーボード (NX の swkbd アプレット / PS5 の SceImeDialog) が
//  無い・使いたくない場面で、 Elements 自身が英数キーボードを描く。
//  キーは押すたびに入力先ダイアログのテキスト欄へ直接流し込むため、 この
//  画面自体は入力内容を保持しない (下の入力欄がそのまま更新される)。
//
//  操作: マウス / タッチ / 矢印キー + Enter / パッド (D-Pad + A=決定,
//  B=閉じる, X=BS, Y=SPACE)。 大文字英数字のみ (v1)。
//---------------------------------------------------------------------------
std::string BuildVirtualKeyboardJson()
{
	// 1 キー分: 固定幅セル + invert_button (focus で反転 = パッド操作可視)
	auto appendKey = [](std::string& j, const char* label, const std::string& id,
	                    int width, bool close_on_click = false,
	                    bool initial_focus = false) {
		j += "{\"type\":\"hsize\",\"width\":" + std::to_string(width) +
		     ",\"child\":{\"type\":\"invert_button\",\"text\":\"" + label +
		     "\",\"id\":\"" + id + "\",\"size\":20";
		if (close_on_click) j += ",\"close_on_click\":true";
		if (initial_focus)  j += ",\"initial_focus\":true";
		j += "}}";
	};

	std::string j;
	j.reserve(4096);
	j += "{";
	j += "\"size\":[560,340],";
	j += "\"background\":[30,32,42,235],";
	j += "\"input\":{\"arrow_focus_nav\":true,\"dpad_mode\":\"focus\","
	     "\"left_stick_mode\":\"focus\",\"shortcuts\":["
	     "{\"pad\":\"x\",\"target\":\"bs\"},"
	     "{\"pad\":\"y\",\"target\":\"spc\"}]},";
	j += "\"content\":{\"type\":\"margin\",\"padding\":16,"
	     "\"child\":{\"type\":\"vtile\",\"children\":[";

	static const char* const rows[] = { "1234567890", "QWERTYUIOP",
	                                    "ASDFGHJKL", "ZXCVBNM" };
	for (size_t i = 0; i < 4; ++i) {
		if (i) j += ",{\"type\":\"vspacer\",\"height\":6},";
		j += "{\"type\":\"align_center\",\"child\":{\"type\":\"htile\",\"children\":[";
		for (const char* p = rows[i]; *p; ++p) {
			if (p != rows[i]) j += ",";
			const char label[2] = { *p, 0 };
			appendKey(j, label, std::string("k_") + *p, 46,
			          false, (i == 0) && (p == rows[i]));
		}
		j += "]}}";
	}

	// 下段: SPACE / BS / DONE。 打鍵ごとに入力先へ確定しているので取消は無い。
	j += ",{\"type\":\"vspacer\",\"height\":12},";
	j += "{\"type\":\"align_center\",\"child\":{\"type\":\"htile\",\"children\":[";
	appendKey(j, "SPACE", "spc", 140);
	j += ",{\"type\":\"hspacer\",\"width\":8},";
	appendKey(j, "BS", "bs", 70);
	j += ",{\"type\":\"hspacer\",\"width\":8},";
	appendKey(j, "DONE", "done", 110, /*close_on_click=*/true);
	j += "]}}";

	j += "]}}";   // vtile / margin
	j += "}";
	return j;
}

} // anonymous

//---------------------------------------------------------------------------
// 内部実装 (PIMPL)
//---------------------------------------------------------------------------
struct tTVPElementsDialogManager::Impl
{
	// Dialog の描画密度モード (スクリプトから ElementsDialog.renderScale で切替)。
	//   0  = auto (既定): 最終 present サイズで直接ラスタライズする。 authored が
	//        surface より大きい画面 (1920x1080 authored を 720p surface へ等) は
	//        縮小率ぶん小さい buffer で描くので CPU ラスタ/転送が最小になる。
	//   >0 = authored 論理サイズ × この倍率で描き、 present 時に拡縮する
	//        (1.0 = 原寸レンダ→縮小表示、 2.0 = 旧 supersampling 相当)。
	float render_scale_mode = 0.0f;

	// 再ラスタライズ抑止 (Dialog.renderCache)。 true (既定) なら変化の無い
	// フレームは overlay_session の再ラスタライズ + アップロードを省略し、
	// レンダラ保持の前回テクスチャをそのまま提示する。
	bool render_cache = true;

	// 部分再描画 (Dialog.partialRedraw)。 true (既定) なら、 ダーティが矩形で
	// 特定できる変化 (キャレット点滅等) はその矩形だけを再ラスタ + 部分転送
	// する。 renderCache 有効時のみ機能 (staging に前回フレームが残る前提)。
	bool partial_redraw = true;

	// 実際にラスタライズした累計回数 (Dialog.renderCount)。 アイドル時に
	// 増えないことの確認・負荷比較用。
	tjs_uint64 raster_count = 0;

	// 区間計測 (Dialog.renderStats)。 累積値、 ResetRenderStats で 0 クリア。
	// steady_clock 呼出はフレームあたり数回なのでオーバーヘッドは無視できる。
	tTVPElementsRenderStats stats;

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
		bool ever_active = false;     // 一度でも active になったか (OnClosed 発火条件)
		bool close_requested = false; // 次フレーム PaintOverlay で teardown

		// close_on_click / Esc で finish したときの action id (Close() 等の
		// 外部要因 close は空のまま)。 teardown 時の OnClosed に渡す。
		ttstr close_action;

		// dialog 論理サイズ (JSON "size" → content フィット後)。
		int dialog_w = 400;
		int dialog_h = 220;

		// present fit の倍率と配置オフセット (window client 座標)。
		// 配置 / 拡縮の基準領域は画面 JSON top-level "base" で選択:
		// "window" (既定) = ウィンドウ全面 / "content" = ゲーム画像 (DestRect)。
		// 拡縮率はゲーム基準面 (primary layer) に対する基準領域の比率
		// (詳細は RenderInstance のコメント)。 マウス座標を dialog 論理座標へ
		// 戻す逆変換にも使う。
		float present_scale = 1.0f;
		float present_off_x = 0.0f;
		float present_off_y = 0.0f;

		// 入力座標の補正用に直近の DestRect 原点を保持。 マウスイベントは
		// TranslateWindowToDrawArea で DestRect 原点を引いた「描画領域基準」で
		// 届く (スケールは掛かっていない) ので、 ウィンドウ座標へ足し戻す。
		int dest_offset_x = 0;
		int dest_offset_y = 0;

		// 直近 render_to_buffer の描画矩形 (dialog 論理座標)。 ヒットテスト用。
		elements_modal::overlay_session::render_rect last_rect{};
		bool has_rect = false;
		bool cursor_inside = false;   // mouse enter/leave 追跡

		// === 再ラスタライズ抑止 (renderCache) 用の前回描画条件 ===
		// session が dirty でなく、 かつ描画条件 (デバイス / buffer ピクセル
		// サイズ / 配置基準 surface) が前回と一致するフレームは、 レンダラが
		// layer キーで保持しているテクスチャを同じ位置に提示するだけで済む。
		bool cache_valid = false;                  // 提示可能な前回描画があるか
		iTVPDrawDevice* cache_device = nullptr;    // 前回描画したデバイス
		int cache_buf_w = 0, cache_buf_h = 0;      // 前回の buffer ピクセルサイズ
		int cache_sw = 0, cache_sh = 0;            // 前回の render_sw/sh (配置基準)
		// 前回の surface サイズ (present fit の基準)。 ウィンドウリサイズ /
		// フルスクリーン切替で変わったら cache_px 等の提示引数が古くなるので
		// 再描画に回す。
		int cache_surf_w = 0, cache_surf_h = 0;
		// 前回の present fit 倍率。 基準面 (primary layer) サイズだけが変わって
		// fit が変わるケース (surface / buffer サイズは同じ) でも提示引数を
		// 作り直すために条件へ含める。
		float cache_fit = 0.0f;
		// 前回の配置 / 拡縮基準領域 ("base":"content" では DestRect)。
		// DestRect だけが動くケース (setViewport 等) で提示位置を作り直す。
		int cache_area_x = 0, cache_area_y = 0;
		int cache_area_w = 0, cache_area_h = 0;
		int cache_px = 0, cache_py = 0;            // 前回 PresentOverlay の引数
		int cache_pw = 0, cache_ph = 0;

		// === navigator フロー (複数画面遷移) ===
		std::unique_ptr<elements_modal::navigator> nav;
		std::map<std::string, std::string> screen_jsons;
		ttstr manifest_base;
		std::string flow_lang;

		// 現画面の資材解決基準ディレクトリ (utf-8)。 universal 遷移の rule 画像を
		// 「遷移を宣言した画面 (= 旧画面)」からの相対で解決するのに使う。 単発
		// JSON / インラインフローでは空のこともある。
		std::string current_resource_base;

		// === 画面切替遷移エフェクト (transitions の effect: fade / universal) ===
		// last_frame: 直近描画フレームの複製 (nav フローのみ毎フレーム更新)。
		// 遷移確定時に from 側スナップショット (trans_from) へ move する。
		// session は finish 後 render_to_buffer が false を返すため、 finish を
		// 検知してからでは旧画面を描き直せない — 直近フレーム保持方式にする。
		std::vector<tjs_uint32> last_frame;
		int last_frame_w = 0, last_frame_h = 0;

		std::vector<tjs_uint32> trans_from;      // 旧画面スナップショット
		int trans_from_w = 0, trans_from_h = 0;
		std::string trans_effect;                // "" = 遷移中でない
		bool trans_started = false;              // 新画面の初回描画で計時開始
		tjs_uint32 trans_start_tick = 0;
		int trans_duration_ms = 0;
		std::vector<tjs_uint8> trans_rule;       // universal の rule (trans_from と同画素数)
		int trans_vague = 64;

		// Dialog.close() 等の外部 close 要求で exit 演出を再生中。 finished() 後は
		// transitions を解決せず (フローを進めず) FinishSingle で終了する。
		bool close_after_exit = false;

		// renderer のテクスチャ識別キー (Instance ごとに一意)。
		const void* LayerKey() const { return static_cast<const void*>(this); }
	};

	std::vector<std::unique_ptr<Instance>> instances;  // z-order (末尾=最前面)

	// i18n の表示言語 (SetLanguage で設定)。 空 = 画面 JSON の "lang" 任せ。
	// BeginScreen で新しい session にも流し込むので、 これ以後に開く画面も
	// 同じ言語で立ち上がる。
	std::string language;

	// DrawDevice ごとの描画アダプタ提供口 (host)。 renderer 自体は DrawDevice が所有し、
	// ここは host ポインタを借用保持するだけ (非所有)。 host 経由で renderer を取得する。
	std::map<iTVPDrawDevice*, iTVPDialogRendererHost*> hosts;

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

	// --- cursor-warp ナビ ("input":{"cursor_warp":true}) ---
	// 直近に入力を転送してきた window (NoteInputWindow で更新、 非所有)。
	// キー/パッドでフォーカスが動いたとき、 この window の実カーソルを
	// フォーカス先へ SetCursorPos し、 mcsTempHidden で隠す。
	iTVPWindow* input_window = nullptr;
	// warp が生む合成 mouse move を実マウスと区別する期待座標 (layer 座標)。
	// 一致 move はカーソル再表示させず (再 hide)、 不一致 = 実マウスで解除。
	// 入力フォーカス (キーボード/パッドの届き先) の直近の持ち主。 上に載って
	// いたダイアログが閉じて下の画面へ戻ったときに、 フォーカス表示と実カーソル
	// を合わせ直すために使う。
	void*   last_focus_owner = nullptr;

	bool    warp_expect_active = false;
	tjs_int warp_expect_x = 0;
	tjs_int warp_expect_y = 0;

	// --- helpers ---

	// host 経由で DrawDevice の renderer を解決する (具象型は知らない)。 名前は従来
	// のまま (呼出側多数) だが、実体は host->GetDialogRenderer()。
	iTVPDialogRenderer* FindRenderer(iTVPDrawDevice* dev) const
	{
		auto it = hosts.find(dev);
		return (it != hosts.end() && it->second) ? it->second->GetDialogRenderer() : nullptr;
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

	// === ホストホットキー (Elements バイパス) ===
	// 登録キーは Forward* の先頭で判定し、 一致したら「非消費 (false)」で返す
	// = TVP_DIALOG_INTERCEPT がそのまま通常配送 (TJS onKeyDown 等) を続行する。
	// テーブルはプロセス共有 (Window 単位ではない)。 モーダル表示中は無効
	// (モーダル確認の Esc=cancel 等を奪わないため)。
	struct HostHotkey
	{
		tjs_uint   vk = 0;
		tjs_uint32 mods = 0;               // TVP_SS_SHIFT|ALT|CTRL のみ
		bool       during_text_input = false;
	};
	std::vector<HostHotkey> host_hotkeys;

	// テキスト入力ウィジェットにキャレットが立っているインスタンスがあるか
	// (TopmostKeyboardFocus のフォールバックと同じ判定)。
	bool AnyTextInputFocused() const
	{
		for (auto const& inst : instances) {
			if (inst->active && inst->session && inst->session->focus_consumes_text())
				return true;
		}
		return false;
	}

	// vk (+ down は mods 完全一致 / up は vk のみ) がホットキーとしてバイパス
	// されるべきか。 mods 比較は SHIFT|ALT|CTRL の 3bit だけ見る (マウスボタン
	// 状態や REPEAT を無視)。
	bool HostHotkeyBypass(tjs_uint vk, tjs_uint32 shiftFlags, bool isUp) const
	{
		if (host_hotkeys.empty()) return false;
		if (AnyModalActive()) return false;   // モーダル優先
		const tjs_uint32 mods =
			shiftFlags & (TVP_SS_SHIFT | TVP_SS_ALT | TVP_SS_CTRL);
		const bool text_focus = AnyTextInputFocused();
		for (auto const& hk : host_hotkeys) {
			if (hk.vk != vk) continue;
			if (!isUp && hk.mods != mods) continue;
			if (text_focus && !hk.during_text_input) continue;
			return true;
		}
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
		// フォールバック: 非モーダル・非 grabFocus のパネルでも、クリック等で
		// テキスト入力ウィジェット (input_box 等) が実際に focus されている間は
		// そのインスタンスへキー/テキストを届ける (クリックでキャレットが出る
		// のに文字が届かないギャップの解消)。非モーダルの未処理キーは従来どおり
		// ゲームへ素通し (handled pass-through) なので、テキスト欄から focus が
		// 外れればゲームのホットキーも復帰する。
		for (auto it = instances.rbegin(); it != instances.rend(); ++it) {
			Instance* inst = it->get();
			if (inst->active && inst->session && inst->session->focus_consumes_text())
				return inst;
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

	// マウス座標 → session へ渡す座標。 入力は TranslateWindowToDrawArea で
	// DestRect 原点を引いた「描画領域基準」で届く (スケールは掛かっていない)
	// ので、 まず原点を足し戻して window client 座標にし、 present fit の
	// 倍率と配置オフセットの逆変換をかけて dialog 論理座標へ戻す
	// (session 内 hit-test は論理座標のため)。
	static float ToSurfaceX(const Instance& inst, tjs_int draw_x)
	{
		return (static_cast<float>(draw_x + inst.dest_offset_x)
		        - inst.present_off_x) / inst.present_scale;
	}
	static float ToSurfaceY(const Instance& inst, tjs_int draw_y)
	{
		return (static_cast<float>(draw_y + inst.dest_offset_y)
		        - inst.present_off_y) / inst.present_scale;
	}

	// 指定 surface 座標が inst の描画矩形内か。
	static bool RectContains(const Instance& inst, float sx, float sy)
	{
		if (!inst.has_rect) return false;
		const auto& r = inst.last_rect;
		return sx >= r.x && sx < (r.x + r.w) && sy >= r.y && sy < (r.y + r.h);
	}

	// このプラットフォームがオンスクリーンキーボード (Android / iOS 等) を持つか。
	// true の場合、 テキスト入力開始はソフトキーボードを画面に出す。 そのため
	// 「ダイアログを開いた瞬間に無条件で開始」ではなく、 テキスト欄に focus が
	// 入ったときだけ開始する focus 駆動に切り替える。 デスクトップ (false) は物理
	// キーボードなので従来どおり開いた時点で開始してよい (ポップアップは出ない)。
	// WINVER (Win32 host) は常に false = デスクトップ扱い。
	//
	// 物理キーボードが繋がっている場合も false (デスクトップ扱い) を返す。 この
	// 状態では SDL の自動判定がキーボード UI を出さないので、 ホストが張った
	// ベースライン (SDL3WindowForm の TVPUpdateBaselineTextInput) をそのまま
	// 使えばよい。 ここで true を返すと focus が外れるたびに HostStopTextInput()
	// でベースラインごと止めてしまい、 ゲーム側 (KAG の Edit レイヤ) の文字入力が
	// 効かなくなる。
	static bool PlatformUsesScreenKeyboard()
	{
		if (!HostHasScreenKeyboard()) return false;
		return !HostHasPhysicalKeyboard();
	}

	// focus 駆動でソフトキーボードを出している最中か (portable のみ使用)。
	bool ime_focus_active = false;

	// UTF-16 サロゲートペアの high surrogate を一時保持する (ForwardKeyPress 用)。
	// WINVER の WM_CHAR は BMP 外 (絵文字 / 拡張漢字) を high/low 2 回に分けて
	// tjs_char (16bit) で配信するため、 high を受けたら保持し、 続く low と合成して
	// 1 コードポイントにする。 0 = 保持なし。
	tjs_uint16 pending_high_surrogate = 0;

	// テキスト入力受信の開始/停止 (ウィンドウ単位なので参照カウント的に扱う)。
	void StartTextInputIfNeeded()
	{
		// portable はここでは開始しない。 UpdateFocusDrivenTextInput() が
		// テキスト欄への focus を検出して開始/停止する。
		if (PlatformUsesScreenKeyboard()) return;
		HostStartTextInput();
	}
	void StopTextInputIfNoInstances()
	{
		if (instances.empty()) {
			// デスクトップ (物理キーボード) では text input を有効のままでも
			// ソフトキーボード等の副作用が無く、 ホスト (ゲーム) が常時テキスト
			// 入力を必要とする場合がある (例: タイトル画面での名前入力)。 ここで
			// 無条件停止するとホスト側の文字入力まで止まるため、 オンスクリーン
			// キーボードを持つ環境でのみ停止する。 デスクトップでは host が設定
			// したベースライン (form 生成時の StartTextInput) をそのまま残す。
			if (PlatformUsesScreenKeyboard()) {
				HostStopTextInput();
			}
			ime_focus_active = false;
		}
	}

	// portable 用: 最前面フォーカスインスタンスのテキスト欄 focus 状態に追従して
	// ソフトキーボードを出し入れする。 PaintOverlay 末尾から毎フレーム呼ぶ。
	// デスクトップ (WINVER 含む) では no-op (開いた時点で開始済み・ポップアップも無い)。
	void UpdateFocusDrivenTextInput()
	{
		// デスクトップは元々 OS キーボードもポップアップも無いので通常は no-op。
		// ただし "always" 指定時は動作確認のためデスクトップでも仮想キーボードを出す。
		if (!PlatformUsesScreenKeyboard() && vk_mode != VKMode::Always) return;

		// 仮想キーボード表示中は、 focus はキーボード自身が持っている
		// (= 下の入力欄は focus_consumes_text() を返さない)。 通常判定に
		// 戻すと即座に閉じてしまうので、 入力先の生存確認だけ行う。
		if (vk_shown) {
			if (!FindInstance(vk_target)) CloseVirtualKeyboard();
			return;
		}

		Instance* owner = TopmostKeyboardFocus();
		bool want = owner && owner->active && owner->session &&
		            owner->session->focus_consumes_text();

		if (want && !ime_focus_active) {
			// 仮想キーボードを使うか (auto = 物理キーボードが無いときだけ)
			const bool use_vk = (vk_mode == VKMode::Always) ||
			                    (vk_mode == VKMode::Auto && !HostHasPhysicalKeyboard());
			if (!use_vk) {
				// 物理キーボードあり / "never": そのまま打てるので text 入力を
				// 有効化するだけ (OS のソフトキーボードは SDL の auto 判定で出ない)。
				HostStartTextInput();
				ime_focus_active = true;
			} else if (owner != vk_dismissed_for) {
				// 物理キーボード無し: OS のキーボードではなく内蔵仮想キーボード。
				// vk_dismissed_for は「閉じた直後に同じ欄で出し直さない」ラッチ。
				ShowVirtualKeyboard(owner);
			}
		} else if (!want) {
			if (ime_focus_active) {
				HostStopTextInput();      // focus が外れた / ダイアログ閉じ
				ime_focus_active = false;
			}
			vk_dismissed_for = nullptr;   // focus が外れたらラッチ解除
		}
	}

	// === 内蔵仮想キーボード (物理キーボード非接続時の入力手段) ===

	// instances に今も居るかで Instance* の生存を確認する (handler を持たない
	// インスタンスもあるので FindByHandler ではなくポインタ照合)。
	Instance* FindInstance(Instance* p) const
	{
		if (!p) return nullptr;
		for (auto const& inst : instances) {
			if (inst.get() == p) return inst->active ? p : nullptr;
		}
		return nullptr;
	}

	void ShowVirtualKeyboard(Instance* target)
	{
		if (vk_shown || !target) return;
		vk_handler.impl = this;
		Instance* inst = PushInstance(&vk_handler, target->host_device,
		                              /*modal=*/true, /*grabFocus=*/true);
		if (!BeginScreen(*inst, BuildVirtualKeyboardJson(), std::string())) {
			TeardownInstance(inst);
			return;
		}
		inst->active = true;
		inst->ever_active = true;
		vk_target = target;
		vk_shown = true;
		TVPAddLog(TJS_W("ElementsDialog: virtual keyboard shown"));
	}

	void CloseVirtualKeyboard()
	{
		Instance* inst = FindByHandler(&vk_handler);
		if (inst && inst->active) RequestClose(*inst);
		// 実際の状態リセットは teardown 後の OnClosed (VirtualKeyboardHandler)
	}

	// キーが押された: 押鍵をそのまま入力先のテキスト欄へ流し込む。
	void OnVirtualKeyboardAction(const std::string& id)
	{
		Instance* t = FindInstance(vk_target);
		if (!t || !t->session) { CloseVirtualKeyboard(); return; }
		if (id.size() > 2 && id[0] == 'k' && id[1] == '_') {
			t->session->on_text_input(id.substr(2).c_str());
		} else if (id == "spc") {
			t->session->on_text_input(" ");
		} else if (id == "bs") {
			// 文字削除はテキストでなくキーイベント (入力欄の編集操作)
			t->session->on_key_down(cycfi::elements::key_code::backspace, 0);
			t->session->on_key_up(cycfi::elements::key_code::backspace, 0);
		}
		// "done" は close_on_click → OnClosed で終了
	}

	void OnVirtualKeyboardClosed()
	{
		vk_shown = false;
		// 閉じた直後は入力先へ focus が戻る。 そのまま出し直すと閉じられなく
		// なるので、 focus が一度離れるまで再表示しない。
		vk_dismissed_for = vk_target;
		vk_target = nullptr;
	}

	struct VirtualKeyboardHandler : public iTVPDialogEventHandler
	{
		Impl* impl = nullptr;
		void OnAction(const ttstr& id, const tTJSVariant&) override
		{
			if (impl) impl->OnVirtualKeyboardAction(TtstrToUtf8(id));
		}
		void OnClosed(const ttstr&) override
		{
			if (impl) impl->OnVirtualKeyboardClosed();
		}
	};
	VirtualKeyboardHandler vk_handler;
	Instance* vk_target = nullptr;          //!< 入力先 (生存確認は FindInstance)
	Instance* vk_dismissed_for = nullptr;   //!< 閉じた直後の再表示抑止ラッチ
	bool vk_shown = false;

	//! 仮想キーボードの動作モード (TJS: ElementsDialog.virtualKeyboard)。
	//! 初期値は環境変数 KRKRZ_FORCE_VIRTUAL_KEYBOARD で "always" になる。
	enum class VKMode { Auto, Always, Never };
	VKMode vk_mode = ForceVirtualKeyboard() ? VKMode::Always : VKMode::Auto;

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
		// OnClosed はリストから外し終えてから発火する (callback 内から
		// 同 handler での Show* 再入があっても FindByHandler が旧インスタンス
		// を拾わないように)。 show 失敗時の teardown では発火しない。
		iTVPDialogEventHandler* handler =
			inst->ever_active ? inst->handler : nullptr;
		ttstr close_action = inst->close_action;
		for (auto it = instances.begin(); it != instances.end(); ++it) {
			if (it->get() == inst) {
				instances.erase(it);
				break;
			}
		}
		StopTextInputIfNoInstances();
		if (handler) handler->OnClosed(close_action);
	}

	// 全インスタンスを即破棄。
	void TeardownAll()
	{
		std::vector<std::pair<iTVPDialogEventHandler*, ttstr>> closed;
		for (auto& inst : instances) {
			if (auto* r = FindRenderer(inst->host_device)) {
				r->ReleaseLayer(inst->LayerKey());
			}
			if (inst->ever_active && inst->handler) {
				closed.emplace_back(inst->handler, inst->close_action);
			}
		}
		instances.clear();
		HostStopTextInput();
		ime_focus_active = false;
		for (auto& [handler, action] : closed) handler->OnClosed(action);
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

	// --- 画面切替遷移エフェクト (fade / universal) ---

	void ClearTransition(Instance& inst)
	{
		inst.trans_from.clear();
		inst.trans_from_w = inst.trans_from_h = 0;
		inst.trans_effect.clear();
		inst.trans_started = false;
		inst.trans_start_tick = 0;
		inst.trans_duration_ms = 0;
		inst.trans_rule.clear();
	}

	// 遷移確定時 (AdvanceFlow、 旧画面 teardown 前) に呼び、 last_frame を from 側
	// スナップショットへ move + rule 画像ロード等の準備をする。
	void PrepareScreenTransition(Instance& inst, const elements_modal::nav_step& step);

	// rule 画像を Storages からロードし 8bpp グレースケール + dst サイズへ展開。
	bool LoadTransitionRule(Instance& inst, const std::string& rule_utf8,
	                        int dst_w, int dst_h);

	// RenderInstance で新画面を描いた buffer に対し、 遷移中なら from と混色する。
	void ApplyScreenTransition(Instance& inst, tjs_uint32* buf,
	                           int w_pixels, int h_pixels);

	// 外部 close 要求 (Dialog.close / QUIT)。 session 生存中は session->close()
	// 経由で exit 演出と協調し、 finished() 後に FinishSingle で終了する。
	void RequestClose(Instance& inst)
	{
		if (inst.session && !inst.session->finished() && !inst.close_requested) {
			inst.close_after_exit = true;
			inst.session->close();   // exit 演出があれば再生後に finished()
		} else {
			inst.active = false;
			inst.close_requested = true;   // 次フレーム teardown (再入安全)
		}
	}

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
	// JSON の top-level "size":[w,h] を peek (上限サイズ)。
	// 注意: widget の "size": N (フォントサイズ等) と衝突しないよう、値が配列
	// ([ で始まる) の "size" だけを採用する (従来は最初の "size" の後の '[' を
	// 拾っていて font size と衝突していた)。
	// "size" が無ければ overlay の既定を surface (ゲーム画面) 全面にする
	// (従来は 400x220 きめうちで、content が大きいとクリップされていた)。
	inst.dialog_w = 0;
	inst.dialog_h = 0;
	bool has_explicit_size = false;
	{
		size_t search = 0;
		while (true) {
			auto pos = json_utf8.find("\"size\"", search);
			if (pos == std::string::npos) break;
			auto colon = json_utf8.find(':', pos);
			size_t vs = (colon != std::string::npos) ? colon + 1
			                                         : std::string::npos;
			while (vs != std::string::npos && vs < json_utf8.size() &&
			       (json_utf8[vs] == ' '  || json_utf8[vs] == '\t' ||
			        json_utf8[vs] == '\n' || json_utf8[vs] == '\r')) vs++;
			if (vs != std::string::npos && vs < json_utf8.size() &&
			    json_utf8[vs] == '[') {
				const char* p = json_utf8.c_str() + vs + 1;
				char* endp = nullptr;
				int w = (int)std::strtol(p, &endp, 10);
				int h = (endp && *endp == ',')
				            ? (int)std::strtol(endp + 1, nullptr, 10) : 0;
				if (w > 0 && h > 0) {
					inst.dialog_w = w; inst.dialog_h = h;
					has_explicit_size = true;
				}
				break;   // 値が配列の "size" を最初に見つけた時点で確定
			}
			search = pos + 6;   // strlen("\"size\"")
		}
	}
	if (!has_explicit_size) {
		int sw = 0, sh = 0;
		if (auto* r = FindRenderer(inst.host_device)) r->GetSurfaceSize(sw, sh);
		if (sw > 0 && sh > 0) { inst.dialog_w = sw; inst.dialog_h = sh; }
		else                  { inst.dialog_w = 400; inst.dialog_h = 220; }
	}

	auto sess = std::make_unique<elements_modal::overlay_session>();
	auto bridge = MakeBridgeCallback(inst.handler);
	// pixel_scale は 1.0 固定 — 実際の描画密度は render_to_buffer に渡す buffer
	// サイズから毎回導出される (render_scale_mode 参照)。
	if (!sess->start(json_utf8, inst.dialog_w, inst.dialog_h, 1.0f,
	                 std::move(bridge), resource_base_utf8)) {
		return false;
	}

	// i18n: ホストが SetLanguage 済みなら、 新しく開く画面もその言語で始める
	// (画面 JSON の "lang" 既定より優先)。 "strings" を持たない画面では no-op。
	if (!language.empty()) sess->set_language(language);

	// run_modal と同じく content の自然サイズへフィット (上側空欄対策)。
	// ただし top-level "size" が明示された画面は作者が寸法を指定しているので
	// 縮めない (指定サイズを尊重)。 明示サイズが surface より大きい場合は
	// RenderInstance 側で surface にスケール present する (1920x1080 authored 画面等)。
	if (!has_explicit_size) {
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
	inst.current_resource_base = resource_base_utf8;
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
	inst.ever_active = true;
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

	elements_modal::nav_step step;
	if (inst.nav) step = inst.nav->advance(r.action, inst.session->transitions());

	// 次画面がある場合のみ遷移エフェクトを準備する (旧画面の直近フレームを
	// from 側スナップに move、 rule 解決は旧画面の resource_base 基準なので
	// StartCurrentScreen で上書きされる前のここで行う)。
	const bool flow_continues = inst.nav && !inst.nav->empty();
	if (flow_continues) PrepareScreenTransition(inst, step);

	inst.session.reset();

	if (!flow_continues) {
		SnapshotResult(inst, action, values);
		inst.active = false;
		inst.close_requested = true;
		inst.close_action = action;
		return;
	}

	if (!StartCurrentScreen(inst)) {
		SnapshotResult(inst, action, values);
		inst.active = false;
		inst.close_requested = true;
		inst.close_action = action;
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
	inst.close_action = action;
	TVPAddLog(TJS_W("ElementsDialog: session auto-finished (close_on_click)"));
}

//---------------------------------------------------------------------------
// 画面切替遷移エフェクト (transitions の effect: "fade" / "universal")
//---------------------------------------------------------------------------
void tTVPElementsDialogManager::Impl::PrepareScreenTransition(
	Instance& inst, const elements_modal::nav_step& step)
{
	ClearTransition(inst);
	if (step.effect.empty()) return;
	if (step.effect != "fade" && step.effect != "universal") {
		TVPAddImportantLog(
			ttstr(TJS_W("ElementsDialog flow: unsupported transition effect: "))
			+ Utf8ToTtstr(step.effect) + TJS_W(" (instant switch)"));
		return;
	}
	// まだ一度も描画していない (初回画面直後など) → スナップ無し = 即切替
	if (inst.last_frame.empty() ||
	    inst.last_frame_w <= 0 || inst.last_frame_h <= 0) return;

	inst.trans_from   = std::move(inst.last_frame);
	inst.trans_from_w = inst.last_frame_w;
	inst.trans_from_h = inst.last_frame_h;
	inst.last_frame.clear();
	inst.last_frame_w = inst.last_frame_h = 0;

	inst.trans_effect      = step.effect;
	inst.trans_duration_ms = (step.duration_ms > 0) ? step.duration_ms : 200;
	inst.trans_started     = false;   // 新画面の初回描画から計時
	inst.trans_vague       = step.vague;

	if (step.effect == "universal") {
		if (step.rule.empty() ||
		    !LoadTransitionRule(inst, step.rule,
		                        inst.trans_from_w, inst.trans_from_h)) {
			TVPAddImportantLog(
				ttstr(TJS_W("ElementsDialog flow: universal rule unavailable, "))
				+ TJS_W("fallback to fade: ") + Utf8ToTtstr(step.rule));
			inst.trans_effect = "fade";
			inst.trans_rule.clear();
		}
	}
}

bool tTVPElementsDialogManager::Impl::LoadTransitionRule(
	Instance& inst, const std::string& rule_utf8, int dst_w, int dst_h)
{
	if (dst_w <= 0 || dst_h <= 0) return false;

	// 解決順: 現画面 (= 遷移を宣言した旧画面) の resource_base 相対 →
	// そのままの Storages パス → autopath 検索。
	const ttstr name = Utf8ToTtstr(rule_utf8);
	ttstr resolved;
	if (!inst.current_resource_base.empty()) {
		ttstr cand = Utf8ToTtstr(inst.current_resource_base) + name;
		if (TVPIsExistentStorage(cand)) resolved = cand;
	}
	if (resolved.IsEmpty() && TVPIsExistentStorage(name)) resolved = name;
	if (resolved.IsEmpty()) {
		ttstr placed = TVPGetPlacedPath(name);   // autopath (無ければ空)
		if (!placed.IsEmpty()) resolved = placed;
	}
	if (resolved.IsEmpty()) return false;

	try {
		tTVPBaseBitmap bmp(16, 16, 8);
		TVPLoadGraphic(&bmp, resolved, 0, 0, 0, glmGrayscale);
		const int src_w = (int)bmp.GetWidth();
		const int src_h = (int)bmp.GetHeight();
		if (src_w <= 0 || src_h <= 0) return false;

		// バイリニアで present バッファサイズへ展開 (rule は 8bpp グレースケール)。
		inst.trans_rule.resize((size_t)dst_w * dst_h);
		for (int y = 0; y < dst_h; ++y) {
			const float sy = (dst_h > 1)
				? (float)y * (src_h - 1) / (dst_h - 1) : 0.0f;
			const int y0 = (int)sy;
			const int y1 = std::min(y0 + 1, src_h - 1);
			const float fy = sy - y0;
			const tjs_uint8* r0 = (const tjs_uint8*)bmp.GetScanLine(y0);
			const tjs_uint8* r1 = (const tjs_uint8*)bmp.GetScanLine(y1);
			tjs_uint8* out = &inst.trans_rule[(size_t)y * dst_w];
			for (int x = 0; x < dst_w; ++x) {
				const float sx = (dst_w > 1)
					? (float)x * (src_w - 1) / (dst_w - 1) : 0.0f;
				const int x0 = (int)sx;
				const int x1 = std::min(x0 + 1, src_w - 1);
				const float fx = sx - x0;
				const float v0 = r0[x0] + (r0[x1] - r0[x0]) * fx;
				const float v1 = r1[x0] + (r1[x1] - r1[x0]) * fx;
				out[x] = (tjs_uint8)(v0 + (v1 - v0) * fy + 0.5f);
			}
		}
		return true;
	} catch (...) {
		// ロード失敗は呼出側で fade フォールバック (ログも呼出側)。
		return false;
	}
}

void tTVPElementsDialogManager::Impl::ApplyScreenTransition(
	Instance& inst, tjs_uint32* buf, int w_pixels, int h_pixels)
{
	if (inst.trans_effect.empty()) return;
	if (inst.trans_from.empty() ||
	    inst.trans_from_w != w_pixels || inst.trans_from_h != h_pixels) {
		// buffer サイズが変わった (画面サイズ / DPI / renderScale 変更) → 即切替
		ClearTransition(inst);
		return;
	}
	const tjs_uint32 now = TVPGetRoughTickCount32();
	if (!inst.trans_started) {
		inst.trans_started = true;
		inst.trans_start_tick = now;
	}
	const tjs_uint32 elapsed = now - inst.trans_start_tick;
	if (inst.trans_duration_ms <= 0 ||
	    elapsed >= (tjs_uint32)inst.trans_duration_ms) {
		ClearTransition(inst);
		return;
	}
	const float t = (float)elapsed / (float)inst.trans_duration_ms;
	const size_t count = (size_t)w_pixels * h_pixels;
	if (inst.trans_effect == "universal" && inst.trans_rule.size() == count) {
		elements_modal::blend_universal_argb8888(
			inst.trans_from.data(), buf, inst.trans_rule.data(),
			t, inst.trans_vague, buf, count);
	} else {
		elements_modal::blend_argb8888(
			inst.trans_from.data(), buf, t, buf, count);
	}
}

void tTVPElementsDialogManager::Impl::RenderInstance(
	Instance& inst, iTVPDrawDevice* device, iTVPDialogRenderer* renderer)
{
	const int w_logical = inst.dialog_w;
	const int h_logical = inst.dialog_h;

	int sw = 0, sh = 0;
	renderer->GetSurfaceSize(sw, sh);
	int dx = 0, dy = 0, dw = 0, dh = 0;
	renderer->GetDestRect(dx, dy, dw, dh);
	if (sw <= 0 || sh <= 0) {
		sw = dx + dw;
		sh = dy + dh;
	}
	// 入力座標の補正用 (ToSurfaceX/Y 参照)。 マウスは DestRect 原点を引いた
	// 描画領域基準で届くため、 足し戻す分を控えておく。
	inst.dest_offset_x = dx;
	inst.dest_offset_y = dy;

	// 配置 / 拡縮の基準領域は、 画面 JSON の top-level "base" で選べる:
	//   "window" (既定) — **ウィンドウ (surface) 全体**。 Dot by dot 等の
	//     インセット表示でもパネルはウィンドウ全面基準に置かれる。
	//   "content"       — **ゲーム画像の表示領域 (DestRect)**。 字幕窓のように
	//     ゲーム画像へ追従させたいパネル向け。 レターボックスやインセット
	//     表示でもゲーム画像に張り付き、 拡縮もゲーム画像と同率になる。
	//     DestRect が無効な場合はウィンドウ基準へフォールバック。
	const bool content_base = inst.session &&
		inst.session->placement_base() == elements_modal::overlay_base::content;
	int area_x = 0, area_y = 0, area_w = sw, area_h = sh;
	if (content_base && dw > 0 && dh > 0) {
		area_x = dx; area_y = dy; area_w = dw; area_h = dh;
	}

	// 拡縮率 (present fit) の基準は「ゲームの基準面 (primary layer サイズ)」。
	// 基準領域が基準面より大きいときは、 ゲーム画像と同様に UI も拡大して
	// フィットさせる: 基準面いっぱいに author した全画面 UI は基準領域全面に、
	// 小さいパネルは基準面に対する相対サイズを保ったまま拡大される。
	// 基準領域が基準面と同サイズ (通常のウィンドウ表示 / "content" 基準で
	// 等倍表示) なら等倍 = authored サイズのまま。
	// ※ この基準をダイアログ自身の authored サイズにすると、 小さいパネルまで
	//    基準領域いっぱいに拡大されてしまう (2026-08-22 の退行。 全画面 UI は
	//    たまたま基準面 = authored なので区別が付かなかった)。
	// 基準面が取れない場合 (primary layer 無しの UI 専用構成) はダイアログ自身の
	// authored サイズを基準にする (= 全画面 UI とみなして基準領域へフィット)。
	// いずれの基準でも、 パネル自身が基準領域へ収まらない場合は収まるまで
	// 縮める (従来からの oversized 縮小)。
	//
	// render_to_buffer には dialog 自身の logical サイズを "surface" として
	// 渡し、 canvas 全体を buffer に描かせる (実ウィンドウサイズを渡すと
	// 中央配置/クリップで一部しか描かれない)。 present 時に fit へ拡縮し、
	// 配置 (align/margin) は manager が基準領域内で行う。
	const int render_sw = w_logical;
	const int render_sh = h_logical;

	float fit = 1.0f;
	if (area_w > 0 && area_h > 0 && w_logical > 0 && h_logical > 0) {
		tjs_int src_w = 0, src_h = 0;
		if (device) device->GetSrcSize(src_w, src_h);
		if (src_w > 0 && src_h > 0) {
			fit = std::min(static_cast<float>(area_w) / src_w,
			               static_cast<float>(area_h) / src_h);
		} else {
			fit = std::min(static_cast<float>(area_w) / w_logical,
			               static_cast<float>(area_h) / h_logical);
		}
		// パネル自身が基準領域からはみ出すなら収まるまで縮小
		fit = std::min(fit, std::min(static_cast<float>(area_w) / w_logical,
		                             static_cast<float>(area_h) / h_logical));
	}

	// 描画密度 (render_scale_mode コメント参照): auto は最終 present ピクセル
	// サイズで直接描く (縮小なら小さい buffer、 拡大なら大きい buffer =
	// フルスクリーン拡大でも滲まない)。 >0 は authored 論理サイズ×倍率で
	// 描いて present 時に拡縮。 密度は buffer サイズとして overlay_session へ
	// 伝わる (canvas scale は session 側が buffer サイズ ÷ view logical から
	// 導出する)。
	const float density = (render_scale_mode > 0.0f) ? render_scale_mode : fit;
	const int w_pixels = std::max(1, static_cast<int>(w_logical * density + 0.5f));
	const int h_pixels = std::max(1, static_cast<int>(h_logical * density + 0.5f));

	const void* layer = inst.LayerKey();

	// 状態更新 (変数/hover poll・演出 tick・view 遅延タスク・ダーティ蓄積)。
	// 描画をスキップするフレームでも毎フレーム必要 (キャレット点滅・遅延
	// focus 適用・退場演出の完了検出が止まるため)。 戻り値 = 再描画が必要か。
	const auto t_update = std::chrono::steady_clock::now();
	const bool dirty = inst.session->update();
	stats.updates++;
	stats.updateUs += ElapsedUs(t_update);

	// update 中に退場演出が完了 (finished) すると以後は描画できない。 今
	// フレームは直前の描画結果をそのまま提示し、 次フレームの PaintOverlay
	// 冒頭の finished 処理 (AdvanceFlow / FinishSingle) に任せる (従来は
	// 最終フレームまで描いてから finished 検出だったため、 空白フレームを
	// 作らないよう提示だけは継続する)。
	if (inst.session->finished()) {
		if (inst.cache_valid && inst.cache_device == device) {
			const auto t_present = std::chrono::steady_clock::now();
			renderer->PresentOverlay(layer, inst.cache_px, inst.cache_py,
			                         inst.cache_pw, inst.cache_ph);
			stats.presents++;
			stats.cachedPresents++;
			stats.presentUs += ElapsedUs(t_present);
		}
		return;
	}

	// 再ラスタライズ抑止 (Dialog.renderCache): session が dirty でなく、 描画
	// 条件 (デバイス / buffer ピクセルサイズ / 配置基準 surface) も前回と同じ
	// なら、 レンダラが layer キーで保持しているテクスチャを同じ位置に提示する
	// だけで済む (ThorVG ラスタ + 全クリア + アップロードを省略)。 画面遷移
	// エフェクトの混色中は毎フレーム絵が変わるので除外。 ウィンドウリサイズ /
	// NX docked⇔携帯 / renderScale 変更はサイズ不一致となり必ず再描画される。
	if (render_cache && !dirty && inst.trans_effect.empty()
	    && inst.cache_valid
	    && inst.cache_device == device
	    && inst.cache_buf_w == w_pixels && inst.cache_buf_h == h_pixels
	    && inst.cache_sw == render_sw && inst.cache_sh == render_sh
	    && inst.cache_surf_w == sw && inst.cache_surf_h == sh
	    && inst.cache_fit == fit
	    && inst.cache_area_x == area_x && inst.cache_area_y == area_y
	    && inst.cache_area_w == area_w && inst.cache_area_h == area_h) {
		const auto t_present = std::chrono::steady_clock::now();
		renderer->PresentOverlay(layer, inst.cache_px, inst.cache_py,
		                         inst.cache_pw, inst.cache_ph);
		stats.presents++;
		stats.cachedPresents++;
		stats.presentUs += ElapsedUs(t_present);
		return;
	}

	const auto t_acquire = std::chrono::steady_clock::now();
	uint32_t* buf = renderer->AcquireBuffer(layer, w_pixels, h_pixels);
	stats.acquireUs += ElapsedUs(t_acquire);
	if (!buf) return;

	// 部分再描画 (Dialog.partialRedraw): renderCache 有効時のみ (staging に
	// 前回フレームが残っている前提)。 描画条件 (デバイス / buffer サイズ /
	// 配置基準 surface) が前回と一致し、 遷移混色中でない場合に限る。
	const bool allow_partial = render_cache && partial_redraw
	    && inst.trans_effect.empty()
	    && inst.cache_valid && inst.cache_device == device
	    && inst.cache_buf_w == w_pixels && inst.cache_buf_h == h_pixels
	    && inst.cache_sw == render_sw && inst.cache_sh == render_sh
	    && inst.cache_surf_w == sw && inst.cache_surf_h == sh
	    && inst.cache_fit == fit
	    && inst.cache_area_x == area_x && inst.cache_area_y == area_y
	    && inst.cache_area_w == area_w && inst.cache_area_h == area_h;

	const auto t_raster = std::chrono::steady_clock::now();
	elements_modal::overlay_session::render_rect rect{};
	elements_modal::overlay_session::render_rect updated{};
	bool ok;
	if (allow_partial) {
		ok = inst.session->render_to_buffer_partial(buf, w_pixels, h_pixels,
		                                            render_sw, render_sh,
		                                            rect, updated);
	} else {
		ok = inst.session->render_to_buffer(buf, w_pixels, h_pixels,
		                                    render_sw, render_sh, rect);
		updated = { 0, 0, w_pixels, h_pixels };
	}
	stats.rasterUs += ElapsedUs(t_raster);
	// 実際に矩形限定で描かれたか (セッション側は条件不成立なら全面へ
	// フォールバックして buffer 全体を返してくる)
	const bool partial = ok && allow_partial
	    && !(updated.x == 0 && updated.y == 0
	         && updated.w == w_pixels && updated.h == h_pixels);
	if (ok) {
		raster_count++;
		stats.rasters++;
		if (partial) stats.partials++;
		// 画面切替遷移中なら旧画面スナップと混色 (in-place、 upload 前に行う)。
		// (遷移中は allow_partial=false なので必ず全面描画になっている)
		ApplyScreenTransition(inst, buf, w_pixels, h_pixels);

		// フローインスタンスは提示フレームの複製を保持する (次の画面切替の
		// from 側スナップ用)。 finish 後の session は再描画できないため、
		// ここで持っておくしかない。 遷移中は混色後 = 実際に見えている絵。
		// 部分更新時は書き換わった矩形だけ複製する (全面 memcpy 回避)。
		if (inst.nav) {
			if (partial && inst.last_frame_w == w_pixels
			    && inst.last_frame_h == h_pixels
			    && inst.last_frame.size()
			       == (size_t)w_pixels * h_pixels) {
				for (int yy = updated.y; yy < updated.y + updated.h; ++yy) {
					std::memcpy(&inst.last_frame[(size_t)yy * w_pixels + updated.x],
					            buf + (size_t)yy * w_pixels + updated.x,
					            (size_t)updated.w * 4);
				}
			} else {
				inst.last_frame.assign(buf, buf + (size_t)w_pixels * h_pixels);
				inst.last_frame_w = w_pixels;
				inst.last_frame_h = h_pixels;
			}
		}
	}
	const auto t_release = std::chrono::steady_clock::now();
	if (ok && partial) {
		renderer->ReleaseBufferRect(layer, updated.x, updated.y,
		                            updated.w, updated.h);
	} else {
		renderer->ReleaseBuffer(layer);
	}
	stats.uploadUs += ElapsedUs(t_release);
	if (!ok) return;

	inst.last_rect = rect;
	inst.has_rect = true;

	// 配置は基準領域 ("base" = ウィンドウ全面 or DestRect) 内で確定する。
	// fit の倍率とオフセットは Instance に保存し、 ToSurfaceX/Y がマウス座標を
	// dialog 論理座標へ逆変換する (マウス操作対応)。
	// 配置は画面 JSON の top-level "align" / "margin" に従う (既定は中央)。
	// session 側も同じ式で placement を計算するので、 拡縮提示でも揃う
	// (字幕窓のように下端へ寄せたいケース)。
	const int pw = static_cast<int>(w_logical * fit + 0.5f);
	const int ph = static_cast<int>(h_logical * fit + 0.5f);
	float ax = 0.5f, ay = 0.5f;
	int amargin = 0;
	inst.session->placement(ax, ay, amargin);
	const int free_x = area_w - pw;
	const int free_y = area_h - ph;
	int px = area_x + amargin + static_cast<int>((free_x - 2 * amargin) * ax);
	int py = area_y + amargin + static_cast<int>((free_y - 2 * amargin) * ay);
	if (px < area_x) px = area_x;
	if (py < area_y) py = area_y;
	inst.present_scale = fit;
	inst.present_off_x = static_cast<float>(px);
	inst.present_off_y = static_cast<float>(py);

	// 次フレーム以降の cached present 用に描画条件と提示引数を記録する。
	inst.cache_valid = true;
	inst.cache_device = device;
	inst.cache_buf_w = w_pixels;
	inst.cache_buf_h = h_pixels;
	inst.cache_sw = render_sw;
	inst.cache_sh = render_sh;
	inst.cache_surf_w = sw;
	inst.cache_surf_h = sh;
	inst.cache_fit = fit;
	inst.cache_area_x = area_x;
	inst.cache_area_y = area_y;
	inst.cache_area_w = area_w;
	inst.cache_area_h = area_h;
	inst.cache_px = px;
	inst.cache_py = py;
	inst.cache_pw = pw;
	inst.cache_ph = ph;

	const auto t_present = std::chrono::steady_clock::now();
	renderer->PresentOverlay(layer, px, py, pw, ph);
	stats.presents++;
	stats.presentUs += ElapsedUs(t_present);
}

//---------------------------------------------------------------------------
// シングルトン
//---------------------------------------------------------------------------
tTVPElementsDialogManager& tTVPElementsDialogManager::Instance()
{
	static tTVPElementsDialogManager instance;
	return instance;
}

// elements_modal の診断ログ (em_logf) を TVP ログへ回す。 既定は stderr で、
// コンソール機では回収できないため。 API 契約は elements_modal/src/em_platform.h。
namespace elements_modal {
using em_log_sink = void (*)(const char* line);
void em_set_log_sink(em_log_sink sink);
}

tTVPElementsDialogManager::tTVPElementsDialogManager()
	: _impl(std::make_unique<Impl>())
{
	elements_modal::em_set_log_sink(
		[](const char* line) {
			TVPAddLog(ttstr("elements_modal: ") + ttstr(line));
		});
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

void tTVPElementsDialogManager::RegisterHostHotkey(
	tjs_uint vk, tjs_uint32 mods, bool duringTextInput)
{
	const tjs_uint32 m = mods & (TVP_SS_SHIFT | TVP_SS_ALT | TVP_SS_CTRL);
	// 同一 (vk, mods) は上書き (duringTextInput の変更を許す)
	for (auto& hk : _impl->host_hotkeys) {
		if (hk.vk == vk && hk.mods == m) {
			hk.during_text_input = duringTextInput;
			return;
		}
	}
	_impl->host_hotkeys.push_back({ vk, m, duringTextInput });
}

void tTVPElementsDialogManager::UnregisterHostHotkey(tjs_uint vk, tjs_uint32 mods)
{
	const tjs_uint32 m = mods & (TVP_SS_SHIFT | TVP_SS_ALT | TVP_SS_CTRL);
	auto& v = _impl->host_hotkeys;
	v.erase(std::remove_if(v.begin(), v.end(),
		[&](const Impl::HostHotkey& hk) { return hk.vk == vk && hk.mods == m; }),
		v.end());
}

void tTVPElementsDialogManager::ClearHostHotkeys()
{
	_impl->host_hotkeys.clear();
}

bool tTVPElementsDialogManager::IsHandlerActive(iTVPDialogEventHandler* handler) const
{
	Impl::Instance* inst = _impl->FindByHandler(handler);
	return inst && inst->active;
}

void tTVPElementsDialogManager::SetRenderScale(float scale)
{
	_impl->render_scale_mode = (scale > 0.0f) ? scale : 0.0f;
}

float tTVPElementsDialogManager::GetRenderScale() const
{
	return _impl->render_scale_mode;
}

void tTVPElementsDialogManager::SetRenderCache(bool enable)
{
	_impl->render_cache = enable;
}

bool tTVPElementsDialogManager::GetRenderCache() const
{
	return _impl->render_cache;
}

// pad_icon テーマの自動選択 (setPadTheme("auto"))。
//   接続中のパッドの系統 (xbox / ps / switch) をそのままテーマにする。
//   パッドが無い / 判らないときは動作プラットフォームで決める
//   (Switch 本体なら switch、 PS5 なら ps、 それ以外は xbox)。
static bool TVPPadThemeAuto = false;

void tTVPElementsDialogManager::SetPadThemeAuto(bool enable)
{
	TVPPadThemeAuto = enable;
	if (enable) ResolveAutoPadTheme();
}

bool tTVPElementsDialogManager::GetPadThemeAuto() const
{
	return TVPPadThemeAuto;
}

void tTVPElementsDialogManager::ResolveAutoPadTheme()
{
	if (!TVPPadThemeAuto || !Application) return;

	tjs_string style = Application->GetJoypadStyle(0);
	if (style.empty()) {
		const std::vector<tjs_string> &tags = Application->GetPlatformTags();
		for (const tjs_string &t : tags) {
			if (t == TJS_W("switch")) { style = TJS_W("switch"); break; }
			if (t == TJS_W("ps5"))    { style = TJS_W("ps");     break; }
		}
	}
	if (style.empty()) style = TJS_W("xbox");

	std::string utf8;
	TVPUtf16ToUtf8(utf8, style);
	auto t = cycfi::elements::parse_pad_theme(utf8);
	if (t != cycfi::elements::pad_theme::none)
		cycfi::elements::set_pad_theme(t);
}

void tTVPElementsDialogManager::SetPartialRedraw(bool enable)
{
	_impl->partial_redraw = enable;
}

bool tTVPElementsDialogManager::GetPartialRedraw() const
{
	return _impl->partial_redraw;
}

tjs_uint64 tTVPElementsDialogManager::GetRenderCount() const
{
	return _impl->raster_count;
}

void tTVPElementsDialogManager::GetRenderStats(tTVPElementsRenderStats& out) const
{
	out = _impl->stats;
}

void tTVPElementsDialogManager::ResetRenderStats()
{
	_impl->stats = tTVPElementsRenderStats{};
}

void tTVPElementsDialogManager::InvalidateOverlays()
{
	for (auto& up : _impl->instances) {
		if (up->session) up->session->invalidate();
	}
}

void tTVPElementsDialogManager::EnsureRuntimeInitialized()
{
	// ELEMENTS_FILE_IO_SUPPORT=OFF でビルドしているため elements 側 default の
	// null_resource_loader が選ばれている。 最初に Storages-backed loader を
	// install する (idempotent)。 順序は install → ThorVG init → font register。
	TVPInstallElementsResourceLoader();
	// 矩形テキスト (text_area) の折返し/禁則/count を本体と同じ
	// glyphware::layoutBlock に寄せる。
	TVPInstallElementsBlockTextBackend();

	elements_modal::init("", /*load_default_fonts=*/false);

	static bool s_fonts_loaded = false;
	if (!s_fonts_loaded) {
#ifdef __WINVER__
		// WINVER: フォントは exe 埋め込み (resources.rc の "BINARY" 型)。 SDL 版の
		// ResourcePath (resource:// / file://./resource/) は WINVER 埋め込みリソースの
		// 代替なので、 WINVER では Win32 リソース API から直接列挙・登録する。
		TVPRegisterElementsFontsFromWinResources();
		TVPApplyRegisteredFontsToElementsTheme();
		s_fonts_loaded = true;
#else
		if (Application) {
			TVPRegisterElementsFontsFromStorageDir(ttstr(Application->ResourcePath().c_str()));
			TVPApplyRegisteredFontsToElementsTheme();
			s_fonts_loaded = true;
		}
#endif
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
		if (_impl->hosts.empty()) {
			TVPAddImportantLog(TJS_W("ElementsDialog: no DrawDevice registered"));
			return nullptr;
		}
		// 提示中のデバイス (直近 PaintOverlay 呼出元) を優先する。 これにより
		// GL デモ等で drawDevice が OGLDrawDevice に切り替わっていても、その上に
		// パネルが出る。 未確定 / レンダラ無しのときのみ map 先頭へフォールバック。
		iTVPDrawDevice* main_device =
			TVPMainWindow ? TVPMainWindow->GetDrawDevice() : nullptr;
		if (_impl->active_device && _impl->FindRenderer(_impl->active_device)) {
			host = _impl->active_device;
		} else if (main_device && _impl->FindRenderer(main_device)) {
			// 複数ウィンドウ時に map 先頭 (= サブウィンドウの device かもしれない)
			// を拾わないよう、メインウィンドウの device を優先する。
			host = main_device;
		} else {
			host = _impl->hosts.begin()->first;
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

	// テーマ自動選択時は、 今つながっているパッドで決め直してから組む
	// (途中でコントローラを替えても次に開く画面から絵が追従する)。
	ResolveAutoPadTheme();

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
	inst->ever_active = true;

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
	// session->close() 経由で exit 演出 ("on":"exit" の animate) と協調する。
	// 演出が無ければ即 finished() → 次フレーム teardown (従来と同じ)。
	_impl->RequestClose(*top);
	TVPAddLog(TJS_W("ElementsDialog: close requested (topmost)"));
}

void tTVPElementsDialogManager::Close(iTVPDialogEventHandler* handler)
{
	Impl::Instance* inst = _impl->FindByHandler(handler);
	if (!inst || !inst->active) return;
	_impl->RequestClose(*inst);
	TVPAddLog(TJS_W("ElementsDialog: close requested (by handler)"));
}

void tTVPElementsDialogManager::DetachHandler(iTVPDialogEventHandler* handler)
{
	// handler (TJS native インスタンス) の破棄時に必ず呼ぶ。 モーダル終了後の
	// teardown は次フレーム PaintOverlay まで遅延するため、 その間に handler が
	// 解放されると TeardownInstance の handler->OnClosed() が解放済みポインタへの
	// 仮想呼び出しになる (Release で AV、 ランチャー等の「ブロッキング表示 →
	// 復帰後すぐ Dialog オブジェクト解放」フローで顕在化)。 ここで参照を切って
	// おけばコールバックは発火せず teardown だけが行われる。
	if (!handler) return;
	bool any = false;
	for (auto& up : _impl->instances) {
		Impl::Instance* inst = up.get();
		if (inst->handler != handler) continue;
		inst->handler = nullptr;
		inst->active = false;
		inst->close_requested = true;   // 次フレーム teardown (再入安全)
		any = true;
	}
	_impl->pending_results.erase(handler);
	if (any) TVPAddLog(TJS_W("ElementsDialog: handler detached"));
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

bool tTVPElementsDialogManager::SetVar(iTVPDialogEventHandler* handler,
                                       const ttstr& name, const ttstr& value)
{
	Impl::Instance* inst = _impl->FindByHandler(handler);
	if (!inst || !inst->active || !inst->session) return false;
	inst->session->set_var(TtstrToUtf8(name), TtstrToUtf8(value));
	return true;
}

void tTVPElementsDialogManager::SetVirtualKeyboardMode(const ttstr& mode)
{
	const std::string m = TtstrToUtf8(mode);
	if (m == "always")     _impl->vk_mode = Impl::VKMode::Always;
	else if (m == "never") _impl->vk_mode = Impl::VKMode::Never;
	else                   _impl->vk_mode = Impl::VKMode::Auto;
	// "never" にしたら表示中のものは畳む
	if (_impl->vk_mode == Impl::VKMode::Never) _impl->CloseVirtualKeyboard();
}

ttstr tTVPElementsDialogManager::GetVirtualKeyboardMode() const
{
	switch (_impl->vk_mode) {
	case Impl::VKMode::Always: return ttstr(TJS_W("always"));
	case Impl::VKMode::Never:  return ttstr(TJS_W("never"));
	default:                   return ttstr(TJS_W("auto"));
	}
}

bool tTVPElementsDialogManager::HasPhysicalKeyboard() const
{
	return HostHasPhysicalKeyboard();
}

void tTVPElementsDialogManager::SetLanguage(const ttstr& lang)
{
	_impl->language = TtstrToUtf8(lang);
	// 表示中の全インスタンスへ即時適用。 flow (navigator) 側にも覚えさせて、
	// 画面遷移で作り直された先でも言語が引き継がれるようにする。
	for (auto& up : _impl->instances) {
		if (!up->session) continue;
		up->session->set_language(_impl->language);
		up->flow_lang = _impl->language;
		if (up->nav) up->nav->set_language(_impl->language);
	}
}

ttstr tTVPElementsDialogManager::GetLanguage() const
{
	return Utf8ToTtstr(_impl->language);
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
	// window 破棄経路でも呼ばれるので、 記録済み入力 window は失効させる
	// (cursor-warp のダングリング防止)。
	_impl->input_window = nullptr;
	_impl->warp_expect_active = false;
	if (_impl->instances.empty()) return;
	_impl->TeardownAll();
	TVPAddLog(TJS_W("ElementsDialog: force-closed (all)"));
}

//---------------------------------------------------------------------------
// 描画アダプタ提供口 (host) 登録
//---------------------------------------------------------------------------
void tTVPElementsDialogManager::RegisterDialogHost(
	iTVPDrawDevice* device, iTVPDialogRendererHost* host)
{
	if (!device || !host) return;
	_impl->hosts[device] = host;
}

void tTVPElementsDialogManager::UnregisterDialogHost(iTVPDrawDevice* device)
{
	// この device をホストとするインスタンスは host/renderer が消える前に teardown。
	// (renderer 破棄前に ReleaseLayer 相当を済ませる)
	std::vector<Impl::Instance*> doomed;
	for (auto& inst : _impl->instances) {
		if (inst->host_device == device) doomed.push_back(inst.get());
	}
	for (auto* inst : doomed) _impl->TeardownInstance(inst);

	_impl->hosts.erase(device);

	// 提示中デバイスが外れたら既定ホストの記録もクリア (次の PaintOverlay で
	// 現行デバイスへ更新される)。 GL 離脱時の OGLDrawDevice 破棄などで発生。
	if (_impl->active_device == device) _impl->active_device = nullptr;
}

//---------------------------------------------------------------------------
// tp_stub 公開の登録 API (プラグイン / 差し替え DrawDevice 向け)。 engine 内蔵
// DrawDevice は manager を直接呼ぶが、プラグインは manager singleton を触れないので
// この free 関数経由で host を登録する。 実体は singleton への委譲。
//---------------------------------------------------------------------------
void TVPRegisterDialogHost(iTVPDrawDevice* device, iTVPDialogRendererHost* host)
{
	tTVPElementsDialogManager::Instance().RegisterDialogHost(device, host);
}

void TVPUnregisterDialogHost(iTVPDrawDevice* device)
{
	tTVPElementsDialogManager::Instance().UnregisterDialogHost(device);
}

//---------------------------------------------------------------------------
// PaintOverlay (DrawDevice::Show() 終端から)
//---------------------------------------------------------------------------
void tTVPElementsDialogManager::PaintOverlay(iTVPDrawDevice* device)
{
	// renderStats: PaintOverlay 全体の所要時間 (early return 含む) と提示
	// フレーム数。 複数 DrawDevice 登録時はデバイス毎に 1 カウントされる。
	_impl->stats.frames++;
	struct TotalGuard {
		tjs_uint64& acc;
		std::chrono::steady_clock::time_point t0 =
			std::chrono::steady_clock::now();
		~TotalGuard() { acc += ElapsedUs(t0); }
	} total_guard{ _impl->stats.totalUs };

	// 提示デバイスが切り替わったら (GL デモの drawDevice 差し替え等)、既存の
	// パネルを現在提示中のデバイスへ移設する。 パネルは create() 内で GL 有効化
	// 直後に表示されることがあり、その時点では旧デバイス (menu を描いた SDL 等)
	// がまだ提示中なので旧デバイスにホストされてしまう。 提示デバイスが実際に
	// 切り替わったこのタイミングで host を追従させ、旧レンダラのレイヤは解放する。
	// element ツリーは ThorVG の CPU ラスタ出力を各レンダラへアップロードする
	// 方式でデバイス非依存なので、host 付け替え + 旧レイヤ解放だけで移設できる。
	//
	// ★ただし移設していいのは「同じ (メイン) ウィンドウ上でデバイスが差し替わ
	//   った」ときだけ。 Window を 2 枚以上開くと各ウィンドウが毎フレーム別々の
	//   DrawDevice で PaintOverlay を呼ぶため、無条件に追従すると overlay が
	//   ウィンドウ間を往復し、サブウィンドウを閉じた瞬間に (その device の
	//   UnregisterDialogHost で) パネルごと teardown されてしまう。
	//   overlay ダイアログはメインウィンドウに出す前提なので、メインウィンドウ
	//   以外の device が提示していても host は動かさない。
	iTVPDrawDevice* main_device = TVPMainWindow ? TVPMainWindow->GetDrawDevice() : nullptr;
	const bool device_is_main = (main_device == nullptr || device == main_device);
	if (_impl->active_device != device && device_is_main) {
		for (auto& up : _impl->instances) {
			Impl::Instance* inst = up.get();
			if (inst->host_device == device) continue;
			if (auto* oldR = _impl->FindRenderer(inst->host_device))
				oldR->ReleaseLayer(inst->LayerKey());
			inst->host_device = device;
			// 旧レンダラのテクスチャは解放済み。 新デバイスでは必ず
			// 再ラスタライズさせる (cached present の対象から外す)。
			inst->cache_valid = false;
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
			// (exit 演出がある画面は、 その再生完了後にここへ来る。)
			if (inst->session->finished()) {
				if (inst->close_after_exit) {
					// Dialog.close() 等の外部 close: transitions を解決せず終了。
					_impl->FinishSingle(*inst);
				} else if (inst->nav) {
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

	// 4) cursor-warp ナビ: キー/パッド由来のフォーカス移動があれば、 実マウス
	//    カーソルをフォーカス先の hot point へ warp してカーソルを一時非表示に
	//    する ("input":{"cursor_warp":true} の画面のみ session が通知してくる)。
	//    render 済みのこのタイミングなら present 変換 (present_scale/off) が
	//    当フレームの値で確定している。
	if (_impl->input_window) {
		Impl::Instance* f = _impl->TopmostKeyboardFocus();
		if (f && f->session && f->host_device == device) {
			// 入力フォーカスの持ち主が変わったら (上のダイアログが閉じて
			// 戻ってきた等)、 こちらのフォーカスは動いていないので
			// take_key_focus_move は発火しない。 カーソルが閉じた
			// ダイアログの位置に取り残されて hover 表示だけ別要素に
			// 付くため、 セッションへ合わせ直しを依頼する。
			if (_impl->last_focus_owner != static_cast<void*>(f)) {
				_impl->last_focus_owner = static_cast<void*>(f);
				f->session->notify_input_focus_gained();
			}
			float sx = 0.0f, sy = 0.0f;
			if (f->session->take_key_focus_move(sx, sy)) {
				// session 座標 → 描画領域基準 (ToSurfaceX/Y の逆変換)。
				// SetCursorPos は「描画矩形内の座標」を受ける契約
				// (実装側で DestRect 原点を足し戻す) ため、 window client へ
				// 直したあと DestRect 原点を引いて渡す。
				tjs_int lx = static_cast<tjs_int>(
					sx * f->present_scale + f->present_off_x + 0.5f)
					- f->dest_offset_x;
				tjs_int ly = static_cast<tjs_int>(
					sy * f->present_scale + f->present_off_y + 0.5f)
					- f->dest_offset_y;
				_impl->warp_expect_active = true;
				_impl->warp_expect_x = lx;
				_impl->warp_expect_y = ly;
				_impl->input_window->SetCursorPos(lx, ly);
				// パッド/キー操作モード: カーソルは隠す。 warp が生む合成
				// mouse move による再表示は ForwardMouseMove 側で抑止する。
				static_cast<tTJSNI_Window*>(
					static_cast<tTJSNI_BaseWindow*>(_impl->input_window))
					->SetMouseCursorState(mcsTempHidden);
			}
		} else if (!f) {
			_impl->last_focus_owner = nullptr;
		}
	}
}

//---------------------------------------------------------------------------
// 入力フォワード
//---------------------------------------------------------------------------
namespace {

namespace ce = cycfi::elements;

// krkrz ネイティブ入力型 → cycfi 中立入力型への変換。 マネージャは SDL/WIN 両
// build 共通で Windows VK / tTVPMouseButton / TVP_SS_* を受け取り、 overlay_session
// の host 非依存 API (mouse_button::what / key_code / pad_button + mod_* の OR) へ
// 直接マップする。 SDL や Win32 のネイティブ enum は経由しない。
ce::mouse_button::what MouseButtonToElements(tTVPMouseButton mb)
{
	switch (mb) {
		case mbMiddle: return ce::mouse_button::middle;
		case mbRight:  return ce::mouse_button::right;
		default:       return ce::mouse_button::left;   // mbLeft ほか
	}
}

int FlagsToElementsMods(tjs_uint32 flags)
{
	int mods = 0;
	if (flags & TVP_SS_SHIFT) mods |= ce::mod_shift;
	if (flags & TVP_SS_CTRL)  mods |= ce::mod_control;
	if (flags & TVP_SS_ALT)   mods |= ce::mod_alt;
	return mods;
}

// マウスボタン → VK コード (ホストホットキーのテーブル照合用。 キー / パッド /
// マウスを同じ VK 空間で扱う)。
tjs_uint MouseButtonToVk(tTVPMouseButton mb)
{
	switch (mb) {
		case mbRight:  return VK_RBUTTON;
		case mbMiddle: return VK_MBUTTON;
		case mbX1:     return VK_XBUTTON1;
		case mbX2:     return VK_XBUTTON2;
		default:       return VK_LBUTTON;   // mbLeft ほか
	}
}

//---------------------------------------------------------------------------
// Windows VK code → cycfi 中立入力型 (key_code | pad_button) 振り分け。
//---------------------------------------------------------------------------
struct vk_routing {
	enum class kind { none, key, pad_button };
	kind           k          = kind::none;
	ce::key_code   key        = ce::key_code::unknown;
	int            extra_mods = 0;
	ce::pad_button pad        = ce::pad_button::unknown;
};

vk_routing RouteVk(tjs_uint vk)
{
	using K  = vk_routing::kind;
	using kc = ce::key_code;
	using pb = ce::pad_button;
	auto as_key = [](kc c, int m = 0) { return vk_routing{K::key, c, m, pb::unknown}; };
	auto as_pad = [](pb b)            { return vk_routing{K::pad_button, kc::unknown, 0, b}; };

	switch (vk) {
		case VK_RETURN: return as_key(kc::enter);
		case VK_TAB:    return as_key(kc::tab);
		case VK_ESCAPE: return as_key(kc::escape);
		case VK_BACK:   return as_key(kc::backspace);
		case VK_DELETE: return as_key(kc::_delete);
		case VK_INSERT: return as_key(kc::insert);
		case VK_HOME:   return as_key(kc::home);
		case VK_END:    return as_key(kc::end);
		case VK_PRIOR:  return as_key(kc::page_up);
		case VK_NEXT:   return as_key(kc::page_down);
		case VK_SPACE:  return as_key(kc::space);
		case VK_LEFT:   return as_key(kc::left);
		case VK_UP:     return as_key(kc::up);
		case VK_RIGHT:  return as_key(kc::right);
		case VK_DOWN:   return as_key(kc::down);

		case 0x1C0: return as_pad(pb::a);          // VK_PAD1  (A)
		case 0x1C1: return as_pad(pb::b);          // VK_PAD2  (B)
		case 0x1C2: return as_pad(pb::x);          // VK_PAD3  (X)
		case 0x1C3: return as_pad(pb::y);          // VK_PAD4  (Y)
		case 0x1C4: return as_pad(pb::lb);         // VK_PAD5  (LB)
		case 0x1C5: return as_pad(pb::rb);         // VK_PAD6  (RB)
		case 0x1C8: return as_pad(pb::back);       // VK_PAD9  (Back)
		case 0x1C9: return as_pad(pb::start);      // VK_PAD10 (Start)
		case 0x1CA: return as_pad(pb::l3);         // VK_PAD11 (L3)
		case 0x1CB: return as_pad(pb::r3);         // VK_PAD12 (R3)

		// 位置基準のフェイスボタン (刻印ではなく配置)。 同じ物理ボタンが
		// VK_PAD1..4 も一緒に飛ばしてくるので、 割り当てる側で使い分ける。
		case 0x1D4: return as_pad(pb::face_south); // VK_PAD_FACE_SOUTH (下)
		case 0x1D5: return as_pad(pb::face_east);  // VK_PAD_FACE_EAST  (右)
		case 0x1D6: return as_pad(pb::face_west);  // VK_PAD_FACE_WEST  (左)
		case 0x1D7: return as_pad(pb::face_north); // VK_PAD_FACE_NORTH (上)

		case 0x1B5: case 0x1CC: case 0x1D0:
			return as_pad(pb::dpad_left);
		case 0x1B6: case 0x1CD: case 0x1D1:
			return as_pad(pb::dpad_up);
		case 0x1B7: case 0x1CE: case 0x1D2:
			return as_pad(pb::dpad_right);
		case 0x1B8: case 0x1CF: case 0x1D3:
			return as_pad(pb::dpad_down);

		case 0x1B9: return as_pad(pb::a);

		default:
			// 数字 (VK_0..9 = 0x30..0x39) / 英字 (VK_A..Z = 0x41..0x5A) は
			// cycfi key_code が大文字 ASCII 準拠なのでそのまま通す。
			if (vk >= '0' && vk <= '9') return as_key(static_cast<kc>(vk));
			if (vk >= 'A' && vk <= 'Z') return as_key(static_cast<kc>(vk));
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
	// ホストホットキー: 登録ボタンはダイアログへ渡さず通常経路へ (非消費)
	if (_impl->HostHotkeyBypass(MouseButtonToVk(mb), flags, /*isUp=*/false))
		return false;
	for (auto it = _impl->instances.rbegin(); it != _impl->instances.rend(); ++it) {
		Impl::Instance* inst = it->get();
		if (!inst->active || !inst->session) continue;
		float sx = Impl::ToSurfaceX(*inst, x);
		float sy = Impl::ToSurfaceY(*inst, y);
		if (inst->modal || Impl::RectContains(*inst, sx, sy)) {
			inst->session->on_mouse_down(sx, sy,
				MouseButtonToElements(mb), FlagsToElementsMods(flags));
			return true;
		}
	}
	return false;
}

bool tTVPElementsDialogManager::ForwardMouseUp(
	tjs_int x, tjs_int y, tTVPMouseButton mb, tjs_uint32 flags)
{
	// ホストホットキー: up は vk のみ一致でバイパス (down と対で漏らさない)
	if (_impl->HostHotkeyBypass(MouseButtonToVk(mb), flags, /*isUp=*/true))
		return false;
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
				MouseButtonToElements(mb), FlagsToElementsMods(flags));
			return true;
		}
	}
	return false;
}

bool tTVPElementsDialogManager::ForwardMouseMove(
	tjs_int x, tjs_int y, tjs_uint32 flags)
{
	// cursor-warp ガード: 自分の SetCursorPos が生んだ合成 move は「実マウス
	// が動いた」と見なさない。 window 層が move で mcsTempHidden→visible に
	// 復帰させるので、 一致 move では再 hide して非表示を維持する (move 自体
	// は session へ流す = hover/hilite がフォーカス先に付く)。 warp 直後は
	// 同座標の move が複数届きうる (SetCursorPos の折返し + OS motion) ため、
	// 不一致 move が来るまで期待座標は保持する。
	if (_impl->warp_expect_active) {
		if (std::abs(x - _impl->warp_expect_x) <= 2 &&
		    std::abs(y - _impl->warp_expect_y) <= 2) {
			if (_impl->input_window) {
				static_cast<tTJSNI_Window*>(
					static_cast<tTJSNI_BaseWindow*>(_impl->input_window))
					->SetMouseCursorState(mcsTempHidden);
			}
		} else {
			_impl->warp_expect_active = false;   // 実マウス: 通常挙動へ復帰
		}
	}

	bool consumed = false;
	Impl::Instance* hit = nullptr;
	// 最前面から: modal なら独占、 非モーダルはヒット判定。
	for (auto it = _impl->instances.rbegin(); it != _impl->instances.rend(); ++it) {
		Impl::Instance* inst = it->get();
		if (!inst->active || !inst->session) continue;
		float sx = Impl::ToSurfaceX(*inst, x);
		float sy = Impl::ToSurfaceY(*inst, y);
		if (!hit && (inst->modal || Impl::RectContains(*inst, sx, sy))) {
			inst->session->on_mouse_move(sx, sy, FlagsToElementsMods(flags));
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

void tTVPElementsDialogManager::NoteInputWindow(iTVPWindow* window)
{
	_impl->input_window = window;
}

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
	// ホストホットキー: 登録キー (VK_PAD* 含む) はダイアログへ渡さず通常経路へ。
	// モーダル表示中とテキスト入力 focus 中 (duringTextInput=false のもの) は
	// バイパスしない — 判定は HostHotkeyBypass 側。
	if (_impl->HostHotkeyBypass(key, shift, /*isUp=*/false)) return false;
	Impl::Instance* f = _impl->TopmostKeyboardFocus();
	if (!f || !f->session) return false;
	auto r = RouteVk(key);
	bool handled = false;
	switch (r.k) {
		case vk_routing::kind::key: {
			int mods = FlagsToElementsMods(shift) | r.extra_mods;
			handled = f->session->on_key_down(r.key, mods);
			break;
		}
		case vk_routing::kind::pad_button:
			handled = f->session->on_pad_button(r.pad, /*down=*/true);
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
	// ホストホットキー: up は vk のみ一致でバイパス (修飾キー変化で漏らさない)
	if (_impl->HostHotkeyBypass(key, shift, /*isUp=*/true)) return false;
	Impl::Instance* f = _impl->TopmostKeyboardFocus();
	if (!f || !f->session) return false;
	auto r = RouteVk(key);
	bool handled = false;
	switch (r.k) {
		case vk_routing::kind::key: {
			int mods = FlagsToElementsMods(shift) | r.extra_mods;
			handled = f->session->on_key_up(r.key, mods);
			break;
		}
		case vk_routing::kind::pad_button:
			handled = f->session->on_pad_button(r.pad, /*down=*/false);
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
	if (!f || !f->session) {
		_impl->pending_high_surrogate = 0;   // フォーカス喪失時は保持もクリア
		return false;
	}

	// key は UTF-16 code unit (tjs_char = 16bit)。 サロゲートペアを合成して
	// 1 コードポイント (cp) にしてから UTF-8 化する。 WINVER の WM_CHAR は BMP 外を
	// high/low 2 回に分けて配信する (SDL は ForwardText で完全 UTF-8 が来るので無関係)。
	tjs_uint32 unit = static_cast<tjs_uint16>(key);
	tjs_uint32 cp;
	if (unit >= 0xD800 && unit <= 0xDBFF) {
		// high surrogate: 保持して low を待つ (まだ出力しない、 消費扱い)。
		_impl->pending_high_surrogate = static_cast<tjs_uint16>(unit);
		return true;
	} else if (unit >= 0xDC00 && unit <= 0xDFFF) {
		// low surrogate: 直前の high と合成。 high が無ければ孤立 low なので無視。
		if (_impl->pending_high_surrogate) {
			cp = 0x10000u
			   + ((static_cast<tjs_uint32>(_impl->pending_high_surrogate) - 0xD800u) << 10)
			   + (unit - 0xDC00u);
			_impl->pending_high_surrogate = 0;
		} else {
			return true;
		}
	} else {
		// 通常の BMP 文字。 保持中の high があれば (不正並び) 破棄する。
		_impl->pending_high_surrogate = 0;
		cp = unit;
	}

	// cp → UTF-8 (最大 4 byte)。
	char buf[8] = {0};
	if (cp < 0x80) {
		buf[0] = (char)cp;
	} else if (cp < 0x800) {
		buf[0] = (char)(0xC0 | (cp >> 6));
		buf[1] = (char)(0x80 | (cp & 0x3F));
	} else if (cp < 0x10000) {
		buf[0] = (char)(0xE0 | (cp >> 12));
		buf[1] = (char)(0x80 | ((cp >> 6) & 0x3F));
		buf[2] = (char)(0x80 | (cp & 0x3F));
	} else {
		buf[0] = (char)(0xF0 | (cp >> 18));
		buf[1] = (char)(0x80 | ((cp >> 12) & 0x3F));
		buf[2] = (char)(0x80 | ((cp >> 6) & 0x3F));
		buf[3] = (char)(0x80 | (cp & 0x3F));
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
	if (_impl->hosts.empty()) {
		TVPAddImportantLog(TJS_W("ElementsDialog: no registered DrawDevice; cannot show test dialog"));
		return;
	}
	ShowTestDialog(_impl->hosts.begin()->first);
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
