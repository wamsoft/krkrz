#include "tjsCommHead.h"
#include "PadOverlayRender.h"
#include "PadOverlay.h"
#include "Application.h"
#include "CharacterSet.h"

#include <SDL3/SDL.h>
#include <cstdio>
#include <string>

namespace {

// 表示レイアウト (kOverlayScale 倍で SDL_SetRenderScale 経由で描画)
constexpr float kOverlayScale = 1.5f;
constexpr int kPanelMargin = 8;
constexpr int kPanelPad    = 6;
constexpr int kHeaderH     = 12;
constexpr int kCellW       = 32;
constexpr int kCellH       = 16;
constexpr int kCellGap     = 4;
constexpr int kMatrixW     = 4 * kCellW + 3 * kCellGap;   // 140
constexpr int kMatrixH     = 4 * kCellH + 3 * kCellGap;   // 76
// 軸表示: 3 行 (LX/LY, RX/RY, LT/RT)、各 12 px (font 8 + 行間 4)
constexpr int kAxisLineH   = 12;
constexpr int kAxesRows    = 3;
constexpr int kAxesH       = kAxisLineH * kAxesRows;      // 36
constexpr int kPanelW      = kMatrixW + kPanelPad * 2;    // 152
constexpr int kPanelH      = kHeaderH + 4 + kMatrixH + 4 + kAxesH + kPanelPad * 2; // 144

// SDL_RenderDebugText は 8x8 ASCII font。kButtonMap 同等の bit 位置に対応する
// 2 文字ラベル。bit 6,7 = L2/R2 (トリガ)、bit 12-15 = DPAD (Lf/Up/Rt/Dn)。
const char *kLabels[16] = {
	"A",  "B",  "X",  "Y",
	"L1", "R1", "L2", "R2",
	"BK", "ST", "LS", "RS",
	"Lf", "Up", "Rt", "Dn",
};

} // anonymous namespace

void TVPRenderPadOverlay(SDL_Renderer *renderer)
{
	if (!TVPPadOverlay::IsEnabled()) return;
	if (!renderer) return;

	int win_w = 0, win_h = 0;
	SDL_GetCurrentRenderOutputSize(renderer, &win_w, &win_h);
	const int scaled_win_w = (int)((float)win_w / kOverlayScale);
	const int scaled_win_h = (int)((float)win_h / kOverlayScale);
	if (scaled_win_w < kPanelW + kPanelMargin * 2) return;
	if (scaled_win_h < kPanelH + kPanelMargin * 2) return;

	// Application 経由で取得 (= GetPadState の処理済み bit を表示)
	const bool has_pad = Application ? Application->HasJoypad(0) : false;
	const tjs_uint32 state = (Application && has_pad) ? Application->GetPadState(0) : 0;

	// 軸 6 種 (有効パッドなし時は 0.0 で埋まる)
	float axes[6] = {0, 0, 0, 0, 0, 0};
	if (has_pad && Application) {
		for (int i = 0; i < 6; ++i) {
			axes[i] = Application->GetPadAxis(0, i);
		}
	}

	float orig_scale_x = 1.0f, orig_scale_y = 1.0f;
	SDL_GetRenderScale(renderer, &orig_scale_x, &orig_scale_y);
	SDL_SetRenderScale(renderer, kOverlayScale, kOverlayScale);

	// memoverlay (右上) と被らないよう左上配置
	const int px = kPanelMargin;
	const int py = kPanelMargin;

	// 半透明背景 + 枠
	SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_BLEND);
	SDL_SetRenderDrawColor(renderer, 0, 0, 0, 180);
	SDL_FRect bg{(float)px, (float)py, (float)kPanelW, (float)kPanelH};
	SDL_RenderFillRect(renderer, &bg);
	SDL_SetRenderDrawColor(renderer, 96, 96, 96, 255);
	SDL_RenderRect(renderer, &bg);

	// ヘッダ: "Pad: <name>" or "Pad: (none)"
	char header[80];
	if (has_pad && Application) {
		std::string name_u8;
		TVPUtf16ToUtf8(name_u8, Application->GetJoypadType(0));
		std::snprintf(header, sizeof(header), "Pad: %s",
			!name_u8.empty() ? name_u8.c_str() : "(unknown)");
	} else {
		std::snprintf(header, sizeof(header), "Pad: (none)");
	}
	SDL_SetRenderDrawColor(renderer, 255, 255, 255, 255);
	SDL_RenderDebugText(renderer,
		(float)(px + kPanelPad),
		(float)(py + kPanelPad),
		header);

	// 4x4 ボタンマトリクス
	const int mx = px + kPanelPad;
	const int my = py + kPanelPad + kHeaderH + 4;
	for (int i = 0; i < 16; ++i) {
		const int col = i % 4;
		const int row = i / 4;
		const int cx = mx + col * (kCellW + kCellGap);
		const int cy = my + row * (kCellH + kCellGap);
		const bool on = ((state >> i) & 1) != 0;

		if (on) {
			SDL_SetRenderDrawColor(renderer, 80, 200, 80, 220);
		} else {
			SDL_SetRenderDrawColor(renderer, 48, 48, 48, 200);
		}
		SDL_FRect cell{(float)cx, (float)cy, (float)kCellW, (float)kCellH};
		SDL_RenderFillRect(renderer, &cell);
		SDL_SetRenderDrawColor(renderer, 128, 128, 128, 255);
		SDL_RenderRect(renderer, &cell);

		const char *label = kLabels[i];
		int label_len = 0;
		for (const char *p = label; *p; ++p) ++label_len;
		const int label_px_w = label_len * 8;
		const float tx = (float)(cx + (kCellW - label_px_w) / 2);
		const float ty = (float)(cy + (kCellH - 8) / 2);
		if (on) {
			SDL_SetRenderDrawColor(renderer, 0, 0, 0, 255);
		} else {
			SDL_SetRenderDrawColor(renderer, 200, 200, 200, 255);
		}
		SDL_RenderDebugText(renderer, tx, ty, label);
	}

	// 軸値: ボタンマトリクス直下に 3 行 × 2 列
	// "LX +0.45 LY -0.32" 形 (符号付き 5 char、幅 17 char = 136 px、マトリクス内)
	const char *axis_labels[6] = {"LX", "LY", "RX", "RY", "LT", "RT"};
	const int ax = mx;
	const int ay0 = my + kMatrixH + 4;
	SDL_SetRenderDrawColor(renderer, has_pad ? 220 : 128, has_pad ? 220 : 128, has_pad ? 220 : 128, 255);
	for (int row = 0; row < kAxesRows; ++row) {
		const int aL = row * 2;
		const int aR = row * 2 + 1;
		char line[40];
		std::snprintf(line, sizeof(line), "%s %+.2f %s %+.2f",
			axis_labels[aL], axes[aL],
			axis_labels[aR], axes[aR]);
		SDL_RenderDebugText(renderer,
			(float)ax,
			(float)(ay0 + row * kAxisLineH),
			line);
	}

	SDL_SetRenderScale(renderer, orig_scale_x, orig_scale_y);
}
