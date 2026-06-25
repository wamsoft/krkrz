//---------------------------------------------------------------------------
//!@file OpenGL ES 用 Elements ダイアログ描画アダプタ
//!
//! tTVPOGLDrawDevice / tTVPSDLOGLDrawDevice の両方で共有される、GL 直接描画版
//! の iTVPDialogRenderer 実装。ステージング CPU バッファ → GLTexture (PBO 経由
//! UpdateTexture) → GLTextureDrawer による straight-alpha 合成描画 という流れ。
//!
//! Host DrawDevice は iTVPGLDialogHost を実装し、GL context / TextureDrawer /
//! DestRect の借用 accessor を提供する。
//---------------------------------------------------------------------------
#ifndef OGL_DIALOG_RENDERER_H
#define OGL_DIALOG_RENDERER_H

#include "elements/DialogRenderer.h"
#include <cstdint>
#include <vector>
#include <map>

class iTVPGLContext;
class GLTexture;
class GLTextureDrawer;

//! @brief OGLDialogRenderer がホスト DrawDevice から GL リソース / DestRect を
//!        借用するための小さな抽象。tTVPOGLDrawDevice と tTVPSDLOGLDrawDevice
//!        がこれを実装する (多重継承)。
class iTVPGLDialogHost
{
public:
	virtual ~iTVPGLDialogHost() = default;

	//! 現在の GL context。InitContext 前 / DoneContext 後は nullptr を返してよい。
	virtual iTVPGLContext * DialogHost_GetGLContext() = 0;

	//! DrawDevice 所有の textured-quad drawer。MakeCurrent 後にのみ呼ぶ。
	virtual GLTextureDrawer * DialogHost_GetTextureDrawer() = 0;

	//! DestRect (= ゲーム画像が描画される画面領域、surface pixel 座標)。
	//! ダイアログの中央配置とマウス座標変換の基準。未確定なら 0,0,0,0 を返す。
	virtual void DialogHost_GetDestRect(int & x, int & y, int & w, int & h) = 0;
};

//! @brief OGL 直接版 (SDLOGL / OGL DrawDevice 共有) の dialog overlay レンダラ。
//!
//!  - AcquireBuffer() : CPU 連続バッファ (w*h*4, 連続 pitch) を返す。
//!  - ReleaseBuffer() : staging → GLTexture へ PBO 経由でアップロード。
//!  - PresentOverlay(): DestRect 同等の pixel 座標で textured quad を blend 描画。
//!
//! ライフサイクル: host DrawDevice が InitContext 時に登録、DoneContext 時に
//! 解除する。Renderer の destructor は GL context が生存していれば MakeCurrent
//! して GLTexture を破棄、生存していなければ glDeleteTextures を呼ばずに
//! handle をリークさせる (UB 回避)。
class tTVPOGLDialogRenderer : public iTVPDialogRenderer
{
public:
	explicit tTVPOGLDialogRenderer(iTVPGLDialogHost * host);
	~tTVPOGLDialogRenderer() override;

	void GetSurfaceSize(int & w, int & h) override;
	void GetDestRect(int & x, int & y, int & w, int & h) override;
	std::uint32_t * AcquireBuffer(const void* layer, int w, int h) override;
	void ReleaseBuffer(const void* layer) override;
	void PresentOverlay(const void* layer, int x, int y, int w, int h) override;
	void ReleaseLayer(const void* layer) override;

private:
	iTVPGLDialogHost * _host;       //!< 借用 (所有しない)

	//! @brief overlay インスタンス (layer) ごとの GLTexture + ステージング。
	struct Layer
	{
		GLTexture * texture = nullptr;  //!< 表示用 (GL handle 所有)
		std::vector<std::uint32_t> staging;
		int w = 0;
		int h = 0;
		bool dirty = false;             //!< staging に書込済みで未アップロード
	};
	std::map<const void*, Layer> _layers;

	//! layer の GLTexture を破棄 (context 生存時のみ glDeleteTextures)。
	void DestroyLayer(Layer& layer);
};

#endif // OGL_DIALOG_RENDERER_H
