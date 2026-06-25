#pragma once

#include "tvpinputdefs.h"

// メッセージ窓ボタン表示
enum {
	mrOk,
	mrAbort,
	mrCancel,
};

// メッセージ窓種別
enum {
    mtCustom = 0,
    mtWarning,
    mtError,
    mtInformation,
    mtConfirmation,
    mtStop,
};

// メッセージ窓ボタン
enum {
	mbOK = 1,
};

// アプリ制御イベント
enum {
	// 描画領域変更
	AM_SURFACE_CHANGED,
	AM_SURFACE_CREATED,
	AM_SURFACE_DESTORYED,

	// レイヤ自体のDrawDeviceへの全再転送
	AM_SURFACE_PAINT_REQUEST,

    // 画面回転
	AM_DISPLAY_ROTATE,
	AM_DISPLAY_RESIZE,

	// 操作イベント
	AM_TOUCH_DOWN,
	AM_TOUCH_MOVE,
	AM_TOUCH_UP,
	AM_KEY_DOWN,
	AM_KEY_UP,
	AM_MOUSE_DOWN,
	AM_MOUSE_MOVE,
	AM_MOUSE_UP,
	AM_MOUSE_WHEEL,

	// 処理開始
	AM_STARTUP_SCRIPT,

	AM_RESUME,
	AM_PAUSE,

	// 画面更新要求
	AM_REQUEST_UPDATE
};


class FormEventHandler {
public:
	virtual void SendMessage( tjs_int message, tjs_int64 wparam, tjs_int64 lparam ) = 0;
	virtual void SendTouchMessage( tjs_int type, float x, float y, float c, int id, tjs_uint64 tick ) = 0;
	virtual void SendMouseMessage( tjs_int type, int button, int shift, int x, int y) = 0;
};
