//---------------------------------------------------------------------------
/*
	TVP2 ( T Visual Presenter 2 )  A script authoring tool
	Copyright (C) 2000 W.Dee <dee@kikyou.info> and contributors

	See details of license at "license.txt"
*/
//---------------------------------------------------------------------------
// "Plugins" class implementation / Service for plug-ins
//---------------------------------------------------------------------------
#ifndef PluginImplH
#define PluginImplH
//---------------------------------------------------------------------------
#include <memory.h>

#include "PluginIntf.h"



//---------------------------------------------------------------------------
/*[*/
//---------------------------------------------------------------------------
// iTVPFunctionExporter, exporting main module's functions
//---------------------------------------------------------------------------
struct iTVPFunctionExporter
{
	virtual bool TJS_INTF_METHOD QueryFunctions(const tjs_char **name, void **function,
		tjs_uint count) = 0;
	virtual bool TJS_INTF_METHOD QueryFunctionsByNarrowString(const char **name,
		void **function, tjs_uint count) = 0;
};
//---------------------------------------------------------------------------

//---------------------------------------------------------------------------
// iTVPStaticPlugin structure for registering static plugins to
//---------------------------------------------------------------------------
struct iTVPStaticPlugin {
    const tjs_char* name;
	int32_t (STDCALL *link)(iTVPFunctionExporter *);
	int32_t (STDCALL *unlink)();
};
extern "C" void TVPRegisterPlugin(const iTVPStaticPlugin* plugin);
//---------------------------------------------------------------------------

/*]*/
//---------------------------------------------------------------------------


//---------------------------------------------------------------------------
struct ITSSModule;
struct IWaveUnpacker;
struct ITSSStorageProvider;
extern "C"
{
	iTVPFunctionExporter * TVPGetFunctionExporter();

	// V2 plug-in
	typedef int32_t (STDCALL * tTVPV2LinkProc)(iTVPFunctionExporter *);
	typedef int32_t (STDCALL * tTVPV2UnlinkProc)();
}
//---------------------------------------------------------------------------

//---------------------------------------------------------------------------
extern void TVPAddExportFunction(const tjs_char *name, void *ptr);
extern void TVPAddExportFunction(const char *name, void *ptr);
// 同梱プラグインの申告 (packinone のような詰め合わせプラグイン向け)
//   詰め合わせ側が V2Link で「この名前のプラグインは自分が持っている」と登録すると、
//   以後 Plugins.link(name) は何もせず成功し、Plugins.canLink(name) も true を返す。
//   旧 PackinOne は Plugins.link を自前で差し替えて同じことをしていた (本体に口が無かったため)。
//   名前はファイル名部分だけを見て大小文字を無視する ("csvParser.dll" と
//   "tools/plugin64/csvparser.dll" は同じものとして扱う)。
TJS_EXP_FUNC_DEF(void, TVPRegisterBundledPlugin, (const ttstr &name));
TJS_EXP_FUNC_DEF(void, TVPUnregisterBundledPlugin, (const ttstr &name));
TJS_EXP_FUNC_DEF(bool, TVPIsBundledPlugin, (const ttstr &name));

TJS_EXP_FUNC_DEF(void, TVPThrowPluginUnboundFunctionError, (const char *funcname));
TJS_EXP_FUNC_DEF(void, TVPThrowPluginUnboundFunctionError, (const tjs_char *funcname));

inline TJS_EXP_FUNC_DEF(void *, TVP_malloc, (size_t size)) { return malloc(size); }
inline TJS_EXP_FUNC_DEF(void *, TVP_realloc, (void *pp, size_t size)) { return realloc(pp, size); }
inline TJS_EXP_FUNC_DEF(void, TVP_free, (void *pp)) { return free(pp); }
TJS_EXP_FUNC_DEF(tjs_int, TVPGetAutoLoadPluginCount, ());
//---------------------------------------------------------------------------


//---------------------------------------------------------------------------
TJS_EXP_FUNC_DEF(int, ZLIB_uncompress, (unsigned char *dest, unsigned long *destlen, const unsigned char *source, unsigned long sourcelen));
TJS_EXP_FUNC_DEF(int, ZLIB_compress, (unsigned char *dest, unsigned long *destlen, const unsigned char *source, unsigned long sourcelen));
TJS_EXP_FUNC_DEF(int, ZLIB_compress2, (unsigned char *dest, unsigned long *destlen, const unsigned char *source, unsigned long sourcelen, int level));

/*[*/

//---------------------------------------------------------------------------
// this stub includes exported function from Independent implementation of
// MD5 (RFC 1321) by Aladdin Enterprises.
//---------------------------------------------------------------------------
// TVP_md5_init, TVP_md5_append, TVP_md5_finish are exported
typedef struct TVP_md5_state_s { tjs_uint8 buffer[4*2+8+4*4+8+64]; } TVP_md5_state_t; // md5_state_t 
//---------------------------------------------------------------------------


/*]*/

TJS_EXP_FUNC_DEF(void, TVP_md5_init, (TVP_md5_state_t *pms));
TJS_EXP_FUNC_DEF(void, TVP_md5_append, (TVP_md5_state_t *pms, const tjs_uint8 *data, int nbytes));
TJS_EXP_FUNC_DEF(void, TVP_md5_finish, (TVP_md5_state_t *pms, tjs_uint8 *digest));

TJS_EXP_FUNC_DEF(void, TVPProcessApplicationMessages, ());
TJS_EXP_FUNC_DEF(void, TVPHandleApplicationMessage, ());

TJS_EXP_FUNC_DEF(bool, TVPRegisterGlobalObject, (const tjs_char *name, iTJSDispatch2 * dsp));
TJS_EXP_FUNC_DEF(bool, TVPRemoveGlobalObject, (const tjs_char *name));

/*[*/
//---------------------------------------------------------------------------
// data types for TVPDoTryBlock
//---------------------------------------------------------------------------
	// TVPDoTryBlock executes specified 'tryblock' in try block.
	// If any exception occured,
	// 'catchblock' is to be executed. 'data' is applicatoin defined data
	// block passed to 'tryblock' and 'catchblock' and 'finallyblock'.
	// if the 'catchblock' returns true, the exception is to be rethrown.
	// if false then the exception is to be vanished.
	// 'finallyblock' can be null, is to be executed whatever the exception
	// is generated or not.

struct tTVPExceptionDesc
{
	ttstr type; // the exception type, currently 'eTJS' or 'unknown'
	ttstr message; // the exception message (if exists. otherwise empty).
};

typedef void (TJS_USERENTRY *tTVPTryBlockFunction)(void * data);
typedef bool (TJS_USERENTRY *tTVPCatchBlockFunction)(void * data, const tTVPExceptionDesc & desc);
typedef void (TJS_USERENTRY *tTVPFinallyBlockFunction)(void *data);
//---------------------------------------------------------------------------




/*]*/

TJS_EXP_FUNC_DEF(void, TVPDoTryBlock, (tTVPTryBlockFunction tryblock, tTVPCatchBlockFunction catchblock, tTVPFinallyBlockFunction finallyblock, void *data));

TJS_EXP_FUNC_DEF(bool, TVPGetFileVersionOf, (const tjs_char* module_filename, tjs_int &major, tjs_int &minor, tjs_int &release, tjs_int &build));


//---------------------------------------------------------------------------
extern bool TVPPluginUnloadedAtSystemExit;
extern void TVPLoadPluigins(void);
//---------------------------------------------------------------------------


#endif
