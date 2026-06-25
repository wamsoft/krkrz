#pragma once

// パッドオーバレイの OpenGL ES 直接版描画。
//
// SDL_Renderer 経由 (sdl3/visual/PadOverlayRender.h の TVPRenderPadOverlay) に
// 対し、OGL 直接 + 自前 8x8 bitmap font + shader/VBO で実装。SDL 依存はなく、
// SDLOGLDrawDevice (sdl3/) と OGLDrawDevice (common/visual/opengl/) のどちらの
// Show 末尾からも呼べる。
//
// 呼び出し前提: 描画対象の GL context が current、glViewport が画面サイズに
// 設定されている。OFF (TVPPadOverlay::IsEnabled()==false) のとき即 return。
//
// font / shader / VBO / texture は lazy init (初回呼出時に確保)、複数回の
// 呼出で同じ resource を再利用する。

void TVPRenderPadOverlayGL();
