/*---------------------------------------------------------------------------*/
/*
	TJS2 Script Engine
	Copyright (C) 2000 W.Dee <dee@kikyou.info> and contributors

	See details of license at "license.txt"
*/
/*---------------------------------------------------------------------------*/
/* "TJS2" type definitions                                                   */
/*---------------------------------------------------------------------------*/

#ifndef __TJSTYPES_H__
#define __TJSTYPES_H__

#include <stdint.h>
#include <stddef.h>

#ifdef HAVE_CONFIG_H
 #include "config.h"

 #ifndef HAVE_STRINGIZE
 # error "preprocessor stringize required."
 #endif

 #if SIZEOF_INT < 4
 # error "sizeof(int) must be larger than or equal to 4."
 #endif
#endif /* end of HAVE_CONFIG_H */



/* Functions that needs to be exported ( for non-class-member functions ) */
/* This should only be applyed for function declaration in headers ( not body ) */
#define TJS_EXP_FUNC_DEF(rettype, name, arg) extern rettype name arg
#define TJS_EXP_FUNC_DEF_ENV(env, rettype, name, arg) extern rettype name arg

/* Functions that needs to be exported ( for class-member functions ) */
#define TJS_METHOD_DEF(rettype, name, arg) rettype name arg
#define TJS_METHOD_DEF_ENV(env, rettype, name, arg) rettype name arg
#define TJS_CONST_METHOD_DEF(rettype, name, arg) rettype name arg const
#define TJS_CONST_METHOD_DEF_ENV(env, rettype, name, arg) rettype name arg const	
#define TJS_STATIC_METHOD_DEF(rettype, name, arg) static rettype name arg
#define TJS_STATIC_METHOD_DEF_ENV(env, rettype, name, arg) static rettype name arg
#define TJS_STATIC_CONST_METHOD_DEF(rettype, name, arg) static rettype name arg const
#define TJS_STATIC_CONST_METHOD_DEF_ENV(env, rettype, name, arg) static rettype name arg const
#define TJS_METHOD_RET_EMPTY
#define TJS_METHOD_RET(type)

/*[*/
typedef int8_t tjs_int8;
typedef uint8_t tjs_uint8;
typedef int16_t tjs_int16;
typedef uint16_t tjs_uint16;
typedef int32_t tjs_int32;
typedef uint32_t tjs_uint32;
typedef int64_t tjs_int64;
typedef uint64_t tjs_uint64;

typedef char tjs_nchar;
typedef double tjs_real;

typedef int tjs_int;
typedef unsigned int tjs_uint;

typedef intptr_t tjs_intptr_t;
typedef uintptr_t tjs_uintptr_t;


#if !defined(_WIN32)
typedef struct tagBITMAPINFOHEADER{
    uint32_t      biSize;
    int32_t       biWidth;
    int32_t       biHeight;
    uint16_t       biPlanes;
    uint16_t       biBitCount;
    uint32_t      biCompression;
    uint32_t      biSizeImage;
    int32_t       biXPelsPerMeter;
    int32_t       biYPelsPerMeter;
    uint32_t      biClrUsed;
    uint32_t      biClrImportant;
} BITMAPINFOHEADER, *PBITMAPINFOHEADER;

typedef struct tagRGBQUAD {
	uint8_t    rgbBlue;
	uint8_t    rgbGreen;
	uint8_t    rgbRed;
	uint8_t    rgbReserved;
} RGBQUAD;

typedef struct tagBITMAPINFO {
    BITMAPINFOHEADER    bmiHeader;
    RGBQUAD             bmiColors[1];
} BITMAPINFO, *PBITMAPINFO;

typedef int32_t HRESULT;
#define S_OK 0
#define E_FAIL 0x80004005
#endif


#if defined(_WIN32)  && !defined(__GNUC__)

#define TJS_HOST_IS_BIG_ENDIAN 0
#define TJS_HOST_IS_LITTLE_ENDIAN 1

#ifndef TJS_INTF_METHOD
#define TJS_INTF_METHOD __cdecl
	/* TJS_INTF_METHOD is "cdecl" (by default)
		since TJS2 2.4.14 (kirikir2 2.25 beta 1) */
#endif

#define TJS_USERENTRY __cdecl

#define TJS_I64_VAL(x) ((tjs_int64)(x##i64))
#define TJS_UI64_VAL(x) ((tjs_uint64)(x##i64))

#ifdef _M_X64
#define TJS_64BIT_OS	/* 64bit windows */
#endif

#else

/* gcc ? */

#ifndef __GNUC__
 #error "GNU C++ required."
#endif
/*
#ifndef HAVE_CONFIG_H
 #error "-DHAVE_CONFIG_H and config.h required."
#endif
*/


#define TJS_I64_VAL(x) ((tjs_int64)(x##LL))
#define TJS_UI64_VAL(x) ((tjs_uint64)(x##LL))

#ifdef WORDS_BIGENDIAN
	#define TJS_HOST_IS_BIG_ENDIAN 1
	#define TJS_HOST_IS_LITTLE_ENDIAN 0
#else
	#define TJS_HOST_IS_BIG_ENDIAN 0
	#define TJS_HOST_IS_LITTLE_ENDIAN 1
#endif

#define TJS_INTF_METHOD
#define TJS_USERENTRY

#endif /* end of defined(_WIN32) && !defined(__GNUC__) */

#define _TJS_W(str)     TJS_W(str)
#define __TJSW_FILE__          _TJS_W(__FILE__)
#define __TJSW_FUNCTION__      _TJS_W(__FUNCTION__)
/*]*/

/*[*/
#define TJS_N(X) X

#ifdef __WINVER__

#ifdef __cplusplus
typedef wchar_t tjs_char;
typedef std::wstring tjs_string;
typedef std::wstring_view tjs_string_view;
#else
typedef tjs_uint16 tjs_char;
#endif

#define TJS_W(X) L##X

#else

#ifdef __cplusplus
typedef char16_t tjs_char;
typedef std::u16string tjs_string;
typedef std::u16string_view tjs_string_view;
#else
typedef tjs_uint16 tjs_char;
#endif

#define TJS_W(X) u##X

#endif


typedef tjs_int32 tjs_error;

typedef tjs_int64 tTVInteger;
typedef tjs_real tTVReal;

typedef size_t tjs_size;
typedef ptrdiff_t tjs_offset;

/* IEEE double manipulation support
 (TJS requires IEEE double(64-bit float) native support on machine or C++ compiler) */

/*

63 62       52 51                         0
+-+-----------+---------------------------+
|s|    exp    |         significand       |
+-+-----------+---------------------------+

s = sign,  negative if this is 1, otherwise positive.



*/

/* double related constants */
#define TJS_IEEE_D_EXP_MAX 1023
#define TJS_IEEE_D_EXP_MIN -1022
#define TJS_IEEE_D_SIGNIFICAND_BITS 52

#define TJS_IEEE_D_EXP_BIAS 1023

/* component extraction */
#define TJS_IEEE_D_SIGN_MASK              (TJS_UI64_VAL(0x8000000000000000))
#define TJS_IEEE_D_EXP_MASK               (TJS_UI64_VAL(0x7ff0000000000000))
#define TJS_IEEE_D_SIGNIFICAND_MASK       (TJS_UI64_VAL(0x000fffffffffffff))
#define TJS_IEEE_D_SIGNIFICAND_MSB_MASK   (TJS_UI64_VAL(0x0008000000000000))

#define TJS_IEEE_D_GET_SIGN(x)   (0!=(x & TJS_IEEE_D_SIGN_MASK))
#define TJS_IEEE_D_GET_EXP(x)  ((tjs_int)(((x & TJS_IEEE_D_EXP_MASK) >> \
								TJS_IEEE_D_SIGNIFICAND_BITS) - TJS_IEEE_D_EXP_BIAS))
#define TJS_IEEE_D_GET_SIGNIFICAND(x) (x & TJS_IEEE_D_SIGNIFICAND_MASK)

/* component composition */
#define TJS_IEEE_D_MAKE_SIGN(x)  ((x)?TJS_UI64_VAL(0x8000000000000000):TJS_UI64_VAL(0))
#define TJS_IEEE_D_MAKE_EXP(x)   ((tjs_uint64)(x + TJS_IEEE_D_EXP_BIAS) << 52)
#define TJS_IEEE_D_MAKE_SIGNIFICAND(x) ((tjs_uint64)(x))

/* special expression */
 /* (quiet) NaN */
  #define TJS_IEEE_D_P_NaN (tjs_uint64)(TJS_IEEE_D_EXP_MASK|TJS_IEEE_D_SIGNIFICAND_MSB_MASK)
  #define TJS_IEEE_D_N_NaN (tjs_uint64)(TJS_IEEE_D_SIGN_MASK|TJS_IEEE_D_P_NaN)
 /* infinite */
  #define TJS_IEEE_D_P_INF (tjs_uint64)(TJS_IEEE_D_EXP_MASK)
  #define TJS_IEEE_D_N_INF (tjs_uint64)(TJS_IEEE_D_SIGN_MASK|TJS_IEEE_D_P_INF)

/* special expression check */
  #define TJS_IEEE_D_IS_NaN(x) ((TJS_IEEE_D_EXP_MASK & (x)) == TJS_IEEE_D_EXP_MASK) && \
				(((x) & TJS_IEEE_D_SIGNIFICAND_MSB_MASK) || \
				(!((x) & TJS_IEEE_D_SIGNIFICAND_MSB_MASK) && \
				((x) & (TJS_IEEE_D_SIGNIFICAND_MASK ^ TJS_IEEE_D_SIGNIFICAND_MSB_MASK))))
  #define TJS_IEEE_D_IS_INF(x) (((TJS_IEEE_D_EXP_MASK & (x)) == TJS_IEEE_D_EXP_MASK) && \
				(!((x) & TJS_IEEE_D_SIGNIFICAND_MASK)))

/*]*/

#endif
