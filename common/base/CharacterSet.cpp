//---------------------------------------------------------------------------
/*
	TVP2 ( T Visual Presenter 2 )  A script authoring tool
	Copyright (C) 2000 W.Dee <dee@kikyou.info> and contributors

	See details of license at "license.txt"
*/
//---------------------------------------------------------------------------
// Character code conversion
//---------------------------------------------------------------------------
#include "tjsCommHead.h"

#include "CharacterSet.h"
#include "MsgIntf.h"
//---------------------------------------------------------------------------
static tjs_int inline TVPUtf16UnitToUtf8(tjs_char in, char * out)
{
	// convert a single BMP character 'in' to utf-8 character 'out'
	// surrogates (0xD800-0xDFFF) are rejected
	if     (in < (1<< 7))
	{
		if(out)
		{
			out[0] = (char)in;
		}
		return 1;
	}
	else if(in < (1<<11))
	{
		if(out)
		{
			out[0] = (char)(0xc0 | (in >> 6));
			out[1] = (char)(0x80 | (in & 0x3f));
		}
		return 2;
	}
	else if(in >= 0xD800 && in <= 0xDFFF)
	{
		// surrogate halves cannot be encoded individually
		return -1;
	}
	else
	{
		if(out)
		{
			out[0] = (char)(0xe0 | (in >> 12));
			out[1] = (char)(0x80 | ((in >> 6) & 0x3f));
			out[2] = (char)(0x80 | (in & 0x3f));
		}
		return 3;
	}
}
//---------------------------------------------------------------------------
static tjs_int inline TVPCodePointToUtf8(tjs_uint32 cp, char * out)
{
	// convert a Unicode code point to utf-8
	if     (cp < 0x80)
	{
		if(out) out[0] = (char)cp;
		return 1;
	}
	else if(cp < 0x800)
	{
		if(out)
		{
			out[0] = (char)(0xc0 | (cp >> 6));
			out[1] = (char)(0x80 | (cp & 0x3f));
		}
		return 2;
	}
	else if(cp < 0x10000)
	{
		if(out)
		{
			out[0] = (char)(0xe0 | (cp >> 12));
			out[1] = (char)(0x80 | ((cp >> 6) & 0x3f));
			out[2] = (char)(0x80 | (cp & 0x3f));
		}
		return 3;
	}
	else if(cp <= 0x10FFFF)
	{
		if(out)
		{
			out[0] = (char)(0xf0 | (cp >> 18));
			out[1] = (char)(0x80 | ((cp >> 12) & 0x3f));
			out[2] = (char)(0x80 | ((cp >> 6) & 0x3f));
			out[3] = (char)(0x80 | (cp & 0x3f));
		}
		return 4;
	}
	return -1;
}
//---------------------------------------------------------------------------
tjs_int TVPWideCharToUtf8String(const tjs_char *in, char * out)
{
	// convert input wide string to output utf-8 string
	int count = 0;
	while(*in)
	{
		tjs_int n;
		tjs_uint32 cp = (tjs_uint16)*in;

		if(cp >= 0xD800 && cp <= 0xDBFF)
		{
			// high surrogate - combine with low surrogate
			tjs_uint32 low = (tjs_uint16)in[1];
			if(low >= 0xDC00 && low <= 0xDFFF)
			{
				cp = 0x10000 + ((cp - 0xD800) << 10) + (low - 0xDC00);
				n = TVPCodePointToUtf8(cp, out);
				if(n == -1) return -1;
				if(out) out += n;
				count += n;
				in += 2;
				continue;
			}
			else
			{
				return -1; // lone high surrogate
			}
		}
		else if(cp >= 0xDC00 && cp <= 0xDFFF)
		{
			return -1; // lone low surrogate
		}

		n = TVPUtf16UnitToUtf8(*in, out);
		if(n == -1) return -1;
		if(out) out += n;
		count += n;
		in++;
	}
	return count;
}
//---------------------------------------------------------------------------
tjs_int TVPWideCharToUtf8String(const tjs_char *in, tjs_uint length, char * out)
{
	// convert input wide string to output utf-8 string
	int count = 0;
	const tjs_char *end = in + length;
	while(*in && in < end)
	{
		tjs_int n;
		tjs_uint32 cp = (tjs_uint16)*in;

		if(cp >= 0xD800 && cp <= 0xDBFF)
		{
			// high surrogate - combine with low surrogate
			if(in + 1 >= end) return -1; // truncated surrogate pair
			tjs_uint32 low = (tjs_uint16)in[1];
			if(low >= 0xDC00 && low <= 0xDFFF)
			{
				cp = 0x10000 + ((cp - 0xD800) << 10) + (low - 0xDC00);
				n = TVPCodePointToUtf8(cp, out);
				if(n == -1) return -1;
				if(out) out += n;
				count += n;
				in += 2;
				continue;
			}
			else
			{
				return -1; // lone high surrogate
			}
		}
		else if(cp >= 0xDC00 && cp <= 0xDFFF)
		{
			return -1; // lone low surrogate
		}

		n = TVPUtf16UnitToUtf8(*in, out);
		if(n == -1) return -1;
		if(out) out += n;
		count += n;
		in++;
	}
	return count;
}
//---------------------------------------------------------------------------
static tjs_int inline TVPUtf8ToWideChar(const char * & in, tjs_char *out)
{
	// convert a utf-8 character from 'in' to wide character(s) 'out'
	// returns number of tjs_char written (1 or 2), or 0 on error
	const unsigned char * & p = (const unsigned char * &)in;
	if(p[0] < 0x80)
	{
		if(out) *out = (tjs_char)in[0];
		in++;
		return 1;
	}
	else if(p[0] < 0xc2)
	{
		// invalid character
		return 0;
	}
	else if(p[0] < 0xe0)
	{
		// two bytes (11bits)
		if((p[1] & 0xc0) != 0x80) return 0;
		if(out) *out = ((p[0] & 0x1f) << 6) + (p[1] & 0x3f);
		in += 2;
		return 1;
	}
	else if(p[0] < 0xf0)
	{
		// three bytes (16bits)
		if((p[1] & 0xc0) != 0x80) return 0;
		if((p[2] & 0xc0) != 0x80) return 0;
		tjs_uint32 cp = ((p[0] & 0x0f) << 12) + ((p[1] & 0x3f) << 6) + (p[2] & 0x3f);
		if(cp >= 0xD800 && cp <= 0xDFFF) return 0; // encoded surrogate half is invalid
		if(out) *out = (tjs_char)cp;
		in += 3;
		return 1;
	}
	else if(p[0] < 0xf8)
	{
		// four bytes (21bits) - produces surrogate pair for UTF-16
		if((p[1] & 0xc0) != 0x80) return 0;
		if((p[2] & 0xc0) != 0x80) return 0;
		if((p[3] & 0xc0) != 0x80) return 0;
		tjs_uint32 cp = ((p[0] & 0x07) << 18) + ((p[1] & 0x3f) << 12) +
			((p[2] & 0x3f) << 6) + (p[3] & 0x3f);
		if(cp > 0x10FFFF) return 0; // outside valid Unicode range
		if(cp >= 0x10000)
		{
			if(out)
			{
				out[0] = (tjs_char)(0xD800 + ((cp - 0x10000) >> 10));
				out[1] = (tjs_char)(0xDC00 + ((cp - 0x10000) & 0x3FF));
			}
			in += 4;
			return 2;
		}
		else
		{
			if(out) *out = (tjs_char)cp;
			in += 4;
			return 1;
		}
	}
	// 5-byte and 6-byte sequences are not valid UTF-8 (RFC 3629)
	return 0;
}
//---------------------------------------------------------------------------
tjs_int TVPUtf8ToWideCharString(const char * in, tjs_char *out)
{
	// convert input utf-8 string to output wide string
	int count = 0;
	while(*in)
	{
		tjs_int n;
		if(out)
		{
			n = TVPUtf8ToWideChar(in, out);
			if(n == 0) return -1; // invalid character found
			out += n;
		}
		else
		{
			n = TVPUtf8ToWideChar(in, NULL);
			if(n == 0) return -1; // invalid character found
		}
		count += n;
	}
	return count;
}
//---------------------------------------------------------------------------
tjs_int TVPUtf8ToWideCharString(const char * in, tjs_uint length, tjs_char *out)
{
	// convert input utf-8 string to output wide string
	int count = 0;
	const char *end = in + length;
	while(*in && in < end)
	{
		if(in + 4 > end)
		{
			// check if the utf-8 sequence fits within the remaining bytes
			const unsigned char ch = *(const unsigned char *)in;

			if(ch >= 0x80)
			{
				tjs_uint len = 0;

				if(ch < 0xc2) return -1;
				else if(ch < 0xe0) len = 2;
				else if(ch < 0xf0) len = 3;
				else if(ch < 0xf8) len = 4;
				else return -1; // 5-byte and 6-byte are invalid

				if(in + len > end) return -1;
			}
		}

		tjs_int n;
		if(out)
		{
			n = TVPUtf8ToWideChar(in, out);
			if(n == 0) return -1; // invalid character found
			out += n;
		}
		else
		{
			n = TVPUtf8ToWideChar(in, NULL);
			if(n == 0) return -1; // invalid character found
		}
		count += n;
	}
	return count;
}
//---------------------------------------------------------------------------
bool TVPUtf8ToUtf16( tjs_string& out, const char *in ) {
	tjs_int len = TVPUtf8ToWideCharString( in, NULL );
	if( len < 0 ) return false;
	if (len == 0) {
		out.clear();
		return true;
	}
	tjs_char* buf = new tjs_char[len];
	if( buf ) {
		try {
			len = TVPUtf8ToWideCharString( in, buf );
			if( len > 0 ) out.assign( buf, len );
			delete[] buf;
		} catch(...) {
			delete[] buf;
			throw;
		}
	}
	return len > 0;
}

bool TVPUtf8ToUtf16( tjs_string& out, const std::string& in ) {
	return TVPUtf8ToUtf16(out, in.c_str());
}

//---------------------------------------------------------------------------
bool TVPUtf16ToUtf8( std::string& out, const tjs_char *in ) {
	tjs_int len = TVPWideCharToUtf8String( in, NULL );
	if( len < 0 ) return false;
	if (len == 0) {
		out.clear();
		return true;
	}
	char* buf = new char[len];
	if( buf ) {
		try {
			len = TVPWideCharToUtf8String( in, buf );
			if( len > 0 ) out.assign( buf, len );
			delete[] buf;
		} catch(...) {
			delete[] buf;
			throw;
		}
	}
	return len > 0;
}

bool TVPUtf16ToUtf8( std::string& out, const tjs_string& in ) {
	return TVPUtf16ToUtf8(out, in.c_str());
}
//---------------------------------------------------------------------------
