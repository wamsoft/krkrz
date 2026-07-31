#include "tjsCommHead.h"
#include "MsgIntf.h"
#include "VideoOvlImpl.h"
#include "CharacterSet.h"

#include "IMoviePlayer.h"
#include "MovieAudioSinkAdapter.h"
#include "Mpeg1MoviePlayer.h"       // MPEG-1 (.mpg/.mpeg) = pl_mpeg 内蔵プレイヤ

#include <vector>
#include <cstdio>

// ラッピング処理
class tTVPMoviePlayer : public iTVPMoviePlayer {
 public:

  tTVPMoviePlayer()
  : mPlayer(nullptr), mUseYUV(false)
  {
  }

  virtual ~tTVPMoviePlayer() {
    // 先に movie-player を破棄してから sink を破棄 (sink には残った
    // pending DecodedBuffer の参照があるが、movie-player 破棄で decoder
    // も消えるので参照先を辿らない)
    delete mPlayer;
  }

  bool Open(const char *filename, bool preferYUV) {
    IMoviePlayer::InitParam param;
    param.Init();
    param.audioSink        = &mSink;
    // preferYUV: YUV plane 直渡し (COLOR_I420 = NOCONV、libyuv 変換なし)。presenter が
    // GPU 側で YUV→RGB する経路用。generic backend (webm/mpeg=libvpx/pl_mpeg) は I420 出力。
    mUseYUV = preferYUV;
    param.videoColorFormat = preferYUV ? IMoviePlayer::COLOR_I420 : IMoviePlayer::COLOR_BGRA;
    mPlayer = IMoviePlayer::CreateMoviePlayer(filename, param);
    if (!mPlayer) {
      return false;
    }
    return true;
  }

  bool OpenStream(IMovieReadStream *stream, bool preferYUV) {
    IMoviePlayer::InitParam param;
    param.Init();
    param.audioSink        = &mSink;
    mUseYUV = preferYUV;
    param.videoColorFormat = preferYUV ? IMoviePlayer::COLOR_I420 : IMoviePlayer::COLOR_BGRA;
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

  virtual void SetOnVideoDecodedPlanes(OnVideoDecodedPlanes callback) {
    if (!mPlayer) return;
    // IMoviePlayer::VideoFrameInfo → iTVPMoviePlayer::VideoPlaneFrame へ変換して転送。
    // src.width/height は coded 寸法 (16 アライン padding 付き) なので、表示寸法へクロップする
    // (crop しないと右端/下端に未定義 chroma 由来の緑帯が出る。plane stride はそのまま)。
    IMoviePlayer *player = mPlayer;
    mPlayer->SetOnVideoDecodedPlanes([callback, player](const IMoviePlayer::VideoFrameInfo &src) {
      iTVPMoviePlayer::VideoPlaneFrame f;
      IMoviePlayer::VideoFormat fmt{};
      player->GetVideoFormat(&fmt);
      f.width  = (fmt.width  > 0 && fmt.width  <= src.width ) ? fmt.width  : src.width;
      f.height = (fmt.height > 0 && fmt.height <= src.height) ? fmt.height : src.height;
      switch (src.colorFormat) {
        case IMoviePlayer::COLOR_I420: f.format = iTVPMoviePlayer::VPF_I420; break;
        case IMoviePlayer::COLOR_NV12: f.format = iTVPMoviePlayer::VPF_NV12; break;
        case IMoviePlayer::COLOR_NV21: f.format = iTVPMoviePlayer::VPF_NV21; break;
        default:                       f.format = iTVPMoviePlayer::VPF_UNKNOWN; break;
      }
      f.planeCount = src.planeCount;
      if (f.planeCount > iTVPMoviePlayer::TVP_VIDEO_PLANE_MAX)
        f.planeCount = iTVPMoviePlayer::TVP_VIDEO_PLANE_MAX;
      for (int i = 0; i < f.planeCount; ++i) {
        f.planes[i].data   = src.planes[i].data;
        f.planes[i].width  = src.planes[i].width;
        f.planes[i].height = src.planes[i].height;
        f.planes[i].stride = src.planes[i].stride;
      }
      callback(f);
    });
  }

  virtual bool SupportsPlanes() const { return mUseYUV; }

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
  bool mUseYUV;   // YUV plane 経路 (COLOR_I420) で開いたか
};

// CreatePlayer (ファイルパス版)
iTVPMoviePlayer*
TVPCreateMoviePlayer(const tjs_char *filename, bool preferYUV)
{
  std::string nfilename;
  TVPUtf16ToUtf8(nfilename, filename);

  // MPEG-1 (.mpg/.mpeg) は movie-player (webm 専用) では扱えないので pl_mpeg 内蔵
  // プレイヤへ振り分ける。pl_mpeg は全データを要するのでファイルを丸読みする。
  if (TVPIsMpeg1Path(nfilename.c_str())) {
    FILE *fp = fopen(nfilename.c_str(), "rb");
    if (!fp) return nullptr;
    fseek(fp, 0, SEEK_END);
    long sz = ftell(fp);
    fseek(fp, 0, SEEK_SET);
    std::vector<uint8_t> data;
    if (sz > 0) {
      data.resize((size_t)sz);
      size_t got = fread(data.data(), 1, (size_t)sz, fp);
      data.resize(got);
    }
    fclose(fp);
    return TVPCreateMpeg1MoviePlayer(std::move(data), preferYUV);
  }

  tTVPMoviePlayer *player =  new tTVPMoviePlayer();
  if (player->Open(nfilename.c_str(), preferYUV)) {
    return player;
  }
  delete player;
  return nullptr;
}

// CreatePlayer (ストリーム版)
iTVPMoviePlayer*
TVPCreateMoviePlayer(IMovieReadStream *stream, const char *filename, bool preferYUV)
{
  // MPEG-1 (.mpg/.mpeg) は pl_mpeg 内蔵プレイヤへ。pl_mpeg は全データを要するので
  // ストリームを丸読みする。所有権契約 (成功時のみ callee が stream を release、
  // 失敗時は呼び出し側が release) を守るため、読み込みでは release せず、player 生成が
  // 成功した時だけここで release する。
  if (TVPIsMpeg1Path(filename)) {
    std::vector<uint8_t> data;
    size_t sz = stream->Size();
    if (sz > 0) {
      data.resize(sz);
      stream->Seek(0, SEEK_SET);
      size_t got = 0;
      while (got < sz) {
        size_t r = stream->Read(data.data() + got, sz - got);
        if (r == 0) break;
        got += r;
      }
      data.resize(got);
    }
    iTVPMoviePlayer *p = data.empty()
      ? nullptr : TVPCreateMpeg1MoviePlayer(std::move(data), preferYUV);
    if (p) { stream->Release(); return p; }
    return nullptr;   // 失敗: stream は呼び出し側が release する
  }

  tTVPMoviePlayer *player = new tTVPMoviePlayer();
  if (player->OpenStream(stream, preferYUV)) {
    return player;
  }
  delete player;
  return nullptr;
}
