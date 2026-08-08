//---------------------------------------------------------------------------
//!@file wasm (Emscripten) 用 MoviePlayer — ブラウザネイティブ <video> 実装
//
// libvpx 等のデコーダを wasm に移植するのではなく、ブラウザの <video> 要素に
// デコード・音声出力・同期を任せる。
//
// - mixer/overlay モード: <video> を DOM オーバレイとして canvas に重ねて表示。
//   デスクトップ SDL 実装 (SDLDrawDevice::UpdateVideoPosition) と同じ
//   「ウィンドウ内接フィット (contain)・中央寄せ」の見た目になる。
//   pointer-events:none なので入力はエンジン (canvas) に素通しされ、
//   ムービー中のクリックスキップ等がそのまま機能する。
// - layer モード: 非表示 <video> + requestVideoFrameCallback で新フレームを
//   検知し、メインループの Pump() (VideoOvlImpl::Update 冒頭) で offscreen
//   canvas に drawImage → getImageData → BGRA swizzle して OnVideoDecoded に
//   渡す (既存のダブルバッファ Bitmap → Layer 経路)。
//
// ソース解決:
// - web:// / webnc:// … HTTP URL を直接 <video> に与える (ストリーム配信。
//   fetch/OPFS を通さないのでメモリに全展開しない。シークは HTTP Range)。
// - それ以外 (MEMFS file:// や xp3 内) … ストレージから全読みして Blob URL。
//
// 音声は <video> がブラウザ経由で直接出す (SDL/WebAudio とは独立)。
// autoplay ポリシーで play() が拒否された場合は次のユーザ操作で再試行する。
//---------------------------------------------------------------------------
#include "tjsCommHead.h"

#ifdef __EMSCRIPTEN__

#include "MoviePlayer.h"
#include "StorageIntf.h"
#include "CharacterSet.h"
#include "DebugIntf.h"

#include <emscripten.h>
#include <string>
#include <vector>

// HttpStorageMedia.cpp 定義。web://,webnc:// 名を HTTP URL へ (それ以外 false)
extern bool TVPGetWebStorageURL(const ttstr &name, std::string &url);

//---------------------------------------------------------------------------
// JS 側実装。プレイヤーは int ハンドルで globalThis.__krkrzVideo に登録する。
//---------------------------------------------------------------------------

// オープン: <video> 生成 + src 設定 + メタデータ確定まで JSPI で待つ。
//   url        : HTTP(S) URL (UTF-8)。data 指定時は無視
//   data, len  : 非 null なら wasm ヒープ上のファイル内容から Blob URL を作る
//   戻り値     : 1=成功 (寸法・尺が確定) / 0=失敗
EM_ASYNC_JS(int, krkrz_video_open_js, (int handle, const char* curl, const void* data, int len), {
	var P = globalThis.__krkrzVideo || (globalThis.__krkrzVideo = {});
	var url, isBlob = false;
	if (data) {
		// ヒープはこの後解放されるのでコピーしてから Blob 化
		url = URL.createObjectURL(new Blob([HEAPU8.slice(data, data + len)]));
		isBlob = true;
	} else {
		url = UTF8ToString(curl);
	}
	var v = document.createElement('video');
	v.preload = 'auto';
	v.playsInline = true;
	// layer モードの canvas 読み出しが汚染 (taint) されないように。
	// 同一オリジンでは無関係、クロスオリジンは COEP 的にどのみち CORS 前提
	v.crossOrigin = 'anonymous';
	// mixer 表示用スタイル (表示時に位置は layout() が設定)
	v.style.cssText = 'position:fixed;display:none;left:0;top:0;' +
		'object-fit:contain;background:transparent;pointer-events:none;z-index:5;';
	document.body.appendChild(v);
	var st = {
		v: v, url: url, isBlob: isBlob,
		layer: false, visible: true, started: false, stopped: true,
		closed: false, newFrame: false, cap: null, ctx: null, img: null,
		onrelayout: null
	};
	// mixer 表示位置: canvas の表示矩形に一致させる (object-fit:contain が
	// SDL 実装と同じ内接フィットをやってくれる)
	st.layout = function() {
		var c = Module['canvas'];
		if (!c) return;
		var r = c.getBoundingClientRect();
		v.style.left = r.left + 'px';
		v.style.top = r.top + 'px';
		v.style.width = r.width + 'px';
		v.style.height = r.height + 'px';
	};
	st.show = function(show) {
		if (show) {
			st.layout();
			v.style.display = 'block';
			if (!st.onrelayout) {
				st.onrelayout = function() { st.layout(); };
				window.addEventListener('resize', st.onrelayout);
				window.addEventListener('scroll', st.onrelayout);
			}
		} else {
			v.style.display = 'none';
			if (st.onrelayout) {
				window.removeEventListener('resize', st.onrelayout);
				window.removeEventListener('scroll', st.onrelayout);
				st.onrelayout = null;
			}
		}
	};
	// 再生終了で最終フレームを消す (デスクトップの ClearVideo 相当)
	v.addEventListener('ended', function() { if (!st.layer) st.show(false); });
	P[handle] = st;
	return await new Promise(function(resolve) {
		var done = false;
		v.addEventListener('loadedmetadata', function() {
			if (!done) { done = true; resolve(1); }
		});
		v.addEventListener('error', function() {
			if (!done) {
				done = true;
				console.error('krkrz video open failed:', url, v.error);
				resolve(0);
			}
		});
		v.src = url;
	});
});

EM_JS(void, krkrz_video_close_js, (int handle), {
	var P = globalThis.__krkrzVideo;
	var st = P && P[handle];
	if (!st) return;
	st.closed = true;
	st.show(false);
	st.v.pause();
	st.v.removeAttribute('src');
	st.v.load();
	st.v.remove();
	if (st.isBlob) URL.revokeObjectURL(st.url);
	delete P[handle];
});

EM_JS(void, krkrz_video_set_layer_mode_js, (int handle, int layer), {
	var st = globalThis.__krkrzVideo[handle];
	if (!st) return;
	st.layer = !!layer;
	if (st.layer) {
		st.show(false);
		// rVFC で新フレームを検知し続ける (Pump が引き取る)
		var schedule = function() {
			if (st.closed) return;
			st.v.requestVideoFrameCallback(function() {
				st.newFrame = true;
				schedule();
			});
		};
		schedule();
	}
});

EM_JS(void, krkrz_video_play_js, (int handle), {
	var st = globalThis.__krkrzVideo[handle];
	if (!st) return;
	st.started = true;
	st.stopped = false;
	if (!st.layer && st.visible) st.show(true);
	st.v.play().catch(function(e) {
		// autoplay ポリシー拒否: 次のユーザ操作で再試行
		console.warn('krkrz video: play() rejected, waiting for user gesture', e);
		var retry = function() { if (!st.closed && !st.stopped) st.v.play().catch(function(){}); };
		window.addEventListener('pointerdown', retry, { once: true });
		window.addEventListener('keydown', retry, { once: true });
	});
});

EM_JS(void, krkrz_video_stop_js, (int handle), {
	var st = globalThis.__krkrzVideo[handle];
	if (!st) return;
	st.stopped = true;
	st.v.pause();
	try { st.v.currentTime = 0; } catch (e) {}
	if (!st.layer) st.show(false);
});

EM_JS(void, krkrz_video_pause_js, (int handle), {
	var st = globalThis.__krkrzVideo[handle];
	if (st) st.v.pause();
});

EM_JS(void, krkrz_video_resume_js, (int handle), {
	var st = globalThis.__krkrzVideo[handle];
	if (st && !st.stopped) st.v.play().catch(function(){});
});

EM_JS(void, krkrz_video_seek_js, (int handle, double posSec), {
	var st = globalThis.__krkrzVideo[handle];
	if (st) { try { st.v.currentTime = posSec; } catch (e) {} }
});

EM_JS(void, krkrz_video_set_loop_js, (int handle, int loop), {
	var st = globalThis.__krkrzVideo[handle];
	if (st) st.v.loop = !!loop;
});

EM_JS(int, krkrz_video_get_loop_js, (int handle), {
	var st = globalThis.__krkrzVideo[handle];
	return (st && st.v.loop) ? 1 : 0;
});

EM_JS(int, krkrz_video_width_js, (int handle), {
	var st = globalThis.__krkrzVideo[handle];
	return st ? st.v.videoWidth : 0;
});

EM_JS(int, krkrz_video_height_js, (int handle), {
	var st = globalThis.__krkrzVideo[handle];
	return st ? st.v.videoHeight : 0;
});

EM_JS(double, krkrz_video_duration_js, (int handle), {
	var st = globalThis.__krkrzVideo[handle];
	if (!st) return 0;
	var d = st.v.duration;
	return isFinite(d) ? d : 0;
});

EM_JS(double, krkrz_video_position_js, (int handle), {
	var st = globalThis.__krkrzVideo[handle];
	return st ? st.v.currentTime : 0;
});

// 「再生セッションが活きているか」。pause 中も true (デスクトップと同様、
// CheckUpdate が Pause を Stop と誤認して onStatusChanged("stop") を
// 飛ばさないため)。ended / stop() / 未再生で false。
EM_JS(int, krkrz_video_is_playing_js, (int handle), {
	var st = globalThis.__krkrzVideo[handle];
	if (!st) return 0;
	return (st.started && !st.stopped && !st.v.ended) ? 1 : 0;
});

EM_JS(void, krkrz_video_set_volume_js, (int handle, double vol), {
	var st = globalThis.__krkrzVideo[handle];
	if (st) st.v.volume = Math.min(1, Math.max(0, vol));
});

EM_JS(double, krkrz_video_get_volume_js, (int handle), {
	var st = globalThis.__krkrzVideo[handle];
	return st ? st.v.volume : 0;
});

EM_JS(void, krkrz_video_set_overlay_visible_js, (int handle, int visible), {
	var st = globalThis.__krkrzVideo[handle];
	if (!st) return;
	st.visible = !!visible;
	if (!st.layer && st.started && !st.stopped) st.show(st.visible);
});

// layer モード: 新フレームがあれば offscreen canvas に取り込み、寸法を返す。
// 続けて krkrz_video_copy_js で BGRA 転送する 2 段構え (間に C++ 側の
// Bitmap 確保が挟まるため)。
EM_JS(int, krkrz_video_grab_js, (int handle, int* out_w, int* out_h), {
	var st = globalThis.__krkrzVideo[handle];
	if (!st || !st.newFrame) return 0;
	st.newFrame = false;
	var v = st.v;
	var w = v.videoWidth, h = v.videoHeight;
	if (!w || !h) return 0;
	if (!st.cap || st.cap.width != w || st.cap.height != h) {
		st.cap = new OffscreenCanvas(w, h);
		st.ctx = st.cap.getContext('2d', { willReadFrequently: true });
	}
	try {
		st.ctx.drawImage(v, 0, 0, w, h);
		st.img = st.ctx.getImageData(0, 0, w, h);
	} catch (e) {
		console.error('krkrz video frame grab failed:', e);
		return 0;
	}
	HEAP32[out_w >> 2] = w;
	HEAP32[out_h >> 2] = h;
	return 1;
});

// grab 済みフレームを dest (pitch バイト刻み) へ RGBA→BGRA 変換しつつコピー
EM_JS(void, krkrz_video_copy_js, (int handle, char* dest, int pitch), {
	var st = globalThis.__krkrzVideo[handle];
	var img = st && st.img;
	if (!img) return;
	var w = img.width, h = img.height, src = img.data;
	var heap = HEAPU8;
	var si = 0;
	for (var y = 0; y < h; y++) {
		var di = dest + y * pitch;
		for (var x = 0; x < w; x++) {
			heap[di]     = src[si + 2]; // B
			heap[di + 1] = src[si + 1]; // G
			heap[di + 2] = src[si];     // R
			heap[di + 3] = src[si + 3]; // A
			si += 4;
			di += 4;
		}
	}
	st.img = null;
});

//---------------------------------------------------------------------------
// iTVPMoviePlayer 実装
//---------------------------------------------------------------------------
namespace {

class tTVPWebMoviePlayer : public iTVPMoviePlayer
{
	int mHandle;
	bool mLayerMode = false;
	OnVideoDecoded mOnVideoDecoded;

	static int NextHandle() {
		static int next = 1;
		return next++;
	}

public:
	tTVPWebMoviePlayer() : mHandle(NextHandle()) {}

	~tTVPWebMoviePlayer() override {
		krkrz_video_close_js(mHandle);
	}

	// name は正規化ストレージ名 (web://./..., file://./... 等)
	bool Open(const ttstr &name) {
		std::string url;
		std::vector<char> buf;
		if (!TVPGetWebStorageURL(name, url)) {
			// web:// 以外はストレージ経由で全読みして Blob URL にする
			// (MEMFS 直下や xp3 アーカイブ内も透過的に扱える)
			try {
				iTJSBinaryStream *st = TVPCreateStream(name, TJS_BS_READ);
				tjs_uint64 size = st->GetSize();
				buf.resize((size_t)size);
				if (size > 0) st->Read(buf.data(), (tjs_uint)size);
				st->Destruct();
			} catch (...) {
				return false; // 呼び出し側が LoadError 化する
			}
		}
		return krkrz_video_open_js(mHandle, url.c_str(),
			buf.empty() ? nullptr : buf.data(), (int)buf.size()) != 0;
	}

	void Play(bool loop = false) override {
		if (loop) krkrz_video_set_loop_js(mHandle, 1);
		krkrz_video_play_js(mHandle);
	}
	void Stop() override { krkrz_video_stop_js(mHandle); }
	void Pause() override { krkrz_video_pause_js(mHandle); }
	void Resume() override { krkrz_video_resume_js(mHandle); }
	void Seek(int64_t posUs) override {
		krkrz_video_seek_js(mHandle, (double)posUs / 1000000.0);
	}
	void SetLoop(bool loop) override {
		krkrz_video_set_loop_js(mHandle, loop ? 1 : 0);
	}

	int32_t Width() const override { return krkrz_video_width_js(mHandle); }
	int32_t Height() const override { return krkrz_video_height_js(mHandle); }
	int64_t Duration() const override {
		return (int64_t)(krkrz_video_duration_js(mHandle) * 1000000.0);
	}
	int64_t Position() const override {
		return (int64_t)(krkrz_video_position_js(mHandle) * 1000000.0);
	}
	bool IsPlaying() const override {
		return krkrz_video_is_playing_js(mHandle) != 0;
	}
	bool Loop() const override { return krkrz_video_get_loop_js(mHandle) != 0; }

	void SetOnVideoDecoded(OnVideoDecoded callback) override {
		mOnVideoDecoded = callback;
	}

	bool IsAudioAvailable() const override { return true; }

	void SetVolume(float volume) override {
		krkrz_video_set_volume_js(mHandle, (double)volume);
	}
	float Volume() const override {
		return (float)krkrz_video_get_volume_js(mHandle);
	}

	void SetLayerMode(bool layer) override {
		mLayerMode = layer;
		krkrz_video_set_layer_mode_js(mHandle, layer ? 1 : 0);
	}

	// メインスレッド毎フレーム (VideoOvlImpl::Update 冒頭) から。
	// layer モードで rVFC が立てた新フレームを OnVideoDecoded に引き渡す
	void Pump() override {
		if (!mLayerMode || !mOnVideoDecoded) return;
		int w = 0, h = 0;
		if (krkrz_video_grab_js(mHandle, &w, &h)) {
			mOnVideoDecoded(w, h, [this](char *dest, int pitch) {
				krkrz_video_copy_js(mHandle, dest, pitch);
			});
		}
	}

	void SetOverlayVisible(bool visible) override {
		krkrz_video_set_overlay_visible_js(mHandle, visible ? 1 : 0);
	}
};

} // anonymous

//---------------------------------------------------------------------------
// エントリポイント (movie_null.cpp の代替)
//---------------------------------------------------------------------------

// CreatePlayer (ファイルパス版)。wasm では正規化ストレージ名が渡ってくる
// (VideoOvlImpl::Open の __EMSCRIPTEN__ 分岐は localname 変換をしない)
iTVPMoviePlayer *
TVPCreateMoviePlayer(const tjs_char *filename, bool preferYUV)
{
	// preferYUV: <video> DOM 再生ではブラウザがデコードするため無視
	tTVPWebMoviePlayer *player = new tTVPWebMoviePlayer();
	if (player->Open(ttstr(filename))) {
		return player;
	}
	delete player;
	return nullptr;
}

// CreatePlayer (ストリーム版)。wasm は KRKRZ_MOVIE_STREAM=OFF なので未使用
iTVPMoviePlayer *
TVPCreateMoviePlayer(IMovieReadStream *stream, const char *filename, bool preferYUV)
{
	return nullptr;
}

#endif // __EMSCRIPTEN__
