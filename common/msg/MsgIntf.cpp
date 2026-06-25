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
#include "tjsString.h"

#include "tjsError.h"
//---------------------------------------------------------------------------
// import message strings
//---------------------------------------------------------------------------

#include "MsgIntf.h"
#include "DebugIntf.h"

//---------------------------------------------------------------------------
// TVPFormatMessage
//---------------------------------------------------------------------------
/*
	these functions do :
	replace each %%, %1, %2 into %, p1, p2.
	%1 must appear only once in the message string, otherwise internal
	buffer will overflow. ( %2 must also so )
*/
ttstr TVPFormatMessage(const tjs_char *msg, const ttstr & p1)
{
	tjs_char *p;
	tjs_char * buf = new tjs_char[TJS_strlen(msg) + p1.GetLen() + 1];
	p = buf;
	for(;*msg;msg++,p++)
	{
		if(*msg == TJS_W('%'))
		{
			if(msg[1] == TJS_W('%'))
			{
				// %%
				*p = TJS_W('%');
				msg++;
				continue;
			}
			else if(msg[1] == TJS_W('1'))
			{
				// %1
				TJS_strcpy(p, p1.c_str());
				p += p1.GetLen();
				p--;
				msg++;
				continue;
			}
		}
		*p = *msg;
	}

	*p = 0;

	ttstr ret(buf);
	delete [] buf;
	return ret;
}
ttstr TVPFormatMessage(const tjs_char *msg, const ttstr & p1, const ttstr & p2)
{
	tjs_char *p;
	tjs_char * buf =
		new tjs_char[TJS_strlen(msg) + p1.GetLen() + p2.GetLen() + 1];
	p = buf;
	for(;*msg;msg++,p++)
	{
		if(*msg == TJS_W('%'))
		{
			if(msg[1] == TJS_W('%'))
			{
				// %%
				*p = TJS_W('%');
				msg++;
				continue;
			}
			else if(msg[1] == TJS_W('1'))
			{
				// %1
				TJS_strcpy(p, p1.c_str());
				p += p1.GetLen();
				p--;
				msg++;
				continue;
			}
			else if(msg[1] == TJS_W('2'))
			{
				// %2
				TJS_strcpy(p, p2.c_str());
				p += p2.GetLen();
				p--;
				msg++;
				continue;
			}
		}
		*p = *msg;
	}

	*p = 0;

	ttstr ret(buf);
	delete [] buf;
	return ret;
}
//---------------------------------------------------------------------------




//---------------------------------------------------------------------------
// TVPThrowExceptionMessage
//---------------------------------------------------------------------------
void TVPThrowExceptionMessage(const tjs_char *msg)
{
	throw eTJSError(msg);
}
void TVPThrowExceptionMessage(const tjs_char *msg, const ttstr & p1, tjs_int num)
{
	throw eTJSError(TVPFormatMessage(msg, p1, ttstr(num)));
}
void TVPThrowExceptionMessage(const tjs_char *msg, const ttstr & p1)
{
	throw eTJSError(TVPFormatMessage(msg, p1));
}
void TVPThrowExceptionMessage(const tjs_char *msg, const ttstr & p1,
	const ttstr & p2)
{
	throw eTJSError(TVPFormatMessage(msg, p1, p2));
}
//---------------------------------------------------------------------------




//---------------------------------------------------------------------------
// version retrieving
//---------------------------------------------------------------------------
tjs_int TVPVersionMajor;
tjs_int TVPVersionMinor;
tjs_int TVPVersionRelease;
tjs_int TVPVersionBuild;
//---------------------------------------------------------------------------

const tjs_char* TVPCompileDate = WIDEN(__DATE__);
const tjs_char* TVPCompileTime = WIDEN(__TIME__);

//---------------------------------------------------------------------------
// version information related functions
//---------------------------------------------------------------------------
extern ttstr TVPReadAboutStringFromResource();
ttstr TVPGetAboutString(void)
{
	TVPGetVersion();
	tjs_char verstr[100];
	TJS_snprintf(verstr, sizeof(verstr)/sizeof(tjs_char), TJS_W("%d.%d.%d.%d"),
		TVPVersionMajor, TVPVersionMinor,
		TVPVersionRelease, TVPVersionBuild);

	tjs_char tjsverstr[100];
	TJS_snprintf(tjsverstr, sizeof(tjsverstr)/sizeof(tjs_char), TJS_W("%d.%d.%d"),
		TJSVersionMajor, TJSVersionMinor, TJSVersionRelease);

	return TVPFormatMessage(TVPReadAboutStringFromResource().c_str(), verstr, tjsverstr) + TVPGetImportantLog();
}
//---------------------------------------------------------------------------
ttstr TVPGetLicenseString(void)
{
	TVPGetVersion();
	tjs_char verstr[100];
	TJS_snprintf(verstr, sizeof(verstr)/sizeof(tjs_char), TJS_W("%d.%d.%d.%d"),
		TVPVersionMajor, TVPVersionMinor,
		TVPVersionRelease, TVPVersionBuild);

	tjs_char tjsverstr[100];
	TJS_snprintf(tjsverstr, sizeof(tjsverstr)/sizeof(tjs_char), TJS_W("%d.%d.%d"),
		TJSVersionMajor, TJSVersionMinor, TJSVersionRelease);
	return TVPFormatMessage( TVPReadAboutStringFromResource().c_str(), verstr, tjsverstr );
}
//---------------------------------------------------------------------------
ttstr TVPGetVersionInformation(void)
{
	TVPGetVersion();
	tjs_char verstr[100];
	TJS_snprintf(verstr, sizeof(verstr)/sizeof(tjs_char), TJS_W("%d.%d.%d.%d"),
		TVPVersionMajor, TVPVersionMinor,
		TVPVersionRelease, TVPVersionBuild);

	tjs_char tjsverstr[100];
	TJS_snprintf(tjsverstr, sizeof(tjsverstr)/sizeof(tjs_char), TJS_W("%d.%d.%d"),
		TJSVersionMajor, TJSVersionMinor, TJSVersionRelease);

	ttstr version = TVPFormatMessage(TVPVersionInformation, verstr, tjsverstr);
	tjs_string str = version.AsStdString();
	str = string_replace_all( str, tjs_string_view(TJS_W("%DATE%")), tjs_string_view(TVPCompileDate) );
	str = string_replace_all( str, tjs_string_view(TJS_W("%TIME%")), tjs_string_view(TVPCompileTime) );
	return ttstr(str);
}
//---------------------------------------------------------------------------
ttstr TVPGetVersionString()
{
	TVPGetVersion();
	tjs_char verstr[100];
	TJS_snprintf(verstr, sizeof(verstr)/sizeof(tjs_char), TJS_W("%d.%d.%d.%d"),
		TVPVersionMajor, TVPVersionMinor,
		TVPVersionRelease, TVPVersionBuild);
	return ttstr(verstr);
}
//---------------------------------------------------------------------------






//---------------------------------------------------------------------------
// Versoin retriving
//---------------------------------------------------------------------------
void TVPGetSystemVersion(tjs_int &major, tjs_int &minor,
	tjs_int &release, tjs_int &build)
{
	TVPGetVersion();
	major = TVPVersionMajor;
	minor = TVPVersionMinor;
	release = TVPVersionRelease;
	build = TVPVersionBuild;
}
//---------------------------------------------------------------------------
void TVPGetTJSVersion(tjs_int &major, tjs_int &minor,
	tjs_int &release)
{
	major = TJSVersionMajor;
	minor = TJSVersionMinor;
	release = TJSVersionRelease;
}
//---------------------------------------------------------------------------



