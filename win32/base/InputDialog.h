//---------------------------------------------------------------------------
// System.inputString 用 テキスト入力モーダルダイアログ (WINVER)
//---------------------------------------------------------------------------
#ifndef InputDialogH
#define InputDialogH

#include "tjs.h"

//! テキスト入力ダイアログを表示する。OK なら true (result に入力文字列)、
//! キャンセルなら false。caption=タイトル / prompt=説明文 / def=初期値。
extern bool TVPInputString(const ttstr &caption, const ttstr &prompt,
	const ttstr &def, ttstr &result);

#endif
