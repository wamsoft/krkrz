#include "tjsCommHead.h"
#include "OGLApplication.h"

OGLApplication::OGLApplication() 
: tTVPApplication() 
{
}

#include "OGLDrawDevice.h"

// アプリ処理用の DrawDevice実装を返す
tTJSNativeClass* 
OGLApplication::GetDefaultDrawDevice()
{
    return new tTJSNC_OGLDrawDevice();
}
