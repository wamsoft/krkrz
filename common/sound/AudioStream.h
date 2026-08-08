
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

	// -- 3D 定位 (spatialization)。既定 no-op = 未対応ストリーム / 非空間化。--
	//    3D は SetSpatializationEnabled(true) の時のみ効く (既定は無効 = 従来どおり
	//    非空間化パススルーで回帰なし)。座標系は miniaudio 準拠 (右手系・Y up・任意単位)。
	virtual void SetSpatializationEnabled(bool enabled) {}
	virtual void Set3DPosition(float x, float y, float z) {}      // 音源のワールド座標
	virtual void Set3DVelocity(float x, float y, float z) {}      // 速度 (ドップラー用)
	virtual void Set3DConeDirection(float x, float y, float z) {} // コーンの向き
	virtual void Set3DCone(float innerAngleRad, float outerAngleRad, float outerGain) {}
	virtual void Set3DMinDistance(float d) {}                     // これ以内は減衰なし
	virtual void Set3DMaxDistance(float d) {}                     // これ以遠は減衰頭打ち
	virtual void Set3DRolloff(float rolloff) {}                   // 減衰の強さ
	virtual void Set3DDopplerFactor(float factor) {}             // ドップラー強度 (0=無効)
	// 減衰モデル (ma_attenuation_model と同値: 0=none/1=inverse/2=linear/3=exponential)
	virtual void Set3DAttenuationModel(int model) {}
};

extern iTVPAudioStream* TVPCreateAudioStream(tTVPAudioStreamParam& param);

// -- 3D 定位のリスナ (聴取者)。engine グローバル (listener index 0) を操作する。--
//    座標系は音源側 (iTVPAudioStream::Set3DPosition 等) と共通 (miniaudio 準拠)。
extern void TVPSetSoundListenerEnabled(bool enabled);
extern void TVPSetSoundListenerPosition(float x, float y, float z);
extern void TVPSetSoundListenerDirection(float x, float y, float z); // 前方向
extern void TVPSetSoundListenerWorldUp(float x, float y, float z);   // 上方向 (既定 0,1,0)
extern void TVPSetSoundListenerVelocity(float x, float y, float z);  // 速度 (ドップラー用)
extern void TVPSetSoundListenerCone(float innerAngleRad, float outerAngleRad, float outerGain);

// オーディオデバイス (miniaudio engine) を起動時に先行初期化する。
// 初回サウンド再生時のデバイスオープン遅延 (音の頭切れ) を防ぐ目的。
// -wspreinit=no で無効化可 (既定 ON)。詳細は AudioStream.cpp を参照。
extern void TVPPreInitAudioDevice();

#endif // _AUDIO_DEVICE_H__
