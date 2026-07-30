/****************************************************************************/
/*! @file
@brief part of KRLMOVIE.DLL

-----------------------------------------------------------------------------
	Copyright (C) 2004 T.Imoto <http://www.kaede-software.com>
-----------------------------------------------------------------------------
@author		T.Imoto
@date		2004/09/22
@note
			2004/09/22	T.Imoto		
*****************************************************************************/

#include <windows.h>
#include "tp_stub.h"
#include "krmovie.h"
#include "webplayer.h"
#include "MFSourceReaderVideo.h"
#include "Mpeg1Video.h"


//----------------------------------------------------------------------------
//! @brief	  	VideoOverlay Object (レイヤ再生用) を取得する
//! @param		callbackwin : 
//! @param		stream : 
//! @param		streamname : 
//! @param		type : 
//! @param		size : 
//! @param		out : VideoOverlay Object
//! @return		エラー文字列
//----------------------------------------------------------------------------
EXPORT(void) GetVideoLayerObject(
	HWND callbackwin, IStream *stream, const tjs_char * streamname,
	const tjs_char *type, unsigned __int64 size, iTVPVideoOverlay **out)
{
	*out = nullptr;
	const wchar_t *wtype = (const wchar_t*)type;

	// .webm は movie-player (libvpx VP8/9・α対応)。MF は Matroska/VP8/9/alpha 非対応。
	if( wtype != nullptr && _wcsicmp( wtype, L".webm" ) == 0 ) {
		tTVPWebpMovie *video = new tTVPWebpMovie(callbackwin);
		if (video->Open(stream)) {
			*out = video;
		} else {
			delete video;
		}
		return;
	}

	// .mpg/.mpeg (MPEG-1) は pl_mpeg 経路 (MF は MPEG-1 デコーダ非搭載)。
	if( wtype != nullptr &&
		( _wcsicmp( wtype, L".mpg" ) == 0 || _wcsicmp( wtype, L".mpeg" ) == 0 ) ) {
		tTVPMpeg1Video *video = new tTVPMpeg1Video( callbackwin );
		if( video->Open( stream, streamname, type, size ) ) {
			*out = video;
		} else {
			video->Release();
		}
		return;
	}

	// それ以外 (.wmv/.asf/.mp4/.m4v/.mov/.avi/未知拡張子) は Media Foundation
	// SourceReader。DirectShow 撤去 (Track V-D) 後の既定経路。MF が demux/decode
	// できない形式は Open が失敗し null を返す (graceful fail)。
	tTVPMFSourceReaderVideo *video = new tTVPMFSourceReaderVideo( callbackwin );
	if( video->Open( stream, streamname, type, size ) ) {
		*out = video;
	} else {
		video->Release();
	}
}
