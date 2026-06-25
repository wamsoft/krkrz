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

  // Video Decoded Frame
  typedef std::function<void(char *dest, int pitch)> DestUpdater;
  typedef std::function<void(int w, int h, DestUpdater updater)> OnVideoDecoded;
  virtual void SetOnVideoDecoded(OnVideoDecoded callback) = 0;

  // audio info
  virtual bool IsAudioAvailable() const = 0;

  // VolumeControl
  virtual void SetVolume(float volume) = 0;
  virtual float Volume() const = 0;

};

extern iTVPMoviePlayer*TVPCreateMoviePlayer(const tjs_char *filename);
extern iTVPMoviePlayer*TVPCreateMoviePlayer(IMovieReadStream *stream, const char *filename);

#endif
