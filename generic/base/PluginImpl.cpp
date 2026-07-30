//---------------------------------------------------------------------------
/*
	TVP2 ( T Visual Presenter 2 )  A script authoring tool
	Copyright (C) 2000 W.Dee <dee@kikyou.info> and contributors

	See details of license at "license.txt"
*/
//---------------------------------------------------------------------------
// "Plugins" class implementation / Service for plug-ins
//---------------------------------------------------------------------------
#include "tjsCommHead.h"

#include <algorithm>
#include <functional>
#include "ScriptMgnIntf.h"
#include "PluginImpl.h"
#include "StorageImpl.h"

#include "MsgImpl.h"
#include "SysInitIntf.h"

#include "tjsHashSearch.h"
#include "EventIntf.h"
#include "TransIntf.h"
#include "tjsArray.h"
#include "tjsDictionary.h"
#include "DebugIntf.h"
#include "FuncStubs.h"
#include "tjs.h"

#include "Application.h"
#include "CharacterSet.h"

// XXX Version Code 
static const tjs_int TVP_VERSION_MAJOR = 1;
static const tjs_int TVP_VERSION_MINOR = 0;
static const tjs_int TVP_VERSION_RELEASE = 0;
static const tjs_int TVP_VERSION_BUILD = 1;

/**
 * GENERIC版のバージョンコード取得。別途再検討のこと
*/
bool TVPGetFileVersionOf(const tjs_char* module_filename, tjs_int& major, tjs_int& minor, tjs_int& release, tjs_int& build ) {
	major = TVP_VERSION_MAJOR;
	minor = TVP_VERSION_MINOR;
	release = TVP_VERSION_RELEASE;
	build = TVP_VERSION_BUILD;
	return true;
}

//---------------------------------------------------------------------------
// Static plugin support
//---------------------------------------------------------------------------

static std::vector<const iTVPStaticPlugin*> TVPStaticPlugins;

extern "C" void TVPRegisterPlugin(const iTVPStaticPlugin* plugin)
{
    if(plugin) {
        TVPStaticPlugins.push_back(plugin);
    }
}

static const iTVPStaticPlugin* TVPFindStaticPlugin(const ttstr& name)
{
    // Extract base name without path and extension for matching
    ttstr basename = name;
    // Remove path
    tjs_int pos = basename.GetLen() - 1;
    while(pos >= 0) {
        tjs_char c = basename[pos];
        if(c == TJS_W('/') || c == TJS_W('\\')) {
            basename = ttstr(basename.c_str() + pos + 1);
            break;
        }
        pos--;
    }
    // Remove extension
    pos = basename.GetLen() - 1;
    while(pos >= 0) {
        if(basename[pos] == TJS_W('.')) {
            basename = ttstr(basename.c_str(), pos);
            break;
        }
        pos--;
    }
    
    for(const auto* plugin : TVPStaticPlugins) {
        if(plugin && plugin->name) {
            ttstr pluginName(plugin->name);
            if(pluginName == basename || pluginName == name) {
                return plugin;
            }
        }
    }
    return nullptr;
}

//---------------------------------------------------------------------------


//---------------------------------------------------------------------------
// export table
//---------------------------------------------------------------------------
static tTJSHashTable<ttstr, void *> TVPExportFuncs;
static bool TVPExportFuncsInit = false;
void TVPAddExportFunction(const char *name, void *ptr)
{
	TVPExportFuncs.Add(name, ptr);
}
void TVPAddExportFunction(const tjs_char *name, void *ptr)
{
	TVPExportFuncs.Add(name, ptr);
}
static void TVPInitExportFuncs()
{
	if(TVPExportFuncsInit) return;
	TVPExportFuncsInit = true;


	// Export functions
	TVPExportFunctions();
}
//---------------------------------------------------------------------------
struct tTVPFunctionExporter : iTVPFunctionExporter
{
	bool TJS_INTF_METHOD QueryFunctions(const tjs_char **name, void **function,
		tjs_uint count);
	bool TJS_INTF_METHOD QueryFunctionsByNarrowString(const char **name,
		void **function, tjs_uint count);
} static TVPFunctionExporter;
//---------------------------------------------------------------------------
bool TJS_INTF_METHOD tTVPFunctionExporter::QueryFunctions(const tjs_char **name, void **function,
		tjs_uint count)
{
	// retrieve function table by given name table.
	// return false if any function is missing.
	bool ret = true;
	ttstr tname;
	for(tjs_uint i = 0; i<count; i++)
	{
		tname = name[i];
		void ** ptr = TVPExportFuncs.Find(tname);
		if(ptr)
			function[i] = *ptr;
		else
			function[i] = NULL, ret= false;
	}
	return ret;
}
//---------------------------------------------------------------------------
bool TJS_INTF_METHOD tTVPFunctionExporter::QueryFunctionsByNarrowString(
	const char **name, void **function, tjs_uint count)
{
	// retrieve function table by given name table.
	// return false if any function is missing.
	bool ret = true;
	ttstr tname;
	for(tjs_uint i = 0; i<count; i++)
	{
		tname = name[i];
		void ** ptr = TVPExportFuncs.Find(tname);
		if(ptr)
			function[i] = *ptr;
		else
			function[i] = NULL, ret= false;
	}
	return ret;
}
//---------------------------------------------------------------------------
extern "C" iTVPFunctionExporter * TVPGetFunctionExporter()
{
	// for external applications
	TVPInitExportFuncs();
    return &TVPFunctionExporter;
}
//---------------------------------------------------------------------------


//---------------------------------------------------------------------------
void TVPThrowPluginUnboundFunctionError(const char *funcname)
{
	TVPThrowExceptionMessage(TVPPluginUnboundFunctionError, funcname);
}
//---------------------------------------------------------------------------
void TVPThrowPluginUnboundFunctionError(const tjs_char *funcname)
{
	TVPThrowExceptionMessage(TVPPluginUnboundFunctionError, funcname);
}
//---------------------------------------------------------------------------


//---------------------------------------------------------------------------
// Plug-ins management
//---------------------------------------------------------------------------
struct tTVPPlugin
{
	ttstr Name;
	void *Instance = nullptr;

	tTVPPluginHolder *Holder = nullptr;

	tTVPV2LinkProc V2Link = nullptr;
	tTVPV2UnlinkProc V2Unlink = nullptr;

	tTVPPlugin(const ttstr & name);
	~tTVPPlugin();

	bool Uninit();
};

static inline tjs_string ChangeFileExt( const tjs_string& path, const tjs_string& ext )
{
	tjs_string::size_type pos = path.find_last_of( TJS_W('.') );
	if( pos != tjs_string::npos ) {
		return path.substr( 0, pos+1 ) + ext;
	} else {
		return path + ext;
	}
};

// プラグイン名をプラットフォーム固有の共有ライブラリ名に変換する。
// 例: "Foo.dll" → Windows:そのまま / macOS:"libFoo.dylib" / Linux等:"libFoo.so"
//     "Foo"     → "libFoo" (拡張子は付与しない、既存動作互換)
static ttstr TVPResolvePluginSoName(const ttstr & name)
{
	ttstr soname = name;
	tjs_int len = soname.GetLen();
	bool isDll = len >= 4 &&
		soname[len-1] == TJS_W('l') &&
		soname[len-2] == TJS_W('l') &&
		soname[len-3] == TJS_W('d') &&
		soname[len-4] == TJS_W('.');

	if(isDll) {
#if defined(_WIN32)
		// Keep XXX.dll as-is on Windows.
#elif defined(__APPLE__)
		if( soname.GetLen() <= 3 ||
			soname[0] != TJS_W('l') ||
			soname[1] != TJS_W('i') ||
			soname[2] != TJS_W('b') ) {
			soname = TJS_W("lib") + soname;
		}
		tjs_string ext = ChangeFileExt( soname.AsStdString(), TJS_W("dylib") );
		soname = ttstr( ext );
#else
		if( soname.GetLen() <= 3 ||
			soname[0] != TJS_W('l') ||
			soname[1] != TJS_W('i') ||
			soname[2] != TJS_W('b') ) {
			soname = TJS_W("lib") + soname;
		}
		tjs_string ext = ChangeFileExt( soname.AsStdString(), TJS_W("so") );
		soname = ttstr( ext );
#endif
	} else {
		// Keep existing behavior for non-dll names.
		if( soname.GetLen() <= 3 ||
			soname[0] != TJS_W('l') ||
			soname[1] != TJS_W('i') ||
			soname[2] != TJS_W('b') ) {
			soname = TJS_W("lib") + soname;
		}
	}
	return soname;
}

//---------------------------------------------------------------------------
tTVPPlugin::tTVPPlugin(const ttstr & name) : Name(name), Instance(nullptr),
	Holder(nullptr), V2Link(nullptr), V2Unlink(nullptr)
{
	// First, try to find a statically registered plugin
	const iTVPStaticPlugin* staticPlugin = TVPFindStaticPlugin(name);
	if(staticPlugin) {
		V2Link = staticPlugin->link;
		V2Unlink = staticPlugin->unlink;

		// link
		if(V2Link) {
			V2Link(TVPGetFunctionExporter());
		}
		return;
	}

	// load shared library
	ttstr soname = TVPResolvePluginSoName(name);

	ttstr libpath;
#ifdef ANDROID
	// 直接ロードのみ可
	libpath = soname;
#else
	Holder = new tTVPPluginHolder(soname);
	if( TVPCheckExistentLocalFile(Holder->GetLocalName()) ) {
		libpath  = Holder->GetLocalName();
	}
#endif
	if (!libpath.IsEmpty()) {
		Instance = Application->LoadLibrary(libpath.c_str());
	}
	if(!Instance)
	{
		delete Holder;
		TVPThrowExceptionMessage(TVPCannotLoadPlugin, name);
	}

	try
	{
		// retrieve each functions
		V2Link = (tTVPV2LinkProc)Application->GetProcAddress(Instance, "V2Link");
		//const char *errmes = dlerror();

		V2Unlink = (tTVPV2UnlinkProc)Application->GetProcAddress(Instance, "V2Unlink");
		//*errmes = dlerror();

		// link
		if(V2Link)
		{
			V2Link(TVPGetFunctionExporter());
		}
	}
	catch(...)
	{
		Application->FreeLibrary(Instance);
		delete Holder;
		throw;
	}
}
//---------------------------------------------------------------------------
tTVPPlugin::~tTVPPlugin()
{
}
//---------------------------------------------------------------------------
bool tTVPPlugin::Uninit()
{
	tTJS *tjs = TVPGetScriptEngine();
	if(tjs) tjs->DoGarbageCollection(); // to release unused objects

	if(V2Unlink)
	{
 		if(TJS_FAILED(V2Unlink())) return false;
	}

	if (Instance)
	{
		// TODO(plugin-unload-crash / generic/SDL 未対応):
		//   プラグインが ncbind 経由で System/Window 等のコアオブジェクトに付与した
		//   ネイティブメソッドの C++ vtable は当該プラグインDLL内に存在する。
		//   スクリプトエンジン終了(tTJS::Shutdown のグローバル解体)で該当オブジェクトが
		//   finalize される時、それより前に本DLLがアンマップされていると、消えた vtable を
		//   call してクラッシュする(tTJSCustomObject::DeleteAllMembers 内で AV)。
		//   実際、単一ファイル>1MB を xp3pack した際 windowEx.dll が System.breathe 目的で
		//   動的ロードされ、終了時にこの不具合で落ちる事例を確認(WIN版)。
		//   WIN版(win32/base/PluginImpl.cpp)はロード時に GetModuleHandleEx(PIN) でDLLを
		//   固定して回避済み。generic/SDL は SDL_LoadObject/SDL_UnloadObject 使用のため
		//   PINが直接使えず未対応。
		//   [可搬な対処案] SDL3Application::LoadLibrary の SDL_LoadObject 直後に pin する:
		//     POSIX: dlopen(path, RTLD_NOW|RTLD_NOLOAD|RTLD_NODELETE)
		//     Win  : GetModuleHandleExW(PIN|FROM_ADDRESS, SDL_LoadFunction(h,"V2Link"), &m)
		//   [本筋] shutdown前にプラグインDLLを FreeLibrary している正確なコード経路を
		//     特定し、その解放を engine 終了後に回すのが妥当(PINは対症的な広い回避策)。
		Application->FreeLibrary(Instance);
	}
	if (Holder) {
		delete Holder;
	}
	return true;
}

//---------------------------------------------------------------------------
tjs_int TVPGetAutoLoadPluginCount() { return 0; }
//---------------------------------------------------------------------------


//---------------------------------------------------------------------------
bool TVPPluginUnloadedAtSystemExit = false;
typedef std::vector<tTVPPlugin*> tTVPPluginVectorType;
struct tTVPPluginVectorStruc
{
	tTVPPluginVectorType Vector;
} static TVPPluginVector;
static void TVPDestroyPluginVector(void)
{
	// state all plugins are to be released
	TVPPluginUnloadedAtSystemExit = true;

	// delete all objects
	tTVPPluginVectorType::iterator i;
	while(TVPPluginVector.Vector.size())
	{
		i = TVPPluginVector.Vector.end() - 1;
		try
		{
			(*i)->Uninit();
			delete *i;
		}
		catch(...)
		{
		}
		TVPPluginVector.Vector.pop_back();
	}
}
tTVPAtExit TVPDestroyPluginVectorAtExit
	(TVP_ATEXIT_PRI_RELEASE, TVPDestroyPluginVector);
//---------------------------------------------------------------------------
static bool TVPPluginLoading = false;
void TVPLoadPlugin(const ttstr & name)
{
	// load plugin
	if(TVPPluginLoading)
		TVPThrowExceptionMessage(TVPCannnotLinkPluginWhilePluginLinking);
			// linking plugin while other plugin is linking, is prohibited
			// by data security reason.

	// 静的プラグインは "wucmt" / "wucmt.dll" 等の表記ゆれで二重 link され
	// ないよう、静的登録名をキーに正規化して重複チェック・登録する
	ttstr key = name;
	if(const iTVPStaticPlugin* staticPlugin = TVPFindStaticPlugin(name))
		key = staticPlugin->name;

	// check whether the same plugin was already loaded
	tTVPPluginVectorType::iterator i;
	for(i = TVPPluginVector.Vector.begin();
		i != TVPPluginVector.Vector.end(); i++)
	{
		if((*i)->Name == key) return;
	}

	tTVPPlugin * p;

	try
	{
		TVPPluginLoading = true;
		p = new tTVPPlugin(key);
		TVPPluginLoading = false;
	}
	catch(...)
	{
		TVPPluginLoading = false;
		throw;
	}

	TVPPluginVector.Vector.push_back(p);
}
//---------------------------------------------------------------------------
bool TVPUnloadPlugin(const ttstr & name)
{
	// unload plugin

	// 静的プラグインは登録名をキーに正規化する (TVPLoadPlugin と対称)
	ttstr key = name;
	if(const iTVPStaticPlugin* staticPlugin = TVPFindStaticPlugin(name))
		key = staticPlugin->name;

	tTVPPluginVectorType::iterator i;
	for(i = TVPPluginVector.Vector.begin();
		i != TVPPluginVector.Vector.end(); i++)
	{
		if((*i)->Name == key)
		{
			if(!(*i)->Uninit()) return false;
			delete *i;
			TVPPluginVector.Vector.erase(i);
			return true;
		}
	}
	TVPThrowExceptionMessage(TVPNotLoadedPlugin, name);
	return false;
}
//---------------------------------------------------------------------------
bool TVPCanLoadPlugin(const ttstr & name)
{
	// 指定名のプラグインがロード可能かを判定する。例外は投げない。
	// TVPLoadPlugin() と同じ解決順:
	//   1) 既ロード判定
	//   2) 静的登録プラグイン
	//   3) プラットフォーム固有の soname へ変換した上で
	//      - 非Android: アプリ/プラグインフォルダの実ファイル探索
	//      - Android : 検索パスが無いので dlopen を試行 (成功時は即解放)

	// 1) 既にロード済みなら true
	for(tTVPPluginVectorType::iterator i = TVPPluginVector.Vector.begin();
		i != TVPPluginVector.Vector.end(); i++)
	{
		if((*i)->Name == name) return true;
	}

	// 2) 静的プラグインに含まれていれば true
	if(TVPFindStaticPlugin(name)) return true;

	// 3) 共有ライブラリとして探索/試行
	ttstr soname = TVPResolvePluginSoName(name);

#ifdef ANDROID
	// 検索パスが使えないので実際にロードを試み、成否のみ返す。
	// 副作用 (.init_array 等) は走るがリンク (V2Link) はしない。
	void* instance = Application->LoadLibrary(soname.c_str());
	if(instance) {
		Application->FreeLibrary(instance);
		return true;
	}
	return false;
#else
	tTVPPluginHolder holder(soname);
	const ttstr & local = holder.GetLocalName();
	if(local.IsEmpty()) return false;
	return TVPCheckExistentLocalFile(local);
#endif
}
//---------------------------------------------------------------------------
void TVPLoadStaticPluigins(void)
{
	// load each plugin
    for(const auto* plugin : TVPStaticPlugins) {
        if(plugin && plugin->name) {
			TVPAddImportantLog(ttstr(TJS_W("(info) Auto Loading Static Plugin:")) + ttstr(plugin->name));
			TVPLoadPlugin(plugin->name);
		}
	}
}
//---------------------------------------------------------------------------

//---------------------------------------------------------------------------
// some service functions for plugin
//---------------------------------------------------------------------------
#include "zlib.h"

int ZLIB_uncompress(unsigned char *dest, unsigned long *destlen,
	const unsigned char *source, unsigned long sourcelen)
{
	return uncompress(dest, destlen, source, sourcelen);
}
//---------------------------------------------------------------------------
int ZLIB_compress(unsigned char *dest, unsigned long *destlen,
	const unsigned char *source, unsigned long sourcelen)
{
	return compress(dest, destlen, source, sourcelen);
}
//---------------------------------------------------------------------------
int ZLIB_compress2(unsigned char *dest, unsigned long *destlen,
	const unsigned char *source, unsigned long sourcelen, int level)
{
	return compress2(dest, destlen, source, sourcelen, level);
}
//---------------------------------------------------------------------------
#include "md5.h"
static char TVP_assert_md5_state_t_size[
	 (sizeof(TVP_md5_state_t) >= sizeof(md5_state_t))];
	// if this errors, sizeof(TVP_md5_state_t) is not equal to sizeof(md5_state_t).
	// sizeof(TVP_md5_state_t) must be equal to sizeof(md5_state_t).
//---------------------------------------------------------------------------
void TVP_md5_init(TVP_md5_state_t *pms)
{
	md5_init((md5_state_t*)pms);
}
//---------------------------------------------------------------------------
void TVP_md5_append(TVP_md5_state_t *pms, const tjs_uint8 *data, int nbytes)
{
	md5_append((md5_state_t*)pms, (const md5_byte_t*)data, nbytes);
}
//---------------------------------------------------------------------------
void TVP_md5_finish(TVP_md5_state_t *pms, tjs_uint8 *digest)
{
	md5_finish((md5_state_t*)pms, digest);
}
//---------------------------------------------------------------------------
void TVPProcessApplicationMessages()
{
	Application->ProcessMessages();
}
//---------------------------------------------------------------------------
void TVPHandleApplicationMessage()
{
	Application->HandleMessage();
}
//---------------------------------------------------------------------------
bool TVPRegisterGlobalObject(const tjs_char *name, iTJSDispatch2 * dsp)
{
	// register given object to global object
	tTJSVariant val(dsp);
	iTJSDispatch2 *global = TVPGetScriptDispatch();
	tjs_error er;
	try
	{
		er = global->PropSet(TJS_MEMBERENSURE, name, NULL, &val, global);
	}
	catch(...)
	{
		global->Release();
		return false;
	}
	global->Release();
	return TJS_SUCCEEDED(er);
}
//---------------------------------------------------------------------------
bool TVPRemoveGlobalObject(const tjs_char *name)
{
	// remove registration of global object
	iTJSDispatch2 *global = TVPGetScriptDispatch();
	if(!global) return false;
	tjs_error er;
	try
	{
		er = global->DeleteMember(0, name, NULL, global);
	}
	catch(...)
	{
		global->Release();
		return false;
	}
	global->Release();
	return TJS_SUCCEEDED(er);
}
//---------------------------------------------------------------------------
void TVPDoTryBlock(
	tTVPTryBlockFunction tryblock,
	tTVPCatchBlockFunction catchblock,
	tTVPFinallyBlockFunction finallyblock,
	void *data)
{
	try
	{
		tryblock(data);
	}
	catch(const eTJS & e)
	{
		if(finallyblock) finallyblock(data);
		tTVPExceptionDesc desc;
		desc.type = TJS_W("eTJS");
		desc.message = e.GetMessage();
		if(catchblock(data, desc)) throw;
		return;
	}
	catch(...)
	{
		if(finallyblock) finallyblock(data);
		tTVPExceptionDesc desc;
		desc.type = TJS_W("unknown");
		if(catchblock(data, desc)) throw;
		return;
	}
	if(finallyblock) finallyblock(data);
}
//---------------------------------------------------------------------------


//---------------------------------------------------------------------------
// TVPCreateNativeClass_Plugins
//---------------------------------------------------------------------------
tTJSNativeClass * TVPCreateNativeClass_Plugins()
{
	tTJSNC_Plugins *cls = new tTJSNC_Plugins();


	// setup some platform-specific members
//---------------------------------------------------------------------------

//-- methods

//----------------------------------------------------------------------
TJS_BEGIN_NATIVE_METHOD_DECL(/*func. name*/link)
{
	if(numparams < 1) return TJS_E_BADPARAMCOUNT;

	ttstr name = *param[0];

	TVPLoadPlugin(name);

	return TJS_S_OK;
}
TJS_END_NATIVE_STATIC_METHOD_DECL_OUTER(/*object to register*/cls,
	/*func. name*/link)
//----------------------------------------------------------------------
TJS_BEGIN_NATIVE_METHOD_DECL(/*func. name*/unlink)
{
	if(numparams < 1) return TJS_E_BADPARAMCOUNT;

	ttstr name = *param[0];

	bool res = TVPUnloadPlugin(name);

	if(result) *result = (tjs_int)res;

	return TJS_S_OK;
}
TJS_END_NATIVE_STATIC_METHOD_DECL_OUTER(/*object to register*/cls,
	/*func. name*/unlink)
//----------------------------------------------------------------------
TJS_BEGIN_NATIVE_METHOD_DECL(/*func. name*/canLink)
{
	if(numparams < 1) return TJS_E_BADPARAMCOUNT;

	ttstr name = *param[0];

	bool res = TVPCanLoadPlugin(name);

	if(result) *result = (tjs_int)res;

	return TJS_S_OK;
}
TJS_END_NATIVE_STATIC_METHOD_DECL_OUTER(/*object to register*/cls,
	/*func. name*/canLink)
//----------------------------------------------------------------------
TJS_BEGIN_NATIVE_METHOD_DECL(getList)
{
	iTJSDispatch2 * array = TJSCreateArrayObject();
	try
	{
		tTVPPluginVectorType::iterator i;
		tjs_int idx = 0;
		for(i = TVPPluginVector.Vector.begin(); i != TVPPluginVector.Vector.end(); i++)
		{
			tTJSVariant val = (*i)->Name.c_str();
			array->PropSetByNum(TJS_MEMBERENSURE, idx++, &val, array);
		}
	
		if (result) *result = tTJSVariant(array, array);
	}
	catch(...)
	{
		array->Release();
		throw;
	}
	array->Release();
	return TJS_S_OK;
}
TJS_END_NATIVE_STATIC_METHOD_DECL_OUTER(cls, getList)
//---------------------------------------------------------------------------


//---------------------------------------------------------------------------
	return cls;
}
//---------------------------------------------------------------------------




