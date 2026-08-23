# Layer / Bitmap / ImageFunction の統合 — 設計検討メモ

**状態**: 方針決定・着手待ち (2026-08-18)。**§6 の推奨順 (P1 → P2 → P3 → P4 判断) で進める**。
プロトタイプ検証は実施済みだがコードは未マージ。実作業の着手は後日。

## 1. 解こうとしている課題

### 課題 A: TJS から見て API が二重で書きづらい

同じ画像処理が `Layer` のインスタンスメソッドと `ImageFunction` のクラスメソッドに分裂している。

- `ImageFunction` は 13 メソッド (`operateRect` / `operateStretch` / `operateAffine` / `copy9Patch` /
  `fillRect` / `colorRect` / `drawText` / `drawGlyph` / `doBoxBlur` / `adjustGamma` / `doGrayScale` /
  `flipLR` / `flipUD`) で、**すべて static、対象は Bitmap のみ**。
- Layer 版が自分のプロパティ (`face` / `holdAlpha` / `clip*` / `font`) から埋めていた値を、
  ImageFunction 版では呼び出し側がすべて明示的に並べる必要がある。第 1 引数が `dst` で書き味も違う。
- 機能差もある。`copyRect` / `stretchCopy` / `affineCopy` 相当は `operate*` + `omOpaque`/`dfOpaque` で
  代用、`drawShapedText` 系は Bitmap 側に存在しない。
- `omAuto` は演算元 Layer の `type` から合成方法を決める指定なので、Bitmap 相手では常に `omAlpha` に落ちる
  (`ImageFunction.cpp` / `LayerIntf.cpp` の `automode = omAlpha` 既定)。

### 課題 B: プラグインが Bitmap を扱えない

Layer に触れているプラグインは **26 ファイル**。詰まりは 3 種類ある。

| 種別 | 内容 | 該当 |
|---|---|---|
| 対象判定 | `tTJSNC_Layer::ClassID` / `IsInstanceOf("Layer")` 決め打ち | 大半 |
| class dispatch | `TVPExecuteExpression("Layer")` でクラスのアクセサをキャッシュし、インスタンスに適用する。Bitmap を渡すと `TJS_GET_NATIVE_INSTANCE(tTJSNI_BaseLayer)` が失敗する | `layerExBase.hpp` を持つ 5 本 (krkrthreepp / layerExDraw / layerExImage / layerExRaster / layerExVector) + `layerExBTOA` |
| Layer 専用メンバ | `update` / `clip*` / `hasImage` / `left,top` / `province*` に依存 | krkr_richtext, layerExSave, shrinkCopy, windowEx ほか |

ncbind で Layer にメンバを登録しているのは **14 ファイル / 11 プラグイン**
(layerExSave 12+3+1+1 / layerExBTOA 8 / shrinkCopy 2 / tftSave / layerExVector / layerExRaster /
layerExLongExposure / layerExImage / layerExDraw / layerExAreaAverage / krkr_richtext 各 1)。

なお `TVPExecuteExpression("Layer")` の用途は 2 通りあり、**登録目的** (layerExSave / layerExLongExposure /
steam) と **dispatch 目的** (layerExBase.hpp 5 本 / layerExBTOA) を混同しないこと。改修が要るのは後者。

## 2. 現状の構造

TJS から見えるクラスは 3 つだが、ピクセルの実体は `tTVPBaseBitmap` 1 種類しかない。

- `tTJSNI_BaseLayer` … `MainImage` + `ProvinceImage` (どちらも `tTVPBaseBitmap*`) + 表示属性 + ツリー + 更新通知
- `tTJSNI_Bitmap` … `Bitmap` (`tTVPBaseBitmap*`) 1 枚のみ
- `ImageFunction` … 上記の描画プリミティブを Bitmap 向けに呼ぶだけの static 群

つまり**描画アルゴリズムは既に完全共有されていて、分裂しているのは「引数を誰が決めるか」と
「どのクラスにメンバが生えているか」だけ**である。

利用者向けの説明は umbrella の [doc/guide/LayerAndBitmap.md](../../doc/guide/LayerAndBitmap.md) にある。

## 3. A案 — 共通アクセス口を tp_stub に用意する

### 3.1 内容

1. **core に「TJS の画像オブジェクト → 共通アクセス interface」の解決関数を追加し、tp_stub に載せる。**

   既存の `iTVPScanLineProvider` (tp_stub 収録済み、トランジションハンドラで実績あり。core 側に
   `tTVPBaseBitmap` を包む実装が `TransIntf.cpp` にある) を土台に、Layer 固有の概念を吸収する口を足す:

   ```cpp
   class iTVPImageBufferAccess : public iTVPScanLineProvider
   {
       // GetWidth / GetHeight / GetPitchBytes / GetPixelFormat /
       // GetScanLine / GetScanLineForWrite は iTVPScanLineProvider から
       virtual tjs_error GetClipRect(tjs_int*l, tjs_int*t, tjs_int*r, tjs_int*b) = 0;
           // Bitmap では画像全体を返す
       virtual tjs_error NotifyUpdate(tjs_int l, tjs_int t, tjs_int r, tjs_int b) = 0;
           // Bitmap では no-op
       virtual tjs_error HasImage(bool *has) = 0;
           // Bitmap では常に true
       virtual tjs_error GetProvince(iTVPImageBufferAccess **out) = 0;
           // Bitmap では null
       virtual tjs_error IsLayer(bool *is) = 0;
   };
   tjs_error TVPGetImageBufferAccess(const tTJSVariant *obj, iTVPImageBufferAccess **out);
   ```

   `update` / `clip*` / `hasImage` / `province` を interface が吸収するので、**プラグイン側から
   Layer/Bitmap の分岐が消える**のが要点。

2. **Bitmap に Layer と同名・同引数順の描画メソッドを生やす。** `bmp.fillRect(...)` が書けるようになり、
   `ImageFunction` は既存互換の薄い shim に降格する (削除はしない)。

3. **プラグインを順次対応。** 対象判定だけの群 → Layer 専用メンバをガードする群 → class dispatch 群。

### 3.2 新規機構は不要

解決関数の中身は「Layer ClassID を試し、駄目なら Bitmap ClassID を試す」であり、
これは既に `LayerIntf.cpp` の `copyRect` / `operateRect` / `stretchCopy` / `operateStretch` /
`affineCopy` / `operateAffine` / `copy9Patch` の `src` 引数解決で本番稼働しているパターンそのもの
(`LayerIntf.cpp:7460` 付近)。したがって A案に技術的な未知はなく、プロトタイプ検証の対象外とした。

### 3.3 残る弱点

**メンバの登録先問題は解決しない。** プラグインが ncbind で Layer クラスに生やしたメソッドは
Bitmap からは呼べないままなので、**Bitmap にも同じものを登録する 2 行目**が各プラグインに要る
(14 ファイル)。また class dispatch 群 (6 本) は「どのクラスのアクセサを引くか」を実行時に
選び分ける改修が必要で、これは 1 行では済まない。

## 4. B案 — 共通ネイティブ基底クラス `ImageBuffer` を新設する

### 4.1 内容

`Layer` と `Bitmap` の共通ネイティブ基底クラス `ImageBuffer` を作り、描画メソッドをそこに一本化する。
`ImageFunction` は互換 shim に降格。プラグインは `ImageBuffer` に 1 回登録すれば両方に効き、
`layerExBase.hpp` の `ObjectCache` も `"Layer"` → `"ImageBuffer"` の差し替えで通る。

### 4.2 使う機構 (既存・未使用)

このエンジンの TJS2 は **ネイティブクラス同士の継承をすでにサポートしている**。

- `tTJSNativeClass : public tTJSExtendableObject` で、`SetSuper()` / `ExtendsClass()` を持つ
  (`tjsNative.h:185`, `tjsObjectExtendable.h`)
- `tTJSExtendableObject` の `FuncCall` / `PropGet` / `PropSet` / `NativeInstanceSupport` などは
  自分で見つからなければ **super へフォールスルーする** (`tjsObjectExtendable.cpp:33-143`)
- `tTJSNativeClass::CreateNew` は super のインスタンスを**別オブジェクトとして生成**し、
  `TJS_CII_SET_SUPRECLASS` で派生インスタンスに接続する (`tjsNative.cpp:414-450`)

core では現在まったく使われていないため、**初採用**になる。

### 4.3 プロトタイプ検証結果 (2026-08-18 実測)

`x64-windows` (SDL3) Release ビルドに `ImageBuffer` を仮実装し、`-replfile` で実測した。
検証後にツリーは元に戻してあり、**パッチのみ
[ImageBufferUnification.prototype.patch](ImageBufferUnification.prototype.patch) として同梱**
してある (未マージ・検証用。そのまま製品コードにする想定ではない)。再現手順:

```bash
cd src/core && git apply doc/ImageBufferUnification.prototype.patch
cd ../.. && cmake --build build/x64-windows --config Release
# 検証後は git -C src/core checkout -- . で戻す
```

パッチが触るのは 6 ファイル: `common/visual/ImageFunction.{h,cpp}` (ImageBuffer クラス本体) /
`common/visual/BitmapIntf.cpp`・`common/visual/LayerIntf.cpp` (後方参照の注入 1 行ずつ) /
`common/base/ScriptMgnIntf.cpp` (登録と `SetSuper`) / `common/tjs2/tjsObjectExtendable.cpp`
(instanceof 修正)。

| # | 検証項目 | 結果 |
|---|---|---|
| 1 | `SetSuper` で Layer / Bitmap に共通基底を持たせられるか | **○** `typeof global.ImageBuffer` = `"Object"`、インスタンス生成も正常 |
| 2 | 基底のメソッドが派生から呼べるか | **○** `bmp.fillRect(...)` が `ImageBuffer` 実装に解決 |
| 3 | 派生の同名メンバが基底を隠すか (互換性の要) | **○** `lay.fillRect(0,0,10,10,0xff112233)` は Layer 実装が動き、`getMainPixel` = `0x112233` |
| 4 | `NativeInstanceSupport` が super へフォールスルーするか | **○** Bitmap オブジェクトから `ImageBuffer::ClassID` を引けた |
| 5 | 基底に実処理を置けるか | **○** 後方参照経由で `ibFill(0xff00ff00)` → `getPixel` = `0x00FF00` |
| 6 | 既存 API への回帰 | **○** `ImageFunction.fillRect` / `Bitmap.getPixel` / `invalidate` すべて正常 |
| 7 | `instanceof` が基底名を見るか | **× → 修正で ○** (下記) |

**要注意の実測所見 2 件:**

**(a) メソッド呼び出し時の `objthis` は super インスタンスになる。**
`bmp.ibProbe()` の中で `objthis` から Bitmap/Layer を解決しようとすると
`ImageBuffer(super instance only)` しか取れなかった。基底クラスに実処理を置くには、
**派生の `Construct` から super のネイティブインスタンスへ後方参照を注入する**必要がある
(`tTJSNI_Bitmap::Construct` / `tTJSNI_BaseLayer::Construct` に 1 行ずつ)。
注入版は検証項目 5 のとおり正しく動いた。

**(b) `instanceof` は素の機構では super を見ない。**
`bmp instanceof "ImageBuffer"` が偽になる。原因は `tTJSExtendableObject::IsInstanceOf` が
`membername != NULL` のときしか super へ委譲せず、`membername == NULL` (オブジェクト自身の判定)
では派生オブジェクトの `ClassNames` しか見ないため。
`tjsObjectExtendable.cpp` の `TJS_CII_SET_SUPRECLASS` 処理で **super のクラス名を派生にも登録する**
修正を入れたところ、`bmp instanceof "ImageBuffer"` / `lay instanceof "ImageBuffer"` が真になり、
`bmp instanceof "Bitmap"` = 真 / `bmp instanceof "Layer"` = 偽 の判定にも副作用は出なかった。
**B案はこの TJS2 層の修正を伴う** (現状 `SetSuper` の利用者が皆無なので影響範囲は限定的)。

**(c) 生成コスト** (5000 回生成、`System.getTickCount` 実測)

| | ベースライン | 基底のみ | 基底 + instanceof 修正 |
|---|---|---|---|
| `new Bitmap(4,4)` × 5000 | 23〜25 ms | 28〜29 ms | 30〜32 ms |
| `new Layer(w, parent)` × 5000 | 125〜148 ms | 134 ms | — |

Bitmap 生成が **1 個あたり約 +1.3 µs (+約 30%)**。オブジェクトが 1 個増えるぶんの素直なコスト。
Layer 生成はもともと 1 個 25〜30 µs かかるのでノイズに埋もれる。
大量の Bitmap を毎フレーム作る使い方をしなければ実害は出ない見込みだが、**数値としては存在する**。

## 5. 比較

| 観点 | A案 | B案 |
|---|---|---|
| core の改修範囲 | 解決関数 + interface + tp_stub 追加 / Bitmap へメソッド追加 | 同左 + `ImageBuffer` クラス新設 + Layer/Bitmap の Construct に後方参照注入 + **TJS2 層 (`tjsObjectExtendable.cpp`) の instanceof 修正** |
| 技術的な未知 | なし (本番稼働中のパターン) | 検証済み。ただし core 初採用の機構に乗る |
| プラグインの対象判定 | interface 1 回で両対応 | 同左 (+ `instanceof "ImageBuffer"` も使える) |
| プラグインのメンバ登録 | **Layer と Bitmap の 2 箇所に登録が要る** (14 ファイル) | **`ImageBuffer` に 1 回**で済む |
| class dispatch 群 (6 本) | 各々でクラス選択の作り込みが要る | `"Layer"` → `"ImageBuffer"` の差し替えが基本線 |
| TJS の書き味 | `bmp.fillRect(...)` が書ける | 同左 |
| 実行時コスト | ゼロ | Bitmap 生成 +約 30% (+1.3 µs/個)、Layer 生成は誤差 |
| 互換性リスク | 低 (追加のみ) | 中。`instanceof` 挙動と `ClassNames` に手が入る。派生の同名メンバ優先は実測で確認済み |
| ロールバック | 容易 | クラス階層が入るぶん戻しづらい |

## 6. 推奨 (この順で進めることに決定)

**A案の 1 (共通アクセス口) を先に入れ、その上で B案へ進む**のが安全と考える。

- A案の 1 は B案でも土台としてそのまま使える (プラグインが「バッファ・クリップ・更新通知」を
  抽象越しに触る形は、基底クラスの有無と直交する)。
- 26 本のプラグイン分岐という一番痛い部分は、A案の 1 だけでほぼ解ける。
- B案の効き所は「登録先の一本化」で、これは**プラグインを実際に何本直すか**が決まってから
  判断しても遅くない。14 ファイルの二重登録が許容できるなら B案は不要とも言える。

段取り案:

1. **P1**: `iTVPImageBufferAccess` + `TVPGetImageBufferAccess` を core に実装し tp_stub に公開
2. **P2**: Bitmap に Layer 同名の描画メソッドを追加、`ImageFunction` を shim 化 (既存挙動は不変)
3. **P3**: プラグイン対応 — 対象判定のみの群 → Layer 専用メンバをガードする群
4. **P4**: 判断ポイント。class dispatch 6 本の改修コストを見て、B案 (`ImageBuffer` 基底) へ進むか、
   二重登録で済ませるかを決める

## 7. 未解決 / 要判断

- `ImageFunction` を将来的に非推奨にするか、恒久的に残すか (既存タイトルの互換性)
- `drawShapedText` 系を Bitmap 側にも展開するか (glyphware 側の対応要否)
- province (領域画像) を共通抽象に含めるか。Bitmap には存在しないので `GetProvince()` が null を
  返す形で吸収する案にしているが、`psdfile` のように Layer 前提で書き込む用途は結局対象外になる
- B案を採る場合、`ImageBuffer` を TJS からユーザが `new` できてしまう点の扱い (コンストラクタで例外を
  投げて抽象クラス扱いにするか)
- B案の Bitmap 生成コスト +30% を許容できるか (大量の小 Bitmap を作る実装があるかの確認)
