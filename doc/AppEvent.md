# AppEvent (アプリイベント機構)

SDL(Generic) 版のスレッド間アプリイベント送受信機構。旧 `NativeEventQueue`
を廃止し、`tTVPApplication` の `SendAppEvent` / `DispatchAppEvent` と
`AppEventInterface` に簡略化したもの。

WINVER ビルドは従来どおり Win32 ユーティリティ窓 (`win32/base/NativeEventQueue`)
を使い続けるため、本書の API は SDL3 / LIB ビルド限定です。共有コードでの
WINVER との分岐については §6 を参照。

## 1. 背景 — 旧 NativeEventQueue の何が問題だったか

旧構造は WINVER の「キューごとに隠し Win32 窓を持ち PostMessage する」設計を
Generic に移植したもので、Generic 側 `tTVPApplication` が以下を抱えていた:

- `command_que_` … `EventCommand{ target, command }` の `std::queue`
- `command_cache_` … `NativeEvent*` の再利用プール
- `event_handlers_` … 登録ハンドラ一覧
- mutex 3 本 (`command_que_mutex_` / `command_cache_mutex_` / `event_handlers_mutex_`)
- `NativeEvent` … `WParam`/`LParam` を union で float/int に切り分ける構造体

送信側は `postEvent(ev, handler)` でプールから `NativeEvent` を取り出してコピーし、
`{handler, ev}` を `command_que_` に積む。`Dispatch()` が drain し、target 指定が
あればそのハンドラを `event_handlers_` から探して送り返し、無ければ `appDispatch`
→ 未消費なら全ハンドラへブロードキャスト、という二段ルーティングだった。

実態としては:

- `handler != nullptr` の送り返しは「送信主を記録 → 同じ相手に返す」だけで、
  受信側は結局 `message` で分岐している。**送信主の記録は不要**。
- `handler == nullptr` の post は `Startup()` の `AM_STARTUP_SCRIPT` のみで、
  それは `appDispatch` が必ず消費する。**全ブロードキャスト経路は実質デッドコード**。
- SDL_Event 自体がスレッドセーフ (`SDL_PushEvent` はロックフリー) なので、
  自前キュー + mutex で受け渡す必要がない。

→ message ID 判定だけ残し、キュー・プール・target ルーティング・mutex 2 本を撤去した。

## 2. 全体構造

```
任意スレッド
    │  Application->SendAppEvent(message, wparam, lparam)
    │     ├─ _SendAppEvent() … 環境別。SDL は即 SDL_PushEvent
    │     └─ 失敗時のみ retry_que_ に積む (唯一の要ロック箇所)
    ▼
SDL イベントキュー (SDL3 が所有・スレッドセーフ)
    │  単一の登録済みユーザイベント型 mAppEventType
    │    user.code  = message
    │    user.data1 = wparam   (64bit 前提)
    │    user.data2 = lparam
    ▼
メインスレッド: SDL_AppEvent (sdl3/environ/main.cpp)
    │  event->type == mAppEventType を判定
    │  → Application->DispatchAppEvent(code, data1, data2)
    ▼
DispatchAppEvent: app 独自イベント (AM_STARTUP_SCRIPT 等) を処理。
                  未消費なら登録ハンドラ全てに配り、各自 message で取捨選択。
```

送信が失敗するのは SDL イベントキューが一時的に満杯等のレアケース。その分は
`retry_que_` に積まれ、毎フレームの `tTVPApplication::Dispatch()` 冒頭で再送される。

## 3. API (`generic/environ/Application.h`)

### AppEventInterface

イベントを受信したいクラスが実装する。

```cpp
class AppEventInterface {
public:
    virtual ~AppEventInterface() {}
    // 自分宛のイベントなら処理して true を返す。判定は message だけで行う。
    virtual bool Dispatch(tjs_int message, tjs_int64 wparam, tjs_int64 lparam) = 0;
};
```

実装クラスは **コンストラクタで `Application->addEventHandler(this)`**、
**デストラクタで `Application->removeEventHandler(this)`** を呼んで自身を登録/解除する
(旧 `Allocate()` / `Deallocate()` の置き換え)。

### tTVPApplication

```cpp
// 環境別実装。SDL は即 SDL_Event に詰めて送信し、失敗したら false。
virtual bool _SendAppEvent(tjs_int message, tjs_int64 wparam, tjs_int64 lparam) = 0;

// 任意スレッドから呼べる。_SendAppEvent し、失敗時だけ retry_que_ に積む(要ロック)。
void SendAppEvent(tjs_int message, tjs_int64 wparam, tjs_int64 lparam);

// システム側からの呼び返し。メインスレッドで動く。
// app 独自イベントを処理し、未消費なら addEventHandler 済みの全ハンドラへ配る。
bool DispatchAppEvent(tjs_int message, tjs_int64 wparam, tjs_int64 lparam);

void addEventHandler(AppEventInterface* handler);
void removeEventHandler(AppEventInterface* handler);

// 吉里吉里内部処理。冒頭で retry_que_ を再送し、UpdateVideoOverlay / DeliverEvents。
void Dispatch();
```

`retry_que_` は `std::queue<std::tuple<tjs_int,tjs_int64,tjs_int64>>` + 専用 mutex。
`event_handlers_` とその mutex は従来どおり残る。

## 4. SDL 実装 (`sdl3/`)

- `SDL_AppInit` で `mAppEventType = SDL_RegisterEvents(1)` を一度だけ確保し
  `SDL3Application` が保持する。
- `_SendAppEvent`: `SDL_Event` を `type = mAppEventType`, `user.code = message`,
  `user.data1 = (void*)wparam`, `user.data2 = (void*)lparam` で構築して
  `SDL_PushEvent`。戻り値が成功なら true。
- `SDL_AppEvent`: `event->type == mAppEventType` のとき
  `app->DispatchAppEvent(event->user.code, (tjs_int64)event->user.data1,
  (tjs_int64)event->user.data2)` を呼び、`SDL_APP_CONTINUE` を返す。

### message ID と SDL イベント型の衝突について

`AM_*` / `TVP_EV_*` の値は SDL の `user.code` (Sint32) に格納するだけで、SDL の
**イベント型空間** (`event->type`) には単一の `mAppEventType` しか使わない。
よって `AM_*` の値が SDL の予約イベント型とぶつかる心配はない。

### 32bit ターゲットについて

`user.data1/data2` は `void*` であり、64bit ポインタ環境では `wparam`/`lparam`
(64bit) をそのまま載せられる。32bit ポインタ環境では桁落ちするため当面非対応
(主要 Generic ターゲットは 64bit)。対応が必要になった場合は小構造体を heap 確保して
ポインタ渡し → dispatch 時に free する経路を足す (TODO)。

## 5. 入力イベント (touch / mouse) は同期処理

旧構造では WindowForm の touch/mouse も `NativeEvent` 経由で非同期 post して
いたが、SDL ビルドでは `SendTouchMessage` / `SendMouseMessage` の発行元は
すべて `sdl3/environ/form.cpp` の SDL イベントコールバック = **メインスレッド**
である。マーシャリング不要なので、WindowForm 内で `OnTouchDown` / `OnMouseDown`
等を **直接同期呼び出し** する。

`OnTouch*` / `OnMouse*` は内部で `TVPPostInputEvent` (TJS レベルの入力イベント
キュー) へ積むだけで、スクリプトを直接実行しないので同期呼び出しでも安全。

これにより、touch が必要としていた 3 つ目の 64bit チャネル (旧 `NativeEvent::Result`
に入れていた `tick`) の置き場問題が解消する。`SendAppEvent` は
`AM_REQUEST_UPDATE` / `AM_DISPLAY_RESIZE` / `AM_SURFACE_*` / `AM_RESUME` /
`AM_PAUSE` 等、payload が int で 2×64bit に収まる通知系専用になる。

## 6. WINVER と共有しているコード

`common/` 配下の 2 クラス `tTVPTimerThread` (`common/utils/TimerThread`) と
`tTVPSoundEventThread` (`common/sound/SoundEventThread`) は WINVER/SDL 両方で
コンパイルされる。WINVER 側はキューごとの Win32 窓機構 (`win32/base/NativeEventQueue`)
を**無改造で温存**するため、この 2 クラスは `#ifdef __WINVER__` で分岐する。

両クラスとも worker スレッドから 1 種類の wake メッセージ
(`TVP_EV_TIMER_THREAD` / `TVP_EV_WAVE_SND_BUF_THREAD`) を投げ、メインスレッドの
ハンドラがペンディング処理を drain するだけで、**wparam/lparam を使わない**。
そのため本体ロジック (例: `HandleWake()`) は共有し、機構だけを分岐する:

```cpp
#ifdef __WINVER__
  #include "NativeEventQueue.h"
#else
  #include "Application.h"
#endif

class tTVPTimerThread : public tTVPThread
#ifndef __WINVER__
    , public AppEventInterface
#endif
{
#ifdef __WINVER__
    NativeEventQueue<tTVPTimerThread> EventQueue;     // ctor: (this, &Proc)
    void Proc(NativeEvent& ev) {
        if (ev.Message == TVP_EV_TIMER_THREAD && !GetTerminated()) HandleWake();
        else EventQueue.HandlerDefault(ev);
    }
#else
    bool Dispatch(tjs_int message, tjs_int64, tjs_int64) override {
        if (message == TVP_EV_TIMER_THREAD && !GetTerminated()) { HandleWake(); return true; }
        return false;
    }
#endif
    void HandleWake(); // 旧 Proc の message 分岐内の本体
};
```

- ctor: `EventQueue.Allocate()` ↔ `Application->addEventHandler(this)`
- dtor: `EventQueue.Deallocate()` ↔ `Application->removeEventHandler(this)`
- post: `EventQueue.PostEvent(NativeEvent(MSG))` ↔ `Application->SendAppEvent(MSG, 0, 0)`

WINVER 専用クライアント (`tTVPVSyncTimingThread` / `tTJSNI_VideoOverlay` /
`tTVPWaveSoundBufferThread` / `tTVPContinuousHandlerCallLimitThread`) は
`win32/` 配下のみでコンパイルされるため一切触らない。

## 7. 移行に伴う削除物

- `generic/base/NativeEventQueue.{h,cpp}` を削除 (`sources.cmake` の該当行も)。
  WINVER 側 `win32/base/NativeEventQueue.{h,cpp}` は残す。
- `tTVPApplication` から `EventCommand` / `command_que_` / `command_cache_` /
  `createNativeEvent` / `releaseNativeEvent` / `postEvent` / `command_que_mutex_` /
  `command_cache_mutex_` / `NativeEvent` 前方宣言 を削除。
- `StorageCache.h` / `QueueSoundBufferImpl.cpp` / `generic/base/EventImpl.cpp` の
  不要な `#include "NativeEventQueue.h"` を除去 (これらはテンプレ未使用)。
