
#ifndef __FILE_PATH_UTIL_H__
#define __FILE_PATH_UTIL_H__

#include <string>
#include <stdlib.h>

#include <shlwapi.h>
#include <winnetwk.h>


inline tjs_string IncludeTrailingBackslash( const tjs_string& path ) {
	if( path[path.length()-1] != TJS_W('\\') ) {
		return tjs_string(path+TJS_W("\\"));
	}
	return tjs_string(path);
}
inline tjs_string ExcludeTrailingBackslash( const tjs_string& path ) {
	if( path[path.length()-1] == TJS_W('\\') ) {
		return tjs_string(path.c_str(),path.length()-1);
	}
	return tjs_string(path);
}
// 末尾の /\ は含まない
inline tjs_string ExtractFileDir( const tjs_string& path ) {
	wchar_t drive[_MAX_DRIVE];
	wchar_t dir[_MAX_DIR];
	_wsplitpath_s((const wchar_t*)path.c_str(), drive, _MAX_DRIVE, dir,_MAX_DIR, NULL, 0, NULL, 0 );
	tjs_string dirstr = tjs_string( (tjs_char*)dir );
	if( dirstr[dirstr.length()-1] != TJS_W('\\') ) {
		return tjs_string( (tjs_char*)drive ) + dirstr;
	} else {
		return tjs_string( (tjs_char*)drive ) + dirstr.substr(0,dirstr.length()-1);
	}
}
// 末尾の /\ を含む
inline tjs_string ExtractFilePath( const tjs_string& path ) {
	wchar_t drive[_MAX_DRIVE];
	wchar_t dir[_MAX_DIR];
	_wsplitpath_s((const wchar_t*)path.c_str(), drive, _MAX_DRIVE, dir,_MAX_DIR, NULL, 0, NULL, 0 );
	return tjs_string( (tjs_char*)drive ) + tjs_string( (tjs_char*)dir );
}

inline bool DirectoryExists( const tjs_string& path ) {
	return (0!=::PathIsDirectory((const wchar_t*)path.c_str()));
}

inline bool FileExists( const tjs_string& path ) {
	return ( (0!=::PathFileExists((const wchar_t*)path.c_str())) && (0==::PathIsDirectory((const wchar_t*)path.c_str())) );
	return false;
}

inline tjs_string ChangeFileExt( const tjs_string& path, const tjs_string& ext ) {
	wchar_t drive[_MAX_DRIVE];
	wchar_t dir[_MAX_DIR];
	wchar_t fname[_MAX_FNAME];
	_wsplitpath_s( (const wchar_t*)path.c_str(), drive, _MAX_DRIVE, dir, _MAX_DIR, fname, _MAX_FNAME, NULL, 0 );
	return tjs_string( (tjs_char*)drive ) + tjs_string( (tjs_char*)dir ) + tjs_string( (tjs_char*)fname ) + ext;
}
inline tjs_string ExtractFileName( const tjs_string& path ) {
	wchar_t fname[_MAX_FNAME];
	wchar_t ext[_MAX_EXT];
	_wsplitpath_s( (const wchar_t*)path.c_str(), NULL, 0, NULL, 0, fname, _MAX_FNAME, ext, _MAX_EXT );
	return tjs_string( (tjs_char*)fname ) + tjs_string( (tjs_char*)ext );
}
inline tjs_string ExtractFileExt( const tjs_string& path ) {
	wchar_t ext[_MAX_EXT];
	_wsplitpath_s( (const wchar_t*)path.c_str(), nullptr, 0, nullptr, 0, nullptr, 0, ext, _MAX_EXT );
	return tjs_string( (tjs_char*)ext );
}
inline tjs_string ExpandUNCFileName( const tjs_string& path ) {
	tjs_string result;
	DWORD InfoSize = 0;
	if( ERROR_MORE_DATA == WNetGetUniversalName( (const wchar_t*)path.c_str(), UNIVERSAL_NAME_INFO_LEVEL, NULL, &InfoSize) ) {
		UNIVERSAL_NAME_INFO* pInfo = reinterpret_cast<UNIVERSAL_NAME_INFO*>( ::GlobalAlloc(GMEM_FIXED, InfoSize) );
		DWORD ret = ::WNetGetUniversalName( (const wchar_t*)path.c_str(), UNIVERSAL_NAME_INFO_LEVEL, pInfo, &InfoSize);
		if( NO_ERROR == ret ) {
			result = tjs_string((const tjs_char*)pInfo->lpUniversalName);
		}
		::GlobalFree(pInfo);
	} else {
		// long-path 対応: 固定 _MAX_PATH バッファをやめ _wfullpath に必要長を確保させる
		// (第1引数 NULL + maxlen 0 で malloc された絶対パスが返る。呼び出し側で free)。
		wchar_t *fullpath = _wfullpath( NULL, (const wchar_t*)path.c_str(), 0 );
		if( fullpath ) { result = tjs_string( (const tjs_char*)fullpath ); free( fullpath ); }
		else { result = tjs_string( (const tjs_char*)path.c_str() ); }
	}
	return result;
}

#endif // __FILE_PATH_UTIL_H__
