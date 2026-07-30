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
#include "tp_stub.h"
#include "krmovie.h"
#include "Mpeg1Video.h"
#include "MFSourceReaderVideo.h"
#include "webplayer.h"
#include "OverlayVideo.h"

// exe へ静的統合 (Track V-A) 時 (krmovie_EXPORTS 未定義) はエクスポート不要。
#if defined(_MSC_VER) && defined(krmovie_EXPORTS)
#if defined(_M_AMD64) || defined(_M_X64)
#pragma comment(linker, "/EXPORT:GetAPIVersion")
#pragma comment(linker, "/EXPORT:GetMFVideoOverlayObject")
// GetMixingVideoOverlayObject export は撤去 (VMR9/D3D9 廃止)
#pragma comment(linker, "/EXPORT:GetVideoOverlayObject")
#pragma comment(linker, "/EXPORT:GetVideoLayerObject")
#pragma comment(linker, "/EXPORT:V2Link")
#pragma comment(linker, "/EXPORT:V2Unlink")
#else
#pragma comment(linker, "/EXPORT:GetAPIVersion=_GetAPIVersion@4")
#pragma comment(linker, "/EXPORT:GetMFVideoOverlayObject=_GetMFVideoOverlayObject@28")
// GetMixingVideoOverlayObject export は撤去 (VMR9/D3D9 廃止, x86)
#pragma comment(linker, "/EXPORT:GetVideoOverlayObject=_GetVideoOverlayObject@28")
#pragma comment(linker, "/EXPORT:GetVideoLayerObject=_GetVideoLayerObject@28")
#pragma comment(linker, "/EXPORT:V2Link=_V2Link@4")
#pragma comment(linker, "/EXPORT:V2Unlink=_V2Unlink@0")
#endif
#endif

// 注: exe へ静的統合 (Track V-A) したため DllMain / リソース版 About 文字列
// (旧 krmovie.rc) は撤去。ライセンスは license.txt を参照。

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
// GetAPIVersion
//---------------------------------------------------------------------------
EXPORT(void) GetAPIVersion(DWORD *ver)
{
	*ver = TVP_KRMOVIE_VER;
}
//---------------------------------------------------------------------------



//---------------------------------------------------------------------------
// V2Link : Initialize TVP plugin interface
//---------------------------------------------------------------------------
EXPORT(HRESULT) V2Link(iTVPFunctionExporter *exporter)
{
// メモリ確保位置でブレークを貼るには以下のメソッドで確保番号を指定する。
// ブレークがかかった後は、呼び出し履歴(コールスタック)を見て、どこで確保されたメモリがリークしているか探る。
// _CrtDumpMemoryLeaks でデバッグ出力にリークしたメモリの確保番号が出るので、それを入れればOK
// 確保順が不確定な場合は辛いが、スクリプトを固定すればほぼ同じ順で確保されるはず。
//	_CrtSetBreakAlloc(53);	// 指定された回数目のメモリ確保時にブレークを貼る

	TVPInitImportStub(exporter);

	// (旧: リソース版 About 文字列ログは exe 静的統合で撤去。ライセンスは license.txt)

	return S_OK;
}
//---------------------------------------------------------------------------
// V2Unlink : Uninitialize TVP plugin interface
//---------------------------------------------------------------------------
EXPORT(HRESULT) V2Unlink()
{
	TVPUninitImportStub();

#ifdef _DEBUG
	_CrtDumpMemoryLeaks();
#endif

	return S_OK;
}
//---------------------------------------------------------------------------

