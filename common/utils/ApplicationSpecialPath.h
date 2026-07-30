
#ifndef __APPLICATION_SPECIAL_PATH_H__
#define __APPLICATION_SPECIAL_PATH_H__

#include <shlobj.h>
#include "FilePathUtil.h"

class ApplicationSpecialPath {
public:
	// 旧実装は非推奨の SHGetSpecialFolderPath + CSIDL_* を使っていたが、
	// Vista 以降の推奨 API SHGetKnownFolderPath + FOLDERID_* に置換。
	static tjs_string GetKnownFolderPath(REFKNOWNFOLDERID rfid) {
		tjs_string result;
		PWSTR ppszPath = NULL;
		if( SUCCEEDED( ::SHGetKnownFolderPath(rfid, 0, NULL, &ppszPath) ) && ppszPath ) {
			result = tjs_string( (const tjs_char*)ppszPath );
		}
		if( ppszPath ) ::CoTaskMemFree( ppszPath );
		return result;
	}
	static inline tjs_string GetPersonalPath() {
		tjs_string path = GetKnownFolderPath(FOLDERID_Documents);
		if( path.empty() ) path = GetKnownFolderPath(FOLDERID_RoamingAppData);

		if(path != TJS_W("")) {
			return path;
		}
		return TJS_W("");
	}
	static inline tjs_string GetAppDataPath() {
		tjs_string path = GetKnownFolderPath(FOLDERID_RoamingAppData);
		if(path != TJS_W("") ) {
			return path;
		}
		return TJS_W("");
	}
	static inline tjs_string GetSavedGamesPath() {
		tjs_string result;
		PWSTR ppszPath = NULL;
		HRESULT hr = ::SHGetKnownFolderPath(FOLDERID_SavedGames, 0, NULL, &ppszPath);
		if( hr == S_OK ) {
			result = tjs_string( (tjs_char*)ppszPath );
			::CoTaskMemFree( ppszPath );
		}
		return result;
	}
	static inline tjs_string ReplaceStringAll( tjs_string src, const tjs_string& target, const tjs_string& dest ) {
		tjs_string::size_type nPos = 0;
		while( (nPos = src.find(target, nPos)) != tjs_string::npos ) {
			src.replace( nPos, target.length(), dest );
		}
		return src;
	}

	static inline tjs_string GetConfigFileName( const tjs_string& exename ) {
		return ChangeFileExt(exename, TJS_W(".cf"));
	}
	static tjs_string GetDataPathDirectory( tjs_string datapath, const tjs_string& exename ) {
		if(datapath == TJS_W("") ) datapath = tjs_string(TJS_W("$(exepath)\\savedata"));

		tjs_string exepath = ExcludeTrailingBackslash(ExtractFileDir(exename));
		tjs_string personalpath = ExcludeTrailingBackslash(GetPersonalPath());
		tjs_string appdatapath = ExcludeTrailingBackslash(GetAppDataPath());
		tjs_string savedgamespath = ExcludeTrailingBackslash(GetSavedGamesPath());
		if(personalpath == TJS_W("")) personalpath = exepath;
		if(appdatapath == TJS_W("")) appdatapath = exepath;
		if(savedgamespath == TJS_W("")) savedgamespath = exepath;

		datapath = ReplaceStringAll(datapath, TJS_W("$(exepath)"), exepath);
		datapath = ReplaceStringAll(datapath, TJS_W("$(personalpath)"), personalpath);
		datapath = ReplaceStringAll(datapath, TJS_W("$(appdatapath)"), appdatapath);
		datapath = ReplaceStringAll(datapath, TJS_W("$(vistapath)"), appdatapath );
		datapath = ReplaceStringAll(datapath, TJS_W("$(savedgamespath)"), savedgamespath);
		return IncludeTrailingBackslash(ExpandUNCFileName(datapath));
	}
	static tjs_string GetUserConfigFileName( const tjs_string& datapath, const tjs_string& exename ) {
		// exepath, personalpath, appdatapath
		return GetDataPathDirectory(datapath, exename) + ExtractFileName(ChangeFileExt(exename, TJS_W(".cfu")));
	}
};


#endif // __APPLICATION_SPECIAL_PATH_H__
