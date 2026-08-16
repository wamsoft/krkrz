pad_advanced — ゲームパッド (多パッド / アナログ軸 / 振動)
==========================================================

ゲームパッドまわりを 1 画面で確認するデモ。接続状態・アナログ軸・ボタン・
振動・デバッグオーバレイを実機で見ながら試せる。

起動:
  krkrz64.exe <このフォルダ>
  まとめて切り替える版は ../gallery (コアデモギャラリー) を参照。

■ 1. パッド番号 (論理インデックス)

  全 API 共通で番号は次の意味:

    0      最後に操作したパッド (last-operated 追従)
    1..N   接続順の実パッド

  「1 人プレイでどのパッドを持っても動く」ようにするなら 0 を使う。
  多人数で「1P はこのパッド」と固定したいなら 1..N を使う。

    System.getJoypadCount()      接続台数 N
    System.hasJoypad(no)         その番号が有効か
    System.getJoypadType(no)     機種名 (SDL=認識名 / WINVER="XInput Controller")
    System.onJoypadChange        論理 0 の識別名が変わったときの通知
                                 (抜けたときは name = "")
    System.padEnabled            パッド機能の有効/無効 (実行時に切替可)

■ 2. アナログ軸 (System.getPadAxis)

    System.getPadAxis(no, axisId)

  axisId は `System.padAxisLeftX` 等の定数 (グローバルの `paLeftX` 等でも同じ)。
  戻り値はスティックが -1.0〜+1.0、トリガが 0.0〜+1.0。未接続なら 0.0。

  デモでは左右スティックを升目 + ドットで、トリガをバーで表示し、下に生の
  数値を出している。**スティックを触っていなくても ±0.05 程度の値が出る**
  のが普通なので、ゲーム側でデッドゾーン処理が要ることが分かる。

■ 3. ボタン (VK_PAD*)

  ボタンは直接取得する API ではなく **キーイベント**として届く。

    VK_PAD1..VK_PAD12          A/B/X/Y, LB/RB, LT/RT, BACK/START, L3/R3
    VK_PADLEFT/RIGHT/UP/DOWN   十字
    VK_PAD_L_* / VK_PAD_R_*    スティックの 8 方向量子化

  発生源は常に論理 0 (最後に操作したパッド)。`System.getKeyState(VK_PADn)` でも
  現在の押下状態を取れる (デモの 16 マスはこちらのポーリング表示)。

  ★ **Elements パネルを出していると VK_PAD* はパネルに消費される**
    (十字 = フォーカス移動 / A = 決定)。ゲーム側の onKeyDown には届かない。
    「このボタンは必ずゲームで受けたい」場合は

        Dialog.registerHotKey(VK_PAD1)

    でホストホットキーとして確保するとパネルをバイパスして onKeyDown へ直行する。
    このデモはボタン観測が目的なので既定で全ボタンを確保している。
    パネルの「ボタンをホストで受ける」を OFF にすると、パッドがパネル操作に
    使われるようになり、ゲーム側のログが止まるのが確認できる。

  ※ demolib は既定で VK_PAD* を論理キー (十字→矢印 / A→Enter) へ読み替えて
    シーンへ渡す。このデモは `rawPadKeys = true` にして生のまま受けている。

■ 4. 振動 (System.rumblePad / stopRumblePad)

    System.rumblePad(no, low, high, durationMs)   low/high は 0〜255
    System.stopRumblePad(no)

  low = 低周波モータ (重い振動)、high = 高周波モータ (細かい振動)。
  パネルの「弱 / 強 / 両方」と「振動時間」スライダで試せる。
  振動非対応のパッドや未接続では偽が返る。

■ 5. デバッグオーバレイ (System.setPadOverlay)

  画面左上に 16 ボタンのマトリクスと 6 軸の数値を出す。SDL3 ビルド限定。
  REPL の `.padoverlay on` / CLI `-padoverlay=1` でも出せる。

■ 関連リファレンス

  doc/reference/System.md    getJoypadCount / hasJoypad / getJoypadType /
                             getPadAxis / rumblePad / stopRumblePad /
                             padEnabled / setPadOverlay / onJoypadChange /
                             padAxis* 定数
  doc/reference/Dialog.md    registerHotKey / unregisterHotKey
  src/core/doc/Gamepad.md    論理インデックスと実装構造 (全ビルド共通の論理層)
  src/core/doc/PadOverlay.md オーバレイの表示内容

■ メモ

  - パッドの抜き差しは「接続情報を取り直す」ボタンか、onJoypadChange の通知で
    反映される。
  - `-joypad=no` で起動するとパッド機能自体が無効になる (CLI が優先)。
