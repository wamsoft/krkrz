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

//#include "GraphicsLoaderImpl.h"

#include "SystemImpl.h"
#include "SystemIntf.h"
#include "SysInitIntf.h"
#include "StorageIntf.h"
//#include "StorageImpl.h"
#include "TickCount.h"
#include "ComplexRect.h"
//#include "WindowImpl.h"
#include "EventIntf.h"
//#include "DInputMgn.h"

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
	// REPL 駆動中はネイティブのブロッキング message box を出さず、 内容を REPL
	// コンソール (= ログ) に流す。 System.inform 等がエージェントから見える
	// ようにするため (応答取得は将来拡張、 現状は既定応答で進む)。
	if (TVPReplActive) {
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
// SDL3 build 限定: 画面右上にメモリ状態のリアルタイム折れ線グラフを
// オーバレイ表示する。引数なしで toggle、bool 引数指定で明示制御。
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
// SDL3 build 限定: 画面左上にゲームパッド 16 ボタンのマトリクスを
// オーバレイ表示する。引数なしで toggle、bool 引数指定で明示制御。
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


