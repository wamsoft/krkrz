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

#ifdef KRKRZ_HAS_ELEMENTS
#include "elements/ElementsDialogManager.h"
#endif

#include <stdio.h>
#include <string>

// ----------------------------------------------------
// glad 初期化用
// ----------------------------------------------------



// ----------------------------------------------------
// SDL3WindowForm 実装
// ----------------------------------------------------

SDL3WindowForm::SDL3WindowForm(class tTJSNI_Window* win)
 : TTVPWindowForm(win)
 , mWindow(nullptr)
 , mVisible(false)
{
	SDL_WindowFlags flags = SDL_WINDOW_HIDDEN;
#if defined(TVP_USE_OPENGL)
	flags |= SDL_WINDOW_OPENGL;
#endif
#if defined(__ANDROID__) || defined(__IOS__) || defined(__ORBIS__) || defined (__PROSPERO__)
	flags |= SDL_WINDOW_FULLSCREEN;
	int width  = 1920;
	int height = 1080; 
#else
	flags |= SDL_WINDOW_RESIZABLE;
	int width = 32;
	int height = 32;
#endif
	// ウィンドウ作成
	mWindow = SDL_CreateWindow("", width, height, flags);
	if (mWindow) {
		// ウィンドウのユーザーデータとして自身を設定
		//SDL_SetWindowFullscreen(mWindow, true);
		SDL_SetPointerProperty(SDL_GetWindowProperties(mWindow), "form", this);
		// テキスト入力を有効化 (SDL_EVENT_TEXT_INPUT を発生させ、KAG の Edit
		// レイヤ等へ文字入力を配送できるようにする。未呼出だと TEXT_INPUT が
		// 一切発生せず、STEINS;GATE 8BIT の start 画面のタイプ入力が通らない)。
		// ただしオンスクリーンキーボードを持つ環境では SDL_StartTextInput が
		// キーボード UI を即座に出してしまう (NX/Ounce ではブロッキングの
		// swkbd アプレット起動、Android ではソフトキーボード表示) ため、
		// 起動時のベースライン有効化は物理キーボード環境限定とする。
		// これらの環境では Elements 側のフォーカス駆動
		// (ElementsDialogManager::UpdateFocusDrivenTextInput) が
		// テキスト欄に focus が入ったときにのみ開始する。
		if (!SDL_HasScreenKeyboardSupport()) {
			SDL_StartTextInput(mWindow);
		}
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

void
SDL3WindowForm::DestroyNativeWindow()
{
	if (mWindow) {
#if defined(TVP_USE_OPENGL)
		// OpenGLコンテキストを破棄
		SDL_GLContext glContext = SDL_GL_GetCurrentContext();
		if (glContext) {
			SDL_GL_DestroyContext(glContext);
		}
#endif
		// ウィンドウを破棄
		SDL_DestroyWindow(mWindow);
		mWindow = nullptr;
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
	if (mWindow) {
		SDL_GetWindowSize(mWindow, &w, &h);
	} else {
		w = 0;
		h = 0;
	}
}

void
SDL3WindowForm::ResizeWindow(int w, int h)
{
	TTVPWindowForm::ResizeWindow(w, h);
	if (mWindow) {
		SDL_SetWindowSize(mWindow, w, h);
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
				SDL_ShowWindow(mWindow);
			} else {
				SDL_HideWindow(mWindow);
			}
		}
	}
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
SDL3WindowForm::SetCursorPos(tjs_int x, tjs_int y)
{
	if (mWindow) {
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
#ifdef KRKRZ_HAS_ELEMENTS
			// modal dialog 表示中は Close を呼ばない (=閉じる動作をキャンセル)。
			// ユーザは dialog 側で意図的に close するまで Window を閉じられない。
			// 非モーダルの常駐 UI だけならウィンドウは閉じてよい。
			if (tTVPElementsDialogManager::Instance().HasModalInstance()) {
				TVPAddLog(TJS_W("Window close cancelled: dialog modal active"));
			} else {
				Close();
			}
#else
			Close();
#endif
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
	
public:
	SDL3GLContext(SDL_Window *window)
	: mWindow(window)
	{
		mGLContext = SDL_GL_GetCurrentContext();
		if (!mGLContext) {
			mGLContext = SDL_GL_CreateContext(mWindow);
		}
	}

	int Release() 
	{
		return 0;
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

iTVPGLContext *iTVPGLContext::GetContext(void *nativeWindow)
{
	return new SDL3GLContext((SDL_Window*)nativeWindow);
}

void* TVPGLGetProcAddress(const char * procname) 
{
	return (void*)SDL_GL_GetProcAddress(procname);
}

#endif
