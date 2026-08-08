elements_flow — Elements フロー画面遷移エフェクト デモ

Dialog.startFlow の複数画面フローで、画面 JSON の transitions に宣言する
画面切替エフェクトを確認するデモ。

  - "effect": "fade"       … クロスフェード
  - "effect": "universal"  … rule 画像によるユニバーサルトランジション
                             ("rule": 画像パス / "vague": 境界ぼかし 0-255)
  - "animate" の "on":"exit" … 退場演出 (Esc / close でも演出完了を待つ)

構成:
  startup.tjs      単体起動ランチャ
  scene.tjs        ElementsFlowScene (DemoScene)
  flow/app.jsonc   画面マニフェスト
  flow/*.jsonc     各画面 (home / fade / wipe / circle)
  flow/rule_*.png  rule 画像 (斜めグラデ / 放射 / 反転放射、生成物)

rule 画像は Python (stdlib) で生成した 8bit グレースケール PNG。値の小さい
画素から先に次画面へ切り替わる。

関連ドキュメント: doc/ElementsDialog.md「画面切替エフェクト」
(umbrella では topics/core/elements_dialog.md)
