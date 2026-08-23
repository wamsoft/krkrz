//---------------------------------------------------------------------------
// PadManager : プラットフォーム非依存のゲームパッド論理管理 (実装)
//---------------------------------------------------------------------------
#include "tjsCommHead.h"
#include "PadManager.h"
#include "tvpinputdefs.h"   // VK_PAD*
#include "SystemIntf.h"     // TVPFireOnJoypadChange
#include "SysInitIntf.h"    // TVPGetCommandLine

//---------------------------------------------------------------------------
// ビット位置 (doc/Gamepad.md §3.1) → VK_PAD* キーコード対応。28 ビット。
// bit24..27 は bit0..3 と同じ物理ボタンを「配置」で指したもの (刻印ではない)。
//---------------------------------------------------------------------------
static const int TVPPadVirtualKeyMap[] = {
	VK_PAD1,        // 0  A
	VK_PAD2,        // 1  B
	VK_PAD3,        // 2  X
	VK_PAD4,        // 3  Y
	VK_PAD5,        // 4  L1
	VK_PAD6,        // 5  R1
	VK_PAD7,        // 6  L2
	VK_PAD8,        // 7  R2
	VK_PAD9,        // 8  BACK/SELECT
	VK_PAD10,       // 9  START
	VK_PAD11,       // 10 L3
	VK_PAD12,       // 11 R3
	VK_PADLEFT,     // 12 ←
	VK_PADUP,       // 13 ↑
	VK_PADRIGHT,    // 14 →
	VK_PADDOWN,     // 15 ↓
	VK_PAD_L_LEFT,  // 16 左スティック ←
	VK_PAD_L_UP,    // 17 左スティック ↑
	VK_PAD_L_RIGHT, // 18 左スティック →
	VK_PAD_L_DOWN,  // 19 左スティック ↓
	VK_PAD_R_LEFT,  // 20 右スティック ←
	VK_PAD_R_UP,    // 21 右スティック ↑
	VK_PAD_R_RIGHT, // 22 右スティック →
	VK_PAD_R_DOWN,  // 23 右スティック ↓
	VK_PAD_FACE_SOUTH, // 24 フェイス 下 (配置基準)
	VK_PAD_FACE_EAST,  // 25 フェイス 右
	VK_PAD_FACE_WEST,  // 26 フェイス 左
	VK_PAD_FACE_NORTH, // 27 フェイス 上
};
#define TVP_NUM_PAD_KEY 28

// 十字系グループ (bit12..23 = 十字 + 左右スティック方向)。トリガ系はその補集合。
static const tjs_uint32 CROSS_GROUP_MASK = 0xfffu << 12;

// active 切替判定に使うマスク。実ボタン + HAT 十字 (bit0..15) のみを見る。
// アナログスティックの方向化ビット (bit16..23) はドリフトで誤切替しないよう除外。
static const tjs_uint32 SWITCH_DETECT_MASK = 0xffffu;

//---------------------------------------------------------------------------
tTVPPadManager::tTVPPadManager()
	: Provider(nullptr), ActivePhysical(-1),
	  LastActiveState(0), LastPushedTrigger(0), ActiveNameValid(false),
	  Enabled(true), CliChecked(false), UserOverride(false)
{
}
//---------------------------------------------------------------------------
tTVPPadManager::~tTVPPadManager()
{
}
//---------------------------------------------------------------------------
int tTVPPadManager::LogicalToPhysical(int no)
{
	if (!Enabled || !Provider) return -1;
	int n = Provider->GetPhysicalPadCount();
	if (n <= 0) return -1;
	if (no == 0) {
		// 論理 0 = 最後に操作したパッド。未確定なら先頭。
		return (ActivePhysical >= 0 && ActivePhysical < n) ? ActivePhysical : 0;
	}
	int phys = no - 1;
	return (phys >= 0 && phys < n) ? phys : -1;
}
//---------------------------------------------------------------------------
void tTVPPadManager::GenerateKeyEvents(tjs_uint32 newstate)
{
	UppedKeys.clear();
	DownedKeys.clear();
	RepeatKeys.clear();

	tjs_uint32 downed = newstate & ~LastActiveState; // newly pressed buttons
	tjs_uint32 upped  = ~newstate & LastActiveState; // newly released buttons

	// キーリピートは十字系とトリガ系で独立に計算する。
	const tjs_uint32 trigger_group_mask = ~CROSS_GROUP_MASK;

	if(!(LastActiveState & CROSS_GROUP_MASK) && (newstate & CROSS_GROUP_MASK))
		CrossKeysRepeater.Down(); // any pressed
	if(!(newstate & CROSS_GROUP_MASK))
		CrossKeysRepeater.Up();   // all released

	if     (downed & trigger_group_mask) TriggerKeysRepeater.Down();
	else if(upped  & trigger_group_mask) TriggerKeysRepeater.Up();

	if(downed & trigger_group_mask) LastPushedTrigger = downed & trigger_group_mask;

	// 押下 / 離しの走査
	for(tjs_int i = 0; i < TVP_NUM_PAD_KEY; i++)
		if((1u<<i) & downed) DownedKeys.push_back(TVPPadVirtualKeyMap[i]);
	for(tjs_int i = 0; i < TVP_NUM_PAD_KEY; i++)
		if((1u<<i) & upped)  UppedKeys.push_back(TVPPadVirtualKeyMap[i]);

	// 十字系のリピート
	tjs_int cnt = CrossKeysRepeater.GetRepeatCount();
	if(cnt)
	{
		tjs_uint32 t = newstate & CROSS_GROUP_MASK;
		do
		{
			for(tjs_int i = 0; i < TVP_NUM_PAD_KEY; i++)
				if((1u<<i) & t) RepeatKeys.push_back(TVPPadVirtualKeyMap[i]);
		} while(--cnt);
	}

	// トリガ系のリピート (最後に押されたトリガ 1 つ)
	cnt = TriggerKeysRepeater.GetRepeatCount();
	if(cnt)
	{
		tjs_uint32 t = LastPushedTrigger;
		do
		{
			// 刻印基準 (bit0..23) と配置基準 (bit24..27) から 1 つずつ。
			// 同じ物理ボタンが両方のビットを立てるので、 片方だけ拾うと
			// もう一方の基準で割り当てたキーがリピートしなくなる。
			for(tjs_int i = 0; i < 24; i++)
			{
				if((1u<<i) & t)
				{
					RepeatKeys.push_back(TVPPadVirtualKeyMap[i]);
					break;
				}
			}
			for(tjs_int i = 24; i < TVP_NUM_PAD_KEY; i++)
			{
				if((1u<<i) & t)
				{
					RepeatKeys.push_back(TVPPadVirtualKeyMap[i]);
					break;
				}
			}
		} while(--cnt);
	}

	LastActiveState = newstate;
}
//---------------------------------------------------------------------------
void tTVPPadManager::Update()
{
	// CLI `-joypad=no/off/false/0` でパッド機能を無効化 (サポート用)。判定は一度だけ。
	if (!CliChecked) {
		CliChecked = true;
		tTJSVariant val;
		// SetEnabled() で実行時に明示指定済みならそちらを優先し CLI は無視。
		if (!UserOverride && TVPGetCommandLine(TJS_W("-joypad"), &val)) {
			ttstr s = val;
			if (s == TJS_W("no") || s == TJS_W("off") ||
			    s == TJS_W("false") || s == TJS_W("0")) {
				Enabled = false;
			}
		}
	}

	if (!Enabled) {
		// パッド無効: 押しっぱなしを解放し、以後ポーリングしない。
		GenerateKeyEvents(0);
		ActivePhysical = -1;
		if (!ActiveNameValid || !LastActiveName.empty()) {
			LastActiveName.clear();
			ActiveNameValid = true;
			TVPFireOnJoypadChange(0, TJS_W(""));
		}
		return;
	}

	if (!Provider) {
		// プロバイダ未設定 = パッド無し。押しっぱなしを解放。
		if (ActiveNameValid && !LastActiveName.empty()) {
			LastActiveName.clear();
			TVPFireOnJoypadChange(0, TJS_W(""));
		}
		ActiveNameValid = true;
		ActivePhysical = -1;
		GenerateKeyEvents(0);
		return;
	}

	int n = Provider->GetPhysicalPadCount();
	if (n < 0) n = 0;
	if ((int)LastPhysStates.size() != n) {
		// 接続台数が変わったら前回状態をリセット
		LastPhysStates.assign(n, 0);
	}

	// 全物理パッドを走査。実ボタンの新規押下があったパッドを active にする。
	std::vector<tjs_uint32> states(n, 0);
	for (int p = 0; p < n; ++p) {
		tjs_uint32 s = Provider->GetPhysicalPadState(p);
		states[p] = s;
		tjs_uint32 downed = s & ~LastPhysStates[p] & SWITCH_DETECT_MASK;
		if (downed) ActivePhysical = p;
		LastPhysStates[p] = s;
	}

	if (n == 0) ActivePhysical = -1;
	else if (ActivePhysical < 0 || ActivePhysical >= n) ActivePhysical = 0;

	// active パッドの識別名が変わったら onJoypadChange(0, name) を発火。
	tjs_string name = (ActivePhysical >= 0)
		? Provider->GetPhysicalPadName(ActivePhysical) : tjs_string();
	if (!ActiveNameValid || name != LastActiveName) {
		LastActiveName = name;
		ActiveNameValid = true;
		TVPFireOnJoypadChange(0, name.c_str());
	}

	// 論理 0 (= active) の状態でキーイベントを生成。
	tjs_uint32 cur = (ActivePhysical >= 0) ? states[ActivePhysical] : 0;
	GenerateKeyEvents(cur);
}
//---------------------------------------------------------------------------
void tTVPPadManager::SuspendState()
{
	// 全キーを離した扱いにして、押しっぱなしを解放する。
	for (auto &s : LastPhysStates) s = 0;
	GenerateKeyEvents(0);
}
//---------------------------------------------------------------------------
tjs_uint32 tTVPPadManager::GetPadState(int no)
{
	int phys = LogicalToPhysical(no);
	if (phys < 0 || !Provider) return 0;
	return Provider->GetPhysicalPadState(phys);
}
//---------------------------------------------------------------------------
float tTVPPadManager::GetPadAxis(int no, int axisId)
{
	int phys = LogicalToPhysical(no);
	if (phys < 0 || !Provider) return 0.0f;
	return Provider->GetPhysicalPadAxis(phys, axisId);
}
//---------------------------------------------------------------------------
tjs_string tTVPPadManager::GetJoypadType(int no)
{
	int phys = LogicalToPhysical(no);
	if (phys < 0 || !Provider) return tjs_string();
	return Provider->GetPhysicalPadName(phys);
}
//---------------------------------------------------------------------------
tjs_string tTVPPadManager::GetJoypadStyle(int no)
{
	int phys = LogicalToPhysical(no);
	if (phys < 0 || !Provider) return tjs_string();
	return Provider->GetPhysicalPadStyle(phys);
}
//---------------------------------------------------------------------------
int tTVPPadManager::GetJoypadCount()
{
	return (Enabled && Provider) ? Provider->GetPhysicalPadCount() : 0;
}
//---------------------------------------------------------------------------
bool tTVPPadManager::HasJoypad(int no)
{
	return LogicalToPhysical(no) >= 0;
}
//---------------------------------------------------------------------------
bool tTVPPadManager::Rumble(int no, int low, int high, int duration_ms)
{
	int phys = LogicalToPhysical(no);
	if (phys < 0 || !Provider) return false;
	return Provider->RumblePhysical(phys, low, high, duration_ms);
}
//---------------------------------------------------------------------------
bool tTVPPadManager::StopRumble(int no)
{
	int phys = LogicalToPhysical(no);
	if (phys < 0 || !Provider) return false;
	return Provider->StopRumblePhysical(phys);
}
//---------------------------------------------------------------------------
bool tTVPPadManager::GetAsyncKeyState(tjs_uint keycode)
{
	int code = -1;
	if (keycode >= VK_PAD1 && keycode <= VK_PAD12) {
		code = keycode - VK_PAD1;
	} else if (keycode >= VK_PADLEFT && keycode <= VK_PADDOWN) {
		code = keycode - VK_PADLEFT + 12;
	} else if (keycode >= VK_PAD_L_LEFT && keycode <= VK_PAD_L_DOWN) {
		code = keycode - VK_PAD_L_LEFT + 16;
	} else if (keycode >= VK_PAD_R_LEFT && keycode <= VK_PAD_R_DOWN) {
		code = keycode - VK_PAD_R_LEFT + 20;
	} else if (keycode >= VK_PAD_FACE_SOUTH && keycode <= VK_PAD_FACE_NORTH) {
		code = keycode - VK_PAD_FACE_SOUTH + 24;
	}
	if (code >= 0) {
		return (LastActiveState & (1u << code)) != 0;
	}
	return false;
}
//---------------------------------------------------------------------------
