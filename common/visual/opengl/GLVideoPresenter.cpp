//---------------------------------------------------------------------------
// OpenGL 版 overlay 動画 presenter 実装。
//
// 従来 OGLDrawDevice が push で持っていた動画バッファ / GLTexture / 合成
// (mVideoBuffer / _video_texture / ShowVideo) を presenter 側へ移設し、pull 型
// (OGLDrawDevice::Show() が RenderVideoFrame を呼ぶ) に統一する。generic 版
// VideoOverlay からは中立 IF (iTVPVideoOverlayPresenter) 経由で扱われる。
//
// GL 操作は RenderVideoFrame / ClearFrame (= 描画スレッド or GL context current な
// クローズ経路) でのみ行う。従来 ClearVideo が GL context 直前でテクスチャを削除して
// いたのと同じ契約。
//---------------------------------------------------------------------------
#include "tjsCommHead.h"

// GL 動画 presenter は overlay 動画を GL で描く GENERIC (SDL/LIB) 専用。WINVER-OGL は
// D3D11/子ウィンドウ fallback を使うため対象外 (KRKRZ_SRC_OPENGL は WIN でもコンパイルされるので
// ここで __GENERIC__ ガード)。
#ifdef __GENERIC__

#include "GLVideoPresenter.h"
#include "VideoOverlayPresenter.h"
#include "GLTexture.h"
#include "GLShaderUtil.h"     // CompileProgram
#include "tjsVariant.h"

#include <mutex>
#include <algorithm>
#include <cstring>
#include <vector>

namespace {

//---------------------------------------------------------------------------
// I420(3 プレーン)→RGB(BT.601 limited) を自前シェーダで描く軽量ドロワ。
// GLTextureDrawer は単一テクスチャ固定なので YUV には使えない。GLShaderUtil で
// 独自プログラムを組み、Y/U/V を R8 テクスチャ 3 枚 (unit 0/1/2) で描く。
// 頂点/UV 規約は GLTextureDrawer と一致させ ComputeFitPosition をそのまま流用。
// GLSL ES 1.00 (texture2D / precision) = ES2 ベースライン。
//---------------------------------------------------------------------------
class GLYUVDrawer {
	GLuint prog;
	GLuint texY, texU, texV;
	GLint  aPos, aUV, uY, uU, uV;
	int    texW, texH;   // 現在の Y プレーン寸法
public:
	GLYUVDrawer() : prog(0), texY(0), texU(0), texV(0)
		, aPos(-1), aUV(-1), uY(-1), uU(-1), uV(-1), texW(0), texH(0) {}

	bool Ensure()
	{
		if (prog) return true;
		static const char *VS =
			"attribute vec2 a_position; attribute vec2 a_texCoord; varying vec2 v_texCoord;\n"
			"void main(){ gl_Position=vec4(a_position,0.0,1.0); v_texCoord=a_texCoord; }\n";
		static const char *FS =
			"precision mediump float;\n"
			"varying vec2 v_texCoord;\n"
			"uniform sampler2D texY; uniform sampler2D texU; uniform sampler2D texV;\n"
			"void main(){\n"
			"  float Y=texture2D(texY,v_texCoord).r;\n"
			"  float U=texture2D(texU,v_texCoord).r;\n"
			"  float V=texture2D(texV,v_texCoord).r;\n"
			"  float y=(Y-0.0627451)*1.164383;\n"   // (Y-16/255)*1.164383
			"  float u=U-0.5019608; float v=V-0.5019608;\n"
			"  float r=y+1.596027*v;\n"
			"  float g=y-0.391762*u-0.812968*v;\n"
			"  float b=y+2.017232*u;\n"
			"  gl_FragColor=vec4(clamp(r,0.0,1.0),clamp(g,0.0,1.0),clamp(b,0.0,1.0),1.0);\n"
			"}\n";
		prog = CompileProgram(VS, FS);
		if (!prog) return false;
		aPos = glGetAttribLocation(prog, "a_position");
		aUV  = glGetAttribLocation(prog, "a_texCoord");
		uY = glGetUniformLocation(prog, "texY");
		uU = glGetUniformLocation(prog, "texU");
		uV = glGetUniformLocation(prog, "texV");
		return true;
	}

	static GLuint MakeR8(int w, int h)
	{
		GLuint t = 0; glGenTextures(1, &t);
		glBindTexture(GL_TEXTURE_2D, t);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
		glTexImage2D(GL_TEXTURE_2D, 0, GL_R8, w, h, 0, GL_RED, GL_UNSIGNED_BYTE, nullptr);
		return t;
	}
	void EnsureTex(int w, int h)
	{
		if (texY && texW == w && texH == h) return;
		FreeTex();
		int cw = (w+1)/2, ch = (h+1)/2;
		texY = MakeR8(w, h); texU = MakeR8(cw, ch); texV = MakeR8(cw, ch);
		texW = w; texH = h;
	}
	//! packed I420 (Y=w×h, U/V=cw×ch) を描く。pos は ComputeFitPosition の clip quad。
	void Draw(const uint8_t *y, const uint8_t *u, const uint8_t *v, int w, int h,
	          int scr_w, int scr_h, const GLfloat pos[8])
	{
		if (!Ensure() || !y || !u || !v || w <= 0 || h <= 0) return;
		EnsureTex(w, h);
		int cw = (w+1)/2, ch = (h+1)/2;
		glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
		glActiveTexture(GL_TEXTURE0); glBindTexture(GL_TEXTURE_2D, texY);
		glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, w,  h,  GL_RED, GL_UNSIGNED_BYTE, y);
		glActiveTexture(GL_TEXTURE1); glBindTexture(GL_TEXTURE_2D, texU);
		glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, cw, ch, GL_RED, GL_UNSIGNED_BYTE, u);
		glActiveTexture(GL_TEXTURE2); glBindTexture(GL_TEXTURE_2D, texV);
		glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, cw, ch, GL_RED, GL_UNSIGNED_BYTE, v);

		glViewport(0, 0, scr_w, scr_h);
		glDisable(GL_DEPTH_TEST); glDisable(GL_STENCIL_TEST);
		glDisable(GL_SCISSOR_TEST); glDisable(GL_CULL_FACE); glDisable(GL_BLEND);
		glUseProgram(prog);
		// UV は GLTextureDrawer と同規約 (lt→(0,1) lb→(0,0) rt→(1,1) rb→(1,0))
		static const GLfloat uv[8] = { 0,1, 0,0, 1,1, 1,0 };
		glEnableVertexAttribArray(aPos); glEnableVertexAttribArray(aUV);
		glUniform1i(uY, 0); glUniform1i(uU, 1); glUniform1i(uV, 2);
		glVertexAttribPointer(aPos, 2, GL_FLOAT, GL_FALSE, 0, pos);
		glVertexAttribPointer(aUV,  2, GL_FLOAT, GL_FALSE, 0, uv);
		glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
		// 後続の GLTextureDrawer(mixer) は unit0 前提なので active を戻す。
		glActiveTexture(GL_TEXTURE0);
	}
	void FreeTex()
	{
		if (texY) glDeleteTextures(1, &texY);
		if (texU) glDeleteTextures(1, &texU);
		if (texV) glDeleteTextures(1, &texV);
		texY = texU = texV = 0; texW = texH = 0;
	}
	void Done()   // GL context current で呼ぶこと
	{
		FreeTex();
		if (prog) { glDeleteProgram(prog); prog = 0; }
	}
};


class tTVPGLVideoOverlayPresenter
	: public iTVPVideoOverlayPresenter
	, public iTVPGLVideoPresenter
{
	std::mutex           mMutex;
	char*                mBuffer;      // ARGB8888 (top-down)
	int                  mWidth, mHeight;
	bool                 mDirty;
	GLTexture*           mTexture;
	iTVPGLVideoPresenterHost* mHost;
	bool                 mActive;

	// YUV(I420) 経路 (SupportsYUV=true。generic movie は I420 native)
	std::vector<uint8_t> mYUV;         // packed I420 (Y=w×h + U/V=cw×ch)
	int                  mYUVW, mYUVH;
	bool                 mIsYUV;       // 最新フレームが YUV か (false=ARGB mBuffer)
	GLYUVDrawer          mYUVDrawer;

	// mixer 追加画像 (動画の上へ α 合成。setMixingLayer の後継)
	std::vector<char>    mMixer;       // BGRA (top-down) packed
	int                  mMixerW, mMixerH;
	tTVPRect             mMixerRect;    // プライマリレイヤ座標
	float                mMixerAlpha;
	bool                 mMixerValid;
	bool                 mMixerDirty;   // GLTexture へ未反映か (α 変更含む)
	GLTexture*           mMixerTex;

public:
	tTVPGLVideoOverlayPresenter()
		: mBuffer(nullptr), mWidth(0), mHeight(0), mDirty(false)
		, mTexture(nullptr), mHost(nullptr), mActive(false)
		, mYUVW(0), mYUVH(0), mIsYUV(false)
		, mMixerW(0), mMixerH(0), mMixerAlpha(1.0f), mMixerValid(false), mMixerDirty(false)
		, mMixerTex(nullptr)
	{ mMixerRect.left = mMixerRect.top = mMixerRect.right = mMixerRect.bottom = 0; }

	~tTVPGLVideoOverlayPresenter() override
	{
		Deactivate();
		FreeAll();   // Close 経路 (GL context current) から来る想定
	}

	//--- iTVPVideoOverlayPresenter (generic VideoOverlay 側、デコードスレッド) ---
	void UpdateFrame(int w, int h, std::function<void(char *dest, int pitch)> updater) override
	{
		std::lock_guard<std::mutex> lock(mMutex);
		if (!mBuffer || mWidth != w || mHeight != h) {
			delete[] mBuffer;
			mBuffer = new char[(size_t)w * h * 4];
			mWidth = w;
			mHeight = h;
		}
		updater(mBuffer, w * 4);
		mIsYUV = false;
		mDirty = true;
	}

	// GL presenter は YUV(I420)→RGB を自前シェーダで描けるので YUV plane 直渡しに対応。
	bool SupportsYUV() const override { return true; }

	void UpdateFrameYUV(const iTVPMoviePlayer::VideoPlaneFrame &frame) override
	{
		// I420 のみ対応 (generic movie は I420 native)。NV12 等が来たら無視 (ARGB 経路が使われる)。
		if (frame.format != iTVPMoviePlayer::VPF_I420 || frame.planeCount < 3) return;
		int w = frame.width, h = frame.height;
		if (w <= 0 || h <= 0) return;
		int cw = (w+1)/2, ch = (h+1)/2;
		std::lock_guard<std::mutex> lock(mMutex);
		mYUV.resize((size_t)w*h + 2*(size_t)cw*ch);
		uint8_t *d = mYUV.data();
		CopyPlane(d, frame.planes[0], w, h);
		CopyPlane(d + (size_t)w*h, frame.planes[1], cw, ch);
		CopyPlane(d + (size_t)w*h + (size_t)cw*ch, frame.planes[2], cw, ch);
		mYUVW = w; mYUVH = h;
		mIsYUV = true; mDirty = true;
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
		char *d = mMixer.data();
		const char *s = (const char*)bgra;
		int rb = w * 4;
		for (int y = 0; y < h; ++y)
			memcpy(d + (size_t)y * rb, s + (ptrdiff_t)y * pitch, rb);
		mMixerW = w; mMixerH = h; mMixerRect = primaryRect; mMixerAlpha = alpha;
		mMixerValid = true; mMixerDirty = true;
	}
	void SetMixerAlpha(float alpha) override
	{
		std::lock_guard<std::mutex> lock(mMutex);
		mMixerAlpha = alpha; mMixerDirty = true;   // α はテクスチャへ焼くので再アップロード
	}
	void ClearMixerImage() override
	{
		std::lock_guard<std::mutex> lock(mMutex);
		mMixerValid = false;
		// GLTexture の破棄は GL context 必須 → 次の RenderVideoFrame/FreeAll で行う
		mMixerDirty = false;
		std::vector<char>().swap(mMixer);
	}

	bool Bind(const tTJSVariant &drawDeviceObj) override
	{
		mHost = nullptr;
		if (drawDeviceObj.Type() != tvtObject) return false;
		tTJSVariantClosure clo = drawDeviceObj.AsObjectClosureNoAddRef();
		if (clo.Object == nullptr) return false;
		tTJSVariant val;
		if (TJS_FAILED(clo.PropGet(0, TJS_W("glVideoPresenterHost"), nullptr, &val, nullptr)))
			return false;
		tjs_int64 p = (tjs_int64)val;
		if (p == 0) return false;
		mHost = reinterpret_cast<iTVPGLVideoPresenterHost*>((intptr_t)p);
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
	//--- iTVPGLVideoPresenter (OGLDrawDevice 側、描画スレッド、GL context current) ---
	bool TJS_INTF_METHOD RenderVideoFrame(const tTVPGLVideoPresenterContext &ctx) override
	{
		std::lock_guard<std::mutex> lock(mMutex);
		if (!ctx.TextureDrawer) return false;

		if (mIsYUV) {
			// YUV(I420) 経路: 自前シェーダで 3 プレーンから GPU で YUV→RGB (CPU 変換なし)。
			if (mYUV.empty() || mYUVW <= 0 || mYUVH <= 0) return false;
			GLfloat pos[8];
			ComputeFitPosition(pos, mYUVW, mYUVH, ctx.TargetWidth, ctx.TargetHeight);
			int cw = (mYUVW+1)/2, ch = (mYUVH+1)/2;
			const uint8_t *Y = mYUV.data();
			const uint8_t *U = Y + (size_t)mYUVW * mYUVH;
			const uint8_t *V = U + (size_t)cw * ch;
			mYUVDrawer.Draw(Y, U, V, mYUVW, mYUVH, ctx.TargetWidth, ctx.TargetHeight, pos);
		} else {
			// ARGB 経路 (movie backend が YUV plane を供給しない場合)
			if (!mBuffer || mWidth <= 0 || mHeight <= 0) return false;
			if (!mTexture || (int)mTexture->width() != mWidth || (int)mTexture->height() != mHeight) {
				delete mTexture;
				mTexture = new GLTexture(mWidth, mHeight);
				mDirty = true;
			}
			if (mDirty && mTexture) {
				char* src = mBuffer;
				int spitch = mWidth * 4;
				int w = mWidth, h = mHeight;
				mTexture->UpdateTexture(0, 0, w, h, [src, spitch, w, h](char *Dest, int pitch) {
					if (pitch == spitch) {
						memcpy(Dest, src, (size_t)pitch * h);
					} else {
						for (int y = 0; y < h; ++y)
							memcpy(Dest + (size_t)y * pitch, src + (size_t)y * spitch, spitch);
					}
				});
				mDirty = false;
			}
			if (!mTexture) return false;
			// 描画先へ内接 (letterbox) 配置・中央寄せ
			GLfloat pos[8];
			ComputeFitPosition(pos, mWidth, mHeight, ctx.TargetWidth, ctx.TargetHeight);
			ctx.TextureDrawer->DrawTexture(mTexture, ctx.TargetWidth, ctx.TargetHeight, pos);
		}

		// mixer 追加画像を動画の上へ α 合成
		DrawMixer(ctx);
		return true;
	}

	static void CopyPlane(uint8_t *dst, const iTVPMoviePlayer::VideoPlaneFrame::PlaneRef &src, int w, int h)
	{
		if (!src.data) return;
		for (int y = 0; y < h; ++y)
			memcpy(dst + (size_t)y * w, src.data + (size_t)y * src.stride, w);
	}

	//! mixer 画像を動画の上へ描く。プライマリレイヤ座標 mMixerRect を ctx.DestRect へマップ。
	void DrawMixer(const tTVPGLVideoPresenterContext &ctx)
	{
		if (!mMixerValid || mMixerW <= 0 || mMixerH <= 0) {
			// クリア要求で残っている GLTexture を破棄 (GL context current)
			if (!mMixerValid && mMixerTex) { delete mMixerTex; mMixerTex = nullptr; }
			return;
		}
		if (mMixerTex && ((int)mMixerTex->width() != mMixerW || (int)mMixerTex->height() != mMixerH)) {
			delete mMixerTex; mMixerTex = nullptr;
		}
		if (!mMixerTex) { mMixerTex = new GLTexture(mMixerW, mMixerH); mMixerDirty = true; }
		if (mMixerDirty && mMixerTex) {
			// BGRA を α = pixelA × mMixerAlpha で焼いてアップロード (DrawTexture blend=straight-alpha)
			char* src = mMixer.data();
			int spitch = mMixerW * 4, w = mMixerW, h = mMixerH;
			float ga = mMixerAlpha; if (ga < 0) ga = 0; if (ga > 1) ga = 1;
			mMixerTex->UpdateTexture(0, 0, w, h, [src, spitch, w, h, ga](char *Dest, int pitch) {
				for (int y = 0; y < h; ++y) {
					const unsigned char* sp = (const unsigned char*)(src + (size_t)y * spitch);
					unsigned char* dp = (unsigned char*)(Dest + (size_t)y * pitch);
					for (int x = 0; x < w; ++x) {
						dp[x*4+0] = sp[x*4+0]; dp[x*4+1] = sp[x*4+1]; dp[x*4+2] = sp[x*4+2];
						dp[x*4+3] = (unsigned char)(sp[x*4+3] * ga + 0.5f);
					}
				}
			});
			mMixerDirty = false;
		}
		if (!mMixerTex) return;

		// プライマリ座標 → 描画先 (DestRect, サーフェス px) → clip 座標 (ComputeFitPosition と同規約)
		int srcW = ctx.SrcWidth  > 0 ? ctx.SrcWidth  : mMixerW;
		int srcH = ctx.SrcHeight > 0 ? ctx.SrcHeight : mMixerH;
		double dw = (double)ctx.DestRect.get_width()  / srcW;
		double dh = (double)ctx.DestRect.get_height() / srcH;
		double px0 = ctx.DestRect.left + mMixerRect.left * dw;
		double py0 = ctx.DestRect.top  + mMixerRect.top  * dh;
		double px1 = ctx.DestRect.left + mMixerRect.right  * dw;
		double py1 = ctx.DestRect.top  + mMixerRect.bottom * dh;
		int sw = ctx.TargetWidth, sh = ctx.TargetHeight;
		if (sw <= 0 || sh <= 0) return;
		float l = (float)(px0 * 2.0 / sw - 1.0);
		float r = (float)(px1 * 2.0 / sw - 1.0);
		// GL clip は +Y が上。mMixer は top-down (行0=視覚 top)。DrawTexture の UV 規約に合わせ
		// 上端行を高い clipY へ出すため頂点の t/b を入替 (これで箱位置は同じ・テクスチャ上下正立)。
		float t = (float)(1.0 - py0 * 2.0 / sh);  // 上端 (py0) → 高い clipY
		float b = (float)(1.0 - py1 * 2.0 / sh);  // 下端 (py1) → 低い clipY
		GLfloat mpos[8] = { l, b,  l, t,  r, b,  r, t };
		ctx.TextureDrawer->DrawTexture(mMixerTex, ctx.TargetWidth, ctx.TargetHeight, mpos, 0, 0, /*blend=*/true);
	}

private:
	void FreeAll()
	{
		delete mTexture;  mTexture = nullptr;   // GLTexture dtor は glDeleteTextures (context 必須)
		delete[] mBuffer; mBuffer = nullptr;
		mWidth = mHeight = 0;
		mDirty = false;
		// YUV 経路も破棄 (GL リソースは context current 経路から解放)
		mYUVDrawer.Done();
		std::vector<uint8_t>().swap(mYUV);
		mYUVW = mYUVH = 0; mIsYUV = false;
		// mixer も破棄 (GLTexture dtor は GL context 必須。FreeAll は context current 経路から呼ばれる)
		delete mMixerTex; mMixerTex = nullptr;
		mMixerValid = false; mMixerDirty = false; mMixerW = mMixerH = 0;
		std::vector<char>().swap(mMixer);
	}

	static void ComputeFitPosition(GLfloat pos[8], int w, int h, int sw, int sh)
	{
		if (sw <= 0 || sh <= 0) {
			pos[0] = -1.0f; pos[1] = -1.0f; pos[2] = -1.0f; pos[3] = 1.0f;
			pos[4] =  1.0f; pos[5] = -1.0f; pos[6] =  1.0f; pos[7] = 1.0f;
			return;
		}
		double scale = std::min((double)sw / w, (double)sh / h);
		int nw = (int)(w * scale);
		int nh = (int)(h * scale);
		int offx = (sw - nw) / 2;
		int offy = (sh - nh) / 2;
		int w2 = sw / 2;
		int h2 = sh / 2;
		float left   = (float)(offx      - w2) / w2;
		float top    = (float)(offy      - h2) / h2;
		float right  = (float)(offx + nw - w2) / w2;
		float bottom = (float)(offy + nh - h2) / h2;
		pos[0] = left;  pos[1] = top;      // left top
		pos[2] = left;  pos[3] = bottom;   // left bottom
		pos[4] = right; pos[5] = top;      // right top
		pos[6] = right; pos[7] = bottom;   // right bottom
	}
};

iTVPVideoOverlayPresenter * CreateGLVideoOverlayPresenter()
{
	return new tTVPGLVideoOverlayPresenter();
}

} // anonymous

void TVPRegisterGLVideoOverlayPresenterFactory()
{
	TVPRegisterVideoOverlayPresenterFactory(&CreateGLVideoOverlayPresenter);
}

#endif // __GENERIC__
