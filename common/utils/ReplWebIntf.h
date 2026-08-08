//---------------------------------------------------------------------------
//!@file TJS WebServer クラス — ブラウザ REPL サーバ (-replweb) の拡張登録口
//
// スクリプト / プラグインが REPL web サーバへ機能を追加公開するためのネイティブ
// クラス。メソッドはインスタンス不要で `WebServer.register(...)` のように呼べる
// (System 同様)。KRKRZ_REPL_WEB ビルド時のみ登録される。
//
//   WebServer.register("/api/foo/", function(req) {
//       // req = %[ method, path, query, body, bytes ]
//       return %[ status:200, mime:"application/json", body:"{}" ];
//   });
//   WebServer.serveStatic("/ui/", "ui/");     // data/ui/** を /ui/** で配信
//   WebServer.broadcast("state", jsonText);   // SSE /sub/state の購読者へ配信
//   WebServer.active;                         // サーバ稼働中か
//   WebServer.url;                            // 待受 URL (未稼働なら空)
//
// ハンドラは常にメインスレッドで呼ばれる。戻り値の規約は ReplWebServer.h 参照。
//---------------------------------------------------------------------------
#ifndef REPL_WEB_INTF_H
#define REPL_WEB_INTF_H
#ifdef KRKRZ_REPL_WEB

#include "tjsNative.h"

class tTJSNC_WebServer : public tTJSNativeClass
{
	typedef tTJSNativeClass inherited;

public:
	tTJSNC_WebServer();
	static tjs_uint32 ClassID;

protected:
	tTJSNativeInstance* CreateNativeInstance();
};

extern tTJSNativeClass* TVPCreateNativeClass_WebServer();

#endif // KRKRZ_REPL_WEB
#endif
