/****************************************************************************/
/*! @file
@brief レイヤ再生用 MPEG-1 ビデオプレイヤ (pl_mpeg) の実装
*****************************************************************************/
#include <windows.h>
#include "tp_stub.h"
#include "Mpeg1Video.h"
#include "MovieAudioSink.h"
#include "D3D11OverlayWindow.h"
#include <stdlib.h>

#define PL_MPEG_IMPLEMENTATION
#include "pl_mpeg.h"

//---------------------------------------------------------------------------
tTVPMpeg1Video::tTVPMpeg1Video( HWND owner, bool overlayOutput, bool preferI420 )
: tTVPLayerVideoBase( owner, overlayOutput, preferI420 ), Plm(nullptr), FileData(nullptr), FileSize(0)
, FrameW(0), FrameH(0), Fps(0.0), Duration(0), HasAudio(false), AudioSampleRate(0)
, LastFrame(nullptr)
{
}
//---------------------------------------------------------------------------
tTVPMpeg1Video::~tTVPMpeg1Video()
{
	StopThread();     // 先にデコードスレッドを止める (Plm を使うため)
	DecoderClose();
}
//---------------------------------------------------------------------------
bool tTVPMpeg1Video::DecoderOpen( IStream *stream, const tjs_char *type, unsigned __int64 size )
{
	if( stream == nullptr ) return false;

	// ストリーム全体をメモリへ読み込む (pl_mpeg はランダムアクセス=シークに全データを
	// 要するため。背景動画等の用途では許容)。size が不明なら Stat で取得。
	tjs_uint64 total = size;
	if( total == 0 )
	{
		STATSTG stat; ZeroMemory( &stat, sizeof(stat) );
		if( SUCCEEDED( stream->Stat( &stat, STATFLAG_NONAME ) ) )
			total = stat.cbSize.QuadPart;
	}
	if( total == 0 || total > (tjs_uint64)SIZE_MAX ) return false;

	FileSize = (size_t)total;
	FileData = (BYTE*)malloc( FileSize );
	if( FileData == nullptr ) return false;

	// 先頭へシークして全読み込み
	LARGE_INTEGER li; li.QuadPart = 0;
	stream->Seek( li, STREAM_SEEK_SET, nullptr );
	size_t got = 0;
	while( got < FileSize )
	{
		ULONG toread = (ULONG)( ( FileSize - got > 0x100000 ) ? 0x100000 : ( FileSize - got ) );
		ULONG rd = 0;
		HRESULT hr = stream->Read( FileData + got, toread, &rd );
		if( FAILED(hr) || rd == 0 ) break;
		got += rd;
	}
	if( got != FileSize ) { free( FileData ); FileData = nullptr; return false; }

	plm_t *plm = plm_create_with_memory( FileData, FileSize, FALSE );
	if( plm == nullptr ) { free( FileData ); FileData = nullptr; return false; }
	if( !plm_has_headers( plm ) )
	{
		plm_destroy( plm );
		free( FileData ); FileData = nullptr;
		return false;
	}
	plm_set_loop( plm, FALSE );

	FrameW = plm_get_width( plm );
	FrameH = plm_get_height( plm );
	Fps    = plm_get_framerate( plm );
	if( Fps <= 0.0 ) Fps = 30.0;
	Duration = (__int64)( plm_get_duration( plm ) * 1000.0 ); // 秒 → ms

	// 音声 (MP2)。pl_mpeg の音声は常に 2ch interleaved float32。
	if( plm_get_num_audio_streams( plm ) > 0 )
	{
		plm_set_audio_enabled( plm, TRUE );
		AudioSampleRate = plm_get_samplerate( plm );
		if( AudioSampleRate > 0 && CreateAudioSink( 2, AudioSampleRate, 32, /*isFloat=*/true ) )
			HasAudio = true;
		else
			plm_set_audio_enabled( plm, FALSE );
	}
	else
	{
		plm_set_audio_enabled( plm, FALSE );
	}

	Plm = plm;
	return FrameW > 0 && FrameH > 0;
}
//---------------------------------------------------------------------------
void tTVPMpeg1Video::DecoderClose()
{
	if( Plm ) { plm_destroy( (plm_t*)Plm ); Plm = nullptr; }
	if( FileData ) { free( FileData ); FileData = nullptr; }
	FileSize = 0;
}
//---------------------------------------------------------------------------
bool tTVPMpeg1Video::DecoderGetInfo( long &width, long &height, double &fps, __int64 &durationMs )
{
	width = FrameW; height = FrameH; fps = Fps; durationMs = Duration;
	return FrameW > 0 && FrameH > 0;
}
//---------------------------------------------------------------------------
bool tTVPMpeg1Video::DecoderReadFrame( BYTE *dst, long pitch, __int64 &outPtsMs, bool &eos )
{
	eos = false;
	if( Plm == nullptr ) { eos = true; return false; }

	plm_frame_t *frame = plm_decode_video( (plm_t*)Plm );
	if( frame == nullptr ) { eos = true; return false; } // 終端

	outPtsMs = (__int64)( frame->time * 1000.0 );

	// BGRA へ変換して dst へ書き込み。dst は最終行 + pitch 負 (ボトムアップ) なので
	// pl_mpeg の per-row (dest + y*stride) がそのままボトムアップ格納になる。
	plm_frame_to_bgra( frame, dst, (int)pitch );

	// pl_mpeg は B/G/R のみ書き alpha (4バイト目) を書かない。レイヤはアルファ有効の
	// ため未書き込みだと透明になってしまうので、不透明 (0xFF) で埋める。
	for( long y = 0; y < FrameH; y++ )
	{
		BYTE *row = dst + (ptrdiff_t)y * pitch;
		for( long x = 0; x < FrameW; x++ )
			row[x * 4 + 3] = 0xFF;
	}
	return true;
}
//---------------------------------------------------------------------------
bool tTVPMpeg1Video::DecoderSeek( __int64 ms )
{
	if( Plm == nullptr ) return false;
	// seek_exact=TRUE で正確な位置へ (キーフレーム間を復号)
	plm_frame_t *f = plm_seek_frame( (plm_t*)Plm, ms / 1000.0, TRUE );
	return f != nullptr;
}
//---------------------------------------------------------------------------
void tTVPMpeg1Video::DecoderPumpAudio()
{
	if( Plm == nullptr || Audio == nullptr ) return;
	// シンクのバッファが十分溜まるまで音声フレームをデコードして供給する。
	// (溜め過ぎない = 遅延と seek 追従のため上限を設ける)
	while( Audio->QueuedBuffers() < 8 )
	{
		plm_samples_t *s = plm_decode_audio( (plm_t*)Plm );
		if( s == nullptr ) break; // これ以上の音声フレーム無し (今は)
		// s->interleaved: count サンプル × 2ch の float
		Audio->Submit( s->interleaved, (size_t)s->count * 2 * sizeof(float) );
	}
}
//---------------------------------------------------------------------------
// overlay: デコードして frame を保持 (present は DecoderPresentOverlay で)
bool tTVPMpeg1Video::DecoderDecodeOverlay( __int64 &pts, bool &eos )
{
	eos = false;
	LastFrame = nullptr;
	if( Plm == nullptr ) { eos = true; return false; }
	plm_frame_t *frame = plm_decode_video( (plm_t*)Plm );
	if( frame == nullptr ) { eos = true; return false; }
	pts = (__int64)( frame->time * 1000.0 );
	LastFrame = frame; // 次の plm_decode まで有効
	return true;
}
//---------------------------------------------------------------------------
void tTVPMpeg1Video::DecoderPresentOverlay( tTVPD3D11OverlayWindow *ov )
{
	if( !ov || LastFrame == nullptr ) return;
	plm_frame_t *f = (plm_frame_t*)LastFrame;
	// pl_mpeg は I420 (planar YUV420)。plane.data / plane.width (=stride)。
	ov->PresentI420(
		f->y.data,  (int)f->y.width,
		f->cb.data, (int)f->cb.width,
		f->cr.data, (int)f->cr.width,
		(int)f->width, (int)f->height );
}
//---------------------------------------------------------------------------
// presenter 経路: 直近フレームの I420 プレーンを返す (基底が同期 copy)。pl_mpeg は I420。
bool tTVPMpeg1Video::DecoderGetI420Planes( const BYTE **y, int *yStride, const BYTE **u, int *uStride,
	const BYTE **v, int *vStride, int *w, int *h )
{
	if( LastFrame == nullptr ) return false;
	plm_frame_t *f = (plm_frame_t*)LastFrame;
	if( y ) *y = f->y.data;   if( yStride ) *yStride = (int)f->y.width;
	if( u ) *u = f->cb.data;  if( uStride ) *uStride = (int)f->cb.width;
	if( v ) *v = f->cr.data;  if( vStride ) *vStride = (int)f->cr.width;
	if( w ) *w = (int)f->width;
	if( h ) *h = (int)f->height;
	return true;
}
//---------------------------------------------------------------------------
