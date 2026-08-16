//---------------------------------------------------------------------------
/*
	See details of license at "license.txt"
*/
//---------------------------------------------------------------------------
// Clipboard Class interface (SDL3)
//
//   generic 版 (generic/utils/ClipboardImpl.cpp) は「クリップボードを持たない
//   環境」向けの空実装。 SDL3 デスクトップでは OS のクリップボードが使えるので
//   こちらを差し替えてリンクする (sources.cmake の KRKRZ_VARIANT=SDL 分岐)。
//---------------------------------------------------------------------------
#include "tjsCommHead.h"
#include "ClipboardIntf.h"
#include "CharacterSet.h"

#include <SDL3/SDL.h>

//---------------------------------------------------------------------------
// clipboard related functions
//---------------------------------------------------------------------------
bool TVPClipboardHasFormat(tTVPClipboardFormat format)
{
	// 現状 cbfText のみ (Clipboard.hasFormat の仕様どおり)
	if( format != cbfText ) return false;
	return SDL_HasClipboardText();
}
//---------------------------------------------------------------------------
void TVPClipboardSetText(const ttstr & text)
{
	std::string utf8;
	TVPUtf16ToUtf8( utf8, tjs_string(text.c_str()) );
	SDL_SetClipboardText( utf8.c_str() );
}
//---------------------------------------------------------------------------
bool TVPClipboardGetText(ttstr & text)
{
	if( !SDL_HasClipboardText() ) return false;

	// SDL_GetClipboardText は常に非 NULL を返し、 呼び出し側が SDL_free する
	char *utf8 = SDL_GetClipboardText();
	if( !utf8 ) return false;
	if( utf8[0] == '\0' ) { SDL_free(utf8); return false; }

	tjs_string utf16;
	TVPUtf8ToUtf16( utf16, std::string(utf8) );
	SDL_free( utf8 );

	text = ttstr( utf16.c_str() );
	return true;
}
//---------------------------------------------------------------------------
