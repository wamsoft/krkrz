# Gamepad (ゲームパッド)

全ビルド共通のゲームパッド入力 API。物理バックエンドは SDL3 / LIB ビルドが
`SDL_Gamepad`、WINVER ビルドが XInput (最大 4 台) で、その上に
プラットフォーム非依存の論理管理層 `tTVPPadManager`
(`common/environ/PadManager.{h,cpp}`) を載せている。各プラットフォームは
物理パッドアクセスを `iTVPPhysicalPadProvider` として実装するだけで、論理層
(下記のインデックス規約・last-operated 追従・キーイベント生成) は共通。

## 論理インデックス (パッド番号)

全 API のパッド番号 `no` は次の意味を持つ:

- `0` : **最後に操作したパッド** (last-operated) への仮想エイリアス
- `1..N` : 実パッド (接続順で安定。`N = getJoypadCount()`)

キーイベント (`VK_PAD*`) は常に `0` 番 (= 最後に操作したパッド) を発生源とする。
「最後に操作した」の判定は実ボタン / 十字キーの新規押下で切り替わり、アナログ
スティックのドリフトでは切り替わらない。同じ物理パッドは `0` と実番号の両方で
参照できる (例: 1 台だけ接続なら `0` と `1` が同じパッド)。

## 1. 全体構造

物理層 → 論理層 (共通) → TJS の 3 層。

```
物理パッド backend (iTVPPhysicalPadProvider 実装)
    │  SDL3/LIB: SDL3Application       (sdl3/environ/pad.cpp, main.cpp)
    │  WINVER  : tTVPXInputPadProvider (win32/environ/XInputPad.cpp)
    │
tTVPPadManager (common/environ/PadManager.cpp)
    │  論理 index 変換 (0=最後に操作 / 1..N=実パッド) + last-operated 追従
    │  + 論理 0 のボタン差分を VK_PAD* キーイベント化 (KeyRepeat 込み)
    │
各ビルドのグルー → MainWindowForm へキー送出
    │  SDL/LIB: tTVPApplication::SendPadEvent  (generic/environ/JoyPad.cpp)
    │  WINVER  : TTVPWindowForm 連続ハンドラ    (win32/environ/WindowFormUnit.cpp)
    │
TJS: System.getPadState/getPadAxis/getJoypadType/… + Window.onKeyDown(VK_PAD*)
```

毎フレーム、フォーカスのあるウィンドウ (WINVER) または `SDL_AppIterate`
(SDL) から pad のポーリング + キーイベント生成が走る。連射 (KeyRepeat) は
十字キー系とトリガ系で別グループ管理 (`PadManager.cpp`)。

## 2. 接続管理と last-operated 追従

複数パッドに対応する。`tTVPPadManager` が毎フレーム全物理パッドを走査し、
実ボタン / 十字キーの新規押下があったパッドを「最後に操作したパッド」
(= 論理 0) に切り替える。切替はボタン入力ベースなのでスティックドリフトでは
起きない。論理 0 の識別名が変わると `onJoypadChange(0, name)` を発火する。

- **SDL3/LIB**: `SDL_EVENT_GAMEPAD_ADDED/REMOVED` で全パッドを開閉し
  `g_open_gamepads` に接続順で保持 (`sdl3/environ/main.cpp`)。物理 index は
  この並び。`gamecontrollerdb.txt` 読込は `pad.cpp:InitPadMaiing()` にあるが
  現状未配線。
- **WINVER**: XInput のユーザスロット 0..3 を毎フレームポーリング
  (`win32/environ/XInputPad.cpp`)。未接続スロットの走査は約 1 秒間引く。
  接続中スロットを接続順に詰めたものが物理 index。XInput 仕様上 Xbox 系
  コントローラのみ・最大 4 台 (汎用 DirectInput パッドは非対応)。

`getJoypadCount()` は実パッド台数 N、`hasJoypad(no)` は指定番号が有効か
(`no=0` は 1 台以上あれば true、`no` は 1..N が有効) を返す。同じ物理パッドは
`0` (最後に操作) と実番号 (1..N) の両方で参照できる。

パッド機能は全体無効化できる (サポート用: 他デバイスの誤パッド認識による誤動作の
回避)。無効時は状態取得・キーイベント生成をいずれも行わない。

- CLI **`-joypad=no`** (`off`/`false`/`0` も可) で起動時から無効化。判定は
  `tTVPPadManager` が `TVPGetCommandLine` で行うため全バリアント共通。
- TJS **`System.padEnabled`** (読み書き) で実行時に切替。明示設定は CLI より優先。
- C++ からは `PadManager` の `SetEnabled/IsEnabled` (Application 経由
  `SetJoypadEnabled/GetJoypadEnabled`)。

## 3. ボタン状態と軸状態

### 3.1 ボタン (28-bit bitmap) — `SDL3Application::GetPadState(int no)`

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
| 24  | 下     | VK_PAD_FACE_SOUTH | SDL_GAMEPAD_BUTTON_SOUTH (配置基準) |
| 25  | 右     | VK_PAD_FACE_EAST  | SDL_GAMEPAD_BUTTON_EAST  (配置基準) |
| 26  | 左     | VK_PAD_FACE_WEST  | SDL_GAMEPAD_BUTTON_WEST  (配置基準) |
| 27  | 上     | VK_PAD_FACE_NORTH | SDL_GAMEPAD_BUTTON_NORTH (配置基準) |

bit 24〜27 は bit 0〜3 と**同じ物理ボタン**を「配置」で指したものです。
bit 0〜3 (VK_PAD1..4) は刻印 A/B/X/Y に解決される (§3.2) のに対し、
こちらは常に 下/右/左/上 を指します。 1 回の押下で両方のビットが立つので、
割り当てる側が「刻印で揃えたいボタン」と「配置で揃えたいボタン」で
使い分けます (例: 決定は刻印の A、 コマンド決定は配置の 上)。

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

未接続パッド、無効な番号、範囲外の axisId はすべて `0.0` を返します。
Y 軸は **下方向が正** です (画面座標と一致。WINVER/XInput は内部で符号反転して
そろえている)。

C++ 側からは `Application->GetPadAxis(no, axisId)` で同じ値が取れます。
ID は `tTVPApplication::TVP_PAD_AXIS_LEFTX` 等の enum を使用。

## 4. TJS API 一覧

全 API 共通で `no` は §「論理インデックス」に従う (`0` = 最後に操作したパッド、
`1..N` = 実パッド)。全ビルド (SDL3 / LIB / WINVER) で利用可能。

| API                                            | 機能                                  |
|------------------------------------------------|---------------------------------------|
| `System.getJoypadType(no=0)`                   | 機種名 (SDL=認識名 / WINVER=`"XInput Controller"`) |
| `System.getJoypadCount()`                      | 接続中の実パッド台数 N                |
| `System.hasJoypad(no=0)`                       | 指定番号が有効か (0=1台以上でtrue / 1..N) |
| `System.getPadAxis(no, axisId)`                | アナログ軸値 (§3.2)                   |
| `System.rumblePad(no, low, high, durationMs)`  | 振動開始 (low/high は 0〜255)         |
| `System.stopRumblePad(no=0)`                   | 振動停止                              |
| `System.setPadOverlay([bool])`                 | デバッグオーバレイ切替 (PadOverlay.md) |
| `System.padAxis*` (定数)                       | 軸 ID 定数 6 個 (§3.2)               |
| `paLeftX` .. `paRightTrigger` (TJS グローバル)  | 軸 ID 定数 6 個 (§3.2)               |
| `System.onJoypadChange(no, name)` (callback)   | 論理0の識別名変化を通知 (無し時 name="") |
| `System.padEnabled` (読み書き)                 | パッド機能の有効/無効 (実行時切替、CLI優先度低) |
| CLI `-joypad=no`                               | 起動時からパッド機能を無効化 (§2)     |
| CLI `-padoverlay=1` / `config.cf`              | 起動時から PadOverlay ON (§5)        |

ボタン押下は直接 API では取れず、`Window.onKeyDown / onKeyUp` で
`VK_PAD1`〜`VK_PAD12` / `VK_PADLEFT`〜`VK_PADDOWN` / `VK_PAD_L_*` / `VK_PAD_R_*`
として受けます (発生源は常に論理 0 = 最後に操作したパッド)。
`System.getKeyState(VK_PADn)` 相当も動きます (`PadManager::GetAsyncKeyState`)。

**Elements ダイアログとの関係**: Elements ダイアログ (パネル) がフォーカスを
持っていると VK_PAD* はダイアログのウィジェット操作 (十字=ナビ / A=決定 /
B=cancel) に消費されます。「このパッドボタンだけは必ずゲーム側で受けたい」
場合は `ElementsDialog.registerHotKey(VK_PAD2)` 等でホストホットキー登録するとダイアログを
バイパスして `onKeyDown` へ直行します (配送優先順位と詳細は
[ElementsDialog.md](ElementsDialog.md) の「入力ルーティング」を参照)。

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
| `common/environ/PadManager.{h,cpp}`       | **論理層 (共通)**。`iTVPPhysicalPadProvider` IF + `tTVPPadManager` (index変換 / last-operated / VK_PAD* キー変換 + キーリピート) |
| `common/visual/KeyRepeat.{h,cpp}`         | キーリピート (十字系 / トリガ系)              |
| `generic/environ/Application.h`           | 抽象 IF (`GetPadState`/`GetPadAxis`/`TVP_PAD_AXIS_*` enum) が `PadManager_` へ委譲。`PadManager_` を保持 |
| `generic/environ/JoyPad.cpp`              | SDL/LIB: `SendPadEvent` (manager 駆動 + MainWindowForm へ送出) / `GetAsyncKeyState` |
| `sdl3/environ/main.cpp`                   | SDL Gamepad 接続管理 (`g_open_gamepads`) + 物理アクセサ |
| `sdl3/environ/pad.cpp`                    | SDL3 物理プロバイダ (`GetPhysicalPadState/Axis/Name/Rumble`) |
| `win32/environ/XInputPad.{h,cpp}`         | **WINVER 物理プロバイダ (XInput)**。最大4台・振動対応 |
| `win32/environ/Application.{h,cpp}`       | WINVER: パッド IF + `PadManager_` + `PadProvider_` + `PadPoll` |
| `win32/environ/WindowFormUnit.cpp`        | WINVER: フォーカスウィンドウで `PadPoll` 駆動 + キー送出 |
| `{generic,win32}/base/SystemImpl.cpp`     | TJS バインディング (getJoypadType/Count/hasJoypad/rumblePad/stopRumblePad/getPadAxis + padAxis* 定数) |
| `common/base/SystemIntf.cpp`              | `TVPFireOnJoypadChange` ← `System.onJoypadChange` |
| `resource/gamecontrollerdb.txt`           | SDL コントローラマッピング (現状未配線)       |

## 7. 将来拡張のメモ

- **センサ (accel/gyro)**: SDL3 `SDL_GetGamepadSensorData` で取れるが、
  `iTVPPhysicalPadProvider` / `tTVPPadManager` に新規メソッドが必要。
  `getPadSensor(no, sensorId, axis)` 形が自然 (WINVER/XInput はセンサ非対応)。
- **gamecontrollerdb 配線**: `pad.cpp:InitPadMaiing()` を `SDL_AppInit` から
  呼ぶ (現状コメントアウトの `SDL_AddGamepadMappingsFromFile` を生かす形でも可)。
- **WINVER の機種名**: XInput は機種名 API を持たないため一律 `"XInput Controller"`。
  個体名が必要なら RawInput / Windows.Gaming.Input との併用が要る。
