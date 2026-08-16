//---------------------------------------------------------------------------
// 共有オンメモリ・フォントストリーム提供
// (旧 FreeType.cpp 内 OpenFontFile/_fontfiles/_fontlist の汎用化)
//---------------------------------------------------------------------------
#include "tjsCommHead.h"
#include "FontStream.h"

#include "StorageIntf.h"       // TVPGetPlacedPath / _InnerTVPCreateStream 経由の open
#include "StorageCache.h"      // TVPGetStorageCacheBuffer / TVPCreateSharedMemoryStream
#include "BinaryStreamBuffer.h"
#include "MsgIntf.h"
#include "DebugIntf.h"

#include <map>
#include <vector>
#include <algorithm>

namespace {

// path (TVPGetPlacedPath 正規化済み) -> 共有バッファの弱参照。
// バッファ実体は StorageCache 表と MRU (下記) と消費者 (FreeType face の
// ストリームビュー / glyphware FontBlob) が shared_ptr で保持する。
std::map<tjs_string, std::weak_ptr<tTJSBinaryStreamBuffer>> FontBufferMap;

// 最近使ったフォントバッファを強参照で pin する MRU (旧 _fontlist と同じ役割)。
std::vector<std::shared_ptr<tTJSBinaryStreamBuffer>> FontBufferMRU;
size_t FontStreamCacheMax = 10;

tTJSCriticalSection FontStreamCS;

// MRU 末尾 (=最新) に積み直し、上限を超えた古い pin を外す
void EntryFontBufferMRU(const std::shared_ptr<tTJSBinaryStreamBuffer> &buf)
{
	auto i = std::remove(FontBufferMRU.begin(), FontBufferMRU.end(), buf);
	FontBufferMRU.erase(i, FontBufferMRU.end());
	while (FontBufferMRU.size() > FontStreamCacheMax) {
		FontBufferMRU.erase(FontBufferMRU.begin());
	}
	if (FontStreamCacheMax > 0) FontBufferMRU.push_back(buf);
}

// 正規化パスで共有バッファを取得/生成。キャッシュ対象外は nullptr。
std::shared_ptr<tTJSBinaryStreamBuffer> GetSharedFontBuffer(const ttstr &placed)
{
	tjs_string key = placed.AsStdString();
	auto n = FontBufferMap.find(key);
	if (n != FontBufferMap.end()) {
		if (auto buf = n->second.lock()) {
			EntryFontBufferMRU(buf);
			return buf;
		}
		FontBufferMap.erase(n);
	}
	auto buf = TVPGetStorageCacheBuffer(placed, true);
	if (buf) {
		FontBufferMap[key] = buf;
		EntryFontBufferMRU(buf);
	}
	return buf;
}

// キャッシュ対象外フォールバック: 全読みして専用バッファ化 (弱参照マップで共有)
std::shared_ptr<tTJSBinaryStreamBuffer> ReadWholeFontBuffer(const ttstr &placed)
{
	std::unique_ptr<iTJSBinaryStream> stream(TVPCreateStream(placed, TJS_BS_READ));
	size_t size = static_cast<size_t>(stream->GetSize());
	std::shared_ptr<tTJSBinaryStreamBuffer> buf(tTJSBinaryStreamBuffer::create(size));
	if (!buf) TVPThrowExceptionMessage(TVPCannotOpenStorage, placed);
	unsigned char *p = const_cast<unsigned char*>(buf->buffer());
	size_t remain = size;
	while (remain > 0) {
		tjs_uint read = stream->Read(p, static_cast<tjs_uint>(remain));
		if (read == 0) break;
		remain -= read;
		p += read;
	}
	FontBufferMap[placed.AsStdString()] = buf;
	EntryFontBufferMRU(buf);
	return buf;
}

} // namespace

std::shared_ptr<iTJSBinaryStream> TVPGetFontStream(const ttstr &storage)
{
	ttstr placed = TVPGetPlacedPath(storage);
	if (placed.IsEmpty()) TVPThrowExceptionMessage(TVPCannotOpenStorage, storage);

	tTJSCriticalSectionHolder lock(FontStreamCS);
	if (auto buf = GetSharedFontBuffer(placed)) {
		return std::shared_ptr<iTJSBinaryStream>(TVPCreateSharedMemoryStream(std::move(buf)));
	}
	// キャッシュ対象外 (閾値超え等): 従来どおり直接ストリーム (共有されない)
	return std::shared_ptr<iTJSBinaryStream>(TVPCreateStream(placed, TJS_BS_READ));
}

std::shared_ptr<tTJSBinaryStreamBuffer> TVPGetFontStreamBuffer(const ttstr &storage)
{
	ttstr placed = TVPGetPlacedPath(storage);
	if (placed.IsEmpty()) TVPThrowExceptionMessage(TVPCannotOpenStorage, storage);

	tTJSCriticalSectionHolder lock(FontStreamCS);
	if (auto buf = GetSharedFontBuffer(placed)) return buf;
	// キャッシュ対象外: 全読み専用バッファ (ゼロコピー消費者はバイト必須)
	return ReadWholeFontBuffer(placed);
}

void TVPSetFontStreamCacheMax(size_t max)
{
	tTJSCriticalSectionHolder lock(FontStreamCS);
	FontStreamCacheMax = max;
	while (FontBufferMRU.size() > FontStreamCacheMax) {
		FontBufferMRU.erase(FontBufferMRU.begin());
	}
}
