#ifndef __OGLApplication_h
#define __OGLApplication_h

#include "Application.h"

class OGLApplication : public tTVPApplication
{
public:
	OGLApplication();
	// アプリ処理用の DrawDevice実装を返す
	virtual tTJSNativeClass* GetDefaultDrawDevice();
};

#endif
