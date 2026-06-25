//---------------------------------------------------------------------------
/*
	TVP2 ( T Visual Presenter 2 )  A script authoring tool
	Copyright (C) 2000 W.Dee <dee@kikyou.info> and contributors

	See details of license at "license.txt"
*/

#include "tjsCommHead.h"

#include <mmsystem.h>

//---------------------------------------------------------------------------
// TVPGetRoughTickCount
// 32bit値のtickカウントを得る
//---------------------------------------------------------------------------
tjs_uint32 TVPGetRoughTickCount32()
{
	return timeGetTime();	// win32 mmsystem.h
}
