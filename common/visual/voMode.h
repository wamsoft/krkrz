/****************************************************************************/
/*! @file
@brief ビデオ再生モード

-----------------------------------------------------------------------------
	Copyright (C) 2004 T.Imoto
-----------------------------------------------------------------------------
@author		T.Imoto
@date		2004/09/19
@note
			2004/09/19	T.Imoto		
*****************************************************************************/

#ifndef __VOMODE_H__
#define __VOMODE_H__

/*[*/
//---------------------------------------------------------------------------
// tTVPVideoOverlayMode
//---------------------------------------------------------------------------
enum tTVPVideoOverlayMode {
	vomOverlay,		// Overlay (最前面描画)。既定。MF-native 形式は HW (IMFMediaEngine) デコード
	vomLayer,		// Draw Layer (レイヤ描画)
	vomMixer,		// Overlay + mixer 追加画像。HW を使わず CPU presenter 固定 (mixer 確実描画)
	vomMFEVR,		// 【非推奨エイリアス】旧 Media Foundation + EVR。現在は vomOverlay と同挙動
};
// Track V-D/V-E: DirectShow/EVR 撤去に伴い実モードは vomOverlay / vomLayer / vomMixer。
// - vomOverlay(既定): 形式ルート統一経路。MF-native (mp4/wmv/asf 等) は HW デコード
//   (IMFMediaEngine)、webm/mpg は CPU。mixer 追加画像は描画しない (HW 経路は動画側が
//   presenter を持つため)。-mediaengine=no で HW を無効化し全形式 CPU へ。
// - vomMixer: mixer 追加画像 (setMixingLayer) を確実に描画したい時の指定。HW を使わず必ず
//   CPU presenter 経路にする (mixer を engine 側で合成 + 音声は自前処理=音量制御が engine 統合)。
// - vomMFEVR は互換のため残す (値 3、挙動は vomOverlay と同じ)。




/*]*/

#endif	// __VOMODE_H__
