//---------------------------------------------------------------------------
/*
	TVP2 ( T Visual Presenter 2 )  A script authoring tool
	Copyright (C) 2000 W.Dee <dee@kikyou.info> and contributors

	See details of license at "license.txt"
*/
//---------------------------------------------------------------------------
// Clipboard Class interface
//---------------------------------------------------------------------------
#include "tjsCommHead.h"
#include "MsgIntf.h"
#include "Exception.h"
#include "ClipboardIntf.h"

//---------------------------------------------------------------------------
// clipboard related functions
//---------------------------------------------------------------------------
bool TVPClipboardHasFormat(tTVPClipboardFormat format)
{
	switch(format) {
		case cbfText: {
		bool result = false;
		if( ::OpenClipboard(0) ) {
			result = 0 != ::IsClipboardFormatAvailable(CF_TEXT);
			if( result == false ) {
				result = 0 != ::IsClipboardFormatAvailable(CF_UNICODETEXT);
			}
			::CloseClipboard();
		}
		return result; // ANSI text or UNICODE text
		}
	case cbfBitmap:
		return 0 != ::IsClipboardFormatAvailable(CF_DIB);
	default:
		return false;
	}
}
//---------------------------------------------------------------------------
void TVPClipboardSetText(const ttstr & text)
{
	if( ::OpenClipboard(0) ) {
		EmptyClipboard();

		HGLOBAL unicodehandle = NULL;
		try {
			// UNICODE 文字列のみを格納する。CF_TEXT (ANSI) は Windows が
			// CF_UNICODETEXT から自動的に合成して要求元に提供するため、明示的に
			// 設定する必要はない。旧実装は ANSI 版も別途書いていたが、冗長な上に
			// AsNarrowStdString での ANSI 変換で非 ANSI 文字が欠落する劣化があった。
			unicodehandle = ::GlobalAlloc(GMEM_DDESHARE | GMEM_MOVEABLE, (text.GetLen() + 1) * sizeof(tjs_char));
			if(!unicodehandle) TVPThrowExceptionMessage( TVPFaildClipboardCopy );

			tjs_char *unimem = (tjs_char*)::GlobalLock(unicodehandle);
			if(unimem) TJS_strcpy(unimem, text.c_str());
			::GlobalUnlock(unicodehandle);

			// SetClipboardData 成功後は HGLOBAL の所有権がクリップボードに移る。
			::SetClipboardData( CF_UNICODETEXT, unicodehandle );
			unicodehandle = NULL;
		} catch(...) {
			if(unicodehandle) ::GlobalFree(unicodehandle);
			::CloseClipboard();
			throw;
		}
		::CloseClipboard();
	}
}
//---------------------------------------------------------------------------
bool TVPClipboardGetText(ttstr & text)
{
	if(!::OpenClipboard(NULL)) return false;

	bool result = false;
	try
	{
		// select CF_UNICODETEXT or CF_TEXT
		UINT formats[2] = { CF_UNICODETEXT, CF_TEXT};
		int format = ::GetPriorityClipboardFormat(formats, 2);

		if(format == CF_UNICODETEXT)
		{
			// try to read unicode text
			HGLOBAL hglb = (HGLOBAL)::GetClipboardData(CF_UNICODETEXT);
			if(hglb != NULL)
			{
				const tjs_char *p = (const tjs_char *)::GlobalLock(hglb);
				if(p)
				{
					try
					{
						text = ttstr(p);
						result = true;
					}
					catch(...)
					{
						::GlobalUnlock(hglb);
						throw;
					}
					::GlobalUnlock(hglb);
				}
			}
		}
		else if(format == CF_TEXT)
		{
			// try to read ansi text
			HGLOBAL hglb = (HGLOBAL)::GetClipboardData(CF_TEXT);
			if(hglb != NULL)
			{
				const char *p = (const char *)::GlobalLock(hglb);
				if(p)
				{
					try
					{
						text = ttstr(p);
						result = true;
					}
					catch(...)
					{
						::GlobalUnlock(hglb);
						throw;
					}
					::GlobalUnlock(hglb);
				}
			}
		}
	}
	catch(...)
	{
		::CloseClipboard();
		throw;
	}
	::CloseClipboard();

	return result;
}
//---------------------------------------------------------------------------
//---------------------------------------------------------------------------
// ビットマップの受け渡し (Clipboard.setAsBitmap / getAsBitmap)
//
// もとは clipboardEx.dll (吉里吉里2 の標準プラグイン) が持っていた機能。
// 使われていたのは画像の出し入れだけだったので本体へ移した。
//
// 置くときは CF_DIB (32bpp / BI_RGB / ボトムアップ)。他のアプリへ渡す都合で
// α は 255 に潰す (クリップボードの DIB のα値は解釈するアプリとしないアプリが
// 混在するため。もとのプラグインも同じ扱いだった)。
// 取るときは GDI に変換させる。パレットや 16/24bpp、BI_BITFIELDS などを自前で
// 展開すると面倒なので、CreateDIBSection で作った 32bpp トップダウンの面へ
// StretchDIBits で描き込んで読み出す。
//---------------------------------------------------------------------------
bool TVPClipboardSetBitmap(const void * bits, tjs_int width, tjs_int height,
	tjs_int pitch)
{
	if(!bits || width <= 0 || height <= 0) return false;

	const size_t linesize = (size_t)width * 4;
	const size_t total = sizeof(BITMAPINFOHEADER) + linesize * height;

	HGLOBAL handle = ::GlobalAlloc(GMEM_MOVEABLE, total);
	if(!handle) return false;

	bool result = false;
	tjs_uint8 * mem = (tjs_uint8*)::GlobalLock(handle);
	if(mem)
	{
		BITMAPINFOHEADER * head = (BITMAPINFOHEADER*)mem;
		ZeroMemory(head, sizeof(*head));
		head->biSize = sizeof(BITMAPINFOHEADER);
		head->biWidth = width;
		head->biHeight = height; // 正値 = ボトムアップ
		head->biPlanes = 1;
		head->biBitCount = 32;
		head->biCompression = BI_RGB;
		head->biSizeImage = (DWORD)(linesize * height);

		tjs_uint8 * dest = mem + sizeof(BITMAPINFOHEADER);
		const tjs_uint8 * src = (const tjs_uint8*)bits;
		for(tjs_int y = 0; y < height; y++)
		{
			// ボトムアップなので行を逆に積む
			const tjs_uint32 * s = (const tjs_uint32*)(src + (tjs_int64)pitch * y);
			tjs_uint32 * d = (tjs_uint32*)(dest + linesize * (height - 1 - y));
			for(tjs_int x = 0; x < width; x++)
				d[x] = s[x] | 0xff000000; // α は 255 に
		}
		::GlobalUnlock(handle);

		if(::OpenClipboard(NULL))
		{
			::EmptyClipboard();
			// 成功したら HGLOBAL の所有権はクリップボードへ移る
			if(::SetClipboardData(CF_DIB, handle)) { handle = NULL; result = true; }
			::CloseClipboard();
		}
	}

	if(handle) ::GlobalFree(handle);

	return result;
}
//---------------------------------------------------------------------------
bool TVPClipboardGetBitmap(std::vector<tjs_uint32> & dest, tjs_int & width,
	tjs_int & height)
{
	if(!::IsClipboardFormatAvailable(CF_DIB)) return false;
	if(!::OpenClipboard(NULL)) return false;

	bool result = false;
	HDC dc = NULL;
	HBITMAP bmp = NULL;
	HGLOBAL hglb = NULL;
	const BITMAPINFO * info = NULL;

	try
	{
		hglb = (HGLOBAL)::GetClipboardData(CF_DIB);
		if(hglb) info = (const BITMAPINFO *)::GlobalLock(hglb);
		if(info)
		{
			const BITMAPINFOHEADER & src = info->bmiHeader;
			tjs_int w = (tjs_int)src.biWidth;
			tjs_int h = (tjs_int)(src.biHeight < 0 ? -src.biHeight : src.biHeight);

			if(w > 0 && h > 0)
			{
				// 画素データの先頭 (ヘッダ + カラーテーブル)
				tjs_int colors = (tjs_int)src.biClrUsed;
				if(colors == 0 && src.biBitCount <= 8) colors = 1 << src.biBitCount;
				size_t offset = src.biSize;
				if(src.biCompression == BI_BITFIELDS) offset += 3 * sizeof(DWORD);
				offset += (size_t)colors * sizeof(RGBQUAD);
				const void * srcbits = (const tjs_uint8*)info + offset;

				// 32bpp トップダウンの面を作って GDI に変換させる
				BITMAPINFOHEADER destinfo;
				ZeroMemory(&destinfo, sizeof(destinfo));
				destinfo.biSize = sizeof(BITMAPINFOHEADER);
				destinfo.biWidth = w;
				destinfo.biHeight = -h; // 負値 = トップダウン
				destinfo.biPlanes = 1;
				destinfo.biBitCount = 32;
				destinfo.biCompression = BI_RGB;

				void * destbits = NULL;
				dc = ::CreateCompatibleDC(NULL);
				if(dc)
					bmp = ::CreateDIBSection(dc, (const BITMAPINFO*)&destinfo,
						DIB_RGB_COLORS, &destbits, NULL, 0);

				if(bmp && destbits)
				{
					HGDIOBJ old = ::SelectObject(dc, bmp);
					int lines = ::StretchDIBits(dc, 0, 0, w, h, 0, 0, w, h,
						srcbits, info, DIB_RGB_COLORS, SRCCOPY);
					::SelectObject(dc, old);
					::GdiFlush();

					if(lines != GDI_ERROR)
					{
						dest.resize((size_t)w * h);
						const tjs_uint32 * s = (const tjs_uint32*)destbits;
						for(size_t i = 0, cnt = (size_t)w * h; i < cnt; i++)
							dest[i] = s[i] | 0xff000000; // α は 255 に
						width = w;
						height = h;
						result = true;
					}
				}
			}
		}
	}
	catch(...)
	{
		if(bmp) ::DeleteObject(bmp);
		if(dc) ::DeleteDC(dc);
		if(info) ::GlobalUnlock(hglb);
		::CloseClipboard();
		throw;
	}

	if(bmp) ::DeleteObject(bmp);
	if(dc) ::DeleteDC(dc);
	if(info) ::GlobalUnlock(hglb);
	::CloseClipboard();

	return result;
}
//---------------------------------------------------------------------------
