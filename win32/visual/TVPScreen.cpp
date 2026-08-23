
#include "tjsCommHead.h"

#include "TVPScreen.h"
#include "Application.h"

#include "DebugIntf.h"
#include "MsgIntf.h"
#include "DisplaySelect.h"

#include <vector>
#include <algorithm>

//! -display= 指定ディスプレイの情報を得る (未指定/解決失敗なら false)。定義は後半
static bool TVPGetStartupMonitorInfo( MONITORINFO& mi );

//! 画面サイズ / デスクトップ領域の基準になるモニタ。
//! メインウィンドウのあるモニタ → -display= 指定 → プライマリ の順で解決する
//! (SDL3 の SDL3Application::BaseDisplayID() と同じ規則)
static bool TVPGetBaseMonitorInfo( MONITORINFO& mi ) {
	HWND hWnd = Application->GetMainWindowHandle();
	if( hWnd != NULL && hWnd != (HWND)INVALID_HANDLE_VALUE ) {
		HMONITOR hMonitor = ::MonitorFromWindow( hWnd, MONITOR_DEFAULTTOPRIMARY );
		if( hMonitor != NULL ) {
			mi.cbSize = sizeof(MONITORINFO);
			if( ::GetMonitorInfo( hMonitor, &mi ) != FALSE ) return true;
		}
	}
	// メインウィンドウがまだ無い間は、-display= 指定があればそのディスプレイ
	if( TVPGetStartupMonitorInfo( mi ) ) return true;

	HMONITOR hMonitor = ::MonitorFromWindow( NULL, MONITOR_DEFAULTTOPRIMARY );
	if( hMonitor != NULL ) {
		mi.cbSize = sizeof(MONITORINFO);
		if( ::GetMonitorInfo( hMonitor, &mi ) != FALSE ) return true;
	}
	return false;
}

int tTVPScreen::GetWidth() {
	MONITORINFO monitorinfo = {sizeof(MONITORINFO)};
	if( TVPGetBaseMonitorInfo( monitorinfo ) ) {
		return monitorinfo.rcMonitor.right - monitorinfo.rcMonitor.left;
	}
	return ::GetSystemMetrics(SM_CXSCREEN);
}
int tTVPScreen::GetHeight() {
	MONITORINFO monitorinfo = {sizeof(MONITORINFO)};
	if( TVPGetBaseMonitorInfo( monitorinfo ) ) {
		return monitorinfo.rcMonitor.bottom - monitorinfo.rcMonitor.top;
	}
	return ::GetSystemMetrics(SM_CYSCREEN);
}

void tTVPScreen::GetDesktopRect( RECT& r ) {
	MONITORINFO monitorinfo = {sizeof(MONITORINFO)};
	if( TVPGetBaseMonitorInfo( monitorinfo ) ) {
		r = monitorinfo.rcWork;
		return;
	}
	::SystemParametersInfo( SPI_GETWORKAREA, 0, &r, 0 );
}
int tTVPScreen::GetDesktopLeft() {
	RECT r;
	GetDesktopRect(r);
	return r.left;
}
int tTVPScreen::GetDesktopTop() {
	RECT r;
	GetDesktopRect(r);
	return r.top;
}
int tTVPScreen::GetDesktopWidth() {
	RECT r;
	GetDesktopRect(r);
	return r.right - r.left;
}
int tTVPScreen::GetDesktopHeight() {
	RECT r;
	GetDesktopRect(r);
	return r.bottom - r.top;
}
// Dump Video card infomation
void TVPDumpDisplayDevices() {
	DISPLAY_DEVICE	displayDevice;
	ZeroMemory( &displayDevice, sizeof(DISPLAY_DEVICE) );
	displayDevice.cb = sizeof(DISPLAY_DEVICE);
	DWORD iDevNum = 0;
	while( EnumDisplayDevices( nullptr, iDevNum, &displayDevice, 0) ) {
		ttstr gpuinfo( TVPFormatMessage(TJS_W("(info) Display Device #%1 : "), ttstr((tjs_int)iDevNum)) );
		gpuinfo += ttstr(TJS_W("Name : ")) + ttstr(displayDevice.DeviceName);
		gpuinfo += ttstr(TJS_W("  Description : ")) + ttstr(displayDevice.DeviceString);
		gpuinfo += ttstr(TJS_W("  ACTIVE")) + (( displayDevice.StateFlags & DISPLAY_DEVICE_ACTIVE ) ? TJS_W(":yes") : TJS_W(":no"));
		gpuinfo += ttstr(TJS_W("  MIRRORING")) + (( displayDevice.StateFlags & DISPLAY_DEVICE_MIRRORING_DRIVER ) ? TJS_W(":yes") : TJS_W(":no"));
		gpuinfo += ttstr(TJS_W("  MODESPRUNED")) + (( displayDevice.StateFlags & DISPLAY_DEVICE_MODESPRUNED ) ? TJS_W(":yes") : TJS_W(":no"));
		gpuinfo += ttstr(TJS_W("  PRIMARY")) + (( displayDevice.StateFlags & DISPLAY_DEVICE_PRIMARY_DEVICE ) ? TJS_W(":yes") : TJS_W(":no"));
		gpuinfo += ttstr(TJS_W("  REMOVABLE")) + (( displayDevice.StateFlags & DISPLAY_DEVICE_REMOVABLE ) ? TJS_W(":yes") : TJS_W(":no"));
		gpuinfo += ttstr(TJS_W("  VGA COMPATIBLE")) + (( displayDevice.StateFlags & DISPLAY_DEVICE_VGA_COMPATIBLE ) ? TJS_W(":yes") : TJS_W(":no"));
		TVPAddImportantLog(gpuinfo);

		ZeroMemory( &displayDevice, sizeof(DISPLAY_DEVICE) );
		displayDevice.cb = sizeof(DISPLAY_DEVICE);
		iDevNum++;
	}
}

//---------------------------------------------------------------------------
// -display= (起動するディスプレイの指定) 実装
//---------------------------------------------------------------------------
namespace {
//! 列挙結果 (共通形 + HMONITOR / 作業領域)
struct tTVPWinDisplay {
	tTVPDisplayEntry	entry;
	HMONITOR			monitor;
	RECT				rcWork;
};

//! "\\.\DISPLAY3" の末尾数値を得る (取れなければ 0)
tjs_int GetGDIDeviceNumber( const wchar_t* device ) {
	const wchar_t* p = device;
	const wchar_t* last_digits = nullptr;
	while( *p ) {
		if( *p >= L'0' && *p <= L'9' ) {
			if( !last_digits ) last_digits = p;
		} else {
			last_digits = nullptr;
		}
		p++;
	}
	if( !last_digits ) return 0;
	return (tjs_int)::_wtoi( last_digits );
}

//! QueryDisplayConfig でモニタのフレンドリ名を得る (取れなければ空)
tjs_string GetMonitorFriendlyName( const wchar_t* gdi_device_name ) {
	UINT32 path_count = 0, mode_count = 0;
	if( ::GetDisplayConfigBufferSizes( QDC_ONLY_ACTIVE_PATHS, &path_count, &mode_count ) != ERROR_SUCCESS )
		return tjs_string();
	std::vector<DISPLAYCONFIG_PATH_INFO> paths( path_count );
	std::vector<DISPLAYCONFIG_MODE_INFO> modes( mode_count );
	if( ::QueryDisplayConfig( QDC_ONLY_ACTIVE_PATHS, &path_count, paths.data(), &mode_count, modes.data(), nullptr ) != ERROR_SUCCESS )
		return tjs_string();
	for( UINT32 i = 0; i < path_count; i++ ) {
		DISPLAYCONFIG_SOURCE_DEVICE_NAME src = {};
		src.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_SOURCE_NAME;
		src.header.size = sizeof(src);
		src.header.adapterId = paths[i].sourceInfo.adapterId;
		src.header.id = paths[i].sourceInfo.id;
		if( ::DisplayConfigGetDeviceInfo( &src.header ) != ERROR_SUCCESS ) continue;
		if( ::wcscmp( src.viewGdiDeviceName, gdi_device_name ) != 0 ) continue;

		DISPLAYCONFIG_TARGET_DEVICE_NAME tgt = {};
		tgt.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_TARGET_NAME;
		tgt.header.size = sizeof(tgt);
		tgt.header.adapterId = paths[i].targetInfo.adapterId;
		tgt.header.id = paths[i].targetInfo.id;
		if( ::DisplayConfigGetDeviceInfo( &tgt.header ) == ERROR_SUCCESS )
			return tjs_string( tgt.monitorFriendlyDeviceName );
	}
	return tjs_string();
}

BOOL CALLBACK TVPEnumMonitorProc( HMONITOR monitor, HDC, LPRECT, LPARAM param ) {
	std::vector<tTVPWinDisplay>* list = reinterpret_cast<std::vector<tTVPWinDisplay>*>(param);
	MONITORINFOEXW mi = {};
	mi.cbSize = sizeof(MONITORINFOEXW);
	if( ::GetMonitorInfoW( monitor, &mi ) == FALSE ) return TRUE;

	tTVPWinDisplay d;
	d.monitor = monitor;
	d.rcWork = mi.rcWork;
	d.entry.index = GetGDIDeviceNumber( mi.szDevice ); // "\\.\DISPLAYn" の n
	d.entry.device = mi.szDevice;
	d.entry.name = GetMonitorFriendlyName( mi.szDevice );
	if( d.entry.name.empty() ) {
		// フレンドリ名が取れない環境ではモニタドライバ名で代用する
		DISPLAY_DEVICEW dd = {};
		dd.cb = sizeof(dd);
		if( ::EnumDisplayDevicesW( mi.szDevice, 0, &dd, 0 ) )
			d.entry.name = dd.DeviceString;
	}
	d.entry.left = mi.rcMonitor.left;
	d.entry.top = mi.rcMonitor.top;
	d.entry.width = mi.rcMonitor.right - mi.rcMonitor.left;
	d.entry.height = mi.rcMonitor.bottom - mi.rcMonitor.top;
	d.entry.primary = ( mi.dwFlags & MONITORINFOF_PRIMARY ) != 0;
	list->push_back( d );
	return TRUE;
}

//! ディスプレイ一覧を列挙する (番号は "\\.\DISPLAYn" の n 順)
const std::vector<tTVPWinDisplay>& GetDisplayList() {
	static std::vector<tTVPWinDisplay> list;
	static bool enumerated = false;
	if( !enumerated ) {
		enumerated = true;
		::EnumDisplayMonitors( NULL, NULL, TVPEnumMonitorProc, (LPARAM)&list );
		std::sort( list.begin(), list.end(), []( const tTVPWinDisplay& a, const tTVPWinDisplay& b ) {
			return a.entry.index < b.entry.index; } );
		// 番号が取れなかったものがあれば 1 origin の連番へ振り直す
		bool broken = false;
		for( tjs_uint i = 0; i < (tjs_uint)list.size(); i++ )
			if( list[i].entry.index <= 0 ) broken = true;
		if( broken ) {
			for( tjs_uint i = 0; i < (tjs_uint)list.size(); i++ )
				list[i].entry.index = (tjs_int)i + 1;
		}
	}
	return list;
}

//! -display= で選ばれたディスプレイ (未指定/解決失敗なら nullptr)
const tTVPWinDisplay* GetStartupDisplay() {
	static const tTVPWinDisplay* selected = nullptr;
	static bool resolved = false;
	if( resolved ) return selected;
	resolved = true;

	tjs_string opt;
	if( !TVPGetStartupDisplayOption( opt ) ) return nullptr;

	const std::vector<tTVPWinDisplay>& list = GetDisplayList();
	std::vector<tTVPDisplayEntry> entries;
	entries.reserve( list.size() );
	for( tjs_uint i = 0; i < (tjs_uint)list.size(); i++ ) entries.push_back( list[i].entry );

	if( TVPIsDisplayListRequest( opt ) ) {
		TVPLogDisplayList( entries );
		return nullptr;
	}
	tjs_int idx = TVPMatchDisplay( opt, entries );
	if( idx < 0 ) {
		TVPAddImportantLog( ttstr(TJS_W("(warning) -display=")) + ttstr(opt) +
			ttstr(TJS_W(" : no such display; using the default one")) );
		TVPLogDisplayList( entries );
		return nullptr;
	}
	selected = &list[idx];
	TVPAddImportantLog( ttstr(TJS_W("(info) -display=")) + ttstr(opt) + ttstr(TJS_W(" -> display ")) +
		ttstr((tjs_int)selected->entry.index) + ttstr(TJS_W(" : ")) +
		ttstr( selected->entry.name.empty() ? selected->entry.device : selected->entry.name ) );
	return selected;
}
} // anonymous namespace
//---------------------------------------------------------------------------
// ファイル前半の TVPGetBaseMonitorInfo から使う (前方宣言済み)
static bool TVPGetStartupMonitorInfo( MONITORINFO& mi ) {
	const tTVPWinDisplay* d = GetStartupDisplay();
	if( !d ) return false;
	mi.cbSize = sizeof(MONITORINFO);
	return ::GetMonitorInfo( d->monitor, &mi ) != FALSE;
}
//---------------------------------------------------------------------------
void TVPMoveWindowToStartupDisplay( HWND hWnd ) {
	if( hWnd == NULL ) return;
	const tTVPWinDisplay* target = GetStartupDisplay();
	if( !target ) return;

	RECT rc;
	if( ::GetWindowRect( hWnd, &rc ) == FALSE ) return;
	int w = rc.right - rc.left;
	int h = rc.bottom - rc.top;

	int l = rc.left;
	int t = rc.top;
	HMONITOR current = ::MonitorFromWindow( hWnd, MONITOR_DEFAULTTONEAREST );
	if( current != target->monitor ) {
		// 別ディスプレイ上にいる場合は、現ディスプレイ作業領域の原点からの
		// 相対位置を保って移動する
		MONITORINFO cur = { sizeof(MONITORINFO) };
		if( ::GetMonitorInfo( current, &cur ) == FALSE ) return;
		l = rc.left - cur.rcWork.left + target->rcWork.left;
		t = rc.top  - cur.rcWork.top  + target->rcWork.top;
	}
	// 既に目的のディスプレイ上にいる場合でも、はみ出しは作業領域内へ寄せる
	// (配置後にスクリプトがウィンドウを大きくした場合など)
	if( l + w > target->rcWork.right )  l = target->rcWork.right - w;
	if( t + h > target->rcWork.bottom ) t = target->rcWork.bottom - h;
	if( l < target->rcWork.left ) l = target->rcWork.left;
	if( t < target->rcWork.top )  t = target->rcWork.top;

	::SetWindowPos( hWnd, NULL, l, t, 0, 0, SWP_NOSIZE|SWP_NOZORDER|SWP_NOACTIVATE );
}
//---------------------------------------------------------------------------
