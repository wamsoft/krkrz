//---------------------------------------------------------------------------
//!@file メッセージ / オプション記述資材の言語選択
//
// エンジン同梱のメッセージ資材 (messages*.json) とオプション記述
// (optiondesc*.json) は、基底 (suffix 無し = 日本語) + 言語別 suffix 付き
// ("-en" / "-chs" / "-cht") のファイル群で構成される。
// ここでは「言語タグ → 読み込むべき suffix の優先順リスト」への変換と、
// 実効言語タグ (-language= オプション → OS 言語) の解決を共通化する。
//
// 利用側:
//   generic/environ/Application.cpp : messages*.json の選択 (SDL3 / LIB)
//   generic/msg/ReadOptionDesc.cpp  : optiondesc*.json の選択 (SDL3 / LIB)
//   win32/msg/ReadOptionDesc.cpp    : BINARY リソースの選択 (WINVER)
// WINVER の文字列テーブル (string_table_*.rc) は PE リソースの言語解決
// (スレッド UI 言語) に従うため対象外。-language= の反映は
// win32/environ/Application.cpp が SetThreadUILanguage で行う。
//---------------------------------------------------------------------------
#ifndef __MSG_LANGUAGE_H__
#define __MSG_LANGUAGE_H__

#include "tjsCommHead.h"
#include <string>
#include <vector>

//! @brief 言語タグからメッセージ資材のファイル名 suffix 候補を優先順で返す。
//!        タグは BCP-47 ("ja-JP" / "en-US" / "zh-Hant-TW" ...) のほか
//!        短縮形 ("ja" / "en" / "chs" / "cht") も受け付ける (大文字小文字無視)。
//!        返り値の末尾は常に "" (基底 = 日本語資材)。
//!        例: "zh-Hant-TW" → {"-cht", "-chs", "-en", ""}
extern std::vector<std::string> TVPGetMessageResourceSuffixesForTag(const tjs_string &tag);

//! @brief 実効言語タグを返す。優先順: -language= オプション → OS 言語
//!        (TVPGetSystemLanguage)。どちらも無ければ空文字。
//!        TVPGetCommandLine が使える段階 (起動引数解決後) でのみ呼ぶこと。
extern tjs_string TVPGetEffectiveMessageLanguageTag();

//! @brief 上 2 つの合成: 実効言語タグに対する suffix 候補を返す。
extern std::vector<std::string> TVPGetMessageResourceSuffixes();

#endif // __MSG_LANGUAGE_H__
