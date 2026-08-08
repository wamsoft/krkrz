//---------------------------------------------------------------------------
// WINVER host: モーダル Elements ダイアログ実行 (overlay-modal) 実装
//
// 宣言は common/visual/elements/ElementsModalRunner.h。 SDL host は
// SDLElementsModalRunner.cpp が独立 SDL_Window + 自前 SDL_PollEvent の nested pump
// で実装する。 WINVER host はゲーム window 上の overlay-modal を Win32 の nested
// メッセージ pump で実装する (描画は VSyncTimingThread が別途 Show() を駆動するので、
// この pump は入力処理 + engine イベント配送 + ダイアログが閉じるまでのブロックを担う)。
//
// 対応:
//   - TVPRunElementsModalOverlay       … ゲーム window 上の overlay-modal (本命)。
//   - TVPRunElementsFlowOverlay*        … navigator フローの overlay-modal。
//   - TVPRunElementsModalWindow        … 独立ウィンドウ modal は WINVER 未対応
//     (別 OS ウィンドウを立てない)。overlay-modal で代替する (title/size は無視)。
//---------------------------------------------------------------------------
#include "tjsCommHead.h"

#ifdef __WINVER__

#include "ElementsModalRunner.h"
#include "ElementsDialogManager.h"
#include "DialogEventHandler.h" // iTVPDialogEventHandler
#include "DebugIntf.h"          // TVPAddImportantLog
#include "Application.h"        // Application->MainWindowForm / GetMainWindowHandle
#include "WindowFormUnit.h"     // TTVPWindowForm::GetHandle (完全型)
#include "SystemControl.h"      // TVPSystemControl
#include "CharacterSet.h"       // TVPUtf8ToUtf16 / TVPUtf16ToUtf8

#include <windowsx.h>           // GET_X_LPARAM / GET_Y_LPARAM
#include <elements_modal/modal.h>
#include <elements_modal/win32_input.h>
#include <memory>
#include <vector>
#include <string>

namespace {

// ダイアログ (handler) が閉じるまでゲーム window の Win32 メッセージを nested で
// pump し、 閉じたら結果を取り出す。 描画は VSyncTimingThread が Show() を毎フレーム
// 駆動するのでここでは行わない。 TranslateMessage が WM_KEYDOWN → WM_CHAR を生成し、
// DispatchMessage が WndProc → 入力 intercept → ForwardMouse/Key/Text へ配送する。
void PumpModalLoop(tTVPElementsDialogManager& mgr,
                   iTVPDialogEventHandler* handler,
                   tTVPElementsModalResult& out_result)
{
	while (mgr.IsHandlerActive(handler)) {
		bool quit = false;
		MSG msg;
		while (::PeekMessage(&msg, NULL, 0, 0, PM_REMOVE)) {
			if (msg.message == WM_QUIT) {
				// アプリ終了要求。 ダイアログを閉じ、 WM_QUIT を再 post して
				// modal を抜けた後にメインループが正しく終了できるようにする。
				mgr.Close(handler);
				::PostQuitMessage((int)msg.wParam);
				quit = true;
				break;
			}
			::TranslateMessage(&msg);   // WM_KEYDOWN → WM_CHAR (テキスト入力)
			::DispatchMessage(&msg);    // → WndProc → OnMouse/OnKey → dialog intercept
			if (!mgr.IsHandlerActive(handler)) break;
		}
		if (quit || !mgr.IsHandlerActive(handler)) break;

		// engine イベント配送 (背後のゲーム / タイマー / 遅延タスクを継続させる。
		// overlay-modal は入力だけブロックし、 ゲーム自体は動き続ける想定)。
		if (TVPSystemControl) TVPSystemControl->ApplicationIdle();

		::Sleep(4);   // CPU を明け渡す (描画は VSyncTimingThread 側)
	}

	// close_on_click / フロー <exit> で finish した場合は mgr が結果をスナップ済み。
	// Esc / 外部 Close 等で結果が無い場合は空のまま返る。
	mgr.TakeLastModalResult(handler, out_result.Action, out_result.Values);
}

} // anonymous

//---------------------------------------------------------------------------
bool TVPRunElementsModalOverlay(
	const std::string& json_utf8,
	iTVPDialogEventHandler* handler,
	tTVPElementsModalResult& out_result,
	const std::map<ttstr, ttstr>* initial_vars)
{
	auto& mgr = tTVPElementsDialogManager::Instance();
	if (mgr.IsHandlerActive(handler)) {
		TVPAddImportantLog(TJS_W("ElementsModal overlay: this handler already active"));
		return false;
	}
	// modal=true で背景の非モーダル UI / ゲームへ入力を通さない。
	if (!mgr.ShowFromJsonString(json_utf8, handler, nullptr, /*modal=*/true)) {
		return false;
	}
	// pump 前に初期変数を注入 (subscribe 済 widget が表示へ反映する)。
	if (initial_vars) {
		for (const auto& kv : *initial_vars) mgr.SetVar(handler, kv.first, kv.second);
	}
	PumpModalLoop(mgr, handler, out_result);
	return true;
}

//---------------------------------------------------------------------------
// 独立 OS ウィンドウ modal (WINVER)。SDL host が elements_modal::run_modal
// (SDL_Window/Renderer 内蔵) で行うのに相当する処理を、Win32 窓 + overlay_session
// (host 中立) + GDI StretchDIBits present で自前実装する。crosshost サンプル
// (examples/overlay_crosshost.cpp の Win32 経路) が実装テンプレ。
//---------------------------------------------------------------------------
namespace {

// elements_modal::value_t → tTJSVariant
tTJSVariant ModalValueToVariant(const elements_modal::value_t& v)
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
			tjs_string u16; TVPUtf8ToUtf16(u16, val);
			out = ttstr(u16.c_str());
		}
	}, v);
	return out;
}

// state widget 値変化 / button click を handler->OnAction へ即時通知する bridge。
elements_modal::event_callback MakeHandlerBridge(iTVPDialogEventHandler* handler)
{
	if (!handler) return {};
	return [handler](const std::string& id, bool is_button_click,
	                 const elements_modal::value_t& payload) {
		tjs_string ts_id; TVPUtf8ToUtf16(ts_id, id);
		ttstr id_tt(ts_id.c_str());
		if (is_button_click) {
			tTJSVariant empty;
			handler->OnAction(id_tt, empty);
		} else {
			tTJSVariant v = ModalValueToVariant(payload);
			handler->OnAction(id_tt, v);
		}
	};
}

// elements_modal::result → tTVPElementsModalResult
void ToKrkrzResult(const elements_modal::result& src, tTVPElementsModalResult& out)
{
	{
		tjs_string u16; TVPUtf8ToUtf16(u16, src.action);
		out.Action = ttstr(u16.c_str());
	}
	for (const auto& kv : src.values) {
		tjs_string u16_id; TVPUtf8ToUtf16(u16_id, kv.first);
		out.Values.emplace(ttstr(u16_id.c_str()), ModalValueToVariant(kv.second));
	}
}

// モーダル窓の状態 (WndProc から GWLP_USERDATA 経由で参照)。
struct ModalWinState
{
	elements_modal::overlay_session* session = nullptr;
	std::vector<std::uint32_t>       staging;      // ARGB8888 = メモリ上 BGRA
	int   view_w = 0, view_h = 0;                  // logical
	int   pw = 0, ph = 0;                          // physical (= logical × scale)
	float scale = 1.0f;
};

inline float ToLogical(int phys, float scale) { return scale > 0.0f ? float(phys) / scale : float(phys); }

// staging (物理 pw×ph) を client 全面へ top-down DIB で present。
void PresentModal(HWND hwnd, ModalWinState& st)
{
	if (!st.session) return;
	elements_modal::overlay_session::render_rect rect;
	bool drew = st.session->render_to_buffer(st.staging.data(), st.pw, st.ph,
	                                          st.view_w, st.view_h, rect);
	if (!drew) return;

	BITMAPINFO bmi{};
	bmi.bmiHeader.biSize        = sizeof(BITMAPINFOHEADER);
	bmi.bmiHeader.biWidth       = st.pw;
	bmi.bmiHeader.biHeight      = -st.ph;   // top-down
	bmi.bmiHeader.biPlanes      = 1;
	bmi.bmiHeader.biBitCount    = 32;
	bmi.bmiHeader.biCompression = BI_RGB;

	RECT cr; ::GetClientRect(hwnd, &cr);
	HDC dc = ::GetDC(hwnd);
	::SetStretchBltMode(dc, HALFTONE);
	::StretchDIBits(dc, 0, 0, cr.right - cr.left, cr.bottom - cr.top,
		0, 0, st.pw, st.ph, st.staging.data(), &bmi, DIB_RGB_COLORS, SRCCOPY);
	::ReleaseDC(hwnd, dc);
}

LRESULT CALLBACK ModalWndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
	auto* st = reinterpret_cast<ModalWinState*>(::GetWindowLongPtr(hwnd, GWLP_USERDATA));
	namespace wi = elements_modal::win32_input;
	if (st && st->session) {
		auto& s = *st->session;
		switch (msg) {
		case WM_PAINT: {
			PAINTSTRUCT ps; ::BeginPaint(hwnd, &ps);
			PresentModal(hwnd, *st);
			::EndPaint(hwnd, &ps);
			return 0;
		}
		case WM_LBUTTONDOWN: case WM_RBUTTONDOWN: case WM_MBUTTONDOWN:
			::SetCapture(hwnd);
			s.on_mouse_down(ToLogical(GET_X_LPARAM(lp), st->scale), ToLogical(GET_Y_LPARAM(lp), st->scale),
				msg == WM_RBUTTONDOWN ? wi::mouse_right() : msg == WM_MBUTTONDOWN ? wi::mouse_middle() : wi::mouse_left(),
				wi::mods());
			::InvalidateRect(hwnd, nullptr, FALSE);
			return 0;
		case WM_LBUTTONUP: case WM_RBUTTONUP: case WM_MBUTTONUP:
			::ReleaseCapture();
			s.on_mouse_up(ToLogical(GET_X_LPARAM(lp), st->scale), ToLogical(GET_Y_LPARAM(lp), st->scale),
				msg == WM_RBUTTONUP ? wi::mouse_right() : msg == WM_MBUTTONUP ? wi::mouse_middle() : wi::mouse_left(),
				wi::mods());
			::InvalidateRect(hwnd, nullptr, FALSE);
			return 0;
		case WM_MOUSEMOVE:
			s.on_mouse_move(ToLogical(GET_X_LPARAM(lp), st->scale), ToLogical(GET_Y_LPARAM(lp), st->scale), wi::mods());
			::InvalidateRect(hwnd, nullptr, FALSE);
			return 0;
		case WM_MOUSEWHEEL:
			s.on_mouse_wheel(0.0f, (float)GET_WHEEL_DELTA_WPARAM(wp) / (float)WHEEL_DELTA, 0.0f, 0.0f);
			::InvalidateRect(hwnd, nullptr, FALSE);
			return 0;
		case WM_KEYDOWN: case WM_SYSKEYDOWN:
			s.on_key_down(wi::key((unsigned)wp), wi::mods());
			::InvalidateRect(hwnd, nullptr, FALSE);
			return 0;
		case WM_KEYUP: case WM_SYSKEYUP:
			s.on_key_up(wi::key((unsigned)wp), wi::mods());
			return 0;
		case WM_CHAR:
			if (wp >= 0x20 && wp != 0x7f) {
				wchar_t wc = (wchar_t)wp;
				char utf8[8];
				int n = ::WideCharToMultiByte(CP_UTF8, 0, &wc, 1, utf8, sizeof(utf8) - 1, nullptr, nullptr);
				if (n > 0) { utf8[n] = '\0'; s.on_text_input(utf8); ::InvalidateRect(hwnd, nullptr, FALSE); }
			}
			return 0;
		case WM_CLOSE:
			s.close();   // Esc 相当 (空 action)
			return 0;
		}
	}
	return ::DefWindowProc(hwnd, msg, wp, lp);
}

// モーダル窓クラスを一度だけ登録。
const wchar_t* EnsureModalWindowClass()
{
	static const wchar_t* kClass = L"TVPElementsModalWindow";
	static bool registered = false;
	if (!registered) {
		WNDCLASSW wc{};
		wc.lpfnWndProc   = ModalWndProc;
		wc.hInstance     = ::GetModuleHandle(nullptr);
		wc.hCursor       = ::LoadCursor(nullptr, IDC_ARROW);
		wc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
		wc.lpszClassName = kClass;
		::RegisterClassW(&wc);
		registered = true;
	}
	return kClass;
}

UINT ModalGetDpi(HWND hwnd)
{
	if (HMODULE u = ::GetModuleHandleW(L"user32")) {
		typedef UINT (WINAPI *GetDpiForWindowFn)(HWND);
		if (auto f = (GetDpiForWindowFn)::GetProcAddress(u, "GetDpiForWindow")) {
			UINT d = f(hwnd);
			if (d) return d;
		}
	}
	return 96;
}

} // anonymous

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

	// フォント + ThorVG runtime を先に初期化 (elements_modal は自前 font 登録しない)。
	tTVPElementsDialogManager::Instance().EnsureRuntimeInitialized();

	// 親 = ゲームメインウィンドウ (モーダル中は無効化する)。
	HWND parent = nullptr;
	if (Application) {
		if (auto* form = Application->MainWindowForm()) parent = form->GetHandle();
	}

	// title utf-16
	std::wstring wtitle;
	{
		if (title.IsEmpty()) wtitle = L"Dialog";
		else wtitle.assign(reinterpret_cast<const wchar_t*>(title.c_str()));
	}

	const wchar_t* cls = EnsureModalWindowClass();

	// まず暫定サイズで作って実 DPI を得る (per-monitor V2 = manifest)。
	HWND hwnd = ::CreateWindowExW(WS_EX_DLGMODALFRAME, cls, wtitle.c_str(),
		WS_CAPTION | WS_SYSMENU, CW_USEDEFAULT, CW_USEDEFAULT, width, height,
		parent, nullptr, ::GetModuleHandle(nullptr), nullptr);
	if (!hwnd) {
		TVPAddImportantLog(TJS_W("ElementsModal: CreateWindow failed → overlay で代替"));
		return TVPRunElementsModalOverlay(json_utf8, handler, out_result);
	}

	ModalWinState st;
	// DPI は親 (= モーダルを中央配置する先) の値を優先。親と同一モニタになるので
	// CW_USEDEFAULT 位置の暫定 DPI より正確 (異DPIマルチモニタでのズレを防ぐ)。
	st.scale  = (float)ModalGetDpi(parent ? parent : hwnd) / 96.0f;
	st.view_w = width;
	st.view_h = height;
	st.pw = (int)(width  * st.scale + 0.5f);
	st.ph = (int)(height * st.scale + 0.5f);
	st.staging.assign((std::size_t)st.pw * st.ph, 0u);

	elements_modal::overlay_session session;
	if (!session.start(json_utf8, width, height, st.scale, MakeHandlerBridge(handler))) {
		::DestroyWindow(hwnd);
		TVPAddImportantLog(TJS_W("ElementsModal: overlay_session.start failed"));
		return false;
	}
	st.session = &session;
	::SetWindowLongPtr(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(&st));

	// client が物理 pw×ph になるよう外形を調整し、親の中央へ配置。
	RECT rc{ 0, 0, st.pw, st.ph };
	::AdjustWindowRect(&rc, WS_CAPTION | WS_SYSMENU, FALSE);
	int ww = rc.right - rc.left, wh = rc.bottom - rc.top;
	int px = CW_USEDEFAULT, py = CW_USEDEFAULT;
	if (parent) {
		RECT pr; ::GetWindowRect(parent, &pr);
		px = pr.left + ((pr.right - pr.left) - ww) / 2;
		py = pr.top  + ((pr.bottom - pr.top) - wh) / 2;
	}
	::SetWindowPos(hwnd, HWND_TOP, px, py, ww, wh,
		(px == CW_USEDEFAULT ? SWP_NOMOVE : 0));

	// モーダル: 親を無効化して入力をこの窓へ集約。
	bool parent_was_enabled = parent && ::IsWindowEnabled(parent);
	if (parent_was_enabled) ::EnableWindow(parent, FALSE);
	::ShowWindow(hwnd, SW_SHOW);
	::SetForegroundWindow(hwnd);
	::SetFocus(hwnd);

	// nested メッセージループ (session.finished() まで)。
	MSG msg;
	bool running = true;
	while (running) {
		while (::PeekMessage(&msg, NULL, 0, 0, PM_REMOVE)) {
			if (msg.message == WM_QUIT) {
				session.close();
				::PostQuitMessage((int)msg.wParam);
				running = false;
				break;
			}
			::TranslateMessage(&msg);   // WM_KEYDOWN → WM_CHAR
			::DispatchMessage(&msg);
			if (session.finished()) { running = false; break; }
		}
		if (!running) break;
		if (session.finished()) break;
		// アニメ (focus ring 等) 進行のため再描画。
		::InvalidateRect(hwnd, nullptr, FALSE);
		::Sleep(16);
	}

	// 親を復帰してから窓を破棄 (Z 順・フォーカスが親へ戻る)。
	if (parent_was_enabled) ::EnableWindow(parent, TRUE);
	if (parent) ::SetForegroundWindow(parent);

	ToKrkrzResult(session.get_result(), out_result);

	::SetWindowLongPtr(hwnd, GWLP_USERDATA, 0);
	st.session = nullptr;
	::DestroyWindow(hwnd);
	return true;
}

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
	if (!mgr.StartFlowFromManifest(manifest_path, handler, nullptr, /*modal=*/true)) {
		return false;
	}
	PumpModalLoop(mgr, handler, out_result);
	return true;
}

//---------------------------------------------------------------------------
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
	if (!mgr.StartFlowFromScreens(screens, entry, handler, nullptr, /*modal=*/true)) {
		return false;
	}
	PumpModalLoop(mgr, handler, out_result);
	return true;
}

#endif // __WINVER__
