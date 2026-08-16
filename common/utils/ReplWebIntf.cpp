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
#include "CharacterSet.h"   // TVPUtf16ToUtf8 (startAt の host 変換)
#include <string>

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
	// start([port])  — サーバをスクリプトから起動 (127.0.0.1、既定 port=8899)。
	//   -replweb オプション無しでも UI サーバを立ち上げられる。戻り値 = 稼働中か。
	TJS_BEGIN_NATIVE_METHOD_DECL(/*func. name*/start)
	{
		tjs_int port = (numparams >= 1 && param[0]->Type() != tvtVoid) ? (tjs_int)*param[0] : 8899;
		TVPReplWeb::StartOn("127.0.0.1", (int)port);
		if (result) *result = (tjs_int)(TVPReplWeb::IsActive() ? 1 : 0);
		return TJS_S_OK;
	}
	TJS_END_NATIVE_METHOD_DECL(/*func. name*/start)
	//---------------------------------------------------------------------------
	// startAt(host, port)  — バインド先を明示して起動 ("0.0.0.0" で全 IF)。
	//   戻り値 = 稼働中か。外部 PC のブラウザから繋ぐ用途。
	TJS_BEGIN_NATIVE_METHOD_DECL(/*func. name*/startAt)
	{
		if (numparams < 2) return TJS_E_BADPARAMCOUNT;
		ttstr host(*param[0]);
		tjs_int port = (tjs_int)*param[1];
		std::string h; { tjs_string t(host.c_str()); TVPUtf16ToUtf8(h, t); }
		TVPReplWeb::StartOn(h, (int)port);
		if (result) *result = (tjs_int)(TVPReplWeb::IsActive() ? 1 : 0);
		return TJS_S_OK;
	}
	TJS_END_NATIVE_METHOD_DECL(/*func. name*/startAt)
	//---------------------------------------------------------------------------
	// stop()  — サーバを停止する (接続を閉じ accept スレッド終了)。
	TJS_BEGIN_NATIVE_METHOD_DECL(/*func. name*/stop)
	{
		TVPReplWeb::Stop();
		if (result) *result = (tjs_int)1;
		return TJS_S_OK;
	}
	TJS_END_NATIVE_METHOD_DECL(/*func. name*/stop)
	//---------------------------------------------------------------------------
	// openBrowser([url [, appMode=true]])  — url をブラウザで開く。appMode 時は
	//   Edge→Chrome を --app モードで試し、不可なら既定ブラウザへフォールバック。
	//   url 省略 (void) で稼働中サーバ URL を使う。戻り値 = 開けたか。
	TJS_BEGIN_NATIVE_METHOD_DECL(/*func. name*/openBrowser)
	{
		ttstr url = (numparams >= 1 && param[0]->Type() != tvtVoid) ? ttstr(*param[0]) : ttstr();
		bool appMode = (numparams >= 2) ? ((tjs_int)*param[1] != 0) : true;
		bool ok = TVPReplWeb::OpenBrowser(url, appMode);
		if (result) *result = (tjs_int)(ok ? 1 : 0);
		return TJS_S_OK;
	}
	TJS_END_NATIVE_METHOD_DECL(/*func. name*/openBrowser)

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
