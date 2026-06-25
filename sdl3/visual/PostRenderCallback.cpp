#include "tjsCommHead.h"
#include "PostRenderCallback.h"

#include <utility>
#include <vector>

namespace {

std::vector<std::pair<tTVPPostRenderCallback, void *>> &Callbacks()
{
	static std::vector<std::pair<tTVPPostRenderCallback, void *>> v;
	return v;
}

std::vector<std::pair<tTVPPostRenderCallbackGL, void *>> &CallbacksGL()
{
	static std::vector<std::pair<tTVPPostRenderCallbackGL, void *>> v;
	return v;
}

} // namespace

// ---- SDL_Renderer 経路 ----

void TVPRegisterPostRenderCallback(tTVPPostRenderCallback cb, void *userdata)
{
	if (!cb) return;
	auto &v = Callbacks();
	for (auto &p : v) {
		if (p.first == cb && p.second == userdata) return;
	}
	v.emplace_back(cb, userdata);
}

void TVPUnregisterPostRenderCallback(tTVPPostRenderCallback cb, void *userdata)
{
	auto &v = Callbacks();
	for (auto it = v.begin(); it != v.end(); ++it) {
		if (it->first == cb && it->second == userdata) {
			v.erase(it);
			return;
		}
	}
}

void TVPDispatchPostRenderCallbacks(SDL_Renderer *renderer)
{
	if (!renderer) return;
	auto &v = Callbacks();
	for (auto &p : v) {
		p.first(renderer, p.second);
	}
}

// ---- OpenGL ES 直接経路 ----

void TVPRegisterPostRenderCallbackGL(tTVPPostRenderCallbackGL cb, void *userdata)
{
	if (!cb) return;
	auto &v = CallbacksGL();
	for (auto &p : v) {
		if (p.first == cb && p.second == userdata) return;
	}
	v.emplace_back(cb, userdata);
}

void TVPUnregisterPostRenderCallbackGL(tTVPPostRenderCallbackGL cb, void *userdata)
{
	auto &v = CallbacksGL();
	for (auto it = v.begin(); it != v.end(); ++it) {
		if (it->first == cb && it->second == userdata) {
			v.erase(it);
			return;
		}
	}
}

void TVPDispatchPostRenderCallbacksGL()
{
	auto &v = CallbacksGL();
	for (auto &p : v) {
		p.first(p.second);
	}
}
