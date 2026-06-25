#pragma once

#include <functional>

/**
 * @brief ローカルファイル操作用抽象インターフェース
 * 環境毎に実装が必要
 */
class iTVPLocalFileSystem {
public:
	virtual bool NormalizeStorageName(tjs_string &name) = 0;
	virtual void GetLocallyAccessibleName(tjs_string &name) = 0;
	virtual void GetListAt(const tjs_char *name, std::function<void(const tjs_char *, bool isDir)> lister, bool withDir) = 0;
	virtual bool RemoveDirectory(const tjs_char *path) = 0;
	virtual bool MakeDirectory(const tjs_char *path) = 0;
	virtual bool ExistentFile(const tjs_char *path) = 0;
	virtual bool ExistentFolder(const tjs_char *path) = 0;
	virtual bool RemoveFile(const tjs_char *path) = 0;
	virtual bool MoveFile(const tjs_char *fromFile, const tjs_char *toFile) = 0;
	virtual iTJSBinaryStream *OpenStream(const tjs_char *path, const tjs_uint32 flags) = 0;
	virtual void CommitSavedata() = 0;
	virtual void RollbackSavedata() = 0;
	virtual tjs_uint64 LastModifiedFileTime(const tjs_char *path) = 0;
	virtual tjs_uint64 FileSize(const tjs_char *path) = 0;
};

extern iTVPLocalFileSystem* TVPCreateLocalFileSystem();
