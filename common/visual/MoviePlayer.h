#ifndef _MOVIE_PLAYER_H__
#define _MOVIE_PLAYER_H__

#include <stdint.h>
#include <functional>

class IMovieReadStream;

/**
 * レイヤ上で動画再生するための汎用インターフェース
*/
class iTVPMoviePlayer {

public:
  
  virtual ~iTVPMoviePlayer() {}

  // --------------------------------------------------------------------

  virtual void Play(bool loop = false) = 0;
  virtual void Stop() = 0;
  virtual void Pause()  = 0;
  virtual void Resume()  = 0;
  virtual void Seek(int64_t posUs)  = 0;
  virtual void SetLoop(bool loop)  = 0;

  virtual int32_t Width() const  = 0;
  virtual int32_t Height() const  = 0;
  virtual int64_t Duration() const  = 0;
  virtual int64_t Position() const  = 0;
  virtual bool IsPlaying() const  = 0;
  virtual bool Loop() const  = 0;

  // --------------------------------------------------------------------

  // Video Decoded Frame (ARGB 系: updater が dest を packed RGBA で埋める最速経路)
  typedef std::function<void(char *dest, int pitch)> DestUpdater;
  typedef std::function<void(int w, int h, DestUpdater updater)> OnVideoDecoded;
  virtual void SetOnVideoDecoded(OnVideoDecoded callback) = 0;

  // Video Decoded Frame (YUV plane 経路: GPU 側で YUV→RGB する presenter 向け)。
  // planes[i].data はコールバックから return した時点で無効になるので同期的に copy すること。
  // I420: planeCount=3 (Y=w×h / U=w/2×h/2 / V=w/2×h/2)。NV12: planeCount=2 (Y / UV interleaved)。
  static const int TVP_VIDEO_PLANE_MAX = 4;
  enum VideoPlaneFormat { VPF_UNKNOWN = 0, VPF_I420, VPF_NV12, VPF_NV21 };
  struct VideoPlaneFrame {
    int width;
    int height;
    VideoPlaneFormat format;
    int planeCount;
    struct PlaneRef { const uint8_t *data; int width; int height; int stride; } planes[TVP_VIDEO_PLANE_MAX];
  };
  typedef std::function<void(const VideoPlaneFrame &frame)> OnVideoDecodedPlanes;
  // 既定 no-op (YUV 非対応の実装は SetOnVideoDecoded のみ実装すればよい)。SetOnVideoDecoded と
  // 排他 (最後に呼んだ方のみ有効)。videoColorFormat を YUV に設定した player でのみ呼ばれる。
  virtual void SetOnVideoDecodedPlanes(OnVideoDecodedPlanes callback) {}
  // この player が YUV plane 経路 (SetOnVideoDecodedPlanes) を実際に供給できるか。
  virtual bool SupportsPlanes() const { return false; }

  // audio info
  virtual bool IsAudioAvailable() const = 0;

  // VolumeControl
  virtual void SetVolume(float volume) = 0;
  virtual float Volume() const = 0;

  // --------------------------------------------------------------------
  // 以下は自前の表示手段/メインループ駆動を持つ実装 (wasm の <video> DOM
  // オーバレイ等) 向けのオプショナルフック。通常のデコーダ実装は
  // デフォルト (no-op) のままでよい。

  // open 直後にモードが通知される。layer=true ならフレームを
  // OnVideoDecoded で供給し、自前表示 (DOM オーバレイ等) は行わない
  virtual void SetLayerMode(bool layer) {}

  // メインスレッドから毎フレーム呼ばれる。デコーダスレッドを持たない
  // 実装がフレーム引き渡し (OnVideoDecoded 呼び出し) を行う場所
  virtual void Pump() {}

  // 非 layer モードの表示制御 (VideoOverlay.visible)。自前表示を
  // 持つ実装のみ意味を持つ
  virtual void SetOverlayVisible(bool visible) {}

};

// preferYUV=true で、可能なら YUV plane 出力 (COLOR_I420) の player を作る (mixer/presenter 経路用)。
// YUV 非対応の decoder / backend では自動的に ARGB にフォールバックする (SupportsPlanes()=false)。
extern iTVPMoviePlayer*TVPCreateMoviePlayer(const tjs_char *filename, bool preferYUV=false);
extern iTVPMoviePlayer*TVPCreateMoviePlayer(IMovieReadStream *stream, const char *filename, bool preferYUV=false);

#endif
