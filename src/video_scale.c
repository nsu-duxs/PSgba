#include "video_scale.h"
#include <string.h>
#include <immintrin.h>

typedef uint32_t u32;

#define VS_TOL 16 /* tolerancia por canal do modo xBR-lite */
#define VS_INLINE static inline __attribute__((always_inline))

/* ------------------------------------------------------------------ */
/* Primitivas de pixel (escalar) - referencia exata do caminho SIMD    */
/* ------------------------------------------------------------------ */

/* Media por canal arredondada para cima: identica a _mm_avg_epu8 */
VS_INLINE u32 px_avg(u32 a, u32 b) {
    return (a | b) - (((a ^ b) & 0xFEFEFEFEu) >> 1);
}

VS_INLINE bool px_sim(u32 a, u32 b) {
    for (int i = 0; i < 32; i += 8) {
        int d = (int)((a >> i) & 0xFFu) - (int)((b >> i) & 0xFFu);
        if (d < 0) {
            d = -d;
        }
        if (d > VS_TOL) {
            return false;
        }
    }
    return true;
}

/* m: 0 = Scale (exato), 1 = HQ-like (exato + 75/25), 2 = xBR-lite (tolerante + 50/50) */
VS_INLINE bool px_eq(int m, u32 a, u32 b) {
    return (m < 2) ? (a == b) : px_sim(a, b);
}

VS_INLINE u32 px_repl(int m, u32 x, u32 e) {
    if (m == 0) {
        return x;
    }
    if (m == 1) {
        return px_avg(x, px_avg(x, e));
    }
    return px_avg(x, e);
}

/* Scale2x (amadvance): B=cima D=esq E=centro F=dir H=baixo */
VS_INLINE void px_scale2x(int m, u32 B, u32 D, u32 E, u32 F, u32 H, u32 o[4]) {
    o[0] = o[1] = o[2] = o[3] = E;
    if (!px_eq(m, B, H) && !px_eq(m, D, F)) {
        if (px_eq(m, D, B)) { o[0] = px_repl(m, D, E); }
        if (px_eq(m, B, F)) { o[1] = px_repl(m, F, E); }
        if (px_eq(m, D, H)) { o[2] = px_repl(m, D, E); }
        if (px_eq(m, H, F)) { o[3] = px_repl(m, F, E); }
    }
}

/* Scale3x (amadvance): vizinhanca A B C / D E F / G H I */
VS_INLINE void px_scale3x(int m, u32 A, u32 B, u32 C, u32 D, u32 E, u32 F,
                          u32 G, u32 H, u32 I, u32 o[9]) {
    for (int k = 0; k < 9; ++k) {
        o[k] = E;
    }
    if (!px_eq(m, B, H) && !px_eq(m, D, F)) {
        bool dB = px_eq(m, D, B);
        bool bF = px_eq(m, B, F);
        bool dH = px_eq(m, D, H);
        bool hF = px_eq(m, H, F);
        if (dB) { o[0] = px_repl(m, D, E); }
        if ((dB && !px_eq(m, E, C)) || (bF && !px_eq(m, E, A))) { o[1] = px_repl(m, B, E); }
        if (bF) { o[2] = px_repl(m, F, E); }
        if ((dB && !px_eq(m, E, G)) || (dH && !px_eq(m, E, A))) { o[3] = px_repl(m, D, E); }
        if ((bF && !px_eq(m, E, I)) || (hF && !px_eq(m, E, C))) { o[5] = px_repl(m, F, E); }
        if (dH) { o[6] = px_repl(m, D, E); }
        if ((dH && !px_eq(m, E, I)) || (hF && !px_eq(m, E, G))) { o[7] = px_repl(m, H, E); }
        if (hF) { o[8] = px_repl(m, F, E); }
    }
}

/* ------------------------------------------------------------------ */
/* Primitivas SIMD (4 pixels ARGB por registrador)                      */
/* ------------------------------------------------------------------ */

VS_INLINE __m128i v_eq(int m, __m128i a, __m128i b) {
    if (m < 2) {
        return _mm_cmpeq_epi32(a, b);
    }
    __m128i ad = _mm_or_si128(_mm_subs_epu8(a, b), _mm_subs_epu8(b, a));
    return _mm_cmpeq_epi32(_mm_subs_epu8(ad, _mm_set1_epi8(VS_TOL)), _mm_setzero_si128());
}

VS_INLINE __m128i v_repl(int m, __m128i x, __m128i e) {
    if (m == 0) {
        return x;
    }
    if (m == 1) {
        return _mm_avg_epu8(x, _mm_avg_epu8(x, e));
    }
    return _mm_avg_epu8(x, e);
}

VS_INLINE __m128i v_not(__m128i a) {
    return _mm_xor_si128(a, _mm_set1_epi32(-1));
}

/* ------------------------------------------------------------------ */
/* XBGR -> ARGB                                                         */
/* ------------------------------------------------------------------ */

void vscale_convert_xbgr(const uint32_t* src_xbgr, uint32_t* dst_argb) {
    const __m128i shuf = _mm_setr_epi8(2, 1, 0, -1, 6, 5, 4, -1, 10, 9, 8, -1, 14, 13, 12, -1);
    const __m128i alpha = _mm_set1_epi32((int)0xFF000000u);
    const int total = VSCALE_SRC_W * VSCALE_SRC_H;

    for (int i = 0; i < total; i += 8) {
        __m128i v0 = _mm_loadu_si128((const __m128i*)&src_xbgr[i]);
        __m128i v1 = _mm_loadu_si128((const __m128i*)&src_xbgr[i + 4]);
        _mm_storeu_si128((__m128i*)&dst_argb[i], _mm_or_si128(_mm_shuffle_epi8(v0, shuf), alpha));
        _mm_storeu_si128((__m128i*)&dst_argb[i + 4], _mm_or_si128(_mm_shuffle_epi8(v1, shuf), alpha));
    }
}

/* ------------------------------------------------------------------ */
/* Nearest (replicacao inteira)                                          */
/* ------------------------------------------------------------------ */

void vscale_nearest(const uint32_t* src, int w, int h, uint32_t* dst, int k) {
    const __m128i s3_0 = _mm_setr_epi8(0, 1, 2, 3, 0, 1, 2, 3, 0, 1, 2, 3, 4, 5, 6, 7);
    const __m128i s3_1 = _mm_setr_epi8(4, 5, 6, 7, 4, 5, 6, 7, 8, 9, 10, 11, 8, 9, 10, 11);
    const __m128i s3_2 = _mm_setr_epi8(8, 9, 10, 11, 12, 13, 14, 15, 12, 13, 14, 15, 12, 13, 14, 15);
    const size_t row_len = (size_t)w * (size_t)k;

    for (int y = 0; y < h; ++y) {
        const u32* s = src + (size_t)y * (size_t)w;
        u32* d = dst + (size_t)y * (size_t)k * row_len;
        int x = 0;

        if (k == 2) {
            for (; x <= w - 4; x += 4) {
                __m128i p = _mm_loadu_si128((const __m128i*)&s[x]);
                _mm_storeu_si128((__m128i*)&d[x * 2], _mm_unpacklo_epi32(p, p));
                _mm_storeu_si128((__m128i*)&d[x * 2 + 4], _mm_unpackhi_epi32(p, p));
            }
        } else if (k == 3) {
            for (; x <= w - 4; x += 4) {
                __m128i p = _mm_loadu_si128((const __m128i*)&s[x]);
                _mm_storeu_si128((__m128i*)&d[x * 3], _mm_shuffle_epi8(p, s3_0));
                _mm_storeu_si128((__m128i*)&d[x * 3 + 4], _mm_shuffle_epi8(p, s3_1));
                _mm_storeu_si128((__m128i*)&d[x * 3 + 8], _mm_shuffle_epi8(p, s3_2));
            }
        } else if (k == 4) {
            for (; x <= w - 4; x += 4) {
                __m128i p = _mm_loadu_si128((const __m128i*)&s[x]);
                _mm_storeu_si128((__m128i*)&d[x * 4], _mm_shuffle_epi32(p, _MM_SHUFFLE(0, 0, 0, 0)));
                _mm_storeu_si128((__m128i*)&d[x * 4 + 4], _mm_shuffle_epi32(p, _MM_SHUFFLE(1, 1, 1, 1)));
                _mm_storeu_si128((__m128i*)&d[x * 4 + 8], _mm_shuffle_epi32(p, _MM_SHUFFLE(2, 2, 2, 2)));
                _mm_storeu_si128((__m128i*)&d[x * 4 + 12], _mm_shuffle_epi32(p, _MM_SHUFFLE(3, 3, 3, 3)));
            }
        } else if (k == 6) {
            for (; x <= w - 4; x += 4) {
                __m128i p = _mm_loadu_si128((const __m128i*)&s[x]);
                u32* o = &d[x * 6];
                _mm_storeu_si128((__m128i*)&o[0], _mm_shuffle_epi32(p, _MM_SHUFFLE(0, 0, 0, 0)));
                _mm_storeu_si128((__m128i*)&o[4], _mm_shuffle_epi32(p, _MM_SHUFFLE(1, 1, 0, 0)));
                _mm_storeu_si128((__m128i*)&o[8], _mm_shuffle_epi32(p, _MM_SHUFFLE(1, 1, 1, 1)));
                _mm_storeu_si128((__m128i*)&o[12], _mm_shuffle_epi32(p, _MM_SHUFFLE(2, 2, 2, 2)));
                _mm_storeu_si128((__m128i*)&o[16], _mm_shuffle_epi32(p, _MM_SHUFFLE(3, 3, 2, 2)));
                _mm_storeu_si128((__m128i*)&o[20], _mm_shuffle_epi32(p, _MM_SHUFFLE(3, 3, 3, 3)));
            }
        }
        for (; x < w; ++x) {
            for (int j = 0; j < k; ++j) {
                d[(size_t)x * (size_t)k + (size_t)j] = s[x];
            }
        }
        for (int r = 1; r < k; ++r) {
            memcpy(d + (size_t)r * row_len, d, row_len * sizeof(u32));
        }
    }
}

/* ------------------------------------------------------------------ */
/* Scale2x / HQ-like / xBR-lite 2x                                      */
/* ------------------------------------------------------------------ */

VS_INLINE void edge2x_px(int m, const u32* ru, const u32* rc, const u32* rd,
                         int x, int w, u32* d0, u32* d1) {
    u32 l = rc[x > 0 ? x - 1 : x];
    u32 r = rc[x < w - 1 ? x + 1 : x];
    u32 o[4];
    px_scale2x(m, ru[x], l, rc[x], r, rd[x], o);
    d0[2 * x] = o[0];
    d0[2 * x + 1] = o[1];
    d1[2 * x] = o[2];
    d1[2 * x + 1] = o[3];
}

VS_INLINE void edge2x_impl(int m, const u32* src, int w, int h, u32* dst) {
    for (int y = 0; y < h; ++y) {
        const u32* rc = src + (size_t)y * (size_t)w;
        const u32* ru = (y > 0) ? rc - w : rc;
        const u32* rd = (y < h - 1) ? rc + w : rc;
        u32* d0 = dst + (size_t)(2 * y) * (size_t)(2 * w);
        u32* d1 = d0 + 2 * w;

        edge2x_px(m, ru, rc, rd, 0, w, d0, d1);
        int x = 1;
        for (; x <= w - 5; x += 4) {
            __m128i E = _mm_loadu_si128((const __m128i*)&rc[x]);
            __m128i D = _mm_loadu_si128((const __m128i*)&rc[x - 1]);
            __m128i F = _mm_loadu_si128((const __m128i*)&rc[x + 1]);
            __m128i B = _mm_loadu_si128((const __m128i*)&ru[x]);
            __m128i H = _mm_loadu_si128((const __m128i*)&rd[x]);

            __m128i cond = v_not(_mm_or_si128(v_eq(m, B, H), v_eq(m, D, F)));
            __m128i rD = v_repl(m, D, E);
            __m128i rF = v_repl(m, F, E);

            __m128i e0 = _mm_blendv_epi8(E, rD, _mm_and_si128(cond, v_eq(m, D, B)));
            __m128i e1 = _mm_blendv_epi8(E, rF, _mm_and_si128(cond, v_eq(m, B, F)));
            __m128i e2 = _mm_blendv_epi8(E, rD, _mm_and_si128(cond, v_eq(m, D, H)));
            __m128i e3 = _mm_blendv_epi8(E, rF, _mm_and_si128(cond, v_eq(m, H, F)));

            _mm_storeu_si128((__m128i*)&d0[x * 2], _mm_unpacklo_epi32(e0, e1));
            _mm_storeu_si128((__m128i*)&d0[x * 2 + 4], _mm_unpackhi_epi32(e0, e1));
            _mm_storeu_si128((__m128i*)&d1[x * 2], _mm_unpacklo_epi32(e2, e3));
            _mm_storeu_si128((__m128i*)&d1[x * 2 + 4], _mm_unpackhi_epi32(e2, e3));
        }
        for (; x < w; ++x) {
            edge2x_px(m, ru, rc, rd, x, w, d0, d1);
        }
    }
}

static void edge2x_scale(const u32* s, int w, int h, u32* d) { edge2x_impl(0, s, w, h, d); }
static void edge2x_hq(const u32* s, int w, int h, u32* d)    { edge2x_impl(1, s, w, h, d); }
static void edge2x_xbr(const u32* s, int w, int h, u32* d)   { edge2x_impl(2, s, w, h, d); }

/* ------------------------------------------------------------------ */
/* Scale3x / HQ-like / xBR-lite 3x                                      */
/* ------------------------------------------------------------------ */

VS_INLINE void edge3x_px(int m, const u32* ru, const u32* rc, const u32* rd,
                         int x, int w, u32* d0, u32* d1, u32* d2) {
    int xl = (x > 0) ? x - 1 : x;
    int xr = (x < w - 1) ? x + 1 : x;
    u32 o[9];
    px_scale3x(m, ru[xl], ru[x], ru[xr], rc[xl], rc[x], rc[xr], rd[xl], rd[x], rd[xr], o);
    u32* p0 = d0 + 3 * x;
    u32* p1 = d1 + 3 * x;
    u32* p2 = d2 + 3 * x;
    p0[0] = o[0]; p0[1] = o[1]; p0[2] = o[2];
    p1[0] = o[3]; p1[1] = o[4]; p1[2] = o[5];
    p2[0] = o[6]; p2[1] = o[7]; p2[2] = o[8];
}

VS_INLINE void edge3x_impl(int m, const u32* src, int w, int h, u32* dst) {
    for (int y = 0; y < h; ++y) {
        const u32* rc = src + (size_t)y * (size_t)w;
        const u32* ru = (y > 0) ? rc - w : rc;
        const u32* rd = (y < h - 1) ? rc + w : rc;
        u32* d0 = dst + (size_t)(3 * y) * (size_t)(3 * w);
        u32* d1 = d0 + 3 * w;
        u32* d2 = d1 + 3 * w;

        edge3x_px(m, ru, rc, rd, 0, w, d0, d1, d2);
        int x = 1;
        for (; x <= w - 5; x += 4) {
            __m128i A = _mm_loadu_si128((const __m128i*)&ru[x - 1]);
            __m128i B = _mm_loadu_si128((const __m128i*)&ru[x]);
            __m128i C = _mm_loadu_si128((const __m128i*)&ru[x + 1]);
            __m128i D = _mm_loadu_si128((const __m128i*)&rc[x - 1]);
            __m128i E = _mm_loadu_si128((const __m128i*)&rc[x]);
            __m128i F = _mm_loadu_si128((const __m128i*)&rc[x + 1]);
            __m128i G = _mm_loadu_si128((const __m128i*)&rd[x - 1]);
            __m128i H = _mm_loadu_si128((const __m128i*)&rd[x]);
            __m128i I = _mm_loadu_si128((const __m128i*)&rd[x + 1]);

            __m128i base = v_not(_mm_or_si128(v_eq(m, B, H), v_eq(m, D, F)));
            __m128i dB = v_eq(m, D, B);
            __m128i bF = v_eq(m, B, F);
            __m128i dH = v_eq(m, D, H);
            __m128i hF = v_eq(m, H, F);
            __m128i nEA = v_not(v_eq(m, E, A));
            __m128i nEC = v_not(v_eq(m, E, C));
            __m128i nEG = v_not(v_eq(m, E, G));
            __m128i nEI = v_not(v_eq(m, E, I));

            __m128i rD = v_repl(m, D, E);
            __m128i rB = v_repl(m, B, E);
            __m128i rF = v_repl(m, F, E);
            __m128i rH = v_repl(m, H, E);

            __m128i c0 = _mm_and_si128(base, dB);
            __m128i c1 = _mm_and_si128(base, _mm_or_si128(_mm_and_si128(dB, nEC), _mm_and_si128(bF, nEA)));
            __m128i c2 = _mm_and_si128(base, bF);
            __m128i c3 = _mm_and_si128(base, _mm_or_si128(_mm_and_si128(dB, nEG), _mm_and_si128(dH, nEA)));
            __m128i c5 = _mm_and_si128(base, _mm_or_si128(_mm_and_si128(bF, nEI), _mm_and_si128(hF, nEC)));
            __m128i c6 = _mm_and_si128(base, dH);
            __m128i c7 = _mm_and_si128(base, _mm_or_si128(_mm_and_si128(dH, nEI), _mm_and_si128(hF, nEG)));
            __m128i c8 = _mm_and_si128(base, hF);

            u32 t[9][4] __attribute__((aligned(16)));
            _mm_store_si128((__m128i*)t[0], _mm_blendv_epi8(E, rD, c0));
            _mm_store_si128((__m128i*)t[1], _mm_blendv_epi8(E, rB, c1));
            _mm_store_si128((__m128i*)t[2], _mm_blendv_epi8(E, rF, c2));
            _mm_store_si128((__m128i*)t[3], _mm_blendv_epi8(E, rD, c3));
            _mm_store_si128((__m128i*)t[4], E);
            _mm_store_si128((__m128i*)t[5], _mm_blendv_epi8(E, rF, c5));
            _mm_store_si128((__m128i*)t[6], _mm_blendv_epi8(E, rD, c6));
            _mm_store_si128((__m128i*)t[7], _mm_blendv_epi8(E, rH, c7));
            _mm_store_si128((__m128i*)t[8], _mm_blendv_epi8(E, rF, c8));

            /* ponytail: scatter escalar 3x3; interleave por shuffle so se o perfil mostrar gargalo */
            for (int i = 0; i < 4; ++i) {
                u32* p0 = d0 + 3 * (x + i);
                u32* p1 = d1 + 3 * (x + i);
                u32* p2 = d2 + 3 * (x + i);
                p0[0] = t[0][i]; p0[1] = t[1][i]; p0[2] = t[2][i];
                p1[0] = t[3][i]; p1[1] = t[4][i]; p1[2] = t[5][i];
                p2[0] = t[6][i]; p2[1] = t[7][i]; p2[2] = t[8][i];
            }
        }
        for (; x < w; ++x) {
            edge3x_px(m, ru, rc, rd, x, w, d0, d1, d2);
        }
    }
}

static void edge3x_scale(const u32* s, int w, int h, u32* d) { edge3x_impl(0, s, w, h, d); }
static void edge3x_hq(const u32* s, int w, int h, u32* d)    { edge3x_impl(1, s, w, h, d); }
static void edge3x_xbr(const u32* s, int w, int h, u32* d)   { edge3x_impl(2, s, w, h, d); }

typedef void (*EdgeFn)(const u32*, int, int, u32*);

static const EdgeFn k_edge2x[3] = { edge2x_scale, edge2x_hq, edge2x_xbr };
static const EdgeFn k_edge3x[3] = { edge3x_scale, edge3x_hq, edge3x_xbr };

const uint32_t* vscale_build(VScaleAlgo algo, int factor, const uint32_t* src1x,
                             uint32_t* final_buf, uint32_t* mid_buf) {
    const int w = VSCALE_SRC_W;
    const int h = VSCALE_SRC_H;

    if (factor <= 1) {
        return src1x;
    }
    if (algo == VSCALE_NEAREST || factor == 5 || factor > 6) {
        /* fatores fora de {2,3,4,6} caem em nearest generico (nunca chegam do menu) */
        vscale_nearest(src1x, w, h, final_buf, factor > VSCALE_MAX_FACTOR ? VSCALE_MAX_FACTOR : factor);
        return final_buf;
    }

    int m = (int)algo - 1; /* SCALEX=0, HQ=1, XBR=2 */
    switch (factor) {
    case 2:
        k_edge2x[m](src1x, w, h, final_buf);
        break;
    case 3:
        k_edge3x[m](src1x, w, h, final_buf);
        break;
    case 4:
        k_edge2x[m](src1x, w, h, mid_buf);
        k_edge2x[m](mid_buf, w * 2, h * 2, final_buf);
        break;
    default: /* 6 = 2x depois 3x */
        k_edge2x[m](src1x, w, h, mid_buf);
        k_edge3x[m](mid_buf, w * 2, h * 2, final_buf);
        break;
    }
    return final_buf;
}

/* ------------------------------------------------------------------ */
/* Ajuste ao viewport (nearest / bilinear)                              */
/* ------------------------------------------------------------------ */

void vscale_map_build(VScaleMap* map, int src_w, int dst_w, bool smooth) {
    if (dst_w > VSCALE_MAX_DST_W) {
        dst_w = VSCALE_MAX_DST_W;
    }
    if (map->valid && map->src_w == src_w && map->dst_w == dst_w && map->smooth == smooth) {
        return;
    }
    for (int dx = 0; dx < dst_w; ++dx) {
        if (!smooth) {
            uint64_t sx = ((uint64_t)(2 * dx + 1) * (uint64_t)src_w) / (uint64_t)(2 * dst_w);
            if (sx >= (uint64_t)src_w) {
                sx = (uint64_t)src_w - 1;
            }
            map->x0[dx] = (uint16_t)sx;
            map->fx[dx] = 0;
        } else {
            int64_t pos = ((((int64_t)(2 * dx + 1) * src_w)) << 16) / (int64_t)(2 * dst_w) - 32768;
            int64_t max_pos = (int64_t)(src_w - 1) << 16;
            if (pos < 0) {
                pos = 0;
            }
            if (pos > max_pos) {
                pos = max_pos;
            }
            map->x0[dx] = (uint16_t)(pos >> 16);
            map->fx[dx] = (uint8_t)((pos >> 8) & 0xFF);
        }
    }
    map->src_w = src_w;
    map->dst_w = dst_w;
    map->smooth = smooth;
    map->valid = true;
}

/* lerp ARGB: (a*(256-f) + b*f) >> 8 por canal (SWAR); identico ao caminho vertical SIMD */
VS_INLINE u32 lerp_argb(u32 a, u32 b, u32 f) {
    u32 rb = (((a & 0x00FF00FFu) * (256u - f)) + ((b & 0x00FF00FFu) * f)) >> 8;
    u32 ag = (((a >> 8) & 0x00FF00FFu) * (256u - f)) + (((b >> 8) & 0x00FF00FFu) * f);
    return (rb & 0x00FF00FFu) | (ag & 0xFF00FF00u);
}

static void hrow_nearest(const u32* s, u32* d, const VScaleMap* m, int dw) {
    for (int dx = 0; dx < dw; ++dx) {
        d[dx] = s[m->x0[dx]];
    }
}

static void hrow_bilinear(const u32* s, u32* d, const VScaleMap* m, int dw) {
    const int max_x = m->src_w - 1;
    for (int dx = 0; dx < dw; ++dx) {
        int x0 = m->x0[dx];
        u32 f = m->fx[dx];
        if (f == 0 || x0 >= max_x) {
            d[dx] = s[x0];
        } else {
            d[dx] = lerp_argb(s[x0], s[x0 + 1], f);
        }
    }
}

static void copy_row(u32* dst, const u32* src, int n) {
    int x = 0;
    for (; x <= n - 8; x += 8) {
        __m128i a = _mm_loadu_si128((const __m128i*)&src[x]);
        __m128i b = _mm_loadu_si128((const __m128i*)&src[x + 4]);
        _mm_storeu_si128((__m128i*)&dst[x], a);
        _mm_storeu_si128((__m128i*)&dst[x + 4], b);
    }
    for (; x < n; ++x) {
        dst[x] = src[x];
    }
}

static void blend_rows(u32* dst, const u32* r0, const u32* r1, u32 fy, int n) {
    const __m128i wa = _mm_set1_epi16((short)(256u - fy));
    const __m128i wb = _mm_set1_epi16((short)fy);
    const __m128i z = _mm_setzero_si128();
    int x = 0;
    for (; x <= n - 4; x += 4) {
        __m128i a = _mm_loadu_si128((const __m128i*)&r0[x]);
        __m128i b = _mm_loadu_si128((const __m128i*)&r1[x]);
        __m128i lo = _mm_srli_epi16(_mm_add_epi16(
            _mm_mullo_epi16(_mm_unpacklo_epi8(a, z), wa),
            _mm_mullo_epi16(_mm_unpacklo_epi8(b, z), wb)), 8);
        __m128i hi = _mm_srli_epi16(_mm_add_epi16(
            _mm_mullo_epi16(_mm_unpackhi_epi8(a, z), wa),
            _mm_mullo_epi16(_mm_unpackhi_epi8(b, z), wb)), 8);
        _mm_storeu_si128((__m128i*)&dst[x], _mm_packus_epi16(lo, hi));
    }
    for (; x < n; ++x) {
        dst[x] = lerp_argb(r0[x], r1[x], fy);
    }
}

typedef struct RowCache {
    const u32* src;
    int sw;
    const VScaleMap* map;
    int dw;
    u32* buf[2];
    int idx[2];
} RowCache;

static const u32* cache_row(RowCache* c, int sy, int keep) {
    if (c->idx[0] == sy) {
        return c->buf[0];
    }
    if (c->idx[1] == sy) {
        return c->buf[1];
    }
    int slot = (c->idx[0] < c->idx[1]) ? 0 : 1;
    if (c->idx[slot] == keep) {
        slot ^= 1;
    }
    const u32* s = c->src + (size_t)sy * (size_t)c->sw;
    if (c->map->smooth) {
        hrow_bilinear(s, c->buf[slot], c->map, c->dw);
    } else {
        hrow_nearest(s, c->buf[slot], c->map, c->dw);
    }
    c->idx[slot] = sy;
    return c->buf[slot];
}

void vscale_fit_rows(const uint32_t* src, int sw, int sh,
                     uint32_t* dst, int dst_stride, int dw, int dh,
                     int row0, int row1, const VScaleMap* map,
                     uint32_t* scratch_a, uint32_t* scratch_b) {
    if (row0 < 0) {
        row0 = 0;
    }
    if (row1 > dh) {
        row1 = dh;
    }

    if (sw == dw && sh == dh) {
        for (int dy = row0; dy < row1; ++dy) {
            copy_row(dst + (size_t)dy * (size_t)dst_stride, src + (size_t)dy * (size_t)sw, dw);
        }
        return;
    }

    RowCache c;
    c.src = src;
    c.sw = sw;
    c.map = map;
    c.dw = dw;
    c.buf[0] = scratch_a;
    c.buf[1] = scratch_b;
    c.idx[0] = -1;
    c.idx[1] = -1;

    for (int dy = row0; dy < row1; ++dy) {
        u32* drow = dst + (size_t)dy * (size_t)dst_stride;

        if (!map->smooth) {
            int sy = (int)(((uint64_t)(2 * dy + 1) * (uint64_t)sh) / (uint64_t)(2 * dh));
            if (sy >= sh) {
                sy = sh - 1;
            }
            copy_row(drow, cache_row(&c, sy, -2), dw);
        } else {
            int64_t pos = (((int64_t)(2 * dy + 1) * sh) << 16) / (int64_t)(2 * dh) - 32768;
            int64_t max_pos = (int64_t)(sh - 1) << 16;
            if (pos < 0) {
                pos = 0;
            }
            if (pos > max_pos) {
                pos = max_pos;
            }
            int sy0 = (int)(pos >> 16);
            u32 fy = (u32)((pos >> 8) & 0xFF);
            int sy1 = (sy0 + 1 < sh) ? sy0 + 1 : sy0;

            const u32* r0 = cache_row(&c, sy0, -2);
            if (fy == 0 || sy1 == sy0) {
                copy_row(drow, r0, dw);
            } else {
                const u32* r1 = cache_row(&c, sy1, sy0);
                blend_rows(drow, r0, r1, fy, dw);
            }
        }
    }
}

