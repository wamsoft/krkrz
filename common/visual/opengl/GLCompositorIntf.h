/**
 * GLCompositor クラス
 *
 * OGLDrawDevice を使わない (= 画面描画デバイスが D3D11/SDL_Renderer 等の) 状況でも、
 * 裏で GLES によるオフスクリーン合成を行い、その結果を Layer へ書き戻すためのクラス。
 * 旧 krkrgles プラグインの GLESAdaptor 相当を、本体内蔵かつ Canvas / Offscreen の
 * 既存 GL コードを共有する形で提供する。
 *
 * 描画デバイスに依存せず、ウィンドウから iTVPGLContext を取得して自前で MakeCurrent する。
 * OGL 描画デバイスが動作中の場合は (WINVER は HWND 単位でキャッシュ+refcount される
 * tTVPEGLContext を) 共有し、そうでない場合 (WINVER 既定の D3D11 等) はオフスクリーン用に
 * コンテキストを生成する。いずれも present (Swap) はせず FBO へのオフスクリーン描画のみ。
 *
 * capture() のコールバックには内部 Canvas が渡され、Canvas のフル API
 * (drawTexture / beginEffect / endEffect / clip 等) がそのまま利用できる。
 * ※ Layer を直接受け取る drawLayer / copyLayer の便利メソッドは別途追加予定。
 */
#ifndef GLCompositorIntfH
#define GLCompositorIntfH

#include "tjsNative.h"

class iTVPGLContext;
class tTJSNI_Canvas;
class tTJSNI_Offscreen;
class tTJSNI_Texture;
class tTJSNI_Matrix32;
class tTJSNI_Bitmap;
class tTJSNI_BaseLayer;

class tTJSNI_GLCompositor : public tTJSNativeInstance
{
	void* NativeWindow;
	iTVPGLContext* Context;

	// 内部 Canvas (描画 API の実体はこれに委譲する)
	tTJSVariant CanvasObject;
	tTJSNI_Canvas* CanvasInstance;
	tTJSNI_Matrix32* CanvasMatrixInstance;

	// レンダーターゲット (オフスクリーン FBO)
	tTJSVariant TargetObject;
	tTJSNI_Offscreen* TargetInstance;
	int TargetWidth;
	int TargetHeight;

	// drawLayer 用: レイヤ画像を受ける Bitmap (CopyFromMainImage で安価に転送)
	tTJSVariant BitmapObject;
	tTJSNI_Bitmap* BitmapInstance;

	int Width;
	int Height;

	// capture の読み戻しで premultiplied-alpha を straight-alpha に戻すか。
	// MSAA 等で縁が premultiplied になる GL 描画 (3D/VRM 立ち絵等) を、吉里吉里の
	// straight-alpha レイヤへ ltAlpha 合成する際の縁の白フリンジを防ぐ。既定 false。
	// 旧 GLESAdaptor.unpremultiply 相当。
	bool Unpremultiply;

	// 自分自身の TJS オブジェクト (Construct の tjs_obj)。callback 呼び出し時に
	// objthis として渡し、コールバックを「incontextof このオブジェクト」で実行させる。
	// ネイティブインスタンスは この TJS オブジェクトに内包されるため、参照循環を避けて
	// 非 AddRef の back-pointer として保持する (GLESAdaptor の objthis 保持と同じ)。
	iTJSDispatch2* Owner;

	void CreateCanvas();
	void EnsureTarget(int w, int h);
	void EnsureBitmap();

public:
	tTJSNI_GLCompositor();
	~tTJSNI_GLCompositor() override;
	tjs_error TJS_INTF_METHOD Construct(tjs_int numparams, tTJSVariant **param, iTJSDispatch2 *tjs_obj) override;
	void TJS_INTF_METHOD Invalidate() override;

	// このコンポジタの GL コンテキストをカレントにする
	void MakeCurrent();

	// オフスクリーン合成の既定サイズ
	void SetScreenSize(int width, int height);
	int GetScreenWidth() const { return Width; }
	int GetScreenHeight() const { return Height; }

	// capture 読み戻し時の un-premultiply の有無 (旧 GLESAdaptor.unpremultiply 相当)
	bool GetUnpremultiply() const { return Unpremultiply; }
	void SetUnpremultiply(bool b) { Unpremultiply = b; }

	// 内部 Canvas を公開 (drawTexture / beginEffect / clip 等はこの canvas 経由で利用可)
	const tTJSVariant& GetCanvasObject() const { return CanvasObject; }
	tTJSNI_Canvas* GetCanvasInstance() const { return CanvasInstance; }

	// layer のサイズのオフスクリーン FBO をクリア色でクリアし、
	// callback(w, h, param) を「incontextof このコンポジタ」で呼んで描画させ、
	// その結果を layer のメインイメージへ読み戻す。コールバック内では this が
	// このコンポジタになるので、this.canvas / this.drawLayer 等が直接使える
	// (GLESAdaptor 互換)。
	void Capture(tTJSNI_BaseLayer* layer, const tTJSVariant& callback, const tTJSVariant& param, tjs_uint32 color);

	// layer をアフィン変換 (a,b,c,d,tx,ty) + 不透明度で現在の描画先へ描く。
	// capture のコールバック内から呼ぶ (BeginDrawing 済みが前提)。
	void DrawLayer(tTJSNI_BaseLayer* layer, float a, float b, float c, float d, float tx, float ty, int opacity);
	// layer を (left,top) にそのまま描く (drawLayer の単純平行移動版)。
	void CopyLayer(tTJSNI_BaseLayer* layer, int left, int top);
};


//---------------------------------------------------------------------------
// tTJSNC_GLCompositor : TJS GLCompositor class
//---------------------------------------------------------------------------
class tTJSNC_GLCompositor : public tTJSNativeClass
{
public:
	tTJSNC_GLCompositor();
	static tjs_uint32 ClassID;

protected:
	tTJSNativeInstance *CreateNativeInstance() override { return new tTJSNI_GLCompositor(); }
};

extern tTJSNativeClass * TVPCreateNativeClass_GLCompositor();
#endif
