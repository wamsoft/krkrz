/****************************************************************************/
/*! @file
@brief 新レイヤ動画プレイヤ (MF SourceReader / pl_mpeg) 共通の音声シンク

Track V。**音声出力は miniaudio (iTVPAudioStream) に統合済み** (旧 XAudio2 実装は
Track V-A' で撤去)。デコーダが渡す PCM は借り物 (MF の IMFMediaBuffer は Unlock で、
pl_mpeg の interleaved は次デコードで上書き) なので、内部へ *コピー* してから stream
へ流し、再生完了通知 (TryPopConsumed) でコピーをフリーリストへ回収して再利用する。
GetPlayedMs() を A/V 同期のマスタクロックとして使う。

公開 API は旧 XAudio2 版と同一なので、レイヤデコーダ (LayerVideoBase /
MFSourceReaderVideo / Mpeg1Video) 側は無改造。Submit / QueuedBuffers / GetPlayedMs /
DrainConsumed は全て decode thread からのみ呼ばれる (single producer/consumer) ため
ロック不要 (stream の consumed キューは iTVPAudioStream 側でスレッド安全)。
*****************************************************************************/
#ifndef __MOVIE_AUDIO_SINK_H__
#define __MOVIE_AUDIO_SINK_H__

#include "AudioStream.h"   // iTVPAudioStream / TVPCreateAudioStream / tTVPAudioStreamParam
#include <vector>
#include <cstdint>
#include <cstddef>
#include <chrono>
#include <thread>

class tTVPMovieAudioSink
{
	// Submit でコピーした PCM の保持単位。stream へ渡したあと再生完了通知で回収し
	// フリーリストへ戻して再利用する (毎フレームの確保を避ける)。
	struct CopyBuffer { std::vector<uint8_t> data; };

	iTVPAudioStream *mStream;
	int   mSampleRate;
	float mVolume;
	std::vector<CopyBuffer*> mAll;   //!< 全コピーバッファ所有 (デストラクタで一括解放)
	std::vector<CopyBuffer*> mFree;  //!< 再利用待ち (mAll の部分集合への非所有ポインタ)

	CopyBuffer *Acquire()
	{
		if (!mFree.empty()) { CopyBuffer *c = mFree.back(); mFree.pop_back(); return c; }
		CopyBuffer *c = new CopyBuffer(); mAll.push_back(c); return c;
	}

public:
	tTVPMovieAudioSink()
	: mStream(nullptr), mSampleRate(0), mVolume(1.0f) {}

	~tTVPMovieAudioSink()
	{
		if (mStream) { mStream->StopStream(); delete mStream; mStream = nullptr; }
		for (CopyBuffer *c : mAll) delete c;
	}

	//! stream は Setup で生成する。生成前でも呼び出せるよう常に true を返す
	//! (旧 XAudio2 版は device 初期化可否を返したが、miniaudio は Setup で判定)。
	bool IsAvailable() const { return true; }

	//! 音声フォーマットを設定して stream を作る。isFloat=true で 32bit float。
	bool Setup(int channels, int sampleRate, int bitsPerSample, bool isFloat)
	{
		if (mStream) return false;
		tTVPAudioStreamParam param;
		param.Channels      = (tjs_uint32)channels;
		param.SampleRate    = (tjs_uint32)sampleRate;
		param.BitsPerSample = (tjs_uint32)bitsPerSample;
		if      (isFloat)            param.SampleType = astFloat32;
		else if (bitsPerSample == 8) param.SampleType = astUInt8;
		else if (bitsPerSample == 32)param.SampleType = astInt32;
		else                         param.SampleType = astInt16;
		mStream = TVPCreateAudioStream(param);
		if (!mStream) return false;
		mSampleRate = sampleRate;
		ApplyVolume();
		return true;
	}

	//! PCM を送出 (内部コピー。stream の寿命とデコーダの借り物バッファを分離)。
	//! param は旧版互換で受けるが、レイヤ経路では未使用 (常に copy 識別子で上書き)。
	void Submit(const void *data, size_t bytes, bool last = false, void * /*param*/ = nullptr)
	{
		if (!mStream) return;
		if (bytes == 0 && !last) return;
		DrainConsumed(); // 先に再生済みコピーを回収してフリーリストへ
		CopyBuffer *c = Acquire();
		c->data.assign(static_cast<const uint8_t*>(data),
					   static_cast<const uint8_t*>(data) + bytes);
		mStream->Enqueue(c->data.data(), c->data.size(), last, c);
	}

	void Start() { if (mStream) mStream->StartStream(); }
	void Stop()  { if (mStream) mStream->StopStream(); }

	void Flush()
	{
		if (!mStream) return;
		bool wasPlaying = mStream->IsPlaying();
		mStream->StopStream();
		// ma_sound_stop は async stop なので最後の callback が抜けるのを少し待つ
		// (Flush は seek 等の低頻度パスでのみ呼ばれる想定)。
		std::this_thread::sleep_for(std::chrono::milliseconds(20));
		mStream->ClearQueue();
		DrainConsumed(); // ClearQueue が consumed へ流したコピーを回収
		if (wasPlaying) mStream->StartStream();
	}

	//! 未再生バッファ数 (供給を絞るための目安 = in-flight コピー数)。
	int QueuedBuffers() const { return (int)(mAll.size() - mFree.size()); }

	//! 再生済みサンプル数 → A/V 同期クロック。
	int64_t GetSamplesPlayed() const
	{
		return mStream ? (int64_t)mStream->GetSamplesPlayed() : 0;
	}
	//! 再生位置 (ms)。音声が無い/未設定なら -1。
	int64_t GetPlayedMs() const
	{
		if (!mStream || mSampleRate <= 0) return -1;
		return GetSamplesPlayed() * 1000 / mSampleRate;
	}

	//! 再生完了エントリを 1 件取り出し、コピーをフリーリストへ戻す。無ければ false。
	//! (レイヤ経路は param を使わないので outParam には常に nullptr を返す)。
	bool TryPopConsumed(void **outParam)
	{
		if (!mStream) return false;
		void *p = nullptr;
		if (!mStream->TryPopConsumed(p)) return false;
		if (p) mFree.push_back(static_cast<CopyBuffer*>(p));
		if (outParam) *outParam = nullptr;
		return true;
	}

	//! 再生完了コピーを全て回収する (decode thread から)。
	void DrainConsumed()
	{
		void *p = nullptr;
		while (TryPopConsumed(&p)) {}
	}

	void SetVolume(float v)
	{
		if (v < 0.0f) v = 0.0f; if (v > 1.0f) v = 1.0f;
		mVolume = v; ApplyVolume();
	}
	float Volume() const { return mVolume; }

private:
	void ApplyVolume()
	{
		// iTVPAudioStream の volume 範囲は 0 .. 100000
		if (mStream) mStream->SetVolume((tjs_int)(mVolume * 100000.0f + 0.5f));
	}
};

#endif // __MOVIE_AUDIO_SINK_H__
