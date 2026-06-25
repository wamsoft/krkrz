//---------------------------------------------------------------------------
/*
	TJS2 Script Engine
	Copyright (C) 2000 W.Dee <dee@kikyou.info> and contributors

	See details of license at "license.txt"
*/
//---------------------------------------------------------------------------
// tjs common header
//---------------------------------------------------------------------------


/*
	Add headers that would not be frequently changed.
*/
#ifndef tjsCommHeadH
#define tjsCommHeadH

#ifdef _WIN32
#define _CRT_SECURE_NO_WARNINGS 1
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX 
#include "targetver.h"
#include <windows.h>
#include <objidl.h> // for IStream
#include <wchar.h> // for wchar_t

#ifdef  _DEBUG
//#define _CRTDBG_MAP_ALLOC
//#include <cstdlib>
//#include <crtdbg.h>
//#define TJS_CRTDBG_MAP_ALLOC
#endif  // _DEBUG

/*
#ifndef DEBUG_NEW
#ifdef  _DEBUG
#define DEBUG_NEW   ::new(_NORMAL_BLOCK, __FILE__, __LINE__)
#else   // _DEBUG
#define DEBUG_NEW   new
#endif  // _DEBUG
#endif  // DEBUG_NEW
#define new DEBUG_NEW
*/

#define STDCALL __stdcall

#else

#define STDCALL
typedef void* HWND;

#endif

#include <string.h>
#include <stdlib.h>
#include <stdio.h>

#include <vector>
#include <string>
#include <stdexcept>
#include <memory>

#include "tjsConfig.h"
#include "tjs.h"

//---------------------------------------------------------------------------
#endif


