#pragma once

// memoverlay の OpenGL ES 直接版描画。
//
// SDL_Renderer 経由 (sdl3/visual/MemoryOverlayRender.h の TVPRenderMemoryOverlay)
// に対し、OGL 直接 + 自前 8x8 bitmap font + shader/VBO で実装。SDL 依存はなく、
// SDLOGLDrawDevice (sdl3/) と OGLDrawDevice (common/visual/opengl/) のどちらの
// Show 末尾からも呼べる。
//
// 呼び出し前提: 描画対象の GL context が current、glViewport が画面サイズに
// 設定されている。
// memoverlay OFF (TVPMemoryOverlay::IsEnabled()==false) のとき即 return。
//
// font / shader / VBO / texture は lazy init (初回呼出時に確保)、複数回の呼出で
// 同じ resource を再利用する。

void TVPRenderMemoryOverlayGL();
