//---------------------------------------------------------------------------
//!@file エンジン設定 (-userconf) — ブラウザ UI
//
// `-userconf` 付きで起動すると、ゲーム本体の初期化前にローカル HTTP サーバ
// (appserve) を立ててブラウザで設定画面を開き、変更を <exe名>.cfu に保存して
// 終了する。WINVER / SDL3 共通 (デスクトップのみ。KRKRZ_USERCONF_WEB)。
// 実装は common/base/UserConfigWeb.cpp、画面は common/base/userconf_web/。
// 設計: doc/UserConfig.md
//---------------------------------------------------------------------------
#ifndef __USER_CONFIG_H__
#define __USER_CONFIG_H__

#include "tjsCommHead.h"

//! @brief `-userconf` が指定されていれば設定画面を実行する。
//! @return true: 実行した (呼び出し側はゲームを起動せずに終了する) /
//!         false: 指定なし (通常起動を続ける)
//!         KRKRZ_USERCONF_WEB が無いビルドでは常に false。
bool TVPExecuteUserConfig();

//! @brief 設定画面で編集する <exe名>.cfu のフルパス (プラットフォームごとの実装)。
//!        データ保存場所が確定した後に呼ぶこと。対象外の環境では空文字列。
tjs_string TVPGetUserConfigFilePath();

//! @brief 設定画面を実行して終了したかどうか (SDL の SDL_AppInit が
//!        SDL_APP_SUCCESS と SDL_APP_FAILURE を区別するのに使う)。
void TVPSetUserConfigExitFlag(bool b);
bool TVPGetUserConfigExitFlag();

#endif
