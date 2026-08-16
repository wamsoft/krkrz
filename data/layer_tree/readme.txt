layer_tree — レイヤツリー / ヒットテスト / フォーカス
====================================================

レイヤの「構造」と「入力の当たり方」を 1 画面で試すデモ。
描画そのものではなく、レイヤツリーの振る舞いを確認するためのもの。

起動:
  krkrz64.exe <このフォルダ>
  まとめて切り替える版は ../gallery (コアデモギャラリー) を参照。

■ 1. ヒットテスト (hitType / hitThreshold / onHitTest)

  背面に BACK レイヤを置き、その上に 2 種類のカードを重ねてある。
  クリックした結果「どのレイヤが受けたか」が右の状態表示に出る。

  A: hitType = htMask (既定)
     マスク (不透明度) の値が hitThreshold 以上の画素だけがマウスを受ける。
     左から α=0 / α=128 / α=255 の 3 帯になっていて、パネルの
     hitThreshold スライダを動かすと「当たる帯」が変わる。
       - threshold 0   … 全面で受ける (矩形として振る舞う)
       - threshold 8   … α=128 と α=255 の帯が当たる (既定)
       - threshold 200 … α=255 の帯だけが当たる
       - threshold 256 … 全て透過して BACK が受ける
     受けなかったクリックは、より奥のレイヤ (ここでは BACK) へ流れる。

  P: hitType = htProvince
     領域 (province) 画像が 0 以外の画素だけがマウスを受ける。
     このデモでは independProvinceImage() + setProvincePixel() で
     「円の内側だけ province = 1」の領域画像をコード生成している。
     見た目 (マスク) は矩形全面なのに円の外はクリックが透過するので、
     **当たり判定と見た目が独立**であることが分かる。

  onHitTest
     hitType / hitThreshold で「当たり」と判定された後にだけ呼ばれ、
     スクリプト側で最終的な可否を決められる。パネルの
     「A の onHitTest で右半分を無効化」で、当たっているはずの右半分を
     透過させられる。

■ 2. 親の状態が子に伝わる (nodeVisible / nodeEnabled)

  親レイヤ 1 枚 + 子レイヤ 2 枚。パネルで親の visible / enabled を切り替えると、
  子の nodeVisible / nodeEnabled (= 祖先まで含めた実効値) が追従することを
  状態表示で確認できる。子自身の visible / enabled は変えていない点に注意。

  enabled = false のレイヤは、htMask のとき「全面が不透明度 0」とみなされる
  = 入力を素通しする。このデモのフォーカス枠レイヤはその性質を利用して、
  カードのクリックを邪魔しないようにしている。

■ 3. 重ね順 (order) とフォーカス連鎖 (focusNext)

  focusable な 4 枚のカードが重なっている。

    - Tab / Shift+Tab      … nextFocusable / prevFocusable でフォーカス移動
    - カードをクリック      … そのカードにフォーカス (focus())
    - パネル「前面へ/背面へ」… bringToFront() / bringToBack()

  フォーカス中のカードは黄色い枠で示される。重ね順を変えると状態表示の
  「重ね順 (手前が上)」が更新される。

■ 関連リファレンス

  doc/reference/Layer.md
    hitType / hitThreshold / onHitTest / nodeVisible / nodeEnabled /
    focusable / focus / focusNext / prevFocusable / bringToFront /
    independProvinceImage / setProvincePixel

■ メモ

  - Layer.visible の既定は false。生成しただけでは表示されない。
  - 描画は DemoWindow.stage 以下に対して行う (demolib の規約)。
    primary へ直接描くと現行エンジンの既知の描画問題を踏む。
