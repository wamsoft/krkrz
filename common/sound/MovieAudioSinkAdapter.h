//---------------------------------------------------------------------------
// MovieAudioSinkAdapter
//   movie-player の IAudioSink を iTVPAudioStream で実装するヘッダオンリー
//   アダプタ。VideoOverlay / WebpMovie が 1 つずつインスタンスを保持し、
//   InitParam.audioSink に渡す。
//
//   Android のみ、Enqueue で受け取った PCM を内部バッファへコピーしてから
//   stream へ流し、codec の param を即 consumed へ返す経路を取る。
//   (理由は下の ENQUEUE セクション参照)
//---------------------------------------------------------------------------
#ifndef _MOVIE_AUDIO_SINK_ADAPTER_H__
#define _MOVIE_AUDIO_SINK_ADAPTER_H__

#include "AudioStream.h"
#include "IAudioSink.h"

#include <chrono>
#include <thread>

#ifdef __ANDROID__
#include <vector>
#include <deque>
#include <memory>
#endif

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
		// (Android) mCopies は unique_ptr 所有。mStream を先に破棄して
		// audio thread を止めてあるので、ここで安全に解放される。
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

#ifdef __ANDROID__
	// -- ENQUEUE (Android) -----------------------------------------------------
	//   data は AMediaCodec の output buffer (借り物)。codec の output slot は
	//   2-4 程度しかなく、param を consumed へ返すまで slot が pin される。
	//   miniaudio (MiniAudioStream) は再生タイミングまで data ポインタを保持
	//   するため、そのまま渡すと codec slot が pin され続け、input 側まで
	//   backpressure が伝搬 → decoder が dequeueInputBuffer の timeout に
	//   張り付き PCM 供給が落ちる → audio callback が無音パディングを挿入する
	//   (= 体感「音だけ遅い」)。
	//
	//   そこで PCM を内部バッファへ *コピー* してから stream へ流し、codec の
	//   param はその場で consumed キューに積んで slot を即解放する。
	//   コピーバッファ自体は stream の consumed 経由でフリーリストに戻して再利用
	//   する (毎フレームの確保を避ける)。
	//
	//   ただし codec slot を即解放すると、それが担っていた「再生に追いつくまで
	//   decoder を待たせる」backpressure が無くなり、音声デコードが青天井に先行
	//   して MiniAudioStream の固定長 PendingRing を溢れさせる (= ドロップ →
	//   早回し/前後ずれ・動画破綻)。これを防ぐため、stream へ未再生のまま積まれて
	//   いるコピー数 (in-flight) が上限を超えている間は decode thread をここで
	//   待たせ、再生 (audio thread の消費) が進むのを待つ。これが従来 codec slot
	//   の backpressure が担っていた律速の代替になる。
	void Enqueue(const void *data, size_t bytes, bool last,
				 void *param) override
	{
		if (!mStream) return;

		// 先に再生済みコピーをフリーリストへ回収
		DrainStreamConsumed();

		if (!data || bytes == 0) {
			// 通常 movie 側は size 0 を sink へ流さないが、last マーカーだけは
			// EOS を stream へ伝える必要があるので空バッファで流す。
			if (last) {
				CopyBuffer *c = AcquireCopy();
				c->data.clear();
				mStream->Enqueue(c->data.data(), 0, true, c);
			}
			if (param) mConsumedParams.push_back(param);
			return;
		}

		// 流量制御 (backpressure 代替): in-flight が上限を超えている間は待つ。
		WaitForInflightRoom();

		CopyBuffer *c = AcquireCopy();
		c->data.assign(static_cast<const tjs_uint8 *>(data),
					   static_cast<const tjs_uint8 *>(data) + bytes);
		// stream へはコピーを渡す。param にはコピー識別子を載せ、再生完了通知
		// (TryPopConsumed) で受け取り次第フリーリストへ戻す。
		mStream->Enqueue(c->data.data(), c->data.size(), last, c);

		// codec slot は即解放できるよう、コピー直後に movie の param を consumed へ。
		if (param) mConsumedParams.push_back(param);
	}

	bool TryPopConsumed(void **outParam) override
	{
		DrainStreamConsumed();
		if (mConsumedParams.empty()) return false;
		if (outParam) *outParam = mConsumedParams.front();
		mConsumedParams.pop_front();
		return true;
	}

	void Flush() override
	{
		if (!mStream) return;
		// movie の param は Enqueue 時点で consumed へ返却済みなので、ここでは
		// stream の pending (コピー) を捨ててコピーを回収するだけでよい。
		bool wasPlaying = mStream->IsPlaying();
		mStream->StopStream();
		// ma_sound_stop は async stop なので最後の callback が抜けるのを少し待つ。
		std::this_thread::sleep_for(std::chrono::milliseconds(20));
		mStream->ClearQueue();
		DrainStreamConsumed(); // ClearQueue が consumed へ流したコピーを回収
		if (wasPlaying) {
			mStream->StartStream();
		}
	}
#else
	void Enqueue(const void *data, size_t bytes, bool last,
				 void *param) override
	{
		if (!mStream) return;
		// iTVPAudioStream::Enqueue は const ポインタを取らないので const_cast。
		// data の所有権は呼び出し側 (movie-player の DecodedBuffer) が consumed
		// 通知まで保つ契約。
		mStream->Enqueue(const_cast<void *>(data), bytes, last, param);
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
#endif

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

#ifdef __ANDROID__
	// stream へ積めるコピーの in-flight 上限。MiniAudioStream の PendingRing
	// 容量 (1024) より十分小さく、かつ underrun しない程度に buffer できる値。
	// codec output buffer 1 個あたり数十 ms 相当なので 128 で ~数秒ぶん。
	static constexpr size_t kMaxInflightCopies = 128;
	// 上限到達時の待ち間隔と、念のための spin 上限 (deadlock 回避の安全弁)。
	static constexpr int    kBackpressureSleepMs = 2;
	static constexpr int    kMaxBackpressureSpins = 500; // 最大 ~1 秒

	// Enqueue でコピーした PCM の保持単位。stream へ渡したあと、再生完了通知で
	// 回収してフリーリストへ戻し再利用する。
	struct CopyBuffer {
		std::vector<tjs_uint8> data;
	};

	// stream が再生完了したコピー (param=CopyBuffer*) を回収してフリーリストへ。
	void DrainStreamConsumed()
	{
		if (!mStream) return;
		void *p = nullptr;
		while (mStream->TryPopConsumed(p)) {
			mFreeCopies.push_back(static_cast<CopyBuffer *>(p));
		}
	}

	// stream へ未再生のまま積まれているコピー数 (確保済み - 再利用待ち)。
	size_t InflightCopies() const { return mCopies.size() - mFreeCopies.size(); }

	// in-flight が上限未満になるまで decode thread を待たせる (backpressure)。
	void WaitForInflightRoom()
	{
		int spin = 0;
		while (InflightCopies() >= kMaxInflightCopies) {
			DrainStreamConsumed();
			if (InflightCopies() < kMaxInflightCopies) break;
			if (++spin > kMaxBackpressureSpins) break; // 安全弁 (通常到達しない)
			std::this_thread::sleep_for(
				std::chrono::milliseconds(kBackpressureSleepMs));
		}
	}

	CopyBuffer *AcquireCopy()
	{
		if (!mFreeCopies.empty()) {
			CopyBuffer *c = mFreeCopies.back();
			mFreeCopies.pop_back();
			return c;
		}
		mCopies.push_back(std::unique_ptr<CopyBuffer>(new CopyBuffer()));
		return mCopies.back().get();
	}
#endif

private:
	iTVPAudioStream *mStream;
	float mVolume;

#ifdef __ANDROID__
	// 全コピーバッファの所有 (デストラクタで一括解放)
	std::vector<std::unique_ptr<CopyBuffer>> mCopies;
	// 再利用待ちのコピー (mCopies の部分集合への非所有ポインタ)
	std::vector<CopyBuffer *> mFreeCopies;
	// movie へ返す consumed param (= codec bufIdx)。Enqueue 時に即積む。
	// Enqueue / TryPopConsumed / Flush は全て movie decode thread からのみ
	// 呼ばれる (single producer/consumer) ためロック不要。
	std::deque<void *> mConsumedParams;
#endif
};

#endif // _MOVIE_AUDIO_SINK_ADAPTER_H__
