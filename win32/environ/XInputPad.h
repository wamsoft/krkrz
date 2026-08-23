//---------------------------------------------------------------------------
// XInputPad : WINVER 用ゲームパッド物理プロバイダ (XInput バックエンド)
//---------------------------------------------------------------------------
// tTVPPadManager (common/environ/PadManager) の iTVPPhysicalPadProvider を
// XInput で実装する。最大 4 台 (XInput 仕様)。振動対応。汎用 DirectInput
// パッドは対象外 (Xbox 系 XInput デバイスのみ)。
//---------------------------------------------------------------------------
#pragma once

#include "tjsCommHead.h"
#include "PadManager.h"
#include <vector>

class tTVPXInputPadProvider : public iTVPPhysicalPadProvider
{
	static const int MAX_SLOTS = 4; // XInput はユーザ 0..3 の 4 スロット

	struct tSlot {
		bool connected;
		tjs_uint32 state;      // 24bit ボタン状態 (doc/Gamepad.md §3.1)
		float axis[6];         // TVP_PAD_AXIS_* の 6 軸
		bool rumbling;
		tjs_uint32 rumbleStopTick; // 振動停止予定 tick (0 = 無期限)
		tSlot() : connected(false), state(0), rumbling(false), rumbleStopTick(0) {
			for (int i = 0; i < 6; i++) axis[i] = 0.0f;
		}
	};
	tSlot Slots[MAX_SLOTS];

	// 接続中スロット (XInput ユーザ index) を接続順で保持。物理 index はこの並び。
	std::vector<int> Connected;

	tjs_uint32 LastScanTick; // 未接続スロットの再スキャン間引き用
	bool FirstPoll;

	void ReadSlot(int user);

public:
	tTVPXInputPadProvider();
	virtual ~tTVPXInputPadProvider();

	// 毎フレーム 1 回呼ぶ: 接続スロットの状態を取り込み、Connected を更新する。
	void Poll();

	// --- iTVPPhysicalPadProvider ---
	virtual int GetPhysicalPadCount() override;
	virtual tjs_uint32 GetPhysicalPadState(int phys) override;
	virtual float GetPhysicalPadAxis(int phys, int axisId) override;
	virtual tjs_string GetPhysicalPadName(int phys) override;
	// XInput は Xbox 配列固定
	virtual tjs_string GetPhysicalPadStyle(int phys) override { return (phys >= 0) ? tjs_string(TJS_W("xbox")) : tjs_string(); }
	virtual bool RumblePhysical(int phys, int low, int high, int duration_ms) override;
	virtual bool StopRumblePhysical(int phys) override;
};
//---------------------------------------------------------------------------
