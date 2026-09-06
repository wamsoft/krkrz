# 最上位ホットキー (`System.registerHotKey`)

イベントポンプの入口でキーを照合し、 **フォーカス中のレイヤ / テキスト入力 /
Elements ダイアログ (モーダル含む) より先に**コールバックを起動する仕組み。
Alt+Enter のフルスクリーン切替や Esc の終了確認のように 「画面が何であっても
効いてほしい」キーを 1 箇所で扱うためのもの。

従来この手のキーは画面ごとの UI 定義とゲーム側のキーフックへ分散していて、
起動ランチャー (モーダル) やタイトルの入力欄にフォーカスがある間は届かなかった。

実装: [`common/environ/HotKeyIntf.{h,cpp}`](../common/environ/HotKeyIntf.h) +
TJS 束縛 `common/base/SystemIntf.cpp` + フック `sdl3/environ/app.cpp`。

## API

```tjs
System.registerHotKey(key, mods, callback);   // 登録 (同一 key+mods は差し替え)
System.unregisterHotKey(key, mods);           // 解除。 戻り値 = 解除できたか
```

- `key` … VK コード (`VK_RETURN` / `VK_ESCAPE` / `VK_F1` …)。
- `mods` … `ssShift` / `ssAlt` / `ssCtrl` の組合せ。 **照合はこの 3 ビットのみ**で、
  NumLock / CapsLock 等の状態は無視する。
- `callback(key, shift)` … 押下時に同期呼び出し。 戻り値の扱いは下記。

```tjs
// Alt+Enter でフルスクリーン切替 (どの画面でも効く)
System.registerHotKey(VK_RETURN, ssAlt, function(key, shift) {
    Window.fullScreen = !Window.fullScreen;
});

// Esc は終了確認。 ただしモーダルダイアログ表示中は
// ダイアログの cancel を優先したいので素通しする
System.registerHotKey(VK_ESCAPE, 0, function(key, shift) {
    if(ElementsDialog.modalActive) return false;   // 消費せず通常の dispatch へ
    askQuit();
    return true;
});
```

## 消費の規則

| 事象 | 挙動 |
|---|---|
| down (非リピート) で `key` + `mods` 一致 | コールバックを呼ぶ。 戻り値が `false` (または 0) なら**消費せず**通常の dispatch へ流す。 それ以外 (`void` 含む) は消費 |
| down のリピート (押しっぱなし) | 消費中のキーなら黙って消費するだけ。 **コールバックは再発火しない** |
| up | **`key` のみで照合**し、 消費した down に対応する up は必ず消費する。 修飾キーを先に離しても (Alt+Enter で Alt を先に離す等) 片割れのキーイベントが入力レイヤへ漏れない |

コールバックが例外を投げてもポンプは壊さず、 `TVPAddImportantLog` へ残して続行する。
登録テーブルはプロセス共有 (Window 単位ではない) で、 エンジン終了時に解放される。

## 配送順序の中での位置

```
0. 最上位ホットキー         … System.registerHotKey  ← ここ (ポンプ入口)
1. Elements モーダル        … 全消費
2. ホストホットキー         … ElementsDialog.registerHotKey (ダイアログへ渡さず通常経路へ)
3. フォーカスパネル         … キー/パッドを送り、 未処理分のみ素通し
4. ゲーム / レイヤ          … 未消費の落ち先
```

1 以降の詳細は [ElementsDialog.md](ElementsDialog.md) 「入力ルーティング」節。

モーダルポンプ (`PumpModalLoop`) も同じ `AppEvent` を通るので、 **モーダル表示中や
画面遷移の合間でも効く**。

## `ElementsDialog.registerHotKey` との違い

| | `System.registerHotKey` | `ElementsDialog.registerHotKey` |
|---|---|---|
| 位置 | 全 dispatch の**上流** (ポンプ入口) | Elements ダイアログの入力転送の先頭 |
| 動作 | 登録キーで**コールバックを起動**して消費 | 登録キーを**ダイアログへ渡さず**通常のゲーム入力経路 (`Window.onKeyDown` 等) へ素通し (専用イベント無し) |
| モーダル中 | **効く** | 効かない (モーダルの Esc=cancel 等を奪わないため) |
| 対象 | キーのみ | キー / パッドボタン (`VK_PAD*`) / マウスボタン (`VK_RBUTTON` 等) |

「ダイアログにフォーカスを渡しつつ特定キーだけホストが取る」= `ElementsDialog.registerHotKey`、
「画面の状態に関わらず必ず先に効かせる」= `System.registerHotKey`。

## `ElementsDialog.modalActive` (読取専用)

`modal=true` なインスタンス (`showModal*` / モーダルフロー) が 1 つでもアクティブなら
`true`。 非モーダルの常駐オーバレイ (字幕 / HUD 等) は含まない。 上の例のように
「モーダル中は別扱い」をホットキーのコールバックが判断するのに使う。

## 制限

- **フックは SDL3 系ビルド (`sdl3/environ/app.cpp` の `AppEvent` 入口) のみ**。
  WINVER ビルドは未配線で、 登録しても発火しない。
- 印字キーの登録は非推奨 (文字入力イベントまでは抑止しない)。
