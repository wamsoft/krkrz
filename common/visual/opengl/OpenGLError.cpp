
#include "tjsCommHead.h"
#include <assert.h>
#include "LogIntf.h"
#include "MsgIntf.h"
#include "OpenGLHeader.h"

bool _CheckGLErrorAndLog(const char *file, const int lineno, const char* funcname) 
{
	GLenum error_code = glGetError();
	if( error_code == GL_NO_ERROR ) return true;
	const char *msg = "Unknown OpenGL error";
	switch( error_code ) {
	case GL_INVALID_ENUM: msg = "GL_INVALID_ENUM"; break;
	case GL_INVALID_VALUE: msg = "GL_INVALID_VALUE"; break;
	case GL_INVALID_OPERATION: msg = "GL_INVALID_OPERATION"; break;
	case GL_OUT_OF_MEMORY: msg = "GL_OUT_OF_MEMORY"; break;
	case GL_INVALID_FRAMEBUFFER_OPERATION: msg = "GL_INVALID_FRAMEBUFFER_OPERATION"; break;
	}
	if( funcname != nullptr ) {
		TVPLog(TVPLOG_LEVEL_ERROR, file, lineno, funcname, "OpenGL error occurred: {:08x} {}", tvpfmt::make_format_args(error_code, msg));
	} else {
		TVPLog(TVPLOG_LEVEL_ERROR, file, lineno, "", "OpenGL error occurred: {:08x} {}", tvpfmt::make_format_args(error_code, msg));
	}
	return false;
}

static GLADapiproc gladload(const char *name)
{
	return (GLADapiproc)TVPGLGetProcAddress(name);
}

static bool glesInited = false;

int TVPOpenGLESVersion = 200;

int TVPGetOpenGLESVersion() { return TVPOpenGLESVersion; }

void InitGLES()
{
	if (!glesInited) {
		// glad の GLES を初期化
		int gles_version = gladLoadGLES2(gladload);
		if (!gles_version) {
			TVPThrowExceptionMessage(TJS_W("Unable to load glad GLES.\n"));
		}
		int major = GLAD_VERSION_MAJOR(gles_version);
		int minor = GLAD_VERSION_MINOR(gles_version);
		TVPLOG_INFO("Loaded GLES {}.{}", major, minor);
		glesInited = true;
	}
}
