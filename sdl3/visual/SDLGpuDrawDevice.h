//---------------------------------------------------------------------------
// SDLGpuDrawDevice: SDLDrawDevice に Canvas (GPU 版) を載せた描画デバイス
//
// TJS からは OGLDrawDevice と同じ使い方 (onInit / onDraw / onDone、createCanvas、
// canvas / texture プロパティ) ができる。GL 版が無いビルドでは Window.OGLDrawDevice
// としても登録する (既存スクリプトがそのまま動くように)。
//
// SDL_Renderer は Canvas と同じ GPU デバイスで作る (SDL_CreateGPURenderer)。
// 毎フレーム: レイヤー画像を texture (GPU) へ転送 → onDraw で Canvas が画面用の
// GPU テクスチャへ描いて submit → SDL_Renderer がそれを画面へ描き、ダイアログ等を
// 重ねて present。
//---------------------------------------------------------------------------
#ifndef SDLGpuDrawDeviceH
#define SDLGpuDrawDeviceH

#include "SDLDrawDevice.h"
#include "EventIntf.h"   // tTVPRepeatedExceptionGuard
#include "GpuDevice.h"
#include <memory>

class tTVPSDLGpuDrawDevice : public tTVPSDLDrawDevice
{
	typedef tTVPSDLDrawDevice inherited;

	iTJSDispatch2* SelfObj = nullptr;
	tTJSVariant CanvasObject;
	class tTJSNI_GpuCanvas* CanvasInstance = nullptr;
	tTJSVariant TextureObject;      // レイヤー画像 (Texture)
	class tTJSNI_GpuTexture* TextureInstance = nullptr;
	std::unique_ptr<TVPGpu::Texture> ScreenTex;   // Canvas の画面用の描画先
	SDL_Texture* ScreenWrap = nullptr;            // それを SDL_Renderer から描くための包み
	int ScreenW = 0, ScreenH = 0;
	bool DoCreateCanvas = false;
	tTVPRepeatedExceptionGuard OnDrawExceptionGuard;
	bool OnDrawDisabled = false;

	void CreateCanvas();
	void DestroyCanvas();
	void SetCanvasObject( const tTJSVariant& val );
	void CreateLayerTexture();
	void DestroyLayerTexture();
	void EnsureScreen( int w, int h );
	void DestroyScreen();
	void FireEvent( const tjs_char* name, bool immediate = true );

protected:
	~tTVPSDLGpuDrawDevice() override;
	SDL_Renderer* CreateSDLRenderer( SDL_Window* sdl_wnd, const char* requested ) override;
	void DestroySDLRenderer( SDL_Renderer* renderer ) override;
	void OnRendererCreated() override;
	void OnRendererDestroying() override;
	void RenderContent( SDL_Renderer* renderer, const SDL_FRect& dst ) override;

public:
	tTVPSDLGpuDrawDevice( iTJSDispatch2* tjs_obj );

	void RequestCreateCanvas();
	void ResumeOnDraw() { OnDrawDisabled = false; OnDrawExceptionGuard.Reset(); }
	const tTJSVariant& GetCanvasObject() const { return CanvasObject; }
	const tTJSVariant& GetTextureObject() const { return TextureObject; }

	void TJS_INTF_METHOD Destruct() override;
	void TJS_INTF_METHOD NotifyLayerResize( iTVPLayerManager* manager ) override;
	void TJS_INTF_METHOD Show() override;
	void TJS_INTF_METHOD NotifyBitmapCompleted( iTVPLayerManager* manager,
		tjs_int x, tjs_int y, const void* bits, const BITMAPINFO* bitmapinfo,
		const tTVPRect& cliprect, tTVPLayerType type, tjs_int opacity ) override;
};

//---------------------------------------------------------------------------
class tTJSNI_SDLGpuDrawDevice : public tTJSNativeInstance
{
	tTVPSDLGpuDrawDevice* Device = nullptr;
public:
	~tTJSNI_SDLGpuDrawDevice() override;
	tjs_error TJS_INTF_METHOD Construct( tjs_int numparams, tTJSVariant** param, iTJSDispatch2* tjs_obj ) override;
	void TJS_INTF_METHOD Invalidate() override;
	tTVPSDLGpuDrawDevice* GetDevice() const { return Device; }
};

class tTJSNC_SDLGpuDrawDevice : public tTJSNativeClass
{
public:
	//! name: TJS のクラス名 ("GPUDrawDevice"。GL 版が無いビルドでは "OGLDrawDevice" でも作る)
	tTJSNC_SDLGpuDrawDevice( const tjs_char* name = TJS_W("GPUDrawDevice") );
	static tjs_uint32 ClassID;
private:
	iTJSNativeInstance* CreateNativeInstance() override { return new tTJSNI_SDLGpuDrawDevice(); }
};

#endif
