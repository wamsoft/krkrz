
#ifndef __TVP_SCREEN_H__
#define __TVP_SCREEN_H__

class tTVPScreen {
public:
	tTVPScreen();
	static int GetWidth();
	static int GetHeight();
	static void GetDesktopRect( RECT& r );
	static int GetDesktopLeft();
	static int GetDesktopTop();
	static int GetDesktopWidth();
	static int GetDesktopHeight();
};
extern void TVPDumpDisplayDevices();

/**
 * -display= で指定されたディスプレイ上へウィンドウを移動する。
 *
 * 別のディスプレイに乗っている場合は、そのディスプレイの作業領域原点からの
 * 相対位置を保ったまま移動する。 いずれの場合も、はみ出しは指定ディスプレイの
 * 作業領域内へ寄せる。 -display= 未指定時は何もしない。
 */
extern void TVPMoveWindowToStartupDisplay( HWND hWnd );

#endif // __TVP_SCREEN_H__
