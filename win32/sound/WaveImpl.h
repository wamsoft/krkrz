//---------------------------------------------------------------------------
/*
	TVP2 ( T Visual Presenter 2 )  A script authoring tool
	Copyright (C) 2000 W.Dee <dee@kikyou.info> and contributors

	See details of license at "license.txt"
*/
//---------------------------------------------------------------------------
// Wave Player implementation
//   DirectSound は撤去済み (Phase1-3, doc/ModernizationRoadmap.md)。
//   実際の音声再生は miniaudio (QueueSoundBuffer) 経由。本ヘッダは ABI 互換
//   スタブ (TVPGetDirectSound / TVPReleaseDirectSound) と、他ビルドと共有する
//   音量<->減衰(mB) 変換ヘルパの宣言のみを残す (実体は SoundWinCompat.cpp)。
//---------------------------------------------------------------------------
#ifndef WaveImplH
#define WaveImplH

#include "WaveIntf.h"

/*[*/
//---------------------------------------------------------------------------
// IDirectSound former declaration
// (DirectSound は撤去済み。互換 ABI TVPGetDirectSound() の戻り値型のためだけに残す)
//---------------------------------------------------------------------------
#ifndef __DSOUND_INCLUDED__
struct IDirectSound;
#endif
/*]*/

//---------------------------------------------------------------------------
TJS_EXP_FUNC_DEF_ENV(__WINVER__, void, TVPReleaseDirectSound, ());
TJS_EXP_FUNC_DEF_ENV(__WINVER__, IDirectSound *, TVPGetDirectSound, ());
extern void TVPResetVolumeToAllSoundBuffer();
extern tjs_int TVPVolumeToDSAttenuate(tjs_int volume);
extern tjs_int TVPDSAttenuateToVolume(tjs_int att);
extern tjs_int TVPPanToDSAttenuate(tjs_int volume);
extern tjs_int TVPDSAttenuateToPan(tjs_int att);
//---------------------------------------------------------------------------

#endif
