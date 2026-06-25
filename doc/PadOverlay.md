# PadOverlay (ゲームパッド状態オーバレイ)

SDL3 ビルドで、画面左上に現在のゲームパッド状態 (16 ボタンの ON/OFF
マトリクス + 6 軸のアナログ値) をリアルタイム表示するデバッグ用
オーバレイです。memoverlay (画面右上) と独立に動き、両方同時に出せます。

WINVER ビルドでも flag は持つが、`tTVPApplication` がパッド抽象 API を
持たないため OGLDrawDevice 経路で出した場合は `Pad: (none)` + 全 OFF +
全軸 +0.00 の表示になります。

## 表示例

```
+--------------------------------+
| Pad: Xbox Series X Controller  |
| [A ][B ][X ][Y ]               |
| [L1][R1][L2][R2]               |
| [BK][ST][LS][RS]               |
| [Lf][Up][Rt][Dn]               |
| LX +0.45 LY -0.32              |
| RX +0.00 RY +0.00              |
| LT +0.00 RT +0.80              |
+--------------------------------+
```

- ヘッダ: SDL ゲームパッド名、未接続時は `(none)`
- セル: 押下中は緑、未押下は暗灰。1.5x スケールで描画
- ラベル順は `kButtonMap[]` (= `Application::GetPadState(0)` の bit 0–15)
- 軸: `Application::GetPadAxis(0, axisId)` の値を `%+.2f` 表示。
  LX/LY/RX/RY は -1.00〜+1.00、LT/RT は 0.00〜+1.00 (符号は常に `+`)。
  未接続時は dim gray 表示

| Bit | ラベル | SDL Gamepad Button |
|---|---|---|
| 0 | A  | SOUTH |
| 1 | B  | EAST |
| 2 | X  | WEST |
| 3 | Y  | NORTH |
| 4 | L1 | LEFT_SHOULDER |
| 5 | R1 | RIGHT_SHOULDER |
| 6 | L2 | LEFT_TRIGGER (axis, 閾値 0.8) |
| 7 | R2 | RIGHT_TRIGGER (axis, 閾値 0.8) |
| 8 | BK | BACK |
| 9 | ST | START |
| 10 | LS | LEFT_STICK (押し込み) |
| 11 | RS | RIGHT_STICK (押し込み) |
| 12 | Lf | DPAD_LEFT |
| 13 | Up | DPAD_UP |
| 14 | Rt | DPAD_RIGHT |
| 15 | Dn | DPAD_DOWN |

bit 16–23 (左右アナログスティックを方向キー化した値) は表示しません。
生のスティック傾きは下段の `LX/LY/RX/RY` 行 (軸値表示) を見てください。
ゲームロジックが実際に見ている `GetPadState(0)` / `GetPadAxis(0, *)` の
値そのままなので、入力がエンジンまで届いているかの確認に使えます。

## 切替

### CLI (起動時から ON)

```bash
krkrz64.exe data/ -padoverlay=1
```

`config.cf` にも書ける (`-padoverlay=1` 行を追加)。`0` または省略時は OFF。
WINVER でも flag は立つが、既定 `BasicDrawDevice` には描画フックがないため
画面には何も出ない (`tTVPOGLDrawDevice` に切替えれば `(none)` 状態で出る)。

### TJS

```tjs
System.setPadOverlay(true);    // 表示開始
System.setPadOverlay(false);   // 表示停止
System.setPadOverlay();        // toggle (戻り値は新しい状態 0/1)
```

### REPL

```
.padoverlay        # toggle
.padoverlay on     # 表示開始
.padoverlay off    # 表示停止
```

## 描画経路

memoverlay と同じ三系統に同居しています。

| DrawDevice | 描画関数 | 描画手段 |
|---|---|---|
| `tTVPSDLDrawDevice` | `TVPRenderPadOverlay(renderer)` | SDL_Renderer + `SDL_RenderDebugText` |
| `tTVPSDLOGLDrawDevice` | `TVPRenderPadOverlayGL()` | OpenGL ES 直接 + 8x8 font + shader/VBO |
| `tTVPOGLDrawDevice` | `TVPRenderPadOverlayGL()` | 同上 (WINVER でも有効) |

OFF 時はいずれも先頭で即 return するので、常時呼び出しても overhead は
ごく僅か。GL 版の shader/font/VBO は lazy init で初回呼出時のみ確保。

## ソースファイル

| ファイル | 役割 |
|---|---|
| `common/base/PadOverlay.{h,cpp}` | ON/OFF フラグ (`TVPPadOverlay::SetEnabled` / `IsEnabled`) + CLI 初期化 (`TVPInitializePadOverlay`) |
| `generic/base/SysInitImpl.cpp` / `win32/base/SysInitImpl.cpp` | 起動時に `TVPInitializePadOverlay()` を呼んで `-padoverlay=1` を反映 |
| `sdl3/visual/PadOverlayRender.{h,cpp}` | SDL_Renderer 経路の描画 |
| `common/visual/opengl/PadOverlayGL.{h,cpp}` | OpenGL ES 直接経路の描画 (font/shader 内蔵) |

memoverlay (`common/base/MemoryOverlay.*` / `sdl3/visual/MemoryOverlayRender.*`
/ `common/visual/opengl/MemoryOverlayGL.*`) と完全に同じ構造で、サンプラ
スレッドが要らないぶんだけ簡素です。
