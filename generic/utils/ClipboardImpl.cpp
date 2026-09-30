//---------------------------------------------------------------------------
/*
	See details of license at "license.txt"
	
	Android 版は Clipboard 機能を持たない
*/
//---------------------------------------------------------------------------
// Clipboard Class interface
//---------------------------------------------------------------------------
#include "tjsCommHead.h"
#include "ClipboardIntf.h"

//---------------------------------------------------------------------------
// clipboard related functions
//---------------------------------------------------------------------------
bool TVPClipboardHasFormat(tTVPClipboardFormat format)
{
	return false;
}
//---------------------------------------------------------------------------
void TVPClipboardSetText(const ttstr & text)
{
}
//---------------------------------------------------------------------------
bool TVPClipboardGetText(ttstr & text)
{
	return false;
}
//---------------------------------------------------------------------------
//---------------------------------------------------------------------------
// ビットマップの受け渡しは未対応 (Windows 版のみ)
//---------------------------------------------------------------------------
bool TVPClipboardSetBitmap(const void * bits, tjs_int width, tjs_int height,
	tjs_int pitch)
{
	return false;
}
//---------------------------------------------------------------------------
bool TVPClipboardGetBitmap(std::vector<tjs_uint32> & dest, tjs_int & width,
	tjs_int & height)
{
	return false;
}
//---------------------------------------------------------------------------
