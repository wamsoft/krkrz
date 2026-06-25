//---------------------------------------------------------------------------
// MovieAudioSinkAdapter
//   movie-player の IAudioSink を iTVPAudioStream で実装するヘッダオンリー
//   アダプタ。VideoOverlay / WebpMovie が 1 つずつインスタンスを保持し、
//   InitParam.audioSink に渡す。
//---------------------------------------------------------------------------
#ifndef _MOVIE_AUDIO_SINK_ADAPTER_H__
#define _MOVIE_AUDIO_SINK_ADAPTER_H__

#include "AudioStream.h"
#include "IAudioSink.h"

#include <chrono>
#include <thread>

class tTVPMovieAudioSinkAdapter : public IAudioSink
{
public:
	tTVPMovieAudioSinkAdapter() : mStream(nullptr), mVolume(1.0f) {}

	~tTVPMovieAudioSinkAdapter() override
	{
		if (mStream) {
			mStream->StopStream();
			delete mStream;
			mStream = nullptr;
		}
	}

	bool Setup(int channels, int sampleRate, int bitsPerSample,
			   Encoding encoding) override
	{
		if (mStream) return false; // 二重 Setup 防止

		tTVPAudioStreamParam param;
		param.Channels      = (tjs_uint32)channels;
		param.SampleRate    = (tjs_uint32)sampleRate;
		param.BitsPerSample = (tjs_uint32)bitsPerSample;
		switch (encoding) {
		case PCM_U8:  param.SampleType = astUInt8;   break;
		case PCM_S16: param.SampleType = astInt16;   break;
		case PCM_S32: param.SampleType = astInt32;   break;
		case PCM_F32: param.SampleType = astFloat32; break;
		default: return false;
		}

		mStream = TVPCreateAudioStream(param);
		if (!mStream) return false;

		// 起動前に cached volume を反映
		ApplyVolumeToStream();
		return true;
	}

	void Enqueue(const void *data, size_t bytes, bool last,
				 void *param) override
	{
		if (!mStream) return;
		// iTVPAudioStream::Enqueue は const ポインタを取らないので const_cast。
		// data の所有権は呼び出し側 (movie-player の DecodedBuffer) が consumed
		// 通知まで保つ契約。
		mStream->Enqueue(const_cast<void *>(data), bytes, last, param);
	}

	void Start() override
	{
		if (mStream) mStream->StartStream();
	}

	void Stop() override
	{
		if (mStream) mStream->StopStream();
	}

	int64_t GetSamplesPlayed() const override
	{
		return mStream ? (int64_t)mStream->GetSamplesPlayed() : 0;
	}

	bool TryPopConsumed(void **outParam) override
	{
		if (!mStream) return false;
		void *p = nullptr;
		bool got = mStream->TryPopConsumed(p);
		if (got && outParam) *outParam = p;
		return got;
	}

	void Flush() override
	{
		if (!mStream) return;
		// iTVPAudioStream::ClearQueue は audio thread が停止している前提なので
		// StopStream → 短い猶予 (audio callback が確実に抜ける) → ClearQueue。
		// 直後に呼び出し側が Start() を再発行する前提なので、ここでは Start し直さない。
		bool wasPlaying = mStream->IsPlaying();
		mStream->StopStream();
		// ma_sound_stop は async stop なので最後の callback が抜けるのを少し待つ。
		// Seek 等の頻度の低いパスでのみ呼ばれる想定。
		std::this_thread::sleep_for(std::chrono::milliseconds(20));
		mStream->ClearQueue();
		if (wasPlaying) {
			mStream->StartStream();
		}
	}

	void SetVolume(float volume) override
	{
		if (volume < 0.0f) volume = 0.0f;
		if (volume > 1.0f) volume = 1.0f;
		mVolume = volume;
		ApplyVolumeToStream();
	}

	float Volume() const override { return mVolume; }

private:
	void ApplyVolumeToStream()
	{
		if (!mStream) return;
		// iTVPAudioStream の volume 範囲は 0 .. 100000
		mStream->SetVolume((tjs_int)(mVolume * 100000.0f + 0.5f));
	}

private:
	iTVPAudioStream *mStream;
	float mVolume;
};

#endif // _MOVIE_AUDIO_SINK_ADAPTER_H__
