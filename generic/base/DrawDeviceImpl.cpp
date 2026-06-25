#define NOMINMAX
#include "tjsCommHead.h"
#include "Application.h"

static tTJSNativeClass *nativeClass = NULL;

tTJSNativeClass* TVPGetDefaultDrawDevice() {
    if (!nativeClass) {
		nativeClass = Application->GetDefaultDrawDevice();
    } else {
        nativeClass->AddRef();
    }
    return nativeClass;
}
