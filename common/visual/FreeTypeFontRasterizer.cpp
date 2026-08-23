
#define _USE_MATH_DEFINES
#include "FreeTypeFontRasterizer.h"
#include "LayerBitmapIntf.h"
#include "StorageIntf.h"   // TVPCheckExistentStorage (バンドル絵文字フォント登録)
#include "FreeType.h"
#include <math.h>
#include "MsgIntf.h"
#include "FontSystem.h"
#include "StringUtil.h"
#include <cmath>
#include <algorithm>
#ifdef __WINVER__
#include "TVPSysFont.h"
#endif

extern void TVPUninitializeFreeFont();
extern FontSystem* TVPFontSystem;

FreeTypeFontRasterizer::FreeTypeFontRasterizer() : RefCount(0), Face(NULL), LastBitmap(NULL), LastEmojiMode(-999) {
	AddRef();
}
FreeTypeFontRasterizer::~FreeTypeFontRasterizer() {
	if( Face ) delete Face;
	Face = NULL;
	TVPUninitializeFreeFont();
}
void FreeTypeFontRasterizer::AddRef() {
	RefCount++;
}
//---------------------------------------------------------------------------
void FreeTypeFontRasterizer::Release() {
	RefCount--;
	LastBitmap = NULL;
	if( RefCount == 0 ) {
		if( Face ) delete Face;
		Face = NULL;

		delete this;
	}
}
//---------------------------------------------------------------------------
void FreeTypeFontRasterizer::ApplyFont( class tTVPNativeBaseBitmap *bmp, bool force ) {
	if( bmp != LastBitmap || force ) {
		ApplyFont( bmp->GetFont() );
		LastBitmap = bmp;
	}
}
//---------------------------------------------------------------------------
// バンドル絵文字フォントを resource:// (埋め込み) から FreeType へ登録する。
// SDL/generic は resource/ が自動列挙されるので通常ここへ来ない (呼び出し側が
// FontExists で先に判定)。主に WINVER 用 (埋め込み BINARY を resource:// メディアで
// 開いて登録)。フォントが無ければ何もしない (mode 毎に一度だけ試行)。
static void TVPEnsureBundledEmojiFontRegistered( tjs_int mode ) {
	static bool triedMono = false, triedColor = false;
	const tjs_char* fname = nullptr;
	if( mode == TVP_EMOJI_MONO ) {
		if( triedMono ) return; triedMono = true;
		fname = TJS_W("notoemoji-regular.ttf");
	} else if( mode == TVP_EMOJI_COLOR ) {
		if( triedColor ) return; triedColor = true;
		fname = TJS_W("notocoloremoji.ttf");
	} else return;
	// リソースの置き場はプラットフォームで変わる (resource://./ / file://./resource/)
	// ため、直書きせず TVPGetResourcePath() を前置する。
	try {
		ttstr bundled = TVPGetResourcePath() + ttstr(fname);
		if( TVPIsExistentStorageNoSearch( bundled ) )
			TVPFontSystem->AddExtraFont( bundled.AsStdString(), nullptr );
	} catch(...) {}
}
//---------------------------------------------------------------------------
void FreeTypeFontRasterizer::ApplyFont( const tTVPFont& font ) {
	CurrentFont = font;
	std::vector<tjs_string> faces;
	tjs_string face = font.Face.AsStdString();
	// TODO 最初に@があった場合にすべて縦書きとして処理する処理は入っていない、縦書き対応するのなら必要。
	if( face[0] == TJS_W(',') ) {
		tjs_string stdname = TVPFontSystem->GetBeingFont(face);
		faces.push_back( stdname );
	} else {
		split( face, tjs_string(TJS_W(",")), faces );
		for( auto i = faces.begin(); i != faces.end(); ) {
			tjs_string& x = *i;
			x = Trim(x);
			// 未登録名でも data/fonts.json のメタデータ対象なら遅延ロードを試みる
			if( TVPFontSystem->FontExists( x ) == false ) {
				TVPFontSystem->EnsureLazyFontLoaded( x );
			}
			if( TVPFontSystem->FontExists( x ) == false ) {
				i = faces.erase( i );
			} else {
				i++;
			}
		}
		if( faces.empty() ) {
			faces.push_back( tjs_string(TVPFontSystem->GetDefaultFontName()) );
		}
	}

	// 絵文字モードに応じて、絵文字フォントをフォールバック連鎖の末尾に追加する。
	// 原フォントに無いコードポイント (絵文字) はここへフォールバックして描画される。
	// 指定 face が未登録 (リソース未収納等) の場合は追加せずスルー (原フォントのまま)。
	tjs_int emode = TVPResolveEmojiMode( font.EmojiMode );
	if( emode == TVP_EMOJI_MONO || emode == TVP_EMOJI_COLOR ) {
		// 指定モードの絵文字 face を試し、解決できなければ埋め込みの mono 絵文字へ
		// フォールバックする (カラー絵文字フォントは exe 未埋め込みのため、fonts.json
		// が無い単体起動等では解決できない。その場合でも豆腐にせず mono で表示する)。
		tjs_int tryModes[2] = { emode, TVP_EMOJI_MONO };
		int nTry = ( emode == TVP_EMOJI_COLOR ) ? 2 : 1;
		for( int t = 0; t < nTry; t++ ) {
			const tjs_char* ename = TVPGetEmojiFaceName( tryModes[t] );
			if( !ename || !ename[0] ) continue;
			// 未登録の場合の登録試行:
			//  1) data/fonts.json のメタデータ経由 (data/ 外だしのカラー絵文字等)
			//  2) 埋め込みリソース (resource://) 経由 (モノクロ絵文字等の同梱分)
			if( !TVPFontSystem->FontExists( tjs_string(ename) ) )
				TVPFontSystem->EnsureLazyFontLoaded( tjs_string(ename) );
			if( !TVPFontSystem->FontExists( tjs_string(ename) ) )
				TVPEnsureBundledEmojiFontRegistered( tryModes[t] );
			if( TVPFontSystem->FontExists( tjs_string(ename) ) ) {
				faces.push_back( tjs_string(ename) );
				break;
			}
		}
	}

	// TVP_FACE_OPTIONS_NO_ANTIALIASING
	// TVP_FACE_OPTIONS_NO_HINTING
	// TVP_FACE_OPTIONS_FORCE_AUTO_HINTING
	tjs_uint32 opt = 0;
	opt |= (font.Flags & TVP_TF_ITALIC) ? TVP_TF_ITALIC : 0;
	opt |= (font.Flags & TVP_TF_BOLD) ? TVP_TF_BOLD : 0;
	opt |= (font.Flags & TVP_TF_UNDERLINE) ? TVP_TF_UNDERLINE : 0;
	opt |= (font.Flags & TVP_TF_STRIKEOUT) ? TVP_TF_STRIKEOUT : 0;
	opt |= (font.Flags & TVP_TF_FONTFILE) ? TVP_FACE_OPTIONS_FILE : 0;
	// カラー絵文字モードでは連鎖全 face を FT_LOAD_COLOR で読む (BGRA グリフ取得用)。
	// emode 変更は下で必ず recreate されるので、この opt は face 再生成時に反映される。
	opt |= (emode == TVP_EMOJI_COLOR) ? TVP_FACE_OPTIONS_COLOR : 0;
	bool recreate = false;
	if( Face ) {
		// 先頭 face 変更 or 絵文字モード変更で連鎖を作り直す
		if( Face->GetFontName() != faces[0] || emode != LastEmojiMode ) {
			delete Face;
			Face = new tFreeTypeFace( faces, opt );
			recreate = true;
		}
	} else {
		Face = new tFreeTypeFace( faces, opt );
		recreate = true;
	}
	LastEmojiMode = emode;
	Face->SetHeight( font.Height < 0 ? -font.Height : font.Height );
	if( recreate == false ) {
		if( font.Flags & TVP_TF_ITALIC ) {
			Face->SetOption(TVP_TF_ITALIC);
		} else {
			Face->ClearOption(TVP_TF_ITALIC);
		}
		if( font.Flags & TVP_TF_BOLD ) {
			Face->SetOption(TVP_TF_BOLD);
		} else {
			Face->ClearOption(TVP_TF_BOLD);
		}
		if( font.Flags & TVP_TF_UNDERLINE ) {
			Face->SetOption(TVP_TF_UNDERLINE);
		} else {
			Face->ClearOption(TVP_TF_UNDERLINE);
		}
		if( font.Flags & TVP_TF_STRIKEOUT ) {
			Face->SetOption(TVP_TF_STRIKEOUT);
		} else {
			Face->ClearOption(TVP_TF_STRIKEOUT);
		}
	}
	LastBitmap = NULL;
}
//---------------------------------------------------------------------------
void FreeTypeFontRasterizer::GetTextExtent(tjs_uint32 ch, tjs_int &w, tjs_int &h) {
	if( Face ) {
		tGlyphMetrics metrics;
		if( Face->GetGlyphSizeFromCharcode( ch, metrics) ) {
			w = metrics.CellIncX;
			h = metrics.CellIncY;
		}
	}
}
//---------------------------------------------------------------------------
tjs_int FreeTypeFontRasterizer::GetAscentHeight() {
	if( Face ) return Face->GetAscent();
	return 0;
}
//---------------------------------------------------------------------------
tTVPCharacterData* FreeTypeFontRasterizer::GetBitmap( const tTVPFontAndCharacterData & font, tjs_int aofsx, tjs_int aofsy ) {
	// 文字単位の絵文字表示指定 (VS15/VS16) を反映する。
	// 既定連鎖 [原フォント..., 絵文字] は原フォント優先なので、★❤ 等 原フォントにも
	// 字形があるコードポイントを絵文字側で出すには、VS16 時に一時的に絵文字モードへ
	// 切替え + 末尾 face 優先にする。VS15 時は原フォント (テキスト) を強制する。
	tjs_int baseMode = TVPResolveEmojiMode( font.Font.EmojiMode );
	tjs_int effMode = baseMode;
	bool preferEmoji = false;
	if( font.EmojiPresentation == TVP_EMOJI_PRESENTATION_EMOJI ) {        // VS16
		effMode = ( baseMode == TVP_EMOJI_NONE ) ? TVP_EMOJI_COLOR : baseMode;
		preferEmoji = true;
	} else if( font.EmojiPresentation == TVP_EMOJI_PRESENTATION_TEXT ) {  // VS15
		effMode = TVP_EMOJI_NONE;
	}
	bool reapplied = false;
	if( effMode != LastEmojiMode ) {
		tTVPFont f = font.Font;
		f.EmojiMode = effMode;
		ApplyFont( f );
		reapplied = true;
	}
	if( Face ) Face->SetPreferLastFace( preferEmoji );

	if( font.Antialiased ) {
		Face->ClearOption( TVP_FACE_OPTIONS_NO_ANTIALIASING );
	} else {
		Face->SetOption( TVP_FACE_OPTIONS_NO_ANTIALIASING );
	}
	if( font.Hinting ) {
		Face->ClearOption( TVP_FACE_OPTIONS_NO_HINTING );
		//Face->SetOption( TVP_FACE_OPTIONS_FORCE_AUTO_HINTING );
	} else {
		Face->SetOption( TVP_FACE_OPTIONS_NO_HINTING );
		//Face->ClearOption( TVP_FACE_OPTIONS_FORCE_AUTO_HINTING );
	}
	tTVPCharacterData* data = Face->GetGlyphFromCharcode(font.Character);
	if( data == NULL ) {
		data = Face->GetGlyphFromCharcode( Face->GetDefaultChar() );
	}
	if( data == NULL ) {
		data = Face->GetGlyphFromCharcode( Face->GetFirstChar() );
	}
	if( data == NULL ) {
		TVPThrowExceptionMessage( TVPFontRasterizeError );
	}

	// VS15/VS16 用に切替えた状態を元へ戻す (data は複製済みなので影響なし)。
	if( Face ) Face->SetPreferLastFace( false );
	if( reapplied ) ApplyFont( font.Font );

	int cx = data->Metrics.CellIncX;
	int cy = data->Metrics.CellIncY;
	if( font.Font.Angle == 0 ) {
		data->Metrics.CellIncX = cx;
		data->Metrics.CellIncY = 0;
	} else if(font.Font.Angle == 2700) {
		data->Metrics.CellIncX = 0;
		data->Metrics.CellIncY = cx;
	} else {
		double angle = font.Font.Angle * (M_PI/1800);
		data->Metrics.CellIncX = static_cast<tjs_int>(  std::cos(angle) * cx);
		data->Metrics.CellIncY = static_cast<tjs_int>(- std::sin(angle) * cx);
	}

	data->Antialiased = font.Antialiased;
	// FullColored は GetGlyphFromCharcode がカラーグリフ(BGRA)に対して設定済み。
	// ここで一律 false にすると絵文字カラー表示が壊れるため上書きしない。
	data->Blured = font.Blured;
	data->BlurWidth = font.BlurWidth;
	data->BlurLevel = font.BlurLevel;

	// apply blur (Blur は FullColored 非対応なのでカラーグリフには適用しない)
	if(font.Blured && !data->FullColored) data->Blur(); // nasty ...
	return data;
}
//---------------------------------------------------------------------------
void FreeTypeFontRasterizer::GetGlyphDrawRect( const ttstr & text, tTVPRect& area ) {
	// アンチエイリアスとヒンティングは有効にする
	Face->ClearOption( TVP_FACE_OPTIONS_NO_ANTIALIASING );
	Face->ClearOption( TVP_FACE_OPTIONS_NO_HINTING );

	area.left = area.top = area.right = area.bottom = 0;
	tjs_int offsetx = 0;
	tjs_int offsety = 0;
	tjs_uint len = text.length();
	for( tjs_uint i = 0; i < len; i++ ) {
		// astral 面 (U+10000..) はサロゲートペアを 1 コードポイントへ結合する
		tjs_uint32 ch = (tjs_uint32)(tjs_uint16)text[i];
		if( ch >= 0xD800 && ch <= 0xDBFF && (i+1) < len ) {
			tjs_uint32 lo = (tjs_uint32)(tjs_uint16)text[i+1];
			if( lo >= 0xDC00 && lo <= 0xDFFF ) {
				ch = 0x10000u + ((ch - 0xD800u) << 10) + (lo - 0xDC00u);
				i++;
			}
		}
		tjs_int ax, ay;
		tTVPRect rt(0,0,0,0);
		bool result = Face->GetGlyphRectFromCharcode(rt,ch,ax,ay);
		if( result == false ) result = Face->GetGlyphRectFromCharcode(rt,Face->GetDefaultChar(),ax,ay);
		if( result == false ) result = Face->GetGlyphRectFromCharcode(rt,Face->GetFirstChar(),ax,ay);
		if( result ) {
			rt.add_offsets( offsetx, offsety );
			if( i != 0 ) {
				area.do_union( rt );
			} else {
				area = rt;
			}
		}
		offsetx += ax;
		offsety = 0;
	}
}
//---------------------------------------------------------------------------
extern bool TVPAddFontToFreeType( const ttstr& storage, std::vector<tjs_string>* faces );
bool FreeTypeFontRasterizer::AddFont( const ttstr& storage, std::vector<tjs_string>* faces ) {
	return TVPAddFontToFreeType( storage, faces );
}
//---------------------------------------------------------------------------
extern void TVPGetFontListFromFreeType(std::vector<ttstr> & list, tjs_uint32 flags, const tTVPFont & font );
void FreeTypeFontRasterizer::GetFontList(std::vector<ttstr> & list, tjs_uint32 flags, const struct tTVPFont & font ) {
#ifdef __WINVER__
	TVPGetFontList( list, flags, font );
#endif
	TVPGetFontListFromFreeType( list, flags, font );
}
//---------------------------------------------------------------------------

