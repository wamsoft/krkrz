//---------------------------------------------------------------------------
/*
	TVP2 ( T Visual Presenter 2 )  A script authoring tool

	See details of license at "license.txt"
*/
//---------------------------------------------------------------------------
// Wave Player implementation
//---------------------------------------------------------------------------
#include "tjsCommHead.h"

//#include "SystemControl.h"
#include "DebugIntf.h"
#include "MsgIntf.h"
#include "StorageIntf.h"
#include "WaveIntf.h"
#include "QueueSoundBufferImpl.h"
#include "PluginImpl.h"
#include "SysInitIntf.h"
#include "ThreadIntf.h"
#include "RealFFT.h"
#include "tjsDictionary.h"   // TJSCreateDictionaryObject
#include "tjsArray.h"        // TJSCreateArrayObject
#include <math.h>
#include <vector>
#include "Random.h"
#include "UtilStreams.h"
#include "TickCount.h"
#include "TVPTimer.h"
#include "Application.h"
#include "UserEvent.h"
#include "LogIntf.h"

#include "SoundEventThread.h"
#include "SoundDecodeThread.h"
#include <algorithm>
#include "SoundSamples.h"

//---------------------------------------------------------------------------
// static function for TJS WaveSoundBuffer class
//---------------------------------------------------------------------------
void TVPQueueSoundSetGlobalVolume(tjs_int v) {
    tTJSNI_QueueSoundBuffer::SetGlobalVolume(v);
}
tjs_int TVPQueueSoundGetGlobalVolume() {
    return tTJSNI_QueueSoundBuffer::GetGlobalVolume();
}
void TVPQueueSoundSetGlobalFocusMode(tTVPSoundGlobalFocusMode b) {
    tTJSNI_QueueSoundBuffer::SetGlobalFocusMode(b);
}
tTVPSoundGlobalFocusMode TVPQueueSoundGetGlobalFocusMode() {
    return tTJSNI_QueueSoundBuffer::GetGlobalFocusMode();
}

//---------------------------------------------------------------------------
//---------------------------------------------------------------------------


//---------------------------------------------------------------------------
// Buffer management
//---------------------------------------------------------------------------
static tTVPSoundBuffers TVPSoundBuffers;

static void TVPShutdownSoundBuffers() {
	TVPSoundBuffers.Shutdown();
}
static tTVPAtExit TVPShutdownWaveSoundBuffersAtExit( TVP_ATEXIT_PRI_PREPARE, TVPShutdownSoundBuffers );
//---------------------------------------------------------------------------
//---------------------------------------------------------------------------

//---------------------------------------------------------------------------
// tTJSNI_QueueSoundBuffer
//---------------------------------------------------------------------------

// miniaudio.cpp
extern tjs_int TVPSoundFrequency;

tTVPSoundGlobalFocusMode TVPSoundGlobalFocusModeByOption = sgfmNeverMute;
tjs_int TVPSoundGlobalFocusMuteVolume = 0;
tjs_int tTJSNI_QueueSoundBuffer::GlobalVolume = 100000;
tTVPSoundGlobalFocusMode tTJSNI_QueueSoundBuffer::GlobalFocusMode = sgfmNeverMute;

//---------------------------------------------------------------------------
// Options management
//---------------------------------------------------------------------------
static bool TVPSoundOptionsInit = false;

void TVPInitSoundOptions()
{
	if (TVPSoundOptionsInit) return;

	tTJSVariant val;
	if(TVPGetCommandLine(TJS_W("-wsfreq"), &val)) {
		TVPSoundFrequency = val;
	}

	TVPSoundOptionsInit = true;
}

//---------------------------------------------------------------------------
tTJSNI_QueueSoundBuffer::tTJSNI_QueueSoundBuffer() : Paused(false)
{
	TVPInitSoundOptions();
	Stream = nullptr;
	Decoder = nullptr;
	LoopManager = nullptr;
	Thread = nullptr;
	UseVisBuffer = false;
	ThreadCallbackEnabled = false;
	Volume = 100000;
	Volume2 = 100000;
	Pan = 0;
	// 3D 定位 (既定は無効=非空間化。miniaudio の既定値に合わせる)
	Use3D = false;
	PosX = PosY = PosZ = 0.0f;
	VelX = VelY = VelZ = 0.0f;
	ConeDirX = 0.0f; ConeDirY = 0.0f; ConeDirZ = -1.0f; // miniaudio 既定の前方向
	ConeInnerRad = 6.283185f; ConeOuterRad = 6.283185f; ConeOuterGain = 0.0f; // 2π=全方位(コーン無効相当)
	MinDistance = 1.0f;
	MaxDistance = 3.402823466e+38f; // FLT_MAX (miniaudio 既定)
	RolloffFactor = 1.0f;
	DopplerFactor = 1.0f;
	AttenuationModel = 1; // inverse (miniaudio 既定)
	for( tjs_uint i = 0; i < BufferCount; i++ ) {
		Buffer[i] = nullptr;
	}

	TVPSoundBuffers.AddBuffer( this );
	// デコードスレッドは初回 Open 時まで生成しない (EnsureDecodeThread)。
	// 多数の SoundBuffer を確保するだけで未使用スレッドが大量常駐するのを避けるため。

	memset( &InputFormat, 0, sizeof( InputFormat ) );
	Looping = false;
	BufferPlaying = false;
	LastCheckedDecodePos = -1;
	LastCheckedTick = 0;
}
//---------------------------------------------------------------------------
tjs_error TJS_INTF_METHOD
tTJSNI_QueueSoundBuffer::Construct(tjs_int numparams, tTJSVariant **param,
		iTJSDispatch2 *tjs_obj)
{
	tjs_error hr = inherited::Construct(numparams, param, tjs_obj);
	if(TJS_FAILED(hr)) return hr;

	return TJS_S_OK;
}
//---------------------------------------------------------------------------
void tTJSNI_QueueSoundBuffer::EnsureDecodeThread()
{
	// 初回 Open 時に呼ばれ、まだ無ければデコードスレッドを生成する。
	// メインスレッド (Open 経路) からのみ呼ばれるので生成競合は起きない。
	if( Thread ) return;
	Thread = new tTVPSoundDecodeThread( this );
#ifdef KRKRZ_CPU_CORE_AUDIO
	Thread->SetProcessorNo(KRKRZ_CPU_CORE_AUDIO);
#endif
}
//---------------------------------------------------------------------------
void TJS_INTF_METHOD tTJSNI_QueueSoundBuffer::Invalidate()
{
	inherited::Invalidate();

	Clear();

	DestroySoundBuffer();

	if( Thread ) delete Thread, Thread = nullptr;

	TVPSoundBuffers.RemoveBuffer( this );
}
//---------------------------------------------------------------------------
void tTJSNI_QueueSoundBuffer::ReleaseSoundBuffer( bool disableevent ) {
	// called at exit ( system uninitialization )
	bool b = CanDeliverEvents;
	if( disableevent )
		CanDeliverEvents = false; // temporarily disables event derivering
	Stop();
	DestroySoundBuffer();
	CanDeliverEvents = b;
}
//---------------------------------------------------------------------------
void tTJSNI_QueueSoundBuffer::DestroySoundBuffer() {

	BufferPlaying = false;

	LabelEventQueue.clear();

	for( tjs_uint i = 0; i < BufferCount; i++ ) {
		if( Buffer[i] ) delete Buffer[i];
		Buffer[i] = nullptr;
	}
	Samples.clear();
}
//---------------------------------------------------------------------------
tjs_int64 tTJSNI_QueueSoundBuffer::GetCurrentPlayingPosition() {
	tjs_int64 result = -1;
	if( Stream ) {
		tTJSCriticalSectionHolder holder(BufferCS);
		tjs_uint64 pos = Stream->GetSamplesPlayed();
		if( Samples.size() > 0 ) {
			auto itr = Samples.begin();
			tTVPSoundSamplesBuffer* sample = *itr;
			tjs_uint count = sample->GetSamplesCount();
			tjs_int offset = (tjs_int)( pos % count );
			result = sample->GetDecodePosition() + offset;
		}
	}
	return result;
}
//---------------------------------------------------------------------------
void tTJSNI_QueueSoundBuffer::ResetSamplePositions() {
	for( tjs_uint i = 0; i < BufferCount; i++ ) {
		if( Buffer[i] ) Buffer[i]->Reset();
	}
	LabelEventQueue.clear();
}
//---------------------------------------------------------------------------
void tTJSNI_QueueSoundBuffer::Clear()
{
	// clear all status and unload current decoder
	Stop();
	ThreadCallbackEnabled = false;
	TVPSoundBuffers.CheckAllSleep();
	if(Thread) Thread->Interrupt();
	if(LoopManager) delete LoopManager, LoopManager = nullptr;
	ClearFilterChain();
	if(Decoder) delete Decoder, Decoder = nullptr;
	BufferPlaying = false;

	Paused = false;

	ResetSamplePositions();

	SetStatus(ssUnload);
}
//---------------------------------------------------------------------------
tjs_uint tTJSNI_QueueSoundBuffer::Decode( void *buffer, tjs_uint bufsamplelen, tTVPWaveSegmentQueue & segments ) {
	// decode one buffer unit
	tjs_uint w = 0;
	try {
		// decode
		if( FilterOutput ) FilterOutput->Decode( (tjs_uint8*)buffer, bufsamplelen, w, segments );
	} catch( ... ) {
		// ignore errors
		w = 0;
	}
	return w;
}
//---------------------------------------------------------------------------
void tTJSNI_QueueSoundBuffer::PushPlaySample( tTVPSoundSamplesBuffer* buffer ) {

	if( !BufferPlaying ) return;

	tTJSCriticalSectionHolder holder(BufferCS);

	ResetLastCheckedDecodePos();

	Samples.push_back(buffer);
	if (Stream) {
		Stream->Enqueue( buffer->GetBuffer(), buffer->GetBufferSize(), buffer->IsEnded(), (void*)buffer );
	} else {
		ReleasePlayedSample(buffer);
	}

#if 0
	tjs_int64 pos = buffer->GetDecodePosition();
	tjs_int64 ppos = GetCurrentPlayingPosition();
	TVPAddLog( TJS_W( "Sample Pos : " ) + ttstr( (tjs_int)ppos ) + TJS_W( "/" ) + ttstr( (tjs_int)pos ) );
#endif

	tjs_int64 decodePos = buffer->GetDecodePosition();
	const std::deque<tTVPWaveLabel> & labels = buffer->GetSegmentQueue().GetLabels();
	if(labels.size() != 0) {
		// add DecodePos offset to each item->Offset
		// and insert into LabelEventQueue
		for( std::deque<tTVPWaveLabel>::const_iterator i = labels.begin(); i != labels.end(); i++) {
			LabelEventQueue.push_back( tTVPWaveLabel(i->Position, i->Name, static_cast<tjs_int>(i->Offset + decodePos)));
		}

		// sort
		std::sort(LabelEventQueue.begin(), LabelEventQueue.end(), tTVPWaveLabel::tSortByOffsetFuncObj());

		// re-schedule label events
		TVPSoundBuffers.ReschedulePendingLabelEvent(GetNearestEventStep());
	}
}
//---------------------------------------------------------------------------
void tTJSNI_QueueSoundBuffer::ReleasePlayedSample(tTVPSoundSamplesBuffer* buffer) {

	{
		tTJSCriticalSectionHolder holder(BufferCS);
		if (Samples.size() > 0) {
			auto itr = std::find(Samples.begin(), Samples.end(), buffer);
			if (itr != Samples.end()) {
				Samples.erase(itr);
			}
		}
	}
	// 再生が終了したバッファ。まだ再生するのなら Decoder へ入れる
	if (!buffer->IsEnded()) {
		if(Thread) Thread->PushSamplesBuffer( buffer );
	}
}
//---------------------------------------------------------------------------
void tTJSNI_QueueSoundBuffer::DrainConsumedBuffers() {
	// decode thread から呼ばれる。
	// audio thread が consumed ring に積んだ完了 buffer をすべて引き取る。
	//
	// Stream ポインタの寿命は StopPlay() が BufferCS 越しに nullptr 化することで
	// 保護されている (StopPlay 側参照)。ここでは BufferCS 内で Stream を捕まえ、
	// その間に TryPopConsumed→ReleasePlayedSample まで一括して行う。
	//   ReleasePlayedSample 内部の BufferCS 取得は再帰ロックなので問題ない。
	while (true) {
		void* param = nullptr;
		{
			tTJSCriticalSectionHolder holder(BufferCS);
			if (!Stream) return;
			if (!Stream->TryPopConsumed(param)) return;
		}
		ReleasePlayedSample((tTVPSoundSamplesBuffer*)param);
	}
}

//---------------------------------------------------------------------------
void tTJSNI_QueueSoundBuffer::Update() {
	tTJSCriticalSectionHolder holder(BufferCS);
	if(!Decoder) return;
	if(!BufferPlaying) return;

	bool continued = true;
	if (Stream) {
		if( Paused ) {
			if( Stream->IsPlaying() ) {
				Stream->StopStream();
			}
		} else {
			if( !Stream->IsPlaying() && !Stream->AtEnd() ) {
				Stream->StartStream();
			}
		}
		if (Stream->AtEnd()) {
			continued = false;
		}
	} else {
		continued = false;
	}
	if (!continued) {
		FlushAllLabelEvents();
		ResetSamplePositions();
		BufferPlaying = false;
		if( LoopManager ) LoopManager->SetPosition( 0 );
	}
}
//---------------------------------------------------------------------------
void tTJSNI_QueueSoundBuffer::ResetLastCheckedDecodePos() {
	if( !Stream ) return;
	// set LastCheckedDecodePos and  LastCheckedTick
	// we shoud reset these values because the clock sources are usually
	// not identical.
	tTJSCriticalSectionHolder holder(BufferCS);
	LastCheckedDecodePos = GetCurrentPlayingPosition();
	LastCheckedTick = TVPGetTickCount();
}
//---------------------------------------------------------------------------
tjs_int tTJSNI_QueueSoundBuffer::FireLabelEventsAndGetNearestLabelEventStep( tjs_int64 tick ) {
	// fire events, event.EventTick <= tick, and return relative time to
	// next nearest event (return TVP_TIMEOFS_INVALID_VALUE for no events).

	// the vector LabelEventQueue must be sorted by the position.
	tTJSCriticalSectionHolder holder(BufferCS);

	if(!BufferPlaying) return TVP_TIMEOFS_INVALID_VALUE; // buffer is not currently playing
	if(!IsPlaying()) return TVP_TIMEOFS_INVALID_VALUE; // direct sound buffer is not currently playing

	if(LabelEventQueue.size() == 0) return TVP_TIMEOFS_INVALID_VALUE; // no more events

	// calculate current playing decodepos
	// at this point, LastCheckedDecodePos must not be -1
	if(LastCheckedDecodePos == -1) ResetLastCheckedDecodePos();
	tjs_int64 decodepos = (tick - LastCheckedTick) * Frequency / 1000 + LastCheckedDecodePos;

	while(true)
	{
		if(LabelEventQueue.size() == 0) break;
		auto i = LabelEventQueue.begin();
		int diff = (tjs_int32)i->Offset - (tjs_int32)decodepos;
		if(diff <= 0)
			InvokeLabelEvent(i->Name);
		else
			break;
		LabelEventQueue.erase(i);
	}

	if(LabelEventQueue.size() == 0) return TVP_TIMEOFS_INVALID_VALUE; // no more events

	return (tjs_int)((LabelEventQueue[0].Offset - (tjs_int32)decodepos) * 1000 / Frequency);
}
//---------------------------------------------------------------------------
tjs_int tTJSNI_QueueSoundBuffer::GetNearestEventStep() {
	// get nearest event stop from current tick
	// (current tick is taken from TVPGetTickCount)
	tTJSCriticalSectionHolder holder(BufferCS);

	if(LabelEventQueue.size() == 0) return TVP_TIMEOFS_INVALID_VALUE; // no more events

	// calculate current playing decodepos
	// at this point, LastCheckedDecodePos must not be -1
	if(LastCheckedDecodePos == -1) ResetLastCheckedDecodePos();
	tjs_int64 decodepos = (TVPGetTickCount() - LastCheckedTick) * Frequency / 1000 + LastCheckedDecodePos;
	return (tjs_int)((LabelEventQueue[0].Offset - (tjs_int32)decodepos) * 1000 / Frequency);
}
//---------------------------------------------------------------------------
void tTJSNI_QueueSoundBuffer::FlushAllLabelEvents() {
	// called at the end of the decode.
	// flush all undelivered events.
	tTJSCriticalSectionHolder holder(BufferCS);

	for( auto i = LabelEventQueue.begin(); i != LabelEventQueue.end(); i++)
		InvokeLabelEvent(i->Name);

	LabelEventQueue.clear();
}
//---------------------------------------------------------------------------
void tTJSNI_QueueSoundBuffer::StartPlay()
{
	if(!Decoder) return;

	// let primary buffer to start running
	// TVPEnsurePrimaryBufferPlay();

	// ensure playing thread
	TVPSoundBuffers.EnsureBufferWorking();

	// play from first
	tjs_int64 predecodedSamples = 0;
	{	// thread protected block
		tTJSCriticalSectionHolder holder(BufferCS);
		Thread->ClearQueue();

		if (Stream) delete Stream, Stream = nullptr;

		for( tjs_uint i = 0; i < BufferCount; i++ ) {
			if( Buffer[i] == nullptr ) {
				Buffer[i] = new tTVPSoundSamplesBuffer( this, i );
			}
			Buffer[i]->Create( &InputFormat, UseVisBuffer );
		}

		{
			tTVPAudioStreamParam param;
			param.Channels   = InputFormat.Channels;		// チャンネル数
			param.SampleRate = InputFormat.SamplesPerSec;		// サンプリングレート
			param.BitsPerSample = InputFormat.BitsPerSample;	// サンプル当たりのビット数
			param.SampleType = astUInt8;
			if( InputFormat.IsFloat ) {
				param.SampleType = astFloat32;	// サンプルの形式
			} else if( param.BitsPerSample == 8 ) {
				param.SampleType = astUInt8;
			} else if( param.BitsPerSample == 16 ) {
				param.SampleType = astInt16;
			} else {
				TVPThrowExceptionMessage(TVPInvalidFormatBitsPerSample);
			}
			Stream = TVPCreateAudioStream( param );
			if( Stream == nullptr ) {
				TVPThrowExceptionMessage(TVPFaildToCreateAudioStream);
			}
			// audio thread が完了 buffer を consumed ring に積んだら decode thread を起こす。
			// 起こされた側 (Execute ループ) は Owner->DrainConsumedBuffers() で取り出す。
			// 旧来の audio thread から ReleasePlayedSample を直接呼ぶ経路は廃止。
			Stream->SetWakeupHandler([](void *userData){
				tTVPSoundDecodeThread *t = (tTVPSoundDecodeThread*)userData;
				if (t) t->Wakeup();
			}, Thread);

			// reset volume, sound position and frequency
			SetVolumeToStream();
			SetFrequencyToStream();
			Set3DParamsToStream(); // キャッシュ済み 3D 状態を新 Stream へ適用
		}

		// reset filter chain
		ResetFilterChain();

		// fill sound buffer with some first samples
		BufferPlaying = true;

		for( tjs_int i = 0; i < BufferCount; i++ ) {
			Buffer[i]->Reset();
			Buffer[i]->Decode();
			Buffer[i]->SetDecodePosition( predecodedSamples );
			predecodedSamples += Buffer[i]->GetInSamples();
			PushPlaySample( Buffer[i] );
		}

		// start playing
		if (!Paused) {
			Stream->StartStream();
		}

		// re-schedule label events
		ResetLastCheckedDecodePos();
		TVPSoundBuffers.ReschedulePendingLabelEvent(GetNearestEventStep());
	}	// end of thread protected block

	// ensure thread
	TVPSoundBuffers.EnsureBufferWorking(); // wake the playing thread up again
	ThreadCallbackEnabled = true;
	Thread->StartDecoding( predecodedSamples );
}
//---------------------------------------------------------------------------
void tTJSNI_QueueSoundBuffer::StopPlay()
{
	if(!Decoder) return;

	// Stream ポインタは decode thread の DrainConsumedBuffers() からも参照される。
	// (1) StopStream で audio callback を止める
	// (2) Stream=nullptr の publish を BufferCS 越しに行い decode thread に visibility 保証
	// (3) 実際の delete (ma_sound_uninit が drain 待ちで block する可能性あり) はロック外
	iTVPAudioStream* tobedeleted = nullptr;
	{
		tTJSCriticalSectionHolder holder(BufferCS);
		if (Stream) {
			Stream->StopStream();
			tobedeleted = Stream;
			Stream = nullptr;
		}
		BufferPlaying = false;
	}
	if (tobedeleted) delete tobedeleted;
}
//---------------------------------------------------------------------------
void tTJSNI_QueueSoundBuffer::Play() {
	// play from first or current position
	if(!Decoder) return;
	if(BufferPlaying) return;

	StopPlay();

	tTJSCriticalSectionHolder holder(BufferCS);

	StartPlay();
	SetStatus(ssPlay);
}
//---------------------------------------------------------------------------
void tTJSNI_QueueSoundBuffer::Stop() {
	// stop playing
	StopPlay();

	// delete thread
	ThreadCallbackEnabled = false;
	TVPSoundBuffers.CheckAllSleep();
	if(Thread) Thread->Interrupt();

	// set status
	if(Status != ssUnload) SetStatus(ssStop);

	// rewind
	if(LoopManager) LoopManager->SetPosition(0);
}
//---------------------------------------------------------------------------
bool tTJSNI_QueueSoundBuffer::GetPaused() const {
	return Paused;
}
//---------------------------------------------------------------------------
void tTJSNI_QueueSoundBuffer::SetPaused(bool b) {
	Paused = b;
}
//---------------------------------------------------------------------------
void tTJSNI_QueueSoundBuffer::Open(const ttstr & storagename) {
	// open a storage and prepare to play
	//TVPEnsurePrimaryBufferPlay(); // let primary buffer to start running

	// ファイルを開く=実際に再生に入る段になって初めてデコードスレッドを起こす
	EnsureDecodeThread();

	Clear();

	Decoder = TVPCreateWaveDecoder(storagename);

	try
	{
		// make manager
		LoopManager = new tTVPWaveLoopManager();
		LoopManager->SetDecoder(Decoder);
		LoopManager->SetLooping(Looping);

		// build filter chain
		RebuildFilterChain();

		// retrieve format
		InputFormat = FilterOutput->GetFormat();
		Frequency = InputFormat.SamplesPerSec;
	}
	catch(...)
	{
		Clear();
		throw;
	}

	// open loop information file
	ttstr sliname = storagename + TJS_W(".sli");
	if(TVPIsExistentStorage(sliname))
	{
		tTVPStreamHolder slistream(sliname);
		char *buffer;
		tjs_uint size;
		buffer = new char [ (size = static_cast<tjs_uint>(slistream->GetSize())) +1];
		try
		{
			TVPReadBuffer(slistream.Get(), buffer, size);
			buffer[size] = 0;

			if(!LoopManager->ReadInformation(buffer))
				TVPThrowExceptionMessage(TVPInvalidLoopInformation, sliname);
			RecreateWaveLabelsObject();
		}
		catch(...)
		{
			delete [] buffer;
			Clear();
			throw;
		}
		delete [] buffer;
	}

	// set status to stop
	SetStatus(ssStop);
}
//---------------------------------------------------------------------------
void tTJSNI_QueueSoundBuffer::SetLooping(bool b) {
	Looping = b;
	if( LoopManager ) LoopManager->SetLooping( Looping );
}
//---------------------------------------------------------------------------
tjs_uint64 tTJSNI_QueueSoundBuffer::GetSamplePosition() {
	tjs_uint64 result = 0;
	if( Stream ) {
		tTJSCriticalSectionHolder holder(BufferCS);
		tjs_uint64 pos = Stream->GetSamplesPlayed();
		if( Samples.size() > 0 ) {
			auto itr = Samples.begin();
			tTVPSoundSamplesBuffer* sample = *itr;
			tjs_uint count = sample->GetSamplesCount();
			tjs_int offset = (tjs_int)( pos % count );
			result = sample->GetSegmentQueue().FilteredPositionToDecodePosition( offset );
		}
	}
	return result;
}
//---------------------------------------------------------------------------
void tTJSNI_QueueSoundBuffer::SetSamplePosition(tjs_uint64 pos) {
	tjs_uint64 possamples = pos; // in samples

	if(InputFormat.TotalSamples && InputFormat.TotalSamples <= possamples) return;

	if(BufferPlaying && IsPlaying()) {
		StopPlay();
		LoopManager->SetPosition(possamples);
		StartPlay();
	} else {
		LoopManager->SetPosition(possamples);
	}
}
//---------------------------------------------------------------------------
tjs_uint64 tTJSNI_QueueSoundBuffer::GetPosition() {
	if(!Decoder) return 0L;
	if(!Stream) return 0L;
	return GetSamplePosition() * 1000 / InputFormat.SamplesPerSec;
}
//---------------------------------------------------------------------------
void tTJSNI_QueueSoundBuffer::SetPosition(tjs_uint64 pos) {
	SetSamplePosition(pos * InputFormat.SamplesPerSec / 1000); // in samples
}
//---------------------------------------------------------------------------
tjs_uint64 tTJSNI_QueueSoundBuffer::GetTotalTime() {
	return InputFormat.TotalSamples * 1000ULL / InputFormat.SamplesPerSec;
}
//---------------------------------------------------------------------------
void tTJSNI_QueueSoundBuffer::SetVolumeToStream() {
	// set current volume/pan to Stream
	if( Stream ) {
		tjs_int v;
		tjs_int mutevol = 100000;
#ifdef ANDROID
		if( !Application->GetActivating() ) {
			mutevol = TVPSoundGlobalFocusMuteVolume;
		}
#else
		if(TVPSoundGlobalFocusModeByOption >= sgfmMuteOnDeactivate &&
			TVPSoundGlobalFocusMuteVolume == 0)
		{
			// no mute needed here;
			// muting will be processed in DirectSound framework.
			;
		}
		else
		{
			// mute mode is choosen from GlobalFocusMode or
			// TVPSoundGlobalFocusModeByOption which is more restrictive.
			tTVPSoundGlobalFocusMode mode =
				GlobalFocusMode > TVPSoundGlobalFocusModeByOption ?
				GlobalFocusMode : TVPSoundGlobalFocusModeByOption;

			switch(mode)
			{
			case sgfmNeverMute:
				;
				break;
			case sgfmMuteOnMinimize:
				if(!  Application->GetNotMinimizing())
					mutevol = TVPSoundGlobalFocusMuteVolume;
				break;
			case sgfmMuteOnDeactivate:
				if(! (  Application->GetActivating() && Application->GetNotMinimizing()))
					mutevol = TVPSoundGlobalFocusMuteVolume;
				break;
			}
		}
#endif
		// compute volume for each buffer
		v = (Volume / 10) * (Volume2 / 10) / 1000;
		v = (v / 10) * (GlobalVolume / 10) / 1000;
		v = (v / 10) * (mutevol / 10) / 1000;
		Stream->SetVolume( v );
		Stream->SetPan( Pan );
	}
}
//---------------------------------------------------------------------------
void tTJSNI_QueueSoundBuffer::SetVolume(tjs_int v) {
	if(v < 0) v = 0;
	if(v > 100000) v = 100000;

	if( Volume != v ) {
		Volume = v;
		SetVolumeToStream();
	}
}
//---------------------------------------------------------------------------
void tTJSNI_QueueSoundBuffer::SetVolume2(tjs_int v) {
	if(v < 0) v = 0;
	if(v > 100000) v = 100000;

	if( Volume2 != v ) {
		Volume2 = v;
		SetVolumeToStream();
	}
}
//---------------------------------------------------------------------------
void tTJSNI_QueueSoundBuffer::SetPan(tjs_int v) {
	if(v < -100000) v = -100000;
	if(v > 100000) v = 100000;
	if( Pan != v ) {
		Pan = v;
		SetVolumeToStream();
	}
}
//---------------------------------------------------------------------------
// 3D 定位: キャッシュ済みパラメータを Stream (ma_sound) へ一括適用する。
// Stream 生成後 (Play 時) と、各 setter で Stream が既にある場合に呼ばれる。
void tTJSNI_QueueSoundBuffer::Set3DParamsToStream() {
	if( !Stream ) return;
	Stream->SetSpatializationEnabled( Use3D );
	if( !Use3D ) return; // 無効時は位置等を送らない (非空間化パススルー)
	Stream->Set3DAttenuationModel( AttenuationModel );
	Stream->Set3DMinDistance( MinDistance );
	Stream->Set3DMaxDistance( MaxDistance );
	Stream->Set3DRolloff( RolloffFactor );
	Stream->Set3DDopplerFactor( DopplerFactor );
	Stream->Set3DPosition( PosX, PosY, PosZ );
	Stream->Set3DVelocity( VelX, VelY, VelZ );
	Stream->Set3DConeDirection( ConeDirX, ConeDirY, ConeDirZ );
	Stream->Set3DCone( ConeInnerRad, ConeOuterRad, ConeOuterGain );
}
//---------------------------------------------------------------------------
void tTJSNI_QueueSoundBuffer::SetUse3D(bool b) {
	if( Use3D != b ) {
		Use3D = b;
		Set3DParamsToStream();
	}
}
//---------------------------------------------------------------------------
void tTJSNI_QueueSoundBuffer::SetPos(float x, float y, float z) {
	PosX = x; PosY = y; PosZ = z;
	if( Stream && Use3D ) Stream->Set3DPosition( x, y, z );
}
void tTJSNI_QueueSoundBuffer::SetPosX(float v) { SetPos( v, PosY, PosZ ); }
void tTJSNI_QueueSoundBuffer::SetPosY(float v) { SetPos( PosX, v, PosZ ); }
void tTJSNI_QueueSoundBuffer::SetPosZ(float v) { SetPos( PosX, PosY, v ); }
//---------------------------------------------------------------------------
void tTJSNI_QueueSoundBuffer::Set3DVelocity(float x, float y, float z) {
	VelX = x; VelY = y; VelZ = z;
	if( Stream && Use3D ) Stream->Set3DVelocity( x, y, z );
}
void tTJSNI_QueueSoundBuffer::Set3DConeDirection(float x, float y, float z) {
	ConeDirX = x; ConeDirY = y; ConeDirZ = z;
	if( Stream && Use3D ) Stream->Set3DConeDirection( x, y, z );
}
void tTJSNI_QueueSoundBuffer::Set3DCone(float innerAngleRad, float outerAngleRad, float outerGain) {
	ConeInnerRad = innerAngleRad; ConeOuterRad = outerAngleRad; ConeOuterGain = outerGain;
	if( Stream && Use3D ) Stream->Set3DCone( innerAngleRad, outerAngleRad, outerGain );
}
void tTJSNI_QueueSoundBuffer::SetMinDistance(float v) {
	MinDistance = v;
	if( Stream && Use3D ) Stream->Set3DMinDistance( v );
}
void tTJSNI_QueueSoundBuffer::SetMaxDistance(float v) {
	MaxDistance = v;
	if( Stream && Use3D ) Stream->Set3DMaxDistance( v );
}
void tTJSNI_QueueSoundBuffer::SetRolloffFactor(float v) {
	RolloffFactor = v;
	if( Stream && Use3D ) Stream->Set3DRolloff( v );
}
void tTJSNI_QueueSoundBuffer::SetDopplerFactor(float v) {
	DopplerFactor = v;
	if( Stream && Use3D ) Stream->Set3DDopplerFactor( v );
}
void tTJSNI_QueueSoundBuffer::SetAttenuationModel(tjs_int m) {
	if( m < 0 ) m = 0; if( m > 3 ) m = 3;
	AttenuationModel = m;
	if( Stream && Use3D ) Stream->Set3DAttenuationModel( m );
}
//---------------------------------------------------------------------------
void tTJSNI_QueueSoundBuffer::SetGlobalVolume(tjs_int v) {
	if(v < 0) v = 0;
	if(v > 100000) v = 100000;

	if( GlobalVolume != v ) {
		GlobalVolume = v;
		TVPSoundBuffers.ResetVolumeToAllSoundBuffer();
	}
}
//---------------------------------------------------------------------------
void tTJSNI_QueueSoundBuffer::SetGlobalFocusMode(tTVPSoundGlobalFocusMode b) {
	if( GlobalFocusMode != b ) {
		GlobalFocusMode = b;
		TVPSoundBuffers.ResetVolumeToAllSoundBuffer();
	}
}
//---------------------------------------------------------------------------
// DirectSound 撤去 (Phase1-3) に伴い旧 WaveImpl から移設。
// WINVER の Application (フォーカス切替時の音量リセット) が呼ぶ。
void TVPResetVolumeToAllSoundBuffer()
{
	TVPSoundBuffers.ResetVolumeToAllSoundBuffer();
}
//---------------------------------------------------------------------------
void tTJSNI_QueueSoundBuffer::SetFrequencyToStream() {
	if(Stream) Stream->SetFrequency( Frequency );
}
//---------------------------------------------------------------------------
void tTJSNI_QueueSoundBuffer::SetFrequency(tjs_int freq) {
	Frequency = freq;
	SetFrequencyToStream();
}
//---------------------------------------------------------------------------
void tTJSNI_QueueSoundBuffer::SetUseVisBuffer(bool b) {
	tTJSCriticalSectionHolder holder(BufferCS);
	if(b) {
		UseVisBuffer = true;
		ResetVisBuffer();
	} else {
		DeallocateVisBuffer();
		UseVisBuffer = false;
	}
}
//---------------------------------------------------------------------------
void tTJSNI_QueueSoundBuffer::TimerBeatHandler() {
	inherited::TimerBeatHandler();

	// check buffer stopping
	if(Status == ssPlay && !BufferPlaying)
	{
		TVPLOG_DEBUG("QueueSoundBuffer: Buffer stopped");
		// buffer was stopped
		ThreadCallbackEnabled = false;
		TVPSoundBuffers.CheckAllSleep();
		if(Thread) Thread->Interrupt();
		SetStatusAsync(ssStop);
	}
}
//---------------------------------------------------------------------------
void tTJSNI_QueueSoundBuffer::ResetVisBuffer() {
	// reset or recreate visualication buffer
	tTJSCriticalSectionHolder holder(BufferCS);
	for( tjs_uint i = 0; i < BufferCount; i++ ) {
		if( Buffer[i] ) Buffer[i]->ResetVisBuffer();
	}
	UseVisBuffer = true;
}
//---------------------------------------------------------------------------
void tTJSNI_QueueSoundBuffer::DeallocateVisBuffer() {
	tTJSCriticalSectionHolder holder(BufferCS);
	for( tjs_uint i = 0; i < BufferCount; i++ ) {
		if( Buffer[i] ) Buffer[i]->DeallocateVisBuffer();
	}
	UseVisBuffer = false;
}
//---------------------------------------------------------------------------
void tTJSNI_QueueSoundBuffer::CopyVisBuffer(tjs_int16 *dest, const tjs_uint8 *src,
	tjs_int numsamples, tjs_int channels) {

	if(channels == 1)
	{
		TVPConvertPCMTo16bits(dest, (const void*)src, InputFormat.Channels,
			InputFormat.BytesPerSample, InputFormat.BitsPerSample,
			InputFormat.IsFloat, numsamples, true);
	}
	else if(channels == InputFormat.Channels)
	{
		TVPConvertPCMTo16bits(dest, (const void*)src, InputFormat.Channels,
			InputFormat.BytesPerSample, InputFormat.BitsPerSample,
			InputFormat.IsFloat, numsamples, false);
	}
}
//---------------------------------------------------------------------------
tjs_int tTJSNI_QueueSoundBuffer::GetVisBuffer(tjs_int16 *dest, tjs_int numsamples, tjs_int channels, tjs_int aheadsamples ) {
	// read visualization buffer samples
	if(!UseVisBuffer) return 0;
	if(!Decoder) return 0;
	if(!IsPlaying() || !BufferPlaying) return 0;

	if(channels != InputFormat.Channels && channels != 1) return 0;

	tjs_int writtensamples = 0;
	tjs_uint blockAlign = InputFormat.BytesPerSample * InputFormat.Channels;
	if( Stream ) {
		tTJSCriticalSectionHolder holder(BufferCS);
		tjs_uint64 pos = Stream->GetSamplesPlayed();
		if( Samples.size() > 0 ) {
			auto itr = Samples.begin();
			if( (*itr)->GetSegmentQueue().GetFilteredLength() == 0 ) return 0;
			tTVPSoundSamplesBuffer* sample = *itr;
			tjs_int count = static_cast<tjs_int>(sample->GetSamplesCount());
			tjs_int offset = (tjs_int)( pos % count ) + aheadsamples;
			for( auto i = Samples.begin(); i != Samples.end(); i++ ) {
				if( offset >= count ) {
					offset -= count;
					continue;
				}
				tjs_int bufrest = count - offset;
				tjs_int copysamples = (bufrest > numsamples ? numsamples : bufrest);
				CopyVisBuffer(dest, (*i)->GetVisBuffer() + offset * blockAlign, copysamples, channels);
				numsamples -= copysamples;
				writtensamples += copysamples;
				if(numsamples <= 0) break;

				dest += channels * copysamples;
				offset = 0;
			}
		}
	}
	return writtensamples;
}
//---------------------------------------------------------------------------
// lip-sync / 解析系。GetVisBuffer(...,1,...) のモノラルダウンミックス+再生位置
// 同期を流用し、C++ 側で RMS / スペクトル / 母音推定を行う。
//---------------------------------------------------------------------------
#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif
static const tjs_int TVP_LIPSYNC_FFT = 1024; // 解析 FFT サイズ (~23ms @44.1kHz)

//---------------------------------------------------------------------------
bool tTJSNI_QueueSoundBuffer::GetSoundLevel(float &rms, float &peak,
	tjs_int windowsamples, tjs_int aheadsamples)
{
	rms = 0.0f; peak = 0.0f;
	if(!UseVisBuffer) { SetUseVisBuffer(true); return false; } // 次フレームから有効
	if(windowsamples <= 0) windowsamples = TVP_LIPSYNC_FFT;
	if(windowsamples > 16384) windowsamples = 16384;

	std::vector<tjs_int16> tmp(windowsamples);
	tjs_int got = GetVisBuffer(tmp.data(), windowsamples, 1, aheadsamples);
	if(got <= 0) return false;

	double sum = 0.0;
	tjs_int pk = 0;
	for(tjs_int i = 0; i < got; i++) {
		tjs_int s = tmp[i];
		tjs_int a = (s < 0) ? -s : s;
		if(a > pk) pk = a;
		double f = tmp[i] / 32768.0;
		sum += f * f;
	}
	rms  = (float)sqrt(sum / got);
	peak = (float)(pk / 32768.0);
	return true;
}
//---------------------------------------------------------------------------
bool tTJSNI_QueueSoundBuffer::ComputeMagnitude(float *mag, tjs_int fftsize, tjs_int aheadsamples)
{
	if(!UseVisBuffer) { SetUseVisBuffer(true); return false; }

	// FFT ワークの遅延確保 (サイズ変化時のみ再確保。ip[0]=0 で rdft が初回に内部初期化)
	if((tjs_int)AnalyzeWork.size() != fftsize) {
		AnalyzeWork.assign(fftsize, 0.0f);
		AnalyzeIp.assign(2 + (int)sqrt((double)fftsize) + 2, 0);
		AnalyzeW.assign(fftsize / 2, 0.0f);
		AnalyzeWindow.assign(fftsize, 0.0f);
		for(tjs_int i = 0; i < fftsize; i++)
			AnalyzeWindow[i] = (float)(0.5 - 0.5 * cos(2.0 * M_PI * i / (fftsize - 1))); // Hann
	}

	std::vector<tjs_int16> tmp(fftsize);
	tjs_int got = GetVisBuffer(tmp.data(), fftsize, 1, aheadsamples);
	if(got <= 0) return false;

	for(tjs_int i = 0; i < fftsize; i++) {
		float s = (i < got) ? (tmp[i] / 32768.0f) : 0.0f; // 不足分はゼロ詰め
		AnalyzeWork[i] = s * AnalyzeWindow[i];
	}

	// Ooura 実 FFT。出力パッキング: a[0]=Re[0](DC), a[1]=Re[N/2](Nyq),
	// a[2k]=Re[k], a[2k+1]=Im[k] (k=1..N/2-1)。
#if defined(_M_IX86) || defined(_M_X64) || defined(__i386__) || defined(__x86_64__)
	rdft_sse(fftsize, 1, AnalyzeWork.data(), AnalyzeIp.data(), AnalyzeW.data());
#elif defined(__ARM_NEON) || defined(__aarch64__)
	rdft_neon(fftsize, 1, AnalyzeWork.data(), AnalyzeIp.data(), AnalyzeW.data());
#else
	rdft(fftsize, 1, AnalyzeWork.data(), AnalyzeIp.data(), AnalyzeW.data());
#endif

	tjs_int half = fftsize / 2;
	float inv = 1.0f / fftsize;
	mag[0] = (float)fabs(AnalyzeWork[0]) * inv; // DC
	for(tjs_int k = 1; k < half; k++) {
		float re = AnalyzeWork[2 * k];
		float im = AnalyzeWork[2 * k + 1];
		mag[k] = sqrtf(re * re + im * im) * inv;
	}
	return true;
}
//---------------------------------------------------------------------------
tjs_int tTJSNI_QueueSoundBuffer::GetSoundSpectrum(float *bands, tjs_int numbands, tjs_int aheadsamples)
{
	for(tjs_int i = 0; i < numbands; i++) bands[i] = 0.0f;
	if(numbands <= 0) return 0;

	const tjs_int fftsize = TVP_LIPSYNC_FFT;
	tjs_int half = fftsize / 2;
	std::vector<float> mag(half);
	if(!ComputeMagnitude(mag.data(), fftsize, aheadsamples)) return 0;

	// bin 1..half-1 を対数配置で numbands に集約 (RMS)
	double minb = 1.0, maxb = (double)half;
	for(tjs_int b = 0; b < numbands; b++) {
		double lo = minb * pow(maxb / minb, (double)b / numbands);
		double hi = minb * pow(maxb / minb, (double)(b + 1) / numbands);
		tjs_int i0 = (tjs_int)lo;
		tjs_int i1 = (tjs_int)hi;
		if(i0 < 1) i0 = 1;
		if(i1 <= i0) i1 = i0 + 1;
		if(i1 > half) i1 = half;
		double e = 0.0;
		for(tjs_int i = i0; i < i1; i++) e += (double)mag[i] * mag[i];
		bands[b] = (float)sqrt(e / (i1 - i0));
	}
	return numbands;
}
//---------------------------------------------------------------------------
bool tTJSNI_QueueSoundBuffer::GetVowel(float *weights, tjs_int aheadsamples)
{
	for(int i = 0; i < 5; i++) weights[i] = 0.0f;

	const tjs_int fftsize = TVP_LIPSYNC_FFT;
	tjs_int half = fftsize / 2;
	std::vector<float> mag(half);
	if(!ComputeMagnitude(mag.data(), fftsize, aheadsamples)) return false;

	tjs_int sr = (tjs_int)InputFormat.SamplesPerSec;
	if(sr <= 0) sr = 44100;

	// 日本語 5 母音の代表フォルマント (F1, F2) [Hz]: a, i, u, e, o
	static const float F1[5] = { 800.0f, 300.0f, 350.0f, 500.0f, 500.0f };
	static const float F2[5] = { 1200.0f, 2300.0f, 1200.0f, 1900.0f, 900.0f };

	// 指定周波数近傍 (±15%+50Hz) のエネルギー総和
	auto energyAround = [&](float hz) -> double {
		float bw = hz * 0.15f + 50.0f;
		tjs_int i0 = (tjs_int)((hz - bw) * fftsize / sr);
		tjs_int i1 = (tjs_int)((hz + bw) * fftsize / sr);
		if(i0 < 1) i0 = 1;
		if(i1 > half) i1 = half;
		if(i1 <= i0) return 0.0;
		double e = 0.0;
		for(tjs_int i = i0; i < i1; i++) e += (double)mag[i] * mag[i];
		return e;
	};

	double sc[5], total = 0.0;
	for(int v = 0; v < 5; v++) {
		sc[v] = energyAround(F1[v]) + energyAround(F2[v]);
		total += sc[v];
	}
	if(total <= 1e-9) return false; // 無音 / フォルマント帯域にエネルギー無し

	for(int v = 0; v < 5; v++) weights[v] = (float)(sc[v] / total);
	return true;
}
//---------------------------------------------------------------------------


//---------------------------------------------------------------------------
// tTJSNC_WaveSoundBuffer
//---------------------------------------------------------------------------
static tTJSNativeInstance *CreateNativeInstance()
{
	return new tTJSNI_QueueSoundBuffer();
}
//---------------------------------------------------------------------------



//---------------------------------------------------------------------------
// TVPCreateNativeClass_WaveSoundBuffer
//---------------------------------------------------------------------------
tTJSNativeClass * TVPCreateNativeClass_QueueSoundBuffer()
{
	tTJSNativeClass *cls = new tTJSNC_WaveSoundBuffer();
	((tTJSNC_WaveSoundBuffer*)cls)->Factory = CreateNativeInstance;
	static tjs_uint32 TJS_NCM_CLASSID;
	TJS_NCM_CLASSID = tTJSNC_WaveSoundBuffer::ClassID;

//----------------------------------------------------------------------
// methods
//----------------------------------------------------------------------
TJS_BEGIN_NATIVE_METHOD_DECL(/*func. name*/freeDirectSound)  /* static */
{
	return TJS_S_OK;
}
TJS_END_NATIVE_METHOD_DECL_OUTER(/*object to register*/cls,
	/*func. name*/freeDirectSound)
//----------------------------------------------------------------------
TJS_BEGIN_NATIVE_METHOD_DECL(/*func. name*/getVisBuffer)
{
	// get samples for visualization 
	TJS_GET_NATIVE_INSTANCE(/*var. name*/_this,
		/*var. type*/tTJSNI_QueueSoundBuffer);

	if(numparams < 3) return TJS_E_BADPARAMCOUNT;
	tjs_int16 *dest = (tjs_int16*)(tjs_intptr_t)(tTVInteger)(*param[0]);

	tjs_int ahead = 0;
	if(numparams >= 4) ahead = (tjs_int)*param[3];

	tjs_int res = _this->GetVisBuffer(dest, *param[1], *param[2], ahead);

	if(result) *result = res;

	return TJS_S_OK;
}
TJS_END_NATIVE_METHOD_DECL_OUTER(/*object to register*/cls,
	/*func. name*/getVisBuffer)
//----------------------------------------------------------------------



//----------------------------------------------------------------------
// properties
//----------------------------------------------------------------------
TJS_BEGIN_NATIVE_PROP_DECL(useVisBuffer)
{
	TJS_BEGIN_NATIVE_PROP_GETTER
	{
		TJS_GET_NATIVE_INSTANCE(/*var. name*/_this,
			/*var. type*/tTJSNI_QueueSoundBuffer);

		*result = _this->GetUseVisBuffer();
		return TJS_S_OK;
	}
	TJS_END_NATIVE_PROP_GETTER

	TJS_BEGIN_NATIVE_PROP_SETTER
	{
		TJS_GET_NATIVE_INSTANCE(/*var. name*/_this,
			/*var. type*/tTJSNI_QueueSoundBuffer);

		_this->SetUseVisBuffer(0!=(tjs_int)*param);

		return TJS_S_OK;
	}
	TJS_END_NATIVE_PROP_SETTER
}
TJS_END_NATIVE_PROP_DECL_OUTER(cls, useVisBuffer)
//----------------------------------------------------------------------
// lip-sync / 解析 API。いずれも要 useVisBuffer (未設定なら自動有効化し、その回は
// 初期値を返す)。ahead は出力レイテンシ補正用の先読みサンプル数 (既定 0)。
//----------------------------------------------------------------------
TJS_BEGIN_NATIVE_METHOD_DECL(/*func. name*/getSoundLevel)
{
	// 再生カーソル付近の音量。%[ rms:.., peak:.. ] (0.0〜1.0) を返す。
	TJS_GET_NATIVE_INSTANCE(/*var. name*/_this, /*var. type*/tTJSNI_QueueSoundBuffer);

	tjs_int ahead  = (numparams >= 1) ? (tjs_int)*param[0] : 0;
	tjs_int window = (numparams >= 2) ? (tjs_int)*param[1] : 0; // 0 = 既定

	float rms = 0.0f, peak = 0.0f;
	_this->GetSoundLevel(rms, peak, window, ahead);

	if(result) {
		iTJSDispatch2 *dic = TJSCreateDictionaryObject();
		tTJSVariant v;
		v = (tjs_real)rms;  dic->PropSet(TJS_MEMBERENSURE, TJS_W("rms"),  NULL, &v, dic);
		v = (tjs_real)peak; dic->PropSet(TJS_MEMBERENSURE, TJS_W("peak"), NULL, &v, dic);
		*result = tTJSVariant(dic, dic);
		dic->Release();
	}
	return TJS_S_OK;
}
TJS_END_NATIVE_METHOD_DECL_OUTER(/*object to register*/cls, /*func. name*/getSoundLevel)
//----------------------------------------------------------------------
TJS_BEGIN_NATIVE_METHOD_DECL(/*func. name*/getSoundSpectrum)
{
	// ログ配置 numbands バンドのスペクトルエネルギーを配列で返す。
	TJS_GET_NATIVE_INSTANCE(/*var. name*/_this, /*var. type*/tTJSNI_QueueSoundBuffer);

	if(numparams < 1) return TJS_E_BADPARAMCOUNT;
	tjs_int numbands = (tjs_int)*param[0];
	if(numbands < 1) numbands = 1;
	if(numbands > 256) numbands = 256;
	tjs_int ahead = (numparams >= 2) ? (tjs_int)*param[1] : 0;

	std::vector<float> bands(numbands);
	_this->GetSoundSpectrum(bands.data(), numbands, ahead);

	if(result) {
		iTJSDispatch2 *arr = TJSCreateArrayObject();
		for(tjs_int i = 0; i < numbands; i++) {
			tTJSVariant v; v = (tjs_real)bands[i];
			arr->PropSetByNum(TJS_MEMBERENSURE, i, &v, arr);
		}
		*result = tTJSVariant(arr, arr);
		arr->Release();
	}
	return TJS_S_OK;
}
TJS_END_NATIVE_METHOD_DECL_OUTER(/*object to register*/cls, /*func. name*/getSoundSpectrum)
//----------------------------------------------------------------------
TJS_BEGIN_NATIVE_METHOD_DECL(/*func. name*/getVowel)
{
	// 日本語 5 母音の推定重み %[ a:,i:,u:,e:,o:, voiced: ] を返す (合計 ~1.0)。
	TJS_GET_NATIVE_INSTANCE(/*var. name*/_this, /*var. type*/tTJSNI_QueueSoundBuffer);

	tjs_int ahead = (numparams >= 1) ? (tjs_int)*param[0] : 0;

	float w[5];
	bool voiced = _this->GetVowel(w, ahead);

	if(result) {
		iTJSDispatch2 *dic = TJSCreateDictionaryObject();
		static const tjs_char *names[5] = { TJS_W("a"), TJS_W("i"), TJS_W("u"), TJS_W("e"), TJS_W("o") };
		for(int i = 0; i < 5; i++) {
			tTJSVariant v; v = (tjs_real)w[i];
			dic->PropSet(TJS_MEMBERENSURE, names[i], NULL, &v, dic);
		}
		tTJSVariant vv; vv = (tjs_int)(voiced ? 1 : 0);
		dic->PropSet(TJS_MEMBERENSURE, TJS_W("voiced"), NULL, &vv, dic);
		*result = tTJSVariant(dic, dic);
		dic->Release();
	}
	return TJS_S_OK;
}
TJS_END_NATIVE_METHOD_DECL_OUTER(/*object to register*/cls, /*func. name*/getVowel)
//----------------------------------------------------------------------
	return cls;
}
//---------------------------------------------------------------------------

