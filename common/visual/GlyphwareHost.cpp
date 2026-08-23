#include "tjsCommHead.h"
#include "GlyphwareHost.h"

#ifdef KRKRZ_USE_GLYPHWARE

#include "StorageIntf.h"   // TVPIsExistentStorage
#include "FontStream.h"    // TVPGetFontStreamBuffer (共有オンメモリバッファ)
#include "../base/BinaryStreamBuffer.h"
#include "CharacterSet.h"  // TVPUtf8ToUtf16 / TVPUtf16ToUtf8
#include "FontSystem.h"    // TVPFontSystem->GetLazyFontStorage
#include "GlyphwareGDIFont.h"  // GDI key prefix + (WINVER) TVPGetGDIFontData
#include "FontVariations.h"    // TVPFontVarPackTag / TVPFontDefaultUseVarStyle
#include "DebugIntf.h"         // TVPAddImportantLog
#include "glyphware/Face.h"

#include <algorithm>
#include <cstdio>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <utility>
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

namespace {
// The one storage/GDI-backed byte source, shared by the registry and by
// privately-opened faces (so both alias the same on-memory font buffer).
std::shared_ptr<glyphware::FontLoader>& SharedFontLoader() {
	static std::shared_ptr<glyphware::FontLoader> loader =
		std::make_shared<tTVPStorageFontLoader>();
	return loader;
}
} // namespace

glyphware::Registry& TVPGetGlyphwareRegistry() {
	static glyphware::Registry registry(SharedFontLoader());
	return registry;
}

std::shared_ptr<glyphware::Face> TVPGlyphwareOpenPrivateFace(const std::string& keyU8,
                                                             int faceIndex) {
	if (keyU8.empty()) return nullptr;
	auto blob = SharedFontLoader()->load(keyU8);
	if (!blob) return nullptr;
	return glyphware::Face::open(std::move(blob), keyU8, faceIndex);
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

//---------------------------------------------------------------------------
// 可変軸つき private face の LRU キャッシュ
//---------------------------------------------------------------------------
namespace {

// 適用済み座標の正規化キー文字列 ("wght=700;wdth=87.5"、タグ昇順・入力順不問)
std::string GlyphwareCoordsKey(const std::vector<std::pair<tjs_uint32, float>>& coords) {
	std::vector<std::pair<tjs_uint32, float>> sorted = coords;
	std::sort(sorted.begin(), sorted.end(),
	          [](const auto& a, const auto& b) { return a.first < b.first; });
	std::string key;
	char buf[48];
	for (const auto& c : sorted) {
		std::snprintf(buf, sizeof(buf), "%08x=%g;", c.first, c.second);
		key += buf;
	}
	return key;
}

struct GlyphwareVarFaceCacheEntry {
	std::string key;   // loaderKey \n faceIndex \n coordsKey
	std::shared_ptr<glyphware::Face> face;
};

// 軸付き face の LRU (front = 最新)。軸をアニメーションさせても face 生成が
// 際限なく増えないようにする。座標は量子化済み (FontVariations.cpp) の前提。
std::vector<GlyphwareVarFaceCacheEntry>& GlyphwareVarFaceCache() {
	static std::vector<GlyphwareVarFaceCacheEntry> cache;
	return cache;
}
constexpr size_t GlyphwareVarFaceCacheMax = 32;

} // anonymous

std::shared_ptr<glyphware::Face> TVPGlyphwareFaceWithVariations(
	const std::shared_ptr<glyphware::Face>& shared,
	const std::vector<std::pair<tjs_uint32, float>>& coords) {
	if (!shared || coords.empty()) return shared;

	// ルール: その face が実際に持つ同名軸だけを適用する (フォールバック連鎖の
	// 各 face で軸構成が違うのが普通)。軸の既定値と一致する指定は落とし、
	// 適用すべき軸が残らなければ共有 face をそのまま使う (無駄なインスタンス化
	// をしない)。
	// 重ね掛けの合成: `shared` が既に軸の入った private face (fonts.json 宣言
	// インスタンス等) の場合もあるので、現在座標を基底にして coords を上書き
	// マージする。private face は共有バイトから常に既定値で開き直すため、
	// 既定値と一致する結果の座標は落として構わない (= 既定へ戻る)。
	std::vector<std::pair<tjs_uint32, float>> merged;
	for (const auto& v : shared->variations()) merged.emplace_back(v.tag, v.value);
	for (const auto& c : coords) {
		bool replaced = false;
		for (auto& m : merged) if (m.first == c.first) { m.second = c.second; replaced = true; break; }
		if (!replaced) merged.emplace_back(c.first, c.second);
	}
	std::vector<std::pair<tjs_uint32, float>> applied;
	for (const auto& c : merged) {
		float mn = 0, def = 0, mx = 0;
		if (!shared->axisRange(c.first, mn, def, mx)) continue;
		float v = c.second;
		if (v < mn) v = mn;
		if (v > mx) v = mx;
		if (v == def) continue;
		applied.emplace_back(c.first, v);
	}
	if (applied.empty()) {
		// 既に軸の入った face から全軸が既定へ戻る場合も、既定状態そのものの
		// (共有) face を返したいところだが、shared 自身が既定とは限らない。
		// 現在座標が全て既定なら shared をそのまま返す。
		bool atDefault = true;
		for (const auto& v : shared->variations()) {
			float mn = 0, def = 0, mx = 0;
			if (shared->axisRange(v.tag, mn, def, mx) && v.value != def) { atDefault = false; break; }
		}
		if (atDefault) return shared;
	}

	const std::string& loaderKey = shared->descriptor().key;
	std::string key = loaderKey + "\n" + std::to_string(shared->faceIndex()) +
	                  "\n" + GlyphwareCoordsKey(applied);

	auto& cache = GlyphwareVarFaceCache();
	for (size_t i = 0; i < cache.size(); ++i) {
		if (cache[i].key == key) {
			// LRU: 先頭へ移動
			if (i != 0) std::rotate(cache.begin(), cache.begin() + i, cache.begin() + i + 1);
			return cache[0].face;
		}
	}

	std::shared_ptr<glyphware::Face> inst =
		TVPGlyphwareOpenPrivateFace(loaderKey, shared->faceIndex());
	if (!inst) return shared;   // 開けない (GDI キー消滅等) → 共有 face で描く
	if (!applied.empty()) {
		std::vector<glyphware::VarCoord> vc;
		vc.reserve(applied.size());
		for (const auto& c : applied) vc.push_back({ c.first, c.second });
		if (!inst->setVariations(vc)) return shared;
	}
	// applied が空 = 全軸が既定へ戻る (private face は既定状態で開かれるので
	// setVariations 不要)。

	cache.insert(cache.begin(), { std::move(key), inst });
	if (cache.size() > GlyphwareVarFaceCacheMax) cache.pop_back();
	return inst;
}

void TVPGlyphwareApplyVariationsToChain(
	std::vector<std::shared_ptr<glyphware::Face>>& chain,
	const std::vector<std::pair<tjs_uint32, float>>& coords) {
	if (coords.empty()) return;
	for (auto& f : chain) f = TVPGlyphwareFaceWithVariations(f, coords);
}

void TVPGlyphwareAutoStyleCoords(const std::shared_ptr<glyphware::Face>& primary,
	std::vector<std::pair<tjs_uint32, float>>& coords,
	bool& synthBold, bool& synthItalic) {
	if (!TVPFontDefaultUseVarStyle || !primary) return;

	const tjs_uint32 tagWght = TVPFontVarPackTag("wght", 4);
	const tjs_uint32 tagSlnt = TVPFontVarPackTag("slnt", 4);
	const tjs_uint32 tagItal = TVPFontVarPackTag("ital", 4);
	auto hasCoord = [&](tjs_uint32 tag) {
		for (const auto& c : coords) if (c.first == tag) return true;
		return false;
	};
	float mn = 0, def = 0, mx = 0;

	if (synthBold) {
		if (hasCoord(tagWght)) {
			// 明示指定の wght が勝つ。軸で太らせているので合成は二重適用になる
			synthBold = false;
		} else if (primary->axisRange(tagWght, mn, def, mx)) {
			coords.emplace_back(tagWght, 700.0f);
			synthBold = false;
		}
	}
	if (synthItalic) {
		if (hasCoord(tagSlnt) || hasCoord(tagItal)) {
			synthItalic = false;
		} else if (primary->axisRange(tagSlnt, mn, def, mx)) {
			coords.emplace_back(tagSlnt, -10.0f);
			synthItalic = false;
		} else if (primary->axisRange(tagItal, mn, def, mx)) {
			coords.emplace_back(tagItal, 1.0f);
			synthItalic = false;
		}
	}
}

namespace {

// fonts.json 宣言 (axes / instance) をトークン名から引いて face へ適用する。
// instance (fvar named instance 名、ASCII 大文字小文字無視) を座標に展開し、
// axes 直書きが同名軸を上書きする。宣言が無い/解決できない場合は face を
// そのまま返す。この上に Font.weight / Font.variations が重ね掛けされる
// (TVPGlyphwareFaceWithVariations が現在座標へ合成するので順序どおり勝つ)。
std::shared_ptr<glyphware::Face> GlyphwareApplyDeclaredVariations(
	std::shared_ptr<glyphware::Face> face, const std::string& tokenU8,
	std::vector<tjs_uint32>* appliedTags = nullptr) {
	if (!face || !TVPFontSystem) return face;
	tjs_string name;
	if (!TVPUtf8ToUtf16(name, tokenU8)) return face;
	tjs_string axesSpec, instName;
	if (!TVPFontSystem->GetLazyFontVariations(name, axesSpec, instName)) return face;

	std::vector<std::pair<tjs_uint32, float>> decl;
	if (!instName.empty()) {
		// named instance 名 → 座標 (ASCII 大文字小文字無視)
		std::string instU8;
		TVPUtf16ToUtf8(instU8, instName);
		auto ieq = [](const std::string& a, const std::string& b) {
			if (a.size() != b.size()) return false;
			for (size_t i = 0; i < a.size(); ++i) {
				char x = a[i], y = b[i];
				if (x >= 'A' && x <= 'Z') x = static_cast<char>(x - 'A' + 'a');
				if (y >= 'A' && y <= 'Z') y = static_cast<char>(y - 'A' + 'a');
				if (x != y) return false;
			}
			return true;
		};
		bool found = false;
		for (const auto& ni : face->descriptor().namedInstances) {
			if (!ieq(ni.name, instU8)) continue;
			for (const auto& c : ni.coords) decl.emplace_back(c.first, c.second);
			found = true;
			break;
		}
		if (!found) {
			TVPAddImportantLog(ttstr(TJS_W("fonts.json: named instance \"")) +
				ttstr(instName.c_str()) + TJS_W("\" not found in font for \"") +
				ttstr(name.c_str()) + TJS_W("\""));
		}
	}
	if (!axesSpec.empty()) {
		// axes 直書きは instance の同名軸を上書き (TVPParseFontVariations は
		// 既存同タグを置き換える)
		std::vector<tTVPFontAxisCoord> axes;
		TVPParseFontVariations(ttstr(axesSpec.c_str()), axes);
		for (const auto& a : axes) {
			bool replaced = false;
			for (auto& d : decl) if (d.first == a.first) { d.second = a.second; replaced = true; break; }
			if (!replaced) decl.push_back(a);
		}
	}
	if (decl.empty()) return face;
	if (appliedTags)
		for (const auto& d : decl) appliedTags->push_back(d.first);
	return TVPGlyphwareFaceWithVariations(face, decl);
}

// Font.setDefaultVariations の登録簿。key = face 指定に使うトークン表記
// (ベース名、suffix 抜き) そのまま。値 = "tag=val,..."。
std::map<std::string, std::string>& GlyphwareDefaultVariationsMap() {
	static std::map<std::string, std::string> map_;
	return map_;
}
std::mutex& GlyphwareDefaultVariationsMutex() {
	static std::mutex mtx_;
	return mtx_;
}

constexpr tjs_uint32 GLYPHWARE_TAG_WGHT = 0x77676874u; // 'wght'

} // anonymous

void TVPGlyphwareSetDefaultVariations(const std::string& nameU8,
	const std::string& axesU8) {
	std::lock_guard<std::mutex> lock(GlyphwareDefaultVariationsMutex());
	if (axesU8.empty())
		GlyphwareDefaultVariationsMap().erase(nameU8);
	else
		GlyphwareDefaultVariationsMap()[nameU8] = axesU8;
}

std::string TVPGlyphwareGetDefaultVariations(const std::string& nameU8) {
	std::lock_guard<std::mutex> lock(GlyphwareDefaultVariationsMutex());
	auto it = GlyphwareDefaultVariationsMap().find(nameU8);
	return it != GlyphwareDefaultVariationsMap().end() ? it->second : std::string();
}

std::shared_ptr<glyphware::Face> TVPGlyphwareFaceForToken(const std::string& tokenU8) {
	// 可変軸 suffix ("#tag=val,...") を分離してから基底トークンを解決する。
	// 注: トークンの comma 分割は呼び出し側 (BuildChain) が済ませている前提。
	// suffix 内の comma は軸の区切りなので、Font.face 等の連鎖表記では
	// suffix 付きトークンは連鎖の**最後**に置くか、軸を 1 つに絞ること
	// (途中に置くと後続軸が次トークンに割れる)。Elements/ブリッジ経由の
	// 単一キーではこの制約は無い。
	std::string base = tokenU8, suffix;
	if (std::size_t pos = tokenU8.find('#'); pos != std::string::npos) {
		base = tokenU8.substr(0, pos);
		suffix = tokenU8.substr(pos + 1);
		std::size_t e = base.find_last_not_of(" \t");
		base = (e == std::string::npos) ? std::string() : base.substr(0, e + 1);
	}
	if (base.empty()) return nullptr;

	auto f = TVPGetGlyphwareRegistry().face(
		TVPGlyphwareEntryForKey(TVPGlyphwareResolveFontKey(base)));
	if (!f) return nullptr;

	// 適用順 (後で適用したものが同名軸で勝つ):
	//   Font.setDefaultVariations の既定軸 → fonts.json 宣言 (axes/instance)
	//   → suffix 軸。どこでも wght が指定されず face が wght 軸を持つ場合は
	//   wght=400 に正規化する (CSS の font-weight 既定に合わせる — fvar 既定が
	//   Regular でない VF (Noto VF=Thin 等) も無指定で Regular 相当に読める)。
	bool wghtSpecified = false;

	{
		auto def = TVPGlyphwareGetDefaultVariations(base);
		if (!def.empty()) {
			std::vector<tTVPFontAxisCoord> coords;
			TVPParseFontVariations(ttstr(def.c_str()), coords);
			if (!coords.empty()) {
				for (const auto& c : coords)
					if (c.first == GLYPHWARE_TAG_WGHT) wghtSpecified = true;
				f = TVPGlyphwareFaceWithVariations(f, coords);
			}
		}
	}

	{
		std::vector<tjs_uint32> declTags;
		f = GlyphwareApplyDeclaredVariations(std::move(f), base, &declTags);
		for (auto t : declTags)
			if (t == GLYPHWARE_TAG_WGHT) wghtSpecified = true;
	}

	if (!suffix.empty()) {
		std::vector<tTVPFontAxisCoord> coords;
		TVPParseFontVariations(ttstr(suffix.c_str()), coords);
		if (!coords.empty()) {
			for (const auto& c : coords)
				if (c.first == GLYPHWARE_TAG_WGHT) wghtSpecified = true;
			f = TVPGlyphwareFaceWithVariations(f, coords);
		}
	}

	if (!wghtSpecified && f) {
		bool hasWght = false;
		for (const auto& ax : f->descriptor().axes)
			if (ax.tag == GLYPHWARE_TAG_WGHT) { hasWght = true; break; }
		if (hasWght) {
			std::vector<std::pair<tjs_uint32, float>> coords;
			coords.emplace_back(GLYPHWARE_TAG_WGHT, 400.0f);
			// 既定が 400 の face は共有 face のまま返る (無駄なインスタンス化無し)
			f = TVPGlyphwareFaceWithVariations(f, coords);
		}
	}
	return f;
}

namespace {

// "tag=value" (可変軸 1 項目) か。連鎖の ',' 分割で、可変軸 suffix 内の
// 軸区切りをトークンの区切りと誤認しないための判定 (タグは 1..4 文字 +
// '=' + 数値。フォント名/パスがこの形になることはない)。
bool GlyphwareIsAxisToken(const std::string& tok) {
	std::size_t eq = tok.find('=');
	if (eq == std::string::npos || eq < 1 || eq > 4) return false;
	char* end = nullptr;
	std::strtof(tok.c_str() + eq + 1, &end);
	return end && *end == '\0' && end != tok.c_str() + eq + 1;
}

} // anonymous

void TVPGlyphwareBuildChain(const std::string& keyU8,
                            std::vector<std::shared_ptr<glyphware::Face>>& chain) {
	// ',' で分割する。ただし直前トークンが '#' を含み、今回のトークンが軸
	// ("tag=value") なら可変軸 suffix の続きとして直前へ結合する
	// ("x.ttf#wght=700,wdth=75,fallback.ttf" → 2 トークン)。
	std::vector<std::string> tokens;
	for (std::size_t s = 0; s <= keyU8.size();) {
		std::size_t comma = keyU8.find(',', s);
		std::string k = keyU8.substr(s, comma == std::string::npos ? std::string::npos : comma - s);
		std::size_t b = k.find_first_not_of(" \t");
		std::size_t e = k.find_last_not_of(" \t");
		if (b != std::string::npos) k = k.substr(b, e - b + 1); else k.clear();
		if (!k.empty()) {
			if (!tokens.empty() && tokens.back().find('#') != std::string::npos &&
			    GlyphwareIsAxisToken(k))
				tokens.back() += "," + k;
			else
				tokens.push_back(std::move(k));
		}
		if (comma == std::string::npos) break;
		s = comma + 1;
	}
	for (const std::string& k : tokens) {
		if (auto f = TVPGlyphwareFaceForToken(k)) chain.push_back(std::move(f));
	}
}

#endif // KRKRZ_USE_GLYPHWARE
