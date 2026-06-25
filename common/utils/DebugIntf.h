//---------------------------------------------------------------------------
/*
	TVP2 ( T Visual Presenter 2 )  A script authoring tool
	Copyright (C) 2000 W.Dee <dee@kikyou.info> and contributors

	See details of license at "license.txt"
*/
//---------------------------------------------------------------------------
// Utilities for Debugging
//---------------------------------------------------------------------------
#ifndef DebugIntfH
#define DebugIntfH

#include "tjsNative.h"

//---------------------------------------------------------------------------
// global definitions
//---------------------------------------------------------------------------
extern bool TVPAutoLogToFileOnError;
extern bool TVPAutoClearLogOnError;
extern bool TVPLoggingToFile;
extern void TVPSetOnLog(void (*func)(const ttstr & line));
TJS_EXP_FUNC_DEF(void, TVPAddLog, (const ttstr &line));
TJS_EXP_FUNC_DEF(void, TVPAddImportantLog, (const ttstr &line));

//---------------------------------------------------------------------------
// TVPPrettyPrint
//
// tTJSVariant を人間可読な文字列に整形する。REPL / デバッグ用途。
//  - void          → "(void)"
//  - null (object) → "(null)"
//  - int/real      → 数値のリテラル表現
//  - string        → ダブルクォート付きエスケープ済み表現
//  - octet         → <% ... %>
//  - array         → [ e1, e2, ... ]  (非 compact は改行インデント付き)
//  - dictionary    → %[ "k" => v, ... ]
//  - その他 object  → "(object: <address>)"
//  - 深さ到達       → 配列/辞書は "[...]" / "%[...]"
//  - 再帰検出       → "(recursion)"
//
// depth: 展開する最大ネスト段数 (0 で容器の中身を畳む)
// compact: true で単一行・最小空白、false で複数行インデント
//---------------------------------------------------------------------------
TJS_EXP_FUNC_DEF(ttstr, TVPPrettyPrint, (const tTJSVariant &variant, int depth, bool compact));
extern ttstr TVPGetLastLog(tjs_uint n);
extern iTJSConsoleOutput * TVPGetTJS2ConsoleOutputGateway();
extern iTJSConsoleOutput * TVPGetTJS2DumpOutputGateway();
extern void TVPTJS2StartDump();
extern void TVPTJS2EndDump();
extern void TVPOnError();
extern ttstr TVPGetImportantLog();
extern void TVPSetLogLocation(const ttstr &loc);
extern tjs_char TVPNativeLogLocation[MAX_PATH];
extern void TVPStartLogToFile(bool clear);


//---------------------------------------------------------------------------
// implement in each platform
//---------------------------------------------------------------------------
//extern void TVPOnErrorHook();
	// called from TVPOnError, on system error.
//---------------------------------------------------------------------------




//---------------------------------------------------------------------------
// tTJSNC_Debug : TJS Debug Class
//---------------------------------------------------------------------------
class tTJSNC_Debug : public tTJSNativeClass
{
public:
	tTJSNC_Debug();

	static tjs_uint32 ClassID;

protected:
	tTJSNativeInstance * CreateNativeInstance();
};
//---------------------------------------------------------------------------
extern tTJSNativeClass * TVPCreateNativeClass_Debug();
//---------------------------------------------------------------------------




#endif
