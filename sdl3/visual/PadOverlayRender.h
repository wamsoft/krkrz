#pragma once

// SDL3 ビルド限定: ゲームパッドオーバレイの SDL_Renderer 描画関数。
// tTVPSDLDrawDevice::Show() の Render lambda 末尾で呼び出す。
// OFF 状態では即 return するので常時呼んでよい。
//
// OpenGL ES 直接版は common/visual/opengl/PadOverlayGL.h に分離している。

struct SDL_Renderer;

void TVPRenderPadOverlay(SDL_Renderer *renderer);
