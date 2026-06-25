#include "tjsCommHead.h"

#include "Application.h"

#include <MsgIntf.h>
#include "SysInitIntf.h"
#include "CharacterSet.h"
#include "StorageIntf.h"

#include <ft2build.h>
#include FT_TRUETYPE_UNPATENTED_H
#include FT_SYNTHESIS_H
#include FT_BITMAP_H
extern FT_Library FreeTypeLibrary;
extern void TVPInitializeFont();

// /system/fonts/ フォントが置かれているフォルダから取得する(Nexus5で約50msかかる)
// フォントが最初に使われる時にFontSystem::InitFontNames経由で呼ばれる
extern void TVPAddSystemFontToFreeType( const tjs_string& storage, std::vector<tjs_string>* faces );
extern void TVPGetSystemFontListFromFreeType( std::vector<tjs_string>& faces );
static bool TVPIsGetAllFontList = false;

void TVPGetAllFontList( std::vector<tjs_string>& list ) 
{
	TVPInitializeFont();
	if( TVPIsGetAllFontList ) {
		TVPGetSystemFontListFromFreeType( list );
	} else {
		// システムフォントを読み込む
		std::vector<tjs_string> fontList;
		Application->GetSystemFontList(fontList);

		// リソース中のフォント一覧を吸い出す
		struct MyLister : iTVPStorageLister {
			std::vector<tjs_string> &list;
			ttstr base;
			MyLister(std::vector<tjs_string> &list, const ttstr &base) : list(list), base(base) {}
			void TJS_INTF_METHOD Add(const ttstr &name) {
				// フォント名を取得する
				ttstr ext = TVPExtractStorageExt(name);
				if( ext == TJS_W(".ttf") || ext == TJS_W(".otf") ) {
					tjs_string path = base.c_str();
					path += name.c_str();
					list.push_back(path);
				}
			}
		} lister(fontList, Application->ResourcePath());
		TVPGetStorageListAt(Application->ResourcePath(), &lister);
		for (auto it=fontList.begin();it != fontList.end(); it++) {
			TVPAddSystemFontToFreeType(*it, &list);
		}
		TVPIsGetAllFontList = true;
	}
}

static bool IsInitDefalutFontName = false;
static bool SelectFont( const std::vector<tjs_string>& faces, tjs_string& face ) 
{
	std::vector<tjs_string> fonts;
	TVPGetAllFontList( fonts );
	for( auto i = faces.begin(); i != faces.end(); ++i ) {
		auto found = std::find( fonts.begin(), fonts.end(), *i );
		if( found != fonts.end() ) {
			face = *i;
			return true;
		}
	}
	return false;
}

const tjs_char *TVPGetDefaultFontName() 
{
	if( IsInitDefalutFontName ) {
		return TVPDefaultFontName;
	}
	TVPDefaultFontName.AssignMessage(TJS_W("Droid Sans Mono Regular"));
	IsInitDefalutFontName =  true;

	// コマンドラインで指定がある場合、そのフォントを使用する
	tTJSVariant opt;
	if(TVPGetCommandLine(TJS_W("-deffont"), &opt)) {
		ttstr str(opt);
		TVPDefaultFontName.AssignMessage( str.c_str() );
	} else {
		std::string lang( Application->getLanguage() );
		tjs_string face;
		if( lang == std::string("ja" ) ) {
			std::vector<tjs_string> facenames{tjs_string(TJS_W("Noto Sans JP Regular")),tjs_string(TJS_W("Noto Sans CJK JP")),tjs_string(TJS_W("MotoyaLMaru W3 mono")),
				tjs_string(TJS_W("MotoyaLCedar W3 mono")),tjs_string(TJS_W("Droid Sans Japanese")),tjs_string(TJS_W("Droid Sans Mono Regular"))};
			if( SelectFont( facenames, face ) ) {
				TVPDefaultFontName.AssignMessage( face.c_str() );
			}
		} else if( lang == std::string("zh" ) ) {
			std::vector<tjs_string> facenames{tjs_string(TJS_W("Noto Sans SC Regular")),tjs_string(TJS_W("Droid Sans Mono Regular"))};
			if( SelectFont( facenames, face ) ) {
				TVPDefaultFontName.AssignMessage( face.c_str() );
			}
		} else if( lang == std::string("ko" ) ) {
			std::vector<tjs_string> facenames{tjs_string(TJS_W("Noto Sans KR Regular")),tjs_string(TJS_W("Droid Sans Mono Regular"))};
			if( SelectFont( facenames, face ) ) {
				TVPDefaultFontName.AssignMessage( face.c_str() );
			}
		} else {
			std::vector<tjs_string> facenames{tjs_string(TJS_W("Droid Sans Mono Regular"))};
			if( SelectFont( facenames, face ) ) {
				TVPDefaultFontName.AssignMessage( face.c_str() );
			}
		}
	}
	return TVPDefaultFontName;
}

void TVPSetDefaultFontName( const tjs_char * name ) 
{
	TVPDefaultFontName.AssignMessage( name );
}

static ttstr TVPDefaultFaceNames;

/**
 * Androidの場合、デフォルトフォントだと各地域固有の文字のみしか入っていないので、Roboto,Droid Sans Monoも候補として返す
 */
const ttstr &TVPGetDefaultFaceNames() 
{
	if( !TVPDefaultFaceNames.IsEmpty() ) {
		return TVPDefaultFaceNames;
	} else {
		TVPDefaultFaceNames = ttstr( TVPGetDefaultFontName() );
		std::string lang( Application->getLanguage() );
		if( false && lang == std::string("ja" ) ) {
			// TODO:存在確認などしてもうちょっと最適なフェイスリストを作る用にした方がいい
			TVPDefaultFaceNames += ttstr(TJS_W("Noto Sans,MotoyaLMaru,Roboto"));
		} else {
			TVPDefaultFaceNames += ttstr(TJS_W(",Roboto"));
		}
		return TVPDefaultFaceNames;
	}
}
