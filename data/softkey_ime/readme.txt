softkey_ime — 文字入力 / 仮想キーボード / IME
==============================================

文字入力まわりを 1 画面で試すデモ。物理キーボードのある PC と、キーボードの
無い環境 (コンソール / モバイル) の両方を想定した入力経路を確認できる。

起動:
  krkrz64.exe <このフォルダ>
  まとめて切り替える版は ../gallery (コアデモギャラリー) を参照。

■ 1. 文字入力は onTextInput で受ける

  onKeyDown / onKeyUp は**仮想キーコード** (VK_*) を扱うイベント。
  実際に入力された**文字**は `onTextInput(text)` で**文字列単位**に届く
  (IME の確定文字列はまとまって 1 回で届く)。制御文字 (BS/Enter 等) や
  ファンクションキーでは発生しない。編集キーは onKeyDown の VK_BACK /
  VK_RETURN で受ける。

  このデモは自前描画の入力欄を用意し、onTextInput で受けて表示・ログ化し、
  BackSpace / Enter は onKeyDown で処理している。

  旧 `onKeyPress(key)` (WM_CHAR 相当・1 文字ずつ) は WINVER でのみ互換の
  ため発火し続ける。SDL 系では発火しない (onTextInput へ一斉移行)。

  ※ demolib の DemoScene に `onTextInput(text)` フックを追加した
    (DemoShell → シーンへ中継)。

■ 2. IME (Layer.imeMode / setAttentionPoint)

  IME の状態は**フォーカスのあるレイヤ**の `imeMode` で決まる。

    imDisable   IME 無効
    imClose     IME は使えるが閉じた状態
    imOpen      IME を開いた状態
    imDontCare  現在の状態のまま

  変換候補ウィンドウの表示位置は `Layer.setAttentionPoint(x, y, font)` で
  指示する (このデモは入力欄の左下を指定)。

  ※ **これらが実際に効くのは WINVER (Win32 IME) ビルド**。SDL ビルドは
    OS 側の入力に任せる作りで、値は保持されるだけ。

■ 3. 内蔵仮想キーボード (Dialog.virtualKeyboard)

  物理キーボードが無い環境では、Elements のテキスト欄 (input_box) に
  focus が入ったとき、engine 内蔵の英数キーボードが overlay で出る。

    Dialog.virtualKeyboard = "auto"    既定。物理キーボードが無いときだけ
                             "always"  常に出す (テスト用。デスクトップでも出る)
                             "never"   出さない (OS 側に任せる)
    Dialog.hasPhysicalKeyboard         物理キーボードの有無 (読み取り専用)

  パネルのスライダで切り替えられる。**"always" にしてパネルの
  「Elements 入力欄」をクリックする**と、デスクトップでも仮想キーボードが
  出る (SPACE / BS / DONE 付きの英数 4 段)。押鍵はそのまま入力欄へ流れる。

  ※ 仮想キーボードが出るのは **Elements のテキスト欄に focus したとき**。
    自前描画の入力欄 (このデモの左側) では出ない。
  ※ `Agent.dialogFocus` だけでは編集状態にならないので、検証は実クリック
    (`Agent.click`) で行う。
  ※ v1 の制限として大文字英数字のみ。

■ 4. System.inputString

  モーダルなテキスト入力。閉じるまで戻らない。SDL ビルドでは Elements の
  ダイアログとして出る。パネルの「System.inputString で入力」から実行できる。

■ 5. クリップボード (Clipboard)

    Clipboard.asText              読み書き。テキストが無いときは void
    Clipboard.hasFormat(cbfText)  テキストがあるか

  パネルの「コピー」「貼り付け」で入力欄との間でやり取りする。

  ※ SDL ビルドのクリップボードは長らく空実装だったが、2026-08-16 に
    SDL3 の `SDL_GetClipboardText` / `SDL_SetClipboardText` で実装した。
    OS のクリップボードと双方向にやり取りできる。

■ 関連リファレンス

  doc/reference/Window.md     onTextInput / onKeyDown / imeMode
  doc/reference/Layer.md      imeMode / setAttentionPoint / focusable / focus
  doc/reference/Clipboard.md  asText / hasFormat
  doc/reference/System.md     inputString
  src/core/doc/ElementsDialog.md
                              「テキスト入力とソフトキーボード」節
                              (仮想キーボードの出る条件と実装)

■ メモ

  - `Agent.keyPress(VK_A)` はキーイベントだけを注入するので **onTextInput は
    発生しない** (文字入力は OS のテキスト入力経路を通るため)。自動テストで
    文字を入れたい場合は `Agent.text("...")` を使う (実入力と同じ経路で
    注入され、Elements のテキスト欄 focus があればそちらが消費し、無ければ
    ゲーム側の onTextInput へ届く)。
  - 仮想キーボードは画面中央に出るため、入力欄を覆うことがある (既知の制限)。
