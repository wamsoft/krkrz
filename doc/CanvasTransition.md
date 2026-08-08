# Canvas.drawTransition — 内蔵トランジション描画

表 (front) と裏 (back) の 2 テクスチャを進行度 `phase` で合成して描画する
内蔵機能。クロスフェードと、rule 画像によるユニバーサルトランジションを
シェーダ内蔵で提供する。従来 TJS 層で `ShaderProgram` を自前生成して
実装していたトランジション (旧ゲームシステムの GPUMode 相当) を、
Canvas の 1 メソッドで置き換えられる。

実装: `common/visual/opengl/CanvasIntf.{h,cpp}`
(`TransitionCrossfadeFragmentShaderText` / `TransitionUniversalFragmentShaderText`、
初回使用時に内蔵 `ShaderProgram` を遅延生成してキャッシュ)。

## API

```tjs
// クロスフェード
canvas.drawTransition(front, back, phase);

// ユニバーサルトランジション (rule 画像)
canvas.drawTransition(front, back, phase, ruleTex);        // vague 既定 64
canvas.drawTransition(front, back, phase, ruleTex, 128);   // vague 指定
```

| 引数 | 意味 |
|---|---|
| `front` / `back` | `Texture` / `Offscreen` (描画元)。phase=0 で front のみ、1 で back のみ |
| `phase` | 進行度 0.0〜1.0 (範囲外はクランプ) |
| `rule` | rule 画像テクスチャ。**`tcfAlpha` で作成する** (`new Texture("rule.png", tcfAlpha)`)。値が小さい画素ほど早く back へ切り替わる。省略 / null でクロスフェード |
| `vague` | 境界ぼかし幅 (rule 値スケール 0-255、既定 64) |

- 配置・変形・ブレンドは `drawTexture` と同じ規約 (`canvas.matrix` /
  `blendMode` / クリップに従う)。頂点 VBO は front テクスチャのものを使う。
- ユニバーサルの閾値は `phase * (1 + vague/255)` をスイープする (phase=1 で
  必ず全画素 back)。Elements ダイアログの画面切替エフェクト (CPU 版
  `elements_modal::blend_universal_argb8888`) と同じ意味論。
- straight-alpha 同士の混色式は旧ゲームシステムの `crossfade.frag` /
  `universal.frag` を踏襲 (半透明ソース同士でも自然な合成)。
- `a_opacity` uniform (既定 1.0) で全体不透明度を掛けられる
  (内蔵シェーダも defaultShader と同じ規約)。

## 使用例 (画面遷移)

```tjs
// onDraw 内: old / cur は Offscreen (旧画面と新画面をそれぞれ描いたもの)
var t = (System.getTickCount() - startTick) / duration;
if (t < 1.0) {
    canvas.drawTransition(oldScreen, curScreen, t, ruleTex, 64);
} else {
    canvas.drawTexture(curScreen);
}
```

関連: Elements ダイアログのフロー画面切替エフェクト (`doc/ElementsDialog.md`
「画面切替エフェクト」) は同じ語彙 (`effect` / `rule` / `vague` / `duration`) の
CPU 実装で、全 DrawDevice で動く。GL 上で自前描画するゲームシステム側は
本メソッドで GPU 合成できる。
