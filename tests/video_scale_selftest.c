/*
 * Self-check do pipeline SIMD (host, sem Orbis):
 *   clang -O2 -msse4.1 -mssse3 tests/video_scale_selftest.c -o vs_test && ./vs_test
 * Compara cada caminho SIMD com a referencia escalar (mesmas primitivas px_*).
 */
#include "../src/video_scale.c"
#include <stdio.h>
#include <stdlib.h>

static u32 rng_state = 12345u;
static u32 rnd(void) {
    rng_state = rng_state * 1664525u + 1013904223u;
    return rng_state >> 8;
}

static void make_image(u32* img, int w, int h) {
    /* paleta curta (cores proximas incluidas) para forcar igualdades e tolerancias */
    static const u32 pal[6] = {
        0xFF101010u, 0xFF181818u, 0xFFE02020u, 0xFFD82818u, 0xFF2040F0u, 0xFFF0F0F0u
    };
    for (int i = 0; i < w * h; ++i) {
        img[i] = pal[rnd() % 6u];
    }
}

static int check_edge(int m, int k) {
    const int w = VSCALE_SRC_W, h = VSCALE_SRC_H;
    static u32 src[VSCALE_SRC_W * VSCALE_SRC_H];
    static u32 got[VSCALE_SRC_W * 3 * VSCALE_SRC_H * 3];
    static u32 ref[VSCALE_SRC_W * 3 * VSCALE_SRC_H * 3];
    make_image(src, w, h);

    if (k == 2) {
        k_edge2x[m](src, w, h, got);
        for (int y = 0; y < h; ++y) {
            const u32* rc = src + y * w;
            const u32* ru = y > 0 ? rc - w : rc;
            const u32* rd = y < h - 1 ? rc + w : rc;
            u32* d0 = ref + (2 * y) * (2 * w);
            for (int x = 0; x < w; ++x) {
                edge2x_px(m, ru, rc, rd, x, w, d0, d0 + 2 * w);
            }
        }
    } else {
        k_edge3x[m](src, w, h, got);
        for (int y = 0; y < h; ++y) {
            const u32* rc = src + y * w;
            const u32* ru = y > 0 ? rc - w : rc;
            const u32* rd = y < h - 1 ? rc + w : rc;
            u32* d0 = ref + (3 * y) * (3 * w);
            for (int x = 0; x < w; ++x) {
                edge3x_px(m, ru, rc, rd, x, w, d0, d0 + 3 * w, d0 + 6 * w);
            }
        }
    }
    return memcmp(got, ref, (size_t)w * k * h * k * sizeof(u32)) == 0;
}

static int check_nearest(int k) {
    static u32 src[VSCALE_SRC_W * VSCALE_SRC_H];
    static u32 got[VSCALE_SRC_W * 6 * VSCALE_SRC_H * 6];
    make_image(src, VSCALE_SRC_W, VSCALE_SRC_H);
    vscale_nearest(src, VSCALE_SRC_W, VSCALE_SRC_H, got, k);
    for (int y = 0; y < VSCALE_SRC_H * k; ++y) {
        for (int x = 0; x < VSCALE_SRC_W * k; ++x) {
            if (got[y * VSCALE_SRC_W * k + x] != src[(y / k) * VSCALE_SRC_W + x / k]) {
                return 0;
            }
        }
    }
    return 1;
}

static int check_fit(int sw, int sh, int dw, int dh, bool smooth) {
    static u32 src[VSCALE_SRC_W * 6 * VSCALE_SRC_H * 6];
    static u32 got[1920 * 1080];
    static u32 sa[1920], sb[1920];
    static VScaleMap map;
    map.valid = false;
    make_image(src, sw, sh);
    vscale_map_build(&map, sw, dw, smooth);

    /* duas bandas disjuntas = mesmo resultado que uma passada */
    vscale_fit_rows(src, sw, sh, got, 1920, dw, dh, 0, dh / 2, &map, sa, sb);
    vscale_fit_rows(src, sw, sh, got, 1920, dw, dh, dh / 2, dh, &map, sa, sb);

    for (int dy = 0; dy < dh; ++dy) {
        for (int dx = 0; dx < dw; ++dx) {
            u32 want;
            if (!smooth) {
                int sx = (int)(((uint64_t)(2 * dx + 1) * sw) / (2 * dw));
                int sy = (int)(((uint64_t)(2 * dy + 1) * sh) / (2 * dh));
                want = src[sy * sw + sx];
            } else {
                int64_t px = (((int64_t)(2 * dx + 1) * sw) << 16) / (2 * dw) - 32768;
                int64_t py = (((int64_t)(2 * dy + 1) * sh) << 16) / (2 * dh) - 32768;
                if (px < 0) px = 0;
                if (py < 0) py = 0;
                if (px > (int64_t)(sw - 1) << 16) px = (int64_t)(sw - 1) << 16;
                if (py > (int64_t)(sh - 1) << 16) py = (int64_t)(sh - 1) << 16;
                int x0 = (int)(px >> 16), y0 = (int)(py >> 16);
                int x1 = x0 + 1 < sw ? x0 + 1 : x0, y1 = y0 + 1 < sh ? y0 + 1 : y0;
                u32 fx = (u32)((px >> 8) & 255), fy = (u32)((py >> 8) & 255);
                u32 top = (fx && x1 != x0) ? lerp_argb(src[y0 * sw + x0], src[y0 * sw + x1], fx) : src[y0 * sw + x0];
                u32 bot = (fx && x1 != x0) ? lerp_argb(src[y1 * sw + x0], src[y1 * sw + x1], fx) : src[y1 * sw + x0];
                want = (fy && y1 != y0) ? lerp_argb(top, bot, fy) : top;
            }
            if (got[dy * 1920 + dx] != want) {
                printf("fit mismatch %dx%d->%dx%d smooth=%d at (%d,%d): %08X != %08X\n",
                       sw, sh, dw, dh, smooth, dx, dy, got[dy * 1920 + dx], want);
                return 0;
            }
        }
    }
    return 1;
}

int main(void) {
    int fails = 0;
    for (int m = 0; m < 3; ++m) {
        for (int k = 2; k <= 3; ++k) {
            int ok = check_edge(m, k);
            printf("edge%dx mode=%d: %s\n", k, m, ok ? "OK" : "FAIL");
            fails += !ok;
        }
    }
    static const int ks[4] = { 2, 3, 4, 6 };
    for (int i = 0; i < 4; ++i) {
        int ok = check_nearest(ks[i]);
        printf("nearest k=%d: %s\n", ks[i], ok ? "OK" : "FAIL");
        fails += !ok;
    }
    for (int s = 0; s < 2; ++s) {
        fails += !check_fit(960, 640, 1620, 1080, s);
        fails += !check_fit(960, 640, 810, 540, s);
        fails += !check_fit(480, 320, 960, 640, s);
        fails += !check_fit(240, 160, 810, 540, s);
        fails += !check_fit(1440, 960, 1620, 1080, s);
    }
    printf(fails ? "FAILED (%d)\n" : "ALL OK\n", fails);
    return fails != 0;
}

