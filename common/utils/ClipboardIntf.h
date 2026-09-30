//---------------------------------------------------------------------------
/*
	TVP2 ( T Visual Presenter 2 )  A script authoring tool
	Copyright (C) 2000 W.Dee <dee@kikyou.info> and contributors

	See details of license at "license.txt"
*/
//---------------------------------------------------------------------------
// Clipboard Class interface
//---------------------------------------------------------------------------
#ifndef ClipboardIntfH
#define ClipboardIntfH
#include "tjsNative.h"
#include <vector>

/*[*/
//---------------------------------------------------------------------------
// tTVPClipboardFormat
//---------------------------------------------------------------------------
enum tTVPClipboardFormat
{
	cbfText = 1,
	cbfBitmap = 2
};
/*]*/


//---------------------------------------------------------------------------
// implement these in each platform
//---------------------------------------------------------------------------
TJS_EXP_FUNC_DEF(bool, TVPClipboardHasFormat, (tTVPClipboardFormat format));
TJS_EXP_FUNC_DEF(void, TVPClipboardSetText, (const ttstr & text));
TJS_EXP_FUNC_DEF(bool, TVPClipboardGetText, (ttstr & text));

//---------------------------------------------------------------------------
// ビットマップの受け渡し (Clipboard.setAsBitmap / getAsBitmap)
//
// ピクセルは 32bpp の BGRA (吉里吉里のレイヤと同じ並び) で、上の行から順に
// 詰まっているものとして扱う。プラグインには公開しないので TJS_EXP_FUNC_DEF は
// 付けない (tp_stub の生成対象にしない)。
//
// Windows 以外は未対応 (false を返す)。クリップボードにビットマップを置く機能は
// もともと Windows 専用プラグイン (clipboardEx.dll) が提供していたもの。
//---------------------------------------------------------------------------

// pitch はバイト単位の行送り。負値は受け付けない (呼ぶ側で上から下に並べ替える)
extern bool TVPClipboardSetBitmap(const void * bits, tjs_int width, tjs_int height,
	tjs_int pitch);

// 成功したら dest に幅 * 高さ分の BGRA を上の行から詰めて返す (α は 255)
extern bool TVPClipboardGetBitmap(std::vector<tjs_uint32> & dest, tjs_int & width,
	tjs_int & height);


//---------------------------------------------------------------------------
// tTJSNI_BaseClipboard
//---------------------------------------------------------------------------
class tTJSNI_BaseClipboard : public tTJSNativeInstance
{
public:
	virtual tjs_error TJS_INTF_METHOD
		Construct(tjs_int numparams, tTJSVariant **param,
			iTJSDispatch2 *dsp);
	virtual void TJS_INTF_METHOD
		Invalidate();
};
//---------------------------------------------------------------------------

//---------------------------------------------------------------------------
// tTJSNC_Clipboard : TJS Clipboard Class
//---------------------------------------------------------------------------
class tTJSNC_Clipboard : public tTJSNativeClass
{
	typedef tTJSNativeClass inherited;
public:
	tTJSNC_Clipboard();
	static tjs_uint32 ClassID;

protected:
	tTJSNativeInstance *CreateNativeInstance();
};
//---------------------------------------------------------------------------
extern tTJSNativeClass * TVPCreateNativeClass_Clipboard();
//---------------------------------------------------------------------------
#endif
