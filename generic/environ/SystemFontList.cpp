//---------------------------------------------------------------------------
// OS にインストールされたフォントを名前だけ遅延登録する (generic / SDL3)
//
// ファイルは開かない。 family 名 → フォントファイルの対応を FontSystem の遅延テーブルへ
// 入れておき、 実際に使われた時点で EnsureLazyFontLoaded が読む (fonts.json と同じ経路)。
// これで Font.face に OS のフォント名 ("Meiryo" / "Noto Sans CJK JP" 等) を書けるように
// なり、 既定フォントの候補にも OS の日本語フォントが入る。
//
// 優先順: 同梱 / fonts.json / 実行時登録 (先に登録された名前は上書きしない) → OS。
//
//   Windows : DirectWrite (IDWriteFactory::GetSystemFontCollection)。 dwrite.dll を
//             実行時に引くのでリンク依存は増えない。 地域名 (メイリオ等) も登録する
//   Linux   : fontconfig (FcFontList)。 libfontconfig.so.1 を dlopen するので、
//             無い環境 (最小構成のコンテナ等) では何もしない
//   その他  : 未対応 (macOS の CoreText / Android の ASystemFontIterator は将来課題)
//
// -systemfont=no で無効にできる (配布物の見た目を OS に左右させたくない場合)。
//---------------------------------------------------------------------------
#include "tjsCommHead.h"

#include "FontSystem.h"
#include "SysInitIntf.h"
#include "StorageIntf.h"
#include "DebugIntf.h"
#include "CharacterSet.h"

#include <string>
#include <utility>
#include <vector>

namespace {

// (family 名, ネイティブのファイルパス)
typedef std::vector<std::pair<tjs_string, tjs_string>> tFontEntries;

#if defined(_WIN32)
} // namespace
#include <windows.h>
#include <dwrite.h>
namespace {

template<class T> void SafeRelease(T *&p) { if(p) { p->Release(); p = nullptr; } }

tjs_string ToTjs(const wchar_t *s) { return tjs_string(reinterpret_cast<const tjs_char *>(s)); }

// ファミリの標準的な書体 (Regular 相当) のファイルパスを得る
bool FamilyFilePath(IDWriteFontFamily *fam, tjs_string &path)
{
	IDWriteFont *font = nullptr;
	if(FAILED(fam->GetFirstMatchingFont(DWRITE_FONT_WEIGHT_NORMAL, DWRITE_FONT_STRETCH_NORMAL,
		DWRITE_FONT_STYLE_NORMAL, &font)) || !font) return false;
	bool ok = false;
	IDWriteFontFace *face = nullptr;
	if(SUCCEEDED(font->CreateFontFace(&face)) && face) {
		UINT32 n = 1;
		IDWriteFontFile *file = nullptr;
		if(SUCCEEDED(face->GetFiles(&n, &file)) && file) {
			const void *key = nullptr; UINT32 keySize = 0;
			IDWriteFontFileLoader *loader = nullptr;
			IDWriteLocalFontFileLoader *local = nullptr;
			if(SUCCEEDED(file->GetReferenceKey(&key, &keySize)) &&
				SUCCEEDED(file->GetLoader(&loader)) && loader &&
				SUCCEEDED(loader->QueryInterface(__uuidof(IDWriteLocalFontFileLoader), (void **)&local)) && local) {
				UINT32 len = 0;
				if(SUCCEEDED(local->GetFilePathLengthFromKey(key, keySize, &len)) && len > 0) {
					std::vector<wchar_t> buf(len + 1);
					if(SUCCEEDED(local->GetFilePathFromKey(key, keySize, buf.data(), len + 1))) {
						path = ToTjs(buf.data());
						ok = true;
					}
				}
			}
			SafeRelease(local);
			SafeRelease(loader);
			SafeRelease(file);
		}
		SafeRelease(face);
	}
	SafeRelease(font);
	return ok;
}

void EnumerateSystemFonts(tFontEntries &out)
{
	HMODULE mod = ::LoadLibraryW(L"dwrite.dll");
	if(!mod) return;
	typedef HRESULT (WINAPI *CreateProc)(DWRITE_FACTORY_TYPE, REFIID, IUnknown **);
	CreateProc create = (CreateProc)::GetProcAddress(mod, "DWriteCreateFactory");
	IDWriteFactory *factory = nullptr;
	if(!create || FAILED(create(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory),
		reinterpret_cast<IUnknown **>(&factory))) || !factory) return;

	IDWriteFontCollection *col = nullptr;
	if(SUCCEEDED(factory->GetSystemFontCollection(&col, FALSE)) && col) {
		UINT32 count = col->GetFontFamilyCount();
		for(UINT32 i = 0; i < count; i++) {
			IDWriteFontFamily *fam = nullptr;
			if(FAILED(col->GetFontFamily(i, &fam)) || !fam) continue;
			tjs_string path;
			IDWriteLocalizedStrings *names = nullptr;
			if(FamilyFilePath(fam, path) && SUCCEEDED(fam->GetFamilyNames(&names)) && names) {
				UINT32 nn = names->GetCount();
				for(UINT32 k = 0; k < nn; k++) {
					UINT32 len = 0;
					if(FAILED(names->GetStringLength(k, &len))) continue;
					std::vector<wchar_t> buf(len + 1);
					if(SUCCEEDED(names->GetString(k, buf.data(), len + 1)))
						out.emplace_back(ToTjs(buf.data()), path);
				}
			}
			SafeRelease(names);
			SafeRelease(fam);
		}
	}
	SafeRelease(col);
	SafeRelease(factory);
	// dwrite.dll は解放しない (共有ファクトリはプロセス内で使い回される)
}

#elif defined(__linux__) && !defined(__ANDROID__)
} // namespace
#include <dlfcn.h>
namespace {

// fontconfig の必要最小限の型 (ABI は安定。 ヘッダ依存を避けるため自前で宣言する)
typedef unsigned char FcChar8;
struct FcPattern;
struct FcConfig;
struct FcObjectSet;
struct FcFontSet { int nfont; int sfont; FcPattern **fonts; };
enum { FcResultMatch = 0 };

void EnumerateSystemFonts(tFontEntries &out)
{
	void *lib = ::dlopen("libfontconfig.so.1", RTLD_NOW | RTLD_LOCAL);
	if(!lib) return;
	typedef FcConfig *(*InitProc)();
	typedef FcPattern *(*PatCreateProc)();
	typedef FcObjectSet *(*OsBuildProc)(const char *, ...);
	typedef FcFontSet *(*ListProc)(FcConfig *, FcPattern *, FcObjectSet *);
	typedef int (*GetStrProc)(const FcPattern *, const char *, int, FcChar8 **);
	typedef int (*GetBoolProc)(const FcPattern *, const char *, int, int *);
	typedef void (*FsDestroyProc)(FcFontSet *);
	typedef void (*OsDestroyProc)(FcObjectSet *);
	typedef void (*PatDestroyProc)(FcPattern *);
	typedef void (*CfgDestroyProc)(FcConfig *);
	InitProc init = (InitProc)::dlsym(lib, "FcInitLoadConfigAndFonts");
	PatCreateProc patCreate = (PatCreateProc)::dlsym(lib, "FcPatternCreate");
	OsBuildProc osBuild = (OsBuildProc)::dlsym(lib, "FcObjectSetBuild");
	ListProc list = (ListProc)::dlsym(lib, "FcFontList");
	GetStrProc getStr = (GetStrProc)::dlsym(lib, "FcPatternGetString");
	GetBoolProc getBool = (GetBoolProc)::dlsym(lib, "FcPatternGetBool");
	FsDestroyProc fsDestroy = (FsDestroyProc)::dlsym(lib, "FcFontSetDestroy");
	OsDestroyProc osDestroy = (OsDestroyProc)::dlsym(lib, "FcObjectSetDestroy");
	PatDestroyProc patDestroy = (PatDestroyProc)::dlsym(lib, "FcPatternDestroy");
	CfgDestroyProc cfgDestroy = (CfgDestroyProc)::dlsym(lib, "FcConfigDestroy");
	if(!init || !patCreate || !osBuild || !list || !getStr || !fsDestroy || !osDestroy || !patDestroy) {
		::dlclose(lib);
		return;
	}
	FcConfig *cfg = init();
	FcPattern *pat = patCreate();
	FcObjectSet *os = osBuild("family", "style", "file", "scalable", (char *)0);
	FcFontSet *fs = (cfg && pat && os) ? list(cfg, pat, os) : nullptr;
	// family 名には Regular の書体のファイルを優先して当てる (先に入った名前が勝つ)。
	// FreeType 側は "family style" で登録し、 family 名だけの別名は Regular の書体に
	// しか作らない。 Medium など別の太さのファイルが先に当たると、 family 名
	// ("Noto Sans CJK JP" 等) で引けずに別のフォントへ落ちていた。
	tFontEntries regular, others;
	if(fs) {
		for(int i = 0; i < fs->nfont; i++) {
			FcPattern *p = fs->fonts[i];
			int scalable = 1;
			if(getBool && getBool(p, "scalable", 0, &scalable) == FcResultMatch && !scalable) continue;
			FcChar8 *file = nullptr;
			if(getStr(p, "file", 0, &file) != FcResultMatch || !file) continue;
			tjs_string path;
			TVPUtf8ToUtf16(path, std::string(reinterpret_cast<const char *>(file)));
			bool is_regular = false;
			for(int n = 0; ; n++) {
				FcChar8 *st = nullptr;
				if(getStr(p, "style", n, &st) != FcResultMatch || !st) break;
				if(std::string(reinterpret_cast<const char *>(st)) == "Regular") { is_regular = true; break; }
			}
			for(int n = 0; ; n++) {
				FcChar8 *fam = nullptr;
				if(getStr(p, "family", n, &fam) != FcResultMatch || !fam) break;
				tjs_string name;
				TVPUtf8ToUtf16(name, std::string(reinterpret_cast<const char *>(fam)));
				(is_regular ? regular : others).emplace_back(name, path);
			}
		}
		fsDestroy(fs);
	}
	out.insert(out.end(), regular.begin(), regular.end());
	out.insert(out.end(), others.begin(), others.end());
	if(os) osDestroy(os);
	if(pat) patDestroy(pat);
	if(cfg && cfgDestroy) cfgDestroy(cfg);
	// libfontconfig は閉じない (FcFini を呼ばずに閉じると内部キャッシュが残るため)
}

#else

void EnumerateSystemFonts(tFontEntries &) {}

#endif

} // namespace

//---------------------------------------------------------------------------
void TVPRegisterSystemFontsLazily( FontSystem& fs )
{
	tTJSVariant opt;
	if(TVPGetCommandLine(TJS_W("-systemfont"), &opt) && ttstr(opt) == TJS_W("no")) return;

	tFontEntries entries;
	try {
		EnumerateSystemFonts(entries);
	} catch(...) {
		return;
	}
	tjs_int added = 0;
	for(const auto &e : entries) {
		tjs_string storage;
		if(fs.GetLazyFontStorage(e.first, storage)) continue;	// 同梱 / fonts.json を優先
		fs.RegisterLazyFont(e.first, TVPNormalizeStorageName(ttstr(e.second.c_str())).AsStdString());
		added++;
	}
	if(added)
		TVPAddLog(ttstr(TJS_W("system fonts: ")) + ttstr(added) + TJS_W(" name(s) registered (loaded on first use)"));
}
//---------------------------------------------------------------------------
