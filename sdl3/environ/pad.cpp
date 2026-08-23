#include "tjsCommHead.h"
#include "CharacterSet.h"
#include "LogIntf.h"
#include "StorageIntf.h"
#include "SysInitIntf.h"   // TVPGetCommandLine (-padbuttons)
#include "DebugIntf.h"     // TVPAddLog

#include "app.h"

#include <string>

// 追加のゲームパッドマッピングを登録する (resource の gamecontrollerdb.txt)。
// パッドを開く前に呼ぶ必要があるので、 main.cpp の EnsureGamepadOpen() の先頭で
// 呼んでいる。 初回のみ実際に読み込む。
static const tjs_char *CONTROLLER_DB = TJS_W("gamecontrollerdb.txt");
static bool initialized = false;

void InitPadMapping()
{
	if (initialized) return;
	initialized = true;   // 失敗しても再試行しない (毎回のパッド接続で読み直さない)

	tjs_string path = Application->ResourcePath() + CONTROLLER_DB;
	tjs_uint64 size = 0;
	auto data = ::TVPReadStream(path.c_str(), &size);
	if (!data) return;   // 無ければ SDL 内蔵の定義だけで動く

	// ★ SDL_AddGamepadMapping は「1 行 1 マッピング」用。 DB ファイル全体を
	//   渡しても先頭行しか登録されない。 複数行は FromIO を使う。
	SDL_IOStream *io = SDL_IOFromConstMem(data.get(), (size_t)size);
	if (!io) return;
	const int added = SDL_AddGamepadMappingsFromIO(io, true /*closeio*/);
	if (added < 0) {
		TVPLOG_ERROR("Failed to load controller mappings: {}", SDL_GetError());
	} else {
		TVPLOG_INFO("Loaded {} controller mapping(s) from gamecontrollerdb.txt", added);
	}
}
// VK_PAD1..4 に対応するフェイスボタン。 添字 0..3 = VK_PAD1..4 (A/B/X/Y)。
//
// この表は「位置」基準 (SDL の SOUTH/EAST/WEST/NORTH をそのまま Xbox の
// A/B/X/Y と読む) なので、 刻印が異なる任天堂系コントローラでは
// VK_PAD1(A) が刻印 B のボタンに乗ってしまう。 既定では
// ResolveFaceButtons() が刻印基準へ解決するので、 こちらは
// padButtonMapping = "position" (従来互換) のときのフォールバック。
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

// --- フェイスボタン (VK_PAD1..4) の割り当て規則 -------------------------
//
// "label"    : 刻印 A/B/X/Y のボタンへ割り当てる (既定)。
//              任天堂系は SOUTH=B / EAST=A / WEST=Y / NORTH=X の刻印なので、
//              位置基準のままだと VK_PAD1(A) が刻印 B に乗ってしまう。
//              PlayStation は ×→A / ○→B / □→X / △→Y と読み替える
//              (= 位置基準と同じ結果なので変化なし)。 Xbox / 汎用も変化なし。
// "position" : 従来どおり SDL の位置 (SOUTH/EAST/WEST/NORTH) をそのまま
//              A/B/X/Y として扱う (Xbox 名準拠)。
//
// 起動オプション -padbuttons=label|position / TJS System.padButtonMapping。
static bool pad_label_mapping = true;

void SDL3Application::SetPadButtonMappingByLabel(bool by_label) { pad_label_mapping = by_label; }
bool SDL3Application::GetPadButtonMappingByLabel()              { return pad_label_mapping; }

// 起動オプション -padbuttons=label|position (config.cf でも可)。
// 初回のパッド状態取得時に一度だけ読む (コマンドラインはその時点で確定済み)。
static void EnsurePadButtonMappingOption()
{
	static bool applied = false;
	if (applied) return;
	applied = true;

	tTJSVariant val;
	if (!TVPGetCommandLine(TJS_W("-padbuttons"), &val)) return;
	ttstr s = val;
	if (s == TJS_W("position") || s == TJS_W("pos")) {
		pad_label_mapping = false;
	} else if (s == TJS_W("label")) {
		pad_label_mapping = true;
	} else {
		TVPAddLog(TJS_W("(warning) -padbuttons: unknown value (use label|position)"));
	}
}

// 解決結果を 1 回だけログに出す (実機でどう解決されたかの確認用)。
static void LogFaceButtonsOnce(SDL_Gamepad *gp, const SDL_GamepadButton out[4], bool resolved)
{
	static SDL_Gamepad *logged = nullptr;
	if (!gp || logged == gp) return;
	logged = gp;

	static const char *kPos[] = { "SOUTH", "EAST", "WEST", "NORTH" };
	auto posname = [](SDL_GamepadButton b) -> const char * {
		switch (b) {
		case SDL_GAMEPAD_BUTTON_SOUTH: return "SOUTH";
		case SDL_GAMEPAD_BUTTON_EAST:  return "EAST";
		case SDL_GAMEPAD_BUTTON_WEST:  return "WEST";
		case SDL_GAMEPAD_BUTTON_NORTH: return "NORTH";
		default: return "?";
		}
	};
	auto labname = [](SDL_GamepadButtonLabel l) -> const char * {
		switch (l) {
		case SDL_GAMEPAD_BUTTON_LABEL_A:        return "A";
		case SDL_GAMEPAD_BUTTON_LABEL_B:        return "B";
		case SDL_GAMEPAD_BUTTON_LABEL_X:        return "X";
		case SDL_GAMEPAD_BUTTON_LABEL_Y:        return "Y";
		case SDL_GAMEPAD_BUTTON_LABEL_CROSS:    return "Cross";
		case SDL_GAMEPAD_BUTTON_LABEL_CIRCLE:   return "Circle";
		case SDL_GAMEPAD_BUTTON_LABEL_SQUARE:   return "Square";
		case SDL_GAMEPAD_BUTTON_LABEL_TRIANGLE: return "Triangle";
		default: return "unknown";
		}
	};
	static const SDL_GamepadButton kFacePos[4] = {
		SDL_GAMEPAD_BUTTON_SOUTH, SDL_GAMEPAD_BUTTON_EAST,
		SDL_GAMEPAD_BUTTON_WEST,  SDL_GAMEPAD_BUTTON_NORTH,
	};
	std::string labels;
	for (int i = 0; i < 4; ++i) {
		if (i) labels += ", ";
		labels += kPos[i];
		labels += "=";
		labels += labname(SDL_GetGamepadButtonLabel(gp, kFacePos[i]));
	}
	const char *name = SDL_GetGamepadName(gp);
	TVPLOG_INFO("Pad face buttons: name='{}' type={} mapping={} labels[{}] "
	            "VK_PAD1={} VK_PAD2={} VK_PAD3={} VK_PAD4={}",
	            name ? name : "?", (int)SDL_GetGamepadType(gp),
	            resolved ? "label" : (pad_label_mapping ? "label(unresolved->position)" : "position"),
	            labels, posname(out[0]), posname(out[1]), posname(out[2]), posname(out[3]));
}

// gp の刻印を見て VK_PAD1..4 に載せるフェイスボタンを決める。
// 判定できなければ位置基準 (button_map) のままにする。
static void ResolveFaceButtons(SDL_Gamepad *gp, SDL_GamepadButton out[4])
{
	EnsurePadButtonMappingOption();
	for (int i = 0; i < 4; ++i) out[i] = button_map[i];
	if (!gp || !pad_label_mapping) return;

	static const SDL_GamepadButton kFaces[4] = {
		SDL_GAMEPAD_BUTTON_SOUTH, SDL_GAMEPAD_BUTTON_EAST,
		SDL_GAMEPAD_BUTTON_WEST,  SDL_GAMEPAD_BUTTON_NORTH,
	};
	SDL_GamepadButton found[4] = {
		SDL_GAMEPAD_BUTTON_INVALID, SDL_GAMEPAD_BUTTON_INVALID,
		SDL_GAMEPAD_BUTTON_INVALID, SDL_GAMEPAD_BUTTON_INVALID,
	};
	for (int i = 0; i < 4; ++i) {
		int slot = -1;
		switch (SDL_GetGamepadButtonLabel(gp, kFaces[i])) {
		case SDL_GAMEPAD_BUTTON_LABEL_A:        case SDL_GAMEPAD_BUTTON_LABEL_CROSS:    slot = 0; break;
		case SDL_GAMEPAD_BUTTON_LABEL_B:        case SDL_GAMEPAD_BUTTON_LABEL_CIRCLE:   slot = 1; break;
		case SDL_GAMEPAD_BUTTON_LABEL_X:        case SDL_GAMEPAD_BUTTON_LABEL_SQUARE:   slot = 2; break;
		case SDL_GAMEPAD_BUTTON_LABEL_Y:        case SDL_GAMEPAD_BUTTON_LABEL_TRIANGLE: slot = 3; break;
		default: break;
		}
		if (slot >= 0) found[slot] = kFaces[i];
	}
	// 4 つ揃ったときだけ採用 (中途半端な結果でボタンが消えるのを避ける)
	for (int i = 0; i < 4; ++i) {
		if (found[i] == SDL_GAMEPAD_BUTTON_INVALID) {
			LogFaceButtonsOnce(gp, out, false);
			return;
		}
	}
	for (int i = 0; i < 4; ++i) out[i] = found[i];
	LogFaceButtonsOnce(gp, out, true);
}

// 指定 SDL_Gamepad から 24bit ボタン状態 (doc/Gamepad.md §3.1) を組み立てる。
static tjs_uint32 BuildPadState(SDL_Gamepad *gp)
{
	if (!gp) return 0;

	tjs_uint32 key_state = 0;

	SDL_GamepadButton face[4];
	ResolveFaceButtons(gp, face);

	// ボタン状態を取得
	for (int i=0; i<16; i++) {
		SDL_GamepadButton btn = (i < 4) ? face[i] : button_map[i];
		if (btn != SDL_GAMEPAD_BUTTON_INVALID) {
			if (SDL_GetGamepadButton(gp, btn)) {
				key_state |= (1 << i);
			}
		}
	}

	// 配置基準のフェイスボタン (bit24..27)。 VK_PAD1..4 は刻印で解決した
	// 結果が乗るので、 こちらは SDL の配置 (下/右/左/上) をそのまま載せる。
	// 同じ物理ボタンが両方のビットを立てる (割り当てる側で使い分ける)。
	{
		static const SDL_GamepadButton kFacePos[4] = {
			SDL_GAMEPAD_BUTTON_SOUTH, SDL_GAMEPAD_BUTTON_EAST,
			SDL_GAMEPAD_BUTTON_WEST,  SDL_GAMEPAD_BUTTON_NORTH,
		};
		for (int i = 0; i < 4; i++) {
			if (SDL_GetGamepadButton(gp, kFacePos[i])) key_state |= (1u << (24 + i));
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

// ボタン表記の系統。 画面に出すボタン絵をどれにするか (刻印が A/B/X/Y か
// ×○□△ か) の判定に使う。 判らない機器は空を返して呼び出し側に任せる。
tjs_string SDL3Application::GetPhysicalPadStyle(int phys)
{
	SDL_Gamepad *gp = TVPGetOpenGamepad(phys);
	if (!gp) return tjs_string();
	switch (SDL_GetGamepadType(gp)) {
	case SDL_GAMEPAD_TYPE_XBOX360:
	case SDL_GAMEPAD_TYPE_XBOXONE:
		return tjs_string(TJS_W("xbox"));
	case SDL_GAMEPAD_TYPE_PS3:
	case SDL_GAMEPAD_TYPE_PS4:
	case SDL_GAMEPAD_TYPE_PS5:
		return tjs_string(TJS_W("ps"));
	case SDL_GAMEPAD_TYPE_NINTENDO_SWITCH_PRO:
	case SDL_GAMEPAD_TYPE_NINTENDO_SWITCH_JOYCON_LEFT:
	case SDL_GAMEPAD_TYPE_NINTENDO_SWITCH_JOYCON_RIGHT:
	case SDL_GAMEPAD_TYPE_NINTENDO_SWITCH_JOYCON_PAIR:
		return tjs_string(TJS_W("switch"));
	default:
		break;
	}
	// 種別を持たない汎用パッドは刻印から推測する (SDL が刻印を知っていれば
	// 任天堂配列かどうかは判る)。
	switch (SDL_GetGamepadButtonLabel(gp, SDL_GAMEPAD_BUTTON_SOUTH)) {
	case SDL_GAMEPAD_BUTTON_LABEL_B:     return tjs_string(TJS_W("switch"));
	case SDL_GAMEPAD_BUTTON_LABEL_CROSS: return tjs_string(TJS_W("ps"));
	case SDL_GAMEPAD_BUTTON_LABEL_A:     return tjs_string(TJS_W("xbox"));
	default: break;
	}
	return tjs_string();
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