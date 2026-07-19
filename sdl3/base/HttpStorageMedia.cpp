//---------------------------------------------------------------------------
//!@file wasm (Emscripten) 用 HTTP ストレージメディア (Phase A: 最小 PoC)
//
// "web://./<path>" 形式のストレージアクセスを、ブラウザの fetch で個別ファイル
// 取得にマップする。ADV のように中小ファイルが逐次読まれる用途で、xp3 に固めず
// バラファイルをサーバに置いてオンデマンド取得するための土台。
//
// krkrz のファイル I/O は同期 (Open が iTJSBinaryStream* を同期返し) だが fetch は
// 非同期。ここは JSPI (EM_ASYNC_JS) でブリッジする。Storage Open は
// main() → SDL_AppIterate → Dispatch → TJS → Storages 経由で呼ばれ、promising な
// main() 配下にあるため suspend が成立する (モーダルと同じ仕組み)。
//
// Phase A ではキャッシュを持たない (毎回 fetch。ただしブラウザ HTTP キャッシュは効く)。
// Phase B で OPFS 永続キャッシュ層を追加する。
//---------------------------------------------------------------------------
#include "tjsCommHead.h"

#ifdef __EMSCRIPTEN__

#include "CharacterSet.h"
#include "StorageIntf.h"
#include "UtilStreams.h"
#include "SysInitIntf.h"
#include "DebugIntf.h"

#include <emscripten.h>
#include <cstdlib>
#include <string>
#include <set>

#include <picojson/picojson.h>

//---------------------------------------------------------------------------
// JSPI fetch import (Phase B: OPFS 永続キャッシュ付き)。
//   curl    : 取得 URL (UTF-8)
//   use_cache: 1=OPFS キャッシュを使う / 0=常に fetch (キャッシュしない)
//   out_ptr : 成功時、malloc したバッファ先頭ポインタを書き込む先 (void**)
//   out_len : 成功時、バイト長を書き込む先 (int*)
//   戻り値  : 1=成功 / 0=失敗 (404・ネットワークエラー等)
//
// OPFS (Origin Private File System) にキャッシュがあればそれを返し、無ければ
// fetch して OPFS に保存する。OPFS API は非同期だが JSPI で await できる
// (メインスレッド上、promising な main() 配下)。同一オリジンのローカル永続
// ストレージなので大容量でもブラウザ HTTP キャッシュより確実に保持される。
//
// 注意: Phase B では ETag/更新検証をしない (一度キャッシュしたら使い続ける)。
// アセット更新はファイル名/URL を変える (cache busting) 運用が前提。開発中は
// pre.js の OPFS クリア手段でリセットする。
//---------------------------------------------------------------------------
EM_ASYNC_JS(int, krkrz_web_fetch, (const char* curl, int use_cache, void** out_ptr, int* out_len), {
	var url = UTF8ToString(curl);
	// ページ側ローディング表示の進捗カウンタ (取得ファイル数。OPFS ヒット含む)
	globalThis.__krkrzWebFetchCount = (globalThis.__krkrzWebFetchCount || 0) + 1;
	// OPFS はフラットなファイル名空間。キーは SHA-256(url) の先頭 40hex +
	// 可読サフィックス (サニタイズ済み末尾 32 文字)。
	// 旧方式の「非英数を '_' に置換」だけでは日本語ファイル名同士が同一キーに
	// 潰れて衝突し (例: faceimage/ジョー.pimg と 非常食.pimg)、キャッシュが
	// 別ファイルの内容を返す実害があった。pre.js 側 (prefetch/バージョン移行)
	// と同一規約であること。
	var key;
	{
		var d = await crypto.subtle.digest('SHA-256', new TextEncoder().encode(url));
		var h = Array.from(new Uint8Array(d)).map(function(b) {
			return b.toString(16).padStart(2, '0');
		}).join('');
		key = h.slice(0, 40) + '_' + url.replace(/[^A-Za-z0-9._-]/g, '_').slice(-32);
	}
	try {
		var ab = null;
		var root = null;
		if (use_cache) {
			try {
				root = await navigator.storage.getDirectory();
				var fh = await root.getFileHandle(key);
				var f = await fh.getFile();
				ab = await f.arrayBuffer();   // キャッシュヒット
			} catch (e) { /* 未キャッシュ */ }
		}
		if (!ab) {
			var resp = await fetch(url);
			if (!resp.ok) return 0;
			ab = await resp.arrayBuffer();
			if (use_cache) {
				try {
					if (!root) root = await navigator.storage.getDirectory();
					var wh = await root.getFileHandle(key, { create: true });
					var w = await wh.createWritable();
					await w.write(ab);
					await w.close();
				} catch (e) { console.warn('krkrz OPFS write failed:', key, e); }
			}
		}
		var u8 = new Uint8Array(ab);
		// ページ側ローディング表示の進捗 (累積ロードバイト数。OPFS ヒット含む)
		globalThis.__krkrzWebFetchBytes = (globalThis.__krkrzWebFetchBytes || 0) + u8.length;
		var p = _malloc(u8.length || 1);
		HEAPU8.set(u8, p);
		HEAPU32[out_ptr >>> 2] = p;
		HEAP32[out_len >>> 2] = u8.length;
		return 1;
	} catch (e) {
		console.error('krkrz_web_fetch failed:', url, e);
		return 0;
	}
});

namespace {

class tTVPHttpStorageMedia : public iTVPStorageMedia
{
	typedef tTVPHttpStorageMedia Self;
	static Self *InstanceCache;    // "web"    : OPFS キャッシュ有り
	static Self *InstanceNoCache;  // "webnc"  : キャッシュ無し (都度 fetch)
	tjs_int RefCount;

	ttstr MediaName;   // "web" / "webnc"
	bool UseCache;     // OPFS キャッシュを使うか

	// fetch のベース URL。web://./foo → fetch(BaseURL + "foo")。
	// 既定は空 (= HTML と同じディレクトリからの相対)。-webbase=<url> で変更可。
	std::string BaseURL;

	bool BaseResolved = false;

	// ファイル一覧マニフェスト (小文字・'/'区切り・先頭"./"無しのパス集合)。
	// krkrz の addAutoPath は GetListAt でフォルダ内ファイルを列挙して検索
	// テーブルを構築するが、サーバ上のバラファイル配信では列挙できないため、
	// ビルド時に tools/gen_manifest.py が生成した JSON を参照する。
	bool ManifestLoaded = false;
	std::set<std::string> ManifestFiles;
	// マニフェストファイル名 (BaseURL 直下)。-webmanifest= で変更可。
	std::string ManifestName = "krkrz_files.json";

	// BaseURL を遅延解決する。AtStart (静的初期化) の時点では TVPGetCommandLine が
	// 未初期化で null function になるため、初回アクセス (Open/Check) 時に取得する。
	void EnsureBase() {
		if (BaseResolved) return;
		BaseResolved = true;
		tTJSVariant val;
		if (TVPGetCommandLine(TJS_W("-webbase"), &val)) {
			tjs_string b = ((ttstr)val).AsStdString();
			TVPUtf16ToUtf8(BaseURL, b);
		}
		if (TVPGetCommandLine(TJS_W("-webmanifest"), &val)) {
			tjs_string m = ((ttstr)val).AsStdString();
			TVPUtf16ToUtf8(ManifestName, m);
		}
	}

	// マニフェストを初回のみ fetch してパースする (JSPI)。
	void EnsureManifest() {
		if (ManifestLoaded) return;
		ManifestLoaded = true;
		EnsureBase();
		std::string url = BaseURL + ManifestName;
		void *ptr = nullptr;
		int len = 0;
		// マニフェスト自体はキャッシュしない (更新検知のため常に取得)
		if (!krkrz_web_fetch(url.c_str(), /*use_cache=*/0, &ptr, &len) || !ptr) {
			TVPAddImportantLog(ttstr(TJS_W("web:// manifest not found: ")) + ttstr(url.c_str()));
			return;
		}
		picojson::value v;
		std::string err;
		picojson::parse(v, (const char*)ptr, (const char*)ptr + len, &err);
		free(ptr);
		if (!err.empty() || !v.is<picojson::array>()) {
			TVPAddImportantLog(ttstr(TJS_W("web:// manifest parse error")));
			return;
		}
		for (const auto &e : v.get<picojson::array>()) {
			if (e.is<std::string>()) ManifestFiles.insert(e.get<std::string>());
		}
		TVPAddLog(ttstr(TJS_W("web:// manifest loaded: ")) +
			ttstr((tjs_int)ManifestFiles.size()) + ttstr(TJS_W(" file(s)")));
	}

	// name ("./main/config.tjs" 等) をマニフェスト照合用キー (小文字・"./"除去) へ
	std::string NormKey(const ttstr &name) {
		std::string path;
		TVPUtf16ToUtf8(path, name.c_str());
		if (path.size() >= 2 && path[0] == '.' && path[1] == '/')
			path = path.substr(2);
		for (auto &c : path) if (c >= 'A' && c <= 'Z') c += 32; // ASCII 小文字化
		return path;
	}

	// マニフェスト由来の UTF-8 文字列を ttstr へ。ttstr(const char*) は narrow=SJIS
	// 解釈のため、非 ASCII ファイル名で変換失敗 → 例外になる。必ず UTF-8→UTF-16 で。
	static ttstr Utf8ToTtstr(const std::string &u8) {
		tjs_string ws;
		TVPUtf8ToUtf16(ws, u8);
		return ttstr(ws.c_str());
	}

	// name ("./scenario/bg.png" 等) を fetch URL へ変換。
	// パス部は ASCII のみ小文字化する: autopath 経由のアクセスはマニフェスト
	// (小文字) 由来で既に小文字、直接パス指定は原ケースのままなので、ここで
	// 揃えないと同一ファイルが別 URL (=別 OPFS キー) になる。配信サーバは
	// 小文字リクエストを実ファイルへ解決できること (同梱 serve.py は
	// 一覧対応表で解決する。Windows 系サーバは元々 case-insensitive)。
	std::string ToUrl(const ttstr &name) {
		EnsureBase();
		std::string path;
		TVPUtf16ToUtf8(path, name.c_str());
		// 先頭の "./" を除去
		if (path.size() >= 2 && path[0] == '.' && path[1] == '/')
			path = path.substr(2);
		// ASCII 小文字化 (UTF-8 マルチバイトは 0x80 以上なので影響しない)
		for (auto &c : path) if (c >= 'A' && c <= 'Z') c += 32;
		return BaseURL + path;
	}

	// URL を fetch して MemoryStream を返す (失敗時 nullptr)。所有権は呼び出し側。
	iTJSBinaryStream *Fetch(const ttstr &name) {
		std::string url = ToUrl(name);
		void *ptr = nullptr;
		int len = 0;
		if (!krkrz_web_fetch(url.c_str(), UseCache ? 1 : 0, &ptr, &len))
			return nullptr;
		// fetch バッファ (malloc) を MemoryStream にコピーして所有させ、元は解放。
		// tTVPMemoryStream(block, size) は参照 (非コピー) なので使わない。
		tTVPMemoryStream *st = new tTVPMemoryStream((tjs_uint)len);
		if (len > 0) st->Write(ptr, (tjs_uint)len);
		st->Seek(0, TJS_BS_SEEK_SET);
		free(ptr);
		return st;
	}

public:
	tTVPHttpStorageMedia(const tjs_char *mediaName, bool useCache)
		: RefCount(1), MediaName(mediaName), UseCache(useCache) {}

	void TJS_INTF_METHOD AddRef(void) override { ++RefCount; }
	void TJS_INTF_METHOD Release(void) override {
		if (RefCount == 1) delete this; else --RefCount;
	}

	virtual void TJS_INTF_METHOD GetName(ttstr &name) override { name = MediaName; }
	virtual void TJS_INTF_METHOD NormalizeDomainName(ttstr &name) override {}
	virtual void TJS_INTF_METHOD NormalizePathName(ttstr &name) override {}

	virtual bool TJS_INTF_METHOD CheckExistentStorage(const ttstr &name) override {
		EnsureManifest();
		if (!ManifestFiles.empty())
			return ManifestFiles.count(NormKey(name)) != 0; // マニフェスト照合 (fetch しない)
		// マニフェストが無い場合のフォールバック: 実際に取得して成否を返す
		iTJSBinaryStream *s = Fetch(name);
		if (!s) return false;
		s->Destruct();
		return true;
	}

	virtual iTJSBinaryStream * TJS_INTF_METHOD Open(const ttstr &name, tjs_uint32 flags) override {
		if ((flags & TJS_BS_ACCESS_MASK) != TJS_BS_READ) return nullptr; // 読み込み専用
		return Fetch(name);
	}

	// フォルダ name 直下のエントリを lister に渡す (addAutoPath のテーブル構築用)。
	// ファイルは名前のみ、サブフォルダは "name/" 形式で渡す (objres.cpp と同規約)。
	virtual void TJS_INTF_METHOD GetListAt(const ttstr &name, iTVPStorageLister *lister) override {
		EnsureManifest();
		if (ManifestFiles.empty()) return;
		// prefix = name を小文字化・"./"除去し末尾を "/" に揃えたもの ("" ならルート)
		std::string prefix = NormKey(name);
		if (!prefix.empty() && prefix.back() != '/') prefix += '/';
		std::set<std::string> emitted; // サブフォルダ名の重複防止
		for (const auto &f : ManifestFiles) {
			if (f.size() <= prefix.size() || f.compare(0, prefix.size(), prefix) != 0)
				continue;
			std::string rest = f.substr(prefix.size()); // prefix 以降
			std::string::size_type slash = rest.find('/');
			if (slash == std::string::npos) {
				lister->Add(Utf8ToTtstr(rest));                 // 直下ファイル
			} else {
				std::string sub = rest.substr(0, slash + 1);    // "subdir/"
				if (emitted.insert(sub).second)
					lister->Add(Utf8ToTtstr(sub));
			}
		}
	}

	virtual void TJS_INTF_METHOD GetLocallyAccessibleName(ttstr &name) override { name.Clear(); }

	// 正規化ストレージ名 ("web://./foo" / "webnc://./foo") を HTTP URL へ変換。
	// web/webnc 以外は false。WebMoviePlayer が <video> の src 解決に使う
	// (fetch/OPFS を通さず、ブラウザにストリーミングさせるため)。
	static bool ResolveUrl(const ttstr &fullname, std::string &url) {
		std::string n;
		TVPUtf16ToUtf8(n, fullname.c_str());
		Self *inst = nullptr;
		std::string rest;
		if (n.compare(0, 6, "web://") == 0) {
			inst = InstanceCache; rest = n.substr(6);
		} else if (n.compare(0, 8, "webnc://") == 0) {
			inst = InstanceNoCache; rest = n.substr(8);
		}
		if (!inst) return false;
		inst->EnsureBase();
		if (rest.size() >= 2 && rest[0] == '.' && rest[1] == '/')
			rest = rest.substr(2);
		// ToUrl と同じ規約 (ASCII 小文字化)。<video> の src も小文字 URL に揃える
		for (auto &c : rest) if (c >= 'A' && c <= 'Z') c += 32;
		url = inst->BaseURL + rest;
		return true;
	}

	static void Load() {
		if (!InstanceCache) {
			InstanceCache = new Self(TJS_W("web"), /*useCache=*/true);
			TVPRegisterStorageMedia(InstanceCache);
		}
		if (!InstanceNoCache) {
			InstanceNoCache = new Self(TJS_W("webnc"), /*useCache=*/false);
			TVPRegisterStorageMedia(InstanceNoCache);
		}
	}
	static void Unload() {
		if (InstanceCache) {
			TVPUnregisterStorageMedia(InstanceCache);
			InstanceCache->Release();
			InstanceCache = nullptr;
		}
		if (InstanceNoCache) {
			TVPUnregisterStorageMedia(InstanceNoCache);
			InstanceNoCache->Release();
			InstanceNoCache = nullptr;
		}
	}
};

tTVPHttpStorageMedia * tTVPHttpStorageMedia::InstanceCache = nullptr;
tTVPHttpStorageMedia * tTVPHttpStorageMedia::InstanceNoCache = nullptr;

tTVPAtStart AtStart(TVP_ATSTART_PRI_PREPARE, tTVPHttpStorageMedia::Load);
tTVPAtExit  AtExit(TVP_ATEXIT_PRI_PREPARE, tTVPHttpStorageMedia::Unload);

} // anonymous

//---------------------------------------------------------------------------
// WebMoviePlayer.cpp から使う URL 解決ヘルパ (宣言は利用側で extern)
//---------------------------------------------------------------------------
bool TVPGetWebStorageURL(const ttstr &name, std::string &url)
{
	return tTVPHttpStorageMedia::ResolveUrl(name, url);
}

#endif // __EMSCRIPTEN__
