//---------------------------------------------------------------------------
/*
	TVP2 ( T Visual Presenter 2 )  A script authoring tool
	Copyright (C) 2000 W.Dee <dee@kikyou.info> and contributors

	See details of license at "license.txt"
*/
//---------------------------------------------------------------------------
// Universal Storage System
//---------------------------------------------------------------------------
#ifndef StorageIntfH
#define StorageIntfH

#include "tjsNative.h"
#include "tjsHashSearch.h"
#include <vector>
#include <memory>

//---------------------------------------------------------------------------
// archive delimiter
//---------------------------------------------------------------------------
extern tjs_char  TVPArchiveDelimiter; //  = '>';



//---------------------------------------------------------------------------
// utilities
//---------------------------------------------------------------------------
ttstr TVPStringFromBMPUnicode(const tjs_uint16 *src, tjs_int maxlen = -1);
//---------------------------------------------------------------------------




//---------------------------------------------------------------------------
// tTVPArchive base archive class
//---------------------------------------------------------------------------
class tTVPArchive
{
private:
	tjs_uint RefCount;

public:
	//-- constructor
	tTVPArchive(const ttstr & name)
		{ ArchiveName = name; Init = false; RefCount = 1; }
	virtual ~tTVPArchive() { ; }

	//-- AddRef and Release
	void AddRef() { RefCount++; }
	void Release() { if(RefCount == 1) delete this; else RefCount--; }

	//-- must be implemented by delivered class
	virtual tjs_uint GetCount() = 0;
	virtual ttstr GetName(tjs_uint idx) = 0;
		// returned name must be already normalized using NormalizeInArchiveStorageName
		// and the index must be sorted by its name, using ttstr::operator < .
		// this is needed by fast directory search.

	virtual iTJSBinaryStream * CreateStreamByIndex(tjs_uint idx) = 0;

	//-- others, implemented in this class
private:

	tTJSHashTable<ttstr, tjs_uint, tTJSHashFunc<ttstr>, 1024> Hash;
	bool Init;
	ttstr ArchiveName;

public:
	static void NormalizeInArchiveStorageName(ttstr & name);

private:
	void AddToHash();
public:
	iTJSBinaryStream * CreateStream(const ttstr & name);
	bool IsExistent(const ttstr & name);

	tjs_int GetFirstIndexStartsWith(const ttstr & prefix);
		// returns first index which have 'prefix' at start of the name.
};
//---------------------------------------------------------------------------




/*[*/
//---------------------------------------------------------------------------
// iTVPStorageMedia
//---------------------------------------------------------------------------
/*
	abstract class for managing media ( like file: http: etc.)
*/
/*]*/
/*[*/
//---------------------------------------------------------------------------
class iTVPStorageLister // callback class for GetListAt
{
public:
	virtual void TJS_INTF_METHOD Add(const ttstr &file) = 0;
};
//---------------------------------------------------------------------------
class iTVPStorageMedia
{
public:
	virtual void TJS_INTF_METHOD AddRef() = 0;
	virtual void TJS_INTF_METHOD Release() = 0;

	virtual void TJS_INTF_METHOD GetName(ttstr &name) = 0;
		// returns media name like "file", "http" etc.

//	virtual bool TJS_INTF_METHOD IsCaseSensitive() = 0;
		// returns whether this media is case sensitive or not

	virtual void TJS_INTF_METHOD NormalizeDomainName(ttstr &name) = 0;
		// normalize domain name according with the media's rule

	virtual void TJS_INTF_METHOD NormalizePathName(ttstr &name) = 0;
		// normalize path name according with the media's rule

	// "name" below is normalized but does not contain media, eg.
	// not "media://domain/path" but "domain/path"

	virtual bool TJS_INTF_METHOD CheckExistentStorage(const ttstr &name) = 0;
		// check file existence

	virtual iTJSBinaryStream * TJS_INTF_METHOD Open(const ttstr & name, tjs_uint32 flags) = 0;
		// open a storage and return a iTJSBinaryStream instance.
		// name does not contain in-archive storage name but
		// is normalized.

	virtual void TJS_INTF_METHOD GetListAt(const ttstr &name, iTVPStorageLister * lister) = 0;
		// list files at given place

	virtual void TJS_INTF_METHOD GetLocallyAccessibleName(ttstr &name) = 0;
		// basically the same as above,
		// check wether given name is easily accessible from local OS filesystem.
		// if true, returns local OS native name. otherwise returns an empty string.
};

class iTVPStorageMedia2 : public iTVPStorageMedia
{
public:
	virtual bool TJS_INTF_METHOD Remove(const ttstr & name) = 0;
	    // remove file or directory. "name" is normalized but does not contain media name.

	virtual bool TJS_INTF_METHOD Move(const ttstr & from, const ttstr & to) = 0;
		// move file or directory. "from" and "to" are normalized but do not contain media name.		

	virtual tjs_uint64 TJS_INTF_METHOD  LastModifiedFileTime(const ttstr &name) = 0;
		// returns last modified file time in 100-nanosecond intervals since January 1, 1601 (UTC).

	virtual tjs_uint64 TJS_INTF_METHOD  FileSize(const ttstr &name) = 0;
		// returns file size in bytes. if the file does not exist or size cannot be determined, return 0.
};

//---------------------------------------------------------------------------
/*]*/



//---------------------------------------------------------------------------
// must be implemented in each platform
//---------------------------------------------------------------------------
extern tTVPArchive * TVPOpenArchive(const ttstr & name);
	// open archive and return tTVPArchive instance.

TJS_EXP_FUNC_DEF(ttstr, TVPGetTemporaryName, ());
	// retrieve file name to store temporary data ( must be unique, local name )

TJS_EXP_FUNC_DEF(ttstr, TVPGetAppPath, ());
	// retrieve program path, in normalized storage name

extern ttstr TVPGetResourcePath();
	// エンジン組み込みリソース (config.cf / messages.json / 同梱フォント等) が
	// 置かれている場所を、末尾 '/' 付きのストレージ名で返す。
	// 参照先はプラットフォームで異なる:
	//   WINVER / desktop  "resource://./"        (exe 埋め込み or OS リソース)
	//   Emscripten (wasm) "file://./resource/"   (MEMFS へ preload)
	// スクリプトからは System.resourcePath。プラットフォーム分岐を書かせない
	// ための口なので、利用側は必ずこれを前置して使うこと。

void TVPPreNormalizeStorageName(ttstr &name);
		// called by TVPNormalizeStorageName before it process the storage name.
		// user may pass the OS's native filename to the TVP storage system,
		// so that this function must convert it to the TVP storage name rules.

iTVPStorageMedia * TVPCreateFileMedia();
	// create basic default "file:" storage media



	/*
extern void TVPPreNormalizeStorageName(ttstr &name);

extern iTJSBinaryStream * TVPOpenStream(const ttstr & name, tjs_uint32 flags);
	// open a storage and return a iTJSBinaryStream instance.
	// name does not contain in-archive storage name but
	// is normalized.

extern bool TVPCheckExistentStorage(const ttstr &name);
	// check file existence


extern ttstr TVPGetMediaCurrent(const ttstr & name);
extern void TVPSetMediaCurrent(const ttstr & name, const ttstr & dir);

extern ttstr TVPGetNativeName(const ttstr &name);
	// retrieve OS native name

extern ttstr TVPGetLocallyAccessibleName(const ttstr &name);
	// check wether given name is easily accessible from local OS filesystem.
	// if true, returns local OS native name. otherwise returns an empty string.

*/
extern bool TVPRemoveFile(const ttstr &name);
	// remove local file ( "name" is a local *native* name )
	// this must not throw an exception ( return false if error )
extern bool TVPRemoveFolder(const ttstr &name);
	// remove local directory ( "name" is a local *native* name )
	// this must not throw an exception ( return false if error )
bool TVPCreateFolders(const ttstr &folder);
	// create folder along with the argument recursively (like mkdir -p).
	// 'folder' must be a local native name.

bool TVPMoveFile(const ttstr &oldname, const ttstr &newname);
	// rename file ( "oldname" and "newname" are local *native* names )
	// this must not throw an exception ( return false if error )

extern void TVPGetStorageListAt(const ttstr &name, iTVPStorageLister *lister);
	// list files at given place

extern tjs_uint64 TVPLastModifiedFileTime(const ttstr &name);
	// returns last modified file time in 100-nanosecond intervals since January 1, 1601 (UTC).
	// "name" is a local *native* name. if the file does not exist, return 0.

extern tjs_uint64 TVPFileSize(const ttstr &name);
	// returns file size in bytes.
	// "name" is a local *native* name. if the file does not exist or size cannot be determined, return 0.

//---------------------------------------------------------------------------

//---------------------------------------------------------------------------
// implementation in this unit
//---------------------------------------------------------------------------
TJS_EXP_FUNC_DEF(void, TVPRegisterStorageMedia, (iTVPStorageMedia *media));
	// register storage media
TJS_EXP_FUNC_DEF(void, TVPRegisterStorageMedia, (iTVPStorageMedia2 *media));
	// register storage media
TJS_EXP_FUNC_DEF(void, TVPUnregisterStorageMedia, (iTVPStorageMedia *media));
	// remove storage media

TJS_EXP_FUNC_DEF(bool, TVPRemoveStorage, (const ttstr &name));
	// remove file path
TJS_EXP_FUNC_DEF(bool, TVPMoveStorage, (const ttstr &from, const ttstr &to));
	// move file path
TJS_EXP_FUNC_DEF(tjs_uint64, TVPLastModifiedFileTimeStorage, (const ttstr &name));
	// returns last modified file time in 100-nanosecond intervals since January 1, 1601 (UTC).

TJS_EXP_FUNC_DEF(tjs_uint64, TVPFileSizeStorage, (const ttstr &name));
	// returns file size in bytes. if the file does not exist or size cannot be determined, return 0.

TJS_EXP_FUNC_DEF(iTJSBinaryStream *, TVPCreateStream, (const ttstr & name, tjs_uint32 flags = 0));
	// open "name" and return iTJSBinaryStream instance.
	// name will be local storage, network storage, in-archive storage, etc...

TJS_EXP_FUNC_DEF(bool, TVPIsExistentStorageNoSearch, (const ttstr &name));
	// if "name" is exists, return true. otherwise return false.
	// this does not search any auto search path.

TJS_EXP_FUNC_DEF(bool, TVPIsExistentStorageNoSearchNoNormalize, (const ttstr &name));

TJS_EXP_FUNC_DEF(ttstr, TVPNormalizeStorageName, (const ttstr & name));

TJS_EXP_FUNC_DEF(void, TVPSetCurrentDirectory, (const ttstr & name));
	// set system current directory.
	// directory must end with path delimiter '/',
	// or archive delimiter '>'.

TJS_EXP_FUNC_DEF(void, TVPGetLocalName, (ttstr &name));

TJS_EXP_FUNC_DEF(ttstr, TVPGetLocallyAccessibleName, (const ttstr &name));

TJS_EXP_FUNC_DEF(ttstr, TVPExtractStorageExt, (const ttstr & name));
	// extract "name"'s extension and return it.


TJS_EXP_FUNC_DEF(ttstr, TVPExtractStorageName, (const ttstr & name));
	// extract "name"'s storage name ( excluding path ) and return it.

TJS_EXP_FUNC_DEF(ttstr, TVPExtractStoragePath, (const ttstr & name));
	// extract "name"'s path ( including last delimiter ) and return it.

TJS_EXP_FUNC_DEF(ttstr, TVPChopStorageExt, (const ttstr & name));
	// chop storage's extension and return it.
	// extensition delimiter '.' will not be held.


TJS_EXP_FUNC_DEF(void, TVPAddAutoPath, (const ttstr & name));
	// add given path to auto search path

TJS_EXP_FUNC_DEF(void, TVPRemoveAutoPath, (const ttstr &name));
	// remove given path from auto search path

TJS_EXP_FUNC_DEF(ttstr, TVPGetPlacedPath, (const ttstr & name));
	// search path and return the path which the "name" is placed.

extern ttstr TVPSearchPlacedPath(const ttstr & name);
	// the same as TVPGetPlacedPath, except for rising exception when the storage
	// is not found.

TJS_EXP_FUNC_DEF(bool, TVPIsExistentStorage, (const ttstr &name));
	// if "name" is exists, return true. otherwise return false.
	// this searches auto search path.

TJS_EXP_FUNC_DEF(void, TVPClearStorageCaches, ());
	// clear all internal storage related caches.

	extern tjs_uint TVPSegmentCacheLimit; // XP3 segment cache limit, in bytes.

//---------------------------------------------------------------------------

std::shared_ptr<uint8_t> TVPReadStream(const tjs_char *name, tjs_uint64 *flen=0);
// read stream and return a buffer. the buffer is \0 terminated.
	// if size is not null, the size of the buffer is returned to it.

std::vector<std::string> *TVPReadLines(const tjs_char *name);
// read lines from file and return a vector of string. the buffer is \0 terminated.
	// if size is not null, the size of the buffer is returned to it.

//---------------------------------------------------------------------------
// tTJSNC_Storages : TJS Storages class
//---------------------------------------------------------------------------
class tTJSNC_Storages : public tTJSNativeClass
{
	typedef tTJSNativeClass inherited;

public:
	tTJSNC_Storages();
	static tjs_uint32 ClassID;

protected:
	tTJSNativeInstance *CreateNativeInstance();
};
//---------------------------------------------------------------------------
extern tTJSNativeClass * TVPCreateNativeClass_Storages();
//---------------------------------------------------------------------------

// 指定の拡張子はファイルロード時にオンメモリにする
// minSize > 0 の場合、ファイルサイズが minSize を超えるもののみキャッシュ対象。
// minSize == 0 (デフォルト) なら従来通り無条件にキャッシュ。
void TVPAddCacheTargetExtension(const ttstr &ext, tjs_uint64 minSize = 0);
void TVPRemoveCacheTargetExtension(const ttstr &ext);
bool TVPIsCacheTargetExtension(const ttstr &ext);
// 拡張子のキャッシュ最小サイズ閾値を取得。登録なしなら false。
bool TVPGetCacheTargetExtensionMinSize(const ttstr &ext, tjs_uint64 *outMinSize);
void TVPClearCacheTargetExtensions();

bool TVPIsCacheTargetFile(const ttstr &name);

//---------------------------------------------------------------------------
// pin / unpin / isPinned (P2: 両層共通)
//
// 「path 単位で sticky 化する」概念は file 層 (StorageCache) と decode 層
// (TVPGraphicCache) の両方に共通して効く。pin の永続管理 (path 集合) は
// ここで一元化し、各層は「load 時に集合参照で pinned 初期化」「pin/unpin
// 操作時に既存 entry の pinned フラグ更新」の責務だけ持つ。
//
// pin は load 前後どちらの順でも反映される:
//   - pin → load: entry 作成時に集合参照で pinned=true 初期化
//   - load → pin: pinCache 呼出で既存 entry の pinned フラグを更新
//---------------------------------------------------------------------------
void TVPPinCache(const ttstr &nname);
void TVPUnpinCache(const ttstr &nname);
bool TVPIsCachePathPinned(const ttstr &nname);
void TVPClearAllCachePins();    // pin 集合を空にする (両層 entry の pinned は触らない)

// cache キーとして使う path に正規化する。
// TVPGetPlacedPath で autopath 解決 (= 物理 path) を試み、解決できない
// (= file が存在しない or 非 file:// scheme) 場合は TVPNormalizeStorageName
// にフォールバック。
//
// 同一 file を異なる名前 (例: "bg.jpg" vs "image/bg.jpg") で load した場合
// でも同じキーに正規化されるよう、TVPLoadGraphic / PrefetchRequest /
// Storages.clearCache 等の cache 系統 API はここで揃える。
ttstr TVPResolveCachePath(const ttstr &input);

// 指定の拡張子は Storages.requestCache 等のプリロード時にデコードまで進めて
// TVPGraphicCache に登録する。Layer.loadImages 同期経路から自動でヒットする。
// minSize はファイルキャッシュ側と同様、ファイルサイズの最小閾値 (現状未使用)。
void TVPAddDecodeTargetExtension(const ttstr &ext, tjs_uint64 minSize = 0);
void TVPRemoveDecodeTargetExtension(const ttstr &ext);
bool TVPIsDecodeTargetExtension(const ttstr &ext);
void TVPClearDecodeTargetExtensions();
// file:// + 拡張子登録済みなら true
bool TVPIsDecodeTargetFile(const ttstr &name);


#endif
