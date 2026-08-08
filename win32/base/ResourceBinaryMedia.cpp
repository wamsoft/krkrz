//---------------------------------------------------------------------------
/*
	TVP2 ( T Visual Presenter 2 )  A script authoring tool
	Copyright (C) 2000 W.Dee <dee@kikyou.info> and contributors

	See details of license at "license.txt"
*/
//---------------------------------------------------------------------------
// WINVER 用 "resource://" ストレージメディア
//   exe に埋め込まれた "BINARY" 型リソース (CMake 生成 resources.rc の
//   `<filename> BINARY "<path>"`) を storage 層のストリームとして開けるようにする。
//   これにより、埋め込みフォント等を "resource://<name>" のパスで
//   TVPCreateStream / OpenFontFile 経由で読める (FreeType の GetFace は
//   フォントをパスで開き直す設計なので、この storage 露出が必要)。
//
//   SDL 系ビルドの generic/app/winres.cpp と同じ scheme "resource" / BINARY 型
//   だが、WINVER では未ビルドのため本ファイルで補う。クラス名は ResourceFS.cpp
//   (scheme "bres" / RCDATA 型) の tTVPResourceStorageMedia と衝突しないよう
//   tTVPResourceBinaryMedia とする。
//---------------------------------------------------------------------------
#include "tjsCommHead.h"
#include "StorageIntf.h"
#include "SysInitIntf.h"
#include "UtilStreams.h"   // tTVPMemoryStream (メモリ参照ストリーム)
#include <windows.h>

#define RESOURCE_MEDIA_NAME TJS_W("resource")

class tTVPResourceBinaryMedia : public iTVPStorageMedia
{
	typedef tTVPResourceBinaryMedia Self;
	static Self *Instance;
	tjs_int RefCount;

	// name (正規化済み・media 名なし、例 "./notoemoji-regular.ttf") から
	// BINARY リソースを引く。RC は文字列リソース名を大文字で格納するため
	// FindResource は大文字で照合する。
	HRSRC Fetch( const ttstr &name, HMODULE *retmodule = nullptr ) {
		HMODULE module = NULL; // exe 本体
		ttstr resname( TVPExtractStorageName(name) ); // basename
		resname.ToUppserCase();
		HRSRC res = ::FindResourceW( module, (const wchar_t*)resname.c_str(), L"BINARY" );
		if( res && retmodule ) *retmodule = module;
		return res;
	}
	const tjs_uint8* FindData( const ttstr &name, tjs_uint &size ) {
		HMODULE module = NULL;
		HRSRC res = Fetch( name, &module );
		if( !res ) return nullptr;
		size = (tjs_uint)::SizeofResource( module, res );
		HGLOBAL global = ::LoadResource( module, res );
		return global ? static_cast<const tjs_uint8*>(::LockResource(global)) : nullptr;
	}

public:
	tTVPResourceBinaryMedia() : RefCount(1) {}
	~tTVPResourceBinaryMedia() {}

	void TJS_INTF_METHOD AddRef(void) override { ++RefCount; }
	void TJS_INTF_METHOD Release(void) override {
		if( RefCount == 1 ) delete this; else --RefCount;
	}

	virtual void TJS_INTF_METHOD GetName(ttstr &name) override { name = RESOURCE_MEDIA_NAME; }
	virtual void TJS_INTF_METHOD NormalizeDomainName(ttstr &name) override { name.ToLowerCase(); }
	virtual void TJS_INTF_METHOD NormalizePathName  (ttstr &name) override { name.ToLowerCase(); }

	virtual bool TJS_INTF_METHOD CheckExistentStorage(const ttstr &name) override {
		return Fetch(name) != NULL;
	}

	virtual iTJSBinaryStream * TJS_INTF_METHOD Open(const ttstr & name, tjs_uint32 flags) override {
		if( (flags & TJS_BS_ACCESS_MASK) != TJS_BS_READ ) return nullptr;
		tjs_uint size = 0;
		const tjs_uint8 *data = FindData( name, size );
		if( !data ) return nullptr;
		// 埋め込みリソースはプロセス寿命の間有効なので参照ストリームで良い (コピー不要)。
		return new tTVPMemoryStream( data, size );
	}

	static BOOL CALLBACK EnumResNameProc(HMODULE, LPCWSTR, LPWSTR lpszName, LONG_PTR lParam) {
		iTVPStorageLister* lister = reinterpret_cast<iTVPStorageLister*>(lParam);
		if( IS_INTRESOURCE(lpszName) ) {
			wchar_t buf[32];
			_snwprintf( buf, 31, L"%d", (int)(ULONG_PTR)lpszName );
			lister->Add( ttstr(buf) );
		} else {
			ttstr n( (const tjs_char*)lpszName );
			n.ToLowerCase();
			lister->Add( n );
		}
		return TRUE;
	}
	virtual void TJS_INTF_METHOD GetListAt(const ttstr &name, iTVPStorageLister * lister) override {
		if( name != TJS_W("") && name != TJS_W("./") ) return;
		::EnumResourceNamesW( NULL, L"BINARY", (ENUMRESNAMEPROCW)EnumResNameProc, (LONG_PTR)lister );
	}

	virtual void TJS_INTF_METHOD GetLocallyAccessibleName(ttstr &name) override { name.Clear(); }

	//--------------------------------------------------------------
	static void Load() {
		if( !Instance ) {
			Instance = new Self();
			TVPRegisterStorageMedia( Instance );
		}
	}
	static void Unload() {
		if( Instance ) {
			TVPUnregisterStorageMedia( Instance );
			Instance->Release();
			Instance = nullptr;
		}
	}
};

tTVPResourceBinaryMedia * tTVPResourceBinaryMedia::Instance = nullptr;

static tTVPAtStart AtStart( TVP_ATSTART_PRI_PREPARE, tTVPResourceBinaryMedia::Load );
static tTVPAtExit  AtExit ( TVP_ATEXIT_PRI_PREPARE,  tTVPResourceBinaryMedia::Unload );
