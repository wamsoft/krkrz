//---------------------------------------------------------------------------
// Canvas (GPU 版) の SDL_GPU バックエンド: SDL 側から使う補助
//---------------------------------------------------------------------------
#ifndef GpuDeviceSDLH
#define GpuDeviceSDLH

#include "GpuDevice.h"
#include <SDL3/SDL.h>

namespace TVPGpu {
//! GPU テクスチャを SDL_Renderer のテクスチャとして包むときの SDL のピクセル形式
SDL_PixelFormat GetSDLPixelFormat( Texture* tex );
}

#endif
