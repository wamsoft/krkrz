#pragma once
//----------------------------------------------------------------------------
//! @file
//! @brief overlay 動画 object の形式ルート (mode 非依存)
//!
//! DirectShow/EVR 撤去 (Track V-D) に伴い、overlay は指定モード (vomOverlay /
//! vomMixer / vomMFEVR) に依らず「形式」で最適経路を選ぶ:
//!   webm     → movie-player (libvpx) + D3D11 YUV present
//!   mpg/mpeg → pl_mpeg + D3D11 YUV present
//!   それ以外 → MF SourceReader + D3D11 BGRA present (wmv/asf/mp4/m4v/mov/avi)
//! いずれも子ウィンドウ D3D11 present で、EVR/MediaSession/DirectShow は使わない。
//----------------------------------------------------------------------------
#include "krmovie.h"   // iTVPVideoOverlay

//! 形式でルートする overlay object を生成する。
//! GetVideoOverlayObject (vomOverlay) / GetMFVideoOverlayObject (vomMixer/vomMFEVR)
//! の双方から呼ばれ、どのモード指定でも同じ結果 (形式依存) になる。
void TVPGetOverlayVideoObject(
	HWND callbackwin, IStream *stream, const tjs_char *streamname,
	const tjs_char *type, unsigned __int64 size, iTVPVideoOverlay **out );
