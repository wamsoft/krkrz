//---------------------------------------------------------------------------
//!@file TJS Agent クラス — エージェント / 自動テスト駆動用の制御 API (SDL3 / WINVER)
//
// REPL (または -replfile チャネル) から呼び出して、 入力イベント注入・画面
// キャプチャ・Elements ダイアログ制御を行うためのネイティブクラス。 メソッドは
// インスタンス不要で `Agent.click(...)` のように呼べる (System 同様)。
//
//   Agent.click(x, y);                 // マウス左クリック (論理座標)
//   Agent.keyPress(VK_RETURN);         // キー down+up
//   Agent.text("hello");               // Elements ダイアログへテキスト入力
//   Agent.dialogs();                   // アクティブダイアログ記述の配列
//   Agent.closeAllDialogs();           // 全ダイアログを強制 teardown
//   Agent.captureScreen("cap.png");    // overlay 込みの実画面を PNG 保存
//---------------------------------------------------------------------------
#ifndef AGENT_CONTROL_INTF_H
#define AGENT_CONTROL_INTF_H

#include "tjsNative.h"

class tTJSNC_Agent : public tTJSNativeClass
{
	typedef tTJSNativeClass inherited;

public:
	tTJSNC_Agent();
	static tjs_uint32 ClassID;

protected:
	tTJSNativeInstance* CreateNativeInstance();
};

extern tTJSNativeClass* TVPCreateNativeClass_Agent();

#endif
