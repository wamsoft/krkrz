#define NOMINMAX
#include "tjsCommHead.h"
#include "BasicDrawDevice.h"
#include "SysInitIntf.h"      // TVPGetCommandLine
#include "DebugIntf.h"        // TVPAddLog / TVPAddImportantLog

#ifdef TVP_USE_OPENGL
#include "OGLDrawDevice.h"
#endif

#ifndef TVP_DISABLE_NULL_DRAWDEVICE
#include "NullDrawDevice.h"
#endif

static tTJSNativeClass *nativeClass = NULL;

//---------------------------------------------------------------------------
//! @brief 起動時の既定 DrawDevice を返す (WINVER)
//!
//! 起動オプション -drawdevice=<name> で選択できる (SDL3 版と同じ指定方法):
//!   basic : BasicDrawDevice (D3D11)。 既定
//!   ogl   : OGLDrawDevice   (OpenGL ES 直接 + Canvas/Texture/Shader/Offscreen)
//!   null  : NullDrawDevice  (描画しない。 検証用)
//!
//! 未指定時は BasicDrawDevice。 なお -drawdevice はあくまで**起動時の既定**で、
//! 実行中に Window.drawDevice へ代入して切り替えるのは従来どおり可能。
//---------------------------------------------------------------------------
tTJSNativeClass* TVPGetDefaultDrawDevice()
{
	if (!nativeClass) {
		tTJSVariant val;
		if (TVPGetCommandLine(TJS_W("-drawdevice"), &val)) {
			ttstr name(val);
			if (name == TJS_W("basic")) {
				TVPAddLog(TJS_W("GetDefaultDrawDevice: -drawdevice=basic -> BasicDrawDevice"));
				nativeClass = new tTJSNC_BasicDrawDevice();
				return nativeClass;
			}
#ifdef TVP_USE_OPENGL
			if (name == TJS_W("ogl")) {
				TVPAddLog(TJS_W("GetDefaultDrawDevice: -drawdevice=ogl -> OGLDrawDevice"));
				nativeClass = new tTJSNC_OGLDrawDevice();
				return nativeClass;
			}
#endif
#ifndef TVP_DISABLE_NULL_DRAWDEVICE
			if (name == TJS_W("null")) {
				TVPAddLog(TJS_W("GetDefaultDrawDevice: -drawdevice=null -> NullDrawDevice"));
				nativeClass = new tTJSNC_NullDrawDevice();
				return nativeClass;
			}
#endif
			// 未知の値 -> 既定へフォールバック (警告)
			TVPAddImportantLog(ttstr(TJS_W("GetDefaultDrawDevice: unknown -drawdevice=")) + name
				+ TJS_W(", fall back to default"));
		}

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
