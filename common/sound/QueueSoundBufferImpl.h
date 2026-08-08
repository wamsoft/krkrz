//---------------------------------------------------------------------------
/*
	TVP2 ( T Visual Presenter 2 )  A script authoring tool
	Copyright (C) 2000 W.Dee <dee@kikyou.info> and contributors

	See details of license at "license.txt"
*/
//---------------------------------------------------------------------------
// Wave Player implementation
//---------------------------------------------------------------------------
#ifndef PortAudioImplH
#define PortAudioImplH


#include "WaveIntf.h"
#include "WaveLoopManager.h"
#include "AudioStream.h"

//---------------------------------------------------------------------------
// Constants
//---------------------------------------------------------------------------
//#define TVP_TIMEOFS_INVALID_VALUE ((tjs_int)(- 2147483648i64)) // invalid value for 32bit time offset
#define TVP_TIMEOFS_INVALID_VALUE ((tjs_int)(0x80000000)) // invalid value for 32bit time offset

//---------------------------------------------------------------------------
/*
struct WaveFormat
{
    int8_t       RIFF[4];
    uint32_t     TotalSize;
    int8_t       Fmt[8];
    uint32_t     FmtSize;
    uint16_t     Format;
    uint16_t     Channel;
    uint32_t     Rate;
    uint32_t     AvgByte;
    uint16_t     Block;
    uint16_t     BitPerSample;
    int8_t       Data[4];
    uint32_t     DataSize;
};
struct tTVPWaveFormat
{
	tjs_uint SamplesPerSec; // sample granule per sec
	tjs_uint Channels;
	tjs_uint BitsPerSample; // per one sample
	tjs_uint BytesPerSample; // per one sample
	tjs_uint64 TotalSamples; // in sample granule; unknown for zero
	tjs_uint64 TotalTime; // in ms; unknown for zero
	tjs_uint32 SpeakerConfig; // bitwise OR of SPEAKER_* constants
	bool IsFloat; // true if the data is IEEE floating point
	bool Seekable;
};
*/
//---------------------------------------------------------------------------
// tTJSNI_PortAudioSoundBuffer : Wave Native Instance
//---------------------------------------------------------------------------
class tTVPWaveLoopManager;
class tTJSNI_QueueSoundBuffer : public tTJSNI_BaseWaveSoundBuffer
{
	typedef  tTJSNI_BaseWaveSoundBuffer inherited;
	tTJSCriticalSection BufferCS;

	tTVPWaveDecoder * Decoder;
	class tTVPSoundDecodeThread * Thread;

	// for sound player
	iTVPAudioStream *Stream;
	std::vector<class tTVPSoundSamplesBuffer*> Samples;

	bool IsPlaying() { return Stream && Stream->IsPlaying(); }

	tjs_int64 GetCurrentPlayingPosition();

	bool Paused;

	tTVPWaveFormat InputFormat;
	bool Looping;

	std::vector<tTVPWaveLabel> LabelEventQueue;

	bool BufferPlaying;	// decode threadが走って、queueに入れていってる状態
	bool UseVisBuffer;

	// double buffering
	static const tjs_uint BufferCount = 2;
	class tTVPSoundSamplesBuffer* Buffer[BufferCount];

	tjs_uint BufferSize;

	tjs_int64 LastCheckedDecodePos; // last sured position (-1 for not checked) and 
	tjs_uint64 LastCheckedTick; // last sured tick time

	tjs_int Volume;
	tjs_int Volume2;
	tjs_int Frequency;
	static tjs_int GlobalVolume;
	static tTVPSoundGlobalFocusMode GlobalFocusMode;
	tjs_int Pan; // -100000 .. 0 .. 100000

	// -- 3D 定位パラメータ (キャッシュ。Stream 生成後に Set3DParamsToStream で適用) --
	bool  Use3D;            // 3D 有効 (既定 false=非空間化=回帰なし)
	float PosX, PosY, PosZ; // 音源ワールド座標
	float VelX, VelY, VelZ; // 速度 (ドップラー)
	float ConeDirX, ConeDirY, ConeDirZ;         // コーンの向き
	float ConeInnerRad, ConeOuterRad, ConeOuterGain; // コーン
	float MinDistance, MaxDistance;             // 距離減衰の範囲
	float RolloffFactor, DopplerFactor;         // 減衰の強さ / ドップラー強度
	tjs_int AttenuationModel;                    // 0=none/1=inverse/2=linear/3=exponential

	void ResetSamplePositions();
	void Clear();

	void StartPlay();
	void StopPlay();

	void ResetLastCheckedDecodePos();
public:
	bool ThreadCallbackEnabled;

	tTJSNI_QueueSoundBuffer();
	tjs_error TJS_INTF_METHOD Construct(tjs_int numparams, tTJSVariant **param,
		iTJSDispatch2 *tjs_obj) override;
	void TJS_INTF_METHOD Invalidate() override;

	void DestroySoundBuffer();
	void ReleaseSoundBuffer( bool disableevent = true );

	tTJSCriticalSection & GetBufferCS() { return BufferCS; }

	void PushPlaySample( class tTVPSoundSamplesBuffer* buffer );
	void ReleasePlayedSample( class tTVPSoundSamplesBuffer* buffer);
	// audio thread が consumed ring に積んだ完了 buffer をすべて引き取る (decode thread から呼ばれる)
	void DrainConsumedBuffers();

	tjs_int FireLabelEventsAndGetNearestLabelEventStep(tjs_int64 tick);
	tjs_int GetNearestEventStep();
	void FlushAllLabelEvents();

//	bool DoDecode(); // for tTVPSoundDecodeThread
	void Update();	// for tTVPSoundEventThread(FillBuffer)

	// デコードスレッドを必要になったとき(初回 Open 時)に遅延生成する。
	// 一度生成したら Invalidate まで生かし続ける。
	void EnsureDecodeThread();

	tjs_uint Decode( void *buffer, tjs_uint bufsamplelen, tTVPWaveSegmentQueue & segments );

	virtual void Open(const ttstr & storagename) override;
	virtual void Play() override;
	virtual void Stop() override;

	virtual bool GetPaused() const override;
	virtual void SetPaused(bool b) override;

	virtual tjs_int GetBitsPerSample() const override { return InputFormat.BitsPerSample; }
	virtual tjs_int GetChannels() const override { return InputFormat.Channels; }

	virtual void SetLooping(bool b) override;
	virtual bool GetLooping() const override { return Looping; }

    virtual tjs_uint64 GetSamplePosition() override;
	virtual void SetSamplePosition(tjs_uint64 pos) override;

    virtual tjs_uint64 GetPosition() override;
	virtual void SetPosition(tjs_uint64 pos) override;

	virtual tjs_uint64 GetTotalTime() override;

	virtual void SetVolume(tjs_int v) override;
	virtual tjs_int GetVolume() const override { return Volume; }
	virtual void SetVolume2(tjs_int v) override;
	virtual tjs_int GetVolume2() const override { return Volume2; }
	virtual void SetPan(tjs_int v) override;
	virtual tjs_int GetPan() const override { return Pan; }

	// -- 3D 定位 (miniaudio spatializer)。パラメータは TJS インスタンス側にキャッシュし、
	//    Stream (ma_sound) 生成後に Set3DParamsToStream() で再適用する (SetVolumeToStream 同様)。
	virtual void SetPos(float x, float y, float z) override;
	virtual void SetPosX(float v) override;
	virtual float GetPosX() const override { return PosX; }
	virtual void SetPosY(float v) override;
	virtual float GetPosY() const override { return PosY; }
	virtual void SetPosZ(float v) override;
	virtual float GetPosZ() const override { return PosZ; }
	virtual void SetUse3D(bool b) override;
	virtual bool GetUse3D() const override { return Use3D; }
	virtual void Set3DVelocity(float x, float y, float z) override;
	virtual void Set3DConeDirection(float x, float y, float z) override;
	virtual void Set3DCone(float innerAngleRad, float outerAngleRad, float outerGain) override;
	virtual void SetMinDistance(float v) override;
	virtual float GetMinDistance() const override { return MinDistance; }
	virtual void SetMaxDistance(float v) override;
	virtual float GetMaxDistance() const override { return MaxDistance; }
	virtual void SetRolloffFactor(float v) override;
	virtual float GetRolloffFactor() const override { return RolloffFactor; }
	virtual void SetDopplerFactor(float v) override;
	virtual float GetDopplerFactor() const override { return DopplerFactor; }
	virtual void SetAttenuationModel(tjs_int m) override;
	virtual tjs_int GetAttenuationModel() const override { return AttenuationModel; }

	virtual tjs_int GetFrequency() const override { return Frequency; }
	virtual void SetFrequency(tjs_int freq) override;


	void SetVolumeToStream();
	void SetFrequencyToStream();
	void Set3DParamsToStream(); // キャッシュ済み 3D 状態を Stream へ適用 (Stream 生成後に呼ぶ)

	static void SetGlobalVolume(tjs_int v);
	static tjs_int GetGlobalVolume() { return GlobalVolume; }
	static void SetGlobalFocusMode(tTVPSoundGlobalFocusMode b);
	static tTVPSoundGlobalFocusMode GetGlobalFocusMode() { return sgfmNeverMute; }

	//-- visualization stuff ----------------------------------------------
	void SetUseVisBuffer(bool b);
	bool GetUseVisBuffer() const { return UseVisBuffer; }

	tjs_int GetVisBuffer(tjs_int16 *dest, tjs_int numsamples, tjs_int channels, tjs_int aheadsamples);

	//-- lip-sync / 解析 (getVisBuffer の再生位置同期を流用) --------------
	// いずれも要 useVisBuffer (未設定なら自動有効化し、その回は false/0 を返す)。
	// 再生カーソル付近 windowsamples の RMS / ピーク (0..1) を返す。
	bool GetSoundLevel(float &rms, float &peak, tjs_int windowsamples, tjs_int aheadsamples);
	// ログ配置 numbands バンドのスペクトルエネルギーを bands[] に書き込む。戻り=書込数。
	tjs_int GetSoundSpectrum(float *bands, tjs_int numbands, tjs_int aheadsamples);
	// 日本語 5 母音 (a,i,u,e,o) の推定重みを weights[5] に書き込む。無音時 false。
	bool GetVowel(float *weights, tjs_int aheadsamples);

protected:
	virtual void TimerBeatHandler() override; // tTJSNI_BaseSoundBuffer::TimerBeatHandler

	void ResetVisBuffer(); // reset or recreate visualication buffer
	void DeallocateVisBuffer();

	void CopyVisBuffer(tjs_int16 *dest, const tjs_uint8 *src, tjs_int numsamples, tjs_int channels);

	//-- lip-sync 解析用 FFT ワーク (遅延確保、std::vector で自動解放) ------
	std::vector<float> AnalyzeWork;    // FFT 入出力 (fftsize)
	std::vector<int>   AnalyzeIp;      // rdft ビット反転テーブル
	std::vector<float> AnalyzeW;       // rdft cos/sin テーブル (fftsize/2)
	std::vector<float> AnalyzeWindow;  // Hann 窓 (fftsize)
	// 再生カーソル付近を Hann 窓掛け FFT し、半スペクトル振幅を mag[fftsize/2] に返す。
	bool ComputeMagnitude(float *mag, tjs_int fftsize, tjs_int aheadsamples);
};
//---------------------------------------------------------------------------

#endif
