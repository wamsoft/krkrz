//---------------------------------------------------------------------------
// pl_mpeg ベースの MPEG-1 ムービプレイヤ (generic/SDL 用) 実装。
//
// WIN 版の win32/movie/Mpeg1Video.cpp + LayerVideoBase.cpp と同じ考え方で、
// 自前のデコードスレッドを持ち A/V 同期しながら iTVPMoviePlayer のコールバック
// (SetOnVideoDecoded=ARGB / SetOnVideoDecodedPlanes=I420) へフレームを供給する。
// 音声は generic 共通の miniaudio シンクアダプタ (tTVPMovieAudioSinkAdapter) で出力し、
// その再生済みサンプル数を A/V 同期のマスタクロックに使う (音声なしはフレーム間 delta)。
//---------------------------------------------------------------------------
#include "tjsCommHead.h"
#include "Mpeg1MoviePlayer.h"
#include "MovieAudioSinkAdapter.h"   // tTVPMovieAudioSinkAdapter (miniaudio 経由)
#include "IAudioSink.h"              // IAudioSink::PCM_F32

#include <atomic>
#include <mutex>
#include <condition_variable>
#include <thread>
#include <chrono>
#include <cstring>
#include <cctype>

#define PL_MPEG_IMPLEMENTATION
#include "pl_mpeg/pl_mpeg.h"

namespace {

//---------------------------------------------------------------------------
class tTVPMpeg1MoviePlayer : public iTVPMoviePlayer
{
	enum State { stStopped, stPlaying, stPaused, stEnded };

	std::vector<uint8_t> mData;      // MPEG-1 全データ (pl_mpeg がシークに要する)
	plm_t               *mPlm;
	int                  mWidth, mHeight;
	double               mFps;
	int64_t              mDurationUs;

	// 音声
	tTVPMovieAudioSinkAdapter mSink;
	bool                 mHasAudio;
	int                  mAudioSampleRate;
	// pl_mpeg の音声サンプルは次デコードで上書きされるため、シンクへ渡す前に
	// プールへコピーする (シンクは再生完了まで参照を保持する契約のため)。
	struct AudioBuf { std::vector<float> data; };
	std::vector<AudioBuf*> mAudioPool;   // 全確保 (dtor で解放)
	std::vector<AudioBuf*> mAudioFree;   // 再利用待ち

	// 再生状態 / スレッド
	std::atomic<State>   mState;
	std::atomic<bool>    mTerminate;
	std::atomic<bool>    mDoSeek;
	std::atomic<int64_t> mSeekUs;
	std::atomic<int64_t> mCurPosUs;
	std::atomic<bool>    mLoop;
	std::thread          mThread;
	std::mutex           mMtx;
	std::condition_variable mCond;

	// A/V 同期クロック基準 (ms)
	bool                 mClockValid;
	int64_t              mAudioEpochMs;
	int64_t              mPtsEpochMs;

	// コールバック (Play 前に設定され、以後不変)
	OnVideoDecoded       mOnDecoded;
	OnVideoDecodedPlanes mOnPlanes;
	bool                 mUseYUV;
	float                mVolume;

public:
	tTVPMpeg1MoviePlayer()
	: mPlm(nullptr), mWidth(0), mHeight(0), mFps(0.0), mDurationUs(0)
	, mHasAudio(false), mAudioSampleRate(0)
	, mState(stStopped), mTerminate(false), mDoSeek(false), mSeekUs(0)
	, mCurPosUs(0), mLoop(false), mClockValid(false), mAudioEpochMs(0), mPtsEpochMs(0)
	, mUseYUV(false), mVolume(1.0f)
	{}

	~tTVPMpeg1MoviePlayer() override
	{
		StopThread();
		if (mPlm) { plm_destroy(mPlm); mPlm = nullptr; }
		for (AudioBuf *b : mAudioPool) delete b;
	}

	// data の所有権を奪って pl_mpeg を初期化する。失敗で false。
	bool Init(std::vector<uint8_t> &&data, bool preferYUV)
	{
		mData = std::move(data);
		if (mData.empty()) return false;

		// free_when_done=FALSE: バッファ所有は mData (このクラス) が持つ
		plm_t *plm = plm_create_with_memory(mData.data(), mData.size(), FALSE);
		if (!plm) return false;
		if (!plm_has_headers(plm)) { plm_destroy(plm); return false; }
		plm_set_loop(plm, FALSE);   // ループは手前で手動制御 (EOS→rewind)

		mWidth  = plm_get_width(plm);
		mHeight = plm_get_height(plm);
		mFps    = plm_get_framerate(plm);
		if (mFps <= 0.0) mFps = 30.0;
		mDurationUs = (int64_t)(plm_get_duration(plm) * 1000000.0);

		// 音声 (MP2)。pl_mpeg は常に 2ch interleaved float32。
		if (plm_get_num_audio_streams(plm) > 0) {
			plm_set_audio_enabled(plm, TRUE);
			mAudioSampleRate = plm_get_samplerate(plm);
			if (mAudioSampleRate > 0 &&
			    mSink.Setup(2, mAudioSampleRate, 32, IAudioSink::PCM_F32)) {
				mHasAudio = true;
			} else {
				plm_set_audio_enabled(plm, FALSE);
			}
		} else {
			plm_set_audio_enabled(plm, FALSE);
		}

		if (mWidth <= 0 || mHeight <= 0) { plm_destroy(plm); return false; }
		mPlm = plm;
		mUseYUV = preferYUV;   // pl_mpeg は I420 native なので preferYUV をそのまま供給可能

		// デコードスレッド開始 (Play まではアイドル)
		mTerminate = false;
		mState = stStopped;
		mThread = std::thread(&tTVPMpeg1MoviePlayer::ThreadMain, this);
		return true;
	}

	//--- iTVPMoviePlayer ---------------------------------------------------
	void Play(bool loop = false) override
	{
		std::lock_guard<std::mutex> lk(mMtx);
		mLoop = loop;
		bool fresh = (mState == stStopped || mState == stEnded);
		if (mState == stEnded) { mDoSeek = true; mSeekUs = 0; }
		if (fresh) mClockValid = false;   // 新規再生はクロック基準を取り直す
		mState = stPlaying;
		mSink.Start();
		mCond.notify_all();
	}
	void Stop() override
	{
		std::lock_guard<std::mutex> lk(mMtx);
		mState = stStopped;
		mClockValid = false;
		mSink.Stop();
		mCond.notify_all();
	}
	void Pause() override
	{
		std::lock_guard<std::mutex> lk(mMtx);
		if (mState == stPlaying) { mState = stPaused; mSink.Stop(); }
		mCond.notify_all();
	}
	void Resume() override
	{
		std::lock_guard<std::mutex> lk(mMtx);
		if (mState == stPaused) { mState = stPlaying; mSink.Start(); }
		mCond.notify_all();
	}
	void Seek(int64_t posUs) override
	{
		std::lock_guard<std::mutex> lk(mMtx);
		mSeekUs = posUs;
		mDoSeek = true;
		mCond.notify_all();
	}
	void SetLoop(bool loop) override { mLoop = loop; }

	int32_t Width() const override  { return mWidth; }
	int32_t Height() const override { return mHeight; }
	int64_t Duration() const override { return mDurationUs; }
	int64_t Position() const override { return mCurPosUs.load(); }
	bool IsPlaying() const override { return mState.load() == stPlaying; }
	bool Loop() const override { return mLoop.load(); }

	void SetOnVideoDecoded(OnVideoDecoded callback) override { mOnDecoded = callback; }
	void SetOnVideoDecodedPlanes(OnVideoDecodedPlanes callback) override { mOnPlanes = callback; }
	bool SupportsPlanes() const override { return mUseYUV; }

	bool IsAudioAvailable() const override { return mHasAudio; }

	void SetVolume(float volume) override { mVolume = volume; mSink.SetVolume(volume); }
	float Volume() const override { return mVolume; }

private:
	//--- スレッド ----------------------------------------------------------
	void StopThread()
	{
		mTerminate = true;
		{ std::lock_guard<std::mutex> lk(mMtx); mCond.notify_all(); }
		if (mThread.joinable()) mThread.join();
	}

	bool Interrupted()
	{
		return mTerminate.load() || mDoSeek.load() || mState.load() != stPlaying;
	}

	// 再生済み位置 (ms)。未確立/音声なしは -1。
	int64_t AudioClockMs()
	{
		if (!mHasAudio || !mClockValid || mAudioSampleRate <= 0) return -1;
		int64_t played = mSink.GetSamplesPlayed();
		if (played < 0) return -1;
		int64_t playedMs = played * 1000 / mAudioSampleRate;
		return mPtsEpochMs + (playedMs - mAudioEpochMs);
	}

	AudioBuf *AcquireAudioBuf()
	{
		if (!mAudioFree.empty()) { AudioBuf *b = mAudioFree.back(); mAudioFree.pop_back(); return b; }
		AudioBuf *b = new AudioBuf();
		mAudioPool.push_back(b);
		return b;
	}
	size_t AudioInflight() const { return mAudioPool.size() - mAudioFree.size(); }

	// 再生済みコピーを回収し、シンクが枯れない程度に音声フレームを供給する。
	void PumpAudio()
	{
		if (!mHasAudio) return;
		void *p = nullptr;
		while (mSink.TryPopConsumed(&p)) if (p) mAudioFree.push_back(static_cast<AudioBuf*>(p));
		// in-flight を上限で絞る (~1s ぶん。溜め過ぎると seek 追従が鈍る)
		const size_t kMaxInflight = 32;
		while (AudioInflight() < kMaxInflight) {
			plm_samples_t *s = plm_decode_audio(mPlm);
			if (!s) break;   // 今は音声フレーム無し
			AudioBuf *b = AcquireAudioBuf();
			size_t n = (size_t)s->count * 2;   // 2ch interleaved
			b->data.assign(s->interleaved, s->interleaved + n);
			mSink.Enqueue(b->data.data(), n * sizeof(float), false, b);
		}
	}

	void ThreadMain()
	{
		int64_t prevPtsMs = 0;
		bool havePrev = false;

		for (;;) {
			{
				std::unique_lock<std::mutex> lk(mMtx);
				mCond.wait(lk, [&]{ return mTerminate.load() || mDoSeek.load() || mState.load() == stPlaying; });
			}
			if (mTerminate) break;

			if (mDoSeek.exchange(false)) {
				plm_seek_frame(mPlm, mSeekUs.load() / 1000000.0, TRUE);
				mSink.Flush();
				mCurPosUs = mSeekUs.load();
				havePrev = false;
				mClockValid = false;
				continue;
			}
			if (mState.load() != stPlaying) continue;

			// 音声を先に供給してシンクを満たす (マスタクロック源)
			if (mHasAudio) PumpAudio();

			plm_frame_t *frame = plm_decode_video(mPlm);
			if (!frame) {
				// 終端。ループ指定ならクロックを取り直して先頭へ、でなければ Ended。
				if (mLoop.load()) {
					plm_rewind(mPlm);
					mSink.Flush();
					mCurPosUs = 0;
					havePrev = false;
					mClockValid = false;
					continue;
				}
				mState = stEnded;
				mCurPosUs = mDurationUs;
				continue;
			}

			int64_t ptsMs = (int64_t)(frame->time * 1000.0);

			if (mHasAudio) {
				if (!mClockValid) {
					int64_t played = mSink.GetSamplesPlayed();
					mAudioEpochMs = (played > 0 && mAudioSampleRate > 0)
					                ? played * 1000 / mAudioSampleRate : 0;
					mPtsEpochMs = ptsMs;
					mClockValid = true;
				}
				// 音声クロックがこのフレームの pts に追いつくまで、音声を供給しつつ待つ
				for (;;) {
					if (Interrupted()) break;
					PumpAudio();
					int64_t clk = AudioClockMs();
					if (clk < 0 || clk >= ptsMs) break;
					int64_t wait = ptsMs - clk;
					if (wait > 100) wait = 100;
					std::unique_lock<std::mutex> lk(mMtx);
					mCond.wait_for(lk, std::chrono::milliseconds(wait), [&]{ return Interrupted(); });
				}
			} else {
				// 音声なし: フレーム間 delta で提示ペースを作る
				int64_t delta = havePrev ? (ptsMs - prevPtsMs) : 0;
				prevPtsMs = ptsMs;
				havePrev = true;
				if (delta > 0) {
					if (delta > 1000) delta = 1000;
					std::unique_lock<std::mutex> lk(mMtx);
					mCond.wait_for(lk, std::chrono::milliseconds(delta), [&]{ return Interrupted(); });
				}
			}
			if (mTerminate) break;
			if (mState.load() != stPlaying) continue;   // stop/pause/seek 割り込み

			DeliverFrame(frame);
			mCurPosUs = ptsMs * 1000;
		}
	}

	void DeliverFrame(plm_frame_t *frame)
	{
		if (mUseYUV && mOnPlanes) {
			// I420 plane 直渡し (presenter が GPU で YUV→RGB)。plane.width が stride
			// (packed) を兼ねる。順序は Y, U(Cb), V(Cr)。
			iTVPMoviePlayer::VideoPlaneFrame f;
			f.width  = (int)frame->width;
			f.height = (int)frame->height;
			f.format = iTVPMoviePlayer::VPF_I420;
			f.planeCount = 3;
			f.planes[0] = { frame->y.data,  (int)frame->y.width,  (int)frame->y.height,  (int)frame->y.width };
			f.planes[1] = { frame->cb.data, (int)frame->cb.width, (int)frame->cb.height, (int)frame->cb.width };
			f.planes[2] = { frame->cr.data, (int)frame->cr.width, (int)frame->cr.height, (int)frame->cr.width };
			mOnPlanes(f);
		} else if (mOnDecoded) {
			// ARGB (BGRA) 経路: consumer が updater(dest,pitch) を同期的に呼ぶ。
			int w = (int)frame->width, h = (int)frame->height;
			mOnDecoded(w, h, [frame, w, h](char *dest, int pitch) {
				plm_frame_to_bgra(frame, (uint8_t*)dest, pitch);
				// pl_mpeg は B/G/R のみ書き α (4バイト目) を書かないので不透明で埋める
				// (WIN 版 Mpeg1Video と同じ。レイヤ経路で透明化しないため)。
				for (int y = 0; y < h; ++y) {
					uint8_t *row = (uint8_t*)dest + (ptrdiff_t)y * pitch;
					for (int x = 0; x < w; ++x) row[x * 4 + 3] = 0xFF;
				}
			});
		}
	}
};

} // anonymous namespace

//---------------------------------------------------------------------------
bool TVPIsMpeg1Path(const char *utf8name)
{
	if (!utf8name) return false;
	size_t len = std::strlen(utf8name);
	auto ends_with = [&](const char *ext) {
		size_t el = std::strlen(ext);
		if (len < el) return false;
		const char *s = utf8name + (len - el);
		for (size_t i = 0; i < el; ++i)
			if (std::tolower((unsigned char)s[i]) != ext[i]) return false;
		return true;
	};
	return ends_with(".mpg") || ends_with(".mpeg");
}

//---------------------------------------------------------------------------
iTVPMoviePlayer *TVPCreateMpeg1MoviePlayer(std::vector<uint8_t> &&data, bool preferYUV)
{
	tTVPMpeg1MoviePlayer *player = new tTVPMpeg1MoviePlayer();
	if (player->Init(std::move(data), preferYUV)) return player;
	delete player;
	return nullptr;
}
