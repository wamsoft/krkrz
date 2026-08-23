//---------------------------------------------------------------------------
/*
	TVP2 ( T Visual Presenter 2 )  A script authoring tool
	Copyright (C) 2000 W.Dee <dee@kikyou.info> and contributors

	See details of license at "license.txt"
*/
//---------------------------------------------------------------------------
// "System" class interface
//---------------------------------------------------------------------------
#ifndef SystemIntfH
#define SystemIntfH
#include "tjsNative.h"

//---------------------------------------------------------------------------
// tTJSNC_System : TJS System class
//---------------------------------------------------------------------------
class tTJSNC_System : public tTJSNativeClass
{
	typedef tTJSNativeClass inherited;

public:
	tTJSNC_System();
	static tjs_uint32 ClassID;

protected:
	tTJSNativeInstance *CreateNativeInstance();
};
//---------------------------------------------------------------------------
extern tTJSNativeClass * TVPCreateNativeClass_System();
//---------------------------------------------------------------------------

//---------------------------------------------------------------------------
TJS_EXP_FUNC_DEF(ttstr, TVPGetPlatformName, ());
		// retrieve platform name (eg. "Win32")
		// implement in each platform.
TJS_EXP_FUNC_DEF(ttstr, TVPGetPlatformTag, ());
TJS_EXP_FUNC_DEF(ttstr, TVPGetSystemLanguage, ());
		// 正規化したプラットフォームタグ ("windows"/"switch"/"switch2"/"ps5"/
		// "android" ...)。 小文字・空白無しなので、 リソース内の
		// config_<tag>.cf の選択やスクリプト側の機種分岐に使える。
		// 複数該当する場合 (Switch2 = switch + switch2) は最も具体的なものを返す。
		// implement in each platform.
TJS_EXP_FUNC_DEF(ttstr, TVPGetOSName, ());
		// retrieve OS name
		// implement in each platform.
TJS_EXP_FUNC_DEF(ttstr, TVPGetBuildVariantName, ());
		// retrieve variant name (eg. "WIN", "SDL")
		// predefined KRKRZ_VARIANT + KRKRZ_VARIANT_OPTION

extern void TVPFireOnApplicationActivateEvent(bool activate_or_deactivate);
extern void TVPFireOnApplicationTerminating();
extern tjs_int TVPGetOSBits();
extern void TVPFireOnJoypadChange(int no, const tjs_char *name);

//---------------------------------------------------------------------------
#endif
