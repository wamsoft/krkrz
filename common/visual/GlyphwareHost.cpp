#include "tjsCommHead.h"
#include "GlyphwareHost.h"

#ifdef KRKRZ_USE_GLYPHWARE

#include "StorageIntf.h"   // TVPIsExistentStorage
#include "FontStream.h"    // TVPGetFontStreamBuffer (共有オンメモリバッファ)
#include "../base/BinaryStreamBuffer.h"
#include "CharacterSet.h"  // TVPUtf8ToUtf16 / TVPUtf16ToUtf8
#include "FontSystem.h"    // TVPFontSystem->GetLazyFontStorage
#include "GlyphwareGDIFont.h"  // GDI key prefix + (WINVER) TVPGetGDIFontData
#include "glyphware/Face.h"

#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

extern const ttstr& TVPGetDefaultFaceNames();   // engine default face chain

namespace {

// A glyphware blob over the engine's shared on-memory font buffer
// (TVPGetFontStreamBuffer = the same StorageCache-backed buffer classic
// FreeType reads through). Contiguous and kept alive for the blob's lifetime,
// so a FreeType memory face can read from it directly with zero copies.
class tTVPStorageFontBlob : public glyphware::FontBlob {
public:
	explicit tTVPStorageFontBlob(std::shared_ptr<tTJSBinaryStreamBuffer> buf)
		: Buffer(std::move(buf)) {}
	const std::uint8_t* data() const noexcept override { return Buffer->buffer(); }
	std::size_t size() const noexcept override { return Buffer->size(); }
private:
	std::shared_ptr<tTJSBinaryStreamBuffer> Buffer;
};

// Maps a glyphware font key to bytes: a "@gdi:<name>" key is an installed GDI
// font (WINVER, extracted via GetFontData); anything else is a UTF-8 storage
// path read through the storage layer (shared on-memory buffer).
class tTVPStorageFontLoader : public glyphware::FontLoader {
public:
	std::shared_ptr<glyphware::FontBlob> load(std::string_view key) override {
		static const std::string gdiPrefix = TVP_GLYPHWARE_GDI_KEY_PREFIX;
		if (key.size() > gdiPrefix.size() &&
		    key.compare(0, gdiPrefix.size(), gdiPrefix) == 0) {
#ifdef __WINVER__
			tjs_string name;
			if (!TVPUtf8ToUtf16(name, std::string(key.substr(gdiPrefix.size())))) return nullptr;
			const tTVPGDIFontData* gd = TVPGetGDIFontData(name);
			if (!gd) return nullptr;
			return std::make_shared<glyphware::OwnedFontBlob>(gd->bytes.data(), gd->bytes.size());
#else
			return nullptr;   // GDI keys only occur on WINVER
#endif
		}
		tjs_string wpath;
		if (!TVPUtf8ToUtf16(wpath, std::string(key))) return nullptr;
		std::shared_ptr<tTJSBinaryStreamBuffer> buf;
		// TVPGetFontStreamBuffer throws on a nonexistent storage; treat an
		// unknown key as "not found" (nullptr) so the caller can fall back.
		try { buf = TVPGetFontStreamBuffer(ttstr(wpath.c_str())); }
		catch (...) { return nullptr; }
		if (!buf || buf->size() == 0) return nullptr;
		return std::make_shared<tTVPStorageFontBlob>(std::move(buf));
	}
};

} // namespace

glyphware::Registry& TVPGetGlyphwareRegistry() {
	static std::shared_ptr<glyphware::FontLoader> loader =
		std::make_shared<tTVPStorageFontLoader>();
	static glyphware::Registry registry(loader);
	return registry;
}

//---------------------------------------------------------------------------
// Font-name -> glyphware key resolution + chain building (shared by
// drawShapedText and the glyphware font rasterizer).
//---------------------------------------------------------------------------
// A token that looks like a storage path — has a scheme (foo://), a path
// separator, or a font-file extension — must be treated as a path, never as a
// GDI font name (CreateFontIndirect would substitute a default face for an
// unknown "name" and GetFontData would succeed on the wrong font).
static bool GlyphwareLooksLikePath(const std::string& s) {
	if (s.find("://") != std::string::npos) return true;
	if (s.find('/') != std::string::npos || s.find('\\') != std::string::npos) return true;
	auto endsWithNoCase = [&](const char* ext) {
		std::size_t n = std::char_traits<char>::length(ext);
		if (s.size() < n) return false;
		for (std::size_t i = 0; i < n; ++i) {
			char c = s[s.size() - n + i];
			if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
			if (c != ext[i]) return false;
		}
		return true;
	};
	return endsWithNoCase(".ttf") || endsWithNoCase(".otf") || endsWithNoCase(".ttc");
}

std::string TVPGlyphwareResolveFontKey(const std::string& tokenU8) {
	tjs_string name;
	bool haveName = TVPUtf8ToUtf16(name, tokenU8);

	// 1. fonts.json declared name -> storage path
	if (TVPFontSystem && haveName) {
		// 宣言テーブル (fonts.json) は classic 経路の初回使用まで遅延構築される。
		// glyphware 側が先に名前解決することもある (-nostartup の REPL 等) ので
		// ここで確実にロードする (ロード済みなら即 return)。
		TVPFontSystem->EnsureFontMetadataLoaded();
		tjs_string storage;
		if (TVPFontSystem->GetLazyFontStorage(name, storage)) {
			std::string out;
			TVPUtf16ToUtf8(out, storage);
			if (!out.empty()) return out;
		}
	}

	// 2. a path-looking token, or an existing storage file/path -> use it
	//    directly (and never let GDI font substitution hijack it).
	if (GlyphwareLooksLikePath(tokenU8)) return tokenU8;
	if (haveName && TVPIsExistentStorage(ttstr(name.c_str()))) return tokenU8;

#ifdef __WINVER__
	// 3. an installed GDI font (system or addFont-registered) -> GDI key.
	if (haveName && TVPGetGDIFontData(name))
		return std::string(TVP_GLYPHWARE_GDI_KEY_PREFIX) + tokenU8;
#endif

	// 4. treat as a storage path as-is.
	return tokenU8;
}

std::string TVPGlyphwareEffectiveKey(const std::string& keyU8) {
	if (!keyU8.empty()) return keyU8;
	const ttstr& def = TVPGetDefaultFaceNames();
	if (def.IsEmpty()) return std::string();
	std::string out;
	TVPUtf16ToUtf8(out, tjs_string(def.c_str()));
	return out;
}

bool TVPGlyphwareFontNameAvailable(const std::string& nameU8) {
	tjs_string name;
	if (!TVPUtf8ToUtf16(name, nameU8)) return false;
	if (TVPFontSystem) {
		TVPFontSystem->EnsureFontMetadataLoaded();
		tjs_string storage;
		if (TVPFontSystem->GetLazyFontStorage(name, storage)) return true;
	}
#ifdef __WINVER__
	if (TVPGetGDIFontData(name)) return true;
#endif
	return false;
}

namespace {

// Whether a resolved key names a GDI font (carries the @gdi: prefix).
bool GlyphwareIsGDIKey(const std::string& keyU8) {
	static const std::string prefix = TVP_GLYPHWARE_GDI_KEY_PREFIX;
	return keyU8.size() > prefix.size() && keyU8.compare(0, prefix.size(), prefix) == 0;
}

// キー→レジストリ entry id のキャッシュ (registry 自体はキー重複を排除しない
// ので、ここが唯一の登録口として重複を防ぐ)
std::unordered_map<std::string, int>& GlyphwareEntryCache() {
	static std::unordered_map<std::string, int> cache;
	return cache;
}

} // namespace

// Reuse one registry entry per font key (avoid growing the registry per draw).
// A GDI key resolves its TTC face index up front so the right face is opened.
int TVPGlyphwareEntryForKey(const std::string& keyU8) {
	auto& cache = GlyphwareEntryCache();
	auto it = cache.find(keyU8);
	if (it != cache.end()) return it->second;
	int faceIndex = 0;
#ifdef __WINVER__
	if (GlyphwareIsGDIKey(keyU8)) {
		tjs_string name;
		if (TVPUtf8ToUtf16(name, keyU8.substr(sizeof(TVP_GLYPHWARE_GDI_KEY_PREFIX) - 1))) {
			if (const tTVPGDIFontData* gd = TVPGetGDIFontData(name)) faceIndex = gd->faceIndex;
		}
	}
#endif
	int id = TVPGetGlyphwareRegistry().registerKey(keyU8, faceIndex);
	cache.emplace(keyU8, id);
	return id;
}

// fonts.json 宣言エントリ (declared descriptor 込み) を登録する。同一キーが
// 登録済みならその id を返す (宣言メタは初回登録時のみ反映)。
int TVPGlyphwareAddDeclaredEntry(glyphware::FontEntry entry) {
	auto& cache = GlyphwareEntryCache();
	auto it = cache.find(entry.key);
	if (it != cache.end()) return it->second;
	std::string key = entry.key;
	int id = TVPGetGlyphwareRegistry().add(std::move(entry));
	cache.emplace(std::move(key), id);
	return id;
}

void TVPGlyphwareBuildChain(const std::string& keyU8,
                            std::vector<std::shared_ptr<glyphware::Face>>& chain) {
	auto& reg = TVPGetGlyphwareRegistry();
	for (std::size_t s = 0; s <= keyU8.size();) {
		std::size_t comma = keyU8.find(',', s);
		std::string k = keyU8.substr(s, comma == std::string::npos ? std::string::npos : comma - s);
		std::size_t b = k.find_first_not_of(" \t");
		std::size_t e = k.find_last_not_of(" \t");
		if (b != std::string::npos) k = k.substr(b, e - b + 1); else k.clear();
		if (!k.empty()) {
			if (auto f = reg.face(TVPGlyphwareEntryForKey(TVPGlyphwareResolveFontKey(k)))) chain.push_back(f);
		}
		if (comma == std::string::npos) break;
		s = comma + 1;
	}
}

#endif // KRKRZ_USE_GLYPHWARE
