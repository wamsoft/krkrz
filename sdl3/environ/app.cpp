#include "tjsCommHead.h"
#include "tjsString.h"
#include "CharacterSet.h"
#include "StorageIntf.h"
#include "LogIntf.h"
#include "SysInitIntf.h"
#include "app.h"
#ifdef KRKRZ_HAS_ELEMENTS
#include "ElementsModalRunner.h"   // TVPInputStringElements
#endif

#include <SDL3/SDL_platform_defines.h>
#include <SDL3/SDL_dialog.h>   // SelectFile (SDL_ShowOpenFileDialog)
#include <SDL3/SDL_misc.h>     // ShellExecute (SDL_OpenURL)

#if defined(SDL_PLATFORM_WINDOWS)
	#include <windows.h>
#elif defined(SDL_PLATFORM_APPLE)
	#include <sys/sysctl.h>
#elif defined(SDL_PLATFORM_ANDROID) || defined(SDL_PLATFORM_LINUX)
	#include <sys/utsname.h>
#endif

#ifdef __EMSCRIPTEN__
	#include <emscripten.h>
#endif

static const char *GetOSVersion()
{
	static thread_local char osVersionBuffer[256] = {};
	#if defined(SDL_PLATFORM_WINDOWS)
		OSVERSIONINFOEX osvi = {};
		osvi.dwOSVersionInfoSize = sizeof(OSVERSIONINFOEX);
		if (GetVersionEx((OSVERSIONINFO*)&osvi)) {
			snprintf(osVersionBuffer, sizeof(osVersionBuffer), "Windows %lu.%lu (Build %lu)",
				osvi.dwMajorVersion, osvi.dwMinorVersion, osvi.dwBuildNumber);
		}
	#elif defined(SDL_PLATFORM_APPLE)
		char version[256] = {};
		size_t len = sizeof(version);
		if (sysctlbyname("kern.osrelease", version, &len, NULL, 0) == 0) {
			snprintf(osVersionBuffer, sizeof(osVersionBuffer), "macOS %s", version);
		}
	#elif defined(SDL_PLATFORM_ANDROID) || defined(SDL_PLATFORM_LINUX)
		struct utsname buf = {};
		if (uname(&buf) == 0) {
			snprintf(osVersionBuffer, sizeof(osVersionBuffer), "Linux %s", buf.release);
		}
	#else
		snprintf(osVersionBuffer, sizeof(osVersionBuffer), "%s", SDL_GetPlatform());
	#endif

	return osVersionBuffer;
}


SDL3Application::SDL3Application()
 : tTVPApplication() 
 ,_Terminated(false)
 ,_TerminateCode(0)
 ,_InBackground(false)
 ,mKirikiriStorage(nullptr)
{
	// アプリイベント用のユーザイベント型を確保 (SDL_Init 済み前提)。
	// 失敗時は (Uint32)-1 が返り、_SendAppEvent は SDL_PushEvent 失敗で
	// リトライキューに回るだけなので致命的ではない。
	mAppEventType = SDL_RegisterEvents(1);

	// ゲームパッド論理管理に物理プロバイダ (= 自分自身) を接続。
	PadManager_.SetProvider(this);

	_language = "ja";
	_country = "jp";

#ifdef __EMSCRIPTEN__
	// wasm: リソースは wasm に埋め込まず、krkrz_web 側が preload バンドルで
	// MEMFS /resource に upfront 配置する (NX の file://basepath/resource と同発想)。
	// file:// で同期読みするため、最初期の config.cf 読み (charset 決定) も安全。
	// 案件別 charset は /resource/config.cf で切り替わる。
	_ResourcePath = TJS_W("file://./resource/");
#else
	// SDL規定 (デスクトップ等は resource:// メディア = OS resource/埋め込み)
	_ResourcePath = TJS_W("resource://./");
#endif

	// platform 
	TVPUtf8ToUtf16(_platformName, SDL_GetPlatform());
	TVPUtf8ToUtf16(_osName, GetOSVersion());
}

SDL3Application::~SDL3Application()
{
	// SDL3 Kirikiri Storageを閉じる
	if (mKirikiriStorage) {
		SDL_CloseStorage(mKirikiriStorage);
		mKirikiriStorage = nullptr;
	}
}

#ifdef __EMSCRIPTEN__
// 起動スクリプト完了時のホスト連携。ページ側が開始待ち (オーディオゲート =
// autoplay ブロック時のクリック待ち等) を要求していれば JSPI でここで待つ。
// この時点はシナリオイベント配信前 (= 最初の発音より前) なので、待ちの後に
// 進行を再開すれば AudioContext は running の状態で最初の音から鳴る。
// 待ちの解決後にローディングオーバーレイの終了フックを呼ぶ。
EM_ASYNC_JS(void, krkrz_host_startup_done, (), {
	if (typeof globalThis.krkrzWaitBeforeStart === 'function') {
		try { await globalThis.krkrzWaitBeforeStart(); } catch (e) {}
	}
	if (typeof globalThis.krkrzOnStartupScriptDone === 'function') {
		try { globalThis.krkrzOnStartupScriptDone(); } catch (e) {}
	}
});
#endif

// 起動スクリプト (AM_STARTUP_SCRIPT) 実行完了。ホスト側のローディング表示を
// 「初回スクリプトロード完了」で終了させるための通知
void
SDL3Application::OnStartupScriptDone()
{
#ifdef __EMSCRIPTEN__
	// ページ側の開始待ち (あれば) + ローディング終了フック (krkrz_web web/pre.js)
	krkrz_host_startup_done();
#endif
}

// アプリ処理用の WindowForm 実装を返す
TTVPWindowForm *
SDL3Application::CreateWindowForm(class tTJSNI_Window *win)
{
	TTVPWindowForm *form = new SDL3WindowForm(win);
	return form;
}

tjs_int 
SDL3Application::ScreenWidth() const
{
	SDL_DisplayID display = SDL_GetPrimaryDisplay();
	if (display) {
		SDL_Rect bounds;
		if (SDL_GetDisplayBounds(display, &bounds) == 0) {
			return bounds.w;
		}
	}
	return 0;
}

tjs_int 
SDL3Application::ScreenHeight() const
{
	SDL_DisplayID display = SDL_GetPrimaryDisplay();
	if (display) {
		SDL_Rect bounds;
		if (SDL_GetDisplayBounds(display, &bounds) == 0) {
			return bounds.h;
		}
	}
	return 0;
}

// アクティブかどうか
bool
SDL3Application::GetActivating() const 
{
	SDL3WindowForm *mainForm = (SDL3WindowForm*)MainWindowForm();
	if (!mainForm) return false;
	SDL_Window *window = (SDL_Window*)(mainForm->NativeWindowHandle());

	Uint32 flags = SDL_GetWindowFlags(window);
	return (flags & SDL_WINDOW_INPUT_FOCUS) != 0;
}

bool
SDL3Application::GetNotMinimizing() const 
{
	SDL3WindowForm *mainForm = (SDL3WindowForm*)MainWindowForm();
	if (!mainForm) return false;
	SDL_Window *window = (SDL_Window*)(mainForm->NativeWindowHandle());

	Uint32 flags = SDL_GetWindowFlags(window);
	return (flags & SDL_WINDOW_MINIMIZED) == 0;
}

// for exception showing
void
SDL3Application::MessageDlg(const tjs_string& string, const tjs_string& caption, int type, int button)
{
	SDL_MessageBoxFlags flags;
	switch (type) {
	case mtWarning:
		flags = SDL_MESSAGEBOX_WARNING;
		break;
	case mtError:
		flags = SDL_MESSAGEBOX_ERROR;
		break;
	case mtInformation:
		flags = SDL_MESSAGEBOX_INFORMATION;
		break;
	case mtConfirmation:
		flags = SDL_MESSAGEBOX_INFORMATION;
		break;
	case mtStop:
		flags = SDL_MESSAGEBOX_ERROR;
		break;
	default:
		flags = SDL_MESSAGEBOX_INFORMATION;
		break;
	}

	std::string str_utf8, cap_utf8;
	TVPUtf16ToUtf8(str_utf8, string);
	TVPUtf16ToUtf8(cap_utf8, caption);

	// 親 window を渡してモーダル化する。 NULL のままだと OS の native message
	// box が「親なし」状態で出て、 ゲーム window を裏に回したり前面に重ねたり
	// できてしまう。 親が居れば z-order が固定され、 OS レベルで親側の入力も
	// ブロックされる (Elements 製モーダルダイアログと同じ振る舞い)。
	SDL_Window* parent = nullptr;
	if (auto* mainForm = (SDL3WindowForm*)MainWindowForm()) {
		parent = static_cast<SDL_Window*>(mainForm->NativeWindowHandle());
	}
	SDL_ShowSimpleMessageBox(flags, cap_utf8.c_str(), str_utf8.c_str(), parent);
}

// ---------------------------------------------------------------------------
// Yes/No 確認ダイアログ (System.confirm)。SDL_ShowMessageBox は同期 (ブロッキング)。
// ---------------------------------------------------------------------------
bool
SDL3Application::ConfirmYesNo(const tjs_string& string, const tjs_string& caption)
{
	std::string str_utf8, cap_utf8;
	TVPUtf16ToUtf8(str_utf8, string);
	TVPUtf16ToUtf8(cap_utf8, caption);

	SDL_Window* parent = nullptr;
	if (auto* mainForm = (SDL3WindowForm*)MainWindowForm()) {
		parent = static_cast<SDL_Window*>(mainForm->NativeWindowHandle());
	}

	// ボタンは配列順。Yes(id=1) を Enter 既定、No(id=0) を Esc 既定に割り当てる。
	const SDL_MessageBoxButtonData buttons[2] = {
		{ SDL_MESSAGEBOX_BUTTON_RETURNKEY_DEFAULT, 1, "\xE3\x81\xAF\xE3\x81\x84" },       // "はい"
		{ SDL_MESSAGEBOX_BUTTON_ESCAPEKEY_DEFAULT, 0, "\xE3\x81\x84\xE3\x81\x84\xE3\x81\x88" }, // "いいえ"
	};
	SDL_MessageBoxData data = {};
	data.flags = SDL_MESSAGEBOX_INFORMATION;
	data.window = parent;
	data.title = cap_utf8.c_str();
	data.message = str_utf8.c_str();
	data.numbuttons = 2;
	data.buttons = buttons;

	int buttonid = 0;
	if (!SDL_ShowMessageBox(&data, &buttonid)) {
		return false; // 表示失敗時は No 扱い
	}
	return buttonid == 1;
}

// ---------------------------------------------------------------------------
// テキスト入力 (System.inputString)。既定は Elements 実装。OS のソフトウェア
// キーボード等を使うプラットフォームは、このメソッドを override して差し替える。
// ---------------------------------------------------------------------------
bool
SDL3Application::InputString(const tjs_string& caption, const tjs_string& prompt,
	const tjs_string& def, tjs_string& result)
{
#ifdef KRKRZ_HAS_ELEMENTS
	ttstr r;
	if (TVPInputStringElements(ttstr(caption.c_str()), ttstr(prompt.c_str()),
		ttstr(def.c_str()), r)) {
		result = r.AsStdString();
		return true;
	}
	return false;
#else
	return false; // Elements 無効ビルドは未対応 (void)
#endif
}

// ---------------------------------------------------------------------------
// ファイル選択ダイアログ (Storages.selectFile)
//   SDL_ShowOpenFileDialog / SDL_ShowSaveFileDialog は非同期 API なので、
//   コールバック完了まで SDL イベントポンプで同期的に待つ (フォルダ選択ダイアログ
//   ShowProjectFolderDialog と同方式)。WINVER の GetOpenFileName 相当。
// ---------------------------------------------------------------------------
namespace {
struct FileDialogResult {
	bool done = false;
	bool selected = false;
	std::string path;
	int filter = -1;
};
static void SDLCALL FileDialogCallback(void *userdata, const char * const *filelist, int filter)
{
	auto *r = static_cast<FileDialogResult *>(userdata);
	if (filelist && *filelist) { r->selected = true; r->path = *filelist; }
	r->filter = filter;
	r->done = true;
}
// 吉里吉里のワイルド指定 "*.mp4;*.webm" を SDL の pattern "mp4;webm" へ変換する。
// "*.*" / "*" は全許可 "*"。拡張子の "*." / "." 前置は取り除く。
static std::string TVPKrkrzWildToSdlPattern(const tjs_string &wild)
{
	std::string w; TVPUtf16ToUtf8(w, wild);
	std::string out;
	size_t i = 0;
	while (i < w.size()) {
		size_t e = w.find(';', i);
		std::string tok = (e == std::string::npos) ? w.substr(i) : w.substr(i, e - i);
		i = (e == std::string::npos) ? w.size() : e + 1;
		while (!tok.empty() && tok.front() == ' ') tok.erase(tok.begin());
		while (!tok.empty() && tok.back() == ' ')  tok.pop_back();
		if (tok.empty()) continue;
		if (tok == "*.*" || tok == "*") { return std::string("*"); }
		if (tok.rfind("*.", 0) == 0)     tok = tok.substr(2);
		else if (tok.rfind(".", 0) == 0) tok = tok.substr(1);
		if (!out.empty()) out += ";";
		out += tok;
	}
	return out.empty() ? std::string("*") : out;
}
} // namespace

bool
SDL3Application::SelectFile( iTJSDispatch2 *params )
{
	if (!params) return false;
	tTJSVariant val;

	// --- filter ("表示名|*.ext;*.ext" の文字列 or その配列) ---
	std::vector<std::string> names, patterns; // c_str() を SDL 呼び出しまで生かす
	if (TJS_SUCCEEDED(params->PropGet(TJS_MEMBERMUSTEXIST, TJS_W("filter"), 0, &val, params))) {
		auto pushOne = [&](const tjs_string &f) {
			tjs_string name, wild;
			tjs_string::size_type vpos = f.find_first_of(TJS_W("|"));
			if (vpos != tjs_string::npos) { name = f.substr(0, vpos); wild = f.substr(vpos + 1); }
			else { name = f; wild = f; }
			std::string n8; TVPUtf16ToUtf8(n8, name);
			names.push_back(n8);
			patterns.push_back(TVPKrkrzWildToSdlPattern(wild));
		};
		if (val.Type() != tvtObject) {
			pushOne(ttstr(val).AsStdString());
		} else {
			iTJSDispatch2 *arr = val.AsObjectNoAddRef();
			tjs_int count = 0; tTJSVariant tmp;
			if (TJS_SUCCEEDED(arr->PropGet(TJS_MEMBERMUSTEXIST, TJS_W("count"), 0, &tmp, arr)))
				count = (tjs_int)tmp;
			for (tjs_int i = 0; i < count; i++)
				if (TJS_SUCCEEDED(arr->PropGetByNum(TJS_MEMBERMUSTEXIST, i, &tmp, arr)))
					pushOne(ttstr(tmp).AsStdString());
		}
	}
	std::vector<SDL_DialogFileFilter> filters;
	for (size_t i = 0; i < names.size(); i++)
		filters.push_back(SDL_DialogFileFilter{ names[i].c_str(), patterns[i].c_str() });

	// --- default location (initialDir 優先、無ければ name のパス) ---
	std::string defloc;
	auto readLocal = [&](const tjs_char *key) -> bool {
		if (TJS_SUCCEEDED(params->PropGet(TJS_MEMBERMUSTEXIST, key, 0, &val, params))) {
			ttstr d(val);
			if (!d.IsEmpty()) {
				d = TVPNormalizeStorageName(d);
				TVPGetLocalName(d);
				TVPUtf16ToUtf8(defloc, d.AsStdString());
				return true;
			}
		}
		return false;
	};
	if (!readLocal(TJS_W("initialDir"))) readLocal(TJS_W("name"));

	bool issave = false;
	if (TJS_SUCCEEDED(params->PropGet(TJS_MEMBERMUSTEXIST, TJS_W("save"), 0, &val, params)))
		issave = val.operator bool();

	SDL_Window *parent = nullptr;
	if (auto *mainForm = (SDL3WindowForm *)MainWindowForm())
		parent = static_cast<SDL_Window *>(mainForm->NativeWindowHandle());

	FileDialogResult result;
	const SDL_DialogFileFilter *pf = filters.empty() ? nullptr : filters.data();
	int nf = (int)filters.size();
	const char *dl = defloc.empty() ? nullptr : defloc.c_str();
	if (issave)
		SDL_ShowSaveFileDialog(FileDialogCallback, &result, parent, pf, nf, dl);
	else
		SDL_ShowOpenFileDialog(FileDialogCallback, &result, parent, pf, nf, dl, false);

	// コールバックが呼ばれるまでイベントポンプで待機 (ProjectFolderDialog と同方式)
	while (!result.done) { SDL_PumpEvents(); SDL_Delay(10); }

	if (!result.selected) return false;

	// 選択パスを正規化ストレージ名で name に書き戻す (WINVER 版と同じ契約)
	tjs_string sel16; TVPUtf8ToUtf16(sel16, result.path);
	val = TVPNormalizeStorageName(ttstr(sel16.c_str()));
	params->PropSet(TJS_MEMBERENSURE, TJS_W("name"), 0, &val, params);
	if (result.filter >= 0) {
		val = (tjs_int)result.filter;
		params->PropSet(TJS_MEMBERENSURE, TJS_W("filterIndex"), 0, &val, params);
	}
	return true;
}

// ---------------------------------------------------------------------------
// フォルダ選択ダイアログ (Storages.selectDirectory)
//   SDL_ShowOpenFolderDialog は非同期なので SelectFile と同様にイベントポンプで
//   同期待ちする。params は %[ name, title, window, rootDir ]。選択されると
//   params["name"] に正規化パスを書き戻す (title は SDL 側に指定口が無いため無視)。
// ---------------------------------------------------------------------------
bool
SDL3Application::SelectDirectory( iTJSDispatch2 *params )
{
	if (!params) return false;
	tTJSVariant val;

	// default location: name 優先、無ければ rootDir
	std::string defloc;
	auto readLocal = [&](const tjs_char *key) -> bool {
		if (TJS_SUCCEEDED(params->PropGet(TJS_MEMBERMUSTEXIST, key, 0, &val, params))) {
			ttstr d(val);
			if (!d.IsEmpty()) {
				d = TVPNormalizeStorageName(d);
				TVPGetLocalName(d);
				TVPUtf16ToUtf8(defloc, d.AsStdString());
				return true;
			}
		}
		return false;
	};
	if (!readLocal(TJS_W("name"))) readLocal(TJS_W("rootDir"));

	SDL_Window *parent = nullptr;
	if (auto *mainForm = (SDL3WindowForm *)MainWindowForm())
		parent = static_cast<SDL_Window *>(mainForm->NativeWindowHandle());

	FileDialogResult result;
	const char *dl = defloc.empty() ? nullptr : defloc.c_str();
	SDL_ShowOpenFolderDialog(FileDialogCallback, &result, parent, dl, false);

	// コールバックが呼ばれるまでイベントポンプで待機
	while (!result.done) { SDL_PumpEvents(); SDL_Delay(10); }

	if (!result.selected) return false;

	tjs_string sel16; TVPUtf8ToUtf16(sel16, result.path);
	val = TVPNormalizeStorageName(ttstr(sel16.c_str()));
	params->PropSet(TJS_MEMBERENSURE, TJS_W("name"), 0, &val, params);
	return true;
}

// ---------------------------------------------------------------------------
// シェル実行 (System.shellExecute) — URL / ファイルを OS 既定ハンドラで開く。
//   SDL_OpenURL は URL のほか、Windows では ShellExecute、Linux では xdg-open 等で
//   ローカルファイルも開ける。引数付き実行 (param) は SDL_OpenURL に無いので無視する
//   (URL/ファイルを開く一般用途をカバー)。
// ---------------------------------------------------------------------------
bool
SDL3Application::ShellExecute(const tjs_char *target, const tjs_char * /*param*/)
{
	if (!target || target[0] == 0) return false;
	std::string url_utf8;
	TVPUtf16ToUtf8(url_utf8, tjs_string(target));
	return SDL_OpenURL(url_utf8.c_str());
}

#ifdef __EMSCRIPTEN__
// wasm 版の未処理スクリプト例外ハンドラ。
// - ネイティブモーダル (SDL_ShowMessageBox) は使わない (メインスレッドブロックで
//   オーディオが途切れ、TVPTerminateSync でアプリごと固まるため)
// - JS 側 (web/pre.js の globalThis.krkrzOnScriptError) に本文と trace を渡し、
//   HTML オーバーレイ表示・音停止・リロード誘導を委ねる
// - false を返して呼び出し側 (TVPShowScriptException) の TVPTerminateSync を抑止
//   (アプリは終了せず、イベント無効状態のまま待機する)
bool
SDL3Application::OnUnhandledScriptException(const tjs_string& message, const tjs_string& trace, int dlgType)
{
	std::string msg_utf8, trace_utf8;
	TVPUtf16ToUtf8(msg_utf8, message);
	TVPUtf16ToUtf8(trace_utf8, trace);

	// メインスレッド (ブラウザ UI スレッド) 上で JS ハンドラを呼ぶ。
	// 未定義時 (シェル未対応) はブラウザ標準の alert にフォールバックする。
	MAIN_THREAD_EM_ASM({
		var msg = UTF8ToString($0);
		var trace = UTF8ToString($1);
		try {
			if (typeof globalThis.krkrzOnScriptError === 'function') {
				globalThis.krkrzOnScriptError(msg, trace);
			} else if (typeof window !== 'undefined') {
				console.error('krkrz script exception:\n' + msg + '\n' + trace);
			}
		} catch (e) { console.error(e); }
	}, msg_utf8.c_str(), trace_utf8.c_str());

	return false; // アプリを終了させない
}
#endif // __EMSCRIPTEN__

// 解像度情報
tjs_int 
SDL3Application::GetDensity() const
{
	// 固定値として返す（実際のDPIを取得する方法もある）
	return 96;
}

#include "SDLDrawDevice.h"
#ifdef TVP_USE_OPENGL
#include "SDLOGLDrawDevice.h"
#include "OGLDrawDevice.h"
#endif
tTJSNativeClass*
SDL3Application::GetDefaultDrawDevice()
{
	// 起動オプション -drawdevice=<name> でデフォルト DrawDevice を選択。
	//   sdl    : SDLDrawDevice    (SDL_Renderer 経由、backend 自動選択)
	//   sdlogl : SDLOGLDrawDevice (OpenGL 直接 + 将来 PBO 経由、Canvas なしの純粋版)
	//   ogl    : OGLDrawDevice    (OpenGL 直接 + Canvas/Texture/Shader/Offscreen 等のフル機能)
	// 未指定時:
	//   TVP_USE_OPENGL=ON  ビルド: sdlogl (Switch 等で TexUp 削減狙い、Phase A skeleton 段階)
	//   TVP_USE_OPENGL=OFF ビルド: sdl    (OpenGL 機能なし)
	tTJSVariant val;
	if (TVPGetCommandLine(TJS_W("-drawdevice"), &val))
	{
		ttstr name(val);
		if (name == TJS_W("sdl")) {
			TVPLOG_INFO("GetDefaultDrawDevice: -drawdevice=sdl -> SDLDrawDevice");
			return new tTJSNC_SDLDrawDevice();
		}
#ifdef TVP_USE_OPENGL
		if (name == TJS_W("sdlogl")) {
			TVPLOG_INFO("GetDefaultDrawDevice: -drawdevice=sdlogl -> SDLOGLDrawDevice");
			return new tTJSNC_SDLOGLDrawDevice();
		}
		if (name == TJS_W("ogl")) {
			TVPLOG_INFO("GetDefaultDrawDevice: -drawdevice=ogl -> OGLDrawDevice");
			return new tTJSNC_OGLDrawDevice();
		}
#endif
		// 未知の値 -> default にフォールバック (警告)
		std::string narrow_name;
		TVPUtf16ToUtf8(narrow_name, name.c_str());
		TVPLOG_WARNING("GetDefaultDrawDevice: unknown -drawdevice={}, fall back to default", narrow_name);
	}

	// 未指定時 default
#ifdef TVP_USE_OPENGL
	TVPLOG_INFO("GetDefaultDrawDevice: default -> SDLOGLDrawDevice");
	return new tTJSNC_SDLOGLDrawDevice();
#else
	TVPLOG_INFO("GetDefaultDrawDevice: default -> SDLDrawDevice");
	return new tTJSNC_SDLDrawDevice();
#endif
}

void
SDL3Application::Terminate(int code)
{
	_Terminated = true;
	_TerminateCode = code;
}

// アプリイベント送信 (任意スレッドから呼ばれる)。SDL_PushEvent はスレッド
// セーフ。確保した単一ユーザイベント型に message / wparam / lparam を載せる。
// 失敗 (キュー満杯等) で false を返し、呼び元 (SendAppEvent) がリトライキューへ。
bool
SDL3Application::_SendAppEvent(tjs_int message, tjs_int64 wparam, tjs_int64 lparam)
{
	SDL_Event ev;
	SDL_zero(ev);
	ev.type = mAppEventType;
	ev.user.code = (Sint32)message;
	ev.user.data1 = (void*)(intptr_t)wparam;
	ev.user.data2 = (void*)(intptr_t)lparam;
	bool ok = SDL_PushEvent(&ev);
	if( !ok )
		TVPLOG_WARNING("[AppEvent] SDL_PushEvent failed (type={}): {}", mAppEventType, SDL_GetError());
	return ok;
}

void
SDL3Application::Exit(int code)
{
	std::exit(code);
}

// DLL処理
void*
SDL3Application::LoadLibrary( const tjs_char* path )
{
	std::string path_utf8;
	TVPUtf16ToUtf8(path_utf8, path);
	void* handle = SDL_LoadObject(path_utf8.c_str());
	if (!handle) {
		const char *error = SDL_GetError();
		TVPLOG_ERROR("Failed to load library: {}", error);
	}
	// TODO(plugin-unload-crash / generic/SDL 未対応):
	//   プラグインDLLが engine 終了(tTJS::Shutdown)より前にアンマップされると、
	//   ncbind で System/Window 等に付与したメソッドの vtable が消えてクラッシュする
	//   (詳細は generic/base/PluginImpl.cpp tTVPPlugin::Uninit の TODO 参照)。
	//   WIN版は win32/base/PluginImpl.cpp で GetModuleHandleEx(PIN) 済みだが SDL は未対応。
	//   ここ(SDL_LoadObject 成功直後)で pin するのが可搬な対処:
	//     POSIX: dlopen(path_utf8.c_str(), RTLD_NOW|RTLD_NOLOAD|RTLD_NODELETE); // 返り値は捨て可
	//     Win  : GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_PIN|..._FROM_ADDRESS,
	//              (LPCWSTR)SDL_LoadFunction(handle,"V2Link"), &m);
	//   ※現状はWIN環境のビルド一気通しが主目的のため後回し。SDLビルドで要実装・要検証。
	return handle;
}

void*
SDL3Application::GetProcAddress( void* handle, const char* func_name)
{
	SDL_SharedObject *so_handle = static_cast<SDL_SharedObject *>(handle);
	void* func = (void*)SDL_LoadFunction(so_handle, func_name);
	if (!func) {
		const char *error = SDL_GetError();
		TVPLOG_ERROR("Failed to get function address: {}", error);
	}
	return func;
}

void 
SDL3Application::FreeLibrary( void* handle )
{
	if (handle) {
		SDL_SharedObject *so_handle = static_cast<SDL_SharedObject *>(handle);
		SDL_UnloadObject(so_handle);
	}
}

// 物理メモリ総量を返す。Windows の long は 32bit なので tjs_uint64 を返す型で
// 統一しないと ullTotalPhys が切り捨てられて TVPTotalPhysMemory が壊れる。
#if defined(SDL_PLATFORM_WINDOWS)
#include <windows.h>
static tjs_uint64 getTotalPhysMemoryBytes() {
    MEMORYSTATUSEX memInfo;
    memInfo.dwLength = sizeof(MEMORYSTATUSEX);
    GlobalMemoryStatusEx(&memInfo);
    return memInfo.ullTotalPhys;
}

#elif defined(sysconf) // LinuxやmacOSなどのUnix系
#include <unistd.h>
static tjs_uint64 getTotalPhysMemoryBytes() {
    return (tjs_uint64)sysconf(_SC_PHYS_PAGES) * (tjs_uint64)sysconf(_SC_PAGE_SIZE);
}
#else
static tjs_uint64 getTotalPhysMemoryBytes() {
    return 0;
}
#endif

tjs_uint64
SDL3Application::GetTotalPhysMemory()
{
	return getTotalPhysMemoryBytes();
}

//< システムフォント一覧取得
void 
SDL3Application::GetSystemFontList(std::vector<tjs_string>& fontFiles)
{
}

// SDL3のイベント処理関数
// この関数はアプリケーションのPollEventSystem内で呼び出される
SDL_AppResult
SDL3Application::AppEvent(const SDL_Event& event)
{
	// アプリイベント (SendAppEvent で送られた user event) の呼び返し。
	// window 紐付けが無いので window lookup より前に処理する。
	if (event.type == mAppEventType) {
		DispatchAppEvent((tjs_int)event.user.code,
		                 (tjs_int64)(intptr_t)event.user.data1,
		                 (tjs_int64)(intptr_t)event.user.data2);
		return SDL_APP_CONTINUE;
	}

	SDL_Window* window = SDL_GetWindowFromID(event.window.windowID);
	if (!window) return SDL_APP_CONTINUE;
	
	SDL3WindowForm* form = (SDL3WindowForm*)SDL_GetPointerProperty(SDL_GetWindowProperties(window), "form", nullptr);
	if (form) {
		form->AppEvent(event); // イベントを無視			
	}
	return SDL_APP_CONTINUE;
}


// SDL3 Kirikiri Storage関連の実装
SDL_Storage*
SDL3Application::GetKirikiriStorage()
{
	if (!mKirikiriStorage) {
		mKirikiriStorage = SDL3KirikiriStorage::CreateStorage();
		if (!mKirikiriStorage) {
			const char *error = SDL_GetError();
			TVPLOG_ERROR("Failed to create SDL3 Kirikiri Storage: ", error);
		} else {
			TVPLOG_DEBUG("SDL3 Kirikiri Storage created successfully");
		}
	}
	return mKirikiriStorage;
}

extern void InitStorageSystem(const char *orgname, const char *appname);

#if defined(SDL_PLATFORM_WINDOWS)

#include "ApplicationSpecialPath.h"
#pragma comment(lib, "mpr.lib")
#pragma comment(lib, "shlwapi.lib")
static tjs_string GetDataPathDirectory( tjs_string datapath, const tjs_string& exename ) {
	return ApplicationSpecialPath::GetDataPathDirectory(datapath, exename);
}

#else

static tjs_string GetDataPathDirectory( tjs_string datapath, const tjs_string& exename ) {
	if(datapath == TJS_W("") ) datapath = tjs_string(TJS_W("$(exepath)\\savedata"));
	ttstr basepath = TVPExtractStoragePath(Application->ExePath());
	tjs_string_view exepath  = tjs_string_view(basepath.c_str()); 
	tjs_string_view userpath = tjs_string_view(TJS_W("user://./")); // SDLデフォルト
	datapath = string_replace_all(datapath, tjs_string_view(TJS_W("$(exepath)")), exepath);
	datapath = string_replace_all(datapath, tjs_string_view(TJS_W("$(personalpath)")), userpath);
	datapath = string_replace_all(datapath, tjs_string_view(TJS_W("$(appdatapath)")), userpath);
	datapath = string_replace_all(datapath, tjs_string_view(TJS_W("$(vistapath)")), userpath );
	datapath = string_replace_all(datapath, tjs_string_view(TJS_W("$(savedgamespath)")), userpath);
	return datapath;
}

#endif

const tjs_string& 
SDL3Application::InitDataPath()
{
	// user:// を初期化
	std::string orgname = "wamsoft";
    std::string appname = "krkrz";
	tTJSVariant val;
	if (TVPGetCommandLine(TJS_W("-orgname"), &val)) {
		tjs_string orgname_str = val.GetString();
		TVPUtf16ToUtf8(orgname, orgname_str.c_str());
	}
	if (TVPGetCommandLine(TJS_W("-appname"), &val)) {
		tjs_string appname_str = val.GetString();
		TVPUtf16ToUtf8(appname, appname_str.c_str());
	}
    InitStorageSystem(orgname.c_str(), appname.c_str());

#if defined(SDL_PLATFORM_WINDOWS) || defined(SDL_PLATFORM_LINUX)
	// -datapth オプションで保存先を差し替え・未定義時は実行ファイルの場所にある savedata
	tjs_string config_datapath;
	if (TVPGetCommandLine(TJS_W("-datapath"), &val)) {
		config_datapath = ((ttstr)val).AsStdString();
	}
	_DataPath = GetDataPathDirectory(config_datapath, ExePath());
#else
	_DataPath = TJS_W("user://./");
#endif

	return _DataPath;
}

void 
SDL3Application::OnInitialize(tTJS* scriptEngine)
{
	// 基底クラスの初期化
	tTVPApplication::OnInitialize(scriptEngine);
	scriptEngine->SetPPValue( TJS_W("sdl"), 1 );
	scriptEngine->SetPPValue( TJS_W("kirikiriz_sdl"), 1 );
}

// SDL3 Kirikiri IOStream関連の実装
SDL_IOStream*
SDL3Application::CreateIOStreamFromPath(const tjs_string& path, tjs_uint32 flags)
{
	return SDL3KirikiriIOStreamWrapper::CreateFromPath(path, flags);
}

SDL_IOStream*
SDL3Application::CreateIOStreamFromBinaryStream(iTJSBinaryStream* stream, bool ownsStream)
{
	return SDL3KirikiriIOStreamWrapper::CreateFromBinaryStream(stream, ownsStream);
}
