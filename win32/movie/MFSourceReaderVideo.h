/****************************************************************************/
/*! @file
@brief レイヤ再生用 MF SourceReader ビデオプレイヤ (.wmv/.mp4/.asf 等)

Track V。Media Foundation の IMFSourceReader で映像を RGB32(BGRA) にデコードし、
CPU フレームをレイヤバッファへ配送する。DirectShow の tTVPDSLayerVideo の代替
(MPEG-1 を除く。MPEG-1 は pl_mpeg 版 tTVPMpeg1Video が担当)。
*****************************************************************************/
#ifndef __MF_SOURCE_READER_VIDEO_H__
#define __MF_SOURCE_READER_VIDEO_H__

#include "LayerVideoBase.h"
#include <mfidl.h>
#include <mfreadwrite.h>
#include <atlbase.h>
#include <vector>

class tTVPMFSourceReaderVideo : public tTVPLayerVideoBase
{
public:
	//! overlayOutput=true で overlay 出力 (子ウィンドウ D3D11 BGRA present)。
	tTVPMFSourceReaderVideo( HWND owner, bool overlayOutput = false );
	virtual ~tTVPMFSourceReaderVideo();

protected:
	virtual bool DecoderOpen( IStream *stream, const tjs_char *type, unsigned __int64 size ) override;
	virtual void DecoderClose() override;
	virtual bool DecoderGetInfo( long &width, long &height, double &fps, __int64 &durationMs ) override;
	virtual bool DecoderReadFrame( BYTE *dst, long pitch, __int64 &outPtsMs, bool &eos ) override;
	virtual bool DecoderSeek( __int64 ms ) override;
	virtual bool DecoderHasAudio() override { return HasAudio; }
	virtual void DecoderPumpAudio() override;
	// overlay 経路: フレームを OverlayBuf(top-down BGRA) にデコードして保持 → present。
	virtual bool DecoderDecodeOverlay( __int64 &pts, bool &eos ) override;
	virtual void DecoderPresentOverlay( class tTVPD3D11OverlayWindow *ov ) override;

private:
	//! 1 映像サンプルを読み、dst(=OverlayBuf 用は nullptr 可)へ書くか OverlayBuf へ格納する。
	bool ReadOneFrame( BYTE *dst, long pitch, __int64 &outPtsMs, bool &eos, bool toOverlayBuf );

	bool MFStarted;
	CComPtr<IMFByteStream>   ByteStream;
	CComPtr<IMFSourceReader> Reader;
	long   FrameW;
	long   FrameH;
	long   SrcStride;   //!< 出力 RGB32 のストライド (符号で上下方向)
	double Fps;
	__int64 Duration;
	bool   HasAudio;
	std::vector<BYTE> OverlayBuf; //!< overlay 用 top-down BGRA フレーム (FrameW*4 * FrameH)
	bool   OverlayFrameValid;     //!< OverlayBuf に有効フレームがあるか
};

#endif // __MF_SOURCE_READER_VIDEO_H__
