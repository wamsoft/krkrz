# メモリリーク監査ログ (2026-05)

2026 年 5 月に実施した kirikiri Z のメモリリーク監査と修正の作業ログ。
発見の文脈、踏める条件、修正方針を記録しておき、後で同種のバグを再現
させない / 退行検知の素材にする用。

メモリ系の常設ドキュメントは `doc/MemoryGuide.md` (使い方) と
`doc/MemoryDesign.md` (設計) を参照。本ログはあくまで「過去にここを
直した」スナップショット。

---

## 対象範囲と進め方

監査は 3 ラウンドに分けた:

| ラウンド | 範囲 | 結論 commit |
|---------|------|------------|
| R1 | `common/base/StorageCache.cpp` + `common/base/StorageIntf.cpp` (自動パス探索 / file 層キャッシュ) | `730ea93d` |
| R2 | `common/tjs2/tjsString*` + `common/tjs2/tjsVariantString*` (TJS 文字列 / heap pool) | `a33959b2` |
| R3 | `common/tjs2/` 全体 (Variant/Object/コンパイラ/VM/RegExp/Serializer 等 80+ ファイル) | `69328c55` |
| R4 | `common/visual/opengl/` 全体 (Canvas/Texture/Shader/Offscreen/VertexBuffer/OGLDrawDevice/GL ラッパ群 13 ファイル) | (本コミット) |

各ラウンドで:
1. 該当領域を読み、leak / use-after-free / 例外路で raw pointer が宙に
   浮くパターンを列挙
2. 「`for (auto x : container)` で値コピー → shared_ptr の use_count
   経由の外部参照判定が常に true」のような既知の落とし穴を grep で
   横展開
3. 確証なく報告すると誤検知になるので、各候補は実コードを開いて検証
4. 修正は MSVC で実ビルドして確認

R3 は範囲が広いので領域別に並列で精査エージェントを 3 本走らせ、その
報告を全件自分で検証 (= 「Variant ctor で AddRef される or されない」
等の前提条件をコードで読む) してから採否を決めた。エージェント報告に
は誤検知も混じっていたので、本ログには検証で確定したものだけを残す。

---

## R1: StorageCache の LRU 駆逐バグ + 二重登録

commit: `730ea93d`

### 確定した問題

#### (a) `TVPClearOldStorageCache` が常に no-op

`common/base/StorageCache.cpp:306` 旧コード抜粋:

```cpp
for (auto it : StorageCacheTable) {   // (A) value copy
    if (TVPStorageCacheEntryReferencedExternally(it.second)) continue;  // (B)
    if (...) {
        ...
        StorageCacheTable.erase(it.first);  // (C) range-for 中の erase
        ...
    }
}
```

二重に壊れていた:
- **(A)+(B)**: `for (auto it : ...)` は `pair<const ttstr, StorageCacheEntry>` を
  値コピーするので `it.second.buffer` の `shared_ptr` が use_count を
  +1 する。`TVPStorageCacheEntryReferencedExternally` は
  `buffer.use_count() > 1` で外部参照を判定しており、コピー自身を
  外部参照と誤判定して **全 entry が `continue` される** → 駆逐は
  起きない
- **(C)**: 値コピーを修正しても `range-for` 中の `.erase` は隠れ iterator
  を無効化する UB

同ファイルの `TVPClearAllStorageCache` / `TVPClearTransientStorageCache`
は明示 iterator + `it = .erase(it)` パターンに正しく書けており、
**この関数 1 つだけ古いパターンで取り残されていた**。

#### (b) cache thread の keepTime 単位ミス

`tTVPStorageCacheThread::Execute` で
`TVPClearOldStorageCache(StorageCacheKeepTime * 1000)` と呼んでいたが、
`keepTime` は `time(NULL) - keepTime` (秒単位) と比較されるため
`* 1000` で 30,000 秒 ≈ 8 時間。仮に (a) を直しても条件が満たされない。

`waitTick = StorageCacheWaitTime * 1000` (ms 単位) と隣接していて取り
違えた様子。

#### (c) CompactEventHook lazy 登録が race

`EnsureStorageCacheCompactHookRegistered` (StorageCache.cpp) と
`TVPGetPlacedPath` 冒頭 (StorageIntf.cpp) で `bool` フラグだけで保護
された lazy 登録があり、cache thread と main thread の初回呼び出しが
同時に走ると compact hook が二重登録される race があった。

### 修正

- (a): 明示 iterator + `it = .erase(it)` パターンに統一、`cutoff` を
  loop 外に括り出し
- (b): `StorageCacheKeepTime * 1000` → `StorageCacheKeepTime`
- (c): 2 箇所とも `std::once_flag` + `std::call_once` に置換、`bool`
  フラグを撤去

### 実害

- (a)+(b): cache thread の back-pressure と FileAllocator pressure
  callback (0.75/0.90) が **両方とも空振り**。`Compact` イベント
  以外では事実上 StorageCache は縮まず、大きなプロジェクトで
  `MaxStorageCacheSize` を超えたまま放置されていた

---

## R2: tjsString / VariantString

commit: `a33959b2`

### 確定した問題

#### (a) `TJSFormatString` 例外路で `ret` がリーク

`common/tjs2/tjsVariantString.cpp:686` の `TJSFormatString` は冒頭で
`ret = TJSAllocVariantStringBuffer(...)` を確保するが、本体内:
- **18 箇所** の `goto error`
- **15 箇所** の `TJS_eTJSVariantError(TJSBadParamCount)` 直接 throw

いずれも `ret` を Release せずに throw / goto する。format 文字列が
壊れている or 引数不足の `format()` 組込関数経由で TJS スクリプト側
から踏ませると、StringHeap セル + LongString バッファをまるごと毎回
リーク。

`'c'`/`'s'` 分岐に `AppendBuffer` throw 用の `str->Release()` try-catch
が部分的に書かれていたが、`ret` の回収は無かった。

修正: 本体全体を try-catch で包み、再 throw 前に `if(ret) ret->Release()`。

#### (b) 代入演算子の "release-first then alloc" による dangling

`common/tjs2/tjsString.h:205, 213, 220, 370`:

```cpp
tTJSString & operator =(const tjs_nchar *rhs) {
    if(Ptr) Ptr->Release();              // 先に開放
    Ptr = TJSAllocVariantString(rhs);    // ← throw すると Ptr は dangling
    return *this;
}
```

同じパターンで `operator =(AnsiString &)` (VCL only) / `operator =(WideString &)` / `AllocBuffer` が壊れていた。`Alloc` が throw すると `Ptr` は **解放済みセルへの生ポインタ** のまま残り、後続の dtor / 再代入で `Release` を呼んで StringHeap 内部 free-list を破壊する。

`operator =(const tTJSString &)` (line 180) は AddRef-then-Release で
正しく書けていたので、同じ "alloc-first, then release-old" 順に揃えた。

#### (c) `SetString` / `ResetString` の自己参照 UAF

`common/tjs2/tjsVariantString.h:89, 110, 147`:

```cpp
void SetString(const tjs_char *ref, tjs_int maxlen = -1) {
    if(LongString) TJSVS_free(LongString), LongString = NULL;   // 先に free
    tjs_int len = (tjs_int)TJS_strlen(ref);                     // ref を読む → UAF
    ...
}
```

`ref` が自身の `LongString` を指していると use-after-free。
`tTJSString::operator=(const tjs_char *)` → `Ptr->ResetString(rhs)` の
経路で踏める (`s = s.c_str()` のような degenerate 代入)。

修正: alloc/copy を先に走らせて、最後に旧 `LongString` を解放する順に
変更。`TJSVS_malloc` が throw した場合も `Length` / `LongString` は
変更されないので強い例外安全性も同時に獲得。`ResetString` の冗長な
先行 free は撤去 (SetString に管理を一本化)。

---

## R3: tjs2 全体監査

commit: 本コミット

検証して確定した leak 9 件:

### HIGH (実シナリオで踏める)

#### 1. `tjsBinarySerializer.cpp` `Read()` — `buffstart` リーク

```cpp
tjs_uint8* buffstart = new tjs_uint8[size];
if( size != stream->Read( buffstart, size ) ) {
    TJS_eTJSError( TJSReadError );           // throw → buffstart 漏れ
}
tTJSVariant* ret = ReadBasicType( buffstart, size, index );  // throw → 漏れ
delete[] buffstart;
```

`Dictionary.load()` 等で不正バイナリを読ませると踏める。**try-catch
で `delete[]` を保証**。

#### 2. `tjsBinarySerializer.cpp` `ReadArray()` — `array`/`value` リーク

`CreateArray` は AddRef 済 (refcount=1) で返るため、ループ内
`ReadBasicType` / `InsertArray` が throw すると `array` が永続リーク。
中間 `value` (new tTJSVariant) も漏れ。**外側 try-catch + 内側 try-catch
で 2 段ガード**。

#### 3. `tjsBinarySerializer.cpp` `ReadDictionary()` — `dic`/`name`/`value` リーク

`CreateDictionary` も AddRef 済。8 箇所の `TJS_eTJSError(TJSReadError)`
+ `ReadString` / `ReadBasicType` / `AddDictionary` の throw で、`dic`
全体 + 取得済 `name` (AddRef 済) + 直前 `value` が漏れる。**3 段
try-catch** で各 scope のオブジェクトを確実に解放。

#### 4. `tjsRegExp.cpp` `replace_regex()` — `OnigRegion*` リーク

```cpp
OnigRegion* region = onig_region_new();
...
hr = funcval.FuncCall(...);        // ユーザ closure
res += ttstr(s, pos);              // alloc throw 可能
res += result.GetString();
```

ユーザ closure や `ttstr` alloc が throw すると `onig_region_free` が
スキップ。try-catch で region を free。

#### 5. `tjsRegExp.cpp` `split_regex()` — `OnigRegion*` リーク

`array->PropSetByNum` / `ttstr` alloc throw で同様。同じ try-catch
パターン。

#### 6. `tjsLex.cpp` `TJSParseOctet()` — `buf` リーク 2 種

(a) **realloc-NULL-assign パターン** (3 箇所):

```cpp
buf = (tjs_uint8*)TJS_realloc(buf, buflen+1);  // 失敗時 NULL 上書き
if(!buf) throw eTJSError(...);                  // ← 旧 buf 永久消失
```

`realloc` が失敗すると元バッファは生きているのに、戻り値 NULL を
変数に上書きしてしまい元バッファを永久ロスト。`tmp = realloc(buf, ...)`
で受けてから判定する形に。

(b) **蓄積済 buf 全体漏れ**: `TJS_eTJSError(TJSStringParseError)` を
parse error 時に 2 箇所で投げているが、蓄積済の `buf` を free して
いない。全体 try-catch で `if(buf) TJS_free(buf)`。

### MEDIUM

#### 7. `tjsByteCodeLoader.cpp` `ReadObjects()` — `srcPos` / `code` / `vdata` 漏れ + 副作用解消

per-iteration の `new[]` で `srcPos` / `code` / `vdata` を確保し
`tTJSInterCodeContext` ctor で所有権を引き渡していたが、間で
`TranslateCodeAddress` 等が破損コードに対し throw すると in-flight
分が全て漏れる。さらに後続 iteration の throw では構築済みの
`objs[0..o-1]` (refcount=1 で hold 中) も全て Release されず漏れ。

加えて副作用として `new[]` / `TJS_free` の不整合 (UB) も発見:
`tTJSInterCodeContext::Finalize` (tjsInterCodeGen.cpp:397, 418) は
`TJS_free(CodeArea)` / `TJS_free(SourcePosArray)` で開放するが、
コンパイラ側は `TJS_malloc` を使うのに対しローダは `new[]` を使って
いた → 普通に Finalize が走ると UB heap 破壊。

修正:
- per-iteration try-catch で in-flight pointer を回収
- 外側 try-catch で構築済 `objs[]` を全 Release
- `srcPos` / `code` を `new[]` → `TJS_malloc` に変更し Finalize と整合
- `vdata` は `tTJSVariant` の dtor が要るので `new[]` のまま (Finalize の
  `delete[] DataArea` 側と整合)

### LOW (OOM 時のみ)

#### 8. `tjsNamespace.cpp` `tTJSLocalSymbolList::Add()`

```cpp
tTJSLocalSymbol *newsym = new tTJSLocalSymbol;
newsym->Name = new tjs_char[...];   // bad_alloc → newsym 漏れ
...
List.push_back(newsym);              // bad_alloc → newsym+Name 漏れ
```

2 段 try-catch でガード。

#### 9. `tjsDictionary.cpp` `forEach` 2 箇所 — `paramList.release()` race

```cpp
new tForEachNameCallback( ..., paramList.release(), ... )
// new が bad_alloc を投げると release 済み raw pointer が orphan
```

`new T(args)` の引数評価 → `operator new` の順なので、`new` が
bad_alloc を投げると `paramList.release()` が返した raw pointer が
誰にも保持されない。

修正: `paramList.get()` で借りる形で `new T(...)` を呼び、ctor 成功
後に `paramList.release()`:

```cpp
auto *raw = new tForEachNameCallback( ..., paramList.get(), numparams );
paramList.release();   // ctor 成功 → 内部 Params に所有権移譲済
std::unique_ptr<...> callback( raw, std::move( deleter ) );
```

---

## 検証して問題なしと判断したもの (R3)

- `tjsScriptCache.cpp:221` `LoadByteCode` — try/catch で blk / loader
  正しく解放
- `tjsByteCodeLoader.cpp:141` Octet AddRef — `RefCount=1` で push、
  dtor で Release。整合
- `tjsGlobalStringMap.cpp:83` RefCount race — single-thread 前提
  init/shutdown で問題なし
- `tjsObjectExtendable.cpp:111` SuperClass NULL — leak ではなく
  NULL deref bug (別件)
- `tjsDate.cpp` / `tjsMath.cpp` / `tjsRandomGenerator.cpp` /
  `tjsMessage.cpp` / `tjsObjectStats.cpp` — 問題なし

---

## 横断パターン (今後 grep する素材)

このセッションで複数箇所に共通して見つかったアンチパターン:

### P1. release-first then alloc

```cpp
if(Ptr) Ptr->Release();
Ptr = NewAlloc();         // throw → Ptr は dangling
```

→ **alloc-first, then release-old** に統一すべき:

```cpp
Resource *newPtr = NewAlloc();
if(Ptr) Ptr->Release();
Ptr = newPtr;
```

R2 (b) で 4 箇所該当。

### P2. raw pointer + try-block 内の例外路で漏れ

`new T[]` / `malloc` / `onig_region_new` / `CreateArray` 等で raw
pointer を取得した後、所有権引き渡しまでの間に複数の throw 経路が
あるパターン。

→ **try-catch で確実に解放**、または `std::unique_ptr<T, custom_deleter>`
等の RAII。

R3 (1)-(6) が該当。RAII の方が綺麗だが、既存コードが raw pointer
中心なので合わせて try-catch にした (将来的に RAII 化する余地あり)。

### P3. range-for 値コピー + use_count 判定

```cpp
for (auto x : container) {
    if (x.shared.use_count() > 1) continue;  // 常に true
    container.erase(...);                    // UB
}
```

→ **明示 iterator** に書き換え、`it = container.erase(it)` パターン。

R1 (a) が該当。他のソースを grep したが他に該当は無かった (修正済)。

### P4. lazy-init を bool フラグだけで保護

```cpp
static bool initialized = false;
if(!initialized) {
    RegisterHook();
    initialized = true;
}
```

任意スレッドから呼ばれうる初期化で同時呼び出しがあると二重登録。

→ `std::once_flag` + `std::call_once`。R1 (c) で 2 箇所。

### P5. `realloc(buf, len)` を `buf` に代入

```cpp
buf = realloc(buf, newlen);
if(!buf) error();           // 失敗時、旧 buf が消失している
```

→ tmp で受けてから判定:

```cpp
T *tmp = realloc(buf, newlen);
if(!tmp) error();
buf = tmp;
```

R3 (6a) で 3 箇所。

---

## R4: OpenGL モジュール全体監査

commit: (本コミット)

`common/visual/opengl/` 配下の Canvas / Texture / ShaderProgram /
Offscreen / VertexBuffer / VertexBinder / OGLDrawDevice /
TextureLayerTreeOwner / GL ラッパ (GLTexture, GLFrameBufferObject,
GLVertexBufferObject, GLShaderUtil) を順に精査。確定した不具合 13 件を
HIGH/MID/LOW で分類。

### HIGH (オーバーフロー / UB / 実害大のリーク)

#### 1. `TextureIntf::LoadMipmapTexture` — ヒープオーバーフロー

`common/visual/opengl/TextureIntf.cpp:371` 旧コード:

```cpp
tjs_int pitch = w * 4;          // 元 bitmap pitch
...
for( i ... count ) {
    tjs_int sw = sizeList->Items[i*2+0];
    tjs_int sh = sizeList->Items[i*2+1];
    tjs_int spitch = sw * 4;    // この mipmap level の pitch
    ...
    buffers.push_back(new tjs_uint32[sw*sh]);
    for( y = 0; y < sh; y++ ) {
        memcpy( &buffer[sw*y], sl, pitch );   // ★ spitch ではない
    }
}
```

mipmap 第 2 レベル以降は `sw < w` なので、`sw*y` オフセットに対して
`pitch (=w*4)` バイト書くと **行ごとに `(w-sw)*4` バイト隣接領域を
踏み潰す** 純粋なヒープ越境。さらに `sl = dstBmp->GetScanLine(...)` は
`sw*4` 幅しか保証がないので **読み出し側も over-read**。

修正: `pitch` → `spitch`。Texture を mipmap 付きで TJS 側からロード
(`Texture(filename, [sizes], ...)` の形) すると確実に発火する経路。

#### 2. `GLFrameBufferObject` — `pbo_` / `glformat_` 未初期化

`common/visual/opengl/GLFrameBufferObject.h:21` 旧:

```cpp
GLFrameBufferObject() : texture_id_(0), framebuffer_id_(0),
                        renderbuffer_id_(0), width_(0), height_(0) {}
//                                                                ↑ pbo_, glformat_ 漏れ
```

`create()` の冒頭で `destory()` が走り、その中で `if (pbo_) glDeleteBuffers(1, &pbo_);`
が **未初期化 GLuint を GL ハンドルとして渡す UB**。Offscreen を構築する
たびに踏む。

修正: 初期化リストに `glformat_(0), pbo_(0)` を追加。

#### 3. `GLFrameBufferObject::create` — 失敗パスで PBO リーク + 状態破壊

`common/visual/opengl/GLFrameBufferObject.cpp:61-79` 旧:

```cpp
if (result == false) { destory(); }   // ID 群をすべて 0 に
else { width_=w; height_=h; }
glBindFramebuffer(...);
// PBO を作成 ← 失敗時もここまで来る
glGenBuffers(1, &pbo_);
glBufferData(...);
```

FBO completeness 失敗で `destory()` した直後に PBO 単体だけが生成され、
ID が `pbo_` に書き戻る → 次回 `create()` の冒頭 `destory()` でその PBO は
解放されるが、**呼び出し側は失敗を受け取って create を再試行しないと
PBO が宙吊り**。また `pbo` だけ生きている不整合状態のまま `false` を
返している。

修正: 失敗時は `destory()` → `glBindFramebuffer(復帰)` → `return false`
で即座に抜け、PBO を作らない。PBO 作成は成功パスへ。

#### 4. `tTVPOGLDrawDevice::Destruct` — GENERIC ビルドで動画リソース全リーク

`common/visual/opengl/OGLDrawDevice.cpp:393-401` `Destruct()` と
`:116` `~tTVPOGLDrawDevice()` は両方とも `_video_texture` / `mVideoBuffer`
を解放しない。`ClearVideo()` は TJS スクリプト側から明示的に呼ばれた
時しか走らないので、**DrawDevice 終了時に毎回 `new GLTexture(...)` と
`new char[w*h*4]` がリーク**。SDL/GENERIC build で動画再生を一度でも
通った場合に確実に発火。

修正: `DoneContext()` の `GLContext->Release()` 直前で `ClearVideo()`
を呼ぶ。`delete _video_texture` は `glDeleteTextures` を伴うため
GL context が current でなければならず、`DoneContext` 内が唯一安全な
位置。

#### 5. `tTVPOGLDrawDevice::UpdateVideo` — 解像度変化時の PBO サイズ不一致

`common/visual/opengl/OGLDrawDevice.cpp:586-606` でサイズ変化時に
`mVideoBuffer` のみ再確保され、`_video_texture` と中の PBO は**旧サイズで
据え置き**。次の `ShowVideo` で:

- `GLTexture::UpdateTexture` が `glMapBufferRange(GL_PIXEL_UNPACK_BUFFER, 0, w*h*4, ...)`
  を旧 PBO 容量 (`old_w*old_h*4`) に対して呼ぶ → `GL_INVALID_VALUE` または
  ドライバ実装によっては map が成功し updator が **GL メモリ側で over-write**。
- `glTexSubImage2D(GL_TEXTURE_2D, 0, x, y, w, h, ...)` が旧テクスチャ寸法の
  範囲外で `GL_INVALID_VALUE`。

修正: `UpdateVideo` はデコーダスレッドから呼ばれるので GL 操作は不可。
render スレッドの `ShowVideo` 側で `_video_texture` のサイズが
`mVideoWidth/mVideoHeight` と食い違っていたら破棄して再生成する。

#### 6. `GLVertexBufferObject::unmapBuffer` — `glUnmapBuffer` が常時 no-op

`common/visual/opengl/GLVertexBufferObject.h:69-80` 旧:

```cpp
void* mapBuffer() {
    glBindBuffer(target_, vbo_id_);
    result = glMapBufferRange(target_, 0, size_, ...);
    glBindBuffer(target_, 0);   // ★ 即 unbind
    return result;
}
void unmapBuffer() {
    glUnmapBuffer(target_);     // ★ どのバッファもバインドされていない
}
```

`glUnmapBuffer` は **currently bound buffer** に対して動くので、unbind
直後だと `GL_INVALID_OPERATION` で no-op。VBO がマップ状態のまま放置され、
描画時に未定義動作。`VertexBuffer.lock/unlock` から TJS に公開されており、
カスタム頂点バッファを使うコードで確実に発火。

修正: `unmapBuffer()` 側で `vbo_id_` を再 bind してから `glUnmapBuffer`
→ `glBindBuffer(target_, 0)` で復帰。

### MID (リーク / 整合性)

#### 7. `GLShaderUtil::CheckLinkStatusAndReturnProgram` — 先行 GL エラー時の program リーク

`common/visual/opengl/GLShaderUtil.cpp:54-58` 旧:

```cpp
if (glGetError() != GL_NO_ERROR)
    return 0;   // ★ program を delete せず捨てる
```

事前に GL エラーが立っていた場合、`glCreateProgram` で確保済みの
`program` GLuint を解放せず 0 を返す。修正: 解放 → return 0。

#### 8. `tTJSNI_Canvas::Invalidate` — Embedded shader Variant 残留

`CreateDefaultShader` で `EmbeddedDefaultShader{,Fill}Object` に初期
シェーダの Variant 参照を保持する。`Invalidate()` は `SetDefaultShader(void)`
→ `DefaultShaderObject.Invalidate()` を行うが、Embedded 側の Variant
参照は明示 Clear されないので **`~tTJSNI_Canvas` の Variant 暗黙 dtor
まで** ref が残る。

実害は (Canvas destruct まで ref が残るだけで) 最終的にはクリーン
されるが、GC タイミング上は緩み。修正: Invalidate の末尾で
`EmbeddedDefault*Object.Clear()` + `Default*ShaderInstance = nullptr`
を defensive に実行。

#### 9. `tTJSNI_TextureLayerTreeOwner::GetWidth/Height` — null deref

`common/visual/opengl/TextureLayerTreeOwner.cpp:102-110` 旧:

```cpp
tjs_int GetWidth() const { return TextureInstance->GetWidth(); }
```

プライマリレイヤ未確定 / `DestroyTexture` 後でも `width` プロパティ
から呼ばれうるため null deref。修正: `return TextureInstance ?
TextureInstance->GetWidth() : 0;`

### LOW (機能バグ / 規律)

#### 10. `GLTexture::setWrapT` が `GL_TEXTURE_WRAP_S` を設定

`common/visual/opengl/GLTexture.h:139`:

```cpp
glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, s );  // ★ WRAP_T のはず
```

`Texture.wrapModeVertical = ...` が効かない機能バグ。修正:
`GL_TEXTURE_WRAP_T`。

#### 11. `GLVertexBufferObject` / `GLFrameBufferObject` — コピー禁止漏れ

両クラスとも生 GLuint を握ってデストラクタで `glDelete*` するが、
コピー/ムーブ禁止が明示されていない。現状インスタンスは
`tTJSNI_*` のメンバとして抱えられるだけで複製はないが、STL コンテナに
入れた瞬間に double `glDelete*` 事故。

修正: コピー/ムーブ 4 つすべて `= delete`。同時に `size_` (vtx) /
`pbo_` `glformat_` (fbo) の未初期化メンバも初期化リストへ。

#### 12. `Canvas::drawTextureAtlas` — シェーダ引数のシャドウイング

`common/visual/opengl/CanvasIntf.cpp:1332-1337` 旧:

```cpp
tTJSNI_ShaderProgram* shader = nullptr;
if( numparams >= 3 ) {
    tTJSNI_ShaderProgram* shader = (tTJSNI_ShaderProgram*)...;  // ★ 内側で再宣言
    if( !shader ) return TJS_E_INVALIDPARAM;
}
_this->DrawTextureAtlas( rect, texture, shader );  // 外側 = 常に nullptr
```

シャドウイングでユーザ指定シェーダが常に無視され `DefaultShaderInstance`
にフォールバック (描画側で `if(!shader)` フォールバックがあるので落ちは
しない)。修正: 内側 `tTJSNI_ShaderProgram* ` 型宣言を削除して外側変数へ代入。

#### 13. `tTJSNI_Offscreen::ExchangeTexture` — friend 経由の private 直書き

`common/visual/opengl/OffscreenIntf.cpp:60-72` 旧:

```cpp
texture->Texture.texture_id_ = oldTex;   // friend で private を直接書換
```

`friend class tTJSNI_Offscreen;` 宣言で GLTexture の private を貫いて
いた。所有権の流れも暗黙で読みにくい。**実害は無い** (size/format は
事前チェックで揃えてあり、PBO は texture-independent なので据え置き OK、
両端の destruct も各々が現在保持する handle を解放するだけで dangling
にはならない) が API として fragile。

修正: GLTexture に `void AdoptTextureId(GLuint id) { texture_id_ = id; }`
を public に追加 (旧 handle は呼び出し側で別コンテナに移譲済みである
ことが前提というセマンティクスを doc 化)、Offscreen 側はそれ経由に。
`friend class tTJSNI_Offscreen;` 削除。所有権の swap を comment で明示。

### スレッド境界の落とし穴 (#5 修正時のメモ)

最初の `#5` 修正は `UpdateVideo` 内で `delete _video_texture` を呼んで
しまい、デコーダスレッドから GL 操作する破壊的 bug を入れかけた。GL
context は render thread でしか current にできない → GL リソースの
生成・破棄は必ず render thread 側 (`ShowVideo` / `Show` / `DoneContext`)
で行う。

OpenGL 系修正は同じ落とし穴を再発しやすいので明示メモ。

---

## 修正範囲外で残った課題 (要検討)

優先度低いが将来的に手をつけるべきもの:

- **`tjsObjectExtendable.cpp:111`** — `SuperClass == NULL` で `Invalidate`
  を呼んでいる経路 (NULL deref)。leak ではないので本ラウンドは見送り
- **tTJSVariantString の "real count - 1" 表現** — `RefCount=0` が
  「1 つ参照あり」を意味する設計。コメントには書かれているが現代的
  には混乱を招く。リファクタは大手術になるので保留
- **TLG decoder の RAII 不足** — 別の懸念領域。今回未着手
- **`tTJSVariantString::Append` の Length 先行更新** — `realloc` が
  throw した場合 Length が新値、データが旧値で不整合。leak ではない
  が状態破壊。今回未着手

---

## ビルド検証

すべてのラウンドで `x64-windows` プリセット (MSVC) で
`krkrz64.exe` のリリースビルドが通ることを確認。

```pwsh
cmake --preset x64-windows
cmake --build build/x64-windows --config Release --target krkrz64
```

clangd の方は `tjsCommHead.h` / `targetver.h` 解決失敗による既存ノイズが
出るが、MSVC 側のコンパイルとは独立した環境問題で本修正と無関係。

---

## コミット一覧

| commit | 内容 |
|--------|------|
| `730ea93d` | StorageCache: TVPClearOldStorageCache の LRU 駆逐バグ修正 + 二重登録対策 |
| `a33959b2` | tjsString: TJSFormatString リーク + 代入演算子例外安全性 + SetString 自己参照 UAF 修正 |
| `69328c55` | tjs2: BinarySerializer / RegExp / Lex / ByteCodeLoader / Namespace / Dictionary のリーク 9 件修正 |
| (本コミット) | OpenGL: LoadMipmapTexture ヒープオーバーフロー + GLFrameBufferObject 未初期化/PBO リーク + OGLDrawDevice 動画リソース漏れ + GLVertexBufferObject unmap 不発 + ShaderUtil GL prog リーク + Canvas Embedded shader 残留 + TextureLayerTreeOwner null deref + GLTexture setWrapT 修正 + GL ラッパ copy 禁止 + drawTextureAtlas シャドウ + Offscreen ExchangeTexture friend 排除 (13 件) |
