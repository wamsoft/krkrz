#pragma once

#include <cstdint>
#include <cstddef>

//------------------------------------------------------
// ファイルデータ用のアロケータ
//------------------------------------------------------

extern "C" void *file_malloc(size_t size);
extern "C" void file_free(void *p);

// アロケータ初期化（TVPInitializeBaseSystems() から呼ばれる）
void TVPInitializeFileAllocator();

// テレメトリ取得用 (TVPHeapDump / System.dumpHeap 等から参照)。
// 未初期化の場合は nullptr を返す。
class iTVPMemoryAllocator;
iTVPMemoryAllocator *TVPGetFileAllocator();

/**
 * @brief ファイル読み込み用バッファ（共有用にサイズ情報も保持）
 *        生成時にサイズ確定の不変オブジェクト。
 */
class tTJSBinaryStreamBuffer {
	unsigned char *mBuffer;
	size_t mSize;

	tTJSBinaryStreamBuffer() : mBuffer(nullptr), mSize(0) {}

public:
	static tTJSBinaryStreamBuffer *create(size_t size) {
		tTJSBinaryStreamBuffer *buf = new tTJSBinaryStreamBuffer();
		buf->mBuffer = (unsigned char *)file_malloc(size);
		if (buf->mBuffer) {
			buf->mSize = size;
			return buf;
		}
		delete buf;
		return nullptr;
	}

	~tTJSBinaryStreamBuffer() {
		if (mBuffer) file_free(mBuffer);
	}

	const unsigned char *buffer() const { return mBuffer; }
	size_t size() const { return mSize; }
};
