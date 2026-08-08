//---------------------------------------------------------------------------
// XInputPad : WINVER 用ゲームパッド物理プロバイダ (XInput バックエンド, 実装)
//---------------------------------------------------------------------------
#include "tjsCommHead.h"
#include "XInputPad.h"

#include <windows.h>
#include <Xinput.h>
#define _USE_MATH_DEFINES
#include <math.h>

#pragma comment(lib, "Xinput.lib")

//---------------------------------------------------------------------------
// アナログスティック → 8 方向 DPAD ビット化 (sdl3/environ/pad.cpp と同一ロジック)。
// x: +右, y: +下 (画面座標系)。半径 0.6 以上で方向ビットを立てる。
//---------------------------------------------------------------------------
enum {
	AK_LEFT  = 0x01,
	AK_UP    = 0x02,
	AK_RIGHT = 0x04,
	AK_DOWN  = 0x08,
};
static void analog_to_key(float x, float y, int key_base, tjs_uint32 &key_state)
{
	if (sqrtf(x*x + y*y) >= 0.6f) {
		int angle = (int)(atan2f(y, x) * 360 / (float)(M_PI * 2) + 360 + 360 / 16) % 360 / 45;
		static const tjs_uint32 stick[] = {
			AK_RIGHT,
			AK_RIGHT | AK_DOWN,
			AK_DOWN,
			AK_LEFT | AK_DOWN,
			AK_LEFT,
			AK_LEFT | AK_UP,
			AK_UP,
			AK_UP | AK_RIGHT };
		key_state |= (stick[angle] << key_base);
	}
}
//---------------------------------------------------------------------------
tTVPXInputPadProvider::tTVPXInputPadProvider()
	: LastScanTick(0), FirstPoll(true)
{
}
//---------------------------------------------------------------------------
tTVPXInputPadProvider::~tTVPXInputPadProvider()
{
	// 終了時に全スロットの振動を止める
	for (int i = 0; i < MAX_SLOTS; i++) {
		if (Slots[i].connected && Slots[i].rumbling) {
			XINPUT_VIBRATION vib = { 0, 0 };
			XInputSetState(i, &vib);
		}
	}
}
//---------------------------------------------------------------------------
void tTVPXInputPadProvider::ReadSlot(int user)
{
	tSlot &s = Slots[user];

	XINPUT_STATE st;
	ZeroMemory(&st, sizeof(st));
	DWORD r = XInputGetState(user, &st);
	if (r != ERROR_SUCCESS) {
		s.connected = false;
		s.state = 0;
		for (int i = 0; i < 6; i++) s.axis[i] = 0.0f;
		s.rumbling = false;
		return;
	}
	s.connected = true;

	const XINPUT_GAMEPAD &g = st.Gamepad;
	tjs_uint32 key = 0;
	const WORD b = g.wButtons;
	if (b & XINPUT_GAMEPAD_A)              key |= (1u << 0);
	if (b & XINPUT_GAMEPAD_B)              key |= (1u << 1);
	if (b & XINPUT_GAMEPAD_X)              key |= (1u << 2);
	if (b & XINPUT_GAMEPAD_Y)              key |= (1u << 3);
	if (b & XINPUT_GAMEPAD_LEFT_SHOULDER)  key |= (1u << 4);
	if (b & XINPUT_GAMEPAD_RIGHT_SHOULDER) key |= (1u << 5);
	// トリガはアナログ (0..255)。閾値 0.8 で L2/R2 ボタン化。
	if (g.bLeftTrigger  > (BYTE)(255 * 0.8f)) key |= (1u << 6);
	if (g.bRightTrigger > (BYTE)(255 * 0.8f)) key |= (1u << 7);
	if (b & XINPUT_GAMEPAD_BACK)           key |= (1u << 8);
	if (b & XINPUT_GAMEPAD_START)          key |= (1u << 9);
	if (b & XINPUT_GAMEPAD_LEFT_THUMB)     key |= (1u << 10);
	if (b & XINPUT_GAMEPAD_RIGHT_THUMB)    key |= (1u << 11);
	if (b & XINPUT_GAMEPAD_DPAD_LEFT)      key |= (1u << 12);
	if (b & XINPUT_GAMEPAD_DPAD_UP)        key |= (1u << 13);
	if (b & XINPUT_GAMEPAD_DPAD_RIGHT)     key |= (1u << 14);
	if (b & XINPUT_GAMEPAD_DPAD_DOWN)      key |= (1u << 15);

	// 軸。XInput の Y は +上なので、画面座標系 (+下) に合わせて符号反転。
	float lx = g.sThumbLX / 32767.0f;
	float ly = -g.sThumbLY / 32767.0f;
	float rx = g.sThumbRX / 32767.0f;
	float ry = -g.sThumbRY / 32767.0f;
	if (lx < -1.0f) lx = -1.0f;
	if (ly < -1.0f) ly = -1.0f;
	if (rx < -1.0f) rx = -1.0f;
	if (ry < -1.0f) ry = -1.0f;

	analog_to_key(lx, ly, 16, key);
	analog_to_key(rx, ry, 20, key);

	s.state = key;
	s.axis[0] = lx; // TVP_PAD_AXIS_LEFTX
	s.axis[1] = ly; // TVP_PAD_AXIS_LEFTY
	s.axis[2] = rx; // TVP_PAD_AXIS_RIGHTX
	s.axis[3] = ry; // TVP_PAD_AXIS_RIGHTY
	s.axis[4] = g.bLeftTrigger  / 255.0f; // TVP_PAD_AXIS_LEFT_TRIGGER
	s.axis[5] = g.bRightTrigger / 255.0f; // TVP_PAD_AXIS_RIGHT_TRIGGER
}
//---------------------------------------------------------------------------
void tTVPXInputPadProvider::Poll()
{
	tjs_uint32 now = (tjs_uint32)::GetTickCount();

	// 未接続スロットの XInputGetState は高コストなので、新規接続の検出は
	// 約 1 秒間引く。接続中スロットは毎フレーム読む。
	bool scanNew = FirstPoll || (now - LastScanTick) >= 1000;
	if (scanNew) LastScanTick = now;
	FirstPoll = false;

	for (int i = 0; i < MAX_SLOTS; i++) {
		if (Slots[i].connected || scanNew) {
			ReadSlot(i);
		}
		// 振動の期限切れ停止
		if (Slots[i].connected && Slots[i].rumbling && Slots[i].rumbleStopTick != 0
		    && (tjs_int32)(now - Slots[i].rumbleStopTick) >= 0) {
			XINPUT_VIBRATION vib = { 0, 0 };
			XInputSetState(i, &vib);
			Slots[i].rumbling = false;
			Slots[i].rumbleStopTick = 0;
		}
	}

	// 接続スロット一覧 (物理 index = 接続ユーザ番号の昇順) を再構築。
	Connected.clear();
	for (int i = 0; i < MAX_SLOTS; i++) {
		if (Slots[i].connected) Connected.push_back(i);
	}
}
//---------------------------------------------------------------------------
int tTVPXInputPadProvider::GetPhysicalPadCount()
{
	return (int)Connected.size();
}
//---------------------------------------------------------------------------
tjs_uint32 tTVPXInputPadProvider::GetPhysicalPadState(int phys)
{
	if (phys < 0 || phys >= (int)Connected.size()) return 0;
	return Slots[Connected[phys]].state;
}
//---------------------------------------------------------------------------
float tTVPXInputPadProvider::GetPhysicalPadAxis(int phys, int axisId)
{
	if (phys < 0 || phys >= (int)Connected.size()) return 0.0f;
	if (axisId < 0 || axisId >= 6) return 0.0f;
	return Slots[Connected[phys]].axis[axisId];
}
//---------------------------------------------------------------------------
tjs_string tTVPXInputPadProvider::GetPhysicalPadName(int phys)
{
	if (phys < 0 || phys >= (int)Connected.size()) return tjs_string();
	// XInput は機種名を提供しないため汎用名を返す (環境依存値)。
	return tjs_string(TJS_W("XInput Controller"));
}
//---------------------------------------------------------------------------
bool tTVPXInputPadProvider::RumblePhysical(int phys, int low, int high, int duration_ms)
{
	if (phys < 0 || phys >= (int)Connected.size()) return false;
	int user = Connected[phys];
	if (low  < 0) low  = 0; if (low  > 255) low  = 255;
	if (high < 0) high = 0; if (high > 255) high = 255;
	XINPUT_VIBRATION vib;
	vib.wLeftMotorSpeed  = (WORD)(low  * 257); // 0..255 -> 0..65535
	vib.wRightMotorSpeed = (WORD)(high * 257);
	if (XInputSetState(user, &vib) != ERROR_SUCCESS) return false;
	Slots[user].rumbling = true;
	// XInput は振動継続時間を持たないので Poll() で期限停止する。
	Slots[user].rumbleStopTick = (duration_ms > 0)
		? ((tjs_uint32)::GetTickCount() + (tjs_uint32)duration_ms) : 0;
	return true;
}
//---------------------------------------------------------------------------
bool tTVPXInputPadProvider::StopRumblePhysical(int phys)
{
	if (phys < 0 || phys >= (int)Connected.size()) return false;
	int user = Connected[phys];
	XINPUT_VIBRATION vib = { 0, 0 };
	bool ok = (XInputSetState(user, &vib) == ERROR_SUCCESS);
	Slots[user].rumbling = false;
	Slots[user].rumbleStopTick = 0;
	return ok;
}
//---------------------------------------------------------------------------
