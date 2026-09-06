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
#include "MsgIntf.h"         // TVPFormatMessage / TVPLicense* メッセージ
#include "SysInitIntf.h"     // TVPGetCommandLine
#include "WinConsole.h"      // TVPWriteStdOutText

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
// 追加モジュールのライセンス登録 (任意)
//   本体へ静的リンクされるが manifest を分けているモジュール向けのフック。
//   ビルド構成でそのモジュールが入ったときだけ TVP_CUSTOM_LICENSES に登録関数名が
//   定義される (未定義なら丸ごと何もしない)。 一覧を引く直前に一度だけ呼ぶので、
//   静的初期化順に依存しない。
//---------------------------------------------------------------------------
#ifdef TVP_CUSTOM_LICENSES
extern void TVP_CUSTOM_LICENSES();
#endif

static void TVPEnsureModuleLicenses()
{
#ifdef TVP_CUSTOM_LICENSES
	static bool registered = false;
	if (!registered) {
		registered = true;
		TVP_CUSTOM_LICENSES();
	}
#endif
}

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
	TVPEnsureModuleLicenses();
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
	TVPEnsureModuleLicenses();
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
	TVPEnsureModuleLicenses();
	if (!sink) return 0;
	std::vector<tTVPLicenseInfo> list;
	TVPGetLicenseList(list);
	for (const auto & e : list) sink->Found(e.Name, e.Group, e.Source);
	return static_cast<tjs_int>(list.size());
}

//---------------------------------------------------------------------------
// 表示・出力のための整形
//---------------------------------------------------------------------------
namespace {

// 収録テキストは UTF-8 / LF 固定なので、表示・出力の直前に TVP_TEXT_EOL へ
// 揃える (Win32 の EDIT コントロールは LF 単独では改行しない)。
// 既に CRLF になっているものは二重変換しない。
ttstr NormalizeEOL(const ttstr & text)
{
#ifdef TJS_TEXT_OUT_CRLF
	const tjs_char * p = text.c_str();
	tjs_string out;
	out.reserve(text.GetLen() + text.GetLen() / 8 + 8);
	for (; *p; p++) {
		if (*p == TJS_W('\n')) out += TJS_W("\r\n");
		else if (*p == TJS_W('\r')) {
			out += TJS_W("\r\n");
			if (p[1] == TJS_W('\n')) p++;   // 既存の CRLF
		} else out += *p;
	}
	return ttstr(out);
#else
	return text;
#endif
}

// 桁を揃えて追記する (幅を超える名前はそのまま流して 1 桁空ける)
void AppendPadded(tjs_string & buf, const ttstr & s, tjs_int width)
{
	buf += s.c_str();
	for (tjs_int i = (tjs_int)s.GetLen(); i < width; i++) buf += TJS_W(' ');
	if ((tjs_int)s.GetLen() >= width) buf += TJS_W(' ');
}

} // namespace

ttstr TVPGetEngineLicenseText()
{
	ttstr text;
	if (TVPGetLicenseText(ttstr(TVP_ENGINE_LICENSE_NAME), text)) return NormalizeEOL(text);
	return ttstr();
}

ttstr TVPGetLicenseListText()
{
	std::vector<tTVPLicenseInfo> list;
	TVPGetLicenseList(list);
	if (list.empty()) return ttstr();

	// 桁幅は実データから決める (極端に長い名前で崩れないよう上限つき)
	tjs_int namew = 0, groupw = 0;
	for (const auto & e : list) {
		if ((tjs_int)e.Name.GetLen()  > namew)  namew  = (tjs_int)e.Name.GetLen();
		if ((tjs_int)e.Group.GetLen() > groupw) groupw = (tjs_int)e.Group.GetLen();
	}
	if (namew  > 40) namew  = 40;
	if (groupw > 24) groupw = 24;

	tjs_string buf;
	buf += TVP_TEXT_EOL;
	buf += TVPFormatMessage(TVPLicenseListHeader, ttstr((tjs_int)list.size())).c_str();
	buf += TVP_TEXT_EOL;
	for (const auto & e : list) {
		buf += TJS_W("  ");
		AppendPadded(buf, e.Name,  namew  + 2);
		AppendPadded(buf, e.Group, groupw + 2);
		buf += e.Source.c_str();
		buf += TVP_TEXT_EOL;
	}
	buf += (const tjs_char *)TVPLicenseListFooter;
	buf += TVP_TEXT_EOL;
	return ttstr(buf);
}

//---------------------------------------------------------------------------
// 起動オプション -license の処理
//   -license           収集済みライセンスの一覧を標準出力へ
//   -license=<名前>    その 1 件の全文を標準出力へ
//   -license=all       全件の全文を標準出力へ
// 本体は GUI サブシステムなので、出力はパイプ / リダイレクトで受けるか、
// 親シェルのコンソールへ attach 済みであることが前提 (TVPWriteStdOutText)。
//---------------------------------------------------------------------------
bool TVPCheckPrintLicense()
{
	tTJSVariant val;
	if (!TVPGetCommandLine(TJS_W("-license"), &val)) return false;
	ttstr arg = val;

	// 値なし (= "yes") / 空 は一覧表示
	if (arg.IsEmpty() || arg == TJS_W("yes")) {
		TVPWriteStdOutText(TVPGetLicenseListText().c_str());
		return true;
	}

	if (arg == TJS_W("all")) {
		std::vector<tTVPLicenseInfo> list;
		TVPGetLicenseList(list);
		for (const auto & e : list) {
			ttstr text;
			if (!TVPGetLicenseText(e.Name, text)) continue;
			ttstr head = ttstr(TVP_TEXT_EOL) +
				TJS_W("======== ") + e.Name +
				TJS_W(" (") + e.Group + TJS_W(" / ") + e.Source + TJS_W(") ========") +
				TVP_TEXT_EOL + TVP_TEXT_EOL;
			TVPWriteStdOutText(head.c_str());
			TVPWriteStdOutText(NormalizeEOL(text).c_str());
			TVPWriteStdOutText(TVP_TEXT_EOL);
		}
		return true;
	}

	ttstr text;
	if (TVPGetLicenseText(arg, text)) {
		TVPWriteStdOutText(NormalizeEOL(text).c_str());
		TVPWriteStdOutText(TVP_TEXT_EOL);
	} else {
		// 見つからないときは候補が分かるよう一覧を続けて出す
		ttstr msg = TVPFormatMessage(TVPLicenseNotFound, arg) + TVP_TEXT_EOL;
		TVPWriteStdOutText(msg.c_str());
		TVPWriteStdOutText(TVPGetLicenseListText().c_str());
	}
	return true;
}
