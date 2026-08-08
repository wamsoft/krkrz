# 3D 音声定位 (WaveSoundBuffer spatializer) — F-1

`WaveSoundBuffer` に **3D 定位 (spatialization)** の TJS API を新設した。miniaudio 内蔵の
spatializer に直接マッピングしており、**位置・距離減衰・ドップラー・指向性コーン**をカバーする。
外部ライブラリ不要。

## なぜ全バリアント横断か

オーディオ出力は WINVER / SDL / LIB の全バリアントで **miniaudio に統一済み**
(`common/sound/AudioStream.cpp`)。3D spatializer を common の miniaudio 経路へ一度実装すれば
全バリアントに効く。旧 DirectSound 3D (DS3D) は「器だけで TJS API が空」だったため機能互換は
不要で、ゼロから設計した。

## 座標系

miniaudio 準拠 = **右手系・Y up・任意単位**。既定のリスナは前方 `-Z`、上 `+Y`。
`+X` が右。距離は位置ベクトルの長さ (同じ単位系)。コーン角度は**ラジアン**。

## TJS API

### WaveSoundBuffer (音源側)

3D は **`use3D = true` の時のみ有効**。既定 `false`(=非空間化パススルー、従来どおりで回帰なし)。
パラメータは再生前でも再生中でも設定でき、内部にキャッシュされて `ma_sound` (再生毎に生成) へ
自動で再適用される。

| メンバ | 種別 | 意味 |
|---|---|---|
| `use3D` | プロパティ (bool) | 3D 定位の有効/無効。既定 false |
| `set3DPosition(x, y, z)` | メソッド | 音源のワールド座標 (`setPos` / `posX`/`posY`/`posZ` も可) |
| `set3DVelocity(x, y, z)` | メソッド | 速度 (ドップラー計算用) |
| `set3DConeDirection(x, y, z)` | メソッド | 指向性コーンの向き |
| `set3DCone(innerRad, outerRad, outerGain)` | メソッド | コーン (内角/外角=ラジアン、外側ゲイン 0..1)。全方位は inner=outer=2π |
| `minDistance` | プロパティ (real) | これ以内は減衰なし (最大音量) |
| `maxDistance` | プロパティ (real) | これ以遠は減衰頭打ち |
| `rolloffFactor` | プロパティ (real) | 距離減衰の強さ |
| `dopplerFactor` | プロパティ (real) | ドップラー強度 (0=無効, 1=標準) |
| `attenuationModel` | プロパティ (int) | 減衰モデル (下記 `am*` 定数) |

### SoundListener (聴取者、engine グローバル)

インスタンス不要の名前空間的クラス (`System` と同様)。engine グローバルの listener (index 0) を操作する。

| メンバ | 意味 |
|---|---|
| `SoundListener.enabled` | プロパティ (bool)。リスナを有効化。3D を使うなら true |
| `SoundListener.setPosition(x, y, z)` | リスナのワールド座標 |
| `SoundListener.setDirection(x, y, z)` | 前方向ベクトル |
| `SoundListener.setWorldUp(x, y, z)` | 上方向 (既定 0,1,0) |
| `SoundListener.setVelocity(x, y, z)` | 速度 (ドップラー用) |
| `SoundListener.setCone(innerRad, outerRad, outerGain)` | リスナ指向性 |

### 減衰モデル定数 (グローバル)

`SysInitScript.tjs` で定義 (`ma_attenuation_model` と同値)。

| 定数 | 値 | 意味 |
|---|---|---|
| `amNone` | 0 | 距離減衰なし |
| `amInverse` | 1 | 逆数減衰 (既定) |
| `amLinear` | 2 | 線形減衰 |
| `amExponential` | 3 | 指数減衰 |

## 使用例

```tjs
// リスナを原点・前方 -Z に置く
SoundListener.enabled = true;
SoundListener.setPosition(0, 0, 0);
SoundListener.setDirection(0, 0, -1);

// 3D 音源
var snd = new WaveSoundBuffer(win);
snd.use3D = true;
snd.attenuationModel = amInverse;
snd.minDistance = 2.0;
snd.maxDistance = 40.0;
snd.dopplerFactor = 1.0;
snd.set3DPosition(-15, 0, -3);  // 左前方
snd.set3DVelocity(7.5, 0, 0);   // 右へ移動 (ドップラー)
snd.open("bgm/bgm01_dummy.ogg");
snd.looping = true;
snd.play();
```

## 実装レイヤ

- **`common/sound/AudioStream.{h,cpp}`** — `iTVPAudioStream` に 3D メソッド群
  (`SetSpatializationEnabled` / `Set3DPosition` / `Set3DVelocity` / `Set3DConeDirection` /
  `Set3DCone` / `Set3DMinDistance` / `Set3DMaxDistance` / `Set3DRolloff` /
  `Set3DDopplerFactor` / `Set3DAttenuationModel`、既定 no-op)。`MiniAudioStream` が
  `ma_sound_set_*` へ実装。リスナは free 関数 `TVPSetSoundListener*` → `ma_engine_listener_*`
  (engine 初期化は `GetMiniAudioEngine()` が保証)。
- **`common/sound/QueueSoundBufferImpl.{h,cpp}`** — 3D パラメータを TJS インスタンス側に
  キャッシュし、`Set3DParamsToStream()` で `Stream` (再生毎に生成) へ適用。`use3D=false`
  の間は位置等を送らず `SetSpatializationEnabled(false)` のみ。
- **`common/sound/WaveIntf.{h,cpp}`** — TJS の WaveSoundBuffer メソッド/プロパティ、および
  `SoundListener` クラス (`tTJSNC_SoundListener`)。
- **`common/base/ScriptMgnIntf.cpp`** — `SoundListener` クラスをグローバル登録。
- **`resource/SysInitScript.tjs`** (と `win32/vcproj/SysInitScript.tjs`) — `am*` 定数。

## デモ

`data/startup.tjs` の「3D 音声デモ」(サウンドメニュー)。リスナを画面中央に固定し、音源を
**左右フライバイ**または**周回**させる。左右パン + 距離減衰 + ドップラーを実聴でき、可視化
レイヤにリスナ (中央) と音源 (点) を表示する。ボタンでモード / ドップラー ON-OFF / 減衰モデルを切替。

## 制約: 前後の定位 (HRTF 非対応)

miniaudio 内蔵 spatializer は **HRTF 非対応**なので、ステレオ出力では**前方と後方を区別できない**
(前も後ろも中央パンになる = コーン・オブ・コンフュージョン)。明確に伝わるのは**左右パン**と
**距離減衰**。真円で周回させると前後が同じ中央パンになり「前に音が来る」感が出ない。
デモの周回モードはこれを補うため、**前方で半径を小さく (近く=大きく)・後方で大きく (遠く=小さく)**
する楕円軌道にして、距離差で前後感を出している。真の前後・上下定位には HRTF (下記段階2) が要る。

## 将来 (段階2)

HRTF / Dolby Atmos オブジェクトベースは別ライブラリ (Steam Audio / `ISpatialAudioClient` 等) が
必要なため段階2。まずは miniaudio 内蔵の基本 3D (パンニング + 距離 + ドップラー + コーン)。
