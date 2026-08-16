softkey_ime — 文字入力 / 仮想キーボード / IME
==============================================

文字入力まわりを 1 画面で試すデモ。物理キーボードのある PC と、キーボードの
無い環境 (コンソール / モバイル) の両方を想定した入力経路を確認できる。

起動:
  krkrz64.exe <このフォルダ>
  まとめて切り替える版は ../gallery (コアデモギャラリー) を参照。

■ 1. 文字入力は onKeyPress で受ける

  onKeyDown / onKeyUp は**仮想キーコード** (VK_*) を扱うイベント。
  実際に入力された**文字**は `onKeyPress(key)` で届く (IME の確定文字もここ)。
  ファンクションキーのように文字と無関係なキーでは発生しない。

  このデモは自前描画の入力欄を用意し、onKeyPress で 1 文字ずつ受けて
  表示・ログ化している (BackSpace = 0x08、Enter = 0x0D も文字として届く)。

  ※ demolib の DemoScene に `onKeyPress(key)` フックを追加した
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

  doc/reference/Window.md     onKeyPress / onKeyDown / imeMode
  doc/reference/Layer.md      imeMode / setAttentionPoint / focusable / focus
  doc/reference/Clipboard.md  asText / hasFormat
  doc/reference/System.md     inputString
  src/core/doc/ElementsDialog.md
                              「テキスト入力とソフトキーボード」節
                              (仮想キーボードの出る条件と実装)

■ メモ

  - `Agent.keyPress(VK_A)` はキーイベントだけを注入するので **onKeyPress は
    発生しない** (文字入力は OS のテキスト入力経路を通るため)。自動テストで
    文字を入れたい場合は Elements 欄 + `Agent.text` を使う。
  - 仮想キーボードは画面中央に出るため、入力欄を覆うことがある (既知の制限)。
