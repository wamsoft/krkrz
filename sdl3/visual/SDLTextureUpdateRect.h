#pragma once

#include "app.h"
#include <vector>
#include <chrono>
#include "BitmapBitsAlloc.h"
#include "DebugIntf.h"
#include "LogIntf.h"
#include "ThreadIntf.h"   // TVPRenderStatsAddTexUpdate / AddTexRender / BumpFrame

//---------------------------------------------------------------------------
// テクスチャ更新用クラス
//---------------------------------------------------------------------------

/**
 * @brief NotifyBitmapCompleted で渡されるピクセル領域を SDL テクスチャへアップロードする。
 *
 * 旧実装は中間バッファ (mBuffer) に memcpy で蓄積し、EndBitmapCompletion で
 * SDL_LockTexture + memcpy + SDL_UnlockTexture を矩形ごとに呼んでいた。
 * Switch 実機計測 (RenderStats: TexRen=200-400 ms/s) で 2 段目の Lock+memcpy が
 * main CPU の主要消費先と判明 → 中間バッファを廃して SDL_UpdateTexture で
 * src から GPU staging に 1 段で流す形に変更 (2026-05-09)。
 *
 * 2026-05-28: renderer 種別により以下 2 モードを切り替える。
 *   - mBottomUp = true  (GPU renderer): テクスチャは DIB と同じ bottom-up 配置で保持し、
 *                                       表示時に SDL_FLIP_VERTICAL で反転。
 *                                       bottom-up DIB ソースに対し memcpy ゼロ。
 *   - mBottomUp = false (SW renderer):  テクスチャは top-down (表示順) で保持し、表示は反転なし。
 *                                       bottom-up DIB ソースに対しては表示順に詰め直す
 *                                       (memcpy 必須)。SW renderer 側の SW_RenderCopyEx
 *                                       (= 中間サーフェス 3 枚 alloc/frame) を回避する目的。
 *
 * texture format が XBGR/ABGR の場合は DIB の BGRA byte order と R/B が逆なので per-pixel swap。
 */

class SDLTextureUpdateRect {

private:
    int             mWidth;
    int             mHeight;
    bool            mHasUpdate;             //< 当フレーム中に Update が 1 回以上呼ばれた
    bool            mBottomUp;              //< テクスチャ memory が bottom-up か (= 表示時 FLIP_VERTICAL を使うか)
    SDL_PixelFormat mTextureFormat;         //< テクスチャの SDL pixel format
    bool            mNeedRBSwap;            //< テクスチャ format が DIB と R/B 逆 (XBGR/ABGR 系)
    std::vector<tjs_uint8> mTmpRect;        //< 行反転 / format 変換用の一時バッファ (rect 単位、再利用)

    static bool IsRGBOrder(SDL_PixelFormat f) {
        // SDL_PIXELFORMAT_*BGR* は LE で memory R,G,B,(A/X) なので DIB (B,G,R,A) と R/B が逆。
        return f == SDL_PIXELFORMAT_XBGR8888 || f == SDL_PIXELFORMAT_ABGR8888;
    }

public:
    SDLTextureUpdateRect()
    : mWidth(0)
    , mHeight(0)
    , mHasUpdate(false)
    , mBottomUp(true)
    , mTextureFormat(SDL_PIXELFORMAT_XRGB8888)
    , mNeedRBSwap(false)
    {}

    ~SDLTextureUpdateRect() {}

    bool Empty() const { return !mHasUpdate; }

    // テクスチャの幾何と memory orientation / format を設定する。
    // SDLDrawDevice::CreateTexture から CreateTexture と同じパラメータで呼ばれる。
    void Configure(int w, int h, bool bottomUp, SDL_PixelFormat fmt) {
        mWidth         = w;
        mHeight        = h;
        mBottomUp      = bottomUp;
        mTextureFormat = fmt;
        mNeedRBSwap    = IsRGBOrder(fmt);
    }

    // 旧 API 互換 (orientation / format は前回値を維持)。
    void Resize(int w, int h) {
        mWidth  = w;
        mHeight = h;
    }

    void Clear() {
        mHasUpdate = false;
    }

    // [NOTE] geometory (x,y,w,h) requires clipping on (0,0,mWidth,mHeight)
    // 呼び出し側 (NotifyBitmapCompleted) は src_p / src_pitch を以下の形で渡してくる:
    //   - bottom-up DIB: src_p = bits + _pitch*(_height-1), src_pitch = -_pitch
    //   - top-down  DIB: src_p = bits,                       src_pitch = +_pitch
    // すなわち visual_top = src_p + src_pitch * src_y + src_x * 4 が常に「表示上の上端行」を指し、
    // 続く h 行は src_pitch (正でも負でも) を加算していくと表示順に取れる。
    void Update(SDL_Texture *texture, int x, int y, int w, int h,
                const tjs_uint8 *src_p, int src_pitch, int src_x, int src_y)
    {
        if (!texture) return;
#ifdef KRKRZ_DRAW_STATS
        const auto _stats_t0 = std::chrono::steady_clock::now();
#endif
        const tjs_uint8 *visual_top = src_p + src_pitch * src_y + src_x * 4;
        const size_t     row_bytes  = (size_t)w * 4;

        // テクスチャ memory 上の宛先 rect。
        // bottom-up モード: 表示 (x,y) は memory (x, mHeight - y - h) へ。
        //                   さらに表示の上端行はメモリ最下位行 (= visual_top + (h-1)*src_pitch) になる。
        // top-down  モード: 表示 (x,y) はそのまま memory (x,y) へ。表示の上端行は visual_top。
        const SDL_Rect rect = mBottomUp
            ? SDL_Rect{ x, mHeight - y - h, w, h }
            : SDL_Rect{ x, y,              w, h };

        const tjs_uint8 *upload_src;
        int              upload_pitch;

        if (!mNeedRBSwap && mBottomUp && src_pitch < 0) {
            // 最速パス: bottom-up DIB + format 一致 = memcpy ゼロ。
            // visual_top + src_pitch*(h-1) でメモリ最下位 (= 表示下端の行 = テクスチャ memory 上端) を取得し、
            // 正 pitch (-src_pitch) で連続行アップロード。
            upload_src   = visual_top + src_pitch * (h - 1);
            upload_pitch = -src_pitch;
        } else if (!mNeedRBSwap && !mBottomUp && src_pitch > 0) {
            // 最速パス: top-down DIB + format 一致 = memcpy ゼロ。
            upload_src   = visual_top;
            upload_pitch = src_pitch;
        } else {
            // 一時バッファに表示順 (top-down) で連続アップロード可能な形に詰め直す。
            // bottom-up モードならその後さらに反転、ではなく、texture memory への
            // 書き込み順が「rect の memory rect (= 上から下) に上 → 下」になるよう詰める。
            //   bottom-up モード → texture mem 上端 = 表示下端 → row (h-1) から逆順
            //   top-down  モード → texture mem 上端 = 表示上端 → row 0 から順
            mTmpRect.resize(row_bytes * (size_t)h);
            tjs_uint8 *dest_top = mTmpRect.data();

            auto copy_row = [&](tjs_uint8 *dst, const tjs_uint8 *src) {
                if (mNeedRBSwap) {
                    TVPRedBlueSwapCopy((tjs_uint32*)dst, (const tjs_uint32*)src, w);
                } else {
                    ::memcpy(dst, src, row_bytes);
                }
            };

            if (mBottomUp) {
                // texture memory: 上端から下端の順に [表示下端行, …, 表示上端行] と並べる
                tjs_uint8 *dest = dest_top;
                const tjs_uint8 *line_src = visual_top + src_pitch * (h - 1);
                for (int line = 0; line < h; ++line) {
                    copy_row(dest, line_src);
                    dest    += row_bytes;
                    line_src -= src_pitch;  // src_pitch<0 のとき +|pitch|, src_pitch>0 のとき -pitch
                }
            } else {
                // texture memory: 上端から下端の順に [表示上端行, …, 表示下端行]
                tjs_uint8 *dest = dest_top;
                const tjs_uint8 *line_src = visual_top;
                for (int line = 0; line < h; ++line) {
                    copy_row(dest, line_src);
                    dest    += row_bytes;
                    line_src += src_pitch;
                }
            }
            upload_src   = dest_top;
            upload_pitch = (int)row_bytes;
        }

        if (!SDL_UpdateTexture(texture, &rect, upload_src, upload_pitch)) {
            TVPLOG_ERROR("SDLTextureUpdateRect::Update SDL_UpdateTexture failed:{}", SDL_GetError());
        }

        mHasUpdate = true;
#ifdef KRKRZ_DRAW_STATS
        const auto _stats_dur = std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now() - _stats_t0).count();
        TVPRenderStatsAddTexUpdate((tjs_uint64)_stats_dur, (tjs_uint64)w * (tjs_uint64)h * 4);
#endif
    }

    // 旧実装では中間バッファから GPU へ flush していたが、Update が直接アップロード
    // するようになったので、ここはフレームカウンタ更新のみ。
    void RenderToTexture(SDL_Texture *texture) {
        (void)texture;
        mHasUpdate = false;
#ifdef KRKRZ_DRAW_STATS
        TVPRenderStatsBumpFrame();
        // TexRen は Update 側に統合された (常に ~0)。互換のためカウンタ自体は残す。
        TVPRenderStatsAddTexRender(0);
#endif
    }
};
