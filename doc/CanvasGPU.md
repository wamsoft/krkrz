# Canvas の描画バックエンド抽象化 (SDL_GPU / GLES) 設計メモ

> 状態: 基本部分は実装済み・master に統合済み (2026-10-08 起票、2026-10-09 統合)。
> 実装状況は «実装状況»、今後の進め方 (GPU モードの確立とプラグインの GPU 版) は «今後の方向» を参照。

## 目的

Canvas 系クラス (Canvas / Texture / Offscreen / ShaderProgram / VertexBuffer / VertexBinder) を
OpenGL ES 以外でも動かす。最初の対象は SDL3 の GPU API (SDL_gpu.h)。

- OpenGL が無く、シェーダーを実行時にコンパイルできない機種でも Canvas を使えるようにする
- 将来は既存の GLES 実装も同じ抽象層の下へ移し、Canvas の実装を 1 本にする

## 方針

1. **抽象層は SDL_GPU 側の作り方に合わせる。** パイプライン状態を前もってまとめる、描画はレンダーパスの中、
   uniform はブロックで渡す、シェーダーは事前変換したもの、という制約の強い側に合わせる。
   GLES はこれを内部で真似できるが、逆 (GL 流の逐次ステート API を SDL_GPU で実装) は難しく遅い。
2. **汎用 RHI にはしない。** Canvas が使う範囲 (テクスチャ・描画先・頂点/インデックス・シェーダー・
   状態の組み合わせ・描画・読み戻し) に絞る。
3. **既存の GL 実装はしばらくそのまま残す。** 新実装は `KRKRZ_CANVAS_BACKEND` で選ぶ。
   GLES バックエンドへの移し替えは新実装が安定してから。
4. **開発は Windows で行う。** SDL_GPU は Windows でも動く (まず Vulkan ドライバ。SPIR-V をそのまま渡せる)。

## 構成

```
TJS クラス (Canvas / Texture / Offscreen / ShaderProgram / VertexBuffer / VertexBinder / Matrix32 / Matrix44)
   │  TJS API は今の GL 版と同じ。GL 固有の値 (nativeHandle, dt*/pt* 定数) の扱いは下記
   ▼
Canvas コア (バックエンド非依存)
   - 状態スタック (save/restore)、行列、clipRect、blendMode → BlendState 変換
   - 組み込み描画 (fill / drawTexture / transition / atlas / 9patch / drawMesh) の頂点生成
   - プリミティブ変換 (LINE_LOOP / TRIANGLE_FAN → インデックス展開、8bit インデックス → 16bit)
   - uniform の影バッファ (名前 → オフセットはリフレクションから)
   ▼
描画デバイス抽象 (tvp::gfx::IDevice ほか)
   ├─ SDL_GPU バックエンド  ← 最初に作る
   └─ GLES バックエンド      ← 後で既存実装から移す
```

### 抽象層のインターフェース (案)

```cpp
namespace tvp::gfx {

enum class TextureFormat { RGBA8, BGRA8, R8, D32F_S8 };  // D24S8 は持たない機種があるので使わない
enum class PrimitiveType { PointList, LineList, LineStrip, TriangleList, TriangleStrip };
enum class IndexType { U16, U32 };
enum class VertexFormat { Float1, Float2, Float3, Float4, UByte4Norm, Short2, Short4, ... };
enum class ShaderStage { Vertex, Fragment };

struct BlendState   { bool enable; BlendFactor srcColor, dstColor, srcAlpha, dstAlpha;
                      BlendOp colorOp, alphaOp; uint8_t writeMask; };
struct StencilState { bool enable; CompareOp compare; StencilOp pass, fail, depthFail;
                      uint8_t readMask, writeMask; };
struct RasterState  { CullMode cull; };

struct VertexAttribute { uint32_t location; VertexFormat format; uint32_t offset; uint32_t bufferSlot; };
struct VertexLayout    { std::vector<VertexAttribute> attributes; std::vector<uint32_t> strides; };

// シェーダーとリフレクション情報。バイトコードは機種ごとの形式 (SPIR-V / 機種専用) を持つ。
struct ShaderReflection {
    struct Uniform   { std::string name; ShaderStage stage; uint32_t offset, size, arrayCount; UniformType type; };
    struct Sampler   { std::string name; ShaderStage stage; uint32_t slot; };
    struct Attribute { std::string name; uint32_t location; VertexFormat format; };
    std::vector<Uniform> uniforms; std::vector<Sampler> samplers; std::vector<Attribute> attributes;
    uint32_t uniformBlockSize[2];   // stage ごとの uniform ブロックの大きさ (std140)
};

class ITexture  { /* width, height, format, isRenderTarget, nativeHandle */ };
class IBuffer   { /* size, usage (vertex / index), Update(offset, data, size) */ };
class IProgram  { /* const ShaderReflection& reflection() */ };

struct PipelineDesc {   // バックエンド側でハッシュしてキャッシュする
    IProgram* program; VertexLayout layout; PrimitiveType primitive;
    BlendState blend; StencilState stencil; RasterState raster;
    TextureFormat colorFormat; bool hasDepthStencil;
};

class IRenderPass {
public:
    virtual void SetViewport(...) = 0;
    virtual void SetScissor(const Rect* r) = 0;        // nullptr で無効
    virtual void SetPipeline(const PipelineDesc&) = 0;
    virtual void SetStencilReference(uint8_t) = 0;
    virtual void SetVertexBuffer(uint32_t slot, IBuffer*, uint32_t offset) = 0;
    virtual void SetIndexBuffer(IBuffer*, IndexType, uint32_t offset) = 0;
    virtual void SetTexture(uint32_t slot, ITexture*, const SamplerDesc&) = 0;
    virtual void PushUniforms(ShaderStage, const void* data, uint32_t size) = 0;
    virtual void Draw(uint32_t vertexCount, uint32_t firstVertex) = 0;
    virtual void DrawIndexed(uint32_t indexCount, uint32_t firstIndex, int32_t vertexOffset) = 0;
};

class IDevice {
public:
    virtual ITexture* CreateTexture(const TextureDesc&) = 0;
    virtual void      UploadTexture(ITexture*, const Rect&, const void* pixels, uint32_t pitch) = 0;
    virtual void      CopyTexture(ITexture* src, const Rect&, ITexture* dst, int x, int y) = 0;
    virtual void      GenerateMipmaps(ITexture*) = 0;
    virtual IBuffer*  CreateBuffer(BufferUsage, uint32_t size) = 0;
    virtual IBuffer*  AllocTransient(BufferUsage, const void* data, uint32_t size, uint32_t* offset) = 0; // クライアント配列の代わり
    virtual IProgram* CreateProgram(const ShaderSource& vs, const ShaderSource& fs) = 0;
    virtual IRenderPass* BeginPass(ITexture* target, const LoadOp& color, const LoadOp& stencil) = 0;
    virtual void      EndPass(IRenderPass*) = 0;
    virtual bool      ReadPixels(ITexture*, const Rect&, void* dst, uint32_t pitch) = 0;  // 同期 (遅い)
    virtual void      Submit() = 0;   // フレームの最後
};

}
```

- テクスチャ更新・コピーはレンダーパスの外でしかできない (SDL_GPU)。デバイス側でパス外に貯めて、
  次の BeginPass の前にまとめて流す。パスの途中で必要になったらパスを切って流し、続きを load で再開する。
- 原点は左上 (SDL_GPU / D3D 流)。GL 版にある FBO の上下反転とシザーの Y 反転はコア側から無くし、
  GLES バックエンドが自分で吸収する。

## シェーダー

### 方式

実行時の GLSL コンパイルを前提にしない。TJS が渡す GLSL (文字列 / ファイル) をビルド時に集めて変換し、
実行時はソースのハッシュで変換済みのものを引く。

```
GLSL ES 1.00 / 3.00 (TJS が渡すのと同じ文字列・ファイル)
  │ ① 正規化 (自前の書き換え)
  │    - attribute/varying/gl_FragColor/texture2D → in/out/出力変数/texture
  │    - ばらの uniform → stage ごとの uniform ブロック
  │        頂点: layout(set=1, binding=0, std140)  フラグメント: layout(set=3, binding=0, std140)
  │    - sampler2D → 頂点 set=0 / フラグメント set=2 の binding を順に
  │    - attribute → layout(location=N)
  │    - precision 指定は落とす
  ▼
#version 450 の GLSL → glslang → SPIR-V
  │ ② リフレクション (spirv-cross): uniform 名 → オフセット・型、sampler 名 → スロット、attribute 名 → location
  ▼
SPIR-V + リフレクション (JSON) → マニフェスト (ソースのハッシュ → エントリ)
  └─ 機種によってはさらに機種専用形式へ変換 (機種側のツールで)
```

- set / binding の割り当ては SDL_GPU の SPIR-V の規約に合わせる
  (頂点: set 0 = テクスチャ, set 1 = uniform / フラグメント: set 2 = テクスチャ, set 3 = uniform)。
- GL ではプログラム単位で共有される uniform が、SDL_GPU では stage ごとのブロックになる。
  両方の stage で使われる uniform は両方のブロックに入れ、代入時に両方へ書く。
- 正規化と SPIR-V 化は C++ の小さなツール (`krkrz_shaderc`、glslang / spirv-cross は vcpkg) にまとめる。
  同じコードを Windows の開発ビルドにも組み込み、**開発中は実行時に変換できる**ようにする
  (マニフェストに無いシェーダーを書き出す収集モードも兼ねる)。
- 収集方法: (a) 開発ビルドで実行時に出会ったソースを書き出す、(b) スクリプト・シェーダーファイルを静的に走査する。
- 組み込みシェーダー (Canvas の default / fill / transition、クリップ、エフェクト) は同じ流れで事前変換して本体に埋め込む。

### TJS API との互換で決めること

| 項目 | 方針 (案) |
|---|---|
| `ShaderProgram(vs, fs, vsIsFile, fsIsFile)` | そのまま。中でハッシュを引く。無ければ例外 (開発ビルドは実行時変換) |
| uniform / attribute の代入 (`shader.u_time = 1.0`) | そのまま。リフレクションで影バッファに書く |
| `nativeHandle` | バックエンドのハンドル (SDL_GPU なら `SDL_GPUTexture*`)。GL の id ではなくなる |
| `VertexBuffer.dt*` / `pt*` 定数 | 値は GL の enum のまま残し、コアで変換する (互換優先) |
| `ptLineLoop` / `ptTriangleFan` | コアでインデックス展開 |
| `VertexBuffer.lock()` | CPU 側の影バッファを返し、`unlock()` で転送を積む |
| `Canvas.capture` | 同期読み戻し (遅い。用途はスクリーンショット程度) |
| `System.openGLESVersion` など GL 専用 | GPU バックエンドでは 0 / 未対応 |

## 画面への出し方 (SDL ビルド)

SDLDrawDevice (SDL_Renderer) と GPU デバイスを共有する (SDL 3.4.0 以降の API で足りる)。

- SDLDrawDevice が `SDL_CreateGPUDevice` で作ったデバイスを `SDL_CreateGPURenderer(device, window)` に渡す
- Canvas は画面サイズの GPU テクスチャに描き、`SDL_PROP_TEXTURE_CREATE_GPU_TEXTURE_POINTER` で
  SDL_Texture として包んで `SDL_RenderTexture` で合成する (前後関係は `SDL_FlushRenderer` で揃える)
- OGLDrawDevice と同じく `onDraw` / `canvas` / `texture` (プライマリレイヤーのテクスチャ) を出す
- Elements ダイアログ・オーバーレイは今の SDL_Renderer 経路のまま

## 段階

0. 画面出力を GPU 経路にする (SDL_Renderer の GPU バックエンドが動くことを各機種で確認)
1. `krkrz_shaderc` (正規化 → SPIR-V → リフレクション → マニフェスト) と組み込みシェーダーの変換
2. 抽象層 + SDL_GPU バックエンド、Texture / Offscreen / VertexBuffer / VertexBinder / ShaderProgram
3. Canvas コア (draw / clear / fill / drawTexture / drawTransition / atlas / 9patch / drawMesh /
   blendMode / clipRect / matrix / save・restore / capture)
4. SDLDrawDevice への組み込み。Windows で GL 版と同じスクリプト (data/gl_canvas など) を見比べる
5. 後回し: ステンシル・マスククリップ、beginEffect/endEffect、GLCompositor、TextureLayerTreeOwner、動画テクスチャ
6. GLES バックエンドへの移し替え (既存 GL 実装の置き換え)

### 記録した描画とリソースの寿命

SDL_GPU のバックエンドは描画を記録しておき、描画先が変わるときや送信のときにまとめて積む。
記録した描画が使うテクスチャ・バッファ・シェーダーを先に手放すと積むときに解放済みを触るので、
これらを手放す前に記録分を積む (`GpuDeviceSDL.cpp` の `FlushBeforeRelease`。SDL 側は積んだ
コマンドが終わるまで実体の解放を待つ)。スクリプトが描画の途中で Texture を invalidate しても安全。

Canvas の描画区間は入れ子にできる (onDraw の中で GLCompositor.capture など)。内側の Canvas が
描画を終えると、外側の Canvas の描画先を付け直す。

## 実装状況 (2026-10-13)

| 段階 | 状況 |
|---|---|
| 1. シェーダー変換 (`common/visual/gpu/gpushaderc.py`) | 済。SPIR-V / 機種専用形式 (PRIVATE) / DXIL (D3D12) のパックを作れる |
| 2. 抽象層 + SDL_GPU バックエンド | 済 (`GpuDevice.h` / `sdl3/visual/GpuDeviceSDL.cpp`) |
| 3. Canvas コア | 済: draw / clear / fill / drawTexture (1〜3 枚) / drawTransition / drawTextureAtlas / draw9Patch / drawMesh / capture / save・restore / blendMode / clipRect / matrix。未: drawText (呼ぶとログを出して素通し) |
| 4. 描画デバイス | 済: `Window.GPUDrawDevice` (`sdl3/visual/SDLGpuDrawDevice.cpp`、SDLDrawDevice の派生)。GL 版の無いビルドでは `Window.OGLDrawDevice` としても登録 |
| 5. 後回しにしたもの | 済: マスク・ステンシルクリップ (2026-10-13。shaders/clip_*.frag、ステンシルは描画先ごとの D32F_S8、捕捉先は描画先と同じ大きさの中間テクスチャを使い回す)、beginEffect/endEffect (2026-10-13。shaders/effect_*.frag、コマンド配列の解釈は GL 版と共通の EffectCommand.cpp)、TextureLayerTreeOwner (2026-10-13。GL 版と同じファイルで、テクスチャを GPU 版にして合成された矩形ごとに転送)、GLCompositor (2026-10-13。`gpu/GpuCompositorIntf.cpp`、TJS のクラス名は同じ。makeCurrent は何もせず GLGetProcAddress は 0)。動画テクスチャ (GL 版の GLVideoPresenter) は不要: GPUDrawDevice は SDLDrawDevice の SDL presenter (`sdlVideoPresenterHost`) をそのまま使い、SDL_Renderer (同じ GPU デバイス) が YUV のまま GPU で変換して Canvas の上に出す。2026-10-13 に再確認 (webm / mpg、全面・矩形・vomMixer・Canvas の上) |
| 6. GLES バックエンドへの移し替え | 未 |
| Windows での確認 | data/gl_canvas の drawTexture・カスタムシェーダー・Offscreen を目視確認 (Vulkan)。クリップは GL 版と同じスクリプト (マスク / mask 無し+矩形クリップ / ステンシル / ステンシル+矩形クリップ / Offscreen 上のステンシル / 入れ子 / begin の閉じ忘れ) の画面を画素比較し、Vulkan・D3D12 ともマスク境界の数画素以外一致。エフェクトも全コマンド (gamma / light / lut / grayscale / colorize / modulate / overcolor / boxBlur / gaussianBlur、連結・矩形クリップ・入れ子) で差 3 以内 (modulate の色相境界の数十画素のみ大きい。noise は乱数なので対象外) (2026-10-13) |
| PS5 | 実機で確認済み: 回転描画 (drawTexture + Matrix32)・カスタムシェーダー・GPU パーティクル (点スプライト)。全画面表示と点スプライトの修正を含む。2026-10-13 に足した組み込みシェーダー (clip_stencil / clip_mask / effect_pointwise / effect_lut / effect_blur) は機種専用形式のパックの作り直しが要る (機種側のビルドで自動再生成)。2026-10-13 に実機確認: クリップ・エフェクト・TextureLayerTreeOwner・GLCompositor (onDraw 内 / タイマーからの capture) の全 22 テストが完走し、GL 版の基準画像と一致 (差は縮小による輪郭と既定フォントの違いのみ。uniform 配列も問題なし) |
| Xbox (D3D12, GDK) | 実機 (Xbox Series devkit) で確認済み: GPU パーティクル (点スプライトを四角形のインスタンス描画で代用)・カスタムシェーダー。組み込み・スクリプトのシェーダーとも GXDK の dxc で事前コンパイルして実行時の再コンパイル無し。Xbox One 版は Series の互換動作で確認 |
| Windows (D3D12) | `-gpudriver=direct3d12` で DXIL を使う。2026-10-11 に確認: drawTexture + Matrix32・Offscreen が Vulkan と同じ絵、カスタムシェーダー・GPU パーティクル (点スプライトの四角形化) も動作 (スクリプトのシェーダーは `-gpushadercollect` で集めて `--target dxil` でパックにし exe の隣に置く。汎用 dxc の DXIL)。SDL_Renderer 側の線描画・Elements も表示 |
| Switch (Vulkan) | コンソール側のビルドで OpenGL の代わりに Vulkan を選んだとき組み込む。実機確認はまだ |

実機で分かったこと:

- 描画デバイスの Canvas の画面は **プライマリレイヤーと同じ大きさ** で作り、レイヤー画像と同じ矩形へ出す。
  ウィンドウの内側の大きさで作って等倍で出すと、内側とレイヤーの大きさが違う機種 (内側 1920x1080 固定・
  レイヤー 1280x720 など) で画面の一部にしか出ない
- HLSL には点の大きさの出力も点の中の座標も無く、spirv-cross は `gl_PointSize` を捨て、`gl_PointCoord` を
  定数にしてしまう。gpushaderc.py は機種専用形式を作るとき、`gl_PointSize` を書くシェーダーには `PSIZE` の
  出力を、`gl_PointCoord` を読むシェーダーには `POINTCOORD` の入力を足す (機種側のヘッダーが機種の
  セマンティクスへ読み替える)
- D3D12 (DXIL) は点の大きさそのものが無い。gpushaderc.py は `--target dxil` のとき、`gl_PointSize` を書く
  頂点シェーダーを «点を四角形に広げる» 形へ書き換える (元の `main` を `_kz_main` に改名し、角の属性
  `_kz_corner` (-1..1) と描画先の大きさ `_kz_viewport` を足して、位置を角の方向へ `gl_PointSize` の半分だけ
  ずらす。`gl_PointCoord` は角から求めた `_kz_pc` に置き換える)。エンジン側 (`GpuCanvas::DrawMesh`) は
  `_kz_corner` を持つシェーダーで点のリストを描くとき、4 頂点の三角形ストリップを点の数だけインスタンス
  描画し、スクリプトの頂点属性はインスタンスごとに進める (`PipelineState.instanceRate` / `DrawCall.instanceCount`)。
  ⚠ インデックス付きの点の描画はまだ四角形化していない (点のまま = 1 ピクセル)
- DXIL を機種向けの dxc (Xbox なら GXDK の dxc) で事前コンパイルするには、シェーダーにルートシグネチャが
  要る。gpushaderc.py は SDL_GPU の D3D12 バックエンドがパイプラインごとに作るものと同じルートシグネチャ
  (頂点 b=space1 / フラグメント t,s=space2・b=space3、組の両方の使用数から決まる) を `KZ_RS` として埋め込む。
  一致しないと実行時にパイプライン生成が失敗するので、SDL 側の作り方が変わったら合わせること

### ビルド

- CMake: `-DKRKRZ_USE_OPENGL=OFF -DKRKRZ_CANVAS_GPU=ON` (OpenGL 版とは排他。SDL ビルドのみ)
- 既定の描画デバイス: GPU 版を入れたビルドは最初から GPUDrawDevice を使う (途中で SDLDrawDevice から差し替えると、
  ドライバによっては画面の取り合いになるため)。`-drawdevice=sdl` で従来の SDLDrawDevice に戻せる
- `-gpudriver=vulkan` などで SDL_GPU のドライバを指定
- Windows は SPIR-V を受け付けるドライバ (Vulkan) が選ばれる。`-gpudriver=direct3d12` を付けたときだけ DXIL を
  要求して D3D12 で動く (組み込みシェーダーは `GpuBuiltinShadersDXIL.inc` を使う)
- Xbox (GDK) は常に D3D12 (DXIL)

### シェーダーの流れ

```
組み込み: shaders/builtin.json → gpushaderc.py builtin → GpuBuiltinShaders.inc (SPIR-V、ソースごと埋め込み)
スクリプト: 実行時に -gpushadercollect=<フォルダ> で見つからなかったソースを書き出す
          → gpushaderc.py pack -o canvas_shaders.kzgs <フォルダ>
          → データに canvas_shaders.kzgs を置く (または -gpushaderpack=<storage>[;<storage>...])
          データに無ければ exe と同じフォルダの canvas_shaders.kzgs を読む (ビルドが exe の隣に置く構成向け)
機種専用形式: gpushaderc.py (pack|builtin) --target private --hlsl-compile-vs "CMD" --hlsl-compile-fs "CMD"
          SPIR-V → spirv-cross で HLSL (SM5.0) → CMD (機種のコンパイラ)。コンパイラと引数は機種側のビルドが与える
D3D12:      gpushaderc.py (pack|builtin) --target dxil [--dxc <dxc.exe>]
          SPIR-V → spirv-cross で HLSL (SM6.0, --hlsl-enable-compat) → dxc で DXIL (ルートシグネチャ埋め込み)
          組み込み: builtin --target dxil --symbol TVPGpuBuiltinShaderPackDXIL → GpuBuiltinShadersDXIL.inc
          (汎用の dxc で作った DXIL。機種は読み込み時に再コンパイルする)
          機種向け: 外枠のステージ処理が --dxc に機種の dxc を渡してパックを作り直し、データ側に置く
```

ツールの実行には Python 3 と glslangValidator / spirv-cross (Vulkan SDK) が要る。DXIL には dxc も要る
(Vulkan SDK の Bin にあるもの、または `--dxc` で指定)。

## 今後の方向: GPU モードの確立とプラグインの GPU 版 (2026-10-09 記録、未着手)

すぐには進めない。下の順で少しずつ進める予定の想定を残しておく。

### 1. 本体の内蔵機能を GPU モードで揃える

SDL ビルドの GPU 描画を SDL_GPU に寄せたもの (KRKRZ_CANVAS_GPU のビルドで GPUDrawDevice を使う状態) を
«GPU モード» と呼ぶ。まず本体の内蔵機能を GPU モードで一通り動かし、**コンソール機 (PS5) まで含めて**
動作を確立させる。

- Canvas の後回し分: ~~ステンシル・マスククリップ~~ (済)、~~beginEffect/endEffect~~ (済)、drawText
- ~~endEffect の opacity 引数~~ (済): 2026-10-15 に OpenGL 版 Canvas へ `endEffect(commands, opacity)` を
  追加し (gles プラグイン互換。合成時に捕捉内容の α へ掛ける。src/core `e5e0d390`)、同日 GPU 版も対応した。
  最後の合成 (`CompositeCapture` が使う組み込みシェーダ `clip_mask`) に `a_opacity` を足し、`EndEffect` から
  渡す (マスククリップの合成は 1.0 のまま)。組み込みのパック (SPIR-V / DXIL) は作り直し済み。
  GLCompositor で読み戻して OpenGL 版と同じ画素になることを Vulkan・D3D12 で確認 (省略 / 255 / 128 / 0 /
  範囲外の丸め / エフェクト内のマスククリップ)。**機種専用形式 (PS5 等) のパックは機種側のビルドで作り直しが要る**
- GL 版にあって GPU 版に無いもの: ~~GLCompositor~~ (済)、~~TextureLayerTreeOwner~~ (済)、~~動画テクスチャ (GLVideoPresenter 相当)~~ (SDL presenter の経路で足りる)
- GPU の renderer 上で、レイヤー合成・動画・Elements のオーバーレイがこれまでどおり出ること
- 機種: Windows (Vulkan) → PS5 → Switch (Vulkan)。Linux / Steam Deck (Vulkan) も同じ SPIR-V で動く見込み。
  macOS (Metal) は SPIR-V を受け付けないので、MSL への変換を gpushaderc.py に足すかどうかを決める
- 最終的には GLES 版も同じ抽象層の下へ移し (段階 6)、Canvas の実装を 1 本にする

### 2. GLES ベースのプラグインに GPU 版を足す

今 GLES で作っている描画系プラグインは、**可能なものから SDL_GPU 版を合わせて整備する**。
GL 版は残し、本体のビルド (KRKRZ_USE_OPENGL / KRKRZ_CANVAS_GPU) に合わせてどちらかを組み込む。

| プラグイン | 内容 | 見込み |
|---|---|---|
| krkrlive2d | Live2D Cubism | SDK の描画部 (Rendering) を SDL_GPU 向けに用意する。モデルの読み込み・更新は共通 |
| krkreffekseer | Effekseer | 描画部 (Renderer) を SDL_GPU で書くか、既存の描画部を使えるかを調べる |
| krkrmotion / krkremote | M2 モーション / E-mote | GLES 版の描画部を SDL_GPU へ移す |
| krkrgles | GLES の API をそのまま TJS に出すもの | GL 前提なので対象外 (GPU モードでは使えない) |
| krkrthreepp | threepp (3D) | threepp が GL 専用なので当面対象外 |

プラグインへ出す口 (決めること):

- 今の GLES プラグインは、本体の GL コンテキストに `TVPGLGetProcAddress` で入り、Canvas / Offscreen /
  Texture へ描いている。GPU 版では tp_stub で **GPU デバイス・コマンドバッファ・描画先テクスチャ** を
  渡す口が要る (レンダーパスの開け閉めとテクスチャ転送の順序は本体の決まりに合わせる)
- プラグインのシェーダーも実行時コンパイルに頼れないので、本体と同じ変換 (gpushaderc.py → SPIR-V /
  機種専用形式) で事前変換する。組み込みのパックをプラグインごとに持つか、本体のパックへまとめるか
- `Texture.nativeHandle` が `SDL_GPUTexture*` になるので、それを受け取って描く形にそろえる

### 3. 別途開発中の SDL_GPU ベースのエンジンと合わせる

別途、SDL_GPU を土台にした新しいエンジンを開発中で、描画はそちらと同じ方向 (SDL_GPU) にそろえる。
抽象層・シェーダー変換・プラグインの描画部 (Live2D / Effekseer / モーション系) は、**両方のコードを
参照しあいながら** 進め、共通にできるものは共通にする。
