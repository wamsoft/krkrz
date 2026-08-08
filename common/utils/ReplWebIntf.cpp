//---------------------------------------------------------------------------
// TJS WebServer クラス実装 — ブラウザ REPL サーバ (-replweb) の拡張登録口
//
// 実体は ReplWebServer.cpp の拡張 API (TVPReplWeb::RegisterHandler 等) への
// 薄いバインド。TJS からの呼び出しは常にメインスレッドなので、そのまま
// 登録系 API を呼んでよい (スレッド契約は ReplWebServer.h 参照)。
//---------------------------------------------------------------------------
#include "tjsCommHead.h"
#ifdef KRKRZ_REPL_WEB

#include "ReplWebIntf.h"
#include "ReplWebServer.h"

//---------------------------------------------------------------------------
// WebServer クラス (インスタンス不要、 System と同様にクラスオブジェクトのメソッド)
//---------------------------------------------------------------------------
tjs_uint32 tTJSNC_WebServer::ClassID = (tjs_uint32)-1;

tTJSNC_WebServer::tTJSNC_WebServer() : inherited(TJS_W("WebServer"))
{
	TJS_BEGIN_NATIVE_MEMBERS(WebServer)
	TJS_DECL_EMPTY_FINALIZE_METHOD
	//---------------------------------------------------------------------------
	TJS_BEGIN_NATIVE_CONSTRUCTOR_DECL(/*var.name*/_this, /*var.type*/tTJSNativeInstance, /*TJS class name*/WebServer)
	{
		return TJS_S_OK;
	}
	TJS_END_NATIVE_CONSTRUCTOR_DECL(/*TJS class name*/WebServer)

	//---------------------------------------------------------------------------
	// register(prefix, handler)  — パスプレフィックスへ動的ハンドラを登録 (上書き可)。
	//   handler(req) はメインスレッドで呼ばれる。req / 戻り値の規約はヘッダ参照。
	TJS_BEGIN_NATIVE_METHOD_DECL(/*func. name*/register)
	{
		if (numparams < 2) return TJS_E_BADPARAMCOUNT;
		ttstr prefix(*param[0]);
		TVPReplWeb::RegisterHandler(prefix, *param[1]);
		if (result) *result = (tjs_int)1;
		return TJS_S_OK;
	}
	TJS_END_NATIVE_METHOD_DECL(/*func. name*/register)
	//---------------------------------------------------------------------------
	// unregister(prefix)  — 動的ハンドラを解除。あったら 1 / 無ければ 0。
	TJS_BEGIN_NATIVE_METHOD_DECL(/*func. name*/unregister)
	{
		if (numparams < 1) return TJS_E_BADPARAMCOUNT;
		ttstr prefix(*param[0]);
		bool found = TVPReplWeb::UnregisterHandler(prefix);
		if (result) *result = (tjs_int)(found ? 1 : 0);
		return TJS_S_OK;
	}
	TJS_END_NATIVE_METHOD_DECL(/*func. name*/unregister)
	//---------------------------------------------------------------------------
	// serveStatic(prefix, storageDir)  — prefix 以下の GET を storageDir + 相対パスの
	//   ストレージから配信する。例: WebServer.serveStatic("/ui/", "ui/")。
	TJS_BEGIN_NATIVE_METHOD_DECL(/*func. name*/serveStatic)
	{
		if (numparams < 2) return TJS_E_BADPARAMCOUNT;
		ttstr prefix(*param[0]);
		ttstr dir(*param[1]);
		TVPReplWeb::RegisterStatic(prefix, dir);
		if (result) *result = (tjs_int)1;
		return TJS_S_OK;
	}
	TJS_END_NATIVE_METHOD_DECL(/*func. name*/serveStatic)
	//---------------------------------------------------------------------------
	// unserveStatic(prefix)  — 静的配信マウントを解除。あったら 1 / 無ければ 0。
	TJS_BEGIN_NATIVE_METHOD_DECL(/*func. name*/unserveStatic)
	{
		if (numparams < 1) return TJS_E_BADPARAMCOUNT;
		ttstr prefix(*param[0]);
		bool found = TVPReplWeb::UnregisterStatic(prefix);
		if (result) *result = (tjs_int)(found ? 1 : 0);
		return TJS_S_OK;
	}
	TJS_END_NATIVE_METHOD_DECL(/*func. name*/unserveStatic)
	//---------------------------------------------------------------------------
	// broadcast(channel, text)  — SSE /sub/<channel> の購読者へ text を配信。
	//   text は改行を含んでよい (SSE 複数 data 行に整形される)。
	TJS_BEGIN_NATIVE_METHOD_DECL(/*func. name*/broadcast)
	{
		if (numparams < 2) return TJS_E_BADPARAMCOUNT;
		ttstr channel(*param[0]);
		ttstr text(*param[1]);
		TVPReplWeb::BroadcastChannel(channel, text);
		if (result) *result = (tjs_int)1;
		return TJS_S_OK;
	}
	TJS_END_NATIVE_METHOD_DECL(/*func. name*/broadcast)

	//---------------------------------------------------------------------------
	// active  — サーバ稼働中か (-replweb 指定なしなら false。登録自体は可能)。
	TJS_BEGIN_NATIVE_PROP_DECL(active)
	{
		TJS_BEGIN_NATIVE_PROP_GETTER
		{
			*result = (tjs_int)(TVPReplWeb::IsActive() ? 1 : 0);
			return TJS_S_OK;
		}
		TJS_END_NATIVE_PROP_GETTER
		TJS_DENY_NATIVE_PROP_SETTER
	}
	TJS_END_NATIVE_PROP_DECL(active)
	//---------------------------------------------------------------------------
	// url  — 待受 URL ("http://host:port/")。未稼働なら空文字列。
	TJS_BEGIN_NATIVE_PROP_DECL(url)
	{
		TJS_BEGIN_NATIVE_PROP_GETTER
		{
			*result = TVPReplWeb::GetURL();
			return TJS_S_OK;
		}
		TJS_END_NATIVE_PROP_GETTER
		TJS_DENY_NATIVE_PROP_SETTER
	}
	TJS_END_NATIVE_PROP_DECL(url)

	TJS_END_NATIVE_MEMBERS
}

//---------------------------------------------------------------------------
tTJSNativeInstance* tTJSNC_WebServer::CreateNativeInstance()
{
	return nullptr;
}

tTJSNativeClass* TVPCreateNativeClass_WebServer()
{
	return new tTJSNC_WebServer();
}

#endif // KRKRZ_REPL_WEB
