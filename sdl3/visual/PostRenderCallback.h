#pragma once

// プラグインから登録可能な post-render コールバック。
// KrkrZ のメイン描画 (テクスチャ転送 + 既存オーバレイ) が終わり、画面提示 (Swap /
// RenderPresent) の直前で呼ばれる。SDL3 系の 2 つのレンダリングパスそれぞれに
// 別の登録 API を用意してある。
//
//   - SDL_Renderer 経路 (SDLDrawDevice / KRKRZ_USE_OPENGL=OFF 時):
//       TVPRegisterPostRenderCallback / TVPDispatchPostRenderCallbacks
//   - OpenGL ES 直接経路 (SDLOGLDrawDevice / KRKRZ_USE_OPENGL=ON 時):
//       TVPRegisterPostRenderCallbackGL / TVPDispatchPostRenderCallbacksGL
//
// 両方を実装するプラグインは両方に登録すれば、どちらのビルドでも描画される。
// 呼び出しは描画スレッド (= TJS スレッド) で同期。登録/解除も同スレッドで行うこと
// (スレッドセーフではない)。

struct SDL_Renderer;

// ---- SDL_Renderer 経路 ----

typedef void (*tTVPPostRenderCallback)(SDL_Renderer *renderer, void *userdata);

// 同じ (cb, userdata) ペアの二重登録は無視される。
void TVPRegisterPostRenderCallback(tTVPPostRenderCallback cb, void *userdata);
// 未登録ペアでも no-op。
void TVPUnregisterPostRenderCallback(tTVPPostRenderCallback cb, void *userdata);
// 内部: SDLDrawDevice::Show() から呼ばれる。
void TVPDispatchPostRenderCallbacks(SDL_Renderer *renderer);

// ---- OpenGL ES 直接経路 ----
//
// 呼び出し時点で GL context が current、glViewport は画面サイズに設定済み。
// プラグインは glGetIntegerv(GL_VIEWPORT, ...) で viewport を取得できる。

typedef void (*tTVPPostRenderCallbackGL)(void *userdata);

void TVPRegisterPostRenderCallbackGL(tTVPPostRenderCallbackGL cb, void *userdata);
void TVPUnregisterPostRenderCallbackGL(tTVPPostRenderCallbackGL cb, void *userdata);
// 内部: SDLOGLDrawDevice::Show() / OGLDrawDevice::Show() から呼ばれる。
void TVPDispatchPostRenderCallbacksGL();
