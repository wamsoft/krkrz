//---------------------------------------------------------------------------
/*
	TVP2 ( T Visual Presenter 2 )  A script authoring tool
	Copyright (C) 2000 W.Dee <dee@kikyou.info> and contributors

	See details of license at "license.txt"
*/
//---------------------------------------------------------------------------
// input related definition
//---------------------------------------------------------------------------


#ifndef __TVPINPUTDEFS_H__
#define __TVPINPUTDEFS_H__

/*[*/
//---------------------------------------------------------------------------
// mouse button
//---------------------------------------------------------------------------
enum tTVPMouseButton
{
	mbLeft,
	mbRight,
	mbMiddle,
	mbX1,
	mbX2
};


//---------------------------------------------------------------------------
// Pointer (pointing device type)
//---------------------------------------------------------------------------
enum class tTVPPointerType : int {
	ptUnknown = 0,
	ptMouseLeft = 1,
	ptMouseRight = 2,
	ptMouseMiddle = 3,
	ptMouseX1 = 4,
	ptMouseX2 = 5,
	ptMouse = 6,
	ptTouch = 7,
	ptPen = 8
};


//---------------------------------------------------------------------------
// IME modes : comes from VCL's TImeMode
//---------------------------------------------------------------------------
enum tTVPImeMode
{
	imDisable,
	imClose,
	imOpen,
	imDontCare,
	imSAlpha,
	imAlpha,
	imHira,
	imSKata,
	imKata,
	imChinese,
	imSHanguel,
	imHanguel
};


//---------------------------------------------------------------------------
// shift state
//---------------------------------------------------------------------------
#define TVP_SS_SHIFT   0x01
#define TVP_SS_ALT     0x02
#define TVP_SS_CTRL    0x04
#define TVP_SS_LEFT    0x08
#define TVP_SS_RIGHT   0x10
#define TVP_SS_MIDDLE  0x20
#define TVP_SS_DOUBLE  0x40
#define TVP_SS_REPEAT  0x80
#define TVP_SS_X1      0x100
#define TVP_SS_X2      0x200


inline bool TVPIsAnyMouseButtonPressedInShiftStateFlags(tjs_uint32 state)
{ return (state & 
	(TVP_SS_LEFT | TVP_SS_RIGHT | TVP_SS_MIDDLE | TVP_SS_DOUBLE | TVP_SS_X1 | TVP_SS_X2)) != 0; }



//---------------------------------------------------------------------------
// JoyPad virtual key codes
//---------------------------------------------------------------------------
// These VKs are KIRIKIRI specific. Not widely used.
#define VK_PAD_FIRST	0x1B0 // first PAD related key code
#define VK_PADLEFT		0x1B5
#define VK_PADUP		0x1B6
#define VK_PADRIGHT		0x1B7
#define VK_PADDOWN		0x1B8
#define	VK_PADCENTER    0x1B9
//0
#define VK_PAD1			0x1C0 // A
#define VK_PAD2			0x1C1 // B
#define VK_PAD3			0x1C2 // X
#define VK_PAD4			0x1C3 // Y
#define VK_PAD5			0x1C4 // L
#define VK_PAD6			0x1C5 // R
#define VK_PAD7			0x1C6 // L2
#define VK_PAD8			0x1C7 // R2
// 8
#define VK_PAD9			0x1C8 // SELECT
#define VK_PAD10		0x1C9 // START
#define VK_PAD11		0x1CA // L3
#define VK_PAD12		0x1CB // R3
#define VK_PAD_L_LEFT	0x1CC // analog left pad emulation
#define VK_PAD_L_UP		0x1CD 
#define VK_PAD_L_RIGHT	0x1CE
#define VK_PAD_L_DOWN	0x1CF
// 16
#define VK_PAD_R_LEFT	0x1D0 // analog right pad emulation
#define VK_PAD_R_UP		0x1D1
#define VK_PAD_R_RIGHT	0x1D2
#define VK_PAD_R_DOWN	0x1D3
// 20
#define VK_PADANY		0x1DF   // returns whether any one of pad buttons are pressed,
							    // in System.getKeyState
#define VK_PAD_LAST		0x1DF   // last PAD related key code

//---------------------------------------------------------------------------
/*]*/
//---------------------------------------------------------------------------

#endif



