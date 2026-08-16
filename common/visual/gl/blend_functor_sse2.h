
#ifndef __BLEND_FUNCTOR_SSE2_H__
#define __BLEND_FUNCTOR_SSE2_H__


extern "C" {
extern unsigned char TVPOpacityOnOpacityTable[256*256];
extern unsigned char TVPNegativeMulTable[256*256];
};

// ソースのアルファを使う
// 非 HDA バリアントなので結果のアルファは 0 にする (C リファレンス互換)
template<typename blend_func>
struct sse2_variation : public blend_func {
	inline tjs_uint32 operator()( tjs_uint32 d, tjs_uint32 s ) const {
		tjs_uint32 a = (s>>24);
		return blend_func::operator()( d, s, a ) & 0x00ffffff;
	}
	inline __m128i operator()( __m128i d, __m128i s ) const {
		__m128i a = s;
		a = _mm_srli_epi32( a, 24 );
		return _mm_and_si128( blend_func::operator()( d, s, a ),
		                      _mm_set1_epi32( 0x00ffffff ) );
	}
};

// ソースのアルファとopacity値を使う
// 非 HDA バリアントなので結果のアルファは 0 にする (C リファレンス互換)
template<typename blend_func>
struct sse2_variation_opa : public blend_func {
	const tjs_int32 opa_;
	const __m128i opa128_;
	const __m128i colormask_;
	inline sse2_variation_opa( tjs_int32 opa )
		: opa_(opa), opa128_(_mm_set1_epi32(opa)),
		  colormask_(_mm_set1_epi32(0x00ffffff)) {}
	inline tjs_uint32 operator()( tjs_uint32 d, tjs_uint32 s ) const {
		// 旧実装の `(s*opa) >> 32` は s.G/B からの繰り上がりが bit32 に乗ると
		// (sa*opa)>>8 と±1ズレる場合があった (C リファレンスと不一致)。
		tjs_uint32 a = ((s >> 24) * opa_) >> 8;
		return blend_func::operator()( d, s, a ) & 0x00ffffff;
	}
	inline __m128i operator()( __m128i d, __m128i s ) const {
		__m128i a = s;
		a = _mm_srli_epi32( a, 24 );
		a = _mm_mullo_epi16( a, opa128_ );
		a = _mm_srli_epi32( a, 8 );
		return _mm_and_si128( blend_func::operator()( d, s, a ), colormask_ );
	}
};

// ソースとデスティネーションのアルファを使う
//template<typename blend_func>
struct sse2_alpha_blend_d_functor {
	const __m128i colormask_;
	const __m128i zero_;
	inline sse2_alpha_blend_d_functor() : colormask_(_mm_set1_epi32(0x00ffffff)), zero_(_mm_setzero_si128()){}
	inline tjs_uint32 operator()( tjs_uint32 d, tjs_uint32 s ) const {
		tjs_uint32 addr = ((s >> 16) & 0xff00) + (d>>24);
		tjs_uint32 sopa = TVPOpacityOnOpacityTable[addr];

		__m128i ma = _mm_cvtsi32_si128( sopa );
		ma = _mm_shufflelo_epi16( ma, _MM_SHUFFLE( 0, 0, 0, 0 )  );	// 00oo00oo00oo00oo
		__m128i ms = _mm_cvtsi32_si128( s );
		ms = _mm_unpacklo_epi8( ms, zero_ );// 00 ss 00 ss 00 ss 00 ss
		__m128i md = _mm_cvtsi32_si128( d );
		md = _mm_unpacklo_epi8( md, zero_ );// 00 dd 00 dd 00 dd 00 dd

		ms = _mm_sub_epi16( ms, md );		// ms -= md
		ms = _mm_mullo_epi16( ms, ma );		// ms *= ma
		md = _mm_slli_epi16( md, 8 );		// md <<= 8
		md = _mm_add_epi16( md, ms );		// md += ms
		md = _mm_srli_epi16( md, 8 );		// ms >>= 8
		md = _mm_packus_epi16( md, md );
		tjs_uint32 ret = _mm_cvtsi128_si32( md );

#if 0
		// TVPNegativeMulTable[addr] = (unsigned char)( 255 - (255-a)*(255-b)/ 255 );
		addr ^= 0xffff; // (a = 255-a, b = 255-b) : ^=xor
		tjs_uint32 tmp = addr;
		addr = (addr&0xff) * (tmp>>8);	// (255-a)*(255-b)
		addr = ~addr;	// result = 255-result : ビット反転
		addr = (addr&0xff00) << 16;	// /255 : 最下位ではなく、一つ上を選択することで/255と同等
#endif
		addr = TVPNegativeMulTable[addr] << 24;
		return (ret&0x00ffffff) | addr;
	}
	inline __m128i operator()( __m128i md1, __m128i ms1 ) const {
		//if( _mm_testc_si128( s, alphamask_ ) ) { return s; }
		//if( _mm_testz_si128( s, alphamask_ ) ) { return d; }
		__m128i sa = ms1;
		sa  = _mm_srli_epi32( sa, 24 );
		__m128i da = md1;
		da = _mm_srli_epi32( da, 24 );
		__m128i maddr = _mm_add_epi32( _mm_slli_epi32(sa, 8), da );
		__m128i dopa = maddr;
#if 0
		tjs_uint32 addr;
		addr = _mm_cvtsi128_si32( maddr );
		tjs_uint32 opa = TVPOpacityOnOpacityTable[addr];
		__m128i mopa = _mm_cvtsi32_si128( opa );

		maddr = _mm_shuffle_epi32( maddr, _MM_SHUFFLE( 0, 3, 2, 1 ) );
		addr = _mm_cvtsi128_si32( maddr );
		opa = TVPOpacityOnOpacityTable[addr];
		__m128i mopa2 = _mm_cvtsi32_si128( opa );
		mopa = _mm_unpacklo_epi32( mopa, mopa2 );

		maddr = _mm_shuffle_epi32( maddr, _MM_SHUFFLE( 0, 3, 2, 1 ) );
		addr = _mm_cvtsi128_si32( maddr );
		opa = TVPOpacityOnOpacityTable[addr];
		mopa2 = _mm_cvtsi32_si128( opa );

		maddr = _mm_shuffle_epi32( maddr, _MM_SHUFFLE( 0, 3, 2, 1 ) );
		addr = _mm_cvtsi128_si32( maddr );
		opa = TVPOpacityOnOpacityTable[addr];
		__m128i mopa3 = _mm_cvtsi32_si128( opa );
		mopa2 = _mm_unpacklo_epi32( mopa2, mopa3 );

		mopa = _mm_unpacklo_epi64( mopa, mopa2 );
#else	// 以下のようにコンパイラ任せの方がいいかも
		__m128i ma1 = _mm_set_epi32(
			TVPOpacityOnOpacityTable[M128I_U32(maddr,3)],
			TVPOpacityOnOpacityTable[M128I_U32(maddr,2)],
			TVPOpacityOnOpacityTable[M128I_U32(maddr,1)],
			TVPOpacityOnOpacityTable[M128I_U32(maddr,0)]);
#endif
#if 0
		tjs_uint32 sopa = M128I_U32(ma1,0);
		tjs_uint32 d = M128I_U32(md1,0);
		tjs_uint32 s = M128I_U32(ms1,0);
		tjs_uint32 d1 = d & 0xff00ff;
		d1 = (d1 + (((s & 0xff00ff) - d1) * sopa >> 8)) & 0xff00ff;
		d &= 0xff00;
		s &= 0xff00;
		tjs_uint32 dest = d1 + ((d + ((s - d) * sopa >> 8)) & 0xff00);
#endif
		__m128i ms2 = ms1;
		__m128i md2 = md1;
		ms1 = _mm_unpacklo_epi8( ms1, zero_ );
		md1 = _mm_unpacklo_epi8( md1, zero_ );
		ma1 = _mm_packs_epi32( ma1, ma1 );		// 0 1 2 3 0 1 2 3
		ma1 = _mm_unpacklo_epi16( ma1, ma1 );	// 0 0 1 1 2 2 3 3
		__m128i ma2 = ma1;
		ma1 = _mm_unpacklo_epi16( ma1, ma1 );	 // 0 0 0 0 1 1 1 1
		ms1 = _mm_sub_epi16( ms1, md1 );	// s -= d
		ms1 = _mm_mullo_epi16( ms1, ma1 );	// s *= a
		md1 = _mm_slli_epi16( md1, 8 );		// d <<= 8
		md1 = _mm_add_epi16( md1, ms1 );	// d += s
		md1 = _mm_srli_epi16( md1, 8 );		// d >>= 8

		ms2 = _mm_unpackhi_epi8( ms2, zero_ );
		md2 = _mm_unpackhi_epi8( md2, zero_ );
		ma2 = _mm_unpackhi_epi16( ma2, ma2 );	// 2 2 2 2 3 3 3 3
		ms2 = _mm_sub_epi16( ms2, md2 );		// s -= d
		ms2 = _mm_mullo_epi16( ms2, ma2 );		// s *= a
		md2 = _mm_slli_epi16( md2, 8 );			// d <<= 8
		md2 = _mm_add_epi16( md2, ms2 );		// d += s
		md2 = _mm_srli_epi16( md2, 8 );			// d >>= 8
		md1 = _mm_packus_epi16( md1, md2 );

		// 旧実装は ((255-a)*(255-b))>>8 で TVPNegativeMulTable を近似していたため
		// /255 と /256 の差で ±1 ズレることがあった。C リファレンスと一致させるため
		// 直接テーブルを引く。
		__m128i mneg = _mm_set_epi32(
			TVPNegativeMulTable[M128I_U32(dopa,3)],
			TVPNegativeMulTable[M128I_U32(dopa,2)],
			TVPNegativeMulTable[M128I_U32(dopa,1)],
			TVPNegativeMulTable[M128I_U32(dopa,0)]);
		mneg = _mm_slli_epi32( mneg, 24 );

		md1 = _mm_and_si128( md1, colormask_ );
		return _mm_or_si128( md1, mneg );
	}
};

template<typename blend_func>
struct sse2_variation_hda : public blend_func {
	__m128i alphamask_;
	__m128i colormask_;
	inline sse2_variation_hda() {
		alphamask_ = _mm_set1_epi32( 0xFF000000 );
		colormask_ = _mm_set1_epi32( 0x00FFFFFF );
	}
	inline sse2_variation_hda( tjs_int32 opa ) : blend_func(opa) {
		alphamask_ = _mm_set1_epi32( 0xFF000000 );
		colormask_ = _mm_set1_epi32( 0x00FFFFFF );
	}
	inline tjs_uint32 operator()( tjs_uint32 d, tjs_uint32 s ) const {
		tjs_uint32 dstalpha = d&0xff000000;
		tjs_uint32 ret = blend_func::operator()( d, s );
		return (ret&0x00ffffff)|dstalpha;
	}
	inline __m128i operator()( __m128i d, __m128i s ) const {
		__m128i dstalpha = _mm_and_si128( d, alphamask_ );
		__m128i ret = blend_func::operator()( d, s );
		ret = _mm_and_si128( ret, colormask_ );
		return _mm_or_si128( ret, dstalpha );
	}
};

struct sse2_alpha_blend_func {
	const __m128i zero_;
	inline sse2_alpha_blend_func() : zero_( _mm_setzero_si128() ) {}
	inline tjs_uint32 one( __m128i md, __m128i ms, __m128i ma ) const {
		ms = _mm_unpacklo_epi8( ms, zero_ );// unpack 00 ss 00 ss 00 ss 00 ss
		ma = _mm_unpacklo_epi16( ma, ma );	// unpack 00 00 00 00 00 aa 00 aa
		md = _mm_unpacklo_epi8( md, zero_ );// unpack 00 dd 00 dd 00 dd 00 dd
		ma = _mm_unpacklo_epi32( ma, ma );	// unpack 00 aa 00 aa 00 aa 00 aa
		ms = _mm_sub_epi16( ms, md );		// ms -= md
		ms = _mm_mullo_epi16( ms, ma );		// ms *= ma
		ms = _mm_srli_epi16( ms, 8 );		// ms >>= 8
		md = _mm_add_epi8( md, ms );			// md += ms : d + ((s-d)*sopa)>>8
		md = _mm_packus_epi16( md, zero_ );	// pack
		return _mm_cvtsi128_si32( md );		// store
	}
	inline __m128i operator()( __m128i md1, __m128i ms1, __m128i ma1 ) const {
		__m128i ms2 = ms1;
		__m128i md2 = md1;

		ms1 = _mm_unpacklo_epi8( ms1, zero_ );
		md1 = _mm_unpacklo_epi8( md1, zero_ );
		ma1 = _mm_packs_epi32( ma1, ma1 );		// 0 1 2 3 0 1 2 3
		ma1 = _mm_unpacklo_epi16( ma1, ma1 );	// 0 0 1 1 2 2 3 3
		__m128i ma2 = ma1;
		ma1 = _mm_unpacklo_epi16( ma1, ma1 );	 // 0 0 0 0 1 1 1 1
		ms1 = _mm_sub_epi16( ms1, md1 );		// s -= d
		ms1 = _mm_mullo_epi16( ms1, ma1 );	// s *= a
		ms1 = _mm_srli_epi16( ms1, 8 );		// s >>= 8
		md1 = _mm_add_epi8( md1, ms1 );		// d += s

		ms2 = _mm_unpackhi_epi8( ms2, zero_ );
		md2 = _mm_unpackhi_epi8( md2, zero_ );
		ma2 = _mm_unpackhi_epi16( ma2, ma2 );	// 2 2 2 2 3 3 3 3
		ms2 = _mm_sub_epi16( ms2, md2 );		// s -= d
		ms2 = _mm_mullo_epi16( ms2, ma2 );		// s *= a
		ms2 = _mm_srli_epi16( ms2, 8 );			// s >>= 8
		md2 = _mm_add_epi8( md2, ms2 );		// d += s
		return _mm_packus_epi16( md1, md2 );
	}
};

struct sse2_alpha_blend : public sse2_alpha_blend_func {
	inline tjs_uint32 operator()( tjs_uint32 d, tjs_uint32 s, tjs_uint32 a ) const {
		__m128i ma = _mm_cvtsi32_si128( a );
		__m128i ms = _mm_cvtsi32_si128( s );
		__m128i md = _mm_cvtsi32_si128( d );
		return sse2_alpha_blend_func::one( md, ms, ma );
	}
	inline __m128i operator()( __m128i md1, __m128i ms1, __m128i ma1 ) const {
		return sse2_alpha_blend_func::operator()( md1, ms1, ma1 );
	}
};


// 非 HDA アルファブレンド (TVPAlphaBlend 用)
// C リファレンス互換のため結果のアルファは 0 にする
struct sse2_variation_straight : public sse2_alpha_blend {
	const __m128i colormask_;
	const __m128i alphamask_;
	inline sse2_variation_straight() : colormask_(_mm_set1_epi32(0x00ffffff)), alphamask_(_mm_set1_epi32(0xff000000)) {}
	inline tjs_uint32 operator()( tjs_uint32 d, tjs_uint32 s ) const {
		if( s >= 0xff000000 ) {
			// 完全不透明: src をそのままコピーするが、alpha は 0 に潰す
			return s & 0x00ffffff;
		}
		tjs_uint32 a = (s>>24);
		return sse2_alpha_blend::operator()( d, s, a ) & 0x00ffffff;
	}
	inline __m128i operator()( __m128i d, __m128i s ) const {
		// SSE4.1
		/*
		if( _mm_testc_si128( s, alphamask_ ) ) {
			return s;	// totally opaque
		}
		if( _mm_testz_si128( s, alphamask_ ) ) {
			return d;	// totally transparent
		}
		*/
		__m128i a = s;
		a = _mm_srli_epi32( a, 24 );
		return _mm_and_si128( sse2_alpha_blend::operator()( d, s, a ), colormask_ );
	}
};

// もっともシンプルなコピー dst = src
struct sse2_const_copy_functor {
	inline tjs_uint32 operator()( tjs_uint32 d, tjs_uint32 s ) const { return s; }
	inline __m128i operator()( __m128i md1, __m128i ms1 ) const { return ms1; }
};
// 単純コピーだけど alpha をコピーしない(HDAと同じ)
struct sse2_color_copy_functor {
	const __m128i colormask_;
	const __m128i alphamask_;
	inline sse2_color_copy_functor() : colormask_(_mm_set1_epi32(0x00ffffff)), alphamask_(_mm_set1_epi32(0xff000000)) {}
	inline tjs_uint32 operator()( tjs_uint32 d, tjs_uint32 s ) const {
		return (d&0xff000000) + (s&0x00ffffff);
	}
	inline __m128i operator()( __m128i md1, __m128i ms1 ) const {
		ms1 = _mm_and_si128( ms1, colormask_ );
		md1 = _mm_and_si128( md1, alphamask_ );
		return _mm_or_si128( md1, ms1 );
	}
};
// alphaだけコピーする : color_copy の src destを反転しただけ
struct sse2_alpha_copy_functor : public sse2_color_copy_functor {
	inline tjs_uint32 operator()( tjs_uint32 d, tjs_uint32 s ) const {
		return sse2_color_copy_functor::operator()( s, d );
	}
	inline __m128i operator()( __m128i md1, __m128i ms1 ) const {
		return sse2_color_copy_functor::operator()( ms1, md1 );
	}
};
// このままコピーするがアルファを0xffで埋める dst = 0xff000000 | src
struct sse2_color_opaque_functor {
	const __m128i alphamask_;
	inline sse2_color_opaque_functor() : alphamask_(_mm_set1_epi32(0xff000000)) {}
	inline tjs_uint32 operator()( tjs_uint32 d, tjs_uint32 s ) const { return 0xff000000 | s; }
	inline __m128i operator()( __m128i md1, __m128i ms1 ) const { return _mm_or_si128( alphamask_, ms1 ); }
};
// 矩形版未実装
struct sse2_alpha_blend_a_functor {
	const __m128i mask_;
	const __m128i zero_;
	inline sse2_alpha_blend_a_functor() : zero_(_mm_setzero_si128()), mask_(_mm_set_epi32(0x0000ffff,0xffffffff,0x0000ffff,0xffffffff)) {}
	inline tjs_uint32 operator()( tjs_uint32 d, tjs_uint32 s ) const {
		__m128i ms = _mm_cvtsi32_si128( s );
		__m128i mo = ms;
		mo = _mm_srli_epi32( mo, 24 );			// >> 24
		__m128i msa = mo;
		msa = _mm_slli_epi64( msa, 48 );		// << 48
		mo = _mm_shufflelo_epi16( mo, _MM_SHUFFLE( 0, 0, 0, 0 )  );	// 00oo00oo00oo00oo
		ms = _mm_unpacklo_epi8( ms, zero_ );	// 00Sa00Si00Si00Si
		ms = _mm_mullo_epi16( ms, mo );			// src * sopa
		ms = _mm_srli_epi16( ms, 8 );			// src * sopa >> 8
		ms = _mm_and_si128( ms, mask_ );		// drop alpha
		ms = _mm_or_si128( ms, msa );			// set original alpha

		__m128i md = _mm_cvtsi32_si128( d );
		md = _mm_unpacklo_epi8( md, zero_ );	// 00Da00Di00Di00Di
		__m128i md2 = md;
		md2 = _mm_mullo_epi16( md2, mo );	// d * sopa
		md2 = _mm_srli_epi16( md2, 8 );		// 00 SaDa 00 SaDi 00 SaDi 00 SaDi
		md = _mm_sub_epi16( md, md2 );		// d - d*sopa
		md = _mm_add_epi16( md, ms );		// (d-d*sopa) + s
		md = _mm_packus_epi16( md, zero_ );
		return _mm_cvtsi128_si32( md );
	}
	inline __m128i operator()( __m128i md, __m128i ms ) const {
		__m128i mo0 = ms;
		mo0 = _mm_srli_epi32( mo0, 24 );
		__m128i msa = mo0;
		msa = _mm_slli_epi32( msa, 24 );		// << 24
		mo0 = _mm_packs_epi32( mo0, mo0 );		// 0 1 2 3 0 1 2 3
		mo0 = _mm_unpacklo_epi16( mo0, mo0 );	// 0 0 1 1 2 2 3 3
		__m128i mo1 = mo0;
		mo1 = _mm_unpacklo_epi16( mo1, mo1 );	// 0 0 0 0 1 1 1 1 o[1]
		mo0 = _mm_unpackhi_epi16( mo0, mo0 );	// 2 2 2 2 3 3 3 3 o[0]
		
		__m128i ms1 = ms;
		ms = _mm_unpackhi_epi8( ms, zero_ );
		ms = _mm_mullo_epi16( ms, mo0 );		// src * sopa
		ms = _mm_srli_epi16( ms, 8 );			// src * sopa >> 8
		ms = _mm_and_si128( ms, mask_ );		// drop alpha
		__m128i msa1 = msa;
		msa = _mm_unpackhi_epi8( msa, zero_ );
		ms = _mm_or_si128( ms, msa );			// set original alpha
		
		ms1 = _mm_unpacklo_epi8( ms1, zero_ );
		ms1 = _mm_srli_epi16( ms1, 8 );			// src * sopa >> 8
		ms1 = _mm_and_si128( ms1, mask_ );		// drop alpha
		msa1 = _mm_unpacklo_epi8( msa1, zero_ );
		ms1 = _mm_or_si128( ms1, msa1 );		// set original alpha

		__m128i md1 = md;
		md = _mm_unpackhi_epi8( md, zero_ );// 00dd00dd00dd00dd d[0]
		__m128i md02 = md;
		md02 = _mm_mullo_epi16( md02, mo0 );	// d * sopa | d[0]
		md02 = _mm_srli_epi16( md02, 8 );	// 00 SaDa 00 SaDi 00 SaDi 00 SaDi | d[0]
		md = _mm_sub_epi16( md, md02 );		// d - d*sopa | d[0]
		md = _mm_add_epi16( md, ms );		// d - d*sopa + s | d[0]

		md1 = _mm_unpacklo_epi8( md1, zero_ );// 00dd00dd00dd00dd d[1]
		__m128i md12 = md1;
		md12 = _mm_mullo_epi16( md12, mo1 );// d * sopa | d[1]
		md12 = _mm_srli_epi16( md12, 8 );	// 00 SaDa 00 SaDi 00 SaDi 00 SaDi | d[1]
		md1 = _mm_sub_epi16( md1, md12 );	// d - d*sopa | d[1]
		md1 = _mm_add_epi16( md1, ms1 );	// d - d*sopa + s | d[1]

		return _mm_packus_epi16( md1, md );
	}
};

//typedef sse2_variation<sse2_alpha_blend>							sse2_alpha_blend_functor;
typedef sse2_variation_straight										sse2_alpha_blend_functor;
typedef sse2_variation_opa<sse2_alpha_blend>						sse2_alpha_blend_o_functor;
typedef sse2_variation_hda<sse2_variation<sse2_alpha_blend> >		sse2_alpha_blend_hda_functor;
typedef sse2_variation_hda<sse2_variation_opa<sse2_alpha_blend> >	sse2_alpha_blend_hda_o_functor;
//typedef sse2_variation_d<sse2_alpha_blend>							sse2_alpha_blend_d_functor;
// sse2_alpha_blend_d_functor
// sse2_alpha_blend_a_functor

// 非 HDA バリアント: C リファレンスは alpha レーンに src.alpha を出力する
// 計算式 (C ref): result_rgb = sat_add( (d * (255-sa)) >> 8, s )
// 旧 SSE2 は `d - (d*sa)>>8` を使っていたが整数 truncation 由来で
// 最大 ±2 ズレるので、C ref と同じ (d * (255-sa)) >> 8 に揃える。
struct sse2_premul_alpha_blend_functor {
	const __m128i zero_;
	const __m128i alphamask_;
	const __m128i colormask_;
	const __m128i m255_;
	inline sse2_premul_alpha_blend_functor()
		: zero_( _mm_setzero_si128() ),
		  alphamask_(_mm_set1_epi32(0xff000000)),
		  colormask_(_mm_set1_epi32(0x00ffffff)),
		  m255_(_mm_set1_epi16(255)) {}
	inline tjs_uint32 operator()( tjs_uint32 d, tjs_uint32 s ) const {
		__m128i ms = _mm_cvtsi32_si128( s );
		tjs_int32 sopa = 255 - (tjs_int32)(s >> 24);    // 255 - sa
		__m128i mo = _mm_cvtsi32_si128( sopa );
		__m128i md = _mm_cvtsi32_si128( d );
		mo = _mm_unpacklo_epi16( mo, mo );	// 0000000000oo00oo
		mo = _mm_unpacklo_epi32( mo, mo );	// 00oo00oo00oo00oo
		md = _mm_unpacklo_epi8( md, zero_ );// 00dd00dd00dd00dd
		md = _mm_mullo_epi16( md, mo );		// d * (255-sa)
		md = _mm_srli_epi16( md, 8 );		// >> 8
		md = _mm_packus_epi16( md, zero_ );	// pack
		md = _mm_adds_epu8( md, ms );		// (d*(255-sa))>>8 + src
		return (_mm_cvtsi128_si32( md ) & 0x00ffffff) | (s & 0xff000000);
	}
	inline __m128i operator()( __m128i d, __m128i s ) const {
		// alpha スナップショット (結果 alpha = src.alpha)
		__m128i s_orig = s;

		__m128i ma1 = s;
		ma1 = _mm_srli_epi32( ma1, 24 );		// s >> 24 = sa
		ma1 = _mm_packs_epi32( ma1, ma1 );		// 0 1 2 3 0 1 2 3
		ma1 = _mm_unpacklo_epi16( ma1, ma1 );	// 0 0 1 1 2 2 3 3
		__m128i ma2 = ma1;
		ma1 = _mm_unpacklo_epi16( ma1, ma1 );	// 0 0 0 0 1 1 1 1
		ma2 = _mm_unpackhi_epi16( ma2, ma2 );	// 2 2 2 2 3 3 3 3
		// sopa = 255 - sa (16bit ごと)
		ma1 = _mm_sub_epi16( m255_, ma1 );
		ma2 = _mm_sub_epi16( m255_, ma2 );

		__m128i md2 = d;
		d = _mm_unpacklo_epi8( d, zero_ );
		d = _mm_mullo_epi16( d, ma1 );	// d * (255-sa)
		d = _mm_srli_epi16( d, 8 );		// >> 8

		md2 = _mm_unpackhi_epi8( md2, zero_ );
		md2 = _mm_mullo_epi16( md2, ma2 );	// d * (255-sa)
		md2 = _mm_srli_epi16( md2, 8 );		// >> 8

		__m128i packed = _mm_packus_epi16( d, md2 );
		packed = _mm_adds_epu8( packed, s );	// (d*(255-sa))>>8 + src
		return _mm_or_si128( _mm_and_si128( packed, colormask_ ),
		                     _mm_and_si128( s_orig, alphamask_ ) );
	}
};
//--------------------------------------------------------------------
// di = di - di*a*opa + si*opa
//              ~~~~~Df ~~~~~~ Sf
//           ~~~~~~~~Ds
//      ~~~~~~~~~~~~~Dq
// additive alpha blend with opacity
// C リファレンス (premulalpha_blend_o_functor):
//   s_pmul     = s * opa >> 8           (全チャネル)
//   sa_pmul    = s_pmul.alpha
//   result_rgb = sat_add( (d * (255 - sa_pmul)) >> 8, s_pmul.rgb )
//   result_a   = sa_pmul
// 旧 SSE2 は `d - (d*sa_pmul>>8) + s_pmul` の減算ベースだったため
// `d * (256 - sa_pmul)/256` 相当となり C ref と最大 ±1 ズレた。
// 整数 truncation を C ref に揃えるため (255 - sa_pmul) の掛け算ベースに変更。
struct sse2_premul_alpha_blend_o_functor {
	const __m128i zero_;
	const __m128i opa_;
	const tjs_int opa_scalar_;
	const __m128i colormask_;
	const __m128i m255_;
	inline sse2_premul_alpha_blend_o_functor( tjs_int opa )
		: zero_( _mm_setzero_si128() ), opa_(_mm_set1_epi16((short)opa)),
		  opa_scalar_(opa),
		  colormask_(_mm_set1_epi32(0x00ffffff)),
		  m255_(_mm_set1_epi32(0xff)) {}
	inline tjs_uint32 operator()( tjs_uint32 d, tjs_uint32 s ) const {
		tjs_uint32 sa = s >> 24;
		tjs_uint32 sa_pmul = (sa * (tjs_uint32)opa_scalar_) >> 8;
		tjs_int32  sopa_inv = 255 - (tjs_int32)sa_pmul;

		__m128i ms = _mm_cvtsi32_si128( s );
		__m128i md = _mm_cvtsi32_si128( d );
		__m128i mo_inv = _mm_cvtsi32_si128( sopa_inv );
		mo_inv = _mm_unpacklo_epi16( mo_inv, mo_inv );
		mo_inv = _mm_unpacklo_epi32( mo_inv, mo_inv );	// 4x16bit へ broadcast

		ms = _mm_unpacklo_epi8( ms, zero_ );
		ms = _mm_mullo_epi16( ms, opa_ );		// s * opa per channel
		ms = _mm_srli_epi16( ms, 8 );			// s_pmul (16bit lanes)

		md = _mm_unpacklo_epi8( md, zero_ );
		md = _mm_mullo_epi16( md, mo_inv );		// d * (255 - sa_pmul)
		md = _mm_srli_epi16( md, 8 );			// >> 8

		md = _mm_add_epi16( md, ms );			// + s_pmul
		md = _mm_packus_epi16( md, zero_ );		// saturate per byte

		return (_mm_cvtsi128_si32( md ) & 0x00ffffff) | (sa_pmul << 24);
	}
	inline __m128i operator()( __m128i d, __m128i s ) const {
		// sa_pmul = (sa * opa) >> 8 per 32-bit lane
		__m128i sa32 = _mm_srli_epi32( s, 24 );
		__m128i sa_pmul32 = _mm_mullo_epi16( sa32, opa_ );
		sa_pmul32 = _mm_srli_epi32( sa_pmul32, 8 );
		__m128i alpha_out = _mm_slli_epi32( sa_pmul32, 24 );

		// (255 - sa_pmul) per lane → broadcast to 4x16bit slots/pixel
		__m128i sopa_inv32 = _mm_sub_epi32( m255_, sa_pmul32 );
		__m128i ma  = _mm_packs_epi32( sopa_inv32, sopa_inv32 );	// [v0,v1,v2,v3,v0,v1,v2,v3]
		ma  = _mm_unpacklo_epi16( ma, ma );							// [v0,v0,v1,v1,v2,v2,v3,v3]
		__m128i ma2 = ma;
		ma  = _mm_unpacklo_epi16( ma,  ma  );						// pixel 0,1
		ma2 = _mm_unpackhi_epi16( ma2, ma2 );						// pixel 2,3

		// s_pmul = s * opa >> 8 per channel
		__m128i s_lo = _mm_unpacklo_epi8( s, zero_ );
		s_lo = _mm_mullo_epi16( s_lo, opa_ );
		s_lo = _mm_srli_epi16( s_lo, 8 );
		__m128i s_hi = _mm_unpackhi_epi8( s, zero_ );
		s_hi = _mm_mullo_epi16( s_hi, opa_ );
		s_hi = _mm_srli_epi16( s_hi, 8 );

		// d * (255 - sa_pmul) >> 8 per channel
		__m128i d_lo = _mm_unpacklo_epi8( d, zero_ );
		d_lo = _mm_mullo_epi16( d_lo, ma );
		d_lo = _mm_srli_epi16( d_lo, 8 );
		__m128i d_hi = _mm_unpackhi_epi8( d, zero_ );
		d_hi = _mm_mullo_epi16( d_hi, ma2 );
		d_hi = _mm_srli_epi16( d_hi, 8 );

		d_lo = _mm_add_epi16( d_lo, s_lo );
		d_hi = _mm_add_epi16( d_hi, s_hi );
		__m128i packed = _mm_packus_epi16( d_lo, d_hi );

		// alpha レーンを (sa*opa)>>8 で上書き (C ref 互換)
		return _mm_or_si128( _mm_and_si128( packed, colormask_ ), alpha_out );
	}
};
/*
	Di = sat(Si, (1-Sa)*Di)
	Da = Sa + Da - SaDa
*/
// additive alpha blend holding destination alpha
// 計算式 (C ref):
//   result_rgb   = sat_add( (d * (255-sa)) >> 8, s_rgb )
//   result_alpha = dst.alpha  (HDA = Hold Destination Alpha)
// 旧 SSE2 は `d - d*sa>>8` だったが整数 truncation でズレるため
// (d * (255-sa)) >> 8 に揃える。
struct sse2_premul_alpha_blend_hda_functor {
	const __m128i zero_;
	const __m128i unpack_colormask_;	// unpacked 16bit lanes 用 (alpha lane=0)
	const __m128i colormask_;			// packed 8bit 用 0x00ffffff
	const __m128i alphamask_packed_;	// packed 8bit 用 0xff000000
	const __m128i m255_;
	inline sse2_premul_alpha_blend_hda_functor() : zero_(_mm_setzero_si128()),
		unpack_colormask_(_mm_set_epi32(0x0000ffff,0xffffffff,0x0000ffff,0xffffffff)),
		colormask_(_mm_set1_epi32(0x00FFFFFF)),
		alphamask_packed_(_mm_set1_epi32(0xFF000000)),
		m255_(_mm_set1_epi16(255)) {}
	inline tjs_uint32 operator()( tjs_uint32 d, tjs_uint32 s ) const {
		__m128i ms = _mm_cvtsi32_si128( s );
		__m128i mo = _mm_cvtsi32_si128( 255 - (tjs_int32)(s >> 24) );	// 255 - sa
		__m128i md = _mm_cvtsi32_si128( d );
		mo = _mm_unpacklo_epi16( mo, mo );		// 0000000000oo00oo
		ms = _mm_and_si128( ms, colormask_ );	// drop alpha from src
		mo = _mm_unpacklo_epi16( mo, mo );		// 00oo00oo00oo00oo
		md = _mm_unpacklo_epi8( md, zero_ );	// 00dd00dd00dd00dd
		md = _mm_mullo_epi16( md, mo );		// d * (255-sa)
		md = _mm_srli_epi16( md, 8 );		// >> 8
		md = _mm_and_si128( md, unpack_colormask_ );	// alpha lane を 0 に
		md = _mm_packus_epi16( md, zero_ );
		md = _mm_adds_epu8( md, ms );		// (d*(255-sa))>>8 + s(rgb)
		// dst.alpha を OR して HDA 完成
		return (_mm_cvtsi128_si32( md ) & 0x00ffffff) | (d & 0xff000000);
	}
	inline __m128i operator()( __m128i md, __m128i s ) const {
		__m128i d_orig = md;
		__m128i mo0 = s;
		mo0 = _mm_srli_epi32( mo0, 24 );
		mo0 = _mm_packs_epi32( mo0, mo0 );		// 0 1 2 3 0 1 2 3
		mo0 = _mm_unpacklo_epi16( mo0, mo0 );	// 0 0 1 1 2 2 3 3
		__m128i mo1 = mo0;
		mo1 = _mm_unpacklo_epi16( mo1, mo1 );	// 0 0 0 0 1 1 1 1 o[1]
		mo0 = _mm_unpackhi_epi16( mo0, mo0 );	// 2 2 2 2 3 3 3 3 o[0]
		mo1 = _mm_sub_epi16( m255_, mo1 );		// 255 - sa
		mo0 = _mm_sub_epi16( m255_, mo0 );

		__m128i md0 = md;
		md = _mm_unpacklo_epi8( md, zero_ );	// 00dd00dd00dd00dd d[1]
		md = _mm_mullo_epi16( md, mo1 );		// d[1] * (255-sa)
		md = _mm_srli_epi16( md, 8 );			// >> 8
		md = _mm_and_si128( md, unpack_colormask_ );	// alpha lane を 0

		md0 = _mm_unpackhi_epi8( md0, zero_ );	// 00dd00dd00dd00dd d[0]
		md0 = _mm_mullo_epi16( md0, mo0 );		// d[0] * (255-sa)
		md0 = _mm_srli_epi16( md0, 8 );			// >> 8
		md0 = _mm_and_si128( md0, unpack_colormask_ );	// alpha lane を 0
		__m128i md02 = _mm_packus_epi16( md, md0 );

		s = _mm_and_si128( s, colormask_ );		// drop alpha from src
		md02 = _mm_adds_epu8( md02, s );		// (d*(255-sa))>>8 + s.rgb
		// dst.alpha を OR して HDA 完成
		return _mm_or_si128( _mm_and_si128( md02, colormask_ ),
		                     _mm_and_si128( d_orig, alphamask_packed_ ) );
	}
};
// additive alpha blend on additive alpha
// additive alpha blend on additive alpha
// C リファレンス (premulalpha_blend_a_a_func):
//   da_new = sat255( da + sa - (da*sa>>8) )
//   rgb    = sat_add( (d_rgb * (255 - sa)) >> 8, s_rgb )
// 旧 SSE2 は `d - d*sa>>8` 減算ベース (= d*(256-sa)/256) で C ref と最大 ±1 ズレた。
// alpha も `d.alpha - d.alpha*sa>>8 + s.alpha` だったため、C ref の
// `da + sa - (da*sa>>8) - 飽和補正` とは別計算で誤差が出ていた。
// RGB は (255-sa) の掛け算ベース、alpha は専用に再計算してマスク合成する。
struct sse2_premul_alpha_blend_a_functor {
	const __m128i zero_;
	const __m128i colormask_;
	const __m128i m255_;
	inline sse2_premul_alpha_blend_a_functor()
		: zero_(_mm_setzero_si128()),
		  colormask_(_mm_set1_epi32(0x00ffffff)),
		  m255_(_mm_set1_epi32(0xff)) {}
	inline tjs_uint32 operator()( tjs_uint32 d, tjs_uint32 s ) const {
		tjs_uint32 da = d >> 24;
		tjs_uint32 sa = s >> 24;
		tjs_uint32 new_a = da + sa - ((da * sa) >> 8);
		new_a -= (new_a >> 8);	// 256→255 飽和補正
		tjs_int32  sopa_inv = 255 - (tjs_int32)sa;

		__m128i ms = _mm_cvtsi32_si128( s );
		__m128i md = _mm_cvtsi32_si128( d );
		__m128i mo = _mm_cvtsi32_si128( sopa_inv );
		mo = _mm_unpacklo_epi16( mo, mo );
		mo = _mm_unpacklo_epi32( mo, mo );		// 4x16bit へ broadcast
		md = _mm_unpacklo_epi8( md, zero_ );
		md = _mm_mullo_epi16( md, mo );			// d * (255 - sa)
		md = _mm_srli_epi16( md, 8 );			// >> 8
		md = _mm_packus_epi16( md, zero_ );
		md = _mm_adds_epu8( md, ms );			// (d*(255-sa))>>8 + s (sat add)
		return (_mm_cvtsi128_si32( md ) & 0x00ffffff) | (new_a << 24);
	}
	inline __m128i operator()( __m128i d, __m128i s ) const {
		// alpha 計算: da + sa - (da*sa>>8) を 32bit lane 単位で
		__m128i sa32 = _mm_srli_epi32( s, 24 );
		__m128i da32 = _mm_srli_epi32( d, 24 );
		__m128i prod = _mm_mullo_epi16( sa32, da32 );	// sa*da は < 65536
		prod = _mm_srli_epi32( prod, 8 );				// (sa*da)>>8
		__m128i new_a = _mm_add_epi32( sa32, da32 );
		new_a = _mm_sub_epi32( new_a, prod );
		__m128i clamp = _mm_srli_epi32( new_a, 8 );		// 256 のとき 1
		new_a = _mm_sub_epi32( new_a, clamp );
		__m128i alpha_out = _mm_slli_epi32( new_a, 24 );

		// RGB: (255 - sa) per lane → broadcast to 4x16bit slots/pixel
		__m128i sopa_inv32 = _mm_sub_epi32( m255_, sa32 );
		__m128i ma  = _mm_packs_epi32( sopa_inv32, sopa_inv32 );
		ma  = _mm_unpacklo_epi16( ma, ma );
		__m128i ma2 = ma;
		ma  = _mm_unpacklo_epi16( ma,  ma  );		// pixel 0,1
		ma2 = _mm_unpackhi_epi16( ma2, ma2 );		// pixel 2,3

		__m128i d_lo = _mm_unpacklo_epi8( d, zero_ );
		d_lo = _mm_mullo_epi16( d_lo, ma );
		d_lo = _mm_srli_epi16( d_lo, 8 );
		__m128i d_hi = _mm_unpackhi_epi8( d, zero_ );
		d_hi = _mm_mullo_epi16( d_hi, ma2 );
		d_hi = _mm_srli_epi16( d_hi, 8 );

		__m128i packed = _mm_packus_epi16( d_lo, d_hi );
		packed = _mm_adds_epu8( packed, s );		// + s (sat add per byte)

		return _mm_or_si128( _mm_and_si128( packed, colormask_ ), alpha_out );
	}
};

// opacity値を使う
// 非 HDA バリアント: 結果のアルファは 0 (C リファレンス互換)
struct sse2_const_alpha_blend_functor {
	const __m128i opa_;
	const __m128i zero_;
	const __m128i colormask_;
	inline sse2_const_alpha_blend_functor( tjs_int32 opa )
		: zero_(_mm_setzero_si128()),
		  opa_(_mm_set1_epi16((short)opa)),
		  colormask_(_mm_set1_epi32(0x00ffffff)) {}
	inline tjs_uint32 operator()( tjs_uint32 d, tjs_uint32 s ) const {
		__m128i ms = _mm_cvtsi32_si128( s );
		__m128i md = _mm_cvtsi32_si128( d );
		ms = _mm_unpacklo_epi8( ms, zero_ );// unpack 00 ss 00 ss 00 ss 00 ss
		md = _mm_unpacklo_epi8( md, zero_ );// unpack 00 dd 00 dd 00 dd 00 dd
		ms = _mm_sub_epi16( ms, md );		// ms -= md
		ms = _mm_mullo_epi16( ms, opa_ );	// ms *= ma
		ms = _mm_srli_epi16( ms, 8 );		// ms >>= 8
		md = _mm_add_epi8( md, ms );		// md += ms : d + ((s-d)*sopa)>>8
		md = _mm_packus_epi16( md, zero_ );	// pack
		return _mm_cvtsi128_si32( md ) & 0x00ffffff;
	}
	inline __m128i operator()( __m128i md1, __m128i ms1 ) const {
		__m128i ms2 = ms1;
		__m128i md2 = md1;

		ms1 = _mm_unpacklo_epi8( ms1, zero_ );
		md1 = _mm_unpacklo_epi8( md1, zero_ );
		ms1 = _mm_sub_epi16( ms1, md1 );	// s -= d
		ms1 = _mm_mullo_epi16( ms1, opa_ );	// s *= a
		ms1 = _mm_srli_epi16( ms1, 8 );		// s >>= 8
		md1 = _mm_add_epi8( md1, ms1 );		// d += s

		ms2 = _mm_unpackhi_epi8( ms2, zero_ );
		md2 = _mm_unpackhi_epi8( md2, zero_ );
		ms2 = _mm_sub_epi16( ms2, md2 );		// s -= d
		ms2 = _mm_mullo_epi16( ms2, opa_ );		// s *= a
		ms2 = _mm_srli_epi16( ms2, 8 );			// s >>= 8
		md2 = _mm_add_epi8( md2, ms2 );		// d += s
		return _mm_and_si128( _mm_packus_epi16( md1, md2 ), colormask_ );
	}
	// 2pixel版でunpack済み(16bit単位)
	inline __m128i two( __m128i md1, __m128i ms1 ) const {
		ms1 = _mm_sub_epi16( ms1, md1 );	// s -= d
		ms1 = _mm_mullo_epi16( ms1, opa_ );	// s *= a
		ms1 = _mm_srli_epi16( ms1, 8 );		// s >>= 8
		return _mm_add_epi8( md1, ms1 );	// d += s
	}
};
typedef sse2_variation_hda<sse2_const_alpha_blend_functor>	sse2_const_alpha_blend_hda_functor;

struct sse2_const_alpha_blend_d_functor {
	const tjs_int32 opa32_;
	const __m128i colormask_;
	const __m128i zero_;
	const __m128i opa_;
	inline sse2_const_alpha_blend_d_functor( tjs_int32 opa )
		: colormask_(_mm_set1_epi32(0x00ffffff)), opa32_(opa<<8), zero_(_mm_setzero_si128()), opa_(_mm_set1_epi32(opa<<8)) {}
	inline tjs_uint32 operator()( tjs_uint32 d, tjs_uint32 s ) const {
		tjs_uint32 addr = opa32_ + (d>>24);
		tjs_uint32 sopa = TVPOpacityOnOpacityTable[addr];
		__m128i ma = _mm_cvtsi32_si128( sopa );
		ma = _mm_shufflelo_epi16( ma, _MM_SHUFFLE( 0, 0, 0, 0 )  );	// 0000000000000000 00oo00oo00oo00oo
		__m128i ms = _mm_cvtsi32_si128( s );
		__m128i md = _mm_cvtsi32_si128( d );
		ms = _mm_unpacklo_epi8( ms, zero_ );// 00 ss 00 ss 00 ss 00 ss
		md = _mm_unpacklo_epi8( md, zero_ );// 00 dd 00 dd 00 dd 00 dd
		ms = _mm_sub_epi16( ms, md );		// ms -= md
		ms = _mm_mullo_epi16( ms, ma );		// ms *= ma
		ms = _mm_srli_epi16( ms, 8 );		// ms >>= 8
		md = _mm_add_epi8( md, ms );		// md += ms : d + ((s-d)*sopa)>>8
		md = _mm_packus_epi16( md, zero_ );	// pack
		tjs_uint32 ret = _mm_cvtsi128_si32( md );		// store
		addr = TVPNegativeMulTable[addr] << 24;
		return (ret&0x00ffffff) | addr;
	}
	inline __m128i operator()( __m128i md1, __m128i ms1 ) const {
		__m128i da = md1;
		da = _mm_srli_epi32( da, 24 );
		__m128i maddr = _mm_add_epi32( opa_, da );
		__m128i dopa = maddr;
		__m128i ma1 = _mm_set_epi32(
			TVPOpacityOnOpacityTable[M128I_U32(maddr,3)],
			TVPOpacityOnOpacityTable[M128I_U32(maddr,2)],
			TVPOpacityOnOpacityTable[M128I_U32(maddr,1)],
			TVPOpacityOnOpacityTable[M128I_U32(maddr,0)]);
		
		ma1 = _mm_packs_epi32( ma1, ma1 );		// 0 1 2 3 0 1 2 3
		ma1 = _mm_unpacklo_epi16( ma1, ma1 );	// 0 0 1 1 2 2 3 3
		__m128i ma2 = ma1;
		ma1 = _mm_unpacklo_epi16( ma1, ma1 );	 // 0 0 0 0 1 1 1 1

		__m128i ms2 = ms1;
		__m128i md2 = md1;
		ms1 = _mm_unpacklo_epi8( ms1, zero_ );
		md1 = _mm_unpacklo_epi8( md1, zero_ );
		ms1 = _mm_sub_epi16( ms1, md1 );	// s -= d
		ms1 = _mm_mullo_epi16( ms1, ma1 );	// s *= a
		ms1 = _mm_srli_epi16( ms1, 8 );		// s >>= 8
		md1 = _mm_add_epi8( md1, ms1 );		// d += s
		
		ma2 = _mm_unpackhi_epi16( ma2, ma2 );	// 2 2 2 2 3 3 3 3
		ms2 = _mm_unpackhi_epi8( ms2, zero_ );
		md2 = _mm_unpackhi_epi8( md2, zero_ );
		ms2 = _mm_sub_epi16( ms2, md2 );		// s -= d
		ms2 = _mm_mullo_epi16( ms2, ma2 );		// s *= a
		ms2 = _mm_srli_epi16( ms2, 8 );			// s >>= 8
		md2 = _mm_add_epi8( md2, ms2 );			// d += s
		md1 = _mm_packus_epi16( md1, md2 );

		// 旧実装は ((255-a)*(255-b))>>8 で TVPNegativeMulTable を近似していたため
		// /255 と /256 の差で ±1 ズレることがあった。C リファレンスと一致させるため
		// 直接テーブルを引く。
		__m128i mneg = _mm_set_epi32(
			TVPNegativeMulTable[M128I_U32(dopa,3)],
			TVPNegativeMulTable[M128I_U32(dopa,2)],
			TVPNegativeMulTable[M128I_U32(dopa,1)],
			TVPNegativeMulTable[M128I_U32(dopa,0)]);
		mneg = _mm_slli_epi32( mneg, 24 );

		md1 = _mm_and_si128( md1, colormask_ );
		return _mm_or_si128( md1, mneg );
	}
};
struct sse2_const_alpha_blend_a_functor {
	const tjs_uint32 opa32_;
	const __m128i opa_;
	const __m128i colormask_;
	const struct sse2_premul_alpha_blend_a_functor blend_;
	inline sse2_const_alpha_blend_a_functor( tjs_int32 opa ) : colormask_(_mm_set1_epi32(0x00ffffff)), opa32_(opa<<24), opa_(_mm_set1_epi32(opa<<24)) {}
	inline tjs_uint32 operator()( tjs_uint32 d, tjs_uint32 s ) const {
		return blend_( d, (s&0x00ffffff)|opa32_ );
	}
	inline __m128i operator()( __m128i md1, __m128i ms1 ) const {
		ms1 = _mm_and_si128( ms1, colormask_ );
		ms1 = _mm_or_si128( ms1, opa_ );
		return blend_( md1, ms1 );
	}
};

//																	sse2_const_alpha_blend_functor;
typedef sse2_const_alpha_blend_functor								sse2_const_alpha_blend_sd_functor;
// tjs_uint32 sse2_const_alpha_blend_functor::operator()( tjs_uint32 d, tjs_uint32 s )
// tjs_uint32 sse2_const_alpha_blend_sd_functor::operator()( tjs_uint32 s1, tjs_uint32 s2 )
// と引数は異なるが、処理内容は同じ
// const_alpha_blend は、dest と src1 を共有しているようなもの dest = dest * src
// const_alpha_blend_sd は、dest = src1 * src2

// sse2_const_copy_functor = TVPCopy はない、memcpy になってる
// sse2_color_copy_functor = TVPCopyColor / TVPLinTransColorCopy
// sse2_alpha_copy_functor = TVPCopyMask
// sse2_color_opaque_functor = TVPCopyOpaqueImage
// sse2_const_alpha_blend_functor = TVPConstAlphaBlend
// sse2_const_alpha_blend_hda_functor = TVPConstAlphaBlend_HDA
// sse2_const_alpha_blend_d_functor = TVPConstAlphaBlend_a
// sse2_const_alpha_blend_a_functor = TVPConstAlphaBlend_a

//--------------------------------------------------------------------
// ここまでアルファブレンド
// ここから加算合成など、psでないブレンドはソースアルファが考慮されない
//--------------------------------------------------------------------
struct sse2_add_blend_functor {
	inline tjs_uint32 operator()( tjs_uint32 d, tjs_uint32 s ) const {
		__m128i md = _mm_cvtsi32_si128( d );
		__m128i ms = _mm_cvtsi32_si128( s );
		md = _mm_adds_epu8( md, ms );	// dest += src
		return _mm_cvtsi128_si32( md );
	}
	inline __m128i operator()( __m128i d, __m128i s ) const {
		return _mm_adds_epu8( d, s );	// dest += src
	}
};

struct sse2_add_blend_hda_functor {
	const __m128i mask_;
	inline sse2_add_blend_hda_functor() : mask_(_mm_set1_epi32(0x00ffffff)) {}
	inline tjs_uint32 operator()( tjs_uint32 d, tjs_uint32 s ) const {
		__m128i md = _mm_cvtsi32_si128( d );
		__m128i ms = _mm_cvtsi32_si128( s );
		ms = _mm_and_si128( ms, mask_ );// mask
		md = _mm_adds_epu8( md, ms );	// dest += src
		return _mm_cvtsi128_si32( md );
	}
	inline __m128i operator()( __m128i d, __m128i s ) const {
		s = _mm_and_si128( s, mask_ );	// 1 mask
		return _mm_adds_epu8( s, d );	// 1 src += dest
	}
};

struct sse2_add_blend_o_functor {
	const __m128i zero_;
	const __m128i opa_;
	inline sse2_add_blend_o_functor( tjs_int opa ) : zero_(_mm_setzero_si128())
		, opa_(_mm_srli_epi64(_mm_set1_epi16((short)opa),16)) {}
		// opa_ = 000000oo00oo00oo
	inline tjs_uint32 operator()( tjs_uint32 d, tjs_uint32 s ) const {
		__m128i md = _mm_cvtsi32_si128( d );
		md = _mm_unpacklo_epi8( md, zero_ );	// md = 00dd00dd00dd00dd
		__m128i ms = _mm_cvtsi32_si128( s );
		ms = _mm_unpacklo_epi8( ms, zero_ );	// ms = 00ss00ss00ss00ss
		ms = _mm_mullo_epi16( ms, opa_ );	// ms *= opa
		ms = _mm_srli_epi16( ms, 8 );		// ms >>=8
		md = _mm_add_epi16( md, ms );		// md += ms
		md = _mm_packus_epi16( md, zero_ );	// pack
		return _mm_cvtsi128_si32( md );
	}
	inline __m128i operator()( __m128i md1, __m128i ms1 ) const {
		__m128i md2 = md1;
		md1 = _mm_unpacklo_epi8( md1, zero_ );	// 00dd00dd00dd00dd
		md2 = _mm_unpackhi_epi8( md2, zero_ );	// 00dd00dd00dd00dd
		__m128i ms2 = ms1;
		ms1 = _mm_unpacklo_epi8( ms1, zero_ );	// 00ss00ss00ss00ss
		ms2 = _mm_unpackhi_epi8( ms2, zero_ );	// 00ss00ss00ss00ss
		ms1 = _mm_mullo_epi16( ms1, opa_ );		// s *= opa
		ms2 = _mm_mullo_epi16( ms2, opa_ );		// s *= opa
		ms1 = _mm_srli_epi16( ms1, 8 );			// s >>=8
		ms2 = _mm_srli_epi16( ms2, 8 );			// s >>=8
		md1 = _mm_add_epi16( md1, ms1 );		// d += s
		md2 = _mm_add_epi16( md2, ms2 );		// d += s
		return _mm_packus_epi16( md1, md2 );
	}
};
// sse2_add_blend_functor = TVPAddBlend_sse2_a
// sse2_add_blend_hda_functor = TVPAddBlend_HDA_sse2_a
// sse2_add_blend_o_functor = TVPAddBlend_o_sse2_a
typedef sse2_add_blend_o_functor			sse2_add_blend_hda_o_functor;	// = TVPAddBlend_HDA_o_sse2_a
//--------------------------------------------------------------------
struct sse2_sub_blend_functor {
	const __m128i xor_;
	inline sse2_sub_blend_functor() : xor_(_mm_set1_epi32(0xffffffff)){}
	inline tjs_uint32 operator()( tjs_uint32 d, tjs_uint32 s ) const {
		__m128i md = _mm_cvtsi32_si128( d );
		__m128i ms = _mm_cvtsi32_si128( s );
		ms = _mm_xor_si128( ms, xor_ );	// xor = 1.0 - src
		md = _mm_subs_epu8( md, ms );	// dest -= src
		return _mm_cvtsi128_si32( md );
	}
	inline __m128i operator()( __m128i d, __m128i s ) const {
		s = _mm_xor_si128( s, xor_ );	// xor = 1.0 - src
		return _mm_subs_epu8( d, s );	// dest -= src
	}
};
struct sse2_sub_blend_hda_functor {
	const __m128i mask_;
	const __m128i xor_;
	inline sse2_sub_blend_hda_functor() : mask_(_mm_set1_epi32(0x00ffffff)), xor_(_mm_set1_epi32(0xffffffff)) {}
	inline tjs_uint32 operator()( tjs_uint32 d, tjs_uint32 s ) const {
		__m128i md = _mm_cvtsi32_si128( d );
		__m128i ms = _mm_cvtsi32_si128( s );
		ms = _mm_xor_si128( ms, xor_ );	// xor = 1.0 - src
		ms = _mm_and_si128( ms, mask_ );// mask
		md = _mm_subs_epu8( md, ms );	// dest -= src
		return _mm_cvtsi128_si32( md );
	}
	inline __m128i operator()( __m128i d, __m128i s ) const {
		s = _mm_xor_si128( s, xor_ );	// xor = 1.0 - src
		s = _mm_and_si128( s, mask_ );	// mask
		return _mm_subs_epu8( d, s );	// dest -= src
	}
};
struct sse2_sub_blend_o_functor {
	const __m128i zero_;
	const __m128i xor_;
	const __m128i opa_;
	inline sse2_sub_blend_o_functor( tjs_int opa ) : zero_(_mm_setzero_si128()), xor_(_mm_set1_epi32(0xffffffff))
		, opa_(_mm_srli_epi64(_mm_set1_epi16((short)opa),16)){}
		// opa_ = 000000oo00oo00oo
	inline tjs_uint32 operator()( tjs_uint32 d, tjs_uint32 s ) const {
		__m128i md = _mm_cvtsi32_si128( d );
		md = _mm_unpacklo_epi8( md, zero_ );// md = 00dd00dd00dd00dd
		__m128i ms = _mm_cvtsi32_si128( s );
		ms = _mm_xor_si128( ms, xor_ );		// not
		ms = _mm_unpacklo_epi8( ms, zero_ );// ms = 00ss00ss00ss00ss
		ms = _mm_mullo_epi16( ms, opa_ );	// ms *= opa
		ms = _mm_srli_epi16( ms, 8 );		// ms >>=8
		md = _mm_sub_epi16( md, ms );		// md -= ms
		md = _mm_packus_epi16( md, zero_ );	// pack
		return _mm_cvtsi128_si32( md );
	}
	inline __m128i operator()( __m128i md1, __m128i ms1 ) const {
		__m128i md2 = md1;
		md1 = _mm_unpacklo_epi8( md1, zero_ );	// 00dd00dd00dd00dd
		md2 = _mm_unpackhi_epi8( md2, zero_ );	// 00dd00dd00dd00dd
		ms1 = _mm_xor_si128( ms1, xor_ );		// not
		__m128i ms2 = ms1;
		ms1 = _mm_unpacklo_epi8( ms1, zero_ );	// 00ss00ss00ss00ss
		ms2 = _mm_unpackhi_epi8( ms2, zero_ );	// 00ss00ss00ss00ss
		ms1 = _mm_mullo_epi16( ms1, opa_ );	// s *= opa
		ms2 = _mm_mullo_epi16( ms2, opa_ );	// s *= opa
		ms1 = _mm_srli_epi16( ms1, 8 );		// s >>=8
		ms2 = _mm_srli_epi16( ms2, 8 );		// s >>=8
		md1 = _mm_sub_epi16( md1, ms1 );	// d -= s
		md2 = _mm_sub_epi16( md2, ms2 );	// d -= s
		return _mm_packus_epi16( md1, md2 );	// pack
	}
};
typedef sse2_sub_blend_o_functor			sse2_sub_blend_hda_o_functor;
//--------------------------------------------------------------------
// 非 HDA バリアント: 結果のアルファは 0 (C リファレンス互換)
struct sse2_mul_blend_functor {
	const __m128i zero_;
	const __m128i colormask_;
	inline sse2_mul_blend_functor() : zero_(_mm_setzero_si128()), colormask_(_mm_set1_epi32(0x00ffffff)) {}
	inline tjs_uint32 operator()( tjs_uint32 d, tjs_uint32 s ) const {
		__m128i md = _mm_cvtsi32_si128( d );
		md = _mm_unpacklo_epi8( md, zero_ );
		__m128i ms = _mm_cvtsi32_si128( s );
		ms = _mm_unpacklo_epi8( ms, zero_ );
		md = _mm_mullo_epi16( md, ms );	// multiply
		md = _mm_srli_epi16( md, 8 );	// >>= 8
		md = _mm_packus_epi16( md, zero_ );	// pack
		return _mm_cvtsi128_si32( md ) & 0x00ffffff;
	}
	inline __m128i operator()( __m128i md1, __m128i ms1 ) const {
		__m128i md2 = md1;
		md1 = _mm_unpacklo_epi8( md1, zero_ );	// lo
		md2 = _mm_unpackhi_epi8( md2, zero_ );	// hi
		__m128i ms2 = ms1;
		ms1 = _mm_unpacklo_epi8( ms1, zero_ );	// lo
		ms2 = _mm_unpackhi_epi8( ms2, zero_ );	// hi
		md1 = _mm_mullo_epi16( md1, ms1 );	// lo multiply
		md2 = _mm_mullo_epi16( md2, ms2 );	// hi multiply
		md1 = _mm_srli_epi16( md1, 8 );	// lo shift
		md2 = _mm_srli_epi16( md2, 8 );	// hi shift
		return _mm_and_si128( _mm_packus_epi16( md1, md2 ), colormask_ );
	}
};
struct sse2_mul_blend_hda_functor {
	const __m128i zero_;
	const __m128i mulmask_;
	const __m128i bitmask_;
	inline sse2_mul_blend_hda_functor() : zero_(_mm_setzero_si128())
		, mulmask_(_mm_set_epi32(0x0000ffff,0xffffffff,0x0000ffff,0xffffffff)), bitmask_(_mm_set_epi32(0x01000000,0x00000000,0x01000000,0x00000000)) {}
		//mulmask_ = 0x0000ffffffffffffLL
		//bitmask_ = 0x0100000000000000LL
	inline tjs_uint32 operator()( tjs_uint32 d, tjs_uint32 s ) const {
		__m128i md = _mm_cvtsi32_si128( d );
		md = _mm_unpacklo_epi8( md, zero_ );	// 00dd00dd00dd00dd
		__m128i ms = _mm_cvtsi32_si128( s );
		ms = _mm_unpacklo_epi8( ms, zero_ );	// 00ss00ss00ss00ss
		ms = _mm_and_si128( ms, mulmask_ );	// src & 0x00ffffff (mask)
		ms = _mm_or_si128( ms, bitmask_ );	// src | 0x100ffffff (bit) かけた時にdstアルファが消えないように
		md = _mm_mullo_epi16( md, ms );	// d * s
		md = _mm_srli_epi16( md, 8 );	// d * s >> 8
		md = _mm_packus_epi16( md, zero_ );	// pack
		return _mm_cvtsi128_si32( md );	// store
	}
	inline __m128i operator()( __m128i md1, __m128i ms1 ) const {
		__m128i md2 = md1;
		md1 = _mm_unpacklo_epi8( md1, zero_ );	// lo 00dd00dd00dd00dd
		md2 = _mm_unpackhi_epi8( md2, zero_ );	// hi 00dd00dd00dd00dd
		__m128i ms2 = ms1;
		ms1 = _mm_unpacklo_epi8( ms1, zero_ );	// lo 00ss00ss00ss00ss
		ms2 = _mm_unpackhi_epi8( ms2, zero_ );	// hi 00ss00ss00ss00ss
		ms1 = _mm_and_si128( ms1, mulmask_ );	// src & 0x00ffffff ( mask )
		ms1 = _mm_or_si128( ms1, bitmask_ );		// src | 0x100ffffff (bit)
		ms2 = _mm_and_si128( ms2, mulmask_ );	// src & 0x00ffffff ( mask )
		ms2 = _mm_or_si128( ms2, bitmask_ );		// src | 0x100ffffff (bit)
		md1 = _mm_mullo_epi16( md1, ms1 );		// d * s
		md2 = _mm_mullo_epi16( md2, ms2 );		// d * s
		md1 = _mm_srli_epi16( md1, 8 );	// d >>= 8
		md2 = _mm_srli_epi16( md2, 8 );	// d >>= 8
		return _mm_packus_epi16( md1, md2 );	// pack
	}
};
// 非 HDA バリアント: 結果のアルファは 0 (C リファレンス互換)
struct sse2_mul_blend_o_functor {
	const __m128i zero_;
	const __m128i opa_;
	const __m128i mask_;
	const __m128i colormask_;
	inline sse2_mul_blend_o_functor( tjs_int opa )
		: zero_(_mm_setzero_si128()), opa_(_mm_set1_epi16((short)opa)),
		  mask_(_mm_set1_epi32(0xffffffff)), colormask_(_mm_set1_epi32(0x00ffffff)) {}
	inline tjs_uint32 operator()( tjs_uint32 d, tjs_uint32 s ) const {
		__m128i md = _mm_cvtsi32_si128( d );
		md = _mm_unpacklo_epi8( md, zero_ );	// 00dd00dd00dd00dd
		__m128i ms = _mm_cvtsi32_si128( s );
		ms = _mm_xor_si128( ms, mask_ );		// not src = (1.0-src)
		ms = _mm_unpacklo_epi8( ms, zero_ );	// 00ss00ss00ss00ss
		ms = _mm_mullo_epi16( ms, opa_ );	// s * opa
		ms = _mm_xor_si128( ms, mask_ );		// not src = (1.0-src)
		ms = _mm_srli_epi16( ms, 8 );		// src >>= 8
		md = _mm_mullo_epi16( md, ms );		// d * s
		md = _mm_srli_epi16( md, 8 );		// d >>= 8
		md = _mm_packus_epi16( md, zero_ );	// pack
		return _mm_cvtsi128_si32( md ) & 0x00ffffff;
	}
	inline __m128i operator()( __m128i md1, __m128i ms1 ) const {
		__m128i md2 = md1;
		md1 = _mm_unpacklo_epi8( md1, zero_ );	// lo 00dd00dd00dd00dd
		md2 = _mm_unpackhi_epi8( md2, zero_ );	// hi 00dd00dd00dd00dd
		ms1 = _mm_xor_si128( ms1, mask_ );		// not src
		__m128i ms2 = ms1;
		ms1 = _mm_unpacklo_epi8( ms1, zero_ );	// lo 00ss00ss00ss00ss
		ms2 = _mm_unpackhi_epi8( ms2, zero_ );	// hi 00ss00ss00ss00ss
		ms1 = _mm_mullo_epi16( ms1, opa_ );	// s * opa
		ms2 = _mm_mullo_epi16( ms2, opa_ );	// s * opa
		ms1 = _mm_xor_si128( ms1, mask_ );	// not src
		ms2 = _mm_xor_si128( ms2, mask_ );	// not src
		ms1 = _mm_srli_epi16( ms1, 8 );		// s >>= 8
		ms2 = _mm_srli_epi16( ms2, 8 );		// s >>= 8
		md1 = _mm_mullo_epi16( md1, ms1 );	// d * s
		md2 = _mm_mullo_epi16( md2, ms2 );	// d * s
		md1 = _mm_srli_epi16( md1, 8 );		// d >>= 8
		md2 = _mm_srli_epi16( md2, 8 );		// d >>= 8
		return _mm_and_si128( _mm_packus_epi16( md1, md2 ), colormask_ );
	}
};

struct sse2_mul_blend_hda_o_functor {
	const __m128i zero_;
	const __m128i opa_;
	const __m128i mask_;
	const __m128i mulmask_;
	const __m128i bitmask_;
	inline sse2_mul_blend_hda_o_functor( tjs_int opa ) : zero_(_mm_setzero_si128()), opa_(_mm_set1_epi16((short)opa))
		, mulmask_(_mm_set_epi32(0x0000ffff,0xffffffff,0x0000ffff,0xffffffff)), bitmask_(_mm_set_epi32(0x01000000,0x00000000,0x01000000,0x00000000))
		, mask_(_mm_set1_epi32(0xffffffff)) {}
		// mulmask_ = 0x0000ffffffffffffLL
		// bitmask_ = 0x0100000000000000LL
		// mask_ = 0xffffffffffffffffLL
	inline tjs_uint32 operator()( tjs_uint32 d, tjs_uint32 s ) const {
		__m128i md = _mm_cvtsi32_si128( d );
		md = _mm_unpacklo_epi8( md, zero_ );	// 00dd00dd00dd00dd
		__m128i ms = _mm_cvtsi32_si128( s );
		ms = _mm_xor_si128( ms, mask_ );		// not src
		ms = _mm_unpacklo_epi8( ms, zero_ );	// 00ss00ss00ss00ss
		ms = _mm_mullo_epi16( ms, opa_ );	// s * opa
		ms = _mm_xor_si128( ms, mask_ );		// not src
		ms = _mm_srli_epi16( ms, 8 );		// s >> 8
		ms = _mm_and_si128( ms, mulmask_ );	// s & 0x00ffffff
		ms = _mm_or_si128( ms, bitmask_ );	// s | 0x100000000
		md = _mm_mullo_epi16( md, ms );		// d * s
		md = _mm_srli_epi16( md, 8 );		// d >>= 8
		md = _mm_packus_epi16( md, zero_ );	// pack
		return _mm_cvtsi128_si32( md );
	}
	inline __m128i operator()( __m128i md1, __m128i ms1 ) const {
		__m128i md2 = md1;
		md1 = _mm_unpacklo_epi8( md1, zero_ );	// 00dd00dd00dd00dd
		md2 = _mm_unpackhi_epi8( md2, zero_ );	// 00dd00dd00dd00dd
		ms1 = _mm_xor_si128( ms1, mask_ );		// not src
		__m128i ms2 = ms1;
		ms1 = _mm_unpacklo_epi8( ms1, zero_ );	// 00ss00ss00ss00ss
		ms2 = _mm_unpackhi_epi8( ms2, zero_ );	// 00ss00ss00ss00ss
		ms1 = _mm_mullo_epi16( ms1, opa_ );	// s * opa
		ms2 = _mm_mullo_epi16( ms2, opa_ );	// s * opa
		ms1 = _mm_xor_si128( ms1, mask_ );	// not src
		ms2 = _mm_xor_si128( ms2, mask_ );	// not src
		ms1 = _mm_srli_epi16( ms1, 8 );		// s >>= 8
		ms2 = _mm_srli_epi16( ms2, 8 );		// s >>= 8
		ms1 = _mm_and_si128( ms1, mulmask_ );// s & 0x00ffffff
		ms1 = _mm_or_si128( ms1, bitmask_ );	// s | 0x100000000
		ms2 = _mm_and_si128( ms2, mulmask_ );// s & 0x00ffffff
		ms2 = _mm_or_si128( ms2, bitmask_ );	// s | 0x100000000
		md1 = _mm_mullo_epi16( md1, ms1 );	// d * s
		md2 = _mm_mullo_epi16( md2, ms2 );	// d * s
		md1 = _mm_srli_epi16( md1, 8 );		// d >>= 8
		md2 = _mm_srli_epi16( md2, 8 );		// d >>= 8
		return _mm_packus_epi16( md1, md2 );	// pack
	}
};
//--------------------------------------------------------------------
// lighten : normal/HDA
// The basic algorithm is:
// dest = b + sat(a-b)
// where sat(x) = x<0?0:x (zero lower saturation)
struct sse2_lighten_blend_functor {
	inline tjs_uint32 operator()( tjs_uint32 d, tjs_uint32 s ) const {
		__m128i ms = _mm_cvtsi32_si128( s );
		__m128i md = _mm_cvtsi32_si128( d );
		ms = _mm_subs_epu8( ms, md );
		ms = _mm_add_epi8( ms, md );
		return _mm_cvtsi128_si32( ms );
	}
	inline __m128i operator()( __m128i d, __m128i s ) const {
		s = _mm_subs_epu8( s, d );
		return _mm_add_epi8( s, d );
	}
};
struct sse2_lighten_blend_hda_functor {
	const __m128i mask_;
	inline sse2_lighten_blend_hda_functor() : mask_(_mm_set1_epi32(0x00ffffff)) {}
	inline tjs_uint32 operator()( tjs_uint32 d, tjs_uint32 s ) const {
		__m128i ms = _mm_cvtsi32_si128( s );
		__m128i md = _mm_cvtsi32_si128( d );
		ms = _mm_and_si128( ms, mask_ );
		ms = _mm_subs_epu8( ms, md );
		ms = _mm_add_epi8( ms, md );
		return _mm_cvtsi128_si32( ms );
	}
	inline __m128i operator()( __m128i d, __m128i s ) const {
		s = _mm_and_si128( s, mask_ );
		s = _mm_subs_epu8( s, d );
		return _mm_add_epi8( s, d );
	}
};
//--------------------------------------------------------------------
// darken : normal/HDA
// dest = b - sat(b-a)
// where sat(x) = x<0?0:x (zero lower saturation)
struct sse2_darken_blend_functor {
	inline tjs_uint32 operator()( tjs_uint32 d, tjs_uint32 s ) const {
		__m128i md = _mm_cvtsi32_si128( d );
		__m128i md2 = md;
		__m128i ms = _mm_cvtsi32_si128( s );
		md2 = _mm_subs_epu8( md2, ms );
		md = _mm_sub_epi8( md, md2 );
		return _mm_cvtsi128_si32( md );
	}
	inline __m128i operator()( __m128i d, __m128i s ) const {
		__m128i d2 = d;
		d2 = _mm_subs_epu8( d2, s );
		return _mm_sub_epi8( d, d2 );
	}
};
struct sse2_darken_blend_hda_functor {
	const __m128i mask_;
	inline sse2_darken_blend_hda_functor() : mask_(_mm_set1_epi32(0x00ffffff)) {}
	inline tjs_uint32 operator()( tjs_uint32 d, tjs_uint32 s ) const {
		__m128i md = _mm_cvtsi32_si128( d );
		//md = _mm_or_si128( md, mask_ ); // オリジナルにあるこれ何？
		__m128i md2 = md;
		__m128i ms = _mm_cvtsi32_si128( s );
		md2 = _mm_subs_epu8( md2, ms );
		md2 = _mm_and_si128( md2, mask_ );
		md = _mm_sub_epi8( md, md2 );
		return _mm_cvtsi128_si32( md );
	}
	inline __m128i operator()( __m128i d, __m128i s ) const {
		__m128i d2 = d;
		d2 = _mm_subs_epu8( d2, s );
		d2 = _mm_and_si128( d2, mask_ );
		return _mm_sub_epi8( d, d2 );
	}
};

//--------------------------------------------------------------------
// screen : normal/HDA/o/HDA o
// 非 HDA バリアント: C リファレンスは alpha レーンに 0xff を出力する
// (~s,~d で乗算した結果 alpha バイトが 0、最終 ~ で 0xff になる)
struct sse2_screen_blend_functor {
	const __m128i mask_;
	const __m128i zero_;
	const __m128i alphamask_;
	const __m128i colormask_;
	inline sse2_screen_blend_functor()
		: zero_(_mm_setzero_si128()), mask_(_mm_set1_epi32(0xffffffff)),
		  alphamask_(_mm_set1_epi32(0xff000000)),
		  colormask_(_mm_set1_epi32(0x00ffffff)) {}
	inline tjs_uint32 operator()( tjs_uint32 d, tjs_uint32 s ) const {
		__m128i md = _mm_cvtsi32_si128( d );
		md = _mm_xor_si128( md, mask_ );	// not dest
		md = _mm_unpacklo_epi8( md, zero_ );
		__m128i ms = _mm_cvtsi32_si128( s );
		ms = _mm_xor_si128( ms, mask_ );	// not src
		ms = _mm_unpacklo_epi8( ms, zero_ );
		md = _mm_mullo_epi16( md, ms );	// d * s
		md = _mm_srli_epi16( md, 8 );	// (d * s) >> 8
		md = _mm_packus_epi16( md, zero_ );	// pack
		md = _mm_xor_si128( md, mask_ );	// not result
		return (_mm_cvtsi128_si32( md ) & 0x00ffffff) | 0xff000000;
	}
	inline __m128i operator()( __m128i md1, __m128i ms1 ) const {
		md1 = _mm_xor_si128( md1, mask_ );	// not dest
		__m128i md2 = md1;
		md1 = _mm_unpacklo_epi8( md1, zero_ );	// lo 00dd00dd00dd00dd
		md2 = _mm_unpackhi_epi8( md2, zero_ );	// hi 00dd00dd00dd00dd
		ms1 = _mm_xor_si128( ms1, mask_ );	// not src
		__m128i ms2 = ms1;
		ms1 = _mm_unpacklo_epi8( ms1, zero_ );	// lo 00ss00ss00ss00ss
		ms2 = _mm_unpackhi_epi8( ms2, zero_ );	// hi 00ss00ss00ss00ss
		md1 = _mm_mullo_epi16( md1, ms1 );	// d * s
		md2 = _mm_mullo_epi16( md2, ms2 );	// d * s
		md1 = _mm_srli_epi16( md1, 8 );	// d >> 8
		md2 = _mm_srli_epi16( md2, 8 );	// d >> 8
		md1 = _mm_packus_epi16( md1, md2 );	// pack
		md1 = _mm_xor_si128( md1, mask_ );	// not result
		return _mm_or_si128( _mm_and_si128( md1, colormask_ ), alphamask_ );
	}
};
struct sse2_screen_blend_hda_functor {
	const __m128i mulmask_;
	const __m128i mul100_;
	const __m128i zero_;
	inline sse2_screen_blend_hda_functor() : zero_(_mm_setzero_si128())
		, mulmask_(_mm_set1_epi32(0x00ffffff)), mul100_(_mm_set_epi32(0x01000000,0x00000000,0x01000000,0x00000000)){}
	inline tjs_uint32 operator()( tjs_uint32 d, tjs_uint32 s ) const {
		__m128i md = _mm_cvtsi32_si128( d );
		md = _mm_xor_si128( md, mulmask_ );	// not dest
		md = _mm_unpacklo_epi8( md, zero_ );
		__m128i ms = _mm_cvtsi32_si128( s );
		ms = _mm_xor_si128( ms, mulmask_ );	// not src
		ms = _mm_and_si128( ms, mulmask_ );	// mask src オリジナルでは抜けてる？
		ms = _mm_unpacklo_epi8( ms, zero_ );
		ms = _mm_or_si128( ms, mul100_ );	// 1 or-1
		md = _mm_mullo_epi16( md, ms );
		md = _mm_srli_epi16( md, 8 );
		md = _mm_packus_epi16( md, zero_ );
		md = _mm_xor_si128( md, mulmask_ );	// not result
		return _mm_cvtsi128_si32( md );
	}
	inline __m128i operator()( __m128i md1, __m128i ms1 ) const {
		md1 = _mm_xor_si128( md1, mulmask_ );	// not dest
		__m128i md2 = md1;
		md1 = _mm_unpacklo_epi8( md1, zero_ );
		md2 = _mm_unpackhi_epi8( md2, zero_ );
		ms1 = _mm_xor_si128( ms1, mulmask_ );	// not src
		ms1 = _mm_and_si128( ms1, mulmask_ );	// mask src
		__m128i ms2 = ms1;
		ms1 = _mm_unpacklo_epi8( ms1, zero_ );
		ms2 = _mm_unpackhi_epi8( ms2, zero_ );
		ms1 = _mm_or_si128( ms1, mul100_ );	// or-1
		ms2 = _mm_or_si128( ms2, mul100_ );	// or-1
		md1 = _mm_mullo_epi16( md1, ms1 );
		md2 = _mm_mullo_epi16( md2, ms2 );
		md1 = _mm_srli_epi16( md1, 8 );
		md2 = _mm_srli_epi16( md2, 8 );
		md1 = _mm_packus_epi16( md1, md2 );
		return _mm_xor_si128( md1, mulmask_ );	// not result
	}
};
struct sse2_screen_blend_o_functor {
	const __m128i mask_;
	const __m128i zero_;
	const __m128i opa_;
	const __m128i alphamask_;
	const __m128i colormask_;
	inline sse2_screen_blend_o_functor( tjs_int opa )
		: zero_(_mm_setzero_si128()), opa_(_mm_set1_epi16((short)opa)),
		  mask_(_mm_set1_epi32(0xffffffff)),
		  alphamask_(_mm_set1_epi32(0xff000000)),
		  colormask_(_mm_set1_epi32(0x00ffffff)) {}
	inline tjs_uint32 operator()( tjs_uint32 d, tjs_uint32 s ) const {
		__m128i md = _mm_cvtsi32_si128( d );
		md = _mm_xor_si128( md, mask_ );		// not dest
		md = _mm_unpacklo_epi8( md, zero_ );
		__m128i ms = _mm_cvtsi32_si128( s );
		ms = _mm_unpacklo_epi8( ms, zero_ );
		ms = _mm_mullo_epi16( ms, opa_ );// opa multiply
		ms = _mm_xor_si128( ms, mask_ );	// not src
		ms = _mm_srli_epi16( ms, 8 );	// opa shift
		md = _mm_mullo_epi16( md, ms );	// multiply
		md = _mm_srli_epi16( md, 8 );	// shift
		md = _mm_packus_epi16( md, zero_ );// pack
		md = _mm_xor_si128( md, mask_ );	// not result
		// alpha は C リファレンス (~tmp、alpha レーン=0 の反転) と同じ 0xff 固定
		return (_mm_cvtsi128_si32( md ) & 0x00ffffff) | 0xff000000;
	}
	inline __m128i operator()( __m128i md1, __m128i ms1 ) const {
		md1 = _mm_xor_si128( md1, mask_ );	// not dest
		__m128i md2 = md1;
		md1 = _mm_unpacklo_epi8( md1, zero_ );
		md2 = _mm_unpackhi_epi8( md2, zero_ );
		__m128i ms2 = ms1;
		ms1 = _mm_unpacklo_epi8( ms1, zero_ );
		ms2 = _mm_unpackhi_epi8( ms2, zero_ );
		ms1 = _mm_mullo_epi16( ms1, opa_ );	// opa multiply
		ms2 = _mm_mullo_epi16( ms2, opa_ );	// opa multiply
		ms1 = _mm_xor_si128( ms1, mask_ );	// not src
		ms2 = _mm_xor_si128( ms2, mask_ );	// not src
		ms1 = _mm_srli_epi16( ms1, 8 );		// opa shift
		ms2 = _mm_srli_epi16( ms2, 8 );		// opa shift
		md1 = _mm_mullo_epi16( md1, ms1 );	// multiply
		md2 = _mm_mullo_epi16( md2, ms2 );	// multiply
		md1 = _mm_srli_epi16( md1, 8 );		// shift
		md2 = _mm_srli_epi16( md2, 8 );		// shift
		md1 = _mm_packus_epi16( md1, md2 );	// pack
		md1 = _mm_xor_si128( md1, mask_ );	// not result
		// alpha は C リファレンスと同じ 0xff 固定 (plain 版と同様)
		return _mm_or_si128( _mm_and_si128( md1, colormask_ ), alphamask_ );
	}
};
struct sse2_screen_blend_hda_o_functor {
	const __m128i zero_;
	const __m128i opa_;
	const __m128i mask_;
	const __m128i mulmask_;
	const __m128i alphamask_;
	inline sse2_screen_blend_hda_o_functor( tjs_int opa ) : zero_(_mm_setzero_si128()), opa_(_mm_set1_epi16((short)opa)), mask_(_mm_set1_epi32(0xffffffff))
		, mulmask_(_mm_set1_epi32(0x00ffffff)), alphamask_(_mm_set1_epi32(0xff000000)){}
	inline tjs_uint32 operator()( tjs_uint32 d, tjs_uint32 s ) const {
		__m128i md = _mm_cvtsi32_si128( d );
		md = _mm_xor_si128( md, mulmask_ );	// not dest
		md = _mm_unpacklo_epi8( md, zero_ );	// 00dd00dd00dd00dd
		__m128i ms = _mm_cvtsi32_si128( s );
		ms = _mm_unpacklo_epi8( ms, zero_ );	// 00ss00ss00ss00ss
		ms = _mm_mullo_epi16( ms, opa_ );	// s * opa
		ms = _mm_xor_si128( ms, mask_ );		// not src
		ms = _mm_srli_epi16( ms, 8 );		// s >> 8
		//ms = _mm_and_si128( ms, alphamask_ );// src mask
		//ms = _mm_or_si128( ms, mulmask_ );	// src alpha ???
		md = _mm_mullo_epi16( md, ms );		// d * s
		md = _mm_srli_epi16( md, 8 );		// d >> 8
		__m128i md2 = _mm_cvtsi32_si128( d );
		md = _mm_packus_epi16( md, zero_ );
		md2 = _mm_and_si128( md2, alphamask_ );	// dest mask
		md = _mm_xor_si128( md, mulmask_ );	// not result
		md = _mm_and_si128( md, mulmask_ );	// drop result alpha
		md = _mm_or_si128( md, md2 );	// restore dest alpha
		return _mm_cvtsi128_si32( md );
	}
	inline __m128i operator()( __m128i md1, __m128i ms1 ) const {
		__m128i mdr = md1;
		mdr = _mm_and_si128( mdr, alphamask_ );	// mask
		md1 = _mm_xor_si128( md1, mulmask_ );	// not dest
		__m128i md2 = md1;
		md1 = _mm_unpacklo_epi8( md1, zero_ );
		md2 = _mm_unpackhi_epi8( md2, zero_ );
		__m128i ms2 = ms1;
		ms1 = _mm_unpacklo_epi8( ms1, zero_ );
		ms2 = _mm_unpackhi_epi8( ms2, zero_ );
		ms1 = _mm_mullo_epi16( ms1, opa_ );	// opa multiply
		ms2 = _mm_mullo_epi16( ms2, opa_ );	// opa multiply
		ms1 = _mm_xor_si128( ms1, mask_ );	// not src
		ms2 = _mm_xor_si128( ms2, mask_ );	// not src
		ms1 = _mm_srli_epi16( ms1, 8 );		// opa shift
		ms2 = _mm_srli_epi16( ms2, 8 );		// opa shift
		md1 = _mm_mullo_epi16( md1, ms1 );	// multiply
		md2 = _mm_mullo_epi16( md2, ms2 );	// multiply
		md1 = _mm_srli_epi16( md1, 8 );		// shift
		md2 = _mm_srli_epi16( md2, 8 );		// shift
		md1 = _mm_packus_epi16( md1, md2 );	// pack
		md1 = _mm_xor_si128( md1, mulmask_ );	// not result
		md1 = _mm_and_si128( md1, mulmask_ );	// drop result alpha
		return _mm_or_si128( md1, mdr );			// restore dest alpha
	}
};


#endif // __BLEND_FUNCTOR_SSE2_H__
