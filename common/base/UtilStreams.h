//---------------------------------------------------------------------------
/*
	TVP2 ( T Visual Presenter 2 )  A script authoring tool
	Copyright (C) 2000 W.Dee <dee@kikyou.info> and contributors

	See details of license at "license.txt"
*/
//---------------------------------------------------------------------------
// Stream related utilities / utility streams
//---------------------------------------------------------------------------
#ifndef UtilStreamsH
#define UtilStreamsH


#include "StorageIntf.h"



//---------------------------------------------------------------------------
// tTVPStreamHolder
//---------------------------------------------------------------------------
class tTVPStreamHolder
{
	iTJSBinaryStream *Stream;
public:
	tTVPStreamHolder()
	{
		Stream = NULL;
	}

	tTVPStreamHolder(const ttstr& name, tjs_uint32 mode = 0)
	{
		Stream = TVPCreateStream(name, mode);
	}

	~tTVPStreamHolder()
	{
		Close();
	}

	iTJSBinaryStream * operator ->() const { return Stream; }

	iTJSBinaryStream * Get() const { return Stream; }

	void Close()
	{
		if(Stream)
		{
			Stream->Destruct();
			Stream = NULL;
		}
	}

	void Disown()
	{
		Stream = NULL;
	}

	void Open(const ttstr & name, tjs_uint32 flag = 0)
	{
		if(Stream) delete Stream, Stream = NULL;
		Stream = TVPCreateStream(name, flag);
	}
};
//---------------------------------------------------------------------------







//---------------------------------------------------------------------------
// tTVPLocalTempStorageHolder
//---------------------------------------------------------------------------
// this class holds storage as local filesystem file ( does not open it ).
// storage is copied to local fliesystem if needed.
class tTVPLocalTempStorageHolder
{
	bool FileMustBeDeleted;
	bool FolderMustBeDeleted;
	ttstr LocalName;
	ttstr LocalFolder;
public:
	tTVPLocalTempStorageHolder(const ttstr & name);
	~tTVPLocalTempStorageHolder();

	bool IsTemporaryFile() const { return FileMustBeDeleted; }

	const ttstr & GetLocalName() const { return LocalName; }
};
//---------------------------------------------------------------------------







//---------------------------------------------------------------------------
// tTVPMemoryStream
//---------------------------------------------------------------------------
/*
	this class provides a iTJSBinaryStream based access method for a memory block.
*/
class tTVPMemoryStream : public iTJSBinaryStream
{
protected:
	void * Block;
	bool Reference;
	tjs_uint Size;
	tjs_uint AllocSize;
	tjs_uint CurrentPos;

public:
	tTVPMemoryStream();
	explicit tTVPMemoryStream(tjs_uint reserve_size);
	tTVPMemoryStream(const void * block, tjs_uint size);
	~tTVPMemoryStream();

	tjs_uint64 TJS_INTF_METHOD Seek(tjs_int64 offset, tjs_int whence);

	tjs_uint TJS_INTF_METHOD Read(void *buffer, tjs_uint read_size);
	tjs_uint TJS_INTF_METHOD Write(const void *buffer, tjs_uint write_size);
	void TJS_INTF_METHOD SetEndOfStorage();

	tjs_uint64 TJS_INTF_METHOD GetSize() { return Size; }

	// non-iTJSBinaryStream based methods
	void * GetInternalBuffer()  const { return Block; }
	void Clear(void);
	void SetSize(tjs_uint size);

protected:
	void Init();

protected:
	virtual void * Alloc(size_t size);
	virtual void * Realloc(void *orgblock, size_t size);
	virtual void Free(void *block);
};
//---------------------------------------------------------------------------





//---------------------------------------------------------------------------
// tTVPPartialStream
//---------------------------------------------------------------------------
/*
	this class offers read-only accesses for a partial of the other stream,
	limited by given start position for original stream and given limited size.
*/
//---------------------------------------------------------------------------
class tTVPPartialStream : public iTJSBinaryStream
{
private:
	iTJSBinaryStream *Stream;
	tjs_uint64 Start;
	tjs_uint64 Size;
	tjs_uint64 CurrentPos;

public:
	tTVPPartialStream(iTJSBinaryStream *stream, tjs_uint64 start, tjs_uint64 size);
	~tTVPPartialStream();

	tjs_uint64 TJS_INTF_METHOD Seek(tjs_int64 offset, tjs_int whence);
	tjs_uint TJS_INTF_METHOD Read(void *buffer, tjs_uint read_size);
	tjs_uint TJS_INTF_METHOD Write(const void *buffer, tjs_uint write_size);

	// void SetEndOfStorage(); // use default behavior

	tjs_uint64 TJS_INTF_METHOD GetSize();
};
//---------------------------------------------------------------------------



#endif
