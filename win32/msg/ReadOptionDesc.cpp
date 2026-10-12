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
#include "MsgLanguage.h"
#include "WindowsUtil.h"
#include "Application.h"
#include "FilePathUtil.h"

#include <string>

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

	// 1) resource/ 由来の BINARY リソース (resources.rc が resource/* を
	//    ファイル名そのままの名前で埋め込む) から、言語 suffix 候補
	//    (-language= / OS 言語) を優先順に試す。
	//    例: en-US → "optiondesc-en.json" → "optiondesc.json"
	for( const std::string &sfx : TVPGetMessageResourceSuffixes() ) {
		std::wstring resName = L"optiondesc";
		resName.append( sfx.begin(), sfx.end() );
		resName += L".json";
		HRSRC hRsrc = ::FindResource( hModule, resName.c_str(), L"BINARY" );
		if( hRsrc == NULL ) continue;
		unsigned int size = ::SizeofResource( hModule, hRsrc );
		HGLOBAL hGlobal = ::LoadResource( hModule, hRsrc );
		if( hGlobal == NULL ) continue;
		const char *buf = reinterpret_cast<const char*>(::LockResource(hGlobal));
		tTVPCommandOptionList *ret = TVPParseCommandDescJson( buf, size );
		if( ret ) return ret;
	}

	// 2) 旧来の TEXT リソース (IDR_OPTION_DESC_JSON) へフォールバック。
	//    KRKRZ_RESOURCE_DIR を案件リソースへ差し替えて optiondesc*.json が
	//    無くなった場合でも、エンジン同梱の記述 (日本語) が使えるようにする。
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

//---------------------------------------------------------------------------
// 全プラグインの option desc を dest へマージする (設定画面 -userconf 用)。
// exe と同じフォルダ、plugin64\ (64bit) / plugin\ (32bit) の .dll / .tpm を見る
// (旧設定ダイアログと同じ範囲)。
//---------------------------------------------------------------------------
static void TVPMergePluginCommandDescIn( tTVPCommandOptionList& dest, const tjs_string& dir, const tjs_string& ext ) {
	tjs_string mask = dir + TJS_W("*.") + ext;
	WIN32_FIND_DATA fd;
	HANDLE hSearch = ::FindFirstFile( reinterpret_cast<LPCWSTR>(mask.c_str()), &fd );
	if( hSearch == INVALID_HANDLE_VALUE ) return;
	do {
		tTVPCommandOptionList* options = TVPGetPluginCommandDesc( (dir + tjs_string( reinterpret_cast<tjs_char*>(fd.cFileName) )).c_str() );
		if( options ) {
			TVPMargeCommandDesc( dest, *options );
			delete options;
		}
	} while( ::FindNextFile( hSearch, &fd ) );
	::FindClose( hSearch );
}
void TVPMergeAllPluginCommandDesc( tTVPCommandOptionList& dest ) {
	tjs_string exedir = IncludeTrailingBackslash( ExtractFileDir( ExePath() ) );
#ifdef TJS_64BIT_OS
	const tjs_string plugindir = exedir + TJS_W("plugin64\\");
#else
	const tjs_string plugindir = exedir + TJS_W("plugin\\");
#endif
	for( const tjs_string& dir : { exedir, plugindir } ) {
		TVPMergePluginCommandDescIn( dest, dir, TJS_W("dll") );
		TVPMergePluginCommandDescIn( dest, dir, TJS_W("tpm") );
	}
}
