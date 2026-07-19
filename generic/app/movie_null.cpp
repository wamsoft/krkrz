#include "tjsCommHead.h"
#include "MoviePlayer.h"

// KRKRZ_USE_MOVIE=OFF 用のスタブ実装。
// movie-player (external/movie-player) を組み込まない構成 (wasm 等) では
// VideoOverlay のオープンは常に失敗する (呼び出し側は nullptr を
// 「オープン失敗」として扱う)。

// CreatePlayer (ファイルパス版)
iTVPMoviePlayer*
TVPCreateMoviePlayer(const tjs_char *filename)
{
  return nullptr;
}

// CreatePlayer (ストリーム版)
iTVPMoviePlayer*
TVPCreateMoviePlayer(IMovieReadStream *stream, const char *filename)
{
  return nullptr;
}
