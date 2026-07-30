/****************************************************************************/
/*! @file
@brief 新レイヤ動画プレイヤ (MF SourceReader / pl_mpeg) 共通の XAudio2 音声シンク

Track V。krmovie は別 DLL で krkrz 本体の miniaudio とリンクできないため、
WIN ネイティブ API の XAudio2 で音声出力する (webplayer の WebpXAudio2Sink と
同方針だが movie-player の IAudioSink には依存しない独立実装)。
GetSamplesPlayed() を A/V 同期のマスタクロックとして使う。
*****************************************************************************/
#ifndef __MOVIE_AUDIO_SINK_H__
#define __MOVIE_AUDIO_SINK_H__

#include <windows.h>
#include <xaudio2.h>
#include <atomic>
#include <cstdint>
#include <cstddef>

#pragma comment(lib, "xaudio2.lib")

class tTVPMovieAudioSink : public IXAudio2VoiceCallback
{
	// 1 producer(decode thread) / 1 consumer(drain) 用ロックフリーリング
	template<typename T, size_t N>
	class SPSCRing {
		static_assert((N & (N - 1)) == 0, "N must be power of 2");
	public:
		SPSCRing() : mHead(0), mTail(0) {}
		bool TryPush(const T &v) {
			size_t h = mHead.load(std::memory_order_relaxed);
			size_t next = (h + 1) & (N - 1);
			if (next == mTail.load(std::memory_order_acquire)) return false;
			mBuffer[h] = v; mHead.store(next, std::memory_order_release); return true;
		}
		bool TryPop(T &out) {
			size_t t = mTail.load(std::memory_order_relaxed);
			if (t == mHead.load(std::memory_order_acquire)) return false;
			out = mBuffer[t]; mTail.store((t + 1) & (N - 1), std::memory_order_release); return true;
		}
	private:
		T mBuffer[N];
		std::atomic<size_t> mHead, mTail;
	};

	struct SubmitContext { uint8_t *data; void *param; };

	IXAudio2 *mXAudio2;
	IXAudio2MasteringVoice *mMaster;
	IXAudio2SourceVoice *mSource;
	int   mSampleRate;
	float mVolume;
	std::atomic<int> mQueued; //!< 未再生バッファ数 (音声供給の絞り込み用)
	SPSCRing<SubmitContext *, 128> mConsumedRing;

public:
	tTVPMovieAudioSink()
	: mXAudio2(nullptr), mMaster(nullptr), mSource(nullptr)
	, mSampleRate(0), mVolume(1.0f), mQueued(0)
	{
		if (FAILED(XAudio2Create(&mXAudio2, 0, XAUDIO2_DEFAULT_PROCESSOR))) { mXAudio2 = nullptr; return; }
		if (FAILED(mXAudio2->CreateMasteringVoice(&mMaster))) mMaster = nullptr;
	}
	~tTVPMovieAudioSink()
	{
		if (mSource) { mSource->Stop(); mSource->FlushSourceBuffers(); mSource->DestroyVoice(); mSource = nullptr; }
		if (mMaster) { mMaster->DestroyVoice(); mMaster = nullptr; }
		if (mXAudio2) { mXAudio2->Release(); mXAudio2 = nullptr; }
		DrainConsumed();
	}

	bool IsAvailable() const { return mXAudio2 != nullptr && mMaster != nullptr; }

	//! 音声フォーマットを設定して source voice を作る。isFloat=true で 32bit float。
	bool Setup(int channels, int sampleRate, int bitsPerSample, bool isFloat)
	{
		if (mSource || !IsAvailable()) return false;
		WAVEFORMATEX fmt = {};
		fmt.wFormatTag      = isFloat ? WAVE_FORMAT_IEEE_FLOAT : WAVE_FORMAT_PCM;
		fmt.nChannels       = (WORD)channels;
		fmt.nSamplesPerSec  = (DWORD)sampleRate;
		fmt.wBitsPerSample  = (WORD)bitsPerSample;
		fmt.nBlockAlign     = (WORD)((channels * bitsPerSample) / 8);
		fmt.nAvgBytesPerSec = fmt.nSamplesPerSec * fmt.nBlockAlign;
		if (FAILED(mXAudio2->CreateSourceVoice(&mSource, &fmt, 0, XAUDIO2_DEFAULT_FREQ_RATIO, this))) {
			mSource = nullptr; return false;
		}
		mSampleRate = sampleRate;
		mSource->SetVolume(mVolume);
		return true;
	}

	//! PCM を送出 (内部コピー。XAudio2 の寿命とデコーダバッファを分離)。
	//! param は再生完了時に TryPopConsumed で返す識別子 (IAudioSink アダプタ用。
	//! 直接利用時は nullptr で可)。
	void Submit(const void *data, size_t bytes, bool last = false, void *param = nullptr)
	{
		if (!mSource || bytes == 0) return;
		SubmitContext *ctx = new SubmitContext;
		ctx->data = new uint8_t[bytes];
		ctx->param = param;
		memcpy(ctx->data, data, bytes);
		XAUDIO2_BUFFER buf = {};
		buf.AudioBytes = (UINT32)bytes;
		buf.pAudioData = ctx->data;
		buf.pContext   = ctx;
		buf.Flags      = last ? XAUDIO2_END_OF_STREAM : 0;
		if (SUCCEEDED(mSource->SubmitSourceBuffer(&buf))) {
			mQueued.fetch_add(1, std::memory_order_relaxed);
		} else {
			delete[] ctx->data; delete ctx;
		}
	}

	void Start() { if (mSource) mSource->Start(); }
	void Stop()  { if (mSource) mSource->Stop(); }
	void Flush() { if (mSource) { mSource->Stop(); mSource->FlushSourceBuffers(); } DrainConsumed(); }

	//! 未再生バッファ数 (供給を絞るための目安)。
	int QueuedBuffers() const { return mQueued.load(std::memory_order_relaxed); }

	//! 再生済みサンプル数 → A/V 同期クロック。
	int64_t GetSamplesPlayed() const {
		if (!mSource) return 0;
		XAUDIO2_VOICE_STATE st = {};
		mSource->GetState(&st, 0);
		return (int64_t)st.SamplesPlayed;
	}
	//! 再生位置 (ms)。音声が無い/未設定なら -1。
	int64_t GetPlayedMs() const {
		if (!mSource || mSampleRate <= 0) return -1;
		return GetSamplesPlayed() * 1000 / mSampleRate;
	}

	//! 再生完了エントリを 1 件取り出し、内部コピーを解放して param を返す。
	//! (IAudioSink アダプタが movie-player の DecodedBuffer 解放に使う)。無ければ false。
	bool TryPopConsumed(void **outParam) {
		SubmitContext *ctx = nullptr;
		if (!mConsumedRing.TryPop(ctx)) return false;
		if (ctx) { if (outParam) *outParam = ctx->param; delete[] ctx->data; delete ctx; }
		else if (outParam) *outParam = nullptr;
		return true;
	}

	//! 再生完了バッファのメモリを解放する (param 不要な直接利用時。decode thread から)。
	void DrainConsumed() {
		void *p = nullptr;
		while (TryPopConsumed(&p)) {}
	}

	void SetVolume(float v) {
		if (v < 0.0f) v = 0.0f; if (v > 1.0f) v = 1.0f;
		mVolume = v; if (mSource) mSource->SetVolume(v);
	}
	float Volume() const { return mVolume; }

	// IXAudio2VoiceCallback (XAudio2 audio thread。重い処理禁止) --------------
	STDMETHODIMP_(void) OnVoiceProcessingPassStart(UINT32) override {}
	STDMETHODIMP_(void) OnVoiceProcessingPassEnd() override {}
	STDMETHODIMP_(void) OnStreamEnd() override {}
	STDMETHODIMP_(void) OnBufferStart(void *) override {}
	STDMETHODIMP_(void) OnBufferEnd(void *pctx) override {
		// 再生完了。delete は decode thread(DrainConsumed) 側で行うため ring へ流す。
		mQueued.fetch_sub(1, std::memory_order_relaxed);
		mConsumedRing.TryPush((SubmitContext *)pctx);
	}
	STDMETHODIMP_(void) OnLoopEnd(void *) override {}
	STDMETHODIMP_(void) OnVoiceError(void *, HRESULT) override {}
};

#endif // __MOVIE_AUDIO_SINK_H__
