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

#include <shellapi.h>
#include <shlobj.h>

#include "GraphicsLoaderImpl.h"

#include "SystemImpl.h"
#include "SystemIntf.h"
#include "SysInitIntf.h"
#include "StorageIntf.h"
#include "StorageImpl.h"
#include "TickCount.h"
#include "ComplexRect.h"
#include "WindowImpl.h"
#include "SystemControl.h"
#include "DInputMgn.h"

#include "Application.h"
#include "TVPScreen.h"
// CompatibleNativeFuncs は撤去 (touch API は Win10 で常在、直接リンク)
#include "DebugIntf.h"
#include "VersionFormUnit.h"
#include "PluginImpl.h"
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

//---------------------------------------------------------------------------
static ttstr TVPAppTitle;
static bool TVPAppTitleInit = false;
extern ttstr TVPGetLicenseString();
//---------------------------------------------------------------------------




//---------------------------------------------------------------------------
// TVPShowSimpleMessageBox
//---------------------------------------------------------------------------
static void TVPShowSimpleMessageBox(const ttstr & text, const ttstr & caption)
{
	HWND hWnd = TVPGetModalWindowOwnerHandle();
	if( hWnd == INVALID_HANDLE_VALUE ) {
		hWnd = NULL;
	}
	::MessageBox( hWnd, (const wchar_t*)text.AsStdString().c_str(), (const wchar_t*)caption.AsStdString().c_str(), MB_OK|MB_ICONINFORMATION );
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

	if(keycode >= VK_PAD_FIRST  && keycode <= VK_PAD_LAST)
	{
		// JoyPad related keys are treated in DInputMgn.cpp
		return TVPGetJoyPadAsyncState(keycode, getcurrent);
	}

	if(keycode == VK_LBUTTON || keycode == VK_RBUTTON)
	{
		// check whether the mouse button is swapped
		if(GetSystemMetrics(SM_SWAPBUTTON))
		{
			// mouse button had been swapped; swap key code
			if(keycode == VK_LBUTTON)
				keycode = VK_RBUTTON;
			else
				keycode = VK_LBUTTON;
		}
	}

	return 0!=( GetAsyncKeyState(keycode) & ( getcurrent?0x8000:0x0001) );
}
//---------------------------------------------------------------------------







//---------------------------------------------------------------------------
// TVPGetPlatformName
//---------------------------------------------------------------------------
ttstr TVPGetPlatformName()
{
	SYSTEM_INFO sysInfo;
	::GetNativeSystemInfo( &sysInfo );
	switch( sysInfo.wProcessorArchitecture )
	{
		case PROCESSOR_ARCHITECTURE_AMD64:
		case PROCESSOR_ARCHITECTURE_IA64:
			return ttstr(TJS_W("Win64"));

		case PROCESSOR_ARCHITECTURE_INTEL:
		case PROCESSOR_ARCHITECTURE_UNKNOWN:
		default:
			return ttstr(TJS_W("Win32"));
	}
}
//---------------------------------------------------------------------------



typedef void (WINAPI *RtlGetVersionFunc)(OSVERSIONINFOEX* );
//---------------------------------------------------------------------------
// TVPGetOSName
//---------------------------------------------------------------------------
ttstr TVPGetOSName()
{
	OSVERSIONINFOEX ovi;
	ovi.dwOSVersionInfoSize = sizeof(ovi);

	bool isGetVersion = false;
	HMODULE hModule = ::LoadLibrary( L"ntdll.dll" );
	if( hModule ) {
		RtlGetVersionFunc func;
		func = (RtlGetVersionFunc)::GetProcAddress( hModule, "RtlGetVersion" );
		if( func ) {
			func( &ovi );
			isGetVersion = true;
		}
		::FreeLibrary( hModule );
		hModule = NULL;
	}
	if( isGetVersion == false ) {
		// Probably do not call on Windows NT
#pragma warning(push)
#pragma warning(disable:4996)
		::GetVersionEx((OSVERSIONINFO*)&ovi);
#pragma warning(pop)
	}
	tjs_char buf[256];
	const tjs_char *osname = NULL;

	// エンジンは Phase0 で _WIN32_WINNT=0x0A00 (Win10) を下限にしたため、
	// 実行され得る OS は Windows 10 以降のみ。RtlGetVersion はマニフェスト
	// シムの影響を受けず実バージョンを返す。
	// Windows 11 は major=10 のままなので build 番号 (>=22000) で区別する。
	if( ovi.dwMajorVersion == 10 ) {
		if( ovi.wProductType == VER_NT_WORKSTATION ) {
			if( ovi.dwBuildNumber >= 22000 )
				osname = TJS_W("Windows 11");
			else
				osname = TJS_W("Windows 10");
		} else {
			// Server は build 番号で世代判定
			if( ovi.dwBuildNumber >= 26100 )
				osname = TJS_W("Windows Server 2025");
			else if( ovi.dwBuildNumber >= 20348 )
				osname = TJS_W("Windows Server 2022");
			else if( ovi.dwBuildNumber >= 17763 )
				osname = TJS_W("Windows Server 2019");
			else
				osname = TJS_W("Windows Server 2016");
		}
	} else {
		// Win10 未満はサポート外 (通常到達しない)。将来の major 繰り上げ用の保険。
		osname = TJS_W("Windows");
	}

	TJS_snprintf(buf, sizeof(buf)/sizeof(tjs_char), TJS_W("%ls %d.%d.%d "), osname, ovi.dwMajorVersion,
		ovi.dwMinorVersion, ovi.dwBuildNumber);

	ttstr str(buf);
	str += ttstr( ovi.szCSDVersion );

	tjs_int major;
	tjs_int minor;
	tjs_int release;
	tjs_int build;
	TVPGetFileVersionOf( TJS_W( "kernel32.dll" ), major, minor, release, build );
	//TJS_snprintf( buf, sizeof( buf ) / sizeof( tjs_char ), TJS_W( " kernel32.dll %d.%d Release %d Build %d " ), major, minor, release, build );
	//str += ttstr( buf );
	if( major >= 10 ) {
		if( release >= 17134 ) {
			str += ttstr( TJS_W("April 2018 Update or later") );
		} else if( release >= 16299 ) {
			str += ttstr( TJS_W("Fall Creators Update") );
		} else if( release >= 15063 ) {
			str += ttstr( TJS_W( "Creators Update" ) );
		} else if( release >= 14393 ) {
			str += ttstr( TJS_W( "Anniversary Update" ) );
		} else if( release >= 10586 ) {
			str += ttstr( TJS_W( "November Update" ) );
		}
	}

	return str;
}
//---------------------------------------------------------------------------

//---------------------------------------------------------------------------
// TVPGetOSBits
//---------------------------------------------------------------------------
tjs_int TVPGetOSBits()
{
	SYSTEM_INFO sysInfo;
	::GetNativeSystemInfo( &sysInfo );
	switch( sysInfo.wProcessorArchitecture )
	{
		case PROCESSOR_ARCHITECTURE_AMD64:
		case PROCESSOR_ARCHITECTURE_IA64:
			return 64;
		case PROCESSOR_ARCHITECTURE_INTEL:
		case PROCESSOR_ARCHITECTURE_UNKNOWN:
		default:
			return 32;
	}
}
//---------------------------------------------------------------------------


//---------------------------------------------------------------------------
// TVPShellExecute
//---------------------------------------------------------------------------
bool TVPShellExecute(const ttstr &target, const ttstr &param)
{
	// open or execute target file
//	ttstr file = TVPGetNativeName(TVPNormalizeStorageName(target));
	if(::ShellExecute(NULL, NULL,
		(const wchar_t*)target.c_str(),
		param.IsEmpty() ? NULL : (const wchar_t*)param.c_str(),
		L"",
		SW_SHOWNORMAL)
		<=(void *)32)
	{
		return false;
	}
	else
	{
		return true;
	}
}
//---------------------------------------------------------------------------




//---------------------------------------------------------------------------
// TVPReadRegValue
//---------------------------------------------------------------------------
static void TVPReadRegValue(tTJSVariant &result, const ttstr & key)
{
	// open specified registry key
	if(key.IsEmpty()) { result.Clear(); return; }

	// check whether the key contains root key name
	HKEY root = HKEY_CURRENT_USER;
	const tjs_char *key_p = key.c_str();

	if(key.StartsWith(TJS_W("HKEY_CLASSES_ROOT")))
	{
		key_p += 17;
		root = HKEY_CLASSES_ROOT;
	}
	else if(key.StartsWith(TJS_W("HKEY_CURRENT_CONFIG")))
	{
		key_p += 19;
		root = HKEY_CURRENT_CONFIG;
	}
	else if(key.StartsWith(TJS_W("HKEY_CURRENT_USER")))
	{
		key_p += 17;
		root = HKEY_CURRENT_USER;
	}
	else if(key.StartsWith(TJS_W("HKEY_LOCAL_MACHINE")))
	{
		key_p += 18;
		root = HKEY_LOCAL_MACHINE;
	}
	else if(key.StartsWith(TJS_W("HKEY_USERS")))
	{
		key_p += 10;
		root = HKEY_USERS;
	}
	else if(key.StartsWith(TJS_W("HKEY_PERFORMANCE_DATA")))
	{
		key_p += 21;
		root = HKEY_PERFORMANCE_DATA;
	}
	else if(key.StartsWith(TJS_W("HKEY_DYN_DATA")))
	{
		key_p += 13;
		root = HKEY_DYN_DATA;
	}

	if(*key_p == TJS_W('\\')) key_p ++;

	// search value name
	const tjs_char *start = key_p;
	key_p += TJS_strlen(key_p);
	key_p--;
	while(start <= key_p && *key_p != TJS_W('\\')) key_p--;
	ttstr valuename(key_p+1);
	if(key_p < start || *key_p != TJS_W('\\')) key_p++;

	ttstr keyname(start, (int)(key_p - start));

	// open key
	HKEY handle;
	LONG res = RegOpenKeyEx(root, (const wchar_t*)keyname.AsStdString().c_str(), 0, KEY_READ, &handle);
	if(res != ERROR_SUCCESS) { result.Clear(); return; }

	// try query value size and read key
	DWORD size;
	DWORD type;

	// query size
	res = RegQueryValueEx(handle, (const wchar_t*)valuename.c_str(), 0, &type, NULL, &size);

	if(res != ERROR_SUCCESS)
	{
		RegCloseKey(handle);
		result.Clear();
		return;
	}


	switch(type)
	{
	case REG_DWORD:
//	case REG_DWORD_LITTLE_ENDIAN: // is actually the same as REG_DWORD
	case REG_DWORD_BIG_ENDIAN:
	case REG_EXPAND_SZ:
	case REG_SZ:
		break; // these should be OK

	case REG_MULTI_SZ: // sorry not yet implemented
	case REG_BINARY:
	case REG_LINK:
	case REG_NONE:
	case REG_RESOURCE_LIST:
	default:
		// not capable types
		RegCloseKey(handle);
		result.Clear();
		return;
	}

	while(true)
	{
		tjs_uint8 * data = new tjs_uint8[size];

		try
		{
			DWORD size2 = size;
			res = RegQueryValueEx(handle, (const wchar_t*)valuename.c_str(), 0, NULL, data, &size2);

			if(res == ERROR_MORE_DATA)
			{
				// more data required
				delete [] data;
				size += 1024;
				continue;
			}
			else if(res != ERROR_SUCCESS)
			{
				RegCloseKey(handle);
				result.Clear();
				return;
			}

			// query succeeded


			// store data into result
			switch(type)
			{
			case REG_DWORD:
//			case REG_DWORD_LITTLE_ENDIAN:
				result = (tTVInteger)*(DWORD*)data;
				break;

			case REG_DWORD_BIG_ENDIAN:
				{
					DWORD val = *(DWORD*)data;
					val = (val >> 24) + ((val >> 8) & 0x0000ff00) +
						((val << 8) & 0x00ff0000) + (val << 24);
					result = (tTVInteger)val;
			  	}
				break;

			case REG_EXPAND_SZ:
			case REG_SZ:
				// data is stored in unicode
				result = ttstr((const tjs_char*)data, size / sizeof(tjs_char));
				break;
			}
		}
		catch(...)
		{
			RegCloseKey(handle);
			delete [] data;
			throw;
		}
		RegCloseKey(handle);
		delete [] data;

		break;
	}
}
//---------------------------------------------------------------------------




//---------------------------------------------------------------------------
// Static function for retrieving special folder path
//---------------------------------------------------------------------------
// 旧実装は非推奨の SHGetSpecialFolderPath + CSIDL_* を使っていたが、
// Vista 以降の推奨 API である SHGetKnownFolderPath + FOLDERID_* に置換。
static ttstr TVPGetKnownFolderPath(REFKNOWNFOLDERID rfid)
{
	ttstr result;
	PWSTR ppszPath = NULL;
	if( SUCCEEDED( ::SHGetKnownFolderPath(rfid, 0, NULL, &ppszPath) ) && ppszPath )
	{
		result = ttstr( (const tjs_char*)ppszPath );
	}
	if( ppszPath ) ::CoTaskMemFree( ppszPath );
	return result;
}
//---------------------------------------------------------------------------




//---------------------------------------------------------------------------
// TVPGetPersonalPath
//---------------------------------------------------------------------------
ttstr TVPGetPersonalPath()
{
	// Retrieve personal directory;
	// This usually refers "My Documents".
	// If this is not exist, returns application data path, then exe path.
	// for windows vista, this refers application data path.
	ttstr path;
	path = TVPGetKnownFolderPath(FOLDERID_Documents);
	if(path.IsEmpty())
		path = TVPGetKnownFolderPath(FOLDERID_RoamingAppData);

	if(!path.IsEmpty())
	{
		path = TVPNormalizeStorageName(path);
		if(path.GetLastChar() != TJS_W('/')) path += TJS_W('/');
		return path;
	}

	return TVPGetAppPath();
}
//---------------------------------------------------------------------------




//---------------------------------------------------------------------------
// TVPGetAppDataPath
//---------------------------------------------------------------------------
ttstr TVPGetAppDataPath()
{
	// Retrieve application data directory;
	// If this is not exist, returns application exe path.

	ttstr path = TVPGetKnownFolderPath(FOLDERID_RoamingAppData);

	if(!path.IsEmpty())
	{
		path = TVPNormalizeStorageName(path);
		if(path.GetLastChar() != TJS_W('/')) path += TJS_W('/');
		return path;
	}

	return TVPGetAppPath();
}
//---------------------------------------------------------------------------



//---------------------------------------------------------------------------
// TVPGetSavedGamesPath
//---------------------------------------------------------------------------
ttstr TVPGetSavedGamesPath()
{
	ttstr path;
	PWSTR ppszPath = NULL;
	HRESULT hr = ::SHGetKnownFolderPath(FOLDERID_SavedGames, 0, NULL, &ppszPath);
	if( hr == S_OK ) {
		path = ttstr( ppszPath );
		::CoTaskMemFree( ppszPath );
	}
	return path;
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
	CreateMutex(NULL, TRUE, (const wchar_t*)lockname.c_str());

	if(GetLastError())
	{
		return false; // already running
	}


	// No need to release the mutex object because the mutex is automatically
	// released when the calling thread exits.

	return true;
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
	int result = 0;
	{  // タッチ API は Win10 で常在 (SM_DIGITIZER で実接続を判定)
		int value = ::GetSystemMetrics( SM_DIGITIZER );

		if( value & NID_INTEGRATED_TOUCH ) result |= tdIntegratedTouch;
		if( value & NID_EXTERNAL_TOUCH ) result |= tdExternalTouch;
		if( value & NID_INTEGRATED_PEN ) result |= tdIntegratedPen;
		if( value & NID_EXTERNAL_PEN ) result |= tdExternalPen;
		if( value & NID_MULTI_INPUT ) result |= tdMultiInput;
		if( value & NID_READY ) result |= tdDigitizerReady;
	}
	int value = ::GetSystemMetrics( SM_MOUSEPRESENT );
	if( value ) {
		result |= tdMouse;
		value = ::GetSystemMetrics( SM_MOUSEWHEELPRESENT );
		if( value ) result |= tdMouseWheel;
	}
	return result;
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
	if(!TVPSystemControlAlive) return;

	// check the state again (because the state may change during the event delivering).
	// but note that this implementation might fire activate events even in the application
	// is already activated (the same as deactivation).
	if(activate_or_deactivate != Application->GetActivating()) return;

	// fire the event
	TVPFireOnApplicationActivateEvent(activate_or_deactivate);
}
//---------------------------------------------------------------------------

//---------------------------------------------------------------------------
void TVPHeapDump()
{
	// per-allocator stats (FileAllocator / BitmapAllocator / SoundAllocator) を先頭に出力
	TVPDumpAllocatorStats("FileAllocator", TVPGetFileAllocator());
	TVPDumpAllocatorStats("BitmapAllocator", tTVPBitmapBitsAlloc::GetAllocator());
	TVPDumpAllocatorStats("SoundAllocator", TVPGetSoundAllocator());
	TVPGlobalAllocStats::Dump();
	TVPDumpTJSObjectStats();
	TVPDumpProcessMemoryInfo();
	TVPDumpSystemAllocatorInfo();
	// キャッシュエントリ件数 (file 層 / decode 層)。pinned 数は内訳。
	// 詳細は Storages.dumpFileCacheList / dumpImageCacheList。
	{
		size_t fc_total = 0, fc_pinned = 0;
		size_t ic_total = 0, ic_pinned = 0;
		TVPGetStorageCacheCount(fc_total, fc_pinned);
		TVPGetGraphicCacheCount(ic_total, ic_pinned);
		TVPLOG_INFO("FileCache: count={} pinned={}", fc_total, fc_pinned);
		TVPLOG_INFO("ImageCache: count={} pinned={}", ic_total, ic_pinned);
	}

	tjs_char buff[128];
	HANDLE heaps[100];
	DWORD c = ::GetProcessHeaps (100, heaps);
	TJS_snprintf( buff, 128, TJS_W("The process has %d heaps."), c );
	TVPAddLog( buff );

	const HANDLE default_heap = ::GetProcessHeap();
	const HANDLE crt_heap = (HANDLE)_get_heap_handle();
	for( unsigned int i = 0; i < c; i++ ) {
		ULONG heap_info = 0;
		SIZE_T ret_size = 0;
		bool isdefault = false;
		bool isCRT = false;
		if( ::HeapQueryInformation( heaps[i], HeapCompatibilityInformation, &heap_info, sizeof(heap_info), &ret_size) ) {
			tjs_char* type = NULL;
			switch( heap_info ) {
			case 0:
				type = TJS_W("standard");
				break;
			case 1:
				type = TJS_W("LAL");
				break;
			case 2:
				type = TJS_W("LFH");
				break;
			default:
				type = TJS_W("unknown");
				break;
			}
			if( heaps[i] == default_heap ) {
				isdefault = true;
			}
			if( heaps [i] == crt_heap ) {
				isCRT = true;
			}

			PROCESS_HEAP_ENTRY entry;
			memset( &entry, 0, sizeof (entry) );
			struct Info {
				int count;
				tjs_int64 total;
				tjs_int64 overhead;
				Info() : count(0), total(0), overhead(0) {}
			} use, uncommit, unused;
			while( ::HeapWalk( heaps[i], &entry) ) {
				if( entry.wFlags & PROCESS_HEAP_ENTRY_BUSY ) {
					use.count++;
					use.total += entry.cbData;
					use.overhead += entry.cbOverhead;
				} else if( entry.wFlags & PROCESS_HEAP_UNCOMMITTED_RANGE ) {
					uncommit.count++;
					uncommit.total += entry.cbData;
					uncommit.overhead += entry.cbOverhead;
				} else {
					unused.count++;
					unused.total += entry.cbData;
					unused.overhead += entry.cbOverhead;
				}
			}
			ttstr mes( TJS_W("#") );
			mes += ttstr((tjs_int)(i+1)) + TJS_W(" type: ") + type;
			if( isdefault ) mes += TJS_W(" [default]");
			if( isCRT ) mes += TJS_W(" [CRT]");
			TVPAddLog( mes );
			TJS_snprintf( buff, 128, TJS_W("  Allocated: %d, size: %lld, overhead: %lld"), use.count, use.total, use.overhead );
			TVPAddLog( buff );
			TJS_snprintf( buff, 128, TJS_W("  Uncommitted: %d, size: %lld, overhead: %lld"), uncommit.count, uncommit.total, uncommit.overhead );
			TVPAddLog( buff );
			TJS_snprintf( buff, 128, TJS_W("  Unused: %d, size: %lld, overhead: %lld"), unused.count, unused.total, unused.overhead );
			TVPAddLog( buff );
		}
	}
}
//---------------------------------------------------------------------------
struct tTVPGlobalHeapCompactCallback : public tTVPCompactEventCallbackIntf
{
	virtual void TJS_INTF_METHOD OnCompact(tjs_int level)
	{
		if(level >= TVP_COMPACT_LEVEL_IDLE)
		{	// Do compact CRT and Global Heap
			HANDLE hHeap = ::GetProcessHeap();
			if( hHeap ) {
				::HeapCompact( hHeap, 0 );
			}
			HANDLE hCrtHeap = (HANDLE)_get_heap_handle();
			if( hCrtHeap && hCrtHeap != hHeap ) {
				::HeapCompact( hCrtHeap, 0 );
			}
		}
	}
} static TVPGlobalHeapCompactCallback;
static bool TVPGlobalHeapCompactCallbackInit = false;
//---------------------------------------------------------------------------
void TVPAddGlobalHeapCompactCallback()
{
	// compact interface initialization
	if(!TVPGlobalHeapCompactCallbackInit)
	{
		TVPAddCompactEventHook(&TVPGlobalHeapCompactCallback);
		TVPGlobalHeapCompactCallbackInit = true;
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
TJS_BEGIN_NATIVE_METHOD_DECL(/*func. name*/shellExecute)
{
	if(numparams < 1) return TJS_E_BADPARAMCOUNT;

	ttstr target = *param[0];
	ttstr execparam;

	if(numparams >= 2) execparam = *param[1];

	bool res = TVPShellExecute(target, execparam);

	if(result) *result = (tjs_int)res;
	return TJS_S_OK;
}
TJS_END_NATIVE_STATIC_METHOD_DECL_OUTER(/*object to register*/cls,
	/*func. name*/shellExecute)
//----------------------------------------------------------------------
TJS_BEGIN_NATIVE_METHOD_DECL(/*func. name*/system)
{
	if(numparams < 1) return TJS_E_BADPARAMCOUNT;

	ttstr target = *param[0];

	int ret = _wsystem((const wchar_t*)target.c_str());

	TVPDeliverCompactEvent(TVP_COMPACT_LEVEL_MAX); // this should clear all caches

	if(result) *result = (tjs_int)ret;
	return TJS_S_OK;
}
TJS_END_NATIVE_STATIC_METHOD_DECL_OUTER(/*object to register*/cls,
	/*func. name*/system)
//----------------------------------------------------------------------
TJS_BEGIN_NATIVE_METHOD_DECL(/*func. name*/readRegValue)
{
	if(numparams < 1) return TJS_E_BADPARAMCOUNT;
	if(!result) return TJS_S_OK;

	ttstr key = *param[0];


	TVPReadRegValue(*result, key);

	return TJS_S_OK;
}
TJS_END_NATIVE_STATIC_METHOD_DECL_OUTER(/*object to register*/cls,
	/*func. name*/readRegValue)
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
TJS_BEGIN_NATIVE_METHOD_DECL(/*func. name*/createAppLock)
{
	if(numparams < 1) return TJS_E_BADPARAMCOUNT;
	if(!result) return TJS_S_OK;

	ttstr lockname = *param[0];

	bool res = TVPCreateAppLock(lockname);

	if(result) *result = (tjs_int)res;

	return TJS_S_OK;
}
TJS_END_NATIVE_STATIC_METHOD_DECL_OUTER(/*object to register*/cls,
	/*func. name*/createAppLock)
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
	// プラットフォーム固有 override もそのまま反映される。
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
// オーバレイ表示する。WINVER build では flag だけ立つが描画は行われない。
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
// オーバレイ表示する。WINVER build では flag だけ立つが描画は行われない。
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
// 利用統計を log に書き出す。WINVER build では memoverlay 自体が描画されない
// ため、log も実質出ない。フラグだけは立つ (TJS 側の互換性のため)。
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
	*(int *)0  = 0;

	return TJS_S_OK;
}
TJS_END_NATIVE_STATIC_METHOD_DECL_OUTER(/*object to register*/cls,
	/*func. name*/nullpo)
//----------------------------------------------------------------------
TJS_BEGIN_NATIVE_METHOD_DECL(/*func. name*/showVersion)
{
	TVPShowVersionForm();
	return TJS_S_OK;
}
TJS_END_NATIVE_STATIC_METHOD_DECL_OUTER(/*object to register*/cls,
	/*func. name*/showVersion)
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
TJS_BEGIN_NATIVE_PROP_DECL(personalPath)
{
	TJS_BEGIN_NATIVE_PROP_GETTER
	{
		*result = TVPGetPersonalPath();
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
		*result = TVPGetAppDataPath();
		return TJS_S_OK;
	}
	TJS_END_NATIVE_PROP_GETTER

	TJS_DENY_NATIVE_PROP_SETTER
}
TJS_END_NATIVE_STATIC_PROP_DECL_OUTER(cls, appDataPath)
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
		static ttstr exename(TVPNormalizeStorageName(ExePath()));
		*result = exename;
		return TJS_S_OK;
	}
	TJS_END_NATIVE_PROP_GETTER

	TJS_DENY_NATIVE_PROP_SETTER
}
TJS_END_NATIVE_STATIC_PROP_DECL_OUTER(cls, exeName)
//----------------------------------------------------------------------
TJS_BEGIN_NATIVE_PROP_DECL(savedGamesPath)
{
	TJS_BEGIN_NATIVE_PROP_GETTER
	{
		*result = TVPGetSavedGamesPath();
		return TJS_S_OK;
	}
	TJS_END_NATIVE_PROP_GETTER

	TJS_DENY_NATIVE_PROP_SETTER
}
TJS_END_NATIVE_STATIC_PROP_DECL_OUTER(cls, savedGamesPath)
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
		*result = tTVPScreen::GetWidth();
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
		*result = tTVPScreen::GetHeight();
		return TJS_S_OK;
	}
	TJS_END_NATIVE_PROP_GETTER

	TJS_DENY_NATIVE_PROP_SETTER
}
TJS_END_NATIVE_STATIC_PROP_DECL_OUTER(cls, screenHeight)
//----------------------------------------------------------------------
TJS_BEGIN_NATIVE_PROP_DECL(desktopLeft)
{
	TJS_BEGIN_NATIVE_PROP_GETTER
	{
		*result = tTVPScreen::GetDesktopLeft();
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
		*result = tTVPScreen::GetDesktopTop();
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
		*result = tTVPScreen::GetDesktopWidth();
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
		*result = tTVPScreen::GetDesktopHeight();
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
//----------------------------------------------------------------------
TJS_BEGIN_NATIVE_PROP_DECL(licenseText)
{
	TJS_BEGIN_NATIVE_PROP_GETTER
	{
		*result = TVPGetLicenseString();
		return TJS_S_OK;
	}
	TJS_END_NATIVE_PROP_GETTER

	TJS_DENY_NATIVE_PROP_SETTER
}
TJS_END_NATIVE_STATIC_PROP_DECL_OUTER(cls, licenseText)
//----------------------------------------------------------------------
TJS_BEGIN_NATIVE_METHOD_DECL(/*func. name*/getJoypadType)
{
	tjs_int no = numparams > 0 ? (tjs_int)*param[0] : 0;
	if (result) {
//		*result = Application->GetJoypadType(no);
	}
	return TJS_S_OK;
}
TJS_END_NATIVE_STATIC_METHOD_DECL_OUTER(/*object to register*/cls,
	/*func. name*/getJoypadType)

	return cls;

}
//---------------------------------------------------------------------------


