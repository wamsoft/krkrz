#pragma once

// SDL3 ビルド限定: メモリオーバレイの SDL_Renderer 描画関数。
// tTVPSDLDrawDevice::Show() の Render lambda 末尾 (SDL_RenderTexture 直後)
// で呼び出す。OFF 状態では即 return するので常時呼んでよい。
//
// renderer == nullptr のときは描画をスキップし、計測値の更新と log 出力だけを
// 行う log-only モードになる (SDLOGLDrawDevice 等の SDL_Renderer 不在経路から
// 計測のため呼ぶ場合に使う)。

struct SDL_Renderer;

void TVPRenderMemoryOverlay(SDL_Renderer *renderer);

// OpenGL ES 直接版は common/visual/opengl/MemoryOverlayGL.h に移動済。
// (SDLOGLDrawDevice / OGLDrawDevice 両方から共通利用)
