#include "tjsCommHead.h"

#include "Application.h"

#include <MsgIntf.h>
#include "SysInitIntf.h"
#include "CharacterSet.h"
#include "StorageIntf.h"
#include "FontSystem.h"   // TVPFontSystem->FontAvailable (既定フォントの候補選び)

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
static bool TVPDefaultFontNameResolved = false;	// 候補のどれかに当たって決まったか
static bool SelectFont( const std::vector<tjs_string>& faces, tjs_string& face ) 
{
	std::vector<tjs_string> fonts;
	TVPGetAllFontList( fonts );
	for( auto i = faces.begin(); i != faces.end(); ++i ) {
		auto found = std::find( fonts.begin(), fonts.end(), *i );
		// fonts.json 宣言名 / 実行時登録名 / OS フォント (遅延登録) も候補に含める
		// (これらはファイルを開くまで TVPGetAllFontList に載らない)
		if( found != fonts.end() || ( TVPFontSystem && TVPFontSystem->FontAvailable( *i ) ) ) {
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
		TVPDefaultFontNameResolved = true;	// 明示指定は選び直さない
	} else {
		std::string lang( Application->getLanguage() );
		tjs_string face;
		if( lang == std::string("ja" ) ) {
			// 同梱 / fonts.json (Noto Sans JP) を優先し、無ければ OS の日本語フォント
			// (Windows / Linux / macOS / Android の代表的なもの) から選ぶ
			std::vector<tjs_string> facenames{
				tjs_string(TJS_W("Noto Sans JP Regular")),
				tjs_string(TJS_W("Yu Gothic UI")),tjs_string(TJS_W("Yu Gothic")),
				tjs_string(TJS_W("Meiryo UI")),tjs_string(TJS_W("Meiryo")),tjs_string(TJS_W("MS UI Gothic")),
				tjs_string(TJS_W("MS Gothic")),
				tjs_string(TJS_W("Hiragino Sans")),tjs_string(TJS_W("Hiragino Kaku Gothic ProN")),
				tjs_string(TJS_W("IPAexGothic")),tjs_string(TJS_W("IPAGothic")),
				tjs_string(TJS_W("VL Gothic")),tjs_string(TJS_W("TakaoGothic")),
				// ファミリ名だけの Noto は OS 版 (可変フォントで既定が Thin のことがある) に
				// 当たりやすいので、 OS の標準日本語フォントより後ろに置く
				tjs_string(TJS_W("Noto Sans CJK JP")),tjs_string(TJS_W("Noto Sans JP")),
				tjs_string(TJS_W("MotoyaLMaru W3 mono")),
				tjs_string(TJS_W("MotoyaLCedar W3 mono")),tjs_string(TJS_W("Droid Sans Japanese")),tjs_string(TJS_W("Droid Sans Mono Regular"))};
			if( SelectFont( facenames, face ) ) {
				TVPDefaultFontName.AssignMessage( face.c_str() );
				TVPDefaultFontNameResolved = true;
			}
		} else if( lang == std::string("zh" ) ) {
			std::vector<tjs_string> facenames{tjs_string(TJS_W("Noto Sans SC Regular")),tjs_string(TJS_W("Droid Sans Mono Regular"))};
			if( SelectFont( facenames, face ) ) {
				TVPDefaultFontName.AssignMessage( face.c_str() );
				TVPDefaultFontNameResolved = true;
			}
		} else if( lang == std::string("ko" ) ) {
			std::vector<tjs_string> facenames{tjs_string(TJS_W("Noto Sans KR Regular")),tjs_string(TJS_W("Droid Sans Mono Regular"))};
			if( SelectFont( facenames, face ) ) {
				TVPDefaultFontName.AssignMessage( face.c_str() );
				TVPDefaultFontNameResolved = true;
			}
		} else {
			std::vector<tjs_string> facenames{tjs_string(TJS_W("Droid Sans Mono Regular"))};
			if( SelectFont( facenames, face ) ) {
				TVPDefaultFontName.AssignMessage( face.c_str() );
				TVPDefaultFontNameResolved = true;
			}
		}
	}
	return TVPDefaultFontName;
}

void TVPSetDefaultFontName( const tjs_char * name ) 
{
	TVPDefaultFontName.AssignMessage( name );
	TVPDefaultFontNameResolved = true;
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

//---------------------------------------------------------------------------
// 既定フォントが候補に当たらずに決まっていた (= 存在しない名前のまま) なら、
// フォントの登録が増えた時点で選び直す。 変わったら true。
bool TVPRetryDefaultFontName()
{
	if( !IsInitDefalutFontName || TVPDefaultFontNameResolved ) return false;
	ttstr before( (const tjs_char*)TVPDefaultFontName );
	IsInitDefalutFontName = false;
	TVPGetDefaultFontName();
	if( ttstr( (const tjs_char*)TVPDefaultFontName ) == before ) return false;
	TVPDefaultFaceNames.Clear();	// 次の TVPGetDefaultFaceNames で作り直す
	return true;
}
