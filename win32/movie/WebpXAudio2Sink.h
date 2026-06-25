//---------------------------------------------------------------------------
// WebpXAudio2Sink
//   krmovie プラグインの webm 経路 (tTVPWebpMovie) 用の IAudioSink 実装。
//   krkrz 本体の miniaudio エンジンとは独立して XAudio2 で音声出力する。
//   (krmovie は別 DLL で krkrz の AudioStream.cpp とリンクできないため、
//    WIN ネイティブ API である XAudio2 を直接使う)
//---------------------------------------------------------------------------
#pragma once

#include "IAudioSink.h"

#include <windows.h>
#include <xaudio2.h>

#include <atomic>
#include <cstddef>
#include <cstdint>

namespace krkrz_webp_sink {

// 1 producer / 1 consumer 用の固定容量ロックフリーリング。
// audio thread (XAudio2 callback) と decoder thread (movie-player) の橋渡し用。
template<typename T, size_t N>
class SPSCRing
{
  static_assert((N & (N - 1)) == 0, "N must be power of 2");

public:
  SPSCRing() : mHead(0), mTail(0) {}

  bool TryPush(const T &v)
  {
    size_t h    = mHead.load(std::memory_order_relaxed);
    size_t next = (h + 1) & (N - 1);
    if (next == mTail.load(std::memory_order_acquire)) return false;
    mBuffer[h] = v;
    mHead.store(next, std::memory_order_release);
    return true;
  }

  bool TryPop(T &out)
  {
    size_t t = mTail.load(std::memory_order_relaxed);
    if (t == mHead.load(std::memory_order_acquire)) return false;
    out = mBuffer[t];
    mTail.store((t + 1) & (N - 1), std::memory_order_release);
    return true;
  }

private:
  T mBuffer[N];
  std::atomic<size_t> mHead;
  std::atomic<size_t> mTail;
};

} // namespace krkrz_webp_sink

class WebpXAudio2Sink : public IAudioSink, public IXAudio2VoiceCallback
{
public:
  WebpXAudio2Sink()
  : mXAudio2(nullptr), mMaster(nullptr), mSource(nullptr), mVolume(1.0f)
  {
    HRESULT hr = XAudio2Create(&mXAudio2, 0, XAUDIO2_DEFAULT_PROCESSOR);
    if (FAILED(hr)) {
      mXAudio2 = nullptr;
      return;
    }
    hr = mXAudio2->CreateMasteringVoice(&mMaster);
    if (FAILED(hr)) {
      mMaster = nullptr;
    }
  }

  ~WebpXAudio2Sink() override
  {
    if (mSource) {
      mSource->Stop();
      mSource->FlushSourceBuffers();
      mSource->DestroyVoice();
      mSource = nullptr;
    }
    if (mMaster) {
      mMaster->DestroyVoice();
      mMaster = nullptr;
    }
    if (mXAudio2) {
      mXAudio2->Release();
      mXAudio2 = nullptr;
    }
  }

  bool Setup(int channels, int sampleRate, int bitsPerSample,
             Encoding encoding) override
  {
    if (mSource) return false;
    if (!mXAudio2 || !mMaster) return false;

    WAVEFORMATEX fmt        = {};
    fmt.wFormatTag      = (encoding == PCM_F32) ? WAVE_FORMAT_IEEE_FLOAT
                                                : WAVE_FORMAT_PCM;
    fmt.nChannels       = (WORD)channels;
    fmt.nSamplesPerSec  = (DWORD)sampleRate;
    fmt.wBitsPerSample  = (WORD)bitsPerSample;
    fmt.nBlockAlign     = (fmt.nChannels * fmt.wBitsPerSample) / 8;
    fmt.nAvgBytesPerSec = fmt.nSamplesPerSec * fmt.nBlockAlign;
    fmt.cbSize          = 0;

    HRESULT hr = mXAudio2->CreateSourceVoice(&mSource, &fmt, 0,
                                             XAUDIO2_DEFAULT_FREQ_RATIO, this);
    if (FAILED(hr)) {
      mSource = nullptr;
      return false;
    }
    mSource->SetVolume(mVolume);
    return true;
  }

  void Enqueue(const void *data, size_t bytes, bool last,
               void *param) override
  {
    if (!mSource) return;

    // XAudio2 は SubmitSourceBuffer で渡したポインタを OnBufferEnd まで参照する。
    // この期間中に呼び出し側 (movie-player) が DecodedBuffer slot を再利用すると
    // 古いデータと新しいデータが混ざって聞こえる。これを回避するため audio data を
    // 内部コピーし、XAudio2 の寿命と decoder slot の寿命を切り離す。
    SubmitContext *ctx = new SubmitContext;
    ctx->data         = new uint8_t[bytes];
    ctx->originalParam = param;
    memcpy(ctx->data, data, bytes);

    XAUDIO2_BUFFER buf = {};
    buf.AudioBytes  = (UINT32)bytes;
    buf.pAudioData  = ctx->data;
    buf.pContext    = ctx;
    buf.Flags       = last ? XAUDIO2_END_OF_STREAM : 0;
    HRESULT hr = mSource->SubmitSourceBuffer(&buf);
    if (FAILED(hr)) {
      // submit 失敗時は ctx をそのまま consumed ring に流して looper 側で
      // 解放 + 通知させる (delete は looper 側固定で audio thread と整合)。
      if (!mConsumedRing.TryPush(ctx)) {
        // ring も満杯ならやむなくここで cleanup。
        delete[] ctx->data;
        delete ctx;
      }
    }
  }

  void Start() override
  {
    if (mSource) mSource->Start();
  }

  void Stop() override
  {
    if (mSource) mSource->Stop();
  }

  int64_t GetSamplesPlayed() const override
  {
    if (!mSource) return 0;
    XAUDIO2_VOICE_STATE state = {};
    mSource->GetState(&state);
    return (int64_t)state.SamplesPlayed;
  }

  bool TryPopConsumed(void **outParam) override
  {
    SubmitContext *ctx = nullptr;
    if (mConsumedRing.TryPop(ctx)) {
      if (ctx) {
        if (outParam) *outParam = ctx->originalParam;
        // 内部コピーバッファの解放はここ (looper thread) で行う。
        // audio thread (OnBufferEnd) で free すると heap lock で
        // 再生スレッドがブロックされるため。
        delete[] ctx->data;
        delete ctx;
      } else if (outParam) {
        *outParam = nullptr;
      }
      return true;
    }
    return false;
  }

  // FlushSourceBuffers は pending を破棄し、各 buffer の OnBufferEnd を
  // 発火させる (= consumed ring に積まれる)。movie-player の Flush 後の
  // DrainAudioSinkConsumed で release される。
  void Flush() override
  {
    if (!mSource) return;
    mSource->Stop();
    mSource->FlushSourceBuffers();
    mSource->Start();
  }

  void SetVolume(float volume) override
  {
    if (volume < 0.0f) volume = 0.0f;
    if (volume > 1.0f) volume = 1.0f;
    mVolume = volume;
    if (mSource) mSource->SetVolume(volume);
  }

  float Volume() const override { return mVolume; }

  // IXAudio2VoiceCallback overrides ----------------------------------------
  // (XAudio2 の audio thread から呼ばれる。重い処理は禁止)
  STDMETHODIMP_(void) OnVoiceProcessingPassStart(UINT32) override {}
  STDMETHODIMP_(void) OnVoiceProcessingPassEnd() override {}
  STDMETHODIMP_(void) OnStreamEnd() override {}
  STDMETHODIMP_(void) OnBufferStart(void *) override {}
  STDMETHODIMP_(void) OnBufferEnd(void *pBufferContext) override
  {
    // audio thread context。重い処理 (delete 含む heap lock) は禁止。
    // SubmitContext を lock-free ring にそのまま流して looper 側で開放する。
    SubmitContext *ctx = (SubmitContext *)pBufferContext;
    mConsumedRing.TryPush(ctx);
  }
  STDMETHODIMP_(void) OnLoopEnd(void *) override {}
  STDMETHODIMP_(void) OnVoiceError(void *, HRESULT) override {}

private:
  // SubmitSourceBuffer に渡す内部 wrapper。
  // data は memcpy 済みのコピー (sink 所有、OnBufferEnd で解放)。
  // originalParam は呼び出し側が Enqueue で渡した識別子で、
  // OnBufferEnd 時に consumed ring へそのまま流す。
  struct SubmitContext
  {
    uint8_t *data;
    void *originalParam;
  };

  IXAudio2 *mXAudio2;
  IXAudio2MasteringVoice *mMaster;
  IXAudio2SourceVoice *mSource;
  float mVolume;

  // consumed ring: capacity 64 (decoder buffer pool 数より十分大きい)
  // SubmitContext* を流し、TryPopConsumed (looper) 側で originalParam 抽出 + delete。
  krkrz_webp_sink::SPSCRing<SubmitContext *, 64> mConsumedRing;
};
