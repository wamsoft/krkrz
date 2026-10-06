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
#include "ElementsInputMap.h"   // VK / マウス → cycfi 中立入力型 (パネルと共用)
#include "ElementsSessionBuild.h" // 画面 JSON → overlay_session の組み立て (パネルと共用)
#include "ElementsLayerPanel.h"    // ホストのレイヤに描くパネル (言語 / 再描画の fan-out)
#include "DebugIntf.h"
#include "MsgIntf.h"
#include "CharacterSet.h"   // TVPUtf8ToUtf16
#include "SysInitIntf.h"    // tTVPAtExit (終了時の ThorVG 後始末)
#include "StorageIntf.h"    // TVPReadStream
#include "Application.h"    // Application, MainWindowForm() / ResourcePath()
#include "WindowIntf.h"     // iTVPWindow (cursor-warp: 仮想カーソル位置) / mcs enum
#include "WindowImpl.h"     // tTJSNI_Window::SetMouseCursorState (cursor-warp hide)
#ifndef __WINVER__
#include "WindowForm.h"     // TTVPWindowForm::NativeWindowHandle() (SDL/generic host)
#endif
#include "ScriptMgnIntf.h"  // TVPGetScriptDispatch (読み上げ: onGameA11yAction)
#include "EventIntf.h"      // TVPAdd/RemoveContinuousEventHook (paint 中 OnAction の遅延配送)
#include "tjsDictionary.h"  // TJSCreateDictionaryObject (OnDrag の payload)
#include "StoragesResourceLoader.h"   // TVPInstallElementsResourceLoader / Fonts
#include "GraphicsLoaderIntf.h"       // TVPLoadGraphic (universal rule 画像)
#include "LayerBitmapIntf.h"          // tTVPBaseBitmap (rule 画像の 8bpp 展開)
#include "TickCount.h"                // TVPGetRoughTickCount32 (遷移エフェクト計時)

#ifndef _WIN32
#include "VirtualKey.h"
#include "LayerIntf.h"     // 読み上げ: Layer の自動 (a11yLayers)
#endif

#include <elements/element/pad_icon.hpp>   // set_pad_theme / parse_pad_theme
#include <elements_modal/modal.h>
#include <elements_modal/navigator.h>   // フロー駆動 (画面遷移スタック)
#include <elements_modal/effects.h>     // 画面切替エフェクト (fade / universal ブレンド)

#include <chrono>   // renderStats 区間計測 (steady_clock)
#include <elements/base_view.hpp>        // cycfi 中立入力型 (mouse_button / key_code / mod_*)
#include <elements/support/theme.hpp>    // get_theme / set_theme (フォーカスリング設定)
#include <elements/element/gamepad.hpp>  // cycfi 中立入力型 (pad_button)

// テキスト入力の開始/停止 (ソフトキーボード制御) は host 依存。 WINVER は Win32 の
// WM_CHAR 経由 (ForwardText) で扱うため SDL は不要。 SDL host のみ SDL3 を引く。
#ifndef __WINVER__
#include <SDL3/SDL.h>        // SDL host: SDL_StartTextInput / SDL_HasScreenKeyboardSupport 等
#endif

#ifdef KRKRZ_HAS_A11Y
#include <elements/a11y/accesskit_host.hpp>   // 読み上げ: OS のアクセシビリティ API へ出す
#endif
#include <elements/support/a11y.hpp>          // 読み上げツリー (snapshot / speech_lines)
#include <mutex>             // 読み上げ: AT 操作のキュー
#include <thread>            // 読み上げ: AT 操作がメインスレッドで来たかの判定
#include <climits>           // INT_MIN (読み上げ: 重なり順の未送信印)
#include <functional>        // 読み上げ: ゲーム slot の木の組み立て
#include <unordered_map>     // 読み上げ: ゲーム slot のノード表

#include <algorithm>         // std::remove_if (ホストホットキー解除)
#include <cstdlib>           // std::strtol
#include <cstring>           // std::memcpy (部分更新時の last_frame 複製)
#include <deque>             // pending_actions (paint 中 OnAction の遅延配送)
#include <map>
#include <set>
#include <memory>
#include <string>
#include <type_traits>
#include <vector>

// ナビ診断ログ (-navlog)。 実体はファイル後半 (TVPGetCommandLine を使うため
// SysInitIntf.h の取り込み位置に合わせてある) なので、 前方宣言だけ置く。
static bool NavLogEnabled();
static void NavLog(const std::string &msg);

//---------------------------------------------------------------------------
// 内部: krkrz ttstr ⇔ utf-8、 value_t → tTJSVariant 変換 + handler ブリッジ
//---------------------------------------------------------------------------
namespace {

using namespace tvp_elements;

// renderStats 用: t0 からの経過 microsecond
tjs_uint64 ElapsedUs(std::chrono::steady_clock::time_point t0)
{
	return static_cast<tjs_uint64>(
		std::chrono::duration_cast<std::chrono::microseconds>(
			std::chrono::steady_clock::now() - t0).count());
}

// 文字列 / 値の変換 (Utf8ToTtstr / TtstrToUtf8 / ValueToVariant /
// DragEventToDict) と event_callback の橋渡しは ElementsSessionBuild.h へ
// 移した (ホストのレイヤに描くパネルと共用するため)。 名前はそのまま使える。

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

// IME の変換 / 変換候補ウィンドウを入力欄へ寄せる (クライアント座標 px)。
// WINVER は Window → TTVPWindowForm → ImeControl と降りて Imm を直接叩く。
inline void HostSetTextInputArea(iTVPWindow* win,
                                 tjs_int x, tjs_int y, tjs_int w, tjs_int h,
                                 tjs_int cursor)
{
	if (!win) return;
	static_cast<tTJSNI_Window*>(win)->SetOverlayTextInputArea(x, y, w, h, cursor);
}

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

// IME の変換 / 変換候補ウィンドウを入力欄へ寄せる。 SDL3 は
// SDL_SetTextInputArea が各バックエンドの実装 (Windows なら
// ImmSetCompositionWindow / ImmSetCandidateWindow) を呼んでくれる。
// 渡す矩形は「ウィンドウ座標」なので、 クライアント px から換算する
// (HiDPI で SDL_GetWindowSize と SDL_GetWindowSizeInPixels がずれる環境向け)。
inline void HostSetTextInputArea(iTVPWindow* /*win*/,
                                 tjs_int x, tjs_int y, tjs_int w, tjs_int h,
                                 tjs_int cursor)
{
	SDL_Window* sw = HostMainWindow();
	if (!sw) return;
	int lw = 0, lh = 0, pw = 0, ph = 0;
	SDL_GetWindowSize(sw, &lw, &lh);
	SDL_GetWindowSizeInPixels(sw, &pw, &ph);
	const float sx = (pw > 0) ? (float)lw / (float)pw : 1.0f;
	const float sy = (ph > 0) ? (float)lh / (float)ph : 1.0f;
	SDL_Rect r;
	r.x = (int)(x * sx);
	r.y = (int)(y * sy);
	r.w = (int)(w * sx);
	r.h = (int)(h * sy);
	SDL_SetTextInputArea(sw, &r, (int)(cursor * sx));
}

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
inline bool HostScreenKeyboardIsFloating() { return false; }
inline const char* HostGetEnv(const char* name) { return getenv(name); }

#else

inline bool HostHasPhysicalKeyboard()
{
	// Steam Deck (gamescope) では Xwayland が常にコアキーボードを提供するため
	// SDL_HasKeyboard() では物理キーボードを検出できない (無条件 true)。 SDL 自身も
	// SteamDeck=1 のときは AutoShowingScreenKeyboard を無条件 true にしており、
	// StartTextInput が即 Steam OSK を出す。 Deck は常に「物理キーボード無し」
	// として focus 駆動に載せる。
	if (SDL_GetHintBoolean("SteamDeck", false)) return false;
	return SDL_HasKeyboard();
}

// OS のスクリーンキーボードが focus 駆動でそのまま使える「非ブロッキングの
// 浮動オーバレイ」か。 Steam Deck の OSK (steam://open/keyboard) がこれで、
// StartTextInput で出て StopTextInput で閉じる。 NX の swkbd / PS5 の
// SceImeDialog のようなブロッキングアプレットの環境は false = 内蔵仮想
// キーボードを使う。
inline bool HostScreenKeyboardIsFloating()
{
	return SDL_GetHintBoolean("SteamDeck", false);
}

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

	// UI の author 基準面サイズ (ElementsDialog.baseSize)。 提示拡縮率 (fit) の分母。
	// 0 (既定) はゲームの基準面 (primary layer サイズ) を使う。 UI をゲーム
	// 画面と別の解像度で author しているタイトル (ゲーム画面 640x400 に対し
	// UI は 1920x1080 等) では primary layer 基準だと fit が過大になり、 部分
	// パネルが拡大されてしまうため、 こちらで author 基準を明示する。
	// primary layer に依存しなくなるので、 ゲーム側が primary layer サイズを
	// 変える演出 (低解像度機種エミュレーション等) にも巻き込まれない。
	int base_size_w = 0;
	int base_size_h = 0;

	// 再ラスタライズ抑止 (ElementsDialog.renderCache)。 true (既定) なら変化の無い
	// フレームは overlay_session の再ラスタライズ + アップロードを省略し、
	// レンダラ保持の前回テクスチャをそのまま提示する。
	bool render_cache = true;

	// 部分再描画 (ElementsDialog.partialRedraw)。 true (既定) なら、 ダーティが矩形で
	// 特定できる変化 (キャレット点滅等) はその矩形だけを再ラスタ + 部分転送
	// する。 renderCache 有効時のみ機能 (staging に前回フレームが残る前提)。
	bool partial_redraw = true;

	// 実際にラスタライズした累計回数 (ElementsDialog.renderCount)。 アイドル時に
	// 増えないことの確認・負荷比較用。
	tjs_uint64 raster_count = 0;

	// 区間計測 (ElementsDialog.renderStats)。 累積値、 ResetRenderStats で 0 クリア。
	// steady_clock 呼出はフレームあたり数回なのでオーバーヘッドは無視できる。
	tTVPElementsRenderStats stats;

	//! @brief 読み上げ (スクリーンリーダー) 用のインスタンスごとの状態。
	//!        session の sink から weak で見る (画面の張り直し / 破棄の順序に
	//!        依存しないように)。
	struct A11ySlot
	{
		int slot = 0;
#ifdef KRKRZ_HAS_A11Y
		std::shared_ptr<cycfi::elements::a11y::sink> os;   //!< OS (AccessKit) の slot
#endif
		cycfi::elements::a11y::snapshot last;              //!< 読み上げログの差分元
		bool has_last = false;
		// OS 側へ最後に伝えた値 (変わったときだけ送る)
		int z = -1;
		bool modal = false;
#ifdef KRKRZ_HAS_A11Y
		cycfi::elements::a11y::transform xf;   // accesskit_host.hpp の型 (A11Y 無効時は不要)
#endif
	};

	//! @brief 1 つの overlay UI インスタンス。 z-order = instances 内の並び順
	//!        (先頭 = 最背面、 末尾 = 最前面)。
	struct Instance
	{
		std::shared_ptr<A11ySlot> a11y;   // 読み上げ (session より先 = 後に壊れる)
		std::unique_ptr<elements_modal::overlay_session> session;

		// このインスタンスに紐付く event handler (TJS ElementsDialog 等)。 layer キーと
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
		// session->update() が呼び出しスタック上にあるか。 update 中の OnAction
		// からネストモーダル (System.inputString 等) の pump が回ると PaintOverlay
		// が再入するため、 このインスタンスの再 update と teardown を抑止する
		// (teardown は close_requested を持ち越して update 完了後のフレームで行う)。
		bool in_update = false;

		// close_on_click / Esc で finish したときの action id (Close() 等の
		// 外部要因 close は空のまま)。 teardown 時の OnClosed に渡す。
		ttstr close_action;

		// このダイアログ表示後に「新規押下 (非リピート)」を観測した VK の集合。
		// 表示前から押しっぱなしのキー/パッドボタンはリピート (TVP_SS_REPEAT)
		// しか届かないため、 集合に無い VK のリピートは配送しない = 一度離す
		// までダイアログに効かない (長押しスキップ中に開いたソフトキーボードへ
		// 決定ボタンが即入力される誤爆の防止)。
		std::set<tjs_uint> armed_vks;

		// キー捕捉中か (SetKeyCapture)。 フォーカス保持中のキー押下と
		// 左以外のマウスボタン押下を handler->OnKeyCapture へ回す。
		bool key_capture = false;

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

		// ElementsDialog.close() 等の外部 close 要求で exit 演出を再生中。 finished() 後は
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
	// キー/パッドでフォーカスが動いたとき、 この window の **仮想カーソル位置**
	// をフォーカス先へ置き (SetVirtualCursorPos)、 実カーソルは mcsTempHidden で
	// 隠す。 **実 OS カーソルは動かさない** (doc/VirtualCursor.md)。
	iTVPWindow* input_window = nullptr;
	// 入力フォーカス (キーボード/パッドの届き先) の直近の持ち主。 上に載って
	// いたダイアログが閉じて下の画面へ戻ったときに、 フォーカス表示と実カーソル
	// を合わせ直すために使う。
	void*   last_focus_owner = nullptr;

	// -navlog 用: warp 要求の通し番号 (ログの追跡用)。
	tjs_int warp_seq = 0;

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

	// 指定 device (= そのウィンドウ) にアクティブなインスタンスが載っているか。
	// 入力インターセプトのゲートに使う (ウィンドウ跨ぎの横取り防止)。
	bool AnyActiveOn(const iTVPDrawDevice* device) const
	{
		if (!device) return false;
		for (auto const& inst : instances)
			if (inst->active && inst->host_device == device) return true;
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

	// キー捕捉中でキーボードフォーカスを持つインスタンス (無ければ nullptr)。
	Instance* KeyCaptureTarget() const
	{
		Instance* f = TopmostKeyboardFocus();
		if (f && f->key_capture && f->session && f->handler) return f;
		return nullptr;
	}

	// 捕捉したキー押下を handler へ渡す。 押しっぱなしのまま捕捉が終わっても
	// 背面のインスタンスへリピートが漏れないよう、 他のインスタンスの
	// 「新規押下を見た VK」からは外しておく。
	void DeliverKeyCapture(Instance* target, tjs_uint vk, tjs_uint32 shift)
	{
		for (auto& up : instances) {
			if (up.get() != target) up->armed_vks.erase(vk);
		}
		target->handler->OnKeyCapture(vk, shift);
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
			SetImeOpenForFocus(false);
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
			// 仮想キーボードを使うか (auto = 物理キーボードが無く、 かつ OS の
			// 浮動スクリーンキーボードも使えないときだけ。 Steam Deck は
			// HostScreenKeyboardIsFloating() = true なので OS 側 = Steam OSK を使う)
			const bool use_vk = (vk_mode == VKMode::Always) ||
			                    (vk_mode == VKMode::Auto && !HostHasPhysicalKeyboard() &&
			                     !HostScreenKeyboardIsFloating());
			if (!use_vk) {
				// 物理キーボードあり / 浮動 OS キーボードあり / "never":
				// text 入力を有効化するだけ。 物理キーボード環境では SDL の
				// auto 判定によりソフトキーボードは出ず、 Steam Deck では
				// このタイミングで Steam OSK が出る (focus が外れると
				// HostStopTextInput で閉じる)。
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

	// === デスクトップ (WINVER) 用: テキスト欄 focus 中だけ IME を開く ===
	//
	// Elements のオーバレイはレイヤツリーの外にいるので、 フォーカスレイヤ連動
	// (Layer.imeMode → LayerManager → DrawDevice → Window) の経路に乗らない。
	// ウィンドウの既定 IME モードは imClose (英数) なので、 そのままでは
	// 入力欄にキャレットが立っていても半角/全角キーを叩くまで日本語が打てない。
	// テキスト欄が編集フォーカスを持っている間だけ imOpen にし、 外れたら
	// ResetImeMode() でウィンドウの既定 (Window.imeMode) へ戻す。
	//
	// 確定文字は従来どおり WM_CHAR → ForwardText で届く。 未確定文字列の
	// インライン表示と変換候補ウィンドウのキャレット追従は未対応 (候補窓は
	// IME 既定位置に出る)。
	//
	// SDL ホストは SDL_StartTextInput 側 (UpdateFocusDrivenTextInput) が
	// テキスト入力の有効化を受け持つのでここでは何もしない。
	iTVPWindow* ime_open_window = nullptr;   // 開いた対象 (非所有。 nullptr = 未オープン)

	// まだ生きているウィンドウか (破棄済みポインタを掴んだままにしない)。
	// input_window / ime_open_window はどちらも非所有の生ポインタなので、
	// 使う直前にウィンドウリストと突き合わせる。 台数は 1〜2 なので線形で充分。
	static bool IsLiveWindow(iTVPWindow* w)
	{
		if (!w) return false;
		const tjs_int n = TVPGetWindowCount();
		for (tjs_int i = 0; i < n; i++) {
			if (static_cast<iTVPWindow*>(TVPGetWindowListAt(i)) == w) return true;
		}
		return false;
	}

	// IME を開く/戻す対象ウィンドウ。 入力を転送してきたウィンドウを優先し、
	// 無ければメインウィンドウ (スクリプトから focus を当てた直後など)。
	iTVPWindow* ImeTargetWindow() const
	{
		if (IsLiveWindow(input_window)) return input_window;
		return TVPMainWindow;
	}

	void SetImeOpenForFocus(bool open)
	{
#ifdef __WINVER__
		if (open == (ime_open_window != nullptr)) return;
		if (open) {
			iTVPWindow* win = ImeTargetWindow();
			if (!win) return;
			ime_open_window = win;
			// 一時上書き。 握っている間はレイヤ側の imeMode 更新 (KAG の
			// メッセージレイヤのフォーカス移動等) を保留させる。 単に
			// SetImeMode すると、 編集中に別レイヤへフォーカスが移った拍子に
			// imDisable/imClose で上書きされて IME が閉じてしまう。
			static_cast<tTJSNI_Window*>(win)->SetOverlayImeMode(::imOpen);
		} else {
			iTVPWindow* win = ime_open_window;
			ime_open_window = nullptr;
			if (IsLiveWindow(win))
				static_cast<tTJSNI_Window*>(win)->ClearOverlayImeMode();
		}
#else
		(void)open;
#endif
	}

	// PaintOverlay 末尾から毎フレーム呼ぶ。
	void UpdateImeFollowFocus()
	{
#ifdef __WINVER__
		Instance* owner = TopmostKeyboardFocus();
		SetImeOpenForFocus(owner && owner->active && owner->session &&
		                   owner->session->focus_consumes_text());
#endif
	}

	// === IME の変換 / 変換候補ウィンドウをキャレット位置へ寄せる ===
	//
	// 何もしないと候補窓は IME 既定位置 (画面/ウィンドウの左上隅) に出る。
	// 編集中のテキスト要素のキャレット矩形を session から取り、 surface 論理座標
	// → ウィンドウクライアント px へ直してホストへ渡す。 WINVER は
	// ImmSetCompositionWindow / ImmSetCandidateWindow、 SDL は
	// SDL_SetTextInputArea (各バックエンドが同等のことをする)。
	//
	// 値はキャレット点滅や文字入力のたびに動くので、 変化したときだけ送る。
	tjs_int ime_area_x = -1, ime_area_y = -1, ime_area_w = -1, ime_area_h = -1;
	tjs_int ime_area_cursor = -1;
	bool    ime_area_valid = false;

	void UpdateTextInputArea()
	{
		Instance* owner = TopmostKeyboardFocus();
		if (!owner || !owner->active || !owner->session ||
		    !owner->session->focus_consumes_text()) {
			ime_area_valid = false;
			return;
		}
		elements_modal::overlay_session::render_rect caret{}, area{};
		if (!owner->session->focus_text_caret(caret, area)) return;

		// surface 論理座標 → window client px (ToSurfaceX/Y の逆変換)。
		auto toClientX = [&](float sx) {
			return (tjs_int)(sx * owner->present_scale + owner->present_off_x);
		};
		auto toClientY = [&](float sy) {
			return (tjs_int)(sy * owner->present_scale + owner->present_off_y);
		};
		const tjs_int ax = toClientX((float)area.x);
		const tjs_int ay = toClientY((float)area.y);
		const tjs_int aw = (tjs_int)(area.w * owner->present_scale);
		const tjs_int ah = (tjs_int)(area.h * owner->present_scale);
		tjs_int cursor = toClientX((float)caret.x) - ax;
		if (cursor < 0) cursor = 0;
		if (aw > 0 && cursor > aw) cursor = aw;

		if (ime_area_valid && ax == ime_area_x && ay == ime_area_y &&
		    aw == ime_area_w && ah == ime_area_h && cursor == ime_area_cursor)
			return;
		ime_area_x = ax; ime_area_y = ay;
		ime_area_w = aw; ime_area_h = ah;
		ime_area_cursor = cursor;
		ime_area_valid = true;

		HostSetTextInputArea(ImeTargetWindow(), ax, ay, aw, ah, cursor);
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
		DetachA11y(*inst);
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
			DetachA11y(*inst);
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
		SetImeOpenForFocus(false);
		for (auto& [handler, action] : closed) handler->OnClosed(action);
	}

	// === 読み上げ (スクリーンリーダー対応) ===
	//
	// OS への口 (AccessKit) はメインウィンドウに 1 つ。 インスタンスはそれぞれ
	// slot として載り、 重なり順 / モーダル / 位置を PaintOverlay の末尾で
	// 同期する (SyncA11y)。 AT からの操作はどのスレッドからも来るのでキューに
	// 積み、 同じ SyncA11y (メインスレッド) で流す。
#ifdef KRKRZ_HAS_A11Y
	std::unique_ptr<cycfi::elements::a11y::accesskit_host> a11y_host;
#endif
	std::string a11y_mode = "auto";
	std::string a11y_label;
	std::vector<std::string> a11y_log;    // utf-8
	size_t a11y_log_dropped = 0;
	int a11y_next_slot = 1;
	struct A11yRequest
	{
		int slot;
		cycfi::elements::a11y::node_id id;
		cycfi::elements::a11y::action act;
		cycfi::elements::a11y::action_arg arg;
	};
	std::mutex a11y_mtx;
	std::vector<A11yRequest> a11y_requests;

	// --- ゲーム本体の slot (ElementsDialog.setGameA11y) ---
	// Layer に描かれた UI (選択肢 / メニュー / 設定画面) はスクリプトがノードの
	// 表で渡す。 slot はダイアログ (1..) より下の 0 番で、 モーダルなダイアログの
	// 表示中は隠れる。 ダイアログが出ていないときの announce もここの live
	// region に載る。 AT の操作は onGameA11yAction でスクリプトへ返す。
	static constexpr int kGameA11ySlot = 0;
	struct GameA11y
	{
		std::shared_ptr<A11ySlot> slot;   // 無ければ未使用
		std::vector<tTVPGameA11yNode> nodes;
		std::string focus;
		std::string live_text;
		bool live_assertive = false;
		cycfi::elements::a11y::snapshot snap;
		std::unordered_map<cycfi::elements::a11y::node_id, std::string> ids;   // → スクリプトの id

		// --- Layer の自動 (a11yLayers) ---
		bool layers = false;
		std::vector<cycfi::elements::a11y::node> auto_nodes;     // 子の並びは設定済み
		std::vector<cycfi::elements::a11y::node_id> auto_top;    // 最上位に並べるもの
		cycfi::elements::a11y::node_id auto_focus = 0;
		// ノード → 操作先 (Layer か、 Elements パネルの中のノード)。 ポインタは
		// 配送の直前にツリーに居るか確かめ直してから使う。
		struct Target
		{
			tTJSNI_BaseLayer* layer = nullptr;
			tTVPElementsLayerPanel* panel = nullptr;
			cycfi::elements::a11y::node_id local = 0;
		};
		std::unordered_map<cycfi::elements::a11y::node_id, Target> auto_targets;
		tjs_uint32 auto_tick = 0;               // 最後に組んだ時刻 (ms)
		bool auto_built = false;
		const tTJSNI_BaseLayer* auto_focused_layer = nullptr;
	} game;

	// Layer の自動の組み直し (描画の外、 continuous イベント)。 AT が繋がって
	// いるか読み上げログを取っている間だけ登録する。
	bool a11y_last_active = false;   // onA11yActiveChanged の発火用

	// スクリーンリーダーの接続 / 切断を ElementsDialog.onA11yActiveChanged へ
	// (描画の外で配る)。 accesskit_host の通知 (Windows / macOS はウィンドウの
	// スレッド = 静止画面でも即時) と SyncA11y の両方から呼ぶ。
	void CheckA11yActiveChanged()
	{
		bool active = false;
#ifdef KRKRZ_HAS_A11Y
		active = a11y_host && a11y_host->is_active();
#endif
		if (active == a11y_last_active) return;
		a11y_last_active = active;
		PendingAction pa{nullptr, TJS_W("onA11yActiveChanged"),
			tTJSVariant((tjs_int)(active ? 1 : 0)), PendingAction::Kind::ClassEvent};
		pending_actions.push_back(std::move(pa));
		ArmActionHook();
		UpdateLayerHook();
	}

	struct A11yLayerHook : public tTVPContinuousEventCallbackIntf {
		Impl* owner = nullptr;
		bool registered = false;
		void TJS_INTF_METHOD OnContinuousCallback(tjs_uint64 tick) override;
	} a11y_layer_hook;

	// AT の操作を受ける (どのスレッドからも来る)。 メインスレッドで来たとき
	// (Windows / macOS: accesskit_host がウィンドウのスレッドへ投げ直す) は
	// その場で流す — 静止画面の WINVER では描画 (SyncA11y) が来ないため。
	std::thread::id a11y_main_thread = std::this_thread::get_id();
	void OnA11yRequest(int slot, cycfi::elements::a11y::node_id id,
	                   cycfi::elements::a11y::action act,
	                   cycfi::elements::a11y::action_arg arg)
	{
		{
			std::lock_guard<std::mutex> lock(a11y_mtx);
			a11y_requests.push_back({slot, id, act, std::move(arg)});
		}
		if (std::this_thread::get_id() == a11y_main_thread && paint_depth <= 0)
			DrainA11yRequests();
	}

	// 読み上げログは REPL / Agent で確かめるとき (REPL の口が開いているとき) だけ溜める
	static bool A11yLogging() { return TVPReplActive; }

	void A11yLogLine(const std::string& line)
	{
		a11y_log.push_back(line);
		if (a11y_log.size() > 2000) {
			a11y_log.erase(a11y_log.begin(), a11y_log.begin() + 1000);
			a11y_log_dropped += 1000;
		}
		tjs_string ws;
		TVPUtf8ToUtf16(ws, "[a11y] " + line);
		TVPAddLog(ttstr(ws.c_str()));
	}

	// view → OS (AccessKit) へ渡しつつ、 読み上げログを作る sink
	struct A11ySink : public cycfi::elements::a11y::sink
	{
		Impl* impl;
		std::weak_ptr<A11ySlot> slot;
		A11ySink(Impl* i, std::weak_ptr<A11ySlot> s) : impl(i), slot(std::move(s)) {}

		bool is_active() const override
		{
			auto s = slot.lock();
			if (!s) return false;
			if (A11yLogging()) return true;
#ifdef KRKRZ_HAS_A11Y
			return s->os && s->os->is_active();
#else
			return false;
#endif
		}

		void tree_changed(const cycfi::elements::a11y::snapshot& full,
		                  const cycfi::elements::a11y::update& delta) override
		{
			auto s = slot.lock();
			if (!s) return;
#ifdef KRKRZ_HAS_A11Y
			if (s->os) s->os->tree_changed(full, delta);
#else
			(void)delta;
#endif
			if (A11yLogging()) {
				for (const auto& line : cycfi::elements::a11y::speech_lines(
				         s->has_last ? &s->last : nullptr, full))
					impl->A11yLogLine(line);
			}
			s->last = full;
			s->has_last = true;
		}
	};

	// 張り直した session を OS / ログへ繋ぐ (BeginScreen の末尾、 OS への口を
	// 開いたとき)。
	void AttachA11y(Instance& inst)
	{
		if (!inst.session) return;
		if (!inst.a11y) {
			inst.a11y = std::make_shared<A11ySlot>();
			inst.a11y->slot = a11y_next_slot++;
		}
		inst.a11y->has_last = false;
		inst.a11y->z = -1;   // 次の SyncA11y で重なり順を送り直す
#ifdef KRKRZ_HAS_A11Y
		inst.a11y->os.reset();
		if (a11y_host) {
			const int slot = inst.a11y->slot;
			std::weak_ptr<A11ySlot> ws = inst.a11y;
			Instance* ip = &inst;
			inst.a11y->os = a11y_host->add_source(slot,
				[this, slot](cycfi::elements::a11y::node_id id,
				             cycfi::elements::a11y::action act,
				             cycfi::elements::a11y::action_arg arg) {
					OnA11yRequest(slot, id, act, std::move(arg));
				},
				// AT が繋がった瞬間のツリー (Windows / macOS はメインスレッドで来る)
				[this, ip]() {
					for (auto& up : instances)
						if (up.get() == ip && ip->session) return ip->session->a11y_snapshot();
					return cycfi::elements::a11y::snapshot{};
				},
				[ws]() {
					auto s = ws.lock();
					return s ? s->xf : cycfi::elements::a11y::transform{};
				});
		}
#endif
		inst.session->a11y_sink(std::make_shared<A11ySink>(this, inst.a11y));
	}

	void DetachA11y(Instance& inst)
	{
#ifdef KRKRZ_HAS_A11Y
		if (inst.a11y && a11y_host) a11y_host->remove_source(inst.a11y->slot);
#else
		(void)inst;
#endif
	}

	// OS への口をメインウィンドウに開く / 閉じる (モードに合わせる)。
	void UpdateA11yHost()
	{
#ifdef KRKRZ_HAS_A11Y
		const bool want = (a11y_mode != "off");
		if (want && !a11y_host) {
			void* hwnd_or_null = nullptr;
#ifdef __WINVER__
			if (TVPMainWindow) hwnd_or_null = TVPMainWindow->GetWindowHandle();
			if (!hwnd_or_null) return;
			a11y_host = cycfi::elements::a11y::accesskit_host::attach(hwnd_or_null);
#else
			SDL_Window* sw = HostMainWindow();
			if (!sw) return;
			(void)hwnd_or_null;
			a11y_host = cycfi::elements::a11y::accesskit_host::attach_sdl(sw);
#endif
			if (a11y_host && !a11y_label.empty()) a11y_host->set_window_label(a11y_label);
			if (a11y_host)
				a11y_host->on_active_changed([this](bool) {
					if (std::this_thread::get_id() == a11y_main_thread) CheckA11yActiveChanged();
				});
			for (auto& up : instances) AttachA11y(*up);
			// ゲーム slot は常設 (live region を先に置いておく。 文字の入った
			// live region が後から現れても、 スクリーンリーダーは読まないことがある)
			if (game.slot) AttachGameA11y();
			else EnsureGameA11y();
		} else if (!want && a11y_host) {
			for (auto& up : instances) {
				if (up->a11y) up->a11y->os.reset();
			}
			if (game.slot) game.slot->os.reset();
			a11y_host.reset();
		}
#endif
	}

	// AT からの操作を流す (メインスレッド)。 ダイアログの操作は session へ、
	// ゲーム slot の操作は onGameA11yAction の配送キューへ。
	void DrainA11yRequests()
	{
		std::vector<A11yRequest> reqs;
		{
			std::lock_guard<std::mutex> lock(a11y_mtx);
			reqs.swap(a11y_requests);
		}
		for (auto& r : reqs) {
			if (r.slot == kGameA11ySlot) {
				QueueGameNodeAction(r.id, r.act, r.arg);
				continue;
			}
			for (auto& up : instances) {
				if (up->a11y && up->a11y->slot == r.slot && up->session && up->active) {
					up->session->a11y_perform(r.id, r.act, std::move(r.arg));
					break;
				}
			}
		}
	}

	// PaintOverlay の末尾 (メインウィンドウのデバイス) から毎フレーム。
	void SyncA11y(iTVPDrawDevice* device)
	{
		DrainA11yRequests();

#ifdef KRKRZ_HAS_A11Y
		UpdateA11yHost();
		UpdateLayerHook();
		CheckA11yActiveChanged();
		if (!a11y_host) return;
		// present 変換 (dialog 論理座標 → window client px) をそのまま使う。
		// Linux (AT-SPI) はウィンドウ座標なので、 px → ウィンドウ座標へ直す。
		// 描画面 (renderer の surface) の座標 → OS 側の単位。 Windows は
		// ウィンドウの物理ピクセル、 macOS はポイント × backingScaleFactor
		// (SDL のウィンドウが高解像度でなくても Retina なら 2)、 Linux (AT-SPI) は
		// ウィンドウ座標。 surface は環境によって物理ピクセルだったり論理サイズ
		// だったりするので、 実際の比で換算する。
		float to_os = 1.0f;
#if !defined(__WINVER__)
		if (SDL_Window* sw = HostMainWindow()) {
			int lw = 0, lh = 0, pw = 0, ph = 0;
			SDL_GetWindowSize(sw, &lw, &lh);
			SDL_GetWindowSizeInPixels(sw, &pw, &ph);
			int surf_w = 0, surf_h = 0;
			if (iTVPDialogRenderer* r = FindRenderer(device)) r->GetSurfaceSize(surf_w, surf_h);
			if (surf_w <= 0) surf_w = pw;
#if defined(__linux__)
			const float target_w = (float)lw;
#elif defined(__APPLE__)
			const float target_w = (float)lw * a11y_host->native_scale();
#else
			const float target_w = (float)pw;
#endif
			if (surf_w > 0 && target_w > 0) to_os = target_w / (float)surf_w;
		}
#endif
		bool changed = false;
		for (size_t i = 0; i < instances.size(); ++i) {
			Instance& inst = *instances[i];
			if (!inst.a11y || !inst.session) continue;
			A11ySlot& s = *inst.a11y;
			const int z = (int)i;
			if (s.z != z) {
				a11y_host->set_z(s.slot, z);
				s.z = z;
				changed = true;
			}
			const bool modal = inst.modal && inst.active;
			if (s.modal != modal) {
				a11y_host->set_modal(s.slot, modal);
				s.modal = modal;
				changed = true;
			}
			cycfi::elements::a11y::transform xf{
				inst.present_scale * to_os, inst.present_scale * to_os,
				inst.present_off_x * to_os, inst.present_off_y * to_os};
			if (xf.sx != s.xf.sx || xf.sy != s.xf.sy || xf.tx != s.xf.tx || xf.ty != s.xf.ty) {
				a11y_host->set_transform(s.slot, xf);
				s.xf = xf;
				changed = true;
			}
		}
		// ゲーム slot: 普段は最背面。 キーを受けているダイアログが無く、 ゲーム側に
		// フォーカスがあるときは最前面へ上げる (AT のフォーカスはいちばん上の
		// slot のものが採られるため。 背面の非モーダルパネルに奪わせない)。
		if (game.slot) {
			A11ySlot& s = *game.slot;
			const bool game_focus = game.snap.focus && game.snap.focus != game.snap.root;
			const int z = (game_focus && !TopmostKeyboardFocus()) ? (int)instances.size() : -1;
			if (s.z != z) {
				a11y_host->set_z(s.slot, z);
				s.z = z;
				changed = true;
			}
			// primary layer の座標 → window client px (DestRect = ゲーム画像の表示領域)
			int dx = 0, dy = 0, dw = 0, dh = 0;
			tjs_int sw = 0, sh = 0;
			if (iTVPDialogRenderer* r = FindRenderer(device)) r->GetDestRect(dx, dy, dw, dh);
			if (device) device->GetSrcSize(sw, sh);
			if (sw > 0 && sh > 0 && dw > 0 && dh > 0) {
				cycfi::elements::a11y::transform xf{
					(float)dw / (float)sw * to_os, (float)dh / (float)sh * to_os,
					(float)dx * to_os, (float)dy * to_os};
				if (xf.sx != s.xf.sx || xf.sy != s.xf.sy || xf.tx != s.xf.tx || xf.ty != s.xf.ty) {
					a11y_host->set_transform(s.slot, xf);
					s.xf = xf;
					changed = true;
				}
			}
		}
		if (changed) a11y_host->flush();
#else
		(void)device;
#endif
	}

	// --- ゲーム slot ---

	// slot を (まだ無ければ) 作る。 setGameA11y / ダイアログが無いときの announce。
	void EnsureGameA11y()
	{
		if (game.slot) return;
		game.slot = std::make_shared<A11ySlot>();
		game.slot->slot = kGameA11ySlot;
		AttachGameA11y();
	}

	// OS の口へ繋ぐ (slot を作ったとき、 OS への口を開いたとき)。
	void AttachGameA11y()
	{
		if (!game.slot) return;
		game.slot->has_last = false;
		game.slot->z = INT_MIN;   // 次の SyncA11y で重なり順を送り直す
#ifdef KRKRZ_HAS_A11Y
		game.slot->os.reset();
		if (a11y_host) {
			std::weak_ptr<A11ySlot> ws = game.slot;
			game.slot->os = a11y_host->add_source(kGameA11ySlot,
				[this](cycfi::elements::a11y::node_id id,
				       cycfi::elements::a11y::action act,
				       cycfi::elements::a11y::action_arg arg) {
					OnA11yRequest(kGameA11ySlot, id, act, std::move(arg));
				},
				[this]() { return game.snap; },
				[ws]() {
					auto s = ws.lock();
					return s ? s->xf : cycfi::elements::a11y::transform{};
				});
		}
#endif
		PushGameA11y();
	}

	void DetachGameA11y()
	{
		if (!game.slot) return;
#ifdef KRKRZ_HAS_A11Y
		if (a11y_host) {
			a11y_host->remove_source(kGameA11ySlot);
			a11y_host->flush();
		}
#endif
		game.slot.reset();
		game.snap = cycfi::elements::a11y::snapshot{};
		game.ids.clear();
		game.auto_nodes.clear();
		game.auto_top.clear();
		game.auto_targets.clear();
		game.auto_focus = 0;
		game.auto_built = false;
		UpdateLayerHook();
	}

	// ロールから既定の操作。 スクリプトはロールと状態だけ書けばよい。
	static std::uint32_t GameA11yActions(cycfi::elements::a11y::role r, std::uint32_t states)
	{
		namespace a11y = cycfi::elements::a11y;
		std::uint32_t a = 0;
		switch (r) {
		case a11y::role::button: case a11y::role::toggle_button:
		case a11y::role::check_box: case a11y::role::radio_button:
		case a11y::role::tab: case a11y::role::menu_item: case a11y::role::list_item:
			a = a11y::bit(a11y::action::click) | a11y::bit(a11y::action::focus);
			break;
		case a11y::role::slider: case a11y::role::spin_button:
			a = a11y::bit(a11y::action::increment) | a11y::bit(a11y::action::decrement)
			  | a11y::bit(a11y::action::set_value) | a11y::bit(a11y::action::focus);
			break;
		case a11y::role::text_input: case a11y::role::multiline_text_input:
			a = a11y::bit(a11y::action::set_value) | a11y::bit(a11y::action::focus);
			break;
		default:
			break;
		}
		if (states & a11y::state::focusable) a |= a11y::bit(a11y::action::focus);
		if (states & a11y::state::disabled) a &= a11y::bit(a11y::action::focus);
		return a;
	}

	// スクリプトのノード表 → snapshot (primary layer 座標のまま。 変換は transform)。
	void BuildGameSnapshot()
	{
		namespace a11y = cycfi::elements::a11y;
		a11y::snapshot out;
		game.ids.clear();

		a11y::node root;
		root.id = a11y::hash_id("\x01krkrz.game");
		root.role = a11y::role::generic;
		tjs_int sw = 0, sh = 0;
		if (TVPMainWindow)
			if (iTVPDrawDevice* d = TVPMainWindow->GetDrawDevice()) d->GetSrcSize(sw, sh);
		root.bounds = cycfi::elements::rect(0, 0, (float)sw, (float)sh);

		std::vector<a11y::node> nodes;
		std::vector<std::string> parents;
		std::unordered_map<std::string, size_t> by_id;
		nodes.reserve(game.nodes.size());
		for (const auto& g : game.nodes) {
			std::string id = TtstrToUtf8(g.id);
			if (id.empty() || by_id.count(id)) {
				TVPAddLog(TJS_W("ElementsDialog.setGameA11y: id が空か重複しているノードを飛ばしました: ") + g.id);
				continue;
			}
			a11y::node n;
			n.id = a11y::hash_id(id);
			n.debug_id = id;
			const std::string role = TtstrToUtf8(g.role);
			auto r = a11y::role_from_name(role);
			if (!r && !role.empty())
				TVPAddLog(TJS_W("ElementsDialog.setGameA11y: 知らない role です (group として扱います): ") + g.role);
			n.role = r ? *r : a11y::role::generic;
			if (n.role == a11y::role::none) n.role = a11y::role::generic;
			n.name = TtstrToUtf8(g.name);
			n.value = TtstrToUtf8(g.value);
			n.description = TtstrToUtf8(g.description);
			const std::string st = TtstrToUtf8(g.states);
			size_t p = 0;
			while (p <= st.size()) {
				size_t e = st.find(',', p);
				if (e == std::string::npos) e = st.size();
				std::string w = st.substr(p, e - p);
				while (!w.empty() && w.front() == ' ') w.erase(w.begin());
				while (!w.empty() && w.back() == ' ') w.pop_back();
				if (!w.empty()) {
					std::uint32_t b = a11y::state_from_name(w);
					if (!b)
						TVPAddLog(TJS_W("ElementsDialog.setGameA11y: 知らない state です: ") + Utf8ToTtstr(w));
					n.states |= b;
				}
				p = e + 1;
			}
			n.states &= ~a11y::state::focused;   // focused は focus 引数で決める
			n.num_value = g.num_value;
			n.num_min = g.num_min;
			n.num_max = g.num_max;
			n.num_step = g.num_step;
			if (g.has_rect)
				n.bounds = cycfi::elements::rect((float)g.x, (float)g.y,
				                                 (float)(g.x + g.w), (float)(g.y + g.h));
			n.actions = GameA11yActions(n.role, n.states);
			if (n.actions & a11y::bit(a11y::action::focus)) n.states |= a11y::state::focusable;
			if (id == game.focus) n.states |= a11y::state::focused;
			by_id[id] = nodes.size();
			game.ids[n.id] = id;
			nodes.push_back(std::move(n));
			parents.push_back(TtstrToUtf8(g.parent));
		}
		for (size_t i = 0; i < nodes.size(); ++i) {
			auto it = parents[i].empty() ? by_id.end() : by_id.find(parents[i]);
			if (it == by_id.end() || it->second == i)
				root.children.push_back(nodes[i].id);
			else
				nodes[it->second].children.push_back(nodes[i].id);
		}

		// Layer の自動 (a11yLayers): setGameA11y のノードの後に並べる
		if (game.layers) {
			for (const auto& n : game.auto_nodes) nodes.push_back(n);
			for (auto id : game.auto_top) root.children.push_back(id);
		}

		// ダイアログが無いときの announce の行き先
		// 常に置く (空のまま待たせておき、 announce で中身を変える)
		a11y::node live;
		live.id = a11y::hash_id("\x01krkrz.game.live");
		live.role = a11y::role::status;
		live.name = game.live_text;
		live.live = game.live_assertive ? a11y::live::assertive : a11y::live::polite;
		root.children.push_back(live.id);

		// 根から pre-order で並べる (親が循環しているノードは届かないので落ちる)
		std::unordered_map<a11y::node_id, size_t> index;
		for (size_t i = 0; i < nodes.size(); ++i) index[nodes[i].id] = i;
		std::vector<bool> done(nodes.size(), false);
		out.root = root.id;
		out.nodes.push_back(root);
		std::function<void(a11y::node_id)> emit = [&](a11y::node_id id) {
			if (id == live.id) {
				out.nodes.push_back(live);
				return;
			}
			auto it = index.find(id);
			if (it == index.end() || done[it->second]) return;
			done[it->second] = true;
			out.nodes.push_back(nodes[it->second]);
			for (auto c : nodes[it->second].children) emit(c);
		};
		for (auto c : root.children) emit(c);

		// rect を省いたノード (list / group) は子を囲む矩形にする。 pre-order
		// なので後ろから畳めば子が先に決まる。
		{
			std::unordered_map<a11y::node_id, size_t> at;
			for (size_t i = 0; i < out.nodes.size(); ++i) at[out.nodes[i].id] = i;
			for (size_t i = out.nodes.size(); i-- > 1;) {
				a11y::node& nd = out.nodes[i];
				if (!nd.bounds.is_empty() || nd.children.empty()) continue;
				bool any = false;
				cycfi::elements::rect u;
				for (auto c : nd.children) {
					auto it = at.find(c);
					if (it == at.end() || out.nodes[it->second].bounds.is_empty()) continue;
					const auto& b = out.nodes[it->second].bounds;
					u = any ? cycfi::elements::max(u, b) : b;   // max = 両方を囲む矩形
					any = true;
				}
				if (any) nd.bounds = u;
			}
		}

		// フォーカス: setGameA11y の focus が優先 (スクリプトが明示している)。
		// 無ければ Layer の自動のフォーカス (window.focusedLayer)。
		out.focus = root.id;
		if (!game.focus.empty()) {
			const a11y::node_id f = a11y::hash_id(game.focus);
			for (const auto& n : out.nodes)
				if (n.id == f) { out.focus = f; break; }
		}
		if (out.focus == root.id && game.layers && game.auto_focus) {
			for (auto& n : out.nodes)
				if (n.id == game.auto_focus) { out.focus = n.id; n.states |= a11y::state::focused; break; }
		}
		out.reindex();
		game.snap = std::move(out);
	}

	// --- Layer の自動 (a11yLayers) ---

	// Layer オブジェクトのメンバを読む (無い / 例外は «無い» 扱い)。
	static bool ReadLayerMember(iTJSDispatch2* obj, const tjs_char* name, tTJSVariant& out)
	{
		if (!obj) return false;
		try {
			if (TJS_FAILED(obj->PropGet(0, name, nullptr, &out, obj))) return false;
		} catch (...) {
			return false;
		}
		return out.Type() != tvtVoid;
	}

	static bool IsLayerInstanceOf(iTJSDispatch2* obj, const tjs_char* cls)
	{
		if (!obj) return false;
		try {
			return obj->IsInstanceOf(0, nullptr, nullptr, cls, obj) == TJS_S_TRUE;
		} catch (...) {
			return false;
		}
	}

	// a11yStates: 配列か "a,b" 形式
	static std::uint32_t ReadStates(const tTJSVariant& v)
	{
		std::string st;
		if (v.Type() == tvtObject && v.AsObjectNoAddRef()) {
			iTJSDispatch2* arr = v.AsObjectNoAddRef();
			tTJSVariant c;
			tjs_int count = 0;
			if (TJS_SUCCEEDED(arr->PropGet(0, TJS_W("count"), nullptr, &c, arr))) count = (tjs_int)c;
			for (tjs_int i = 0; i < count; ++i) {
				tTJSVariant e;
				if (TJS_FAILED(arr->PropGetByNum(0, i, &e, arr)) || e.Type() == tvtVoid) continue;
				st += TtstrToUtf8(ttstr(e)) + ",";
			}
		} else {
			st = TtstrToUtf8(ttstr(v));
		}
		std::uint32_t bits = 0;
		size_t p = 0;
		while (p <= st.size()) {
			size_t e = st.find(',', p);
			if (e == std::string::npos) e = st.size();
			std::string w = st.substr(p, e - p);
			while (!w.empty() && w.front() == ' ') w.erase(w.begin());
			while (!w.empty() && w.back() == ' ') w.pop_back();
			if (!w.empty()) bits |= cycfi::elements::a11y::state_from_name(w);
			p = e + 1;
		}
		return bits;
	}

	// メインウィンドウの Layer ツリーから自動のノードを組む。
	void CollectAutoLayers()
	{
		game.auto_nodes.clear();
		game.auto_top.clear();
		game.auto_targets.clear();
		game.auto_focus = 0;
		game.auto_focused_layer = nullptr;
		game.auto_built = true;
		if (!game.layers || !TVPMainWindow) return;
		iTVPDrawDevice* dev = TVPMainWindow->GetDrawDevice();
		if (!dev) return;
		tTJSNI_BaseLayer* pri = dev->GetPrimaryLayer();
		if (!pri) return;
		tTJSNI_BaseLayer* focused = dev->GetFocusedLayer();
		game.auto_focused_layer = focused;
		std::unordered_map<std::string, int> used;   // id 文字列の重複よけ
		const tjs_int n = (tjs_int)pri->GetCount();
		for (tjs_int i = 0; i < n; ++i)
			WalkAutoLayer(pri->GetChildren(i), 0, focused, used);
	}

	// parent = 親のノード (0 = 最上位)
	void WalkAutoLayer(tTJSNI_BaseLayer* lay, cycfi::elements::a11y::node_id parent,
	                   tTJSNI_BaseLayer* focused, std::unordered_map<std::string, int>& used)
	{
		namespace a11y = cycfi::elements::a11y;
		if (!lay || !lay->GetVisible()) return;
		iTJSDispatch2* obj = lay->GetOwnerNoAddRef();
		tTJSVariant v;
		if (ReadLayerMember(obj, TJS_W("a11yHidden"), v) && (tjs_int)v) return;

		tTVPElementsLayerPanel* panel = tTVPElementsLayerPanel::FindByLayer(lay);
		tTJSVariant vname, vrole;
		const bool has_name = ReadLayerMember(obj, TJS_W("a11yName"), vname);
		const bool has_role = ReadLayerMember(obj, TJS_W("a11yRole"), vrole);
		const bool chain = lay->GetFocusable() && lay->GetJoinFocusChain();

		a11y::node_id self = parent;
		if (chain || has_name || has_role || panel) {
			a11y::node nd;
			// id: Layer.name (重複したら #2, #3 …)。 名前が無ければ «layer»
			std::string base = TtstrToUtf8(lay->GetName());
			if (base.empty()) base = "layer";
			std::string id = "layer:" + base;
			int& k = used[id];
			if (++k > 1) id += "#" + std::to_string(k);
			nd.id = a11y::hash_id("\x02" + id);
			nd.debug_id = id;

			// role: a11yRole > クラスからの推定 > 押せるものは button
			std::optional<a11y::role> r;
			if (has_role) r = a11y::role_from_name(TtstrToUtf8(ttstr(vrole)));
			if (!r) {
				if (IsLayerInstanceOf(obj, TJS_W("CheckBoxLayer"))) r = a11y::role::check_box;
				else if (IsLayerInstanceOf(obj, TJS_W("EditLayer"))) r = a11y::role::text_input;
				else if (chain) r = a11y::role::button;
				else if (panel) r = a11y::role::generic;   // 中の部品を束ねる
				else r = a11y::role::label;
			}
			nd.role = (*r == a11y::role::none) ? a11y::role::generic : *r;

			// name: a11yName > hint
			if (has_name) nd.name = TtstrToUtf8(ttstr(vname));
			else nd.name = TtstrToUtf8(lay->GetHint());
			if (ReadLayerMember(obj, TJS_W("a11yValue"), v)) nd.value = TtstrToUtf8(ttstr(v));
			else if (nd.role == a11y::role::text_input && ReadLayerMember(obj, TJS_W("text"), v))
				nd.value = TtstrToUtf8(ttstr(v));
			if (ReadLayerMember(obj, TJS_W("a11yDescription"), v)) nd.description = TtstrToUtf8(ttstr(v));
			if (ReadLayerMember(obj, TJS_W("a11yStates"), v)) nd.states = ReadStates(v);
			else if (nd.role == a11y::role::check_box && ReadLayerMember(obj, TJS_W("checked"), v) && (tjs_int)v)
				nd.states |= a11y::state::checked;
			nd.states &= ~a11y::state::focused;
			if (!lay->GetNodeEnabled()) nd.states |= a11y::state::disabled;
			if (chain) nd.states |= a11y::state::focusable;

			tjs_int x = 0, y = 0;
			lay->ToPrimaryCoordinates(x, y);
			nd.bounds = cycfi::elements::rect((float)x, (float)y,
				(float)(x + (tjs_int)lay->GetWidth()), (float)(y + (tjs_int)lay->GetHeight()));
			nd.actions = GameA11yActions(nd.role, nd.states);
			if (nd.actions & a11y::bit(a11y::action::focus)) nd.states |= a11y::state::focusable;
			if (lay == focused) game.auto_focus = nd.id;

			self = nd.id;
			GameA11y::Target t;
			t.layer = lay;
			game.auto_targets[nd.id] = t;
			AppendAutoNode(std::move(nd), parent);

			// Layer に描いている Elements パネルの中身を接ぐ
			if (panel) GraftPanel(panel, self, x, y, lay == focused);
		}

		const tjs_int n = (tjs_int)lay->GetCount();
		for (tjs_int i = 0; i < n; ++i)
			WalkAutoLayer(lay->GetChildren(i), self, focused, used);
	}

	void AppendAutoNode(cycfi::elements::a11y::node nd, cycfi::elements::a11y::node_id parent)
	{
		if (parent == 0) {
			game.auto_top.push_back(nd.id);
		} else {
			for (auto& p : game.auto_nodes)
				if (p.id == parent) { p.children.push_back(nd.id); break; }
		}
		game.auto_nodes.push_back(std::move(nd));
	}

	// パネルの読み上げツリーを、 そのレイヤのノードの下へ (id は混ぜ直す)。
	void GraftPanel(tTVPElementsLayerPanel* panel, cycfi::elements::a11y::node_id under,
	                tjs_int ox, tjs_int oy, bool layer_focused)
	{
		namespace a11y = cycfi::elements::a11y;
		a11y::snapshot ps = panel->A11ySnapshot();
		if (ps.nodes.empty()) return;
		const std::uint64_t salt = std::uint64_t(reinterpret_cast<std::uintptr_t>(panel)) * 0x9E3779B97F4A7C15ull;
		auto remap = [salt](a11y::node_id id) { return (id * 0xBF58476D1CE4E5B9ull) ^ salt; };
		for (const auto& pn : ps.nodes) {
			if (pn.id == ps.root) continue;
			a11y::node nd = pn;
			nd.id = remap(pn.id);
			for (auto& c : nd.children) c = remap(c);
			if (!nd.bounds.is_empty())
				nd.bounds = cycfi::elements::rect(nd.bounds.left + ox, nd.bounds.top + oy,
				                                  nd.bounds.right + ox, nd.bounds.bottom + oy);
			nd.states &= ~a11y::state::focused;
			if (layer_focused && pn.id == ps.focus) game.auto_focus = nd.id;
			GameA11y::Target t;
			t.panel = panel;
			t.local = pn.id;
			game.auto_targets[nd.id] = t;
			game.auto_nodes.push_back(std::move(nd));
		}
		// パネルの根の子をレイヤのノードの子にする
		if (const a11y::node* pr = ps.find(ps.root)) {
			for (auto& p : game.auto_nodes)
				if (p.id == under) {
					for (auto c : pr->children) p.children.push_back(remap(c));
					break;
				}
		}
	}

	// 組み直す頻度: 150ms ごと + フォーカス中の Layer が変わったら即時。
	// force = Agent.a11yTree 等、 今の状態が要るとき。
	void RefreshAutoLayers(bool force)
	{
		if (!game.layers) return;
		const tjs_uint32 now = TVPGetRoughTickCount32();
		const tTJSNI_BaseLayer* focused = nullptr;
		if (TVPMainWindow)
			if (iTVPDrawDevice* dev = TVPMainWindow->GetDrawDevice()) focused = dev->GetFocusedLayer();
		if (!force && game.auto_built && focused == game.auto_focused_layer
		    && now - game.auto_tick < 150)
			return;
		game.auto_tick = now;
		EnsureGameA11y();
		CollectAutoLayers();
		PushGameA11y();
	}

	bool LayerInTree(const tTJSNI_BaseLayer* root, const tTJSNI_BaseLayer* target)
	{
		if (!root) return false;
		if (root == target) return true;
		auto* r = const_cast<tTJSNI_BaseLayer*>(root);
		const tjs_int n = (tjs_int)r->GetCount();
		for (tjs_int i = 0; i < n; ++i)
			if (LayerInTree(r->GetChildren(i), target)) return true;
		return false;
	}

	// AT / Agent からのゲーム slot の操作 (どちらのノードか振り分ける)。
	bool QueueGameNodeAction(cycfi::elements::a11y::node_id id, cycfi::elements::a11y::action act,
	                         const cycfi::elements::a11y::action_arg& arg)
	{
		auto it = game.ids.find(id);
		if (it != game.ids.end()) {
			QueueGameA11yAction(it->second, act, arg);
			return true;
		}
		auto at = game.auto_targets.find(id);
		if (at == game.auto_targets.end()) return false;
		PendingAction pa{nullptr, ttstr(), tTJSVariant((tjs_int)act), PendingAction::Kind::LayerA11y};
		pa.target = at->second;
		if (arg.text) pa.extra = Utf8ToTtstr(*arg.text);
		else if (arg.number) {
			char b[64];
			snprintf(b, sizeof(b), "%.17g", *arg.number);
			pa.extra = Utf8ToTtstr(b);
		}
		pending_actions.push_back(std::move(pa));
		ArmActionHook();
		return true;
	}

	// 描画の外 (action_hook) で Layer / パネルを操作する。
	void DispatchLayerA11yAction(const GameA11y::Target& t, cycfi::elements::a11y::action act,
	                             const ttstr& arg)
	{
		namespace a11y = cycfi::elements::a11y;
		if (t.panel) {
			if (!tTVPElementsLayerPanel::IsAlive(t.panel)) return;
			a11y::action_arg aa;
			if (!arg.IsEmpty()) aa.text = TtstrToUtf8(arg);
			t.panel->A11yPerform(t.local, act, std::move(aa));
			return;
		}
		if (!TVPMainWindow) return;
		iTVPDrawDevice* dev = TVPMainWindow->GetDrawDevice();
		if (!dev || !LayerInTree(dev->GetPrimaryLayer(), t.layer)) return;   // 消えた
		tTJSNI_BaseLayer* lay = t.layer;
		// onA11yAction(action, arg) があればそちらへ。 false を返したら既定の処理も
		if (iTJSDispatch2* obj = lay->GetOwnerNoAddRef()) {
			tTJSVariant fn;
			if (ReadLayerMember(obj, TJS_W("onA11yAction"), fn) && fn.Type() == tvtObject) {
				tTJSVariant a0(Utf8ToTtstr(a11y::action_name(act))), a1(arg), res;
				tTJSVariant* args[] = {&a0, &a1};
				try {
					fn.AsObjectClosureNoAddRef().FuncCall(0, nullptr, nullptr, &res, 2, args, obj);
				} catch (eTJSScriptError& e) {
					TVPAddLog(TJS_W("Layer.onA11yAction: ") + e.GetMessage());
				} catch (eTJS& e) {
					TVPAddLog(TJS_W("Layer.onA11yAction: ") + e.GetMessage());
				}
				if (!(res.Type() != tvtVoid && !(tjs_int)res)) return;   // false 以外 = 処理済み
			}
		}
		if (act == a11y::action::focus || act == a11y::action::click) {
			if (lay->GetNodeFocusable()) lay->SetFocus(true);
		}
		if (act == a11y::action::click) {
			// フォーカスした Layer に Enter を押させる (キー操作できる部品の «押す»)
			TVPPostInputEvent(new tTVPOnKeyDownInputEvent(TVPMainWindow, VK_RETURN, 0));
			TVPPostInputEvent(new tTVPOnKeyUpInputEvent(TVPMainWindow, VK_RETURN, 0));
		}
	}

	// Layer の自動の組み直しフック: a11yLayers かつ (AT 接続中 or 読み上げログ中)
	void UpdateLayerHook()
	{
		bool want = game.layers && A11yLogging();
#ifdef KRKRZ_HAS_A11Y
		want = want || (game.layers && a11y_host && a11y_host->is_active());
#endif
		if (want && !a11y_layer_hook.registered) {
			a11y_layer_hook.owner = this;
			TVPAddContinuousEventHook(&a11y_layer_hook);
			a11y_layer_hook.registered = true;
		} else if (!want && a11y_layer_hook.registered) {
			TVPRemoveContinuousEventHook(&a11y_layer_hook);
			a11y_layer_hook.registered = false;
		}
	}

	// 組み直して OS / 読み上げログへ流す。
	void PushGameA11y()
	{
		if (!game.slot) return;
		BuildGameSnapshot();
		A11ySlot& s = *game.slot;
		auto delta = cycfi::elements::a11y::diff(s.has_last ? &s.last : nullptr, game.snap);
		if (s.has_last && delta.empty()) return;
		A11ySink(this, game.slot).tree_changed(game.snap, delta);
	}

	// onGameA11yAction(id, action, arg) を描画の外で配る。
	void QueueGameA11yAction(const std::string& id, cycfi::elements::a11y::action act,
	                         const cycfi::elements::a11y::action_arg& arg)
	{
		std::string a;
		if (arg.text) a = *arg.text;
		else if (arg.number) {
			char b[64];
			snprintf(b, sizeof(b), "%.17g", *arg.number);
			a = b;
		}
		PendingAction pa{nullptr, Utf8ToTtstr(id),
			tTJSVariant(Utf8ToTtstr(cycfi::elements::a11y::action_name(act))),
			PendingAction::Kind::GameA11y};
		pa.extra = Utf8ToTtstr(a);
		pending_actions.push_back(std::move(pa));
		ArmActionHook();
	}

	static void DispatchGameA11yAction(const ttstr& id, const ttstr& action, const ttstr& arg)
	{
		iTJSDispatch2* global = TVPGetScriptDispatch();
		if (!global) return;
		tTJSVariant cls;
		if (TJS_SUCCEEDED(global->PropGet(0, TJS_W("ElementsDialog"), nullptr, &cls, global))
		    && cls.Type() == tvtObject) {
			iTJSDispatch2* c = cls.AsObjectNoAddRef();
			tTJSVariant fn;
			if (c && TJS_SUCCEEDED(c->PropGet(0, TJS_W("onGameA11yAction"), nullptr, &fn, c))
			    && fn.Type() == tvtObject) {
				tTJSVariant a0(id), a1(action), a2(arg);
				tTJSVariant* args[] = {&a0, &a1, &a2};
				tTJSVariantClosure clo = fn.AsObjectClosureNoAddRef();
				try {
					clo.FuncCall(0, nullptr, nullptr, nullptr, 3, args, nullptr);
				} catch (eTJSScriptError& e) {
					TVPAddLog(TJS_W("ElementsDialog.onGameA11yAction: ") + e.GetMessage());
				} catch (eTJS& e) {
					TVPAddLog(TJS_W("ElementsDialog.onGameA11yAction: ") + e.GetMessage());
				}
			}
		}
		global->Release();
	}

	// ElementsDialog.<name>(arg) を呼ぶ (定義されていなければ何もしない)。
	static void CallClassEvent(const ttstr& name, const tTJSVariant& arg)
	{
		iTJSDispatch2* global = TVPGetScriptDispatch();
		if (!global) return;
		tTJSVariant cls;
		if (TJS_SUCCEEDED(global->PropGet(0, TJS_W("ElementsDialog"), nullptr, &cls, global))
		    && cls.Type() == tvtObject) {
			iTJSDispatch2* c = cls.AsObjectNoAddRef();
			tTJSVariant fn;
			if (c && TJS_SUCCEEDED(c->PropGet(0, name.c_str(), nullptr, &fn, c))
			    && fn.Type() == tvtObject) {
				tTJSVariant a0(arg);
				tTJSVariant* args[] = {&a0};
				try {
					fn.AsObjectClosureNoAddRef().FuncCall(0, nullptr, nullptr, nullptr, 1, args, nullptr);
				} catch (eTJSScriptError& e) {
					TVPAddLog(TJS_W("ElementsDialog.") + name + TJS_W(": ") + e.GetMessage());
				} catch (eTJS& e) {
					TVPAddLog(TJS_W("ElementsDialog.") + name + TJS_W(": ") + e.GetMessage());
				}
			}
		}
		global->Release();
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

	// 外部 close 要求 (ElementsDialog.close / QUIT)。 session 生存中は session->close()
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

	// === paint 中に発火した OnAction の遅延配送 ===
	// PaintOverlay → RenderInstance → session->update() の最中に button click
	// 等の action が発火することがある (touch 経由の click 等)。 その場で
	// handler->OnAction を呼ぶと、 コールバック内で開かれるブロッキング
	// モーダル (System.inputString 等) の nested pump が window update の
	// 再入禁止 (TVPDeliverWindowUpdateEvents) に阻まれ、 一切描画されない
	// まま固まる (Steam Deck 実機で発生、 2026-08-24)。 paint 中の action は
	// キューに積み、 continuous イベントフック (window update の外で毎フレーム
	// 呼ばれる) から配送する。 入力経路 (ForwardMouse* 等) から発火した action
	// は paint_depth==0 なので従来どおり即時配送される。
	int paint_depth = 0;

	// overlay インスタンスを持たないが生きている handler (ホストのレイヤへ描く
	// パネル)。 通知キューは 1 本を共用するので、 配送前の生存確認をここにも
	// 通す (詳細は ElementsDialogManager.h の RegisterExternalHandler)。
	std::set<iTVPDialogEventHandler*> external_handlers;

	//! @brief 通知を配送してよい handler か (どちらかに生きていれば true)。
	bool IsHandlerLive(iTVPDialogEventHandler* h)
	{
		if (!h) return false;
		if (FindByHandler(h)) return true;
		return external_handlers.count(h) != 0;
	}

	struct PendingAction {
		enum class Kind { Action, Drag, Var, GameA11y, LayerA11y, ClassEvent };
		iTVPDialogEventHandler* handler;
		ttstr id;               // Action: widget id / Var: 変数名 / GameA11y: ノード id
		tTJSVariant payload;    // Var: 値 (文字列) / GameA11y: 操作名
		Kind kind = Kind::Action;
		ttstr extra;            // GameA11y / LayerA11y: 操作の引数 (set_value の値)
		GameA11y::Target target;   // LayerA11y: 操作先 (payload は action の数値)
	};
	std::deque<PendingAction> pending_actions;
	struct ActionDrainHook : public tTVPContinuousEventCallbackIntf {
		Impl* owner = nullptr;
		bool registered = false;
		void TJS_INTF_METHOD OnContinuousCallback(tjs_uint64 tick) override;
	} action_hook;

	void QueueOrDispatchAction(iTVPDialogEventHandler* handler,
	                           const ttstr& id, const tTJSVariant& payload)
	{
		if (paint_depth <= 0) {
			handler->OnAction(id, payload);
			return;
		}
		pending_actions.push_back({handler, id, payload});
		if (!action_hook.registered) {
			action_hook.owner = this;
			TVPAddContinuousEventHook(&action_hook);
			action_hook.registered = true;
		}
	}

	// ドラッグ通知。 action と同じキューに積んで **順序を保つ**
	// (「離した」の後に「押した」が届くと状態機械が壊れるため)。
	// move だけは、 キュー末尾が同じ handler の move なら差し替える
	// (paint 中に溜まった移動は最新の位置だけ配れば足りる)。
	void QueueOrDispatchDrag(iTVPDialogEventHandler* handler,
	                         const tTJSVariant& payload, bool coalesce)
	{
		if (paint_depth <= 0 && pending_actions.empty()) {
			handler->OnDrag(payload);
			return;
		}
		if (coalesce && !pending_actions.empty()) {
			PendingAction& back = pending_actions.back();
			if (back.kind == PendingAction::Kind::Drag &&
			    back.handler == handler) {
				back.payload = payload;
				return;
			}
		}
		pending_actions.push_back(
			{handler, ttstr(), payload, PendingAction::Kind::Drag});
		ArmActionHook();
	}

	// 変数変化通知。 変数は «状態» なので即時配送はせず必ずキューへ積む
	// (elements の入力処理 / 描画の最中に TJS を走らせない)。 同じ handler +
	// 同じ変数名が既に積まれていれば値だけ差し替える (1 フレームぶんの連続
	// 変化 — hover 移動・ドラッグ中の座標書込など — は最新値だけ配れば足りる)。
	// 差し替えは «その場» で行うので、 action との相対順序は崩れない。
	void QueueVar(iTVPDialogEventHandler* handler,
	              const ttstr& name, const ttstr& value)
	{
		for (auto& a : pending_actions) {
			if (a.kind == PendingAction::Kind::Var &&
			    a.handler == handler && a.id == name) {
				a.payload = value;
				return;
			}
		}
		pending_actions.push_back(
			{handler, name, tTJSVariant(value), PendingAction::Kind::Var});
		ArmActionHook();
	}

	// キュー配送用の continuous フックを (未登録なら) 登録する。
	void ArmActionHook()
	{
		if (action_hook.registered) return;
		action_hook.owner = this;
		TVPAddContinuousEventHook(&action_hook);
		action_hook.registered = true;
	}

	// 変数観測 (OnVar) の張り直し。 handler が WantsVarNotify で観測を望んだ
	// ときだけ watcher を張る — hover 連動 (vars_on_hover) やドラッグ座標は
	// 毎フレーム書かれるので、 観測しないホストには一切コストを掛けない。
	// 名前リストが空なら全変数、 非空ならその名前だけ通知する。
	// 画面 (session) は遷移のたびに作り直されるので開始時に毎回呼ぶこと。
	void ApplyVarWatch(Instance& inst)
	{
		if (!inst.session) return;
		tvp_elements::ApplyVarWatch(*inst.session, inst.handler);
	}

	void DrainPendingActions()
	{
		if (action_hook.registered) {
			TVPRemoveContinuousEventHook(&action_hook);
			action_hook.registered = false;
		}
		std::deque<PendingAction> q;
		q.swap(pending_actions);
		for (auto& a : q) {
			if (a.kind == PendingAction::Kind::GameA11y) {
				DispatchGameA11yAction(a.id, ttstr(a.payload), a.extra);
				continue;
			}
			if (a.kind == PendingAction::Kind::ClassEvent) {
				CallClassEvent(a.id, a.payload);   // ElementsDialog.<id>(payload)
				continue;
			}
			if (a.kind == PendingAction::Kind::LayerA11y) {
				DispatchLayerA11yAction(a.target,
					(cycfi::elements::a11y::action)(tjs_int)a.payload, a.extra);
				continue;
			}
			// 配送前に handler が生きているか確認する (close_on_click で
			// 閉じたダイアログの action は、 handler が短命 (スタック上の
			// no-op handler 等) の可能性があるため捨てる)。 レイヤパネルの
			// handler は overlay インスタンスを持たないので external_handlers
			// も見る。
			if (!IsHandlerLive(a.handler)) continue;
			switch (a.kind) {
			case PendingAction::Kind::Drag:
				a.handler->OnDrag(a.payload);
				break;
			case PendingAction::Kind::Var:
				a.handler->OnVar(a.id, ttstr(a.payload));
				break;
			default:
				a.handler->OnAction(a.id, a.payload);
				break;
			}
		}
	}

	// 1 インスタンス分の描画 (close/finish 処理は呼出側で済ませる)。
	void RenderInstance(Instance& inst, iTVPDrawDevice* device,
	                    iTVPDialogRenderer* renderer);
};

//---------------------------------------------------------------------------
void TJS_INTF_METHOD
tTVPElementsDialogManager::Impl::ActionDrainHook::OnContinuousCallback(tjs_uint64)
{
	// TVPDeliverContinuousEvent から呼ばれる = window update の外 (安全な文脈)。
	if (owner) owner->DrainPendingActions();
}

void TJS_INTF_METHOD
tTVPElementsDialogManager::Impl::A11yLayerHook::OnContinuousCallback(tjs_uint64)
{
	if (!owner) return;
	owner->RefreshAutoLayers(false);
	owner->UpdateLayerHook();   // AT が離れたら外れる
}

//---------------------------------------------------------------------------
// Impl: フロー / 描画メソッド (out-of-line)
//---------------------------------------------------------------------------
//---------------------------------------------------------------------------
// 画面 JSON の **top-level** "size": [w, h] だけを拾う (上限サイズの peek)。
//
// widget 側にも配列値の "size" を取るものがある (spacer の "size": [w,h]、
// atlas_slider の 9-slice thumb の "size": [w,h] など) ので、 単純な文字列
// 検索では入れ子の値を先に掴んでしまい、 ダイアログがその大きさに縮む。
// 文字列と JSONC コメントを飛ばしつつ括弧の深さを数え、 **深さ 1 のキー**
// だけを見る。 フォントサイズのような数値の "size" は値が配列でないので
// 従来どおり無視される。
//---------------------------------------------------------------------------
static bool PeekTopLevelSize(const std::string& js, int& out_w, int& out_h)
{
	auto is_ws = [](char c) {
		return c == ' ' || c == '\t' || c == '\n' || c == '\r';
	};
	int depth = 0;
	for (size_t i = 0; i < js.size(); ++i) {
		const char c = js[i];
		// JSONC コメント (画面 JSON は JSONC を許す) を飛ばす
		if (c == '/' && i + 1 < js.size()) {
			if (js[i + 1] == '/') {
				i = js.find('\n', i);
				if (i == std::string::npos) break;
				continue;
			}
			if (js[i + 1] == '*') {
				i = js.find("*/", i + 2);
				if (i == std::string::npos) break;
				++i;   // "*/" の 2 文字目 (ループの ++i と合わせて読み飛ばす)
				continue;
			}
		}
		if (c == '"') {
			// 文字列 (キーまたは値)。 終端まで飛ばす。
			size_t j = i + 1;
			for (; j < js.size(); ++j) {
				if (js[j] == 0x5C) { ++j; continue; }   // escape
				if (js[j] == '"') break;
			}
			const size_t len = (j <= js.size() ? j : js.size()) - (i + 1);
			const bool is_size_key =
				(depth == 1 && len == 4 && js.compare(i + 1, 4, "size") == 0);
			i = (j < js.size()) ? j : js.size() - 1;
			if (!is_size_key) continue;

			size_t k = i + 1;
			while (k < js.size() && is_ws(js[k])) ++k;
			if (k >= js.size() || js[k] != ':') continue;
			++k;
			while (k < js.size() && is_ws(js[k])) ++k;
			if (k >= js.size() || js[k] != '[') continue;   // 数値 size は無視
			const char* p = js.c_str() + k + 1;
			char* endp = nullptr;
			const long w = std::strtol(p, &endp, 10);
			long h = 0;
			if (endp && *endp == ',') h = std::strtol(endp + 1, nullptr, 10);
			if (w > 0 && h > 0) {
				out_w = static_cast<int>(w);
				out_h = static_cast<int>(h);
				return true;
			}
			continue;
		}
		if (c == '{' || c == '[')      ++depth;
		else if (c == '}' || c == ']') --depth;
	}
	return false;
}

//---------------------------------------------------------------------------
bool tTVPElementsDialogManager::Impl::BeginScreen(
	Instance& inst, const std::string& json_utf8,
	const std::string& resource_base_utf8)
{
	// JSON の top-level "size":[w,h] を peek (上限サイズ)。
	// "size" が無ければ overlay の既定を surface (ゲーム画面) 全面にする
	// (従来は 400x220 きめうちで、content が大きいとクリップされていた)。
	inst.dialog_w = 0;
	inst.dialog_h = 0;
	bool has_explicit_size = PeekTopLevelSize(json_utf8,
	                                          inst.dialog_w, inst.dialog_h);
	if (!has_explicit_size) {
		int sw = 0, sh = 0;
		if (auto* r = FindRenderer(inst.host_device)) r->GetSurfaceSize(sw, sh);
		if (sw > 0 && sh > 0) { inst.dialog_w = sw; inst.dialog_h = sh; }
		else                  { inst.dialog_w = 400; inst.dialog_h = 220; }
	}

	// session の組み立ては出力先 (overlay / ホストのレイヤ) で共通なので
	// ElementsSessionBuild へ寄せてある (action / drag の橋渡し、 言語の適用、
	// 変数観測の張り直しまで済む)。
	tvp_elements::SessionOptions sopt;
	sopt.width         = inst.dialog_w;
	sopt.height        = inst.dialog_h;
	sopt.resource_base = resource_base_utf8;
	sopt.language      = language;
	auto sess = tvp_elements::BuildSession(json_utf8, sopt, inst.handler);
	if (!sess) return false;

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
	// 読み上げ: 新しい画面を OS / ログへ繋ぐ (画面ごとに作り直されるので毎回)
	AttachA11y(inst);
	// 変数観測 (OnVar) は BuildSession が張っている (画面ごとに session が
	// 作り直されるので、 遷移先の画面でも張り直される)。
	inst.current_resource_base = resource_base_utf8;
	inst.has_rect = false;
	inst.armed_vks.clear();   // 押しっぱなしキーの誤爆防止状態をリセット

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
		// ElementsDialog.baseSize が設定されていればそれを基準面とする (UI を
		// ゲーム画面と別解像度で author するタイトル向け)。 未設定なら
		// 従来どおりゲームの基準面 (primary layer サイズ)。
		tjs_int src_w = base_size_w, src_h = base_size_h;
		if ((src_w <= 0 || src_h <= 0) && device)
			device->GetSrcSize(src_w, src_h);
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

	// 再入ガード: このインスタンスの session->update() が呼び出しスタック上に
	// ある (update 中の OnAction からネストモーダルの pump が回り、 PaintOverlay
	// が再入した) 場合は、 update を再入させず前回描画のキャッシュ提示のみ行う
	// (elements session の update は再入不可)。 ネストモーダルの下で背景として
	// 静止表示され続ける。
	if (inst.in_update) {
		if (inst.cache_valid && inst.cache_device == device) {
			renderer->PresentOverlay(layer, inst.cache_px, inst.cache_py,
			                         inst.cache_pw, inst.cache_ph);
			stats.presents++;
			stats.cachedPresents++;
		}
		return;
	}

	// 状態更新 (変数/hover poll・演出 tick・view 遅延タスク・ダーティ蓄積)。
	// 描画をスキップするフレームでも毎フレーム必要 (キャレット点滅・遅延
	// focus 適用・退場演出の完了検出が止まるため)。 戻り値 = 再描画が必要か。
	const auto t_update = std::chrono::steady_clock::now();
	inst.in_update = true;
	const bool dirty = inst.session->update();
	inst.in_update = false;
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

	// 再ラスタライズ抑止 (ElementsDialog.renderCache): session が dirty でなく、 描画
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

	// 部分再描画 (ElementsDialog.partialRedraw): renderCache 有効時のみ (staging に
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

	if (!allow_partial && render_cache && partial_redraw && NavLogEnabled()) {
		// -navlog: 部分再描画を諦めた理由を出す (どの一致条件が崩れたか)。
		// 画面生成直後の 1 フレーム目は valid=0 で出るのが正常。
		char ab[256];
		snprintf(ab, sizeof(ab),
			"no-partial: trans=%d valid=%d dev=%d buf=%d(%dx%d vs %dx%d)"
			" render=%d(%dx%d vs %dx%d) surf=%d fit=%d area=%d",
			inst.trans_effect.empty() ? 0 : 1, inst.cache_valid ? 1 : 0,
			(inst.cache_device == device) ? 1 : 0,
			(inst.cache_buf_w == w_pixels && inst.cache_buf_h == h_pixels) ? 1 : 0,
			(int)inst.cache_buf_w, (int)inst.cache_buf_h, (int)w_pixels, (int)h_pixels,
			(inst.cache_sw == render_sw && inst.cache_sh == render_sh) ? 1 : 0,
			(int)inst.cache_sw, (int)inst.cache_sh, (int)render_sw, (int)render_sh,
			(inst.cache_surf_w == sw && inst.cache_surf_h == sh) ? 1 : 0,
			(inst.cache_fit == fit) ? 1 : 0,
			(inst.cache_area_x == area_x && inst.cache_area_y == area_y &&
			 inst.cache_area_w == area_w && inst.cache_area_h == area_h) ? 1 : 0);
		NavLog(ab);
	}

	const auto t_raster = std::chrono::steady_clock::now();
	elements_modal::overlay_session::render_rect rect{};
	elements_modal::overlay_session::render_rect updated{};
	bool ok;
	// surface には 0,0 を渡して session 内部のアンカー配置を無効化する
	// (out_rect = (0,0,コンテンツ実寸))。 session はコンテンツを buffer
	// (canvas) 原点に描き、 on_mouse は last_rect を引いて view 座標へ戻す。
	// ここで非 0 の surface (旧: dialog 論理サイズ) を渡すと last_rect に
	// アンカーオフセットが乗り、 「描画は canvas 原点 / ヒット判定はアンカー
	// 位置」の不一致でマウス反応位置がずれていた (authored "size" > 内容実寸
	// かつ非中央 "align" の画面で顕在化。 自動フィット中央の画面は canvas =
	// 内容実寸でオフセット 0 のため露見しなかった)。 配置は下の manager 側
	// placement が行う。
	if (allow_partial) {
		ok = inst.session->render_to_buffer_partial(buf, w_pixels, h_pixels,
		                                            0, 0,
		                                            rect, updated);
	} else {
		ok = inst.session->render_to_buffer(buf, w_pixels, h_pixels,
		                                    0, 0, rect);
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
	// 配置の基準は canvas (authored サイズ) ではなく**コンテンツ実寸**
	// (rect = render の実描画 bbox。 canvas 原点起点)。 canvas 原点 =
	// コンテンツ原点なので、 px,py にはコンテンツを置きたい位置をそのまま
	// 入れればよい。 authored サイズ基準にすると、 内容が authored より
	// 小さい画面 (size 明示の部分パネル等) で bottom/right 寄せが内容ぶん
	// だけ手前へずれて見えていた。
	const int pw = static_cast<int>(w_logical * fit + 0.5f);
	const int ph = static_cast<int>(h_logical * fit + 0.5f);
	const int content_w = static_cast<int>(rect.w * fit + 0.5f);
	const int content_h = static_cast<int>(rect.h * fit + 0.5f);
	float ax = 0.5f, ay = 0.5f;
	int amargin = 0;
	inst.session->placement(ax, ay, amargin);
	const int free_x = area_w - content_w;
	const int free_y = area_h - content_h;
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
// elements ランタイム (ThorVG) を一度でも初期化したか。 終了時の後始末を
// «使ったときだけ» 走らせるための記録で、 EnsureRuntimeInitialized が立てる。
//---------------------------------------------------------------------------
static bool& TVPElementsRuntimeInited()
{
	static bool inited = false;
	return inited;
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
void em_set_nav_log(bool enable);
}

tTVPElementsDialogManager::tTVPElementsDialogManager()
	: _impl(std::make_unique<Impl>())
{
	elements_modal::em_set_log_sink(
		[](const char* line) {
			// em_logf の行は **UTF-8**。 ttstr(const char*) に渡すと SJIS
			// ビルド (TVP_TEXT_READ_ANSI_MBCS) では CP932 として解釈され、
			// 日本語まじりの診断が «UNICODE 文字列に変換できません» 例外に
			// 化ける (elements 側の新しいメッセージで顕在化)。 明示変換する。
			tjs_string w;
			TVPUtf8ToUtf16(w, std::string(line ? line : ""));
			TVPAddLog(ttstr(TJS_W("elements_modal: ")) + ttstr(w.c_str()));
		});
}

tTVPElementsDialogManager::~tTVPElementsDialogManager()
{
	// 遅延 action 配送フックの解除 (登録したまま破棄すると dangling)
	if (_impl && _impl->action_hook.registered) {
		TVPRemoveContinuousEventHook(&_impl->action_hook);
		_impl->action_hook.registered = false;
	}
	if (_impl && _impl->a11y_layer_hook.registered) {
		TVPRemoveContinuousEventHook(&_impl->a11y_layer_hook);
		_impl->a11y_layer_hook.registered = false;
	}
}

//---------------------------------------------------------------------------
// 終了時の後始末 — ThorVG を畳む
//
// elements_modal::init() で立てた ThorVG は、 対になる shutdown() を呼ばないと
// tvg::Initializer::term() に到達せず、 その先の LoaderMgr::term() (「グローバル
// に使われているフォントローダを掃除する」処理) が飛ぶ。 掃除されなかった
// ローダは ThorVG の名前空間スコープ static (_activeLoaders) に載ったまま
// プロセス終了を迎え、 CRT の atexit がそれを破棄するときに
// 「関数内 static なので先に死んでいるフォントマネージャ」(gw ビルドなら
// GwFontManager、 ft ビルドなら FtFontManager) を触る。 順序は決定的で、
// 確率的なのは «そのアドレスがまだ読めるか» だけ — つまり間欠クラッシュになる。
// 吉里吉里は WINVER / SDL とも CRT の atexit を通って終了する (WINVER は
// ExitProcess をコメントアウト済み、 SDL は std::exit) ので、 顕在化しうる。
//
// **畳む順序が肝**で、 view / canvas が 1 つでも生きていると
// SwRenderer::term() が «まだ canvas がある» と弾き、 Initializer::term() は
// LoaderMgr::term() の手前で早期 return する (戻り値 InsufficientCondition)。
// そのため先に ForceClose() で全インスタンスを畳んでから shutdown() を呼ぶ。
// elements 側の測定用 scratch canvas とアトラスの pixmap キャッシュは
// shutdown() が内部で手放す (elements `47871878` 以降)。
//---------------------------------------------------------------------------
void tTVPElementsDialogManager::ShutdownRuntime()
{
	if (!TVPElementsRuntimeInited()) return;
	TVPElementsRuntimeInited() = false;
	ForceClose();
	_impl->game.layers = false;
	_impl->DetachGameA11y();
#ifdef KRKRZ_HAS_A11Y
	_impl->a11y_host.reset();   // 読み上げ: OS への口を外す
#endif
	elements_modal::shutdown();
	// 「畳んだ」ことをログに残す。 終了時クラッシュを追うとき、 この行が
	// 出ているかどうかが最初の切り分けになる。
	TVPAddLog(TJS_W("ElementsDialog: runtime shut down (ThorVG terminated)"));
}

//---------------------------------------------------------------------------
// 読み上げ (スクリーンリーダー対応)
//---------------------------------------------------------------------------
namespace {
std::string A11yJsonString(const std::string& s)
{
	std::string out = "\"";
	for (unsigned char c : s) {
		switch (c) {
		case '"':  out += "\\\""; break;
		case '\\': out += "\\\\"; break;
		case '\n': out += "\\n"; break;
		default:
			if (c < 0x20) { char b[8]; snprintf(b, sizeof(b), "\\u%04x", c); out += b; }
			else out += (char)c;
		}
	}
	return out + "\"";
}
} // namespace

ttstr tTVPElementsDialogManager::A11yTreeJson() const
{
	_impl->RefreshAutoLayers(true);   // Layer の自動は今の状態で
	std::string out = "{\"dialogs\":[";
	bool first = true;
	for (size_t i = 0; i < _impl->instances.size(); ++i) {
		const auto& inst = *_impl->instances[i];
		if (!inst.session || !inst.active) continue;
		if (!first) out += ",";
		first = false;
		std::string screen = inst.nav ? inst.nav->current() : std::string();
		out += "{\"index\":" + std::to_string(i) + ",\"screen\":" + A11yJsonString(screen)
		     + ",\"modal\":" + (inst.modal ? "true" : "false")
		     + ",\"tree\":" + inst.session->a11y_dump_json() + "}";
	}
	out += "],\"game\":";
	if (_impl->game.slot) {
		bool hidden = false;
		for (const auto& up : _impl->instances)
			if (up->session && up->active && up->modal) hidden = true;
		out += "{\"hidden\":" + std::string(hidden ? "true" : "false")
		     + ",\"tree\":" + cycfi::elements::a11y::to_json(_impl->game.snap) + "}";
	} else {
		out += "null";
	}
	out += "}";
	return Utf8ToTtstr(out);
}

std::vector<ttstr> tTVPElementsDialogManager::A11yLog(size_t since, size_t& next) const
{
	next = _impl->a11y_log_dropped + _impl->a11y_log.size();
	std::vector<ttstr> out;
	size_t start = (since > _impl->a11y_log_dropped) ? since - _impl->a11y_log_dropped : 0;
	for (size_t i = start; i < _impl->a11y_log.size(); ++i)
		out.push_back(Utf8ToTtstr(_impl->a11y_log[i]));
	return out;
}

bool tTVPElementsDialogManager::A11yAction(const ttstr& node, const ttstr& action,
                                           const ttstr& arg)
{
	const std::string n = TtstrToUtf8(node), a = TtstrToUtf8(action), v = TtstrToUtf8(arg);
	for (size_t i = _impl->instances.size(); i-- > 0;) {
		auto& inst = *_impl->instances[i];
		if (!inst.session || !inst.active) continue;
		if (inst.session->a11y_perform(n, a, v)) return true;
		if (inst.modal) return false;   // ゲーム slot はモーダルの下で隠れている
	}
	// ゲーム slot (AT と同じく onGameA11yAction で返す)
	if (!_impl->game.slot) return false;
	auto act = cycfi::elements::a11y::action_from_name(a);
	if (!act) return false;
	_impl->RefreshAutoLayers(true);
	for (const auto& nd : _impl->game.snap.nodes) {
		if (nd.id == _impl->game.snap.root) continue;
		if (cycfi::elements::a11y::id_string(nd) != n) continue;
		if (!(nd.actions & cycfi::elements::a11y::bit(*act))) return false;
		cycfi::elements::a11y::action_arg arg;
		if (!v.empty()) arg.text = v;
		return _impl->QueueGameNodeAction(nd.id, *act, arg);
	}
	return false;
}

void tTVPElementsDialogManager::SetA11yLayers(bool on)
{
	if (_impl->game.layers == on) return;
	_impl->game.layers = on;
	if (on) {
		_impl->RefreshAutoLayers(true);
	} else {
		_impl->game.auto_nodes.clear();
		_impl->game.auto_top.clear();
		_impl->game.auto_targets.clear();
		_impl->game.auto_focus = 0;
		_impl->game.auto_built = false;
		_impl->PushGameA11y();
	}
	_impl->UpdateLayerHook();
}

bool tTVPElementsDialogManager::GetA11yLayers() const
{
	return _impl->game.layers;
}

void tTVPElementsDialogManager::A11yAnnounce(const ttstr& text, bool assertive)
{
	const std::string t = TtstrToUtf8(text);
	for (size_t i = _impl->instances.size(); i-- > 0;) {
		auto& inst = *_impl->instances[i];
		if (!inst.session || !inst.active) continue;
		inst.session->announce(t, assertive);
		return;
	}
	// 画面が 1 枚も出ていない: ゲーム slot の live region で読ませる。 同じ文を
	// 続けて読ませるときは見えない文字で変化を付ける (live region は変化で読む)。
	_impl->EnsureGameA11y();
	std::string next = t;
	if (next == _impl->game.live_text) next += "\xE2\x80\x8B";
	_impl->game.live_text = next;
	_impl->game.live_assertive = assertive;
	_impl->PushGameA11y();
}

void tTVPElementsDialogManager::SetGameA11y(const std::vector<tTVPGameA11yNode>& nodes,
                                            const ttstr& focus)
{
	_impl->EnsureGameA11y();
	_impl->game.nodes = nodes;
	_impl->game.focus = TtstrToUtf8(focus);
	_impl->PushGameA11y();
}

void tTVPElementsDialogManager::ClearGameA11y()
{
	// slot は残す (live region は常設。 ダイアログが無いときの announce の行き先)
	_impl->game.nodes.clear();
	_impl->game.focus.clear();
	if (_impl->game.slot) _impl->PushGameA11y();
}

bool tTVPElementsDialogManager::A11yActive() const
{
#ifdef KRKRZ_HAS_A11Y
	return _impl->a11y_host && _impl->a11y_host->is_active();
#else
	return false;
#endif
}

void tTVPElementsDialogManager::SetA11yMode(const ttstr& mode)
{
	std::string m = TtstrToUtf8(mode);
	if (m != "auto" && m != "off") {
		TVPAddLog(TJS_W("ElementsDialog.a11yMode: \"auto\" か \"off\" を指定してください"));
		return;
	}
	_impl->a11y_mode = m;
	_impl->UpdateA11yHost();
}

ttstr tTVPElementsDialogManager::GetA11yMode() const
{
	return Utf8ToTtstr(_impl->a11y_mode);
}

void tTVPElementsDialogManager::SetA11yLabel(const ttstr& label)
{
	_impl->a11y_label = TtstrToUtf8(label);
#ifdef KRKRZ_HAS_A11Y
	if (_impl->a11y_host) {
		_impl->a11y_host->set_window_label(_impl->a11y_label);
		_impl->a11y_host->flush();
	}
#endif
}

ttstr tTVPElementsDialogManager::GetA11yLabel() const
{
	return Utf8ToTtstr(_impl->a11y_label);
}

static void TVPShutdownElementsRuntime()
{
	// Elements を一度も使っていないプロセスでは何もしない。 Instance() は
	// 呼べば manager を作ってしまうので、 フラグを先に見る。
	if (!TVPElementsRuntimeInited()) return;
	tTVPElementsDialogManager::Instance().ShutdownRuntime();
}

// TVPSystemUninit → TVPCauseAtExit から呼ばれる。 優先度は昇順に実行されるので
// SHUTDOWN(100) はフォントラスタライザ等の解放 (RELEASE=1000) より前になる。
// このとき TJS エンジンは既に落ちている (TVPUninitScriptEngine が先) ので、
// ElementsDialog / ElementsPanel の TJS オブジェクトは解放済み。
static tTVPAtExit TVPShutdownElementsRuntimeAtExit(
	TVP_ATEXIT_PRI_SHUTDOWN, TVPShutdownElementsRuntime);

void tTVPElementsDialogManager::DispatchAction(iTVPDialogEventHandler* handler,
	const ttstr& id, const tTJSVariant& payload)
{
	if (!handler) return;
	_impl->QueueOrDispatchAction(handler, id, payload);
}

void tTVPElementsDialogManager::DispatchDrag(iTVPDialogEventHandler* handler,
	const tTJSVariant& payload, bool coalesce)
{
	if (!handler) return;
	_impl->QueueOrDispatchDrag(handler, payload, coalesce);
}

void tTVPElementsDialogManager::DispatchVar(iTVPDialogEventHandler* handler,
	const ttstr& name, const ttstr& value)
{
	if (!handler) return;
	_impl->QueueVar(handler, name, value);
}

bool tTVPElementsDialogManager::GetVar(iTVPDialogEventHandler* handler,
                                       const ttstr& name, ttstr& out) const
{
	Impl::Instance* inst = _impl->FindByHandler(handler);
	if (!inst || !inst->active || !inst->session) return false;
	std::string v;
	if (!inst->session->get_var(TtstrToUtf8(name), v)) return false;
	out = Utf8ToTtstr(v);
	return true;
}

std::vector<tTVPElementsDialogManager::VarInfo>
tTVPElementsDialogManager::DescribeVars(iTVPDialogEventHandler* handler) const
{
	std::vector<VarInfo> out;
	Impl::Instance* inst = _impl->FindByHandler(handler);
	if (!inst || !inst->session) return out;
	for (auto const& d : inst->session->list_vars()) {
		VarInfo info;
		info.name  = Utf8ToTtstr(d.name);
		info.value = Utf8ToTtstr(d.value);
		info.used_by.reserve(d.used_by.size());
		for (auto const& u : d.used_by) {
			info.used_by.emplace_back(Utf8ToTtstr(u.first),
			                          Utf8ToTtstr(u.second));
		}
		out.push_back(std::move(info));
	}
	return out;
}

void tTVPElementsDialogManager::RefreshVarWatch(iTVPDialogEventHandler* handler)
{
	Impl::Instance* inst = _impl->FindByHandler(handler);
	if (inst) _impl->ApplyVarWatch(*inst);
}

bool tTVPElementsDialogManager::IsModalActive() const
{
	return _impl->AnyActive();
}

bool tTVPElementsDialogManager::IsActiveOnDevice(iTVPDrawDevice* device) const
{
	return _impl->AnyActiveOn(device);
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

void tTVPElementsDialogManager::SetBaseSize(int w, int h)
{
	if (w > 0 && h > 0) {
		_impl->base_size_w = w;
		_impl->base_size_h = h;
	} else {
		_impl->base_size_w = 0;
		_impl->base_size_h = 0;
	}
}

void tTVPElementsDialogManager::GetBaseSize(int& w, int& h) const
{
	w = _impl->base_size_w;
	h = _impl->base_size_h;
}

void tTVPElementsDialogManager::SetRenderCache(bool enable)
{
	_impl->render_cache = enable;
}

bool tTVPElementsDialogManager::GetRenderCache() const
{
	return _impl->render_cache;
}

#include "SysInitIntf.h"   // TVPGetCommandLine (-navlog)
// ナビ診断ログ (-navlog 起動オプションで有効)。 フォーカス移動 / cursor-warp /
// パッド方向キーの到着を ms 時刻付きで出す (長押し時の説明文とハイライトの
// ずれ調査用)。 既定は無効で挙動に影響しない。
static bool NavLogEnabled()
{
	static int enabled = -1;
	if (enabled < 0) {
		tTJSVariant v;
		enabled = TVPGetCommandLine(TJS_W("-navlog"), &v) ? 1 : 0;
		// session (elements_modal) 側のナビ診断ログも同じフラグで動かす。
		// コマンドラインが解析済みの最初の呼び出し (描画/入力) で確定する。
		elements_modal::em_set_nav_log(enabled == 1);
	}
	return enabled == 1;
}
static void NavLog(const std::string &msg)
{
	if (!NavLogEnabled()) return;
	static const auto t0 = std::chrono::steady_clock::now();
	auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
		std::chrono::steady_clock::now() - t0).count();
	char buf[64];
	snprintf(buf, sizeof(buf), "[nav %6lld] ", (long long)ms);
	ttstr line(buf);
	TVPAddLog(line + Utf8ToTtstr(msg));
}

// pad_icon テーマの自動選択 (setPadTheme("auto"))。
//   接続中のパッドの系統 (xbox / ps / switch) をそのままテーマにする。
//   パッドが無い / 判らないときは動作プラットフォームで決める
//   (Switch 本体なら switch、 PS5 なら ps)。 それ以外 (Windows 等) は
//   パッドが 1 つでもつながっていれば xbox、 1 つも無ければ keyboard
//   (パッド無しで Xbox の絵を出しても操作できるキーが判らないため)。
static bool TVPPadThemeAuto = false;
// PollAutoPadTheme 用: 前回見たパッドの接続数と系統。 変化したときだけ決め直す。
static tjs_int    TVPPadThemeSeenCount = -1;
static tjs_string TVPPadThemeSeenStyle;

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

	const tjs_int count = Application->GetJoypadCount();
	tjs_string style = Application->GetJoypadStyle(0);
	TVPPadThemeSeenCount = count;
	TVPPadThemeSeenStyle = style;
	if (style.empty()) {
		const std::vector<tjs_string> &tags = Application->GetPlatformTags();
		for (const tjs_string &t : tags) {
			if (t == TJS_W("switch")) { style = TJS_W("switch"); break; }
			if (t == TJS_W("ps5"))    { style = TJS_W("ps");     break; }
		}
	}
	if (style.empty()) style = (count > 0) ? TJS_W("xbox") : TJS_W("keyboard");

	std::string utf8;
	TVPUtf16ToUtf8(utf8, style);
	auto t = cycfi::elements::parse_pad_theme(utf8);
	if (t == cycfi::elements::pad_theme::none) return;
	if (t == cycfi::elements::get_pad_theme()) return;
	cycfi::elements::set_pad_theme(t);
	// 出ている画面の pad_icon は次の描画で新しい theme の絵に差し替わる。
	InvalidateOverlays();
}

void tTVPElementsDialogManager::PollAutoPadTheme()
{
	if (!TVPPadThemeAuto || !Application) return;
	const tjs_int count = Application->GetJoypadCount();
	if (count == TVPPadThemeSeenCount) {
		if (count == 0) return;
		if (Application->GetJoypadStyle(0) == TVPPadThemeSeenStyle) return;
	}
	ResolveAutoPadTheme();
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
	// パネルにも同じ要求を配る (registerImage の mem:// 差替は共有 store なので
	// overlay もパネルも同じきっかけで描き直す必要がある)。
	tTVPElementsLayerPanel::InvalidateAll();
}

//---------------------------------------------------------------------------
void tTVPElementsDialogManager::RegisterExternalHandler(
	iTVPDialogEventHandler* handler)
{
	if (handler) _impl->external_handlers.insert(handler);
}

//---------------------------------------------------------------------------
void tTVPElementsDialogManager::UnregisterExternalHandler(
	iTVPDialogEventHandler* handler)
{
	if (!handler) return;
	_impl->external_handlers.erase(handler);
	// キューに残っている通知も捨てる (解放済み handler へ配送しないため)。
	auto& q = _impl->pending_actions;
	q.erase(std::remove_if(q.begin(), q.end(),
		[handler](const Impl::PendingAction& a) { return a.handler == handler; }),
		q.end());
}

//---------------------------------------------------------------------------
void tTVPElementsDialogManager::PushDeferScope() { ++_impl->paint_depth; }
void tTVPElementsDialogManager::PopDeferScope()
{
	if (_impl->paint_depth > 0) --_impl->paint_depth;
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

	if (elements_modal::init("", /*load_default_fonts=*/false))
		TVPElementsRuntimeInited() = true;

	// authored 画面は自前のフォーカス表示 (focused frame / focus_link 装飾)
	// を持つため、 elements テーマの既定フォーカスリング (青枠) は全面 OFF に
	// する (theme.focus_ring_enabled が用意する application-wide スイッチ)。
	// ただし ElementsDialog.focusRing がスクリプトから明示設定済みならそれを尊重する
	// (初回画面表示でここが後から走っても上書きしない)。
	static bool s_theme_tuned = false;
	if (!s_theme_tuned) {
		if (!FocusRingUserSet) {
			auto thm = cycfi::elements::get_theme();
			thm.focus_ring_enabled = false;
			cycfi::elements::set_theme(thm);
		}
		s_theme_tuned = true;
	}

	static bool s_fonts_loaded = false;
	if (!s_fonts_loaded) {
#ifdef __WINVER__
		// WINVER: フォントは exe 埋め込み (resources.rc の "BINARY" 型)。 SDL 版の
		// ResourcePath (resource:// / file://./resource/) は WINVER 埋め込みリソースの
		// 代替なので、 WINVER では Win32 リソース API から直接列挙・登録する。
		TVPRegisterElementsFontsFromWinResources();
		TVPRegisterElementsHostDefaultFonts();
		TVPApplyRegisteredFontsToElementsTheme();
		s_fonts_loaded = true;
#else
		if (Application) {
			TVPRegisterElementsFontsFromStorageDir(ttstr(Application->ResourcePath().c_str()));
			TVPRegisterElementsHostDefaultFonts();
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
		if (inst->session) {
			info.focused   = Utf8ToTtstr(inst->session->focused_id());
			info.textFocus = inst->session->focus_consumes_text();
		}
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

bool tTVPElementsDialogManager::GetTextInputArea(tjs_int& x, tjs_int& y,
	tjs_int& w, tjs_int& h, tjs_int& cursor) const
{
	if (!_impl->ime_area_valid) return false;
	x = _impl->ime_area_x;
	y = _impl->ime_area_y;
	w = _impl->ime_area_w;
	h = _impl->ime_area_h;
	cursor = _impl->ime_area_cursor;
	return true;
}

bool tTVPElementsDialogManager::SetVar(iTVPDialogEventHandler* handler,
                                       const ttstr& name, const ttstr& value)
{
	Impl::Instance* inst = _impl->FindByHandler(handler);
	if (!inst || !inst->active || !inst->session) return false;
	inst->session->set_var(TtstrToUtf8(name), TtstrToUtf8(value));
	return true;
}

bool tTVPElementsDialogManager::SetKeyCapture(iTVPDialogEventHandler* handler,
                                              bool on)
{
	Impl::Instance* inst = _impl->FindByHandler(handler);
	if (!inst || !inst->active || !inst->session) return false;
	inst->key_capture = on;
	return true;
}

bool tTVPElementsDialogManager::FocusWidget(iTVPDialogEventHandler* handler,
                                            const ttstr& id)
{
	Impl::Instance* inst = _impl->FindByHandler(handler);
	if (!inst || !inst->active || !inst->session) return false;
	inst->session->focus_by_id(TtstrToUtf8(id));
	return true;
}

bool tTVPElementsDialogManager::ActivateWidget(iTVPDialogEventHandler* handler,
                                               const ttstr& id)
{
	Impl::Instance* inst = _impl->FindByHandler(handler);
	if (!inst || !inst->active || !inst->session) return false;
	// activate_by_id は focus を即時適用してから Enter を送る
	// (ActivateWidgetById と同じ)。
	return inst->session->activate_by_id(TtstrToUtf8(id));
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
	// ホストのレイヤに描くパネルにも同じ言語を配る (プロセス全体の設定)。
	tTVPElementsLayerPanel::SetLanguageAll(_impl->language);
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

void tTVPElementsDialogManager::FlushPendingTeardowns()
{
	// PaintOverlay 冒頭の close_requested 処理の即時版 (ヘッダのコメント参照)。
	// update() がスタック上にあるものは触らない (通常フレームに任せる)。
	std::vector<Impl::Instance*> to_teardown;
	for (size_t i = 0; i < _impl->instances.size(); ++i) {
		Impl::Instance* inst = _impl->instances[i].get();
		if (inst->in_update) continue;
		if (!inst->close_requested) continue;
		inst->close_requested = false;
		to_teardown.push_back(inst);
	}
	for (auto* inst : to_teardown) {
		bool alive = false;
		for (auto& up : _impl->instances) {
			if (up.get() == inst) { alive = true; break; }
		}
		if (alive) _impl->TeardownInstance(inst);
	}
}

void tTVPElementsDialogManager::ForceClose()
{
	// window 破棄経路でも呼ばれるので、 記録済み入力 window は失効させる
	// (cursor-warp のダングリング防止)。
	_impl->input_window = nullptr;
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
		tTVPElementsRenderStats& st;
		tTVPElementsRenderStats  before;
		std::chrono::steady_clock::time_point t0 =
			std::chrono::steady_clock::now();
		TotalGuard(tTVPElementsRenderStats& s) : st(s), before(s) {}
		~TotalGuard() {
			tjs_uint64 us = ElapsedUs(t0);
			st.totalUs += us;
			// -navlog: 100ms を超えた提示フレームは段ごとの内訳を出す
			// (どこで固まっているかの切り分け用。 既定は無効)
			if (us >= 100000 && NavLogEnabled()) {
				char buf[200];
				snprintf(buf, sizeof(buf),
					"slow frame %llu ms: update=%llu raster=%llu acquire=%llu upload=%llu present=%llu (ms)",
					(unsigned long long)(us / 1000),
					(unsigned long long)((st.updateUs  - before.updateUs)  / 1000),
					(unsigned long long)((st.rasterUs  - before.rasterUs)  / 1000),
					(unsigned long long)((st.acquireUs - before.acquireUs) / 1000),
					(unsigned long long)((st.uploadUs  - before.uploadUs)  / 1000),
					(unsigned long long)((st.presentUs - before.presentUs) / 1000));
				NavLog(buf);
			}
		}
	} total_guard{ _impl->stats };

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

	// パッドの抜き差しに pad_icon のテーマを追従させる (setPadTheme("auto"))。
	if (device_is_main) PollAutoPadTheme();

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

	// !! この関数は再入する !!
	// RenderInstance → session->update() がボタン click 等の OnAction を発火し、
	// その中でゲーム/ホストが System.inputString 等の「ブロッキング overlay
	// モーダル + ネスト pump」を開くと、 ネストしたフレームでこの関数が再帰的に
	// 呼ばれ、 その間に instances が増減する (ネスト側モーダルの表示と teardown)。
	// range-for のイテレータは erase で無効化されるため、 全フェーズを index
	// ベース + 都度 size() 再検証で回し、 集めたポインタは生存確認してから使う。
	// (実クラッシュ: パネルのボタンから System.inputString → モーダルを閉じた
	//  瞬間に外側の range-for が無効イテレータを踏んで AV。 2026-08-24)
	//
	// paint 深度: この間に発火した OnAction は QueueOrDispatchAction が
	// キューへ積み、 continuous フックが paint の外で配送する (Impl の
	// pending_actions のコメント参照)。
	struct PaintDepthScope {
		int& d;
		PaintDepthScope(int& x) : d(x) { ++d; }
		~PaintDepthScope() { --d; }
	} paint_depth_scope(_impl->paint_depth);

	// 1) close 予約 / 自動 finish の処理 (この device のインスタンスのみ)。
	//    teardown でリスト要素が消えるので、 ポインタを集めてから処理する。
	{
		std::vector<Impl::Instance*> to_teardown;
		for (size_t i = 0; i < _impl->instances.size(); ++i) {
			Impl::Instance* inst = _impl->instances[i].get();
			if (inst->host_device != device) continue;

			// update() がスタック上にある (ネストモーダル pump からの再入) 間は
			// このインスタンスを破棄も finish 処理もしない。 close_requested は
			// 立てたまま持ち越し、 update 完了後のフレームで teardown する。
			if (inst->in_update) continue;

			if (inst->close_requested) {
				inst->close_requested = false;
				to_teardown.push_back(inst);
				continue;
			}
			if (!inst->active || !inst->session) continue;

			// "close_on_click" / Esc 等で session が自動 finish したとき。
			// (exit 演出がある画面は、 その再生完了後にここへ来る。)
			// AdvanceFlow は OnScreenLeave/Enter コールバックを発火するので
			// ここも再入しうる → index ループ。
			if (inst->session->finished()) {
				if (inst->close_after_exit) {
					// ElementsDialog.close() 等の外部 close: transitions を解決せず終了。
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
		for (auto* inst : to_teardown) {
			// ネスト実行中に既に破棄されたインスタンスはスキップ (生存確認)
			bool alive = false;
			for (auto& up : _impl->instances) {
				if (up.get() == inst) { alive = true; break; }
			}
			if (alive) _impl->TeardownInstance(inst);
		}
	}

	// 2) 描画 (z-order 奥→手前 = instances 先頭→末尾)。
	//    RenderInstance 内の session->update() が OnAction → ネストモーダルを
	//    起こしうるので index ベース (上のコメント参照)。
	iTVPDialogRenderer* renderer = _impl->FindRenderer(device);
	if (!renderer) return;
	// パッド軸ナビ (dpad/スティック) は入力対象の最前面セッションだけが行う。
	// 背面セッションで方向キーを押しっぱなしのまま上に別画面を開くと、 背面が
	// リピートでフォーカスを動かし続ける。 update() 前に設定する。
	Impl::Instance* pad_target = _impl->TopmostKeyboardFocus();
	if (NavLogEnabled() && pad_target && pad_target->session) {
		static std::string s_last_focus;
		size_t pt_idx = 0;
		for (size_t i = 0; i < _impl->instances.size(); ++i)
			if (_impl->instances[i].get() == pad_target) { pt_idx = i; break; }
		const std::string &fid = pad_target->session->focused_id();
		std::string cur = "#" + std::to_string(pt_idx) + " "
		                  + (fid.empty() ? std::string("(none)") : fid);
		if (cur != s_last_focus) {
			s_last_focus = cur;
			NavLog("focus -> " + cur);
		}
	}
	for (auto& up : _impl->instances) {
		if (up->session) up->session->set_pad_nav_active(up.get() == pad_target);
	}
	for (size_t i = 0; i < _impl->instances.size(); ++i) {
		Impl::Instance* inst = _impl->instances[i].get();
		if (inst->host_device != device) continue;
		if (!inst->active || !inst->session) continue;
		_impl->RenderInstance(*inst, device, renderer);
	}

	// 3) portable: テキスト欄への focus 状態に追従してソフトキーボードを出し入れ。
	_impl->UpdateFocusDrivenTextInput();
	//    デスクトップ (WINVER): 同じ focus 状態に追従して IME を開閉する。
	_impl->UpdateImeFollowFocus();
	//    変換 / 変換候補ウィンドウをキャレット位置へ寄せる (WINVER / SDL 共通)。
	_impl->UpdateTextInputArea();
	//    読み上げ: AT の操作を流し、 重なり順 / モーダル / 位置を OS 側へ合わせる。
	if (device_is_main) _impl->SyncA11y(device);

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
				// 仮想カーソル位置は「描画矩形内の座標」を受ける契約
				// (実装側で DestRect 原点を足し戻す) ため、 window client へ
				// 直したあと DestRect 原点を引いて渡す。
				tjs_int lx = static_cast<tjs_int>(
					sx * f->present_scale + f->present_off_x + 0.5f)
					- f->dest_offset_x;
				tjs_int ly = static_cast<tjs_int>(
					sy * f->present_scale + f->present_off_y + 0.5f)
					- f->dest_offset_y;
				_impl->warp_seq++;
				if (NavLogEnabled()) {
					size_t f_idx = 0;
					for (size_t i = 0; i < _impl->instances.size(); ++i)
						if (_impl->instances[i].get() == f) { f_idx = i; break; }
					char wb[240];
					snprintf(wb, sizeof(wb),
						"warp request -> %d,%d (#%zu surface %.1f,%.1f"
						" scale=%.4f off=%.1f,%.1f dest=%d,%d for %s)",
						(int)lx, (int)ly, f_idx, sx, sy,
						f->present_scale, f->present_off_x, f->present_off_y,
						(int)f->dest_offset_x, (int)f->dest_offset_y,
						f->session->focused_id().c_str());
					NavLog(std::string("#") + std::to_string(_impl->warp_seq)
					       + " " + wb);
				}
				// **実 OS カーソルは動かさない**。 仮想カーソル位置だけを
				// フォーカス先へ置く (doc/VirtualCursor.md)。 hover 判定も
				// Layer.cursorX/Y もこの位置を見るので、 OS を一往復させずに
				// 「カーソルがフォーカス先に乗っている」状態が作れる。
				// これにより「返ってきた mouse move は自分の warp の echo か」
				// という推測判定そのものが不要になった。
				static_cast<tTJSNI_Window*>(
					static_cast<tTJSNI_BaseWindow*>(_impl->input_window))
					->SetVirtualCursorPos(lx, ly);
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
// 入力変換 (VK / tTVPMouseButton / TVP_SS_* → cycfi 中立型) は
// ElementsInputMap.h へ移した (ホストのレイヤに描くパネルと共用するため)。
using namespace tvp_elements_input;

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
	// キー捕捉中: 左以外のボタン押下は捕捉先へ渡して消費する
	// (右クリックでの取り消し等)。 左クリックはウィジェットへ通常配送。
	if (mb != mbLeft) {
		if (Impl::Instance* c = _impl->KeyCaptureTarget()) {
			_impl->DeliverKeyCapture(c, MouseButtonToVk(mb), flags);
			return true;
		}
	}
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
	// キー捕捉中: 左以外のボタンの離しは捨てる (押下と対で消費)
	if (mb != mbLeft && _impl->KeyCaptureTarget()) return true;
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
	if (NavLogEnabled()) {
		NavLog(std::string("mouse move ") + std::to_string(x) + "," + std::to_string(y));
	}
	// 仮想カーソル位置の導入により、 ここへ来る mouse move は **常に実マウス**
	// になった (キー / パッドのナビは実 OS カーソルを動かさず仮想位置だけを
	// 更新する)。 かつては「自分が出した warp の折返しか」を座標で推測して
	// いたが、 その判定ごと不要になっている。 doc/VirtualCursor.md 参照。

	bool consumed = false;
	Impl::Instance* hit = nullptr;
	// 最前面から: modal なら独占、 非モーダルはヒット判定。
	for (auto it = _impl->instances.rbegin(); it != _impl->instances.rend(); ++it) {
		Impl::Instance* inst = it->get();
		if (!inst->active || !inst->session) continue;
		float sx = Impl::ToSurfaceX(*inst, x);
		float sy = Impl::ToSurfaceY(*inst, y);
		if (!hit && (inst->modal || Impl::RectContains(*inst, sx, sy))) {
			if (NavLogEnabled())
				NavLog("  hover -> #" + std::to_string(
					_impl->instances.size() - 1 - (size_t)(it - _impl->instances.rbegin()))
					+ " surface " + std::to_string((int)sx) + "," + std::to_string((int)sy));
			inst->session->on_mouse_move(sx, sy, FlagsToElementsMods(flags));
			if (NavLogEnabled())
				NavLog("    after hover focus = " + inst->session->focused_id());
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
	// キー捕捉中: ウィジェットにもホストホットキーにも回さず捕捉先へ渡す。
	// リピートの扱い (無視するか等) は受け手が shift の TVP_SS_REPEAT で決める。
	if (Impl::Instance* c = _impl->KeyCaptureTarget()) {
		if (!(shift & TVP_SS_REPEAT)) c->armed_vks.insert(key);
		_impl->DeliverKeyCapture(c, key, shift);
		return true;
	}
	// ホストホットキー: 登録キー (VK_PAD* 含む) はダイアログへ渡さず通常経路へ。
	// モーダル表示中とテキスト入力 focus 中 (duringTextInput=false のもの) は
	// バイパスしない — 判定は HostHotkeyBypass 側。
	if (_impl->HostHotkeyBypass(key, shift, /*isUp=*/false)) return false;
	Impl::Instance* f = _impl->TopmostKeyboardFocus();
	if (!f || !f->session) return false;
	// ダイアログ表示前から押しっぱなしのキーはリピートしか届かない。 新規
	// 押下を見ていない VK のリピートは配送しない (一度離すまで効かない)。
	// 長押しスキップ中に自動で開くソフトキーボードへ決定ボタンが即入力される
	// 誤爆の防止。 非モーダルは素通し (ゲーム側の長押し継続)。
	if (shift & TVP_SS_REPEAT) {
		if (!f->armed_vks.count(key)) return f->modal ? true : false;
	} else {
		f->armed_vks.insert(key);
	}
	auto r = RouteVk(key);
	if (NavLogEnabled() && r.k != vk_routing::kind::none) {
		char kb[80];
		snprintf(kb, sizeof(kb), "key down vk=0x%X %s%s", (unsigned)key,
		         (r.k == vk_routing::kind::pad_button) ? "pad" : "key",
		         (shift & TVP_SS_REPEAT) ? " (repeat)" : "");
		NavLog(kb);
	}
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
	// キー捕捉中: キーアップは捨てる
	if (_impl->KeyCaptureTarget()) return true;
	// ホストホットキー: up は vk のみ一致でバイパス (修飾キー変化で漏らさない)
	if (_impl->HostHotkeyBypass(key, shift, /*isUp=*/true)) return false;
	Impl::Instance* f = _impl->TopmostKeyboardFocus();
	if (!f || !f->session) return false;
	auto r = RouteVk(key);
	if (NavLogEnabled() && r.k != vk_routing::kind::none) {
		char kb[64];
		snprintf(kb, sizeof(kb), "key up   vk=0x%X", (unsigned)key);
		NavLog(kb);
	}
	bool handled = false;
	switch (r.k) {
		case vk_routing::kind::key: {
			int mods = FlagsToElementsMods(shift) | r.extra_mods;
			handled = f->session->on_key_up(r.key, mods);
			break;
		}
		case vk_routing::kind::pad_button:
			handled = f->session->on_pad_button(r.pad, /*down=*/false);
			// 「離し」は背面のインスタンスにも配る。
			//
			// ⚠ この配送**単体では**「覆われている間ずっと押したまま戻る」
			//    ケースを防げない (離しが発生しないため)。それを防いでいるのは
			//    elements 側の「suspend 中は軸の押下値も消す」(view.cpp) の方。
			//    ここは「離しが届くのが suspend フラグの適用より前」という
			//    狭いレースを埋める二重化として置いてある。
			//    実測の内訳は doc/ElementsAudit.md §5 を参照。
			// 離しはどの view に届いても状態を戻すだけなので副作用は無い。
			for (auto& up : _impl->instances) {
				Impl::Instance* inst = up.get();
				if (inst == f || !inst->active || !inst->session) continue;
				inst->session->on_pad_button(r.pad, /*down=*/false);
			}
			break;
		case vk_routing::kind::none:
			handled = false;
			break;
	}
	return f->modal ? true : handled;
}

bool tTVPElementsDialogManager::ForwardKeyPress(tjs_char key)
{
	// キー捕捉中: 文字入力は捨てる (キー押下として捕捉先へ渡し済み)
	if (_impl->KeyCaptureTarget()) {
		_impl->pending_high_surrogate = 0;
		return true;
	}
	Impl::Instance* f = _impl->TopmostKeyboardFocus();
	if (!f || !f->session) {
		_impl->pending_high_surrogate = 0;   // フォーカス喪失時は保持もクリア
		return false;
	}
	// 非モーダルパネル (grabFocus=true 含む) は、 実際にテキストを消費する
	// ウィジェット (input_box 等) に focus が入っていなければ素通しする。
	// ここで無条件に消費すると、 パネルを開いたままゲーム側 (KAG Edit レイヤや
	// window.onKeyPress) へ文字が一切届かなくなる。 未処理キーの pass-through
	// (ForwardKeyDown の handled 素通し) と同じ契約。 modal はゲームへ漏らさない。
	if (!f->modal && !f->session->focus_consumes_text()) {
		_impl->pending_high_surrogate = 0;
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

	// 制御文字 (WINVER の WM_CHAR は BS=0x08 / Enter=0x0D / Tab / Esc も文字と
	// して配信する) はテキストとして入力欄へ流さない。 編集キーとしての処理は
	// ForwardKeyDown (キーイベント) 側が済ませており、 ここで on_text_input へ
	// 通すと入力欄に制御文字が挿入されて「BS で消えない」ように見える。
	// テキスト focus 中なのでゲームへも漏らさない (消費して捨てる)。
	if (cp < 0x20 || cp == 0x7F) return true;

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
	// キー捕捉中: 文字入力は捨てる
	if (_impl->KeyCaptureTarget()) return true;
	Impl::Instance* f = _impl->TopmostKeyboardFocus();
	if (!f || !f->session || !utf8_text) return false;
	// 非モーダルパネルは input_box 等に実際に focus が無ければ素通し
	// (ForwardKeyPress と同じ契約。 無条件消費するとパネル表示中に
	// ゲーム側へ文字が届かなくなる)。 modal はゲームへ漏らさない。
	if (!f->modal && !f->session->focus_consumes_text()) return false;
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
