#include "tjsCommHead.h"
#include "CharacterSet.h"
#include "DebugIntf.h"
#include "LogIntf.h"
#include "StorageIntf.h"

#include "app.h"
#include <filesystem>
#include <SDL3/SDL_dialog.h>

class MySDL3Application : public SDL3Application  {

public:
    MySDL3Application() {}
	virtual ~MySDL3Application(){}
    virtual bool InitPath();
    virtual const tjs_string& TempPath() const; //< テンポラリ領域のパス
};

static inline tjs_string IncludeTrailingBackslash( const tjs_string& path ) {
	if( path[path.length()-1] != TJS_W('/') ) {
		return tjs_string(path+TJS_W("/"));
	}
	return tjs_string(path);
}

static inline void checkLastDelimiter(std::string &path, char delimiter) 
{
	// 最後の文字がデリミタでない場合に追加する
	if (path.empty() || path.back() != delimiter) {
		path += delimiter;
	}
}

// ---------------------------------------------------------------------------
// フォルダ選択ダイアログ (同期ラッパー)
// SDL_ShowOpenFolderDialog は非同期 API なので、コールバック結果を
// SDL イベントポンプで同期的に待つ。
// ---------------------------------------------------------------------------
struct FolderDialogResult {
	bool done = false;
	bool selected = false;
	std::string path;
};

static void SDLCALL FolderDialogCallback(void *userdata, const char * const *filelist, int filter)
{
	auto *result = static_cast<FolderDialogResult *>(userdata);
	if (filelist && *filelist) {
		result->selected = true;
		result->path = *filelist;
	}
	result->done = true;
}

// プロジェクトフォルダ選択ダイアログを表示し、選択されたパスを返す。
// キャンセルまたはエラー時は空文字列を返す。
static std::string ShowProjectFolderDialog()
{
	char* cwd = SDL_GetCurrentDirectory();
	FolderDialogResult result;
	SDL_ShowOpenFolderDialog(FolderDialogCallback, &result, nullptr, cwd, false);
	SDL_free(cwd);

	// コールバックが呼ばれるまでイベントポンプで待機
	while (!result.done) {
		SDL_PumpEvents();
		SDL_Delay(10);
	}
	return result.path;
}

static bool IsExistent(const char *path)
{
	tjs_string _path;
	TVPUtf8ToUtf16(_path, path);
	return TVPIsExistentStorageNoSearch(_path.c_str());
}

bool MySDL3Application::InitPath()
{
    // プラグインパス
    // 実行ファイルのパス

#if defined(SDL_PLATFORM_ANDROID)
	// BootstrapActivity が `<filesDir>/assets/` に *.xp3 をコピー済みなら
	// setAssetCacheDir 経由で受け取った絶対パスをそのまま appPath にする
	// (SDL3 の Android backend は絶対パスを通常 filesystem として扱う)。
	//
	// 未設定 (= cache 未生成 / BootstrapActivity を経由していない) ケース用
	// にレガシー AssetManager 経路を残してある。レガシー側は SDL3 が「相対パス
	// なら assets」と扱う仕様を使うため、appPath を `/_assets/` のような
	// **絶対パス風マーカー** にしておき、SDL_GetLocallyAccessibleName 側で
	// その先頭を丸ごと剥がして相対パス化してから SDL に渡す。マーカーを使う
	// ことで「真の絶対パス (= cache 経由)」と「assets 相対変換が必要な経路」
	// を分岐できる。
	extern std::string g_AndroidAssetCacheDir;
	std::string appPath = g_AndroidAssetCacheDir.empty()
		? std::string("/_assets/")
		: g_AndroidAssetCacheDir;
#else
	std::string appPath = SDL_GetBasePath();
#endif
	char delimiter = appPath.back();

	// 引数でプロジェクトパスを明示指定
	std::string projectPath;
	if (_nargs.size() > 1) {
		std::filesystem::path p(_nargs[1].c_str());
		// C++20 以降 std::filesystem::path::u8string() は std::u8string を返すため
		// std::string にそのまま代入・連結できない。バイト列は UTF-8 のまま
		// reinterpret して std::string 化する。
		auto u8 = p.u8string();
		std::string pathU8(reinterpret_cast<const char*>(u8.c_str()), u8.size());
		if (p.is_relative()) {
			projectPath = appPath;
			projectPath += pathU8;
		} else {
			projectPath = std::move(pathU8);
		}
		checkLastDelimiter(projectPath, delimiter);
	} else {
		if (IsExistent((appPath + "data.xp3").c_str())) {
			projectPath = appPath + "data.xp3>";
			TVPLOG_INFO("data.xp3 found, using as project path");
		} else if (IsExistent((appPath + "data/startup.tjs").c_str())) {
			projectPath = appPath + "data/";
			TVPLOG_INFO("data/startup.tjs found, using data/ as project path");
		} else {
#if defined(SDL_PLATFORM_ANDROID)
			return false;
#else
			// 自動探索で見つからなかった場合、フォルダ選択ダイアログを表示
			TVPLOG_INFO("No project data found automatically, showing folder selection dialog");
			std::string selected = ShowProjectFolderDialog();
			if (!selected.empty()) {
				projectPath = selected;
				checkLastDelimiter(projectPath, delimiter);
				TVPLOG_INFO("User selected project folder: {}", projectPath);
			} else {
				return false;
			}
#endif
		}
	}
	TVPLOG_INFO("appPath: {}", appPath);
	TVPLOG_ERROR("projectPath: {}", projectPath);

	TVPUtf8ToUtf16(_AppPath, appPath);
	TVPUtf8ToUtf16(_ProjectPath, projectPath);

	/// XXX
	_ExePath = _AppPath + TJS_W("krkrz.exe");
#if defined(SDL_PLATFORM_ANDROID)
	_PluginPath = TJS_W("");
#elif defined(SDL_PLATFORM_APPLE)
	_PluginPath = _AppPath;
#elif defined(TJS_64BIT_OS)
	_PluginPath = _AppPath + TJS_W("plugin64/");;
#else
	_PluginPath = _AppPath + TJS_W("plugin/");
#endif

#if defined(SDL_PLATFORM_WINDOWS)
	::SetDllDirectory((wchar_t*)PluginPath().c_str());
#endif

	return true;
}

const tjs_string& MySDL3Application::TempPath() const
{
    static bool inited = false;
    static tjs_string _TempPath;
    if (!inited) {
        inited = true;
        // テンポラリフォルダのパス・標準関数
        auto tempU8 = std::filesystem::temp_directory_path().u8string();
        std::string tempPath(reinterpret_cast<const char*>(tempU8.c_str()), tempU8.size());
		tempPath += std::filesystem::path::preferred_separator;
        TVPUtf8ToUtf16(_TempPath, tempPath);
    }
    return _TempPath;
}

// 機種別グローバル初期化処理（デスクトップ/Windows 版）
void TVPAppInit(int argc, char *argv[])
{
    // デスクトップ版では特に必要な初期化処理はない
}

SDL3Application *GetSDL3Application()
{
    // シングルトン。 過去は呼出毎に `new MySDL3Application()` していたが、
    // tTVPApplication の ctor が `Application = this` でグローバルを奪う
    // (Application.cpp:56) ため、 2 回目以降の呼出で form の addEventHandler 登録
    // (旧インスタンスの event_handlers_) が孤立し、 dispatch 経路が破綻する。
    // 既存 Application グローバルを再利用する形にする。
    if (!Application) {
        new MySDL3Application();   // ctor が Application = this を設定
    }
    return static_cast<SDL3Application*>(Application);
}

bool SDL_CommitSavedata()
{
	return true;
}

bool SDL_RollbackSavedata()
{
	return true;
}

bool SDL_NormalizeStorageName(tjs_string &name)
{
	// if the name is an OS's native expression, change it according with the
	// TVP storage system naming rule.
	tjs_int namelen = name.length();
	if(namelen == 0) return false;

	// windows drive:path expression
	if(namelen >= 2)
	{
		if((name[0] >= TJS_W('a') && name[0]<=TJS_W('z') ||
			name[0] >= TJS_W('A') && name[0]<=TJS_W('Z') ) &&
			name[1] == TJS_W(':'))
		{
			// Windows drive:path expression
			tjs_string newname(TJS_W("file://./"));
			newname += name[0];
			newname += (name.c_str()+2);
            name = newname;
			return true;
		}
	}

	if (namelen >= 5 && name.substr(0, 5) == TJS_W("file:"))
	{
		// すでに既定のパス
		return false;
	}

	// Check if path is absolute (simple check without std::filesystem)
	bool is_absolute = false;
	#if defined(SDL_PLATFORM_WINDOWS)		
		// Windows: check for drive letter (C:) or UNC path (\\)
		if (namelen >= 2) {
			if ((name[1] == TJS_W(':')) || 
				(name[0] == TJS_W('\\') && name[1] == TJS_W('\\'))) {
				is_absolute = true;
			}
		}
	#else
		// Unix-like: check for leading /
		if (namelen >= 1 && name[0] == TJS_W('/')) {
			is_absolute = true;
		}
	#endif
	
	if (is_absolute) {
		// Windows drive:path expression
		tjs_string newname(TJS_W("file://./"));
		name = newname + name;
		return true;
	}

	return false;
}

void SDL_GetLocallyAccessibleName(tjs_string &name)
{
#if defined(SDL_PLATFORM_WINDOWS)
	const tjs_char *ptr = name.c_str();
	tjs_string newname;

	if(TJS_strncmp(ptr, TJS_W("./"), 2))
	{
		// differs from "./",
		// this may be a UNC file name.
		// UNC first two chars must be "\\\\" ?
		// AFAIK 32-bit version of Windows assumes that '/' can be used as a path
		// delimiter. Can UNC "\\\\" be replaced by "//" though ?
		newname = tjs_string(TJS_W("\\\\")) + ptr;
	}
	else
	{
		ptr += 2;  // skip "./"
		if(!*ptr) {
			newname = TJS_W("");
		} else {
			tjs_char dch = tolower(*ptr);
			if (dch < TJS_W('a') || dch > TJS_W('z')) {
				newname = TJS_W("");
			} else {
				ptr++;
				if(*ptr != TJS_W('/')) {
					newname = TJS_W("");
				} else {
					newname = dch;
					newname += TJS_W(":");
					newname += ptr;
				}
			}
		}
	}
	// change path delimiter to '/'
	std::replace(newname.begin(), newname.end(), TJS_W('/'), TJS_W('\\'));

	name = newname;
#elif defined(SDL_PLATFORM_ANDROID)
	// SDL3 (Android backend) は「相対パス → assets、絶対パス → filesystem」
	// と内部でルーティングするので、2 経路でその挙動を使い分ける:
	//   (A) cache モード (BootstrapActivity が xp3 を `<filesDir>/assets/` に
	//       コピー済み): appPath は real な絶対パス (例 `/data/user/0/pkg/files/assets/`)。
	//       kirikiri 内部で "./" がついて "./data/user/0/.../data.xp3" となるので、
	//       他の Unix と同じく "." だけ除去して leading "/" を残し絶対パスとして渡す。
	//   (B) legacy AssetManager 経路 (cache 未生成 fallback): appPath は
	//       `/_assets/` というマーカー。"./_assets/data.xp3" を丸ごと剥がして
	//       "data.xp3" にし、SDL3 に相対パスとして渡すと assets 直行する。
	// 判定は **`./_assets/` 先頭一致** で分岐。
	const tjs_char *ptr = name.c_str();
	static const tjs_char kLegacyPrefix[] = TJS_W("./_assets/");
	static const size_t kLegacyPrefixLen = sizeof(kLegacyPrefix) / sizeof(tjs_char) - 1; // 10
	if (TJS_strncmp(ptr, kLegacyPrefix, kLegacyPrefixLen) == 0) {
		// legacy AssetManager 経路: marker prefix を丸ごと剥がして相対パス化
		name = ptr + kLegacyPrefixLen;
	} else if (ptr[0] == '.' && ptr[1] == '/') {
		// cache 経路: "." だけ除去して絶対パスとして残す
		name = ptr + 1;
	}
#else
	const tjs_char *ptr = name.c_str();
	// 先頭の "." を取り除く
	if (ptr[0] == '.' && ptr[1] == '/') {
		name = ptr + 1;
	}
#endif

}

bool SDL_GetListAt(const tjs_char *name, std::function<void(const tjs_char *, bool isDir)> lister, bool withDir)
{
	return false;
}

iTJSBinaryStream* SDL_OpenStream(const char *path, const tjs_uint32 flags)
{
	return nullptr;
}
