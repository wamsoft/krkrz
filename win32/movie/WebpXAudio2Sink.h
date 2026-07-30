//---------------------------------------------------------------------------
// WebpXAudio2Sink
//   krmovie の webm 経路 (tTVPWebpMovie / movie-player) 用の IAudioSink 実装。
//   XAudio2 の実処理は共通の tTVPMovieAudioSink に一本化し、本クラスは
//   movie-player の IAudioSink 契約 (Enqueue/param/TryPopConsumed) に合わせる
//   薄いアダプタに徹する。
//   (以前は本ファイルに XAudio2 実装一式が重複していたが、MF/pl_mpeg 用の
//    tTVPMovieAudioSink と同一だったため統合した。)
//---------------------------------------------------------------------------
#pragma once

#include "IAudioSink.h"
#include "MovieAudioSink.h"

class WebpXAudio2Sink : public IAudioSink
{
public:
	bool Setup(int channels, int sampleRate, int bitsPerSample, Encoding encoding) override
	{
		return mSink.Setup(channels, sampleRate, bitsPerSample, /*isFloat=*/encoding == PCM_F32);
	}
	void Enqueue(const void *data, size_t bytes, bool last, void *param) override
	{
		// tTVPMovieAudioSink は内部コピーするので、Enqueue から戻れば呼び出し側
		// (movie-player の DecodedBuffer) は安全。param は再生完了時に
		// TryPopConsumed で返す (バックプレッシャは XAudio2 の OnBufferEnd 基準)。
		mSink.Submit(data, bytes, last, param);
	}
	void Start() override { mSink.Start(); }
	void Stop()  override { mSink.Stop(); }
	int64_t GetSamplesPlayed() const override { return mSink.GetSamplesPlayed(); }
	bool TryPopConsumed(void **outParam) override { return mSink.TryPopConsumed(outParam); }
	void Flush() override { mSink.Flush(); }
	void SetVolume(float volume) override { mSink.SetVolume(volume); }
	float Volume() const override { return mSink.Volume(); }

private:
	tTVPMovieAudioSink mSink;
};
