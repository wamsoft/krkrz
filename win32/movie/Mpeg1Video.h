/****************************************************************************/
/*! @file
@brief レイヤ再生用 MPEG-1 ビデオプレイヤ (.mpg/.mpeg, pl_mpeg)

Track V。Media Foundation は MPEG-1 デコーダを持たないため、DirectShow 撤去後の
.mpg (MPEG-1 プログラムストリーム) 再生を pl_mpeg (単一ヘッダ) で担う。
映像を BGRA にデコードし、CPU フレームをレイヤバッファへ配送する。
*****************************************************************************/
#ifndef __MPEG1_VIDEO_H__
#define __MPEG1_VIDEO_H__

#include "LayerVideoBase.h"

class tTVPMpeg1Video : public tTVPLayerVideoBase
{
public:
	tTVPMpeg1Video( HWND owner, bool overlayOutput = false );
	virtual ~tTVPMpeg1Video();

protected:
	virtual bool DecoderOpen( IStream *stream, const tjs_char *type, unsigned __int64 size ) override;
	virtual void DecoderClose() override;
	virtual bool DecoderGetInfo( long &width, long &height, double &fps, __int64 &durationMs ) override;
	virtual bool DecoderReadFrame( BYTE *dst, long pitch, __int64 &outPtsMs, bool &eos ) override;
	virtual bool DecoderSeek( __int64 ms ) override;
	virtual bool DecoderHasAudio() override { return HasAudio; }
	virtual void DecoderPumpAudio() override;
	virtual bool DecoderDecodeOverlay( __int64 &pts, bool &eos ) override;
	virtual void DecoderPresentOverlay( class tTVPD3D11OverlayWindow *ov ) override;

private:
	void   *Plm;        //!< plm_t* (pl_mpeg.h を本ヘッダに晒さないため void*)
	BYTE   *FileData;   //!< ストリーム全体をメモリに読み込んだもの (シーク対応)
	size_t  FileSize;
	long    FrameW;
	long    FrameH;
	double  Fps;
	__int64 Duration;
	bool    HasAudio;
	int     AudioSampleRate;
	void   *LastFrame;  //!< overlay: 直近 plm_decode_video の frame (present まで保持)
};

#endif // __MPEG1_VIDEO_H__
