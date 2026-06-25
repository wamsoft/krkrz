#define NOMINMAX
#include "tjsCommHead.h"
#include "BasicDrawDevice.h"

#ifndef TVP_DISABLE_NULL_DRAWDEVICE
#include "NullDrawDevice.h"
#endif

static tTJSNativeClass *nativeClass = NULL;

tTJSNativeClass* TVPGetDefaultDrawDevice() 
{
	if (!nativeClass) {
		#ifndef TVP_DISABLE_NULL_DRAWDEVICE
			extern int GetSystemSecurityOption(const char *name);
			if (GetSystemSecurityOption("disabled3d9") == 0) {
				nativeClass = new tTJSNC_BasicDrawDevice();
			} else {
				nativeClass = new tTJSNC_NullDrawDevice();
			}
		#else
			nativeClass = new tTJSNC_BasicDrawDevice();
		#endif
	} else {
		nativeClass->AddRef();
	}
	return nativeClass;
}
