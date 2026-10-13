//---------------------------------------------------------------------------
// GLCompositor の GPU 版 (SDL_GPU)
//
// TJS から見たクラス名・API は GL 版 (common/visual/opengl/GLCompositorIntf.h) と同じ。
// 描画デバイスに依らず、共有の GPU デバイスの上で中間テクスチャに Canvas で描き、
// 結果を Layer のメインイメージへ読み戻す。GL 版と違ってコンテキストは無いので
// makeCurrent は何もせず、GLGetProcAddress は 0 を返す。
//---------------------------------------------------------------------------
#ifndef GpuCompositorIntfH
#define GpuCompositorIntfH

#include "tjsNative.h"
#include "GpuDevice.h"
#include <memory>

class tTJSNI_GpuCanvas;
class tTJSNI_Matrix32;
class tTJSNI_Bitmap;
class tTJSNI_BaseLayer;

class tTJSNI_GpuCompositor : public tTJSNativeInstance
{
	// 内部 Canvas (描画 API の実体はこれに委譲する)
	tTJSVariant CanvasObject;
	tTJSNI_GpuCanvas* CanvasInstance = nullptr;
	tTJSNI_Matrix32* CanvasMatrixInstance = nullptr;

	// 描画先 (capture の対象レイヤーと同じ大きさ)
	std::unique_ptr<TVPGpu::Texture> Target;

	// drawLayer 用: レイヤー画像を受ける Bitmap
	tTJSVariant BitmapObject;
	tTJSNI_Bitmap* BitmapInstance = nullptr;

	int Width = 32;
	int Height = 32;
	bool Unpremultiply = false;

	// 自分自身の TJS オブジェクト (callback の objthis。非 AddRef)
	iTJSDispatch2* Owner = nullptr;

	void CreateCanvas();
	void EnsureTarget( int w, int h );
	void EnsureBitmap();

public:
	tjs_error TJS_INTF_METHOD Construct( tjs_int numparams, tTJSVariant** param, iTJSDispatch2* tjs_obj ) override;
	void TJS_INTF_METHOD Invalidate() override;

	void SetScreenSize( int width, int height ) { Width = width; Height = height; }
	int GetScreenWidth() const { return Width; }
	int GetScreenHeight() const { return Height; }
	bool GetUnpremultiply() const { return Unpremultiply; }
	void SetUnpremultiply( bool b ) { Unpremultiply = b; }
	const tTJSVariant& GetCanvasObject() const { return CanvasObject; }
	tTJSNI_GpuCanvas* GetCanvasInstance() const { return CanvasInstance; }

	//! layer の大きさの描画先をクリアし、callback(w, h, param) を「incontextof このコンポジタ」で
	//! 呼んで描かせ、結果を layer のメインイメージへ読み戻す
	void Capture( tTJSNI_BaseLayer* layer, const tTJSVariant& callback, const tTJSVariant& param, tjs_uint32 color );
	//! layer をアフィン変換 (a,b,c,d,tx,ty) + 不透明度で現在の描画先へ描く (capture のコールバック内から呼ぶ)
	void DrawLayer( tTJSNI_BaseLayer* layer, float a, float b, float c, float d, float tx, float ty, int opacity );
	void CopyLayer( tTJSNI_BaseLayer* layer, int left, int top );
};

class tTJSNC_GpuCompositor : public tTJSNativeClass
{
public:
	tTJSNC_GpuCompositor();
	static tjs_uint32 ClassID;
protected:
	tTJSNativeInstance* CreateNativeInstance() override { return new tTJSNI_GpuCompositor(); }
};

extern tTJSNativeClass* TVPCreateNativeClass_GpuCompositor();

#endif
