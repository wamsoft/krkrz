//---------------------------------------------------------------------------
/*
	TVP2 ( T Visual Presenter 2 )  A script authoring tool
	Copyright (C) 2000 W.Dee <dee@kikyou.info> and contributors

	See details of license at "license.txt"
*/
//---------------------------------------------------------------------------
// "System" class implementation
//---------------------------------------------------------------------------
#include "tjsCommHead.h"
#include "tjsDictionary.h"         // TJSCreateDictionaryObject
#ifdef _WIN32
#include <shellapi.h>              // TVPExecuteProgram: ShellExecuteW (App Paths 解決)
#include "ApplicationSpecialPath.h"   // System.personalPath / appDataPath (WINVER 互換)
#endif

//#include "GraphicsLoaderImpl.h"

#include "SystemImpl.h"
#include "SystemIntf.h"
#include "SysInitIntf.h"
#include "StorageIntf.h"
//#include "StorageImpl.h"
#ifdef KRKRZ_USE_REPL_FILECHANNEL
#include "ReplModal.h"   // TVPReplConfirm / TVPReplInputString / TVPReplTrySelect
#endif
#ifdef KRKRZ_REPL_WEB
#include "ReplWebServer.h"   // TVPReplWeb::GetURL (System.replWebURL)
#endif
#include "TickCount.h"
#include "ComplexRect.h"
//#include "WindowImpl.h"
#include "EventIntf.h"

#include "Application.h"
//#include "CompatibleNativeFuncs.h"
#include "LogIntf.h"
#include "DebugIntf.h"             // TVPAddImportantLog (REPL 駆動時の MessageDlg ルーティング)
#include "CharacterSet.h"
#include "BinaryStreamBuffer.h"     // TVPGetFileAllocator
#include "SoundAllocator.h"         // TVPGetSoundAllocator
#include "BitmapBitsAlloc.h"        // tTVPBitmapBitsAlloc::GetAllocator
#include "MemoryAllocatorStats.h"   // TVPDumpAllocatorStats
#include "ProcessMemory.h"          // TVPDumpProcessMemoryInfo
#include "GlobalAllocStats.h"       // TVPGlobalAllocStats::Dump
#include "AllocTagScope.h"          // TVPPushAllocTag / TVPPopAllocTag
#include "tjsObjectStats.h"         // TVPDumpTJSObjectStats
#include "MemoryOverlay.h"          // TVPMemoryOverlay::SetEnabled
#include "SystemAllocatorInfo.h"    // TVPDumpSystemAllocatorInfo
#include "PadOverlay.h"             // TVPPadOverlay::SetEnabled
#include "ThreadIntf.h"             // TVPDrawStatsLogEnabled
#include "StorageCache.h"           // TVPGetStorageCacheCount
#include "GraphicsLoaderIntf.h"     // TVPGetGraphicCacheCount

#if !defined(_WIN32)
#include "VirtualKey.h"
#endif

extern bool TVPAddFontToFreeType( const ttstr& storage, std::vector<tjs_string>* faces );

//---------------------------------------------------------------------------
static ttstr TVPAppTitle;
static bool TVPAppTitleInit = false;
//---------------------------------------------------------------------------

//---------------------------------------------------------------------------
// TVPShowSimpleMessageBox
//---------------------------------------------------------------------------
static void TVPShowSimpleMessageBox(const ttstr & text, const ttstr & caption)
{
	// エージェント運転 (-replfile = モーダル応答チャネルあり) 中はブロッキング
	// ダイアログを出さず、 内容をログに流して既定応答で進む (自動運転を止めない)。
	// チャネルの無い REPL (-replweb / console のみ。 Deck の replweb 運用が典型)
	// は人が画面を見ている前提なので通常どおり実 UI を出す (SDL は overlay
	// モーダル。 その pump は REPL を drain するのでエージェントからも
	// Agent.click / dialogClick で閉じられる)。
	bool agentSuppress = false;
#ifdef KRKRZ_USE_REPL_FILECHANNEL
	agentSuppress = TVPReplModalActive();
#else
	agentSuppress = TVPReplActive;   // チャネル機構の無いビルドは従来どおり抑止
#endif
	if (agentSuppress) {
		TVPAddImportantLog(ttstr(TJS_W("[dialog] ")) + caption +
			ttstr(TJS_W(": ")) + text);
		return;
	}
	Application->MessageDlg(text.AsStdString(), caption.AsStdString(), 0, 0);
}
//---------------------------------------------------------------------------

//---------------------------------------------------------------------------
// TVPGetAsyncKeyState
//---------------------------------------------------------------------------
bool TVPGetAsyncKeyState(tjs_uint keycode, bool getcurrent)
{
	// get keyboard state asynchronously.
	// return current key state if getcurrent is true.
	// otherwise, return whether the key is pushed during previous call of
	// TVPGetAsyncKeyState at the same keycode.

	//if(keycode >= VKEY_PAD_FIRST  && keycode <= VKEY_PAD_LAST)
	//{
	//	// JoyPad related keys are treated in DInputMgn.cpp
	//	return TVPGetJoyPadAsyncState(keycode, getcurrent);
	//}

	bool ret = Application->GetAsyncKeyState(keycode, getcurrent);
	//int result = ret ? 1 : 0;
	//TVPLOG_DEBUG("keystate:{:08x} ret:{}", keycode, result);
	return ret;
}
//---------------------------------------------------------------------------


//---------------------------------------------------------------------------
// TVPGetPlatformName
//---------------------------------------------------------------------------
ttstr TVPGetPlatformName()
{
	ttstr platform = Application->getPlatformName().c_str();
	return platform;
}
//---------------------------------------------------------------------------

//---------------------------------------------------------------------------
// TVPGetPlatformTag
//---------------------------------------------------------------------------
ttstr TVPGetPlatformTag()
{
	// GetPlatformTags() は「一般 → 具体」順なので末尾が最も具体的
	const std::vector<tjs_string> &tags = Application->GetPlatformTags();
	if (tags.empty()) return ttstr();
	return ttstr(tags.back().c_str());
}
//---------------------------------------------------------------------------

//---------------------------------------------------------------------------
// TVPGetSystemLanguage
//   本体 (OS / ハード) の表示言語を BCP-47 で返す。 取得できなければ空文字。
//   機種ごとの取得手段は Application 派生クラス側 (SDL3 既定 =
//   SDL_GetPreferredLocales / NX = nn::oe / PS5 = sceSystemServiceParam)。
//---------------------------------------------------------------------------
ttstr TVPGetSystemLanguage()
{
	const std::string lang = Application->GetSystemLanguage();
	if (lang.empty()) return ttstr();
	return ttstr(lang.c_str());
}
//---------------------------------------------------------------------------

//---------------------------------------------------------------------------
// TVPGetOSName
//---------------------------------------------------------------------------
ttstr TVPGetOSName()
{
	ttstr osName = Application->getOsName().c_str();
	return osName;
}
//---------------------------------------------------------------------------

//---------------------------------------------------------------------------
// TVPGetOSBits
//---------------------------------------------------------------------------
tjs_int TVPGetOSBits()
{
	// Platform-specific OS bits detection
	#if defined(__aarch64__) || defined(_M_ARM64) || defined(TJS_64BIT_OS)
		return 64;
	#else
		return 32;
	#endif
}
//---------------------------------------------------------------------------

//---------------------------------------------------------------------------
// TVPShellExecute
//---------------------------------------------------------------------------
bool TVPShellExecute(const ttstr &target, const ttstr &param)
{
	return Application->ShellExecute(target.AsStdString().c_str(), param.length() == 0 ? NULL : param.AsStdString().c_str());
}
//---------------------------------------------------------------------------
// TVPExecuteProgram — 実行ファイルを引数付きで起動する (プログラム実行専用)。
// URL/ファイルを既定ハンドラで開く TVPShellExecute (SDL_OpenURL 経由・引数不可) とは
// 別処理。デスクトップ Windows では App Paths / PATH 解決込みの Win32 ShellExecute で
// 機種依存に実装する (例 "msedge.exe" --app=<url>)。非 Windows は未対応 (false)。
//---------------------------------------------------------------------------
bool TVPExecuteProgram(const ttstr &exe, const ttstr &args)
{
#ifdef _WIN32
	if(exe.IsEmpty()) return false;
	return (INT_PTR)::ShellExecuteW(NULL, L"open",
		(const wchar_t*)exe.c_str(),
		args.IsEmpty() ? NULL : (const wchar_t*)args.c_str(),
		NULL,
		SW_SHOWNORMAL) > 32;
#else
	(void)exe; (void)args;
	return false;
#endif
}
//---------------------------------------------------------------------------


//---------------------------------------------------------------------------
// TVPCreateAppLock
//---------------------------------------------------------------------------
extern int GetSystemSecurityOption(const char *name);
bool TVPCreateAppLock(const ttstr &lockname)
{
	// [CUSTOM-MODIFIED] System.createAppLock(...) always return true security-option
	static const int nolock = GetSystemSecurityOption("disableapplock");
	if (nolock > 0) return true;

	// lock application using mutex
	return Application->CreateAppLock(lockname.AsStdString());
}
//---------------------------------------------------------------------------


//---------------------------------------------------------------------------
enum tTVPTouchDevice {
	tdNone				= 0,
	tdIntegratedTouch	= 0x00000001,
	tdExternalTouch		= 0x00000002,
	tdIntegratedPen		= 0x00000004,
	tdExternalPen		= 0x00000008,
	tdMultiInput		= 0x00000040,
	tdDigitizerReady	= 0x00000080,
	tdMouse				= 0x00000100,
	tdMouseWheel		= 0x00000200
};
/**
 * タッチデバイス(とマウス)の接続状態を取得する
 **/
static int TVPGetSupportTouchDevice()
{
	// 常に組み込みタッチパネルを返す
	return tdIntegratedTouch | tdMultiInput;
}
//---------------------------------------------------------------------------


//---------------------------------------------------------------------------
// System.onActivate and System.onDeactivate related
//---------------------------------------------------------------------------
static void TVPOnApplicationActivate(bool activate_or_deactivate);
//---------------------------------------------------------------------------
class tTVPOnApplicationActivateEvent : public tTVPBaseInputEvent
{
	static tTVPUniqueTagForInputEvent Tag;
	bool ActivateOrDeactivate; // true for activate; otherwise deactivate
public:
	tTVPOnApplicationActivateEvent(bool activate_or_deactivate) :
		tTVPBaseInputEvent(Application, Tag),
		ActivateOrDeactivate(activate_or_deactivate) {};
	void Deliver() const
	{ TVPOnApplicationActivate(ActivateOrDeactivate); }
};
tTVPUniqueTagForInputEvent tTVPOnApplicationActivateEvent              ::Tag;
//---------------------------------------------------------------------------
void TVPPostApplicationActivateEvent()
{
	TVPPostInputEvent(new tTVPOnApplicationActivateEvent(true), TVP_EPT_REMOVE_POST);
}
//---------------------------------------------------------------------------
void TVPPostApplicationDeactivateEvent()
{
	TVPPostInputEvent(new tTVPOnApplicationActivateEvent(false), TVP_EPT_REMOVE_POST);
}
//---------------------------------------------------------------------------
static void TVPOnApplicationActivate(bool activate_or_deactivate)
{
	// called by event system, to fire System.onActivate or
	// System.onDeactivate event
	//if(!TVPSystemControlAlive) return;

	// check the state again (because the state may change during the event delivering).
	// but note that this implementation might fire activate events even in the application
	// is already activated (the same as deactivation).
	if(activate_or_deactivate != Application->GetActivating()) return;

	// fire the event
	TVPFireOnApplicationActivateEvent(activate_or_deactivate);
}
//---------------------------------------------------------------------------

//---------------------------------------------------------------------------
// メモリ状態の総合ダンプ。win32 版 TVPHeapDump (HeapWalk) と対をなす
// generic 用実装。OS-level ヒープ走査は持たないため per-allocator stats
// のみ。OS RSS/VSize 取得は M3 で追加予定。
void TVPHeapDump()
{
	TVPDumpAllocatorStats("FileAllocator", TVPGetFileAllocator());
	TVPDumpAllocatorStats("BitmapAllocator", tTVPBitmapBitsAlloc::GetAllocator());
	TVPDumpAllocatorStats("SoundAllocator", TVPGetSoundAllocator());
	TVPGlobalAllocStats::Dump();
	TVPDumpTJSObjectStats();
	TVPDumpProcessMemoryInfo();
	TVPDumpSystemAllocatorInfo();
	// キャッシュエントリ件数 (file 層 / decode 層) を 1 行ずつ。
	// pinned 数は内訳。詳細は Storages.dumpFileCacheList / dumpImageCacheList。
	{
		size_t fc_total = 0, fc_pinned = 0;
		size_t ic_total = 0, ic_pinned = 0;
		TVPGetStorageCacheCount(fc_total, fc_pinned);
		TVPGetGraphicCacheCount(ic_total, ic_pinned);
		TVPLOG_INFO("FileCache: count={} pinned={}", fc_total, fc_pinned);
		TVPLOG_INFO("ImageCache: count={} pinned={}", ic_total, ic_pinned);
	}
}
//---------------------------------------------------------------------------


//---------------------------------------------------------------------------
// TVPCreateNativeClass_System
//---------------------------------------------------------------------------
tTJSNativeClass * TVPCreateNativeClass_System()
{
	tTJSNC_System *cls = new tTJSNC_System();


	// setup some platform-specific members
//----------------------------------------------------------------------

//-- methods

//----------------------------------------------------------------------
TJS_BEGIN_NATIVE_METHOD_DECL(/*func. name*/inform)
{
	// show simple message box
	if(numparams < 1) return TJS_E_BADPARAMCOUNT;

	ttstr text = *param[0];

	ttstr caption;
	if(numparams >= 2 && param[1]->Type() != tvtVoid)
		caption = *param[1];
	else
		caption = TJS_W("Information");

	TVPShowSimpleMessageBox(text, caption);

	if(result) result->Clear();

	return TJS_S_OK;
}
TJS_END_NATIVE_STATIC_METHOD_DECL_OUTER(/*object to register*/cls,
	/*func. name*/inform)
//----------------------------------------------------------------------
TJS_BEGIN_NATIVE_METHOD_DECL(/*func. name*/confirm)
{
	// Yes/No モーダル確認。Yes なら真、No なら偽を返す。
	if(numparams < 1) return TJS_E_BADPARAMCOUNT;

	ttstr text = *param[0];

	ttstr caption;
	if(numparams >= 2 && param[1]->Type() != tvtVoid)
		caption = *param[1];
	else
		caption = TJS_W("Confirmation");

	// エージェント運転 (-replfile) 中はモーダル応答チャネルが Yes/No を返す
	// (ブロックしない自動運転)。 チャネルの無い REPL (-replweb / console のみ)
	// は人が居る前提で実 UI (SDL は overlay モーダル) を出す。 overlay の pump
	// は REPL を drain するのでエージェントからも操作できる。
	bool ret;
	bool handled = false;
#ifdef KRKRZ_USE_REPL_FILECHANNEL
	if (TVPReplActive) {
		bool ans = false;
		if (TVPReplConfirm(text, caption, ans)) { ret = ans; handled = true; }
	}
#endif
	if (!handled) {
		ret = Application ? Application->ConfirmYesNo(text.AsStdString(), caption.AsStdString()) : true;
	}

	if(result) *result = ret;

	return TJS_S_OK;
}
TJS_END_NATIVE_STATIC_METHOD_DECL_OUTER(/*object to register*/cls,
	/*func. name*/confirm)
//----------------------------------------------------------------------
TJS_BEGIN_NATIVE_METHOD_DECL(/*func. name*/inputString)
{
	// System.inputString(caption, prompt, default="") -> 入力文字列 / キャンセルで void
	if(numparams < 1) return TJS_E_BADPARAMCOUNT;

	ttstr caption = *param[0];
	ttstr prompt  = (numparams >= 2 && param[1]->Type() != tvtVoid) ? ttstr(*param[1]) : caption;
	ttstr def;
	if(numparams >= 3 && param[2]->Type() != tvtVoid) def = *param[2];

	// エージェント運転 (-replfile) 中はモーダル応答チャネルが入力文字列を返す。
	// チャネルの無い REPL (-replweb / console のみ) は実 UI へフォールスルー
	// (confirm と同じ方針。 Deck の replweb 運用で overlay が出るようにする)。
	ttstr out;
#ifdef KRKRZ_USE_REPL_FILECHANNEL
	if(TVPReplActive) {
		bool cancelled = false;
		if(TVPReplInputString(caption, prompt, def, out, cancelled)) {
			if(result) { if(cancelled) result->Clear(); else *result = out; }
			return TJS_S_OK;
		}
	}
#endif
	tjs_string r;
	bool ok = Application ? Application->InputString(caption.AsStdString(),
		prompt.AsStdString(), def.AsStdString(), r) : false;
	if(result) { if(ok) *result = ttstr(r.c_str()); else result->Clear(); }
	return TJS_S_OK;
}
TJS_END_NATIVE_STATIC_METHOD_DECL_OUTER(/*object to register*/cls,
	/*func. name*/inputString)
//----------------------------------------------------------------------
TJS_BEGIN_NATIVE_METHOD_DECL(/*func. name*/getTickCount)
{
	if(result)
	{
		TVPStartTickCount();

		*result = (tjs_int64) TVPGetTickCount();
	}
	return TJS_S_OK;
}
TJS_END_NATIVE_STATIC_METHOD_DECL_OUTER(/*object to register*/cls,
	/*func. name*/getTickCount)
//----------------------------------------------------------------------
TJS_BEGIN_NATIVE_METHOD_DECL(/*func. name*/getKeyState)
{
	if(numparams < 1) return TJS_E_BADPARAMCOUNT;

	tjs_uint code = (tjs_int)*param[0];

	bool getcurrent = true;
	if(numparams >= 2) getcurrent = 0!=(tjs_int)*param[1];

	bool res = TVPGetAsyncKeyState(code, getcurrent);

	if(result) *result = (tjs_int)res;
	return TJS_S_OK;
}
TJS_END_NATIVE_STATIC_METHOD_DECL_OUTER(/*object to register*/cls,
	/*func. name*/getKeyState)
//----------------------------------------------------------------------
TJS_BEGIN_NATIVE_METHOD_DECL(/*func. name*/getArgument)
{
	if(numparams < 1) return TJS_E_BADPARAMCOUNT;
	if(!result) return TJS_S_OK;

	ttstr name = *param[0];

	bool res = TVPGetCommandLine(name.c_str(), result);

	if(!res) result->Clear();

	return TJS_S_OK;
}
TJS_END_NATIVE_STATIC_METHOD_DECL_OUTER(/*object to register*/cls,
	/*func. name*/getArgument)
//----------------------------------------------------------------------
TJS_BEGIN_NATIVE_METHOD_DECL(/*func. name*/setArgument)
{
	if(numparams < 2) return TJS_E_BADPARAMCOUNT;

	ttstr name = *param[0];
	ttstr value = *param[1];

	TVPSetCommandLine(name.c_str(), value);

	return TJS_S_OK;
}
TJS_END_NATIVE_STATIC_METHOD_DECL_OUTER(/*object to register*/cls,
	/*func. name*/setArgument)
//----------------------------------------------------------------------
TJS_BEGIN_NATIVE_METHOD_DECL(/*func. name*/dumpHeap)
{
	TVPHeapDump();
	return TJS_S_OK;
}
TJS_END_NATIVE_STATIC_METHOD_DECL_OUTER(/*object to register*/cls,
	/*func. name*/dumpHeap)
//----------------------------------------------------------------------
// システムアロケータ情報を取得する。
// コンソール機等のプラットフォームアロケータが提供する情報を含む。
// 戻り値は Dictionary:
//   %[
//     totalFreeSize: ...,       // 空き領域合計
//     allocatableSize: ...,     // 確保可能最大サイズ
//     processRss: ...,          // プロセス RSS
//     processPeakRss: ...,      // プロセス peak RSS
//     processVsize: ...,        // プロセス virtual size
//     systemTotalPhysical: ..., // システム物理メモリ総量
//     systemAvailPhysical: ..., // システム利用可能物理メモリ
//   ]
// 値が取得できない項目はキー自体が存在しない (TJS で typeof が "Object" 扱いの void)。
TJS_BEGIN_NATIVE_METHOD_DECL(/*func. name*/getSystemAllocatorInfo)
{
	iTJSDispatch2 *dict = TJSCreateDictionaryObject();
	if (!dict) return TJS_E_FAIL;

	// TVPGetSystemAllocatorInfo() が内部で Application に delegate するので
	// プラットフォーム固有 override (NXSystemAllocatorInfo 等) もそのまま反映される。
	iTVPSystemAllocatorInfo *info = TVPGetSystemAllocatorInfo();
	if (info) {
		auto stats = info->GetStats();

		// SIZE_MAX (= 取得不可) のキーは dict に入れない。
		// TJS 側からは dict["xxx"] が void になり、`xxx in dict` で判定可能。
		auto setVal = [&](const tjs_char *name, size_t val) {
			if (val == SIZE_MAX) return;
			tTJSVariant v(static_cast<tjs_int64>(val));
			dict->PropSet(TJS_MEMBERENSURE, name, nullptr, &v, dict);
		};

		setVal(TJS_W("totalFreeSize"),       stats.total_free_size);
		setVal(TJS_W("allocatableSize"),     stats.allocatable_size);
		setVal(TJS_W("processRss"),          stats.process_rss);
		setVal(TJS_W("processPeakRss"),      stats.process_peak_rss);
		setVal(TJS_W("processVsize"),        stats.process_vsize);
		setVal(TJS_W("systemTotalPhysical"), stats.system_total_physical);
		setVal(TJS_W("systemAvailPhysical"), stats.system_avail_physical);
		setVal(TJS_W("usedSize"),            stats.used_size);
		setVal(TJS_W("peakUsedSize"),        stats.peak_used_size);
		setVal(TJS_W("totalSize"),           stats.total_size);
	}

	if (result) *result = tTJSVariant(dict, dict);
	dict->Release();
	return TJS_S_OK;
}
TJS_END_NATIVE_STATIC_METHOD_DECL_OUTER(/*object to register*/cls,
	/*func. name*/getSystemAllocatorInfo)
//----------------------------------------------------------------------
// File/Bitmap allocator の peak_used を current_used に揃え直す。
// MemoryOverlay の "(peak X.XX)" 表示を「ここから先の最大」に
// リセットしたいときに使う。REPL `.mempeakclear` と同等。
TJS_BEGIN_NATIVE_METHOD_DECL(/*func. name*/resetMemoryPeak)
{
	if (auto *fa = TVPGetFileAllocator())               fa->resetPeak();
	if (auto *ba = tTVPBitmapBitsAlloc::GetAllocator()) ba->resetPeak();
	if (auto *sa = TVPGetSoundAllocator())              sa->resetPeak();
	TVPGlobalAllocStats::ResetKrkrzPeak();
	return TJS_S_OK;
}
TJS_END_NATIVE_STATIC_METHOD_DECL_OUTER(/*object to register*/cls,
	/*func. name*/resetMemoryPeak)
//----------------------------------------------------------------------
// thread-local tag stack に push する。Krkrz allocator (operator new +
// TJS_malloc) で起きる確保がこの tag 名に振り分けられる。終了は endAllocTag()。
// tag 名は TVPAllocTag enum 名 ("TJS2" / "User" / "GraphicsLoader" 等)。
// 一致しない名前は User として扱う。
//   System.beginAllocTag("User");
//   loadChapter(3);
//   System.endAllocTag();
TJS_BEGIN_NATIVE_METHOD_DECL(/*func. name*/beginAllocTag)
{
	if (numparams < 1) return TJS_E_BADPARAMCOUNT;
	ttstr name = *param[0];
	tTJSNarrowStringHolder narrow(name.c_str());
	TVPPushAllocTag(TVPAllocTagFromName(narrow));
	return TJS_S_OK;
}
TJS_END_NATIVE_STATIC_METHOD_DECL_OUTER(/*object to register*/cls,
	/*func. name*/beginAllocTag)
//----------------------------------------------------------------------
TJS_BEGIN_NATIVE_METHOD_DECL(/*func. name*/endAllocTag)
{
	TVPPopAllocTag();
	return TJS_S_OK;
}
TJS_END_NATIVE_STATIC_METHOD_DECL_OUTER(/*object to register*/cls,
	/*func. name*/endAllocTag)
//----------------------------------------------------------------------
// 画面右上にメモリ状態のリアルタイム折れ線グラフをオーバレイ表示する。
// 引数なしで toggle、bool 引数指定で明示制御。 flag は全ビルド共通で、
// 描画するのは OGL 系 DrawDevice と SDLDrawDevice。
TJS_BEGIN_NATIVE_METHOD_DECL(/*func. name*/setMemoryOverlay)
{
	bool enable;
	if (numparams >= 1 && param[0]->Type() != tvtVoid) {
		enable = ((tjs_int)*param[0]) != 0;
	} else {
		enable = !TVPMemoryOverlay::IsEnabled();
	}
	TVPMemoryOverlay::SetEnabled(enable);
	if (result) *result = (tjs_int)(enable ? 1 : 0);
	return TJS_S_OK;
}
TJS_END_NATIVE_STATIC_METHOD_DECL_OUTER(/*object to register*/cls,
	/*func. name*/setMemoryOverlay)
//----------------------------------------------------------------------
// 画面左上にゲームパッド 16 ボタンのマトリクスをオーバレイ表示する。
// 引数なしで toggle、bool 引数指定で明示制御。 描画条件は setMemoryOverlay と同じ。
TJS_BEGIN_NATIVE_METHOD_DECL(/*func. name*/setPadOverlay)
{
	bool enable;
	if (numparams >= 1 && param[0]->Type() != tvtVoid) {
		enable = ((tjs_int)*param[0]) != 0;
	} else {
		enable = !TVPPadOverlay::IsEnabled();
	}
	TVPPadOverlay::SetEnabled(enable);
	if (result) *result = (tjs_int)(enable ? 1 : 0);
	return TJS_S_OK;
}
TJS_END_NATIVE_STATIC_METHOD_DECL_OUTER(/*object to register*/cls,
	/*func. name*/setPadOverlay)
//----------------------------------------------------------------------
// KRKRZ_DRAW_STATS=ON ビルド + memoverlay 有効時、500ms ごとに DrawThreadPool
// 利用統計を log に書き出す。実機 (Switch 等) でリアルタイム表示が速く流れて
// 読めないとき用。引数なしで toggle、bool 引数で明示制御。OFF ビルドや
// memoverlay 無効時は呼んでも実害はないが log は出ない。
TJS_BEGIN_NATIVE_METHOD_DECL(/*func. name*/setDrawStatsLog)
{
	bool enable;
	if (numparams >= 1 && param[0]->Type() != tvtVoid) {
		enable = ((tjs_int)*param[0]) != 0;
	} else {
		enable = !TVPDrawStatsLogEnabled;
	}
	TVPDrawStatsLogEnabled = enable;
	if (result) *result = (tjs_int)(enable ? 1 : 0);
	return TJS_S_OK;
}
TJS_END_NATIVE_STATIC_METHOD_DECL_OUTER(/*object to register*/cls,
	/*func. name*/setDrawStatsLog)
//----------------------------------------------------------------------
#ifdef TVP_USE_OPENGL
// GL テクスチャメモリ計測 (GLTexture.cpp の自由関数を extern 参照)。
// GL ヘッダを include せずに済むよう、 ここでは宣言だけする。
extern void TVPGetGLTextureMemory(tjs_uint64 *texture_bytes, tjs_uint64 *pbo_bytes,
                                  tjs_uint64 *peak_bytes,
                                  tjs_uint32 *texture_count, tjs_uint32 *pbo_count);
extern void TVPSetGLTextureMemoryLog(bool enable);
extern bool TVPGetGLTextureMemoryLogEnabled();
extern void TVPResetGLTextureMemoryPeak();
extern void TVPLogGLTextureMemory(const char *tag);

// 現在の GL テクスチャメモリ使用量を返す。
//   System.getTextureMemory() → %[
//     texture: ...,       // テクスチャ実体の合計バイト数
//     pbo: ...,           // アップロード用 PBO の合計バイト数 (遅延確保)
//     total: ...,         // 合計
//     peak: ...,          // total の最大値
//     textureCount: ...,  // 生存テクスチャ数
//     pboCount: ...       // 生存 PBO 数
//   ]
// 引数に文字列を渡すと、 その名前付きで 1 行ログにも出す
// (シーン境界などに目印を打ちたいとき用)。
TJS_BEGIN_NATIVE_METHOD_DECL(/*func. name*/getTextureMemory)
{
	tjs_uint64 tex = 0, pbo = 0, peak = 0;
	tjs_uint32 texcount = 0, pbocount = 0;
	TVPGetGLTextureMemory(&tex, &pbo, &peak, &texcount, &pbocount);

	if (numparams >= 1 && param[0]->Type() != tvtVoid) {
		ttstr tag = *param[0];
		tTJSNarrowStringHolder narrow(tag.c_str());
		TVPLogGLTextureMemory((const char *)narrow);
	}

	iTJSDispatch2 *dict = TJSCreateDictionaryObject();
	if (!dict) return TJS_E_FAIL;
	auto setVal = [&](const tjs_char *name, tjs_uint64 val) {
		tTJSVariant v(static_cast<tjs_int64>(val));
		dict->PropSet(TJS_MEMBERENSURE, name, nullptr, &v, dict);
	};
	setVal(TJS_W("texture"),      tex);
	setVal(TJS_W("pbo"),          pbo);
	setVal(TJS_W("total"),        tex + pbo);
	setVal(TJS_W("peak"),         peak);
	setVal(TJS_W("textureCount"), texcount);
	setVal(TJS_W("pboCount"),     pbocount);

	if (result) *result = tTJSVariant(dict, dict);
	dict->Release();
	return TJS_S_OK;
}
TJS_END_NATIVE_STATIC_METHOD_DECL_OUTER(/*object to register*/cls,
	/*func. name*/getTextureMemory)
//----------------------------------------------------------------------
// GL テクスチャメモリ合計が一定量 (8MiB) 増減するたびに log へ出す機能の
// ON/OFF。引数なしで toggle、bool 引数で明示制御。既定は OFF
// (GLTexture::MemLogEnabled = false)。
TJS_BEGIN_NATIVE_METHOD_DECL(/*func. name*/setTextureMemoryLog)
{
	bool enable;
	if (numparams >= 1 && param[0]->Type() != tvtVoid) {
		enable = ((tjs_int)*param[0]) != 0;
	} else {
		enable = !TVPGetGLTextureMemoryLogEnabled();
	}
	TVPSetGLTextureMemoryLog(enable);
	if (result) *result = (tjs_int)(enable ? 1 : 0);
	return TJS_S_OK;
}
TJS_END_NATIVE_STATIC_METHOD_DECL_OUTER(/*object to register*/cls,
	/*func. name*/setTextureMemoryLog)
//----------------------------------------------------------------------
// GL テクスチャメモリの peak を現在値へリセットする。
// 「ここから先の最大」を測りたいときに使う。
TJS_BEGIN_NATIVE_METHOD_DECL(/*func. name*/resetTextureMemoryPeak)
{
	TVPResetGLTextureMemoryPeak();
	return TJS_S_OK;
}
TJS_END_NATIVE_STATIC_METHOD_DECL_OUTER(/*object to register*/cls,
	/*func. name*/resetTextureMemoryPeak)
//----------------------------------------------------------------------
#endif // TVP_USE_OPENGL
//----------------------------------------------------------------------
TJS_BEGIN_NATIVE_METHOD_DECL(/*func. name*/nullpo)
{
	// force make a null-po
#ifdef __GNUC__
	__builtin_trap();
#else
	*(int *)0  = 0;
#endif

	return TJS_S_OK;
}
TJS_END_NATIVE_STATIC_METHOD_DECL_OUTER(/*object to register*/cls,
	/*func. name*/nullpo)
//---------------------------------------------------------------------------

//----------------------------------------------------------------------

//-- properties

//----------------------------------------------------------------------
TJS_BEGIN_NATIVE_PROP_DECL(exePath)
{
	TJS_BEGIN_NATIVE_PROP_GETTER
	{
		*result = TVPGetAppPath();
		return TJS_S_OK;
	}
	TJS_END_NATIVE_PROP_GETTER

	TJS_DENY_NATIVE_PROP_SETTER
}
TJS_END_NATIVE_STATIC_PROP_DECL_OUTER(cls, exePath)
//----------------------------------------------------------------------
// エンジン組み込みリソースの置き場 (末尾 '/' 付き)。
// desktop = "resource://./" / wasm = "file://./resource/" のように
// プラットフォームで変わるので、同梱フォント等を参照するときはこれを前置する。
TJS_BEGIN_NATIVE_PROP_DECL(resourcePath)
{
	TJS_BEGIN_NATIVE_PROP_GETTER
	{
		*result = TVPGetResourcePath();
		return TJS_S_OK;
	}
	TJS_END_NATIVE_PROP_GETTER

	TJS_DENY_NATIVE_PROP_SETTER
}
TJS_END_NATIVE_STATIC_PROP_DECL_OUTER(cls, resourcePath)
//----------------------------------------------------------------------
// -replweb で開いているブラウザ REPL ビューワーの URL。未起動なら空文字列。
TJS_BEGIN_NATIVE_PROP_DECL(replWebURL)
{
	TJS_BEGIN_NATIVE_PROP_GETTER
	{
#ifdef KRKRZ_REPL_WEB
		*result = TVPReplWeb::GetURL();
#else
		*result = ttstr();
#endif
		return TJS_S_OK;
	}
	TJS_END_NATIVE_PROP_GETTER

	TJS_DENY_NATIVE_PROP_SETTER
}
TJS_END_NATIVE_STATIC_PROP_DECL_OUTER(cls, replWebURL)
//----------------------------------------------------------------------
TJS_BEGIN_NATIVE_PROP_DECL(dataPath)
{
	TJS_BEGIN_NATIVE_PROP_GETTER
	{
		*result = TVPDataPath;
		return TJS_S_OK;
	}
	TJS_END_NATIVE_PROP_GETTER

	TJS_DENY_NATIVE_PROP_SETTER
}
TJS_END_NATIVE_STATIC_PROP_DECL_OUTER(cls, dataPath)
//----------------------------------------------------------------------
// WINVER 互換のユーザーフォルダ参照。 KAG (MainWindow.tjs checkSave) が
// セーブ場所の書き込みに失敗したときのフォールバック先として参照する。
// SDL 版に無いと「メンバ "personalPath" が見つかりません」で落ちる。
// Windows 以外は専用フォルダを提供しないので exePath を返す
// (スクリプト側は exePath と等しければ「別置き場なし」として扱う)。
TJS_BEGIN_NATIVE_PROP_DECL(personalPath)
{
	TJS_BEGIN_NATIVE_PROP_GETTER
	{
#ifdef _WIN32
		// "My Documents" (無ければ RoamingAppData)。 WINVER の
		// TVPGetPersonalPath と同じ解決順・同じ正規化。
		tjs_string path = ApplicationSpecialPath::GetPersonalPath();
		if(!path.empty()) {
			ttstr p = TVPNormalizeStorageName(ttstr(path.c_str()));
			if(p.GetLastChar() != TJS_W('/')) p += TJS_W('/');
			*result = p;
			return TJS_S_OK;
		}
#endif
		*result = TVPGetAppPath();
		return TJS_S_OK;
	}
	TJS_END_NATIVE_PROP_GETTER

	TJS_DENY_NATIVE_PROP_SETTER
}
TJS_END_NATIVE_STATIC_PROP_DECL_OUTER(cls, personalPath)
//----------------------------------------------------------------------
TJS_BEGIN_NATIVE_PROP_DECL(appDataPath)
{
	TJS_BEGIN_NATIVE_PROP_GETTER
	{
#ifdef _WIN32
		// RoamingAppData (WINVER の TVPGetAppDataPath と同じ)
		tjs_string path = ApplicationSpecialPath::GetAppDataPath();
		if(!path.empty()) {
			ttstr p = TVPNormalizeStorageName(ttstr(path.c_str()));
			if(p.GetLastChar() != TJS_W('/')) p += TJS_W('/');
			*result = p;
			return TJS_S_OK;
		}
#endif
		*result = TVPGetAppPath();
		return TJS_S_OK;
	}
	TJS_END_NATIVE_PROP_GETTER

	TJS_DENY_NATIVE_PROP_SETTER
}
TJS_END_NATIVE_STATIC_PROP_DECL_OUTER(cls, appDataPath)
//----------------------------------------------------------------------
TJS_BEGIN_NATIVE_PROP_DECL(exeName)
{
	TJS_BEGIN_NATIVE_PROP_GETTER
	{
		static ttstr exename(TVPNormalizeStorageName(Application->ExePath()));
		*result = exename;
		return TJS_S_OK;
	}
	TJS_END_NATIVE_PROP_GETTER

	TJS_DENY_NATIVE_PROP_SETTER
}
TJS_END_NATIVE_STATIC_PROP_DECL_OUTER(cls, exeName)
//----------------------------------------------------------------------
TJS_BEGIN_NATIVE_PROP_DECL(title)
{
	TJS_BEGIN_NATIVE_PROP_GETTER
	{
		if(!TVPAppTitleInit)
		{
			TVPAppTitleInit = true;
			TVPAppTitle = Application->GetTitle();
		}
		*result = TVPAppTitle;
		return TJS_S_OK;
	}
	TJS_END_NATIVE_PROP_GETTER

	TJS_BEGIN_NATIVE_PROP_SETTER
	{
		TVPAppTitle = *param;
		Application->SetTitle( TVPAppTitle.AsStdString() );
		return TJS_S_OK;
	}
	TJS_END_NATIVE_PROP_SETTER
}
TJS_END_NATIVE_STATIC_PROP_DECL_OUTER(cls, title)
//----------------------------------------------------------------------
TJS_BEGIN_NATIVE_PROP_DECL(screenWidth)
{
	TJS_BEGIN_NATIVE_PROP_GETTER
	{
		*result = Application->ScreenWidth();
		return TJS_S_OK;
	}
	TJS_END_NATIVE_PROP_GETTER

	TJS_DENY_NATIVE_PROP_SETTER
}
TJS_END_NATIVE_STATIC_PROP_DECL_OUTER(cls, screenWidth)
//----------------------------------------------------------------------
TJS_BEGIN_NATIVE_PROP_DECL(screenHeight)
{
	TJS_BEGIN_NATIVE_PROP_GETTER
	{
		*result = Application->ScreenHeight();
		return TJS_S_OK;
	}
	TJS_END_NATIVE_PROP_GETTER

	TJS_DENY_NATIVE_PROP_SETTER
}
TJS_END_NATIVE_STATIC_PROP_DECL_OUTER(cls, screenHeight)
//----------------------------------------------------------------------
// デスクトップ (作業領域) の矩形。 win32 版 krkrz の System.desktop* 互換。
// ウィンドウ位置の画面内クランプ等に使う。
TJS_BEGIN_NATIVE_PROP_DECL(desktopLeft)
{
	TJS_BEGIN_NATIVE_PROP_GETTER
	{
		*result = Application->DesktopLeft();
		return TJS_S_OK;
	}
	TJS_END_NATIVE_PROP_GETTER

	TJS_DENY_NATIVE_PROP_SETTER
}
TJS_END_NATIVE_STATIC_PROP_DECL_OUTER(cls, desktopLeft)
//----------------------------------------------------------------------
TJS_BEGIN_NATIVE_PROP_DECL(desktopTop)
{
	TJS_BEGIN_NATIVE_PROP_GETTER
	{
		*result = Application->DesktopTop();
		return TJS_S_OK;
	}
	TJS_END_NATIVE_PROP_GETTER

	TJS_DENY_NATIVE_PROP_SETTER
}
TJS_END_NATIVE_STATIC_PROP_DECL_OUTER(cls, desktopTop)
//----------------------------------------------------------------------
TJS_BEGIN_NATIVE_PROP_DECL(desktopWidth)
{
	TJS_BEGIN_NATIVE_PROP_GETTER
	{
		*result = Application->DesktopWidth();
		return TJS_S_OK;
	}
	TJS_END_NATIVE_PROP_GETTER

	TJS_DENY_NATIVE_PROP_SETTER
}
TJS_END_NATIVE_STATIC_PROP_DECL_OUTER(cls, desktopWidth)
//----------------------------------------------------------------------
TJS_BEGIN_NATIVE_PROP_DECL(desktopHeight)
{
	TJS_BEGIN_NATIVE_PROP_GETTER
	{
		*result = Application->DesktopHeight();
		return TJS_S_OK;
	}
	TJS_END_NATIVE_PROP_GETTER

	TJS_DENY_NATIVE_PROP_SETTER
}
TJS_END_NATIVE_STATIC_PROP_DECL_OUTER(cls, desktopHeight)
//----------------------------------------------------------------------
TJS_BEGIN_NATIVE_PROP_DECL(touchDevice)
{
	TJS_BEGIN_NATIVE_PROP_GETTER
	{
		*result = TVPGetSupportTouchDevice();
		return TJS_S_OK;
	}
	TJS_END_NATIVE_PROP_GETTER

	TJS_DENY_NATIVE_PROP_SETTER
}
TJS_END_NATIVE_STATIC_PROP_DECL_OUTER(cls, touchDevice)


TJS_BEGIN_NATIVE_METHOD_DECL(/*func. name*/getJoypadType)
{
	tjs_int no = numparams > 0 ? (tjs_int)*param[0] : 0;
	if (result) {
		*result = Application->GetJoypadType(no);
	}
	return TJS_S_OK;
}
TJS_END_NATIVE_STATIC_METHOD_DECL_OUTER(/*object to register*/cls,
	/*func. name*/getJoypadType)


TJS_BEGIN_NATIVE_METHOD_DECL(/*func. name*/rumblePad)
{
	if(numparams < 4) return TJS_E_BADPARAMCOUNT;
	tjs_int no = (tjs_int)*param[0];
	tjs_int low = (tjs_int)*param[1];
	tjs_int high = (tjs_int)*param[2];
	tjs_int duration = (tjs_int)*param[3];
	bool ret = Application->RumbleGamepad(no, low, high, duration);
	if(result) *result = (tjs_int)ret;
	return TJS_S_OK;
}
TJS_END_NATIVE_STATIC_METHOD_DECL_OUTER(/*object to register*/cls,
	/*func. name*/rumblePad)


TJS_BEGIN_NATIVE_METHOD_DECL(/*func. name*/stopRumblePad)
{
	tjs_int no = numparams > 0 ? (tjs_int)*param[0] : 0;
	bool ret = Application->StopRumbleGamepad(no);
	if(result) *result = (tjs_int)ret;
	return TJS_S_OK;
}
TJS_END_NATIVE_STATIC_METHOD_DECL_OUTER(/*object to register*/cls,
	/*func. name*/stopRumblePad)


TJS_BEGIN_NATIVE_METHOD_DECL(/*func. name*/getJoypadCount)
{
	if(result) *result = Application->GetJoypadCount();
	return TJS_S_OK;
}
TJS_END_NATIVE_STATIC_METHOD_DECL_OUTER(/*object to register*/cls,
	/*func. name*/getJoypadCount)


TJS_BEGIN_NATIVE_METHOD_DECL(/*func. name*/hasJoypad)
{
	tjs_int no = numparams > 0 ? (tjs_int)*param[0] : 0;
	if(result) *result = (tjs_int)Application->HasJoypad(no);
	return TJS_S_OK;
}
TJS_END_NATIVE_STATIC_METHOD_DECL_OUTER(/*object to register*/cls,
	/*func. name*/hasJoypad)


// 指定パッドの指定軸のアナログ値を返す (doc/Gamepad.md §3)。
// 第1引数: パッド番号 (現状 0 のみ有効)
// 第2引数: 軸 ID (System.padAxisLeftX 等の定数を使用)
// 戻り値:  スティック -1.0〜+1.0、トリガ 0.0〜+1.0、未接続/無効値で 0.0
TJS_BEGIN_NATIVE_METHOD_DECL(/*func. name*/getPadAxis)
{
	if(numparams < 2) return TJS_E_BADPARAMCOUNT;
	tjs_int no     = (tjs_int)*param[0];
	tjs_int axisId = (tjs_int)*param[1];
	float v = Application->GetPadAxis(no, axisId);
	if(result) *result = (tjs_real)v;
	return TJS_S_OK;
}
TJS_END_NATIVE_STATIC_METHOD_DECL_OUTER(/*object to register*/cls,
	/*func. name*/getPadAxis)


// パッド軸 ID 定数 (readonly)。値は SDL_GamepadAxis と同値だが、スクリプト側は
// SDL に依存せずに System.padAxis* で参照可能 (詳細 doc/Gamepad.md §3)。
#define TVP_DEF_PAD_AXIS_PROP(propname, value) \
	TJS_BEGIN_NATIVE_PROP_DECL(propname) \
	{ \
		TJS_BEGIN_NATIVE_PROP_GETTER \
		{ \
			*result = (tjs_int)(value); \
			return TJS_S_OK; \
		} \
		TJS_END_NATIVE_PROP_GETTER \
		TJS_DENY_NATIVE_PROP_SETTER \
	} \
	TJS_END_NATIVE_STATIC_PROP_DECL_OUTER(cls, propname)

TVP_DEF_PAD_AXIS_PROP(padAxisLeftX,         tTVPApplication::TVP_PAD_AXIS_LEFTX)
TVP_DEF_PAD_AXIS_PROP(padAxisLeftY,         tTVPApplication::TVP_PAD_AXIS_LEFTY)
TVP_DEF_PAD_AXIS_PROP(padAxisRightX,        tTVPApplication::TVP_PAD_AXIS_RIGHTX)
TVP_DEF_PAD_AXIS_PROP(padAxisRightY,        tTVPApplication::TVP_PAD_AXIS_RIGHTY)
TVP_DEF_PAD_AXIS_PROP(padAxisLeftTrigger,   tTVPApplication::TVP_PAD_AXIS_LEFT_TRIGGER)
TVP_DEF_PAD_AXIS_PROP(padAxisRightTrigger,  tTVPApplication::TVP_PAD_AXIS_RIGHT_TRIGGER)

#undef TVP_DEF_PAD_AXIS_PROP


// ゲームパッド機能の有効/無効 (読み書き)。CLI -joypad より優先される。
// 他デバイスの誤パッド認識による誤動作を実行時に回避する用途など。
TJS_BEGIN_NATIVE_PROP_DECL(padEnabled)
{
	TJS_BEGIN_NATIVE_PROP_GETTER
	{
		*result = (tjs_int)(Application->GetJoypadEnabled() ? 1 : 0);
		return TJS_S_OK;
	}
	TJS_END_NATIVE_PROP_GETTER

	TJS_BEGIN_NATIVE_PROP_SETTER
	{
		Application->SetJoypadEnabled(param->operator bool());
		return TJS_S_OK;
	}
	TJS_END_NATIVE_PROP_SETTER
}
TJS_END_NATIVE_STATIC_PROP_DECL_OUTER(cls, padEnabled)


// VK_PAD1..4 (A/B/X/Y) をどのフェイスボタンに割り当てるか (読み書き)。
//   "label"    … ボタンの刻印に合わせる (既定)。任天堂系は SOUTH=B / EAST=A /
//                WEST=Y / NORTH=X なので、位置基準とは A/B・X/Y が入れ替わる。
//                PlayStation は ×→A / ○→B / □→X / △→Y (位置基準と同結果)。
//   "position" … SDL の位置 (SOUTH/EAST/WEST/NORTH) をそのまま A/B/X/Y と読む
//                (Xbox 名準拠。従来互換)。
// 起動オプション -padbuttons=label|position と同じもの。詳細 doc/Gamepad.md。
TJS_BEGIN_NATIVE_PROP_DECL(padButtonMapping)
{
	TJS_BEGIN_NATIVE_PROP_GETTER
	{
		*result = Application->GetPadButtonMappingByLabel() ?
			ttstr(TJS_W("label")) : ttstr(TJS_W("position"));
		return TJS_S_OK;
	}
	TJS_END_NATIVE_PROP_GETTER

	TJS_BEGIN_NATIVE_PROP_SETTER
	{
		ttstr s = *param;
		if (s == TJS_W("label")) {
			Application->SetPadButtonMappingByLabel(true);
		} else if (s == TJS_W("position")) {
			Application->SetPadButtonMappingByLabel(false);
		} else {
			TVPAddLog(TJS_W("(warning) System.padButtonMapping: must be \"label\" or \"position\""));
			return TJS_E_INVALIDPARAM;
		}
		return TJS_S_OK;
	}
	TJS_END_NATIVE_PROP_SETTER
}
TJS_END_NATIVE_STATIC_PROP_DECL_OUTER(cls, padButtonMapping)


// 接続しているパッドのボタン表記の系統 (読み取り専用)。
//   "xbox" / "ps" / "switch"。 判らないときは空文字列。
//   画面に出すボタン絵をどれにするかの判断に使う
//   (ElementsDialog.setPadTheme("auto") はこれを見ている)。
TJS_BEGIN_NATIVE_PROP_DECL(padStyle)
{
	TJS_BEGIN_NATIVE_PROP_GETTER
	{
		*result = ttstr(Application->GetJoypadStyle(0).c_str());
		return TJS_S_OK;
	}
	TJS_END_NATIVE_PROP_GETTER

	TJS_DENY_NATIVE_PROP_SETTER
}
TJS_END_NATIVE_STATIC_PROP_DECL_OUTER(cls, padStyle)


TJS_BEGIN_NATIVE_METHOD_DECL(/*func. name*/addFont)
{
	// show simple message box
	if(numparams < 1) return TJS_E_BADPARAMCOUNT;

	ttstr storage = *param[0];
	std::vector<tjs_string> faces;
	TVPAddFontToFreeType( storage, &faces);

	return TJS_S_OK;
}
TJS_END_NATIVE_STATIC_METHOD_DECL_OUTER(/*object to register*/cls,
	/*func. name*/addFont)

	return cls;

}
//---------------------------------------------------------------------------


