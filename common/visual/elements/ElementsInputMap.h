//---------------------------------------------------------------------------
//!@file krkrz ネイティブ入力型 → cycfi (elements) 中立入力型への変換
//
// 吉里吉里側は SDL / WINVER どちらの build でも **Windows VK コード /
// tTVPMouseButton / TVP_SS_* フラグ**で入力を受ける。 elements 側は
// host 非依存の中立型 (`mouse_button::what` / `key_code` / `pad_button` +
// `mod_*` の OR) で受ける。 その対応表だけをここに置く。
//
// SDL や Win32 のネイティブ enum は経由しない (どちらの build でも同じ表)。
//
// 利用側:
//   - ElementsDialogManager  … overlay ダイアログの Forward* 系
//   - ElementsLayerPanel     … ホストのレイヤに描くパネル (座標変換なしで
//                              レイヤのマウスハンドラから直に流す)
//
// ヘッダオンリー (inline)。 cycfi のヘッダを引くので、 elements を使う
// 翻訳単位からのみ include すること。
//---------------------------------------------------------------------------
#ifndef ELEMENTS_INPUT_MAP_H
#define ELEMENTS_INPUT_MAP_H

#include "tjsCommHead.h"
#include "tvpinputdefs.h"   // tTVPMouseButton / TVP_SS_*

// VK_* の定義。 Win32 では windows.h が持っているので、 非 Win32 だけ
// エンジン同梱の対応表を引く (ElementsDialogManager.cpp と同じ扱い)。
#ifndef _WIN32
#include "VirtualKey.h"
#endif

#include <elements/base_view.hpp>        // mouse_button / key_code / mod_*
#include <elements/element/gamepad.hpp>  // pad_button

namespace tvp_elements_input {

namespace ce = cycfi::elements;

//---------------------------------------------------------------------------
//! @brief tTVPMouseButton → cycfi のボタン種別。
inline ce::mouse_button::what MouseButtonToElements(tTVPMouseButton mb)
{
	switch (mb) {
		case mbMiddle: return ce::mouse_button::middle;
		case mbRight:  return ce::mouse_button::right;
		default:       return ce::mouse_button::left;   // mbLeft ほか
	}
}

//---------------------------------------------------------------------------
//! @brief TVP_SS_* シフト状態 → cycfi の修飾キービット (OR)。
inline int FlagsToElementsMods(tjs_uint32 flags)
{
	int mods = 0;
	if (flags & TVP_SS_SHIFT) mods |= ce::mod_shift;
	if (flags & TVP_SS_CTRL)  mods |= ce::mod_control;
	if (flags & TVP_SS_ALT)   mods |= ce::mod_alt;
	return mods;
}

//---------------------------------------------------------------------------
//! @brief マウスボタン → VK コード。
//!
//! ホストホットキーの表はキー / パッド / マウスを **同じ VK 空間**で扱うので、
//! 照合のためにマウスボタンも VK へ寄せる。
inline tjs_uint MouseButtonToVk(tTVPMouseButton mb)
{
	switch (mb) {
		case mbRight:  return VK_RBUTTON;
		case mbMiddle: return VK_MBUTTON;
		case mbX1:     return VK_XBUTTON1;
		case mbX2:     return VK_XBUTTON2;
		default:       return VK_LBUTTON;   // mbLeft ほか
	}
}

//---------------------------------------------------------------------------
//! @brief Windows VK code の振り分け先 (キー / パッドボタン / 対応なし)。
struct vk_routing
{
	enum class kind { none, key, pad_button };
	kind           k          = kind::none;
	ce::key_code   key        = ce::key_code::unknown;
	int            extra_mods = 0;
	ce::pad_button pad        = ce::pad_button::unknown;
};

//---------------------------------------------------------------------------
//! @brief Windows VK code → cycfi 中立入力型 (key_code | pad_button) 振り分け。
inline vk_routing RouteVk(tjs_uint vk)
{
	using K  = vk_routing::kind;
	using kc = ce::key_code;
	using pb = ce::pad_button;
	auto as_key = [](kc c, int m = 0) { return vk_routing{K::key, c, m, pb::unknown}; };
	auto as_pad = [](pb b)            { return vk_routing{K::pad_button, kc::unknown, 0, b}; };

	switch (vk) {
		case VK_RETURN: return as_key(kc::enter);
		case VK_TAB:    return as_key(kc::tab);
		case VK_ESCAPE: return as_key(kc::escape);
		case VK_BACK:   return as_key(kc::backspace);
		case VK_DELETE: return as_key(kc::_delete);
		case VK_INSERT: return as_key(kc::insert);
		case VK_HOME:   return as_key(kc::home);
		case VK_END:    return as_key(kc::end);
		case VK_PRIOR:  return as_key(kc::page_up);
		case VK_NEXT:   return as_key(kc::page_down);
		case VK_SPACE:  return as_key(kc::space);
		case VK_LEFT:   return as_key(kc::left);
		case VK_UP:     return as_key(kc::up);
		case VK_RIGHT:  return as_key(kc::right);
		case VK_DOWN:   return as_key(kc::down);

		case 0x1C0: return as_pad(pb::a);          // VK_PAD1  (A)
		case 0x1C1: return as_pad(pb::b);          // VK_PAD2  (B)
		case 0x1C2: return as_pad(pb::x);          // VK_PAD3  (X)
		case 0x1C3: return as_pad(pb::y);          // VK_PAD4  (Y)
		case 0x1C4: return as_pad(pb::lb);         // VK_PAD5  (LB)
		case 0x1C5: return as_pad(pb::rb);         // VK_PAD6  (RB)
		// トリガ (Switch の ZL/ZR、 PS5 の L2/R2)。 elements 側は
		// lt_click / rt_click ("l2" / "r2") で受ける。
		case 0x1C6: return as_pad(pb::lt_click);   // VK_PAD7  (LT/ZL/L2)
		case 0x1C7: return as_pad(pb::rt_click);   // VK_PAD8  (RT/ZR/R2)
		case 0x1C8: return as_pad(pb::back);       // VK_PAD9  (Back)
		case 0x1C9: return as_pad(pb::start);      // VK_PAD10 (Start)
		case 0x1CA: return as_pad(pb::l3);         // VK_PAD11 (L3)
		case 0x1CB: return as_pad(pb::r3);         // VK_PAD12 (R3)

		// 位置基準のフェイスボタン (刻印ではなく配置)。 同じ物理ボタンが
		// VK_PAD1..4 も一緒に飛ばしてくるので、 割り当てる側で使い分ける。
		case 0x1D4: return as_pad(pb::face_south); // VK_PAD_FACE_SOUTH (下)
		case 0x1D5: return as_pad(pb::face_east);  // VK_PAD_FACE_EAST  (右)
		case 0x1D6: return as_pad(pb::face_west);  // VK_PAD_FACE_WEST  (左)
		case 0x1D7: return as_pad(pb::face_north); // VK_PAD_FACE_NORTH (上)

		case 0x1B5: case 0x1CC: case 0x1D0:
			return as_pad(pb::dpad_left);
		case 0x1B6: case 0x1CD: case 0x1D1:
			return as_pad(pb::dpad_up);
		case 0x1B7: case 0x1CE: case 0x1D2:
			return as_pad(pb::dpad_right);
		case 0x1B8: case 0x1CF: case 0x1D3:
			return as_pad(pb::dpad_down);

		case 0x1B9: return as_pad(pb::a);

		default:
			// 数字 (VK_0..9 = 0x30..0x39) / 英字 (VK_A..Z = 0x41..0x5A) は
			// cycfi key_code が大文字 ASCII 準拠なのでそのまま通す。
			if (vk >= '0' && vk <= '9') return as_key(static_cast<kc>(vk));
			if (vk >= 'A' && vk <= 'Z') return as_key(static_cast<kc>(vk));
			return vk_routing{};
	}
}

} // namespace tvp_elements_input

#endif
