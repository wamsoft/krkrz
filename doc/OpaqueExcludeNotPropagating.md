# opaque 除外が中間レイヤで伝播せず下層が無駄合成される件 (調査メモ)

2026-06-17 調査。Android (SDL3/GL) で `VideoOverlay` の **layer モード(vomLayer)**
全画面再生を計測した際に判明した、**既存の挙動**に関する記録。未修正。

## 症状

全面 opaque (`ltOpaque`) の動画レイヤを最前面に置いても、その**下にあるレイヤ群が
毎フレーム合成される**。本来、最前面に全画面 opaque があれば下層は描画スキップできる
はずだが、効いていない。

実測 (テストシーン, 1920x1080, Android 実機):
- 下層レイヤなし (背景 + 動画のみ): **57fps** (VSync 上限)
- 下層レイヤあり (後述の構造): **12fps**、`DrawStats` で `Wkr≈2800ms/s` の合成コスト

※ これは「8 行ストライプ分割 (gsotSimple)」とは**別問題**。ストライプ分割の方は
`generic/base/SysInitImpl.cpp` で SDL3 既定を `gsotNone` にして解消済み
(135 stripe → 1)。本件はそれを直した後も残る、下層レイヤ自体の合成コスト。

## 再現したレイヤ構造

```
primaryLayer (ltAlpha)
├─ base1 (BaseLayer, type 未指定)      ← 中間レイヤ
│   ├─ layer1..4 (ltAlpha 半透明)
├─ base2 (BaseLayer, type 未指定)      ← 中間レイヤ
│   ├─ bg, ev
└─ videolay (ltOpaque, 全画面, 最前面)  ← 動画 (vomLayer)
```

動画は `videolay` (全画面 `ltOpaque`) に AssignMainImage され、最前面 (最後に追加)。

## 原因 (推定)

`common/visual/LayerIntf.cpp` の `tTJSNI_BaseLayer::QueryUpdateExcludeRect()`
(関数冒頭 5181 付近) の伝播条件:

```cpp
parentvisible = parentvisible && Visible &&
    (DisplayType == ltOpaque || DisplayType == ltAlpha ||
     DisplayType == ltAddAlpha || DisplayType == ltPsNormal) &&
    Opacity == 255;
```

- 子の走査は `TVP_LAYER_FOR_EACH_CHILD_NOLOCK_BACKWARD` = 前面→背面で正しい。
  最前面の `videolay` が先に処理され、全画面の除外矩形 `rect` を出す。
- ところが、その除外を**受け取る中間レイヤ `base1`/`base2` の `DisplayType` が
  上記の許可集合 (`ltOpaque/ltAlpha/ltAddAlpha/ltPsNormal`) から外れていると、
  そこで `parentvisible=false` になり、配下 (layer1..4 / bg / ev) の
  `UpdateExcludeRect` が `clear()` される** (5202 付近)。
- 結果、配下レイヤの `CopySelf()` (5523 付近、`UpdateExcludeRect` を見て全面被覆なら
  描画スキップする実装) が「除外なし」と判断し、全部描画してしまう。

つまり「全画面 opaque による下層スキップ」は、**間に挟まる中間レイヤが特定の
DisplayType でないと貫通しない**。BaseLayer のようなトランジション用中間層を
挟むと効かない。

## 関連する git 履歴 (要確認)

`common/visual/LayerIntf.cpp` 直近に合成 walk の最適化と revert あり:
- `e620aaa0 LayerIntf: NeedsCompletion フラグで Layer 木 walk を skip (Phase 9 対策)`
- `cb255e8f LayerIntf: MarkNeedsBeforeCompletion を強制親祖先 propagate に変更`
- `1b032875 Revert "...NeedsCompletion フラグで Layer 木 walk を skip" + 続く propagate 修正`

この辺りの最適化で除外伝播が壊れた可能性も視野に入れて、過去の動作と比較するとよい。

## 今後の方針

実案件のレイヤ構造はこのテストシーンとは大きく異なるため、**実案件で同種の無駄合成が
起きるかを先に確認**してから、本件を追うか判断する。追う場合の候補:

1. `QueryUpdateExcludeRect` の伝播条件を見直し、中間レイヤが「自分自身は描画に寄与
   しない透明な器」であっても除外矩形を素通しさせる (誤って隠れ層を消さないよう要検証)。
2. 上記 git の NeedsCompletion 最適化前後で除外挙動が変わっていないか bisect。
3. それ以前に、そもそも全画面動画は mixer モードを使う運用で回避するのも手。

## このとき同時に入れた高速化 (本件とは別、適用済み)

- `sdl3/visual/SDLOGLTextureUpdateRect.h`: 更新矩形を永続ステージングへ集約し、
  集約矩形を 1 回でアップロード (per-rect の glTexSubImage 連発を回避)。
- `generic/base/SysInitImpl.cpp`: SDL3 既定を `gsotNone` (8 行ストライプ分割を無効)。
- `generic/visual/VideoOvlImpl.cpp`: vomLayer のフロー制御。前フレーム未消費中は
  デコード変換 (updater) を呼ばずドロップ → ティアリング解消 + 無駄変換削減。
