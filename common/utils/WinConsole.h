//---------------------------------------------------------------------------
// Windows コンソール attach/detach 共通ヘルパ
//
// WINVER (GUI サブシステム) と SDL3 (Windows では GUI サブシステム) の両方で、
// 親プロセス (シェル) のコンソールに attach してログ出力を可視化するために使う。
// 非 Windows プラットフォームでは no-op。
//---------------------------------------------------------------------------
#ifndef WIN_CONSOLE_H
#define WIN_CONSOLE_H

#include "tjsConfig.h"   // tjs_char

// 親プロセスのコンソールに attach する。既に自プロセスがコンソールを持っている
// 場合は何もしない。attach に成功したら内部フラグを立てる。
// 戻り値: attach 済み (または最初からコンソールを持っていた) なら true。
bool TVPAttachWindowsConsole();

// TVPAttachWindowsConsole() で attach したコンソールを解放する。
// もともと attach していない場合は何もしない。
void TVPDetachWindowsConsole();

// 現在、本ヘルパ経由で親コンソールに attach 済みか。
bool TVPIsAttachedWindowsConsole();

// 標準出力へテキストを書き出す (改行は呼び出し側が含める)。
// Windows では GUI サブシステムでも CRT の stdout が親シェルに繋がらないため、
// STD_OUTPUT_HANDLE へ直接書く。真のコンソールなら WriteConsoleW (UTF-16)、
// パイプ/ファイルへのリダイレクトなら UTF-8 で WriteFile。
// 非 Windows では UTF-8 に変換して stdout へ書く。
void TVPWriteStdOutText(const tjs_char *text);

#endif
