#pragma once

#include "TextureIntf.h"
#include "GLTexture.h"
#include "Application.h"
#include <chrono>
#include <vector>
#include <cstring>
#include "ThreadIntf.h"   // TVPRenderStatsAddTexUpdate / AddTexRender / BumpFrame

//---------------------------------------------------------------------------
// SDLOGLDrawDevice 用テクスチャ更新クラス
//---------------------------------------------------------------------------

/**
 * @brief SDLOGLDrawDevice 専用のテクスチャ更新 (更新矩形の集約版)。
 *
 * 旧実装は NotifyBitmapCompleted から渡される dirty rect ごとに即座に
 * GLTexture::UpdateTexture (PBO map → memcpy → glTexSubImage2D) を呼んでいた。
 * これは全面動画再生のように 1 フレームの更新領域が多数 (実測 ~169 個) の
 * 小矩形に断片化するケースで、per-rect の PBO map/unmap + glTexSubImage2D の
 * ドライバ同期コストが支配的になり (Update 壁時間 ~977ms/s, FPS ~8) ボトルネックだった。
 *
 * 本実装は 1 フレーム分の更新を「テクスチャ同サイズの永続ステージングバッファ」へ
 * dest 座標で集約し、RenderToTexture で集約矩形 (union) を 1 回だけアップロードする。
 *   - 各 Update: src 矩形 → staging[dest] へ 1 段 memcpy (BGRA 並びはここで揃える)
 *   - RenderToTexture: union 矩形を 1 回 PBO 経由でアップロード
 *
 * staging は「GPU テクスチャと同一内容を保持する唯一のアップロード元」なので、
 * union 矩形内に当該フレームで未更新の隙間が含まれても、そこは前フレームまでの
 * 正しい内容を保持しており再アップロードしても破綻しない (= 座標オフセット仮定に
 * 依存しないので安全)。
 *
 * 疎な更新 (union 面積 >> 実更新面積) で全面再アップロードに退化するのを避けるため、
 * 実更新面積が union 面積の一定割合に満たない場合は記録した個別矩形を staging から
 * per-rect でアップロードするフォールバックを持つ。
 */

class SDLOGLTextureUpdateRect {

private:
    struct Rect4 { int x, y, w, h; };

    int  mWidth;
    int  mHeight;
    bool mHasUpdate;

    // テクスチャと同サイズ・GPU と同じバイト順を保持する永続ステージングバッファ。
    std::vector<tjs_uint8> mStaging;

    // 1 フレーム分の集約矩形 (union)。[mUx0,mUx1) x [mUy0,mUy1)
    int  mUx0, mUy0, mUx1, mUy1;
    // 実際に更新された総面積 (union との比較で密/疎を判定)。
    tjs_int64 mCoveredArea;
    // 個別矩形の記録 (疎なときの per-rect フォールバック用)。容量は再利用する。
    std::vector<Rect4> mRects;

    void ResetUnion() {
        mUx0 = mUy0 = 0x7fffffff;
        mUx1 = mUy1 = -0x7fffffff;
        mCoveredArea = 0;
        mRects.clear();
    }
    bool HasUnion() const { return mUx1 > mUx0 && mUy1 > mUy0; }

    // staging の (sx,sy,sw,sh) を PBO 経由で 1 回アップロード。
    // (中間バッファを介さない UpdateTextureDirect も試したが、 本画面のように
    //  大きな矩形を 1 回で上げる使い方では実測差が無かったので従来どおり)
    void UploadFromStaging(tTJSNI_Texture *texture, int sx, int sy, int sw, int sh) {
        const int dst_pitch = mWidth * 4;
        const tjs_uint8 *base = mStaging.data();
        const auto _up_t0 = std::chrono::steady_clock::now();
        texture->UpdateTexture(sx, sy, sw, sh,
            [base, dst_pitch, sx, sy, sw, sh](char *dest, int pitch) {
                const tjs_uint8 *s = base + (size_t)sy * dst_pitch + (size_t)sx * 4;
                char *d = dest;
                const int row_bytes = sw * 4;
                for (int line = 0; line < sh; line++) {
                    ::memcpy(d, s, row_bytes);
                    d += pitch;
                    s += dst_pitch;
                }
            });
        // 転送コストは常時計測する (System.renderStats)。
        TVPRenderStatsAddTexUpload(
            (tjs_uint64)std::chrono::duration_cast<std::chrono::nanoseconds>(
                std::chrono::steady_clock::now() - _up_t0).count(),
            (tjs_uint64)sw * (tjs_uint64)sh * 4);
    }

public:
    SDLOGLTextureUpdateRect()
    : mWidth(0)
    , mHeight(0)
    , mHasUpdate(false)
    {
        ResetUnion();
    }

    ~SDLOGLTextureUpdateRect() {}

    bool Empty() const { return !mHasUpdate; }

    void Resize(int w, int h) {
        mWidth  = w;
        mHeight = h;
        mStaging.assign((size_t)w * (size_t)h * 4, 0);
        ResetUnion();
    }

    void Clear() {
        mHasUpdate = false;
        ResetUnion();
    }

    // [NOTE] geometory (x,y,w,h) requires clipping on (0,0,mWidth,mHeight)
    void Update(tTJSNI_Texture *texture, int x, int y, int w, int h,
                const tjs_uint8 *src_p, int src_pitch, int src_x, int src_y)
    {
        if (!texture) return;
        if (mStaging.empty() || w <= 0 || h <= 0) return;
#ifdef KRKRZ_DRAW_STATS
        const auto _stats_t0 = std::chrono::steady_clock::now();
#endif
        const tjs_uint8 *src = src_p + src_pitch * src_y + src_x * 4;
        const int row_bytes = w * 4;
        const int dst_pitch = mWidth * 4;
        const bool support_bgra = GLTexture::SupportBGRA();

        // src 矩形を staging の dest 座標へ集約 (1 段 memcpy、BGRA 並びを揃える)。
        tjs_uint8 *dst = mStaging.data() + (size_t)y * dst_pitch + (size_t)x * 4;
        const tjs_uint8 *s = src;
        tjs_uint8 *d = dst;
        if (support_bgra) {
            for (int line = 0; line < h; line++) {
                ::memcpy(d, s, row_bytes);
                d += dst_pitch;
                s += src_pitch;
            }
        } else {
            for (int line = 0; line < h; line++) {
                TVPRedBlueSwapCopy((tjs_uint32*)d, (const tjs_uint32*)s, w);
                d += dst_pitch;
                s += src_pitch;
            }
        }

        // union / 個別矩形を記録。
        if (x < mUx0) mUx0 = x;
        if (y < mUy0) mUy0 = y;
        if (x + w > mUx1) mUx1 = x + w;
        if (y + h > mUy1) mUy1 = y + h;
        mCoveredArea += (tjs_int64)w * (tjs_int64)h;
        mRects.push_back(Rect4{ x, y, w, h });

        mHasUpdate = true;
#ifdef KRKRZ_DRAW_STATS
        const auto _stats_dur = std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now() - _stats_t0).count();
        TVPRenderStatsAddTexUpdate((tjs_uint64)_stats_dur, (tjs_uint64)w * (tjs_uint64)h * 4);
#endif
    }

    // 集約した更新を GPU へ流す。union が密なら 1 回、疎なら個別矩形を流す。
    void RenderToTexture(tTJSNI_Texture *texture) {
#ifdef KRKRZ_DRAW_STATS
        const auto _stats_t0 = std::chrono::steady_clock::now();
#endif
        TVPRenderStatsBumpUploadFrame();
        if (texture && HasUnion()) {
            const int ux = mUx0, uy = mUy0;
            const int uw = mUx1 - mUx0, uh = mUy1 - mUy0;
            const tjs_int64 bboxArea = (tjs_int64)uw * (tjs_int64)uh;

            // 密 (union 面積の半分以上が実更新) または矩形 1 個なら union を 1 回。
            if (mRects.size() <= 1 || mCoveredArea * 2 >= bboxArea) {
                UploadFromStaging(texture, ux, uy, uw, uh);
            } else {
                // 疎: 全面再アップロードを避け、個別矩形を staging から流す。
                for (const Rect4 &r : mRects) {
                    UploadFromStaging(texture, r.x, r.y, r.w, r.h);
                }
            }
        }
        mHasUpdate = false;
        ResetUnion();
#ifdef KRKRZ_DRAW_STATS
        const auto _stats_dur = std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now() - _stats_t0).count();
        TVPRenderStatsBumpFrame();
        TVPRenderStatsAddTexRender((tjs_uint64)_stats_dur);
#endif
    }
};
