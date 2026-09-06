//---------------------------------------------------------------------------
/*
	TVP2 ( T Visual Presenter 2 )  A script authoring tool
	Copyright (C) 2000 W.Dee <dee@kikyou.info> and contributors

	See details of license at "license.txt"
*/
//---------------------------------------------------------------------------
// Definition of Messages and Message Related Utilities
//---------------------------------------------------------------------------
#include "tjsCommHead.h"

#include "PluginImpl.h"

#include "Application.h"

#define TVP_MSG_DECL(name, msg) tTJSMessageHolder name(TJS_W(#name), msg);
#define TVP_MSG_DECL_CONST(name, msg) tTJSMessageHolder name(TJS_W(#name), msg, /*regist*/false);
#define TVP_MSG_DECL_NULL(name) tTJSMessageHolder name(TJS_W(#name), TJS_W(#name));
#include "MsgImpl.h"

//---------------------------------------------------------------------------
// version retrieving
//---------------------------------------------------------------------------
void TVPGetVersion(void)
{
	static bool DoGet=true;
	if(DoGet)
	{
		DoGet = false;

		TVPVersionMajor = 0;
		TVPVersionMinor = 0;
		TVPVersionRelease = 0;
		TVPVersionBuild = 0;

		TVPGetFileVersionOf(ExePath().c_str(), TVPVersionMajor, TVPVersionMinor,
			TVPVersionRelease, TVPVersionBuild);
	}
}
