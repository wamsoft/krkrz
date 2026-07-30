//---------------------------------------------------------------------------
/*
	TVP2 ( T Visual Presenter 2 )  A script authoring tool
	Copyright (C) 2000 W.Dee <dee@kikyou.info> and contributors

	See details of license at "license.txt"
*/

#include "tjsCommHead.h"

#include "TickCount.h"

//---------------------------------------------------------------------------
// TVPGetRoughTickCount32 / 64
//---------------------------------------------------------------------------
// tick カウントの取得元。
// 旧実装は mmsystem の timeGetTime() (32bit・分解能は timeBeginPeriod 依存) で、
// システム全体のタイマ分解能を 1ms に引き上げる timeBeginPeriod に精度を頼って
// いた。Phase4-2 で timeBeginPeriod を廃止 (システム全体の分解能変更をやめる)
// したのに伴い、タイマ分解能に依存しない高分解能かつ単調増加のカウンタである
// QueryPerformanceCounter を tick 源に採用する。
// (GetTickCount64 は 64bit だが分解能が ~15.6ms と粗く、timeBeginPeriod 廃止で
//  従来 ~1ms あった tick 精度が退化してしまう。QPC なら SDL 版の SDL_GetTicks
//  相当の高分解能を、システムに副作用を与えずに得られる。)
//---------------------------------------------------------------------------
static LARGE_INTEGER TVPGetQPCFrequency()
{
	LARGE_INTEGER freq;
	// QueryPerformanceFrequency は WinXP 以降で常に成功し、周波数は起動中不変。
	::QueryPerformanceFrequency(&freq);
	return freq;
}
//---------------------------------------------------------------------------
tjs_uint64 TVPGetRoughTickCount64()
{
	static const LARGE_INTEGER freq = TVPGetQPCFrequency();
	LARGE_INTEGER count;
	::QueryPerformanceCounter(&count);
	// ミリ秒へ変換。count.QuadPart * 1000 は長時間稼働で 64bit を溢れ得るため、
	// 商と剰余に分けてオーバフローを避ける。
	tjs_uint64 c = (tjs_uint64)count.QuadPart;
	tjs_uint64 f = (tjs_uint64)freq.QuadPart;
	return (c / f) * 1000 + ((c % f) * 1000) / f;
}
//---------------------------------------------------------------------------
tjs_uint32 TVPGetRoughTickCount32()
{
	return (tjs_uint32)TVPGetRoughTickCount64();
}
//---------------------------------------------------------------------------
