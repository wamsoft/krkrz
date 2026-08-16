#ifndef __FONT_STREAM_H__
#define __FONT_STREAM_H__

#include <memory>
#include "tjsCommHead.h"

class tTJSBinaryStreamBuffer;

//---------------------------------------------------------------------------
// 共有オンメモリ・フォントストリーム提供
// (旧 FreeType.cpp 内 OpenFontFile/_fontfiles/_fontlist の汎用化)
//
// 同一ストレージのフォントは StorageCache 上の 1 バッファを全消費者
// (classic FreeType / glyphware / プラグイン) で共有し、MRU (既定10) で
// バッファを pin する。XP3 内フォント対応・全ファイルオンメモリ。
//---------------------------------------------------------------------------

// フォントを共有バッファ上のストリームビューとして開く (classic FreeType 等
// ストリーム消費者向け)。キャッシュ対象外 (拡張子閾値超え等) の場合は
// 従来どおり直接ファイルストリームを返す (共有されない)。
// ストレージが開けない場合は例外。
std::shared_ptr<iTJSBinaryStream> TVPGetFontStream(const ttstr &storage);

// フォントの共有バッファそのものを取得する (glyphware 等、連続メモリを
// 直接参照するゼロコピー消費者向け)。戻り値の shared_ptr が生存する間
// バッファは保持される。キャッシュ対象外の場合も全読みした専用バッファを
// 返す (このバッファも本 API 内の弱参照マップで共有される)。
// ストレージが開けない場合は例外。
std::shared_ptr<tTJSBinaryStreamBuffer> TVPGetFontStreamBuffer(const ttstr &storage);

// MRU pin 数の上限 (既定 10)。0 で pin 無効 (弱参照のみ)。
void TVPSetFontStreamCacheMax(size_t max);

#endif // __FONT_STREAM_H__
