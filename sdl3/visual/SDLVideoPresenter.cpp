//---------------------------------------------------------------------------
// SDL 版 overlay 動画 presenter 実装。
//
// pull 型 (SDLDrawDevice::Show() が RenderVideoFrame を呼ぶ)。ARGB 経路 (UpdateFrame) と
// YUV plane 経路 (UpdateFrameYUV → SDL の YUV テクスチャで GPU 側 YUV→RGB) の両対応。
// generic 版 VideoOverlay からは中立 IF (iTVPVideoOverlayPresenter) 経由で扱われる。
//---------------------------------------------------------------------------
#include "tjsCommHead.h"
#include "SDLVideoPresenter.h"
#include "VideoOverlayPresenter.h"
#include "tjsVariant.h"
#include "DebugIntf.h"
#include "LogIntf.h"                 // TVPLOG_ERROR

#include <SDL3/SDL.h>
#include <mutex>
#include <atomic>
#include <algorithm>
#include <cstring>
#include <vector>

namespace {

class tTVPSDLVideoOverlayPresenter
	: public iTVPVideoOverlayPresenter
	, public iTVPSDLVideoPresenter
{
	std::mutex           mMutex;
	// ARGB 経路
	char*                mBuffer;      // ARGB8888 (top-down)
	// YUV 経路 (I420/NV12 を packed に copy 保持)
	std::vector<uint8_t> mYUV;
	iTVPMoviePlayer::VideoPlaneFormat mYUVFormat;
	int                  mWidth, mHeight;
	bool                 mIsYUV;       // 現在保持しているフレームが YUV か
	bool                 mDirty;
	SDL_Texture*         mTexture;
	int                  mTexW, mTexH;
	bool                 mTexIsYUV;
	SDL_Renderer*        mTexRenderer;
	iTVPSDLVideoPresenterHost* mHost;
	bool                 mActive;
	std::atomic<bool>    mVisible;    // overlay 表示可否 (WINVER 仕様: 既定 false)。描画スレッドから読む

	// mixer 追加画像 (動画の上へ α 合成。setMixingLayer の後継)
	std::vector<uint8_t> mMixer;        // BGRA (ARGB8888 メモリ並び) top-down packed
	int                  mMixerW, mMixerH;
	tTVPRect             mMixerRect;     // プライマリレイヤ座標での配置矩形
	float                mMixerAlpha;
	bool                 mMixerValid;
	bool                 mMixerDirty;    // Back を texture へ未反映か
	SDL_Texture*         mMixerTex;
	int                  mMixerTexW, mMixerTexH;
	SDL_Renderer*        mMixerTexRenderer;

public:
	tTVPSDLVideoOverlayPresenter()
		: mBuffer(nullptr), mYUVFormat(iTVPMoviePlayer::VPF_UNKNOWN)
		, mWidth(0), mHeight(0), mIsYUV(false), mDirty(false)
		, mTexture(nullptr), mTexW(0), mTexH(0), mTexIsYUV(false), mTexRenderer(nullptr)
		, mHost(nullptr), mActive(false), mVisible(false)
		, mMixerW(0), mMixerH(0), mMixerAlpha(1.0f), mMixerValid(false), mMixerDirty(false)
		, mMixerTex(nullptr), mMixerTexW(0), mMixerTexH(0), mMixerTexRenderer(nullptr)
	{ mMixerRect.left = mMixerRect.top = mMixerRect.right = mMixerRect.bottom = 0; }

	~tTVPSDLVideoOverlayPresenter() override
	{
		Deactivate();
		FreeAll();
	}

	//--- iTVPVideoOverlayPresenter (generic VideoOverlay 側) ---------------
	bool Bind(const tTJSVariant &drawDeviceObj) override
	{
		mHost = nullptr;
		if (drawDeviceObj.Type() != tvtObject) return false;
		tTJSVariantClosure clo = drawDeviceObj.AsObjectClosureNoAddRef();
		if (clo.Object == nullptr) return false;
		tTJSVariant val;
		if (TJS_FAILED(clo.PropGet(0, TJS_W("sdlVideoPresenterHost"), nullptr, &val, nullptr)))
			return false;
		tjs_int64 p = (tjs_int64)val;
		if (p == 0) return false;
		mHost = reinterpret_cast<iTVPSDLVideoPresenterHost*>((intptr_t)p);
		return true;
	}

	void Activate() override
	{
		if (mHost && !mActive) { mHost->AddVideoPresenter(this); mActive = true; }
	}
	void Deactivate() override
	{
		if (mHost && mActive) mHost->RemoveVideoPresenter(this);
		mActive = false;
	}

	//--- 表示可否 (WINVER の VideoOverlay.visible と同仕様) --------------------
	// SetVisible = iTVPVideoOverlayPresenter (generic 側), IsVisible = iTVPSDLVideoPresenter
	// (SDLDrawDevice 側)。同じ mVisible を両 IF が参照する。
	void SetVisible(bool visible) override { mVisible.store(visible, std::memory_order_relaxed); }
	bool TJS_INTF_METHOD IsVisible() const override { return mVisible.load(std::memory_order_relaxed); }

	bool SupportsYUV() const override { return true; }   // SDL は YUV テクスチャ内蔵

	void UpdateFrame(int w, int h, std::function<void(char *dest, int pitch)> updater) override
	{
		std::lock_guard<std::mutex> lock(mMutex);
		if (!mBuffer || mWidth != w || mHeight != h || mIsYUV) {
			delete[] mBuffer;
			mBuffer = new char[(size_t)w * h * 4];
			mWidth = w; mHeight = h;
		}
		updater(mBuffer, w * 4);
		mIsYUV = false;
		mDirty = true;
	}

	void UpdateFrameYUV(const iTVPMoviePlayer::VideoPlaneFrame &frame) override
	{
		std::lock_guard<std::mutex> lock(mMutex);
		int w = frame.width, h = frame.height;
		if (w <= 0 || h <= 0 || frame.planeCount < 2) return;
		mWidth = w; mHeight = h; mYUVFormat = frame.format; mIsYUV = true;
		// planes を packed (stride=plane 幅) に copy して保持。
		if (frame.format == iTVPMoviePlayer::VPF_I420 && frame.planeCount >= 3) {
			int cw = (w + 1) / 2, ch = (h + 1) / 2;
			mYUV.resize((size_t)w * h + 2 * (size_t)cw * ch);
			CopyPlane(&mYUV[0],                    frame.planes[0], w,  h);
			CopyPlane(&mYUV[(size_t)w*h],          frame.planes[1], cw, ch);
			CopyPlane(&mYUV[(size_t)w*h+(size_t)cw*ch], frame.planes[2], cw, ch);
		} else { // NV12 / NV21: Y + interleaved UV
			int cw = (w + 1) / 2, ch = (h + 1) / 2;
			mYUV.resize((size_t)w * h + 2 * (size_t)cw * ch);
			CopyPlane(&mYUV[0], frame.planes[0], w, h);
			CopyPlane(&mYUV[(size_t)w*h], frame.planes[1], cw * 2, ch);  // UV interleaved: 幅 cw*2
		}
		mDirty = true;
	}

	void ClearFrame() override
	{
		std::lock_guard<std::mutex> lock(mMutex);
		FreeAll();
	}

	//--- mixer 追加画像 (setMixingLayer の後継) ----------------------------
	void SetMixerImage(const void *bgra, int w, int h, int pitch,
	                   const tTVPRect &primaryRect, float alpha) override
	{
		std::lock_guard<std::mutex> lock(mMutex);
		if (!bgra || w <= 0 || h <= 0) { mMixerValid = false; return; }
		mMixer.resize((size_t)w * h * 4);
		uint8_t *d = mMixer.data();
		const uint8_t *s = (const uint8_t*)bgra;
		int rb = w * 4;
		for (int y = 0; y < h; ++y)
			memcpy(d + (size_t)y * rb, s + (ptrdiff_t)y * pitch, rb);
		mMixerW = w; mMixerH = h; mMixerRect = primaryRect; mMixerAlpha = alpha;
		mMixerValid = true; mMixerDirty = true;
	}
	void SetMixerAlpha(float alpha) override
	{
		std::lock_guard<std::mutex> lock(mMutex);
		mMixerAlpha = alpha;
	}
	void ClearMixerImage() override
	{
		std::lock_guard<std::mutex> lock(mMutex);
		mMixerValid = false;
		if (mMixerTex) { SDL_DestroyTexture(mMixerTex); mMixerTex = nullptr; }
		mMixerTexW = mMixerTexH = 0;
		std::vector<uint8_t>().swap(mMixer);
	}

	//--- iTVPSDLVideoPresenter (SDLDrawDevice 側、描画スレッド) -------------
	bool TJS_INTF_METHOD RenderVideoFrame(const tTVPSDLVideoPresenterContext &ctx) override
	{
		std::lock_guard<std::mutex> lock(mMutex);
		if (mWidth <= 0 || mHeight <= 0 || !ctx.Renderer) return false;
		if (mIsYUV ? mYUV.empty() : (mBuffer == nullptr)) return false;

		// テクスチャ (形式/サイズ/renderer 変化で作り直し)
		if (!mTexture || mTexW != mWidth || mTexH != mHeight ||
		    mTexIsYUV != mIsYUV || mTexRenderer != ctx.Renderer) {
			if (mTexture) { SDL_DestroyTexture(mTexture); mTexture = nullptr; }
			SDL_PixelFormat fmt = mIsYUV
				? (mYUVFormat == iTVPMoviePlayer::VPF_NV12 ? SDL_PIXELFORMAT_NV12
				 : mYUVFormat == iTVPMoviePlayer::VPF_NV21 ? SDL_PIXELFORMAT_NV21
				 : SDL_PIXELFORMAT_IYUV)
				: SDL_PIXELFORMAT_ARGB8888;
			mTexture = SDL_CreateTexture(ctx.Renderer, fmt, SDL_TEXTUREACCESS_STREAMING, mWidth, mHeight);
			mTexW = mWidth; mTexH = mHeight; mTexIsYUV = mIsYUV; mTexRenderer = ctx.Renderer;
			mDirty = true;
			if (!mTexture) { TVPLOG_ERROR("SDLVideoPresenter: CreateTexture failed:{}", SDL_GetError()); return false; }
		}
		if (mDirty) {
			if (mIsYUV) UploadYUV();
			else UploadARGB();
			mDirty = false;
		}

		// 描画先へ内接 (letterbox) 配置・中央寄せ
		SDL_FRect dst;
		int sw = ctx.TargetWidth, sh = ctx.TargetHeight;
		if (sw > 0 && sh > 0) {
			double scale = std::min((double)sw / mWidth, (double)sh / mHeight);
			int nw = (int)(mWidth * scale), nh = (int)(mHeight * scale);
			dst.x = (float)((sw - nw) / 2); dst.y = (float)((sh - nh) / 2);
			dst.w = (float)nw; dst.h = (float)nh;
		} else { dst.x = 0; dst.y = 0; dst.w = (float)mWidth; dst.h = (float)mHeight; }
		SDL_SetRenderDrawBlendMode(ctx.Renderer, SDL_BLENDMODE_NONE);
		SDL_RenderTexture(ctx.Renderer, mTexture, nullptr, &dst);

		// mixer 追加画像を動画の上へ α 合成 (プライマリ座標 → DestRect へ変換)
		DrawMixer(ctx);
		return true;
	}

private:
	//! mixer 画像を動画の上へ描く。プライマリレイヤ座標の mMixerRect を ctx.DestRect へマップする。
	void DrawMixer(const tTVPSDLVideoPresenterContext &ctx)
	{
		if (!mMixerValid || mMixerW <= 0 || mMixerH <= 0) return;
		if (mMixerTex && (mMixerTexW != mMixerW || mMixerTexH != mMixerH || mMixerTexRenderer != ctx.Renderer)) {
			SDL_DestroyTexture(mMixerTex); mMixerTex = nullptr;
		}
		if (!mMixerTex) {
			mMixerTex = SDL_CreateTexture(ctx.Renderer, SDL_PIXELFORMAT_ARGB8888,
				SDL_TEXTUREACCESS_STREAMING, mMixerW, mMixerH);
			if (!mMixerTex) return;
			mMixerTexW = mMixerW; mMixerTexH = mMixerH; mMixerTexRenderer = ctx.Renderer;
			mMixerDirty = true;
		}
		if (mMixerDirty) {
			void *pixels = nullptr; int pitch = 0;
			if (SDL_LockTexture(mMixerTex, nullptr, &pixels, &pitch)) {
				int rb = mMixerW * 4;
				if (pitch == rb) memcpy(pixels, mMixer.data(), (size_t)rb * mMixerH);
				else for (int y = 0; y < mMixerH; ++y)
					memcpy((char*)pixels + (size_t)y * pitch, mMixer.data() + (size_t)y * rb, rb);
				SDL_UnlockTexture(mMixerTex);
			}
			mMixerDirty = false;
		}
		// プライマリ座標 → 描画先 (DestRect) へスケール
		int srcW = ctx.SrcWidth  > 0 ? ctx.SrcWidth  : mMixerW;
		int srcH = ctx.SrcHeight > 0 ? ctx.SrcHeight : mMixerH;
		double dw = (double)ctx.DestRect.get_width()  / srcW;
		double dh = (double)ctx.DestRect.get_height() / srcH;
		SDL_FRect dst;
		dst.x = (float)(ctx.DestRect.left + mMixerRect.left * dw);
		dst.y = (float)(ctx.DestRect.top  + mMixerRect.top  * dh);
		dst.w = (float)((mMixerRect.right  - mMixerRect.left) * dw);
		dst.h = (float)((mMixerRect.bottom - mMixerRect.top ) * dh);
		float a = mMixerAlpha; if (a < 0) a = 0; if (a > 1) a = 1;
		SDL_SetTextureBlendMode(mMixerTex, SDL_BLENDMODE_BLEND);
		SDL_SetTextureAlphaModFloat(mMixerTex, a);
		SDL_RenderTexture(ctx.Renderer, mMixerTex, nullptr, &dst);
	}

	static void CopyPlane(uint8_t *dst, const iTVPMoviePlayer::VideoPlaneFrame::PlaneRef &src, int w, int h)
	{
		if (!src.data) return;
		for (int y = 0; y < h; ++y)
			memcpy(dst + (size_t)y * w, src.data + (size_t)y * src.stride, w);
	}

	void UploadARGB()
	{
		void* pixels = nullptr; int pitch = 0;
		if (SDL_LockTexture(mTexture, nullptr, &pixels, &pitch)) {
			int spitch = mWidth * 4;
			if (pitch == spitch) memcpy(pixels, mBuffer, (size_t)pitch * mHeight);
			else for (int y = 0; y < mHeight; ++y)
				memcpy((char*)pixels + (size_t)y * pitch, mBuffer + (size_t)y * spitch, spitch);
			SDL_UnlockTexture(mTexture);
		}
	}

	void UploadYUV()
	{
		int w = mWidth, h = mHeight, cw = (w + 1) / 2, ch = (h + 1) / 2;
		const uint8_t* Y = &mYUV[0];
		if (mYUVFormat == iTVPMoviePlayer::VPF_I420) {
			const uint8_t* U = Y + (size_t)w * h;
			const uint8_t* V = U + (size_t)cw * ch;
			SDL_UpdateYUVTexture(mTexture, nullptr, Y, w, U, cw, V, cw);
		} else { // NV12 / NV21
			const uint8_t* UV = Y + (size_t)w * h;
			SDL_UpdateNVTexture(mTexture, nullptr, Y, w, UV, cw * 2);
		}
	}

	void FreeAll()
	{
		if (mTexture) { SDL_DestroyTexture(mTexture); mTexture = nullptr; }
		delete[] mBuffer; mBuffer = nullptr;
		mYUV.clear();
		mWidth = mHeight = mTexW = mTexH = 0;
		mIsYUV = mTexIsYUV = false; mDirty = false; mTexRenderer = nullptr;
		// mixer 画像も破棄 (Close/停止時)
		if (mMixerTex) { SDL_DestroyTexture(mMixerTex); mMixerTex = nullptr; }
		mMixerTexW = mMixerTexH = 0; mMixerTexRenderer = nullptr;
		mMixerValid = false; mMixerDirty = false;
		std::vector<uint8_t>().swap(mMixer);
	}
};

iTVPVideoOverlayPresenter * CreateSDLVideoOverlayPresenter()
{
	return new tTVPSDLVideoOverlayPresenter();
}

} // anonymous

void TVPRegisterSDLVideoOverlayPresenterFactory()
{
	TVPRegisterVideoOverlayPresenterFactory(&CreateSDLVideoOverlayPresenter);
}
