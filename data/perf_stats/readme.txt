perf_stats — 画面転送コストとメモリの計測
==========================================

「fps は出ているのに重い」を数値で切り分けるためのデモ。
合成済み画面を GPU テクスチャへ送るコスト (System.renderStats) と、
メモリ / アロケータの状態 (System.getSystemAllocatorInfo) を、実際に負荷を
かけながら 500ms ごとに差分表示する。

起動:
  krkrz64.exe <このフォルダ>
  まとめて切り替える版は ../gallery (コアデモギャラリー) を参照。

■ 1. 画面転送 (System.renderStats)

  renderStats は累積カウンタなので、2 回読んで差分を取り、経過実時間との比
  で見る。 このデモが表示している値:

    提示フレーム     フレーム数 / 秒
    転送回数         テクスチャ転送の呼び出し回数 / 秒 (= ダーティ矩形の数)
    転送量           転送バイト数 / 秒
    転送 1 回あたり  平均バイト数と平均所要時間
    フレーム当たり   1 フレームで転送に使った時間
    転送時間         1 秒あたり転送に使った時間 (ms)
    転送率           転送時間 / 経過実時間

  ★ 転送率が高くても fps が出ているなら、待ちの多くは **vsync 同期ぶん**
     (次のフレームを待つ時間が転送呼び出しの中に現れている)。 本当に転送が
     足を引っ張っているときは fps が落ちる。 表示のヒント行はこの判定を
     出している。

■ 2. 負荷パターン

  パネルの「負荷」スライダで切り替える。 同じ 60fps でも転送コストが
  まったく違うことが分かる (以下は Windows / SDL3 / GLES(ANGLE) の実測例)。

    0: なし (静止)      6 回/秒     1.8 MB/秒   転送率 3.0%
       … 統計カードの再描画ぶんだけ

    1: 小矩形 x60       4192 回/秒  6.0 MB/秒   転送率 3.2%
       … 小さいダーティ矩形を毎フレーム 60 個。 呼び出し回数は 700 倍
         なのに、転送量が小さいので総コストは安い

    2: 全面塗り         60 回/秒    55.1 MB/秒  転送率 92.8%
       … 440x502 を毎フレーム全面更新。 呼び出しは 1 回/フレームでも
         1 回が 940KB あり、転送 (と vsync 待ち) で時間の大半を使う

    3: 中矩形アニメ     70 回/秒    4.3 MB/秒   転送率 3.2%
       … 同じ絵の変化量でも「消す矩形」「描く矩形」だけを update すれば
         2 の 1/13 の転送量で済む。 **動いた所だけ update する**の効果

  → 「更新の面積」ではなく「1 回あたりの転送量 × 回数」で決まる。
    全画面を毎フレーム塗り直す作りは、それだけで転送に張り付く。

■ 3. 転送経路の A/B (System.texUploadUsePBO)

  0 = 既定 (転送サイズ 256KB 以上なら PBO、未満は直接転送)
  1 = 常に PBO / 2 = 常に直接転送

  切り替えると計測もリセットされる。 なお Windows の GLES 実装 (ANGLE) は
  サイズによらず常に直接転送になるため、この環境では 1 と 2 で差が出ない
  (実測でも 15.4ms / 15.4ms で同値)。 差が出るのはネイティブ GLES の環境。

■ 3-b. ビルドによる転送方式の違い

  上の数値は SDL3 / GLES (ダーティ矩形ごとに glTexSubImage2D) のもの。
  WINVER (Direct3D 11) も 2026-08-16 からダーティ矩形単位の
  UpdateSubresource になっており、同じように負荷で数値が変わる。

    WINVER 実測 (1280x720 / 120Hz):
      なし (静止)    1.8 MB/秒・転送率 0.0%
      小矩形 x60     44.3 MB/秒・2.1%
      全面塗り       45.8 MB/秒・1.2%
      中矩形アニメ   9.5 MB/秒・0.4%
    (差分更新化する前はどの負荷でも 421.9 MB/秒・転送率 5% で一定だった。
     詳細は src/core/doc/D3D11Migration.md の追補節)

  ※ WINVER の「提示フレーム」は合成フレームを描いた回数。 画面に変化が無い
    フレームでは転送そのものを行わないので、転送回数が 0 になることがある。

■ 4. メモリ / アロケータ

  System.getSystemAllocatorInfo() の値を表示する。 **取得できない項目は
  キー自体が辞書に無い**ので、`"usedSize" in m` のように存在を見てから読む
  (デモの sampleMemory() がその書き方の実例)。

  パネルのボタン:
    doCompact          … System.doCompact(clAll)。 各種キャッシュ解放 + GC
    画像キャッシュ破棄 … System.clearGraphicCache()
    メモリピークをリセット … System.resetMemoryPeak() (peak を現在値に)
    メモリオーバレイ   … System.setMemoryOverlay() (SDL3 ビルド限定)

■ 関連リファレンス

  src/core/doc/ScreenTransfer.md
    画面転送コストの経路・計測・数値の読み方 (バリアント差の注意点も)
  doc/reference/System.md
    renderStats / renderStatsReset / texUploadUsePBO /
    getSystemAllocatorInfo / resetMemoryPeak / doCompact /
    clearGraphicCache / setMemoryOverlay / beginAllocTag / endAllocTag
  Elements overlay 側の内訳は Dialog.renderStats (コアデモ elements_bench)

■ メモ

  - 統計カード自体の再描画も転送を発生させる (負荷 0 でも 6 回/秒)。
    計測のベースラインとして頭に入れておく。
  - 転送はドライバ都合でブロックすることがあり、その待ちも時間に含まれる。
