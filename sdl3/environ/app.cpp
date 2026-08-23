#include "tjsCommHead.h"
#include "tjsString.h"
#include "CharacterSet.h"
#include "StorageIntf.h"
#include "LogIntf.h"
#include "SysInitIntf.h"
#include "DebugIntf.h"
#include "DisplaySelect.h"
#include "app.h"

#include <vector>
#ifdef KRKRZ_HAS_ELEMENTS
#include "ElementsModalRunner.h"   // TVPInputStringElements
#endif

#include <SDL3/SDL_platform_defines.h>
#include <SDL3/SDL_dialog.h>   // SelectFile (SDL_ShowOpenFileDialog)
#include <SDL3/SDL_misc.h>     // ShellExecute (SDL_OpenURL)
#include <SDL3/SDL_locale.h>   // GetSystemLanguage (SDL_GetPreferredLocales)

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
	// _platformTags は初回参照時に構築する (getPlatformTagSpec が virtual なので
	// ここで呼ぶと派生クラスの override が効かない)
}

// --- プラットフォームタグ ------------------------------------------------
//
// config_<tag>.cf の選択や System.platformTag に使う短い識別子。
//
// ★ タグの値そのもの (機種の呼称) は本体には持たない。
//    getPlatformTagSpec() を機種依存部の Application 派生クラスが override して
//    返す (getPlatformName() が SDL 側から来るのと同じ考え方)。
//    ';' 区切りで複数返せる。 並びは「一般 → 具体」で、 世代違いなどで
//    共通設定と個別設定を重ねたいときに使う。
//
// ここでの既定は、 標準マクロで判別できる汎用プラットフォームのみ。
const char *SDL3Application::getPlatformTagSpec() const
{
#if defined(_WIN32)
	return "windows";
#elif defined(__EMSCRIPTEN__)
	return "web";
#elif defined(__ANDROID__)
	return "android";
#elif defined(__APPLE__)
	return "macos";
#elif defined(__linux__)
	return "linux";
#else
	return "";
#endif
}

// --- 本体の表示言語 (System.systemLanguage) --------------------------------
//
// SDL_GetPreferredLocales() は優先順のリストを返すので先頭を採る。
// SDL_Locale は language ("ja") と country ("JP", 無ければ nullptr) の 2 つ。
// BCP-47 として "ja-JP" のように連結して返す (country 無しなら言語のみ)。
//
// SDL に locale バックエンドが無い機種 (NX / PS5) では count=0 / nullptr が
// 返るので、 そこは機種依存部の派生クラスが override して本体 API を叩く。
std::string SDL3Application::GetSystemLanguage() const
{
	int count = 0;
	SDL_Locale **locales = SDL_GetPreferredLocales(&count);
	if (!locales) return std::string();

	std::string result;
	if (count > 0 && locales[0] && locales[0]->language) {
		result = locales[0]->language;
		if (locales[0]->country && locales[0]->country[0]) {
			result += '-';
			result += locales[0]->country;
		}
	}
	SDL_free(locales);
	return result;
}

// 仮想関数を使うので、 コンストラクタではなく初回参照時に構築する
// (基底クラスの構築中に呼ぶと派生の override が効かないため)。
void SDL3Application::InitPlatformTags() const
{
	_platformTags.clear();

	const char *spec_c = getPlatformTagSpec();
	const std::string spec = spec_c ? spec_c : "";
	std::string cur;
	for (size_t i = 0; i <= spec.size(); ++i) {
		const char c = (i < spec.size()) ? spec[i] : ';';
		if (c == ';' || c == ',' || c == ' ') {
			if (!cur.empty()) {
				tjs_string t;
				TVPUtf8ToUtf16(t, cur.c_str());
				_platformTags.push_back(t);
				cur.clear();
			}
		} else {
			// 小文字化して英数のみ残す (ファイル名に使うため)
			if (c >= 'A' && c <= 'Z')      cur += (char)(c - 'A' + 'a');
			else if ((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9')) cur += c;
		}
	}
	_platformTagsInit = true;
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

//---------------------------------------------------------------------------
// -display= (起動するディスプレイの指定) 実装
//---------------------------------------------------------------------------
namespace {
//! 接続中のディスプレイを共通形で列挙する (番号は SDL の並び順に 1 origin)
std::vector<tTVPDisplayEntry> EnumDisplayEntries(std::vector<SDL_DisplayID> &ids)
{
	std::vector<tTVPDisplayEntry> entries;
	int count = 0;
	SDL_DisplayID *list = SDL_GetDisplays(&count);
	if (!list) return entries;
	SDL_DisplayID primary = SDL_GetPrimaryDisplay();
	for (int i = 0; i < count; i++) {
		tTVPDisplayEntry e;
		e.index = (tjs_int)i + 1;
		const char *name = SDL_GetDisplayName(list[i]);
		if (name) {
			tjs_string wname;
			TVPUtf8ToUtf16(wname, name);
			e.name = wname;
		}
		SDL_Rect bounds;
		if (SDL_GetDisplayBounds(list[i], &bounds)) {
			e.left = bounds.x; e.top = bounds.y;
			e.width = bounds.w; e.height = bounds.h;
		}
		e.primary = (list[i] == primary);
		entries.push_back(e);
		ids.push_back(list[i]);
	}
	SDL_free(list);
	return entries;
}
} // anonymous namespace
//---------------------------------------------------------------------------
SDL_DisplayID TVPGetStartupDisplayID()
{
	static SDL_DisplayID selected = 0;
	static bool resolved = false;
	if (resolved) return selected;
	resolved = true;

	tjs_string opt;
	if (!TVPGetStartupDisplayOption(opt)) return 0;

	std::vector<SDL_DisplayID> ids;
	std::vector<tTVPDisplayEntry> entries = EnumDisplayEntries(ids);
	if (TVPIsDisplayListRequest(opt)) {
		TVPLogDisplayList(entries);
		return 0;
	}
	tjs_int idx = TVPMatchDisplay(opt, entries);
	if (idx < 0) {
		TVPAddImportantLog(ttstr(TJS_W("(warning) -display=")) + ttstr(opt) +
			ttstr(TJS_W(" : no such display; using the default one")));
		TVPLogDisplayList(entries);
		return 0;
	}
	selected = ids[idx];
	TVPAddImportantLog(ttstr(TJS_W("(info) -display=")) + ttstr(opt) + ttstr(TJS_W(" -> display ")) +
		ttstr((tjs_int)entries[idx].index) + ttstr(TJS_W(" : ")) + ttstr(entries[idx].name));
	return selected;
}
//---------------------------------------------------------------------------
void TVPSDLSetWindowPositionKeepingSize(SDL_Window *window, int x, int y)
{
	if (!window) return;
	int w = 0, h = 0;
	SDL_GetWindowSize(window, &w, &h);
	SDL_SetWindowPosition(window, x, y);
	if (w <= 0 || h <= 0) return;
	int aw = 0, ah = 0;
	SDL_GetWindowSize(window, &aw, &ah);
	if (aw != w || ah != h) {
		// 移動先の DPI が違ってサイズが変わってしまったので戻す
		SDL_SetWindowSize(window, w, h);
	}
}
//---------------------------------------------------------------------------
void TVPMoveWindowToStartupDisplay(SDL_Window *window)
{
	if (!window) return;
	SDL_DisplayID target = TVPGetStartupDisplayID();
	if (!target) return;

	SDL_Rect tb;
	if (!SDL_GetDisplayUsableBounds(target, &tb)) return;

	int x = 0, y = 0, w = 0, h = 0;
	SDL_GetWindowPosition(window, &x, &y);
	SDL_GetWindowSize(window, &w, &h);
	// SDL の位置/サイズはクライアント領域基準。作業領域への収まりを見るには
	// 枠 (タイトルバー含む) を足した外側の矩形で判定する必要がある
	int bt = 0, bl = 0, bb = 0, br = 0;
	SDL_GetWindowBordersSize(window, &bt, &bl, &bb, &br);
	int ow = w + bl + br;
	int oh = h + bt + bb;

	int l = x, t = y;
	SDL_DisplayID current = SDL_GetDisplayForWindow(window);
	if (current != target) {
		// 別ディスプレイ上にいる場合は、現ディスプレイ作業領域の原点からの
		// 相対位置を保って移動する
		SDL_Rect cb;
		if (!current || !SDL_GetDisplayUsableBounds(current, &cb)) { cb = tb; cb.x = 0; cb.y = 0; }
		l = x - cb.x + tb.x;
		t = y - cb.y + tb.y;
	}
	// 既に目的のディスプレイ上にいる場合でも、はみ出しは作業領域内へ寄せる
	// (配置後にスクリプトがウィンドウを大きくした場合など)
	int ol = l - bl, ot = t - bt;
	if (ol + ow > tb.x + tb.w) ol = tb.x + tb.w - ow;
	if (ot + oh > tb.y + tb.h) ot = tb.y + tb.h - oh;
	if (ol < tb.x) ol = tb.x;
	if (ot < tb.y) ot = tb.y;

	TVPSDLSetWindowPositionKeepingSize(window, ol + bl, ot + bt);
}
//---------------------------------------------------------------------------

// スクリーンサイズ等の基準になるディスプレイ。
// System.screenWidth/screenHeight は「メインウィンドウのあるディスプレイが対象、
// メインウィンドウが無ければプライマリ」と規定されている (WINVER も同じ挙動)。
// -display= 指定時は、メインウィンドウが出来るまでの間もそのディスプレイを使う。
SDL_DisplayID
SDL3Application::BaseDisplayID() const
{
	if (SDL3WindowForm *mainForm = (SDL3WindowForm*)MainWindowForm()) {
		if (SDL_Window *window = (SDL_Window*)(mainForm->NativeWindowHandle())) {
			SDL_DisplayID display = SDL_GetDisplayForWindow(window);
			if (display) return display;
		}
	}
	SDL_DisplayID display = TVPGetStartupDisplayID();
	if (display) return display;
	return SDL_GetPrimaryDisplay();
}

tjs_int
SDL3Application::ScreenWidth() const
{
	SDL_DisplayID display = BaseDisplayID();
	if (display) {
		SDL_Rect bounds;
		// SDL3 の SDL_GetDisplayBounds は成功で true を返す (SDL2 の
		// 「成功で 0」から変わっている)。== 0 で見ると成功時に 0 を返して
		// しまい、System.screenWidth / screenHeight が常に 0 になる。
		if (SDL_GetDisplayBounds(display, &bounds)) {
			return bounds.w;
		}
	}
	return 0;
}

tjs_int
SDL3Application::ScreenHeight() const
{
	SDL_DisplayID display = BaseDisplayID();
	if (display) {
		SDL_Rect bounds;
		// SDL3 の SDL_GetDisplayBounds は成功で true を返す (SDL2 の
		// 「成功で 0」から変わっている)。== 0 で見ると成功時に 0 を返して
		// しまい、System.screenWidth / screenHeight が常に 0 になる。
		if (SDL_GetDisplayBounds(display, &bounds)) {
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
	// WINVER (GetDpiForWindow) と揃えて実 DPI を返す。SDL の content scale は
	// 96dpi = 1.0 倍なので 96 を掛ける。取れない環境は 96 (等倍) にフォールバック。
	float scale = 0.0f;
	if (SDL3WindowForm *mainForm = (SDL3WindowForm*)MainWindowForm()) {
		if (SDL_Window *window = (SDL_Window*)(mainForm->NativeWindowHandle())) {
			scale = SDL_GetWindowDisplayScale(window);
		}
	}
	if (scale <= 0.0f) {
		SDL_DisplayID display = BaseDisplayID();
		if (display) scale = SDL_GetDisplayContentScale(display);
	}
	if (scale <= 0.0f) return 96;
	return (tjs_int)(scale * 96.0f + 0.5f);
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

// モーダルウィンドウ表示中に、モーダル以外のウィンドウへ配ってはいけない
// (= ユーザ操作由来の) イベントかどうか。 リサイズ/再描画/フォーカス等の
// システム系はモーダル中も処理させる必要があるので通す。
static bool IsUserInputEvent(const SDL_Event& event)
{
	switch (event.type) {
	case SDL_EVENT_KEY_DOWN:
	case SDL_EVENT_KEY_UP:
	case SDL_EVENT_TEXT_INPUT:
	case SDL_EVENT_TEXT_EDITING:
	case SDL_EVENT_MOUSE_MOTION:
	case SDL_EVENT_MOUSE_BUTTON_DOWN:
	case SDL_EVENT_MOUSE_BUTTON_UP:
	case SDL_EVENT_MOUSE_WHEEL:
	case SDL_EVENT_FINGER_DOWN:
	case SDL_EVENT_FINGER_UP:
	case SDL_EVENT_FINGER_MOTION:
	case SDL_EVENT_DROP_BEGIN:
	case SDL_EVENT_DROP_FILE:
	case SDL_EVENT_DROP_TEXT:
	case SDL_EVENT_DROP_COMPLETE:
	case SDL_EVENT_WINDOW_CLOSE_REQUESTED: // モーダル中は他ウィンドウを閉じさせない
		return true;
	default:
		return false;
	}
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
		// モーダルウィンドウ表示中は、そのウィンドウ以外へのユーザ入力を捨てる。
		// SDL_SetWindowModal が効く環境では OS 側でも弾かれるが、未対応環境
		// (と、既にキューに積まれていたイベント) のためにここでも排他する。
		TTVPWindowForm* modal = TTVPWindowForm::GetModalWindowForm();
		if (modal && modal != form && IsUserInputEvent(event)) {
			return SDL_APP_CONTINUE;
		}
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
