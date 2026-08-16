#ifndef __BLEND_FUNCTOR_NEON_H__
#define __BLEND_FUNCTOR_NEON_H__

#include "neonutil.h"

extern "C"
{
    extern unsigned char TVPOpacityOnOpacityTable[256 * 256];
    extern unsigned char TVPNegativeMulTable[256 * 256];
};

// 端数処理にC実装版を参照する用
#include "blend_functor_c.h"

// ベーシックなアルファブレンド処理のベース
struct neon_alpha_blend : public alpha_blend_func
{
    // C言語版をそのまま呼び出す
    inline tjs_uint32 operator()(tjs_uint32 d, tjs_uint32 s, tjs_uint32 a) const
    {
        return alpha_blend_func::operator()(d, s, a);
    }

    inline uint8x8x4_t operator()(uint8x8x4_t md, uint8x8x4_t ms, uint8x8_t ma) const
    {
        // C ref (alpha_blend_func) の per-channel 等価式:
        //   result = d + ((s - d) * a) >> 8      (算術右シフト)
        // 旧実装は (s*a + d*(255-a)) >> 8 という代替式で ±1 ズレが出ていた。
        // SSE2/AVX2 と同じく int16 符号付き演算で書き直す。alpha レーンは
        // C ref が 0 にするので vdup_n_u8(0) で揃える (HDA variant 側で保持)。
        int16x8_t a16 = vreinterpretq_s16_u16(vmovl_u8(ma));
        auto blend_ch = [&](uint8x8_t d, uint8x8_t s) -> uint8x8_t {
            int16x8_t d16   = vreinterpretq_s16_u16(vmovl_u8(d));
            int16x8_t s16   = vreinterpretq_s16_u16(vmovl_u8(s));
            int16x8_t diff  = vsubq_s16(s16, d16);        // -255..255 (s16 内)
            // diff * a16 は -65025..65025 で s16 範囲 (-32768..32767) を越えるので
            // s32 に widen して掛ける
            int32x4_t prod_lo = vmull_s16(vget_low_s16(diff),  vget_low_s16(a16));
            int32x4_t prod_hi = vmull_s16(vget_high_s16(diff), vget_high_s16(a16));
            int32x4_t sh_lo   = vshrq_n_s32(prod_lo, 8);  // 算術右シフト (floor)
            int32x4_t sh_hi   = vshrq_n_s32(prod_hi, 8);
            int16x8_t shifted = vcombine_s16(vmovn_s32(sh_lo), vmovn_s32(sh_hi));
            int16x8_t res     = vaddq_s16(d16, shifted);
            return vqmovun_s16(res);                       // sat u8 (常に 0..255 内)
        };
        B_VEC(md) = blend_ch(B_VEC(md), B_VEC(ms));
        G_VEC(md) = blend_ch(G_VEC(md), G_VEC(ms));
        R_VEC(md) = blend_ch(R_VEC(md), R_VEC(ms));
        A_VEC(md) = vdup_n_u8(0);
        return md;
    }
};

// ソースのアルファを使う
template<typename blend_func>
struct neon_variation : public blend_func
{
    inline tjs_uint32 operator()(tjs_uint32 d, tjs_uint32 s) const
    {
        tjs_uint32 a = (s >> 24);
        return blend_func::operator()(d, s, a);
    }

    inline uint8x8x4_t operator()(uint8x8x4_t md, uint8x8x4_t ms) const
    {
        return blend_func::operator()(md, ms, A_VEC(ms));
    }
};

// ソースのアルファとopacity値を使う
template<typename blend_func>
struct neon_variation_opa : public blend_func
{
    const tjs_int32 opa_;
    const uint8x8_t mopa;
    inline neon_variation_opa(tjs_int32 opa)
    : opa_(opa)
    , mopa(vdup_n_u8(opa & 0xff))
    {}

    inline tjs_uint32 operator()(tjs_uint32 d, tjs_uint32 s) const
    {
        // 旧実装の `(s*opa) >> 32` は s.G/B からの繰り上がりが bit32 に乗ると
        // (sa*opa)>>8 と±1ズレることがあった (Phase B で SSE2/AVX2 と同じ修正)。
        tjs_uint32 a = ((s >> 24) * opa_) >> 8;
        return blend_func::operator()(d, s, a);
    }

    inline uint8x8x4_t operator()(uint8x8x4_t md, uint8x8x4_t ms) const
    {
        uint16x8_t msa_mopa_16 = vmull_u8(A_VEC(ms), mopa);
        return blend_func::operator()(md, ms, vmovn_u16(vshrq_n_u16(msa_mopa_16, 8)));
    }
};

// ソースとデスティネーションのアルファを使う
struct neon_alpha_blend_d_functor : public neon_alpha_blend
{
    inline tjs_uint32 operator()(tjs_uint32 d, tjs_uint32 s) const
    {
        tjs_uint32 addr      = ((s >> 16) & 0xff00) + (d >> 24);
        tjs_uint32 destalpha = TVPNegativeMulTable[addr] << 24;
        tjs_uint32 sopa      = TVPOpacityOnOpacityTable[addr];
        return neon_alpha_blend::operator()(d, s, sopa) + destalpha;
    }

    inline uint8x8x4_t operator()(uint8x8x4_t md, uint8x8x4_t ms) const
    {
        // ※基本的には neon_const_alpha_blend_d_functor と同じ処理なので
        // 修正時は合わせて対応のこと

        // テーブルを利用したコードを組む
        // 遅そうだが TVPAlphaBlend を TVPAlphaBlend_d に置き換えてのテストでは
        // c版が15msのところこのneon版で9.5msだったので、マシはマシみたい

        // src/dst のαをメモリにストア
        tjs_uint8 md_a[8], ms_a[8];
        vst1_u8(md_a, A_VEC(md));
        vst1_u8(ms_a, A_VEC(ms));

        // メモリから複数レーンにロードする命令はないので一旦リニアメモリに
        // 配置後、ロード命令でαベクタに読み込んだ上でブレンド処理へ渡す
        tjs_uint8 tmp_sopa[8];
        tjs_uint8 tmp_dopa[8];
        for (int i = 0; i < 8; i++) {
            tmp_sopa[i] = TVPOpacityOnOpacityTable[ms_a[i] << 8 | md_a[i]];
            tmp_dopa[i] = TVPNegativeMulTable[ms_a[i] << 8 | md_a[i]];
        }
        uint8x8x4_t ret = neon_alpha_blend::operator()(md, ms, vld1_u8(tmp_sopa));
        A_VEC(ret)      = vld1_u8(tmp_dopa);

        return ret;
    }
};

template<typename blend_func>
struct neon_variation_hda : public blend_func
{
    inline neon_variation_hda() {}
    inline neon_variation_hda(tjs_int32 opa)
    : blend_func(opa)
    {}

    inline tjs_uint32 operator()(tjs_uint32 d, tjs_uint32 s) const
    {
        tjs_uint32 dstalpha = d & 0xff000000;
        tjs_uint32 ret      = blend_func::operator()(d, s);
        return (ret & 0x00ffffff) | dstalpha;
    }

    inline uint8x8x4_t operator()(uint8x8x4_t md, uint8x8x4_t ms) const
    {
        uint8x8_t dstalpha = A_VEC(md);
        uint8x8x4_t ret    = blend_func::operator()(md, ms);
        A_VEC(ret)         = dstalpha;
        return ret;
    }
};

// もっともシンプルなコピー dst = src
struct neon_const_copy_functor
{
    inline tjs_uint32 operator()(tjs_uint32 d, tjs_uint32 s) const { return s; }
    inline uint8x8x4_t operator()(uint8x8x4_t md1, uint8x8x4_t ms1) const { return ms1; }
};

// 単純コピーだけど alpha をコピーしない(HDAと同じ)
struct neon_color_copy_functor
{
    inline tjs_uint32 operator()(tjs_uint32 d, tjs_uint32 s) const
    {
        return (d & 0xff000000) + (s & 0x00ffffff);
    }

    inline uint8x8x4_t operator()(uint8x8x4_t md1, uint8x8x4_t ms1) const
    {
        A_VEC(ms1) = A_VEC(md1);
        return ms1;
    }
};

// alphaだけコピーする : color_copy の src destを反転しただけ
struct neon_alpha_copy_functor : public neon_color_copy_functor
{
    inline tjs_uint32 operator()(tjs_uint32 d, tjs_uint32 s) const
    {
        return neon_color_copy_functor::operator()(s, d);
    }

    inline uint8x8x4_t operator()(uint8x8x4_t md1, uint8x8x4_t ms1) const
    {
        return neon_color_copy_functor::operator()(ms1, md1);
    }
};

// このままコピーするがアルファを0xffで埋める dst = 0xff000000 | src
struct neon_color_opaque_functor
{
    inline tjs_uint32 operator()(tjs_uint32 d, tjs_uint32 s) const
    {
        return 0xff000000 | s;
    }

    inline uint8x8x4_t operator()(uint8x8x4_t md1, uint8x8x4_t ms1) const
    {
        A_VEC(ms1) = vdup_n_u8(0xff);
        return ms1;
    }
};

// AVX2 版由来だが、元々未使用っぽい。一応ダミーのガワをおいておく
struct neon_alpha_blend_a_functor
{
    inline tjs_uint32 operator()(tjs_uint32 d, tjs_uint32 s) const { return s; }
    inline uint8x8x4_t operator()(uint8x8x4_t md, uint8x8x4_t ms) const { return ms; }
};

typedef neon_variation<neon_alpha_blend> neon_alpha_blend_functor;
typedef neon_variation_opa<neon_alpha_blend> neon_alpha_blend_o_functor;
typedef neon_variation_hda<neon_variation<neon_alpha_blend>> neon_alpha_blend_hda_functor;
typedef neon_variation_hda<neon_variation_opa<neon_alpha_blend>>
    neon_alpha_blend_hda_o_functor;
// neon_alpha_blend_d_functor
// neon_alpha_blend_a_functor

// scalar 用 helper: byte saturate add
static inline tjs_uint32 neon_scalar_sat_add_byte(tjs_uint32 a, tjs_uint32 b)
{
    return a + b > 255 ? 255 : a + b;
}

// TVPAdditiveAlphaBlend (非 HDA / src not pre-multiplied):
//   sa_inv     = 255 - src.alpha
//   result_rgb = sat_add( (d_rgb * sa_inv) >> 8, s_rgb )
//   result_a   = src.alpha
// Phase B / AVX2 と同じく `d - (d*sa>>8)` 減算ベースではなく `(d*(255-sa))>>8`
// 掛け算ベースにして C リファレンスと一致させる。
struct neon_premul_alpha_blend_functor
{
    inline tjs_uint32 operator()(tjs_uint32 d, tjs_uint32 s) const
    {
        tjs_uint32 sa  = s >> 24;
        tjs_uint32 sai = 255 - sa;
        tjs_uint32 dB = d & 0xff, dG = (d >> 8) & 0xff, dR = (d >> 16) & 0xff;
        tjs_uint32 sB = s & 0xff, sG = (s >> 8) & 0xff, sR = (s >> 16) & 0xff;
        tjs_uint32 rB = neon_scalar_sat_add_byte((dB * sai) >> 8, sB);
        tjs_uint32 rG = neon_scalar_sat_add_byte((dG * sai) >> 8, sG);
        tjs_uint32 rR = neon_scalar_sat_add_byte((dR * sai) >> 8, sR);
        return (sa << 24) | (rR << 16) | (rG << 8) | rB;
    }
    inline uint8x8x4_t operator()(uint8x8x4_t md, uint8x8x4_t ms) const
    {
        uint8x8_t sa     = A_VEC(ms);
        uint8x8_t sa_inv = vmvn_u8(sa); // 255 - sa
        // (d * (255-sa)) >> 8 per channel, sat add s
        B_VEC(md) = vqadd_u8(vshrn_n_u16(vmull_u8(B_VEC(md), sa_inv), 8), B_VEC(ms));
        G_VEC(md) = vqadd_u8(vshrn_n_u16(vmull_u8(G_VEC(md), sa_inv), 8), G_VEC(ms));
        R_VEC(md) = vqadd_u8(vshrn_n_u16(vmull_u8(R_VEC(md), sa_inv), 8), R_VEC(ms));
        A_VEC(md) = sa; // 非 HDA バリアント: result alpha = src alpha
        return md;
    }
};

//--------------------------------------------------------------------
// di = di - di*a*opa + si*opa
//              ~~~~~Df ~~~~~~ Sf
//           ~~~~~~~~Ds
//      ~~~~~~~~~~~~~Dq
// additive alpha blend with opacity
// TVPAdditiveAlphaBlend_o (with opacity, 非 HDA):
//   sa_pmul    = (src.alpha * opa) >> 8
//   sa_pmul_inv = 255 - sa_pmul
//   s_pmul     = (s * opa) >> 8 (全チャネル)
//   result_rgb = sat_add( (d * sa_pmul_inv) >> 8, s_pmul.rgb )
//   result_a   = sa_pmul
struct neon_premul_alpha_blend_o_functor
{
    const tjs_int32 opa_scalar_;
    const uint8x8_t opa_vec_;
    inline neon_premul_alpha_blend_o_functor(tjs_int32 opa)
    : opa_scalar_(opa)
    , opa_vec_(vdup_n_u8((uint8_t)opa))
    {}

    inline tjs_uint32 operator()(tjs_uint32 d, tjs_uint32 s) const
    {
        tjs_uint32 sa      = s >> 24;
        tjs_uint32 sa_pmul = (sa * (tjs_uint32)opa_scalar_) >> 8;
        tjs_uint32 sai     = 255 - sa_pmul;
        tjs_uint32 dB = d & 0xff, dG = (d >> 8) & 0xff, dR = (d >> 16) & 0xff;
        tjs_uint32 sB = s & 0xff, sG = (s >> 8) & 0xff, sR = (s >> 16) & 0xff;
        tjs_uint32 spB = (sB * (tjs_uint32)opa_scalar_) >> 8;
        tjs_uint32 spG = (sG * (tjs_uint32)opa_scalar_) >> 8;
        tjs_uint32 spR = (sR * (tjs_uint32)opa_scalar_) >> 8;
        tjs_uint32 rB  = neon_scalar_sat_add_byte((dB * sai) >> 8, spB);
        tjs_uint32 rG  = neon_scalar_sat_add_byte((dG * sai) >> 8, spG);
        tjs_uint32 rR  = neon_scalar_sat_add_byte((dR * sai) >> 8, spR);
        return (sa_pmul << 24) | (rR << 16) | (rG << 8) | rB;
    }

    inline uint8x8x4_t operator()(uint8x8x4_t md, uint8x8x4_t ms) const
    {
        // s_pmul = (s * opa) >> 8 全チャネル
        uint8x8_t sB = vshrn_n_u16(vmull_u8(B_VEC(ms), opa_vec_), 8);
        uint8x8_t sG = vshrn_n_u16(vmull_u8(G_VEC(ms), opa_vec_), 8);
        uint8x8_t sR = vshrn_n_u16(vmull_u8(R_VEC(ms), opa_vec_), 8);
        uint8x8_t sA = vshrn_n_u16(vmull_u8(A_VEC(ms), opa_vec_), 8);

        uint8x8_t sa_inv = vmvn_u8(sA); // 255 - sa_pmul

        B_VEC(md) = vqadd_u8(vshrn_n_u16(vmull_u8(B_VEC(md), sa_inv), 8), sB);
        G_VEC(md) = vqadd_u8(vshrn_n_u16(vmull_u8(G_VEC(md), sa_inv), 8), sG);
        R_VEC(md) = vqadd_u8(vshrn_n_u16(vmull_u8(R_VEC(md), sa_inv), 8), sR);
        A_VEC(md) = sA; // result alpha = sa_pmul
        return md;
    }
};

// TVPAdditiveAlphaBlend_HDA (HDA = preserve dst alpha):
//   sa_inv     = 255 - src.alpha
//   result_rgb = sat_add( (d_rgb * sa_inv) >> 8, s_rgb )
//   result_a   = dst.alpha (unchanged)
struct neon_premul_alpha_blend_hda_functor
{
    inline tjs_uint32 operator()(tjs_uint32 d, tjs_uint32 s) const
    {
        tjs_uint32 sa  = s >> 24;
        tjs_uint32 sai = 255 - sa;
        tjs_uint32 dB = d & 0xff, dG = (d >> 8) & 0xff, dR = (d >> 16) & 0xff;
        tjs_uint32 sB = s & 0xff, sG = (s >> 8) & 0xff, sR = (s >> 16) & 0xff;
        tjs_uint32 rB = neon_scalar_sat_add_byte((dB * sai) >> 8, sB);
        tjs_uint32 rG = neon_scalar_sat_add_byte((dG * sai) >> 8, sG);
        tjs_uint32 rR = neon_scalar_sat_add_byte((dR * sai) >> 8, sR);
        return (d & 0xff000000) | (rR << 16) | (rG << 8) | rB;
    }
    inline uint8x8x4_t operator()(uint8x8x4_t md, uint8x8x4_t ms) const
    {
        uint8x8_t sa_inv = vmvn_u8(A_VEC(ms)); // 255 - sa
        uint8x8_t da_save = A_VEC(md);
        B_VEC(md) = vqadd_u8(vshrn_n_u16(vmull_u8(B_VEC(md), sa_inv), 8), B_VEC(ms));
        G_VEC(md) = vqadd_u8(vshrn_n_u16(vmull_u8(G_VEC(md), sa_inv), 8), G_VEC(ms));
        R_VEC(md) = vqadd_u8(vshrn_n_u16(vmull_u8(R_VEC(md), sa_inv), 8), R_VEC(ms));
        A_VEC(md) = da_save;
        return md;
    }
};

// TVPAdditiveAlphaBlend_a (premul-on-premul):
//   sa_inv     = 255 - src.alpha
//   da_new     = sat255( da + sa - (da*sa>>8) )
//   result_rgb = sat_add( (d_rgb * sa_inv) >> 8, s_rgb )
//   result_a   = da_new
struct neon_premul_alpha_blend_a_functor
{
    inline tjs_uint32 operator()(tjs_uint32 d, tjs_uint32 s) const
    {
        tjs_uint32 da    = d >> 24;
        tjs_uint32 sa    = s >> 24;
        tjs_uint32 new_a = da + sa - ((da * sa) >> 8);
        new_a -= (new_a >> 8); // 256 → 255 飽和補正
        tjs_uint32 sai = 255 - sa;
        tjs_uint32 dB = d & 0xff, dG = (d >> 8) & 0xff, dR = (d >> 16) & 0xff;
        tjs_uint32 sB = s & 0xff, sG = (s >> 8) & 0xff, sR = (s >> 16) & 0xff;
        tjs_uint32 rB = neon_scalar_sat_add_byte((dB * sai) >> 8, sB);
        tjs_uint32 rG = neon_scalar_sat_add_byte((dG * sai) >> 8, sG);
        tjs_uint32 rR = neon_scalar_sat_add_byte((dR * sai) >> 8, sR);
        return (new_a << 24) | (rR << 16) | (rG << 8) | rB;
    }
    inline uint8x8x4_t operator()(uint8x8x4_t md, uint8x8x4_t ms) const
    {
        uint8x8_t sa     = A_VEC(ms);
        uint8x8_t da     = A_VEC(md);
        uint8x8_t sa_inv = vmvn_u8(sa);
        // da_new = da + sa - (da*sa>>8) ; これは uint8 の sat 加算で表現可能?
        // sat: da + sa - x where x = (da*sa)>>8 ≤ min(da, sa) → 結果は ≤ 255。
        // ここでは vshrn_n_u16(vmull_u8(da, sa), 8) = (da*sa)>>8 を計算してから
        // (da + sa) - x を 16bit で計算 → 8bit にナロー saturate。
        uint16x8_t prod = vmull_u8(da, sa);
        uint8x8_t  pm   = vshrn_n_u16(prod, 8); // (da*sa) >> 8
        // 16bit に揃えてから減算 (uint16 で大きさは収まる)
        uint16x8_t da16 = vmovl_u8(da);
        uint16x8_t sa16 = vmovl_u8(sa);
        uint16x8_t pm16 = vmovl_u8(pm);
        uint16x8_t sum  = vsubq_u16(vaddq_u16(da16, sa16), pm16);
        uint8x8_t  new_a = vqmovn_u16(sum); // saturate 255

        B_VEC(md) = vqadd_u8(vshrn_n_u16(vmull_u8(B_VEC(md), sa_inv), 8), B_VEC(ms));
        G_VEC(md) = vqadd_u8(vshrn_n_u16(vmull_u8(G_VEC(md), sa_inv), 8), G_VEC(ms));
        R_VEC(md) = vqadd_u8(vshrn_n_u16(vmull_u8(R_VEC(md), sa_inv), 8), R_VEC(ms));
        A_VEC(md) = new_a;
        return md;
    }
};

// opacity値を使う
struct neon_const_alpha_blend_functor : public neon_alpha_blend
{
    const tjs_int32 opa_;
    const uint8x8_t ma;
    inline neon_const_alpha_blend_functor(tjs_int32 opa)
    : opa_(opa)
    , ma(vdup_n_u8(opa & 0xff))
    {}

    inline tjs_uint32 operator()(tjs_uint32 d, tjs_uint32 s) const
    {
        return neon_alpha_blend::operator()(d, s, opa_);
    }

    inline uint8x8x4_t operator()(uint8x8x4_t md, uint8x8x4_t ms) const
    {
        return neon_alpha_blend::operator()(md, ms, ma);
    }
};
typedef neon_variation_hda<neon_const_alpha_blend_functor>
    neon_const_alpha_blend_hda_functor;

struct neon_const_alpha_blend_d_functor : public neon_alpha_blend
{
    const tjs_uint32 opa_;
    inline neon_const_alpha_blend_d_functor(tjs_int32 opa)
    : opa_((opa & 0xff) << 8)
    {}

    inline tjs_uint32 operator()(tjs_uint32 d, tjs_uint32 s) const
    {
        tjs_uint32 addr      = opa_ + (d >> 24);
        tjs_uint32 destalpha = TVPNegativeMulTable[addr] << 24;
        tjs_uint32 sopa      = TVPOpacityOnOpacityTable[addr];
        return neon_alpha_blend::operator()(d, s, sopa) + destalpha;
    }

    inline uint8x8x4_t operator()(uint8x8x4_t md, uint8x8x4_t ms) const
    {
        // ※基本的には neon_alpha_blend_d_functor と同じ処理なので
        // 修正時は合わせて対応のこと

        // dst のαをメモリにストア
        tjs_uint8 md_a[8];
        vst1_u8(md_a, A_VEC(md));

        // メモリから複数レーンにロードする命令はないので一旦リニアメモリに
        // 配置後、ロード命令でαベクタに読み込んだ上でブレンド処理へ渡す
        tjs_uint8 tmp_sopa[8];
        tjs_uint8 tmp_dopa[8];
        for (int i = 0; i < 8; i++) {
            tmp_sopa[i] = TVPOpacityOnOpacityTable[opa_ | md_a[i]];
            tmp_dopa[i] = TVPNegativeMulTable[opa_ | md_a[i]];
        }
        uint8x8x4_t ret = neon_alpha_blend::operator()(md, ms, vld1_u8(tmp_sopa));
        A_VEC(ret)      = vld1_u8(tmp_dopa);

        return ret;
    }
};

struct neon_const_alpha_blend_a_functor
{
    const tjs_uint32 opa32_;
    const tjs_uint8 opa8_;
    const struct neon_premul_alpha_blend_a_functor blend_;
    inline neon_const_alpha_blend_a_functor(tjs_int32 opa)
    : opa32_(opa << 24)
    , opa8_(opa & 0xff)
    , blend_{}
    {}

    inline tjs_uint32 operator()(tjs_uint32 d, tjs_uint32 s) const
    {
        return blend_(d, (s & 0x00ffffff) | opa32_);
    }

    inline uint8x8x4_t operator()(uint8x8x4_t md1, uint8x8x4_t ms1) const
    {
        A_VEC(ms1) = vdup_n_u8(opa8_);
        return blend_(md1, ms1);
    }
};

// neon_const_alpha_blend_functor;
typedef neon_const_alpha_blend_functor neon_const_alpha_blend_sd_functor;

// ※以下AVX2コードのコメントそのまま
//
// tjs_uint32 neon_const_alpha_blend_functor::operator()( tjs_uint32 d, tjs_uint32 s )
// tjs_uint32 neon_const_alpha_blend_sd_functor::operator()( tjs_uint32 s1, tjs_uint32 s2
// ) と引数は異なるが、処理内容は同じ const_alpha_blend は、dest と src1
// を共有しているようなもの dest = dest * src const_alpha_blend_sd は、dest = src1 * src2

// neon_const_copy_functor = TVPCopy はない、memcpy になってる
// neon_color_copy_functor = TVPCopyColor / TVPLinTransColorCopy
// neon_alpha_copy_functor = TVPCopyMask
// neon_color_opaque_functor = TVPCopyOpaqueImage
// neon_const_alpha_blend_functor = TVPConstAlphaBlend
// neon_const_alpha_blend_hda_functor = TVPConstAlphaBlend_HDA
// neon_const_alpha_blend_d_functor = TVPConstAlphaBlend_a
// neon_const_alpha_blend_a_functor = TVPConstAlphaBlend_a

//--------------------------------------------------------------------
// ここから加算/減算/乗算/Lighten/Darken/Screen 系 (PsBlend ではない素朴なブレンド)
// SSE2 / AVX2 と同型のパターンで実装。SIMD parity test の harness で
// SSE2 と byte-exact 一致させる方針 (PsBlend tolerance とは別カテゴリ)。
//--------------------------------------------------------------------

// TVPAddBlend: dst = sat( dst + src )  全チャネル
struct neon_add_blend_functor
{
    inline tjs_uint32 operator()(tjs_uint32 d, tjs_uint32 s) const
    {
        tjs_uint32 b = neon_scalar_sat_add_byte(d & 0xff, s & 0xff);
        tjs_uint32 g = neon_scalar_sat_add_byte((d >> 8) & 0xff, (s >> 8) & 0xff);
        tjs_uint32 r = neon_scalar_sat_add_byte((d >> 16) & 0xff, (s >> 16) & 0xff);
        tjs_uint32 a = neon_scalar_sat_add_byte((d >> 24) & 0xff, (s >> 24) & 0xff);
        return (a << 24) | (r << 16) | (g << 8) | b;
    }
    inline uint8x8x4_t operator()(uint8x8x4_t md, uint8x8x4_t ms) const
    {
        B_VEC(md) = vqadd_u8(B_VEC(md), B_VEC(ms));
        G_VEC(md) = vqadd_u8(G_VEC(md), G_VEC(ms));
        R_VEC(md) = vqadd_u8(R_VEC(md), R_VEC(ms));
        A_VEC(md) = vqadd_u8(A_VEC(md), A_VEC(ms));
        return md;
    }
};
struct neon_add_blend_hda_functor : public neon_add_blend_functor
{
    inline tjs_uint32 operator()(tjs_uint32 d, tjs_uint32 s) const
    {
        return (d & 0xff000000) | (neon_add_blend_functor::operator()(d, s) & 0x00ffffff);
    }
    inline uint8x8x4_t operator()(uint8x8x4_t md, uint8x8x4_t ms) const
    {
        uint8x8_t da = A_VEC(md);
        md            = neon_add_blend_functor::operator()(md, ms);
        A_VEC(md)     = da;
        return md;
    }
};
// _o variant: src を opa で減衰させてから add (sat にはならない、足し込んでから clamp)
struct neon_add_blend_o_functor
{
    const tjs_int32 opa_scalar_;
    const uint8x8_t opa_vec_;
    inline neon_add_blend_o_functor(tjs_int32 opa)
    : opa_scalar_(opa)
    , opa_vec_(vdup_n_u8((uint8_t)opa))
    {}
    inline tjs_uint32 operator()(tjs_uint32 d, tjs_uint32 s) const
    {
        // C ref (add_blend_func) は s を opa でスケールする際に alpha バイトを
        // マスクで除外してから全チャネル sat add するので、alpha は dest 側が
        // そのまま残る (sat(da + 0) = da)
        tjs_uint32 sB = ((s & 0xff) * (tjs_uint32)opa_scalar_) >> 8;
        tjs_uint32 sG = (((s >> 8) & 0xff) * (tjs_uint32)opa_scalar_) >> 8;
        tjs_uint32 sR = (((s >> 16) & 0xff) * (tjs_uint32)opa_scalar_) >> 8;
        tjs_uint32 b  = neon_scalar_sat_add_byte(d & 0xff, sB);
        tjs_uint32 g  = neon_scalar_sat_add_byte((d >> 8) & 0xff, sG);
        tjs_uint32 r  = neon_scalar_sat_add_byte((d >> 16) & 0xff, sR);
        return (d & 0xff000000u) | (r << 16) | (g << 8) | b;
    }
    inline uint8x8x4_t operator()(uint8x8x4_t md, uint8x8x4_t ms) const
    {
        B_VEC(md) = vqadd_u8(B_VEC(md), vshrn_n_u16(vmull_u8(B_VEC(ms), opa_vec_), 8));
        G_VEC(md) = vqadd_u8(G_VEC(md), vshrn_n_u16(vmull_u8(G_VEC(ms), opa_vec_), 8));
        R_VEC(md) = vqadd_u8(R_VEC(md), vshrn_n_u16(vmull_u8(R_VEC(ms), opa_vec_), 8));
        // A_VEC(md) はそのまま残す (dest alpha 保持)
        return md;
    }
};
typedef neon_add_blend_o_functor neon_add_blend_hda_o_functor; // SSE2 と同じく alias

// TVPSubBlend: dst = sat( dst - (255 - src) ) 全チャネル
struct neon_sub_blend_functor
{
    inline tjs_uint32 operator()(tjs_uint32 d, tjs_uint32 s) const
    {
        tjs_uint32 ns = ~s;
        auto sub     = [](tjs_uint32 a, tjs_uint32 b) -> tjs_uint32 {
            return a > b ? a - b : 0;
        };
        tjs_uint32 b = sub(d & 0xff, ns & 0xff);
        tjs_uint32 g = sub((d >> 8) & 0xff, (ns >> 8) & 0xff);
        tjs_uint32 r = sub((d >> 16) & 0xff, (ns >> 16) & 0xff);
        tjs_uint32 a = sub((d >> 24) & 0xff, (ns >> 24) & 0xff);
        return (a << 24) | (r << 16) | (g << 8) | b;
    }
    inline uint8x8x4_t operator()(uint8x8x4_t md, uint8x8x4_t ms) const
    {
        B_VEC(md) = vqsub_u8(B_VEC(md), vmvn_u8(B_VEC(ms)));
        G_VEC(md) = vqsub_u8(G_VEC(md), vmvn_u8(G_VEC(ms)));
        R_VEC(md) = vqsub_u8(R_VEC(md), vmvn_u8(R_VEC(ms)));
        A_VEC(md) = vqsub_u8(A_VEC(md), vmvn_u8(A_VEC(ms)));
        return md;
    }
};
struct neon_sub_blend_hda_functor : public neon_sub_blend_functor
{
    inline tjs_uint32 operator()(tjs_uint32 d, tjs_uint32 s) const
    {
        return (d & 0xff000000) | (neon_sub_blend_functor::operator()(d, s) & 0x00ffffff);
    }
    inline uint8x8x4_t operator()(uint8x8x4_t md, uint8x8x4_t ms) const
    {
        uint8x8_t da = A_VEC(md);
        md            = neon_sub_blend_functor::operator()(md, ms);
        A_VEC(md)     = da;
        return md;
    }
};
struct neon_sub_blend_o_functor
{
    const tjs_int32 opa_scalar_;
    const uint8x8_t opa_vec_;
    inline neon_sub_blend_o_functor(tjs_int32 opa)
    : opa_scalar_(opa)
    , opa_vec_(vdup_n_u8((uint8_t)opa))
    {}
    inline tjs_uint32 operator()(tjs_uint32 d, tjs_uint32 s) const
    {
        // ns = ~s, then ns * opa >> 8, sat sub from d
        tjs_uint32 ns = ~s;
        auto submul   = [&](tjs_uint32 dch, tjs_uint32 nsch) -> tjs_uint32 {
            tjs_uint32 sub = (nsch * (tjs_uint32)opa_scalar_) >> 8;
            return dch > sub ? dch - sub : 0;
        };
        tjs_uint32 b = submul(d & 0xff, ns & 0xff);
        tjs_uint32 g = submul((d >> 8) & 0xff, (ns >> 8) & 0xff);
        tjs_uint32 r = submul((d >> 16) & 0xff, (ns >> 16) & 0xff);
        tjs_uint32 a = submul((d >> 24) & 0xff, (ns >> 24) & 0xff);
        return (a << 24) | (r << 16) | (g << 8) | b;
    }
    inline uint8x8x4_t operator()(uint8x8x4_t md, uint8x8x4_t ms) const
    {
        uint8x8_t nsB = vmvn_u8(B_VEC(ms));
        uint8x8_t nsG = vmvn_u8(G_VEC(ms));
        uint8x8_t nsR = vmvn_u8(R_VEC(ms));
        uint8x8_t nsA = vmvn_u8(A_VEC(ms));
        B_VEC(md) = vqsub_u8(B_VEC(md), vshrn_n_u16(vmull_u8(nsB, opa_vec_), 8));
        G_VEC(md) = vqsub_u8(G_VEC(md), vshrn_n_u16(vmull_u8(nsG, opa_vec_), 8));
        R_VEC(md) = vqsub_u8(R_VEC(md), vshrn_n_u16(vmull_u8(nsR, opa_vec_), 8));
        A_VEC(md) = vqsub_u8(A_VEC(md), vshrn_n_u16(vmull_u8(nsA, opa_vec_), 8));
        return md;
    }
};
typedef neon_sub_blend_o_functor neon_sub_blend_hda_o_functor;

// TVPMulBlend: dst = (dst * src) >> 8 (全チャネル、結果 alpha は C ref と同じく 0)
struct neon_mul_blend_functor
{
    inline tjs_uint32 operator()(tjs_uint32 d, tjs_uint32 s) const
    {
        tjs_uint32 b = ((d & 0xff) * (s & 0xff)) >> 8;
        tjs_uint32 g = (((d >> 8) & 0xff) * ((s >> 8) & 0xff)) >> 8;
        tjs_uint32 r = (((d >> 16) & 0xff) * ((s >> 16) & 0xff)) >> 8;
        return (r << 16) | (g << 8) | b;
    }
    inline uint8x8x4_t operator()(uint8x8x4_t md, uint8x8x4_t ms) const
    {
        B_VEC(md) = vshrn_n_u16(vmull_u8(B_VEC(md), B_VEC(ms)), 8);
        G_VEC(md) = vshrn_n_u16(vmull_u8(G_VEC(md), G_VEC(ms)), 8);
        R_VEC(md) = vshrn_n_u16(vmull_u8(R_VEC(md), R_VEC(ms)), 8);
        A_VEC(md) = vdup_n_u8(0);
        return md;
    }
};
struct neon_mul_blend_hda_functor : public neon_mul_blend_functor
{
    inline tjs_uint32 operator()(tjs_uint32 d, tjs_uint32 s) const
    {
        return (d & 0xff000000) | neon_mul_blend_functor::operator()(d, s);
    }
    inline uint8x8x4_t operator()(uint8x8x4_t md, uint8x8x4_t ms) const
    {
        uint8x8_t da = A_VEC(md);
        md            = neon_mul_blend_functor::operator()(md, ms);
        A_VEC(md)     = da;
        return md;
    }
};

// TVPMulBlend_o: per byte, s'_b = 255 - (((255 - src_b) * opa) >> 8);
//                         dst_b = (dst_b * s'_b) >> 8; 結果 alpha = 0
// C ref (TVPMulBlend_o_c) と SSE2 (sse2_mul_blend_o_functor) の両方と byte-exact 一致。
// MIN3_VARIATION 経由で _o のみ wired (HDA_o は SSE2/C ref 共に非対応)。
struct neon_mul_blend_o_functor
{
    const tjs_int32 opa_scalar_;
    const uint8x8_t opa_vec_;
    inline neon_mul_blend_o_functor(tjs_int32 opa)
    : opa_scalar_(opa)
    , opa_vec_(vdup_n_u8((uint8_t)opa))
    {}
    inline tjs_uint32 operator()(tjs_uint32 d, tjs_uint32 s) const
    {
        auto sp = [&](tjs_uint32 sb) -> tjs_uint32 {
            return 255u - (((255u - sb) * (tjs_uint32)opa_scalar_) >> 8);
        };
        tjs_uint32 sB = sp(s & 0xff);
        tjs_uint32 sG = sp((s >> 8) & 0xff);
        tjs_uint32 sR = sp((s >> 16) & 0xff);
        tjs_uint32 b  = ((d & 0xff) * sB) >> 8;
        tjs_uint32 g  = (((d >> 8) & 0xff) * sG) >> 8;
        tjs_uint32 r  = (((d >> 16) & 0xff) * sR) >> 8;
        return (r << 16) | (g << 8) | b;
    }
    inline uint8x8x4_t operator()(uint8x8x4_t md, uint8x8x4_t ms) const
    {
        auto chan = [&](uint8x8_t d_b, uint8x8_t s_b) -> uint8x8_t {
            uint8x8_t  ns   = vmvn_u8(s_b);                        // 255 - s
            uint8x8_t  nso8 = vshrn_n_u16(vmull_u8(ns, opa_vec_), 8); // ((255-s)*opa) >> 8
            uint8x8_t  spv  = vmvn_u8(nso8);                       // 255 - ((255-s)*opa>>8) = s'
            return vshrn_n_u16(vmull_u8(d_b, spv), 8);             // (d * s') >> 8
        };
        B_VEC(md) = chan(B_VEC(md), B_VEC(ms));
        G_VEC(md) = chan(G_VEC(md), G_VEC(ms));
        R_VEC(md) = chan(R_VEC(md), R_VEC(ms));
        A_VEC(md) = vdup_n_u8(0);
        return md;
    }
};

// TVPLightenBlend: max(d, s) per channel (C ref は 32bit 一括 max なので alpha も max)
struct neon_lighten_blend_functor
{
    inline tjs_uint32 operator()(tjs_uint32 d, tjs_uint32 s) const
    {
        auto mx = [](tjs_uint32 a, tjs_uint32 b) { return a > b ? a : b; };
        tjs_uint32 b = mx(d & 0xff, s & 0xff);
        tjs_uint32 g = mx((d >> 8) & 0xff, (s >> 8) & 0xff);
        tjs_uint32 r = mx((d >> 16) & 0xff, (s >> 16) & 0xff);
        tjs_uint32 a = mx((d >> 24) & 0xff, (s >> 24) & 0xff);
        return (a << 24) | (r << 16) | (g << 8) | b;
    }
    inline uint8x8x4_t operator()(uint8x8x4_t md, uint8x8x4_t ms) const
    {
        B_VEC(md) = vmax_u8(B_VEC(md), B_VEC(ms));
        G_VEC(md) = vmax_u8(G_VEC(md), G_VEC(ms));
        R_VEC(md) = vmax_u8(R_VEC(md), R_VEC(ms));
        A_VEC(md) = vmax_u8(A_VEC(md), A_VEC(ms));
        return md;
    }
};
struct neon_lighten_blend_hda_functor : public neon_lighten_blend_functor
{
    inline tjs_uint32 operator()(tjs_uint32 d, tjs_uint32 s) const
    {
        return (d & 0xff000000) | neon_lighten_blend_functor::operator()(d, s);
    }
    inline uint8x8x4_t operator()(uint8x8x4_t md, uint8x8x4_t ms) const
    {
        uint8x8_t da = A_VEC(md);
        md            = neon_lighten_blend_functor::operator()(md, ms);
        A_VEC(md)     = da;
        return md;
    }
};

// TVPDarkenBlend: min(d, s) per channel (C ref は 32bit 一括 min なので alpha も min)
struct neon_darken_blend_functor
{
    inline tjs_uint32 operator()(tjs_uint32 d, tjs_uint32 s) const
    {
        auto mn = [](tjs_uint32 a, tjs_uint32 b) { return a < b ? a : b; };
        tjs_uint32 b = mn(d & 0xff, s & 0xff);
        tjs_uint32 g = mn((d >> 8) & 0xff, (s >> 8) & 0xff);
        tjs_uint32 r = mn((d >> 16) & 0xff, (s >> 16) & 0xff);
        tjs_uint32 a = mn((d >> 24) & 0xff, (s >> 24) & 0xff);
        return (a << 24) | (r << 16) | (g << 8) | b;
    }
    inline uint8x8x4_t operator()(uint8x8x4_t md, uint8x8x4_t ms) const
    {
        B_VEC(md) = vmin_u8(B_VEC(md), B_VEC(ms));
        G_VEC(md) = vmin_u8(G_VEC(md), G_VEC(ms));
        R_VEC(md) = vmin_u8(R_VEC(md), R_VEC(ms));
        A_VEC(md) = vmin_u8(A_VEC(md), A_VEC(ms));
        return md;
    }
};
struct neon_darken_blend_hda_functor : public neon_darken_blend_functor
{
    inline tjs_uint32 operator()(tjs_uint32 d, tjs_uint32 s) const
    {
        return (d & 0xff000000) | neon_darken_blend_functor::operator()(d, s);
    }
    inline uint8x8x4_t operator()(uint8x8x4_t md, uint8x8x4_t ms) const
    {
        uint8x8_t da = A_VEC(md);
        md            = neon_darken_blend_functor::operator()(md, ms);
        A_VEC(md)     = da;
        return md;
    }
};

// TVPScreenBlend: C ref 形式で ~(((~d) * (~s)) >> 8) per channel
//   (`d + s - (d*s)>>8` と数学的には同じだが、整数 truncation のため byte-exact
//    には ~d,~s を掛け算する方式でないと ±1 ズレる)
//   alpha は C ref が ~tmp の上位バイトを常に 0xff にするので固定で 0xff。
struct neon_screen_blend_functor
{
    inline tjs_uint32 operator()(tjs_uint32 d, tjs_uint32 s) const
    {
        auto sc = [](tjs_uint32 a, tjs_uint32 b) -> tjs_uint32 {
            tjs_uint32 na = 255 - a;
            tjs_uint32 nb = 255 - b;
            return 255 - ((na * nb) >> 8);
        };
        tjs_uint32 b = sc(d & 0xff, s & 0xff);
        tjs_uint32 g = sc((d >> 8) & 0xff, (s >> 8) & 0xff);
        tjs_uint32 r = sc((d >> 16) & 0xff, (s >> 16) & 0xff);
        return 0xff000000u | (r << 16) | (g << 8) | b;
    }
    inline uint8x8x4_t operator()(uint8x8x4_t md, uint8x8x4_t ms) const
    {
        auto blend = [](uint8x8_t a, uint8x8_t b) -> uint8x8_t {
            uint8x8_t na = vmvn_u8(a);
            uint8x8_t nb = vmvn_u8(b);
            uint16x8_t prod = vmull_u8(na, nb);        // (~a) * (~b)
            uint8x8_t  pm   = vshrn_n_u16(prod, 8);     // >> 8
            return vmvn_u8(pm);                         // ~pm
        };
        B_VEC(md) = blend(B_VEC(md), B_VEC(ms));
        G_VEC(md) = blend(G_VEC(md), G_VEC(ms));
        R_VEC(md) = blend(R_VEC(md), R_VEC(ms));
        A_VEC(md) = vdup_n_u8(0xff);
        return md;
    }
};
struct neon_screen_blend_hda_functor : public neon_screen_blend_functor
{
    inline tjs_uint32 operator()(tjs_uint32 d, tjs_uint32 s) const
    {
        // 基底は alpha を 0xff 固定で返すため、そのまま OR すると dest alpha が
        // 常に 0xff 化してしまう。色 24bit だけ取り出して dest alpha を保持する。
        return (d & 0xff000000)
             | (neon_screen_blend_functor::operator()(d, s) & 0x00ffffff);
    }
    inline uint8x8x4_t operator()(uint8x8x4_t md, uint8x8x4_t ms) const
    {
        uint8x8_t da = A_VEC(md);
        md            = neon_screen_blend_functor::operator()(md, ms);
        A_VEC(md)     = da;
        return md;
    }
};

#endif // __BLEND_FUNCTOR_NEON_H__
