#ifndef __TARGET_VER_H__
#define __TARGET_VER_H__

// SDKDDKVer.h をインクルードすると、利用できる最も上位の Windows プラットフォームが定義されます。

// 以前の Windows プラットフォーム用にアプリケーションをビルドする場合は、WinSDKVer.h をインクルードし、
// SDKDDKVer.h をインクルードする前に、サポート対象とするプラットフォームを示すように _WIN32_WINNT マクロを設定します。
// 最低ターゲット = Windows 10 (WinXP/Vista/7/8 はサポート外)。
// これにより touch/gesture/SetThreadDescription 等 Win7+/Win10+ API を直接リンクできる。
#define WINVER 0x0A00 // Windows 10
#define _WIN32_WINNT 0x0A00 // Windows 10
#include <SDKDDKVer.h>

#endif
