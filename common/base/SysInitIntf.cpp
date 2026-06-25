//---------------------------------------------------------------------------
/*
	TVP2 ( T Visual Presenter 2 )  A script authoring tool
	Copyright (C) 2000 W.Dee <dee@kikyou.info> and contributors

	See details of license at "license.txt"
*/
//---------------------------------------------------------------------------
// System Initialization and Uninitialization
//---------------------------------------------------------------------------
#include "tjsCommHead.h"

#include <vector>
#include <algorithm>
#include <functional>

#include "tjsUtils.h"
#include "SysInitIntf.h"
#include "ScriptMgnIntf.h"
#include "LogIntf.h"
#include "tvpgl.h"


//---------------------------------------------------------------------------
// global data
//---------------------------------------------------------------------------
ttstr TVPProjectDir; // project directory (in unified storage name)
ttstr TVPDataPath; // data directory (in unified storage name)
//---------------------------------------------------------------------------

// REPL 起動状態フラグ (常にコンパイルされる TU で定義し、 REPL 無効ビルドでも
// シンボルが存在するようにする)。 REPL.cpp が設定する。
bool TVPReplActive = false;
//---------------------------------------------------------------------------



extern void TVPGL_C_Init();

//---------------------------------------------------------------------------
// TVPStartup for startup functions
//---------------------------------------------------------------------------
static void TVPCauseAtStart();
void TVPStartup()
{
	TVPCauseAtStart();
}

//---------------------------------------------------------------------------
// TVPSystemInit : Entire System Initialization
//---------------------------------------------------------------------------
void TVPSystemInit(void)
{
	TVPBeforeSystemInit();

	TVPInitScriptEngine();

	TVPInitTVPGL();
	TVPGL_C_Init();

	TVPAfterSystemInit();
}
//---------------------------------------------------------------------------





//---------------------------------------------------------------------------
// TVPSystemUninit : System shutdown, cleanup, etc...
//---------------------------------------------------------------------------
static void TVPCauseAtExit();
bool TVPSystemUninitCalled = false;
void TVPSystemUninit(void)
{
	if(TVPSystemUninitCalled) return;
	TVPSystemUninitCalled = true;

	TVPBeforeSystemUninit();

	TVPUninitTVPGL();

	try
	{
		TVPUninitScriptEngine();
	}
	catch(...)
	{
		// ignore errors
	}

	TVPAfterSystemUninit();

	TVPCauseAtExit();
}
//---------------------------------------------------------------------------



//---------------------------------------------------------------------------
// TVPAddAtExitHandler related
//---------------------------------------------------------------------------
struct tTVPAtExitInfo
{
	tTVPAtExitInfo(tjs_int pri, void(*handler)())
	{
		Priority = pri, Handler = handler;
	}

	tjs_int Priority;
	void (*Handler)();

	bool operator <(const tTVPAtExitInfo & r) const
		{ return this->Priority < r.Priority; }
	bool operator >(const tTVPAtExitInfo & r) const
		{ return this->Priority > r.Priority; }
	bool operator ==(const tTVPAtExitInfo & r) const
		{ return this->Priority == r.Priority; }

};

static std::vector<tTVPAtExitInfo> *TVPAtStartInfos = NULL;
//---------------------------------------------------------------------------
void TVPAddAtStartHandler(tjs_int pri, void (*handler)())
{
	if(!TVPAtStartInfos) TVPAtStartInfos = new std::vector<tTVPAtExitInfo>();
	TVPAtStartInfos->push_back(tTVPAtExitInfo(pri, handler));
}
//---------------------------------------------------------------------------
static void TVPCauseAtStart()
{
	if (TVPAtStartInfos) {
		std::sort(TVPAtStartInfos->begin(), TVPAtStartInfos->end()); // descending sort

		for(auto i = TVPAtStartInfos->begin(); i!=TVPAtStartInfos->end(); i++)
		{
			i->Handler();
		}

		delete TVPAtStartInfos;
		TVPAtStartInfos = NULL;
	}
}
//---------------------------------------------------------------------------


static std::vector<tTVPAtExitInfo> *TVPAtExitInfos = NULL;
static bool TVPAtExitShutdown = false;
//---------------------------------------------------------------------------
void TVPAddAtExitHandler(tjs_int pri, void (*handler)())
{
	if(TVPAtExitShutdown) return;

	if(!TVPAtExitInfos) TVPAtExitInfos = new std::vector<tTVPAtExitInfo>();
	TVPAtExitInfos->push_back(tTVPAtExitInfo(pri, handler));
}
//---------------------------------------------------------------------------
static void TVPCauseAtExit()
{
	if(TVPAtExitShutdown) return;
	TVPAtExitShutdown = true;

	std::sort(TVPAtExitInfos->begin(), TVPAtExitInfos->end()); // descending sort

	for(auto i = TVPAtExitInfos->begin(); i!=TVPAtExitInfos->end(); i++)
	{
		i->Handler();
	}

	delete TVPAtExitInfos;
	TVPAtExitInfos = NULL;
}
//---------------------------------------------------------------------------


int TVPGetCommandLineInt(const tjs_char * name, int defaultValue)
{
	tTJSVariant value;
	if(TVPGetCommandLine(name, &value))
	{
		if(value.Type() == tvtString)
		{
			const tjs_char * str = value.GetString();
			if(TJS_stricmp(str, TJS_W("yes")) == 0 ||
				TJS_stricmp(str, TJS_W("on")) == 0 ||
				TJS_stricmp(str, TJS_W("true")) == 0)
			{
				return 1;
			}

			if(TJS_stricmp(str, TJS_W("no")) == 0 ||
				TJS_stricmp(str, TJS_W("off")) == 0 ||
				TJS_stricmp(str, TJS_W("false")) == 0)
			{
				return 0;
			}
		}

		try
		{
			return value.AsInteger();
		}
		catch(...)
		{
			return defaultValue;
		}
	}
	return defaultValue;
}
