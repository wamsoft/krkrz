#include "tjsCommHead.h"

#if !defined(_WIN32)
#include "VirtualKey.h"
#endif

#include "app.h"

// SDL_Keycode から仮想キーコードへの変換テーブル
static const tjs_uint16 VK_TRANSLATE_TABLE[] = {
    0,                  // 0
    0,                  // 1
    0,                  // 2
    0,                  // 3
    0,                  // 4
    0,                  // 5
    0,                  // 6
    0,                  // 7
    VK_BACK,            // SDLK_BACKSPACE (8)
    VK_TAB,             // SDLK_TAB (9)
    0,                  // 10
    0,                  // 11
    VK_CLEAR,           // SDLK_CLEAR (12)
    VK_RETURN,          // SDLK_RETURN (13)
    0,                  // 14
    0,                  // 15
    0,                  // 16
    0,                  // 17
    0,                  // 18
    0,                  // 19
    0,                  // 20
    0,                  // 21
    0,                  // 22
    0,                  // 23
    0,                  // 24
    0,                  // 25
    0,                  // 26
    VK_ESCAPE,          // SDLK_ESCAPE (27)
    0,                  // 28
    0,                  // 29
    0,                  // 30
    0,                  // 31
    VK_SPACE,           // SDLK_SPACE (32)
    0,                  // SDLK_EXCLAIM
    0,                  // SDLK_DBLAPOSTROPHE
    0,                  // SDLK_HASH
    0,                  // SDLK_DOLLAR
    0,                  // SDLK_PERCENT
    0,                  // SDLK_AMPERSAND
    VK_OEM_7,           // SDLK_APOSTROPHE
    0,                  // SDLK_LEFTPAREN
    0,                  // SDLK_RIGHTPAREN
    0,                  // SDLK_ASTERISK
    VK_OEM_PLUS,        // SDLK_PLUS
    VK_OEM_COMMA,       // SDLK_COMMA (44)
    VK_OEM_MINUS,       // SDLK_MINUS (45)
    VK_OEM_PERIOD,      // SDLK_PERIOD (46)
    VK_OEM_2,           // SDLK_SLASH (47)
    '0',                // SDLK_0 (48)
    '1',                // SDLK_1 (49)
    '2',                // SDLK_2 (50)
    '3',                // SDLK_3 (51)
    '4',                // SDLK_4 (52)
    '5',                // SDLK_5 (53)
    '6',                // SDLK_6 (54)
    '7',                // SDLK_7 (55)
    '8',                // SDLK_8 (56)
    '9',                // SDLK_9 (57)
    VK_OEM_1,           // SDLK_COLON (58)
    VK_OEM_1,           // SDLK_SEMICOLON (59)
    0,                  // SDLK_LESS (60)
    VK_OEM_PLUS,        // SDLK_EQUALS (61)
    0,                  // SDLK_GREATER (62)
    0,                  // SDLK_QUESTION (63)
    0,                  // SDLK_AT (64)
    0,                  // 65
    0,                  // 66
    0,                  // 67
    0,                  // 68
    0,                  // 69
    0,                  // 70
    0,                  // 71
    0,                  // 72
    0,                  // 73
    0,                  // 74
    0,                  // 75
    0,                  // 76
    0,                  // 77
    0,                  // 78
    0,                  // 79
    0,                  // 80
    0,                  // 81
    0,                  // 82
    0,                  // 83
    0,                  // 84
    0,                  // 85
    0,                  // 86
    0,                  // 87
    0,                  // 88
    0,                  // 89
    0,                  // 90
    VK_OEM_4,           // SDLK_LEFTBRACKET (91)
    VK_OEM_2,           // SDLK_BACKSLASH (92)
    VK_OEM_6,           // SDLK_RIGHTBRACKET (93)
    0,                  // SDLK_CARET (94)
    0,                  // SDLK_UNDERSCORE (95)   
    0,                  // SDLK_GRAVE (96)       
    'A',                // SDLK_a (97)
    'B',                // SDLK_b (98)
    'C',                // SDLK_c (99)
    'D',                // SDLK_d (100)
    'E',                // SDLK_e (101)
    'F',                // SDLK_f (102)
    'G',                // SDLK_g (103)
    'H',                // SDLK_h (104)
    'I',                // SDLK_i (105)
    'J',                // SDLK_j (106)
    'K',                // SDLK_k (107)
    'L',                // SDLK_l (108)
    'M',                // SDLK_m (109)
    'N',                // SDLK_n (110)
    'O',                // SDLK_o (111)
    'P',                // SDLK_p (112)
    'Q',                // SDLK_q (113)
    'R',                // SDLK_r (114)
    'S',                // SDLK_s (115)
    'T',                // SDLK_t (116)
    'U',                // SDLK_u (117)
    'V',                // SDLK_v (118)
    'W',                // SDLK_w (119)
    'X',                // SDLK_x (120)
    'Y',                // SDLK_y (121)
    'Z',                // SDLK_z (122)
    VK_OEM_4,           // SDLK_LEFTBRACE (123)
    VK_OEM_5,           // SDLK_PIPE (124)
    VK_OEM_6,           // SDLK_RIGHTBRACE (125)
    VK_OEM_3,           // SDLK_TILDE (126)
    VK_DELETE,          // SDLK_DELETE (127)
    0,                  // SDLK_PLUSMINUS (128)
};

// 特殊キーの変換テーブル (SDLK_F1 から始まる連番のキー)
static const tjs_uint16 VK_SPECIAL_KEYS[] = {
    VK_F1,              // SDLK_F1
    VK_F2,              // SDLK_F2
    VK_F3,              // SDLK_F3
    VK_F4,              // SDLK_F4
    VK_F5,              // SDLK_F5
    VK_F6,              // SDLK_F6
    VK_F7,              // SDLK_F7
    VK_F8,              // SDLK_F8
    VK_F9,              // SDLK_F9
    VK_F10,             // SDLK_F10
    VK_F11,             // SDLK_F11
    VK_F12,             // SDLK_F12
    VK_F13,             // SDLK_F13
    VK_F14,             // SDLK_F14
    VK_F15,             // SDLK_F15
    VK_F16,             // SDLK_F16
    VK_F17,             // SDLK_F17
    VK_F18,             // SDLK_F18
    VK_F19,             // SDLK_F19
    VK_F20,             // SDLK_F20
    VK_F21,             // SDLK_F21
    VK_F22,             // SDLK_F22
    VK_F23,             // SDLK_F23
    VK_F24,             // SDLK_F24
};

static int VK_TRANSLATE_TABLE_REV[256];

// SDL_Keycode から仮想キーコードへの変換関数
tjs_uint16 TVPTransSDLKeyToVirtualKey(tjs_int sdlKey) 
{
    // 標準的なASCII範囲のキー
    if (sdlKey >= 0 && sdlKey < sizeof(VK_TRANSLATE_TABLE) / sizeof(tjs_uint16)) {
        return VK_TRANSLATE_TABLE[sdlKey];
    }
    
    // 特殊キー
    switch (sdlKey) {
        case SDLK_DELETE:      return VK_DELETE;
        case SDLK_INSERT:      return VK_INSERT;
        case SDLK_HOME:        return VK_HOME;
        case SDLK_END:         return VK_END;
        case SDLK_PAGEUP:      return VK_PRIOR;
        case SDLK_PAGEDOWN:    return VK_NEXT;
        case SDLK_RIGHT:       return VK_RIGHT;
        case SDLK_LEFT:        return VK_LEFT;
        case SDLK_DOWN:        return VK_DOWN;
        case SDLK_UP:          return VK_UP;
        case SDLK_NUMLOCKCLEAR: return VK_NUMLOCK;
        case SDLK_CAPSLOCK:    return VK_CAPITAL;
        case SDLK_SCROLLLOCK:  return VK_SCROLL;
        case SDLK_RSHIFT:      return VK_RSHIFT;
        case SDLK_LSHIFT:      return VK_LSHIFT;
        case SDLK_RCTRL:       return VK_RCONTROL;
        case SDLK_LCTRL:       return VK_LCONTROL;
        case SDLK_RALT:        return VK_RMENU;
        case SDLK_LALT:        return VK_LMENU;
        case SDLK_RGUI:        return VK_RWIN;
        case SDLK_LGUI:        return VK_LWIN;
        case SDLK_PRINTSCREEN: return VK_PRINT;
        case SDLK_APPLICATION: return VK_APPS;
    }
    
    // ファンクションキー
    //
    // ※SDL の SDLK_F13..F24 は F1..F12 と連番になっていない。 間には
    //   PrintScreen / カーソルキー / **テンキー一式** が挟まる:
    //     F1..F12   = 0x4000003A..0x40000045
    //     (この間)  = PrintScreen 〜 KP_EQUALS (0x40000046..0x40000067)
    //     F13..F24  = 0x40000068..0x40000073
    //   これを F1..F24 の一括範囲で判定すると、 テンキーまで巻き込んだうえ
    //   24 要素しかない VK_SPECIAL_KEYS を範囲外参照して不定値を返す
    //   (テンキー Enter が Return にならない等)。 2 つに分けて見る。
    if (sdlKey >= SDLK_F1 && sdlKey <= SDLK_F12) {
        return VK_SPECIAL_KEYS[sdlKey - SDLK_F1];
    }
    if (sdlKey >= SDLK_F13 && sdlKey <= SDLK_F24) {
        return VK_SPECIAL_KEYS[12 + (sdlKey - SDLK_F13)];
    }

    // テンキー
    if (sdlKey >= SDLK_KP_1 && sdlKey <= SDLK_KP_9) {
        return VK_NUMPAD1 + (sdlKey - SDLK_KP_1);
    }
    
    switch (sdlKey) {
        case SDLK_KP_0:        return VK_NUMPAD0;
        case SDLK_KP_PERIOD:   return VK_DECIMAL;
        case SDLK_KP_DIVIDE:   return VK_DIVIDE;
        case SDLK_KP_MULTIPLY: return VK_MULTIPLY;
        case SDLK_KP_MINUS:    return VK_SUBTRACT;
        case SDLK_KP_PLUS:     return VK_ADD;
        case SDLK_KP_ENTER:    return VK_RETURN;
        case SDLK_KP_EQUALS:   return VK_OEM_PLUS;
    }
    
    return 0;
}

static void InitKey()
{
    memset(VK_TRANSLATE_TABLE_REV, 0, sizeof(VK_TRANSLATE_TABLE_REV));
    
    // 通常のASCIIキーの逆マッピングを設定
    for (int i = 0; i < sizeof(VK_TRANSLATE_TABLE) / sizeof(tjs_uint16); i++) {
        tjs_uint16 code = VK_TRANSLATE_TABLE[i];
        if (code != 0 && code < 256) {
            VK_TRANSLATE_TABLE_REV[code] = i;
        }
    }
    
    // 特殊キーの逆マッピングを設定
    VK_TRANSLATE_TABLE_REV[VK_DELETE] = SDLK_DELETE;
    VK_TRANSLATE_TABLE_REV[VK_INSERT] = SDLK_INSERT;
    VK_TRANSLATE_TABLE_REV[VK_HOME] = SDLK_HOME;
    VK_TRANSLATE_TABLE_REV[VK_END] = SDLK_END;
    VK_TRANSLATE_TABLE_REV[VK_PRIOR] = SDLK_PAGEUP;
    VK_TRANSLATE_TABLE_REV[VK_NEXT] = SDLK_PAGEDOWN;
    VK_TRANSLATE_TABLE_REV[VK_RIGHT] = SDLK_RIGHT;
    VK_TRANSLATE_TABLE_REV[VK_LEFT] = SDLK_LEFT;
    VK_TRANSLATE_TABLE_REV[VK_DOWN] = SDLK_DOWN;
    VK_TRANSLATE_TABLE_REV[VK_UP] = SDLK_UP;
    VK_TRANSLATE_TABLE_REV[VK_NUMLOCK] = SDLK_NUMLOCKCLEAR;
    VK_TRANSLATE_TABLE_REV[VK_CAPITAL] = SDLK_CAPSLOCK;
    VK_TRANSLATE_TABLE_REV[VK_SCROLL] = SDLK_SCROLLLOCK;
    
    // ファンクションキーの逆マッピングを設定
    // (F13..F24 は F1..F12 と連番でない。 TVPTransSDLKeyToVirtualKey 側の注記参照)
    for (int i = 0; i < 12; i++) {
        VK_TRANSLATE_TABLE_REV[VK_F1 + i]      = SDLK_F1 + i;
        VK_TRANSLATE_TABLE_REV[VK_F13 + i]     = SDLK_F13 + i;
    }
    
    // テンキーの逆マッピングを設定
    for (int i = 0; i < 9; i++) {
        VK_TRANSLATE_TABLE_REV[VK_NUMPAD1 + i] = SDLK_KP_1 + i;
    }
    VK_TRANSLATE_TABLE_REV[VK_NUMPAD0] = SDLK_KP_0;
    VK_TRANSLATE_TABLE_REV[VK_DECIMAL] = SDLK_KP_PERIOD;
    VK_TRANSLATE_TABLE_REV[VK_DIVIDE] = SDLK_KP_DIVIDE;
    VK_TRANSLATE_TABLE_REV[VK_MULTIPLY] = SDLK_KP_MULTIPLY;
    VK_TRANSLATE_TABLE_REV[VK_SUBTRACT] = SDLK_KP_MINUS;
    VK_TRANSLATE_TABLE_REV[VK_ADD] = SDLK_KP_PLUS;
}

#include "LogIntf.h"

bool SDL3Application::GetAsyncKeyState(tjs_uint keycode, bool getcurrent)
{
    // 特殊キーの処理
    SDL_Keymod mod = SDL_GetModState();
    Uint32 mouse = SDL_GetMouseState(nullptr, nullptr);
    switch (keycode) {
    case VK_LSHIFT: return ((mod & SDL_KMOD_LSHIFT) != 0);
    case VK_LCONTROL: return ((mod & SDL_KMOD_LCTRL) != 0);
    case VK_LMENU: return ((mod & SDL_KMOD_LALT) != 0);
    case VK_RSHIFT: return ((mod & SDL_KMOD_RSHIFT) != 0);
    case VK_RCONTROL: return ((mod & SDL_KMOD_RCTRL) != 0);
    case VK_RMENU: return ((mod & SDL_KMOD_RALT) != 0);
    case VK_SHIFT: return ((mod & SDL_KMOD_SHIFT) != 0);
    case VK_CONTROL: return ((mod & SDL_KMOD_CTRL) != 0);
    case VK_MENU: return ((mod & SDL_KMOD_ALT) != 0);
    case VK_CAPITAL: return ((mod & SDL_KMOD_CAPS) != 0);
    case VK_NUMLOCK: return ((mod & SDL_KMOD_NUM) != 0);
    case VK_SCROLL: return ((mod & SDL_KMOD_SCROLL) != 0);
    case VK_LWIN: return ((mod & SDL_KMOD_LGUI) != 0); // 左Winキー
    case VK_RWIN: return ((mod & SDL_KMOD_RGUI) != 0); // 右Winキー
    case VK_LBUTTON: return ((mouse & SDL_BUTTON_LMASK) != 0); // 左クリック
	case VK_RBUTTON: return ((mouse & SDL_BUTTON_RMASK) != 0); // 右クリック
	case VK_MBUTTON: return ((mouse & SDL_BUTTON_MMASK) != 0); // 中央クリック
	case VK_XBUTTON1: return ((mouse & SDL_BUTTON_X1MASK) != 0); // X1ボタン
	case VK_XBUTTON2: return ((mouse & SDL_BUTTON_X2MASK) != 0); // X2ボタン
    }

    // その他キー
    if (keycode >= 0 && keycode < 256) {
        int sdlCode = VK_TRANSLATE_TABLE_REV[keycode];
        if (sdlCode) {
            const bool *state = SDL_GetKeyboardState(NULL);
            SDL_Keymod keymod;
            SDL_Scancode scancode = SDL_GetScancodeFromKey(sdlCode, &keymod);
            if (scancode != SDL_SCANCODE_UNKNOWN) {
                // keymod は「このキーコードを出すのに要る修飾」。判定は
                // 「必要な修飾グループが押されているか」だけを見る。
                // 旧実装の mod == keymod (完全一致) は NumLock/CapsLock が
                // ON なだけで常に false になり、VK_RETURN 等の押しっぱなし
                // 判定 (getKeyState) が全滅していた。
                auto groupOK = [&](SDL_Keymod group) -> bool {
                    return !(keymod & group) || (mod & group);
                };
                return state[scancode]
                    && groupOK(SDL_KMOD_SHIFT) && groupOK(SDL_KMOD_CTRL)
                    && groupOK(SDL_KMOD_ALT)   && groupOK(SDL_KMOD_GUI);
            }
        }
    }
    
    return tTVPApplication::GetAsyncKeyState(keycode, getcurrent);
}

#include "SysInitIntf.h"
static tTVPAtStart AtStart(TVP_ATSTART_PRI_PREPARE, InitKey);
