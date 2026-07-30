#include "tjsCommHead.h"
#include "MsgIntf.h"
#include "LogIntf.h"
#include "SysInitIntf.h"
#include "SoundAllocator.h"
#include "AudioStream.h"

#include <memory>
#include <assert.h>
#include <algorithm>
#include <atomic>
#include <cmath>

#define MINIAUDIO_IMPLEMENTATION

#define MA_NO_RESOURCE_MANAGER
#define MA_USE_STDINT
#include "miniaudio.h"

//---------------------------------------------------------------------------
// miniaudio allocation callbacks
// ma_engine 内部 (resampler / converter / device ring 等) の確保を全て
// SoundAllocator 経由に流す。これで pool ベース管理 + Sound tag 集計が効く。
//---------------------------------------------------------------------------
static void *MAOnMalloc(size_t sz, void * /*pUserData*/)
{
	return sound_malloc(sz);
}
static void *MAOnRealloc(void *p, size_t sz, void * /*pUserData*/)
{
	return sound_realloc(p, sz);
}
static void MAOnFree(void *p, void * /*pUserData*/)
{
	sound_free(p);
}
static const ma_allocation_callbacks &GetMiniAudioAllocationCallbacks()
{
	static ma_allocation_callbacks cb = {
		nullptr,    // pUserData
		MAOnMalloc, // onMalloc
		MAOnRealloc,// onRealloc
		MAOnFree    // onFree
	};
	return cb;
}

tjs_int TVPSoundFrequency = 48000;
// 0 = デバイスネイティブ ch (WASAPI エンドポイント構成) に追従。
// engine 生成後に実際の ch 数をここへキャッシュする。SDL 版は audio.cpp が
// SetMiniAudioSpec で SDL デバイスの ch を事前設定する。
tjs_int TVPSoundChannels = 0;
static const int VOLUME_MAX = 100000;

// WIN 版 DirectSound 経路 (win32/sound/WaveImpl.cpp::TVPVolumeToDSAttenuate)
// の知覚カーブを SDL/miniaudio 側で再現するための係数。WIN 版と同じ既定値
// 3322 で「スライダ 50% ≒ -10 dB ≒ 知覚的に半分」になる。
// -wsvolfactor オプションで上書き可能 (WIN 版と同じ名前/範囲)。
// 2000 を指定すると pow の指数が 1.0 になり実質 linear (旧 SDL 動作と等価)。
static tjs_int TVPVolumeLogFactor = 3322;

//---------------------------------------------------------------------------

TVPLogLevel MALogLevelToTVPLogLevel(ma_uint32 level)
{
	switch (level) {
	case MA_LOG_LEVEL_DEBUG:
		return TVPLOG_LEVEL_DEBUG;
	case MA_LOG_LEVEL_INFO:
		return TVPLOG_LEVEL_INFO;
	case MA_LOG_LEVEL_WARNING:
		return TVPLOG_LEVEL_WARNING;
	case MA_LOG_LEVEL_ERROR:
		return TVPLOG_LEVEL_ERROR;
	default:
		return TVPLOG_LEVEL_OFF;
	}
}

static void OnLog(void *pUserData, ma_uint32 level, const char *pMessage)
{
	TVPLogLevel logLevel = MALogLevelToTVPLogLevel(level);
	TVPLOG_IMPL(logLevel, "miniaudio: {}", pMessage);
}

static ma_engine *gEngine = NULL;

void InitMiniAudio()
{
	if (!gEngine) {
		// -wsvolfactor を解釈 (WIN 版 WaveImpl.cpp と同じ命名/範囲)。
		// volume の知覚カーブを linear → log に補正する係数で、既定 3322。
		{
			tTJSVariant val;
			if (TVPGetCommandLine(TJS_W("-wsvolfactor"), &val)) {
				tjs_int n = (tjs_int)val;
				if (n > 0 && n < 200000) TVPVolumeLogFactor = n;
			}
		}
		TVPLOG_INFO("Initializing miniaudio engine...");
    	gEngine = (ma_engine *)sound_malloc(sizeof(ma_engine));
    	if (gEngine) {
			ma_log_callback_init(OnLog, NULL);

			ma_engine_config engineConfig = ma_engine_config_init();
			// channels=0 のときはデバイスネイティブ ch (エンドポイント構成) を採用して
			// エンドポイント追従にする (5.1/7.1 をパススルーできる)。SDL 版は
			// SetMiniAudioSpec で SDL デバイスの ch が事前設定される。
			engineConfig.channels = TVPSoundChannels;
			engineConfig.sampleRate = TVPSoundFrequency; // サンプルレートは48000Hz
			// miniaudio 内部 alloc を SoundAllocator 経由に流す。
			engineConfig.allocationCallbacks = GetMiniAudioAllocationCallbacks();

			ma_result result = ma_engine_init(&engineConfig, gEngine);
			if (result != MA_SUCCESS) {
				const char *msg = ma_result_description(result);
				TVPLOG_ERROR("failed to initialize miniaudio engine: {}", msg);
			} else {
				// 実際に開かれた ch 数を反映 (以降 GetMiniAudioSpec 等が参照)
				TVPSoundChannels = ma_engine_get_channels(gEngine);
				TVPLOG_INFO("miniaudio engine initialized: {} channels, {} Hz",
					(int)ma_engine_get_channels(gEngine), (int)ma_engine_get_sample_rate(gEngine));
			}
	    }
	}
}

ma_engine *GetMiniAudioEngine()
{
	InitMiniAudio();
	return gEngine;
}

// -wsfreq 等のサウンドオプションを反映する (QueueSoundBuffer 生成時にも呼ばれる)。
extern void TVPInitSoundOptions();

// 起動シーケンスからオーディオデバイス (miniaudio engine) を先行初期化する。
// 遅延初期化のままだと初回サウンド再生時に WASAPI デバイスを開くため、
// その分の遅延で音の頭が欠けることがある。起動時にデバイスオープンを
// 前倒ししておくことでこれを防ぐ。-wspreinit=no/off/false/0 で従来の
// 遅延初期化に戻せる (broken driver 等で起動時オープンを避けたい場合の逃げ道)。
// SDL 版は InitAudioSystem() が起動時に InitMiniAudio() を呼ぶため不要。
void TVPPreInitAudioDevice()
{
	if (gEngine) return; // 既に初期化済み

	tTJSVariant val;
	if (TVPGetCommandLine(TJS_W("-wspreinit"), &val)) {
		ttstr s = val; s.ToLowerCase();
		if (s == TJS_W("no") || s == TJS_W("off") || s == TJS_W("false") || s == TJS_W("0"))
			return;
	}

	// 遅延初期化経路 (QueueSoundBuffer ctor → GetMiniAudioEngine) と同じ順序で
	// オプション反映 → engine 初期化を行う。
	TVPInitSoundOptions();
	InitMiniAudio();
}

static void DoneMiniAudio()
{
	if (gEngine) {
    	ma_engine_uninit(gEngine);
    	sound_free(gEngine);
    	gEngine = NULL;
  	}
}

static tTVPAtExit TVPUninitAudioDeviceAtExit
( TVP_ATEXIT_PRI_RELEASE, DoneMiniAudio );

void SetMiniAudioSpec(int channels, int sampleRate)
{
	if (gEngine) {
		TVPLOG_ERROR("miniaudio engine already initialized, cannot change spec");
	} else {
		TVPSoundChannels = channels;
		TVPSoundFrequency = sampleRate;
	}
}

void GetMiniAudioSpec(int &channels, int &sampleRate)
{
	if (gEngine) {
		channels = ma_engine_get_channels(gEngine);
		sampleRate = ma_engine_get_sample_rate(gEngine);
	} else {
		channels = TVPSoundChannels;
		sampleRate = TVPSoundFrequency;
	}
}

static ma_format typeConv(TVPAudioSampleType sampleType)
{
	switch (sampleType) {
	case astUInt8:
		return ma_format_u8;
	case astInt16:
		return ma_format_s16;
	case astInt24:
		return ma_format_s24;
	case astInt32:
		return ma_format_s32;
	case astFloat32:
		return ma_format_f32;
	default:
		return ma_format_unknown; // 未知のフォーマット
	}
}

/**
 * PCMフレームを読み出す
 * フォーマットは変更可能
 * チャンネルとレートは一致してる想定
 */
void ReadMiniAudioPcmFrames(void *buffer, int frameCount, TVPAudioSampleType type=astFloat32)
{
	if (gEngine) {
		ma_format output_format = typeConv(type);
		if (output_format == ma_format_f32) {
			// 形式が同じなのでそのまま出力
		    ma_engine_read_pcm_frames(gEngine, buffer, frameCount, NULL);        
		} else {
			// 処理用バッファ
			static std::vector<uint8_t> tempBuffer;

			int channels = ma_engine_get_channels(gEngine);
			int frameSize = ma_get_bytes_per_frame(ma_format_f32, channels);
			size_t size = frameCount * frameSize;
			if (tempBuffer.size() < size) {
				tempBuffer.resize(size);
			}
			void *tempBufferPtr = tempBuffer.data();
			ma_engine_read_pcm_frames(gEngine, tempBufferPtr, frameCount, NULL);
			// 変換処理
			ma_convert_pcm_frames_format(buffer, output_format, tempBufferPtr, ma_format_f32, frameCount, channels, ma_dither_mode_none);

		#if 0
			//TVPLOG_DEBUG("Converting PCM frames from f32 to {} bytes", ma_get_bytes_per_sample(output_format));
			// Directly convert from float32 to the requested output format
			float* src = static_cast<float*>(tempBufferPtr);
			ma_uint64 totalSamples = frameCount * channels;
			switch (output_format) {
				case ma_format_u8: {
					// Convert float32 [-1.0,1.0] to uint8 [0,255]
					uint8_t* dst = static_cast<uint8_t*>(buffer);
					for (ma_uint64 i = 0; i < totalSamples; i++) {
						float sample = src[i] * 0.5f + 0.5f; // Map [-1,1] to [0,1]
						sample = std::max(0.0f, std::min(1.0f, sample)); // Clamp to [0,1]
						dst[i] = static_cast<uint8_t>(sample * 255.0f);
					}
					break;
				}
				case ma_format_s16: {
					// Convert float32 [-1.0,1.0] to int16 [-32768,32767]
					int16_t* dst = static_cast<int16_t*>(buffer);
					for (ma_uint64 i = 0; i < totalSamples; i++) {
						float sample = src[i];
						sample = std::max(-1.0f, std::min(1.0f, sample)); // Clamp to [-1,1]
						dst[i] = static_cast<int16_t>(sample * 32767.0f);
					}
					break;
				}
				case ma_format_s24: {
					// Convert float32 [-1.0,1.0] to int24 [-8388608,8388607]
					uint8_t* dst = static_cast<uint8_t*>(buffer);
					for (ma_uint64 i = 0; i < totalSamples; i++) {
						float sample = src[i];
						sample = std::max(-1.0f, std::min(1.0f, sample)); // Clamp to [-1,1]
						int32_t value = static_cast<int32_t>(sample * 8388607.0f);
						
						// Write as little endian 24-bit
						dst[i*3+0] = (value & 0x000000FF);
						dst[i*3+1] = (value & 0x0000FF00) >> 8;
						dst[i*3+2] = (value & 0x00FF0000) >> 16;
					}
					break;
				}
				case ma_format_s32: {
					// Convert float32 [-1.0,1.0] to int32 [-2147483648,2147483647]
					int32_t* dst = static_cast<int32_t*>(buffer);
					for (ma_uint64 i = 0; i < totalSamples; i++) {
						float sample = src[i];
						sample = std::max(-1.0f, std::min(1.0f, sample)); // Clamp to [-1,1]
						dst[i] = static_cast<int32_t>(sample * 2147483647.0f);
					}
					break;
				}
				default:
					TVPLOG_ERROR("Unsupported output format: {}", (int)output_format);
					break;
			}
		#endif

		}			
	}
}

// --------------------------------------------------------------------------------
// ストリーム実装
// --------------------------------------------------------------------------------

// 1 producer / 1 consumer 用の固定容量ロックフリーリングバッファ。
// N は 2 のべき乗。コンシューマ側が追い越されない (audio thread 側が常に front から
// 順に消費し、producer は free 容量を見て push する) ことを呼び出し側が保証する。
template<typename T, size_t N>
class TVPSPSCRing {
	static_assert((N & (N - 1)) == 0, "N must be power of 2");
	T Buffer[N];
	std::atomic<size_t> Head{0};	// producer index
	std::atomic<size_t> Tail{0};	// consumer index

public:
	TVPSPSCRing() = default;

	bool TryPush(const T& v) {
		size_t h = Head.load(std::memory_order_relaxed);
		size_t next = (h + 1) & (N - 1);
		if (next == Tail.load(std::memory_order_acquire)) return false; // full
		Buffer[h] = v;
		Head.store(next, std::memory_order_release);
		return true;
	}

	bool TryPop(T& outValue) {
		size_t t = Tail.load(std::memory_order_relaxed);
		if (t == Head.load(std::memory_order_acquire)) return false; // empty
		outValue = Buffer[t];
		Tail.store((t + 1) & (N - 1), std::memory_order_release);
		return true;
	}

	// front を覗くだけ (ポインタは AdvancePop までは有効)
	T* TryPeek() {
		size_t t = Tail.load(std::memory_order_relaxed);
		if (t == Head.load(std::memory_order_acquire)) return nullptr;
		return &Buffer[t];
	}

	void AdvancePop() {
		size_t t = Tail.load(std::memory_order_relaxed);
		Tail.store((t + 1) & (N - 1), std::memory_order_release);
	}

	bool IsEmpty() const {
		return Tail.load(std::memory_order_acquire) == Head.load(std::memory_order_acquire);
	}
};

class MiniAudioStream;

struct my_data_source {
	ma_data_source_base base;
	ma_format Format;
	ma_uint32 Channels;
	ma_uint32 SampleRate;
	MiniAudioStream *Stream;
};

class MiniAudioStream : public iTVPAudioStream {

	// Pending/Consumed の容量。
	// QueueSoundBuffer 経路は BufferCount=2 で十分だが、movie-player 経路は
	// HW decoder が起動冒頭に realtime 以上で burst して数百チャンク級まで一時的
	// に積まれる。取りこぼすと再生せずに release してしまい音が虫食いになるので、
	// 1 entry ~32B × 1024 = 32KB と十分大きな容量を確保する。N は 2 のべき乗。
	// 64 だと NX 版で確認したのと同等の overflow が再現する。
	static const size_t RING_CAPACITY = 1024;

	struct DataBuffer {
		void *data;
		size_t size;
		bool last;
		void *param;
		DataBuffer() : data(nullptr), size(0), last(false), param(nullptr) {}
		DataBuffer(void *data, size_t size, bool last, void *param)
			: data(data), size(size), last(last), param(param) {}
	};

public:
	MiniAudioStream( const tTVPAudioStreamParam& param );
	virtual ~MiniAudioStream();

	virtual void SetWakeupHandler( StreamWakeupHandler handler, void* user ) override {
		// 呼び出しは Stream 開始前 (StartStream の前) に行うこと。
		// 開始後の差し替えは想定していない (audio thread と非同期になる)。
		WakeupHandler.store(handler, std::memory_order_release);
		WakeupUser.store(user, std::memory_order_release);
	}

	// 再生用データの投入（吉里吉里側から、典型的には decode thread）
	virtual void Enqueue( void *data, size_t size, bool last, void *param ) override {
		// BufferCount=2 設計で滞留は最大 2、容量 8 なのでまず満杯にならないが
		// 念のため失敗時は consumed 側に直送して欠落を回避する。
		if (!PendingRing.TryPush(DataBuffer(data, size, last, param))) {
			static int s_dropCount = 0;
			++s_dropCount;
			TVPLOG_WARNING("MiniAudioStream: Enqueue OVERFLOW drop count={} size={}",
			               s_dropCount, size);
			ConsumedRing.TryPush(param);
			NotifyConsumed();
		}
	}

	virtual tjs_uint64 GetSamplesPlayed() const override {
		return ma_sound_get_time_in_pcm_frames(&sound) * SampleRate / TVPSoundFrequency;
	}

	virtual bool TryPopConsumed( void*& outParam ) override {
		return ConsumedRing.TryPop(outParam);
	}

	// 注意: audio thread が停止している (StopStream 後) 状態で呼ぶこと。
	virtual void ClearQueue() override {
		while (DataBuffer* front = PendingRing.TryPeek()) {
			ConsumedRing.TryPush(front->param);
			PendingRing.AdvancePop();
		}
		ReadPosition = 0;
		EosReached.store(false, std::memory_order_release);
		NotifyConsumed();
	}

	virtual void StartStream() override {
		// 再開時は EOS フラグもリセットする。
		// (ma_sound_start 内部でも ma_sound::atEnd は MA_FALSE に戻されるので両者を揃える)
		EosReached.store(false, std::memory_order_release);
		ma_sound_start(&sound);
	}

	virtual void StopStream() override{ 
	    ma_sound_stop(&sound);
	}

	virtual bool IsPlaying() const override {
		return ma_sound_is_playing(&sound) != 0;
	}

	virtual bool AtEnd() const override {
		return ma_sound_at_end(&sound) == MA_TRUE;
	}

	virtual void SetVolume(tjs_int vol) override {
		if( vol > VOLUME_MAX ) vol = VOLUME_MAX;
		if( vol < 0) { vol = 0; }
		if( AudioVolumeValue != vol ) {
			AudioVolumeValue = vol;
			float level;
			if (vol <= 0) {
				level = 0.0f;
			} else {
				// WIN 版 DirectSound 経路と等価な perceptual curve に変換する。
				//   WIN:  TVPVolumeToDSAttenuate(v) = log10(v/100000) * Factor (mB)
				//         → DS 内部で 10^(att/2000) = (v/100000)^(Factor/2000)
				//   SDL:  同じ式を直接 linear gain として計算し miniaudio へ渡す。
				// Factor 既定 3322 でスライダ 50% ≒ -10 dB (知覚的に半分)。
				float normalized = (float)vol / (float)VOLUME_MAX;
				level = std::pow(normalized, (float)TVPVolumeLogFactor / 2000.0f);
				if (level < 0.0f) level = 0.0f;
				if (level > 1.0f) level = 1.0f;
			}
            ma_sound_set_volume(&sound, level);
		}
	}
	virtual tjs_int GetVolume() const override { return AudioVolumeValue; }

	virtual void SetPan(tjs_int pan) override {
		if( pan < -VOLUME_MAX ) pan = -VOLUME_MAX;
		else if( pan > VOLUME_MAX ) pan = VOLUME_MAX;
		if (AudioBalanceValue != pan) {
			AudioBalanceValue = pan;
            float panValue = (float)AudioBalanceValue / (float)VOLUME_MAX;
            ma_sound_set_pan(&sound, panValue);
		}
	}
	virtual tjs_int GetPan() const override { return AudioBalanceValue; }

	virtual void SetFrequency(tjs_int freq) override {
		if (AudioFrequency != freq) {
			AudioFrequency = freq;
            float pitch = (float)AudioFrequency / (float)SampleRate;
			ma_sound_set_pitch(&sound, pitch);
		}
	}
	virtual tjs_int GetFrequency() const override { return AudioFrequency; }

	// 再生用データの読み出し（再生ライブラリから吸い上げ）
	// 注意: miniaudio の audio callback スレッドから呼ばれる。
	// このパスではロック取得・vector 操作・syscall 等は避け、
	// 完了通知も lock-free ring + 1 wakeup のみに留める。
	ma_result ReadData(void* pFramesOut, ma_uint64 frameCount, ma_uint64* pFramesRead) {
		// EOS 確定済み: framesRead=0 で MA_AT_END を返し、miniaudio 側に
		// ma_sound_at_end を立てさせる。これがないと、last=true 消費 call では
		// framesRead==frameCount で返ってしまい外側 ma_data_source_read_pcm_frames が
		// MA_AT_END を握り潰す (totalFramesProcessed>0 → MA_SUCCESS) ため、
		// ma_sound_at_end() が永久に false のままになり QueueSoundBuffer の status が
		// ssPlay → ssStop に遷移しなくなる。
		if (EosReached.load(std::memory_order_acquire)) {
			if (pFramesRead) *pFramesRead = 0;
			return MA_AT_END;
		}

		char *dst = (char*)pFramesOut;
		ma_uint64 size = frameCount * FrameSize;
		bool last = false;
		bool didConsume = false;

		while (!last && size > 0) {
			DataBuffer* front = PendingRing.TryPeek();
			if (!front) break;

			ma_uint64 remain = front->size - ReadPosition;
			if (size < remain) {
				memcpy(dst, (char*)front->data + ReadPosition, (size_t)size);
				dst += size;
				ReadPosition += size;
				size = 0;
			} else {
				memcpy(dst, (char*)front->data + ReadPosition, (size_t)remain);
				if (front->last) last = true;
				void *param = front->param;
				PendingRing.AdvancePop();
				ReadPosition = 0;
				// consumed ring は容量 RING_CAPACITY あり、滞留も BufferCount(=2) 程度なので
				// 通常は失敗しないが、万一失敗しても再生品質を優先しドロップする。
				ConsumedRing.TryPush(param);
				didConsume = true;
				dst += remain;
				size -= remain;
			}
		}

		if (didConsume) NotifyConsumed();

		// pending が一時的に空 (アンダーラン) になった場合、残りを silence で埋める。
		// ここで MA_AT_END を返してしまうと miniaudio が ma_sound を auto-stop し、
		// 以降データを enqueue しても callback が呼ばれなくなる (動画音声のように
		// producer と consumer のタイミングがずれて瞬間的に空になる経路で致命的)。
		// 真の EOS は EOS マーカーバッファ (last=true) でのみ MA_AT_END を返す。
		if (size > 0) {
			memset(dst, 0, (size_t)size);
		}

		// last=true バッファを実際に消費した瞬間に EOS フラグを立てる。
		// この call では framesRead=frameCount + MA_AT_END を返し、続く callback の
		// ReadData で framesRead=0 + MA_AT_END を返して ma_sound_at_end を確定させる。
		if (last) {
			EosReached.store(true, std::memory_order_release);
		}

		if (pFramesRead) *pFramesRead = frameCount;
		return last ? MA_AT_END : MA_SUCCESS;
	}

private:
	void NotifyConsumed() {
		// audio thread 上で 1 回だけ呼ぶ。handler は Event.Set 等の最小処理に限定される想定。
		StreamWakeupHandler h = WakeupHandler.load(std::memory_order_acquire);
		if (h) h(WakeupUser.load(std::memory_order_acquire));
	}

	std::atomic<StreamWakeupHandler> WakeupHandler;
	std::atomic<void*> WakeupUser;

	tjs_int SampleRate;
	tjs_int FrameSize;
	tjs_int AudioVolumeValue;
	tjs_int AudioBalanceValue;
	tjs_int AudioFrequency;

    my_data_source data_source;
    ma_sound sound;

	TVPSPSCRing<DataBuffer, RING_CAPACITY> PendingRing;
	TVPSPSCRing<void*, RING_CAPACITY> ConsumedRing;
	// audio thread のみがアクセス。front buffer 内の現在位置 (バイト)。
	ma_uint64 ReadPosition;

	// last=true バッファを消費した瞬間に立つ EOS フラグ。一度立つと、PendingRing の
	// 内容に関わらず ReadData は (framesRead=0, MA_AT_END) を返し続け、miniaudio の
	// ma_sound_at_end() が true に転じる契機を作る。
	// StartStream() / ClearQueue() でリセット。
	// (movie 経路の単発 underrun と EOS を区別するため必要)
	std::atomic<bool> EosReached;
};

// --------------------------------------------------------------------------------
// デバイス側実装
// --------------------------------------------------------------------------------

iTVPAudioStream* TVPCreateAudioStream(tTVPAudioStreamParam &param) 
{
	MiniAudioStream* stream = new MiniAudioStream(param);
	return stream;
}

// --------------------------------------------------------------------------------
// miniaudio データストリーム実装
// --------------------------------------------------------------------------------

static ma_result my_data_source_read(ma_data_source* pDataSource, void* pFramesOut, ma_uint64 frameCount, ma_uint64* pFramesRead)
{
	my_data_source *self = (my_data_source*)pDataSource;
	if (self) {
		MiniAudioStream *stream = self->Stream;
		if (stream) {
			return stream->ReadData(pFramesOut, frameCount, pFramesRead);
		}
	}
	// Read data here. Output in the same format returned by my_data_source_get_data_format().
	return MA_AT_END;
}

static ma_result my_data_source_seek(ma_data_source* pDataSource, ma_uint64 frameIndex)
{
	return MA_NOT_IMPLEMENTED;
}

static ma_result my_data_source_get_data_format(ma_data_source* pDataSource, ma_format* pFormat, ma_uint32* pChannels, ma_uint32* pSampleRate, ma_channel* pChannelMap, size_t channelMapCap)
{
	// Return the format of the data here.
	my_data_source *self = (my_data_source*)pDataSource;
	if (pFormat) {
		*pFormat = self->Format;
	}
	if (pChannels) {
		*pChannels = self->Channels;
	}
	if (pSampleRate) {
		*pSampleRate = self->SampleRate;
	}
	return MA_SUCCESS;
}

static ma_result my_data_source_get_cursor(ma_data_source* pDataSource, ma_uint64* pCursor)
{
	if (pCursor) {
		*pCursor = 0;
	}
	return MA_NOT_IMPLEMENTED;
}

static ma_result my_data_source_get_length(ma_data_source* pDataSource, ma_uint64* pLength)
{
	if (pLength) {
		*pLength = 0;
	}
	return MA_NOT_IMPLEMENTED;
};

static ma_data_source_vtable g_my_data_source_vtable =
{
	my_data_source_read,
	my_data_source_seek,
	my_data_source_get_data_format,
	my_data_source_get_cursor,
	my_data_source_get_length,
	NULL,
	MA_DATA_SOURCE_SELF_MANAGED_RANGE_AND_LOOP_POINT
};

// --------------------------------------------------------------------------------
// ストリーム実装
// --------------------------------------------------------------------------------

MiniAudioStream::MiniAudioStream(const tTVPAudioStreamParam& param )
: WakeupHandler(nullptr)
, WakeupUser(nullptr)
, SampleRate(param.SampleRate)
, FrameSize(param.BitsPerSample/8 * param.Channels)
, AudioVolumeValue(VOLUME_MAX)
, AudioBalanceValue(0)
, AudioFrequency(param.SampleRate)
, ReadPosition(0)
, EosReached(false)
{
	data_source.Format     = ((param.BitsPerSample == 8)? ma_format_u8 : ((param.BitsPerSample == 16)? ma_format_s16 : ma_format_f32));
	data_source.Channels   = param.Channels;
	data_source.SampleRate = param.SampleRate;
	data_source.Stream     = this;

    auto dataSourceConfig = ma_data_source_config_init();
    dataSourceConfig.vtable = &g_my_data_source_vtable;
    ma_data_source_init(&dataSourceConfig, &data_source);

    // NO_SPATIALIZATION: 3D 空間化を切り、ソース ch → engine ch を channel-map ベースで
    // 変換 (5.1 音源を discrete にパススルー、ステレオはフロントへアップミックス)。
    // pan/volume/pitch は spatialization とは独立に効く。
    ma_sound_init_from_data_source(GetMiniAudioEngine(), &data_source,
        MA_SOUND_FLAG_NO_SPATIALIZATION, NULL, &sound);
}

MiniAudioStream::~MiniAudioStream()
{
    ma_sound_uninit(&sound);
    ma_data_source_uninit(&data_source);
}

