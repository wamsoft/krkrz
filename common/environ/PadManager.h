//---------------------------------------------------------------------------
// PadManager : プラットフォーム非依存のゲームパッド論理管理
//---------------------------------------------------------------------------
// 物理パッド (backend 依存) へのアクセスを iTVPPhysicalPadProvider で抽象化し、
// その上に「論理インデックス」層を載せる。
//
//   論理 0     = 最後に操作したパッド (last-operated) への仮想エイリアス
//   論理 1..N  = 実パッド (物理 index 0..N-1、接続順で安定)
//
// キーイベント (VK_PAD*) は常に論理 0 (= 最後に操作したパッド) を発生源とする。
// バックエンド (SDL_Gamepad / XInput 等) はこのクラスを共通で使う。
//---------------------------------------------------------------------------
#pragma once

#include "tjsCommHead.h"
#include "KeyRepeat.h"
#include <vector>

//---------------------------------------------------------------------------
// iTVPPhysicalPadProvider : 物理パッド 1 台ぶんへの backend 依存アクセス
//   phys は 0..(GetPhysicalPadCount()-1) の物理インデックス。
//---------------------------------------------------------------------------
class iTVPPhysicalPadProvider
{
public:
	virtual ~iTVPPhysicalPadProvider() {}

	// 接続中の物理パッド台数
	virtual int GetPhysicalPadCount() = 0;

	// 指定物理パッドの 24bit ボタン状態 (doc/Gamepad.md §3.1 のビット割り当て)。
	// 未接続・無効 index は 0。
	virtual tjs_uint32 GetPhysicalPadState(int phys) = 0;

	// 指定物理パッドの軸値 (tTVPApplication::TVP_PAD_AXIS_*)。無効時 0.0f。
	virtual float GetPhysicalPadAxis(int phys, int axisId) = 0;

	// 指定物理パッドの機種名 (環境依存)。無効時は空文字列。
	virtual tjs_string GetPhysicalPadName(int phys) = 0;

	// 振動。low/high は 0〜255。未対応/無効時は false。
	virtual bool RumblePhysical(int phys, int low, int high, int duration_ms) = 0;
	virtual bool StopRumblePhysical(int phys) = 0;
};

//---------------------------------------------------------------------------
// tTVPPadManager : 論理インデックス層 + last-operated 追従 + キーイベント生成
//---------------------------------------------------------------------------
class tTVPPadManager
{
	iTVPPhysicalPadProvider *Provider;

	// 最後に操作した物理パッド index (-1 = 未確定)
	int ActivePhysical;

	// 物理パッド毎の前回状態 (どのパッドが新規に押されたか = active 切替判定用)
	std::vector<tjs_uint32> LastPhysStates;

	// 論理 0 のキーイベント生成用ベースライン (active 追従、単一)
	tjs_uint32 LastActiveState;
	tjs_uint32 LastPushedTrigger;

	// キーリピート (十字系 / トリガ系で別グループ)
	tTVPKeyRepeatEmulator CrossKeysRepeater;
	tTVPKeyRepeatEmulator TriggerKeysRepeater;

	// 直近 Update() で生成したキーイベント (VK_PAD* コード)
	std::vector<int> UppedKeys;
	std::vector<int> DownedKeys;
	std::vector<int> RepeatKeys;

	// onJoypadChange 発火用の直近アクティブ名
	tjs_string LastActiveName;
	bool ActiveNameValid;

	// パッド機能の有効/無効。CLI `-joypad=no` 等で無効化できる (サポート用:
	// まれに他デバイスが誤ってパッド認識され誤動作するケースの回避)。
	// TJS `System.padEnabled` / SetEnabled() で実行時にも切替可能。
	bool Enabled;
	bool CliChecked;   // -joypad の CLI 判定を一度だけ行うため
	bool UserOverride; // SetEnabled() が明示的に呼ばれたら true (CLI より優先)

	// 論理 0 の状態からキーイベント差分を生成する内部処理
	void GenerateKeyEvents(tjs_uint32 newstate);

public:
	tTVPPadManager();
	~tTVPPadManager();

	void SetProvider(iTVPPhysicalPadProvider *p) { Provider = p; }

	// パッド機能の有効/無効 (実行時切替。CLI `-joypad` より優先される)。
	void SetEnabled(bool b) { Enabled = b; UserOverride = true; }
	bool IsEnabled() const { return Enabled; }

	// 論理 index -> 物理 index。無効なら -1。
	//   0     : ActivePhysical (未確定なら先頭パッド)
	//   1..N  : 物理 (no-1)
	int LogicalToPhysical(int no);

	// 毎フレーム呼び出し: 全物理パッドを走査して active を更新し、
	// 論理 0 のキーイベント (Upped/Downed/Repeat) を生成する。
	void Update();

	// ウィンドウ非アクティブ時など: 全キーを離した状態として扱う。
	void SuspendState();

	const std::vector<int>& GetUppedKeys()  const { return UppedKeys; }
	const std::vector<int>& GetDownedKeys() const { return DownedKeys; }
	const std::vector<int>& GetRepeatKeys() const { return RepeatKeys; }

	// --- 論理アクセサ (TJS API から利用) ---
	tjs_uint32 GetPadState(int no);
	float      GetPadAxis(int no, int axisId);
	tjs_string GetJoypadType(int no);
	int        GetJoypadCount();
	bool       HasJoypad(int no);
	bool       Rumble(int no, int low, int high, int duration_ms);
	bool       StopRumble(int no);

	// パッドキーの押下状態 (論理 0 = 最後に操作したパッド基準)
	bool GetAsyncKeyState(tjs_uint keycode);
};
//---------------------------------------------------------------------------
