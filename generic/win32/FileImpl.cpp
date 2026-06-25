#include "tjsCommHead.h"
#include <LocalFileSystem.h>
#include "MsgIntf.h"

#include <functional>
#include <algorithm>
#include <shlwapi.h>
#pragma comment (lib, "shlwapi.lib")

/**
 * 吉里吉里用ローカルファイルインターフェース 標準関数
 */
class StdFileSystem : public iTVPLocalFileSystem
{
public:
	StdFileSystem();
	virtual ~StdFileSystem();

	virtual bool NormalizeStorageName(tjs_string &name);
	virtual void GetLocallyAccessibleName(tjs_string &name);
	virtual void GetListAt(const tjs_char *name, std::function<void(const tjs_char *, bool isDir)> lister, bool withDir);
	virtual bool RemoveDirectory(const tjs_char *path);
	virtual bool MakeDirectory(const tjs_char *path);
	virtual bool ExistentFile(const tjs_char *path);
	virtual bool ExistentFolder(const tjs_char *path);
	virtual bool RemoveFile(const tjs_char *path);
	virtual bool MoveFile(const tjs_char *fromFile, const tjs_char *toFile);
	virtual iTJSBinaryStream *OpenStream(const tjs_char *path, const tjs_uint32 flags);
	virtual void CommitSavedata();
	virtual void RollbackSavedata();
	virtual tjs_uint64 LastModifiedFileTime(const tjs_char *path);
	virtual tjs_uint64 FileSize(const tjs_char *path);
};

StdFileSystem::StdFileSystem()
{
}

StdFileSystem::~StdFileSystem() 
{
}


//---------------------------------------------------------------------------
// iTVPLocalStorage
//---------------------------------------------------------------------------

/**
 * @brief ファイル名正規化
 * @return 正規化実行された
 */
bool
StdFileSystem::NormalizeStorageName(tjs_string &name)
{
	// if the name is an OS's native expression, change it according with the
	// TVP storage system naming rule.
	tjs_int namelen = name.length();
	if(namelen == 0) return false;

	if(namelen >= 2)
	{
		if((name[0] >= TJS_W('a') && name[0]<=TJS_W('z') ||
			name[0] >= TJS_W('A') && name[0]<=TJS_W('Z') ) &&
			name[1] == TJS_W(':'))
		{
			// Windows drive:path expression
			tjs_string newname(TJS_W("file://./"));
			newname += name[0];
			newname += (name.c_str()+2);
            name = newname;
			return true;
		}
	}

	if(namelen>=3)
	{
		if(name[0] == TJS_W('\\') && name[1] == TJS_W('\\') ||
			name[0] == TJS_W('/') && name[1] == TJS_W('/'))
		{
			// unc expression
			name = tjs_string(TJS_W("file:")) + name;
			return true;
		}
	}

	return false;
}

//< ファイル名のローカル化処理
// ディスク名復活処理など
void
StdFileSystem::GetLocallyAccessibleName(tjs_string &name)
{
	const tjs_char *ptr = name.c_str();
	tjs_string newname;

	if(TJS_strncmp(ptr, TJS_W("./"), 2))
	{
		// differs from "./",
		// this may be a UNC file name.
		// UNC first two chars must be "\\\\" ?
		// AFAIK 32-bit version of Windows assumes that '/' can be used as a path
		// delimiter. Can UNC "\\\\" be replaced by "//" though ?
		newname = tjs_string(TJS_W("\\\\")) + ptr;
	}
	else
	{
		ptr += 2;  // skip "./"
		if(!*ptr) {
			newname = TJS_W("");
		} else {
			tjs_char dch = tolower(*ptr);
			if (dch < TJS_W('a') || dch > TJS_W('z')) {
				newname = TJS_W("");
			} else {
				ptr++;
				if(*ptr != TJS_W('/')) {
					newname = TJS_W("");
				} else {
					newname = dch;
					newname += TJS_W(":");
					newname += ptr;
				}
			}
		}
	}
	// change path delimiter to '/'
	std::replace(newname.begin(), newname.end(), TJS_W('\\'), TJS_W('/'));

	name = newname;
}

void
StdFileSystem::GetListAt(const tjs_char *name, std::function<void(const tjs_char *, bool isDir)> lister, bool withDir)
{
    tjs_string path = name;
	path += TJS_W("*.*");
    
    // perform UNICODE operation
	WIN32_FIND_DATAW ffd;
	HANDLE handle = ::FindFirstFileW((const wchar_t*)path.c_str(), &ffd);
	if(handle != INVALID_HANDLE_VALUE)
	{
		BOOL cont;
		do
		{
			bool isDir = !!(ffd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY);
			if (!isDir || (isDir && withDir)) {
				ttstr file( reinterpret_cast<tjs_char*>(ffd.cFileName) );
				lister(file.c_str(), isDir);
			}
			cont = ::FindNextFile(handle, &ffd);
		} while(cont);
		FindClose(handle);
	}

}

bool
StdFileSystem::RemoveDirectory(const tjs_char *path)
{
	return ::RemoveDirectoryW((const wchar_t*)path) == 0;
}

bool
StdFileSystem::MakeDirectory(const tjs_char *path)
{
    return ::CreateDirectoryW((const wchar_t*)path, NULL) == 0;
}


#ifdef TVP_LOCALFILE_FORCE_CASESENSITIVE
#include <shellapi.h>
#endif

bool
StdFileSystem::ExistentFile(const tjs_char *path)
{
	DWORD attrib = ::GetFileAttributesW((const wchar_t*)path);
	if(attrib == 0xffffffff || (attrib & FILE_ATTRIBUTE_DIRECTORY))
		return false; // not a file

#ifdef TVP_LOCALFILE_FORCE_CASESENSITIVE
	// case 違いでエラーを返す
	SHFILEINFOW info;
	if (SHGetFileInfoW((const wchar_t*)path, 0, &info, sizeof(info), SHGFI_DISPLAYNAME ) ) {
		if (TJS_strcmp((const tjs_char*)info.szDisplayName, (const tjs_char*)::PathFindFileNameW((const wchar_t*)path)) != 0) {
			return false;
		}
	}
#endif

	return true; // a file
}

bool
StdFileSystem::ExistentFolder(const tjs_char *path)
{
	DWORD attrib = ::GetFileAttributesW((const wchar_t*)path);
	if(attrib != 0xffffffff && (attrib & FILE_ATTRIBUTE_DIRECTORY))
		return true; // a folder
	else
		return false; // not a folder
}

bool
StdFileSystem::RemoveFile(const tjs_char *path)
{
	BOOL r = false;
	if (*path) {
		r = ::DeleteFileW((const wchar_t*)path);
	}
	return !! r;
}

bool 
StdFileSystem::MoveFile(const tjs_char *fromFile, const tjs_char *toFile)
{
	BOOL r = false;
	if (*fromFile && *toFile) {
		r = ::MoveFileW((const wchar_t*)fromFile, (const wchar_t*)toFile);
	}
	return !! r;
}

//---------------------------------------------------------------------------
// tTVPLocalFileStream
//---------------------------------------------------------------------------

class tTVPLocalFileStream : public iTJSBinaryStream
{
public:
	tTVPLocalFileStream();
	~tTVPLocalFileStream();

	bool Open(const tjs_char *path, tjs_uint32 flag);

	tjs_uint64 TJS_INTF_METHOD Seek(tjs_int64 offset, tjs_int whence);

	tjs_uint TJS_INTF_METHOD Read(void *buffer, tjs_uint read_size);
	tjs_uint TJS_INTF_METHOD Write(const void *buffer, tjs_uint write_size);

	void TJS_INTF_METHOD SetEndOfStorage();

	tjs_uint64 TJS_INTF_METHOD GetSize();

private:
	FILE *Handle;
};

//---------------------------------------------------------------------------

tTVPLocalFileStream::tTVPLocalFileStream()
 :Handle(0)
{
}

bool
tTVPLocalFileStream::Open(const tjs_char *path, tjs_uint32 flag)
{
	tjs_uint32 access = flag & TJS_BS_ACCESS_MASK;
	const wchar_t* mode = L"rb";
	switch(access)
	{
	case TJS_BS_READ:
		mode = L"rb";		break;
	case TJS_BS_WRITE:
		mode = L"wb";		break;
	case TJS_BS_APPEND:
		mode = L"ab";		break;
	case TJS_BS_UPDATE:
		mode = L"rb+";		break;
	}

	tjs_int trycount = 0;
	Handle = _wfopen((const wchar_t*)path, mode );

	if (Handle && access == TJS_BS_APPEND) // move the file pointer to last
		fseek(Handle, 0, SEEK_END);

	return Handle != nullptr;
}
//---------------------------------------------------------------------------

tTVPLocalFileStream::~tTVPLocalFileStream()
{
	if(Handle!=nullptr) {
		fflush(Handle);
		fclose(Handle);
	}
}

//---------------------------------------------------------------------------
tjs_uint64 TJS_INTF_METHOD tTVPLocalFileStream::Seek(tjs_int64 offset, tjs_int whence)
{
	int dwmm;
	switch(whence)
	{
	case TJS_BS_SEEK_SET:	dwmm = SEEK_SET;	break;
	case TJS_BS_SEEK_CUR:	dwmm = SEEK_CUR;	break;
	case TJS_BS_SEEK_END:	dwmm = SEEK_END;	break;
	default:				dwmm = SEEK_SET;	break; // may be enough
	}

	if( _fseeki64( Handle, offset, dwmm ) )
		TVPThrowExceptionMessage(TVPSeekError);

	tjs_uint low = ftell(Handle );
	return low;
}
//---------------------------------------------------------------------------
tjs_uint TJS_INTF_METHOD tTVPLocalFileStream::Read(void *buffer, tjs_uint read_size)
{
	size_t ret = fread( buffer, 1, read_size, Handle );
	return (tjs_uint)ret;
}
//---------------------------------------------------------------------------
tjs_uint TJS_INTF_METHOD tTVPLocalFileStream::Write(const void *buffer, tjs_uint write_size)
{
	size_t ret = fwrite( buffer, 1, write_size, Handle );
	return (tjs_uint)ret;
}
//---------------------------------------------------------------------------
void TJS_INTF_METHOD tTVPLocalFileStream::SetEndOfStorage()
{
	if( fseek( Handle, 0, SEEK_END ) )
		TVPThrowExceptionMessage(TVPSeekError);
}
//---------------------------------------------------------------------------
tjs_uint64 TJS_INTF_METHOD tTVPLocalFileStream::GetSize()
{
	tjs_uint64 ret = 0;
	if (Handle) {
		struct __stat64 stbuf;
		if(_fstat64( fileno(Handle), &stbuf) != 0 ) {
			TVPThrowExceptionMessage(TVPSeekError);
		}
		ret = stbuf.st_size;
	}
	return ret;
}
//---------------------------------------------------------------------------

iTJSBinaryStream *
StdFileSystem::OpenStream(const tjs_char *path, const tjs_uint32 flags)
{
	tTVPLocalFileStream *stream = new tTVPLocalFileStream();
	if (stream->Open(path, flags)) {
		return stream;
	}
	delete stream;
	return 0;
}

void
StdFileSystem::CommitSavedata()
{
}

void
StdFileSystem::RollbackSavedata()
{
}

tjs_uint64
StdFileSystem::LastModifiedFileTime(const tjs_char *path)
{
	tjs_uint64 ret = 0;
	struct __stat64 stbuf;
	if(_wstat64( (const wchar_t*)path, &stbuf) != 0 ) {
		TVPThrowExceptionMessage(TVPSeekError);
	}
	ret = stbuf.st_mtime;
	// FILETIME 互換に変換
	ret += 11644473600LL; // 1970-01-01T00
	return ret;
}

tjs_uint64
StdFileSystem::FileSize(const tjs_char *path)
{
	struct __stat64 stbuf;
	if(_wstat64( (const wchar_t*)path, &stbuf) != 0 ) {
		return 0;
	}
	return static_cast<tjs_uint64>(stbuf.st_size);
}

iTVPLocalFileSystem *
TVPCreateLocalFileSystem()
{
	return new StdFileSystem();
}
