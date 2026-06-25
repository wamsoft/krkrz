# Gamepad (ゲームパッド)

SDL3 ビルドのゲームパッド入力 API。WINVER ビルドは `tTVPApplication`
のパッド系仮想関数を override していないため、本書の API は実質
SDL3 ビルド (および LIB ビルド) 限定です。

## 1. 全体構造

3 層に分かれます。

```
SDL3 イベント (sdl3/environ/main.cpp)
    │  グローバル `SDL_Gamepad *gamepad` を SDL_EVENT_GAMEPAD_ADDED/REMOVED で抱える
    │
SDL3Application::GetPadState / GetPadAxis / RumbleGamepad …
    │  (sdl3/environ/pad.cpp, main.cpp)
    │
tTVPApplication::SendPadEvent → AM_KEY_DOWN/UP として MainWindow へ
    │  (generic/environ/JoyPad.cpp)
    │
TJS: System.getPadAxis / getPadState 相当はキーイベント / System.onJoypadChange など
```

毎フレーム `SDL_AppIterate` から `app->SendPadEvent()` が走り、ボタン
状態の差分をキーイベント化します (`sdl3/environ/main.cpp:447`)。
連射 (KeyRepeat) は十字キー系とトリガ系で別グループ管理 (`JoyPad.cpp`)。

## 2. 接続管理

現状はメインパッド 1 台のみ運用です (API シグネチャは `int no` を取りますが
内部では `no == 0` 以外は無効値を返します)。

- `SDL_EVENT_GAMEPAD_ADDED`: 既存 `gamepad` が NULL のときだけ
  `SDL_OpenGamepad()` で開き、`TVPFireOnJoypadChange(0, name)` を発火
- `SDL_EVENT_GAMEPAD_REMOVED`: 自分の所持しているパッドが外れたら
  close + `TVPFireOnJoypadChange(0, "")` を発火
- 接続中の全パッド一覧は `SDL_GetGamepads()` で取れる (`getJoypadCount`)

`USE_LAST_PUSHDOWN_PAD` で「最後にボタン / タッチパッド DOWN が来たパッドを
メインに切替える」モードを ON/OFF できます (`sdl3/environ/main.cpp:20`)。
**既定は ON (=1)**。OFF にすると旧挙動 (最初に認識したパッドを保持、それが
切断されるまで他パッドに切替わらない) に戻ります。

ボタン入力ベースでの切替なのでスティックドリフトでは切替わりません。複数
パッド同時制御は別課題で、本フラグはあくまで「メイン 1 枚を最後に触ったもの
に追従させる」だけの機能です。

`gamecontrollerdb.txt` を読み込むコードは `sdl3/environ/pad.cpp` に
`InitPadMaiing()` として書かれていますが、現状呼び出し元がありません
(未配線)。

## 3. ボタン状態と軸状態

### 3.1 ボタン (24-bit bitmap) — `SDL3Application::GetPadState(int no)`

`pad.cpp` で SDL Gamepad API から組み立てて返します。スクリプトからは
直接見えず、`SendPadEvent` 経由でキーイベントに変換されます。

| bit | ラベル | VK              | SDL Gamepad Button                |
|-----|--------|-----------------|-----------------------------------|
| 0   | A      | VK_PAD1         | SDL_GAMEPAD_BUTTON_SOUTH          |
| 1   | B      | VK_PAD2         | SDL_GAMEPAD_BUTTON_EAST           |
| 2   | X      | VK_PAD3         | SDL_GAMEPAD_BUTTON_WEST           |
| 3   | Y      | VK_PAD4         | SDL_GAMEPAD_BUTTON_NORTH          |
| 4   | L1     | VK_PAD5         | SDL_GAMEPAD_BUTTON_LEFT_SHOULDER  |
| 5   | R1     | VK_PAD6         | SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER |
| 6   | L2     | VK_PAD7         | LEFT_TRIGGER 軸 (閾値 0.8)        |
| 7   | R2     | VK_PAD8         | RIGHT_TRIGGER 軸 (閾値 0.8)       |
| 8   | BACK   | VK_PAD9         | SDL_GAMEPAD_BUTTON_BACK           |
| 9   | START  | VK_PAD10        | SDL_GAMEPAD_BUTTON_START          |
| 10  | L3     | VK_PAD11        | SDL_GAMEPAD_BUTTON_LEFT_STICK     |
| 11  | R3     | VK_PAD12        | SDL_GAMEPAD_BUTTON_RIGHT_STICK    |
| 12  | ←      | VK_PADLEFT      | SDL_GAMEPAD_BUTTON_DPAD_LEFT      |
| 13  | ↑      | VK_PADUP        | SDL_GAMEPAD_BUTTON_DPAD_UP        |
| 14  | →      | VK_PADRIGHT     | SDL_GAMEPAD_BUTTON_DPAD_RIGHT     |
| 15  | ↓      | VK_PADDOWN      | SDL_GAMEPAD_BUTTON_DPAD_DOWN      |
| 16  | L_←    | VK_PAD_L_LEFT   | 左スティックを 8 方向量子化 (半径 0.6 以上) |
| 17  | L_↑    | VK_PAD_L_UP     | 〃                                |
| 18  | L_→    | VK_PAD_L_RIGHT  | 〃                                |
| 19  | L_↓    | VK_PAD_L_DOWN   | 〃                                |
| 20  | R_←    | VK_PAD_R_LEFT   | 右スティックを 8 方向量子化       |
| 21  | R_↑    | VK_PAD_R_UP     | 〃                                |
| 22  | R_→    | VK_PAD_R_RIGHT  | 〃                                |
| 23  | R_↓    | VK_PAD_R_DOWN   | 〃                                |

bit 16〜23 は左右スティックを `analog_to_key()` で 8 方向 DPAD 化した値で、
半径 0.6 以上のときに方向ビットが立ちます。**生のスティック値が欲しい
場合は次節の `getPadAxis` を使ってください。**

### 3.2 軸 (アナログ値) — `System.getPadAxis(no, axisId)` 【新規】

スティックの傾き / トリガの押し込み量を float で取得します。デッドゾーンは
適用しないので、呼び元で必要に応じて切り捨ててください。

```tjs
var x = System.getPadAxis(0, paLeftX);          // -1.0 〜 +1.0
var y = System.getPadAxis(0, paLeftY);          // -1.0 〜 +1.0
var t = System.getPadAxis(0, paLeftTrigger);    // 0.0 〜 +1.0
```

軸 ID 定数は **TJS グローバル定数** と **System の readonly プロパティ** の
両方を用意してあります。値はすべて同じで、好みに合わせて使い分け可能。

| TJS グローバル    | System プロパティ              | 値 | 範囲           | 内容                              |
|-------------------|--------------------------------|----|----------------|-----------------------------------|
| `paLeftX`         | `System.padAxisLeftX`          | 0  | -1.0 〜 +1.0   | 左スティック X (-1 = 左, +1 = 右) |
| `paLeftY`         | `System.padAxisLeftY`          | 1  | -1.0 〜 +1.0   | 左スティック Y (-1 = 上, +1 = 下) |
| `paRightX`        | `System.padAxisRightX`         | 2  | -1.0 〜 +1.0   | 右スティック X                    |
| `paRightY`        | `System.padAxisRightY`         | 3  | -1.0 〜 +1.0   | 右スティック Y                    |
| `paLeftTrigger`   | `System.padAxisLeftTrigger`    | 4  |  0.0 〜 +1.0   | L2 アナログ                       |
| `paRightTrigger`  | `System.padAxisRightTrigger`   | 5  |  0.0 〜 +1.0   | R2 アナログ                       |

TJS グローバルは `resource/SysInitScript.tjs` の `const` ブロックで定義。
定数値は意図的に `SDL_GamepadAxis` (`LEFTX=0` 〜 `RIGHT_TRIGGER=5`) と同値で
そろえてあります (`pad.cpp` で `static_assert` 検証)。

未接続パッド、`no != 0`、範囲外の axisId はすべて `0.0` を返します。
Y 軸は SDL3 と同じく **下方向が正** です (画面座標と一致)。

C++ 側からは `Application->GetPadAxis(no, axisId)` で同じ値が取れます。
ID は `tTVPApplication::TVP_PAD_AXIS_LEFTX` 等の enum を使用。

## 4. TJS API 一覧

| API                                            | 機能                                  | ソース                  |
|------------------------------------------------|---------------------------------------|-------------------------|
| `System.getJoypadType(no=0)`                   | SDL 認識名 (例 `Xbox Series X Controller`) | `pad`/`main.cpp`     |
| `System.getJoypadCount()`                      | 接続中のパッド総数                    | `main.cpp`              |
| `System.hasJoypad(no=0)`                       | 指定番号が有効か (現状 0 のみ true 可) | `main.cpp`             |
| `System.getPadAxis(no, axisId)`                | アナログ軸値 (§3.2)                   | `pad.cpp`               |
| `System.rumblePad(no, low, high, durationMs)`  | 振動開始 (low/high は 0〜255)         | `main.cpp`              |
| `System.stopRumblePad(no=0)`                   | 振動停止                              | `main.cpp`              |
| `System.setPadOverlay([bool])`                 | デバッグオーバレイ切替 (PadOverlay.md) | `SystemImpl.cpp`       |
| `System.padAxis*` (定数)                       | 軸 ID 定数 6 個 (§3.2)               | `SystemImpl.cpp`        |
| `paLeftX` .. `paRightTrigger` (TJS グローバル)  | 軸 ID 定数 6 個 (§3.2)               | `resource/SysInitScript.tjs` |
| `System.onJoypadChange(no, name)` (callback)   | 接続/切断通知 (切断時は name="")      | `SystemIntf.cpp`        |
| CLI `-padoverlay=1` / `config.cf`              | 起動時から PadOverlay ON (§5)        | `common/base/PadOverlay.cpp` |

ボタン押下は直接 API では取れず、`Window.onKeyDown / onKeyUp` で
`VK_PAD1`〜`VK_PAD12` / `VK_PADLEFT`〜`VK_PADDOWN` / `VK_PAD_L_*` / `VK_PAD_R_*`
として受けます。`System.getKeyState(VK_PADn)` 相当も `GetAsyncKeyState` 経由で
動きます (`generic/environ/JoyPad.cpp:57`)。

## 5. デバッグオーバレイ

詳細は [PadOverlay.md](PadOverlay.md) を参照。`System.setPadOverlay(true)` /
REPL `.padoverlay on` / CLI `-padoverlay=1` のいずれかで画面左上に
**16 ボタンマトリクス + 6 軸のアナログ値** を表示します。軸値は本書 §3.2 の
`GetPadAxis` 出力そのまま (`%+.2f`、LX/LY/RX/RY は -1.00〜+1.00、
LT/RT は 0.00〜+1.00) を 3 行 × 2 列で描画するので、デッドゾーン調整や
スティック挙動の確認に利用できます。

## 6. ソースファイル

| ファイル                                  | 役割                                          |
|-------------------------------------------|-----------------------------------------------|
| `generic/environ/Application.h`           | 抽象 IF (`GetPadState`/`GetPadAxis`/`TVP_PAD_AXIS_*` enum 他) |
| `generic/environ/JoyPad.cpp`              | ビット → VK_PAD* キーイベント変換 + キーリピート |
| `sdl3/environ/main.cpp`                   | SDL Gamepad 接続管理 + RumbleGamepad / onJoypadChange |
| `sdl3/environ/pad.cpp`                    | `GetPadState` / `GetPadAxis` の SDL3 実装      |
| `sdl3/environ/joystick.cpp`               | 旧 SDL_Joystick 実装 (sources.cmake で OFF、未ビルド) |
| `generic/base/SystemImpl.cpp`             | TJS バインディング (`getPadAxis` メソッド + `padAxis*` 定数 + 既存) |
| `common/base/SystemIntf.cpp`              | `TVPFireOnJoypadChange` ← `System.onJoypadChange` |
| `resource/gamecontrollerdb.txt`           | SDL コントローラマッピング (現状未配線)       |

## 7. 将来拡張のメモ

- **N 台対応**: `main.cpp` のグローバル `SDL_Gamepad *gamepad` を
  `std::vector<SDL_Gamepad*>` に置き換え、`HasJoypad(no)` / `GetPadState(no)` /
  `GetPadAxis(no, axisId)` / Rumble 系を index 引きにする。`SendPadEvent` の
  Last 状態も pad 毎に持つ必要あり。`onJoypadChange` の `no` 引数は今でも
  渡しているので TJS API 側の変更は不要。
- **センサ (accel/gyro)**: SDL3 `SDL_GetGamepadSensorData` で取れるが、
  `tTVPApplication` に新規仮想関数が必要。`getPadSensor(no, sensorId, axis)`
  形が自然。
- **gamecontrollerdb 配線**: `pad.cpp:InitPadMaiing()` を `SDL_AppInit` から
  呼ぶ (現状コメントアウトの `SDL_AddGamepadMappingsFromFile` を生かす形でも可)。
- **WINVER 側実装**: `BasicDrawDevice` ベースの旧 Win32 ビルドで XInput 等を
  使う場合は `tTVPApplication::GetPadState/GetPadAxis` を override する派生を
  `win32/environ/` 配下に追加すること。
