//---------------------------------------------------------------------------
/*
	TVP2 ( T Visual Presenter 2 )  A script authoring tool
	Copyright (C) 2000 W.Dee <dee@kikyou.info> and contributors

	See details of license at "license.txt"
*/
//---------------------------------------------------------------------------
// "tTVPFont" definition
//---------------------------------------------------------------------------

#ifndef __TVPFONTSTRUC_H__
#define __TVPFONTSTRUC_H__

#include "tjsCommHead.h"

//---------------------------------------------------------------------------
// 絵文字レンダリングモード (Font.emojiMode / グローバル既定)
//   絵文字コードポイントを、原フォントに無ければ絵文字フォントへフォールバック
//   させるかを制御する。指定した絵文字フォントが未登録 (リソース未収納等) の
//   場合はスルー (原フォントのまま = none 相当) する。
//---------------------------------------------------------------------------
#define TVP_EMOJI_DEFAULT (-1)  // グローバル既定 (Font.defaultEmojiMode) に従う
#define TVP_EMOJI_NONE    0     // 絵文字フォントを使わない (原フォントのみ)
#define TVP_EMOJI_MONO    1     // モノクロ絵文字フォントをフォールバックに使う
#define TVP_EMOJI_COLOR   2     // カラー絵文字フォントをフォールバックに使う

// 異体字セレクタ (VS15/VS16) による文字単位の絵文字表示指定。
// テキスト中で基底文字の直後に付くと、その1文字の表示を明示的に切り替える。
// (自動判定 = Emoji_Presentation 既定は行わない。明示指定時のみ)
#define TVP_EMOJI_PRESENTATION_DEFAULT 0  // セレクタ無し (emojiMode に従う)
#define TVP_EMOJI_PRESENTATION_EMOJI   1  // VS16 (U+FE0F): 絵文字フォントを優先
#define TVP_EMOJI_PRESENTATION_TEXT    2  // VS15 (U+FE0E): 原フォント (テキスト) を強制
#define TVP_EMOJI_VS15 0xFE0E
#define TVP_EMOJI_VS16 0xFE0F

//---------------------------------------------------------------------------
// tTVPFont definition
//---------------------------------------------------------------------------
struct tTVPFont
{
	tjs_int Height; // height of text
	tjs_uint32 Flags;
	tjs_int Angle; // rotation angle ( in tenths of degrees ) 0 .. 1800 .. 3600

	ttstr Face; // font name

	tjs_int EmojiMode = TVP_EMOJI_DEFAULT; // 絵文字モード (TVP_EMOJI_*)

	bool operator == (const tTVPFont & rhs) const
	{
		return Height == rhs.Height &&
			Flags == rhs.Flags &&
			Angle == rhs.Angle &&
			Face == rhs.Face &&
			EmojiMode == rhs.EmojiMode;
	}
	bool operator != (const tTVPFont & rhs) const {
		return !(operator==(rhs));
	}
};


/*[*/
//---------------------------------------------------------------------------
// font ralated constants
//---------------------------------------------------------------------------
#define TVP_TF_ITALIC    0x0100
#define TVP_TF_BOLD      0x0200
#define TVP_TF_UNDERLINE 0x0400
#define TVP_TF_STRIKEOUT 0x0800
#define TVP_TF_FONTFILE  0x1000


//---------------------------------------------------------------------------
#define TVP_FSF_FIXEDPITCH    0x01      // fsfFixedPitch
#define TVP_FSF_SAMECHARSET   0x02      // fsfSameCharSet
#define TVP_FSF_NOVERTICAL    0x04      // fsfNoVertical
#define TVP_FSF_TRUETYPEONLY  0x08      // fsfTrueTypeOnly
#define TVP_FSF_IGNORESYMBOL  0x10      // fsfIgnoreSymbol
#define TVP_FSF_USEFONTFACE   0x100  // fsfUseFontFace

/*]*/

//---------------------------------------------------------------------------
#endif
