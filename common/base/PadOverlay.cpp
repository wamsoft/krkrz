#include "tjsCommHead.h"
#include "PadOverlay.h"
#include "SysInitIntf.h"  // TVPGetCommandLine
#include "DebugIntf.h"    // TVPAddLog

#include <atomic>

namespace {
std::atomic<bool> g_enabled{false};
}

namespace TVPPadOverlay {

void SetEnabled(bool enabled)
{
	g_enabled.store(enabled, std::memory_order_relaxed);
}

bool IsEnabled()
{
	return g_enabled.load(std::memory_order_relaxed);
}

} // namespace TVPPadOverlay

void TVPInitializePadOverlay()
{
	tTJSVariant val;
	if (TVPGetCommandLine(TJS_W("-padoverlay"), &val)) {
		if (((tjs_int)val) != 0) {
			TVPPadOverlay::SetEnabled(true);
			TVPAddLog(TJS_W("(info) PadOverlay enabled at startup"));
		}
	}
}
