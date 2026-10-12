//---------------------------------------------------------------------------
// 実行ファイルに結合された XP3 アーカイブの探索 (ExecutableOverlay.h 参照)
//---------------------------------------------------------------------------
#include "ExecutableOverlay.h"

#include <algorithm>
#include <cstring>
#include <vector>

namespace {

uint16_t le16(const uint8_t *p) { return (uint16_t)(p[0] | (p[1] << 8)); }
uint32_t le32(const uint8_t *p) { return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24); }
uint64_t le64(const uint8_t *p) { return (uint64_t)le32(p) | ((uint64_t)le32(p + 4) << 32); }

bool readExact(const tTVPReadAtFunc &read, uint64_t pos, void *buf, size_t len)
{
	return read(pos, buf, len) == len;
}

// PE: 各セクションの PointerToRawData + SizeOfRawData の最大
// (Authenticode の証明書表はこの後ろに付くので、探索の範囲に入るだけで害はない)
uint64_t peImageEnd(const tTVPReadAtFunc &read, uint64_t fileSize)
{
	uint8_t dos[64];
	if(!readExact(read, 0, dos, sizeof(dos))) return 0;
	const uint32_t peOfs = le32(dos + 0x3c);
	uint8_t coff[24];
	if((uint64_t)peOfs + sizeof(coff) > fileSize || !readExact(read, peOfs, coff, sizeof(coff))) return 0;
	if(memcmp(coff, "PE\0\0", 4) != 0) return 0;
	const uint16_t numSections = le16(coff + 4 + 2);
	const uint16_t optSize = le16(coff + 4 + 16);
	if(numSections == 0 || numSections > 96) return 0;
	std::vector<uint8_t> table((size_t)numSections * 40);
	const uint64_t tableOfs = (uint64_t)peOfs + 24 + optSize;
	if(tableOfs + table.size() > fileSize || !readExact(read, tableOfs, table.data(), table.size())) return 0;
	uint64_t end = tableOfs + table.size();
	for(uint16_t i = 0; i < numSections; i++) {
		const uint8_t *s = table.data() + (size_t)i * 40;
		const uint32_t rawSize = le32(s + 16);
		const uint32_t rawPtr = le32(s + 20);
		if(rawSize) end = std::max<uint64_t>(end, (uint64_t)rawPtr + rawSize);
	}
	return end <= fileSize ? end : 0;
}

// ELF (リトルエンディアン、32 / 64bit): プログラムヘッダ・セクションヘッダの表と、
// 中身のあるセグメント・セクション (SHT_NOBITS 以外) の終わりの最大
uint64_t elfImageEnd(const tTVPReadAtFunc &read, uint64_t fileSize)
{
	uint8_t h[64];
	if(!readExact(read, 0, h, sizeof(h))) return 0;
	const bool is64 = h[4] == 2;
	if((h[4] != 1 && h[4] != 2) || h[5] != 1) return 0;   // リトルエンディアンのみ
	uint64_t phoff, shoff;
	uint16_t phentsize, phnum, shentsize, shnum;
	if(is64) {
		phoff = le64(h + 0x20); shoff = le64(h + 0x28);
		phentsize = le16(h + 0x36); phnum = le16(h + 0x38);
		shentsize = le16(h + 0x3a); shnum = le16(h + 0x3c);
	} else {
		phoff = le32(h + 0x1c); shoff = le32(h + 0x20);
		phentsize = le16(h + 0x2a); phnum = le16(h + 0x2c);
		shentsize = le16(h + 0x2e); shnum = le16(h + 0x30);
	}
	uint64_t end = is64 ? 64 : 52;
	if(phnum && phentsize) {
		const uint64_t size = (uint64_t)phnum * phentsize;
		if(phoff + size > fileSize) return 0;
		std::vector<uint8_t> t((size_t)size);
		if(!readExact(read, phoff, t.data(), t.size())) return 0;
		end = std::max(end, phoff + size);
		for(uint16_t i = 0; i < phnum; i++) {
			const uint8_t *p = t.data() + (size_t)i * phentsize;
			const uint64_t ofs = is64 ? le64(p + 8) : le32(p + 4);
			const uint64_t fsz = is64 ? le64(p + 0x20) : le32(p + 0x10);
			if(fsz) end = std::max(end, ofs + fsz);
		}
	}
	if(shnum && shentsize) {
		const uint64_t size = (uint64_t)shnum * shentsize;
		if(shoff + size > fileSize) return 0;
		std::vector<uint8_t> t((size_t)size);
		if(!readExact(read, shoff, t.data(), t.size())) return 0;
		end = std::max(end, shoff + size);
		for(uint16_t i = 0; i < shnum; i++) {
			const uint8_t *s = t.data() + (size_t)i * shentsize;
			const uint32_t type = le32(s + 4);
			if(type == 8 /* SHT_NOBITS */) continue;
			const uint64_t ofs = is64 ? le64(s + 0x18) : le32(s + 0x10);
			const uint64_t sz = is64 ? le64(s + 0x20) : le32(s + 0x14);
			if(sz) end = std::max(end, ofs + sz);
		}
	}
	return end <= fileSize ? end : 0;
}

// XP3 のヘッダ。コード中にそのまま書くと exe 自身の中に現れるので 2 つに分けてある
// (XP3Archive.cpp と同じ扱い)
const uint8_t *xp3Mark()
{
	static uint8_t mark[11];
	static bool init = false;
	if(!init) {
		static const uint8_t m1[] = { 0x58, 0x50, 0x33, 0x0d, 0x0a, 0x20, 0x0a, 0x1a };
		static const uint8_t m2[] = { 0x8b, 0x67, 0x01 };
		memcpy(mark, m1, 8);
		memcpy(mark + 8, m2, 3);
		init = true;
	}
	return mark;
}

} // namespace

tTVPExecutableKind TVPGetExecutableKind(const uint8_t *head, size_t len)
{
	if(len >= 2 && head[0] == 'M' && head[1] == 'Z') return tTVPExecutableKind::PE;
	if(len >= 4 && head[0] == 0x7f && head[1] == 'E' && head[2] == 'L' && head[3] == 'F') return tTVPExecutableKind::ELF;
	return tTVPExecutableKind::None;
}

uint64_t TVPGetExecutableImageEnd(const tTVPReadAtFunc &read, uint64_t fileSize)
{
	uint8_t head[4];
	if(!readExact(read, 0, head, sizeof(head))) return 0;
	switch(TVPGetExecutableKind(head, sizeof(head))) {
	case tTVPExecutableKind::PE:  return peImageEnd(read, fileSize);
	case tTVPExecutableKind::ELF: return elfImageEnd(read, fileSize);
	default:                      return 0;
	}
}

bool TVPFindXP3MarkFrom(const tTVPReadAtFunc &read, uint64_t from, uint64_t fileSize, uint64_t &offset)
{
	const uint8_t *mark = xp3Mark();
	from = (from + 15) & ~(uint64_t)15;
	const size_t chunk = 256 * 1024;
	std::vector<uint8_t> buf(chunk + 16);
	for(uint64_t pos = from; pos + 11 <= fileSize; pos += chunk) {
		const size_t want = (size_t)std::min<uint64_t>(chunk + 16, fileSize - pos);
		const size_t got = read(pos, buf.data(), want);
		if(got < 11) break;
		for(size_t p = 0; p + 11 <= got && p < chunk; p += 16) {
			if(buf[p] == 0x58 && !memcmp(buf.data() + p, mark, 11)) {
				offset = pos + p;
				return true;
			}
		}
	}
	return false;
}

bool TVPFindXP3InExecutable(const tTVPReadAtFunc &read, uint64_t fileSize, uint64_t &offset)
{
	uint8_t head[4];
	if(!readExact(read, 0, head, sizeof(head))) return false;
	if(TVPGetExecutableKind(head, sizeof(head)) == tTVPExecutableKind::None) return false;
	uint64_t start = TVPGetExecutableImageEnd(read, fileSize);
	// ヘッダを解釈できないときは以前と同じく先頭 (16 バイト目) から探す
	if(start == 0) start = 16;
	if(start >= fileSize) return false;   // 後ろに何も付いていない
	return TVPFindXP3MarkFrom(read, start, fileSize, offset);
}
