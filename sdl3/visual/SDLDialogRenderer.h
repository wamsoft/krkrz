//---------------------------------------------------------------------------
//!@file SDL3 用 Elements ダイアログ描画アダプタ
//---------------------------------------------------------------------------
#ifndef SDL_DIALOG_RENDERER_H
#define SDL_DIALOG_RENDERER_H

#include "elements/DialogRenderer.h"
#include <SDL3/SDL.h>
#include <vector>
#include <map>
#include <cstdint>

class tTVPSDLDrawDevice;

//! @brief tTVPSDLDrawDevice 上に Elements の SW レンダ結果をオーバーレイするアダプタ
//!
//! Elements canvas は連続 (pitch == w*4) なバッファ前提だが、SDL_Texture の
//! pitch はドライバ依存で w*4 と一致しないことがあるため、中間ステージング
//! バッファを 1 枚保持し:
//!  - AcquireBuffer() でステージングバッファ (w*h*4) を返す
//!  - ReleaseBuffer() で SDL_UpdateTexture により texture へコピー
//!  - PresentOverlay() で SDL_RenderTexture を実行
class tTVPSDLDialogRenderer : public iTVPDialogRenderer
{
public:
	explicit tTVPSDLDialogRenderer(tTVPSDLDrawDevice * device);
	~tTVPSDLDialogRenderer() override;

	void GetSurfaceSize(int& w, int& h) override;
	void GetDestRect(int& x, int& y, int& w, int& h) override;
	uint32_t * AcquireBuffer(const void* layer, int w, int h) override;
	void ReleaseBuffer(const void* layer) override;
	void ReleaseBufferRect(const void* layer, int x, int y, int w, int h) override;
	void PresentOverlay(const void* layer, int x, int y, int w, int h) override;
	void ReleaseLayer(const void* layer) override;

private:
	tTVPSDLDrawDevice * _device;     //!< 借用 (所有しない)

	//! @brief overlay インスタンス (layer) ごとのテクスチャ + ステージング。
	struct Layer
	{
		SDL_Texture * texture = nullptr;
		std::vector<std::uint32_t> staging;
		int w = 0;
		int h = 0;
	};
	std::map<const void*, Layer> _layers;

	void DestroyLayer(Layer& layer);
};

#endif
