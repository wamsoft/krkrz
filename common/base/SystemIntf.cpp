//---------------------------------------------------------------------------
/*
	TVP2 ( T Visual Presenter 2 )  A script authoring tool
	Copyright (C) 2000 W.Dee <dee@kikyou.info> and contributors

	See details of license at "license.txt"
*/
//---------------------------------------------------------------------------
// "System" class interface
//---------------------------------------------------------------------------
#include "tjsCommHead.h"

#include "tjsMessage.h"
#include "SystemIntf.h"
#include "SysInitIntf.h"
#include "SysInitImpl.h"
#include "MsgIntf.h"
#include "GraphicsLoaderIntf.h"
#include "EventIntf.h"
#include "LayerIntf.h"
#include "LayerBitmapIntf.h"
#include "Random.h"
#include "LicenseIntf.h"
#include "tjsArray.h"
#include "tjsDictionary.h"
#include "ThreadIntf.h"   // TVPGetTexUploadStats / TVPResetTexUploadStats
#ifdef TVP_USE_OPENGL
#include "GLTexture.h"    // texUploadUsePBO (転送経路の強制指定)
#endif
#include "ScriptMgnIntf.h"
#include "DebugIntf.h"
#ifdef KRKRZ_USE_REPL
#include "ScreenCapture.h"     // TVPRequestScreenCapture / TVPGetLastScreenCapture
#endif

#ifdef TVP_USE_OPENGL
extern int TVPGetOpenGLESVersion();
#else
int TVPGetOpenGLESVersion()
{
	return 0;
}
void* TVPGLGetProcAddress(const char * procname) 
{
	return nullptr;
}
#endif

#ifndef KRKRZ_VARIANT
#define KRKRZ_VARIANT WIN
#endif

#define STR(x) #x
#define XSTR(x) STR(x)

//---------------------------------------------------------------------------
// TVPGetBuildVariantName
//---------------------------------------------------------------------------
ttstr TVPGetBuildVariantName()
{
	#ifdef KRKRZ_VARIANT_OPTION
	static ttstr variant = ttstr(XSTR(KRKRZ_VARIANT) XSTR(KRKRZ_VARIANT_OPTION));
	#else
	static ttstr variant = ttstr(XSTR(KRKRZ_VARIANT));
	#endif
	return variant;
}

//---------------------------------------------------------------------------
// TVPFireOnApplicationActivateEvent
//---------------------------------------------------------------------------
void TVPFireOnApplicationActivateEvent(bool activate_or_deactivate)
{
	// get the script engine
	tTJS *engine = TVPGetScriptEngine();
	if(!engine)
		return; // the script engine had been shutdown

	// get System.onActivate or System.onDeactivate
	// and call it.
	iTJSDispatch2 * global = TVPGetScriptEngine()->GetGlobalNoAddRef();
	if(!global) return;

	tTJSVariant val;
	tTJSVariant val2;
	tTJSVariantClosure clo;
	tTJSVariantClosure func;

	try
	{
		tjs_error er;
		er = global->PropGet(TJS_MEMBERMUSTEXIST, TJS_W("System"), NULL, &val, global);
		if(TJS_FAILED(er)) return;

		if(val.Type() != tvtObject) return;

		clo = val.AsObjectClosureNoAddRef();

		if(clo.Object == NULL) return;

		clo.PropGet(TJS_MEMBERMUSTEXIST,
				activate_or_deactivate?
					TJS_W("onActivate"):
					TJS_W("onDeactivate"),
			NULL, &val2, NULL);

		if(val2.Type() != tvtObject) return;

		func = val2.AsObjectClosureNoAddRef();
	}
	catch(const eTJS &e)
	{
		// the system should not throw exceptions during retrieving the function
		TVPAddLog( TVPFormatMessage( TVPErrorInRetrievingSystemOnActivateOnDeactivate, e.GetMessage() ) );
		return;
	}

	if(func.Object != NULL) func.FuncCall(0, NULL, NULL, NULL, 0, NULL, NULL);
}
//---------------------------------------------------------------------------
void TVPFireOnApplicationTerminating()
{
	// get the script engine
	tTJS *engine = TVPGetScriptEngine();
	if(!engine)
		return; // the script engine had been shutdown

	// get System.onActivate or System.onDeactivate
	// and call it.
	iTJSDispatch2 * global = TVPGetScriptEngine()->GetGlobalNoAddRef();
	if(!global) return;

	tTJSVariant val;
	tTJSVariant val2;
	tTJSVariantClosure clo;
	tTJSVariantClosure func;

	try
	{
		tjs_error er;
		er = global->PropGet(TJS_MEMBERMUSTEXIST, TJS_W("System"), NULL, &val, global);
		if(TJS_FAILED(er)) return;

		if(val.Type() != tvtObject) return;

		clo = val.AsObjectClosureNoAddRef();

		if(clo.Object == NULL) return;

		clo.PropGet(TJS_MEMBERMUSTEXIST, TJS_W("onTerminating"), NULL, &val2, NULL);

		if(val2.Type() != tvtObject) return;

		func = val2.AsObjectClosureNoAddRef();
	}
	catch(const eTJS &e)
	{
		// the system should not throw exceptions during retrieving the function
		TVPAddLog( TVPFormatMessage( TVPErrorInRetrievingSystemOnActivateOnDeactivate, e.GetMessage() ) );
		return;
	}

	if(func.Object != NULL) func.FuncCall(0, NULL, NULL, NULL, 0, NULL, NULL);
}


//---------------------------------------------------------------------------
// TVPFireOnApplicationActivateEvent
//---------------------------------------------------------------------------
void TVPFireOnJoypadChange(int no, const tjs_char *name)
{
	// get the script engine
	tTJS *engine = TVPGetScriptEngine();
	if(!engine)
		return; // the script engine had been shutdown

	// get System.onActivate or System.onDeactivate
	// and call it.
	iTJSDispatch2 * global = TVPGetScriptEngine()->GetGlobalNoAddRef();
	if(!global) return;

	tTJSVariant val;
	tTJSVariant val2;
	tTJSVariantClosure clo;
	tTJSVariantClosure func;

	try
	{
		tjs_error er;
		er = global->PropGet(TJS_MEMBERMUSTEXIST, TJS_W("System"), NULL, &val, global);
		if(TJS_FAILED(er)) return;

		if(val.Type() != tvtObject) return;

		clo = val.AsObjectClosureNoAddRef();

		if(clo.Object == NULL) return;

		clo.PropGet(TJS_MEMBERMUSTEXIST, TJS_W("onJoypadChange"), NULL, &val2, NULL);

		if(val2.Type() != tvtObject) return;

		func = val2.AsObjectClosureNoAddRef();
	}

	catch(const eTJS &e)
	{
		// the system should not throw exceptions during retrieving the function
		// XXX メッセージ割当必要
		TVPAddLog( TVPFormatMessage( TJS_W("TVPErrorInRetrievingSystemOnActivateOnDeactivate"), e.GetMessage() ) );
		return;
	}

	if(func.Object != NULL) {
		tjs_int paramCount = 2;
		tTJSVariant* paramList[2];
		tTJSVariant param1 = (tjs_int)no;
		tTJSVariant param2 = name;
		paramList[0] = &param1;
		paramList[1] = &param2;
		func.FuncCall(0, NULL, NULL, NULL, paramCount, paramList, NULL);
	}
}
//---------------------------------------------------------------------------




//---------------------------------------------------------------------------
// tTJSNC_System
//---------------------------------------------------------------------------
tjs_uint32 tTJSNC_System::ClassID = -1;
tTJSNC_System::tTJSNC_System() : inherited(TJS_W("System"))
{
	// registration of native members

	TJS_BEGIN_NATIVE_MEMBERS(System)
	TJS_DECL_EMPTY_FINALIZE_METHOD
//----------------------------------------------------------------------
TJS_BEGIN_NATIVE_CONSTRUCTOR_DECL_NO_INSTANCE(/*TJS class name*/System)
{
	return TJS_S_OK;
}
TJS_END_NATIVE_CONSTRUCTOR_DECL(/*TJS class name*/System)
//----------------------------------------------------------------------

//-- methods

//----------------------------------------------------------------------
TJS_BEGIN_NATIVE_METHOD_DECL(/*func. name*/terminate)
{
	int code = numparams > 0 ? static_cast<int>(*param[0]) : 0;
	TVPTerminateAsync(code);

	return TJS_S_OK;
}
TJS_END_NATIVE_STATIC_METHOD_DECL(/*func. name*/terminate)
//----------------------------------------------------------------------
TJS_BEGIN_NATIVE_METHOD_DECL(/*func. name*/exit)
{
	// this method does not return

	int code = numparams > 0 ? static_cast<int>(*param[0]) : 0;
	TVPTerminateSync(code);

	return TJS_S_OK;
}
TJS_END_NATIVE_STATIC_METHOD_DECL(/*func. name*/exit)
//---------------------------------------------------------------------------
TJS_BEGIN_NATIVE_METHOD_DECL(/*func. name*/addContinuousHandler)
{
	// add function to continus handler list

	if(numparams < 1) return TJS_E_BADPARAMCOUNT;

	tTJSVariantClosure clo = param[0]->AsObjectClosureNoAddRef();

	TVPAddContinuousHandler(clo);

	return TJS_S_OK;
}
TJS_END_NATIVE_STATIC_METHOD_DECL(/*func. name*/addContinuousHandler)
//---------------------------------------------------------------------------
#ifdef KRKRZ_USE_REPL
TJS_BEGIN_NATIVE_METHOD_DECL(/*func. name*/captureScreen)
{
	// captureScreen(path [, x, y, w, h]) : 次フレームの present 直前に overlay 込みの
	// 実画面を読み戻して PNG 保存する要求を立てる。実際の保存は DrawDevice::Show()。
	// SDL の Agent.captureScreen と同等だが、Agent 非対応の WINVER でも使えるよう
	// REPL 有効時は System 側に用意する (テスト/検証用)。
	if(numparams < 1) return TJS_E_BADPARAMCOUNT;
	ttstr path = *param[0];
	tjs_int x = (numparams > 1) ? (tjs_int)*param[1] : 0;
	tjs_int y = (numparams > 2) ? (tjs_int)*param[2] : 0;
	tjs_int w = (numparams > 3) ? (tjs_int)*param[3] : 0;
	tjs_int h = (numparams > 4) ? (tjs_int)*param[4] : 0;
	TVPRequestScreenCapture(path, x, y, w, h);
	if(result) *result = path;
	return TJS_S_OK;
}
TJS_END_NATIVE_STATIC_METHOD_DECL(/*func. name*/captureScreen)
//---------------------------------------------------------------------------
#endif // KRKRZ_USE_REPL
TJS_BEGIN_NATIVE_METHOD_DECL(/*func. name*/getLicenseList)
{
	// getLicenseList() : 本体内蔵 + プラグイン登録 + storage (licenses/*.txt) の
	// 全ライセンス一覧を %[name, group, source] の配列で返す。
	// ライセンス表示 UI (フォント選択画面等) を TJS 側で自由に組むための口。
	if(result)
	{
		std::vector<tTVPLicenseInfo> list;
		TVPGetLicenseList(list);
		iTJSDispatch2 *dsp = TJSCreateArrayObject();
		tTJSVariant tmp(dsp, dsp);
		*result = tmp;
		dsp->Release();
		for(tjs_uint i = 0; i < list.size(); i++)
		{
			iTJSDispatch2 *dic = TJSCreateDictionaryObject();
			tTJSVariant tv;
			tv = list[i].Name;   dic->PropSet(TJS_MEMBERENSURE, TJS_W("name"),   nullptr, &tv, dic);
			tv = list[i].Group;  dic->PropSet(TJS_MEMBERENSURE, TJS_W("group"),  nullptr, &tv, dic);
			tv = list[i].Source; dic->PropSet(TJS_MEMBERENSURE, TJS_W("source"), nullptr, &tv, dic);
			tmp = tTJSVariant(dic, dic);
			dic->Release();
			dsp->PropSetByNum(TJS_MEMBERENSURE, i, &tmp, dsp);
		}
	}
	return TJS_S_OK;
}
TJS_END_NATIVE_STATIC_METHOD_DECL(/*func. name*/getLicenseList)
//---------------------------------------------------------------------------
TJS_BEGIN_NATIVE_METHOD_DECL(/*func. name*/getLicenseText)
{
	// getLicenseText(name) : 名前でライセンス文 (UTF-8 収録を文字列化) を返す。
	// 見つからなければ void
	if(numparams < 1) return TJS_E_BADPARAMCOUNT;
	ttstr name = *param[0];
	ttstr text;
	if(TVPGetLicenseText(name, text))
	{
		if(result) *result = text;
	}
	else
	{
		if(result) result->Clear();
	}
	return TJS_S_OK;
}
TJS_END_NATIVE_STATIC_METHOD_DECL(/*func. name*/getLicenseText)
//---------------------------------------------------------------------------
TJS_BEGIN_NATIVE_METHOD_DECL(/*func. name*/removeContinuousHandler)
{
	// remove function from continuous handler list

	if(numparams < 1) return TJS_E_BADPARAMCOUNT;

	tTJSVariantClosure clo = param[0]->AsObjectClosureNoAddRef();

	TVPRemoveContinuousHandler(clo);

	return TJS_S_OK;
}
TJS_END_NATIVE_STATIC_METHOD_DECL(/*func. name*/removeContinuousHandler)
//---------------------------------------------------------------------------
TJS_BEGIN_NATIVE_METHOD_DECL(/*func. name*/toActualColor)
{
	// convert color codes to 0xRRGGBB format.

	if(numparams < 1) return TJS_E_BADPARAMCOUNT;

	if(result)
	{
		tjs_uint32 color = (tjs_int)(*param[0]);
		color = TVPToActualColor(color);
		*result = (tjs_int)color;
	}

	return TJS_S_OK;
}
TJS_END_NATIVE_STATIC_METHOD_DECL(/*func. name*/toActualColor)
//---------------------------------------------------------------------------
TJS_BEGIN_NATIVE_METHOD_DECL(/*func. name*/clearGraphicCache)
{
	// clear graphic cache
	TVPClearGraphicCache();

	return TJS_S_OK;
}
TJS_END_NATIVE_STATIC_METHOD_DECL(/*func. name*/clearGraphicCache)
//---------------------------------------------------------------------------
TJS_BEGIN_NATIVE_METHOD_DECL(/*func. name*/touchImages)
{
	// try to cache graphics

	if(numparams < 1) return TJS_E_BADPARAMCOUNT;

	std::vector<ttstr> storages;
	tTJSVariantClosure array = param[0]->AsObjectClosureNoAddRef();

	tjs_int count = 0;
	while(true)
	{
		tTJSVariant val;
		if(TJS_FAILED(array.Object->PropGetByNum(0, count, &val, array.ObjThis)))
			break;
		if(val.Type() == tvtVoid) break;
		storages.push_back(ttstr(val));
		count++;
	}

	tjs_int64 limit = 0;
	tjs_uint64 timeout = 0;

	if(numparams >= 2 && param[1]->Type() != tvtVoid) limit = (tjs_int64)*param[1];
	if(numparams >= 3 && param[2]->Type() != tvtVoid) timeout = (tjs_int64)*param[2];

	TVPTouchImages(storages, limit, timeout);

	return TJS_S_OK;
}
TJS_END_NATIVE_STATIC_METHOD_DECL(/*func. name*/touchImages)
//---------------------------------------------------------------------------
TJS_BEGIN_NATIVE_METHOD_DECL(/*func. name*/createUUID)
{
	// create UUID
	// return UUID string in form of "43abda37-c597-4646-a279-c27a1373af90"

	tjs_uint8 uuid[16];

	TVPGetRandomBits128(uuid);

	uuid[8] &= 0x3f;
	uuid[8] |= 0x80; // override clock_seq

	uuid[6] &= 0x0f;
	uuid[6] |= 0x40; // override version

	tjs_char buf[40];
	TJS_snprintf(buf, sizeof(buf)/sizeof(tjs_char),
TJS_W("%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x"),
		uuid[ 0], uuid[ 1], uuid[ 2], uuid[ 3],
		uuid[ 4], uuid[ 5], uuid[ 6], uuid[ 7],
		uuid[ 8], uuid[ 9], uuid[10], uuid[11],
		uuid[12], uuid[13], uuid[14], uuid[15]);

	if(result) *result = tTJSVariant(buf);

	return TJS_S_OK;
}
TJS_END_NATIVE_STATIC_METHOD_DECL(/*func. name*/createUUID)
//---------------------------------------------------------------------------
TJS_BEGIN_NATIVE_METHOD_DECL(/*func. name*/assignMessage)
{
	// assign system message

	if(numparams < 2) return TJS_E_BADPARAMCOUNT;

	ttstr id(*param[0]);
	ttstr msg(*param[1]);
	bool createnew = numparams > 2 && param[2]->operator bool();

	bool res = TJSAssignMessage(id.c_str(), msg.c_str(), createnew);

	if(result) *result = tTJSVariant((tjs_int)res);

	return TJS_S_OK;
}
TJS_END_NATIVE_STATIC_METHOD_DECL(/*func. name*/assignMessage)
//---------------------------------------------------------------------------
TJS_BEGIN_NATIVE_METHOD_DECL(/*func. name*/doCompact)
{
	// compact memory usage

	tjs_int level = TVP_COMPACT_LEVEL_MAX;

	if(numparams >= 1 && param[0]->Type() != tvtVoid)
		level = (tjs_int)*param[0];

	TVPDeliverCompactEvent(level);

	return TJS_S_OK;
}
TJS_END_NATIVE_STATIC_METHOD_DECL(/*func. name*/doCompact)
//----------------------------------------------------------------------

//--properties

//----------------------------------------------------------------------
TJS_BEGIN_NATIVE_PROP_DECL(versionString)
{
	TJS_BEGIN_NATIVE_PROP_GETTER
	{
		if (result) *result = TVPGetVersionString();
		return TJS_S_OK;
	}
	TJS_END_NATIVE_PROP_GETTER

	TJS_DENY_NATIVE_PROP_SETTER
}
TJS_END_NATIVE_STATIC_PROP_DECL(versionString)
//----------------------------------------------------------------------
TJS_BEGIN_NATIVE_PROP_DECL(versionInformation)
{
	TJS_BEGIN_NATIVE_PROP_GETTER
	{
		if (result) *result = TVPGetVersionInformation();
		return TJS_S_OK;
	}
	TJS_END_NATIVE_PROP_GETTER

	TJS_DENY_NATIVE_PROP_SETTER
}
TJS_END_NATIVE_STATIC_PROP_DECL(versionInformation)
//----------------------------------------------------------------------
TJS_BEGIN_NATIVE_PROP_DECL(eventDisabled)
{
	TJS_BEGIN_NATIVE_PROP_GETTER
	{
		if (result) *result = TVPGetSystemEventDisabledState();
		return TJS_S_OK;
	}
	TJS_END_NATIVE_PROP_GETTER

	TJS_BEGIN_NATIVE_PROP_SETTER
	{
		TVPSetSystemEventDisabledState(param->operator bool());
		return TJS_S_OK;
	}
	TJS_END_NATIVE_PROP_SETTER
}
TJS_END_NATIVE_STATIC_PROP_DECL(eventDisabled)
//----------------------------------------------------------------------
TJS_BEGIN_NATIVE_PROP_DECL(graphicCacheLimit)
{
	TJS_BEGIN_NATIVE_PROP_GETTER
	{
		if (result) *result = (tjs_int)TVPGetGraphicCacheLimit();
		return TJS_S_OK;
	}
	TJS_END_NATIVE_PROP_GETTER

	TJS_BEGIN_NATIVE_PROP_SETTER
	{
		TVPSetGraphicCacheLimit((tjs_int)*param);
		return TJS_S_OK;
	}
	TJS_END_NATIVE_PROP_SETTER
}
TJS_END_NATIVE_STATIC_PROP_DECL(graphicCacheLimit)
//----------------------------------------------------------------------
TJS_BEGIN_NATIVE_PROP_DECL(platformName)
{
	TJS_BEGIN_NATIVE_PROP_GETTER
	{
		if (result) *result = TVPGetPlatformName();
		return TJS_S_OK;
	}
	TJS_END_NATIVE_PROP_GETTER

	TJS_DENY_NATIVE_PROP_SETTER
}
TJS_END_NATIVE_STATIC_PROP_DECL(platformName)
//----------------------------------------------------------------------
// 正規化したプラットフォームタグ ("windows"/"switch"/"switch2"/"ps5"/...)。
// platformName が SDL の生文字列 ("Nintendo Switch" 等。空白入り・表記ゆれあり)
// なのに対し、こちらは小文字・空白無しなので比較やファイル名に使える。
// リソース内の config_<tag>.cf もこのタグで選択される。
TJS_BEGIN_NATIVE_PROP_DECL(platformTag)
{
	TJS_BEGIN_NATIVE_PROP_GETTER
	{
		if (result) *result = TVPGetPlatformTag();
		return TJS_S_OK;
	}
	TJS_END_NATIVE_PROP_GETTER

	TJS_DENY_NATIVE_PROP_SETTER
}
TJS_END_NATIVE_STATIC_PROP_DECL(platformTag)
//----------------------------------------------------------------------
// 本体 (OS / ハード) の表示言語。 BCP-47 の言語タグ ("ja" / "en-US" /
// "zh-Hant" / "zh-Hans" ...)。 取得できない環境では空文字。
//
// ゲーム側の既定言語をハードの設定に合わせるための口。 言語そのものの
// 選択・保存はゲーム側の責務で、 ここでは「本体が何語設定か」だけを返す。
// 地域まで要らなければ '-' より前を見ればよい。
//
// ★ 旧 getLangName プラグイン (System.getCurrentUILangName) の置き換え。
//    プラグイン版は Win が英語の言語名 ("Japanese")、 NX がコード
//    ("ja"/"cn"/"tw") と戻り値が不揃いで、 PS5 は実装が無く "ja" 固定
//    だった。 こちらは全機種 BCP-47 で統一している。
TJS_BEGIN_NATIVE_PROP_DECL(systemLanguage)
{
	TJS_BEGIN_NATIVE_PROP_GETTER
	{
		if (result) *result = TVPGetSystemLanguage();
		return TJS_S_OK;
	}
	TJS_END_NATIVE_PROP_GETTER

	TJS_DENY_NATIVE_PROP_SETTER
}
TJS_END_NATIVE_STATIC_PROP_DECL(systemLanguage)
//----------------------------------------------------------------------
TJS_BEGIN_NATIVE_PROP_DECL(osName)
{
	TJS_BEGIN_NATIVE_PROP_GETTER
	{
		if (result) *result = TVPGetOSName();
		return TJS_S_OK;
	}
	TJS_END_NATIVE_PROP_GETTER

	TJS_DENY_NATIVE_PROP_SETTER
}
TJS_END_NATIVE_STATIC_PROP_DECL(osName)
//----------------------------------------------------------------------
TJS_BEGIN_NATIVE_PROP_DECL(buildVariantName)
{
	TJS_BEGIN_NATIVE_PROP_GETTER
	{
		if (result) *result = TVPGetBuildVariantName();
		return TJS_S_OK;
	}
	TJS_END_NATIVE_PROP_GETTER

	TJS_DENY_NATIVE_PROP_SETTER
}
TJS_END_NATIVE_STATIC_PROP_DECL(buildVariantName)
//----------------------------------------------------------------------
TJS_BEGIN_NATIVE_PROP_DECL(exitOnWindowClose)
{
	TJS_BEGIN_NATIVE_PROP_GETTER
	{
		if (result) *result = TVPTerminateOnWindowClose;
		return TJS_S_OK;
	}
	TJS_END_NATIVE_PROP_GETTER

	TJS_BEGIN_NATIVE_PROP_SETTER
	{
		TVPTerminateOnWindowClose = 0!=(tjs_int)*param;
		return TJS_S_OK;
	}
	TJS_END_NATIVE_PROP_SETTER
}
TJS_END_NATIVE_STATIC_PROP_DECL(exitOnWindowClose)
//----------------------------------------------------------------------
TJS_BEGIN_NATIVE_PROP_DECL(drawThreadNum)
{
        TJS_BEGIN_NATIVE_PROP_GETTER
          {
            if (result) *result = TVPDrawThreadNum;
            return TJS_S_OK;
          }
        TJS_END_NATIVE_PROP_GETTER
        TJS_BEGIN_NATIVE_PROP_SETTER
          {
            TVPDrawThreadNum = (tjs_int)*param;
            return TJS_S_OK;
          }
	TJS_END_NATIVE_PROP_SETTER
}
TJS_END_NATIVE_STATIC_PROP_DECL(drawThreadNum)
//----------------------------------------------------------------------
TJS_BEGIN_NATIVE_PROP_DECL(processorNum)
{
        TJS_BEGIN_NATIVE_PROP_GETTER
          {
            if (result) *result = TVPGetProcessorNum();
            return TJS_S_OK;
          }
        TJS_END_NATIVE_PROP_GETTER
	TJS_DENY_NATIVE_PROP_SETTER
}
TJS_END_NATIVE_STATIC_PROP_DECL(processorNum)
//----------------------------------------------------------------------
TJS_BEGIN_NATIVE_PROP_DECL(exeBits)
{
        TJS_BEGIN_NATIVE_PROP_GETTER
          {
#ifdef TJS_64BIT_OS
            if (result) *result = 64;
#else
            if (result) *result = 32;
#endif
            return TJS_S_OK;
          }
        TJS_END_NATIVE_PROP_GETTER
	TJS_DENY_NATIVE_PROP_SETTER
}
TJS_END_NATIVE_STATIC_PROP_DECL(exeBits)
//----------------------------------------------------------------------
TJS_BEGIN_NATIVE_PROP_DECL(osBits)
{
        TJS_BEGIN_NATIVE_PROP_GETTER
          {
          	if (result) *result = TVPGetOSBits();
            return TJS_S_OK;
          }
        TJS_END_NATIVE_PROP_GETTER
	TJS_DENY_NATIVE_PROP_SETTER
}
TJS_END_NATIVE_STATIC_PROP_DECL(osBits)
//----------------------------------------------------------------------
TJS_BEGIN_NATIVE_PROP_DECL(exitOnNoWindowStartup)
{
	TJS_BEGIN_NATIVE_PROP_GETTER
	{
		if (result) *result = TVPTerminateOnNoWindowStartup;
		return TJS_S_OK;
	}
	TJS_END_NATIVE_PROP_GETTER
	TJS_BEGIN_NATIVE_PROP_SETTER
	{
		TVPTerminateOnNoWindowStartup = 0!=(tjs_int)*param;
		return TJS_S_OK;
	}
	TJS_END_NATIVE_PROP_SETTER
}
TJS_END_NATIVE_STATIC_PROP_DECL(exitOnNoWindowStartup)
//----------------------------------------------------------------------
TJS_BEGIN_NATIVE_PROP_DECL(isWindows) {
	TJS_BEGIN_NATIVE_PROP_GETTER {
#ifdef __WINVER__
		if (result) *result = (tjs_int)1;
#else
		if (result) *result = (tjs_int)0;
#endif
		return TJS_S_OK;
	}
	TJS_END_NATIVE_PROP_GETTER
	TJS_DENY_NATIVE_PROP_SETTER
}
TJS_END_NATIVE_STATIC_PROP_DECL(isWindows)
//----------------------------------------------------------------------
TJS_BEGIN_NATIVE_PROP_DECL(isGeneric) {
	TJS_BEGIN_NATIVE_PROP_GETTER {
#ifdef __GENERIC__
		if (result) *result = (tjs_int)1;
#else
		if (result) *result = (tjs_int)0;
#endif
		return TJS_S_OK;
	}
	TJS_END_NATIVE_PROP_GETTER
	TJS_DENY_NATIVE_PROP_SETTER
}
TJS_END_NATIVE_STATIC_PROP_DECL(isGeneric)
//----------------------------------------------------------------------
#ifdef TVP_USE_OPENGL
TJS_BEGIN_NATIVE_PROP_DECL(openGLESVersion) {
	TJS_BEGIN_NATIVE_PROP_GETTER {
		if (result) *result = (tjs_int)TVPGetOpenGLESVersion();
		return TJS_S_OK;
	}
	TJS_END_NATIVE_PROP_GETTER
	TJS_DENY_NATIVE_PROP_SETTER
}
TJS_END_NATIVE_STATIC_PROP_DECL(openGLESVersion)
#endif
//----------------------------------------------------------------------
// renderStats プロパティ (static・読取専用):
//   画面バッファ → GPU テクスチャの転送コスト。 常時計測 (ビルドオプション不要)
//   なので、 実機で 「転送が詰まっていないか」 をそのまま確認できる。
//   すべて累積値なので **2 回読んで差分を取り、 経過実時間との比**で見る。
//   Dialog.renderStats (overlay 側) と同じ使い方。
TJS_BEGIN_NATIVE_PROP_DECL(renderStats)
{
	TJS_BEGIN_NATIVE_PROP_GETTER
	{
		if (result) {
			TVPTexUploadStats s;
			TVPGetTexUploadStats(s);
			iTJSDispatch2 * dic = TJSCreateDictionaryObject();
			auto put = [dic](const tjs_char * name, tjs_uint64 v) {
				tjs_int64 sv = (tjs_int64)v;
				tTJSVariant tmp(sv);
				dic->PropSet(TJS_MEMBERENSURE, name, NULL, &tmp, dic);
			};
			// 転送 1 回 = dirty 矩形 1 個。 frames は転送フェーズの実行回数
			// (≒ 画面更新フレーム数) で、 更新の無いフレームは含まれない。
			put(TJS_W("texUploadUs"),    s.upload_ns / 1000);
			put(TJS_W("texUploads"),     s.upload_count);
			put(TJS_W("texUploadBytes"), s.upload_bytes);
			put(TJS_W("frames"),         s.frame_count);
			*result = tTJSVariant(dic, dic);
			dic->Release();
		}
		return TJS_S_OK;
	}
	TJS_END_NATIVE_PROP_GETTER
	TJS_DENY_NATIVE_PROP_SETTER
}
TJS_END_NATIVE_STATIC_PROP_DECL(renderStats)
//----------------------------------------------------------------------
// renderStatsReset メソッド: renderStats のカウンタを 0 に戻す (計測区間の開始)。
TJS_BEGIN_NATIVE_METHOD_DECL(/*func. name*/renderStatsReset)
{
	TVPResetTexUploadStats();
	if (result) *result = 1;
	return TJS_S_OK;
}
TJS_END_NATIVE_METHOD_DECL(/*func. name*/renderStatsReset)
//----------------------------------------------------------------------
#ifdef TVP_USE_OPENGL
// texUploadUsePBO プロパティ (static): 画面転送に PBO を使うかの強制指定。
//   void (既定) = 用途ごとの既定 (本画面 = PBO / overlay = 直接転送)
//   true / false = 両方まとめて強制する
// 実機のように環境変数を渡せない環境で転送経路を A/B するための口。
TJS_BEGIN_NATIVE_PROP_DECL(texUploadUsePBO)
{
	TJS_BEGIN_NATIVE_PROP_GETTER
	{
		if (result) {
			const int o = GLTexture::GetUploadOverride();
			if (o < 0) result->Clear();          // void = 既定のまま
			else       *result = (tjs_int)o;
		}
		return TJS_S_OK;
	}
	TJS_END_NATIVE_PROP_GETTER

	TJS_BEGIN_NATIVE_PROP_SETTER
	{
		if (param->Type() == tvtVoid) GLTexture::SetUploadOverride(-1);
		else                          GLTexture::SetUploadOverride((tjs_int)*param ? 1 : 0);
		return TJS_S_OK;
	}
	TJS_END_NATIVE_PROP_SETTER
}
TJS_END_NATIVE_STATIC_PROP_DECL(texUploadUsePBO)
#endif
//----------------------------------------------------------------------
	TJS_END_NATIVE_MEMBERS


	// register default "exceptionHandler" member
	tTJSVariant val((iTJSDispatch2*)NULL, (iTJSDispatch2*)NULL);
	PropSet(TJS_MEMBERENSURE, TJS_W("exceptionHandler"), NULL, &val, this);

	// and onActivate, onDeactivate
	PropSet(TJS_MEMBERENSURE, TJS_W("onActivate"), NULL, &val, this);
	PropSet(TJS_MEMBERENSURE, TJS_W("onDeactivate"), NULL, &val, this);
	PropSet(TJS_MEMBERENSURE, TJS_W("onJoypadChange"), NULL, &val, this);
}
//---------------------------------------------------------------------------
tTJSNativeInstance * tTJSNC_System::CreateNativeInstance()
{
	// this class cannot create an instance
	TVPThrowExceptionMessage(TVPCannotCreateInstance);

	return NULL;
}
//---------------------------------------------------------------------------




