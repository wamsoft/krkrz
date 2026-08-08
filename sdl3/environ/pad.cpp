#include "tjsCommHead.h"
#include "CharacterSet.h"
#include "LogIntf.h"
#include "StorageIntf.h"

#include "app.h"

// パッド初期化 XXX 処理確認中
static const tjs_char *CONTROLLER_DB = TJS_W("gamecontrollerdb.txt");
static bool initialized = false;

void InitPadMaiing() 
{
	if (initialized) return;

	tjs_string path = Application->ResourcePath() + CONTROLLER_DB;	
	tjs_uint64 size;
	auto data = ::TVPReadStream(path.c_str(), &size);
	if (data) {
		const char *mapping = (const char *)data.get();
		if (SDL_AddGamepadMapping(mapping) < 0) {
			const char *error = SDL_GetError();
			TVPLOG_ERROR("Failed to load controller mappings: {}", error);
		}
	}
	initialized = true;
}
static SDL_GamepadButton button_map[] = {
	SDL_GAMEPAD_BUTTON_SOUTH,
	SDL_GAMEPAD_BUTTON_EAST,
	SDL_GAMEPAD_BUTTON_WEST,
	SDL_GAMEPAD_BUTTON_NORTH,
	SDL_GAMEPAD_BUTTON_LEFT_SHOULDER,
	SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER,
	SDL_GAMEPAD_BUTTON_INVALID,
	SDL_GAMEPAD_BUTTON_INVALID,
	SDL_GAMEPAD_BUTTON_BACK,
    SDL_GAMEPAD_BUTTON_START,
    SDL_GAMEPAD_BUTTON_LEFT_STICK,
    SDL_GAMEPAD_BUTTON_RIGHT_STICK,
	SDL_GAMEPAD_BUTTON_DPAD_LEFT,
	SDL_GAMEPAD_BUTTON_DPAD_UP,
	SDL_GAMEPAD_BUTTON_DPAD_RIGHT,
	SDL_GAMEPAD_BUTTON_DPAD_DOWN,
};

#define _USE_MATH_DEFINES
#include <math.h>

enum {
	KEY_LEFT   = 0x01,
	KEY_UP     = 0x02,
	KEY_RIGHT  = 0x04,
	KEY_DOWN   = 0x08,
} Keys;

// アナログスティックの入力を方向キーに変換する
static void analog_to_key(float x, float y, int key_base, tjs_uint32 &key_state)
{
	// アナログパッドの判定
	if (sqrt(x*x+y*y) >= 0.6) {
		int angle = (int)(atan2f(y, x) * 360 / (M_PI * 2) + 360 + 360 / 16) % 360 / 45;
		static tjs_uint32 stick[] = { 
				KEY_RIGHT,
				KEY_RIGHT | KEY_DOWN,
				KEY_DOWN,
				KEY_LEFT | KEY_DOWN,
				KEY_LEFT,
				KEY_LEFT | KEY_UP,
				KEY_UP,
				KEY_UP | KEY_RIGHT };
		key_state |= (stick[angle] << key_base);
	}
}

// 物理パッドアクセス (main.cpp)。物理 index は g_open_gamepads の並び (接続順)。
extern SDL_Gamepad *TVPGetOpenGamepad(int idx);
extern int TVPGetOpenGamepadCount();

// 指定 SDL_Gamepad から 24bit ボタン状態 (doc/Gamepad.md §3.1) を組み立てる。
static tjs_uint32 BuildPadState(SDL_Gamepad *gp)
{
	if (!gp) return 0;

	tjs_uint32 key_state = 0;

	// ボタン状態を取得
	for (int i=0; i<16; i++) {
		SDL_GamepadButton btn = button_map[i];
		if (btn != SDL_GAMEPAD_BUTTON_INVALID) {
			if (SDL_GetGamepadButton(gp, btn)) {
				key_state |= (1 << i);
			}
		}
	}

	// トリガーをL2/R2ボタンに反映
	float triggerThreshold = 0.8f;
	float leftTrigger = SDL_GetGamepadAxis(gp, SDL_GAMEPAD_AXIS_LEFT_TRIGGER) / 32767.0f;
	float rightTrigger = SDL_GetGamepadAxis(gp, SDL_GAMEPAD_AXIS_RIGHT_TRIGGER) / 32767.0f;
	if (leftTrigger > triggerThreshold) key_state |= (1 << 6);
	if (rightTrigger > triggerThreshold) key_state |= (1 << 7);

	// アナログスティックから方向キー情報を反映
	float leftX  = SDL_GetGamepadAxis(gp, SDL_GAMEPAD_AXIS_LEFTX) / 32767.0f;
	float leftY  = SDL_GetGamepadAxis(gp, SDL_GAMEPAD_AXIS_LEFTY) / 32767.0f;
	float rightX = SDL_GetGamepadAxis(gp, SDL_GAMEPAD_AXIS_RIGHTX) / 32767.0f;
	float rightY = SDL_GetGamepadAxis(gp, SDL_GAMEPAD_AXIS_RIGHTY) / 32767.0f;
	analog_to_key(leftX, leftY,   16, key_state);
	analog_to_key(rightX, rightY, 20, key_state);

	return key_state;
}

int SDL3Application::GetPhysicalPadCount()
{
	return TVPGetOpenGamepadCount();
}

tjs_uint32 SDL3Application::GetPhysicalPadState(int phys)
{
	return BuildPadState(TVPGetOpenGamepad(phys));
}

// 軸 ID は SDL_GamepadAxis と同値で定義済み (Application.h)。下記 static_assert で
// 値ズレを起こしたらコンパイル時に検出する。
static_assert((int)tTVPApplication::TVP_PAD_AXIS_LEFTX         == (int)SDL_GAMEPAD_AXIS_LEFTX,         "");
static_assert((int)tTVPApplication::TVP_PAD_AXIS_LEFTY         == (int)SDL_GAMEPAD_AXIS_LEFTY,         "");
static_assert((int)tTVPApplication::TVP_PAD_AXIS_RIGHTX        == (int)SDL_GAMEPAD_AXIS_RIGHTX,        "");
static_assert((int)tTVPApplication::TVP_PAD_AXIS_RIGHTY        == (int)SDL_GAMEPAD_AXIS_RIGHTY,        "");
static_assert((int)tTVPApplication::TVP_PAD_AXIS_LEFT_TRIGGER  == (int)SDL_GAMEPAD_AXIS_LEFT_TRIGGER,  "");
static_assert((int)tTVPApplication::TVP_PAD_AXIS_RIGHT_TRIGGER == (int)SDL_GAMEPAD_AXIS_RIGHT_TRIGGER, "");

float SDL3Application::GetPhysicalPadAxis(int phys, int axisId)
{
	SDL_Gamepad *gp = TVPGetOpenGamepad(phys);
	if (!gp) return 0.0f;
	if (axisId < 0 || axisId >= TVP_PAD_AXIS_COUNT) return 0.0f;
	// SDL_GetGamepadAxis: スティック -32768〜32767、トリガ 0〜32767
	Sint16 raw = SDL_GetGamepadAxis(gp, (SDL_GamepadAxis)axisId);
	float v = raw / 32767.0f;
	// raw = -32768 のとき -1.00003... になるので clamp
	if (v < -1.0f) v = -1.0f;
	return v;
}

tjs_string SDL3Application::GetPhysicalPadName(int phys)
{
	SDL_Gamepad *gp = TVPGetOpenGamepad(phys);
	if (!gp) return tjs_string();
	tjs_string name;
	TVPUtf8ToUtf16(name, SDL_GetGamepadName(gp));
	return name;
}

bool SDL3Application::RumblePhysical(int phys, int low, int high, int duration_ms)
{
	SDL_Gamepad *gp = TVPGetOpenGamepad(phys);
	if (!gp) return false;
	// 0〜255 を 0〜0xFFFF にスケール
	Uint16 low16 = (Uint16)((low * 0xFFFF) / 255);
	Uint16 high16 = (Uint16)((high * 0xFFFF) / 255);
	return SDL_RumbleGamepad(gp, low16, high16, (Uint32)duration_ms);
}

bool SDL3Application::StopRumblePhysical(int phys)
{
	SDL_Gamepad *gp = TVPGetOpenGamepad(phys);
	if (!gp) return false;
	return SDL_RumbleGamepad(gp, 0, 0, 0);
}