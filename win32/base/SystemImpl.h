//---------------------------------------------------------------------------
/*
	TVP2 ( T Visual Presenter 2 )  A script authoring tool
	Copyright (C) 2000 W.Dee <dee@kikyou.info> and contributors

	See details of license at "license.txt"
*/
//---------------------------------------------------------------------------
// "System" class implementation
//---------------------------------------------------------------------------
#ifndef SystemImplH
#define SystemImplH
//---------------------------------------------------------------------------
TJS_EXP_FUNC_DEF_ENV(__WINVER__, bool, TVPGetAsyncKeyState, (tjs_uint keycode, bool getcurrent = true));
//---------------------------------------------------------------------------
extern void TVPPostApplicationActivateEvent();
extern void TVPPostApplicationDeactivateEvent();
extern bool TVPShellExecute(const ttstr &target, const ttstr &param);
// メモリ状態の総合ダンプ。FileAllocator + BitmapAllocator の per-allocator stats、
// (Win32) HeapWalk 結果、(M3 で追加) プロセス全体 RSS/VSize をログに出力する。
// System.dumpHeap() / 周期ダンプ / atexit / REPL コマンド からの共通入口。
extern void TVPHeapDump();
//---------------------------------------------------------------------------
#endif
