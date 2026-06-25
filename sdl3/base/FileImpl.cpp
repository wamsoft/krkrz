#include "tjsCommHead.h"
#include "CharacterSet.h"
#include "LogIntf.h"
#include <LocalFileSystem.h>

#include <functional>

#include <SDL3/SDL.h>
#include <SDL3/SDL_storage.h>

#include "app.h"

/**
 * 吉里吉里用ローカルファイルインターフェース 標準関数
 */
class SDL3FileSystem : public iTVPLocalFileSystem
{
public:
	SDL3FileSystem();
	virtual ~SDL3FileSystem();

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

SDL3FileSystem::SDL3FileSystem()
{
}

SDL3FileSystem::~SDL3FileSystem() 
{
}

/**
 * @brief ファイル名正規化
 * @return 正規化実行された
 */
bool
SDL3FileSystem::NormalizeStorageName(tjs_string &name)
{
	return SDL_NormalizeStorageName(name);
}

//< ファイル名のローカル化処理
void
SDL3FileSystem::GetLocallyAccessibleName(tjs_string &name)
{
	SDL_GetLocallyAccessibleName(name);
}

void
SDL3FileSystem::GetListAt(const tjs_char *name, std::function<void(const tjs_char *, bool isDir)> lister, bool withDir)
{
	if (SDL_GetListAt(name, lister, withDir)) return;

	// Convert tjs_char* to UTF-8 for SDL3
	std::string path_utf8;
	TVPUtf16ToUtf8(path_utf8, name);
	
	SDL_PathInfo pathInfo;
	if (SDL_GetPathInfo(path_utf8.c_str(), &pathInfo) && pathInfo.type == SDL_PATHTYPE_DIRECTORY) {
		SDL_GlobFlags flags = SDL_GLOB_CASEINSENSITIVE;
		char **files = SDL_GlobDirectory(path_utf8.c_str(), "*", flags, nullptr);
		if (files) {
			for (int i = 0; files[i]; ++i) {
				SDL_PathInfo fileInfo;
				std::string fullPath = path_utf8 + files[i];
				if (SDL_GetPathInfo(fullPath.c_str(), &fileInfo)) {
					bool isDir = (fileInfo.type == SDL_PATHTYPE_DIRECTORY);
					if (!isDir || (withDir && isDir)) {
						// Convert filename back to tjs_char
						tjs_string filename;
						TVPUtf8ToUtf16(filename, files[i]);
						//TVPLOG_DEBUG("SDL3FileSystem::GetListAt: {}", filename);
						lister(filename.c_str(), isDir);
					}
				}
			}
			SDL_free(files);
		}
	}




}

bool
SDL3FileSystem::RemoveDirectory(const tjs_char *path)
{
	// Convert tjs_char* to UTF-8 for SDL3
	std::string path_utf8;
	TVPUtf16ToUtf8(path_utf8, path);
	TVPLOG_DEBUG("Removing directory: {}", path_utf8);

	return SDL_RemovePath(path_utf8.c_str());
}

bool
SDL3FileSystem::MakeDirectory(const tjs_char *path)
{
	// Convert tjs_char* to UTF-8 for SDL3
	std::string path_utf8;
	TVPUtf16ToUtf8(path_utf8, path);
	TVPLOG_DEBUG("Creating directory: {}", path_utf8);
	
	return SDL_CreateDirectory(path_utf8.c_str());
}

bool
SDL3FileSystem::ExistentFile(const tjs_char *path)
{
	// Convert tjs_char* to UTF-8 for SDL3
	std::string path_utf8;
	TVPUtf16ToUtf8(path_utf8, path);
	//TVPLOG_DEBUG("Checking if file exists: {}", path_utf8);

	// path_utf8 の中に　& という文字が含まれている場合は低層で abort されるので false　を返す
	if (path_utf8.find('&') != std::string::npos) {
		TVPLOG_ERROR("Path contains invalid character '&': {}", path_utf8);
		return false;
	}

	SDL_PathInfo pathInfo;
	return SDL_GetPathInfo(path_utf8.c_str(), &pathInfo) && pathInfo.type == SDL_PATHTYPE_FILE;
}

bool
SDL3FileSystem::ExistentFolder(const tjs_char *path)
{
	// Convert tjs_char* to UTF-8 for SDL3
	std::string path_utf8;
	TVPUtf16ToUtf8(path_utf8, path);
	TVPLOG_DEBUG("Checking if folder exists: {}", path_utf8);

	SDL_PathInfo pathInfo;
	return SDL_GetPathInfo(path_utf8.c_str(), &pathInfo) && pathInfo.type == SDL_PATHTYPE_DIRECTORY;
}

bool
SDL3FileSystem::RemoveFile(const tjs_char *path)
{
	// Convert tjs_char* to UTF-8 for SDL3
	std::string path_utf8;
	TVPUtf16ToUtf8(path_utf8, path);
	TVPLOG_DEBUG("Removing file: {}", path_utf8);

	return SDL_RemovePath(path_utf8.c_str());
}

bool 
SDL3FileSystem::MoveFile(const tjs_char *fromFile, const tjs_char *toFile)
{
	// Convert paths to UTF-8 for SDL3
	std::string from_utf8, to_utf8;
	TVPUtf16ToUtf8(from_utf8, fromFile);
	TVPUtf16ToUtf8(to_utf8, toFile);
	TVPLOG_DEBUG("Moving file from {} to {}", from_utf8, to_utf8);

	return SDL_RenamePath(from_utf8.c_str(), to_utf8.c_str());
}

//---------------------------------------------------------------------------
// tTVPLocalFileStream
//---------------------------------------------------------------------------

class tTVPLocalFileStream : public iTJSBinaryStream
{
public:
	tTVPLocalFileStream();
	~tTVPLocalFileStream();

	bool Open(const char *path, tjs_uint32 flag);

	tjs_uint64 TJS_INTF_METHOD Seek(tjs_int64 offset, tjs_int whence);

	tjs_uint TJS_INTF_METHOD Read(void *buffer, tjs_uint read_size);
	tjs_uint TJS_INTF_METHOD Write(const void *buffer, tjs_uint write_size);

	void TJS_INTF_METHOD SetEndOfStorage();

	tjs_uint64 TJS_INTF_METHOD GetSize();

private:
	std::string file_path_utf8;
	SDL_IOStream *io_stream;
	bool is_read_mode;
	bool truncate;
	tjs_uint64 file_size;
	tjs_uint64 truncate_pos;
};

//---------------------------------------------------------------------------

tTVPLocalFileStream::tTVPLocalFileStream() : io_stream(nullptr), is_read_mode(false), truncate(false), file_size(0), truncate_pos(0)
{
}

bool
tTVPLocalFileStream::Open(const char *path, tjs_uint32 flag)
{
	tjs_uint32 access = flag & TJS_BS_ACCESS_MASK;
	
	const char* mode = "rb";
	switch(access)
	{
	case TJS_BS_READ:
		is_read_mode = true;
		mode = "rb";
		break;
	case TJS_BS_WRITE:
		is_read_mode = false;
		mode = "wb";
		break; 
	case TJS_BS_APPEND:
		is_read_mode = false;
		mode = "ab";
		break;
	case TJS_BS_UPDATE:
		is_read_mode = false;
		mode = "rb+";
		break;
	}
	
	TVPLOG_DEBUG("Opening file: {} with mode: {}", path, mode);
	io_stream = SDL_IOFromFile(path, mode);
	if (io_stream && is_read_mode) {
		// Get file size for read mode
		file_size = SDL_GetIOSize(io_stream);
	}
	
	return io_stream != nullptr;
}
//---------------------------------------------------------------------------

tTVPLocalFileStream::~tTVPLocalFileStream()
{
	if (io_stream) {
		SDL_CloseIO(io_stream);
		if (truncate) {
			// SDL3 doesn't have a direct file truncate function, so we need to use platform-specific code
			// or recreate the file with the truncated content
			// For now, we'll skip this functionality as it's rarely used
			// TODO: Implement file truncation if needed
		}
	}
}

//---------------------------------------------------------------------------
tjs_uint64 TJS_INTF_METHOD tTVPLocalFileStream::Seek(tjs_int64 offset, tjs_int whence)
{
	if (!io_stream) return 0;
	
	SDL_IOWhence sdl_whence;
	switch(whence)
	{
	case TJS_BS_SEEK_SET:	sdl_whence = SDL_IO_SEEK_SET; break;
	case TJS_BS_SEEK_CUR:	sdl_whence = SDL_IO_SEEK_CUR; break;
	case TJS_BS_SEEK_END:	sdl_whence = SDL_IO_SEEK_END; break;
	default:				sdl_whence = SDL_IO_SEEK_SET; break;
	}
	
	Sint64 result = SDL_SeekIO(io_stream, offset, sdl_whence);
	return (result >= 0) ? static_cast<tjs_uint64>(result) : 0;
}
//---------------------------------------------------------------------------
tjs_uint TJS_INTF_METHOD tTVPLocalFileStream::Read(void *buffer, tjs_uint read_size)
{
	if (!io_stream || !is_read_mode) return 0;
	
	size_t bytes_read = SDL_ReadIO(io_stream, buffer, read_size);
	return static_cast<tjs_uint>(bytes_read);
}
//---------------------------------------------------------------------------
tjs_uint TJS_INTF_METHOD tTVPLocalFileStream::Write(const void *buffer, tjs_uint write_size)
{
	if (!io_stream || is_read_mode) return 0;
	
	size_t bytes_written = SDL_WriteIO(io_stream, buffer, write_size);
	return static_cast<tjs_uint>(bytes_written);
}
//---------------------------------------------------------------------------
void TJS_INTF_METHOD tTVPLocalFileStream::SetEndOfStorage()
{
	if (!io_stream) return;
	
	truncate = true;
	if (is_read_mode) {
		truncate_pos = SDL_TellIO(io_stream);
		file_size = truncate_pos;
	} else {
		truncate_pos = SDL_TellIO(io_stream);
	}
}
//---------------------------------------------------------------------------
tjs_uint64 TJS_INTF_METHOD tTVPLocalFileStream::GetSize()
{
	if (!io_stream) return 0;
	
	if (truncate) {
		return truncate_pos;
	}
	if (is_read_mode) {
		return file_size;
	}
	
	// For write mode, get current size
	Sint64 current_pos = SDL_TellIO(io_stream);
	Sint64 size = SDL_GetIOSize(io_stream);
	return (size >= 0) ? static_cast<tjs_uint64>(size) : 0;
}
//---------------------------------------------------------------------------

iTJSBinaryStream *
CreateStreamFromSDL(const char *path, const tjs_uint32 flags)
{
	tTVPLocalFileStream *stream = new tTVPLocalFileStream();
	if (stream->Open(path, flags)) {
		return stream;
	}
	delete stream;
	return 0;
}


iTJSBinaryStream *
SDL3FileSystem::OpenStream(const tjs_char *path, const tjs_uint32 flags)
{
	std::string file_path_utf8;
	TVPUtf16ToUtf8(file_path_utf8, path);
	iTJSBinaryStream *stream = SDL_OpenStream(file_path_utf8.c_str(), flags);
	if (stream) return stream;
	return CreateStreamFromSDL(file_path_utf8.c_str(), flags);
}

void
SDL3FileSystem::CommitSavedata()
{
	if (!SDL_CommitSavedata()) {
		auto msg = SDL_GetError();
		TVPLOG_ERROR("Failed to commit save data:{}", msg);
	}
}

void
SDL3FileSystem::RollbackSavedata()
{
	if (!SDL_RollbackSavedata()) {
		auto msg = SDL_GetError();
		TVPLOG_ERROR("Failed to rollback save data:{}", msg);
	}
}

tjs_uint64
SDL3FileSystem::LastModifiedFileTime(const tjs_char *path)
{
	std::string file_path_utf8;
	TVPUtf16ToUtf8(file_path_utf8, path);
	SDL_PathInfo pathInfo;
	if (SDL_GetPathInfo(file_path_utf8.c_str(), &pathInfo) && pathInfo.type == SDL_PATHTYPE_FILE) {
		constexpr tjs_uint64 UNIX_EPOCH_IN_100NS = 11644473600000000000ULL; // 100-ns intervals from 1601-01-01 to 1970-01-01
		tjs_uint64 mod_time_100ns = static_cast<tjs_uint64>(pathInfo.modify_time) * 10000000ULL + UNIX_EPOCH_IN_100NS;
		return mod_time_100ns;
	}
	return 0;
}

tjs_uint64
SDL3FileSystem::FileSize(const tjs_char *path)
{
	std::string file_path_utf8;
	TVPUtf16ToUtf8(file_path_utf8, path);
	SDL_PathInfo pathInfo;
	if (SDL_GetPathInfo(file_path_utf8.c_str(), &pathInfo) && pathInfo.type == SDL_PATHTYPE_FILE) {
		return static_cast<tjs_uint64>(pathInfo.size);
	}
	return 0;
}

iTVPLocalFileSystem *
TVPCreateLocalFileSystem()
{
	return new SDL3FileSystem();
}