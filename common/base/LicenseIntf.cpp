//---------------------------------------------------------------------------
/*
	TVP2 ( T Visual Presenter 2 )  A script authoring tool
	Copyright (C) 2000 W.Dee <dee@kikyou.info> and contributors

	See details of license at "license.txt"
*/
//---------------------------------------------------------------------------
// ライセンス情報の内部保持 (圧縮) と収集 — 実装
//---------------------------------------------------------------------------
#include "tjsCommHead.h"
#include "LicenseIntf.h"

#include "StorageIntf.h"     // TVPGetStorageListAt / TVPReadStream / iTVPStorageLister
#include "CharacterSet.h"    // TVPUtf8ToUtf16
#include "DebugIntf.h"

#include <zlib.h>

#include <map>
#include <memory>
#include <string>
#include <vector>

namespace {

//---------------------------------------------------------------------------
// プラグイン登録分の保持
//---------------------------------------------------------------------------
struct tRegisteredLicense
{
	ttstr Group;
	// deflated (参照保持) か text のどちらか
	const tjs_uint8 * Deflated = nullptr;
	tjs_uint DeflatedSize = 0;
	tjs_uint OriginalSize = 0;
	ttstr Text;
	bool IsText = false;
};

std::map<tjs_string, tRegisteredLicense> & RegisteredLicenses()
{
	static std::map<tjs_string, tRegisteredLicense> s;
	return s;
}

//---------------------------------------------------------------------------
// zlib deflate 済み UTF-8 テキストを ttstr へ解凍する
//---------------------------------------------------------------------------
bool InflateLicense(const unsigned char * deflated, unsigned int deflatedSize,
	unsigned int originalSize, ttstr & out)
{
	if (!deflated || deflatedSize == 0 || originalSize == 0) return false;
	std::unique_ptr<char[]> buf(new char[originalSize]);
	uLongf destLen = originalSize;
	if (uncompress(reinterpret_cast<Bytef*>(buf.get()), &destLen,
			deflated, deflatedSize) != Z_OK) return false;
	std::string u8(buf.get(), static_cast<size_t>(destLen));
	tjs_string ws;
	if (!TVPUtf8ToUtf16(ws, u8)) return false;
	out = ttstr(ws.c_str());
	return true;
}

//---------------------------------------------------------------------------
// storage 収集: プロジェクト data の licenses/*.txt
// (案件が追加フォント等のライセンス文を置くだけで一覧に載る)
//---------------------------------------------------------------------------
const tjs_char * TVP_LICENSE_STORAGE_DIR = TJS_W("licenses/");

struct tLicenseFileLister : iTVPStorageLister
{
	std::vector<ttstr> files;
	void TJS_INTF_METHOD Add(const ttstr & name) override
	{
		ttstr ext = TVPExtractStorageExt(name);
		if (ext == TJS_W(".txt") || ext == TJS_W(".md")) files.push_back(name);
	}
};

void EnumStorageLicenses(std::vector<ttstr> & names)
{
	tLicenseFileLister lister;
	try {
		// 相対ディレクトリはプロジェクト基準の完全形へ正規化してから列挙する
		ttstr dir = TVPNormalizeStorageName(ttstr(TVP_LICENSE_STORAGE_DIR));
		TVPGetStorageListAt(dir, &lister);
	} catch (...) {
		return;   // licenses/ が無いプロジェクトでは空
	}
	for (const auto & f : lister.files) {
		// 表示名 = 拡張子を除いたファイル名
		ttstr stem = f;
		const tjs_char * p = stem.c_str();
		tjs_int dot = -1;
		for (tjs_int i = 0; p[i]; i++) if (p[i] == TJS_W('.')) dot = i;
		if (dot > 0) stem = ttstr(p, dot);
		names.push_back(stem);
	}
}

bool ReadStorageLicense(const ttstr & name, ttstr & text)
{
	// licenses/<name>.txt → 無ければ .md
	static const tjs_char * exts[] = { TJS_W(".txt"), TJS_W(".md") };
	for (const tjs_char * ext : exts) {
		ttstr path = ttstr(TVP_LICENSE_STORAGE_DIR) + name + ext;
		if (!TVPIsExistentStorage(path)) continue;
		try {
			tjs_uint64 len = 0;
			auto buf = TVPReadStream(path.c_str(), &len);
			if (!buf || len == 0) return false;
			std::string u8(reinterpret_cast<const char*>(buf.get()),
				static_cast<size_t>(len));
			if (u8.size() >= 3 && (unsigned char)u8[0] == 0xEF &&
				(unsigned char)u8[1] == 0xBB && (unsigned char)u8[2] == 0xBF)
				u8.erase(0, 3);   // UTF-8 BOM
			tjs_string ws;
			if (!TVPUtf8ToUtf16(ws, u8)) return false;
			text = ttstr(ws.c_str());
			return true;
		} catch (...) {
			return false;
		}
	}
	return false;
}

} // namespace

//---------------------------------------------------------------------------
// 公開 API
//---------------------------------------------------------------------------
void TVPRegisterLicense(const ttstr & name, const ttstr & group,
	const tjs_uint8 * deflated, tjs_uint deflatedSize, tjs_uint originalSize)
{
	if (name.IsEmpty() || !deflated || deflatedSize == 0) return;
	tRegisteredLicense e;
	e.Group = group;
	e.Deflated = deflated;
	e.DeflatedSize = deflatedSize;
	e.OriginalSize = originalSize;
	e.IsText = false;
	RegisteredLicenses()[name.AsStdString()] = std::move(e);
}

void TVPRegisterLicenseText(const ttstr & name, const ttstr & group, const ttstr & text)
{
	if (name.IsEmpty() || text.IsEmpty()) return;
	tRegisteredLicense e;
	e.Group = group;
	e.Text = text;
	e.IsText = true;
	RegisteredLicenses()[name.AsStdString()] = std::move(e);
}

bool TVPGetLicenseText(const ttstr & name, ttstr & text)
{
	// 1. プラグイン登録分
	auto & reg = RegisteredLicenses();
	auto it = reg.find(name.AsStdString());
	if (it != reg.end()) {
		if (it->second.IsText) { text = it->second.Text; return true; }
		return InflateLicense(it->second.Deflated, it->second.DeflatedSize,
			it->second.OriginalSize, text);
	}
	// 2. 本体内蔵
	for (int i = 0; i < TVPBuiltinLicenseCount; i++) {
		if (name == TVPBuiltinLicenses[i].name) {
			return InflateLicense(TVPBuiltinLicenses[i].deflated,
				TVPBuiltinLicenses[i].deflatedSize,
				TVPBuiltinLicenses[i].originalSize, text);
		}
	}
	// 3. storage (licenses/<name>.txt)
	return ReadStorageLicense(name, text);
}

void TVPGetLicenseList(std::vector<tTVPLicenseInfo> & out)
{
	// 同名の重複はプラグイン登録 > 内蔵 > storage の優先で 1 件に畳む
	// (プラグインが本体内蔵と同じコンポーネントを登録しても二重表示しない)
	std::map<tjs_string, size_t> index;
	auto push = [&](const ttstr & name, const ttstr & group, const tjs_char * source,
	                bool override_existing) {
		auto key = name.AsStdString();
		auto it = index.find(key);
		if (it != index.end()) {
			if (override_existing) {
				out[it->second].Group = group;
				out[it->second].Source = source;
			}
			return;
		}
		tTVPLicenseInfo info;
		info.Name = name;
		info.Group = group;
		info.Source = source;
		index[key] = out.size();
		out.push_back(info);
	};

	// 内蔵
	for (int i = 0; i < TVPBuiltinLicenseCount; i++) {
		push(ttstr(TVPBuiltinLicenses[i].name), ttstr(TVPBuiltinLicenses[i].group),
			TJS_W("builtin"), false);
	}
	// プラグイン登録 (同名内蔵より優先)
	for (const auto & kv : RegisteredLicenses()) {
		push(ttstr(kv.first.c_str()), kv.second.Group, TJS_W("plugin"), true);
	}
	// storage (licenses/*.txt|md)
	std::vector<ttstr> names;
	EnumStorageLicenses(names);
	for (const auto & n : names) {
		push(n, ttstr(TJS_W("data")), TJS_W("storage"), false);
	}
}

tjs_int TVPEnumLicenses(iTVPLicenseListSink * sink)
{
	if (!sink) return 0;
	std::vector<tTVPLicenseInfo> list;
	TVPGetLicenseList(list);
	for (const auto & e : list) sink->Found(e.Name, e.Group, e.Source);
	return static_cast<tjs_int>(list.size());
}
