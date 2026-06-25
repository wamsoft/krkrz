//---------------------------------------------------------------------------
// UserConfig オプション記述ローダ (Win32 / PE リソース版)
//
// JSON パーサ・マージは common/msg/ReadOptionDescUtil.cpp に共通化済み。
// このファイルは Win32 リソース機構経由で JSON バイト列を引き出す部分のみ。
//---------------------------------------------------------------------------
#include "tjsCommHead.h"
#include "MsgIntf.h"
#include "DebugIntf.h"
#include "resource.h"
#include "CharacterSet.h"
#include "ReadOptionDesc.h"
#include "WindowsUtil.h"

extern "C" {
static BOOL CALLBACK EnumResTypeProc( HMODULE hModule, LPTSTR lpszType, LONG_PTR lParam ) {
	if( !IS_INTRESOURCE(lpszType) ) {
		OutputDebugString( lpszType );
	}
	return TRUE;
}
static BOOL CALLBACK TVPEnumResNameProc( HMODULE hModule, LPCTSTR lpszType, LPTSTR lpszName, LONG_PTR lParam ) {
	if( !IS_INTRESOURCE(lpszName) ) {
		OutputDebugString( lpszName );
	}
	return TRUE;
}
};

tTVPCommandOptionList* TVPGetPluginCommandDesc( const tjs_char* name ) {
	HMODULE hModule = ::LoadLibraryEx( (const wchar_t*)name, NULL, LOAD_LIBRARY_AS_DATAFILE );
	if( hModule == NULL ) return NULL;
	const char *buf = NULL;
	unsigned int size = 0;
	tTVPCommandOptionList* ret = NULL;
	try {
		HRSRC hRsrc = ::FindResource(hModule, L"IDR_OPTION_DESC_JSON", L"TEXT" );
		if( hRsrc != NULL ) {
			size = ::SizeofResource( hModule, hRsrc );
			HGLOBAL hGlobal = ::LoadResource( hModule, hRsrc );
			if( hGlobal != NULL ) {
				buf = reinterpret_cast<const char*>(::LockResource(hGlobal));
			}
		}
		ret = TVPParseCommandDescJson( buf, size );
	} catch(...) {
		::FreeLibrary( hModule );
		throw;
	}
	::FreeLibrary( hModule );
	return ret;
}

tTVPCommandOptionList* TVPGetEngineCommandDesc() {
	HMODULE hModule = ::GetModuleHandle(NULL);
	if( hModule == NULL ) return NULL;
	const char *buf = NULL;
	unsigned int size = 0;
	HRSRC hRsrc = ::FindResource(hModule, MAKEINTRESOURCE(IDR_OPTION_DESC_JSON), TEXT("TEXT"));
	if( hRsrc != NULL ) {
		size = ::SizeofResource( hModule, hRsrc );
		HGLOBAL hGlobal = ::LoadResource( hModule, hRsrc );
		if( hGlobal != NULL ) {
			buf = reinterpret_cast<const char*>(::LockResource(hGlobal));
		}
	}
	return TVPParseCommandDescJson( buf, size );
}
