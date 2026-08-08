//---------------------------------------------------------------------------
/*
	TVP2 ( T Visual Presenter 2 )  A script authoring tool
	Copyright (C) 2000 W.Dee <dee@kikyou.info> and contributors

	See details of license at "license.txt"
*/
//---------------------------------------------------------------------------
// Wave Player interface
//---------------------------------------------------------------------------
#ifndef WaveIntfH
#define WaveIntfH

#include "tjsNative.h"
#include "SoundBufferBaseIntf.h"
#include "tjsUtils.h"


/*[*/
//---------------------------------------------------------------------------
// Sound Global Focus Mode
//---------------------------------------------------------------------------
enum tTVPSoundGlobalFocusMode
{
	/*0*/ sgfmNeverMute,			// never mutes
	/*1*/ sgfmMuteOnMinimize,		// will mute on the application minimize
	/*2*/ sgfmMuteOnDeactivate		// will mute on the application deactivation
};
//---------------------------------------------------------------------------



/*]*/
//---------------------------------------------------------------------------
// GUID identifying WAVEFORMATEXTENSIBLE sub format
//---------------------------------------------------------------------------
extern tjs_uint8 TVP_GUID_KSDATAFORMAT_SUBTYPE_PCM[16];
extern tjs_uint8 TVP_GUID_KSDATAFORMAT_SUBTYPE_IEEE_FLOAT[16];
//---------------------------------------------------------------------------


/*[*/
//---------------------------------------------------------------------------
// PCM data format (internal use)
//---------------------------------------------------------------------------
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
//---------------------------------------------------------------------------



/*]*/
//---------------------------------------------------------------------------
// PCM bit depth converter
//---------------------------------------------------------------------------
TJS_EXP_FUNC_DEF(void, TVPConvertPCMTo16bits, (tjs_int16 *output, const void *input, const tTVPWaveFormat &format, tjs_int count, bool downmix));
TJS_EXP_FUNC_DEF(void, TVPConvertPCMTo16bits, (tjs_int16 *output, const void *input, tjs_int channels, tjs_int bytespersample, tjs_int bitspersample, bool isfloat, tjs_int count, bool downmix));
TJS_EXP_FUNC_DEF(void, TVPConvertPCMToFloat, (float *output, const void *input, tjs_int channels, tjs_int bytespersample, tjs_int bitspersample, bool isfloat, tjs_int count));
TJS_EXP_FUNC_DEF(void, TVPConvertPCMToFloat, (float *output, const void *input, const tTVPWaveFormat &format, tjs_int count));
//---------------------------------------------------------------------------



/*[*/
//---------------------------------------------------------------------------
// tTVPWaveDecoder interface
//---------------------------------------------------------------------------
class tTVPWaveDecoder
{
public:
	virtual ~tTVPWaveDecoder() {};

	virtual void GetFormat(tTVPWaveFormat & format) = 0;
		/* Retrieve PCM format, etc. */

	virtual bool Render(void *buf, tjs_uint bufsamplelen, tjs_uint& rendered) = 0;
		/*
			Render PCM from current position.
			where "buf" is a destination buffer, "bufsamplelen" is the buffer's
			length in sample granule, "rendered" is to be an actual number of
			written sample granule.
			returns whether the decoding is to be continued.
			because "redered" can be lesser than "bufsamplelen", the player
			should not end until the returned value becomes false.
		*/

	virtual bool SetPosition(tjs_uint64 samplepos) = 0;
		/*
			Seek to "samplepos". "samplepos" must be given in unit of sample granule.
			returns whether the seeking is succeeded.
		*/
};
//---------------------------------------------------------------------------
class tTVPWaveDecoderCreator
{
public:
	virtual tTVPWaveDecoder * Create(const ttstr & storagename,
		const ttstr &extension) = 0;
		/*
			Create tTVPWaveDecoder instance. returns NULL if failed.
		*/
};
//---------------------------------------------------------------------------
/*]*/




//---------------------------------------------------------------------------
// tTVPWaveDecoder interface management
//---------------------------------------------------------------------------
TJS_EXP_FUNC_DEF(void, TVPRegisterWaveDecoderCreator, (tTVPWaveDecoderCreator *d));
TJS_EXP_FUNC_DEF(void, TVPUnregisterWaveDecoderCreator, (tTVPWaveDecoderCreator *d));
TJS_EXP_FUNC_DEF(tTVPWaveDecoder *, TVPCreateWaveDecoder, (const ttstr & storagename));

//---------------------------------------------------------------------------
// 曲別ゲイン取得コールバック (WaveSoundBuffer.setGainQueryCallback で登録)。
// デコーダが Create 時 (= スクリプトの WaveSoundBuffer.open() 呼び出し =
// メインスレッド) に storagename(URL) を渡して呼び、適用する追加ゲイン(dB)を
// 得る。コールバック未登録なら 0dB。TJS closure を呼ぶためメインスレッド限定。
//---------------------------------------------------------------------------
TJS_EXP_FUNC_DEF(float, TVPQueryUserSoundGainDB, (const ttstr & storagename));
//---------------------------------------------------------------------------


//---------------------------------------------------------------------------
// interface for basic filter management
//---------------------------------------------------------------------------
class tTVPSampleAndLabelSource;
class iTVPBasicWaveFilter
{
public:
	// recreate filter. filter will remain owned by the each filter instance.
	virtual tTVPSampleAndLabelSource * Recreate(tTVPSampleAndLabelSource * source) = 0;
	virtual void Clear(void) = 0;
	virtual void Update(void) = 0;
	virtual void Reset(void) = 0;
};
//---------------------------------------------------------------------------



//---------------------------------------------------------------------------
// tTJSNI_BaseWaveSoundBuffer
//---------------------------------------------------------------------------
class tTVPWaveLoopManager;
class tTJSNI_BaseWaveSoundBuffer : public tTJSNI_SoundBuffer
{
	typedef tTJSNI_SoundBuffer inherited;

	iTJSDispatch2 * WaveFlagsObject;
	iTJSDispatch2 * WaveLabelsObject;

	struct tFilterObjectAndInterface
	{
		tTJSVariant Filter; // filter object
		iTVPBasicWaveFilter * Interface; // filter interface
		tFilterObjectAndInterface(
			const tTJSVariant & filter,
			iTVPBasicWaveFilter * interf) :
			Filter(filter), Interface(interf) {;}
	};
	std::vector<tFilterObjectAndInterface> FilterInterfaces; // backupped filter interface array

protected:
	tTVPWaveLoopManager * LoopManager; // will be set by tTJSNI_WaveSoundBuffer
	tTVPSampleAndLabelSource * FilterOutput; // filter output
	iTJSDispatch2 * Filters; // wave filters array (TJS2 array object)
public:
	tTJSNI_BaseWaveSoundBuffer();
	tjs_error TJS_INTF_METHOD
	Construct(tjs_int numparams, tTJSVariant **param,
		iTJSDispatch2 *tjs_obj);
	void TJS_INTF_METHOD Invalidate();

protected:
	void InvokeLabelEvent(const ttstr & name);
	void RecreateWaveLabelsObject();
	void RebuildFilterChain();
	void ClearFilterChain();
	void ResetFilterChain();
	void UpdateFilterChain();

public:
	iTJSDispatch2 * GetWaveFlagsObjectNoAddRef();
	iTJSDispatch2 * GetWaveLabelsObjectNoAddRef();
	tTVPWaveLoopManager * GetWaveLoopManager() const { return LoopManager; }
	iTJSDispatch2 * GetFiltersNoAddRef() { return Filters; }

	// method
	virtual void Open(const ttstr & storagename) = 0;
	virtual void Play() = 0;
	virtual void Stop() = 0;
	virtual void SetPos(float x, float y, float z) = 0;

	// properties
    virtual tjs_uint64 GetPosition() = 0;
	virtual void SetPosition(tjs_uint64 pos) = 0;
    virtual tjs_uint64 GetSamplePosition() = 0;
	virtual void SetSamplePosition(tjs_uint64 pos) = 0;
	virtual bool GetPaused() const = 0;
	virtual void SetPaused(bool b) = 0;
	virtual tjs_uint64 GetTotalTime() = 0;
	virtual void SetLooping(bool b) = 0;
	virtual bool GetLooping() const = 0;
	virtual void SetVolume(tjs_int v) = 0;
	virtual tjs_int GetVolume() const = 0;
	virtual void SetVolume2(tjs_int v) = 0;
	virtual tjs_int GetVolume2() const = 0;
	virtual void SetPan(tjs_int v) = 0;
	virtual tjs_int GetPan() const = 0;
	virtual void SetPosX(float v) = 0;
	virtual float GetPosX() const = 0;
	virtual void SetPosY(float v) = 0;
	virtual float GetPosY() const = 0;
	virtual void SetPosZ(float v) = 0;
	virtual float GetPosZ() const = 0;
	// -- 3D 定位 (miniaudio spatializer)。use3D=true の時のみ有効化 (既定 false=回帰なし)。--
	virtual void SetUse3D(bool b) = 0;
	virtual bool GetUse3D() const = 0;
	virtual void Set3DVelocity(float x, float y, float z) = 0;      // ドップラー用
	virtual void Set3DConeDirection(float x, float y, float z) = 0; // コーンの向き
	virtual void Set3DCone(float innerAngleRad, float outerAngleRad, float outerGain) = 0;
	virtual void SetMinDistance(float v) = 0;
	virtual float GetMinDistance() const = 0;
	virtual void SetMaxDistance(float v) = 0;
	virtual float GetMaxDistance() const = 0;
	virtual void SetRolloffFactor(float v) = 0;
	virtual float GetRolloffFactor() const = 0;
	virtual void SetDopplerFactor(float v) = 0;
	virtual float GetDopplerFactor() const = 0;
	virtual void SetAttenuationModel(tjs_int m) = 0; // 0=none/1=inverse/2=linear/3=exponential
	virtual tjs_int GetAttenuationModel() const = 0;
	virtual tjs_int GetFrequency() const = 0;
	virtual void SetFrequency(tjs_int freq) = 0;
	virtual tjs_int GetBitsPerSample() const = 0;
	virtual tjs_int GetChannels() const = 0;
};
//---------------------------------------------------------------------------





//---------------------------------------------------------------------------
// tTJSNC_WaveSoundBuffer : TJS WaveSoundBuffer class
//---------------------------------------------------------------------------
class tTJSNC_WaveSoundBuffer : public tTJSNativeClass
{
public:
	tTJSNC_WaveSoundBuffer();
	static tjs_uint32 ClassID;

	typedef tTJSNativeInstance *(*FuncCreateNativeInstance)();
	FuncCreateNativeInstance Factory;
protected:
	tTJSNativeInstance *CreateNativeInstance() { return Factory(); }
};
//---------------------------------------------------------------------------
extern tTJSNativeClass * TVPCreateNativeClass_SoundBuffer();
//---------------------------------------------------------------------------



//---------------------------------------------------------------------------
// tTJSNC_SoundListener : TJS SoundListener class
//   3D 定位のリスナ (聴取者)。engine グローバル (listener index 0) を操作する
//   インスタンス不要の名前空間的クラス (System と同様)。
//---------------------------------------------------------------------------
class tTJSNC_SoundListener : public tTJSNativeClass
{
public:
	tTJSNC_SoundListener();
	static tjs_uint32 ClassID;
protected:
	tTJSNativeInstance *CreateNativeInstance() { return NULL; } // 実体を持たない
};
//---------------------------------------------------------------------------
extern tTJSNativeClass * TVPCreateNativeClass_SoundListener();
//---------------------------------------------------------------------------






//---------------------------------------------------------------------------
// tTJSNI_WaveFlags : Wave Flags object
//---------------------------------------------------------------------------
class tTJSNI_WaveFlags : public tTJSNativeInstance
{
	typedef tTJSNativeInstance inherited;

	tTJSNI_BaseWaveSoundBuffer * Buffer;

public:
	tTJSNI_WaveFlags();
	~tTJSNI_WaveFlags();
	tjs_error TJS_INTF_METHOD Construct(tjs_int numparams, tTJSVariant **param,
		iTJSDispatch2 *tjs_obj);
	void TJS_INTF_METHOD Invalidate();

	tTJSNI_BaseWaveSoundBuffer * GetBuffer() const { return Buffer; }
};
//---------------------------------------------------------------------------



//---------------------------------------------------------------------------
// tTJSNC_WaveFlags : Wave Flags class
//---------------------------------------------------------------------------
class tTJSNC_WaveFlags : public tTJSNativeClass
{
public:
	tTJSNC_WaveFlags();
	static tjs_uint32 ClassID;

protected:
	tTJSNativeInstance *CreateNativeInstance() { return new tTJSNI_WaveFlags(); }
};
//---------------------------------------------------------------------------
iTJSDispatch2 * TVPCreateWaveFlagsObject(iTJSDispatch2 * buffer);
//---------------------------------------------------------------------------






#endif
