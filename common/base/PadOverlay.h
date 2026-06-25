#pragma once

// ゲームパッド状態リアルタイム監視オーバレイ用の ON/OFF 保持。
// SDL3 ビルド限定で画面左上に 16 ボタンのマトリクスを表示する。
// 描画は sdl3/visual/PadOverlayRender.cpp (SDL_Renderer 経路) と
// common/visual/opengl/PadOverlayGL.cpp (OpenGL ES 直接経路) が
// それぞれ DrawDevice の Show 末尾から呼び出す。
//
// memoverlay と異なりサンプラ thread は持たない。描画関数が呼ばれた
// タイミングで SDL_Gamepad の現在状態を直接読みに行く。

namespace TVPPadOverlay {

void SetEnabled(bool enabled);
bool IsEnabled();

} // namespace TVPPadOverlay

// CLI -padoverlay=1 を読んで起動時から ON にする (SDL3 build のみ実描画、
// WINVER は flag だけ立つ)。SysInitImpl から呼ばれる。
void TVPInitializePadOverlay();
