//---------------------------------------------------------------------------
// pl_mpeg ベースの MPEG-1 (.mpg/.mpeg) ムービプレイヤ (generic/SDL 用)。
//
// external/movie-player は webm (VP8/9 + vorbis/opus) 専用なので、WIN 版に内蔵した
// pl_mpeg (win32/movie/Mpeg1Video.cpp) と同等の MPEG-1 再生を generic/SDL でも
// 使えるようにする。iTVPMoviePlayer を直接実装し、自前のデコードスレッドで
// VideoOverlay へフレームを供給する (webm 経路と同じ consumer 契約)。
//---------------------------------------------------------------------------
#ifndef _MPEG1_MOVIE_PLAYER_H__
#define _MPEG1_MOVIE_PLAYER_H__

#include "MoviePlayer.h"
#include <vector>
#include <cstdint>

// data (MPEG-1 プログラムストリーム全体) から MPEG-1 プレイヤを生成する。
// pl_mpeg はシークに全データを要するため、呼び出し側で全読みしてから渡す
// (data の所有権を奪う = move)。生成失敗 (ヘッダ不正等) 時は nullptr。
// preferYUV=true かつ映像ありのとき SupportsPlanes()=true (pl_mpeg は I420 native)。
iTVPMoviePlayer *TVPCreateMpeg1MoviePlayer(std::vector<uint8_t> &&data, bool preferYUV);

// 拡張子 (.mpg / .mpeg) 判定 (大文字小文字無視)。null / 拡張子無しは false。
bool TVPIsMpeg1Path(const char *utf8name);

#endif // _MPEG1_MOVIE_PLAYER_H__
