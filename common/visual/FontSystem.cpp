

#include "tjsCommHead.h"

#include "FontSystem.h"
#include "StringUtil.h"
#include "MsgIntf.h"
#include <vector>
#include "FontRasterizer.h"
#include "StorageIntf.h"   // TVPCreateStream / TVPIsExistentStorage
#include "tjs.h"           // iTJSBinaryStream / TVPReadBuffer
#include "FontVariations.h" // TVPNormalizeFontVariations (fonts.json の axes 宣言)
#include <cstdio>           // snprintf
// PICOJSON_USE_INT64 は CMakeLists.txt でビルド全体に定義される
#include "picojson/picojson.h"


extern void TVPGetAllFontList( std::vector<tjs_string>& list );
extern const tjs_char *TVPGetDefaultFontName();
extern void TVPSetDefaultFontName( const tjs_char * name );
extern const ttstr &TVPGetDefaultFaceNames();

void FontSystem::InitFontNames() {
	// enumlate all fonts
	if(FontNamesInit) return;

	std::vector<tjs_string> list;
	TVPGetAllFontList( list );
	size_t count = list.size();
	for( size_t i = 0; i < count; i++ ) {
		AddFont( list[i] );
	}

	FontNamesInit = true;

	// data/fonts.json のメタデータを読み、遅延ロード対象を登録する
	LoadFontMetadata();
}
//---------------------------------------------------------------------------
void FontSystem::LoadFontMetadata() {
	if( FontMetadataLoaded ) return;
	FontMetadataLoaded = true;

	ttstr metaname( TJS_W("fonts.json") );
	if( !TVPIsExistentStorage( metaname ) ) return;

	std::string text;
	try {
		iTJSBinaryStream* st = TVPCreateStream( metaname, TJS_BS_READ );
		if( !st ) return;
		tjs_uint64 sz = st->GetSize();
		if( sz > 0 && sz < 16u*1024u*1024u ) {
			text.resize( (size_t)sz );
			TVPReadBuffer( st, &text[0], (tjs_uint)sz );
		}
		delete st;
	} catch( ... ) { return; }
	if( text.empty() ) return;

	picojson::value root;
	std::string err = picojson::parse( root, text );
	if( !err.empty() || !root.is<picojson::object>() ) return;
	const picojson::value& fonts = root.get("fonts");
	if( !fonts.is<picojson::array>() ) return;

	// family / aliases を name->storage の遅延テーブルへ登録する
	// (ここでは FreeType パースはせず、初回使用時に EnsureLazyFontLoaded で読む)
	for( const auto& fv : fonts.get<picojson::array>() ) {
		if( !fv.is<picojson::object>() ) continue;
		const picojson::value& file = fv.get("file");
		const picojson::value& fam  = fv.get("family");
		if( !file.is<std::string>() || !fam.is<std::string>() ) continue;
		// family 名は当面 ASCII 前提 (非 ASCII は将来 UTF-8 変換を要検討)
		tjs_string storage = ttstr( file.get<std::string>().c_str() ).AsStdString();
		tjs_string famName = ttstr( fam.get<std::string>().c_str() ).AsStdString();

		// 可変軸宣言 (任意): "axes": {"wght": 600, ...} 直書きと
		// "instance": "SemiBold" (fvar named instance 名)。両方あれば適用時に
		// axes が instance を上書きする。宣言名 (family/aliases) 単位で保持し、
		// 同じファイルを別名 + 別軸で複数宣言できる (例: 通常と SemiBold)。
		tjs_string axesSpec, instName;
		{
			const picojson::value& inst = fv.get("instance");
			if( inst.is<std::string>() )
				instName = ttstr( inst.get<std::string>().c_str() ).AsStdString();
			const picojson::value& axes = fv.get("axes");
			if( axes.is<picojson::object>() ) {
				std::string spec;
				for( const auto& kv : axes.get<picojson::object>() ) {
					if( !kv.second.is<double>() ) continue;
					if( !spec.empty() ) spec += ",";
					char buf[64];
					std::snprintf( buf, sizeof(buf), "%s=%g",
					               kv.first.c_str(), kv.second.get<double>() );
					spec += buf;
				}
				if( !spec.empty() ) {
					try {
						// 正規化 (小文字化/昇順/量子化)。不正タグ入りは宣言ごと無視
						axesSpec = TVPNormalizeFontVariations( ttstr(spec.c_str()) ).AsStdString();
					} catch( ... ) { axesSpec.clear(); }
				}
			}
		}
		const bool hasVar = !axesSpec.empty() || !instName.empty();

		LazyFontFiles[ famName ] = storage;
		LazyFontStorageAll[ famName ] = storage;   // 永続 (erase されない)
		if( hasVar ) LazyFontVariations[ famName ] = { axesSpec, instName };
		const picojson::value& al = fv.get("aliases");
		if( al.is<picojson::array>() ) {
			for( const auto& a : al.get<picojson::array>() )
				if( a.is<std::string>() ) {
					tjs_string aliasName = ttstr( a.get<std::string>().c_str() ).AsStdString();
					LazyFontFiles[ aliasName ] = storage;
					LazyFontStorageAll[ aliasName ] = storage;
					if( hasVar ) LazyFontVariations[ aliasName ] = { axesSpec, instName };
				}
		}
	}
}
//---------------------------------------------------------------------------
bool FontSystem::EnsureLazyFontLoaded( const tjs_string &name ) {
	InitFontNames();
	auto it = LazyFontFiles.find( name );
	if( it == LazyFontFiles.end() ) return false;
	tjs_string storage = it->second;
	LazyFontFiles.erase( it );  // 一度だけ試行
	try {
		AddExtraFont( storage, nullptr );  // 実 family 名で TVPFontNames へ登録される
	} catch( ... ) { return false; }
	return FontExists( name );
}
//---------------------------------------------------------------------------
bool FontSystem::GetLazyFontStorage( const tjs_string& name, tjs_string& storage ) const {
	// 永続マップ (LazyFontStorageAll) を参照する。EnsureLazyFontLoaded が
	// LazyFontFiles から erase 済みでも name->storage は引ける (FreeType が先に
	// 遅延ロードしたフォントを glyphware も同じストレージで開けるようにするため)。
	auto it = LazyFontStorageAll.find( name );
	if( it == LazyFontStorageAll.end() ) return false;
	storage = it->second;
	return true;
}
//---------------------------------------------------------------------------
bool FontSystem::GetLazyFontVariations( const tjs_string& name, tjs_string& axesSpec,
                                        tjs_string& instanceName ) const {
	auto it = LazyFontVariations.find( name );
	if( it == LazyFontVariations.end() ) return false;
	axesSpec = it->second.first;
	instanceName = it->second.second;
	return true;
}
//---------------------------------------------------------------------------
void FontSystem::RegisterLazyFont( const tjs_string& name, const tjs_string& storage ) {
	LazyFontFiles[ name ] = storage;
	LazyFontStorageAll[ name ] = storage;
}
//---------------------------------------------------------------------------
void FontSystem::EnumerateLazyFontStorages( std::vector<std::pair<tjs_string, tjs_string>>& out ) const {
	out.reserve( out.size() + LazyFontStorageAll.size() );
	for( const auto& kv : LazyFontStorageAll ) out.emplace_back( kv.first, kv.second );
}
//---------------------------------------------------------------------------
void FontSystem::AddFont( const tjs_string& name ) {
	TVPFontNames.Add( name, 1 );
}
//---------------------------------------------------------------------------
bool FontSystem::FontExists( const tjs_string &name ) {
	// check existence of font
	InitFontNames();

	int * t = TVPFontNames.Find(name);
	return t != NULL;
}

FontSystem::FontSystem() : FontNamesInit(false), DefaultLOGFONTCreated(false) {
	ConstructDefaultFont();
}

void FontSystem::ConstructDefaultFont() {
	if( !DefaultLOGFONTCreated ) {
		DefaultLOGFONTCreated = true;
		DefaultFont.Height = -12;
		DefaultFont.Flags = 0;
		DefaultFont.Angle = 0;
		DefaultFont.Face = TVPGetDefaultFaceNames();
	}
}

tjs_string FontSystem::GetBeingFont(tjs_string fonts) {
	// retrieve being font in the system.
	// font candidates are given by "fonts", separated by comma.

	bool vfont;

	if(fonts.c_str()[0] == TJS_W('@')) {     // for vertical writing
		fonts = fonts.c_str() + 1;
		vfont = true;
	} else {
		vfont = false;
	}

	bool prev_empty_name = false;
	while(fonts!=TJS_W("")) {
		tjs_string fontname;
		tjs_string::size_type pos = fonts.find_first_of(TJS_W(","));
		if( pos != std::string::npos ) {
			fontname = Trim( fonts.substr( 0, pos) );
			fonts = fonts.c_str()+pos+1;
		} else {
			fontname = Trim(fonts);
			fonts=TJS_W("");
		}

		// no existing check if previously specified font candidate is empty
		// eg. ",Fontname"

		if(fontname != TJS_W("") && (prev_empty_name || FontExists(fontname) ) ) {
			if(vfont && fontname.c_str()[0] != TJS_W('@')) {
				return  TJS_W("@") + fontname;
			} else {
				return fontname;
			}
		}

		prev_empty_name = (fontname == TJS_W(""));
	}

	if(vfont) {
		return tjs_string(TJS_W("@")) + tjs_string(TVPGetDefaultFontName());
	} else {
		return tjs_string(TVPGetDefaultFontName());
	}
}
//---------------------------------------------------------------------------
void FontSystem::AddExtraFont( const tjs_string& storage, std::vector<ttstr>* faces ) {
	std::vector<tjs_string> loadface;
	if( GetCurrentRasterizer()->AddFont( storage, &loadface ) ) {
		for( auto i = loadface.begin(); i != loadface.end(); ++i ) {
			AddFont( *i );
			// 実行時登録 (Font.addFont) / bundled 登録したフォントの name->storage を
			// 永続マップへ記録し、glyphware など別経路が名前で解決できるようにする。
			LazyFontStorageAll[ *i ] = storage;
		}
	}
	if( faces ) {
		for( auto i = loadface.begin(); i != loadface.end(); ++i ) {
			faces->push_back( ttstr( *i ) );
		}
	}
}
//---------------------------------------------------------------------------
const tjs_char* FontSystem::GetDefaultFontName() const {
	return TVPGetDefaultFontName();
}
//---------------------------------------------------------------------------
void FontSystem::SetDefaultFontName( const tjs_char* name ) {
	TVPSetDefaultFontName( name );
	DefaultFont.Face = ttstr(TVPGetDefaultFontName());
}
//---------------------------------------------------------------------------
void FontSystem::GetFontList(std::vector<ttstr> & list, tjs_uint32 flags, const struct tTVPFont & font ) {
	GetCurrentRasterizer()->GetFontList( list, flags, font );
}
//---------------------------------------------------------------------------
