#include "tjsCommHead.h"
#include "MemoryOverlayRender.h"

#ifdef KRKRZ_ENABLE_MEMORY_OVERLAY

#include "MemoryOverlay.h"
#include "ThreadIntf.h"
#include "DebugIntf.h"

#include <SDL3/SDL.h>
#include <cstdio>
#include <cstring>
#include <vector>
#include <algorithm>

namespace {

constexpr float kOverlayScale = 1.5f;    // パネル全体をこの倍率で描画 (SDL_SetRenderScale)
constexpr int kPanelW       = 320;
// Sound 行 + SysFree 行 ぶんで +24 (kRowH × 2)
#if defined(KRKRZ_DRAW_STATS) && defined(KRKRZ_SDLMEMORY_STAT)
// Draw stats + render stats で +7 行 + キャッシュ件数 2 行 + GlobalAlloc 2 行 (GblK + GblS) + Sound 1 行 + SysFree 1 行
constexpr int kPanelH       = 308;
constexpr int kHeaderH      = 228;
#elif defined(KRKRZ_DRAW_STATS)
// Draw stats あり、SDL stats なし → GlobalAlloc 1 行のみ (GblK) + Sound 1 行 + SysFree 1 行
constexpr int kPanelH       = 296;
constexpr int kHeaderH      = 216;
#elif defined(KRKRZ_SDLMEMORY_STAT)
// 通常時: 11 行 (FPS + File + Bitmap + Sound + RSS + Alloc/s + FileCache + ImageCache + GblK + GblS + SysFree)
constexpr int kPanelH       = 224;
constexpr int kHeaderH      = 144;
#else
// SDL stats なし: 10 行 (GblS が消える、Sound + SysFree 追加)
constexpr int kPanelH       = 212;
constexpr int kHeaderH      = 132;
#endif
constexpr int kPanelMargin  = 8;
constexpr int kRowH         = 12;
constexpr uint32_t kFpsRefreshMs = 500; // 500ms ごとに表示値を更新
constexpr uint32_t kStatsRefreshMs = 500; // DrawStats の表示値更新間隔

struct SeriesSpec {
	const char *name;
	SDL_Color   color;
};

double ExtractFile  (const TVPMemoryOverlaySample &s) { return (double)s.file_used; }
double ExtractBitmap(const TVPMemoryOverlaySample &s) { return (double)s.bitmap_used; }
double ExtractSound (const TVPMemoryOverlaySample &s) { return (double)s.sound_used; }
double ExtractRSS   (const TVPMemoryOverlaySample &s) { return (double)s.process_rss; }

// "D:\work\.../foo.cpp:123" のような長い path から filename:line のみ抽出。
// log を読みやすくするためで、site ポインタは literal なのでこの関数では指していない。
const char *TrimSitePath(const char *p) {
	if (!p) return "";
	const char *last = p;
	for (const char *s = p; *s; ++s) {
		if (*s == '/' || *s == '\\') last = s + 1;
	}
	return last;
}

void DrawSeriesLine(SDL_Renderer *r, const std::vector<TVPMemoryOverlaySample> &samples,
                    double (*extract)(const TVPMemoryOverlaySample &),
                    SDL_Color color, double max_val,
                    int graph_x, int graph_y, int graph_w, int graph_h)
{
	if (samples.size() < 2 || max_val <= 0) return;
	SDL_SetRenderDrawColor(r, color.r, color.g, color.b, color.a);
	const size_t n = samples.size();
	const double x_step = (n > 1) ? (double)graph_w / (double)(TVPMemoryOverlay::kMaxSamples - 1) : 0.0;
	// 右端を最新サンプルに合わせる: index i は右から数える
	const size_t base = TVPMemoryOverlay::kMaxSamples - n; // n < kMaxSamples のとき左を空ける
	for (size_t i = 1; i < n; ++i) {
		double v0 = extract(samples[i - 1]);
		double v1 = extract(samples[i]);
		float x0 = (float)(graph_x + (base + i - 1) * x_step);
		float x1 = (float)(graph_x + (base + i)     * x_step);
		float y0 = (float)(graph_y + graph_h - (v0 / max_val) * graph_h);
		float y1 = (float)(graph_y + graph_h - (v1 / max_val) * graph_h);
		SDL_RenderLine(r, x0, y0, x1, y1);
	}
}

} // anonymous namespace

// renderer == nullptr の場合は描画 skip、計測値更新 + log 出力のみ実行する
// (SDLOGLDrawDevice 等の SDL_Renderer 不在経路から計測のため呼ばれる)。
void TVPRenderMemoryOverlay(SDL_Renderer *renderer)
{
	const bool log_only = (renderer == nullptr);
	if (!TVPMemoryOverlay::IsEnabled()) return;

	// FPS 計測。本関数は Show() の Render lambda 末尾から呼ばれるので、
	// 呼び出し回数 = レンダリング frame 数。500ms 単位で平均 FPS を更新。
	// (overlay OFF のときは関数が早期 return するので静止状態にもなる)
	static uint32_t s_fps_last_refresh_ms = 0;
	static int      s_fps_frame_count     = 0;
	static double   s_fps_value           = 0.0;
	{
		const uint32_t now_ms = SDL_GetTicks();
		s_fps_frame_count++;
		if (s_fps_last_refresh_ms == 0) {
			s_fps_last_refresh_ms = now_ms;
		} else {
			const uint32_t elapsed = now_ms - s_fps_last_refresh_ms;
			if (elapsed >= kFpsRefreshMs) {
				s_fps_value = (double)s_fps_frame_count * 1000.0 / (double)elapsed;
				s_fps_frame_count     = 0;
				s_fps_last_refresh_ms = now_ms;
			}
		}
	}

	std::vector<TVPMemoryOverlaySample> samples;
	TVPMemoryOverlay::GetSnapshot(samples);
	if (samples.empty()) return;

	int win_w = 0, win_h = 0;
	int scaled_win_w = 0;
	int px = 0, py = 0;
	float orig_scale_x = 1.0f, orig_scale_y = 1.0f;
	if (!log_only) {
		SDL_GetCurrentRenderOutputSize(renderer, &win_w, &win_h);
		scaled_win_w = (int)((float)win_w / kOverlayScale);
		if (scaled_win_w < kPanelW + kPanelMargin * 2) return; // 小さすぎる

		// パネル全体を kOverlayScale 倍で描画。SDL_SetRenderScale はテキストや
		// 矩形すべてに効くので、コード側はパネル"論理座標"のままで OK。
		// ここで設定し、関数末尾で元に戻す。
		SDL_GetRenderScale(renderer, &orig_scale_x, &orig_scale_y);
		SDL_SetRenderScale(renderer, kOverlayScale, kOverlayScale);

		// スケール後の座標系で右上に配置する
		px = scaled_win_w - kPanelW - kPanelMargin;
		py = kPanelMargin;

		// 半透明背景
		SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_BLEND);
		SDL_SetRenderDrawColor(renderer, 0, 0, 0, 180);
		SDL_FRect bg{(float)px, (float)py, (float)kPanelW, (float)kPanelH};
		SDL_RenderFillRect(renderer, &bg);

		// 枠
		SDL_SetRenderDrawColor(renderer, 96, 96, 96, 255);
		SDL_RenderRect(renderer, &bg);
	}

	// バイト系 4 系列の最大値を共通スケールで決定 (見た目が比較しやすい)
	double max_bytes = 0.0;
	for (auto &s : samples) {
		if ((double)s.file_used   > max_bytes) max_bytes = (double)s.file_used;
		if ((double)s.bitmap_used > max_bytes) max_bytes = (double)s.bitmap_used;
		if ((double)s.sound_used  > max_bytes) max_bytes = (double)s.sound_used;
		if ((double)s.process_rss > max_bytes) max_bytes = (double)s.process_rss;
	}
	if (max_bytes <= 0) max_bytes = 1.0;

	// テキスト (現在値)
	auto &latest = samples.back();
	char buf[160];
	const SDL_Color cFPS {255, 255, 255, 255};
	const SDL_Color cFile{255, 96,  96,  255};
	const SDL_Color cBmap{96,  255, 96,  255};
	const SDL_Color cSnd {255, 160, 255, 255};
	const SDL_Color cRSS {96,  160, 255, 255};
	const SDL_Color cRate{255, 220, 96,  255};

	int line_y = py + 4;

	auto putLine = [&](SDL_Color c, const char *fmt, auto... args) {
		// log_only モード (renderer == nullptr) では描画スキップ、line_y も進めない。
		if (!renderer) return;
		SDL_SetRenderDrawColor(renderer, c.r, c.g, c.b, c.a);
		// fmt はテンプレート展開時に literal に解決されるが、
		// -Wformat-security 警告回避のため分岐で SDL_RenderDebugText に直渡し。
		if constexpr (sizeof...(args) == 0) {
			SDL_RenderDebugText(renderer, (float)(px + 6), (float)line_y, fmt);
		} else {
			std::snprintf(buf, sizeof(buf), fmt, args...);
			SDL_RenderDebugText(renderer, (float)(px + 6), (float)line_y, buf);
		}
		line_y += kRowH;
	};

	putLine(cFPS, "FPS:     %7.1f", s_fps_value);

	const double MB = 1024.0 * 1024.0;
	putLine(cFile, "File:    %7.2f MB (peak %7.2f)",
	        latest.file_used   / MB, latest.file_peak   / MB);
	putLine(cBmap, "Bitmap:  %7.2f MB (peak %7.2f)",
	        latest.bitmap_used / MB, latest.bitmap_peak / MB);
	putLine(cSnd,  "Sound:   %7.2f MB (peak %7.2f)",
	        latest.sound_used  / MB, latest.sound_peak  / MB);
	putLine(cRSS,  "RSS:     %7.2f MB", latest.process_rss / MB);

	// alloc rate (直近 1 サンプル分の delta を 1 秒換算)
	if (samples.size() >= 2) {
		auto &prev = samples[samples.size() - 2];
		uint64_t df = latest.file_alloc_count   - prev.file_alloc_count;
		uint64_t db = latest.bitmap_alloc_count - prev.bitmap_alloc_count;
		uint64_t ds = latest.sound_alloc_count  - prev.sound_alloc_count;
		double mul = 1000.0 / (double)TVPMemoryOverlay::kSampleIntervalMs;
		putLine(cRate, "Alloc/s F:%5llu B:%5llu S:%5llu",
		        (unsigned long long)((double)df * mul),
		        (unsigned long long)((double)db * mul),
		        (unsigned long long)((double)ds * mul));
	} else {
		putLine(cRate, "Alloc/s  (collecting...)");
	}

	// キャッシュエントリ件数 (file 層 / decode 層)。pinned は内訳。
	const SDL_Color cFCache{160, 192, 255, 255};
	const SDL_Color cICache{255, 192, 160, 255};
	putLine(cFCache, "FileCache:  %5zu (pin %5zu)",
	        latest.file_cache_count, latest.file_cache_pinned);
	putLine(cICache, "ImageCache: %5zu (pin %5zu)",
	        latest.image_cache_count, latest.image_cache_pinned);

	// GlobalAllocStats (operator new + TJS_malloc + SDL3 alloc を一元集計)。
	// pool_cap > 0 なら pool 経路、0 は無効化または tracking 未活性。
	// fallback > 0 は pool 容量超過の累積回数 (赤系で警告色)。
	// GblS (SDL) 行は KRKRZ_SDLMEMORY_STAT=ON のときだけ表示。
	const SDL_Color cGblK    {200, 255, 200, 255};
	const SDL_Color cGblWarn {255, 120, 120, 255};
	if (latest.krkrz_pool_cap > 0) {
		bool ovf = latest.krkrz_fallback_count > 0;
		putLine(ovf ? cGblWarn : cGblK,
		        "GblK live%5.1fM pool%5.1f/%4lluM fb%llu",
		        latest.krkrz_live      / MB,
		        latest.krkrz_pool_used / MB,
		        (unsigned long long)(latest.krkrz_pool_cap / (uint64_t)MB),
		        (unsigned long long)latest.krkrz_fallback_count);
	} else {
		putLine(cGblK, "GblK live%5.1fM (no pool)", latest.krkrz_live / MB);
	}
#ifdef KRKRZ_SDLMEMORY_STAT
	const SDL_Color cGblS    {200, 220, 255, 255};
	if (latest.sdl_pool_cap > 0) {
		bool ovf = latest.sdl_fallback_count > 0;
		putLine(ovf ? cGblWarn : cGblS,
		        "GblS live%5.1fM pool%5.1f/%4lluM fb%llu",
		        latest.sdl_live      / MB,
		        latest.sdl_pool_used / MB,
		        (unsigned long long)(latest.sdl_pool_cap / (uint64_t)MB),
		        (unsigned long long)latest.sdl_fallback_count);
	} else {
		putLine(cGblS, "GblS live%5.1fM (no pool)", latest.sdl_live / MB);
	}
#endif

	// システム空き容量 (iTVPSystemAllocatorInfo 経由)。
	// コンソール機等のプラットフォーム固有実装では正確な空き容量が取得できる。
	// 一般 OS ではシステムの空き物理メモリが近似値として入る。
	// パネル高は kPanelH 側で常に確保済みなので、データ未取得時は "--" を表示。
	const SDL_Color cSysFree{128, 255, 220, 255};
	if (latest.sys_total_free > 0 || latest.sys_allocatable > 0) {
		putLine(cSysFree, "SysFree: %6.1fM  Allocatable: %6.1fM",
		        latest.sys_total_free  / MB,
		        latest.sys_allocatable / MB);
	} else {
		putLine(cSysFree, "SysFree: --       Allocatable: --");
	}

#ifdef KRKRZ_DRAW_STATS
	// DrawThreadPool 利用統計。500ms ごとに前回 snapshot との delta を計算して
	// 1 秒換算で表示する (Switch 等で他コアが使われない問題の調査用)。
	static TVPDrawThreadStatsSnapshot s_prev_stats = {};
	static bool                       s_stats_inited = false;
	static uint32_t                   s_stats_last_refresh_ms = 0;
	// 表示用に保持する 1 秒換算済みの値
	static double s_disp_begin_per_sec = 0.0;
	static int    s_disp_t1_pct        = 0;
	static int    s_disp_nt_pct        = 0;  // d_begin=0 のとき 0 (NT=100% と誤表示しないため独立に保持)
	static double s_disp_worker_ms_per_sec = 0.0;
	static double s_disp_main_ms_per_sec   = 0.0;
	static double s_disp_spin_ms_per_sec   = 0.0;
	{
		const uint32_t now_ms = SDL_GetTicks();
		if (!s_stats_inited) {
			TVPGetDrawThreadStats(s_prev_stats);
			s_stats_last_refresh_ms = now_ms;
			s_stats_inited = true;
		} else if (now_ms - s_stats_last_refresh_ms >= kStatsRefreshMs) {
			TVPDrawThreadStatsSnapshot cur;
			TVPGetDrawThreadStats(cur);
			const uint32_t elapsed = now_ms - s_stats_last_refresh_ms;
			const double per_sec = elapsed > 0 ? (1000.0 / (double)elapsed) : 0.0;
			tjs_uint64 d_begin  = cur.begin_count      - s_prev_stats.begin_count;
			tjs_uint64 d_t1     = cur.task_hist[1]     - s_prev_stats.task_hist[1];
			tjs_uint64 d_worker = cur.worker_active_ns - s_prev_stats.worker_active_ns;
			tjs_uint64 d_main   = cur.main_active_ns   - s_prev_stats.main_active_ns;
			tjs_uint64 d_spin   = cur.wait_spin_ns     - s_prev_stats.wait_spin_ns;
			s_disp_begin_per_sec     = (double)d_begin * per_sec;
			s_disp_t1_pct            = d_begin ? (int)(100 * d_t1 / d_begin) : 0;
			s_disp_nt_pct            = d_begin ? (100 - s_disp_t1_pct)       : 0;
			s_disp_worker_ms_per_sec = (double)d_worker / 1.0e6 * per_sec;
			s_disp_main_ms_per_sec   = (double)d_main   / 1.0e6 * per_sec;
			s_disp_spin_ms_per_sec   = (double)d_spin   / 1.0e6 * per_sec;
			s_prev_stats = cur;
			s_stats_last_refresh_ms = now_ms;
			// TJS から System.setDrawStatsLog(true) されている間は、
			// 同タイミングで log にも書き出す (実機のリアルタイム表示が
			// 速く流れて記録できないとき用)。FPS は参考値として併記。
			if (TVPDrawStatsLogEnabled) {
				char log_buf[256];
				std::snprintf(log_buf, sizeof(log_buf),
					"DrawStats: FPS=%.1f Draw=%.0f/s 1T=%d%% NT=%d%% Wkr=%.1fms/s Main=%.1fms/s Spin=%.1fms/s",
					s_fps_value,
					s_disp_begin_per_sec, s_disp_t1_pct, s_disp_nt_pct,
					s_disp_worker_ms_per_sec, s_disp_main_ms_per_sec, s_disp_spin_ms_per_sec);
				TVPAddLog(ttstr(log_buf));

				// 上位 callsite を delta で計算して別行に出力。"site=<count>/<t1_count>"
				// 形式 (count=その slot の dispatch 回数、t1_count=うち 1 thread に落ちた回数)。
				static TVPDrawCallsiteSnapshot s_prev_sites[TVPDrawCallsiteMax] = {};
				TVPDrawCallsiteSnapshot cur_sites[TVPDrawCallsiteMax];
				TVPGetDrawCallsiteSnapshots(cur_sites);
				struct Entry { const char *site; tjs_uint64 dcount; tjs_uint64 dt1; };
				Entry entries[TVPDrawCallsiteMax];
				int n_entries = 0;
				for (int i = 0; i < TVPDrawCallsiteMax; ++i) {
					if (!cur_sites[i].site) continue;
					tjs_uint64 dc = cur_sites[i].count    - s_prev_sites[i].count;
					if (dc == 0) continue;
					tjs_uint64 dt = cur_sites[i].t1_count - s_prev_sites[i].t1_count;
					entries[n_entries++] = { cur_sites[i].site, dc, dt };
				}
				std::sort(entries, entries + n_entries,
					[](const Entry &a, const Entry &b) { return a.dcount > b.dcount; });
				if (n_entries > 0) {
					char site_buf[512];
					int written = std::snprintf(site_buf, sizeof(site_buf), "DrawSites:");
					int top = (n_entries < 3) ? n_entries : 3;
					for (int i = 0; i < top && written < (int)sizeof(site_buf) - 1; ++i) {
						int n = std::snprintf(site_buf + written, sizeof(site_buf) - written,
							" %s=%llu/%llu",
							TrimSitePath(entries[i].site),
							(unsigned long long)entries[i].dcount,
							(unsigned long long)entries[i].dt1);
						if (n < 0) break;
						written += n;
					}
					TVPAddLog(ttstr(site_buf));
				}
				for (int i = 0; i < TVPDrawCallsiteMax; ++i) s_prev_sites[i] = cur_sites[i];
			}
		}
	}
	const SDL_Color cDraw{220, 180, 255, 255};
	putLine(cDraw, "Draw   %5.0f/s  1T:%2d%% NT:%2d%%",
	        s_disp_begin_per_sec, s_disp_t1_pct, s_disp_nt_pct);
	putLine(cDraw, "Wkr:%5.1f Main:%5.1f Spin:%5.1f ms/s",
	        s_disp_worker_ms_per_sec, s_disp_main_ms_per_sec, s_disp_spin_ms_per_sec);

	// テクスチャ更新 (DrawThreadPool 外の main 占有経路) を 1 行追加。
	// TexUp = src→中間 memcpy / TexRen = 中間→GPU memcpy / MB/s = Update 側コピー量。
	static TVPRenderStatsSnapshot s_prev_render = {};
	static bool                   s_render_inited = false;
	static uint32_t               s_render_last_refresh_ms = 0;
	static double s_disp_tex_update_ms_per_sec = 0.0;
	static double s_disp_tex_render_ms_per_sec = 0.0;
	static double s_disp_tex_mb_per_sec        = 0.0;
	{
		const uint32_t now_ms_r = SDL_GetTicks();
		if (!s_render_inited) {
			TVPGetRenderStats(s_prev_render);
			s_render_last_refresh_ms = now_ms_r;
			s_render_inited = true;
		} else if (now_ms_r - s_render_last_refresh_ms >= kStatsRefreshMs) {
			TVPRenderStatsSnapshot cur_r;
			TVPGetRenderStats(cur_r);
			const uint32_t elapsed = now_ms_r - s_render_last_refresh_ms;
			const double per_sec = elapsed > 0 ? (1000.0 / (double)elapsed) : 0.0;
			tjs_uint64 d_up = cur_r.tex_update_ns - s_prev_render.tex_update_ns;
			tjs_uint64 d_rn = cur_r.tex_render_ns - s_prev_render.tex_render_ns;
			tjs_uint64 d_by = cur_r.tex_bytes     - s_prev_render.tex_bytes;
			s_disp_tex_update_ms_per_sec = (double)d_up / 1.0e6 * per_sec;
			s_disp_tex_render_ms_per_sec = (double)d_rn / 1.0e6 * per_sec;
			s_disp_tex_mb_per_sec        = (double)d_by / (1024.0 * 1024.0) * per_sec;
			s_prev_render = cur_r;
			s_render_last_refresh_ms = now_ms_r;
			if (TVPDrawStatsLogEnabled) {
				char log_buf[256];
				std::snprintf(log_buf, sizeof(log_buf),
					"RenderStats: TexUp=%.1fms/s TexRen=%.1fms/s Copy=%.1fMB/s",
					s_disp_tex_update_ms_per_sec, s_disp_tex_render_ms_per_sec,
					s_disp_tex_mb_per_sec);
				TVPAddLog(ttstr(log_buf));
			}
		}
	}
	const SDL_Color cTex{255, 200, 120, 255};
	putLine(cTex, "TexUp:%4.0f Ren:%4.0f Copy:%5.0fMB/s",
	        s_disp_tex_update_ms_per_sec, s_disp_tex_render_ms_per_sec, s_disp_tex_mb_per_sec);

	// Show() 内 section 別計測 (DrawThreadPool 外 + テクスチャ転送外で消えてる時間の正体探索用)。
	// Clr=SDL_RenderClear, Tex=SDL_RenderTextureRotated, Ovl=TVPRenderMemoryOverlay,
	// Pres=SDL_RenderPresent (vsync 待ち + flush)。
	static double s_disp_show_clear_ms_per_sec   = 0.0;
	static double s_disp_show_tex_ms_per_sec     = 0.0;
	static double s_disp_show_overlay_ms_per_sec = 0.0;
	static double s_disp_show_present_ms_per_sec = 0.0;
	{
		// 上の RenderStats refresh で s_prev_render が更新済み。snapshot は再取得せず
		// すでに refresh タイミングだったかどうかの判定だけ流用したいが、ここでは
		// 簡単のため独立に refresh する (同じ 500ms で動く)。
		static TVPRenderStatsSnapshot s_prev_show = {};
		static bool                   s_show_inited = false;
		static uint32_t               s_show_last_refresh_ms = 0;
		const uint32_t now_ms_s = SDL_GetTicks();
		if (!s_show_inited) {
			TVPGetRenderStats(s_prev_show);
			s_show_last_refresh_ms = now_ms_s;
			s_show_inited = true;
		} else if (now_ms_s - s_show_last_refresh_ms >= kStatsRefreshMs) {
			TVPRenderStatsSnapshot cur_s;
			TVPGetRenderStats(cur_s);
			const uint32_t elapsed = now_ms_s - s_show_last_refresh_ms;
			const double per_sec = elapsed > 0 ? (1000.0 / (double)elapsed) : 0.0;
			tjs_uint64 d_clr = cur_s.show_clear_ns   - s_prev_show.show_clear_ns;
			tjs_uint64 d_tx  = cur_s.show_tex_ns     - s_prev_show.show_tex_ns;
			tjs_uint64 d_ov  = cur_s.show_overlay_ns - s_prev_show.show_overlay_ns;
			tjs_uint64 d_pr  = cur_s.show_present_ns - s_prev_show.show_present_ns;
			s_disp_show_clear_ms_per_sec   = (double)d_clr / 1.0e6 * per_sec;
			s_disp_show_tex_ms_per_sec     = (double)d_tx  / 1.0e6 * per_sec;
			s_disp_show_overlay_ms_per_sec = (double)d_ov  / 1.0e6 * per_sec;
			s_disp_show_present_ms_per_sec = (double)d_pr  / 1.0e6 * per_sec;
			s_prev_show = cur_s;
			s_show_last_refresh_ms = now_ms_s;
			if (TVPDrawStatsLogEnabled) {
				char log_buf[256];
				std::snprintf(log_buf, sizeof(log_buf),
					"ShowStats: Clear=%.1fms/s Tex=%.1fms/s Overlay=%.1fms/s Present=%.1fms/s",
					s_disp_show_clear_ms_per_sec, s_disp_show_tex_ms_per_sec,
					s_disp_show_overlay_ms_per_sec, s_disp_show_present_ms_per_sec);
				TVPAddLog(ttstr(log_buf));
			}
		}
	}
	const SDL_Color cShow{180, 220, 255, 255};
	putLine(cShow, "Show Clr:%4.0f Tex:%4.0f Ovl:%4.0f Pres:%4.0f",
	        s_disp_show_clear_ms_per_sec, s_disp_show_tex_ms_per_sec,
	        s_disp_show_overlay_ms_per_sec, s_disp_show_present_ms_per_sec);

	// Frame phase 計測 (1 frame の main core 占有を Update / Show / Dispatch で 3 分割)。
	// Update は Layer 合成 (TexUp/TexRen 含む)、Show は GPU 描画 (Clr/Tex/Ovl/Pres 含む)、
	// Dispatch は event/scenario engine。3 つの和に近い値が main core 占有の本体。
	// 「Update - (TexUp+TexRen)」が未計測だった Layer 合成パイプライン部分を表す。
	static double s_disp_frame_update_ms_per_sec   = 0.0;
	static double s_disp_frame_show_ms_per_sec     = 0.0;
	static double s_disp_frame_dispatch_ms_per_sec = 0.0;
	{
		static TVPRenderStatsSnapshot s_prev_frame = {};
		static bool                   s_frame_inited = false;
		static uint32_t               s_frame_last_refresh_ms = 0;
		const uint32_t now_ms_f = SDL_GetTicks();
		if (!s_frame_inited) {
			TVPGetRenderStats(s_prev_frame);
			s_frame_last_refresh_ms = now_ms_f;
			s_frame_inited = true;
		} else if (now_ms_f - s_frame_last_refresh_ms >= kStatsRefreshMs) {
			TVPRenderStatsSnapshot cur_f;
			TVPGetRenderStats(cur_f);
			const uint32_t elapsed = now_ms_f - s_frame_last_refresh_ms;
			const double per_sec = elapsed > 0 ? (1000.0 / (double)elapsed) : 0.0;
			tjs_uint64 d_up = cur_f.frame_update_ns   - s_prev_frame.frame_update_ns;
			tjs_uint64 d_sh = cur_f.frame_show_ns     - s_prev_frame.frame_show_ns;
			tjs_uint64 d_ds = cur_f.frame_dispatch_ns - s_prev_frame.frame_dispatch_ns;
			s_disp_frame_update_ms_per_sec   = (double)d_up / 1.0e6 * per_sec;
			s_disp_frame_show_ms_per_sec     = (double)d_sh / 1.0e6 * per_sec;
			s_disp_frame_dispatch_ms_per_sec = (double)d_ds / 1.0e6 * per_sec;
			s_prev_frame = cur_f;
			s_frame_last_refresh_ms = now_ms_f;
			if (TVPDrawStatsLogEnabled) {
				char log_buf[256];
				std::snprintf(log_buf, sizeof(log_buf),
					"FrameStats: Update=%.1fms/s Show=%.1fms/s Dispatch=%.1fms/s",
					s_disp_frame_update_ms_per_sec, s_disp_frame_show_ms_per_sec,
					s_disp_frame_dispatch_ms_per_sec);
				TVPAddLog(ttstr(log_buf));
			}
		}
	}
	const SDL_Color cFrame{200, 255, 200, 255};
	putLine(cFrame, "Frame Up:%4.0f Sho:%4.0f Dsp:%4.0f",
	        s_disp_frame_update_ms_per_sec, s_disp_frame_show_ms_per_sec,
	        s_disp_frame_dispatch_ms_per_sec);

	// Layer 合成パイプライン phase 計測 (Phase 7+8、未計測 ~200 ms/s の場所特定用)。
	// CmpW = CompleteForWindow 全体、Cmp = InternalComplete2 top-level only、Drw = Draw 累計 (再帰込み)。
	// Update - CmpW ≈ UpdateToDrawDevice overhead (ほぼ 0 のはず)。
	// CmpW - Cmp ≈ BeforeCompletion + AfterCompletion + StartBitmapCompletion + EndBitmapCompletion +
	//             NotifyUpdateRegionFixed + GetUpdateRegionForCompletion (= 未計測 ~200 ms/s の真の場所)。
	// Cmp - (TexUp+Main+Spin) ≈ InternalComplete2 内の main only 処理 (実測ほぼ 0)。
	static double s_disp_layer_complete_window_ms_per_sec = 0.0;
	static double s_disp_layer_complete_ms_per_sec        = 0.0;
	static double s_disp_layer_draw_ms_per_sec            = 0.0;
	{
		static TVPRenderStatsSnapshot s_prev_layer = {};
		static bool                   s_layer_inited = false;
		static uint32_t               s_layer_last_refresh_ms = 0;
		const uint32_t now_ms_l = SDL_GetTicks();
		if (!s_layer_inited) {
			TVPGetRenderStats(s_prev_layer);
			s_layer_last_refresh_ms = now_ms_l;
			s_layer_inited = true;
		} else if (now_ms_l - s_layer_last_refresh_ms >= kStatsRefreshMs) {
			TVPRenderStatsSnapshot cur_l;
			TVPGetRenderStats(cur_l);
			const uint32_t elapsed = now_ms_l - s_layer_last_refresh_ms;
			const double per_sec = elapsed > 0 ? (1000.0 / (double)elapsed) : 0.0;
			tjs_uint64 d_cw = cur_l.layer_complete_window_ns - s_prev_layer.layer_complete_window_ns;
			tjs_uint64 d_cm = cur_l.layer_complete_ns        - s_prev_layer.layer_complete_ns;
			tjs_uint64 d_dr = cur_l.layer_draw_ns            - s_prev_layer.layer_draw_ns;
			s_disp_layer_complete_window_ms_per_sec = (double)d_cw / 1.0e6 * per_sec;
			s_disp_layer_complete_ms_per_sec        = (double)d_cm / 1.0e6 * per_sec;
			s_disp_layer_draw_ms_per_sec            = (double)d_dr / 1.0e6 * per_sec;
			s_prev_layer = cur_l;
			s_layer_last_refresh_ms = now_ms_l;
			if (TVPDrawStatsLogEnabled) {
				char log_buf[256];
				std::snprintf(log_buf, sizeof(log_buf),
					"LayerStats: CompleteW=%.1fms/s Complete=%.1fms/s Draw=%.1fms/s",
					s_disp_layer_complete_window_ms_per_sec,
					s_disp_layer_complete_ms_per_sec, s_disp_layer_draw_ms_per_sec);
				TVPAddLog(ttstr(log_buf));
			}
		}
	}
	const SDL_Color cLayer{220, 200, 255, 255};
	putLine(cLayer, "Layer CmpW:%4.0f Cmp:%4.0f Drw:%4.0f",
	        s_disp_layer_complete_window_ms_per_sec,
	        s_disp_layer_complete_ms_per_sec, s_disp_layer_draw_ms_per_sec);

	// Phase 9: BeforeCompletion / AfterCompletion 個別計測 (Layer 木全体に再帰、top-level only)。
	// CmpW - Cmp ≈ Bef + Aft + 微量 (NotifyUpdateRegionFixed / GetUpdateRegionForCompletion /
	// StartBitmapCompletion / EndBitmapCompletion はほぼ無視可)。
	// 「何もしてない Layer 木 traversal」の onPaint/Transition チェックがコスト主因の仮説を検証。
	static double s_disp_layer_before_ms_per_sec = 0.0;
	static double s_disp_layer_after_ms_per_sec  = 0.0;
	{
		static TVPRenderStatsSnapshot s_prev_ba = {};
		static bool                   s_ba_inited = false;
		static uint32_t               s_ba_last_refresh_ms = 0;
		const uint32_t now_ms_ba = SDL_GetTicks();
		if (!s_ba_inited) {
			TVPGetRenderStats(s_prev_ba);
			s_ba_last_refresh_ms = now_ms_ba;
			s_ba_inited = true;
		} else if (now_ms_ba - s_ba_last_refresh_ms >= kStatsRefreshMs) {
			TVPRenderStatsSnapshot cur_ba;
			TVPGetRenderStats(cur_ba);
			const uint32_t elapsed = now_ms_ba - s_ba_last_refresh_ms;
			const double per_sec = elapsed > 0 ? (1000.0 / (double)elapsed) : 0.0;
			tjs_uint64 d_b = cur_ba.layer_before_completion_ns - s_prev_ba.layer_before_completion_ns;
			tjs_uint64 d_a = cur_ba.layer_after_completion_ns  - s_prev_ba.layer_after_completion_ns;
			s_disp_layer_before_ms_per_sec = (double)d_b / 1.0e6 * per_sec;
			s_disp_layer_after_ms_per_sec  = (double)d_a / 1.0e6 * per_sec;
			s_prev_ba = cur_ba;
			s_ba_last_refresh_ms = now_ms_ba;
			if (TVPDrawStatsLogEnabled) {
				char log_buf[256];
				std::snprintf(log_buf, sizeof(log_buf),
					"LayerExStats: Before=%.1fms/s After=%.1fms/s",
					s_disp_layer_before_ms_per_sec, s_disp_layer_after_ms_per_sec);
				TVPAddLog(ttstr(log_buf));
			}
		}
	}
	const SDL_Color cLayerEx{200, 180, 240, 255};
	putLine(cLayerEx, "LayerEx Bef:%4.0f Aft:%4.0f",
	        s_disp_layer_before_ms_per_sec, s_disp_layer_after_ms_per_sec);
#endif

	// 折れ線グラフ領域
	if (!log_only) {
		const int graph_x = px + 6;
		const int graph_y = py + kHeaderH;
		const int graph_w = kPanelW - 12;
		const int graph_h = kPanelH - kHeaderH - 6;

		DrawSeriesLine(renderer, samples, &ExtractFile,   cFile, max_bytes, graph_x, graph_y, graph_w, graph_h);
		DrawSeriesLine(renderer, samples, &ExtractBitmap, cBmap, max_bytes, graph_x, graph_y, graph_w, graph_h);
		DrawSeriesLine(renderer, samples, &ExtractSound,  cSnd,  max_bytes, graph_x, graph_y, graph_w, graph_h);
		DrawSeriesLine(renderer, samples, &ExtractRSS,    cRSS,  max_bytes, graph_x, graph_y, graph_w, graph_h);

		// scale を元に戻す (後続の通常描画に影響させない)
		SDL_SetRenderScale(renderer, orig_scale_x, orig_scale_y);
	}
}

#else // !KRKRZ_ENABLE_MEMORY_OVERLAY

// OFF 時: SDLDrawDevice 等から呼ばれても何もしない stub。
void TVPRenderMemoryOverlay(SDL_Renderer *) {}

#endif // KRKRZ_ENABLE_MEMORY_OVERLAY
