#include "tjsCommHead.h"
#include "CharacterSet.h"
#include "LogIntf.h"

#include "app.h"

#define _USE_MATH_DEFINES
#include <math.h>

enum {
    KEY_LEFT   = 0x01,
    KEY_UP     = 0x02,
    KEY_RIGHT  = 0x04,
    KEY_DOWN   = 0x08,
} Keys;

// アナログスティックの入力を方向キーに変換する
static void analog_to_key(float x, float y, int key_base, tjs_uint32 &key_state)
{
    // アナログパッドの判定
    if (sqrt(x*x+y*y) >= 0.6) {
        int angle = (int)(atan2f(y, x) * 360 / (M_PI * 2) + 360 + 360 / 16) % 360 / 45;
        static tjs_uint32 stick[] = { 
                KEY_RIGHT,
                KEY_RIGHT | KEY_DOWN,
                KEY_DOWN,
                KEY_LEFT | KEY_DOWN,
                KEY_LEFT,
                KEY_LEFT | KEY_UP,
                KEY_UP,
                KEY_UP | KEY_RIGHT };
        key_state |= (stick[angle] << key_base);
    }
}

extern SDL_Joystick *joystick;

tjs_uint32 SDL3Application::GetPadState(int no)
{
    if (!joystick) {
        return 0;
    }

    // パッドキー状態
    tjs_uint32 key_state = 0;

    // ボタン状態を取得
    for (int i=0; i<12; i++) {
        if (SDL_GetJoystickButton(joystick, i)) {
            key_state |= (1 << i);
        }
    }
    // 配置基準のフェイスボタン (bit24..27)。 生ジョイスティックは刻印が
    // 判らないので、 SDL の慣例 (ボタン 0..3 = 下/右/左/上) をそのまま写す。
    for (int i = 0; i < 4; i++) {
        if (SDL_GetJoystickButton(joystick, i)) key_state |= (1u << (24 + i));
    }

    // HAT（方向パッド）の状態を取得
    int hat_count = SDL_GetNumJoystickHats(joystick);
    if (hat_count > 0) {
        Uint8 hat_state = SDL_GetJoystickHat(joystick, 0);        
        if (hat_state & SDL_HAT_LEFT) {
            key_state |= (1 << 12);
        }
        if (hat_state & SDL_HAT_UP) {
            key_state |= (1 << 13);
        }
        if (hat_state & SDL_HAT_RIGHT) {
            key_state |= (1 << 14);
        }
        if (hat_state & SDL_HAT_DOWN) {
            key_state |= (1 << 15);
        }
    }

    // アナログスティック（軸）から方向キー情報を反映
    int axis_count = SDL_GetNumJoystickAxes(joystick);
    
    if (axis_count >= 2) {
        // 左スティック（通常は軸0,1）
        float leftX = SDL_GetJoystickAxis(joystick, 0) / 32767.0f;
        float leftY = SDL_GetJoystickAxis(joystick, 1) / 32767.0f;
        analog_to_key(leftX, leftY, 16, key_state);
    }
    
    if (axis_count >= 4) {
        // 右スティック（通常は軸2,3）
        float rightX = SDL_GetJoystickAxis(joystick, 2) / 32767.0f;
        float rightY = SDL_GetJoystickAxis(joystick, 3) / 32767.0f;
        analog_to_key(rightX, rightY, 20, key_state);
    }        
    
    // トリガー（軸4,5）をL2/R2ボタンに反映
    float triggerThreshold = 0.8f;
    if (axis_count >= 5) {
        float leftTrigger = SDL_GetJoystickAxis(joystick, 4) / 32767.0f;
        if (leftTrigger > triggerThreshold) {
            key_state |= (1 << (VK_PAD7 - VK_PAD1));
        }
    }    
    if (axis_count >= 6) {
        float rightTrigger = SDL_GetJoystickAxis(joystick, 5) / 32767.0f;
        if (rightTrigger > triggerThreshold) {
            key_state |= (1 << (VK_PAD8 - VK_PAD1));
        }
    }
    
    //TVPLOG_DEBUG("GetPadState: {:x}", key_state);
    return key_state;	
}