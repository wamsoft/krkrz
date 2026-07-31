//---------------------------------------------------------------------------
// 動画オーバレイ presenter factory の登録レジストリ (中立)。
// 実体の presenter は各環境 (sdl3/visual/SDLVideoPresenter.cpp、
// common/visual/opengl/GLVideoPresenter.cpp 等) が供給し、起動時に
// TVPRegisterVideoOverlayPresenterFactory で自分の factory を登録する。
// 現行 DrawDevice に合う presenter は TVPCreateBoundVideoOverlayPresenter が
// 各 factory を試して選ぶ (RegisterWith が成功した最初のもの)。
//---------------------------------------------------------------------------
#include "tjsCommHead.h"
#include "VideoOverlayPresenter.h"

namespace {
const int TVPMaxVideoPresenterFactories = 8;
tTVPVideoOverlayPresenterFactory TVPVideoPresenterFactories[TVPMaxVideoPresenterFactories] = {};
int TVPVideoPresenterFactoryCount = 0;
}

void TVPRegisterVideoOverlayPresenterFactory(tTVPVideoOverlayPresenterFactory f)
{
	if (!f) return;
	for (int i = 0; i < TVPVideoPresenterFactoryCount; ++i)
		if (TVPVideoPresenterFactories[i] == f) return;   // 冪等
	if (TVPVideoPresenterFactoryCount < TVPMaxVideoPresenterFactories)
		TVPVideoPresenterFactories[TVPVideoPresenterFactoryCount++] = f;
}

iTVPVideoOverlayPresenter * TVPCreateBoundVideoOverlayPresenter(const tTJSVariant &drawDeviceObj)
{
	for (int i = 0; i < TVPVideoPresenterFactoryCount; ++i) {
		iTVPVideoOverlayPresenter* p = TVPVideoPresenterFactories[i]();
		if (!p) continue;
		if (p->Bind(drawDeviceObj)) return p;   // 現行デバイスに対応 → 束縛して採用 (pull は未開始)
		delete p;                               // 非対応 → 次の factory を試す
	}
	return nullptr;
}
