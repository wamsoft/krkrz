#include "tjsCommHead.h"
#include "StorageIntf.h"
#include "CharacterSet.h"
#include "MsgIntf.h"
#include "LogIntf.h"
#include "DebugIntf.h"
#include "VideoOvlIntf.h"

#if !defined(_WIN32)
#include "VirtualKey.h"
#endif

#include "app.h"
#include "OpenGLContext.h"
#include "MsgImpl.h"       // TVPCannotShowModal* (generic)
#include "WindowIntf.h"    // TVPGetWindowCount
#ifdef KRKRZ_USE_REPL
#include "REPL.h"          // TVPDrainREPL: modal 中も REPL/agent を回す
#endif

#ifdef KRKRZ_HAS_ELEMENTS
#include "elements/ElementsDialogManager.h"
#endif

#include <stdio.h>
#include <string>
#include <unordered_map>

// ----------------------------------------------------
// glad 初期化用
// ----------------------------------------------------



// ----------------------------------------------------
// SDL3WindowForm 実装
// ----------------------------------------------------

// ホスト (ゲーム本体) 向けテキスト入力のベースライン有効化を現在の状況に合わせる。
//
// デスクトップは常時有効でよい。 オンスクリーンキーボードを持つ環境 (NX / PS5 /
// Android) は SDL_StartTextInput がキーボード UI を出してしまう (NX ならブロッキング
// の swkbd アプレット、 PS5 なら SceImeDialog) ため、 従来は一切呼んでいなかった。
// しかしそれだと物理 (USB) キーボードを繋いだときに SDL_SendKeyboardText() が
// SDL_TextInputActive() のゲートで捨てられ、 ESC や矢印などのキーイベントだけ届いて
// 文字が一切入力できない (NX 実機で確認)。
//
// 物理キーボードがある場合は SDL 側の自動判定 (AutoShowingScreenKeyboard) が
// キーボード UI の表示を抑止するので、 有効化してよい。 キーボードは実行中に
// 抜き差しされるので SDL_EVENT_KEYBOARD_ADDED / REMOVED で追従する。
static void TVPUpdateBaselineTextInput(SDL_Window* window)
{
	if (!window) return;
	if (!SDL_HasScreenKeyboardSupport()) {
		SDL_StartTextInput(window);
		return;
	}
	if (SDL_HasKeyboard()) {
		SDL_StartTextInput(window);
	} else {
		// 物理キーボード無し。 ここで有効のままにすると OS のキーボード UI が
		// 出るので止める。 入力手段はゲーム側のソフトキーボードか、 Elements の
		// フォーカス駆動 (UpdateFocusDrivenTextInput) が担当する。
		SDL_StopTextInput(window);
	}
}

SDL3WindowForm::SDL3WindowForm(class tTJSNI_Window* win)
 : TTVPWindowForm(win)
 , mWindow(nullptr)
 , mVisible(false)
 , mBorderStyle(bsSizeable)
 , mMinWidth(0), mMinHeight(0), mMaxWidth(0), mMaxHeight(0)
 , mStartupDisplayApplied(false)
 , mFixedSurfaceWidth(0), mFixedSurfaceHeight(0)
{
	SDL_WindowFlags flags = SDL_WINDOW_HIDDEN;
#if defined(TVP_USE_OPENGL)
	flags |= SDL_WINDOW_OPENGL;
#endif
#if defined(__ANDROID__) || defined(__IOS__) || defined(__ORBIS__) || defined (__PROSPERO__)
	flags |= SDL_WINDOW_FULLSCREEN;
	int width  = 1920;
	int height = 1080; 
	// 常にフルスクリーンの環境では、 SDL が返すウィンドウサイズは実
	// フレームバッファ (PS5 なら 3840x2160) になる。 描画の基準面まで
	// それに合わせると、 1920x1080 で作った UI が実画面の中央に原寸で
	// 出てしまう (Elements の overlay は縮小はしても拡大しないため)。
	// 基準面はここで要求した論理サイズに固定し、 実サイズへの引き伸ばしは
	// SDL_SetRenderLogicalPresentation に任せる。
	mFixedSurfaceWidth  = width;
	mFixedSurfaceHeight = height;
#else
	flags |= SDL_WINDOW_RESIZABLE;
	int width = 32;
	int height = 32;
#endif
	// ウィンドウ作成
	mWindow = SDL_CreateWindow("", width, height, flags);
	ApplyAspectLock();   // 比率固定が先に設定されていれば反映
	if (mWindow) {
		// ウィンドウのユーザーデータとして自身を設定
		//SDL_SetWindowFullscreen(mWindow, true);
		SDL_SetPointerProperty(SDL_GetWindowProperties(mWindow), "form", this);
		// -display= 指定があれば、まだ非表示のうちに目的のディスプレイへ移して
		// おく (スクリプトが位置を指定した場合は初回表示時に再度寄せ直す)
		TVPMoveWindowToStartupDisplay(mWindow);
		// テキスト入力を有効化 (SDL_EVENT_TEXT_INPUT を発生させ、KAG の Edit
		// レイヤ等へ文字入力を配送できるようにする。未呼出だと TEXT_INPUT が
		// 一切発生せず、ゲーム側 (タイトル画面の名前入力等) のタイプ入力が
		// 通らない)。
		// ただしオンスクリーンキーボードを持つ環境では SDL_StartTextInput が
		// キーボード UI を即座に出してしまう (NX/Ounce ではブロッキングの
		// swkbd アプレット起動、Android ではソフトキーボード表示) ため、
		// 起動時のベースライン有効化は物理キーボード環境限定とする。
		// これらの環境で物理キーボードが無い場合は Elements 側のフォーカス駆動
		// (ElementsDialogManager::UpdateFocusDrivenTextInput) が
		// テキスト欄に focus が入ったときにのみ開始する。
		// なお NX では起動直後 (キーボードのポーリング前) はまだ
		// SDL_HasKeyboard() が false なので、 実際の有効化は後述の
		// SDL_EVENT_KEYBOARD_ADDED 受信時に行われる。
		TVPUpdateBaselineTextInput(mWindow);
	} else {
		const char *error = SDL_GetError();
		TVPLOG_ERROR("SDL3WindowForm: Failed to create SDL Window: {}", error);
	}
	mEnableTouch = checkTouchDevice();
}

bool 
SDL3WindowForm::checkTouchDevice()
{
	int count = 0;
	SDL_TouchID * touchIDS = SDL_GetTouchDevices(&count);
	if (touchIDS) {
		for (int i = 0; i < count; ++i) {
			SDL_TouchID touchID = touchIDS[i];
			const char *name = SDL_GetTouchDeviceName(touchID);
			TVPLOG_DEBUG("Touch Device ID: {}, Name: {}", touchID, name);
		}
		SDL_free(touchIDS);
	}
	return count > 0;
}


SDL3WindowForm::~SDL3WindowForm()
{
	DestroyNativeWindow();
}

#if defined(TVP_USE_OPENGL)
// ウィンドウ毎の「主コンテキスト」(画面描画デバイス用) を一度だけ確定してキャッシュする。
//
// ★なぜ必要か: separateShared=false は本来「ウィンドウの主コンテキストを返す」意味だが、
//   SDL 実装が SDL_GL_GetCurrentContext() (=主ではなく "現在" のコンテキスト) を返して
//   いたため、画面デバイスがコンテキストを確定する初回ペイント時点で、先に生成された
//   GLCompositor の分離コンテキストが makeCurrent されていると、それを主コンテキストと
//   誤って採用してしまう。結果、画面デバイスと compositor が同一 GL コンテキストに縮退し、
//   capture の FBO/ステートが画面提示 (FBO0 present) を壊して画面が黒くなる。
//   → 主コンテキストをウィンドウ毎に一度だけ確定してキャッシュし、以降はカレントに依らず
//     同じものを返す。初回確定は compositor 生成前 (SetWindowInterface / InitGLES 時) に
//     行われるため、真の主コンテキストが捕捉される。主コンテキストはウィンドウ生存中は
//     破棄しない (従来の false 分岐と同じ寿命)。
static std::unordered_map<SDL_Window*, SDL_GLContext> sMainGLContexts;

// GLES コンテキストを生成する。要求バージョン (main.cpp で設定する既定 3.2) で
// 作れない場合は 3.1 → 3.0 → 2.0 と下げて再試行する。
// 例: ANGLE の D3D11 バックエンド (EGL フォールバック時 / -forceegl=yes 時) は
// ES 3.1 までしか作れず、3.2 要求は EGL_BAD_ATTRIBUTE で失敗する。
// 成功した属性はそのまま残るので、以降のコンテキスト生成 (compositor の分離
// コンテキスト等) も同じバージョンで作られる。
// 実際に得られたバージョンは後段の InitGLES が "Loaded GLES x.y" でログに出す。
SDL_GLContext TVPCreateGLContextWithFallback(SDL_Window *window)
{
	SDL_GLContext ctx = SDL_GL_CreateContext(window);
	if (ctx) return ctx;
	static const int candidates[][2] = { {3,1}, {3,0}, {2,0} };
	for (const auto &v : candidates) {
		TVPLOG_WARNING("SDL_GL_CreateContext failed ({}); retrying with GLES {}.{}",
		               SDL_GetError(), v[0], v[1]);
		SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, v[0]);
		SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, v[1]);
		ctx = SDL_GL_CreateContext(window);
		if (ctx) return ctx;
	}
	// ここで失敗すると後段の gladLoadGLES2 も必ず失敗する。原因究明の
	// 手がかり (SDL のエラーと環境要件) をこの時点でログに残す。
	TVPLOG_ERROR("SDL_GL_CreateContext failed: {}", SDL_GetError());
	TVPLOG_ERROR("GLES context is unavailable. On Windows this needs either");
	TVPLOG_ERROR("  - an OpenGL driver with ES profile support");
	TVPLOG_ERROR("    (WGL_EXT_create_context_es2_profile), or");
	TVPLOG_ERROR("  - ANGLE's libEGL.dll / libGLESv2.dll next to the executable");
	TVPLOG_ERROR("    (required when -forceegl=yes).");
	return nullptr;
}

static SDL_GLContext TVPGetOrCreateMainGLContext(SDL_Window *window)
{
	auto it = sMainGLContexts.find(window);
	if (it != sMainGLContexts.end() && it->second) return it->second;
	// 初回のみ: 現行の主コンテキストを採用 (無ければ生成) してキャッシュ。
	// 複数ウィンドウ (Window を 2 枚目以降作った場合) では、2 枚目以降も
	// 1 枚目のコンテキストを共有する (SDL は同じピクセルフォーマットなら
	// MakeCurrent 先のウィンドウを差し替えられる)。 ウィンドウ毎に別コンテキスト
	// にすると、レイヤ更新時のテクスチャアップロードが「そのときカレントの」
	// コンテキストへ行ってしまい、他方のウィンドウの画面が更新されなくなる。
	// 共有ぶんの破棄は DestroyNativeWindow が参照数を見て最後の 1 つで行う。
	SDL_GLContext ctx = SDL_GL_GetCurrentContext();
	if (!ctx) ctx = TVPCreateGLContextWithFallback(window);
	sMainGLContexts[window] = ctx;
	return ctx;
}
#endif // TVP_USE_OPENGL

void
SDL3WindowForm::DestroyNativeWindow()
{
	if (mWindow) {
#if defined(TVP_USE_OPENGL)
		// OpenGLコンテキストを破棄。
		// このウィンドウの主コンテキスト (キャッシュ) があればそれを確定的に破棄し、
		// エントリを消す。compositor の分離コンテキストは所有元 (SDL3GLContext,
		// mOwned=true) の Release で破棄されるためここでは触らない (カレントを無条件
		// 破棄すると、既に破棄済みの compositor コンテキストを二重破棄する恐れがある)。
		// ★キャッシュに無い = このウィンドウは GL コンテキストを一度も作って
		//   いない。ここで「カレントを破棄」してはならない: 複数ウィンドウ時、
		//   カレントは他ウィンドウ (通常はメインウィンドウ) の主コンテキストで
		//   あり、サブウィンドウを閉じただけでメイン画面の描画が死ぬ。
		//   また主コンテキストは複数ウィンドウで共有されうるので、他のウィンドウが
		//   まだ参照している間は破棄しない (最後の 1 つを閉じたときだけ破棄)。
		SDL_GLContext shared_ctx = nullptr;   // 生存側へ張り直すコンテキスト
		SDL_Window *survivor = nullptr;
		auto it = sMainGLContexts.find(mWindow);
		if (it != sMainGLContexts.end()) {
			SDL_GLContext ctx = it->second;
			sMainGLContexts.erase(it);
			for (const auto &kv : sMainGLContexts) {
				if (kv.second == ctx) { survivor = kv.first; break; }
			}
			if (ctx && !survivor) {
				SDL_GL_DestroyContext(ctx);
			} else {
				shared_ctx = ctx;
			}
		}
#endif
		// ウィンドウを破棄
		SDL_DestroyWindow(mWindow);
		mWindow = nullptr;
#if defined(TVP_USE_OPENGL)
		// 共有コンテキストが残っているなら、カレントを生存ウィンドウへ張り直す。
		// SDL_DestroyWindow は破棄対象がカレントだった場合にコンテキストの
		// カレントを外すため、張り直さないと直後のテクスチャ更新が
		// 「カレント無し」で黙って捨てられ、残ったウィンドウの画面が古いまま
		// 止まる (サブウィンドウを閉じるとメイン画面が更新されなくなる)。
		if (shared_ctx && survivor) SDL_GL_MakeCurrent(survivor, shared_ctx);
#endif
	}
}

void 
SDL3WindowForm::OnCloseCancel()
{
	// SDLではCloseイベントを直接キャンセルする必要無し
	// 単にCloseイベントを無視する。CLose 時はオブジェクト側から破棄処理が走る
}

void
SDL3WindowForm::GetSurfaceSize(int &w, int &h) const
{
	if (mFixedSurfaceWidth > 0 && mFixedSurfaceHeight > 0) {
		w = mFixedSurfaceWidth;
		h = mFixedSurfaceHeight;
	} else if (mWindow) {
		SDL_GetWindowSize(mWindow, &w, &h);
	} else {
		w = 0;
		h = 0;
	}
}

void
SDL3WindowForm::ResizeWindow(int w, int h)
{
	if (mWindow) {
		SDL_SetWindowSize(mWindow, w, h);
		// min/max でクランプされることがあるので、要求値ではなく実サイズを
		// 基底の surface サイズ (ビューポート計算の外側サイズ) へ反映する。
		// 論理サーフェスを固定する環境 (常にフルスクリーン) ではその値を使う
		// (実サイズを流し込むと DestRect が実フレームバッファ基準になり、
		//  論理サイズで提示している分だけ拡大されてしまう)。
		int aw = w, ah = h;
		GetSurfaceSize(aw, ah);
		TTVPWindowForm::ResizeWindow(aw, ah);
	} else {
		TTVPWindowForm::ResizeWindow(w, h);
	}
}

void
SDL3WindowForm::SetCaption(const tjs_string& caption)
{
	if (mWindow) {
		std::string ncaption;
		TVPUtf16ToUtf8(ncaption, caption);
		SDL_SetWindowTitle(mWindow, ncaption.c_str());
	}
}

// 表示制御
bool 
SDL3WindowForm::GetVisible() const
{
	return mVisible;
}

void 
SDL3WindowForm::SetVisible(bool b)
{
	if (mVisible != b) {
		mVisible = b;
		if (mWindow) {
			if (mVisible) {
				// -display= 指定時、初回表示の直前に目的のディスプレイへ寄せる
				// (スクリプトが setPos した後でも拾えるようにするため。以降は
				//  ユーザがウィンドウを動かせるよう一度きりの適用とする)
				if (!mStartupDisplayApplied) {
					mStartupDisplayApplied = true;
					TVPMoveWindowToStartupDisplay(mWindow);
				}
				SDL_ShowWindow(mWindow);
			} else {
				SDL_HideWindow(mWindow);
			}
		}
	}
}

//---------------------------------------------------------------------------
// モーダル表示 (Window.showModal)
//
//   SDL3 のイベントループ (SDL_AppEvent/SDL_AppIterate コールバック) は
//   ネストできないので、ここでは既存の SDL_PollEvent + Application の
//   AppEvent/AppIterate/Dispatch を自前で回すネストループを組む
//   (Elements の SDLElementsModalRunner::PumpModalLoop と同じ方式)。
//
//   他ウィンドウの入力抑止は二段構え:
//     1. SDL_SetWindowParent + SDL_SetWindowModal で OS レベルのモーダル化を
//        試みる (Windows / X11 / Wayland で有効)。
//     2. 効かない環境でも、 SDL3Application::AppEvent が
//        TTVPWindowForm::GetModalWindowForm() を見てモーダル以外の
//        ウィンドウ宛ユーザ入力を捨てるので、エンジン内での排他は保証される。
//
//   ウィンドウを複数作れない環境 (モバイル/コンソール等) では、そもそも
//   2 枚目の SDL_CreateWindow が失敗して mWindow が null になるため、
//   ここで「モーダル非対応」例外にする (呼び出し側で出さない設計にする前提)。
//---------------------------------------------------------------------------
#ifdef __EMSCRIPTEN__
// wasm メインループ (sdl3/environ/main.cpp) が定義する JSPI import。
// requestAnimationFrame を await して次フレームまでメインスタックを suspend する。
extern "C" void krkrz_jspi_wait_frame();
#endif

void
SDL3WindowForm::ShowWindowAsModal()
{
	if( !mWindow ) {
		// ウィンドウ自体が作れていない (複数ウィンドウ非対応環境など)
		TVPThrowExceptionMessage(TVPModalWindowIsNotSupported);
	}
	if( GetVisible() || in_mode_ ) {
		TVPThrowExceptionMessage(TVPCannotShowModalAreadyShowed);
	}
	if( TVPGetWindowCount() <= 1 ) {
		// 1 個しか Window が無い時はモーダル化する意味が無いのと、不具合の元
		TVPThrowExceptionMessage(TVPCannotShowModalSingleWindow);
	}

	SDL3Application *app = GetSDL3Application();
	if( !app ) TVPThrowExceptionMessage(TVPModalWindowIsNotSupported);

	// 親ウィンドウ (メインウィンドウ) を得て OS レベルのモーダル化を試みる
	SDL_Window *parent = nullptr;
	if( TTVPWindowForm *mainform = Application->MainWindowForm() ) {
		if( mainform != this ) parent = (SDL_Window*)mainform->NativeWindowHandle();
	}
	// SDL は「閉じる要求が来たウィンドウが最後のトップレベル可視ウィンドウなら
	// SDL_EVENT_QUIT を投げる」(SDL_windowevents.c)。 親付き (= 子) ウィンドウは
	// この数に入らないため、 モーダルの × を押すと「本体 1 枚しか残っていない」と
	// 判定されてアプリごと終了してしまう。 モーダル中だけ自動終了を止める。
	const char *prev_quit_hint = SDL_GetHint(SDL_HINT_QUIT_ON_LAST_WINDOW_CLOSE);
	std::string saved_quit_hint = prev_quit_hint ? prev_quit_hint : "";
	SDL_SetHint(SDL_HINT_QUIT_ON_LAST_WINDOW_CLOSE, "0");

	bool os_modal = false;
	if( parent ) {
		if( SDL_SetWindowParent(mWindow, parent) ) {
			os_modal = SDL_SetWindowModal(mWindow, true);
		}
		if( !os_modal ) {
			// 環境が未対応でもエンジン内の入力抑止だけで動くので致命ではない
			TVPAddLog( ttstr(TJS_W("(info) showModal: OS-level modal is unavailable ("))
				+ ttstr(SDL_GetError()) + TJS_W("); using engine-side input blocking only.") );
		}
	}

	in_mode_ = true;
	modal_result_ = TVP_MODAL_NONE;
	PushModalWindowForm(this);

	try {
		SetVisible(true);
		SDL_RaiseWindow(mWindow);

		while( modal_result_ == TVP_MODAL_NONE ) {
			SDL_PumpEvents();
			SDL_Event ev;
			while( SDL_PollEvent(&ev) ) {
				if( ev.type == SDL_EVENT_QUIT ) {
					// 終了要求はモーダルを畳んでから外側へ回す
					modal_result_ = TVP_MODAL_CANCEL;
					SDL_PushEvent(&ev);
					break;
				}
				app->AppEvent(ev);
				if( modal_result_ != TVP_MODAL_NONE ) break;
			}
			if( modal_result_ != TVP_MODAL_NONE ) break;

			app->AppIterate();
			app->SendPadEvent();
			if( !app->IsInBackground() ) app->RequestUpdate();
			app->Dispatch();
#ifdef KRKRZ_USE_REPL
			TVPDrainREPL();   // modal 中も REPL / Agent を処理する
#endif
			if( app->IsTerminated() ) modal_result_ = TVP_MODAL_CANCEL;

#ifdef __EMSCRIPTEN__
			// wasm: SDL_Delay でメインスレッドを止めるとブラウザのイベント
			// ループごと凍るので、 JSPI で次フレームまで suspend する。
			krkrz_jspi_wait_frame();
#else
			SDL_Delay(8);
#endif
		}
	} catch(...) {
		PopModalWindowForm(this);
		in_mode_ = false;
		if( os_modal ) SDL_SetWindowModal(mWindow, false);
		if( parent ) SDL_SetWindowParent(mWindow, nullptr);
		SetVisible(false);
		SDL_SetHint(SDL_HINT_QUIT_ON_LAST_WINDOW_CLOSE,
			saved_quit_hint.empty() ? nullptr : saved_quit_hint.c_str());
		throw;
	}

	PopModalWindowForm(this);
	in_mode_ = false;
	if( os_modal ) SDL_SetWindowModal(mWindow, false);
	if( parent ) SDL_SetWindowParent(mWindow, nullptr);
	SetVisible(false);
	SDL_SetHint(SDL_HINT_QUIT_ON_LAST_WINDOW_CLOSE,
		saved_quit_hint.empty() ? nullptr : saved_quit_hint.c_str());
}

//---------------------------------------------------------------------------
// ウィンドウの位置 / サイズ / 装飾
//
//   generic の既定はすべて空実装 (位置もサイズも 0、枠なし、常に全画面) で、
//   モバイル/コンソールのように「1 枚の全画面ウィンドウしか無い」環境を想定
//   している。 デスクトップの SDL3 では実際の SDL_Window を操作する。
//
//   SDL の window size はクライアント (描画) 領域。 吉里吉里の Window.width /
//   height は装飾込みの外側サイズなので、SDL_GetWindowBordersSize で得た枠幅を
//   足し引きして変換する (取得できない環境では枠 0 として扱う)。
//---------------------------------------------------------------------------
namespace {
struct SDLBorders { int top = 0, left = 0, bottom = 0, right = 0; };

SDLBorders GetBorders(SDL_Window *w)
{
	SDLBorders b;
	if (w) SDL_GetWindowBordersSize(w, &b.top, &b.left, &b.bottom, &b.right);
	return b;
}
} // anonymous

// ウィンドウ移動は必ず TVPSDLSetWindowPositionKeepingSize 経由で行う。
// 別 DPI のモニタへ移してもクライアントの物理ピクセルサイズを維持するため
// (src/core/doc/WindowGeometry.md §4 の DPI ポリシー)。
void
SDL3WindowForm::MoveWindowTo(int l, int t)
{
	if (!mWindow) return;
	TVPSDLSetWindowPositionKeepingSize(mWindow, l, t);
	// surface サイズ (ビューポート計算の外側サイズ) を実値へ同期する
	// (論理サーフェス固定の環境ではその値)
	int w = 0, h = 0;
	GetSurfaceSize(w, h);
	TTVPWindowForm::ResizeWindow(w, h);
}

void
SDL3WindowForm::SetLeft(int l)
{
	if (!mWindow) return;
	int x = 0, y = 0;
	SDL_GetWindowPosition(mWindow, &x, &y);
	MoveWindowTo(l, y);
}

int
SDL3WindowForm::GetLeft() const
{
	if (!mWindow) return 0;
	int x = 0, y = 0;
	SDL_GetWindowPosition(mWindow, &x, &y);
	return x;
}

void
SDL3WindowForm::SetTop(int t)
{
	if (!mWindow) return;
	int x = 0, y = 0;
	SDL_GetWindowPosition(mWindow, &x, &y);
	MoveWindowTo(x, t);
}

int
SDL3WindowForm::GetTop() const
{
	if (!mWindow) return 0;
	int x = 0, y = 0;
	SDL_GetWindowPosition(mWindow, &x, &y);
	return y;
}

void
SDL3WindowForm::SetPosition(int l, int t)
{
	MoveWindowTo(l, t);
}

int
SDL3WindowForm::GetWidth() const
{
	if (!mWindow) return 0;
	int w = 0, h = 0;
	SDL_GetWindowSize(mWindow, &w, &h);
	SDLBorders b = GetBorders(mWindow);
	return w + b.left + b.right;
}

int
SDL3WindowForm::GetHeight() const
{
	if (!mWindow) return 0;
	int w = 0, h = 0;
	SDL_GetWindowSize(mWindow, &w, &h);
	SDLBorders b = GetBorders(mWindow);
	return h + b.top + b.bottom;
}

void
SDL3WindowForm::SetWidth(int w)
{
	SetSize(w, GetHeight());
}

void
SDL3WindowForm::SetHeight(int h)
{
	SetSize(GetWidth(), h);
}

void
SDL3WindowForm::SetSize(int w, int h)
{
	if (!mWindow) return;
	SDLBorders b = GetBorders(mWindow);
	int cw = w - (b.left + b.right);
	int ch = h - (b.top + b.bottom);
	if (cw < 1) cw = 1;
	if (ch < 1) ch = 1;
	ResizeWindow(cw, ch);   // 内部の surface サイズ追跡も更新される
}

//---------------------------------------------------------------------------
// 画面比率の固定。 SDL にリサイズ時の拘束を掛ける (min=max=同じ比率)。
// 0 (未設定) のときは拘束を外す。
//---------------------------------------------------------------------------
void SDL3WindowForm::ApplyAspectLock()
{
	if (!mWindow) return;
	int aw = GetAspectLockW(), ah = GetAspectLockH();
	float r = (aw > 0 && ah > 0) ? (float)aw / (float)ah : 0.0f;
	SDL_SetWindowAspectRatio(mWindow, r, r);
}

// min/max は「内側 (クライアント) サイズ」基準。SDL の
// SDL_SetWindowMinimum/MaximumSize がそのままクライアント基準なので素通し。
void SDL3WindowForm::SetMinWidth(int v)  { SetMinSize(v, mMinHeight); }
void SDL3WindowForm::SetMinHeight(int v) { SetMinSize(mMinWidth, v); }
void SDL3WindowForm::SetMinSize(int w, int h)
{
	mMinWidth = w; mMinHeight = h;
	if (mWindow) SDL_SetWindowMinimumSize(mWindow, w > 0 ? w : 0, h > 0 ? h : 0);
}
void SDL3WindowForm::SetMaxWidth(int v)  { SetMaxSize(v, mMaxHeight); }
void SDL3WindowForm::SetMaxHeight(int v) { SetMaxSize(mMaxWidth, v); }
void SDL3WindowForm::SetMaxSize(int w, int h)
{
	mMaxWidth = w; mMaxHeight = h;
	if (mWindow) SDL_SetWindowMaximumSize(mWindow, w > 0 ? w : 0, h > 0 ? h : 0);
}
int SDL3WindowForm::GetMinWidth() const  { return mMinWidth; }
int SDL3WindowForm::GetMinHeight()       { return mMinHeight; }
int SDL3WindowForm::GetMaxWidth()        { return mMaxWidth; }
int SDL3WindowForm::GetMaxHeight()       { return mMaxHeight; }

//---------------------------------------------------------------------------
// 内側 (クライアント) サイズ。
//   基底はリサイズ要求値をキャッシュして返すため、min/max でクランプされたり
//   要求が通らなかったときに実サイズと食い違う。 SDL へ問い合わせて実値を返す。
//---------------------------------------------------------------------------
int
SDL3WindowForm::GetInnerWidth() const
{
	if (mFixedSurfaceWidth > 0) return mFixedSurfaceWidth;
	if (!mWindow) return TTVPWindowForm::GetInnerWidth();
	int w = 0, h = 0;
	SDL_GetWindowSize(mWindow, &w, &h);
	return w;
}

int
SDL3WindowForm::GetInnerHeight() const
{
	if (mFixedSurfaceHeight > 0) return mFixedSurfaceHeight;
	if (!mWindow) return TTVPWindowForm::GetInnerHeight();
	int w = 0, h = 0;
	SDL_GetWindowSize(mWindow, &w, &h);
	return h;
}

// SDL には「枠スタイル」という 1 つの属性は無く、枠の有無 (bordered) と
// リサイズ可否 (resizable) の組み合わせで表現する。値そのものは保持して返す。
void
SDL3WindowForm::SetBorderStyle(enum tTVPBorderStyle st)
{
	mBorderStyle = st;
	if (!mWindow) return;
	bool bordered = (st != bsNone);
	bool resizable = (st == bsSizeable || st == bsSizeToolWin);
	SDL_SetWindowBordered(mWindow, bordered);
	SDL_SetWindowResizable(mWindow, resizable);
}

enum tTVPBorderStyle
SDL3WindowForm::GetBorderStyle() const
{
	return mBorderStyle;
}

void
SDL3WindowForm::SetStayOnTop(bool b)
{
	if (mWindow) SDL_SetWindowAlwaysOnTop(mWindow, b);
}

bool
SDL3WindowForm::GetStayOnTop() const
{
	if (!mWindow) return false;
	return (SDL_GetWindowFlags(mWindow) & SDL_WINDOW_ALWAYS_ON_TOP) != 0;
}

void
SDL3WindowForm::BringToFront()
{
	if (mWindow) SDL_RaiseWindow(mWindow);
}

void
SDL3WindowForm::SetFullScreenMode(bool b)
{
	if (mWindow) SDL_SetWindowFullscreen(mWindow, b);
}

bool
SDL3WindowForm::GetFullScreenMode() const
{
	if (!mWindow) return false;
	return (SDL_GetWindowFlags(mWindow) & SDL_WINDOW_FULLSCREEN) != 0;
}

void
SDL3WindowForm::GetCursorPos(tjs_int &x, tjs_int &y)
{
	float xpos = 0, ypos = 0;
	if (mWindow) {
		SDL_GetMouseState(&xpos, &ypos);
	}
	x = (tjs_int)xpos;
	y = (tjs_int)ypos;
}

void
SDL3WindowForm::SetCursorVisible(bool visible)
{
	// SDL のカーソル表示はプロセスグローバル (ウィンドウ単位ではない)。
	// TTVPWindowForm::SetMouseCursorState が状態変化時のみ呼んでくる。
	if (visible) SDL_ShowCursor();
	else         SDL_HideCursor();
}

void
SDL3WindowForm::SetCursorPos(tjs_int x, tjs_int y)
{
	if (mWindow) {
		// 引数は「描画矩形内の座標」(iTVPWindow::SetCursorPos の契約。 入力側の
		// OnMouse* が TranslateWindowToDrawArea で destRect オフセットを引いた
		// 座標で流れてくるのと対)。 SDL の warp はウィンドウクライアント座標
		// なので destRect オフセットを足し戻す (win32 実装と同じ変換)。
		// フルスクリーンのレターボックス等で destRect が (0,0) 以外のとき、
		// これが無いと warp 先がオフセット分ズレる。
		TranslateDrawAreaToWindow(x, y);
		SDL_WarpMouseInWindow(mWindow, (float)x, (float)y);
	}
}

void
SDL3WindowForm::SetEnableTouch( bool b )
{
	mEnableTouch = b && checkTouchDevice();
}

bool
SDL3WindowForm::GetEnableTouch() const
{
	return mEnableTouch;
}

bool
SDL3WindowForm::GetEnableTouchMouse() const
{
	return SDL_GetHintBoolean(SDL_HINT_TOUCH_MOUSE_EVENTS, true) == true;
}

void
SDL3WindowForm::SetEnableTouchMouse( bool b )
{
	SDL_SetHint(SDL_HINT_TOUCH_MOUSE_EVENTS, b ? "1" : "0");
}

extern tjs_uint16 TVPTransSDLKeyToVirtualKey(tjs_int sdlKey);

//< SDLイベント処理
bool
SDL3WindowForm::AppEvent(const SDL_Event& event)
{
	switch (event.type) {
		case SDL_EVENT_KEYBOARD_ADDED:
		case SDL_EVENT_KEYBOARD_REMOVED: {
			// 物理キーボードの抜き差しでベースラインのテキスト入力を切り替える。
			// (NX/PS5 は起動後に初めてキーボードが登録されるのでここが本番)
			TVPUpdateBaselineTextInput(mWindow);
			break;
		}
		case SDL_EVENT_KEY_DOWN:
		case SDL_EVENT_KEY_UP: {
#ifdef KRKRZ_HAS_ELEMENTS
			// F12 で Elements テストダイアログをトグル (Phase 3 MVP デバッグ用)。
			// KEY_DOWN でトグル、KEY_UP は KEY_DOWN とペアで Layer に流れない
			// よう同様に消費する。
			if (event.key.key == SDLK_F12) {
				if (event.type == SDL_EVENT_KEY_DOWN && !event.key.repeat) {
					TVPShowElementsTestDialog();
				}
				return true;
			}
#endif
			int message = (event.type == SDL_EVENT_KEY_UP) ? AM_KEY_UP : AM_KEY_DOWN;
			int key = TVPTransSDLKeyToVirtualKey(event.key.key);
			int shift = 0;
			if (event.key.mod & SDL_KMOD_SHIFT) {
				SendMessage(message, VK_SHIFT, 0);
				shift |= TVP_SS_SHIFT;
			}
			if (event.key.mod & SDL_KMOD_CTRL) {
				SendMessage(message, VK_CONTROL, 0);
				shift |= TVP_SS_CTRL;
			}
			if (event.key.mod & SDL_KMOD_ALT) {
				SendMessage(message, VK_MENU, 0);
				shift |= TVP_SS_ALT;
			}
			if (event.key.repeat) shift |= TVP_SS_REPEAT;
			SendMessage(message, key, shift);
			break;
		}
		case SDL_EVENT_TEXT_INPUT: {
#ifdef KRKRZ_HAS_ELEMENTS
			// Elements ダイアログ (modal / 常駐 HUD 問わず) が表示中の場合、
			// キーボードフォーカスを持つインスタンスがあればそこへ文字入力を
			// 流し消費する。 フォーカスが無い (grabFocus=false の常駐 HUD 等) と
			// ForwardText は false を返すので、 その場合は下のゲーム配送へ流す。
			// (IsModalActive は実際には「いずれかのダイアログがアクティブ」の意)
			if (tTVPElementsDialogManager::Instance().IsModalActive()) {
				if (tTVPElementsDialogManager::Instance().ForwardText(event.text.text))
					return true;
			}
#endif
			// modal 非表示中は KAG(ゲーム) へ文字入力を配送する。
			// SDL の UTF-8 テキストを UTF-16(tjs_char) に変換し、1文字ずつ
			// onKeyPress として送る (win32 の WM_CHAR 相当)。
			const char* u8 = event.text.text;
			if (u8 && *u8) {
				tjs_int n = TVPUtf8ToWideCharString(u8, NULL);
				if (n > 0) {
					tjs_char* buf = new tjs_char[n + 1];
					TVPUtf8ToWideCharString(u8, buf);
					for (tjs_int i = 0; i < n; i++) {
						if (buf[i]) OnKeyPress((tjs_int)buf[i], 0, false, false);
					}
					delete[] buf;
				}
			}
			break;
		}
		case SDL_EVENT_MOUSE_BUTTON_DOWN:
		case SDL_EVENT_MOUSE_BUTTON_UP: {
			static int buttonmap[] = {mbLeft, mbMiddle, mbRight, mbX1, mbX2};
			int button = buttonmap[event.button.button-1];
			int message = (event.type == SDL_EVENT_MOUSE_BUTTON_UP) ? AM_MOUSE_UP : AM_MOUSE_DOWN;
			int shift = 0;
			int mod = SDL_GetModState();
			if (mod & SDL_KMOD_SHIFT) shift |= TVP_SS_SHIFT;
			if (mod & SDL_KMOD_CTRL) shift |= TVP_SS_CTRL;
			if (mod & SDL_KMOD_ALT) shift |= TVP_SS_ALT;
			// ダブルクリックは win32 の WM_*BUTTONDBLCLK と同じ順序で
			// (dblclick → down) 流す。generic 側で onDoubleClick を発火し、
			// 続く up での onClick を抑止する。
			if (message == AM_MOUSE_DOWN && event.button.clicks >= 2) {
				SendMouseMessage(AM_MOUSE_DBLCLK, button, shift, event.button.x, event.button.y);
			}
			SendMouseMessage(message, button, shift, event.button.x, event.button.y);
			break;
		}
		case SDL_EVENT_MOUSE_MOTION: {
			int shift = 0;
			int mod = SDL_GetModState();
			if (mod & SDL_KMOD_SHIFT) shift |= TVP_SS_SHIFT;
			if (mod & SDL_KMOD_CTRL) shift |= TVP_SS_CTRL;
			if (mod & SDL_KMOD_ALT) shift |= TVP_SS_ALT;
			SendMouseMessage(AM_MOUSE_MOVE, 0, shift, event.motion.x, event.motion.y);
			break;
		}
		case SDL_EVENT_MOUSE_WHEEL: {
			int shift = 0;
			int mod = SDL_GetModState();
			if (mod & SDL_KMOD_SHIFT) shift |= TVP_SS_SHIFT;
			if (mod & SDL_KMOD_CTRL) shift |= TVP_SS_CTRL;
			if (mod & SDL_KMOD_ALT) shift |= TVP_SS_ALT;
			float x, y;
			SDL_GetMouseState(&x, &y);
			SendMouseMessage(AM_MOUSE_WHEEL, (int)event.wheel.y, shift, (int)x, (int)y);
			break;
		}
		case SDL_EVENT_FINGER_DOWN:
		case SDL_EVENT_FINGER_UP:
		case SDL_EVENT_FINGER_MOTION: {
			if (mEnableTouch) {
				int type = (event.type == SDL_EVENT_FINGER_UP) ? AM_TOUCH_UP :
						(event.type == SDL_EVENT_FINGER_DOWN) ? AM_TOUCH_DOWN : AM_TOUCH_MOVE;
				float x = event.tfinger.x * mSurfaceWidth; // Convert to pixels
				float y = event.tfinger.y * mSurfaceHeight; // Convert to pixels
				float c = event.tfinger.pressure; // Pressure
				int id = event.tfinger.fingerID; // Finger ID
				tjs_uint64 tick = event.tfinger.timestamp; // Timestamp in nanoseconds
				SendTouchMessage(type, x, y, c, id, tick);
			}
			break;
		}
		case SDL_EVENT_WINDOW_CLOSE_REQUESTED: {
			TVPLOG_DEBUG("Window close requested");
			// modal dialog 表示中でも Close 要求はゲーム側 (onCloseQuery) へ
			// 届ける。 KAG 側は askOnClose なら Elements の終了確認モーダルを
			// さらに上へ重ねて表示し、 確定するまで実際には閉じない
			// (super.onCloseQuery(false) でキャンセルされる)。
			// 旧実装はモーダル表示中ここで握り潰していた (「dialog 側で意図的に
			// close するまで閉じられない」ガード) が、 boot ランチャーや
			// grabFocus メニュー表示中に × が無反応になるため撤廃 (2026-08-06)。
			// モーダル多重 (ランチャー上の終了確認等) は manager の複数
			// インスタンス + nested pump で成立する。
			Close();
			break;
		}
		case SDL_EVENT_WINDOW_RESIZED: {
			int w = event.window.data1;
			int h = event.window.data2;
			TVPLOG_DEBUG("Window resized: {}x{}", w, h);
			SendMessage(AM_DISPLAY_RESIZE, w, h);
			break;
		}
	}
	return true;
}

#ifdef TVP_USE_OPENGL

// ----------------------------------------------------
// OpenGLコンテキスト実装
// ----------------------------------------------------

class SDL3GLContext : public iTVPGLContext
{
private:
	SDL_Window *mWindow;
	SDL_GLContext mGLContext;
	bool mOwned;      ///< このラッパが mGLContext を所有し破棄する責任を持つか (分離コンテキスト時 true)
	int mRefCount;    ///< 分離コンテキスト用の参照カウント (mOwned 時のみ有効)

public:
	// separateShared=false: 画面デバイス用。ウィンドウの「主コンテキスト」(一度だけ確定して
	//   キャッシュされる。TVPGetOrCreateMainGLContext 参照) を返す。破棄しない
	//   (mOwned=false, Release は no-op)。★カレントを直接採用しないのが要点
	//   (compositor の分離コンテキストを誤採用して黒画面になるのを防ぐ)。
	// separateShared=true: オフスクリーン合成 (GLCompositor) 用。画面デバイスと FBO/GL
	//   ステートを共有して黒画面になるのを避けるため、専用の GL コンテキストを生成する。
	//   現在のコンテキストがあればそれと共有グループにし (SDL_GL_SHARE_WITH_CURRENT_CONTEXT。
	//   テクスチャ/シェーダ等のリソースは共有、FBO/VAO 等コンテナは独立)、生成後は
	//   直前のカレントコンテキストへ戻して画面デバイスの状態を乱さない。このラッパが
	//   所有し Release() で破棄する。
	SDL3GLContext(SDL_Window *window, bool separateShared)
	: mWindow(window), mGLContext(nullptr), mOwned(false), mRefCount(1)
	{
		if (separateShared) {
			SDL_GLContext prev = SDL_GL_GetCurrentContext();
			// 現在のコンテキストがあれば共有グループにする (無ければ単独コンテキスト)
			SDL_GL_SetAttribute(SDL_GL_SHARE_WITH_CURRENT_CONTEXT, prev ? 1 : 0);
			mGLContext = TVPCreateGLContextWithFallback(mWindow);  // 生成すると mGLContext がカレントになる
			if (!mGLContext) TVPLOG_ERROR("SDL_GL_CreateContext (compositor) failed: {}", SDL_GetError());
			// 後続の画面コンテキスト生成に共有属性が波及しないよう戻す
			SDL_GL_SetAttribute(SDL_GL_SHARE_WITH_CURRENT_CONTEXT, 0);
			// 直前のコンテキストを復帰し、画面デバイスの想定を乱さない
			if (prev) SDL_GL_MakeCurrent(mWindow, prev);
			mOwned = (mGLContext != nullptr);
		} else {
			// ウィンドウの主コンテキストを取得 (初回のみ確定・以降キャッシュ)。
			// カレント (=compositor の分離コンテキストの可能性) をそのまま採らない。
			mGLContext = TVPGetOrCreateMainGLContext(mWindow);
			// 画面用は従来どおり破棄しない (mOwned=false)
		}
	}

	int Release()
	{
		if (!mOwned) return 0;   // 画面用: 従来どおり no-op (主コンテキストは破棄しない)
		if (--mRefCount == 0) {
			if (mGLContext) {
				SDL_GL_DestroyContext(mGLContext);
				mGLContext = nullptr;
			}
			delete this;
			return 0;
		}
		return mRefCount;
	}

	void *NativeWindow() 
	{
		return mWindow;
	}

	void GetSurfaceSize(int *width, int *height) 
	{
		if (mWindow) {
			SDL_GetWindowSize(mWindow, width, height);
		}
	}

	void MakeCurrent() 
	{
		if (mWindow && mGLContext) {
			SDL_GL_MakeCurrent(mWindow, mGLContext);
		}
	}

	void Swap() 
	{
		if (mWindow) {
			SDL_GL_SwapWindow(mWindow);
		}
	}

	void SetWaitVSync(bool waitVSync) {
		if (mWindow) {
			SDL_GL_MakeCurrent(mWindow, mGLContext);
			SDL_GL_SetSwapInterval(waitVSync ? 1 : 0);
		}
	}
};

iTVPGLContext *iTVPGLContext::GetContext(void *nativeWindow, bool separateShared)
{
	return new SDL3GLContext((SDL_Window*)nativeWindow, separateShared);
}

void* TVPGLGetProcAddress(const char * procname) 
{
	return (void*)SDL_GL_GetProcAddress(procname);
}

#endif
