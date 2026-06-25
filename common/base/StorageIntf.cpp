//---------------------------------------------------------------------------
/*
	TVP2 ( T Visual Presenter 2 )  A script authoring tool
	Copyright (C) 2000 W.Dee <dee@kikyou.info> and contributors

	See details of license at "license.txt"
*/
//---------------------------------------------------------------------------
// Universal Storage System
//---------------------------------------------------------------------------
#include "tjsCommHead.h"

#include <algorithm>
#include <atomic>
#include <new>
#include <stdexcept>
#include <map>
#include <mutex>
#include <set>
#include "StorageIntf.h"
#include "tjsUtils.h"
#include "MsgIntf.h"
#include "EventIntf.h"
#include "DebugIntf.h"
#include "tjsArray.h"
#include "SysInitIntf.h"
#include "XP3Archive.h"
#include "TickCount.h"
#include "StringUtil.h"
#include "tjsDictionary.h"
#include "Application.h"
#include "StorageCache.h"
#include "LogIntf.h"
#include "GraphicsLoaderIntf.h"
#include "GraphicsLoadThread.h"

#define TVP_DEFAULT_ARCHIVE_CACHE_NUM 64
#define TVP_DEFAULT_AUTOPATH_CACHE_NUM 256


//---------------------------------------------------------------------------
// オプション
//---------------------------------------------------------------------------
static bool TVPIsInitStorageOptions = false;
static bool TVPIgnoreFileProperty = false;
//---------------------------------------------------------------------------
static void TVPInitStorageOptions() {
	if( TVPIsInitStorageOptions ) return;

	tTJSVariant val;
	if( TVPGetCommandLine( TJS_W( "-ignorefileprop" ), &val ) ) {
		ttstr str( val );
		if( str == TJS_W( "yes" ) )
			TVPIgnoreFileProperty = true;
		else
			TVPIgnoreFileProperty = false;
	}
	TVPIsInitStorageOptions = true;
}
//---------------------------------------------------------------------------


//---------------------------------------------------------------------------
// global variables
//---------------------------------------------------------------------------
// current media ( ex. "http" "ftp" "file" )
ttstr TVPCurrentMedia;
// archive delimiter
// this changes '>' from '#' since 2.19 beta 14
tjs_char  TVPArchiveDelimiter = '>';
//---------------------------------------------------------------------------




//---------------------------------------------------------------------------
// statics
//---------------------------------------------------------------------------
static tTJSCriticalSection TVPCreateStreamCS;
//---------------------------------------------------------------------------



//---------------------------------------------------------------------------
// cache対象拡張子
//---------------------------------------------------------------------------

// 拡張子 → 最小キャッシュ対象サイズ。0 の場合は無条件でキャッシュ。
static std::map<ttstr, tjs_uint64> TVPCacheTargetExtensions;
static tTJSCriticalSection TVPCacheTargetExtensionsCS;

// 拡張子 → 最小サイズ。Storages.requestCache 経路でデコードまで進める対象。
static std::map<ttstr, tjs_uint64> TVPDecodeTargetExtensions;
static tTJSCriticalSection TVPDecodeTargetExtensionsCS;

//---------------------------------------------------------------------------
static ttstr TVPNormalizeCacheTargetExtension(const ttstr &ext)
{
	ttstr normalized_ext = ext;
	if(!normalized_ext.IsEmpty() && normalized_ext[0] != TJS_W('.'))
	{
		normalized_ext = TJS_W(".") + normalized_ext;
	}

	tjs_char *p = normalized_ext.Independ();
	if (p) {
		while(*p)
		{
			if(*p >= TJS_W('A') && *p <= TJS_W('Z'))
				*p += TJS_W('a') - TJS_W('A');
			p++;
		}
	}
	return normalized_ext;
}
//---------------------------------------------------------------------------
void TVPAddCacheTargetExtension(const ttstr &ext, tjs_uint64 minSize)
{
	ttstr normalized_ext = TVPNormalizeCacheTargetExtension(ext);
	tTJSCriticalSectionHolder cs_holder(TVPCacheTargetExtensionsCS);
	TVPCacheTargetExtensions[normalized_ext] = minSize;
}
//---------------------------------------------------------------------------
void TVPRemoveCacheTargetExtension(const ttstr &ext)
{
	ttstr normalized_ext = TVPNormalizeCacheTargetExtension(ext);
	tTJSCriticalSectionHolder cs_holder(TVPCacheTargetExtensionsCS);
	TVPCacheTargetExtensions.erase(normalized_ext);
}
//---------------------------------------------------------------------------
bool TVPIsCacheTargetExtension(const ttstr &ext)
{
	ttstr normalized_ext = TVPNormalizeCacheTargetExtension(ext);
	tTJSCriticalSectionHolder cs_holder(TVPCacheTargetExtensionsCS);
	return TVPCacheTargetExtensions.find(normalized_ext) != TVPCacheTargetExtensions.end();
}
//---------------------------------------------------------------------------
bool TVPGetCacheTargetExtensionMinSize(const ttstr &ext, tjs_uint64 *outMinSize)
{
	ttstr normalized_ext = TVPNormalizeCacheTargetExtension(ext);
	tTJSCriticalSectionHolder cs_holder(TVPCacheTargetExtensionsCS);
	auto i = TVPCacheTargetExtensions.find(normalized_ext);
	if (i == TVPCacheTargetExtensions.end()) return false;
	if (outMinSize) *outMinSize = i->second;
	return true;
}
//---------------------------------------------------------------------------
void TVPClearCacheTargetExtensions()
{
	tTJSCriticalSectionHolder cs_holder(TVPCacheTargetExtensionsCS);
	TVPCacheTargetExtensions.clear();
}

// キャッシュ対象かどうかの判定
bool TVPIsCacheTargetFile(const ttstr &name)
{
	return name != "" && name.StartsWith(TJS_W("file://"));
}

//---------------------------------------------------------------------------
// pin 集合 (P2)
// path 単位で両層 (file/decode) を sticky 化する。永続管理はここで一元化。
//---------------------------------------------------------------------------
static std::set<ttstr> TVPPinnedCachePaths;
static tTJSCriticalSection TVPPinnedCachePathsCS;

bool TVPIsCachePathPinned(const ttstr &nname)
{
	tTJSCriticalSectionHolder cs(TVPPinnedCachePathsCS);
	return TVPPinnedCachePaths.find(nname) != TVPPinnedCachePaths.end();
}

// pin/unpin の path 正規化:
//   pin set には**両方の正規化結果** (norm = NormalizeStorageName と
//   placed = GetPlacedPath) を入れる。これは pin 時に file が存在しないと
//   GetPlacedPath が空を返すなどの edge case に備えるため。
//   通常運用では cache キーは TVPResolveCachePath で placed に統一される
//   ので、norm form 単独のヒットは autopath 未解決時のフォールバック用。
static void TVPCollectCachePathVariants(const ttstr &input,
                                          ttstr &out_norm,
                                          ttstr &out_placed)
{
	out_norm = TVPNormalizeStorageName(input);
	out_placed = TVPGetPlacedPath(input);  // ファイルが見つからなければ空
	if (out_placed == out_norm) out_placed = ttstr();  // 同じなら片方だけで OK
}

// cache キー用の path 正規化。GetPlacedPath で autopath 解決を試み、
// 失敗時は NormalizeStorageName にフォールバック。
ttstr TVPResolveCachePath(const ttstr &input)
{
	ttstr placed = TVPGetPlacedPath(input);
	if (!placed.IsEmpty()) return placed;
	return TVPNormalizeStorageName(input);
}

void TVPPinCache(const ttstr &input)
{
	ttstr norm, placed;
	TVPCollectCachePathVariants(input, norm, placed);

	bool inserted_norm = false, inserted_placed = false;
	{
		tTJSCriticalSectionHolder cs(TVPPinnedCachePathsCS);
		if (!norm.IsEmpty()) inserted_norm = TVPPinnedCachePaths.insert(norm).second;
		if (!placed.IsEmpty()) inserted_placed = TVPPinnedCachePaths.insert(placed).second;
	}
	if (inserted_norm || inserted_placed) {
		if (placed.IsEmpty()) {
			TVPLOG_DEBUG("Cache:pin:{}", norm);
		} else {
			TVPLOG_DEBUG("Cache:pin:{} (+placed:{})", norm, placed);
		}
	}
	// 既に load 済の entry があればその pinned フラグを立てる (両 variant)
	if (!norm.IsEmpty()) {
		TVPSetStorageCacheEntryPinned(norm, true);
		TVPSetGraphicCacheEntryPinned(norm, true);
	}
	if (!placed.IsEmpty()) {
		TVPSetStorageCacheEntryPinned(placed, true);
		TVPSetGraphicCacheEntryPinned(placed, true);
	}

	// pin 対象を実際にロードして cache に乗せる (= Storages.requestCache 相当)。
	// pin set には先に登録済みなので、cache push のタイミングで pinned=true で
	// 初期化される。idempotent (既に cache 済みの path に再度発火しても、
	// cache thread 側で skip される)。
	// path は scheme/拡張子を含むべきなので placed (autopath 解決済) を優先。
	if (Application) {
		const ttstr &request_path = !placed.IsEmpty() ? placed : norm;
		if (!request_path.IsEmpty()) {
			if (TVPIsCacheTargetFile(request_path)) {
				Application->CacheFileRequest(request_path, /*fast=*/false, /*minSize=*/0);
			}
			if (TVPIsDecodeTargetFile(request_path)) {
				Application->LoadImagePrefetchRequest(request_path);
			}
		}
	}
}

void TVPUnpinCache(const ttstr &input)
{
	ttstr norm, placed;
	TVPCollectCachePathVariants(input, norm, placed);

	bool was_pinned = false;
	{
		tTJSCriticalSectionHolder cs(TVPPinnedCachePathsCS);
		if (!norm.IsEmpty()) was_pinned |= TVPPinnedCachePaths.erase(norm) > 0;
		if (!placed.IsEmpty()) was_pinned |= TVPPinnedCachePaths.erase(placed) > 0;
	}
	if (was_pinned) {
		TVPLOG_DEBUG("Cache:unpin:{}{}", norm,
		             placed.IsEmpty() ? ttstr() : ttstr(TJS_W(" (+placed)")));
	}
	// 既存 entry の pinned フラグを解除 (両 variant、次の駆逐サイクルで chop 候補に)
	if (!norm.IsEmpty()) {
		TVPSetStorageCacheEntryPinned(norm, false);
		TVPSetGraphicCacheEntryPinned(norm, false);
	}
	if (!placed.IsEmpty()) {
		TVPSetStorageCacheEntryPinned(placed, false);
		TVPSetGraphicCacheEntryPinned(placed, false);
	}
}

void TVPClearAllCachePins()
{
	size_t had;
	{
		tTJSCriticalSectionHolder cs(TVPPinnedCachePathsCS);
		had = TVPPinnedCachePaths.size();
		TVPPinnedCachePaths.clear();
	}
	if (had > 0) {
		TVPLOG_DEBUG("Cache:clearAllPins: count={}", had);
	}
}

//---------------------------------------------------------------------------
// decode-target 拡張子 (Storages.requestCache でデコードまで進める対象)
//---------------------------------------------------------------------------
void TVPAddDecodeTargetExtension(const ttstr &ext, tjs_uint64 minSize)
{
	ttstr normalized_ext = TVPNormalizeCacheTargetExtension(ext);
	tTJSCriticalSectionHolder cs_holder(TVPDecodeTargetExtensionsCS);
	TVPDecodeTargetExtensions[normalized_ext] = minSize;
}
//---------------------------------------------------------------------------
void TVPRemoveDecodeTargetExtension(const ttstr &ext)
{
	ttstr normalized_ext = TVPNormalizeCacheTargetExtension(ext);
	tTJSCriticalSectionHolder cs_holder(TVPDecodeTargetExtensionsCS);
	TVPDecodeTargetExtensions.erase(normalized_ext);
}
//---------------------------------------------------------------------------
bool TVPIsDecodeTargetExtension(const ttstr &ext)
{
	ttstr normalized_ext = TVPNormalizeCacheTargetExtension(ext);
	tTJSCriticalSectionHolder cs_holder(TVPDecodeTargetExtensionsCS);
	return TVPDecodeTargetExtensions.find(normalized_ext) != TVPDecodeTargetExtensions.end();
}
//---------------------------------------------------------------------------
void TVPClearDecodeTargetExtensions()
{
	tTJSCriticalSectionHolder cs_holder(TVPDecodeTargetExtensionsCS);
	TVPDecodeTargetExtensions.clear();
}
//---------------------------------------------------------------------------
// decode prefetch は path-keyed の TVPGraphicCache に展開後 bitmap を登録する
// 仕組みなので、scheme は問わない (file:// / psb:// / psd:// 等の任意の
// スキームで動作)。worker が tTVPStreamHolder 経由でストリームを開く際、
// 各 scheme の iTVPStorageMedia::Open が呼ばれる。TVPCreateStream は
// TVPCreateStreamCS で global lock 済みなので worker からの並列 open でも安全。
bool TVPIsDecodeTargetFile(const ttstr &name)
{
	if(name.IsEmpty()) return false;
	ttstr ext = TVPExtractStorageExt(name);
	return TVPIsDecodeTargetExtension(ext);
}

//---------------------------------------------------------------------------
// utilities
//---------------------------------------------------------------------------
ttstr TVPStringFromBMPUnicode(const tjs_uint16 *src, tjs_int maxlen)
{
	// convert to ttstr from BMP unicode
	if(sizeof(tjs_char) == 2)
	{
		// sizeof(tjs_char) is 2 (windows native)
		if(maxlen == -1)
			return ttstr((const tjs_char*)src);
		else
			return ttstr((const tjs_char*)src, maxlen);
	}
	else if(sizeof(tjs_char) == 4)
	{
		// sizeof(tjs_char) is 4 (UCS32)
  		// FIXME: NOT TESTED CODE
		tjs_int len = 0;
		const tjs_uint16 *p = src;
		while(*p) len++, p++;
		if(maxlen != -1 && len > maxlen) len = maxlen;
		ttstr ret((tTJSStringBufferLength)(len));
		tjs_char *dest = ret.Independ();
		p = src;
		while(len && *p)
		{
			*dest = *p;
			dest++;
			p++;
			len --;
		}
		*dest = 0;
		ret.FixLen();
		return ret;
	}
	return (const tjs_char*)TVPTjsCharMustBeTwoOrFour;
}
//---------------------------------------------------------------------------






//---------------------------------------------------------------------------
// tTVPStorageMediaManager
//---------------------------------------------------------------------------
class tTVPStorageMediaManager
{
	class tMediaNameString : public tTJSString
	{
	public:
		bool operator == (const tMediaNameString &rhs) const
		{
			const tjs_char * l_p = c_str();
			const tjs_char * r_p = rhs.c_str();

			while(*l_p && *r_p)
			{
				if(*l_p == TJS_W(':')) break;
				if(*r_p == TJS_W(':')) break;
				if(*l_p != *r_p) break;
				l_p++;
				r_p++;
			}
			if((*l_p == TJS_W(':') || *l_p == 0) &&
				(*r_p == TJS_W(':') || *r_p == 0)) return true;
			return false;
		}
	};

	class tHashFunc
	{
	public:
		static tjs_uint32 Make(const tMediaNameString &key)
		{
			if(key.IsEmpty()) return 0;
			const tjs_char *str = key.c_str();
			tjs_uint32 ret = 0;
			while(*str && *str != ':')
			{
				ret += *str;
				ret += (ret << 10);
				ret ^= (ret >> 6);
				str++;
			}
			ret += (ret << 3);
			ret ^= (ret >> 11);
			ret += (ret << 15);
			if(!ret) ret = (tjs_uint32)-1;
			return ret;
		}
	};

	class tMediaRecord
	{
	public:
		int version;
		ttstr CurrentDomain;
		ttstr CurrentPath;
		tTJSRefHolder<iTVPStorageMedia> MediaIntf;
		tjs_int MediaNameLen;
//		bool IsCaseSensitive;

		tMediaRecord(iTVPStorageMedia *media) : version(1), MediaIntf(media), CurrentDomain("."), CurrentPath("/")
			{ ttstr name; media->GetName(name); MediaNameLen = name.GetLen();
			/*IsCaseSensitive = media->IsCaseSensitive();*/ }

		tMediaRecord(iTVPStorageMedia2 *media) : version(2), MediaIntf(media), CurrentDomain("."), CurrentPath("/")
			{ ttstr name; media->GetName(name); MediaNameLen = name.GetLen();
			/*IsCaseSensitive = media->IsCaseSensitive();*/ }


		const tjs_char *GetDomainAndPath(const ttstr &name)
		{
			return name.c_str() + MediaNameLen + 3;
				// 3 = strlen("://")
		}
	};

	typedef tTJSHashTable<tMediaNameString, tMediaRecord, tHashFunc, 16> tHashTable;

	tHashTable HashTable;

public:
	tTVPStorageMediaManager();
	~tTVPStorageMediaManager();

private:
	static void ThrowUnsupportedMediaType(const ttstr &name);
	tMediaRecord * GetMediaRecord(const ttstr &name);

public:
	void Register(iTVPStorageMedia * media);
	void Unregister(iTVPStorageMedia * media);
	void Register(iTVPStorageMedia2 * media);

	ttstr NormalizeStorageName(const ttstr &name, ttstr *ret_media = NULL,
		ttstr *ret_domain = NULL, ttstr *ret_path = NULL);

	void SetCurrentDirectory(const ttstr &name);

	static ttstr ExtractMediaName(const ttstr &name);

	bool CheckExistentStorage(const ttstr & name);
	iTJSBinaryStream * Open(const ttstr & name, tjs_uint32 flags);
	void GetListAt(const ttstr &name, iTVPStorageLister *lister);
	ttstr GetLocallyAccessibleName(const ttstr &name);
	
	bool Remove(const ttstr & name);
	bool Move(const ttstr & from, const ttstr & to);
	tjs_uint64 LastModifiedFileTime(const ttstr &name);
	tjs_uint64 FileSize(const ttstr &name);

} TVPStorageMediaManager;
//---------------------------------------------------------------------------
tTVPStorageMediaManager::tTVPStorageMediaManager()
{
	iTVPStorageMedia *filemedia = TVPCreateFileMedia();
	Register(filemedia);
	filemedia->Release();
}
//---------------------------------------------------------------------------
tTVPStorageMediaManager::~tTVPStorageMediaManager()
{
}
//---------------------------------------------------------------------------
void tTVPStorageMediaManager::ThrowUnsupportedMediaType(const ttstr &name)
{
	TVPThrowExceptionMessage(TVPUnsupportedMediaName, ExtractMediaName(name));
}
//---------------------------------------------------------------------------
tTVPStorageMediaManager::tMediaRecord *
	tTVPStorageMediaManager::GetMediaRecord(const ttstr &name)
{
	tMediaRecord *rec = HashTable.Find(*(tMediaNameString*)&name);
	if(!rec) ThrowUnsupportedMediaType(name);
	return rec;
}
//---------------------------------------------------------------------------
void tTVPStorageMediaManager::Register(iTVPStorageMedia * media)
{
	ttstr medianame;
	media->GetName(medianame);

	tMediaRecord *rec = HashTable.Find(*(tMediaNameString*)&medianame);
	if(rec)
		TVPThrowExceptionMessage( TVPMediaNameHadAlreadyBeenRegistered, medianame );

	tMediaRecord new_rec(media);

	HashTable.Add(*(tMediaNameString*)&medianame, new_rec);
}
void tTVPStorageMediaManager::Register(iTVPStorageMedia2 * media)
{
	ttstr medianame;
	media->GetName(medianame);

	tMediaRecord *rec = HashTable.Find(*(tMediaNameString*)&medianame);
	if(rec)
		TVPThrowExceptionMessage( TVPMediaNameHadAlreadyBeenRegistered, medianame );

	tMediaRecord new_rec(media);

	HashTable.Add(*(tMediaNameString*)&medianame, new_rec);
}
//---------------------------------------------------------------------------
void tTVPStorageMediaManager::Unregister(iTVPStorageMedia * media)
{
	ttstr medianame;
	media->GetName(medianame);

	tMediaRecord *rec = HashTable.Find(*(tMediaNameString*)&medianame);
	if(!rec)
		TVPThrowExceptionMessage( TVPMediaNameIsNotRegistered, medianame );
	HashTable.Delete(*(tMediaNameString*)&medianame);
}
//---------------------------------------------------------------------------
ttstr tTVPStorageMediaManager::NormalizeStorageName(const ttstr &name,
	ttstr *ret_media, ttstr *ret_domain, ttstr *ret_path)
{
	// Normalize storage name.

	// storage name is basically in following form:
	// media://domain/path

	// media is sort of access method, like "file", "http" ...etc.
	// domain represents in which computer the data is.
	// path is where the data is in the computer.

	// empty check
	if(name.IsEmpty()) return name; // empty name is empty name

	// pre-normalize
	const tjs_char *pca;//, *pcb, *pcc;
	tjs_char *pa, *pb, *pc;

	ttstr tmp(name);
	TVPPreNormalizeStorageName(tmp);

	// unify path delimiter
	pa = tmp.Independ();
	while(*pa)
	{
		if(*pa == TJS_W('\\')) *pa = TJS_W('/');
		pa++;
	}

	// save in-archive storage name and normalize it
	ttstr inarchive_name;
	bool inarc_name_found = false;
	pca = tmp.c_str();
	pa = const_cast<tjs_char *>(TJS_strchr(pca, TVPArchiveDelimiter));
	if(pa)
	{
		inarchive_name = ttstr(pa + 1);
		tTVPArchive::NormalizeInArchiveStorageName(inarchive_name);
		inarc_name_found = true;
		tmp = ttstr(pca, (int)(pa - pca));
	}
	if(tmp.IsEmpty()) TVPThrowExceptionMessage(TVPInvalidPathName, name);


	// split the name into media, domain, path
	// (and guess what component is omitted)
	ttstr media, domain, path;

	// - find media name
	//   media name is: /^[A-Za-z]+:/
	pa = pb = tmp.Independ();
	while(*pa)
	{
		if(!(
			*pa >= TJS_W('A') && *pa <= TJS_W('Z') ||
			*pa >= TJS_W('a') && *pa <= TJS_W('z') )) break;
		pa ++;
	}

	if(*pa == TJS_W(':'))
	{
		// media name found
		media = ttstr(pb, (int)(pa - pb));
		pa ++;
	}
	else
	{
		pa = pb;
	}

	// - find domain name
	// at this place, pa may point one of following:
	//  ///path        (domain is omitted)
	//  //domain/path  (none is omitted)
	//  /path          (domain is omitted)
	//  relative-path  (domain and current path are omitted)

	if(pa[0] == TJS_W('/'))
	{
		if(pa[1] == TJS_W('/'))
		{
			if(pa[2] == TJS_W('/'))
			{
				// slash count 3: domain is ommited
				pa += 2;
			}
			else
			{
				// slash count 2: none is omitted
				pa += 2;
				// find '/' as a domain delimiter
				pc = TJS_strchr(pa, TJS_W('/'));
				if(!pc)
					TVPThrowExceptionMessage(TVPInvalidPathName, name);
				domain = ttstr(pa, (int)(pc - pa));
				pa = pc;
			}
		}
		else
		{
			// slash count 1: domain is omitted
			;
			//
		}
	}

	// - get path name
	path = pa;

	// supply omitted and normalize
	if(media.IsEmpty())
	{
		media = TVPCurrentMedia;
	}
	else
	{
		// normalize media name ( make them all small )
		tjs_char *p = media.Independ();
		while(*p)
		{
			if(*p >= TJS_W('A') && *p <= TJS_W('Z'))
				*p += (TJS_W('a') - TJS_W('A'));
			p ++;
		}
	}

	tMediaRecord * mediarec = GetMediaRecord(media);

	if(domain.IsEmpty()) domain = mediarec->CurrentDomain;
	mediarec->MediaIntf.GetObjectNoAddRef()->NormalizeDomainName(domain);

	if(path.IsEmpty())
	{
		path = TJS_W("/");
	}
	else if(path.c_str()[0] != TJS_W('/'))
	{
		path = mediarec->CurrentPath + path;
	}
	mediarec->MediaIntf.GetObjectNoAddRef()->NormalizePathName(path);

	// compress redudant path accesses
	if(inarc_name_found)
	{
		tjs_char tmp[2];
		tmp[0] = TVPArchiveDelimiter;
		tmp[1] = 0;
		path += tmp + inarchive_name;
	}

	pa = pb = pc = path.Independ(); // pa = read pointer, pb = write pointer, pc = start
	tjs_int dot_count = -1;

	while(true)
	{
		if(*pa == TVPArchiveDelimiter || *pa == TJS_W('/') || *pa == 0)
		{
			tjs_char delim = 0;

			if(*pa && dot_count == 0)
			{
				// duplicated slashes
				pb --;
			}
			else if(dot_count > 0)
			{
				pb --;
				while(pb >= pc)
				{
					if(*pb == TJS_W('/') || *pb == TVPArchiveDelimiter)
					{
						dot_count --;
						if(dot_count == 0)
						{
							delim = *pb;
							break;
						}
						if(*pb == TVPArchiveDelimiter) TVPThrowExceptionMessage(TVPInvalidPathName, name);
					}
					pb --;
				}
				if(pb < pc) TVPThrowExceptionMessage(TVPInvalidPathName, name);
			}

			if(!delim)
				*pb = *pa;
			else
				*pb = delim;
			if(*pa == 0) break;
			pb ++;
			pa ++;
			dot_count = 0;
		}
		else if(*pa == TJS_W('.'))
		{
			*(pb++) = *(pa++);
			if(dot_count != -1) dot_count ++;
		}
		else
		{
			*(pb++) = *(pa++);
			dot_count = -1;
		}
	}

	path.FixLen();

	// merge and return normalize storage name
	if(ret_media) *ret_media = media;
	if(ret_domain) *ret_domain = domain;
	if(ret_path) *ret_path = path;

	tmp = media + TJS_W("://") + domain + path;

	return tmp;
}
//---------------------------------------------------------------------------
void tTVPStorageMediaManager::SetCurrentDirectory(const ttstr &name)
{
	tjs_char ch = name.GetLastChar();
	if(ch != TJS_W('/') && ch != TJS_W('\\') && ch != TVPArchiveDelimiter)
		TVPThrowExceptionMessage(TVPMissingPathDelimiterAtLast);

	ttstr media, domain, path;
	NormalizeStorageName(name, &media, &domain, &path);

	tMediaRecord *rec = GetMediaRecord(media);
	rec->CurrentDomain = domain;
	rec->CurrentPath = path;
	TVPCurrentMedia = media;
}
//---------------------------------------------------------------------------
ttstr tTVPStorageMediaManager::ExtractMediaName(const ttstr &name)
{
	// extract media name from normalized storage named "name".
	// returned media name does not contain colon.

	const tjs_char * p = name.c_str();
	const tjs_char * po = p;
	while(*p && *p != TJS_W(':')) p++;
	return ttstr(po, (int)(p - po));
}
//---------------------------------------------------------------------------
bool tTVPStorageMediaManager::CheckExistentStorage(const ttstr & name)
{
	// gateway for CheckExistentStorage
	// name must not be an in-archive storage name
	tMediaRecord *rec = GetMediaRecord(name);
	return rec->MediaIntf.GetObjectNoAddRef()->CheckExistentStorage(rec->GetDomainAndPath(name));
}
//---------------------------------------------------------------------------
iTJSBinaryStream * tTVPStorageMediaManager::Open(const ttstr & name, tjs_uint32 flags)
{
	// gateway for Open
	// name must not be an in-archive storage name
	tMediaRecord *rec = GetMediaRecord(name);
	return rec->MediaIntf.GetObjectNoAddRef()->Open(rec->GetDomainAndPath(name), flags);
}
//---------------------------------------------------------------------------
void tTVPStorageMediaManager::GetListAt(const ttstr &name, iTVPStorageLister * lister)
{
	// gateway for GetListAt
	// name must not be an in-archive storage name
	tMediaRecord *rec = GetMediaRecord(name);
	/*return */rec->MediaIntf.GetObjectNoAddRef()->GetListAt(rec->GetDomainAndPath(name), lister);
}
//---------------------------------------------------------------------------
ttstr tTVPStorageMediaManager::GetLocallyAccessibleName(const ttstr &name)
{
	// gateway for GetLocallyAccessibleName
	// name must not be an in-archive storage name
	tMediaRecord *rec = GetMediaRecord(name);
	ttstr dname = rec->GetDomainAndPath(name);
	rec->MediaIntf.GetObjectNoAddRef()->GetLocallyAccessibleName(dname);
	return dname;
}
//---------------------------------------------------------------------------

bool tTVPStorageMediaManager::Remove(const ttstr & name)
{
	tMediaRecord *rec = GetMediaRecord(name);
	ttstr dname = rec->GetDomainAndPath(name);
	if (rec->version >= 2) {
		// if media supports iTVPStorageMedia2, use Remove2 method
		TVPLOG_DEBUG("Trying to remove storage by media's Remove method: {}", name);
		iTVPStorageMedia2 *media2 = (iTVPStorageMedia2 *)(rec->MediaIntf.GetObjectNoAddRef());
		if (media2 && media2->Remove(dname)) {
			TVPLOG_DEBUG("Remove by media's Remove method succeeded: {}", name);
			return true;
		}
	}
	rec->MediaIntf.GetObjectNoAddRef()->GetLocallyAccessibleName(dname);
	if (dname != "") {
		// if the file is accessible from local file system, try to remove it by ourselves
		TVPLOG_DEBUG("delete file:{}", dname);
		return TVPRemoveFile(dname);
	}
	return false;
}

bool tTVPStorageMediaManager::Move(const ttstr & from, const ttstr & to)
{
	tMediaRecord *rec = GetMediaRecord(from);
	tMediaRecord *rec_to   = GetMediaRecord(to);
	if (rec != rec_to) {
		return false;
	}
	ttstr dname_from = rec->GetDomainAndPath(from);
	ttstr dname_to   = rec->GetDomainAndPath(to);

	if (rec->version >= 2) {
		// if media supports iTVPStorageMedia2, use Remove method
		TVPLOG_DEBUG("Trying to move storage by media's Move method: from:{} to:{}", from, to);
		iTVPStorageMedia2 *media2 = (iTVPStorageMedia2 *)(rec->MediaIntf.GetObjectNoAddRef());
		if (media2 && media2->Move(dname_from, dname_to)) {
			TVPLOG_DEBUG("Move by media's Move method succeeded: from:{} to:{}", from, to);
			return true;
		}
	}
	rec->MediaIntf.GetObjectNoAddRef()->GetLocallyAccessibleName(dname_from);
	rec->MediaIntf.GetObjectNoAddRef()->GetLocallyAccessibleName(dname_to);
	if (dname_from != "" && dname_to != "") {
		// if both files are accessible from local file system, try to move it by ourselves
		TVPLOG_DEBUG("move file: from:{} to:{}", dname_from, dname_to);
		if (TVPMoveFile(dname_from, dname_to)) {
			return true;
		}
	}
	return false;
}

tjs_uint64 tTVPStorageMediaManager::LastModifiedFileTime(const ttstr &name)
{
	tMediaRecord *rec = GetMediaRecord(name);
	ttstr dname = rec->GetDomainAndPath(name);
	if (rec->version >= 2) {
		// if media supports iTVPStorageMedia2, use LastModifiedFileTime method
		TVPLOG_DEBUG("Trying to get last modified file time by media's LastModifiedFileTime method: {}", name);
		iTVPStorageMedia2 *media2 = (iTVPStorageMedia2 *)(rec->MediaIntf.GetObjectNoAddRef());
		if (media2) {
			tjs_uint64 mod_time = media2->LastModifiedFileTime(dname);
			if (mod_time != 0) {
				TVPLOG_DEBUG("Get last modified file time by media's LastModifiedFileTime method succeeded: {}, mod_time: {}", name, mod_time);
				return mod_time;
			}
		}
	}
	rec->MediaIntf.GetObjectNoAddRef()->GetLocallyAccessibleName(dname);
	if (dname != "") {
		// if the file is accessible from local file system, try to get last modified file time by ourselves
		TVPLOG_DEBUG("Trying to get last modified file time by local file system: {}", name);
		tjs_uint64 mod_time = TVPLastModifiedFileTime(dname);
		if (mod_time != 0) {
			TVPLOG_DEBUG("Get last modified file time by local file system succeeded: {}, mod_time: {}", name, mod_time);
			return mod_time;
		}
	}
	return 0;
}

tjs_uint64 tTVPStorageMediaManager::FileSize(const ttstr &name)
{
	tMediaRecord *rec = GetMediaRecord(name);
	ttstr dname = rec->GetDomainAndPath(name);
	if (rec->version >= 2) {
		// if media supports iTVPStorageMedia2, use FileSize method
		iTVPStorageMedia2 *media2 = (iTVPStorageMedia2 *)(rec->MediaIntf.GetObjectNoAddRef());
		if (media2) {
			tjs_uint64 size = media2->FileSize(dname);
			if (size != 0) {
				return size;
			}
		}
	}
	rec->MediaIntf.GetObjectNoAddRef()->GetLocallyAccessibleName(dname);
	if (dname != "") {
		// if the file is accessible from local file system, try to get size by ourselves
		tjs_uint64 size = TVPFileSize(dname);
		if (size != 0) {
			return size;
		}
	}
	return 0;
}

//---------------------------------------------------------------------------
void TVPRegisterStorageMedia(iTVPStorageMedia *media)
{
	TVPStorageMediaManager.Register(media);
}
//---------------------------------------------------------------------------
void TVPUnregisterStorageMedia(iTVPStorageMedia *media)
{
	TVPStorageMediaManager.Unregister(media);
}
//---------------------------------------------------------------------------
void TVPRegisterStorageMedia(iTVPStorageMedia2 *media)
{
	TVPStorageMediaManager.Register(media);
}

//---------------------------------------------------------------------------
bool TVPRemoveStorage(const ttstr &name)
{
	// 削除する path について、_TVPCreateStream の WRITE 時と同じ要領で
	// file 層 / decode 層の cache から該当 entry を駆逐する。これで「ファイルを
	// 消した直後に同 path を再 load して古い decode 結果が返る」を防ぐ。
	// 失敗しても cache miss が増えるだけで害はないので、Remove 前に無条件で行う。
	ttstr norm = TVPNormalizeStorageName(name);
	TVPClearStorageCache(norm, /*force=*/true);
	TVPClearGraphicCacheEntry(norm);
	return TVPStorageMediaManager.Remove(name);
}
//---------------------------------------------------------------------------
bool TVPMoveStorage(const ttstr &from, const ttstr &to)
{
	// from は消える、to は中身が置き換わる、どちらも cache 内容が stale 化する。
	// 両 path について file 層 / decode 層の cache を駆逐する。
	ttstr norm_from = TVPNormalizeStorageName(from);
	ttstr norm_to   = TVPNormalizeStorageName(to);
	TVPClearStorageCache(norm_from, /*force=*/true);
	TVPClearGraphicCacheEntry(norm_from);
	TVPClearStorageCache(norm_to,   /*force=*/true);
	TVPClearGraphicCacheEntry(norm_to);
	return TVPStorageMediaManager.Move(from, to);
}

tjs_uint64 TVPLastModifiedFileTimeStorage(const ttstr &name)
{
	return TVPStorageMediaManager.LastModifiedFileTime(name);
}

tjs_uint64 TVPFileSizeStorage(const ttstr &name)
{
	return TVPStorageMediaManager.FileSize(name);
}

//---------------------------------------------------------------------------
// TVPNormalizeStorgeName : storage name normalization
//---------------------------------------------------------------------------
ttstr TVPNormalizeStorageName(const ttstr & _name)
	// TODO: check what is done in TVPNormalizeStorageName
{
	return TVPStorageMediaManager.NormalizeStorageName(_name);
}
//---------------------------------------------------------------------------







//---------------------------------------------------------------------------
// TVPSetCurrentDirectory
//---------------------------------------------------------------------------
void TVPSetCurrentDirectory(const ttstr & _name)
{
	TVPStorageMediaManager.SetCurrentDirectory(_name);
	TVPClearStorageCaches();
}
//---------------------------------------------------------------------------





//---------------------------------------------------------------------------
// TVPGetLocalName and TVPGetLocallyAccessibleName
//---------------------------------------------------------------------------
void TVPGetLocalName(ttstr &name)
{
	ttstr tmp = TVPGetLocallyAccessibleName(name);
	if(tmp.IsEmpty()) TVPThrowExceptionMessage(TVPCannotGetLocalName, name);
	name = tmp;
}
//---------------------------------------------------------------------------
ttstr TVPGetLocallyAccessibleName(const ttstr &name)
{
	if(TJS_strchr(name.c_str(), TVPArchiveDelimiter)) return TJS_W("");
		 // in-archive storage is always not accessible from local file system
	return TVPStorageMediaManager.GetLocallyAccessibleName(name);
}
//---------------------------------------------------------------------------


void TVPGetStorageListAt(const ttstr &name, iTVPStorageLister *lister)
{
	TVPStorageMediaManager.GetListAt(name, lister);
}

//---------------------------------------------------------------------------
// tTVPArchive
//---------------------------------------------------------------------------
void tTVPArchive::NormalizeInArchiveStorageName(ttstr & name)
{
	// normalization of in-archive storage name does :
	if(name.IsEmpty()) return;

	// make all characters small
	// change '\\' to '/'
	tjs_char *ptr = name.Independ();
	while(*ptr)
	{
		if(*ptr >= TJS_W('A') && *ptr <= TJS_W('Z'))
			*ptr += TJS_W('a') - TJS_W('A');
		else if(*ptr == TJS_W('\\'))
			*ptr = TJS_W('/');
		ptr++;
	}

	// eliminate duplicated slashes
	ptr = name.Independ();
	tjs_char *org_ptr = ptr;
	tjs_char *dest = ptr;
	while(*ptr)
	{
		if(*ptr != TJS_W('/'))
		{
			*dest = *ptr;
			ptr ++;
			dest ++;
		}
		else
		{
			if(ptr != org_ptr)
			{
				*dest = *ptr;
				ptr ++;
				dest ++;
			}
			while(*ptr == TJS_W('/')) ptr++;
		}
	}
	*dest = 0;

	name.FixLen();
}
//---------------------------------------------------------------------------
void tTVPArchive::AddToHash()
{
	// enter all names to the hash table
	tjs_uint Count = GetCount();
	tjs_uint i;
	for(i = 0; i < Count; i++)
	{
		ttstr name = GetName(i);
		NormalizeInArchiveStorageName(name);
		Hash.Add(name, i);
	}
}
//---------------------------------------------------------------------------
iTJSBinaryStream * tTVPArchive::CreateStream(const ttstr & name)
{
	if(name.IsEmpty()) return NULL;

	if(!Init)
	{
		Init = true;
		AddToHash();
	}

	tjs_uint *p = Hash.Find(name);
	if(!p) TVPThrowExceptionMessage(TVPStorageInArchiveNotFound,
		name, ArchiveName);

	return CreateStreamByIndex(*p);
}
//---------------------------------------------------------------------------
bool tTVPArchive::IsExistent(const ttstr & name)
{
	if(name.IsEmpty()) return false;

	if(!Init)
	{
		Init = true;
		AddToHash();
	}

	return Hash.Find(name) != NULL;
}
//---------------------------------------------------------------------------
tjs_int tTVPArchive::GetFirstIndexStartsWith(const ttstr & prefix)
{
	// returns first index which have 'prefix' at start of the name.
	// returns -1 if the target is not found.
	// the item must be sorted by ttstr::operator < , otherwise this function
	// will not work propertly.
	tjs_uint total_count = GetCount();
	tjs_int s = 0, e = total_count;
	while(e - s > 1)
	{
		tjs_int m = (e + s) / 2;
		if(!(GetName(m) < prefix))
		{
			// m is after or at the target
			e = m;
		}
		else
		{
			// m is before the target
			s = m;
		}
	}

	// at this point, s or s+1 should point the target.
	// be certain.
	if(s >= (tjs_int)total_count) return -1; // out of the index
	if(GetName(s).StartsWith(prefix)) return s;
	s++;
	if(s >= (tjs_int)total_count) return -1; // out of the index
	if(GetName(s).StartsWith(prefix)) return s;
	return -1;
}
//---------------------------------------------------------------------------





//---------------------------------------------------------------------------
// tTVPArchiveCache
//---------------------------------------------------------------------------
class tTVPArchiveCache
{
	typedef tTJSRefHolder<tTVPArchive> tHolder;
	tTJSHashCache<ttstr, tHolder> ArchiveCache;
	tTJSCriticalSection CS;


public:
	tTVPArchiveCache() : ArchiveCache(TVP_DEFAULT_ARCHIVE_CACHE_NUM)
	{
	}

	~tTVPArchiveCache()
	{
	}

	void SetMaxCount(tjs_int maxcount)
	{
		ArchiveCache.SetMaxCount(maxcount);
	}

	void Clear()
	{
		// releases all elements
		ArchiveCache.Clear();
	}

	tTVPArchive * Get(ttstr name)
	{
		name = TVPNormalizeStorageName(name);
		tTJSCSH csh(CS);
		tjs_uint32 hash = tTJSHashCache<ttstr, tHolder>::MakeHash(name);
		tHolder *ptr = ArchiveCache.FindAndTouchWithHash(name, hash);
		if(ptr)
		{
			// exist in the cache
			return ptr->GetObject();
		}

		if(!TVPIsExistentStorageNoSearch(name))
		{
			// storage not found
			TVPThrowExceptionMessage(TVPCannotFindStorage, name);
		}

		// not exist in the cache
		tTVPArchive *arc = TVPOpenArchive(name);
		tHolder holder(arc);
		ArchiveCache.AddWithHash(name, hash, holder);
		return arc;
	}

private:

} TVPArchiveCache;
static void TVPClearArchiveCache() { TVPArchiveCache.Clear(); }
static tTVPAtExit TVPClearArchiveCacheAtExit
	(TVP_ATEXIT_PRI_SHUTDOWN, TVPClearArchiveCache);
//---------------------------------------------------------------------------







//---------------------------------------------------------------------------
// TVPIsExistentStorageNoSearch
//---------------------------------------------------------------------------
bool TVPIsExistentStorageNoSearchNoNormalize(const ttstr &name)
{
	// does name contain > ?
	tTJSCriticalSectionHolder cs_holder(TVPCreateStreamCS);

	const tjs_char * sharp_pos = TJS_strchr(name.c_str(), TVPArchiveDelimiter);
	if(sharp_pos)
	{
		// this storagename indicates a file in an archive

		ttstr arcname(name, (int)(sharp_pos - name.c_str()));

		tTVPArchive *arc;
		arc = TVPArchiveCache.Get(arcname);
		bool ret;
		try
		{
			ttstr in_arc_name(sharp_pos + 1);
			tTVPArchive::NormalizeInArchiveStorageName(in_arc_name);
			ret = arc->IsExistent(in_arc_name);
		}
		catch(...)
		{
			arc->Release();
			throw;
		}
		arc->Release();
		return ret;
	}

	return TVPStorageMediaManager.CheckExistentStorage(name);
}
//---------------------------------------------------------------------------
bool TVPIsExistentStorageNoSearch(const ttstr &_name)
{
	return TVPIsExistentStorageNoSearchNoNormalize(TVPNormalizeStorageName(_name));
}
//---------------------------------------------------------------------------





//---------------------------------------------------------------------------
// TVPExtractStorageExt
//---------------------------------------------------------------------------
ttstr TVPExtractStorageExt(const ttstr & name)
{
	// extract an extension from name.
	// returned string will contain extension delimiter ( '.' ), except for
	// missing extension of the input string.
	// ( returns null string when input string does not have an extension )

	const tjs_char * s = name.c_str();
	tjs_int slen = name.GetLen();
	const tjs_char * p = s + slen;
	p--;
	while(p>=s)
	{
		if(*p == TJS_W('\\')) break;
		if(*p == TJS_W('/')) break;
		if(*p == TVPArchiveDelimiter) break;
		if(*p == TJS_W('.'))
		{
			// found extension delimiter
			tjs_int extlen = (tjs_int)(slen - ( p - s ));
			return ttstr(p, extlen);
		}

		p--;
	}

	// not found
	return ttstr();
}
//---------------------------------------------------------------------------



//---------------------------------------------------------------------------
// TVPExtractStorageName
//---------------------------------------------------------------------------
ttstr TVPExtractStorageName(const ttstr & name)
{
	// extract "name"'s storage name ( excluding path ) and return it.
	const tjs_char * s = name.c_str();
	tjs_int slen = name.GetLen();
	const tjs_char * p = s + slen;
	p--;
	while(p>=s)
	{
		if(*p == TJS_W('\\')) break;
		if(*p == TJS_W('/')) break;
		if(*p == TVPArchiveDelimiter) break;

		p--;
	}

	p++;
	if(p == s)
		return name;
	else
		return ttstr(p, (int)(slen - (p -s)));
}
//---------------------------------------------------------------------------




//---------------------------------------------------------------------------
// TVPExtractStoragePath
//---------------------------------------------------------------------------
ttstr TVPExtractStoragePath(const ttstr & name)
{
	// extract "name"'s path ( including last delimiter ) and return it.
	const tjs_char * s = name.c_str();
	tjs_int slen = name.GetLen();
	const tjs_char * p = s + slen;
	p--;
	while(p>=s)
	{
		if(*p == TJS_W('\\')) break;
		if(*p == TJS_W('/')) break;
		if(*p == TVPArchiveDelimiter) break;

		p--;
	}

	p++;
	return ttstr(s, (int)(p-s));
}
//---------------------------------------------------------------------------




//---------------------------------------------------------------------------
// TVPChopStorageExt
//---------------------------------------------------------------------------
extern ttstr TVPChopStorageExt(const ttstr & name)
{
	// chop storage's extension and return it.
	const tjs_char * s = name.c_str();
	tjs_int slen = name.GetLen();
	const tjs_char * p = s + slen;
	p--;
	while(p>=s)
	{
		if(*p == TJS_W('\\')) break;
		if(*p == TJS_W('/')) break;
		if(*p == TVPArchiveDelimiter) break;
		if(*p == TJS_W('.'))
		{
			// found extension delimiter
			return ttstr(s, (int)(p-s));
		}

		p--;
	}

	// not found
	return name;
}
//---------------------------------------------------------------------------





//---------------------------------------------------------------------------
// Auto search path support
//---------------------------------------------------------------------------
struct tTVPFileInfo
{
	static const tjs_int EXIST_PROP = 0x01;
	static const tjs_int EMPTY_FILE = 0x02;

	ttstr FilePath;
	iTJSDispatch2* Property = nullptr;
	tjs_int Flag = 0;

	tTVPFileInfo( const ttstr& path, tjs_int exist )
		: FilePath( path ), Flag( exist ) {
	}
	tTVPFileInfo( const ttstr& path, iTJSDispatch2* prop = nullptr )
	: FilePath(path), Property(prop)
	{
		if( Property ) Property->AddRef();
	}
	tTVPFileInfo( const tTVPFileInfo& info )
	: FilePath(info.FilePath), Property(info.Property), Flag(info.Flag )
	{
		if( Property ) Property->AddRef();
	}
	~tTVPFileInfo()
	{
		if( Property ) Property->Release();
	}
	tTVPFileInfo &operator=(const tTVPFileInfo &rhs)
	{
		if (this != &rhs) {
			if( Property ) Property->Release();
			FilePath = rhs.FilePath;
			Property = rhs.Property;
			Flag = rhs.Flag;
			if( Property )
			{
				Property->AddRef();
			}
		}
		return *this;
	}
	bool ExistProp() const {
		return (Flag & EXIST_PROP) != 0; 
	}
	bool ExistFile() const {
		return ( Flag & EMPTY_FILE ) == 0;
	}
};
#define TVP_AUTO_PATH_HASH_SIZE 1024
std::vector<ttstr> TVPAutoPathList;
tTJSHashCache<ttstr, ttstr> TVPAutoPathCache(TVP_DEFAULT_AUTOPATH_CACHE_NUM);
tTJSHashTable<ttstr, tTVPFileInfo, tTJSHashFunc<ttstr>, TVP_AUTO_PATH_HASH_SIZE>
	TVPAutoPathTable;
bool AutoPathTableInit = false;
//---------------------------------------------------------------------------

void TVPAddAutoPathTable(const ttstr & name, const tTVPFileInfo &info)
{
#ifdef TVP_AUTOPATH_IGNORECASE
	TVPAutoPathTable.Add(name.AsLowerCase(), info);
#else
	TVPAutoPathTable.Add(name, info);
#endif
}

tTVPFileInfo *TVPFindAutoPathTable(const ttstr &name)
{
#ifdef TVP_AUTOPATH_IGNORECASE
	return TVPAutoPathTable.Find(name.AsLowerCase());
#else
	return TVPAutoPathTable.Find(name);
#endif
}


// ttstr(tTJSVariantString) の RefCount は非atomicで、文字列ヒープのセル確保/解放は
// ロックされていても RefCount の増減自体は保護されない。AutoPath キャッシュは
// キャッシュ系ワーカースレッドからも参照されるため、COW バッファを共有したまま
// 別スレッドへ渡す/別スレッドが触るキャッシュへ格納すると、同一 VS の RefCount を
// 複数スレッドが同時に増減して二重解放→メモリ例外を起こす。
// c_str() から作り直して独立した VS を持たせ、スレッド間でバッファを共有させない。
static inline ttstr TVPMakeIndependentString(const ttstr & s)
{
	if(s.IsEmpty()) return ttstr();
	return ttstr(s.c_str());
}

void TVPClearAutoPathCacheFile(const ttstr & name)
{
	tTJSCriticalSectionHolder cs_holder(TVPCreateStreamCS);
	// キャッシュから情報を消して再検索されるようにする
	TVPAutoPathCache.Delete(name);
}

void TVPAddAutoPathCacheFile(const ttstr & name)
{
	tTJSCriticalSectionHolder cs_holder(TVPCreateStreamCS);
	// キャッシュから情報を消して再検索されるようにする
	// キー/値ともに独立した VS を格納し、呼び出し元(別スレッド)の文字列と
	// バッファを共有させない。
	TVPAutoPathCache.Add(TVPMakeIndependentString(name),
		TVPMakeIndependentString(name));
}

//---------------------------------------------------------------------------
static void TVPClearAutoPathCache()
{
	TVPAutoPathCache.Clear();
	TVPAutoPathTable.Clear();
	AutoPathTableInit = false;
}
//---------------------------------------------------------------------------
struct tTVPClearAutoPathCacheCallback : public tTVPCompactEventCallbackIntf
{
	virtual void TJS_INTF_METHOD OnCompact(tjs_int level)
	{
		if(level >= TVP_COMPACT_LEVEL_DEACTIVATE)
		{
			// clear the auto search path cache on application deactivate
			tTJSCriticalSectionHolder cs_holder(TVPCreateStreamCS);
			TVPClearAutoPathCache();
		}
	}
} static TVPClearAutoPathCacheCallback;
//---------------------------------------------------------------------------
void TVPAddAutoPath(const ttstr & name)
{
	tTJSCriticalSectionHolder cs_holder(TVPCreateStreamCS);

	tjs_char lastchar = name.GetLastChar();
	if(lastchar != TVPArchiveDelimiter && lastchar != TJS_W('/') && lastchar != TJS_W('\\'))
		TVPThrowExceptionMessage(TVPMissingPathDelimiterAtLast);

	ttstr normalized = TVPNormalizeStorageName(name);

	std::vector<ttstr>::iterator i =
		std::find(TVPAutoPathList.begin(), TVPAutoPathList.end(), normalized);
	if(i == TVPAutoPathList.end())
		TVPAutoPathList.push_back(normalized);

	TVPClearAutoPathCache();
}
//---------------------------------------------------------------------------
void TVPRemoveAutoPath(const ttstr &name)
{
	tTJSCriticalSectionHolder cs_holder(TVPCreateStreamCS);

	tjs_char lastchar = name.GetLastChar();
	if(lastchar != TVPArchiveDelimiter && lastchar != TJS_W('/') && lastchar != TJS_W('\\'))
		TVPThrowExceptionMessage(TVPMissingPathDelimiterAtLast);

	ttstr normalized = TVPNormalizeStorageName(name);

	std::vector<ttstr>::iterator i =
		std::find(TVPAutoPathList.begin(), TVPAutoPathList.end(), normalized);
	if(i != TVPAutoPathList.end())
		TVPAutoPathList.erase(i);

	TVPClearAutoPathCache();
}
//---------------------------------------------------------------------------
static tjs_uint TVPRebuildAutoPathTable()
{
	// rebuild auto path table
	TVPInitStorageOptions();

	tTJSCriticalSectionHolder cs_holder(TVPCreateStreamCS);
	if(AutoPathTableInit) return 0;

	TVPAutoPathTable.Clear();

	tjs_uint64 tick = TVPGetTickCount();
 	TVPAddLog( (const tjs_char*)TVPInfoRebuildingAutoPath );

	tjs_uint totalcount = 0;

	std::vector<ttstr>::iterator it;
	for(it = TVPAutoPathList.begin(); it != TVPAutoPathList.end(); it++)
	{
		const ttstr & path = *it;
		tjs_uint count = 0;

		const tjs_char * sharp_pos = TJS_strchr(path.c_str(), TVPArchiveDelimiter);
		if(sharp_pos)
		{
			// this storagename indicates a file in an archive

			ttstr arcname(path, (int)(sharp_pos - path.c_str()));
			ttstr in_arc_name(sharp_pos + 1);
			tTVPArchive::NormalizeInArchiveStorageName(in_arc_name);
			tjs_int in_arc_name_len = in_arc_name.GetLen();

			tTVPArchive *arc;
			arc = TVPArchiveCache.Get(arcname);

			try
			{
				tjs_uint storagecount = arc->GetCount();

				// get first index which the item has 'in_arc_name' as its start
				// of the string.
				tjs_int i = arc->GetFirstIndexStartsWith(in_arc_name);
				if(i != -1)
				{
					for(; i < (tjs_int)storagecount; i++)
					{
						ttstr name = arc->GetName(i);

						if(name.StartsWith(in_arc_name))
						{
							if(!TJS_strchr(name.c_str() + in_arc_name_len, TJS_W('/')))
							{
								ttstr sname = TVPExtractStorageName(name);
								// TODO アーカイブの時もプロパティ情報追加
								TVPAddAutoPathTable(sname, tTVPFileInfo(path + sname) );
								count ++;
							}
						}
						else
						{
							// no need to check more;
							// because the list is sorted by the name.
							break;
						}
					}
				}
			}
			catch(...)
			{
				arc->Release();
				throw;
			}
			arc->Release();
		}
		else
		{
			// normal folder
			class tLister : public iTVPStorageLister
			{
				const ttstr EXT;
			public:
				tLister() : EXT(TJS_W(".prop")) {}
				std::set<ttstr>		list;
				std::vector<ttstr>	prop;
				void TJS_INTF_METHOD Add(const ttstr &file)
				{
					ttstr ext = TVPExtractStorageExt( file );
					if( ext == EXT )
					{
						prop.push_back( file );
					}
					list.insert( file );
				}
			} lister;
			TVPStorageMediaManager.GetListAt(path, &lister);

			if( !TVPIgnoreFileProperty )
			{
				// プロパティがあるファイルを追加する
				for( auto i = lister.prop.begin(); i != lister.prop.end(); i++ ) {
					// プロパティがある場合はとりあえず登録だけしておき、プロパティ取得時に実際に読み込みを行う
					ttstr fname = TVPChopStorageExt( *i );
					auto file = lister.list.find( fname );
					if( file != lister.list.end() ) {
						// ファイルがある場合
						lister.list.erase( *file );
						TVPAddAutoPathTable( *file, tTVPFileInfo( path + *file, tTVPFileInfo::EXIST_PROP ) );
					} else {
						TVPAddAutoPathTable( fname.AsLowerCase(), tTVPFileInfo( path + fname, tTVPFileInfo::EXIST_PROP | tTVPFileInfo::EMPTY_FILE ) );
					}
				}
			}
			// プロパティのないファイルを追加する
			for( auto i = lister.list.begin(); i != lister.list.end(); i++)
			{
				TVPAddAutoPathTable(*i, tTVPFileInfo(path + *i) );
				count ++;
			}
		}

//		TVPAddLog(ttstr(TJS_W("(info) Path ")) + path + TJS_W(" contains ") +
//			ttstr((tjs_int)count) + TJS_W(" file(s)."));

		totalcount += count;
	}

	tjs_uint64 endtick = TVPGetTickCount();

	TVPAddLog(ttstr(TJS_W("(info) Total ")) +
			ttstr((tjs_int)totalcount) + TJS_W(" file(s) found, ") +
			ttstr((tjs_int)TVPAutoPathTable.GetCount()) + TJS_W(" file(s) activated.") + 
			TJS_W(" (") + ttstr((tjs_int)(endtick - tick)) + TJS_W("ms)"));

	AutoPathTableInit = true;

	return totalcount;
}
//---------------------------------------------------------------------------




//---------------------------------------------------------------------------
// TVPGetPlacedPath
//---------------------------------------------------------------------------
ttstr TVPGetPlacedPath(const ttstr & name)
{
	try
	{
		tTJSCriticalSectionHolder cs_holder(TVPCreateStreamCS);

#ifndef TVP_DONT_CLEAR_AUTOPATH_CACHE
	// search path and return the path which the "name" is placed.
	// returned name is normalized. returns empty string if the storage is not
	// found.
	//
	// TVPGetPlacedPath は TVPCreateStream / TVPClearStorageCache / cache thread
	// 等から任意スレッドで呼ばれる。bool フラグだけでは初回呼び出しが同時に
	// 走った場合に二重登録される (= compact 時に AutoPath cache が 2 回 clear
	// される) ため、std::call_once で塞ぐ。
	{
		static std::once_flag flag;
		std::call_once(flag, []{
			TVPAddCompactEventHook(&TVPClearAutoPathCacheCallback);
		});
	}
#endif

		ttstr * incache = TVPAutoPathCache.FindAndTouch(name);
		if(incache) return TVPMakeIndependentString(*incache); // found in cache

		ttstr normalized(TVPNormalizeStorageName(name));

		bool found = TVPIsExistentStorageNoSearchNoNormalize(normalized);
		if(found)
		{
			// found in current folder
			// キャッシュへは独立 VS を格納し、返り値も独立 VS にして
			// スレッド間でバッファを共有させない。
			TVPAutoPathCache.Add(TVPMakeIndependentString(name),
				TVPMakeIndependentString(normalized));
			return TVPMakeIndependentString(normalized);
		}

		// not found in current folder
		// search through auto path table

		ttstr storagename = TVPExtractStorageName(normalized);

		TVPRebuildAutoPathTable(); // ensure auto path table
		tTVPFileInfo *result = TVPFindAutoPathTable(storagename);
		if(result && (result->Flag & tTVPFileInfo::EMPTY_FILE) == 0 )
		{
			// found in table
			TVPAutoPathCache.Add(TVPMakeIndependentString(name),
				TVPMakeIndependentString(result->FilePath));
			return TVPMakeIndependentString(result->FilePath);
		}

		// not found
		TVPAutoPathCache.Add(TVPMakeIndependentString(name), ttstr());
		return ttstr();
	}
	catch(const std::bad_alloc &)
	{
		static std::atomic<bool> warned(false);
		if(!warned.exchange(true, std::memory_order_relaxed)) {
			TVPAddImportantLog(TJS_W("(warn) TVPGetPlacedPath: memory allocation failed (further warnings suppressed)"));
		}
		return ttstr();
	}
}
//---------------------------------------------------------------------------




//---------------------------------------------------------------------------
/**
 * TVPGetPlacedPath
 * @param name file name
 * @param extlist extension list(delimiter |) ex. ".bmp|.png|.jpg"
 * @return normalized path.
 */
ttstr TVPGetPlacedPath(const ttstr & name, const ttstr& extlist )
{
	tjs_string exts = extlist.AsStdString();
	std::vector<tjs_string> ext;
	split( exts, tjs_string( TJS_W( "|" ) ), ext );
	ttstr filename = TVPChopStorageExt( name );
	for( auto i = ext.begin(); i != ext.end(); i++ ) {
		if( !((*i).empty()) ) {
			ttstr fullname = filename + ttstr( *i );
			ttstr ret = TVPGetPlacedPath( fullname );
			if( !ret.IsEmpty() ) {
				return ret;
			}
		}
	}
	return ttstr();
}
//---------------------------------------------------------------------------



//---------------------------------------------------------------------------
// TVPSearchPlacedPath
//---------------------------------------------------------------------------
ttstr TVPSearchPlacedPath(const ttstr & name)
{
	ttstr place = TVPGetPlacedPath(name);
	if(place.IsEmpty()) TVPThrowExceptionMessage(TVPCannotFindStorage, name);
	return place;
}
//---------------------------------------------------------------------------




//---------------------------------------------------------------------------
// TVPIsExistentStorage
//---------------------------------------------------------------------------
bool TVPIsExistentStorage(const ttstr &name)
{
	return !TVPGetPlacedPath(name).IsEmpty();
}
//---------------------------------------------------------------------------



//---------------------------------------------------------------------------
// TVPGetFilePropertyNoAddRef
//---------------------------------------------------------------------------
iTJSDispatch2* TVPGetFilePropertyNoAddRef( const ttstr& name )
{
	TVPRebuildAutoPathTable(); // ensure auto path table
	tTJSCriticalSectionHolder cs_holder(TVPCreateStreamCS);
	tTVPFileInfo *result = TVPAutoPathTable.Find( name );
	if( result && ( result->Flag & tTVPFileInfo::EXIST_PROP) ) {
		// found in table
		if( !result->Property ) {
			ttstr path = result->FilePath + ".prop";
			tTJSVariant dic;
			ttstr mode;
			if( TJSReadDictionaryObject( dic, path, mode ) == TJS_S_OK ) {
				result->Property = dic.AsObject();
			}
		}
		return result->Property;
	}
	return nullptr;
}

// 内部呼び出し用
// name は正規化済み
iTJSBinaryStream * _InnerTVPCreateStream(const ttstr &name, tjs_uint32 flags)
{
	tjs_uint32 access = flags & TJS_BS_ACCESS_MASK;

	// does name contain > ?
	const tjs_char * sharp_pos = TJS_strchr(name.c_str(), TVPArchiveDelimiter);
	if(sharp_pos)
	{
		// this storagename indicates a file in an archive
		if((flags & TJS_BS_ACCESS_MASK ) !=TJS_BS_READ)
			TVPThrowExceptionMessage(TVPCannotWriteToArchive);

		ttstr arcname(name, (int)(sharp_pos - name.c_str()));

		tTVPArchive *arc;
		iTJSBinaryStream *stream;
		arc = TVPArchiveCache.Get(arcname);
		try
		{
			ttstr in_arc_name(sharp_pos + 1);
			tTVPArchive::NormalizeInArchiveStorageName(in_arc_name);
			stream = arc->CreateStream(in_arc_name);
		}
		catch(...)
		{
			arc->Release();
#ifndef TVP_DONT_CLEAR_AUTOPATH_CACHE
			if(access >= 1) TVPClearStorageCaches();
#else
			if(access >= 1) TVPClearXP3SegmentCache();
#endif
			throw;
		}
#ifndef TVP_DONT_CLEAR_AUTOPATH_CACHE
		if(access >= 1) TVPClearStorageCaches();
#else
		if(access >= 1) TVPClearXP3SegmentCache();
#endif
		arc->Release();
		return stream;
	}

	iTJSBinaryStream *stream;
	try
	{
		stream = TVPStorageMediaManager.Open(name, flags);
	}
	catch(...)
	{
#ifndef TVP_DONT_CLEAR_AUTOPATH_CACHE
		if(access >= 1) TVPClearStorageCaches();
#else
		if(access >= 1) TVPClearXP3SegmentCache();
#endif
		throw;
	}
#ifndef TVP_DONT_CLEAR_AUTOPATH_CACHE
	if(access >= 1) TVPClearStorageCaches();
#else
	if(access >= 1) {
		// キャッシュから情報を消して再検索されるようにする
		{
			tTJSCriticalSectionHolder cs_holder(TVPCreateStreamCS);
			TVPAutoPathCache.Delete(name);
		}
		// XXX こっちはいる？
		TVPClearXP3SegmentCache();
	}
#endif
	return stream;
}

//---------------------------------------------------------------------------
// TVPCreateStream
//---------------------------------------------------------------------------
static iTJSBinaryStream * _TVPCreateStream(const ttstr & _name, tjs_uint32 flags)
{
	tTJSCriticalSectionHolder cs_holder(TVPCreateStreamCS);

	tjs_uint32 access = flags & TJS_BS_ACCESS_MASK;

	ttstr name;

	if(access == TJS_BS_WRITE)
		name = TVPNormalizeStorageName(_name);
	else
		name = TVPGetPlacedPath(_name); // file must exist

	if(name.IsEmpty()) TVPThrowExceptionMessage(TVPCannotOpenStorage, _name);

	// Read 以外 (WRITE/APPEND/UPDATE) で開く path は中身が変わる可能性があるので
	// 両層 cache から該当 path を駆逐する。これで「ファイルを書き換えた直後に
	// 同 path を再 load して古い decode 結果が返る」を防ぐ。各 SaveHandler や
	// 独自書き込み経路ごとに TVPClearGraphicCache 等を呼んで予防していたのを
	// ここに集約。
	// stale 化が目的なので force=true で表から強制削除 (外部保持中の stream は
	// 旧 buffer を抱えたまま残るが、次の reader 用に表エントリは無効化)。
	if(access != TJS_BS_READ) {
		TVPClearStorageCache(name, /*force=*/true);
		TVPClearGraphicCacheEntry(name);
	}

	if (access == TJS_BS_READ && TVPIsCacheTargetFile(name)) {

		// 拡張子を切り出す
		ttstr ext = TVPExtractStorageExt(name);

		TVPLOG_DEBUG("_TVPCreateStream: {} ext:{}", name, ext);
		iTJSBinaryStream *stream = TVPGetStorageCache(name, TVPIsCacheTargetExtension(ext));
		if (stream) {
			return stream;
		}
		TVPClearStorageCache(name);
	}

	return _InnerTVPCreateStream(name, flags);
}

iTJSBinaryStream * TVPCreateStream(const ttstr & _name, tjs_uint32 flags)
{
	try
	{
		return _TVPCreateStream(_name, flags);
	}
	catch(eTJSScriptException &e)
	{
		if(TJS_strchr(_name.c_str(), '#'))
			e.AppendMessage(TJS_W("[") +
				TVPFormatMessage(TVPFilenameContainsSharpWarn, _name) + TJS_W("]"));
		throw e;
	}
	catch(eTJSScriptError &e)
	{
		if(TJS_strchr(_name.c_str(), '#'))
			e.AppendMessage(TJS_W("[") +
				TVPFormatMessage(TVPFilenameContainsSharpWarn, _name) + TJS_W("]"));
		throw e;
	}
	catch(eTJSError &e)
	{
		if(TJS_strchr(_name.c_str(), '#'))
			e.AppendMessage(TJS_W("[") +
				TVPFormatMessage(TVPFilenameContainsSharpWarn, _name) + TJS_W("]"));
		throw e;
	}
	catch(...)
	{
		// check whether the filename contains '#' (former delimiter for archive
		// filename before 2.19 beta 14)
		if(TJS_strchr(_name.c_str(), '#'))
			TVPAddLog(TVPFormatMessage(TVPFilenameContainsSharpWarn, _name));
		throw;
	}
}
//---------------------------------------------------------------------------

std::shared_ptr<uint8_t> TVPReadStream(const tjs_char *name, tjs_uint64 *flen)
{
	std::unique_ptr<iTJSBinaryStream> stream(::TVPCreateStream(name, TJS_BS_READ));
	if (stream) {
		size_t size = stream->GetSize();
		if (size > 0) {
			uint8_t* data = new uint8_t[size + 2];
			size_t s = size;
			uint8_t *p = data;
			while (s > 0) {
				size_t read = stream->Read(p, s);
				if (read == 0) {
					break;
				}
				s -= read;
				p += read;
			}
			// 文字列として参照できるように末尾に0設定しておく
			data[size] = '\0';
			data[size+1] = '\0';
			// 正規のサイズを返す
			if (flen) {
				*flen = size;
			}
			return std::shared_ptr<uint8_t>(data, std::default_delete<uint8_t[]>());
		}
	}
    return 0;
}

#include <sstream>
static std::vector<std::string>* 
ReadLinesFromString(const char *str)
{
	if (str) {
		std::vector<std::string>* ret = new std::vector<std::string>();
		if (ret) {
			std::istringstream istr(str);	
			std::string line;
			while (std::getline(istr, line)){
				ret->push_back(line);
			}
			return ret;
		}
	}
	return nullptr;
}

std::vector<std::string> *TVPReadLines(const tjs_char *name)
{
	if (TVPIsExistentStorage(name)) {
		std::unique_ptr<iTJSBinaryStream> stream(::TVPCreateStream(name, TJS_BS_READ));
		if (stream) {
			size_t size = stream->GetSize();
			if (size > 0) {
				std::unique_ptr<char[]> tmp(new char[size+2]);
				char *ptr = tmp.get();
				if (ptr) {
					stream->Read(ptr, size);
					// 文字列として参照できるように末尾に0設定しておく
					ptr[size] = '\0';
					ptr[size+1] = '\0';
					return ReadLinesFromString(ptr);
				}
			}
		}
	}
	return 0;
}


//---------------------------------------------------------------------------
// TVPClearStorageCaches
//---------------------------------------------------------------------------
void TVPClearStorageCaches()
{
	// clear all storage related caches
	TVPClearXP3SegmentCache();
	{
		tTJSCriticalSectionHolder cs_holder(TVPCreateStreamCS);
		TVPClearAutoPathCache();
	}
}
//---------------------------------------------------------------------------




//---------------------------------------------------------------------------
// tTJSNC_Storages
//---------------------------------------------------------------------------
tjs_uint32 tTJSNC_Storages::ClassID = -1;
tTJSNC_Storages::tTJSNC_Storages() : inherited(TJS_W("Storages"))
{
	// registration of native members

	TJS_BEGIN_NATIVE_MEMBERS(Storages)
	TJS_DECL_EMPTY_FINALIZE_METHOD
//----------------------------------------------------------------------

//-- methods

//----------------------------------------------------------------------
TJS_BEGIN_NATIVE_METHOD_DECL(/*func. name*/addAutoPath)
{
	if(numparams < 1) return TJS_E_BADPARAMCOUNT;

	ttstr path = *param[0];

	TVPAddAutoPath(path);

	if(result) result->Clear();

	return TJS_S_OK;
}
TJS_END_NATIVE_STATIC_METHOD_DECL(/*func. name*/addAutoPath)
//----------------------------------------------------------------------
TJS_BEGIN_NATIVE_METHOD_DECL(/*func. name*/removeAutoPath)
{
	if(numparams < 1) return TJS_E_BADPARAMCOUNT;

	ttstr path = *param[0];

	TVPRemoveAutoPath(path);

	if(result) result->Clear();

	return TJS_S_OK;
}
TJS_END_NATIVE_STATIC_METHOD_DECL(/*func. name*/removeAutoPath)
//----------------------------------------------------------------------
TJS_BEGIN_NATIVE_METHOD_DECL(/*func. name*/getFullPath)
{
	if(numparams < 1) return TJS_E_BADPARAMCOUNT;

	ttstr path = *param[0];

	if(result)
		*result = TVPNormalizeStorageName(path);

	return TJS_S_OK;
}
TJS_END_NATIVE_STATIC_METHOD_DECL(/*func. name*/getFullPath)
//----------------------------------------------------------------------
TJS_BEGIN_NATIVE_METHOD_DECL(/*func. name*/getPlacedPath)
{
	if(numparams < 1) return TJS_E_BADPARAMCOUNT;

	ttstr path = *param[0];
	if(result)
	{
		if( numparams >= 2 )
		{
			ttstr ext = *param[1];
			*result = TVPGetPlacedPath( path, ext );
		}
		else
		{
			*result = TVPGetPlacedPath( path );
		}
	}

	return TJS_S_OK;
}
TJS_END_NATIVE_STATIC_METHOD_DECL(/*func. name*/getPlacedPath)
//----------------------------------------------------------------------
TJS_BEGIN_NATIVE_METHOD_DECL(/*func. name*/isExistentStorage)
{
	if(numparams < 1) return TJS_E_BADPARAMCOUNT;

	ttstr path = *param[0];

	if(result)
		*result = (tjs_int)TVPIsExistentStorage(path);

	return TJS_S_OK;
}
TJS_END_NATIVE_STATIC_METHOD_DECL(/*func. name*/isExistentStorage)
//----------------------------------------------------------------------
TJS_BEGIN_NATIVE_METHOD_DECL(/*func. name*/extractStorageExt)
{
	if(numparams < 1) return TJS_E_BADPARAMCOUNT;

	ttstr path = *param[0];

	if(result)
		*result = TVPExtractStorageExt(path);

	return TJS_S_OK;
}
TJS_END_NATIVE_STATIC_METHOD_DECL(/*func. name*/extractStorageExt)
//----------------------------------------------------------------------
TJS_BEGIN_NATIVE_METHOD_DECL(/*func. name*/extractStorageName)
{
	if(numparams < 1) return TJS_E_BADPARAMCOUNT;

	ttstr path = *param[0];

	if(result)
		*result = TVPExtractStorageName(path);

	return TJS_S_OK;
}
TJS_END_NATIVE_STATIC_METHOD_DECL(/*func. name*/extractStorageName)
//----------------------------------------------------------------------
TJS_BEGIN_NATIVE_METHOD_DECL(/*func. name*/extractStoragePath)
{
	if(numparams < 1) return TJS_E_BADPARAMCOUNT;

	ttstr path = *param[0];

	if(result)
		*result = TVPExtractStoragePath(path);

	return TJS_S_OK;
}
TJS_END_NATIVE_STATIC_METHOD_DECL(/*func. name*/extractStoragePath)
//----------------------------------------------------------------------
TJS_BEGIN_NATIVE_METHOD_DECL(/*func. name*/chopStorageExt)
{
	if(numparams < 1) return TJS_E_BADPARAMCOUNT;

	ttstr path = *param[0];

	if(result)
		*result = TVPChopStorageExt(path);

	return TJS_S_OK;
}
TJS_END_NATIVE_STATIC_METHOD_DECL(/*func. name*/chopStorageExt)
//----------------------------------------------------------------------
TJS_BEGIN_NATIVE_METHOD_DECL(/*func. name*/clearArchiveCache)
{
	TVPClearArchiveCache();
	return TJS_S_OK;
}
TJS_END_NATIVE_STATIC_METHOD_DECL(/*func. name*/clearArchiveCache)
//----------------------------------------------------------------------
TJS_BEGIN_NATIVE_METHOD_DECL(/*func. name*/getFileProperty) {
	if( numparams < 1 ) return TJS_E_BADPARAMCOUNT;

	ttstr path = *param[0];
	if( result ) {
		iTJSDispatch2* dic = TVPGetFilePropertyNoAddRef( path );
		*result = tTJSVariant( dic, dic );
	}
	return TJS_S_OK;
}
TJS_END_NATIVE_STATIC_METHOD_DECL(/*func. name*/getFileProperty )
//----------------------------------------------------------------------
TJS_BEGIN_NATIVE_METHOD_DECL(/*func. name*/addCacheTargetExtension) {
	if( numparams < 1 ) return TJS_E_BADPARAMCOUNT;
	ttstr ext = *param[0];
	if( ext.IsEmpty() ) return TJS_E_INVALIDPARAM;
	tjs_uint64 minSize = 0;
	if( numparams >= 2 && param[1]->Type() != tvtVoid ) {
		tjs_int64 v = (tjs_int64)*param[1];
		if (v > 0) minSize = (tjs_uint64)v;
	}
	TVPAddCacheTargetExtension(ext, minSize);
	return TJS_S_OK;
}
TJS_END_NATIVE_STATIC_METHOD_DECL(/*func. name*/addCacheTargetExtension )
//----------------------------------------------------------------------
TJS_BEGIN_NATIVE_METHOD_DECL(/*func. name*/addDecodeTargetExtension) {
	if( numparams < 1 ) return TJS_E_BADPARAMCOUNT;
	ttstr ext = *param[0];
	if( ext.IsEmpty() ) return TJS_E_INVALIDPARAM;
	tjs_uint64 minSize = 0;
	if( numparams >= 2 && param[1]->Type() != tvtVoid ) {
		tjs_int64 v = (tjs_int64)*param[1];
		if (v > 0) minSize = (tjs_uint64)v;
	}
	TVPAddDecodeTargetExtension(ext, minSize);
	return TJS_S_OK;
}
TJS_END_NATIVE_STATIC_METHOD_DECL(/*func. name*/addDecodeTargetExtension )
//----------------------------------------------------------------------
TJS_BEGIN_NATIVE_METHOD_DECL(/*func. name*/removeDecodeTargetExtension) {
	if( numparams < 1 ) return TJS_E_BADPARAMCOUNT;
	ttstr ext = *param[0];
	if( ext.IsEmpty() ) return TJS_E_INVALIDPARAM;
	TVPRemoveDecodeTargetExtension(ext);
	return TJS_S_OK;
}
TJS_END_NATIVE_STATIC_METHOD_DECL(/*func. name*/removeDecodeTargetExtension )
//----------------------------------------------------------------------
TJS_BEGIN_NATIVE_METHOD_DECL(/*func. name*/requestCache) {
	if( numparams < 1 ) return TJS_E_BADPARAMCOUNT;
	ttstr path = *param[0];
	tjs_uint64 minSize = 0;
	if( numparams >= 2 && param[1]->Type() != tvtVoid ) {
		tjs_int64 v = (tjs_int64)*param[1];
		if (v > 0) minSize = (tjs_uint64)v;
	}
	ttstr nname = TVPNormalizeStorageName(path);
	// file:// の場合のみ file 層キャッシュを発火 (psb:// / psd:// 等の
	// MediaStorage プラグイン経由のスキームでは file 層は適用外なので
	// CacheFileRequest を呼ばず、警告ログも出さない)
	if( TVPIsCacheTargetFile(nname) ) {
		Application->CacheFileRequest(path, false, minSize);
	}
	// 拡張子が decode-target に登録されていれば、scheme を問わず decode
	// prefetch を発火 (worker は tTVPStreamHolder → 各 MediaStorage の
	// Open 経由でストリームを得る。TVPCreateStreamCS で global lock 済)
	if( TVPIsDecodeTargetFile(nname) ) {
		Application->LoadImagePrefetchRequest(path);
	}
	return TJS_S_OK;
}
TJS_END_NATIVE_STATIC_METHOD_DECL(/*func. name*/requestCache )
//----------------------------------------------------------------------
TJS_BEGIN_NATIVE_METHOD_DECL(/*func. name*/clearCache) {
	// 両層 (file 層 / decode 層) を対象にした evict。
	// 引数なし: transient 全消し (P2 で pinned エントリを残す)
	// 引数あり: path 単位 evict (両層)
	// (旧実装は numparams>1 という条件で path 引数が事実上機能していなかった
	//  バグがあった。ここで修正)
	ttstr path = (numparams >= 1 && param[0]->Type() != tvtVoid) ? *param[0] : TJS_W("");
	if( path.IsEmpty() ) {
		// transient 全消し (両層)
		Application->CacheFileClear(path); // file 層全消し
		TVPClearTransientGraphicCache();   // decode 層 transient 全消し
		TVPFlushImagePrefetchQueue();
	} else {
		// path 単位。decode 層 cache key は autopath 解決後の物理 path で
		// 揃えているので TVPResolveCachePath で同じ正規化を行う。
		// file 層 cache は元から GetPlacedPath で登録されているので同様。
		ttstr resolved = TVPResolveCachePath(path);
		Application->CacheFileClear(resolved); // file 層 path 単位
		TVPClearGraphicCacheEntry(resolved);   // decode 層 path 単位
	}
	return TJS_S_OK;
}
TJS_END_NATIVE_STATIC_METHOD_DECL(/*func. name*/clearCache )
//----------------------------------------------------------------------
TJS_BEGIN_NATIVE_METHOD_DECL(/*func. name*/clearAllCaches) {
	// 両層全消し (P2 以降は pinned 含めて全部消える MAX Compact 相当)
	Application->CacheFileClear(TJS_W(""));
	TVPClearGraphicCache();
	TVPFlushImagePrefetchQueue();
	return TJS_S_OK;
}
TJS_END_NATIVE_STATIC_METHOD_DECL(/*func. name*/clearAllCaches )
//----------------------------------------------------------------------
TJS_BEGIN_NATIVE_METHOD_DECL(/*func. name*/clearTransientCaches) {
	// 両層 transient 全消し (pinned は残る)。タイトル戻り想定。
	TVPClearTransientStorageCache();
	TVPClearTransientGraphicCache();
	TVPFlushImagePrefetchQueue();
	return TJS_S_OK;
}
TJS_END_NATIVE_STATIC_METHOD_DECL(/*func. name*/clearTransientCaches )
//----------------------------------------------------------------------
TJS_BEGIN_NATIVE_METHOD_DECL(/*func. name*/pinCache) {
	if( numparams < 1 ) return TJS_E_BADPARAMCOUNT;
	ttstr path = *param[0];
	ttstr nname = TVPNormalizeStorageName(path);
	TVPPinCache(nname);
	return TJS_S_OK;
}
TJS_END_NATIVE_STATIC_METHOD_DECL(/*func. name*/pinCache )
//----------------------------------------------------------------------
TJS_BEGIN_NATIVE_METHOD_DECL(/*func. name*/unpinCache) {
	if( numparams < 1 ) return TJS_E_BADPARAMCOUNT;
	ttstr path = *param[0];
	ttstr nname = TVPNormalizeStorageName(path);
	TVPUnpinCache(nname);
	return TJS_S_OK;
}
TJS_END_NATIVE_STATIC_METHOD_DECL(/*func. name*/unpinCache )
//----------------------------------------------------------------------
TJS_BEGIN_NATIVE_METHOD_DECL(/*func. name*/isCachePinned) {
	if( numparams < 1 ) return TJS_E_BADPARAMCOUNT;
	ttstr path = *param[0];
	ttstr nname = TVPNormalizeStorageName(path);
	if( result ) {
		*result = TVPIsCachePathPinned(nname) ? 1 : 0;
	}
	return TJS_S_OK;
}
TJS_END_NATIVE_STATIC_METHOD_DECL(/*func. name*/isCachePinned )
//----------------------------------------------------------------------
// file 層キャッシュエントリ一覧。Array<Dictionary> で返す。
// 各エントリ: %[ path:..., size:..., lastaccess:..., usecount:..., pinned:0/1 ]
TJS_BEGIN_NATIVE_METHOD_DECL(/*func. name*/getFileCacheList) {
	if( !result ) return TJS_S_OK;
	std::vector<TVPStorageCacheEntryInfo> entries;
	TVPGetStorageCacheEntries(entries);
	iTJSDispatch2 *array = TJSCreateArrayObject();
	try {
		tjs_int idx = 0;
		for( auto &e : entries ) {
			iTJSDispatch2 *dic = TJSCreateDictionaryObject();
			try {
				tTJSVariant v;
				v = e.name;                  dic->PropSet(TJS_MEMBERENSURE, TJS_W("path"),       0, &v, dic);
				v = (tjs_int64)e.size;       dic->PropSet(TJS_MEMBERENSURE, TJS_W("size"),       0, &v, dic);
				v = (tjs_int64)e.lastaccess; dic->PropSet(TJS_MEMBERENSURE, TJS_W("lastaccess"), 0, &v, dic);
				v = (tjs_int)e.usecount;     dic->PropSet(TJS_MEMBERENSURE, TJS_W("usecount"),   0, &v, dic);
				v = e.pinned ? 1 : 0;        dic->PropSet(TJS_MEMBERENSURE, TJS_W("pinned"),     0, &v, dic);
				tTJSVariant dv(dic, dic);
				array->PropSetByNum(TJS_MEMBERENSURE, idx++, &dv, array);
			} catch(...) { dic->Release(); throw; }
			dic->Release();
		}
		*result = tTJSVariant(array, array);
	} catch(...) { array->Release(); throw; }
	array->Release();
	return TJS_S_OK;
}
TJS_END_NATIVE_STATIC_METHOD_DECL(/*func. name*/getFileCacheList )
//----------------------------------------------------------------------
// decode 層キャッシュエントリ一覧。Array<Dictionary> で返す。
// 各エントリ: %[ path:..., keyidx:..., mode:..., dw:..., dh:...,
//                width:..., height:..., bytes:..., pinned:0/1 ]
TJS_BEGIN_NATIVE_METHOD_DECL(/*func. name*/getImageCacheList) {
	if( !result ) return TJS_S_OK;
	std::vector<TVPGraphicCacheEntryInfo> entries;
	TVPGetGraphicCacheEntries(entries);
	iTJSDispatch2 *array = TJSCreateArrayObject();
	try {
		tjs_int idx = 0;
		for( auto &e : entries ) {
			iTJSDispatch2 *dic = TJSCreateDictionaryObject();
			try {
				tTJSVariant v;
				v = e.name;             dic->PropSet(TJS_MEMBERENSURE, TJS_W("path"),   0, &v, dic);
				v = (tjs_int)e.keyidx;  dic->PropSet(TJS_MEMBERENSURE, TJS_W("keyidx"), 0, &v, dic);
				v = (tjs_int)e.mode;    dic->PropSet(TJS_MEMBERENSURE, TJS_W("mode"),   0, &v, dic);
				v = (tjs_int)e.dw;      dic->PropSet(TJS_MEMBERENSURE, TJS_W("dw"),     0, &v, dic);
				v = (tjs_int)e.dh;      dic->PropSet(TJS_MEMBERENSURE, TJS_W("dh"),     0, &v, dic);
				v = (tjs_int)e.width;   dic->PropSet(TJS_MEMBERENSURE, TJS_W("width"),  0, &v, dic);
				v = (tjs_int)e.height;  dic->PropSet(TJS_MEMBERENSURE, TJS_W("height"), 0, &v, dic);
				v = (tjs_int64)e.bytes; dic->PropSet(TJS_MEMBERENSURE, TJS_W("bytes"),  0, &v, dic);
				v = e.pinned ? 1 : 0;   dic->PropSet(TJS_MEMBERENSURE, TJS_W("pinned"), 0, &v, dic);
				tTJSVariant dv(dic, dic);
				array->PropSetByNum(TJS_MEMBERENSURE, idx++, &dv, array);
			} catch(...) { dic->Release(); throw; }
			dic->Release();
		}
		*result = tTJSVariant(array, array);
	} catch(...) { array->Release(); throw; }
	array->Release();
	return TJS_S_OK;
}
TJS_END_NATIVE_STATIC_METHOD_DECL(/*func. name*/getImageCacheList )
//----------------------------------------------------------------------
// file 層キャッシュ一覧をログ出力。WARNING (TVPAddImportantLog) で
// MASTER ビルドでも見えるレベルで出す (調査用途のため)。
// 共通実装は TVPDumpFileCacheList()/TVPDumpImageCacheList() で本体定義 (REPL から
// も同じ実体を呼ぶ)。
TJS_BEGIN_NATIVE_METHOD_DECL(/*func. name*/dumpFileCacheList) {
	TVPDumpFileCacheList();
	return TJS_S_OK;
}
TJS_END_NATIVE_STATIC_METHOD_DECL(/*func. name*/dumpFileCacheList )
//----------------------------------------------------------------------
// decode 層キャッシュ一覧をログ出力。
TJS_BEGIN_NATIVE_METHOD_DECL(/*func. name*/dumpImageCacheList) {
	TVPDumpImageCacheList();
	return TJS_S_OK;
}
TJS_END_NATIVE_STATIC_METHOD_DECL(/*func. name*/dumpImageCacheList )
TJS_BEGIN_NATIVE_METHOD_DECL(/*func. name*/clearOldCache) {
	int keepTime = numparams > 1 ? (int)*param[0] : 0;
	int force = numparams > 2 ? (int)*param[1] : 0;
	Application->CacheFileClearOld(keepTime, force!=0);
	return TJS_S_OK;
}
TJS_END_NATIVE_STATIC_METHOD_DECL(/*func. name*/clearOldCache )
//----------------------------------------------------------------------
TJS_BEGIN_NATIVE_METHOD_DECL(/*func. name*/setCacheMaxSize) {
	if( numparams < 1 ) return TJS_E_BADPARAMCOUNT;
	tjs_int size = *param[0];
	Application->CacheFileSetMaxSize(size);
	return TJS_S_OK;
}
TJS_END_NATIVE_STATIC_METHOD_DECL(/*func. name*/setCacheMaxSize )
TJS_BEGIN_NATIVE_METHOD_DECL(/*func. name*/isCacheLoading) {
	if( result ) {
		*result = Application->CacheIsLoading() ? 1:0;
	}
	return TJS_S_OK;
}
TJS_END_NATIVE_STATIC_METHOD_DECL(/*func. name*/isCacheLoading )
//----------------------------------------------------------------------
// 画像 decode prefetch (Storages.requestCache 経路の async decode) が
// 進行中かどうか。requestCache 後に loadImages する前の polling 用。
TJS_BEGIN_NATIVE_METHOD_DECL(/*func. name*/isImagePrefetchLoading) {
	if( result ) {
		*result = TVPIsImagePrefetchLoading() ? 1 : 0;
	}
	return TJS_S_OK;
}
TJS_END_NATIVE_STATIC_METHOD_DECL(/*func. name*/isImagePrefetchLoading )
//----------------------------------------------------------------------
TJS_BEGIN_NATIVE_METHOD_DECL(/*func. name*/requestFastCache) {
	if( numparams < 1 ) return TJS_E_BADPARAMCOUNT;
	ttstr path = *param[0];
	tjs_uint64 minSize = 0;
	if( numparams >= 2 && param[1]->Type() != tvtVoid ) {
		tjs_int64 v = (tjs_int64)*param[1];
		if (v > 0) minSize = (tjs_uint64)v;
	}
	ttstr nname = TVPNormalizeStorageName(path);
	if( TVPIsCacheTargetFile(nname) ) {
		Application->CacheFileRequest(path, true, minSize);
	}
	if( TVPIsDecodeTargetFile(nname) ) {
		Application->LoadImagePrefetchRequest(path);
	}
	return TJS_S_OK;
}
TJS_END_NATIVE_STATIC_METHOD_DECL(/*func. name*/requestFastCache )
//----------------------------------------------------------------------
TJS_BEGIN_NATIVE_METHOD_DECL(/*func. name*/clearFastCache) {
	// 旧 fast 系 alias。clearCache と同じく両層対応に揃える。
	// (旧実装は numparams>1 で path 引数が事実上無効だったバグを修正)
	ttstr path = (numparams >= 1 && param[0]->Type() != tvtVoid) ? *param[0] : TJS_W("");
	if( path.IsEmpty() ) {
		Application->CacheFileClear(path);
		TVPClearTransientGraphicCache();
		TVPFlushImagePrefetchQueue();
	} else {
		// clearCache と同じく autopath 解決後の物理 path で揃える
		ttstr resolved = TVPResolveCachePath(path);
		Application->CacheFileClear(resolved);
		TVPClearGraphicCacheEntry(resolved);
	}
	return TJS_S_OK;
}
TJS_END_NATIVE_STATIC_METHOD_DECL(/*func. name*/clearFastCache )
//----------------------------------------------------------------------
TJS_BEGIN_NATIVE_METHOD_DECL(/*func. name*/isFastCacheLoading) {
	if( result ) {
		*result = Application->CacheIsLoading(true) ? 1:0;
	}
	return TJS_S_OK;
}
TJS_END_NATIVE_STATIC_METHOD_DECL(/*func. name*/isFastCacheLoading )
//----------------------------------------------------------------------

//----------------------------------------------------------------------
	TJS_END_NATIVE_MEMBERS
}
//---------------------------------------------------------------------------
tTJSNativeInstance * tTJSNC_Storages::CreateNativeInstance()
{
	// this class cannot create an instance
	TVPThrowExceptionMessage(TVPCannotCreateInstance);

	return NULL;
}
//---------------------------------------------------------------------------



