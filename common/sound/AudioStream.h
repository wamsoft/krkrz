
#ifndef _AUDIO_DEVICE_H__
#define _AUDIO_DEVICE_H__

enum TVPAudioSampleType {
	astUInt8,
	astInt16,
	astInt24,
	astInt32,
	astFloat32,
};

struct tTVPAudioStreamParam {
	tjs_uint32 Channels;		// チャンネル数
	tjs_uint32 SampleRate;		// サンプリングレート
	tjs_uint32 BitsPerSample;	// サンプル当たりのビット数
	TVPAudioSampleType SampleType;	// サンプルの形式
};

// 再生済みバッファを通知する経路は SetCallback ではなく
// SetWakeupHandler + TryPopConsumed のポーリングに変更されている。
// audio callback スレッドが BufferCS / OneLoopCS / Event.Set 等の
// 非自明な処理を行わないようにするためのもの。
//   - audio thread は ReadData 内で完了 buffer の param を Consumed ring に push し
//     wakeup handler を 1 回呼ぶだけ
//   - 呼び出し側 (decode thread) はその wakeup を受けて TryPopConsumed で排出する
typedef void (*StreamWakeupHandler)(void* userData);

class iTVPAudioStream
{
public:
	virtual ~iTVPAudioStream(){}

	// audio thread から完了通知を呼び出し側のスレッドへ渡すフック
	// (handler は audio callback コンテキストで呼ばれるので、内部では Event.Set 等の
	//  最低限の wakeup のみを行うこと)
	virtual void SetWakeupHandler( StreamWakeupHandler handler, void* user ) = 0;

	// 再生用データの投入 (1 producer 想定)
	virtual void Enqueue( void *data, size_t size, bool last, void *param ) = 0;

	// 再生済み buffer の param を 1 件取り出す。なければ false。
	// 1 consumer 想定 (典型的には decode thread)
	virtual bool TryPopConsumed( void*& outParam ) = 0;

	// pending を全て破棄して consumed 側へ移す
	// (再生停止時の cleanup 用。audio thread が停止していない状態で呼んではいけない)
	virtual void ClearQueue() = 0;

	virtual void StartStream() = 0;
	virtual void StopStream() = 0;

	virtual bool IsPlaying() const = 0; // 再生中
	virtual bool AtEnd() const = 0;     // 再生終了済み

	virtual tjs_uint64 GetSamplesPlayed() const = 0;

	virtual void SetVolume(tjs_int vol) = 0;
	virtual tjs_int GetVolume() const = 0;
	virtual void SetPan(tjs_int pan) = 0;
	virtual tjs_int GetPan() const = 0;
	virtual void SetFrequency(tjs_int freq) = 0;
	virtual tjs_int GetFrequency() const = 0;
};

extern iTVPAudioStream* TVPCreateAudioStream(tTVPAudioStreamParam& param);

#endif // _AUDIO_DEVICE_H__
