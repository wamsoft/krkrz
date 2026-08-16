
#ifndef __FONT_SYSTEM_H__
#define __FONT_SYSTEM_H__

#include "tjsCommHead.h"
#include "tvpfontstruc.h"
#include "tjsHashSearch.h"
#include <string>
#include <unordered_map>

class tTVPWStringHash {
public:
	static tjs_uint32 Make(const tjs_string &val)
	{
		const tjs_char* ptr = val.c_str();
		if(*ptr == 0) return 0;
		tjs_uint32 v = 0;
		while(*ptr)
		{
			v += *ptr;
			v += (v << 10);
			v ^= (v >> 6);
			ptr++;
		}
		v += (v << 3);
		v ^= (v >> 11);
		v += (v << 15);
		if(!v) v = (tjs_uint32)-1;
		return v;
	}
};

class FontSystem {
	bool FontNamesInit;
	tTJSHashTable<tjs_string, tjs_int, tTVPWStringHash> TVPFontNames;

	tTVPFont DefaultFont;
	bool DefaultLOGFONTCreated;

	void InitFontNames();
	//---------------------------------------------------------------------------
	void AddFont( const tjs_string& name );

	void ConstructDefaultFont();

	// data/fonts.json 由来の遅延ロード対象: フォント名(family/alias) -> ストレージ名。
	// 起動時にメタデータを読んでここへ登録するだけ(FreeType パースはしない)。
	// 実ファイルは EnsureLazyFontLoaded() で初回使用時に AddExtraFont される。
	std::unordered_map<tjs_string, tjs_string> LazyFontFiles;
	// 上と同じ name->storage だが EnsureLazyFontLoaded で erase されない永続版。
	// glyphware など FreeType 遅延ロードとは別経路の name 解決に使う
	// (GetLazyFontStorage)。FreeType が先に消費しても名前→ストレージは引ける。
	std::unordered_map<tjs_string, tjs_string> LazyFontStorageAll;
	bool FontMetadataLoaded = false;
	void LoadFontMetadata();

public:
	FontSystem();
	tjs_string GetBeingFont(tjs_string fonts);
	const tTVPFont& GetDefaultFont() const {
		return DefaultFont;
	}
	bool FontExists( const tjs_string &name );
	// メタデータ(data/fonts.json)登録名なら初回だけ実ファイルを遅延ロードする。
	// ロードして登録できたら true。既ロード/対象外は false。
	bool EnsureLazyFontLoaded( const tjs_string &name );
	// Font.addFont でストレージ内のフォントを追加する
	// faces が期待通り動作するのは FreeType のみ、GDI では読み込まれたフェイス名は現在正しく取得出来ない
	void AddExtraFont( const tjs_string& storage, std::vector<ttstr>* faces );

	// data/fonts.json 宣言名(family/alias) からストレージ名を引く。glyphware など
	// フォント名でフォントファイルを解決したい利用側向け。見つかれば true。
	bool GetLazyFontStorage( const tjs_string& name, tjs_string& storage ) const;
	// 既知の name→storage 対応 (fonts.json 宣言 + 実行時登録) を列挙する
	// (フォントサービスの検索がレジストリへ流し込むのに使う)
	void EnumerateLazyFontStorages( std::vector<std::pair<tjs_string, tjs_string>>& out ) const;
	// 宣言のみの遅延フォント登録 (fonts.json 1 エントリ相当。ファイルは開かず、
	// 初回使用時に EnsureLazyFontLoaded でロードされる)
	void RegisterLazyFont( const tjs_string& name, const tjs_string& storage );
	// fonts.json 未読なら読む (Font.queryFonts 等、描画前にメタデータが要る経路用)
	void EnsureFontMetadataLoaded() { LoadFontMetadata(); }
	// ロード済みフォントの実 face 名→storage を記録する (遅延ロードは仕掛けず
	// LazyFontStorageAll のみ。generic/SDL のシステム・同梱フォント登録経路が
	// glyphware 等の名前解決に載せるために使う。AddExtraFont の記録と同等)
	void RecordLoadedFontStorage( const tjs_string& name, const tjs_string& storage ) {
		LazyFontStorageAll[ name ] = storage;
	}

	const tjs_char* GetDefaultFontName() const;
	void SetDefaultFontName( const tjs_char* name );

	void GetFontList(std::vector<ttstr> & list, tjs_uint32 flags, const struct tTVPFont & font );
};

// プロセス唯一の FontSystem (実体は LayerBitmapImpl.cpp)。
extern FontSystem* TVPFontSystem;

#endif // __FONT_SYSTEM_H__

