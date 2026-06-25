//---------------------------------------------------------------------------
/*
	TVP2 ( T Visual Presenter 2 )  A script authoring tool
	Copyright (C) 2000 W.Dee <dee@kikyou.info> and contributors

	See details of license at "license.txt"
*/
//---------------------------------------------------------------------------
// "Plugins" class interface
//---------------------------------------------------------------------------
#ifndef PluginIntfH
#define PluginIntfH

#include "tjsNative.h"

//---------------------------------------------------------------------------
// tTJSNC_Plugins : TJS Plugins class
//---------------------------------------------------------------------------
class tTJSNC_Plugins : public tTJSNativeClass
{
	typedef tTJSNativeClass inherited;

public:
	tTJSNC_Plugins();
	static tjs_uint32 ClassID;

protected:
	tTJSNativeInstance *CreateNativeInstance();
};
//---------------------------------------------------------------------------
extern tTJSNativeClass * TVPCreateNativeClass_Plugins();
//---------------------------------------------------------------------------


//---------------------------------------------------------------------------
// プラグインのロード/アンロード/ロード可否判定 (共通インタフェース)
//   - TVPLoadPlugin   : 指定名のプラグインをロードする (失敗時例外)
//   - TVPUnloadPlugin : 指定名のプラグインをアンロードする (見つからなければ例外)
//   - TVPCanLoadPlugin: 指定名のプラグインがロード可能かを判定する (例外を投げない)
// 各プラットフォーム (win32 / generic) で個別に実装される。
//---------------------------------------------------------------------------
extern void TVPLoadPlugin(const ttstr & name);
extern bool TVPUnloadPlugin(const ttstr & name);
extern bool TVPCanLoadPlugin(const ttstr & name);
//---------------------------------------------------------------------------


#endif
