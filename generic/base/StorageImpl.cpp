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

#include "MsgIntf.h"

#include "StorageImpl.h"
#include "WindowImpl.h"
#include "SysInitIntf.h"
#ifdef KRKRZ_USE_REPL_FILECHANNEL
#include "ReplModal.h"   // TVPReplTrySelect
#endif
#include "LogIntf.h"
#include "Random.h"
#include "XP3Archive.h"

#include "Application.h"
#include "StringUtil.h"
#include "TickCount.h"
#include "CharacterSet.h"
#include "tjsArray.h"

#ifndef _WIN32
#include <sys/types.h>
#include <sys/stat.h>
#include <unistd.h>
#endif


#include "LocalFileSystem.h"
static iTVPLocalFileSystem *LocalFileSystem = nullptr;

extern void TVPClearAutoPathCacheFile(const ttstr & name);
extern void TVPAddAutoPathCacheFile(const ttstr & name);

void InitLocalFileSystem()
{
	if (LocalFileSystem == nullptr) {
		LocalFileSystem = TVPCreateLocalFileSystem();
	}
}

//---------------------------------------------------------------------------
// tTVPFileMedia
//---------------------------------------------------------------------------
class tTVPFileMedia : public iTVPStorageMedia
{
	tjs_uint RefCount;

public:
	tTVPFileMedia() { RefCount = 1; }
	~tTVPFileMedia() {;}

	void TJS_INTF_METHOD AddRef() { RefCount ++; }
	void TJS_INTF_METHOD Release()
	{
		if(RefCount == 1)
			delete this;
		else
			RefCount --;
	}

	void TJS_INTF_METHOD GetName(ttstr &name) { name = TJS_W("file"); }

	void TJS_INTF_METHOD NormalizeDomainName(ttstr &name);
	void TJS_INTF_METHOD NormalizePathName(ttstr &name);
	bool TJS_INTF_METHOD CheckExistentStorage(const ttstr &name);
	iTJSBinaryStream * TJS_INTF_METHOD Open(const ttstr & name, tjs_uint32 flags);
	void TJS_INTF_METHOD GetListAt(const ttstr &name, iTVPStorageLister *lister);
	void TJS_INTF_METHOD GetLocallyAccessibleName(ttstr &name);

public:
	void TJS_INTF_METHOD GetLocalName(ttstr &name);
};
//---------------------------------------------------------------------------
void TJS_INTF_METHOD tTVPFileMedia::NormalizeDomainName(ttstr &name)
{
	// normalize domain name
	// make all characters small
	tjs_char *p = name.Independ();
	while(*p)
	{
		if(*p >= TJS_W('A') && *p <= TJS_W('Z'))
			*p += TJS_W('a') - TJS_W('A');
		p++;
	}
}
//---------------------------------------------------------------------------
void TJS_INTF_METHOD tTVPFileMedia::NormalizePathName(ttstr &name)
{
#ifdef TVP_NO_NORMALIZE_PATH
	// dont normalize path name
#else
	// normalize path name
	// make all characters small
	tjs_char *p = name.Independ();
	while(*p)
	{
		if(*p >= TJS_W('A') && *p <= TJS_W('Z'))
			*p += TJS_W('a') - TJS_W('A');
		p++;
	}
#endif
}
//---------------------------------------------------------------------------
bool TJS_INTF_METHOD tTVPFileMedia::CheckExistentStorage(const ttstr &name)
{
	if(name.IsEmpty()) return false;

	ttstr _name(name);
	GetLocallyAccessibleName(_name);
	if(_name.IsEmpty()) return false;

	return TVPCheckExistentLocalFile(_name);
}

ttstr TVPLocalExtractFilePath(const ttstr & name);
bool TVPCreateFolders(const ttstr &folder);

//---------------------------------------------------------------------------
iTJSBinaryStream * TJS_INTF_METHOD tTVPFileMedia::Open(const ttstr & name, tjs_uint32 flag)
{
	tjs_uint32 access = flag & TJS_BS_ACCESS_MASK;

	// open storage named "name".
	// currently only local/network(by OS) storage systems are supported.
	if(name.IsEmpty())
		TVPThrowExceptionMessage(TVPCannotOpenStorage, TJS_W("\"\""));

	ttstr origname = name;
	ttstr _name(name);
	GetLocalName(_name);


	tjs_int trycount = 0;
	iTJSBinaryStream *ret;

	// 書き込み用に開けない場合はフォルダ作成を試みる
retry:
	ret = LocalFileSystem->OpenStream(_name.c_str(), flag);
	if(ret == nullptr)
	{
		if(trycount == 0 && access == TJS_BS_WRITE)
		{
			trycount++;

			// retry after creating the folder
			TVPCreateFolders(TVPLocalExtractFilePath(_name));
			goto retry;
		}
		TVPThrowExceptionMessage(TVPCannotOpenStorage, origname);
	}

	return ret;
}
//---------------------------------------------------------------------------
void TJS_INTF_METHOD tTVPFileMedia::GetListAt(const ttstr &_name, iTVPStorageLister *lister)
{
	ttstr name(_name);
	GetLocalName(name);
	LocalFileSystem->GetListAt(name.c_str(), [lister](const tjs_char *filename, bool isDir) {
		ttstr file = filename;
#ifdef TVP_NO_NORMALIZE_PATH
#else
		tjs_char *p = file.Independ();
		while(*p) {
			// make all characters small
			if(*p >= TJS_W('A') && *p <= TJS_W('Z'))
				*p += TJS_W('a') - TJS_W('A');
			p++;
		}
#endif
		lister->Add(file);
	}, false);
}
//---------------------------------------------------------------------------
void TJS_INTF_METHOD tTVPFileMedia::GetLocallyAccessibleName(ttstr &name)
{
	tjs_string xname = name.c_str();
	LocalFileSystem->GetLocallyAccessibleName(xname);
	name = xname.c_str();
}
//---------------------------------------------------------------------------
void TJS_INTF_METHOD tTVPFileMedia::GetLocalName(ttstr &name)
{
	ttstr tmp = name;
	GetLocallyAccessibleName(tmp);
	if(tmp.IsEmpty()) TVPThrowExceptionMessage(TVPCannotGetLocalName, name);
	name = tmp;
}
//---------------------------------------------------------------------------



//---------------------------------------------------------------------------
iTVPStorageMedia * TVPCreateFileMedia()
{
	InitLocalFileSystem();
	return new tTVPFileMedia;
}
//---------------------------------------------------------------------------





//---------------------------------------------------------------------------
// TVPPreNormalizeStorageName
//---------------------------------------------------------------------------
void TVPPreNormalizeStorageName(ttstr &name)
{
	tjs_int namelen = name.length();
	if(namelen == 0) return;

	tjs_string xname = name.c_str();
	if (LocalFileSystem->NormalizeStorageName(xname)) {
		name = xname.c_str();
	}
}

//---------------------------------------------------------------------------


//---------------------------------------------------------------------------
// TVPGetTemporaryName
//---------------------------------------------------------------------------
static tjs_int TVPTempUniqueNum = 0;
static tTJSCriticalSection TVPTempUniqueNumCS;
static ttstr TVPTempPath;
bool TVPTempPathInit = false;
static tjs_int TVPProcessID;
ttstr TVPGetTemporaryName()
{
	tjs_int num;

	{
		tTJSCriticalSectionHolder holder(TVPTempUniqueNumCS);

		if(!TVPTempPathInit)
		{
			TVPTempPath = ttstr( Application->TempPath().c_str() );
			if(TVPTempPath.GetLastChar() != TJS_W('\\')) TVPTempPath += TJS_W("\\");
			TVPProcessID = static_cast<tjs_int>( getpid() );
			TVPTempUniqueNum = static_cast<tjs_int>( TVPGetRoughTickCount32() );
			TVPTempPathInit = true;
		}
		num = TVPTempUniqueNum ++;
	}

	unsigned char buf[16];
	TVPGetRandomBits128(buf);
	tjs_char random[128];
	TJS_snprintf(random, sizeof(random)/sizeof(tjs_char), TJS_W("%02x%02x%02x%02x%02x%02x"),
		buf[0], buf[1], buf[2], buf[3],
		buf[4], buf[5]);

	return TVPTempPath + TJS_W("krkr_") + ttstr(random) +
		TJS_W("_") + ttstr(num) + TJS_W("_") + ttstr(TVPProcessID);
}
//---------------------------------------------------------------------------






//---------------------------------------------------------------------------
// TVPRemoveFile
//---------------------------------------------------------------------------
bool TVPRemoveFile(const ttstr &name)
{
	return LocalFileSystem->RemoveFile(name.c_str());
}
//---------------------------------------------------------------------------



//---------------------------------------------------------------------------
// TVPRemoveFolder
//---------------------------------------------------------------------------
bool TVPRemoveFolder(const ttstr &name)
{
	return 0==LocalFileSystem->RemoveDirectory(name.c_str());
}
//---------------------------------------------------------------------------

//---------------------------------------------------------------------------
// TVPMoveFile
//---------------------------------------------------------------------------
bool TVPMoveFile(const ttstr &oldname, const ttstr &newname)
{
	// rename file ( "oldname" and "newname" are local *native* names )
	// this must not throw an exception ( return false if error )
	return LocalFileSystem->MoveFile(oldname.c_str(), newname.c_str());
}

//---------------------------------------------------------------------------
// TVPLastModifiedFileTime
//---------------------------------------------------------------------------
tjs_uint64 TVPLastModifiedFileTime(const ttstr &name)
{
	return LocalFileSystem->LastModifiedFileTime(name.c_str());
}

//---------------------------------------------------------------------------
// TVPFileSize
//---------------------------------------------------------------------------
tjs_uint64 TVPFileSize(const ttstr &name)
{
	return LocalFileSystem->FileSize(name.c_str());
}

//---------------------------------------------------------------------------
// TVPGetAppPath
//---------------------------------------------------------------------------
ttstr TVPGetAppPath()
{
	static ttstr exepath(TVPExtractStoragePath(TVPNormalizeStorageName(Application->AppPath())));
	return exepath;
}
//---------------------------------------------------------------------------

//---------------------------------------------------------------------------
// TVPGetResourcePath
//---------------------------------------------------------------------------
ttstr TVPGetResourcePath()
{
	// Application 実装 (SDL3 / Android / NX 等) が持つ既定リソースパス。
	// desktop = "resource://./" / wasm = "file://./resource/" のように変わる。
	return ttstr(Application->ResourcePath().c_str());
}
//---------------------------------------------------------------------------






//---------------------------------------------------------------------------
// TVPCheckExistantLocalFile
//---------------------------------------------------------------------------
bool TVPCheckExistentLocalFile(const ttstr &name)
{
	InitLocalFileSystem();
	return LocalFileSystem->ExistentFile(name.c_str());
}
//---------------------------------------------------------------------------




//---------------------------------------------------------------------------
// TVPCheckExistantLocalFolder
//---------------------------------------------------------------------------
bool TVPCheckExistentLocalFolder(const ttstr &name)
{
	InitLocalFileSystem();
	return LocalFileSystem->ExistentFolder(name.c_str());
}

//---------------------------------------------------------------------------

//---------------------------------------------------------------------------
// TVPOpenArchive
//---------------------------------------------------------------------------
tTVPArchive * TVPOpenArchive(const ttstr & name)
{
	return new tTVPXP3Archive(name);
}
//---------------------------------------------------------------------------


//---------------------------------------------------------------------------
// TVPLocalExtrectFilePath
//---------------------------------------------------------------------------
ttstr TVPLocalExtractFilePath(const ttstr & name)
{
	// this extracts given name's path under local filename rule
	const tjs_char *p = name.c_str();
	tjs_int i = name.GetLen() -1;
	for(; i >= 0; i--)
	{
		if(p[i] == TJS_W(':') || p[i] == TJS_W('/') ||
			p[i] == TJS_W('\\'))
			break;
	}
	return ttstr(p, i + 1);
}
//---------------------------------------------------------------------------




//---------------------------------------------------------------------------
// TVPCreateFolders
//---------------------------------------------------------------------------
static bool _TVPCreateFolders(const ttstr &folder)
{
	// create directories along with "folder"
	if(folder.IsEmpty()) return true;

	if(TVPCheckExistentLocalFolder(folder))
		return true; // already created

	const tjs_char *p = folder.c_str();
	tjs_int i = folder.GetLen() - 1;

	if(p[i] == TJS_W(':')) return true;

	while(i >= 0 && (p[i] == TJS_W('/') || p[i] == TJS_W('\\'))) i--;

	if(i >= 0 && p[i] == TJS_W(':')) return true;

	for(; i >= 0; i--)
	{
		if(p[i] == TJS_W(':') || p[i] == TJS_W('/') ||
			p[i] == TJS_W('\\'))
			break;
	}

	ttstr parent(p, i + 1);

	if(!_TVPCreateFolders(parent)) return false;

	// LocalFileSystem (SDL3FileSystem) の MakeDirectory は SDL_CreateDirectory
	// 準拠で「成功 = true」。 旧実装は mkdir(2) の「成功 = 0」を仮定した
	// 0 == 比較になっており、 成功を失敗と判定して親までしか作られなかった
	// (2 階層以上の新規フォルダへの書き込みが常に失敗していた)
	return LocalFileSystem->MakeDirectory(folder.c_str());
}

bool TVPCreateFolders(const ttstr &folder)
{
	if(folder.IsEmpty()) return true;

	const tjs_char *p = folder.c_str();
	tjs_int i = folder.GetLen() - 1;

	if(p[i] == TJS_W(':')) return true;

	if(p[i] == TJS_W('/') || p[i] == TJS_W('\\')) i--;

	return _TVPCreateFolders(ttstr(p, i+1));
}
//---------------------------------------------------------------------------

//---------------------------------------------------------------------------
// TVPOpenStream
//---------------------------------------------------------------------------
iTJSBinaryStream * TVPOpenStream(const ttstr & _name, tjs_uint32 flag)
{
	InitLocalFileSystem();

	tjs_uint32 access = flag & TJS_BS_ACCESS_MASK;

	// open storage named "name".
	// currently only local/network(by OS) storage systems are supported.
	if(_name.IsEmpty())
		TVPThrowExceptionMessage(TVPCannotOpenStorage, TJS_W("\"\""));

	ttstr origname = _name;
	ttstr name(_name);
	TVPGetLocalName(name);

	tjs_int trycount = 0;
	iTJSBinaryStream *ret;

	// 書き込み用に開けない場合はフォルダ作成を試みる
retry:
	ret = LocalFileSystem->OpenStream(name.c_str(), flag);
	if(ret == nullptr)
	{
		if(trycount == 0 && access == TJS_BS_WRITE)
		{
			trycount++;

			// retry after creating the folder
			TVPCreateFolders(TVPLocalExtractFilePath(name));
			goto retry;
		}
		TVPThrowExceptionMessage(TVPCannotOpenStorage, origname);
	}

	// push current tick as an environment noise
	// (timing information from file accesses may be good noises)
	tjs_uint32 tick = TVPGetRoughTickCount32();
	TVPPushEnvironNoise(&tick, sizeof(tick));

	return ret;
}
//---------------------------------------------------------------------------







//---------------------------------------------------------------------------
// tTVPPluginHolder
//---------------------------------------------------------------------------
tTVPPluginHolder::tTVPPluginHolder(const ttstr &aname)
: LocalTempStorageHolder(nullptr)
{
	// not found in TVP storage system; search exepath, exepath\plugin	
	ttstr basepath = Application->AppPath();
	ttstr pname = basepath + aname;
	if(TVPCheckExistentLocalFile(pname)) {
		LocalPath = pname;
		return;
	}

	basepath = Application->PluginPath();
	pname = basepath + aname;
	if(TVPCheckExistentLocalFile(pname))
	{
		LocalPath = pname;
		return;
	}
}
//---------------------------------------------------------------------------
tTVPPluginHolder::~tTVPPluginHolder()
{
	if(LocalTempStorageHolder)
	{
		delete LocalTempStorageHolder;
	}
}
//---------------------------------------------------------------------------
const ttstr & tTVPPluginHolder::GetLocalName() const
{
	if(LocalTempStorageHolder) return LocalTempStorageHolder->GetLocalName();
	return LocalPath;
}
//---------------------------------------------------------------------------

static bool setDirListFile(iTJSDispatch2 *array, tjs_int count, ttstr const &file) 
{
	// [dirlist] 配列に追加する
	tTJSVariant val(file);
	array->PropSetByNum(0, count, &val, array);
	return true;
}

static void _dirtree(const ttstr &path, const ttstr &subdir, iTJSDispatch2 *array, tjs_int &count, bool dironly) 
{
	LocalFileSystem->GetListAt(path.c_str(), [array, &count, &path, &subdir, dironly](const tjs_char *filename, bool isDir) {
		ttstr file = filename;
	#ifdef TVP_NO_NORMALIZE_PATH
	#else
		tjs_char *p = file.Independ();
		while(*p) {
			// make all characters small
			if(*p >= TJS_W('A') && *p <= TJS_W('Z'))
				*p += TJS_W('a') - TJS_W('A');
			p++;
		}
	#endif
		if (isDir) {
			ttstr name(subdir + file + TJS_W("/"));
			setDirListFile(array, count++, name);
			_dirtree(path + file + TJS_W("/"), name, array, count, dironly);
		} else if (!dironly) {
			ttstr name(subdir + file);
			setDirListFile(array, count++, name);
		}
	}, dironly);
}

//---------------------------------------------------------------------------
// TVPCreateNativeClass_Storages
//---------------------------------------------------------------------------
tTJSNativeClass * TVPCreateNativeClass_Storages()
{
	InitLocalFileSystem();
	tTJSNC_Storages *cls = new tTJSNC_Storages();

	// setup some platform-specific members
//----------------------------------------------------------------------

//-- methods

//----------------------------------------------------------------------
TJS_BEGIN_NATIVE_METHOD_DECL(/*func. name*/searchCD)
{
	return TJS_E_NOTIMPL;
}
TJS_END_NATIVE_STATIC_METHOD_DECL_OUTER(/*object to register*/cls,
	/*func. name*/searchCD)
//----------------------------------------------------------------------
TJS_BEGIN_NATIVE_METHOD_DECL(/*func. name*/getLocalName)
{
	if(numparams < 1) return TJS_E_BADPARAMCOUNT;

	if(result)
	{
		ttstr str(TVPNormalizeStorageName(*param[0]));
		TVPGetLocalName(str);
		*result = str;
	}

	return TJS_S_OK;
}
TJS_END_NATIVE_STATIC_METHOD_DECL_OUTER(/*object to register*/cls,
	/*func. name*/getLocalName)
//----------------------------------------------------------------------
TJS_BEGIN_NATIVE_METHOD_DECL(/*func. name*/selectFile)
{
	if(numparams < 1) return TJS_E_BADPARAMCOUNT;

	iTJSDispatch2 * dsp =  param[0]->AsObjectNoAddRef();

#ifdef KRKRZ_USE_REPL_FILECHANNEL
	if(TVPReplActive) {
		int r = TVPReplTrySelect(dsp, false);
		if(r >= 0) { if(result) *result = (tjs_int)(r == 1); return TJS_S_OK; }
	}
#endif

	// ファイル選択ダイアログは環境依存機能なので Application に委譲する
	// (SDL3 は SDL_ShowOpenFileDialog で実装。非対応環境は false を返す)。
	bool res = Application ? Application->SelectFile(dsp) : false;

	if(result) *result = (tjs_int)res;

	return TJS_S_OK;
}
TJS_END_NATIVE_STATIC_METHOD_DECL_OUTER(/*object to register*/cls,
	/*func. name*/selectFile)
//----------------------------------------------------------------------
TJS_BEGIN_NATIVE_METHOD_DECL(/*func. name*/selectDirectory)
{
	if(numparams < 1) return TJS_E_BADPARAMCOUNT;

	iTJSDispatch2 * dsp = param[0]->AsObjectNoAddRef();

#ifdef KRKRZ_USE_REPL_FILECHANNEL
	if(TVPReplActive) {
		int r = TVPReplTrySelect(dsp, true);
		if(r >= 0) { if(result) *result = (tjs_int)(r == 1); return TJS_S_OK; }
	}
#endif

	// フォルダ選択ダイアログは環境依存機能なので Application に委譲する
	// (SDL3 は SDL_ShowOpenFolderDialog で実装。非対応環境は false を返す)。
	bool res = Application ? Application->SelectDirectory(dsp) : false;

	if(result) *result = (tjs_int)res;

	return TJS_S_OK;
}
TJS_END_NATIVE_STATIC_METHOD_DECL_OUTER(/*object to register*/cls,
	/*func. name*/selectDirectory)
//----------------------------------------------------------------------
TJS_BEGIN_NATIVE_METHOD_DECL(/*func. name*/isExistentDirectory)
{
	if(numparams < 1) return TJS_E_BADPARAMCOUNT;

	ttstr path = *param[0];

	if(path.GetLastChar() != TJS_W('/')) {
		TVPThrowExceptionMessage(TVPRequireSlashEndOfDirectory);
	}
	path = TVPNormalizeStorageName(path);
	TVPGetLocalName(path);
	if(result)
		*result = TVPCheckExistentLocalFolder(path) ? 1:0;

	return TJS_S_OK;
}
TJS_END_NATIVE_STATIC_METHOD_DECL_OUTER(/*object to register*/cls,
	/*func. name*/isExistentDirectory)
//----------------------------------------------------------------------
TJS_BEGIN_NATIVE_METHOD_DECL(/*func. name*/dirlist)
{
	if(numparams < 1) return TJS_E_BADPARAMCOUNT;

	ttstr path = *param[0];

	if(path.GetLastChar() != TJS_W('/')) {
		TVPThrowExceptionMessage(TVPRequireSlashEndOfDirectory);
	}

	path = TVPNormalizeStorageName(path);
	TVPGetLocalName(path);

	if(result) {
		// Array クラスのオブジェクトを作成
		iTJSDispatch2 * array = TJSCreateArrayObject();
		tjs_int count = 0;

		LocalFileSystem->GetListAt(path.c_str(), [array, &count](const tjs_char *filename, bool isDir) {
			ttstr file = filename;
			if (isDir) {
				file += TJS_W("/");
			}
	#ifdef TVP_NO_NORMALIZE_PATH
	#else
			tjs_char *p = file.Independ();
			while(*p) {
				// make all characters small
				if(*p >= TJS_W('A') && *p <= TJS_W('Z'))
					*p += TJS_W('a') - TJS_W('A');
				p++;
			}
	#endif
			setDirListFile(array, count++, file);
		}, true);
		*result = tTJSVariant(array, array);
		array->Release();
	}

	return TJS_S_OK;
}
TJS_END_NATIVE_STATIC_METHOD_DECL_OUTER(/*object to register*/cls,
	/*func. name*/dirlist)
//----------------------------------------------------------------------
TJS_BEGIN_NATIVE_METHOD_DECL(/*func. name*/dirtree)
{
	if (numparams < 1) return TJS_E_BADPARAMCOUNT;
	bool dironly = numparams > 1 ? param[1]->operator bool() : false;
		
	ttstr path(TVPNormalizeStorageName(ttstr(*param[0])+TJS_W("/")));
	TVPGetLocalName(path);

	if(result) {
		iTJSDispatch2 * array = TJSCreateArrayObject();
		tjs_int count = 0;

		_dirtree(path, TJS_W(""), array, count, dironly);

		*result = tTJSVariant(array, array);
		array->Release();
	}
	return TJS_S_OK;
}
TJS_END_NATIVE_STATIC_METHOD_DECL_OUTER(/*object to register*/cls,
	/*func. name*/dirtree)
//----------------------------------------------------------------------
	TJS_BEGIN_NATIVE_METHOD_DECL(/*func. name*/commitSavedata)
{
	LocalFileSystem->CommitSavedata();
	return TJS_S_OK;
}
TJS_END_NATIVE_STATIC_METHOD_DECL_OUTER(/*object to register*/cls,
	/*func. name*/commitSavedata)
TJS_BEGIN_NATIVE_METHOD_DECL(/*func. name*/rollbackSavedata)
{
	LocalFileSystem->RollbackSavedata();
	return TJS_S_OK;
}
TJS_END_NATIVE_STATIC_METHOD_DECL_OUTER(/*object to register*/cls,
	/*func. name*/rollbackSavedata)
TJS_BEGIN_NATIVE_METHOD_DECL(/*func. name*/moveFile)
{
	if(numparams < 2) return TJS_E_BADPARAMCOUNT;

	tTJSVariant *from = param[0];
	tTJSVariant *to   = param[1];

	bool ret = false;
	if (from && to && from->Type() == tvtString && to->Type() == tvtString) {

		// 正規パス
		ttstr fromFile = TVPNormalizeStorageName(from->AsString());
		ttstr toFile   = TVPNormalizeStorageName(to->AsString());

		TVPLOG_DEBUG("move from:{} to:{}", fromFile, toFile);

		if (TVPMoveStorage(fromFile, toFile)) {
			ret = true;
			TVPClearAutoPathCacheFile(fromFile);
			TVPAddAutoPathCacheFile(toFile);
		}
	}
	if (result) {
		*result = ret ? 1:0;
	}
	return TJS_S_OK;
}
TJS_END_NATIVE_STATIC_METHOD_DECL_OUTER(/*object to register*/cls,
	/*func. name*/moveFile)
TJS_BEGIN_NATIVE_METHOD_DECL(/*func. name*/deleteFile)
{
	if(numparams < 1) return TJS_E_BADPARAMCOUNT;

	tTJSVariant *path = param[0];

	bool ret = false;
	if (path && path->Type() == tvtString) {
		// 正規パス
		ttstr pathFile = TVPNormalizeStorageName(path->AsString());
		TVPLOG_DEBUG("delete file:{}", pathFile);
		if (TVPRemoveStorage(pathFile)) {
			ret = true;
			TVPClearAutoPathCacheFile(pathFile);
		}
	}

	if (result) {
		*result = ret ? 1:0;
	}
	return TJS_S_OK;
}
TJS_END_NATIVE_STATIC_METHOD_DECL_OUTER(/*object to register*/cls,
	/*func. name*/deleteFile)

TJS_BEGIN_NATIVE_METHOD_DECL(/*func. name*/getLastModifiedFileTime)
{
	if(numparams < 1) return TJS_E_BADPARAMCOUNT;

	tTJSVariant *path = param[0];

	tjs_uint64 ret = 0;
	if (path && path->Type() == tvtString) {
		// 正規パス
		ttstr pathFile = TVPNormalizeStorageName(path->AsString());
		ret = TVPLastModifiedFileTimeStorage(pathFile);
	}

	if (result) {
		*result = (tTVInteger)ret;
	}
	return TJS_S_OK;
}
TJS_END_NATIVE_STATIC_METHOD_DECL_OUTER(/*object to register*/cls,
	/*func. name*/getLastModifiedFileTime)

	//----------------------------------------------------------------------

	return cls;

}
//---------------------------------------------------------------------------

