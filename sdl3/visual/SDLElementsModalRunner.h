//---------------------------------------------------------------------------
//!@file 独立 SDL_Window でモーダル Elements ダイアログを実行 (Phase 6c)
//
// 宣言は host 非依存の common/visual/elements/ElementsModalRunner.h へ移設した。
// このヘッダは SDL host 側 (SDLElementsModalRunner.cpp / SDLElementsUserConfig.cpp)
// の既存 include 互換のために残している薄いエイリアス。 実装 (独立 SDL_Window +
// 自前 SDL_PollEvent の nested pump) は SDLElementsModalRunner.cpp。
//---------------------------------------------------------------------------
#ifndef SDL_ELEMENTS_MODAL_RUNNER_H
#define SDL_ELEMENTS_MODAL_RUNNER_H

#include "ElementsModalRunner.h"

#endif
