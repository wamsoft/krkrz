//---------------------------------------------------------------------------
/*
	TVP2 ( T Visual Presenter 2 )  A script authoring tool
	Copyright (C) 2000 W.Dee <dee@kikyou.info> and contributors

	See details of license at "license.txt"
*/
//---------------------------------------------------------------------------
// Graphics Loader ( loads graphic format from storage )
//---------------------------------------------------------------------------

#include "tjsCommHead.h"

#include <stdlib.h>
#include "GraphicsLoaderIntf.h"
#include "LayerBitmapIntf.h"
#include "LayerIntf.h"
#include "StorageIntf.h"
#include "MsgIntf.h"
#include "tjsHashSearch.h"
#include "EventIntf.h"
#include "SysInitIntf.h"
#include "DebugIntf.h"
#include "LogIntf.h"
#include "AllocTagScope.h"
#include "tvpgl.h"
#include "TickCount.h"
//#include "DetectCPU.h"
#include "UtilStreams.h"
#include "tjsDictionary.h"
#include "ScriptMgnIntf.h"
#include "GraphicsLoadThread.h"
#include <cstdlib>
#include <cmath>
#include "StorageImpl.h"
#include "StorageCache.h"  // P3: file→decode auto-drop の TVPClearStorageCache 用

//---------------------------------------------------------------------------

void tTVPGraphicHandlerType::Load( void* formatdata, void *callbackdata, tTVPGraphicSizeCallback sizecallback, tTVPGraphicScanLineCallback scanlinecallback,
	tTVPMetaInfoPushCallback metainfopushcallback, iTJSBinaryStream *src, tjs_int32 keyidx, tTVPGraphicLoadMode mode)
{
	if( LoadHandler == NULL ) TVPThrowExceptionMessage(TVPUnknownGraphicFormat, TJS_W("unknown"));

#ifdef __WINVER__
	if( IsPlugin )
	{
		tTVPIStreamAdapter *istream = new tTVPIStreamAdapter(src);
		try {
			LoadHandlerPlugin( formatdata, callbackdata, sizecallback, scanlinecallback, metainfopushcallback,
				istream, keyidx, mode);
		} catch(...) {
			istream->ClearStream();
			istream->Release();
			throw;
		}
		istream->ClearStream();
		istream->Release();
	}
	else
#endif
	{
		LoadHandler( formatdata, callbackdata, sizecallback, scanlinecallback, metainfopushcallback,
			src, keyidx, mode);
	}
}
void tTVPGraphicHandlerType::Save( const ttstr & storagename, const ttstr & mode, const tTVPBaseBitmap* image, iTJSDispatch2* meta )
{
	if( SaveHandler == NULL ) TVPThrowExceptionMessage(TVPUnknownGraphicFormat, mode );

	// cache 駆逐は _TVPCreateStream(WRITE) 側で対象 path を両層 evict する
	// 形に集約済 (StorageIntf.cpp)。ここで個別に呼ぶ必要なし。
	iTJSBinaryStream *stream = TVPCreateStream(TVPNormalizeStorageName(storagename), TJS_BS_WRITE);
#ifdef __WINVER__
	if( IsPlugin )
	{
		tTVPIStreamAdapter *istream = new tTVPIStreamAdapter(stream);
		try {
			tjs_uint h = image->GetHeight();
			tjs_uint w = image->GetWidth();
			SaveHandlerPlugin( FormatData, (void*)image, istream, mode, w, h, tTVPBitmapScanLineCallbackForSave, meta );
		} catch(...) {
			istream->Release();
			throw;
		}
		istream->Release();
	}
	else
#endif
	{
		try {
			SaveHandler( FormatData, stream, image, mode, meta );
		} catch(...) {
			delete stream;
			throw;
		}
		delete stream;
	}
}
void tTVPGraphicHandlerType::Header( iTJSBinaryStream *src, iTJSDispatch2** dic )
{
	if( HeaderHandler == NULL ) TVPThrowExceptionMessage(TVPUnknownGraphicFormat, TJS_W("unknown") );

#ifdef __WINVER__
	if( IsPlugin )
	{
		tTVPIStreamAdapter *istream = new tTVPIStreamAdapter(src);
		try {
			HeaderHandlerPlugin( FormatData, istream, dic );
		} catch(...) {
			istream->ClearStream();
			istream->Release();
			throw;
		}
		istream->ClearStream();
		istream->Release();
	}
	else
#endif
	{
		HeaderHandler( FormatData, src, dic );
	}
}

//---------------------------------------------------------------------------

bool TVPAcceptSaveAsBMP( void* formatdata, const ttstr & type, class iTJSDispatch2** dic )
{
	bool result = false;
	if( type.StartsWith(TJS_W("bmp")) ) result = true;
	else if( type == TJS_W(".bmp") ) result = true;
	else if( type == TJS_W(".dib") ) result = true;
	if( result && dic ) {
		tTJSVariant result;
		TVPExecuteExpression(
			TJS_W("(const)%[")
			TJS_W("\"bpp\"=>(const)%[\"type\"=>\"select\",\"items\"=>(const)[\"32\",\"24\",\"8\"],\"desc\"=>\"bpp\",\"default\"=>0]")
			TJS_W("]"),
			NULL, &result );
		if( result.Type() == tvtObject ) {
			*dic = result.AsObject();
		}
		//*dic = TJSCreateDictionaryObject();
	}
	return result;
}
//---------------------------------------------------------------------------
// Graphics Format Management
//---------------------------------------------------------------------------

class tTVPGraphicType
{
public:
	tTJSHashTable<ttstr, tTVPGraphicHandlerType> Hash;
	std::vector<tTVPGraphicHandlerType> Handlers;

	static bool Avail;

	tTVPGraphicType()
	{
		// register some native-supported formats
		Handlers.push_back(tTVPGraphicHandlerType(
			TJS_W(".bmp"), TVPLoadBMP, TVPLoadHeaderBMP, TVPSaveAsBMP, TVPAcceptSaveAsBMP, NULL));
		Handlers.push_back(tTVPGraphicHandlerType(
			TJS_W(".dib"), TVPLoadBMP, TVPLoadHeaderBMP, TVPSaveAsBMP, TVPAcceptSaveAsBMP, NULL));
		Handlers.push_back(tTVPGraphicHandlerType(
			TJS_W(".jpeg"), TVPLoadJPEG, TVPLoadHeaderJPG, TVPSaveAsJPG, TVPAcceptSaveAsJPG, NULL));
		Handlers.push_back(tTVPGraphicHandlerType(
			TJS_W(".jpg"), TVPLoadJPEG, TVPLoadHeaderJPG, TVPSaveAsJPG, TVPAcceptSaveAsJPG, NULL));
		Handlers.push_back(tTVPGraphicHandlerType(
			TJS_W(".jif"), TVPLoadJPEG, TVPLoadHeaderJPG, TVPSaveAsJPG, TVPAcceptSaveAsJPG, NULL));
		Handlers.push_back(tTVPGraphicHandlerType(
			TJS_W(".png"), TVPLoadPNG, TVPLoadHeaderPNG, TVPSaveAsPNG, TVPAcceptSaveAsPNG, NULL));
		Handlers.push_back(tTVPGraphicHandlerType(
			TJS_W(".tlg"), TVPLoadTLG, TVPLoadHeaderTLG, TVPSaveAsTLG, TVPAcceptSaveAsTLG, NULL));
		Handlers.push_back(tTVPGraphicHandlerType(
			TJS_W(".tlg5"), TVPLoadTLG, TVPLoadHeaderTLG, TVPSaveAsTLG, TVPAcceptSaveAsTLG, NULL));
		Handlers.push_back(tTVPGraphicHandlerType(
			TJS_W(".tlg6"), TVPLoadTLG, TVPLoadHeaderTLG, TVPSaveAsTLG, TVPAcceptSaveAsTLG, NULL));
		// JPEG XR (.jxr) は WINVER の OS WIC コーデック依存の WINVER 専用対応だった
		// が、旧式・低採用の Microsoft 独自フォーマットで移植性も無いため撤去した。
		ReCreateHash();
		Avail = true;
	}

	~tTVPGraphicType()
	{
		Avail = false;
	}

	void ReCreateHash()
	{
		// re-create hash table for faster search

		std::vector<tTVPGraphicHandlerType>::iterator i;
		for(i = Handlers.begin();
			i!= Handlers.end(); i++)
		{
			Hash.Add(i->Extension, *i);
		}
	}

	void Register( const tTVPGraphicHandlerType& hander )
	{
		// register graphic format to the table.
		Handlers.push_back(hander);
		ReCreateHash();
	}

	void Unregister( const tTVPGraphicHandlerType& hander )
	{
		// unregister format from table.

		std::vector<tTVPGraphicHandlerType>::iterator i;

		if(Handlers.size() > 0)
		{
			//for(i = Handlers.end() -1; i >= Handlers.begin(); i--)
			for(i = Handlers.begin(); i != Handlers.end(); i++)
			{
				if(hander == *i)
				{
					Handlers.erase(i);
					break;
				}
			}
		}

		ReCreateHash();
	}

} static TVPGraphicType;
bool tTVPGraphicType::Avail = false;
//---------------------------------------------------------------------------
void TVPRegisterGraphicLoadingHandler(const ttstr & name,
	tTVPGraphicLoadingHandler loading,
	tTVPGraphicHeaderLoadingHandler header,
	tTVPGraphicSaveHandler save,
	tTVPGraphicAcceptSaveHandler accept,
	void * formatdata)
{
	// name must be un-capitalized
	if(TVPGraphicType.Avail)
	{
		TVPGraphicType.Register(tTVPGraphicHandlerType(name, loading, header, save, accept, formatdata));
	}
}
//---------------------------------------------------------------------------
void TVPUnregisterGraphicLoadingHandler(const ttstr & name,
	tTVPGraphicLoadingHandler loading,
	tTVPGraphicHeaderLoadingHandler header,
	tTVPGraphicSaveHandler save,
	tTVPGraphicAcceptSaveHandler accept,
	void * formatdata)
{
	// name must be un-capitalized
	if(TVPGraphicType.Avail)
	{
		TVPGraphicType.Unregister(tTVPGraphicHandlerType(name, loading, header, save, accept, formatdata));
	}
}
#ifdef __WINVER__
//---------------------------------------------------------------------------
void TVPRegisterGraphicLoadingHandler(const ttstr & name,
	tTVPGraphicLoadingHandlerForPlugin loading,
	tTVPGraphicHeaderLoadingHandlerForPlugin header,
	tTVPGraphicSaveHandlerForPlugin save,
	tTVPGraphicAcceptSaveHandler accept,
	void* formatdata)
{
	// name must be un-capitalized
	if(TVPGraphicType.Avail)
	{
		TVPGraphicType.Register(tTVPGraphicHandlerType(name, loading, header, save, accept, formatdata));
	}
}
//---------------------------------------------------------------------------
void TVPUnregisterGraphicLoadingHandler(const ttstr & name,
	tTVPGraphicLoadingHandlerForPlugin loading,
	tTVPGraphicHeaderLoadingHandlerForPlugin header,
	tTVPGraphicSaveHandlerForPlugin save,
	tTVPGraphicAcceptSaveHandler accept,
	void* formatdata)
{
	// name must be un-capitalized
	if(TVPGraphicType.Avail)
	{
		TVPGraphicType.Unregister(tTVPGraphicHandlerType(name, loading, header, save, accept, formatdata));
	}
}
#endif
//---------------------------------------------------------------------------
tTVPGraphicHandlerType* TVPGetGraphicLoadHandler( const ttstr& ext )
{
	return TVPGraphicType.Hash.Find(ext);
}
/*
	loading handlers return whether the image contains an alpha channel.
*/
//---------------------------------------------------------------------------
const void* tTVPBitmapScanLineCallbackForSave(void *callbackdata, tjs_int y)
{
	tTVPBaseBitmap* image = (tTVPBaseBitmap*)callbackdata;
	return image->GetScanLine(y);
}
//---------------------------------------------------------------------------
void TVPLoadImageHeader( const ttstr & storagename, iTJSDispatch2** dic )
{
	if( dic == NULL ) return;

	ttstr ext = TVPExtractStorageExt(storagename);
	if(ext == TJS_W("")) TVPThrowExceptionMessage(TVPUnknownGraphicFormat, storagename);
	tTVPGraphicHandlerType * handler = TVPGraphicType.Hash.Find(ext);
	if(!handler) TVPThrowExceptionMessage(TVPUnknownGraphicFormat, storagename);

	tTVPStreamHolder holder(storagename); // open a storage named "storagename"
	handler->Header( holder.Get(), dic );
}
//---------------------------------------------------------------------------
void TVPSaveImage( const ttstr & storagename, const ttstr & mode, const tTVPBaseBitmap* image, iTJSDispatch2* meta )
{
	if(!image->Is32BPP())
		TVPThrowInternalError;

	tTVPGraphicHandlerType * handler;
	tTJSHashTable<ttstr, tTVPGraphicHandlerType>::tIterator i;
	for(i = TVPGraphicType.Hash.GetFirst(); !i.IsNull(); i++)
	{
		handler = & i.GetValue();
		if( handler->AcceptSave( mode, NULL ) )
		{
			break;
		}
		else
		{
			handler = NULL;
		}
	}
	if( handler ) handler->Save( storagename, mode, image, meta );
	else TVPThrowExceptionMessage(TVPUnknownGraphicFormat, mode);
}
//---------------------------------------------------------------------------
bool TVPGetSaveOption( const ttstr & type, iTJSDispatch2** dic )
{
	tTVPGraphicHandlerType * handler;
	tTJSHashTable<ttstr, tTVPGraphicHandlerType>::tIterator i;
	for(i = TVPGraphicType.Hash.GetFirst(); !i.IsNull(); i++ )
	{
		handler = & i.GetValue();
		if( handler->AcceptSave( type, dic ) )
		{
			return true;
		}
	}
	return false;
}
//---------------------------------------------------------------------------


//---------------------------------------------------------------------------
// BMP loading handler
//---------------------------------------------------------------------------

//---------------------------------------------------------------------------
#define TVP_BMP_READ_LINE_MAX 8
void TVPInternalLoadBMP(void *callbackdata,
	tTVPGraphicSizeCallback sizecallback,
	tTVPGraphicScanLineCallback scanlinecallback,
	TVP_WIN_BITMAPINFOHEADER &bi,
	const tjs_uint8 *palsrc,
	iTJSBinaryStream * src,
	tjs_int keyidx,
	tTVPBMPAlphaType alphatype,
	tTVPGraphicLoadMode mode)
{
	// mostly taken ( but totally re-written ) from SDL,
	// http://www.libsdl.org/

	// TODO: only checked on Win32 platform


	if(bi.biSize == 12)
	{
		// OS/2
		bi.biCompression = BI_RGB;
		bi.biClrUsed = 1 << bi.biBitCount;
	}

	tjs_uint16 orgbitcount = bi.biBitCount;
	if(bi.biBitCount == 1 || bi.biBitCount == 4)
	{
		bi.biBitCount = 8;
	}

	switch(bi.biCompression)
	{
	case BI_RGB:
		// if there are no masks, use the defaults
		break; // use default
/*
		if( bf.bfOffBits == ( 14 + bi.biSize) )
		{
		}
		// fall through -- read the RGB masks
*/
	case BI_BITFIELDS:
		TVPThrowExceptionMessage(TVPImageLoadError, (const tjs_char*)TVPBitFieldsNotSupported );

	default:
 		TVPThrowExceptionMessage(TVPImageLoadError, (const tjs_char*)TVPCompressedBmpNotSupported );
	}

	// load palette
	tjs_uint32 palette[256];   // (msb) argb (lsb)
	if(orgbitcount <= 8)
	{
		if(bi.biClrUsed == 0) bi.biClrUsed = 1 << orgbitcount ;
		if(bi.biSize == 12)
		{
			// read OS/2 palette
			for(tjs_uint i = 0; i < bi.biClrUsed; i++)
			{
				palette[i] = palsrc[0] + (palsrc[1]<<8) + (palsrc[2]<<16) +
					0xff000000;
				palsrc += 3;
			}
		}
		else
		{
			// read Windows palette
			for(tjs_uint i = 0; i<bi.biClrUsed; i++)
			{
				palette[i] = palsrc[0] + (palsrc[1]<<8) + (palsrc[2]<<16) +
					0xff000000;
					// we assume here that the palette's unused segment is useless.
					// fill it with 0xff ( = completely opaque )
				palsrc += 4;
			}
		}

		if(mode == glmGrayscale)
		{
			TVPDoGrayScale(palette, 256);
		}

		if(keyidx != -1)
		{
			// if color key by palette index is specified
			palette[keyidx&0xff] &= 0x00ffffff; // make keyidx transparent
		}
	}
	else
	{
		if(mode == glmPalettized)
			TVPThrowExceptionMessage(TVPImageLoadError, (const tjs_char*)TVPUnsupportedColorModeForPalettImage );
	}

	tjs_int height;
	height = bi.biHeight<0?-bi.biHeight:bi.biHeight;
		// positive value of bi.biHeight indicates top-down DIB

	sizecallback(callbackdata, bi.biWidth, height);

	tjs_int pitch;
	pitch = (((bi.biWidth * orgbitcount) + 31) & ~31) /8;
	tjs_uint8 *readbuf = (tjs_uint8 *)TJSAlignedAlloc(pitch * TVP_BMP_READ_LINE_MAX, 4);
	tjs_uint8 *buf;
	tjs_int bufremain = 0;
	try
	{
		// process per a line
		tjs_int src_y = 0;
		tjs_int dest_y;
		if(bi.biHeight>0) dest_y = bi.biHeight-1; else dest_y = 0;

		for(; src_y < height; src_y++)
		{
			if(bufremain == 0)
			{
				tjs_int remain = height - src_y;
				tjs_int read_lines = remain > TVP_BMP_READ_LINE_MAX ?
					TVP_BMP_READ_LINE_MAX : remain;
				TVPReadBuffer(src, readbuf, pitch * read_lines);
				bufremain = read_lines;
				buf = readbuf;
			}

			void *scanline = scanlinecallback(callbackdata, dest_y);
			if(!scanline) break;

			switch(orgbitcount)
			{
				// convert pixel format
			case 1:
				if(mode == glmPalettized)
				{
					TVPBLExpand1BitTo8Bit(
						(tjs_uint8*)scanline,
						(tjs_uint8*)buf, bi.biWidth);
				}
				else if(mode == glmGrayscale)
				{
					TVPBLExpand1BitTo8BitPal(
						(tjs_uint8*)scanline,
						(tjs_uint8*)buf, bi.biWidth, palette);
				}
				else
				{
					TVPBLExpand1BitTo32BitPal(
						(tjs_uint32*)scanline,
						(tjs_uint8*)buf, bi.biWidth, palette);
				}
				break;

			case 4:
				if(mode == glmPalettized)
				{
					TVPBLExpand4BitTo8Bit(
						(tjs_uint8*)scanline,
						(tjs_uint8*)buf, bi.biWidth);
				}
				else if(mode == glmGrayscale)
				{
					TVPBLExpand4BitTo8BitPal(
						(tjs_uint8*)scanline,
						(tjs_uint8*)buf, bi.biWidth, palette);
				}
				else
				{
					TVPBLExpand4BitTo32BitPal(
						(tjs_uint32*)scanline,
						(tjs_uint8*)buf, bi.biWidth, palette);
				}
				break;

			case 8:
				if(mode == glmPalettized)
				{
					// intact copy
					memcpy(scanline, buf, bi.biWidth);
				}
				else
				if(mode == glmGrayscale)
				{
					// convert to grayscale
					TVPBLExpand8BitTo8BitPal(
						(tjs_uint8*)scanline,
						(tjs_uint8*)buf, bi.biWidth, palette);
				}
				else
				{
					TVPBLExpand8BitTo32BitPal(
						(tjs_uint32*)scanline,
						(tjs_uint8*)buf, bi.biWidth, palette);
				}
				break;

			case 15:
			case 16:
				if(mode == glmGrayscale)
				{
					TVPBLConvert15BitTo8Bit(
						(tjs_uint8*)scanline,
						(tjs_uint16*)buf, bi.biWidth);
				}
				else
				{
					TVPBLConvert15BitTo32Bit(
						(tjs_uint32*)scanline,
						(tjs_uint16*)buf, bi.biWidth);
				}
				break;

			case 24:
				if(mode == glmGrayscale)
				{
					TVPBLConvert24BitTo8Bit(
						(tjs_uint8*)scanline,
						(tjs_uint8*)buf, bi.biWidth);
				}
				else
				{
					TVPBLConvert24BitTo32Bit(
						(tjs_uint32*)scanline,
						(tjs_uint8*)buf, bi.biWidth);
				}
				break;

			case 32:
				if(mode == glmGrayscale)
				{
					TVPBLConvert32BitTo8Bit(
						(tjs_uint8*)scanline,
						(tjs_uint32*)buf, bi.biWidth);
				}
				else
				{
					if(alphatype == batNone)
					{
						// alpha channel is not given by the bitmap.
						// destination alpha is filled with 255.
						TVPBLConvert32BitTo32Bit_NoneAlpha(
							(tjs_uint32*)scanline,
							(tjs_uint32*)buf, bi.biWidth);
					}
					else if(alphatype == batMulAlpha)
					{
						// this is the TVP native representation of the alpha channel.
						// simply copy from the buffer.
						TVPBLConvert32BitTo32Bit_MulAddAlpha(
							(tjs_uint32*)scanline,
							(tjs_uint32*)buf, bi.biWidth);
					}
					else if(alphatype == batAddAlpha)
					{
						// this is alternate representation of the alpha channel,
						// this must be converted to TVP native representation.
						TVPBLConvert32BitTo32Bit_AddAlpha(
							(tjs_uint32*)scanline,
							(tjs_uint32*)buf, bi.biWidth);

					}
				}
				break;
			}

			scanlinecallback(callbackdata, -1); // image was written

			if(bi.biHeight>0) dest_y--; else dest_y++;
			buf += pitch;
			bufremain--;
		}
		if(mode == glmNormalRGBA)
		{
			for(tjs_int y = 0; y < height; y++)
			{
				tjs_uint32 *current = (tjs_uint32*)scanlinecallback(callbackdata, y);
				TVPRedBlueSwap( current, bi.biWidth );
			}
		}
	}
	catch(...)
	{
		TJSAlignedDealloc(readbuf);
		throw;
	}

	TJSAlignedDealloc(readbuf);
}
//---------------------------------------------------------------------------
void TVPLoadBMP(void* formatdata, void *callbackdata, tTVPGraphicSizeCallback sizecallback,
	tTVPGraphicScanLineCallback scanlinecallback, tTVPMetaInfoPushCallback metainfopushcallback,
	iTJSBinaryStream *src, tjs_int keyidx,  tTVPGraphicLoadMode mode)
{
	// Windows BMP Loader
	// mostly taken ( but totally re-written ) from SDL,
	// http://www.libsdl.org/

	// TODO: only checked in Win32 platform



	tjs_uint64 firstpos = src->GetPosition();

	// check the magic
	tjs_uint8 magic[2];
	TVPReadBuffer(src, magic, 2);
	if(magic[0] != TJS_N('B') || magic[1] != TJS_N('M'))
		TVPThrowExceptionMessage(TVPImageLoadError, (const tjs_char*)TVPNotWindowsBmp );

	// read the BITMAPFILEHEADER
	TVP_WIN_BITMAPFILEHEADER bf;
	bf.bfSize = TVPReadI32LE(src);
	bf.bfReserved1 = TVPReadI16LE(src);
	bf.bfReserved2 = TVPReadI16LE(src);
	bf.bfOffBits = TVPReadI32LE(src);

	// read the BITMAPINFOHEADER
	TVP_WIN_BITMAPINFOHEADER bi;
	bi.biSize = TVPReadI32LE(src);
	if(bi.biSize == 12)
	{
		// OS/2 Bitmap
		memset(&bi, 0, sizeof(bi));
		bi.biWidth = (tjs_uint32)TVPReadI16LE(src);
		bi.biHeight = (tjs_uint32)TVPReadI16LE(src);
		bi.biPlanes = TVPReadI16LE(src);
		bi.biBitCount = TVPReadI16LE(src);
		bi.biClrUsed = 1 << bi.biBitCount;
	}
	else if(bi.biSize == 40)
	{
		// Windows Bitmap
		bi.biWidth = TVPReadI32LE(src);
		bi.biHeight = TVPReadI32LE(src);
		bi.biPlanes = TVPReadI16LE(src);
		bi.biBitCount = TVPReadI16LE(src);
		bi.biCompression = TVPReadI32LE(src);
		bi.biSizeImage = TVPReadI32LE(src);
		bi.biXPelsPerMeter = TVPReadI32LE(src);
		bi.biYPelsPerMeter = TVPReadI32LE(src);
		bi.biClrUsed = TVPReadI32LE(src);
		bi.biClrImportant = TVPReadI32LE(src);
	}
	else
	{
		TVPThrowExceptionMessage(TVPImageLoadError, (const tjs_char*)TVPUnsupportedHeaderVersion );
	}


	// load palette
	tjs_int palsize = (bi.biBitCount <= 8) ?
		((bi.biClrUsed == 0 ? (1<<bi.biBitCount) : bi.biClrUsed) *
		((bi.biSize == 12) ? 3:4)) : 0;  // bi.biSize == 12 ( OS/2 palette )
	tjs_uint8 *palette = NULL;

	if(palsize) palette = new tjs_uint8 [palsize];

	try
	{
		TVPReadBuffer(src, palette, palsize);
		TVPStreamSetPosition(src, firstpos + bf.bfOffBits);

		TVPInternalLoadBMP(callbackdata, sizecallback, scanlinecallback,
			bi, palette, src, keyidx, batMulAlpha, mode);
	}
	catch(...)
	{
		if(palette) delete [] palette;
		throw;
	}
	if(palette) delete [] palette;
}
//---------------------------------------------------------------------------





//---------------------------------------------------------------------------
// BMP saving handler
//---------------------------------------------------------------------------
static void TVPWriteLE16(iTJSBinaryStream * stream, tjs_uint16 number)
{
	tjs_uint8 data[2];
	data[0] = number & 0xff;
	data[1] = (number >> 8) & 0xff;
	TVPWriteBuffer(stream, data, 2);
}
//---------------------------------------------------------------------------
static void TVPWriteLE32(iTJSBinaryStream * stream, tjs_uint32 number)
{
	tjs_uint8 data[4];
	data[0] = number & 0xff;
	data[1] = (number >> 8) & 0xff;
	data[2] = (number >> 16) & 0xff;
	data[3] = (number >> 24) & 0xff;
	TVPWriteBuffer(stream, data, 4);
}
//---------------------------------------------------------------------------
void TVPSaveAsBMP( void* formatdata, iTJSBinaryStream* dst, const tTVPBaseBitmap* bmp, const ttstr & mode, iTJSDispatch2* meta )
{
	tjs_int pixelbytes;

	if(mode == TJS_W("bmp32") || mode == TJS_W("bmp"))
		pixelbytes = 4;
	else if(mode == TJS_W("bmp24"))
		pixelbytes = 3;
	else if(mode == TJS_W("bmp8"))
		pixelbytes = 1;
	else
		pixelbytes = 4;

	if( meta )
	{
		tTJSVariant val;
		tjs_error er = meta->PropGet(TJS_MEMBERMUSTEXIST, TJS_W("bpp"), NULL, &val, meta);
		if(TJS_SUCCEEDED(er))
		{
			tjs_int index = (tjs_int)val.AsInteger();
			switch( index ) {
			case 0: pixelbytes = 4; break;
			case 1: pixelbytes = 3; break;
			case 2: pixelbytes = 1; break;
			};
		}
	}

	// open stream
	iTJSBinaryStream *stream = dst;
	tjs_uint8 * buf = NULL;

	try
	{
		// 旧実装は冒頭で TVPClearGraphicCache() を呼んでいた (decode 層全消し)。
		// 過剰なので tTVPGraphicHandlerType::Save 側で対象 path の entry のみ
		// 駆逐する形に変更済み。

		// prepare header
		tjs_uint bmppitch = bmp->GetWidth() * pixelbytes;
		bmppitch = (((bmppitch - 1) >> 2) + 1) << 2;

		TVPWriteLE16(stream, 0x4d42);  /* bfType */
		TVPWriteLE32(stream, sizeof(TVP_WIN_BITMAPFILEHEADER) +
				sizeof(TVP_WIN_BITMAPINFOHEADER) + bmppitch * bmp->GetHeight() +
				(pixelbytes == 1 ? 1024 : 0)); /* bfSize */
		TVPWriteLE16(stream, 0); /* bfReserved1 */
		TVPWriteLE16(stream, 0); /* bfReserved2 */
		TVPWriteLE32(stream, sizeof(TVP_WIN_BITMAPFILEHEADER) +
				sizeof(TVP_WIN_BITMAPINFOHEADER)+
				(pixelbytes == 1 ? 1024 : 0)); /* bfOffBits */

		TVPWriteLE32(stream, sizeof(TVP_WIN_BITMAPINFOHEADER)); /* biSize */
		TVPWriteLE32(stream, bmp->GetWidth()); /* biWidth */
		TVPWriteLE32(stream, bmp->GetHeight()); /* biHeight */
		TVPWriteLE16(stream, 1); /* biPlanes */
		TVPWriteLE16(stream, pixelbytes * 8); /* biBitCount */
		TVPWriteLE32(stream, BI_RGB); /* biCompression */
		TVPWriteLE32(stream, 0); /* biSizeImage */
		TVPWriteLE32(stream, 0); /* biXPelsPerMeter */
		TVPWriteLE32(stream, 0); /* biYPelsPerMeter */
		TVPWriteLE32(stream, 0); /* biClrUsed */
		TVPWriteLE32(stream, 0); /* biClrImportant */

		// write palette
		if(pixelbytes == 1)
		{
			tjs_uint8 palette[1024];
			tjs_uint8 * p = palette;
			for(tjs_int i = 0; i < 256; i++)
			{
				p[0] = TVP252DitherPalette[0][i];
				p[1] = TVP252DitherPalette[1][i];
				p[2] = TVP252DitherPalette[2][i];
				p[3] = 0;
				p += 4;
			}
			TVPWriteBuffer(stream, palette, 1024);
		}

		// write bitmap body
		for(tjs_int y = bmp->GetHeight() - 1; y >= 0; y --)
		{
			if(!buf) buf = new tjs_uint8[bmppitch];
			if(pixelbytes == 4)
			{
				memcpy(buf, bmp->GetScanLine(y), bmppitch);
			}
			else if(pixelbytes == 1)
			{
				TVPDither32BitTo8Bit(buf, (const tjs_uint32*)bmp->GetScanLine(y),
					bmp->GetWidth(), 0, y);  
			}
			else
			{
				const tjs_uint8 *src = (const tjs_uint8 *)bmp->GetScanLine(y);
				tjs_uint8 *dest = buf;
				tjs_int w = bmp->GetWidth();
				for(tjs_int x = 0; x < w; x++)
				{
					dest[0] = src[0];
					dest[1] = src[1];
					dest[2] = src[2];
					dest += 3;
					src += 4;
				}
			}
			TVPWriteBuffer(stream, buf, bmppitch);
		}
	}
	catch(...)
	{
		if(buf) delete [] buf;
		throw;
	}
	if(buf) delete [] buf;
}
//---------------------------------------------------------------------------

void TVPLoadHeaderBMP( void* formatdata, iTJSBinaryStream *src, iTJSDispatch2** dic )
{
	tjs_uint64 firstpos = src->GetPosition();

	// check the magic
	tjs_uint8 magic[2];
	TVPReadBuffer(src, magic, 2);
	if(magic[0] != TJS_N('B') || magic[1] != TJS_N('M'))
		TVPThrowExceptionMessage(TVPImageLoadError, (const tjs_char*)TVPNotWindowsBmp );

	// read the BITMAPFILEHEADER
	TVP_WIN_BITMAPFILEHEADER bf;
	bf.bfSize = TVPReadI32LE(src);
	bf.bfReserved1 = TVPReadI16LE(src);
	bf.bfReserved2 = TVPReadI16LE(src);
	bf.bfOffBits = TVPReadI32LE(src);

	// read the BITMAPINFOHEADER
	TVP_WIN_BITMAPINFOHEADER bi;
	bi.biSize = TVPReadI32LE(src);
	if(bi.biSize == 12)
	{
		// OS/2 Bitmap
		memset(&bi, 0, sizeof(bi));
		bi.biWidth = (tjs_uint32)TVPReadI16LE(src);
		bi.biHeight = (tjs_uint32)TVPReadI16LE(src);
		bi.biPlanes = TVPReadI16LE(src);
		bi.biBitCount = TVPReadI16LE(src);
		bi.biClrUsed = 1 << bi.biBitCount;
	}
	else if(bi.biSize == 40)
	{
		// Windows Bitmap
		bi.biWidth = TVPReadI32LE(src);
		bi.biHeight = TVPReadI32LE(src);
		bi.biPlanes = TVPReadI16LE(src);
		bi.biBitCount = TVPReadI16LE(src);
		bi.biCompression = TVPReadI32LE(src);
		bi.biSizeImage = TVPReadI32LE(src);
		bi.biXPelsPerMeter = TVPReadI32LE(src);
		bi.biYPelsPerMeter = TVPReadI32LE(src);
		bi.biClrUsed = TVPReadI32LE(src);
		bi.biClrImportant = TVPReadI32LE(src);
	}
	else
	{
		TVPThrowExceptionMessage(TVPImageLoadError, (const tjs_char*)TVPUnsupportedHeaderVersion );
	}

	tjs_int palsize = (bi.biBitCount <= 8) ?
		((bi.biClrUsed == 0 ? (1<<bi.biBitCount) : bi.biClrUsed) *
		((bi.biSize == 12) ? 3:4)) : 0;  // bi.biSize == 12 ( OS/2 palette )
	palsize = palsize > 0 ? 1 : 0;

	*dic = TJSCreateDictionaryObject();
	tTJSVariant val(bi.biWidth);
	(*dic)->PropSet(TJS_MEMBERENSURE, TJS_W("width"), 0, &val, (*dic) );
	val = tTJSVariant(bi.biHeight);
	(*dic)->PropSet(TJS_MEMBERENSURE, TJS_W("height"), 0, &val, (*dic) );
	val = tTJSVariant(bi.biBitCount);
	(*dic)->PropSet(TJS_MEMBERENSURE, TJS_W("bpp"), 0, &val, (*dic) );
	val = tTJSVariant(palsize);
	(*dic)->PropSet(TJS_MEMBERENSURE, TJS_W("palette"), 0, &val, (*dic) );
}

//---------------------------------------------------------------------------
// TVPLoadGraphic related
//---------------------------------------------------------------------------
enum tTVPLoadGraphicType
{
	lgtFullColor, // full 32bit color
	lgtPalGray, // palettized or grayscale
	lgtMask // mask
};
struct tTVPLoadGraphicData
{
	ttstr Name;
	tTVPBaseBitmap *Dest;
	tTVPLoadGraphicType Type;
	tjs_int ColorKey;
	tjs_uint8 *Buffer;
	tjs_uint ScanLineNum;
	tjs_uint DesW;
	tjs_uint DesH;
	tjs_uint OrgW;
	tjs_uint OrgH;
	tjs_uint BufW;
	tjs_uint BufH;
	bool NeedMetaInfo;
	bool Unpadding;
	std::vector<tTVPGraphicMetaInfoPair> * MetaInfo;
};
//---------------------------------------------------------------------------
static void TVPLoadGraphic_SizeCallback(void *callbackdata, tjs_uint w,
	tjs_uint h)
{
	tTVPLoadGraphicData * data = (tTVPLoadGraphicData *)callbackdata;

	// check size
	data->OrgW = w;
	data->OrgH = h;
	if(data->DesW && w < data->DesW) w = data->DesW;
	if(data->DesH && h < data->DesH) h = data->DesH;
	data->BufW = w;
	data->BufH = h;

	// create buffer
	if(data->Type == lgtMask)
	{
		// mask ( _m ) load

		// check the image previously loaded
		if(data->Dest->GetWidth() != w || data->Dest->GetHeight() != h)
			TVPThrowExceptionMessage(TVPMaskSizeMismatch);

		// allocate line buffer
		data->Buffer = new tjs_uint8 [w];
	}
	else
	{
		// normal load or province load
		data->Dest->Recreate(w, h, data->Type!=lgtFullColor?8:32, data->Unpadding);
	}
}
//---------------------------------------------------------------------------
static void * TVPLoadGraphic_ScanLineCallback(void *callbackdata, tjs_int y)
{
	tTVPLoadGraphicData * data = (tTVPLoadGraphicData *)callbackdata;

	if(y >= 0)
	{
		// query of line buffer

		data->ScanLineNum = y;
		if(data->Type == lgtMask)
		{
			// mask
			return data->Buffer;
		}
		else
		{
			// return the scanline for writing
			return data->Dest->GetScanLineForWrite(y);
		}
	}
	else
	{
		// y==-1 indicates the buffer previously returned was written

		if(data->Type == lgtMask)
		{
			// mask

			// tile for horizontal direction
			tjs_uint i;
			for(i = data->OrgW; i<data->BufW; i+=data->OrgW)
			{
				tjs_uint w = data->BufW - i;
				w = w > data->OrgW ? data->OrgW : w;
				memcpy(data->Buffer + i, data->Buffer, w);
			}

			// bind mask buffer to main image buffer ( and tile for vertical )
			for(i = data->ScanLineNum; i<data->BufH; i+=data->OrgH)
			{
				TVPBindMaskToMain(
					(tjs_uint32*)data->Dest->GetScanLineForWrite(i),
					data->Buffer, data->BufW);
			}
			return NULL;
		}
		else if(data->Type == lgtFullColor)
		{
			tjs_uint32 * sl =
				(tjs_uint32*)data->Dest->GetScanLineForWrite(data->ScanLineNum);
			if((data->ColorKey & 0xff000000) == 0x00000000)
			{
				// make alpha from color key
				TVPMakeAlphaFromKey(
					sl,
					data->BufW,
					data->ColorKey);
			}

			// tile for horizontal direction
			tjs_uint i;
			for(i = data->OrgW; i<data->BufW; i+=data->OrgW)
			{
				tjs_uint w = data->BufW - i;
				w = w > data->OrgW ? data->OrgW : w;
				memcpy(sl + i, sl, w * sizeof(tjs_uint32));
			}

			// tile for vertical direction
			for(i = data->ScanLineNum + data->OrgH; i<data->BufH; i+=data->OrgH)
			{
				memcpy(
					(tjs_uint32*)data->Dest->GetScanLineForWrite(i),
					sl,
					data->BufW * sizeof(tjs_uint32) );
			}

			return NULL;
		}
		else if(data->Type == lgtPalGray)
		{
			// nothing to do
			if(data->OrgW < data->BufW || data->OrgH < data->BufH)
			{
				tjs_uint8 * sl =
					(tjs_uint8*)data->Dest->GetScanLineForWrite(data->ScanLineNum);
				tjs_uint i;

				// tile for horizontal direction
				for(i = data->OrgW; i<data->BufW; i+=data->OrgW)
				{
					tjs_uint w = data->BufW - i;
					w = w > data->OrgW ? data->OrgW : w;
					memcpy(sl + i, sl, w * sizeof(tjs_uint8));
				}

				// tile for vertical direction
				for(i = data->ScanLineNum + data->OrgH; i<data->BufH; i+=data->OrgH)
				{
					memcpy(
						(tjs_uint8*)data->Dest->GetScanLineForWrite(i),
						sl,
						data->BufW * sizeof(tjs_uint8));
				}
			}

			return NULL;
		}
	}
	return NULL;
}
//---------------------------------------------------------------------------
static void TVPLoadGraphic_MetaInfoPushCallback(void *callbackdata,
	const ttstr & name, const ttstr & value)
{
	tTVPLoadGraphicData * data = (tTVPLoadGraphicData *)callbackdata;

	if(data->NeedMetaInfo)
	{
		if(!data->MetaInfo) data->MetaInfo = new std::vector<tTVPGraphicMetaInfoPair>();
		data->MetaInfo->push_back(tTVPGraphicMetaInfoPair(name, value));
	}
}
//---------------------------------------------------------------------------
//static int _USERENTRY TVPColorCompareFunc(const void *_a, const void *_b)
static int TVPColorCompareFunc(const void *_a, const void *_b)
{
	tjs_uint32 a = *(const tjs_uint32*)_a;
	tjs_uint32 b = *(const tjs_uint32*)_b;

	if(a<b) return -1;
	if(a==b) return 0;
	return 1;
}
//---------------------------------------------------------------------------
static void TVPMakeAlphaFromAdaptiveColor(tTVPBaseBitmap *dest)
{
	// make adaptive color key and make alpha from it.
	// adaptive color key is most used(popular) color at first line of the
	// graphic.
	if(!dest->Is32BPP()) return;

	// copy first line to buffer
	tjs_int w = dest->GetWidth();
	tjs_int pitch =  std::abs(dest->GetPitchBytes());
	tjs_uint32 * buffer = new tjs_uint32[pitch];

	try
	{
		const void *d = dest->GetScanLine(0);

		memcpy(buffer, d, pitch);
		tjs_int i;
		for(i = w -1; i>=0; i--) buffer[i] &= 0xffffff;
		buffer[w] = (tjs_uint32)-1; // a sentinel

		// sort by color
		qsort(buffer, dest->GetWidth(), sizeof(tjs_uint32), TVPColorCompareFunc);

		// find most used color
		tjs_int maxlen = 0;
		tjs_uint32 maxlencolor = 0;
		tjs_uint32 pcolor = (tjs_uint32)-1;
		tjs_int l = 0;
		for(i = 0; i< w+1; i++)
		{
			if(buffer[i] != pcolor)
			{
				if(maxlen < l)
				{
					maxlen = l;
					maxlencolor = pcolor;
					l = 0;
				}
			}
			else
			{
				l++;
			}
			pcolor = buffer[i];
		}

		if(maxlencolor == (tjs_uint32)-1)
		{
			// may color be not found...
			maxlencolor = 0; // black is a default colorkey
		}

		// make alpha from maxlencolor
		tjs_int h;
		for(h = dest->GetHeight()-1; h>=0; h--)
		{
			TVPMakeAlphaFromKey((tjs_uint32*)dest->GetScanLineForWrite(h),
				w, maxlencolor);

		}

	}
	catch(...)
	{
		delete [] buffer;
		throw;
	}

	delete [] buffer;
}
//---------------------------------------------------------------------------
static void TVPDoAlphaColorMat(tTVPBaseBitmap *dest, tjs_uint32 color)
{
	// Do alpha matting.
	// 'mat' means underlying color of the image. This function piles
	// specified color under the image, then blend. The output image
	// will be totally opaque. This function always assumes the image
	// has pixel value for alpha blend mode, not additive alpha blend mode.
	if(!dest->Is32BPP()) return;

	tjs_int w = dest->GetWidth();
	tjs_int h = dest->GetHeight();

	for(tjs_int y = 0; y < h; y++)
	{
		tjs_uint32 * buffer = (tjs_uint32*)dest->GetScanLineForWrite(y);
		TVPAlphaColorMat(buffer, color, w);
	}
}
//---------------------------------------------------------------------------



//---------------------------------------------------------------------------
iTJSDispatch2 * TVPMetaInfoPairsToDictionary(
	std::vector<tTVPGraphicMetaInfoPair> *vec)
{
	if(!vec) return NULL;
	std::vector<tTVPGraphicMetaInfoPair>::iterator i;
	iTJSDispatch2 *dic = TJSCreateDictionaryObject();
	try
	{
		for(i = vec->begin(); i != vec->end(); i++)
		{
			tTJSVariant val(i->Value);
			dic->PropSet(TJS_MEMBERENSURE, i->Name.c_str(), 0,
				&val, dic);
		}
	}
	catch(...)
	{
		dic->Release();
		throw;
	}
	return dic;
}
//---------------------------------------------------------------------------




//---------------------------------------------------------------------------
// Graphics Cache Management
//---------------------------------------------------------------------------
bool TVPAllocGraphicCacheOnHeap = false;
	// this allocates graphic cache's store memory on heap, rather than
	// sharing bitmap object. ( since sucking win9x cannot have so many bitmap
	// object at once, WinNT/2000 is ok. )
	// this will take more time for memory copying.
//---------------------------------------------------------------------------
struct tTVPGraphicsSearchData
{
	// Name は decode 層キャッシュキーの一部。prefetch worker が挿入し main /
	// pressure callback が削除する越境キーなので、非 atomic RefCount を持つ
	// ttstr では二重解放が起きる。RefCount を持たない tjs_string で保持する
	// (doc/TtstrDataRetention.md H4)。境界では AsStdString() で独立化する。
	tjs_string Name;
	tjs_int32 KeyIdx; // color key index
	tTVPGraphicLoadMode Mode; // image mode
	tjs_uint DesW; // desired width ( 0 for original size )
	tjs_uint DesH; // desired height ( 0 for original size )

	bool operator == (const tTVPGraphicsSearchData &rhs) const
	{
		return KeyIdx == rhs.KeyIdx && Mode == rhs.Mode &&
			Name == rhs.Name && DesW == rhs.DesW && DesH == rhs.DesH;
	}
};
//---------------------------------------------------------------------------
class tTVPGraphicsSearchHashFunc
{
public:
	static tjs_uint32 Make(const tTVPGraphicsSearchData &val)
	{
		// Name は tjs_string。内容ベースで ttstr 版と同一アルゴリズムの
		// tjs_char* 特殊化を使う (空文字は両者とも 0 を返す)。
		tjs_uint32 v = tTJSHashFunc<tjs_char *>::Make(val.Name.c_str());

		v ^= val.KeyIdx + (val.KeyIdx >> 23);
		v ^= (val.Mode << 30);
		v ^= val.DesW + (val.DesW >> 8);
		v ^= val.DesH + (val.DesH >> 8);
		return v;
	}
};
//---------------------------------------------------------------------------
class tTVPGraphicImageData
{
private:
	tTVPBaseBitmap *Bitmap;
	tjs_uint8 * RawData;
	tjs_int Width;
	tjs_int Height;
	tjs_int PixelSize;

public:
	// 値側 path。push 時 worker、内部ロード/読出し時 main が触る。境界で独立化
	// するため tjs_string で保持する (doc/TtstrDataRetention.md H5)。
	tjs_string ProvinceName;

	std::vector<tTVPGraphicMetaInfoPair> * MetaInfo;

private:
	tjs_int RefCount;
	tjs_uint Size;
	bool Pinned;            // P2: pin (sticky) フラグ。LRU/transient 駆逐対象外

public:
	tTVPGraphicImageData()
	{
		RefCount = 1; Size = 0; Bitmap = NULL; RawData = NULL;
		MetaInfo = NULL;
		Pinned = false;
	}
	~tTVPGraphicImageData()
	{
		if(Bitmap) delete Bitmap;
		if(RawData) delete [] RawData;
		if(MetaInfo) delete MetaInfo;
	}

	void AssignBitmap(const tTVPBaseBitmap *bmp)
	{
		if(Bitmap) delete Bitmap, Bitmap = NULL;
		if(RawData) delete [] RawData, RawData = NULL;

		Width = bmp->GetWidth();
		Height = bmp->GetHeight();
		PixelSize = bmp->Is32BPP()?4:1;
		Size =  Width*Height*PixelSize;

		if(!TVPAllocGraphicCacheOnHeap)
		{
			// simply assin to Bitmap
			Bitmap = new tTVPBaseBitmap(*bmp);
		}
		else
		{
			// allocate heap and copy to it
			tjs_int h = Height;
			RawData = new tjs_uint8 [ Size ];
			tjs_uint8 *p = RawData;
			tjs_int rawpitch = Width * PixelSize;
			for(h--; h>=0; h--)
			{
				memcpy(p, bmp->GetScanLine(h), rawpitch);
				p += rawpitch;
			}
		}
	}

	void AssignToBitmap(tTVPBaseBitmap *bmp) const
	{
		if(!TVPAllocGraphicCacheOnHeap)
		{
			// simply assign to Bitmap
			if(Bitmap) bmp->AssignBitmap(*Bitmap);
		}
		else
		{
			// copy from the rawdata heap
			if(RawData)
			{
				bmp->Recreate(Width, Height, PixelSize==4?32:8);
				tjs_int h = Height;
				tjs_uint8 *p = RawData;
				tjs_int rawpitch = Width * PixelSize;
				for(h--; h>=0; h--)
				{
					memcpy(bmp->GetScanLineForWrite(h), p, rawpitch);
					p += rawpitch;
				}
			}
		}
	}

	tjs_uint GetSize() const { return Size; }
	tjs_int GetWidth() const { return Width; }
	tjs_int GetHeight() const { return Height; }
	tjs_int GetPixelSize() const { return PixelSize; }

	bool IsPinned() const { return Pinned; }
	void SetPinned(bool v) { Pinned = v; }

	void AddRef() { RefCount ++; }
	void Release()
	{
		if(RefCount == 1)
		{
			delete this;
		}
		else
		{
			RefCount--;
		}
	}
};
//---------------------------------------------------------------------------
typedef tTJSRefHolder<tTVPGraphicImageData> tTVPGraphicImageHolder;

typedef
tTJSHashTable<tTVPGraphicsSearchData, tTVPGraphicImageHolder, tTVPGraphicsSearchHashFunc>
	tTVPGraphicCache;
tTVPGraphicCache TVPGraphicCache;
static bool TVPGraphicCacheEnabled = false;
static tjs_uint64 TVPGraphicCacheLimit = 0;
static tjs_uint64 TVPGraphicCacheTotalBytes = 0;
tjs_uint64 TVPGraphicCacheSystemLimit = 0; // maximum possible value of  TVPGraphicCacheLimit
// 非同期 prefetch worker スレッドからも TVPGraphicCache を触れるよう CS で保護する。
// 既存のメインスレッド経由の触り口 (TVPCheckImageCache / TVPHasImageCache /
// TVPPushGraphicCache / TVPClearGraphicCache / TVPLoadGraphic 内部ロジック)
// を全てこの CS でガード。粒度は粗いが、もとから処理は短時間 (hash 操作 + LRU
// chop) なので問題にならない想定。
static tTJSCriticalSection TVPGraphicCacheCS;
//---------------------------------------------------------------------------
static void TVPCheckGraphicCacheLimit()
{
	// 呼び出し元で TVPGraphicCacheCS を取得済みであることを前提とする
	// P2: pinned エントリは chop 対象外。LRU 末尾から非 pinned を探して削除する。
	while(TVPGraphicCacheTotalBytes > TVPGraphicCacheLimit)
	{
		// LRU 末尾から最初の非 pinned エントリを探す
		tTVPGraphicCache::tIterator i = TVPGraphicCache.GetLast();
		while(!i.IsNull() && i.GetValue().GetObjectNoAddRef()->IsPinned())
		{
			i--;
		}
		if(i.IsNull())
		{
			// 全部 pinned。これ以上駆逐できない (上限超過のまま継続)
			ttstr msg(TJS_W("TVPGraphicCache: limit exceeded but all entries are pinned (totalBytes="));
			msg += ttstr((tjs_int)TVPGraphicCacheTotalBytes);
			msg += TJS_W(" limit=");
			msg += ttstr((tjs_int)TVPGraphicCacheLimit);
			msg += TJS_W(")");
			TVPAddImportantLog(msg);
			break;
		}
		// 該当 entry を削除。ChopLast(1) は末尾依存なので個別 Delete を使う
		tTVPGraphicsSearchData key = i.GetKey();
		tjs_uint size = i.GetValue().GetObjectNoAddRef()->GetSize();
		TVPLOG_DEBUG("ImageCache:lruChop:{} size={} total={} limit={}",
		             key.Name, size, (tjs_uint64)TVPGraphicCacheTotalBytes,
		             (tjs_uint64)TVPGraphicCacheLimit);
		if(TVPGraphicCacheTotalBytes >= size)
			TVPGraphicCacheTotalBytes -= size;
		else
			TVPGraphicCacheTotalBytes = 0;
		TVPGraphicCache.Delete(key);
	}
}
//---------------------------------------------------------------------------
void TVPClearGraphicCache()
{
	tTJSCriticalSectionHolder cs(TVPGraphicCacheCS);
	const tjs_uint count = TVPGraphicCache.GetCount();
	const tjs_uint64 bytes = TVPGraphicCacheTotalBytes;
	TVPGraphicCache.Clear();
	TVPGraphicCacheTotalBytes = 0;
	if(count > 0)
		TVPLOG_DEBUG("ImageCache:clearAll: dropped={} bytes={}", count, bytes);
}
//---------------------------------------------------------------------------
// path 単位 evict。同一 Name で (keyidx, mode, dw, dh) の異なる複数エントリが
// 並立しうるため、Name 一致するエントリを全て削除する。
// nname は事前に正規化済みであること (TVPNormalizeStorageName 通過済み)。
//---------------------------------------------------------------------------
void TVPClearGraphicCacheEntry(const ttstr& nname)
{
	if(!TVPGraphicCacheEnabled) return;
	tTJSCriticalSectionHolder cs(TVPGraphicCacheCS);

	// iteration 中の削除で iterator 失効を避けるため、まずキーを集める
	// (キー Name は tjs_string なので同型で比較する)
	tjs_string nn = nname.AsStdString();
	std::vector<tTVPGraphicsSearchData> hits;
	for(tTVPGraphicCache::tIterator i = TVPGraphicCache.GetFirst();
		!i.IsNull(); i++)
	{
		if(i.GetKey().Name == nn)
		{
			hits.push_back(i.GetKey());
		}
	}
	tjs_uint64 dropped_bytes = 0;
	for(const auto &k : hits)
	{
		tTVPGraphicImageHolder *ptr = TVPGraphicCache.Find(k);
		if(ptr)
		{
			tjs_uint size = ptr->GetObjectNoAddRef()->GetSize();
			dropped_bytes += size;
			if(TVPGraphicCacheTotalBytes >= size)
				TVPGraphicCacheTotalBytes -= size;
			else
				TVPGraphicCacheTotalBytes = 0;
			TVPGraphicCache.Delete(k);
		}
	}
	if(!hits.empty())
		TVPLOG_DEBUG("ImageCache:clearEntry:{} dropped={} bytes={}",
		             nname, hits.size(), dropped_bytes);
}
//---------------------------------------------------------------------------
// transient 全消し (pinned エントリは残す)。
//---------------------------------------------------------------------------
void TVPClearTransientGraphicCache()
{
	if(!TVPGraphicCacheEnabled) {
		// 無効化されている場合は念のため Clear だけしておく
		TVPClearGraphicCache();
		return;
	}
	tTJSCriticalSectionHolder cs(TVPGraphicCacheCS);

	// pinned 以外のキーを集めて削除
	std::vector<tTVPGraphicsSearchData> drop_keys;
	tjs_uint kept_pinned = 0;
	for(tTVPGraphicCache::tIterator i = TVPGraphicCache.GetFirst();
		!i.IsNull(); i++)
	{
		if(!i.GetValue().GetObjectNoAddRef()->IsPinned())
		{
			drop_keys.push_back(i.GetKey());
		}
		else
		{
			++kept_pinned;
		}
	}
	tjs_uint64 dropped_bytes = 0;
	for(const auto &k : drop_keys)
	{
		tTVPGraphicImageHolder *ptr = TVPGraphicCache.Find(k);
		if(ptr)
		{
			tjs_uint size = ptr->GetObjectNoAddRef()->GetSize();
			dropped_bytes += size;
			if(TVPGraphicCacheTotalBytes >= size)
				TVPGraphicCacheTotalBytes -= size;
			else
				TVPGraphicCacheTotalBytes = 0;
			TVPGraphicCache.Delete(k);
		}
	}
	TVPLOG_DEBUG("ImageCache:clearTransient: dropped={} bytes={} kept(pinned)={}",
	             drop_keys.size(), dropped_bytes, kept_pinned);
}
//---------------------------------------------------------------------------
// 現在の decode 層キャッシュエントリを全件コピー。
// Storages.getImageCacheList / dumpImageCacheList / MemoryOverlay 等の観測系で利用。
//---------------------------------------------------------------------------
void TVPGetGraphicCacheEntries(std::vector<TVPGraphicCacheEntryInfo> &out)
{
	out.clear();
	if(!TVPGraphicCacheEnabled) return;
	tTJSCriticalSectionHolder cs(TVPGraphicCacheCS);
	for(tTVPGraphicCache::tIterator i = TVPGraphicCache.GetFirst();
		!i.IsNull(); i++)
	{
		const tTVPGraphicsSearchData &k = i.GetKey();
		const tTVPGraphicImageData *d = i.GetValue().GetObjectNoAddRef();
		TVPGraphicCacheEntryInfo info;
		// k.Name は tjs_string。observation 用 ttstr へは独立コピーで変換。
		info.name   = ttstr(k.Name);
		info.keyidx = k.KeyIdx;
		info.mode   = k.Mode;
		info.dw     = k.DesW;
		info.dh     = k.DesH;
		info.width  = d ? (tjs_uint)d->GetWidth()  : 0;
		info.height = d ? (tjs_uint)d->GetHeight() : 0;
		info.bytes  = d ? d->GetSize() : 0;
		info.pinned = d ? d->IsPinned() : false;
		out.push_back(info);
	}
}
//---------------------------------------------------------------------------
// 件数のみ取得 (overlay 等の軽い観測用)。
//---------------------------------------------------------------------------
void TVPGetGraphicCacheCount(size_t &total, size_t &pinned)
{
	total = 0;
	pinned = 0;
	if(!TVPGraphicCacheEnabled) return;
	tTJSCriticalSectionHolder cs(TVPGraphicCacheCS);
	for(tTVPGraphicCache::tIterator i = TVPGraphicCache.GetFirst();
		!i.IsNull(); i++)
	{
		++total;
		const tTVPGraphicImageData *d = i.GetValue().GetObjectNoAddRef();
		if(d && d->IsPinned()) ++pinned;
	}
}
//---------------------------------------------------------------------------
// decode 層キャッシュ一覧を WARNING ログに出力。
// 1 行目: サマリ (件数 + pinned 数 + 総バイト)
// 続く各行: per-entry 詳細 (path / size(WxH) / bytes / pinned 印)。
// 既定以外のキー (colorkey/mode/desired) があるエントリのみ末尾に併記。
//---------------------------------------------------------------------------
void TVPDumpImageCacheList()
{
	std::vector<TVPGraphicCacheEntryInfo> entries;
	TVPGetGraphicCacheEntries(entries);
	tjs_uint64 total_bytes = 0;
	size_t pinned_count = 0;
	for(auto &e : entries) {
		total_bytes += e.bytes;
		if(e.pinned) ++pinned_count;
	}
	tjs_char buf[128];
	{
		ttstr msg = TJS_W("ImageCache: ");
		TJS_snprintf(buf, sizeof(buf)/sizeof(tjs_char), TJS_W("%zu"), entries.size());
		msg += buf;
		msg += TJS_W(" entries (pinned=");
		TJS_snprintf(buf, sizeof(buf)/sizeof(tjs_char), TJS_W("%zu"), pinned_count);
		msg += buf;
		msg += TJS_W(", totalBytes=");
		TJS_snprintf(buf, sizeof(buf)/sizeof(tjs_char), TJS_W("%llu"),
		             (unsigned long long)total_bytes);
		msg += buf;
		msg += TJS_W(")");
		TVPAddImportantLog(msg);
	}
	// TVP_clNone = 0x1fffffff (tp_stub.h)。既定キーは clNone なので、
	// それ以外 (実際に colorkey 指定された) エントリだけ key= を出す。
	const tjs_int32 kClNone = (tjs_int32)0x1fffffff;
	for(auto &e : entries) {
		ttstr msg = e.pinned ? TJS_W("  [pin] ") : TJS_W("        ");
		msg += e.name;
		TJS_snprintf(buf, sizeof(buf)/sizeof(tjs_char),
		             TJS_W(" %ux%u bytes=%u"),
		             e.width, e.height, e.bytes);
		msg += buf;
		// 既定キー以外があるときだけ追記
		if(e.keyidx != kClNone) {
			TJS_snprintf(buf, sizeof(buf)/sizeof(tjs_char),
			             TJS_W(" key=0x%x"), (unsigned)e.keyidx);
			msg += buf;
		}
		if(e.mode != 0 /*glmNormal*/) {
			TJS_snprintf(buf, sizeof(buf)/sizeof(tjs_char),
			             TJS_W(" mode=%d"), (int)e.mode);
			msg += buf;
		}
		if(e.dw != 0 || e.dh != 0) {
			TJS_snprintf(buf, sizeof(buf)/sizeof(tjs_char),
			             TJS_W(" desired=%ux%u"), e.dw, e.dh);
			msg += buf;
		}
		TVPAddImportantLog(msg);
	}
}
//---------------------------------------------------------------------------
// 同名の全 entry に対して pinned 状態を変更。
// (同 path で colorkey/mode/dw/dh の異なる複数 entry が並立しうる)
// nname は事前に正規化済みであること。
//---------------------------------------------------------------------------
void TVPSetGraphicCacheEntryPinned(const ttstr &nname, bool pinned)
{
	if(!TVPGraphicCacheEnabled) return;
	tTJSCriticalSectionHolder cs(TVPGraphicCacheCS);
	tjs_uint changed = 0;
	tjs_string nn = nname.AsStdString();
	for(tTVPGraphicCache::tIterator i = TVPGraphicCache.GetFirst();
		!i.IsNull(); i++)
	{
		if(i.GetKey().Name == nn)
		{
			tTVPGraphicImageData *d = i.GetValue().GetObjectNoAddRef();
			if(d->IsPinned() != pinned) {
				d->SetPinned(pinned);
				++changed;
			}
		}
	}
	if(changed > 0)
		TVPLOG_DEBUG("ImageCache:pin:{}={} entries={}",
		             nname, pinned ? "true" : "false", changed);
}
static tTVPAtExit
	TVPUninitMessageLoad(TVP_ATEXIT_PRI_RELEASE, TVPClearGraphicCache);
//---------------------------------------------------------------------------
struct tTVPClearGraphicCacheCallback : public tTVPCompactEventCallbackIntf
{
	virtual void TJS_INTF_METHOD OnCompact(tjs_int level)
	{
		// P4: Compact level の意味付け再定義 (doc/legacy/ImagePreloadAndCache.md §18.2 C)
		//   IDLE / DEACTIVATE: 何もしない (P5 で IDLE 時 dormant 整理を入れる予定)
		//   MINIMIZE: transient 全消し (pinned エントリは残す)
		//   MAX:      pinned 含めて全消し (アプリ終了/OOM 相当)
		if(level >= TVP_COMPACT_LEVEL_MAX)
		{
			TVPLOG_DEBUG("ImageCache:compact level={} -> clearAll + flushPrefetch", (int)level);
			TVPFlushImagePrefetchQueue();
			TVPClearGraphicCache();
		}
		else if(level >= TVP_COMPACT_LEVEL_MINIMIZE)
		{
			TVPLOG_DEBUG("ImageCache:compact level={} -> clearTransient + flushPrefetch", (int)level);
			TVPFlushImagePrefetchQueue();
			TVPClearTransientGraphicCache();
		}
	}
} static TVPClearGraphicCacheCallback;
static bool TVPClearGraphicCacheCallbackInit = false;
//---------------------------------------------------------------------------
void TVPPushGraphicCache( const ttstr& nname, tTVPBaseBitmap* bmp, std::vector<tTVPGraphicMetaInfoPair>* meta )
{
	if( TVPGraphicCacheEnabled ) {
		// graphic compact initialization
		// (Add/Remove event hook はメインスレッド前提の処理だが、TVPPushGraphicCache
		//  自体はワーカースレッドからも呼ばれうる。CompactEventHook の登録は
		//  起動時に 1 度走れば十分なので、最初の登録はメインスレッド (TVPLoadGraphic
		//  経路) で済ませる前提とする。worker からの初回 push の前に
		//  メインスレッドで一度でも push/load が走っていれば登録済みとなる)
		if(!TVPClearGraphicCacheCallbackInit)
		{
			TVPAddCompactEventHook(&TVPClearGraphicCacheCallback);
			TVPClearGraphicCacheCallbackInit = true;
		}

		tTVPGraphicImageData* data = NULL;
		try {
			tjs_uint32 hash;
			tTVPGraphicsSearchData searchdata;

			searchdata.Name = nname.AsStdString();
			searchdata.KeyIdx = TVP_clNone;
			searchdata.Mode = glmNormal;
			searchdata.DesW = 0;
			searchdata.DesH = 0;

			hash = tTVPGraphicCache::MakeHash(searchdata);

			data = new tTVPGraphicImageData();
			data->AssignBitmap( bmp );
			data->ProvinceName = TJS_W("");
			data->MetaInfo = meta;
			meta = NULL;
			// P2: pin 集合に登録済みなら pinned=true で初期化
			data->SetPinned(TVPIsCachePathPinned(nname));

			tjs_uint datasize = 0;
			bool was_pinned = false;
			{
				tTJSCriticalSectionHolder cs(TVPGraphicCacheCS);
				// check size limit
				TVPCheckGraphicCacheLimit();

				// push into hash table
				datasize = data->GetSize();
				was_pinned = data->IsPinned();
				TVPGraphicCacheTotalBytes += datasize;
				tTVPGraphicImageHolder holder(data);
				TVPGraphicCache.AddWithHash(searchdata, hash, holder);
			}
			TVPLOG_DEBUG("ImageCache:push:{} {}x{} bytes={} pinned={}",
			             nname, bmp->GetWidth(), bmp->GetHeight(), datasize,
			             was_pinned ? "true" : "false");
			// P3: decode 層に積めたので file 層 raw bytes は不要 (file→decode auto-drop)
			//     TVPGraphicCacheCS を解放してから StorageCacheCS を取る (CS 入れ子回避)
			TVPClearStorageCache(nname);
		} catch(...) {
			if(meta) delete meta;
			if(data) data->Release();
			throw;
		}
		if(data) data->Release();
	} else {
		if( meta ) delete meta;
	}
}
//---------------------------------------------------------------------------
bool TVPCheckImageCache( const ttstr& nname, tTVPBaseBitmap* dest, tTVPGraphicLoadMode mode, tjs_uint dw, tjs_uint dh, tjs_int32 keyidx, iTJSDispatch2** metainfo )
{
	tjs_uint32 hash;
	tTVPGraphicsSearchData searchdata;
	if(TVPGraphicCacheEnabled)
	{
		searchdata.Name = nname.AsStdString();
		searchdata.KeyIdx = keyidx;
		searchdata.Mode = mode;
		searchdata.DesW = dw;
		searchdata.DesH = dh;

		hash = tTVPGraphicCache::MakeHash(searchdata);

		tTJSCriticalSectionHolder cs(TVPGraphicCacheCS);
		tTVPGraphicImageHolder * ptr =
			TVPGraphicCache.FindAndTouchWithHash(searchdata, hash);
		if(ptr)
		{
			// found in cache
			ptr->GetObjectNoAddRef()->AssignToBitmap(dest);
			if(metainfo)
				*metainfo = TVPMetaInfoPairsToDictionary(ptr->GetObjectNoAddRef()->MetaInfo);
			TVPLOG_DEBUG("ImageCache:hit:{} mode={} key={} {}x{}",
			             nname, (int)mode, keyidx, dw, dh);
			return true;
		}
	}
	return false;
}
//---------------------------------------------------------------------------
// 検索だけする
bool TVPHasImageCache( const ttstr& nname, tTVPGraphicLoadMode mode, tjs_uint dw, tjs_uint dh, tjs_int32 keyidx )
{
	tjs_uint32 hash;
	tTVPGraphicsSearchData searchdata;
	if(TVPGraphicCacheEnabled)
	{
		searchdata.Name = nname.AsStdString();
		searchdata.KeyIdx = keyidx;
		searchdata.Mode = mode;
		searchdata.DesW = dw;
		searchdata.DesH = dh;

		hash = tTVPGraphicCache::MakeHash(searchdata);

		tTJSCriticalSectionHolder cs(TVPGraphicCacheCS);
		tTVPGraphicImageHolder * ptr =
			TVPGraphicCache.FindAndTouchWithHash(searchdata, hash);
		if(ptr)
		{
			return true;
		}
	}
	return false;
}
//---------------------------------------------------------------------------
static bool TVPInternalLoadGraphic(tTVPBaseBitmap *dest, const ttstr &_name,
	tjs_uint32 keyidx, tjs_uint desw, tjs_int desh, std::vector<tTVPGraphicMetaInfoPair> * * MetaInfo,
		tTVPGraphicLoadMode mode, ttstr *provincename, bool unpadding=false,
		ttstr *resolvedname=nullptr)
{
	// name must be normalized.
	// if "provincename" is non-null, this function set it to province storage
	// name ( with _p suffix ) for convinience.
	// if "resolvedname" is non-null, this function sets it to the actually
	// opened storage name (= input name with auto-completed extension).
	// desw and desh are desired size. if the actual picture is smaller than
	// the given size, the graphic is to be tiled. give 0,0 to obtain default
	// size graphic.


	// graphic compact initialization
	if(!TVPClearGraphicCacheCallbackInit)
	{
		TVPAddCompactEventHook(&TVPClearGraphicCacheCallback);
		TVPClearGraphicCacheCallbackInit = true;
	}


	// search according with its extension
	tjs_int namelen = _name.GetLen();
	ttstr name(_name);

	ttstr ext = TVPExtractStorageExt(name);
	int extlen = ext.GetLen();
	tTVPGraphicHandlerType * handler;

	if(ext == TJS_W(""))
	{
		// missing extension
		// suggest registered extensions
		tTJSHashTable<ttstr, tTVPGraphicHandlerType>::tIterator i;
		for(i = TVPGraphicType.Hash.GetFirst(); !i.IsNull(); /*i++*/)
		{
			ttstr newname = name + i.GetKey();
			if(TVPIsExistentStorage(newname))
			{
				// file found
				name = newname;
				break;
			}
			i++;
		}
		if(i.IsNull())
		{
			// not found
			TVPThrowExceptionMessage(TVPCannotSuggestGraphicExtension, name);
		}

		handler = & i.GetValue();
	}
	else
	{
		handler = TVPGraphicType.Hash.Find(ext);
	}

	if(!handler) TVPThrowExceptionMessage(TVPUnknownGraphicFormat, name);

	// 拡張子補完後の名前を呼び出し元に通知 (file 層 cache の drop 対象を
	// 特定するため。auto-drop 時に unresolved な _name で TVPGetPlacedPath
	// を呼ぶと存在しないので例外になる)
	if(resolvedname) *resolvedname = name;

	tTVPStreamHolder holder(name); // open a storage named "name"

	if (holder.Get()->GetSize() == 0)
		TVPThrowExceptionMessage(TVPImageLoadError, name);	

	// load the image
	tTVPLoadGraphicData data;
	data.Dest = dest;
	data.ColorKey = keyidx;
	data.Type = (mode == glmNormal || mode == glmNormalRGBA) ? lgtFullColor : lgtPalGray;
	data.Name = name;
	data.DesW = desw;
	data.DesH = desh;
	data.NeedMetaInfo = true;
	data.MetaInfo = NULL;
	data.Unpadding = unpadding;

	bool keyadapt = (keyidx == TVP_clAdapt);
	bool doalphacolormat = TVP_Is_clAlphaMat(keyidx);
	tjs_uint32 alphamatcolor = TVP_get_clAlphaMat(keyidx);

	if(TVP_Is_clPalIdx(keyidx))
	{
		// pass the palette index number to the handler.
		// ( since only Graphic Loading Handler can process the palette information )
		keyidx = TVP_get_clPalIdx(keyidx);
	}
	else
	{
		keyidx = -1;
	}

	(handler->Load)(handler->FormatData, (void*)&data, TVPLoadGraphic_SizeCallback,
		TVPLoadGraphic_ScanLineCallback, TVPLoadGraphic_MetaInfoPushCallback,
		holder.Get(), keyidx, mode);

	*MetaInfo = data.MetaInfo;

	if(keyadapt && mode == glmNormal)
	{
		// adaptive color key
		TVPMakeAlphaFromAdaptiveColor(dest);
	}


	if(mode != glmNormal) return true;

#ifndef TVP_DONT_AUTOLOAD_PROVINCE
	if(provincename)
	{
		// set province name
		*provincename = ttstr(_name, namelen-extlen) + TJS_W("_p");

		// search extensions
		tTJSHashTable<ttstr, tTVPGraphicHandlerType>::tIterator i;
		for(i = TVPGraphicType.Hash.GetFirst(); !i.IsNull(); /*i++*/)
		{
			ttstr newname = *provincename + i.GetKey();
			if(TVPIsExistentStorage(newname))
			{
				// file found
				*provincename = newname;
				break;
			}
			i++;
		}
		if(i.IsNull())
		{
			// not found
			provincename->Clear();
		}
	}
#else
	if (provincename) {
		provincename->Clear();
	}
#endif

#ifndef TVP_DONT_AUTOLOAD_MASK
	// mask image handling ( addding _m suffix with the filename )
	while(true)
	{
		name = ttstr(_name, namelen-extlen) + TJS_W("_m") + ext;
		if(ext.IsEmpty())
		{
			// missing extension
			// suggest registered extensions
			tTJSHashTable<ttstr, tTVPGraphicHandlerType>::tIterator i;
			for(i = TVPGraphicType.Hash.GetFirst(); !i.IsNull(); /*i++*/)
			{
				ttstr newname = name;
				newname += i.GetKey();
				if(TVPIsExistentStorage(newname))
				{
					// file found
					name = newname;
					break;
				}
				i++;
			}
			if(i.IsNull())
			{
				// not found
				handler = NULL;
				break;
			}

			handler = & i.GetValue();
			break;
		}
		else
		{
			if(!TVPIsExistentStorage(name))
			{
				// not found
				ext.Clear();
				continue; // retry searching
			}
			handler = TVPGraphicType.Hash.Find(ext);
			break;
		}
	}

	if(handler)
	{
		// open the mask file
		holder.Open(name);

		// fill "data"'s member
	    data.Type = lgtMask;
	    data.Name = name;
		data.Buffer = NULL;
		data.DesW = desw;
		data.DesH = desh;
		data.NeedMetaInfo = false;

	    try
	    {
			// load image via handler
			(handler->Load)(handler->FormatData, (void*)&data,
				TVPLoadGraphic_SizeCallback, TVPLoadGraphic_ScanLineCallback,
				NULL,
				holder.Get(), -1, glmGrayscale);
	    }
		catch(...)
	    {
			if(data.Buffer) delete [] data.Buffer;
			throw;
		}

	    if(data.Buffer) delete [] data.Buffer;
	}
#endif

	// do color matting
	if(doalphacolormat)
	{
		// alpha color mat
		TVPDoAlphaColorMat(dest, alphamatcolor);
	}

	return true;
}
//---------------------------------------------------------------------------





//---------------------------------------------------------------------------
// TVPLoadGraphic
//---------------------------------------------------------------------------
void TVPLoadGraphic(tTVPBaseBitmap *dest, const ttstr &name, tjs_int32 keyidx,
	tjs_uint desw, tjs_uint desh,
	tTVPGraphicLoadMode mode, ttstr *provincename, iTJSDispatch2 ** metainfo, bool unpadding)
{
	// decoder 作業バッファ / metainfo / cache record 等の operator new を
	// GraphicsLoader tag に振り分け。
	TVPAllocTagScope _alloc_tag_scope("GraphicsLoader");
	// loading with cache management
	// cache key は autopath 解決後の物理 path に統一する。
	// "bg.jpg" (autopath 経由) と "image/bg.jpg" (直接 path) の両方が同じ
	// 物理 file を指す場合、cache キーが揃って二重 cache を回避できる。
	ttstr nname = TVPResolveCachePath(name);
	tjs_uint32 hash;
	tTVPGraphicsSearchData searchdata;

	if(TVPGraphicCacheEnabled)
	{
		searchdata.Name = nname.AsStdString();
		searchdata.KeyIdx = keyidx;
		searchdata.Mode = mode;
		searchdata.DesW = desw;
		searchdata.DesH = desh;

		hash = tTVPGraphicCache::MakeHash(searchdata);

		{
			tTJSCriticalSectionHolder cs(TVPGraphicCacheCS);
			tTVPGraphicImageHolder * ptr =
				TVPGraphicCache.FindAndTouchWithHash(searchdata, hash);
			if(ptr)
			{
				// found in cache
				ptr->GetObjectNoAddRef()->AssignToBitmap(dest);
				if(provincename) *provincename = ptr->GetObjectNoAddRef()->ProvinceName.c_str();
				if(metainfo)
					*metainfo = TVPMetaInfoPairsToDictionary(ptr->GetObjectNoAddRef()->MetaInfo);
				return;
			}
		}

		// 既定キー (TVP_clNone, glmNormal, 0, 0) のとき、async prefetch が
		// 進行中なら完了まで待ってからキャッシュ再検索する。
		// prefetch は常に既定キーで登録されるので、それ以外の引数で
		// 呼ばれた場合は待っても意味がない (= 同期 decode に直行する)
		if(keyidx == TVP_clNone && mode == glmNormal && desw == 0 && desh == 0)
		{
			if(TVPWaitForImagePrefetch(nname, 10 * 1000))
			{
				// 完了通知あり → キャッシュ再検索
				tTJSCriticalSectionHolder cs(TVPGraphicCacheCS);
				tTVPGraphicImageHolder * ptr =
					TVPGraphicCache.FindAndTouchWithHash(searchdata, hash);
				if(ptr)
				{
					ptr->GetObjectNoAddRef()->AssignToBitmap(dest);
					if(provincename) *provincename = ptr->GetObjectNoAddRef()->ProvinceName.c_str();
					if(metainfo)
						*metainfo = TVPMetaInfoPairsToDictionary(ptr->GetObjectNoAddRef()->MetaInfo);
					return;
				}
				// 完了したのにキャッシュにない = エラー終了。同期 decode へフォールスルー
			}
		}
	}

	// not found

	// load into dest
	tTVPGraphicImageData * data = NULL;

	ttstr pn;
	ttstr resolved_name; // 拡張子補完後の実 path (P3 file→decode auto-drop 用)
	std::vector<tTVPGraphicMetaInfoPair> * mi = NULL;
	try
	{
		TVPInternalLoadGraphic(dest, nname, keyidx, desw, desh, &mi, mode, &pn, unpadding, &resolved_name);

		if(provincename) *provincename = pn;
		if(metainfo)
			*metainfo = TVPMetaInfoPairsToDictionary(mi);

		if(TVPGraphicCacheEnabled)
		{
			data = new tTVPGraphicImageData();
			data->AssignBitmap(dest);
			data->ProvinceName = pn.c_str();
			data->MetaInfo = mi; // now mi is managed under tTVPGraphicImageData
			mi = NULL;
			// P2: pin 集合に登録済みなら pinned=true で初期化
			data->SetPinned(TVPIsCachePathPinned(nname));

			{
				tTJSCriticalSectionHolder cs(TVPGraphicCacheCS);
				// check size limit
				TVPCheckGraphicCacheLimit();

				// push into hash table
				tjs_uint datasize = data->GetSize();
				TVPGraphicCacheTotalBytes += datasize;
				tTVPGraphicImageHolder holder(data);
				TVPGraphicCache.AddWithHash(searchdata, hash, holder);
			}
			// P3: decode 層に積めたので file 層 raw bytes は不要 (file→decode auto-drop)
			//     TVPGraphicCacheCS を解放してから StorageCacheCS を取る (CS 入れ子回避)。
			//     呼び出し元 nname は拡張子補完前のことがあるので、
			//     TVPInternalLoadGraphic が返した resolved_name (実 path) を使う。
			if(!resolved_name.IsEmpty()) {
				TVPClearStorageCache(resolved_name);
			}
		}
	}
	catch(...)
	{
		if(mi) delete mi;
		if(data) data->Release();
		throw;
	}

	if(mi) delete mi;
	if(data) data->Release();
}
//---------------------------------------------------------------------------



//---------------------------------------------------------------------------
// TVPTouchImages
//---------------------------------------------------------------------------
void TVPTouchImages(const std::vector<ttstr> & storages, tjs_int64 limit,
	tjs_uint64 timeout)
{
	// preload graphic files into the cache.
	// "limit" is a limit memory for preload, in bytes.
	// this function gives up when "timeout" (in ms) expired.
	// currently this function only loads normal graphics.
	// (univ.trans rule graphics nor province image may not work properly)

	if(!TVPGraphicCacheLimit) return;

	tjs_uint64 limitbytes;
	if(limit >= 0)
	{
		if( (tjs_uint64)limit > TVPGraphicCacheLimit || limit == 0)
			limitbytes = TVPGraphicCacheLimit;
		else
			limitbytes = limit;
	}
	else
	{
		// negative value of limit indicates remaining bytes after loading
		if((tjs_uint64)-limit >= TVPGraphicCacheLimit) return;
		limitbytes = TVPGraphicCacheLimit + limit;
	}

	tjs_int count = 0;
	tjs_uint64 bytes = 0;
	tjs_uint64 starttime = TVPGetTickCount();
	tjs_uint64 limittime = starttime + timeout;
	tTVPBaseBitmap tmp(32, 32, 32);
 	ttstr statusstr( (const tjs_char*)TVPInfoTouching );
	bool first = true;
	while((tjs_uint)count < storages.size())
	{
		if(timeout && TVPGetTickCount() >= limittime)
		{
			statusstr += (const tjs_char*)TVPAbortedTimeOut;
			break;
		}
		if(bytes >= limitbytes)
		{
			statusstr += (const tjs_char*)TVPAbortedLimitByte;
			break;
		}

		try
		{
			if(!first) statusstr += TJS_W(", ");
			first = false;
			statusstr += storages[count];

			TVPLoadGraphic(&tmp, storages[count++], TVP_clNone,
				0, 0, glmNormal, NULL); // load image

			// get image size
			tTVPGraphicImageData * data = new tTVPGraphicImageData();
			try
			{
				data->AssignBitmap(&tmp);
				bytes += data->GetSize();
			}
			catch(...)
			{
				data->Release();
				throw;
			}
			data->Release();
		}
		catch(eTJS &e)
		{
			statusstr += TJS_W("(error!:");
			statusstr += e.GetMessage();
			statusstr += TJS_W(")");
		}
		catch(...)
		{
			// ignore all errors
		}
	}

	// re-touch graphic cache to ensure that more earlier graphics in storages
	// array can get more priority in cache order.
	count--;
	for(;count >= 0; count--)
	{
		tTVPGraphicsSearchData searchdata;
		searchdata.Name = TVPNormalizeStorageName(storages[count]).AsStdString();
		searchdata.KeyIdx = TVP_clNone;
		searchdata.Mode = glmNormal;
		searchdata.DesW = 0;
		searchdata.DesH = 0;

		tjs_uint32 hash = tTVPGraphicCache::MakeHash(searchdata);

		TVPGraphicCache.FindAndTouchWithHash(searchdata, hash);
	}

	statusstr += TJS_W(" (elapsed ");
	statusstr += ttstr((tjs_int)(TVPGetTickCount() - starttime));
	statusstr += TJS_W("ms)");

	TVPAddLog(statusstr);
}
//---------------------------------------------------------------------------







//---------------------------------------------------------------------------
// TVPSetGraphicCacheLimit
//---------------------------------------------------------------------------
void TVPSetGraphicCacheLimit(tjs_uint64 limit)
{
	// set limit of graphic cache by total bytes.
	if(limit == 0 )
	{
		TVPGraphicCacheLimit = limit;
		TVPGraphicCacheEnabled = false;
	}
	else if(limit == -1)
	{
		TVPGraphicCacheLimit = TVPGraphicCacheSystemLimit;
		TVPGraphicCacheEnabled = true;
	}
	else
	{
		if(limit > TVPGraphicCacheSystemLimit)
			limit = TVPGraphicCacheSystemLimit;
		TVPGraphicCacheLimit = limit;
		TVPGraphicCacheEnabled = true;
	}

	TVPCheckGraphicCacheLimit();
}
//---------------------------------------------------------------------------
tjs_uint64 TVPGetGraphicCacheLimit()
{
	return TVPGraphicCacheLimit;
}
//---------------------------------------------------------------------------




