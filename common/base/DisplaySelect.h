//---------------------------------------------------------------------------
// -display= 起動オプション (起動するディスプレイの指定) 共通部
//
//   ディスプレイ (モニタ) の列挙自体はプラットフォーム毎に実装が必要なので
//   (WINVER: win32/visual/TVPScreen.cpp / SDL3: sdl3/environ/app.cpp)、
//   ここには
//     ・オプション文字列の取得
//     ・番号 (1 origin) / 名前の部分一致による選択
//     ・一覧のログ出力 (-display=list)
//   という、バリアント間で挙動を揃えたい部分だけを置く。
//---------------------------------------------------------------------------
#ifndef __DISPLAY_SELECT_H__
#define __DISPLAY_SELECT_H__

#include "tjsCommHead.h"
#include <vector>

//! ディスプレイ 1 枚ぶんの情報 (プラットフォーム非依存の共通形)
struct tTVPDisplayEntry
{
	tjs_int index;      //!< 1 origin。-display= に指定する番号
	tjs_string name;    //!< 表示名 (モニタのフレンドリ名等。無ければ空)
	tjs_string device;  //!< デバイス名 (WINVER なら "\\\\.\\DISPLAY1"。無ければ空)
	tjs_int left, top;  //!< 表示位置 (デスクトップ全体座標)
	tjs_int width, height;
	bool primary;       //!< プライマリディスプレイか

	tTVPDisplayEntry() : index(0), left(0), top(0), width(0), height(0), primary(false) {}
};

//! -display= の指定文字列を取得する (未指定/空なら false)
bool TVPGetStartupDisplayOption(tjs_string &opt);

//! 指定が「一覧表示のみ」(list / ? / help) かどうか
bool TVPIsDisplayListRequest(const tjs_string &opt);

//! ディスプレイ一覧をログへ出力する
void TVPLogDisplayList(const std::vector<tTVPDisplayEntry> &list);

/**
 * 指定文字列に対応するディスプレイを list から選ぶ。
 *
 * ・全て数字なら 1 origin の番号指定
 * ・"primary" ならプライマリディスプレイ
 * ・それ以外は name / device に対する部分一致 (ASCII は大小無視)
 *
 * @return list 内の添字。見つからなければ -1
 */
tjs_int TVPMatchDisplay(const tjs_string &opt, const std::vector<tTVPDisplayEntry> &list);

#endif // __DISPLAY_SELECT_H__
