# Elements 仕様精査 — 入力リピート / キャッシュ / 描画

Elements (`external/elements` = wamsoft/elements、その中の `external/elements_modal`)
と、それを載せるエンジン側 (`common/visual/elements/`) について、
**「同じことを二重にやっていないか」「必要以上に抱えていないか」「必要以上に
描いていないか」** の 3 軸で 2026-09-11 に行った精査の記録。

案件で多発した「キーリピートで連打状態になる」不具合は、リピートがトリガでは
あったが真因はマウス座標の取り扱いだった。その後始末で積み上がったガードが
意味のあるものとして残っているか、構造を複雑にしただけになっていないかの
検証を含む。

確認用サンプル = `data/elements_audit/` (シナリオ番号を各項に記載)。
関連 SSOT = [ElementsDialog.md](ElementsDialog.md) / [Gamepad.md](Gamepad.md) /
[ScreenTransfer.md](ScreenTransfer.md) / [MemoryDesign.md](MemoryDesign.md)。

対応したら項目に ✅ と対応コミットを書き、**消さずに残す**。

---

## 0. 入力の流れ (現状)

```
[OS キーボード]  ── OS オートリピート ──→ KeyDown + TVP_SS_REPEAT ──┐
                                                                     │
[パッド (物理)] ── tTVPPadManager::Update()  (毎フレーム走査)        │
                   └ tTVPKeyRepeatEmulator ×2 (十字系 / トリガ系)    │
                      HoldTime 500ms / IntervalTime 30ms             │
                      (-paddelay / -padinterval)                     │
                      → VK_PAD* の KeyDown + TVP_SS_REPEAT ─────────┤
                      WINVER: WindowFormUnit.cpp:459                 │
                      SDL/generic: generic/environ/JoyPad.cpp        │
                                                                     ▼
                                    tTVPElementsDialogManager::OnKeyDown
                                     ├ HostHotkeyBypass      (登録キーは素通し)
                                     ├ armed_vks ゲート      (:3285)
                                     └ RouteVk               (ElementsInputMap.h:89)
                                                ▼
                          overlay_session::on_key_down / on_pad_button
                                                ▼
       view::key()                ← 矢印キーはここで 1 回 = OS のリピート速度
       view::pad_button_event()   ← dpad は「軸値を立てて return」(view.cpp:1338)
                                                ▼
       view::process_pad_axes()   ← ★フォーカス送りを作るのはここだけ
                                     initial_delay 400ms / rep 60ms (view.cpp:1596)
```

**二重ステップは起きていない** (精査で確認済み)。

- `view::pad_button_event` は dpad を `feed_dpad_axis` で軸値に変換した時点で
  `return true` し、pad→key 合成 (step 3) には進まない。エンジン側のリピートが
  何度届いても `st.current = ±1` の再代入にしかならず、ステップは生まれない。
- `RouteVk` (`ElementsInputMap.h:136-141`) は dpad の VK を矢印キーではなく
  `pad_button` へ落とす。キー経路と軸経路が同時に走ることはない。

---

## 1. cursor-warp の echo 判定が 2 層に重複している (優先度: 高)

**連打バグの再発条件そのもの。** 真因がマウス座標の取り扱いだったのに、
修正が座標依存の推測判定を 2 つに増やす形で着地している。

### 現状

「この mouse move は自分が出した cursor-warp の echo か？」という判定が、
エンジンと elements_modal の両方に独立実装されている。

| | エンジン | elements_modal |
|---|---|---|
| 場所 | `ElementsDialogManager.cpp:3171-3192` | `overlay_session.cpp:1729-1735` |
| 状態 | `warp_expect_active` / `warp_expect_x` / `warp_expect_y` | `warp_issued` / `warp_target` |
| 判定 | `abs(x - expect) <= 2` | `abs(p.x - target.x) <= 2.0f` |
| **座標系** | **レイヤ (描画矩形) 座標** | **view 論理座標** |
| 目的 | カーソル非表示を維持するか | `last_nav_source` を mouse に戻すか |

### 問題

同じ許容幅 ±2px を、**倍率の違う 2 つの座標系**に適用している。
view 座標 = (レイヤ座標 − `last_rect`) ÷ `present_scale` なので、
`present_scale ≠ 1` の画面 (ウィンドウ拡縮 / Steam Deck / DPI 100% 以外) では
2 つの判定が食い違う。

`present_scale = 0.5` なら、レイヤ座標で 2px のずれは view 座標で 4px になり、
**エンジンは「warp の echo」と判断してカーソルを隠したまま、elements は
「実マウスが動いた」と判断して `last_nav_source` を mouse に倒す**。
この不一致こそ `overlay_session.cpp:1712-1720` のコメントが書いている

> 隣接する 2 項目の間でフォーカスとポインタが振動し続け、グリッド状の UI
> (ソフトウェアキーボード等) が操作不能になる

= 連打状態の発生条件である。

### 直し方

合成 move を**推測ではなく明示タグ**にする。エンジンは自分が `SetCursorPos`
した直後だと知っているので、そのとき配送する move に「合成」フラグを立てて
`on_mouse_move` へ渡し、elements 側の `warp_issued` / `warp_target` / ±2px を
**丸ごと削除**する。

- `overlay_session::on_mouse_move(float sx, float sy, int mods, bool synthetic)`
  の 1 引数追加で済む (既存呼出は `false` 既定で互換)。
- 座標系の違いも許容幅の二重管理も消え、判定箇所が 1 つになる。
- (当時の想定) エンジン側の `warp_expect_*` はカーソル非表示の維持に引き続き
  使う。→ **その後の仮想カーソル移行で `warp_expect_*` ごと撤去された。**

### ✅ 対応済み (2026-09-12) → その後**機構ごと撤去**

> 📌 **2026-09-12 のうちに [仮想カーソル位置](VirtualCursor.md)へ移行し、
> この判定は両側から消えた。** 実カーソルを直接動かして OS を一往復するからこそ
> 「返ってきた move は自分の echo か」の判定が要ったので、仮想位置にしたら
> 発生源ごと無くなった。以下は撤去前の経緯 (記録)。

engine `ElementsDialogManager.cpp` が判定を 1 箇所で行い、結果を
`on_mouse_move(..., bool synthetic)` で session へ渡す形にした。
elements_modal 側の `warp_issued` / `warp_target` / ±2px 照合は削除
(`warp_target` は診断ログ用にのみ残す)。`ElementsLayerPanel` /
`WinElementsModalRunner` は warp を持たないので既定の `false` のまま。

### 確認手順

サンプル **シナリオ 1** (`cursor_warp` グリッド、`-audittest` で自動)。
倍率 1/1 と 1/2 の 2 本を走らせ、矢印キーを 150ms ごとに注入して
フォーカスを動かす。判定は「倍率を変えても振動 (A→B→A) が 0」。

⚠ **実マウスに触れていると測定にならない**。キーでフォーカスを動かした直後に
実マウスの移動が届くとナビ種別が mouse へ倒れ、warp の発火条件 (直近のナビ
入力がキー/パッド) から外れて **warp 自体が起きなくなる**。実際、最初の計測は
これで汚染されて `osc=2` を出していた (`note_warp` が 1 度も出ていないことで
気付いた)。

**汚染の確実な判定は `-navlog` の `warp guard: MISMATCH` の有無**。warp の
往復は実マウスが止まっていれば必ず `d=0,0` で一致する (下の §1-b 参照) ので、
MISMATCH が 1 行でも出ていたらその回は実マウスが動いている。
サンプルの `@audit` 行が出す `mousemoves` は**弱い指標でしかない** —
パネルが出ている間はオーバーレイがマウス移動を消費するため、実マウスが
動いていても 0 のままになる。

### 計測結果 (2026-09-12、WINVER x64 Release、`mousemoves=0` の回)

| | 倍率 | keys | moves | osc |
|---|---|---|---|---|
| 修正前 | 1/1 | 12 | 6 | 0 |
| 修正前 | 1/2 | 14 | 8 | 0 |
| 修正後 | 1/1 | 13 | 5 | 0 |
| 修正後 | 1/2 | 14 | 6 | 0 |

**このハーネスでは修正前後で差が出ない** (どちらも振動 0)。§1 が狙った
「両層の判定が食い違う窓」は、実誤差 e に対し engine が `e ≤ 2` (レイヤ px)、
session が `e ≤ 2 × present_scale` (view px) で判定するときの差分なので、
`present_scale = 1/2` なら `1 < e ≤ 2` という狭い範囲でしか踏まない。
今回の走行はその窓に入らなかった。

したがって §1 の修正は **「踏むと直る」ことを実測できた類のものではなく、
判定を 1 箇所へ寄せて窓そのものを消す構造修正**である。座標系の違う二重
判定が消え、削除した状態 (`warp_issued` / `warp_target` 照合) のぶん
session 側が単純になった。

---

---

## 1-b. ✅ warp の往復誤差は無かった (2026-09-12 に取り下げ)

§1 の計測中、実マウスに触れていないつもりの走行で
`warp guard: MISMATCH (d=3〜7)` が頻発したため、いったん
「warp の往復誤差が許容幅を超えて warp モードが勝手に切れる」新規バグとして
起票した。**これは誤りだった。**

### 切り分け方

`-navlog` に次の 2 つを足して 1 対 1 で追えるようにした (診断は残してある)。

- warp 要求に通し番号 (`#N warp request -> x,y ...`)
- `SetCursorPos` 直後に `GetCursorPos` で**同じ座標系で読み戻す**
  (`#N warp landed -> ax,ay (asked lx,ly d=..)`)。ここで既にずれていれば
  設定側 (DPI 変換 / クランプ) の問題、一致していれば echo の配送側の問題、
  と切り分けられる
- guard 側にも通し番号と受信座標 (`warp guard #N: ... got=x,y (d=..)`)

### 結果 (2026-09-12、WINVER x64 Release、実マウス非接触)

| | 件数 |
|---|---|
| `warp landed` の `d=0,0` | 11 / 11 |
| `warp guard` の `match` | 13 / 13 |
| `MISMATCH` | 0 |

倍率 1/1 と 1/2 のどちらでも**往復は厳密に一致**する。座標変換の鎖が
平行移動のみ (`TranslateDrawAreaToWindow` / `TranslateWindowToDrawArea` =
`LastSentDrawDeviceDestRect` の原点を足し引きするだけ) である以上これが正しく、
±2px の許容幅も妥当だった。

### 教訓

最初の計測で `mousemoves=0` だったので「実マウスは動いていない」と判断したが、
**パネルが出ている間はオーバーレイがマウス移動を消費してシーンの
`onMouseMove` まで届かない**ため、このカウンタは 0 のままだった。
cursor-warp まわりを測るときの汚染判定は `warp guard: MISMATCH` を見ること。

## 2. ✅ 一覧の更新コストが行数に比例する (2026-09-12 対応)

### 現状

cycfi の `composite_base::for_each_visible` (`composite.cpp:88-110`) は
`port_bounds` との交差で子を確実にカリングし、`view_bounds()`
(`view.hpp:424-433`) が部分再描画中はダーティ矩形を返すので、
カリングがそのまま部分再描画の効率化になっている。`canvas` (PSD 絶対配置) も
`composite_base::draw` に委譲しており正常。

ところが **`list::draw` (`json_layout.cpp:7291-7305`) だけがこれを上書きして、
可視判定を落としている**。

```cpp
for (std::size_t i = 0; i < size(); ++i) {
    if (!row_has_data(int(i))) continue;   // 件数不足の行は描かない
    auto& e = at(i);
    ce::context ectx{ctx, &e, bounds_of(ctx, i)};
    e.draw(ectx);                          // ← intersects(bounds, port_bounds) が無い
}
```

### 問題

`row_has_data` でしか間引いていないため、**部分再描画中 (`draw_bounds` が
小さなダーティ矩形) でも全行を描く**。行テンプレートが atlas 画像 + ラベル
数枚という普通の構成だと、画面の隅でキャレットが点滅するたびに一覧全体が
再描画される。`hit_element` (`:7308`) は `bounds.includes(p)` で正しく
絞っているので、draw だけが取り残された形。

### 直し方

`row_has_data` の次に交差判定を足す (elements `5da5301d` の次のコミットで対応済み)。

```cpp
auto const b = bounds_of(ctx, i);
if (!ce::intersects(b, ce::get_port_bounds(ctx))) continue;
```

これは正しい修正だが、**単体では数値が動かない** — 真因は下記のとおり
`limits()` 側にあった。

### 確認手順

サンプル **シナリオ 2** (`data/elements_audit`、`-audittest` で自動巡回)。
1 枚のパネルに多行 `list` と小さなラベルを置き、**ラベルだけ**を 100ms ごとに
`setVar` で書き換えて `ElementsDialog.renderStats` を見る。部分再描画は
セッション (パネル) 単位なので、ダーティ矩形を小さくするには list と同じ
パネルの中で変化させる必要がある (別パネルにすると list 側は再描画されず
測定にならない)。

### ★ 追跡の結果 (2026-09-12) — 真因は `list::limits()` の毎フレーム全行走査

最初の計測 (2026-09-11) では「カリングを入れても行数への比例が消えない」と
見えた。追い込んだ結果、**その計測自体に 2 つの誤りがあった**上で、
**別の真因が見つかった**。

#### 計測の誤り (訂正)

1. **partialRedraw OFF の A/B ステップを混ぜて数えていた。**
   `raster partial=0` を行数別に集計したつもりが、A/B のために意図的に
   `partialRedraw` を切ったステップぶんを一緒に数えていた。partial ON の
   ステップは 20 ラスタすべて partial で成立している。
2. **`list::draw` のカリングは効いている。** 部分再描画中はダーティ矩形
   (例: 行数 64 で view 換算 (9,9)-(262,50)) が list の bounds
   ((16,56)-(536,2232)) と交差しないので、そもそも `list::draw` が
   呼ばれない (canvas 側の `for_each_visible` が先に落とす)。

#### 真因

それでも `raster_us_r` は行数に比例した (8 行 413us → 64 行 1059us)。
ダーティ矩形は行数が増えるほど**小さく**なる (パネルが縮小 present される
ため 81x13px まで下がる) のに、1 ラスタが約 1ms かかっていた。

計測したところ **`list_rows_element::limits()` が毎フレーム呼ばれ、その中で
全行の `limits()` を回していた** (`json_layout.cpp`)。

```
list::limits calls=1080 rows_total=39616 (rows=64)
```

60 フレームあたり 60 回呼ばれ、1 回ごとに 64 行 = 3840 回の `row->limits()`。
行の中身はラベルなので、1 回ごとにテキスト計測 (→ §4 の `text_metrics`
キャッシュ参照) が走る。**描画のカリングをすり抜けてここだけが O(行数) で
残っている**のが、「小さな変更しかしていないのにコストが行数に比例する」の
正体だった。

この `limits()` 内の全行走査はコメントに「副作用のある widget 用」とある
とおり、`text_var` のラベル等へ新しい値を拾わせるための呼び出しである。

#### なぜ毎フレーム呼ばれるのか

`view::draw` が**無条件に `set_limits()` を呼ぶ** (`lib/src/view.cpp`)。
これは cycfi 本体の作りで、レイアウト自体は `subj_bounds` が変わったときしか
やり直さないのに、**limits だけは毎フレーム木全体を歩き直す**。

```cpp
void view::draw(canvas& cnv)
{
   ...
   set_limits();            // ← 毎フレーム。木全体の limits() を再計算
   ...
   if (subj_bounds != _current_bounds)   // レイアウトは変化時だけ
      _main_element.layout(ctx);
   _main_element.draw(ctx);
}
```

つまり**部分再描画でも木全体の `limits()` は走る**。`list` はそれに加えて
自前で全行を歩いていたので、O(行数) がそのまま乗っていた。

なお `list_rows_element::limits()` の**戻り値は spec から計算**していて
(`pitch * rows + row_h`)、行の `limits()` の結果を使っていない。つまりこの
走査は純粋に副作用目的だった。副作用の中身を追ったが、`text_var` のラベルも
一覧の «窓» も**購読 (`subscribe` → `set_text`) で更新されていて**、
`limits()` に依存している実装は見つからなかった。

#### ✅ 対応済み (2026-09-12)

部分再描画中 (`view::draw_bounds()` が非空) は、**ダーティ矩形に掛かる行だけ**
`limits()` を一巡するようにした。そこ以外の行はそのフレームで描かれないので
副作用の機会が要らず、掛かる行には従来どおり機会が残る。全面描画は従来どおり
全行を一巡するので、万一 `limits()` の副作用に依存する widget があっても
全面描画のたびに機会がある。

`limits()` の context からは自分の bounds を引けないので、直近 `draw` の
`ctx.bounds` を控えて基準に使う (`_last_bounds`。まだ描いていなければ従来どおり
全行)。`bounds_of` の算術は `row_rect(base, ix)` に切り出して共用。

#### 効果 (2026-09-12、WINVER x64 Release、`us/raster`)

| レイアウト | 行数 | 修正前 | 修正後 |
|---|---|---|---|
| canvas | 8 | 413 | 350 |
| canvas | 16 | 611 | 350 |
| canvas | 32 | 684 | 343 |
| canvas | 64 | 1059 | **304** |
| vtile | 8 | 180 | 142 |
| vtile | 64 | 904 | **93** |

**行数への比例が消えた**。vtile の 64 行は 904 → 93us = 約 10 倍。
(行数が増えるほどわずかに安くなるのは、パネルが縮小 present されて
ダーティ矩形が小さくなるため。)

実画面も確認済み — 64 行の一覧が全行正しく描画され、部分再描画も
15/15 で成立している (`-auditshot` で保存した PNG で目視)。

#### 残る一般論 (対象外)

`view::draw` が毎フレーム木全体の `limits()` を歩くこと自体は cycfi 本体の
作りなので残っている。widget 数の多い画面 (PSD 由来の canvas 配置など) では、
部分再描画にしても **O(画面全体の widget 数)** の下限がある。ここを下げるなら
`set_limits()` を「レイアウトが無効化されたときだけ」に変える必要があり、
fork の範囲としては大きい。今回の `list` のように「自分の limits が子に
依存しない composite」を個別に軽くするのが費用対効果がよい。

#### -navlog に残した診断

追跡に使った 2 つは常設の診断として残してある (`-navlog` 時のみ出力)。

- `raster partial=N (allow=.. full=.. rect=.. bufsame=..) dirty_px=.. buf=.. view=.. rs=..`
  — ラスタ 1 回ごとに、部分にできたか / できなかった理由
- `no-partial: trans=.. valid=.. dev=.. buf=.. render=.. surf=.. fit=.. area=..`
  — ホスト側が部分再描画を許可しなかったとき、どの一致条件が崩れたか
  (画面生成直後の 1 フレーム目は `valid=0` で出るのが正常)

### 参考: 最初の計測値 (2026-09-11)

`raster_us_r` = ラスタ 1 回あたり us、1 区間 15〜20 ラスタの平均。

| レイアウト | 行数 | partialRedraw | us/raster |
|---|---|---|---|
| canvas | 8 | ON | 413 |
| canvas | 16 | ON | 611 |
| canvas | 32 | ON | 684 |
| canvas | 64 | ON | 1059 |
| canvas | 64 | **OFF** | 1585 |
| vtile | 8 | ON | 180 |
| vtile | 64 | ON | 904 |

判定は「行数を 8 → 64 に増やしても `us/raster` が横ばい」。

> ⚠ 計測時の注意: 1 区間のラスタ回数が 2 回程度だと実行ごとのブレが大きく、
> partial ON/OFF の差が出たり出なかったりする (実際に 1530/1549 = 差なし と
> 965/1389 = 33% 差 の両方を観測した)。サンプルは 100ms 間隔で変化させて
> 1 区間 15〜20 ラスタを確保している。

### 残りのスケール懸念 (今回は対象外)

`composite_base::for_each_visible` は交差判定自体は O(n) の線形走査で、
`scroller` の中に vtile で数百エントリ並べる画面では描画をスキップしても
毎フレーム全件の `bounds_of` + `intersects` を回す。現状の規模なら無視できる。
本格的に多数エントリを扱うなら `list` の窓方式 (`rows` 個のテンプレート複製 +
`index_offset_var`、データ件数に対して O(1)) に寄せるのが正解で、cycfi 側の
`dynamic_list` (`lib/src/element/list.cpp`) は現在 elements_modal からは
使われていない。

---

## 3. ✅ アトラスキャッシュの解放の口 (2026-09-12 対応)

### 現状のキャッシュ一覧

| キャッシュ | 場所 | 予算 | 解放 |
|---|---|---|---|
| アトラス decode (RGBA) | `json_layout.cpp:713-796` | **192MB** | **プロセス終了時のみ** |
| テキスト run ビットマップ | `text_backend_tvg.cpp:124` | 4M px (≒16MB) | LRU trim あり |
| text_metrics | `canvas.cpp:869-877` | 4096 エントリ | 超えたら全 clear (項 4) |
| glyphware face | `glyph_layout_gw.cpp:69` | 無制限 | なし |
| mem:// 画像ストア | エンジン `registerImage` | — | `unregisterImage` / `clearImages` |

### 問題

`release_atlas_pixmaps()` (`json_layout.cpp:809`) は shutdown 専用で、他に
呼び元が無い。タイトルメニューで 150MB 積んだ状態のままゲーム本編に入る。

192MB という予算自体は「長時間プレイ後のヒープ断片化で 20〜30MB の連続領域が
取れずデコードに失敗し、画像なしで画面が組まれる」問題への対処として妥当
(経緯は elements_modal README「アトラスのデコードキャッシュ」節)。
問題は**ホストが場面境界で落とす手段が無い**ことと、**常駐量が見えない**こと。

### ✅ 対応済み (2026-09-12)

elements_modal に 3 つの口を足し (`modal.h`、 プロセス全体で 1 つ)、
krkrz の TJS へ公開した。

| elements_modal | TJS |
|---|---|
| `atlas_cache_stats(bytes, count, budget)` | `ElementsDialog.atlasCacheStats` (読取専用、 辞書) |
| `trim_atlas_cache(budget)` | `ElementsDialog.trimAtlasCache(budget = 0)` → 解放バイト数 |
| `set_atlas_cache_budget(budget)` | `ElementsDialog.atlasCacheBudget` (代入可) |

- `trim` は**予算そのものは変えない** (一時的な切り詰め)。0 で「使われていない
  ものを全部」。恒久的に下げるなら `atlasCacheBudget` へ代入する。
- ⚠ **表示中の画面が使っているアトラスは参照が残るので捨てられない**。
  場面の切れ目 (画面を閉じた後) に呼ぶこと。

### 計測 (2026-09-12、WINVER x64 Release)

サンプル **シナリオ 3**。1536x1536 のアトラスを 8 枚 (RGBA 展開で 72MB) 使う
画面を開き → 閉じ → `trimAtlasCache(0)` の順に踏んで常駐量を記録する。
資材は `-genassets` で生成する (リポジトリには置かない)。

| 段 | 常駐 (bytes) | |
|---|---|---|
| 画面を開いている間 | 75,497,472 | = 72MB (8 × 1536×1536×4) |
| **画面を閉じた後** | **75,497,472** | **閉じても 1 バイトも減らない** (従来の問題) |
| `trimAtlasCache(0)` 後 | **0** | 解放 75,497,472 バイト |

「閉じただけでは減らない」ことと「口を叩けば落ちる」ことの両方が数字で出た。

### 本体キャッシュとの関係 (確認結果: 健全)

Elements は `TVPReadStream` でバイトを読んで ThorVG で自前デコードするため、
本体の **decode 層キャッシュ**は経由しない。**file 層キャッシュ**は
`Storages.addCacheTargetExtension` によるオプトイン方式
(`StorageIntf.cpp:117`) なので、案件が `.png` を登録していなければ二重常駐は
しない。**ただし登録していれば「圧縮バイト (本体) + 展開 RGBA (Elements)」が
同時に載る** — 案件側の設定は一度確認する価値がある。

場面をまたぐフォント face (`glyph_layout_gw.cpp`) も解放されない。件数は
少ないはずだが、`fonts.json` の遅延ロードで増えた分がそのまま残るので、
常駐量の計測対象には入れる。

### 残件

- 案件側で「どの場面の切れ目で `trimAtlasCache()` を呼ぶか」を決める必要がある
  (engine は口を出すだけで、呼ぶ場所はゲーム側の責務)。
- 予算 192MB が妥当かは実案件で見る。`atlasCacheStats` で常駐量が読めるように
  なったので、実際の画面構成で測れる。

---

## 4. ✅ `text_metrics` キャッシュの全 clear (2026-09-12 対応)

### 現状

```cpp
// lib/src/support/canvas.cpp:869-877
static std::unordered_map<std::string, text_metrics> cache;
...
if (cache.size() > 4096)
   cache.clear();
```

### 問題

4096 件を超えた瞬間に**全部捨てる**ので、直後のフレームで全テキストの計測を
やり直す。再現性のあるフレーム落ちになる。

### ✅ 対応済み (2026-09-12) — ただし計測では差が出せなかった

上限に達したら**直近の使用が古い順に 1/4 だけ捨てる**ようにした
(`nth_element` で閾値を求めて erase。上限到達時だけなので頻度は低い)。
エントリに `last_use` を持たせ、ヒット時に更新する。

#### 計測 (2026-09-12、WINVER x64 Release)

サンプル **シナリオ 4** を「安定ラベル 120 個 (毎フレーム同じ文字列 =
キャッシュがヒットする側) + 変動ラベル 40 個 (毎フレーム新しい文字列 =
キャッシュを埋める側)」で組み、1 フレーム単位のラスタ時間の最大値
(`frame_spike_us`) を見た。生成キー数はラスタ数 × 40 で 6000 件、
上限 4096 を確実に跨いでいる。

| | 平均 us/raster | frame_spike_us |
|---|---|---|
| 修正前 (全 clear) | 2156 / 2152 | 3660 / 5174 |
| 修正後 (LRU 1/4) | 2142 | 3911 |

**差が出ない。** フレーム間のばらつき (平均の 1.5〜2.4 倍) が、跨いだ
フレームの追加コスト (安定 120 個の測り直し ≒ 1ms) と同じ桁のため。

#### なぜ測れなかったか / どう測れば見えるか

このシナリオは**変動ラベルが多すぎる**。全 clear で余計に発生する測り直しは
「安定ラベル 120 個」だけで、定常状態でも毎フレーム 40 個は測っているので、
比が小さい (6000 回中の 120〜240 回 = 2〜4%)。

崖が痛いのは「**安定したラベルが大量にあり、churn はゆっくり**」な画面
(通常のゲーム画面に近い形)。ただしそれを再現しようとすると 4096 件に到達する
まで数分かかるので、短い自動テストには載せにくい。上限を実行時に下げられる
口があれば短時間で再現できる。

#### 判断: 残す

- 全 clear は**構造として崖を作る**(working set を丸ごと捨てる)。LRU なら
  直近使っているエントリ = 安定ラベルが残るので、崖が原理的に消える。
- 変更は 30 行程度で局所的、定常状態のコストは変わらない
  (ヒット時の `last_use` 更新だけ)。
- `measure_text` が高価な低速機 (コンソール等) では、跨いだフレームの
  測り直し 4096 件ぶんの差はデスクトップよりずっと大きく出る。

**「デスクトップでは効果を計測できなかった」ことを明記した上で残す**。
元の「4096 件で全 clear」自体も計測に基づいた値ではなかった。

#### 📌 残件: 実際の画面での効果計測

このサンプルでは崖を再現できないので、**効果の確認は実案件の画面で行う**。
狙いどおりなら「安定ラベルが大量にあり churn がゆっくり」な画面ほど効く。

- **実案件の画面で計測する。** 合成された試験画面ではなく、実際に安定ラベルが
  多い画面 (メニュー / ステータス / 一覧など) を長時間出しっぱなしにして、
  上限を跨ぐ瞬間のフレーム落ちが消えるかを見る。
- **コンソール機 (CS) でも計測する。** `measure_text` のコストがデスクトップ
  よりずっと高いので、差が最も出るのはこちら。デスクトップで見えなかった
  ものがここで見える可能性がある。
- 見かた: 上限到達は「起動からの累計で 4096 種類の文字列を測った時点」なので、
  長時間プレイで初めて跨ぐ。短いテストでは踏まない。`System.renderStats` /
  `ElementsDialog.renderStats` の 1 フレーム最大値か、体感のフレーム落ちで見る。
- 上限を実行時に下げられる口 (`ElementsDialog` 側の設定など) があれば短時間で
  再現できるので、計測するならそれを先に足すのが早い。

---

## 5. ✅ 押しっぱなしガードの棚卸し (2026-09-12 実測)

「押しっぱなしのボタンが別の画面に効いてしまう」という**同じ 1 つの不変条件**に、
4 つの独立機構が当たっている。当初は「#4 が入った今 #3 は冗長では」と見立てて
いたが、**実測したら逆だった**。

| # | 機構 | 場所 |
|---|---|---|
| 1 | `armed_vks` | engine `ElementsDialogManager.cpp:3285` |
| 2 | `set_pad_nav_active(false)` / `suspend_pad_nav` | engine `c9fb4cb6` + elements `14e24c6e` |
| 3 | suspend 時に `st.current` をクリア | elements `6e976b9f` (`view.cpp`) |
| 4 | パッド離しの全インスタンス配送 | engine `c23061cb` |

### 計測方法

サンプル **シナリオ 5**。十字右を押しっぱなしにしたまま前面パネルを開閉し、
**戻った後に背面のフォーカスが動くか** (`after_restore`) を数える。
小ケースを 2 つに分けた:

- **holdcase 0** … 覆われている**間に離す** (前面にしか離しが届かない状況)
- **holdcase 1** … 覆われている間ずっと**押したまま**復帰する

### 結果 (WINVER x64 Release、`after_restore` の値)

| #3 | #4 | holdcase 0 | holdcase 1 |
|---|---|---|---|
| ON | ON | 0 | 0 |
| **OFF** | ON | 0 | **1** |
| ON | **OFF** | 0 | 0 |
| **OFF** | **OFF** | **4** | **1** |

- **holdcase 0 は #3 / #4 どちらか一方で防げる** — ここは確かに二重化
- **holdcase 1 は #3 だけが防ぐ**。#4 は「離し」が発生しないので原理的に無力
- したがって **#3 が主、#4 は holdcase 0 に対する二重化**。
  当初の見立て (#4 があるので #3 は冗長) は**逆だった**

### 判断: 両方残す

- **#3 は外せない。** holdcase 1 を防ぐのはこれだけ。
  「覆われている間ずっと押していた方向キーは、戻っても押し直すまで効かない」
  という挙動は、サブ画面を開いた後に背面が勝手に動かないという意味で妥当。
- **#4 は #3 があれば実測上は不要**だが、「離しが届くのが suspend フラグの
  適用より前」という狭いレースを埋める二重化として残す。コストはパッドの
  離し 1 回あたりインスタンス数ぶんのループで無視できる。
- ただし **#4 のコメントが誤解を招く** (「覆われている間に来た離しが前面にしか
  届かない」問題の対処と読めるが、それは #3 が既に塞いでいる)。コメントを
  実態に合わせて直した。

### ⚠ ハーネスの落とし穴 (記録)

最初の計測は **4 通りすべて 0** で、`#3` を外しても `#4` を外しても暴走が
出なかった。原因は**グリッドが端で clamp していた**こと — フォーカスが
右端 (`g0_3`) に着いた時点で移動先が無くなり、「押しっぱなしで回り続ける」
暴走が起きても画面上の変化として現れない。`focus_wrap: true` を入れて
初めて陽性対照 (両方 OFF で `after_restore=4`) が取れた。

**「壊しても壊れない検査」は検査になっていない。** ガードを外した状態で
症状が出ることを先に確かめること (陽性対照)。

### ⚠ ビルドツリーの落とし穴 (記録)

`src/core/build/x64-windows-win` と umbrella の `build/x64-windows-win` の
**2 つのビルドツリーが存在する**。`cd src/core` した状態で
`cmake --build build/x64-windows-win` を実行すると前者が更新され、
umbrella 側の exe は古いまま — それに気付かず「#4 単独では防げない」という
誤った結論を一度出した。**ビルドは必ずリポジトリルートから実行する。**


## 6. ✅ リピート周期の一本化 (2026-09-12 対応)

### 現状

| 入力 | 遅延 / 間隔 | 決めている場所 |
|---|---|---|
| キーボード ↑↓ | OS の設定 | Windows / SDL |
| パッド十字 | 400ms / 60ms | `view::_axis_repeat_delay_ms` (画面 JSON `input.repeat_delay_ms` / `repeat_rate_ms`) |
| (エンジンの VK_PAD リピート) | 500ms / 30ms | `-paddelay` / `-padinterval` |

### 問題

3 段目は **Elements のナビ速度に一切効かない** (軸値の再代入にしかならない)。
`-paddelay` を変えても UI のカーソル送りが変わらないのは、オプション名から
期待される挙動と食い違う。

### ✅ 対応済み (2026-09-12)

`-paddelay` / `-padinterval` が**明示指定されたときだけ**、その値を Elements の
軸リピート既定として流し込む。

- elements 側に `overlay_session::set_axis_repeat_default(delay_ms, rate_ms)`
  を追加。**`start()` より前に呼ぶ**契約で、`start()` の中で
  `input_defaults.jsonc` の後・画面別 `"input"` の前に当てる。
- エンジン側は `SessionOptions::padRepeatDelayMs / padRepeatRateMs` を追加し、
  `BuildSession` が未指定なら起動オプションを読む
  (`PadRepeatFromCommandLine`)。overlay と `ElementsPanel` の両経路に効く。
- 優先順は **画面 JSON > 起動オプション > `input_defaults.jsonc` > 組込既定**。
- 未指定なら従来どおり 400ms / 押し込み量連動。**既定の操作感は変えない**。

キーボードの矢印キーは OS のリピート設定のままとした (テキスト入力と同じ
リズムであることがユーザの期待で、ここを UI 独自周期にすると違和感が出る)。
「キーボードとパッドで送り速度が違う」ことは仕様として受け入れる。

### 計測 (2026-09-12、WINVER x64 Release)

サンプル **シナリオ 6**。十字右を**押しっぱなし**にして (`Agent.keyDown` して
離さない)、フォーカスの送り間隔を測る。dpad は `on_pad_button` で軸値になり、
その後の送りは elements のタイマが作るので、押し続けていれば追加入力なしで
送られ続ける。

| | 画面 JSON 指定なし | 画面 JSON が指定 (300/120) |
|---|---|---|
| 起動オプションなし | 平均 234ms | 平均 225ms |
| `-paddelay=1000 -padinterval=200` | 平均 **608ms** | 平均 **217ms** |

- 画面が指定していなければ起動オプションが効く (234 → 608ms)
- 画面が指定していれば起動オプションを付けても変わらない (225 → 217ms)
  = **画面 JSON > 起動オプション** の優先順が実証できた

### ドキュメント

- `doc/guide/CommandLine.md` の `-paddelay` / `-padinterval` に波及を追記
- `resource/optiondesc*.json` (4 言語) の説明にも 1 文追記
- elements_modal `README.md` の `"input"` ブロックに優先順を明記

### 📌 保留: リピート «処理» そのものの統合 (2026-09-12 時点で検討中)

上の対応は「生成場所はそのまま、設定値だけ揃える」もの。リピートの**処理
自体**を 1 か所へ統合する案も検討したが、結論は出していないので案だけ残す。

現状の 3 系統:

| 入力 | リピートを作っているもの | クロック |
|---|---|---|
| キーボードのナビキー | **OS** | OS の設定 |
| パッド十字 | elements `view::process_pad_axes` | `axis_repeat` |
| スティック | elements `view::process_pad_axes` | `axis_repeat` |
| (VK_PAD* のキーリピート) | engine `tTVPKeyRepeatEmulator` | `-paddelay` / `-padinterval` |

最後の 1 つは Elements のナビ送りには効かない (軸値の再代入にしかならない)。

#### 案 A: エンジンが全部作る

**そのままでは成立しない。** engine のリピート生成器は 24bit の**ボタン状態**
からしか作らず、スティックが対象外。このまま寄せると「十字 = engine クロック /
スティック = elements クロック」となり今より不統一になる。成立させるには
engine 側に「軸値からリピートを合成する」機構を新設することになる。

#### 案 B: elements が全部持ってエンジンへ流す — **成立しない**

「elements が権威になり、エンジンの現行処理は消す」形。**これは採れない。**

- パッドのキーリピート (`VK_PAD*` + `ssRepeat`) は**ダイアログが 1 枚も
  開いていない状態でもゲーム側 TJS の `onKeyDown` に届く必要がある**。
  elements が権威だと、UI が出ていない間はリピートを作る主体が居なくなる。
- 「UI が無くても elements のタイマだけ回す」形にすると、エンジンのコア入力が
  UI ライブラリに依存する。elements は krkrz 非依存の汎用ライブラリなので
  依存の向きとして逆。

なお「キーボードの OS リピートを飲み込んで elements が held 状態を自前で
追う」部分だけを取っても、**キーアップの取りこぼしでフォーカスが走り続ける**
事故の範囲をキーボードへ広げることになる (§5 のガード群が守っているのが
まさにそれ)。テキスト入力は OS リピートのままにしたいので分岐も要る。

#### 案 C (推奨): 生成は今のまま、**周期だけ揃える** = スロットル

engine は `TVP_SS_REPEAT` を持ち、elements には `key_action::repeat` が元から
ある (`base_view.hpp:120`) が、ブリッジの `on_key_down(key, mods)` が OS リピート
を潰していて区別できない。そこで

1. ブリッジが `on_key_down` に repeat フラグを渡す (引数 1 本)
2. `view` がナビキーの repeat に `axis_repeat` と同じ delay/rate を適用し、
   早すぎる分を**捨てる**

**イベントを生成せず落とすだけ**なので、押しっぱなしが暴走する経路が増えず、
held 状態を自前で持つ必要もない。キーボードとパッドが同じ設定値で揃う。
制約は「OS より遅くはできるが速くはできない」こと。
規模はブリッジ 1 引数 + view 30 行程度、検証はシナリオ 6 にキーボード
押しっぱなし版を足すだけ。

#### 結論: 完全統合するなら「エンジンが権威」しかない

権威を片方に寄せる 2 通りのうち、**案 B は上記のとおり成立しない**ので、
残るのは案 A の方向 (エンジンが全部持って elements へ流す)。ただし *全部* の
中身は 2 点に限定される。

- **エンジンが持てるのは «リピートのクロック» までで、軸の話全部ではない。**
  スティックの value モード (スライダを倒し量で連続的に動かす / デッドゾーン /
  倒し量連動レート) は**毎フレームの生の軸値**が要るので、その経路は elements
  に残る。離散リピートへ潰すと value モードが壊れる。
  → 「ナビの送りはエンジンが刻む / 軸値は従来どおり流す」の二本立てになる。
- **elements 側の機構は «消す» のではなく «切れるようにする»。** elements は
  krkrz 非依存の汎用ライブラリで、他のホスト (`modal.cpp` / Win32 ランナ) から
  も使う。タイマを削るとホスト単体でパッドナビのリピートが無くなるので、
  「ホストがリピートを駆動する」モードを足して既定は従来のまま、が正しい形。
  背面 view 用の `suspend_pad_nav` が既にあるので、その仲間として素直に入る。

**エンジン側に足りないもの**: 現行の `tTVPKeyRepeatEmulator` は 24bit の
**ボタン状態**からしか作らず、スティックが対象外。「全部持つ」にするなら
**軸値からリピートを合成する処理を新設**することになる。デッドゾーンと
倒し量連動レートの規則は今 elements 側にあるので、その移設が実質の作業量。

**得られるもの**: 全入力源を見ているのはエンジンだけ (キーボード / ボタン /
軸)、**リピートを止めるべきライフサイクル事象** (ウィンドウ非アクティブ /
モーダル開閉) を持っているのもエンジン — §5 のガードが分散しているのは権威が
分かれているからでもある。ゲーム側 TJS の `onKeyDown` リピートと UI の
リピートが同じ設定で揃うのも利点。キーボードが既に「OS が権威、elements は
消費するだけ」で動いているので、その一般化としても筋が通る。


---

## 7. ✅ フェイスボタンの二重配送 — 仕様として明記 (2026-09-12 対応)

`ElementsInputMap.h:129-134` のとおり、**同じ物理ボタンが刻印基準
(`VK_PAD1..4` → `a`/`b`/`x`/`y`) と位置基準 (`VK_PAD_FACE_*` →
`face_south`/`face_east`/`face_west`/`face_north`) の 2 系統で届く**。

これは「刻印で指したい」(Xbox の A / PS の ×) と「位置で指したい」
(下のボタン) という**概念の違い**であり、どちらも必要なので統合はしない。
実害が出るのは**画面が同じ物理ボタンに両系統をバインドしたとき**だけで、
その場合 1 押しで 2 回発火する。

### ✅ 対応済み (2026-09-12)

**明記した場所**

- elements_modal `README.md` の `"bindings"` の `pad` 一覧に注意を追加
- `doc/ElementsDialog.md` のパッド設定節に注意を追加
- umbrella `doc/topics/core/gamepad.md` — もともと「1 回の押下で刻印側と位置側の
  両方のキーイベントが届く」ことは書かれていたので、**その帰結** (同じ物理
  ボタンに両系統を割り当てると 1 押しで 2 回発火する) を追記

**警告も入れた**

画面 JSON の pad バインド (`pad_bindings` / `shortcuts`) を build 時に走査し、
刻印基準 (`a`/`b`/`x`/`y`) と位置基準 (`face_*`) を**同じ画面で併用**していたら
`em_logf` で注意を出す。

⚠ 「`a` と `face_south` が同じ物理ボタン」とは**限らない** — どの刻印がどの位置
かはコントローラ依存 (任天堂系は A が右・B が下) なので、静的には「両方使って
いる」ことまでしか言えない。そのため断定せず注意に留めている。

---

## 確認済みで問題の無かったもの (記録)

- dpad の二重ステップ — 起きていない (§0)
- `canvas` (PSD 絶対配置) の描画カリング — `composite_base::draw` へ委譲で正常
- `list` の窓方式 — データ件数に対して O(1)。設計は正しい
- `static_text_box` のクリップ打ち切り — elements `3deb85fd` で上端/下端が対になった
- Elements のアトラスと本体 decode 層キャッシュの二重保持 — 経路が別で発生しない (§3)
