# DrawThreadPool 利用率の計測 (KRKRZ_DRAW_STATS)

`TVPBeginThreadTask` / `TVPExecThreadTask` / `TVPEndThreadTask` で構成される
描画用スレッドプール (`DrawThreadPool`) の利用率を atomic カウンタで採取し、
画面オーバレイで観察するための計測機能。

主な用途は **「メイン CPU だけ詰まって他コアが空いている」現象の原因特定**。
Switch 等の組み込み環境で典型的に発生するこの状況は、原因として:

- そもそも threading 経路に乗っていない描画処理が支配的
- `GetAdaptiveThreadNum` の閾値で 1 thread に落ちている率が高い
- thread sync (mutex/notify/spin) のオーバーヘッドが処理本体を超えている

の 3 系統が考えられるが、外から見ているだけでは区別できない。本機能で
1 秒換算の数値を見ることで、どのパターンに該当するかが切り分けられる。

設計と背景は `memory/handoff_graphics_mt_redesign.md` (内部 handoff) を参照。
本ドキュメントは使い方と出力の読み方を扱う。

> 「合成結果を GPU へ送る」側 (画面転送) のコストは別軸。そちらは
> [ScreenTransfer.md](ScreenTransfer.md) を参照 (`System.renderStats`)。

---

## 1. ビルド

CMake オプションで OFF/ON を切替。**デフォルト OFF (本番ビルドに影響なし)**。

```bash
# 通常ビルド (DRAW_STATS なし)
cmake --preset x64-windows
cmake --build build/x64-windows --config Release

# 計測有効化
cmake --preset x64-windows -DKRKRZ_DRAW_STATS=ON
cmake --build build/x64-windows --config Release
```

OFF 時は計測マクロが空展開されるので atomic 操作が一切生成されず、
コードサイズ・実行時オーバーヘッドはゼロ。

### 1.1 閾値チューニング `KRKRZ_THREAD_PIXEL_SCALE`

`GetAdaptiveThreadNum` 等の判定 `pixelNum >= factor * SCALE` の **SCALE** を
ビルド時に上書きできる:

```bash
# 旧 PC 挙動 (1T 多め) で動かしたい場合
cmake --preset x64-windows -DKRKRZ_THREAD_PIXEL_SCALE=500
```

- **デフォルト 100** (Switch 実機 500/200/100 比較で決定、2026-05-08)
- 500 (旧仕様) では Switch で 1T 99% / Main 過多 / Wkr 不活用
- 200/100 で NT 比率が増え、Main が下がり、FPS 改善 (Switch で +30%)
- 100 から下は頭打ち (画像サイズ分布の二極化で改善余地が消える)
- PC では SCALE=100 でも 1T が増える方向だが、CPU 性能に余裕があるので影響軽微

影響する callsite:
- `LayerBitmapIntf.cpp` の `GetAdaptiveThreadNum` (Layer 系 8 サイト)
- `gl/ResampleImage{,SSE2,AVX2}.cpp` の `pixelNum >= 50 * SCALE` 全 10 箇所

---

## 2. 表示の有効化

memoverlay (画面オーバレイ) が有効な状態で `KRKRZ_DRAW_STATS=ON` ビルドを
起動すると、自動的に追加 2 行が出る (専用の有効化フラグはない)。

memoverlay を出すには以下のいずれか:

```bash
# CLI で有効化
krkrz64.exe data/ -memoverlay=1
```

```tjs
// TJS API
System.setMemoryOverlay(true);
```

```
// REPL
.memoverlay
```

memoverlay 全般の使い方は `doc/MemoryGuide.md` を参照。

**描画デバイス依存**: memoverlay を描画するのは `SDLDrawDevice` /
`SDLOGLDrawDevice` / `OGLDrawDevice`。WINVER 既定の `BasicDrawDevice` (D3D11)
には描画フックが無いため表示できないが、**WINVER でも drawDevice を OGL 系へ
切り替えれば表示される** (GLES 直接描画版 `MemoryOverlayGL.{h,cpp}`)。
Switch ビルドでは memoverlay が表示される。

### 2.1 log への書き出し (`System.setDrawStatsLog`)

オーバレイの数値は 500ms ごとに変化するため、実機で目視記録するのが
難しいケースがある。その場合は TJS から log 出力を有効化できる:

```tjs
System.setDrawStatsLog(true);   // 以後 500ms ごとに log へ追記
// ...再現したい操作...
System.setDrawStatsLog(false);  // 停止
```

引数なしで呼ぶと toggle。出力例 (1 ティック 2 行ペア):

```
DrawStats: FPS=59.9 Draw=120/s 1T=30% NT=70% Wkr=12.5ms/s Main=18.3ms/s Spin=0.8ms/s
DrawSites: LayerBitmapIntf.cpp:242=80/78 LayerBitmapIntf.cpp:347=30/28 ResampleImage.cpp:523=10/0
DrawStats: FPS=59.8 Draw=118/s 1T=28% NT=72% Wkr=12.7ms/s Main=18.0ms/s Spin=0.9ms/s
DrawSites: LayerBitmapIntf.cpp:242=78/76 LayerBitmapIntf.cpp:347=28/26 ResampleImage.cpp:523=12/0
...
```

- **`DrawStats:` 行**: `DrawStats §4` のフィールドに加えて先頭に `FPS=N.N` (overlay 上の値と同じ)
- **`DrawSites:` 行**: 直近 500ms に `TVPBeginThreadTask` を呼んだ callsite の上位 3 件を `<file>:<line>=<count>/<t1_count>` で表示。
  - `count`: その site が dispatch された総回数 / 500ms
  - `t1_count`: そのうち `taskNum=1` (= 1 thread に落ちた) 回数
  - 1T% が高い (count ≈ t1_count) callsite が多発しているなら、その site の `factor` を下げるか別経路化を検討する手がかりになる
  - delta が 0 の slot はスキップされる。table 容量 32 (現状全コードで site 数 ~15)

`TVPAddLog` (INFO レベル) で出るので、`MASTER` ビルドでは log に乗らない
点に注意 (`MASTER` は WARNING 以上のみ)。実機調査では `MASTER` 無し +
`-loglevel=INFO` で実行すれば log ファイルに残せる。

**前提**:
- `KRKRZ_DRAW_STATS=ON` でビルドしてあること (OFF ビルドでは API 自体は
  存在するが、数値が常に 0 なので何も書き出さない)
- memoverlay が ON であること (polling が memoverlay 側 tick に乗っている
  ため)。WINVER build では memoverlay 自体が描画されないので log も出ない

500ms 単位なので 1 秒に 2 行、長時間取ると行数が増える点は留意。

---

## 3. 出力フォーマット

`KRKRZ_DRAW_STATS=ON` ビルドで memoverlay を有効化すると、既存の
7 行 (FPS / File / Bitmap / RSS / Alloc/s / FileCache / ImageCache) の下に
追加の 7 行が出る:

```
FPS:         59.9
File:       12.34 MB (peak max ...)
Bitmap:      8.21 MB (cumulative)
RSS:        180.5 MB
Alloc/s  File:    12  Bitmap:    34
FileCache: count=42 pinned=4
ImageCache: count=12 pinned=2
Draw      120/s   1T:30% NT:70%             ← DrawStats 1 行目
Wkr:12.5 Main:18.3 Spin: 0.8 ms/s            ← DrawStats 2 行目
TexUp:  45 Ren:  20 Copy:  240MB/s            ← RenderStats (テクスチャ更新)
Show Clr:  3 Tex:  5 Ovl:  2 Pres:120        ← ShowStats (Render() 内 section 別)
Frame Up: 380 Sho: 130 Dsp:  50              ← FrameStats (1 frame の main core 占有 3 分割)
Layer CmpW: 480 Cmp: 360 Drw: 280            ← LayerStats (Layer 合成 phase)
LayerEx Bef:  90 Aft:  20                     ← LayerExStats (Before/After Completion)
```

数値は **過去 500ms 間の累計を 1 秒換算** したもの。500ms ごとに更新される。

---

## 4. 各指標の意味

### 4.1 1 行目: `Draw   N/s   1T:X% NT:Y%`

| フィールド | 意味 |
|---|---|
| `N/s` | `TVPBeginThreadTask` 呼出頻度 (= 描画分散経路を通った回数 / 秒) |
| `1T:X%` | そのうち taskNum=1 (= 1 スレッド強制) で実行された割合 |
| `NT:Y%` | taskNum >= 2 (= 複数スレッド) で実行された割合 (= 100 - X) |

`GetAdaptiveThreadNum(pixelNum, factor)` は
`pixelNum < factor * KRKRZ_THREAD_PIXEL_SCALE` のとき 1 thread を返すため、
小画像の合成は 1T カウントに入る。SCALE のデフォルトは 100 (§1.1 参照)。

### 4.3 RenderStats 行: `TexUp:N Ren:N Copy:NMB/s`

DrawThreadPool **外**で main CPU を使う「テクスチャ更新パス」の計測。
DrawDevice 実装ごとに集計位置が異なる:

- `tTVPSDLDrawDevice` 経路 → `SDLTextureUpdateRect::Update / RenderToTexture`
- `tTVPOGLDrawDevice` / `SDLOGLDrawDevice` 経路 → `SDLOGLTextureUpdateRect`
  (PBO 直接 map のため `RenderToTexture` 相当のコピー段は実質ゼロ)

| フィールド | 意味 |
|---|---|
| `TexUp` | `Update()` (src→中間バッファ memcpy または PBO map) の累計時間 ms/s |
| `Ren` | `RenderToTexture()` (中間バッファ→GPU staging memcpy) の累計時間 ms/s。OGL 経路では ~0 |
| `Copy` | Update 側コピー量 (片方向) MB/s |

α フェード等で画面全体 dirty のとき、SDL_Renderer 経路では `TexUp + Ren` が
main CPU を 200-400 ms/s 消費する想定。OGL 経路は `GL_MAP_INVALIDATE_BUFFER_BIT`
による PBO orphan で TexUp 自体が大幅に減る (Switch 実機で 6-10 倍減を実証)。
**`Copy` が大きい (例 480 MB/s+) 場合は memcpy 帯域支配**で、中間バッファを廃して
直接 GPU staging に書く方向で短縮できる可能性あり。

`DrawStats:` ログ行と並んで `RenderStats:` 行も log 出力される (setDrawStatsLog 有効時)。

### 4.4 ShowStats 行: `Show Clr:N Tex:N Ovl:N Pres:N`

DrawDevice 各実装の `Render()` / `Show()` 内 section 別計測。
DrawThreadPool 外 + テクスチャ転送外で消費される main CPU の正体探索用。

`tTVPSDLDrawDevice` (SDL_Renderer 経路):
| フィールド | 意味 |
|---|---|
| `Clr` | `SDL_RenderClear` (背景塗り) の累計時間 ms/s |
| `Tex` | `SDL_RenderTextureRotated` / `SDL_RenderTexture` (本体描画) の累計時間 ms/s |
| `Ovl` | `TVPRenderMemoryOverlay` (memoverlay 描画) の累計時間 ms/s |
| `Pres` | `SDL_RenderPresent` (vsync 待ち + flush) の累計時間 ms/s |

`SDLOGLDrawDevice` / `tTVPOGLDrawDevice` (GLES 経路) では同じフィールド名で
GL コマンドベースの section に対応 (`Clr` = `glClear`、`Tex` = shader draw 一式、
`Ovl` = `TVPRenderMemoryOverlayGL`、`Pres` = `SwapBuffers`)。

Switch 等の vsync 同期環境では `Pres` が大きくなる可能性が高い (60fps なら
最大 16.7ms/frame = 1000ms/s)。`Tex` が大きい場合は GPU 側の texture upload
完了待ちが含まれる可能性 (per-rect SDL_UpdateTexture が driver async でキュー
された分が、描画コマンド発行時点で同期される)。

`DrawStats:` ログと並んで `ShowStats:` 行も log 出力される (setDrawStatsLog 有効時)。

### 4.5 FrameStats 行: `Frame Up:N Sho:N Dsp:N`

1 frame の main core 占有を 3 つの大きな phase に分けた値。Update / Show / Dispatch
それぞれの関数全体の経過時間を atomic 累積する。

| フィールド | 意味 |
|---|---|
| `Up` | `tTVPDrawDevice::Update()` 全体時間 (Layer 合成 + TexUp + TexRen を含む) |
| `Sho` | `tTVPDrawDevice::Show()` 全体時間 (Clr + Tex + Ovl + Pres を含む) |
| `Dsp` | `tTVPApplication::Dispatch()` 全体時間 (event 処理 / scenario engine など) |

**Up - (TexUp + TexRen)** が「未計測の Layer 合成パイプライン処理」(= NotifyBitmapCompleted
を呼ぶまでの dirty region 走査 / 各 layer 合成 dispatch / DrawThreadPool 経由の
main 部分) の時間を表す。

**Sho - (Clr + Tex + Ovl + Pres)** はほぼゼロのはず (差があれば section 計測の漏れ)。

`DrawStats:` ログと並んで `FrameStats:` 行も log 出力される (setDrawStatsLog 有効時)。

### 4.6 LayerStats 行: `Layer CmpW:N Cmp:N Drw:N`

Layer 合成パイプライン内の更に細かい phase。FrameStats の `Up` と組み合わせて
未計測時間がどこにあるかを段階的に絞り込む。

| フィールド | 意味 |
|---|---|
| `CmpW` | `tTJSNI_BaseLayer::CompleteForWindow` 全体時間 |
| `Cmp`  | `tTJSNI_BaseLayer::InternalComplete2` top-level only (再帰除外) の累計時間 |
| `Drw`  | `tTJSNI_BaseLayer::Draw` 累計時間 (再帰込み = Layer 木全体の合成時間) |

差分の解釈 (内側から外側へ):
- `Cmp - (TexUp + Main + Spin)` ≈ InternalComplete2 内の main only 処理
  (実測ほぼ 0 = Layer 木 traversal のオーバーヘッドは無視可能)
- **`CmpW - Cmp` ≈ BeforeCompletion + AfterCompletion + StartBitmapCompletion +
  EndBitmapCompletion + NotifyUpdateRegionFixed + GetUpdateRegionForCompletion**
  (= 未計測 ~200 ms/s の主たる場所)
- `Up - CmpW` ≈ UpdateToDrawDevice 経由の overhead (ほぼ 0 のはず)

`Drw` は再帰累積なので wall clock を超えうる (Layer 木が深いほど大きい)。改善余地
特定には `CmpW - Cmp` を見るのが有効。

`InternalComplete2` の再帰検出は `thread_local` な depth カウンタで行う
(LayerIntf.cpp 内、CompleteForWindow → InternalComplete2 → Draw → child->Draw →
親 InternalComplete2 という再帰経路を考慮)。

`DrawStats:` ログと並んで `LayerStats:` 行も log 出力される (setDrawStatsLog 有効時)。

### 4.7 LayerExStats 行: `LayerEx Bef:N Aft:N`

`CmpW - Cmp` の中身を更に分解 (Phase 9)。`BeforeCompletion` / `AfterCompletion`
は Layer 木全体に再帰的に呼ばれるが、ここでは top-level only で計測 (1 frame
1 回 + 子 Layer 全部の合計時間)。

| フィールド | 意味 |
|---|---|
| `Bef` | `tTJSNI_BaseLayer::BeforeCompletion` top-level only (Layer 木 traversal 累計) |
| `Aft` | `tTJSNI_BaseLayer::AfterCompletion` top-level only (Layer 木 traversal 累計) |

`CmpW - Cmp ≈ Bef + Aft + 微量` のはず。BeforeCompletion 内では `onPaint` event
fire や Transition 処理が発火可能性あり、AfterCompletion 内では Transition の
EndProcess。通常運用では両方 false で「何もしない if 文と再帰」だけ走る。それでも
Layer 数が多い VN ではこれが累積で 5-7 ms/frame になる仮説の検証用。

`DrawStats:` ログと並んで `LayerExStats:` 行も log 出力される (setDrawStatsLog 有効時)。

#### 注: NeedsCompletion フラグによる walk skip 案は撤回 (2026-05)

過去に `NeedsCompletion` フラグを各 Layer に持たせ、`BeforeCompletion` /
`AfterCompletion` 入口で false なら subtree 全 skip する最適化を試した
(e620aaa0 + cb255e8f)。Heavy で Bef 200 → 10 ms/s 程度を狙ったが、
**CG モードでの特殊な再描画制御で誤動作**することが判明し撤回 (1b032875)。

具体的には、スクリプト処理の途中で `onPaint` 等が呼び戻される経路があり、
その時点で確定していた `NeedsCompletion` 状態に基づき walk が skip された後、
スクリプトが行った後続の変更が最終フレームに反映されない (= フラグの再評価
タイミングが「スクリプト全部終わってから」を前提にしていたため、コール途中の
中間状態を捉えられない)。

Bef/Aft の負荷削減を再挑戦する場合は、以下のいずれかの設計が必要:
- script からの再入 (onPaint 中の TJS 実行) を考慮した flag 無効化経路
- flag 方式ではなく「dirty 通知側で必ず立てる」厳密 invariant を全変更経路で担保
- そもそも skip ではなく BeforeCompletion 本体の重い分岐を畳む方向

### 4.2 2 行目: `Wkr:N Main:N Spin:N ms/s`

| フィールド | 意味 |
|---|---|
| `Wkr` | worker (= メイン以外) のアクティブ時間累計 (ミリ秒/秒) |
| `Main` | メインスレッドが最後の 1 task を直接実行していた時間 (ms/s) |
| `Spin` | メインが `WaitForTask` で他 worker 完了待ちしていた時間 (ms/s) |

`Main` には DrawThreadPool **経由の** メイン作業時間しか入らない。
DrawThreadPool を通らない描画処理 (= スレッド化されていない経路) は
ここに計上されない (= 4 行目のすべてが小さくても、メインが激しく忙しい
可能性がある)。

`Spin` は最後のタスクを除く N-1 個の worker タスクが終わるまでの待機時間。
**現在は `condition_variable::wait` で実装されている** (旧実装は busy-spin)
ため、メイン CPU は wait 中 sleep する (CPU を焼かない)。worker 側は
最後の 1 個が完了したときだけ lock+notify、それ以外は notify を取らない。
名称 "Spin" は歴史的経緯で残置 (実態は "WaitForTask 滞在時間")。

#### 単位の留意点

- **ms/s = "そのスレッドが秒間に費やした実時間" (ms)**
- N コア環境で全コアが 100% 使われていれば各 ms/s は最大 1000、合計で N×1000 まで行きうる
- `Wkr` の上限は (N-1) × 1000 (DrawThreadPool は最後のタスクをメインに任せるので worker 数は N-1)

---

## 5. 典型的な読み方 / 判断パターン

### A. 分散経路に乗っていない

```
Draw      5/s   1T:90% NT:10%
Wkr: 0.2 Main: 1.5 Spin: 0.0 ms/s
```

- `N/s` が低い → そもそも `TVPBeginThreadTask` が呼ばれていない
- `Wkr` も `Main` も小さい → 計測対象外の処理がメイン CPU を占有している

**結論**: 描画ボトルネックは DrawThreadPool **の外** にある。
threading 化されていない別経路 (Layer composite / 動画デコード /
TJS スクリプト本体 / その他) を疑う。

### B. 1 thread に落ちる率が高い

```
Draw    150/s   1T:85% NT:15%
Wkr: 1.2 Main:18.5 Spin: 0.3 ms/s
```

- `N/s` 高い (描画頻発) のに `1T:` 高い → 大半の描画タスクが小画像
- `Main` が `Wkr` より大きい → メインが直接処理してる時間が長い

**結論**: `GetAdaptiveThreadNum` の閾値 (`factor*SCALE`) が
高すぎるか、callsite の factor 値が実機 CPU と合っていない可能性。
Switch のように単コア性能が PC より低い環境では、より小さい画像でも
threading 化したほうが得かもしれない。

改善方向の第一歩: **`-DKRKRZ_THREAD_PIXEL_SCALE=200` 等で閾値全体を下げて再計測**
(§1.1 参照)。`DrawSites:` ログで犯人 callsite を特定済みなら、callsite 単位の
factor 値や個別の threshold 表現を見直す手も取れる。

### C. sync overhead 支配

```
Draw    300/s   1T:20% NT:80%
Wkr: 8.5 Main:12.3 Spin:25.7 ms/s
```

- `NT:` 高い (multi-thread に行ってる)
- `Spin` が `Wkr` より大きい → メインが worker 待ちで詰まってる時間が長い
- メインの spin は busy-loop なのでメイン CPU 100% 占有

**結論**: thread dispatch / sync overhead が支配的。`WaitForTask` は
既に cv.wait 化されている (busy-spin は撤去済み) ので、Spin が大きい
ということは worker の最後の完了が遅い (= 個別タスクの平均完了時間が
長い) を意味する。改善方向: dispatch 単位の粒度を上げる (callsite 単位の
factor 値を上げる)、`SetTask` 経路の atomic 化、または NT 化を諦める
(SCALE を上げる方向)。

### D. 健全 (改善余地小)

```
Draw    100/s   1T:10% NT:90%
Wkr:18.5 Main:12.3 Spin: 1.2 ms/s
```

- `Wkr` が大きい (他 worker が稼働している)
- `Spin` が小さい (sync 待ちは少ない)
- `Wkr / (N-1)` ≈ `Main` (worker と main が均等に働いている)

**結論**: 分散がうまく機能している。DrawThreadPool 関連の改善余地は小さい。
ボトルネックは別の場所。

---

## 6. 実装メモ

### 6.1 計測ポイント

`common/utils/ThreadIntf.cpp` の DrawThreadPool 内に atomic カウンタを
仕込んでいる:

| カウンタ | 加算箇所 |
|---|---|
| `begin_count` | `BeginTask()` 入口 |
| `task_hist[k]` | `BeginTask(k)` 入口 (k は taskNum) |
| `worker_active_ns` | `DrawThread::Execute()` 内、task 実行の前後で `steady_clock::now()` 差分 |
| `main_active_ns` | `ExecTask()` 内、最後のタスク (= main 直接実行) の前後 |
| `wait_spin_ns` | `WaitForTask()` 入口〜出口の差分 |

すべて `std::memory_order_relaxed` の atomic operation。ホットパスに
1 nanosecond 級のオーバーヘッドが入るが、Switch 環境でも実害なし。

### 6.2 Snapshot API

外部から累計値を取得するための公開 API:

```cpp
struct TVPDrawThreadStatsSnapshot {
    tjs_uint64 begin_count;
    tjs_uint64 task_hist[TVPMaxThreadNum + 1];
    tjs_uint64 worker_active_ns;
    tjs_uint64 main_active_ns;
    tjs_uint64 wait_spin_ns;
    tjs_uint64 snapshot_tick_ms;  // steady_clock の ms tick
};
void TVPGetDrawThreadStats(TVPDrawThreadStatsSnapshot &out);
```

OFF ビルドではすべてのフィールドが 0 で返る (関数自体は常に存在)。
プラグイン等から取得して別 UI に出すこともできる。

### 6.2.1 callsite tracking

`KRKRZ_DRAW_STATS=ON` のとき、`TVPBeginThreadTask(num)` 呼出は header の
マクロで `TVPBeginThreadTaskAt(num, __FILE__ ":" __LINE__)` に置き換わる。
リテラルポインタをキーにした 32-slot の open-addressing table に
`(count, t1_count)` を atomic で蓄積する。スレッド競合時は CAS で site を
書き込み、衝突 slot は次へ probing する。

スナップショット API:

```cpp
const int TVPDrawCallsiteMax = 32;
struct TVPDrawCallsiteSnapshot {
    const char *site;     // null = slot 未使用
    tjs_uint64 count;
    tjs_uint64 t1_count;
};
void TVPGetDrawCallsiteSnapshots(TVPDrawCallsiteSnapshot out[TVPDrawCallsiteMax]);
```

callsite は順序保証なし (hash 順)。呼び出し側で前回 snapshot との delta を
取り、`count` で desc sort して上位 N を選ぶ。OFF ビルドでは全 slot null/0 を返す。

### 6.3 表示側

`sdl3/visual/MemoryOverlayRender.cpp` で前回 snapshot を static 変数に
保持しておき、500ms 経過したら現在値との delta を取って 1 秒換算する。
表示文字列は次のリフレッシュまで固定 (フリッカ防止)。

---

## 7. 制限事項 / 注意

- **DrawThreadPool を経由しない処理は計測対象外**
  Layer composite (`TVPLayerManager` 経由)、動画フレーム描画、SDL_Renderer
  自体の負荷、TJS スクリプト実行などは含まれない。これらは別の手段
  (FPS, RSS, OS のプロファイラ) で観測する。

- **Switch 上での steady_clock 精度**
  `std::chrono::steady_clock` の tick 単位は実装依存。Switch SDK の
  実装が ns 精度で動いていることは前提だが、未確認。tick 値が大きく
  ずれる場合は `Wkr/Main/Spin` も比例してずれる (相対値としては正しい)。

- **OFF ビルドでは追加行は出ない**
  パネル高は条件付きコンパイルで OFF/ON で切り替わる。同じ `data/`
  でも、`KRKRZ_DRAW_STATS=ON` でビルドし直さない限り表示は増えない。

- **計測の有効/無効を実行時切替できない**
  CMake オプションでビルド時固定。実行時切替は今のところ対応していない
  (実装規模に対して需要が見えないため)。

---

## 8. 関連ドキュメント

- `doc/MemoryGuide.md` — メモリ系の観測機能の使い方 (memoverlay 含む)
- `doc/MemoryDesign.md` — メモリアロケータ容量ネゴと内部実装
- `memory/handoff_graphics_mt_redesign.md` — 描画 MT 再設計 handoff
  (内部メモリ。MT2-MT5 の改善案、判断基準、撤退ライン)
