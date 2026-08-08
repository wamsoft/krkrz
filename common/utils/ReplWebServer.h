//---------------------------------------------------------------------------
// ブラウザ REPL ビューワー (HTTP + SSE サーバ)
//   -replweb=<port> で 127.0.0.1:<port> に軽量 HTTP サーバを立て、
//   ブラウザから 上=ログ / 下=入力 で REPL を操作する。端末非依存で
//   選択/コピー/貼り付け/スクロール/検索がブラウザネイティブに効く。
//   KRKRZ_REPL_WEB ビルド時のみ実体を持つ。
//---------------------------------------------------------------------------
#pragma once
#ifdef KRKRZ_REPL_WEB

#include "tjsCommHead.h"
#include "LogIntf.h"   // TVPLogLevel

namespace TVPReplWeb {

/// -replweb=<port> が指定されているか
bool Wanted();

/// サーバを起動 (accept スレッド開始 + ログ sink 設定)。多重起動は無視。
void Start();

/// サーバを停止 (接続を閉じスレッドを終了)。
void Stop();

/// ログ 1 行を全 SSE クライアントへ配信 (thread-safe)。Start 後のみ有効。
void LogLine(TVPLogLevel level, const char* utf8_line);

/// 稼働中かどうか (thread-safe)
bool IsActive();

/// 現在待ち受けている URL ("http://host:port/")。未起動なら空文字列。
ttstr GetURL();

//---------------------------------------------------------------------------
// 拡張 API (TJS クラス WebServer / プラグインが利用する登録口)
//
// 動的ハンドラ: パスプレフィックス最長一致で HTTP リクエストを TJS callable へ
// dispatch する。ハンドラは常に「メインスレッド上で」呼ばれる (GL / TJS /
// エンジン API を自由に触ってよい)。呼び出し規約:
//   handler(req)  req = %[ method, path, query, body, bytes ]
//     戻り値: 文字列 = 200 application/json / octet = 200 octet-stream /
//             整数 = そのステータスで空ボディ / void = 204 /
//             辞書 = %[ status, mime, body(文字列 or octet) ]
//
// 以下の登録系/broadcast はメインスレッドから呼ぶこと (TJS からの呼び出しは
// 常にメインスレッドなので通常は意識不要)。
//---------------------------------------------------------------------------

/// 動的ハンドラを登録 (同一 prefix は上書き)。prefix は "/" 始まり。
/// サーバ未起動でも登録は保持される (起動時から有効)。
void RegisterHandler(const ttstr& prefix, const tTJSVariant& handler);

/// 動的ハンドラを解除。登録が無ければ false。
bool UnregisterHandler(const ttstr& prefix);

/// 静的配信マウントを登録: prefix 以下の GET を storageDir + 相対パスの
/// ストレージ (Storages 検索対象) から配信する。例: ("/ui/", "ui/")。
void RegisterStatic(const ttstr& prefix, const ttstr& storageDir);

/// 静的配信マウントを解除。登録が無ければ false。
bool UnregisterStatic(const ttstr& prefix);

/// 汎用 SSE チャネル (/sub/<channel>) の購読者へ payload を配信 (改行可)。
void BroadcastChannel(const ttstr& channel, const ttstr& payload);

/// 全ハンドラ/マウントを解放 (TJS クロージャの参照を手放す)。
/// スクリプトエンジン終了より前にメインスレッドで呼ぶこと。
void ClearHandlers();

} // namespace TVPReplWeb

#endif // KRKRZ_REPL_WEB
