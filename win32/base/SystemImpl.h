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
// 実行ファイルを「引数付きで起動」する専用処理 (URL/ファイルを既定ハンドラで開く
// TVPShellExecute とは別。デスクトップの「プログラム実行」用)。exe は App Paths /
// PATH で解決される (例 "msedge.exe")。非対応プラットフォームでは false を返す。
extern bool TVPExecuteProgram(const ttstr &exe, const ttstr &args);
// メモリ状態の総合ダンプ。FileAllocator + BitmapAllocator の per-allocator stats、
// (Win32) HeapWalk 結果、(M3 で追加) プロセス全体 RSS/VSize をログに出力する。
// System.dumpHeap() / 周期ダンプ / atexit / REPL コマンド からの共通入口。
extern void TVPHeapDump();
//---------------------------------------------------------------------------
#endif
