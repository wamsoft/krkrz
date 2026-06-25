#include "tjsCommHead.h"
#include "MsgIntf.h"
#include "VideoOvlImpl.h"
#include "CharacterSet.h"

#include "IMoviePlayer.h"
#include "MovieAudioSinkAdapter.h"

// ラッピング処理
class tTVPMoviePlayer : public iTVPMoviePlayer {
 public:

  tTVPMoviePlayer()
  : mPlayer(nullptr)
  {
  }

  virtual ~tTVPMoviePlayer() {
    // 先に movie-player を破棄してから sink を破棄 (sink には残った
    // pending DecodedBuffer の参照があるが、movie-player 破棄で decoder
    // も消えるので参照先を辿らない)
    delete mPlayer;
  }

  bool Open(const char *filename) {
    IMoviePlayer::InitParam param;
    param.Init();
    param.audioSink        = &mSink;
    param.videoColorFormat = IMoviePlayer::COLOR_BGRA;
    mPlayer = IMoviePlayer::CreateMoviePlayer(filename, param);
    if (!mPlayer) {
      return false;
    }
    return true;
  }

  bool OpenStream(IMovieReadStream *stream) {
    IMoviePlayer::InitParam param;
    param.Init();
    param.audioSink        = &mSink;
    param.videoColorFormat = IMoviePlayer::COLOR_BGRA;
    mPlayer = IMoviePlayer::CreateMoviePlayer(stream, param);
    if (!mPlayer) {
      return false;
    }
    return true;
  }

  virtual void Play(bool loop = false) {
    mPlayer->Play(loop);
  }
  virtual void Stop() {
    mPlayer->Stop();
  }
  virtual void Pause() {
    mPlayer->Pause();
  }
  virtual void Resume() {
    mPlayer->Resume();
  }
  virtual void Seek(int64_t posUs) {
    mPlayer->Seek(posUs);
  }
  virtual void SetLoop(bool loop) {
    mPlayer->SetLoop(loop);
  }

  virtual int32_t Width() const {
    IMoviePlayer::VideoFormat format;
    mPlayer->GetVideoFormat(&format);
    return format.width;
  }
  virtual int32_t Height() const {
    IMoviePlayer::VideoFormat format;
    mPlayer->GetVideoFormat(&format);
    return format.height;
  }
  virtual int64_t Duration() const {
    return mPlayer->Duration();
  }
  virtual int64_t Position() const {
    return mPlayer->Position();
  }
  virtual bool IsPlaying() const {
    return mPlayer->IsPlaying();
  }
  virtual bool Loop() const {
    return mPlayer->Loop();
  }

  virtual void SetOnVideoDecoded(OnVideoDecoded callback) {
    if (mPlayer) {
      mPlayer->SetOnVideoDecoded(callback);
    }
  }

  // audio info
  virtual bool IsAudioAvailable() const                  {
    return mPlayer->IsAudioAvailable();
  }

  virtual void SetVolume(float volume) {
    mPlayer->SetVolume(volume);
  }

  virtual float Volume() const {
    return mPlayer->Volume();
  }

 private:
  IMoviePlayer* mPlayer;
  // 音声出力用 sink。mPlayer より先に置いて mPlayer 破棄まで生きる。
  tTVPMovieAudioSinkAdapter mSink;
  iTVPMoviePlayer::OnVideoDecoded mVideoDecoded;
  void *mUserData;
};

// CreatePlayer (ファイルパス版)
iTVPMoviePlayer*
TVPCreateMoviePlayer(const tjs_char *filename)
{
  std::string nfilename;
  TVPUtf16ToUtf8(nfilename, filename);
  tTVPMoviePlayer *player =  new tTVPMoviePlayer();
  if (player->Open(nfilename.c_str())) {
    return player;
  }
  delete player;
  return nullptr;
}

// CreatePlayer (ストリーム版)
iTVPMoviePlayer*
TVPCreateMoviePlayer(IMovieReadStream *stream, const char *filename)
{
  tTVPMoviePlayer *player = new tTVPMoviePlayer();
  if (player->OpenStream(stream)) {
    return player;
  }
  delete player;
  return nullptr;
}
