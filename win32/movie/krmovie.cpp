//---------------------------------------------------------------------------
// krmovie.cpp ( part of KRMOVIE.DLL )
// (c)2001-2009, W.Dee <dee@kikyou.info> and contributors
//---------------------------------------------------------------------------

/*
	We must separate this module because sucking MS library has a lack of
	compiler portability.

	This requires DirectX7 or later or Windows Media Player 6.4 or later for
	playbacking MPEG streams.

	Modified by T.Imoto <http://www.kaede-software.com>
*/

//---------------------------------------------------------------------------

#include <windows.h>
// Track V-A: exe へ直接統合 (tp_stub 境界を撤去)。engine の実ヘッダを直接参照する。
#include "tjsCommHead.h"
#include "MsgIntf.h"       // TVPThrowExceptionMessage
#include "krmovie.h"
#include "Mpeg1Video.h"
#include "MFSourceReaderVideo.h"
#include "webplayer.h"
#include "OverlayVideo.h"

// Track V-A: krmovie は exe へ直接統合済み (DLL ではない)。旧 DLL エクスポート
// (/EXPORT pragma・DllMain・リソース版 About) は撤去。engine 側 (VideoOvlImpl) は
// これらの関数を extern "C" 宣言で直接呼ぶ。ライセンスは license.txt を参照。

//---------------------------------------------------------------------------
// TVPGetOverlayVideoObject : overlay 動画 object の形式ルート (mode 非依存)
//---------------------------------------------------------------------------
//! DirectShow/EVR 撤去 (Track V-D) に伴い、指定モード (vomOverlay / vomMixer /
//! vomMFEVR) に依らず「形式」で最適経路を選ぶ。GetVideoOverlayObject と
//! GetMFVideoOverlayObject の双方がここへ委譲するため、どのモードを指定しても
//! 同じ結果 (全形式再生可) になる。
//!   webm       → tTVPWebpMovie(overlay)          [movie-player + D3D11 YUV]
//!   mpg/mpeg   → tTVPMpeg1Video(overlay)         [pl_mpeg + D3D11 YUV]
//!   それ以外   → tTVPMFSourceReaderVideo(overlay) [MF SourceReader + D3D11 BGRA]
void TVPGetOverlayVideoObject(
	HWND callbackwin, IStream *stream, const tjs_char * streamname,
	const tjs_char *type, unsigned __int64 size, iTVPVideoOverlay **out)
{
	*out = nullptr;
	const wchar_t *wtype = (const wchar_t*)type;

	// .webm は EVR で扱えない (Matroska demux / VP8/9 / alpha 非対応) → 新 D3D11 経路。
	if( wtype != nullptr && _wcsicmp( wtype, L".webm" ) == 0 )
	{
		tTVPWebpMovie *video = new tTVPWebpMovie( callbackwin, /*overlayOutput=*/true );
		if( video->Open( stream ) ) {
			*out = video;
		} else {
			video->Release();
		}
		return;
	}

	// .mpg/.mpeg (MPEG-1) は MF/EVR にデコーダが無い → pl_mpeg + 新 D3D11 経路。
	if( wtype != nullptr &&
		( _wcsicmp( wtype, L".mpg" ) == 0 || _wcsicmp( wtype, L".mpeg" ) == 0 ) )
	{
		tTVPMpeg1Video *video = new tTVPMpeg1Video( callbackwin, /*overlayOutput=*/true );
		if( video->Open( stream, streamname, type, size ) ) {
			*out = video;
		} else {
			video->Release();
		}
		return;
	}

	// それ以外 (wmv/asf/mp4/m4v/mov/avi/未知拡張子) は MF SourceReader で BGRA へ
	// デコードし、layer と同じ D3D11 子ウィンドウ present で表示する。EVR/MediaSession
	// を使わないので teardown レース (前の動画停止直後の open 失敗) が起きず堅牢。
	tTVPMFSourceReaderVideo *video = new tTVPMFSourceReaderVideo( callbackwin, /*overlayOutput=*/true );
	if( video->Open( stream, streamname, type, size ) ) {
		*out = video;
	} else {
		video->Release();
	}
}
//---------------------------------------------------------------------------
// GetVideoOverlayObject (vomOverlay)
//---------------------------------------------------------------------------
EXPORT(void) GetVideoOverlayObject(
	HWND callbackwin, IStream *stream, const tjs_char * streamname,
	const tjs_char *type, unsigned __int64 size, iTVPVideoOverlay **out)
{
	// vomOverlay 指定。形式ルートは統一ディスパッチャに委譲 (mode 非依存)。
	TVPGetOverlayVideoObject( callbackwin, stream, streamname, type, size, out );
}
//---------------------------------------------------------------------------
// GetMFVideoOverlayObject (vomMixer / vomMFEVR)
//---------------------------------------------------------------------------
//! 旧 EVR(MediaSession) 経路のエントリだが、DirectShow/EVR 撤去 (Track V-D) 後は
//! vomOverlay と同じ統一ディスパッチャ (形式ルート) に委譲する。よって vomMixer /
//! vomMFEVR を指定しても vomOverlay と同結果 (全形式再生可) になる。
EXPORT(void) GetMFVideoOverlayObject(
	HWND callbackwin, IStream *stream, const tjs_char * streamname,
	const tjs_char *type, unsigned __int64 size, iTVPVideoOverlay **out)
{
	TVPGetOverlayVideoObject( callbackwin, stream, streamname, type, size, out );
}
//---------------------------------------------------------------------------



//---------------------------------------------------------------------------
// Track V-A: 旧 DLL プラグイン契約 (GetAPIVersion / V2Link / V2Unlink /
// TVPInitImportStub) は exe 直接統合で不要となり撤去。engine の TVP* 関数は
// tp_stub 経由ではなく実シンボルへ直接リンクされる。
//---------------------------------------------------------------------------

