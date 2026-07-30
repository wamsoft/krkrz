/****************************************************************************/
/*! @file
@brief レイヤ再生用 MF SourceReader ビデオプレイヤの実装
*****************************************************************************/
#include <windows.h>
#include "tp_stub.h"
#include "MFSourceReaderVideo.h"
#include "MovieAudioSink.h"
#include "D3D11OverlayWindow.h"
#include <mfapi.h>
#include <mfidl.h>
#include <mferror.h>
#include <Mfreadwrite.h>
#include <propvarutil.h>

#pragma comment(lib, "mfplat.lib")
#pragma comment(lib, "mfreadwrite.lib")
#pragma comment(lib, "mfuuid.lib")
#pragma comment(lib, "propsys.lib")   // PropVariantToInt64

//---------------------------------------------------------------------------
tTVPMFSourceReaderVideo::tTVPMFSourceReaderVideo( HWND owner, bool overlayOutput )
: tTVPLayerVideoBase( owner, overlayOutput ), MFStarted(false)
, FrameW(0), FrameH(0), SrcStride(0), Fps(0.0), Duration(0), HasAudio(false)
, OverlayFrameValid(false)
{
}
//---------------------------------------------------------------------------
tTVPMFSourceReaderVideo::~tTVPMFSourceReaderVideo()
{
	StopThread();     // 先にデコードスレッドを止める (Reader を使うため)
	DecoderClose();
}
//---------------------------------------------------------------------------
bool tTVPMFSourceReaderVideo::DecoderOpen( IStream *stream, const tjs_char *type, unsigned __int64 size )
{
	HRESULT hr;
	if( FAILED( hr = MFStartup( MF_VERSION ) ) ) return false;
	MFStarted = true;

	// IStream → IMFByteStream (OS 標準, baseclasses 非依存, Win7+)。
	if( FAILED( hr = MFCreateMFByteStreamOnStream( stream, &ByteStream ) ) ) return false;

	// SourceReader 生成属性:
	//  - ENABLE_ADVANCED_VIDEO_PROCESSING: 任意入力(H.264/HEVC/VC-1 等)から RGB32 への
	//    変換 (ビデオプロセッサ挿入) を確実にする。
	//  - DISABLE_DXVA: ハードウェア DXVA デコードを無効化しソフトウェアデコードにする。
	//    どうせ CPU へ読み戻すため HW デコードの旨味は無く、H.264 の HW デコーダ MFT が
	//    エンジンの D3D11 デバイスと競合して open がハングする問題を回避する。
	CComPtr<IMFAttributes> readerAttrs;
	if( SUCCEEDED( MFCreateAttributes( &readerAttrs, 2 ) ) )
	{
		readerAttrs->SetUINT32( MF_SOURCE_READER_ENABLE_ADVANCED_VIDEO_PROCESSING, TRUE );
		readerAttrs->SetUINT32( MF_SOURCE_READER_DISABLE_DXVA, TRUE );
	}

	// SourceReader 生成 (映像のみ有効化)
	if( FAILED( hr = MFCreateSourceReaderFromByteStream( ByteStream, readerAttrs, &Reader ) ) ) return false;

	// 全ストリーム無効化 → 最初の映像ストリームのみ有効化
	Reader->SetStreamSelection( (DWORD)MF_SOURCE_READER_ALL_STREAMS, FALSE );
	Reader->SetStreamSelection( (DWORD)MF_SOURCE_READER_FIRST_VIDEO_STREAM, TRUE );

	// 出力形式を RGB32 (=メモリ上 BGRA) に交渉 (MF が内部でデコーダ + 変換器を挿入)
	CComPtr<IMFMediaType> outType;
	if( FAILED( hr = MFCreateMediaType( &outType ) ) ) return false;
	outType->SetGUID( MF_MT_MAJOR_TYPE, MFMediaType_Video );
	outType->SetGUID( MF_MT_SUBTYPE, MFVideoFormat_RGB32 );
	if( FAILED( hr = Reader->SetCurrentMediaType( (DWORD)MF_SOURCE_READER_FIRST_VIDEO_STREAM, NULL, outType ) ) )
		return false;

	// 交渉後の実形式から寸法/ストライド/FPS を取得
	CComPtr<IMFMediaType> cur;
	if( FAILED( hr = Reader->GetCurrentMediaType( (DWORD)MF_SOURCE_READER_FIRST_VIDEO_STREAM, &cur ) ) )
		return false;

	UINT32 w = 0, h = 0;
	MFGetAttributeSize( cur, MF_MT_FRAME_SIZE, &w, &h );
	FrameW = (long)w; FrameH = (long)h;

	UINT32 num = 0, den = 0;
	if( SUCCEEDED( MFGetAttributeRatio( cur, MF_MT_FRAME_RATE, &num, &den ) ) && den != 0 )
		Fps = (double)num / (double)den;
	else
		Fps = 30.0;

	// ストライド (符号で上下方向。取得できなければ既定 = 上下なし top-down 正)
	LONG stride = 0;
	if( SUCCEEDED( cur->GetUINT32( MF_MT_DEFAULT_STRIDE, (UINT32*)&stride ) ) )
		SrcStride = stride;
	else
		SrcStride = (LONG)w * 4; // top-down 既定

	// 長さ (100ns → ms)
	PROPVARIANT var;
	PropVariantInit( &var );
	if( SUCCEEDED( Reader->GetPresentationAttribute( (DWORD)MF_SOURCE_READER_MEDIASOURCE, MF_PD_DURATION, &var ) ) )
	{
		LONGLONG dur100ns = 0;
		if( SUCCEEDED( PropVariantToInt64( var, &dur100ns ) ) )
			Duration = dur100ns / 10000; // ms
	}
	PropVariantClear( &var );

	// 音声ストリーム (あれば PCM 16bit で有効化し、シンクを作る)
	Reader->SetStreamSelection( (DWORD)MF_SOURCE_READER_FIRST_AUDIO_STREAM, TRUE );
	CComPtr<IMFMediaType> aout;
	if( SUCCEEDED( MFCreateMediaType( &aout ) ) )
	{
		aout->SetGUID( MF_MT_MAJOR_TYPE, MFMediaType_Audio );
		aout->SetGUID( MF_MT_SUBTYPE, MFAudioFormat_PCM );
		if( SUCCEEDED( Reader->SetCurrentMediaType( (DWORD)MF_SOURCE_READER_FIRST_AUDIO_STREAM, NULL, aout ) ) )
		{
			CComPtr<IMFMediaType> acur;
			if( SUCCEEDED( Reader->GetCurrentMediaType( (DWORD)MF_SOURCE_READER_FIRST_AUDIO_STREAM, &acur ) ) )
			{
				UINT32 ch = 0, sr = 0, bits = 0;
				acur->GetUINT32( MF_MT_AUDIO_NUM_CHANNELS, &ch );
				acur->GetUINT32( MF_MT_AUDIO_SAMPLES_PER_SECOND, &sr );
				acur->GetUINT32( MF_MT_AUDIO_BITS_PER_SAMPLE, &bits );
				if( ch > 0 && sr > 0 && bits > 0 &&
					CreateAudioSink( (int)ch, (int)sr, (int)bits, /*isFloat=*/false ) )
					HasAudio = true;
			}
		}
	}
	if( !HasAudio )
		Reader->SetStreamSelection( (DWORD)MF_SOURCE_READER_FIRST_AUDIO_STREAM, FALSE );

	return FrameW > 0 && FrameH > 0;
}
//---------------------------------------------------------------------------
void tTVPMFSourceReaderVideo::DecoderClose()
{
	Reader.Release();
	if( ByteStream.p )
	{
		ByteStream->Close();
		ByteStream.Release();
	}
	if( MFStarted )
	{
		MFShutdown();
		MFStarted = false;
	}
}
//---------------------------------------------------------------------------
bool tTVPMFSourceReaderVideo::DecoderGetInfo( long &width, long &height, double &fps, __int64 &durationMs )
{
	width = FrameW; height = FrameH; fps = Fps; durationMs = Duration;
	return FrameW > 0 && FrameH > 0;
}
//---------------------------------------------------------------------------
bool tTVPMFSourceReaderVideo::DecoderReadFrame( BYTE *dst, long pitch, __int64 &outPtsMs, bool &eos )
{
	return ReadOneFrame( dst, pitch, outPtsMs, eos, /*toOverlayBuf=*/false );
}
//---------------------------------------------------------------------------
// 映像サンプルを 1 枚読み、layer なら dst(pitch<0=ボトムアップ)へ、overlay なら
// OverlayBuf(top-down BGRA)へ格納する。共通処理。
bool tTVPMFSourceReaderVideo::ReadOneFrame( BYTE *dst, long pitch, __int64 &outPtsMs, bool &eos, bool toOverlayBuf )
{
	eos = false;
	if( !Reader ) { eos = true; return false; }

	DWORD streamIndex = 0, flags = 0;
	LONGLONG llTimestamp = 0;
	CComPtr<IMFSample> sample;
	HRESULT hr = Reader->ReadSample( (DWORD)MF_SOURCE_READER_FIRST_VIDEO_STREAM, 0,
		&streamIndex, &flags, &llTimestamp, &sample );
	if( FAILED(hr) ) { eos = true; return false; }
	if( flags & MF_SOURCE_READERF_ENDOFSTREAM ) { eos = true; return false; }
	if( !sample ) { outPtsMs = llTimestamp / 10000; return false; } // フレーム無し (継続)

	outPtsMs = llTimestamp / 10000; // 100ns → ms

	CComPtr<IMFMediaBuffer> buffer;
	if( FAILED( hr = sample->ConvertToContiguousBuffer( &buffer ) ) ) return false;

	BYTE *src = nullptr;
	DWORD maxlen = 0, curlen = 0;
	if( FAILED( hr = buffer->Lock( &src, &maxlen, &curlen ) ) ) return false;

	long rowBytes = FrameW * 4;
	bool topDown = ( SrcStride >= 0 );
	long absStride = SrcStride >= 0 ? SrcStride : -SrcStride;
	if( absStride < rowBytes ) absStride = rowBytes;

	if( toOverlayBuf )
	{
		// overlay: top-down で OverlayBuf へ (D3D11 テクスチャは top-down)
		OverlayBuf.resize( (size_t)rowBytes * FrameH );
		BYTE *ob = OverlayBuf.data();
		for( long i = 0; i < FrameH; i++ )
		{
			const BYTE *srcRow = topDown
				? ( src + (size_t)i * absStride )
				: ( src + (size_t)( FrameH - 1 - i ) * absStride );
			BYTE *dstRow = ob + (size_t)i * rowBytes;
			memcpy( dstRow, srcRow, rowBytes );
			for( long x = 3; x < rowBytes; x += 4 ) dstRow[x] = 0xFF;
		}
		OverlayFrameValid = true;
	}
	else
	{
		// layer: dst は image 行 i を dst + i*pitch へ (pitch<0 = ボトムアップ格納)
		for( long i = 0; i < FrameH; i++ )
		{
			const BYTE *srcRow = topDown
				? ( src + (size_t)i * absStride )
				: ( src + (size_t)( FrameH - 1 - i ) * absStride );
			BYTE *dstRow = dst + (ptrdiff_t)i * pitch;
			memcpy( dstRow, srcRow, rowBytes );
			// MF の RGB32 (X8R8G8B8) の X (=α) バイトは未定義。レイヤがα合成モードのとき
			// 透明化してしまわないよう、α無し形式 (wmv/mp4) は不透明 (0xFF) で埋める。
			for( long x = 3; x < rowBytes; x += 4 ) dstRow[x] = 0xFF;
		}
	}

	buffer->Unlock();
	return true;
}
//---------------------------------------------------------------------------
bool tTVPMFSourceReaderVideo::DecoderDecodeOverlay( __int64 &pts, bool &eos )
{
	OverlayFrameValid = false;
	return ReadOneFrame( nullptr, 0, pts, eos, /*toOverlayBuf=*/true );
}
//---------------------------------------------------------------------------
void tTVPMFSourceReaderVideo::DecoderPresentOverlay( tTVPD3D11OverlayWindow *ov )
{
	if( !ov || !OverlayFrameValid || OverlayBuf.empty() ) return;
	ov->PresentBGRA( OverlayBuf.data(), FrameW * 4, FrameW, FrameH );
	// デバッグ: KRMOVIE_OVERLAY_DUMP があれば ~100 フレーム目を BMP 保存 (自己検証用)
	static int frameCount = 0;
	static bool dumped = false;
	if( !dumped && ++frameCount >= 100 ) {
		const char *dp = getenv( "KRMOVIE_OVERLAY_DUMP" );
		if( dp && *dp ) {
			wchar_t wp[1024]; wp[0]=0;
			MultiByteToWideChar( CP_ACP, 0, dp, -1, wp, 1024 );
			ov->DebugSaveLastFrame( wp );
			dumped = true;
		}
	}
}
//---------------------------------------------------------------------------
bool tTVPMFSourceReaderVideo::DecoderSeek( __int64 ms )
{
	if( !Reader ) return false;
	PROPVARIANT var;
	PropVariantInit( &var );
	var.vt = VT_I8;
	var.hVal.QuadPart = ms * 10000; // ms → 100ns
	HRESULT hr = Reader->SetCurrentPosition( GUID_NULL, var );
	PropVariantClear( &var );
	return SUCCEEDED(hr);
}
//---------------------------------------------------------------------------
void tTVPMFSourceReaderVideo::DecoderPumpAudio()
{
	if( !Reader || !Audio || !HasAudio ) return;
	// シンクのキューが十分溜まるまで音声サンプルを読み出して供給する。
	while( Audio->QueuedBuffers() < 8 )
	{
		DWORD sidx = 0, flags = 0;
		LONGLONG ts = 0;
		CComPtr<IMFSample> sample;
		HRESULT hr = Reader->ReadSample( (DWORD)MF_SOURCE_READER_FIRST_AUDIO_STREAM, 0,
			&sidx, &flags, &ts, &sample );
		if( FAILED(hr) ) break;
		if( flags & MF_SOURCE_READERF_ENDOFSTREAM ) break;
		if( !sample ) break; // 今は音声サンプル無し (ギャップ等)。次回に回す。

		CComPtr<IMFMediaBuffer> buf;
		if( FAILED( sample->ConvertToContiguousBuffer( &buf ) ) ) break;
		BYTE *p = nullptr; DWORD cur = 0;
		if( FAILED( buf->Lock( &p, NULL, &cur ) ) ) break;
		Audio->Submit( p, cur );
		buf->Unlock();
	}
}
//---------------------------------------------------------------------------
