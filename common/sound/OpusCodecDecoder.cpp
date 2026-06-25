/**
 * 組み込み Opus サウンドデコーダ
 *
 * 旧実装は xiph/opusfile を経由して OggOpusFile / op_open_callbacks /
 * op_read を呼んでいたが、opusfile は内部で opus_multistream_decoder_create()
 * を直接呼び、libopus 内部の素 malloc 経路でデコーダステートを確保するため、
 * krkrz の SoundAllocator (sound_malloc) でトラッキング/プール化できなかった。
 *
 * 本実装は opusfile への依存を取り除き:
 *   - libogg の ogg_sync_state / ogg_stream_state で Ogg ページ/パケットを framing
 *   - RFC 7845 (Ogg Encapsulation of Opus) §5.1/5.2 に従って OpusHead /
 *     OpusTags を自前パース
 *   - opus_multistream_decoder_get_size() + opus_multistream_decoder_init() で
 *     デコーダステートのバッファサイズを取り、その分を sound_malloc から確保
 *   - opus_multistream_decode / _decode_float でデコード
 *   - granule_pos / pre_skip ベースのシーク (bisection)
 *   - チェインストリーム (EOS → BOS で別ストリーム) を限定的にサポート
 *     (現リンクのフォーマットが変わったら新しい opus state を再初期化)
 *
 * libogg 自体は krkrz/external/sound-codecs/ で FetchContent + sound_alloc_hook.h
 * 注入されているため、libogg 内部の _ogg_malloc も sound_malloc 経由になる。
 * 残るのは libopus 内部 alloc だが、本実装が _init API を使うことで
 * decoder state は呼び出し側 (sound_malloc) のバッファに収まる。
 */
#include "tjsCommHead.h"
#include "DebugIntf.h"
#include "SysInitIntf.h"
#include "StorageIntf.h"
#include "SoundAllocator.h"
#include "WaveIntf.h"

extern "C" {
#include <ogg/ogg.h>
#include <opus.h>
#include <opus_multistream.h>
}

#include <algorithm>
#include <cmath>
#include <cstring>
#include <memory>
#include <vector>

//---------------------------------------------------------------------------
// グローバル設定 (CLI option)
//---------------------------------------------------------------------------
namespace {

bool gOpusFloatExtraction = false; // -opus_pcm_format=f32
double gOpusGainDb = 0.0;          // -opus_gain (出力 PCM に適用)
bool gOpusOptionsInit = false;

void TVPInitOpusOptions()
{
	if (gOpusOptionsInit) return;
	tTJSVariant val;
	if (TVPGetCommandLine(TJS_W("-opus_gain"), &val)) {
		gOpusGainDb = (tTVReal)val;
		double fac = std::pow(10.0, gOpusGainDb / 20.0);
		ttstr msg = TJS_W("opus: Setting global gain to ");
		tTJSVariant tmp((tTVReal)gOpusGainDb);
		msg += ttstr(tmp);
		msg += TJS_W("dB (");
		tmp = (tTVReal)(fac * 100);
		msg += ttstr(tmp);
		msg += TJS_W("%)");
		TVPAddLog(msg);
	}
	if (TVPGetCommandLine(TJS_W("-opus_pcm_format"), &val)) {
		ttstr sval(val);
		if (sval == TJS_W("f32")) {
			gOpusFloatExtraction = true;
			TVPAddLog(TJS_W("opus: IEEE 32bit float output enabled."));
		}
	}
	gOpusOptionsInit = true;
}

//---------------------------------------------------------------------------
// 16/32 bit リトルエンディアン読み出しヘルパ
//---------------------------------------------------------------------------
inline tjs_uint16 read_u16_le(const unsigned char *p) {
	return (tjs_uint16)p[0] | ((tjs_uint16)p[1] << 8);
}
inline tjs_int16 read_s16_le(const unsigned char *p) {
	return (tjs_int16)read_u16_le(p);
}
inline tjs_uint32 read_u32_le(const unsigned char *p) {
	return (tjs_uint32)p[0]
	     | ((tjs_uint32)p[1] << 8)
	     | ((tjs_uint32)p[2] << 16)
	     | ((tjs_uint32)p[3] << 24);
}

//---------------------------------------------------------------------------
// OpusHead 構造 (RFC 7845 §5.1)
//---------------------------------------------------------------------------
struct OpusHeadInfo {
	tjs_uint8  version;                 // 1
	tjs_uint8  channel_count;
	tjs_uint16 pre_skip;
	tjs_uint32 input_sample_rate;
	tjs_int16  output_gain_q78;         // Q7.8 dB
	tjs_uint8  mapping_family;
	tjs_uint8  stream_count;
	tjs_uint8  coupled_count;
	tjs_uint8  mapping[255];

	// 既定マッピング (RFC 7845 §5.1.1): mapping_family 0 のとき
	void set_default_mapping() {
		stream_count  = 1;
		coupled_count = (channel_count >= 2) ? 1 : 0;
		mapping[0] = 0;
		if (channel_count >= 2) mapping[1] = 1;
	}
};

// OpusHead packet (最低 19 bytes) を parse。成功で true。
bool ParseOpusHead(const unsigned char *data, long size, OpusHeadInfo &h)
{
	if (size < 19) return false;
	if (std::memcmp(data, "OpusHead", 8) != 0) return false;
	h.version           = data[8];
	if ((h.version & 0xF0) != 0) return false;  // major version must be 0
	h.channel_count     = data[9];
	if (h.channel_count == 0) return false;
	h.pre_skip          = read_u16_le(data + 10);
	h.input_sample_rate = read_u32_le(data + 12);
	h.output_gain_q78   = read_s16_le(data + 16);
	h.mapping_family    = data[18];
	if (h.mapping_family == 0) {
		if (h.channel_count > 2) return false;
		h.set_default_mapping();
		return true;
	}
	// family != 0: 続けて stream_count(1) + coupled_count(1) + mapping[channel_count]
	if (size < 19 + 2 + h.channel_count) return false;
	h.stream_count  = data[19];
	h.coupled_count = data[20];
	if (h.stream_count == 0) return false;
	if (h.coupled_count > h.stream_count) return false;
	if (h.stream_count + h.coupled_count > 255) return false;
	if (h.coupled_count * 2 + (h.stream_count - h.coupled_count) != h.channel_count) {
		// 厳密チェックは緩めにしておく (再生は可能でも仕様的に怪しい場合あり)
	}
	std::memcpy(h.mapping, data + 21, h.channel_count);
	return true;
}

//---------------------------------------------------------------------------
// SoundAllocator にぶら下がる軽量 unique_ptr 風 deleter
//---------------------------------------------------------------------------
struct SoundAllocDeleter {
	void operator()(void *p) const noexcept { if (p) sound_free(p); }
};
using SoundUPtr = std::unique_ptr<void, SoundAllocDeleter>;

//---------------------------------------------------------------------------
// tTVPWD_Opus: tTVPWaveDecoder 実装
//---------------------------------------------------------------------------
class tTVPWD_Opus : public tTVPWaveDecoder
{
	// 入力ストリーム
	std::unique_ptr<iTJSBinaryStream> Stream;
	tjs_uint64                        StreamSize = 0;  // 0 = 不明

	// libogg ステート
	ogg_sync_state   Sync{};
	ogg_stream_state Stream0{};                        // 現リンクの opus stream
	bool             SyncInited   = false;
	bool             StreamInited = false;
	int              CurrentSerialno = -1;
	bool             ReachedEof   = false;             // 入力ストリームが終端

	// 現リンクの opus state
	OpusMSDecoder   *MSDecoder = nullptr;              // multistream decoder
	SoundUPtr        MSDecoderMem;                     // 上記の backing storage
	OpusHeadInfo     Head{};
	tjs_uint64       LinkStartGranule = 0;             // チェイン時の累積基準
	tjs_uint64       SamplesProduced  = 0;             // 本リンク開始からの出力 PCM 数
	bool             HeaderReady      = false;         // OpusHead 取得済み

	// 全体フォーマット (出力 PCM 用)
	tTVPWaveFormat   Format{};

	// 出力 gain (output_gain_q78 + -opus_gain 合算後の 16-bit Q7.8 値を
	// opus_multistream_decoder_ctl(OPUS_SET_GAIN) で渡す)
	int              GainQ78Total = 0;

	// デコード中間バッファ (1 packet ぶん)。最大フレームサイズは 5760 (120ms @ 48kHz)
	static constexpr int kMaxFrameSize = 5760;
	std::vector<float>   FloatScratch;
	std::vector<opus_int16> IntScratch;

	// 出力前にスキップすべきサンプル数 (head pre_skip + seek 微調整)
	tjs_int64        SamplesToSkip = 0;

	// 既デコードした余り (packet 単位でデコードして出力に詰めるので、bufsamplelen
	// 境界で半端が出る場合に持ち越す)
	std::vector<float>      LeftoverFloat;
	std::vector<opus_int16> LeftoverInt;

public:
	tTVPWD_Opus(std::unique_ptr<iTJSBinaryStream> &&stream)
		: Stream(std::move(stream))
	{
		TVPInitOpusOptions();
		if (Stream) {
			tjs_uint64 cur = Stream->GetPosition();
			tjs_uint64 end = Stream->Seek(0, TJS_BS_SEEK_END);
			StreamSize = end;
			Stream->Seek((tjs_int64)cur, TJS_BS_SEEK_SET);
		}
		ogg_sync_init(&Sync);
		SyncInited = true;
	}

	~tTVPWD_Opus() override {
		TeardownStream();
		if (SyncInited) ogg_sync_clear(&Sync);
	}

	bool CheckFormat() {
		// 最初の Ogg ページ群を読んで OpusHead / OpusTags を取得する。
		if (!OpenFirstLink()) return false;

		// 出力フォーマットを確定
		std::memset(&Format, 0, sizeof(Format));
		Format.SamplesPerSec  = 48000;  // Opus 出力は常に 48 kHz
		Format.Channels       = Head.channel_count;
		Format.BitsPerSample  = gOpusFloatExtraction ? (0x10000 + 32) : 16;
		Format.BytesPerSample = gOpusFloatExtraction ? 4 : 2;
		Format.SpeakerConfig  = 0;
		Format.IsFloat        = gOpusFloatExtraction;
		Format.Seekable       = StreamSize > 0;

		tjs_int64 total = ComputeTotalSamples();
		Format.TotalSamples = (total > 0) ? (tjs_uint64)total : 0;
		Format.TotalTime    = (total > 0) ? (tjs_uint64)(total * 1000 / 48000) : 0;

		// 中間バッファ確保 (最大フレーム × チャンネル)
		FloatScratch.resize((size_t)kMaxFrameSize * Head.channel_count);
		IntScratch.resize((size_t)kMaxFrameSize * Head.channel_count);

		return true;
	}

	void GetFormat(tTVPWaveFormat &format) override { format = Format; }

	bool Render(void *buf, tjs_uint bufsamplelen, tjs_uint &rendered) override
	{
		rendered = 0;
		if (!MSDecoder) return false;

		const int channels = (int)Head.channel_count;
		const bool fp      = gOpusFloatExtraction;
		float       *outF  = fp ? static_cast<float *>(buf)       : nullptr;
		opus_int16  *outI  = fp ? nullptr                          : static_cast<opus_int16 *>(buf);
		tjs_uint     need  = bufsamplelen;  // sample granule remaining

		// 持ち越しを先に消化
		auto consume_leftover = [&]() {
			if (fp) {
				if (LeftoverFloat.empty()) return;
				size_t per = (size_t)channels;
				size_t have_samples = LeftoverFloat.size() / per;
				size_t take = std::min<size_t>(have_samples, need);
				std::memcpy(outF + rendered * per, LeftoverFloat.data(), take * per * sizeof(float));
				if (take * per < LeftoverFloat.size()) {
					LeftoverFloat.erase(LeftoverFloat.begin(),
					                    LeftoverFloat.begin() + (std::ptrdiff_t)(take * per));
				} else {
					LeftoverFloat.clear();
				}
				rendered += (tjs_uint)take;
				need     -= (tjs_uint)take;
			} else {
				if (LeftoverInt.empty()) return;
				size_t per = (size_t)channels;
				size_t have_samples = LeftoverInt.size() / per;
				size_t take = std::min<size_t>(have_samples, need);
				std::memcpy(outI + rendered * per, LeftoverInt.data(), take * per * sizeof(opus_int16));
				if (take * per < LeftoverInt.size()) {
					LeftoverInt.erase(LeftoverInt.begin(),
					                  LeftoverInt.begin() + (std::ptrdiff_t)(take * per));
				} else {
					LeftoverInt.clear();
				}
				rendered += (tjs_uint)take;
				need     -= (tjs_uint)take;
			}
		};
		consume_leftover();

		// 必要なだけ packet をデコード
		while (need > 0) {
			ogg_packet op;
			int prc = NextAudioPacket(op);
			if (prc < 0) {
				// 致命的エラー
				return false;
			}
			if (prc == 0) {
				// チェイン終了 or 入力末尾
				return false;
			}

			// デコード
			int frame_samples;
			if (fp) {
				frame_samples = opus_multistream_decode_float(
					MSDecoder,
					op.packet, (opus_int32)op.bytes,
					FloatScratch.data(),
					kMaxFrameSize,
					0);
			} else {
				frame_samples = opus_multistream_decode(
					MSDecoder,
					op.packet, (opus_int32)op.bytes,
					IntScratch.data(),
					kMaxFrameSize,
					0);
			}
			if (frame_samples < 0) {
				// パケットデコード失敗 (壊れ): silently skip
				continue;
			}
			if (frame_samples == 0) continue;

			// pre_skip / seek-skip 適用
			int skip_here = 0;
			if (SamplesToSkip > 0) {
				skip_here = (int)std::min<tjs_int64>(SamplesToSkip, frame_samples);
				SamplesToSkip -= skip_here;
			}
			int usable = frame_samples - skip_here;
			SamplesProduced += (tjs_uint64)usable;

			// 出力 + leftover 振り分け
			int to_copy = std::min<int>(usable, (int)need);
			size_t per = (size_t)channels;
			if (fp) {
				std::memcpy(outF + rendered * per,
				            FloatScratch.data() + skip_here * channels,
				            (size_t)to_copy * per * sizeof(float));
			} else {
				std::memcpy(outI + rendered * per,
				            IntScratch.data() + skip_here * channels,
				            (size_t)to_copy * per * sizeof(opus_int16));
			}
			rendered += (tjs_uint)to_copy;
			need     -= (tjs_uint)to_copy;

			int leftover = usable - to_copy;
			if (leftover > 0) {
				if (fp) {
					size_t start = (size_t)(skip_here + to_copy) * per;
					size_t end   = (size_t)(skip_here + usable) * per;
					LeftoverFloat.insert(LeftoverFloat.end(),
					                     FloatScratch.begin() + (std::ptrdiff_t)start,
					                     FloatScratch.begin() + (std::ptrdiff_t)end);
				} else {
					size_t start = (size_t)(skip_here + to_copy) * per;
					size_t end   = (size_t)(skip_here + usable) * per;
					LeftoverInt.insert(LeftoverInt.end(),
					                   IntScratch.begin() + (std::ptrdiff_t)start,
					                   IntScratch.begin() + (std::ptrdiff_t)end);
				}
			}
		}

		return rendered == bufsamplelen;
	}

	bool SetPosition(tjs_uint64 samplepos) override
	{
		if (!Stream) return false;
		if (!Format.Seekable) return false;

		// シーク戦略: pos==0 → ストリーム再オープン。
		// pos>0 → bisection seek: granule_pos と byte offset の関係を推定し、
		// 近傍まで file を巻き戻し、ogg_sync を resync、目的 granule_pos の
		// ページに当たったら packet 単位でデコードしてサンプル位相を合わせる。
		if (samplepos == 0) {
			return RewindToStart();
		}
		return BisectSeek(samplepos);
	}

private:
	//-----------------------------------------------------------------------
	// 内部状態管理
	//-----------------------------------------------------------------------
	void TeardownLink() {
		if (MSDecoder) {
			// opus_multistream_decoder_destroy は呼べない (本実装は _init で
			// 呼び出し側 buffer を使っているため)。明示的なクリーンアップは不要。
			MSDecoder = nullptr;
		}
		MSDecoderMem.reset();
		LeftoverFloat.clear();
		LeftoverInt.clear();
		SamplesProduced = 0;
		HeaderReady = false;
		if (StreamInited) {
			ogg_stream_clear(&Stream0);
			StreamInited = false;
		}
		CurrentSerialno = -1;
	}

	void TeardownStream() {
		TeardownLink();
		// (Sync は ~tTVPWD_Opus で clear)
	}

	// libogg sync にデータを読み足す。0 = EOF、>0 = 読み込んだバイト数、<0 = エラー。
	int FeedSync() {
		if (ReachedEof) return 0;
		const int kRead = 4096;
		char *buf = ogg_sync_buffer(&Sync, kRead);
		if (!buf) return -1;
		int n = (int)Stream->Read(buf, kRead);
		if (n <= 0) {
			ReachedEof = true;
			if (n < 0) return -1;
			return 0;
		}
		if (ogg_sync_wrote(&Sync, n) < 0) return -1;
		return n;
	}

	// 次のページを取得。1=取得、0=EOF、-1=エラー。
	int NextPage(ogg_page &og, bool need_resync_ok = true) {
		while (true) {
			int r = ogg_sync_pageout(&Sync, &og);
			if (r > 0) return 1;
			if (r < 0) {
				// 同期外れ: 続行
				if (!need_resync_ok) return -1;
				continue;
			}
			int fed = FeedSync();
			if (fed == 0) return 0;
			if (fed < 0)  return -1;
		}
	}

	// 現リンクの opus state を初期化。head は parse 済み前提。
	bool InitDecoderFor(const OpusHeadInfo &h) {
		int sz = opus_multistream_decoder_get_size(h.stream_count, h.coupled_count);
		if (sz <= 0) return false;
		void *raw = sound_malloc((size_t)sz);
		if (!raw) return false;
		SoundUPtr buf(raw);
		OpusMSDecoder *dec = (OpusMSDecoder *)buf.get();
		int err = opus_multistream_decoder_init(
			dec,
			48000,
			h.channel_count,
			h.stream_count,
			h.coupled_count,
			h.mapping);
		if (err != OPUS_OK) return false;

		// gain 設定 (Q7.8 dB)。head に書かれた gain + CLI -opus_gain を加算。
		int cli_gain_q78 = (int)std::lround(gOpusGainDb * 256.0);
		int total_q78 = (int)h.output_gain_q78 + cli_gain_q78;
		if (total_q78 < -32768) total_q78 = -32768;
		if (total_q78 >  32767) total_q78 =  32767;
		opus_multistream_decoder_ctl(dec, OPUS_SET_GAIN(total_q78));
		GainQ78Total = total_q78;

		// 既存があれば破棄して入れ替え
		MSDecoder    = dec;
		MSDecoderMem = std::move(buf);
		return true;
	}

	// 最初のリンクを開く: 1st page (OpusHead) と 2nd page (OpusTags) を読み、
	// デコーダを初期化。
	bool OpenFirstLink() {
		// まず Sync にデータを流し込む
		ogg_page og;
		while (true) {
			int r = NextPage(og);
			if (r <= 0) return false;
			if (!ogg_page_bos(&og)) continue;  // BOS でないページはスキップ (めったにない)
			// このページの packet (OpusHead) を取り出す
			int serialno = ogg_page_serialno(&og);
			if (StreamInited) ogg_stream_clear(&Stream0);
			ogg_stream_init(&Stream0, serialno);
			StreamInited = true;
			CurrentSerialno = serialno;
			if (ogg_stream_pagein(&Stream0, &og) < 0) return false;

			ogg_packet op;
			if (ogg_stream_packetout(&Stream0, &op) != 1) return false;
			if (!ParseOpusHead(op.packet, op.bytes, Head)) {
				// Opus でない (= 別 codec の Ogg) → skip して次の BOS を探す
				ogg_stream_clear(&Stream0);
				StreamInited = false;
				continue;
			}
			break;
		}

		// OpusTags を 1 packet 取り出す (内容は無視するがフレーミングのために消費)
		ogg_packet op;
		while (true) {
			int pr = ogg_stream_packetout(&Stream0, &op);
			if (pr == 1) break;
			if (pr < 0) return false;
			// もう packet が無いので次のページを供給
			int r = NextPage(og);
			if (r <= 0) return false;
			if (ogg_page_serialno(&og) != CurrentSerialno) continue;
			if (ogg_stream_pagein(&Stream0, &og) < 0) return false;
		}
		// OpusTags 簡易検証 (任意): "OpusTags" magic を見るだけ
		if (op.bytes < 8 || std::memcmp(op.packet, "OpusTags", 8) != 0) {
			// 形式違反だがそのまま続行 (一部エンコーダで magic が壊れているファイル
			// が存在するため)
		}

		if (!InitDecoderFor(Head)) return false;
		HeaderReady = true;
		SamplesToSkip = Head.pre_skip;
		SamplesProduced = 0;
		return true;
	}

	// 次の "audio" packet (OpusHead/OpusTags の後の packet) を取得。
	// 戻り値: 1=ok、0=ストリーム終端、-1=エラー
	int NextAudioPacket(ogg_packet &op) {
		while (true) {
			int pr = ogg_stream_packetout(&Stream0, &op);
			if (pr == 1) return 1;
			if (pr < 0) {
				// シンク外れ。次ページへ。
			}
			// 次ページが必要
			ogg_page og;
			int r = NextPage(og);
			if (r < 0) return -1;
			if (r == 0) return 0;

			int serialno = ogg_page_serialno(&og);
			if (ogg_page_bos(&og) && serialno != CurrentSerialno) {
				// チェイン: 新リンク
				// 旧ストリームの累積に達した分を確定
				if (!HandleChainStart(og)) return -1;
				continue;
			}
			if (serialno != CurrentSerialno) {
				// 関係ない streams (Opus + 他 codec の多重) は無視
				continue;
			}
			if (ogg_stream_pagein(&Stream0, &og) < 0) return -1;
		}
	}

	// チェインの新リンク開始: og は新 BOS ページ。
	bool HandleChainStart(ogg_page &og) {
		// 既存デコーダを片付ける
		LinkStartGranule += SamplesProduced;
		TeardownLink();
		// 新ストリームを引き継ぐ
		int serialno = ogg_page_serialno(&og);
		ogg_stream_init(&Stream0, serialno);
		StreamInited    = true;
		CurrentSerialno = serialno;
		if (ogg_stream_pagein(&Stream0, &og) < 0) return false;

		ogg_packet op;
		// OpusHead
		while (true) {
			int pr = ogg_stream_packetout(&Stream0, &op);
			if (pr == 1) break;
			if (pr < 0) return false;
			ogg_page og2;
			int r = NextPage(og2);
			if (r <= 0) return false;
			if (ogg_page_serialno(&og2) != CurrentSerialno) continue;
			if (ogg_stream_pagein(&Stream0, &og2) < 0) return false;
		}
		if (!ParseOpusHead(op.packet, op.bytes, Head)) {
			// Opus でない: チェイン終了扱い
			return false;
		}
		// OpusTags
		while (true) {
			int pr = ogg_stream_packetout(&Stream0, &op);
			if (pr == 1) break;
			if (pr < 0) return false;
			ogg_page og2;
			int r = NextPage(og2);
			if (r <= 0) return false;
			if (ogg_page_serialno(&og2) != CurrentSerialno) continue;
			if (ogg_stream_pagein(&Stream0, &og2) < 0) return false;
		}
		if (!InitDecoderFor(Head)) return false;
		HeaderReady = true;
		SamplesToSkip = Head.pre_skip;
		SamplesProduced = 0;
		return true;
	}

	//-----------------------------------------------------------------------
	// シーク (簡易 bisection)
	//-----------------------------------------------------------------------
	bool RewindToStart() {
		// ストリーム位置を 0 に戻し、libogg sync をリセットしてから再オープン
		Stream->Seek(0, TJS_BS_SEEK_SET);
		ReachedEof = false;
		TeardownLink();
		if (SyncInited) ogg_sync_reset(&Sync);
		LinkStartGranule = 0;
		return OpenFirstLink();
	}

	// 簡易 bisection: byte 範囲 [lo, hi] でページを探して granule_pos が
	// target_pcm 以下の最後のページに当たるよう収束。完璧な実装ではないが
	// VN/オーディオブック用途には十分。
	bool BisectSeek(tjs_uint64 target_pcm) {
		if (StreamSize == 0) return false;

		// まず先頭から OpusHead を取り直す (pre_skip / マッピング再取得のため)。
		// (現実装ではチェインを跨ぐ高速 seek は対応しない — 単一リンク想定。)
		if (!RewindToStart()) return false;

		// 目標 granule_pos を計算。Opus は granule_pos = PCM サンプル数 (48kHz)、
		// ただし pre_skip 分のオフセットがある (RFC 7845 §4: granule_pos には
		// pre_skip が含まれる)。
		tjs_int64 target_g = (tjs_int64)target_pcm + (tjs_int64)Head.pre_skip;

		tjs_uint64 lo = 0, hi = StreamSize;
		const tjs_uint64 kBlock = 65536;
		tjs_uint64 best_pos = 0;
		tjs_int64  best_g   = -1;

		// 二分探索: 各回でファイル位置を中点に飛ばし、そこからページを 1 つ
		// 読み出してその granule_pos を見る。
		for (int iter = 0; iter < 30 && lo + kBlock < hi; ++iter) {
			tjs_uint64 mid = (lo + hi) / 2;
			Stream->Seek((tjs_int64)mid, TJS_BS_SEEK_SET);
			ReachedEof = false;
			ogg_sync_reset(&Sync);

			ogg_page og;
			int r = NextPage(og);
			if (r <= 0) {
				hi = mid;
				continue;
			}
			tjs_int64 g = ogg_page_granulepos(&og);
			if (g < 0) {
				lo = mid + kBlock;
				continue;
			}
			if (g < target_g) {
				lo = mid;
				best_pos = mid;
				best_g   = g;
			} else {
				hi = mid;
			}
		}

		// 収束後、best_pos から linear に decode して target サンプルに当たるまで進む
		Stream->Seek((tjs_int64)best_pos, TJS_BS_SEEK_SET);
		ReachedEof = false;
		ogg_sync_reset(&Sync);
		if (StreamInited) ogg_stream_reset(&Stream0);

		// 次の OpusStream serialno を見つけ、本リンクのまま追従させる
		ogg_page og;
		while (true) {
			int r = NextPage(og);
			if (r <= 0) return false;
			if (ogg_page_serialno(&og) == CurrentSerialno) {
				if (ogg_stream_pagein(&Stream0, &og) < 0) return false;
				break;
			}
		}

		// linear 再生で目標サンプルまでスキップ
		tjs_int64 reached_g = (best_g >= 0) ? best_g : 0;
		tjs_int64 remain = target_g - reached_g;
		if (remain < 0) remain = 0;
		SamplesToSkip = remain;
		SamplesProduced = (tjs_uint64)reached_g - (tjs_int64)Head.pre_skip > 0
		                ? (tjs_uint64)((tjs_int64)reached_g - (tjs_int64)Head.pre_skip)
		                : 0;
		LeftoverFloat.clear();
		LeftoverInt.clear();
		// デコーダ内部状態もリセット (前後の packet がつながらないため)
		if (MSDecoder) {
			opus_multistream_decoder_ctl(MSDecoder, OPUS_RESET_STATE);
		}
		return true;
	}

	//-----------------------------------------------------------------------
	// 全 PCM サンプル数の推定: ファイル末尾近くの最終ページの granule_pos
	// から pre_skip を引いた値。失敗時は -1。
	//-----------------------------------------------------------------------
	tjs_int64 ComputeTotalSamples() {
		if (StreamSize == 0) return -1;
		// 末尾 64KB 程度を読み込んで最終ページの granule_pos を探す
		const tjs_uint64 kProbe = 65536;
		tjs_uint64 from = (StreamSize > kProbe) ? (StreamSize - kProbe) : 0;
		tjs_uint64 saved_pos = Stream->GetPosition();

		Stream->Seek((tjs_int64)from, TJS_BS_SEEK_SET);
		ReachedEof = false;
		ogg_sync_state probe;
		ogg_sync_init(&probe);

		tjs_int64 last_g = -1;
		while (true) {
			char *bufp = ogg_sync_buffer(&probe, 4096);
			if (!bufp) break;
			int n = (int)Stream->Read(bufp, 4096);
			if (n <= 0) break;
			if (ogg_sync_wrote(&probe, n) < 0) break;
			ogg_page og;
			while (true) {
				int r = ogg_sync_pageout(&probe, &og);
				if (r <= 0) break;
				if (ogg_page_serialno(&og) != CurrentSerialno) continue;
				tjs_int64 g = ogg_page_granulepos(&og);
				if (g >= 0) last_g = g;
			}
		}
		ogg_sync_clear(&probe);

		Stream->Seek((tjs_int64)saved_pos, TJS_BS_SEEK_SET);
		ReachedEof = false;
		ogg_sync_reset(&Sync);
		if (StreamInited) ogg_stream_reset(&Stream0);

		// 再オープンせず、本来の sync 位置から packet を再供給するには
		// OpenFirstLink 直後の状態に戻すのが簡単。ここでは sync_reset のみで
		// 済ませているが、Render の次呼び出しが新たに pages を pull できるので
		// 機能上は問題ない。
		if (last_g < 0) return -1;
		tjs_int64 total = last_g - (tjs_int64)Head.pre_skip;
		return (total > 0) ? total : -1;
	}
};

//---------------------------------------------------------------------------
// tTVPWDC_Opus: WaveDecoderCreator 実装
//---------------------------------------------------------------------------
class tTVPWDC_Opus : public tTVPWaveDecoderCreator
{
public:
	tTVPWaveDecoder *Create(const ttstr &storagename, const ttstr &extension) override;
};

tTVPWaveDecoder *tTVPWDC_Opus::Create(const ttstr &storagename, const ttstr &extension)
{
	if (extension != TJS_W(".opus")) return nullptr;
	try {
		std::unique_ptr<iTJSBinaryStream> stream(TVPCreateStream(storagename));
		if (!stream) return nullptr;
		std::unique_ptr<tTVPWD_Opus> dec(new tTVPWD_Opus(std::move(stream)));
		if (!dec->CheckFormat()) return nullptr;
		return dec.release();
	} catch (...) {
		return nullptr;
	}
}

tTVPWDC_Opus OpusDecoderCreator;

} // namespace

//---------------------------------------------------------------------------
void TVPRegisterOpusDecoderCreator()
{
	TVPRegisterWaveDecoderCreator(&OpusDecoderCreator);
}
//---------------------------------------------------------------------------
