/**
 * 組み込み opus sound decocer
 */
#include "tjsCommHead.h"
#include "DebugIntf.h"
#include "SysInitIntf.h"
#include "StorageIntf.h"
#include "WaveIntf.h"

extern "C" {
#include <vorbis/vorbisfile.h>
}

#include <cmath>
#include <cstdlib> // std::atof (ReplayGain タグ)
#include <memory>

static bool FloatExtraction = false; // true if output format is IEEE 32-bit float
static double gVorbisGlobalGainDb = 0.0; // -ogg_gain / -vorbis_gain (全体ゲイン, dB)
static int    gVorbisReplayGainMode = 0; // 0=none(既定) / 1=track / 2=album
static bool TVPVorbisOptionsInit = false;
static void TVPInitVorbisOptions() {
	if(TVPVorbisOptionsInit) return;

	// retrieve options from commandline
	tTJSVariant val;
	// 全体ゲイン: -ogg_gain (現行) / -vorbis_gain (旧 wuvorbis 互換)。
	// libvorbis を改変せず、Render 時に PCM を float 域でスケールして適用する。
	if( TVPGetCommandLine(TJS_W("-ogg_gain"), &val) || TVPGetCommandLine(TJS_W("-vorbis_gain"), &val) ) {
		gVorbisGlobalGainDb = (tTVReal)val;
		double fac = std::pow(10.0, gVorbisGlobalGainDb / 20);
		ttstr debug_str = TJS_W("ogg: Setting global gain to ");
		tTJSVariant tmp((tTVReal)gVorbisGlobalGainDb);
		debug_str += ttstr(tmp);
		debug_str += TJS_W("dB (");
		tmp = (tTVReal)(fac * 100);
		debug_str += ttstr(tmp);
		debug_str += TJS_W("%)");
		TVPAddLog(debug_str);
	}

	// ReplayGain: -ogg_rg / -vorbis_rg (none(既定) / track / album)。
	// 既存再生を変えないよう既定 OFF (opt-in)。
	if( TVPGetCommandLine(TJS_W("-ogg_rg"), &val) || TVPGetCommandLine(TJS_W("-vorbis_rg"), &val) ) {
		ttstr sval(val);
		if( sval == TJS_W("track") ) gVorbisReplayGainMode = 1;
		else if( sval == TJS_W("album") ) gVorbisReplayGainMode = 2;
		else gVorbisReplayGainMode = 0;
		if( gVorbisReplayGainMode )
			TVPAddLog( TJS_W("ogg: ReplayGain enabled (") + sval + TJS_W(")") );
	}

/*
	if( TVPGetCommandLine(TJS_W("-ogg_pcm_format"), &val) ) {
		ttstr sval(val);
		if( sval == TJS_W("f32") ) {
			FloatExtraction = true;
			TVPAddLog(TJS_W("ogg: IEEE 32bit float output enabled."));
		}
	}
*/

	TVPVorbisOptionsInit = true;
}
//---------------------------------------------------------------------------
// tTVPWD_RIFFWave
//---------------------------------------------------------------------------
class tTVPWD_Vorbis : public tTVPWaveDecoder
{
	std::unique_ptr<iTJSBinaryStream> Stream;
	bool InputFileInit; // whether InputFile is inited
	OggVorbis_File InputFile; // OggVorbis_File instance
	tTVPWaveFormat Format; // output PCM format
	int CurrentSection; // current section in ogg stream
	float GainFactor = 1.0f; // 適用する線形ゲイン (1.0=無効時は int16 高速経路)

public:
	tTVPWD_Vorbis( std::unique_ptr<iTJSBinaryStream>&& stream ) : Stream(std::move(stream)), InputFileInit(false), CurrentSection(-1) {
		TVPInitVorbisOptions();
	}
	virtual ~tTVPWD_Vorbis() {
		if(InputFileInit)
		{
			ov_clear(&InputFile);
			InputFileInit = false;
		}
	};
	bool CheckFormat() {
		// open input stream via op_open_callbacks
		int err = 0;
		ov_callbacks callbacks = { read_func, seek_func, close_func, tell_func };
		if (ov_open_callbacks(this, &InputFile, NULL, 0, callbacks) < 0) {
			// error!
			return false;
		}
		InputFileInit = true;

		// retrieve PCM information
		vorbis_info *vi;
		vi = ov_info(&InputFile, -1);
		if(!vi)
		{
			return false;
		}

		// set Format up
		memset( &Format, 0, sizeof(Format) );
		Format.SamplesPerSec = vi->rate;
		Format.Channels = vi->channels;
		Format.BitsPerSample = FloatExtraction ? (0x10000 + 32) :  16;
		Format.BytesPerSample = Format.BitsPerSample / 8;
		Format.SpeakerConfig = 0;
		Format.IsFloat = FloatExtraction;
		Format.Seekable = true;

		ogg_int64_t pcmtotal = ov_pcm_total(&InputFile, -1); // PCM total samples
		if( pcmtotal < 0 ) pcmtotal = 0;
		Format.TotalSamples = pcmtotal;

		double timetotal = (double)pcmtotal / 48000.0;
		if( timetotal < 0 ) {
			Format.TotalTime = 0;
		} else {
			Format.TotalTime = (tjs_uint64)( timetotal * 1000.0 );
		}

		return true;
	}

	// 全体ゲイン + ReplayGain タグ + 曲別コールバック を合算し線形ゲインを決める。
	// CheckFormat 後 (InputFile オープン済み) に Create からメインスレッドで呼ぶ。
	void SetupGain(const ttstr & url) {
		if(!InputFileInit) return;
		double db = gVorbisGlobalGainDb;
		if(gVorbisReplayGainMode != 0) {
			vorbis_comment *vc = ov_comment(&InputFile, -1);
			if(vc) {
				const char *track = vorbis_comment_query(vc, "replaygain_track_gain", 0);
				const char *album = vorbis_comment_query(vc, "replaygain_album_gain", 0);
				const char *sel = (gVorbisReplayGainMode == 2) ? (album ? album : track) : track;
				if(sel) db += std::atof(sel); // dB
			}
		}
		db += TVPQueryUserSoundGainDB(url); // 曲別コールバック (未登録は 0)
		GainFactor = (db != 0.0) ? (float)std::pow(10.0, db / 20.0) : 1.0f;
	}

	/** Retrieve PCM format, etc. */
	void GetFormat(tTVPWaveFormat & format) override { format = Format; }

	/**
		Render PCM from current position.
		where "buf" is a destination buffer, "bufsamplelen" is the buffer's
		length in sample granule, "rendered" is to be an actual number of
		written sample granule.
		returns whether the decoding is to be continued.
		because "redered" can be lesser than "bufsamplelen", the player
		should not end until the returned value becomes false.
	*/
	bool Render(void *buf, tjs_uint bufsamplelen, tjs_uint& rendered)  override {
		// render output PCM
		if(!InputFileInit) return false; // InputFile is yet not inited

		// --- ゲイン適用経路: float でデコード→線形スケール→int16 変換 (clamp) ---
		// GainFactor==1 (既定) の通常時はこの分岐を通らないので追加コスト無し。
		// float 域で乗算してから量子化するため gain>1 でも適切に clip する。
		if( !FloatExtraction && GainFactor != 1.0f ) {
			const int ch = Format.Channels;
			const float g = GainFactor;
			tjs_uint done = 0;            // 書き込んだサンプル (per channel)
			tjs_int16 *out = (tjs_int16*)buf;
			while( done < bufsamplelen ) {
				float **pcm = nullptr;
				long ns = ov_read_float(&InputFile, &pcm, (int)(bufsamplelen - done), &CurrentSection);
				if( ns < 0 ) continue;   // デコード未準備。リトライ
				if( ns == 0 ) break;     // 終端
				for(long i = 0; i < ns; i++) {
					for(int c = 0; c < ch; c++) {
						float v = pcm[c][i] * g;
						int s = (int)std::lround(v * 32767.0f);
						if(s > 32767) s = 32767; else if(s < -32768) s = -32768;
						*out++ = (tjs_int16)s;
					}
				}
				done += (tjs_uint)ns;
			}
			rendered = done;
			return done >= bufsamplelen;
		}

		int pcmsize = FloatExtraction ? 4 : 2;
		int res;
		int pos = 0; // decoded PCM (in bytes)
		const int ch = Format.Channels;
		int remain = bufsamplelen * ch * pcmsize;

		if( FloatExtraction ) {
			/*
			while( remain ) {
				do {
					res = ov_read_float(&InputFile, (float*)((char*)buf + pos), remain, &CurrentSection );
				} while( res < 0 );
				if( res == 0 ) break;
				pos += res * ch * pcmsize;
				remain -= res * ch;
			}
			*/
		} else {
			while( remain ) {
				do {
					res = ov_read(&InputFile, (char*)buf + pos, remain, 0, pcmsize, 1, &CurrentSection );
				} while( res < 0 ); // ov_read would return a negative number
								// if the decoding is not ready
				if( res == 0 ) break;
				pos += res;
				remain -= res;
			}
		}

		pos /= (ch * pcmsize); // convert to PCM position
		rendered = pos; // return renderd PCM samples
		if((unsigned int)pos < bufsamplelen)
			return false;	// end of stream

		return true;
	}

	/*
		Seek to "samplepos". "samplepos" must be given in unit of sample granule.
		returns whether the seeking is succeeded.
	*/
	bool SetPosition(tjs_uint64 samplepos)  override {
		// set PCM position (seek)
		if(!InputFileInit) return false;

		if( 0 != ov_pcm_seek(&InputFile, samplepos) ) {
			return false;
		}
		return true;
	}

private:
	size_t static read_func(void *ptr, size_t size, size_t nmemb, void *stream) {
		// read function (wrapper for IStream)
		size_t nbytes = size * nmemb;
		tTVPWD_Vorbis * decoder = (tTVPWD_Vorbis*)stream;
		if( !decoder->Stream ) return 0;
		int bytesread = static_cast<int>(decoder->Stream->Read(ptr, static_cast<tjs_uint>(nbytes)));
		if( bytesread >= 0 ) return bytesread;
		return -1; // failed
	}
	int static seek_func(void *stream, ogg_int64_t offset, int whence) {
		// seek function (wrapper for IStream)
		tTVPWD_Vorbis * decoder = (tTVPWD_Vorbis*)stream;
		if( !decoder->Stream ) return -1;

		int seek_type = TJS_BS_SEEK_SET;
		switch(whence)
		{
		case SEEK_SET:
			seek_type = TJS_BS_SEEK_SET;
			break;
		case SEEK_CUR:
			seek_type = TJS_BS_SEEK_CUR;
			break;
		case SEEK_END:
			seek_type = TJS_BS_SEEK_END;
			break;
		}
		tjs_uint64 curpos = decoder->Stream->GetPosition();
		tjs_uint64 newpos = decoder->Stream->Seek(static_cast<tjs_int64>(offset), seek_type);
		return curpos != newpos ? 0 : 1;
	}
	int static close_func(void *stream) {
		tTVPWD_Vorbis * decoder = (tTVPWD_Vorbis*)stream;
		if( !decoder->Stream ) return EOF;
		decoder->Stream.reset();
		return 0;
	}
	long static tell_func(void *stream) {
		tTVPWD_Vorbis * decoder = (tTVPWD_Vorbis*)stream;
		if( !decoder->Stream ) return EOF;
		return static_cast<long>(decoder->Stream->GetPosition());
	}
};

//---------------------------------------------------------------------------
// Vorbis Decoder creator
//---------------------------------------------------------------------------
class tTVPWDC_Vorbis : public tTVPWaveDecoderCreator
{
public:
	tTVPWaveDecoder * Create(const ttstr & storagename, const ttstr & extension);
};
//---------------------------------------------------------------------------
tTVPWaveDecoder * tTVPWDC_Vorbis::Create(const ttstr & storagename, const ttstr &extension)
{
	if(extension != TJS_W(".ogg")) return nullptr;

	// ストリームオープンの失敗 (ファイルが無い等) は「このデコーダで扱えない」
	// ではないので握りつぶさず伝播させる。catch で畳むのはフォーマット判定
	// まわりの失敗のみ
	std::unique_ptr<iTJSBinaryStream> stream( TVPCreateStream(storagename) );
	if( !stream ) return nullptr;

	try {
		std::unique_ptr<tTVPWD_Vorbis> decoder( new tTVPWD_Vorbis( std::move(stream) ) );
		if( decoder->CheckFormat() == false ) {
			return nullptr;
		}
		decoder->SetupGain(storagename); // 全体/ReplayGain/曲別コールバックのゲイン決定
		return decoder.release();
	} catch(...) {
		return nullptr;
	}
}
//---------------------------------------------------------------------------
tTVPWDC_Vorbis VorbisDecoderCreator;
//---------------------------------------------------------------------------
void TVPRegisterVorbisDecoderCreator()
{
	TVPRegisterWaveDecoderCreator(&VorbisDecoderCreator);
}
//---------------------------------------------------------------------------

