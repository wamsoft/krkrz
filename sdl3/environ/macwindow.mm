//---------------------------------------------------------------------------
// macOS (Cocoa) 専用のウィンドウ補助
//
// SDL3 (3.4) の Cocoa バックエンドは SDL_GetWindowBordersSize を実装しておらず、
// 常に失敗する (枠 0)。 吉里吉里の Window.width / height (装飾込みの外形) が
// タイトルバーを含まなくなるので、 NSWindow のフレームと内容領域の差から求める。
//---------------------------------------------------------------------------
#import <AppKit/AppKit.h>
#include <SDL3/SDL.h>
#include <cmath>

// 装飾の厚み (SDL のウィンドウ座標 = ポイント)。 取れなければ false
bool TVPCocoaGetWindowBordersSize(SDL_Window *window, int *top, int *left, int *bottom, int *right)
{
	if (!window) return false;
	NSWindow *nswindow = (__bridge NSWindow *)SDL_GetPointerProperty(
		SDL_GetWindowProperties(window), SDL_PROP_WINDOW_COCOA_WINDOW_POINTER, NULL);
	if (!nswindow) return false;
	// フルスクリーン中は装飾が無い (タイトルバーは隠れている)
	if ((nswindow.styleMask & NSWindowStyleMaskFullScreen) != 0) {
		*top = *left = *bottom = *right = 0;
		return true;
	}
	const NSRect frame = nswindow.frame;
	const NSRect content = [nswindow contentRectForFrameRect:frame];
	// AppKit の座標は左下原点 (y 上向き)
	*left   = (int)std::lround(NSMinX(content) - NSMinX(frame));
	*right  = (int)std::lround(NSMaxX(frame) - NSMaxX(content));
	*bottom = (int)std::lround(NSMinY(content) - NSMinY(frame));
	*top    = (int)std::lround(NSMaxY(frame) - NSMaxY(content));
	return true;
}
