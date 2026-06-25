#include "tjsCommHead.h"
#include <LocalFileSystem.h>
#include <chrono>
#include <functional>
#include <filesystem>
#include <fstream>

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

	if (namelen >= 5 && name.substr(0, 5) == TJS_W("file:"))
	{
		// すでに既定のパス
		return false;
	}

	std::filesystem::path path = name.c_str();
	if (path.is_absolute()) {
		// Windows drive:path expression
		tjs_string newname(TJS_W("file://./"));
		name = newname + name;
		return true;
	}

	return false;
}

//< ファイル名のローカル化処理
void
StdFileSystem::GetLocallyAccessibleName(tjs_string &name)
{
	const tjs_char *ptr = name.c_str();
	if (ptr[0] == '.' && ptr[1] == '/') {
		if (ptr[3] == ':') {
			name = ptr + 2;
		} else {
			name = ptr + 1;
		}
	}
}

void
StdFileSystem::GetListAt(const tjs_char *name, std::function<void(const tjs_char *, bool isDir)> lister, bool withDir)
{
	if (std::filesystem::is_directory(name)) {
		for (auto x : std::filesystem::directory_iterator(name)) {
			bool isDir = x.is_directory();
			if (x.is_regular_file() || (withDir && isDir)) {
#ifdef _WIN32
				lister((const tjs_char *)x.path().filename().wstring().c_str(), isDir);
#else
				lister(x.path().filename().u16string().c_str(), isDir);
#endif
			}
		}
	}
}

bool
StdFileSystem::RemoveDirectory(const tjs_char *path)
{
	std::error_code err;
    return std::filesystem::remove_all(path, err);
}

bool
StdFileSystem::MakeDirectory(const tjs_char *path)
{
	std::error_code err;
    return std::filesystem::create_directory(path, err);
}

bool
StdFileSystem::ExistentFile(const tjs_char *path)
{
	std::error_code err;
	return std::filesystem::exists(path, err);
}

bool
StdFileSystem::ExistentFolder(const tjs_char *path)
{
	std::error_code err;
	return std::filesystem::is_directory(path);
}

bool
StdFileSystem::RemoveFile(const tjs_char *path)
{
	bool r = false;
	if (*path) {
		std::error_code err;
		r = std::filesystem::remove(path, err);
	}
	return r;
}

bool 
StdFileSystem::MoveFile(const tjs_char *fromFile, const tjs_char *toFile)
{
	bool r = false;
	if (*fromFile && *toFile) {
		std::error_code err;
		std::filesystem::rename(fromFile, toFile, err);
		r = !err;
	}
	return r;
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
	std::filesystem::path fs_path;
	std::fstream fs;
	bool fs_is_read;
	bool truncate;
	std::streampos fs_size;
	std::streampos truncate_pos;
};

//---------------------------------------------------------------------------

tTVPLocalFileStream::tTVPLocalFileStream() : truncate(false), truncate_pos(0)
{
}

bool
tTVPLocalFileStream::Open(const tjs_char *path, tjs_uint32 flag)
{
	fs_path = path;
	tjs_uint32 access = flag & TJS_BS_ACCESS_MASK;

	std::ios_base::openmode mode = std::ios_base::binary;

	switch(access)
	{
	case TJS_BS_READ:
		fs_is_read = true;
		mode |= std::ios_base::in ; break;
	case TJS_BS_WRITE:
		fs_is_read = false;
		mode |= std::ios_base::out | std::ios::trunc; break; 
	case TJS_BS_APPEND:
		fs_is_read = false;
		mode |= std::ios_base::out | std::ios_base::app; break;
	case TJS_BS_UPDATE:
		fs_is_read = false;
		mode |= std::ios_base::out; break;
	}
	if (fs_is_read) {
		fs_size = std::filesystem::file_size(fs_path);
	}
	fs.open(fs_path, mode);
	return fs.is_open();
}
//---------------------------------------------------------------------------

tTVPLocalFileStream::~tTVPLocalFileStream()
{
	if (fs.is_open()) {
		fs.close();
		if (truncate) {
			std::filesystem::resize_file(fs_path, truncate_pos);
		}
	}
}

//---------------------------------------------------------------------------
tjs_uint64 TJS_INTF_METHOD tTVPLocalFileStream::Seek(tjs_int64 offset, tjs_int whence)
{
	std::fstream::seekdir dir;
	switch(whence)
	{
	case TJS_BS_SEEK_SET:	dir = std::ios_base::beg; break;
	case TJS_BS_SEEK_CUR:	dir = std::ios_base::cur; break;
	case TJS_BS_SEEK_END:	dir = std::ios_base::end; break;
	default:				dir = std::ios_base::beg; break; // may be enough
	}
	if (fs_is_read) {
		fs.seekg(offset, dir);
		return fs.tellg();
	} else {
		fs.seekp(offset, dir);
		return fs.tellp();
	}
}
//---------------------------------------------------------------------------
tjs_uint TJS_INTF_METHOD tTVPLocalFileStream::Read(void *buffer, tjs_uint read_size)
{
	if (fs_is_read) {
		fs.read((char*)buffer, read_size);
		return (tjs_uint)fs.gcount();
	}
	return 0;
}
//---------------------------------------------------------------------------
tjs_uint TJS_INTF_METHOD tTVPLocalFileStream::Write(const void *buffer, tjs_uint write_size)
{
	if (!fs_is_read) {
		auto before = fs.tellp();
		fs.write((char*)buffer, write_size);
		return (tjs_uint)(fs.tellp() - before);
	}
	return 0;
}
//---------------------------------------------------------------------------
void TJS_INTF_METHOD tTVPLocalFileStream::SetEndOfStorage()
{
	truncate = true;
	if (fs_is_read) {
		truncate_pos = fs_size = fs.tellg();
	} else {
		truncate_pos = fs.tellp();
	}
}
//---------------------------------------------------------------------------
tjs_uint64 TJS_INTF_METHOD tTVPLocalFileStream::GetSize()
{
	if (truncate) {
		return truncate_pos;
	}
	if (fs_is_read) {
		return fs_size;
	}
	std::streampos pos = fs.tellp();
	fs.seekp(0, std::ios_base::end);
	std::streampos ret = fs.tellp();
	fs.seekp(pos, std::ios_base::beg);
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
	std::error_code err;
	auto ftime = std::filesystem::last_write_time(path, err);
	if (err) {
		return static_cast<tjs_uint64>(-1);
	}

	// Convert file_clock time to system_clock time by comparing each clock's "now".
	// This absorbs implementation-dependent epoch/duration differences.
	auto systime = std::chrono::system_clock::now() + (ftime - std::filesystem::file_time_type::clock::now());
	const auto unix_100ns = std::chrono::duration_cast<std::chrono::duration<tjs_int64, std::ratio<1, 10000000>>>(
		systime.time_since_epoch()).count();

	// FILETIME: 100ns intervals since 1601-01-01 UTC.
	return static_cast<tjs_uint64>(unix_100ns + 116444736000000000LL);
}

tjs_uint64
StdFileSystem::FileSize(const tjs_char *path)
{
	std::error_code err;
	auto sz = std::filesystem::file_size(path, err);
	if (err) {
		return 0;
	}
	return static_cast<tjs_uint64>(sz);
}

iTVPLocalFileSystem *
TVPCreateLocalFileSystem()
{
	return new StdFileSystem();
}