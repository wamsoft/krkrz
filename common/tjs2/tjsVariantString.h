//---------------------------------------------------------------------------
/*
	TJS2 Script Engine
	Copyright (C) 2000 W.Dee <dee@kikyou.info> and contributors

	See details of license at "license.txt"
*/
//---------------------------------------------------------------------------
// string heap management used by tTJSVariant and tTJSString
//---------------------------------------------------------------------------
#ifndef tjsVariantStringH
#define tjsVariantStringH

#include "tjsConfig.h"
#include <stdlib.h>
#include <string.h>

namespace TJS
{

// #define TJS_DEBUG_UNRELEASED_STRING
// #define TJS_DEBUG_CHECK_STRING_HEAP_INTEGRITY
// #define TJS_DEBUG_DUMP_STRING

/*[*/
//---------------------------------------------------------------------------
// tTJSVariantString stuff
//---------------------------------------------------------------------------
#define TJS_VS_SHORT_LEN 21
/*]*/
class tTJSVariantString;
extern tjs_int TJSGetShorterStrLen(const tjs_char *str, tjs_int max);
extern tTJSVariantString * TJSAllocStringHeap(void);
extern void TJSDeallocStringHeap(tTJSVariantString * vs);
extern void TJSThrowStringAllocError();
extern void TJSThrowNarrowToWideConversionError();
extern void TJSCompactStringHeap();
#ifdef TJS_DEBUG_DUMP_STRING
extern void TJSDumpStringHeap(void);
#endif
//---------------------------------------------------------------------------




//---------------------------------------------------------------------------
// base memory allocation functions for long string
//---------------------------------------------------------------------------
TJS_EXP_FUNC_DEF(tjs_char *, TJSVS_malloc, (tjs_uint len));
TJS_EXP_FUNC_DEF(tjs_char *, TJSVS_realloc, (tjs_char *buf, tjs_uint len));
TJS_EXP_FUNC_DEF(void, TJSVS_free, (tjs_char *buf));
//---------------------------------------------------------------------------




/*[*/
//---------------------------------------------------------------------------
// tTJSVariantString
//---------------------------------------------------------------------------
#pragma pack(push, 4)
struct tTJSVariantString_S
{
	tjs_int RefCount; // reference count - 1
	tjs_char *LongString;
	tjs_char ShortString[TJS_VS_SHORT_LEN +1];
	tjs_int Length; // string length
	tjs_uint32 HeapFlag;
	tjs_uint32 Hint;
};
#pragma pack(pop)
/*]*/
#ifdef __GNUC__
#pragma GCC diagnostic push
//#pragma GCC diagnostic ignored "-Wundefined-bool-conversion"
#endif
/*start-of-tTJSVariantString*/
class tTJSVariantString : public tTJSVariantString_S
{
public:

	TJS_METHOD_DEF(void, AddRef, ())
	{
		RefCount++;
	}

	TJS_METHOD_DEF(void, Release, ());

	// 旧実装は冒頭で `if(LongString) TJSVS_free(LongString)` してから ref を
	// 読みに行っていたため、ref が自分の LongString を指していると use-after-free。
	// (tTJSString::operator=(const tjs_char *) → ResetString → SetString と入ってきて
	//  rhs が自身の c_str() を指すような degenerate な代入で踏める)
	// alloc/copy を先に走らせて、最後に旧 LongString を解放する形に変更。
	// TJSVS_malloc が throw した場合も Length / LongString は変更されないので
	// 強い例外安全性も同時に得られる。
	TJS_METHOD_DEF(void, SetString, (const tjs_char *ref, tjs_int maxlen = -1))
	{
		tjs_int len;
		if(maxlen != -1)
			len = TJSGetShorterStrLen(ref, maxlen);
		else
			len = (tjs_int)TJS_strlen(ref);

		if(len>TJS_VS_SHORT_LEN)
		{
			tjs_char *newLong = TJSVS_malloc(len+1);
			TJS_strcpy_maxlen(newLong, ref, len);
			if(LongString) TJSVS_free(LongString);
			LongString = newLong;
		}
		else
		{
			// ShortString は struct 内領域なので LongString とは別物。
			// ref が旧 LongString を指していてもまだ生きている状態でコピーできる。
			TJS_strcpy_maxlen(ShortString, ref, len);
			if(LongString) TJSVS_free(LongString), LongString = NULL;
		}
		Length = len;
	}

	TJS_METHOD_DEF(void, SetString, (const tjs_nchar *ref))
	{
		// alloc-then-free 順に揃える (tjs_char 版と同じ理由 + 強い例外安全性)。
		tjs_int len = (tjs_int)TJS_narrowtowidelen(ref);
		if(len == -1) TJSThrowNarrowToWideConversionError();

		if(len>TJS_VS_SHORT_LEN)
		{
			tjs_char *newLong = TJSVS_malloc(len+1);
			newLong[TJS_narrowtowide(newLong, ref, len)] = 0;
			if(LongString) TJSVS_free(LongString);
			LongString = newLong;
		}
		else
		{
			ShortString[TJS_narrowtowide(ShortString, ref, TJS_VS_SHORT_LEN)] = 0;
			if(LongString) TJSVS_free(LongString), LongString = NULL;
		}
		Length = len;
	}

	TJS_METHOD_DEF(void, AllocBuffer, (tjs_uint len))
	{
		/* note that you must call FixLength if you allocate larger than the
			actual string size */

		if(LongString) TJSVS_free(LongString), LongString = NULL;

		Length = len;
		if(len>TJS_VS_SHORT_LEN)
		{
			LongString = TJSVS_malloc(len+1);
			LongString[len] = 0;
		}
		else
		{
			ShortString[len] = 0;
		}
	}

	TJS_METHOD_DEF(void, ResetString, (const tjs_char *ref))
	{
		// 旧実装は明示的に LongString を free してから SetString を呼んでいた。
		// SetString 側で旧 LongString の管理 (self-ref 安全 + alloc 失敗時の roll-back) が
		// 完結するようになったため、ここでの先行 free は不要 (むしろ self-ref UAF の
		// 原因だった)。実体的には SetString と同義になるが互換のため残置。
		SetString(ref);
	}


	TJS_METHOD_DEF(void, AppendBuffer, (tjs_uint applen))
	{
		/* note that you must call FixLength if you allocate larger than the
			actual string size */

		// assume this != NULL
		tjs_int newlen = Length += applen;
		if(LongString)
		{
			// still long string
			LongString = TJSVS_realloc(LongString, newlen + 1);
			LongString[newlen] = 0;
			return;
		}
		else
		{
			if(newlen <= TJS_VS_SHORT_LEN)
			{
				// still short string
				ShortString[newlen] = 0;
				return;
			}
			// becomes a long string
			tjs_char *newbuf = TJSVS_malloc(newlen+1);
			TJS_strcpy(newbuf, ShortString);
			LongString = newbuf;
			LongString[newlen] = 0;
			return;
		}

	}

	TJS_METHOD_DEF(void, Append, (const tjs_char *str))
	{
		// assume this != NULL
		Append(str, (tjs_int)TJS_strlen(str));
	}

	TJS_METHOD_DEF(void, Append, (const tjs_char *str, tjs_int applen))
	{
		// assume this != NULL
		tjs_int orglen = Length;
		tjs_int newlen = Length += applen;
		if(LongString)
		{
			// still long string
			LongString = TJSVS_realloc(LongString, newlen + 1);
			TJS_strcpy(LongString+orglen, str);
			return;
		}
		else
		{
			if(newlen <= TJS_VS_SHORT_LEN)
			{
				// still short string
				TJS_strcpy(ShortString + orglen, str);
				return;
			}
			// becomes a long string
			tjs_char *newbuf = TJSVS_malloc(newlen+1);
			TJS_strcpy(newbuf, ShortString);
			TJS_strcpy(newbuf+orglen, str);
			LongString = newbuf;
			return;
		}
	}

	TJS_CONST_METHOD_DEF(TJS_METHOD_RET(const tjs_char *), operator const tjs_char *, ())
	{
		return LongString?LongString:ShortString;
	}

	TJS_CONST_METHOD_DEF(tjs_int, GetLength, ())
	{
		return Length;
	}

	TJS_METHOD_DEF(tTJSVariantString *, FixLength, ());

	TJS_METHOD_DEF(tjs_uint32 *, GetHint, ()) { return &Hint; }

	TJS_CONST_METHOD_DEF(tTVInteger, ToInteger, ());
	TJS_CONST_METHOD_DEF(tTVReal, ToReal, ());
	TJS_CONST_METHOD_DEF(void, ToNumber, (tTJSVariant &dest));

	TJS_CONST_METHOD_DEF(tjs_int, GetRefCount, ())
	{
		return RefCount;
	}

	tjs_int QueryPersistSize() const
	{
		return sizeof(tjs_uint) +
			GetLength() * sizeof(tjs_char);
	}

	void Persist(tjs_uint8 *dest) const
	{
		tjs_uint size;
		const tjs_char *ptr = LongString?LongString:ShortString;
		*(tjs_uint*)dest = size = GetLength();
		dest += sizeof(tjs_uint);
		while(size--)
		{
			*(tjs_char*)dest = *ptr;
			dest += sizeof(tjs_char);
			ptr++;
		}
	}
};
/*end-of-tTJSVariantString*/
//---------------------------------------------------------------------------
#ifdef __GNUC__
#pragma GCC diagnostic pop
#endif




//---------------------------------------------------------------------------
TJS_EXP_FUNC_DEF(tTJSVariantString *, TJSAllocVariantString, (const tjs_char *ref1,
	const tjs_char *ref2));
TJS_EXP_FUNC_DEF(tTJSVariantString *, TJSAllocVariantString, (const tjs_char *ref, tjs_int n));
TJS_EXP_FUNC_DEF(tTJSVariantString *, TJSAllocVariantString, (const tjs_char *ref));
TJS_EXP_FUNC_DEF(tTJSVariantString *, TJSAllocVariantString, (const tjs_nchar *ref));
TJS_EXP_FUNC_DEF(tTJSVariantString *, TJSAllocVariantString, (const tjs_uint8 **src));
TJS_EXP_FUNC_DEF(tTJSVariantString *, TJSAllocVariantStringBuffer, (tjs_uint len));
TJS_EXP_FUNC_DEF(tTJSVariantString *, TJSAppendVariantString, (tTJSVariantString *str,
	const tjs_char *app));
TJS_EXP_FUNC_DEF(tTJSVariantString *, TJSAppendVariantString, (tTJSVariantString *str,
	const tTJSVariantString *app));
TJS_EXP_FUNC_DEF(tTJSVariantString *, TJSFormatString, (const tjs_char *format, tjs_uint numparams,
	tTJSVariant **params));

//---------------------------------------------------------------------------
} // namespace TJS
#endif
