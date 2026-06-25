//---------------------------------------------------------------------------
//!@file Elements ベース UserConfig (Phase 7: 起動時設定 UI)
//
// `-userconf` 起動引数を検出すると、ゲーム本体の初期化前に専用 SDL_Window で
// Elements UI を表示し、設定を編集して .cfu に保存する。終了後、krkrz は
// SDL_APP_SUCCESS で終了する想定。
//
// 実装は sdl3/visual/SDLElementsUserConfig.cpp (SDL3 ビルド専用)。
//---------------------------------------------------------------------------
#ifndef ELEMENTS_USER_CONFIG_H
#define ELEMENTS_USER_CONFIG_H

//! @brief `-userconf` 引数を検出したら UserConfig フローを実行する。
//! @return true: フロー実行済み (caller は graceful exit すべき) /
//!         false: `-userconf` 指定なし、通常起動を継続
bool TVPExecuteElementsUserConfig();

//! @brief UserConfig フローが実行されたかどうかのフラグ操作。
//!        InitializeApplication() が return false する直前にセットし、
//!        SDL_AppInit 側で SDL_APP_FAILURE と区別するために参照する。
void TVPSetUserConfigExitFlag(bool b);
bool TVPGetUserConfigExitFlag();

#endif
