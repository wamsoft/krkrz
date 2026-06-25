//---------------------------------------------------------------------------
/*
	TVP2 ( T Visual Presenter 2 )  A script authoring tool
	Copyright (C) 2000 W.Dee <dee@kikyou.info> and contributors

	See details of license at "license.txt"
*/
//---------------------------------------------------------------------------
// Text read/write stream
//---------------------------------------------------------------------------
#include "tjsCommHead.h"

#include <zlib.h>

#include "TextStream.h"
#include "MsgIntf.h"
#include "DebugIntf.h"
#include "EventIntf.h"
#include "UtilStreams.h"
#include "tjsError.h"


/*
	Text stream is used by TJS's Array.saveStruct, Dictionary.saveStruct etc.
	to input/output binary files.
*/

//---------------------------------------------------------------------------
iTJSBinaryStream * TVPCreateBinaryStreamForRead(const ttstr & name,
	const ttstr & modestr)
{
	// check o mode
	iTJSBinaryStream* stream = TVPCreateStream(name, TJS_BS_READ);

	const tjs_char* o_ofs = TJS_strchr(modestr.c_str(), TJS_W('o'));
	if(o_ofs != NULL) {
		// seek to offset
		o_ofs++;
		tjs_char buf[256];
		int i;
		for(i = 0; i < 255; i++) {
			if(o_ofs[i] >= TJS_W('0') && o_ofs[i] <= TJS_W('9'))
				buf[i] = o_ofs[i];
			else break;
		}
		buf[i] = 0;
		tjs_uint64 ofs = ttstr(buf).AsInteger();
		TVPStreamSetPosition(stream, ofs);
	}
	return stream;
}
//---------------------------------------------------------------------------
iTJSBinaryStream * TVPCreateBinaryStreamForWrite(const ttstr & name,
	const ttstr & modestr)
{
	iTJSBinaryStream* stream;
	// check o mode
	const tjs_char * o_ofs;
	o_ofs = TJS_strchr(modestr.c_str(), TJS_W('o'));
	if(o_ofs != NULL) {
		// seek to offset
		o_ofs++;
		tjs_char buf[256];
		int i;
		for(i = 0; i < 255; i++) {
			if(o_ofs[i] >= TJS_W('0') && o_ofs[i] <= TJS_W('9'))
				buf[i] = o_ofs[i];
			else break;
		}
		buf[i] = 0;
		tjs_uint64 ofs = ttstr(buf).AsInteger();
		stream = TVPCreateStream(name, TJS_BS_UPDATE);
		TVPStreamSetPosition(stream, ofs);
	} else {
		stream = TVPCreateStream(name, TJS_BS_WRITE);
	}
	return stream;
}




