//---------------------------------------------------------------------------
/*
	TVP2 ( T Visual Presenter 2 )  A script authoring tool
	Copyright (C) 2000 W.Dee <dee@kikyou.info> and contributors

	See details of license at "license.txt"
*/
//---------------------------------------------------------------------------
// DirectSound 撤去 (Phase1-3) に伴う WINVER 用互換シム。
//  - TVPGetDirectSound / TVPReleaseDirectSound: 公開 ABI 互換スタブ
//    (名前・シグネチャ維持。DirectSound は無いので NULL / no-op)
//  - 音量 <-> 減衰(mB) 変換ヘルパ: DirectSound 由来だが純粋な log 数学で、
//    WINVER の動画音量 (VideoOvlImpl) 等が使うため残す。
// 実際の音声再生は miniaudio (QueueSoundBuffer) 経由。
//---------------------------------------------------------------------------
#include "tjsCommHead.h"
#include "SysInitIntf.h"
#include <math.h>

struct IDirectSound;

//---------------------------------------------------------------------------
// ABI 互換スタブ (旧プラグイン互換のため名前・シグネチャ維持)
//---------------------------------------------------------------------------
IDirectSound * TVPGetDirectSound() { return NULL; }
void TVPReleaseDirectSound() {}

//---------------------------------------------------------------------------
// 音量 <-> 減衰(mB) 変換 (旧 WaveImpl.cpp より移設)
//---------------------------------------------------------------------------
static tjs_int TVPVolumeLogFactor = 3322;
static bool TVPLogTableInit = false;
static tjs_int TVPLogTable[101];
static void TVPInitLogTable()
{
	if(TVPLogTableInit) return;
	TVPLogTableInit = true;
	// -wsvolfactor で上書き可 (旧 WaveImpl と同じ既定/範囲)
	tTJSVariant val;
	if(TVPGetCommandLine(TJS_W("-wsvolfactor"), &val))
	{
		tjs_int n = (tjs_int)val;
		if(n > 0 && n < 200000) TVPVolumeLogFactor = n;
	}
	TVPLogTable[0] = -10000;
	for(tjs_int i = 1; i <= 100; i++)
		TVPLogTable[i] = static_cast<tjs_int>( log10((double)i/100.0)*TVPVolumeLogFactor );
}
//---------------------------------------------------------------------------
tjs_int TVPVolumeToDSAttenuate(tjs_int volume)
{
	TVPInitLogTable();
	volume = volume / 1000;
	if(volume > 100) volume = 100;
	if(volume < 0 ) volume = 0;
	return TVPLogTable[volume];
}
//---------------------------------------------------------------------------
tjs_int TVPDSAttenuateToVolume(tjs_int att)
{
	if(att <= -10000) return 0;
	return (tjs_int)(pow(10, (double)att / TVPVolumeLogFactor) * 100.0) * 1000;
}
//---------------------------------------------------------------------------
tjs_int TVPPanToDSAttenuate(tjs_int volume)
{
	TVPInitLogTable();
	volume = volume / 1000;
	if(volume > 100) volume = 100;
	if(volume < -100 ) volume = -100;
	if(volume < 0)
		return TVPLogTable[100 - (-volume)];
	else
		return -TVPLogTable[100 - volume];
}
//---------------------------------------------------------------------------
tjs_int TVPDSAttenuateToPan(tjs_int att)
{
	if(att <= -10000) return -100000;
	if(att >=  10000) return  100000;
	if(att < 0)
		return (100 - (tjs_int)(pow(10, (double)att /  TVPVolumeLogFactor) * 100.0)) * -1000;
	else
		return (100 - (tjs_int)(pow(10, (double)att / -TVPVolumeLogFactor) * 100.0)) *  1000;
}
//---------------------------------------------------------------------------
