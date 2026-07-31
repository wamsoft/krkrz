//---------------------------------------------------------------------------
//!@file Agent 入力注入 / 再描画要求のプラットフォーム seam
//
// Agent クラス (common/environ/AgentControlIntf.cpp) はこの seam 経由で
// メインウィンドウへ入力イベントを注入する。実装はプラットフォームごと:
//   SDL3  : sdl3/environ/AgentInput.cpp  (TTVPWindowForm::SendMouseMessage/SendMessage)
//   WINVER: win32/environ/AgentInput.cpp (TTVPWindowForm::OnMouse*/OnKey*)
// いずれも実入力と同じ経路を通すので、ゲームにも Elements ダイアログにも届く。
// 戻り値 false = メインウィンドウ未生成 (注入先が無い)。
//---------------------------------------------------------------------------
#ifndef AGENT_INPUT_H
#define AGENT_INPUT_H

#include "tjsCommHead.h"

bool TVPAgentInjectMouseMove(int shift, int x, int y);
bool TVPAgentInjectMouseButton(bool down, int button, int shift, int x, int y);
bool TVPAgentInjectWheel(int delta, int shift, int x, int y);
bool TVPAgentInjectKey(bool down, tjs_int64 vk, tjs_int64 shift);

//! @brief アイドル時でもキャプチャ要求が消化されるよう再描画を促す。
void TVPAgentRequestRedraw();

#endif
