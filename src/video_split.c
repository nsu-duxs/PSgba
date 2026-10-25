#include "video_split.h"
#include "audio_ps4.h"
#include "ui_menu.h"
#include <string.h>
#include <time.h>
#include <orbis/libkernel.h>
#include <orbis/VideoOut.h>
#include <orbis/UserService.h>
#include <orbis/SystemService.h>

#define PS4_VIDEO_PIXEL_FORMAT_ARGB_SRGB 0x80000000U
#define FRAME_BYTES (PS4_SCREEN_WIDTH * PS4_SCREEN_HEIGHT * sizeof(uint32_t))
#define NUM_FRAMEBUFFERS 3

static off_t s_direct_mem_offset = 0;
static size_t s_total_alloc_size = 0;
static void* s_framebuffers[NUM_FRAMEBUFFERS] = { NULL, NULL, NULL };
static int32_t s_video_handle = -1;
static OrbisKernelEqueue s_flip_queue = 0;
static int s_buffer_index = 0;
static int64_t s_flip_arg = 1;
static int64_t s_submitted_flip_arg = 0;
static int s_clear_frames_remaining = NUM_FRAMEBUFFERS;
static int s_prev_active_count = -1;
static int s_prev_audio_focus = -1;
static bool s_prev_focus_highlight = false;
static bool s_prev_osd_open = false;
static bool s_prev_show_fps = true;
static UIAppState s_prev_ui_state = UI_STATE_LAUNCHER;
static PS4VideoResMode s_res_mode = PS4_RES_AUTO_HD;
static PS4VideoResMode s_prev_res_mode = PS4_RES_AUTO_HD;
static int s_fps_frame_count = 0;
static uint64_t s_fps_last_us = 0;
static int s_current_fps = 60;

/* Internal high-resolution upscaling surfaces (1x: 240x160, 2x HD: 480x320, 4x UHD: 960x640) */
static uint32_t s_buf_1x[GBA_SCREEN_WIDTH * GBA_SCREEN_HEIGHT];
static uint32_t s_buf_2x[(GBA_SCREEN_WIDTH * 2) * (GBA_SCREEN_HEIGHT * 2)];
static uint32_t s_buf_4x[(GBA_SCREEN_WIDTH * 4) * (GBA_SCREEN_HEIGHT * 4)];
static uint32_t s_horiz_row_a[PS4_SCREEN_WIDTH];
static uint32_t s_horiz_row_b[PS4_SCREEN_WIDTH];

/*
 * Converts 2 packed mGBA 32-bit XBGR8 pixels (0x00BBGGRR_00BBGGRR)
 * into 2 packed PS4 ARGB8 pixels (0xFFRRGGBB_FFRRGGBB) in 4 bitwise ops.
 */
static inline uint64_t xbgr2_to_argb2(uint64_t p01) {
    uint64_t ag = (p01 & 0x0000FF000000FF00ULL) | 0xFF000000FF000000ULL;
    uint64_t r  = (p01 & 0x000000FF000000FFULL) << 16;
    uint64_t b  = (p01 & 0x00FF000000FF0000ULL) >> 16;
    return ag | r | b;
}

static inline uint32_t blend_argb_50(uint32_t a, uint32_t b) {
    return 0xFF000000U | (((a & 0x00FEFEFEU) + (b & 0x00FEFEFEU)) >> 1);
}

static inline uint32_t blend_argb_75_25(uint32_t dom, uint32_t sub) {
    uint32_t mid = blend_argb_50(dom, sub);
    return blend_argb_50(dom, mid);
}

static inline void convert_xbgr_to_argb_1x(
    const uint32_t* __restrict src_xbgr,
    uint32_t* __restrict dst_argb
) {
    const uint64_t* __restrict s64 = (const uint64_t*)src_xbgr;
    uint64_t* __restrict d64 = (uint64_t*)dst_argb;
    const int pairs = (GBA_SCREEN_WIDTH * GBA_SCREEN_HEIGHT) / 2;
    for (int i = 0; i < pairs; ++i) {
        d64[i] = xbgr2_to_argb2(s64[i]);
    }
}

/*
 * HQ-EPX (Scale2x + Diagonal Subpixel Anti-Aliasing):
 * Doubles internal resolution (e.g. 240x160 -> 480x320, or 480x320 -> 960x640),
 * reconstructing diagonal edges and smoothing pixel steps while keeping text sharp.
 */
static void upscale_epx_aa_2x(
    const uint32_t* __restrict src,
    int src_w,
    int src_h,
    uint32_t* __restrict dst
) {
    const int dst_w = src_w * 2;

    for (int y = 0; y < src_h; ++y) {
        const uint32_t* row_u = (y > 0) ? &src[(y - 1) * src_w] : &src[y * src_w];
        const uint32_t* row_c = &src[y * src_w];
        const uint32_t* row_d = (y + 1 < src_h) ? &src[(y + 1) * src_w] : &src[y * src_w];

        uint32_t* dst_r0 = &dst[(y * 2) * dst_w];
        uint32_t* dst_r1 = &dst[(y * 2 + 1) * dst_w];

        for (int x = 0; x < src_w; ++x) {
            uint32_t p = row_c[x];
            uint32_t l = (x > 0) ? row_c[x - 1] : p;
            uint32_t r = (x + 1 < src_w) ? row_c[x + 1] : p;
            uint32_t u = row_u[x];
            uint32_t d = row_d[x];

            uint32_t e0 = p;
            uint32_t e1 = p;
            uint32_t e2 = p;
            uint32_t e3 = p;

            if (l != r && u != d) {
                if (l == u) {
                    e0 = blend_argb_75_25(u, p);
                }
                if (r == u) {
                    e1 = blend_argb_75_25(u, p);
                }
                if (l == d) {
                    e2 = blend_argb_75_25(d, p);
                }
                if (r == d) {
                    e3 = blend_argb_75_25(d, p);
                }
            }

            dst_r0[x * 2 + 0] = e0;
            dst_r0[x * 2 + 1] = e1;
            dst_r1[x * 2 + 0] = e2;
            dst_r1[x * 2 + 1] = e3;
        }
    }
}

static inline void resample_row_subpixel(
    const uint32_t* __restrict src_row,
    int src_w,
    uint32_t* __restrict dst_row,
    int dst_w
) {
    uint32_t step_fp = (uint32_t)(((uint64_t)src_w << 16) / (uint32_t)dst_w);
    uint32_t pos_fp = 0;
    int max_x = src_w - 1;

    for (int dx = 0; dx < dst_w; ++dx) {
        int sx0 = (int)(pos_fp >> 16);
        uint32_t frac = (pos_fp >> 14) & 3U;
        uint32_t c0 = src_row[sx0];
        if (frac >= 2U && sx0 < max_x) {
            uint32_t c1 = src_row[sx0 + 1];
            if (c0 != c1) {
                c0 = (frac == 2U) ? blend_argb_50(c0, c1) : blend_argb_75_25(c1, c0);
            }
        }
        dst_row[dx] = c0;
        pos_fp += step_fp;
    }
}

/*
 * Fast 64-bit SWAR Subpixel Blitter from an HD upscaled surface (480x320 or 960x640)
 * to any target viewport on the 1080p PS4 framebuffer.
 * Horizontally resamples each unique source row at most once, then writes 64-bit pairs.
 */
static void blit_hd_surface_wc64(
    uint32_t* __restrict dst_fb,
    const uint32_t* __restrict src_surf,
    int src_w,
    int src_h,
    int dst_x,
    int dst_y,
    int dst_w,
    int dst_h
) {
    if (src_w == dst_w && src_h == dst_h) {
        const int pairs = dst_w / 2;
        for (int y = 0; y < dst_h; ++y) {
            const uint64_t* __restrict s64 = (const uint64_t*)&src_surf[y * src_w];
            uint64_t* __restrict d64 = (uint64_t*)&dst_fb[(dst_y + y) * PS4_SCREEN_WIDTH + dst_x];
            for (int i = 0; i < pairs; ++i) {
                d64[i] = s64[i];
            }
        }
        return;
    }

    uint32_t step_y_fp = (uint32_t)(((uint64_t)src_h << 16) / (uint32_t)dst_h);
    uint32_t pos_y_fp = 0;
    int cached_sy_a = -1;
    int cached_sy_b = -1;
    const int pairs = dst_w / 2;

    for (int dy = 0; dy < dst_h; ++dy) {
        int sy0 = (int)(pos_y_fp >> 16);
        int sy1 = (sy0 + 1 < src_h) ? (sy0 + 1) : sy0;
        uint32_t frac_y = (pos_y_fp >> 14) & 3U;

        if (sy0 != cached_sy_a) {
            if (sy0 == cached_sy_b) {
                memcpy(s_horiz_row_a, s_horiz_row_b, (size_t)dst_w * sizeof(uint32_t));
                cached_sy_a = sy0;
            } else {
                resample_row_subpixel(&src_surf[sy0 * src_w], src_w, s_horiz_row_a, dst_w);
                cached_sy_a = sy0;
            }
        }

        uint64_t* __restrict d64 = (uint64_t*)&dst_fb[(dst_y + dy) * PS4_SCREEN_WIDTH + dst_x];
        const uint64_t* __restrict ra64 = (const uint64_t*)s_horiz_row_a;

        if (frac_y >= 2U && sy1 != sy0) {
            if (sy1 != cached_sy_b) {
                resample_row_subpixel(&src_surf[sy1 * src_w], src_w, s_horiz_row_b, dst_w);
                cached_sy_b = sy1;
            }
            const uint64_t* __restrict rb64 = (const uint64_t*)s_horiz_row_b;
            for (int i = 0; i < pairs; ++i) {
                uint64_t a = ra64[i];
                uint64_t b = rb64[i];
                d64[i] = 0xFF000000FF000000ULL |
                         (((a & 0x00FEFEFE00FEFEFEULL) + (b & 0x00FEFEFE00FEFEFEULL)) >> 1);
            }
        } else {
            for (int i = 0; i < pairs; ++i) {
                d64[i] = ra64[i];
            }
        }

        pos_y_fp += step_y_fp;
    }
}

/*
 * Fast 3x nearest-neighbor scaler (240x160 -> 720x480)
 * Packs 4 source pixels (p0..p3) into 6x uint64_t (12 pixels) per unrolled iteration,
 * writing directly to all 3 destination rows without reading back from Write-Combining memory.
 */
static inline void blit_3x_wc64(
    uint32_t* __restrict dst_fb,
    const uint32_t* __restrict src_buf,
    int dst_x_start,
    int dst_y_start
) {
    const uint32_t* src_row = src_buf;
    uint32_t* dst_base = &dst_fb[dst_y_start * PS4_SCREEN_WIDTH + dst_x_start];

    for (int sy = 0; sy < GBA_SCREEN_HEIGHT; ++sy) {
        uint64_t* __restrict d0 = (uint64_t*)(dst_base);
        uint64_t* __restrict d1 = (uint64_t*)(dst_base + PS4_SCREEN_WIDTH);
        uint64_t* __restrict d2 = (uint64_t*)(dst_base + PS4_SCREEN_WIDTH * 2);

        const uint64_t* __restrict s64 = (const uint64_t*)src_row;
        for (int i = 0; i < (GBA_SCREEN_WIDTH / 4); ++i) {
            uint64_t p01 = xbgr2_to_argb2(s64[0]);
            uint64_t p23 = xbgr2_to_argb2(s64[1]);
            s64 += 2;

            uint64_t p0 = (uint32_t)p01;
            uint64_t p1 = p01 >> 32;
            uint64_t p2 = (uint32_t)p23;
            uint64_t p3 = p23 >> 32;

            uint64_t w0 = (p0 << 32) | p0;
            uint64_t w1 = (p1 << 32) | p0;
            uint64_t w2 = (p1 << 32) | p1;
            uint64_t w3 = (p2 << 32) | p2;
            uint64_t w4 = (p3 << 32) | p2;
            uint64_t w5 = (p3 << 32) | p3;

            d0[0] = w0; d0[1] = w1; d0[2] = w2;
            d0[3] = w3; d0[4] = w4; d0[5] = w5;

            d1[0] = w0; d1[1] = w1; d1[2] = w2;
            d1[3] = w3; d1[4] = w4; d1[5] = w5;

            d2[0] = w0; d2[1] = w1; d2[2] = w2;
            d2[3] = w3; d2[4] = w4; d2[5] = w5;

            d0 += 6;
            d1 += 6;
            d2 += 6;
        }

        src_row += GBA_SCREEN_WIDTH;
        dst_base += PS4_SCREEN_WIDTH * 3;
    }
}

/*
 * Fast 4x nearest-neighbor scaler (240x160 -> 960x640)
 * Packs 4 source pixels into 8x uint64_t (16 pixels) per unrolled iteration.
 */
static inline void blit_4x_wc64(
    uint32_t* __restrict dst_fb,
    const uint32_t* __restrict src_buf,
    int dst_x_start,
    int dst_y_start
) {
    const uint32_t* src_row = src_buf;
    uint32_t* dst_base = &dst_fb[dst_y_start * PS4_SCREEN_WIDTH + dst_x_start];

    for (int sy = 0; sy < GBA_SCREEN_HEIGHT; ++sy) {
        uint64_t* __restrict d0 = (uint64_t*)(dst_base);
        uint64_t* __restrict d1 = (uint64_t*)(dst_base + PS4_SCREEN_WIDTH);
        uint64_t* __restrict d2 = (uint64_t*)(dst_base + PS4_SCREEN_WIDTH * 2);
        uint64_t* __restrict d3 = (uint64_t*)(dst_base + PS4_SCREEN_WIDTH * 3);

        const uint64_t* __restrict s64 = (const uint64_t*)src_row;
        for (int i = 0; i < (GBA_SCREEN_WIDTH / 4); ++i) {
            uint64_t p01 = xbgr2_to_argb2(s64[0]);
            uint64_t p23 = xbgr2_to_argb2(s64[1]);
            s64 += 2;

            uint64_t p0 = (uint32_t)p01;
            uint64_t p1 = p01 >> 32;
            uint64_t p2 = (uint32_t)p23;
            uint64_t p3 = p23 >> 32;

            uint64_t w0 = (p0 << 32) | p0;
            uint64_t w1 = (p1 << 32) | p1;
            uint64_t w2 = (p2 << 32) | p2;
            uint64_t w3 = (p3 << 32) | p3;

            d0[0] = w0; d0[1] = w0; d0[2] = w1; d0[3] = w1;
            d0[4] = w2; d0[5] = w2; d0[6] = w3; d0[7] = w3;

            d1[0] = w0; d1[1] = w0; d1[2] = w1; d1[3] = w1;
            d1[4] = w2; d1[5] = w2; d1[6] = w3; d1[7] = w3;

            d2[0] = w0; d2[1] = w0; d2[2] = w1; d2[3] = w1;
            d2[4] = w2; d2[5] = w2; d2[6] = w3; d2[7] = w3;

            d3[0] = w0; d3[1] = w0; d3[2] = w1; d3[3] = w1;
            d3[4] = w2; d3[5] = w2; d3[6] = w3; d3[7] = w3;

            d0 += 8;
            d1 += 8;
            d2 += 8;
            d3 += 8;
        }

        src_row += GBA_SCREEN_WIDTH;
        dst_base += PS4_SCREEN_WIDTH * 4;
    }
}

/*
 * Fast 6x nearest-neighbor scaler (240x160 -> 1440x960)
 * Packs 2 source pixels into 6x uint64_t (12 pixels) per unrolled iteration.
 */
static inline void blit_6x_wc64(
    uint32_t* __restrict dst_fb,
    const uint32_t* __restrict src_buf,
    int dst_x_start,
    int dst_y_start
) {
    const uint32_t* src_row = src_buf;
    uint32_t* dst_base = &dst_fb[dst_y_start * PS4_SCREEN_WIDTH + dst_x_start];

    for (int sy = 0; sy < GBA_SCREEN_HEIGHT; ++sy) {
        uint64_t* __restrict d0 = (uint64_t*)(dst_base);
        uint64_t* __restrict d1 = (uint64_t*)(dst_base + PS4_SCREEN_WIDTH);
        uint64_t* __restrict d2 = (uint64_t*)(dst_base + PS4_SCREEN_WIDTH * 2);
        uint64_t* __restrict d3 = (uint64_t*)(dst_base + PS4_SCREEN_WIDTH * 3);
        uint64_t* __restrict d4 = (uint64_t*)(dst_base + PS4_SCREEN_WIDTH * 4);
        uint64_t* __restrict d5 = (uint64_t*)(dst_base + PS4_SCREEN_WIDTH * 5);

        const uint64_t* __restrict s64 = (const uint64_t*)src_row;
        for (int i = 0; i < (GBA_SCREEN_WIDTH / 2); ++i) {
            uint64_t p01 = xbgr2_to_argb2(s64[0]);
            s64 += 1;

            uint64_t p0 = (uint32_t)p01;
            uint64_t p1 = p01 >> 32;

            uint64_t w0 = (p0 << 32) | p0;
            uint64_t w1 = (p1 << 32) | p1;

            d0[0] = w0; d0[1] = w0; d0[2] = w0;
            d0[3] = w1; d0[4] = w1; d0[5] = w1;

            d1[0] = w0; d1[1] = w0; d1[2] = w0;
            d1[3] = w1; d1[4] = w1; d1[5] = w1;

            d2[0] = w0; d2[1] = w0; d2[2] = w0;
            d2[3] = w1; d2[4] = w1; d2[5] = w1;

            d3[0] = w0; d3[1] = w0; d3[2] = w0;
            d3[3] = w1; d3[4] = w1; d3[5] = w1;

            d4[0] = w0; d4[1] = w0; d4[2] = w0;
            d4[3] = w1; d4[4] = w1; d4[5] = w1;

            d5[0] = w0; d5[1] = w0; d5[2] = w0;
            d5[3] = w1; d5[4] = w1; d5[5] = w1;

            d0 += 6;
            d1 += 6;
            d2 += 6;
            d3 += 6;
            d4 += 6;
            d5 += 6;
        }

        src_row += GBA_SCREEN_WIDTH;
        dst_base += PS4_SCREEN_WIDTH * 6;
    }
}

/*
 * Borda interna (inset) fina e nitida contornando o viewport ativo de cada jogador.
 * Ao desenhar dentro dos limites [x, x+w) e [y, y+h), nenhuma borda e cortada,
 * mesmo em resolucao cheia 1080p ou nas extremidades da tela.
 */
static void draw_viewport_border(
    uint32_t* fb,
    int x,
    int y,
    int w,
    int h,
    int thickness,
    uint32_t color
) {
    if (!fb || w <= 0 || h <= 0 || thickness <= 0) {
        return;
    }
    if (thickness * 2 > w) {
        thickness = w / 2;
    }
    if (thickness * 2 > h) {
        thickness = h / 2;
    }

    for (int t = 0; t < thickness; ++t) {
        int y_top = y + t;
        int y_bot = y + h - 1 - t;
        int x_left = x + t;
        int x_right = x + w - 1 - t;

        int x_start = (x >= 0) ? x : 0;
        int x_end = (x + w <= PS4_SCREEN_WIDTH) ? (x + w) : PS4_SCREEN_WIDTH;
        int y_start = (y >= 0) ? y : 0;
        int y_end = (y + h <= PS4_SCREEN_HEIGHT) ? (y + h) : PS4_SCREEN_HEIGHT;

        if (y_top >= 0 && y_top < PS4_SCREEN_HEIGHT) {
            uint32_t* row = &fb[y_top * PS4_SCREEN_WIDTH];
            for (int px = x_start; px < x_end; ++px) {
                row[px] = color;
            }
        }
        if (y_bot >= 0 && y_bot < PS4_SCREEN_HEIGHT && y_bot != y_top) {
            uint32_t* row = &fb[y_bot * PS4_SCREEN_WIDTH];
            for (int px = x_start; px < x_end; ++px) {
                row[px] = color;
            }
        }
        if (x_left >= 0 && x_left < PS4_SCREEN_WIDTH) {
            for (int py = y_start; py < y_end; ++py) {
                fb[py * PS4_SCREEN_WIDTH + x_left] = color;
            }
        }
        if (x_right >= 0 && x_right < PS4_SCREEN_WIDTH && x_right != x_left) {
            for (int py = y_start; py < y_end; ++py) {
                fb[py * PS4_SCREEN_WIDTH + x_right] = color;
            }
        }
    }
}

static void wait_for_flip(int64_t flip_arg) {
    if (s_video_handle < 0 || !s_flip_queue) {
        return;
    }

    OrbisKernelEvent evt;
    int count = 0;
    OrbisVideoOutFlipStatus status;

    for (;;) {
        memset(&status, 0, sizeof(status));
        if (sceVideoOutGetFlipStatus(s_video_handle, &status) >= 0 &&
            status.flipArg >= flip_arg) {
            break;
        }
        if (sceKernelWaitEqueue(s_flip_queue, &evt, 1, &count, NULL) != 0) {
            break;
        }
    }
}

bool video_init(void) {
    sceSystemServiceHideSplashScreen();

    s_video_handle = sceVideoOutOpen(ORBIS_VIDEO_USER_MAIN,
                                     ORBIS_VIDEO_OUT_BUS_MAIN, 0, NULL);
    if (s_video_handle < 0) {
        return false;
    }

    if (sceKernelCreateEqueue(&s_flip_queue, "gba4p_flip_queue") < 0) {
        video_shutdown();
        return false;
    }
    sceVideoOutAddFlipEvent(s_flip_queue, s_video_handle, NULL);

    size_t total_size = FRAME_BYTES * NUM_FRAMEBUFFERS;
    total_size = (total_size + 0x1FFFFF) & ~((size_t)0x1FFFFF);
    s_total_alloc_size = total_size;

    int ret = sceKernelAllocateDirectMemory(0, sceKernelGetDirectMemorySize(),
                                            total_size, 0x200000, 3,
                                            &s_direct_mem_offset);
    if (ret < 0) {
        video_shutdown();
        return false;
    }

    void* base_addr = NULL;
    ret = sceKernelMapDirectMemory(&base_addr, total_size, 0x33, 0,
                                   s_direct_mem_offset, 0x200000);
    if (ret < 0 || !base_addr) {
        sceKernelReleaseDirectMemory(s_direct_mem_offset, s_total_alloc_size);
        s_direct_mem_offset = 0;
        s_total_alloc_size = 0;
        video_shutdown();
        return false;
    }

    for (int i = 0; i < NUM_FRAMEBUFFERS; ++i) {
        s_framebuffers[i] = (void*)((uintptr_t)base_addr + (size_t)i * FRAME_BYTES);
    }
    memset(s_framebuffers[0], 0, total_size);

    OrbisVideoOutBufferAttribute attr;
    memset(&attr, 0, sizeof(attr));
    /* Tiling: 1 = Linear (0 = Tile), Aspect: 0 = 16:9 */
    sceVideoOutSetBufferAttribute(
        &attr,
        PS4_VIDEO_PIXEL_FORMAT_ARGB_SRGB,
        1,
        0,
        PS4_SCREEN_WIDTH,
        PS4_SCREEN_HEIGHT,
        PS4_SCREEN_WIDTH
    );

    ret = sceVideoOutRegisterBuffers(s_video_handle, 0, s_framebuffers, NUM_FRAMEBUFFERS, &attr);
    if (ret < 0) {
        video_shutdown();
        return false;
    }

    /* Flip rate: 0 = 60Hz */
    sceVideoOutSetFlipRate(s_video_handle, 0);

    s_buffer_index = 0;
    s_flip_arg = 1;
    s_submitted_flip_arg = 0;
    s_clear_frames_remaining = NUM_FRAMEBUFFERS;
    return true;
}

void video_set_res_mode(PS4VideoResMode mode) {
    if (mode >= 0 && mode < PS4_RES_MODE_COUNT) {
        s_res_mode = mode;
        s_clear_frames_remaining = NUM_FRAMEBUFFERS;
    }
}

PS4VideoResMode video_get_res_mode(void) {
    return s_res_mode;
}

void video_cycle_res_mode(int delta) {
    int next = ((int)s_res_mode + delta) % (int)PS4_RES_MODE_COUNT;
    if (next < 0) {
        next += (int)PS4_RES_MODE_COUNT;
    }
    video_set_res_mode((PS4VideoResMode)next);
}

const char* video_get_res_mode_name(PS4VideoResMode mode) {
    switch (mode) {
    case PS4_RES_AUTO_HD:
        return "Auto HD (4x Ultra HD em 1P/2P, 2x HD em 3P/4P)";
    case PS4_RES_4X_UHD:
        return "4x Ultra HD (960x640 Scale4x + Suavizacao)";
    case PS4_RES_2X_HD:
        return "2x HD (480x320 Scale2x + Suavizacao)";
    case PS4_RES_FULL_16_9:
        return "Tela Cheia 16:9 HD (1080p / Quadrante 100%)";
    case PS4_RES_1X_NATIVE:
    default:
        return "1x Original (240x160 Pixel Puro)";
    }
}

static void render_instance_hd(
    uint32_t* __restrict back_buffer,
    const uint32_t* __restrict src_xbgr,
    int internal_scale, /* 2 = 480x320, 4 = 960x640 */
    int dst_x,
    int dst_y,
    int dst_w,
    int dst_h
) {
    convert_xbgr_to_argb_1x(src_xbgr, s_buf_1x);
    upscale_epx_aa_2x(s_buf_1x, GBA_SCREEN_WIDTH, GBA_SCREEN_HEIGHT, s_buf_2x);

    if (internal_scale >= 4) {
        upscale_epx_aa_2x(s_buf_2x, GBA_SCREEN_WIDTH * 2, GBA_SCREEN_HEIGHT * 2, s_buf_4x);
        blit_hd_surface_wc64(back_buffer, s_buf_4x,
                             GBA_SCREEN_WIDTH * 4, GBA_SCREEN_HEIGHT * 4,
                             dst_x, dst_y, dst_w, dst_h);
    } else {
        blit_hd_surface_wc64(back_buffer, s_buf_2x,
                             GBA_SCREEN_WIDTH * 2, GBA_SCREEN_HEIGHT * 2,
                             dst_x, dst_y, dst_w, dst_h);
    }
}

static void update_fps_counter(void) {
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) {
        return;
    }
    uint64_t now_us = (uint64_t)ts.tv_sec * 1000000ULL + (uint64_t)(ts.tv_nsec / 1000L);
    if (s_fps_last_us == 0) {
        s_fps_last_us = now_us;
        s_fps_frame_count = 0;
        return;
    }
    s_fps_frame_count++;
    uint64_t elapsed_us = now_us - s_fps_last_us;
    if (elapsed_us >= 250000ULL) {
        s_current_fps = (int)((s_fps_frame_count * 1000000ULL + (elapsed_us / 2ULL)) / elapsed_us);
        if (s_current_fps > 60) {
            s_current_fps = 60;
        }
        s_fps_frame_count = 0;
        s_fps_last_us = now_us;
    }
}

void video_request_clear_framebuffers(void) {
    s_clear_frames_remaining = NUM_FRAMEBUFFERS;
}

void video_render_split_screen(struct PS4GBA_Context* ctx) {
    uint32_t* back_buffer = (uint32_t*)s_framebuffers[s_buffer_index];
    if (!back_buffer) {
        return;
    }

    update_fps_counter();

    UIAppState ui_state = ui_menu_get_state();
    if (ui_state == UI_STATE_LAUNCHER) {
        s_prev_ui_state = ui_state;
        s_clear_frames_remaining = NUM_FRAMEBUFFERS;
        ui_menu_render_launcher(back_buffer);
        if (s_submitted_flip_arg > 0) {
            wait_for_flip(s_submitted_flip_arg);
        }
        int64_t flip_id = s_flip_arg++;
        sceVideoOutSubmitFlip(s_video_handle, s_buffer_index,
                              ORBIS_VIDEO_OUT_FLIP_VSYNC, flip_id);
        s_submitted_flip_arg = flip_id;
        s_buffer_index = (s_buffer_index + 1) % NUM_FRAMEBUFFERS;
        return;
    }

    int active_count = ctx ? ctx->active_count : 0;
    int audio_focus = audio_get_focus_player();
    bool focus_highlight = (audio_get_mode() == PS4_AUDIO_MODE_FOCUS);
    bool osd_open = ui_menu_is_osd_open();
    bool show_fps = ui_menu_is_fps_visible();

    if (s_prev_ui_state != ui_state ||
        s_prev_active_count != active_count ||
        s_prev_audio_focus != audio_focus ||
        s_prev_focus_highlight != focus_highlight ||
        s_prev_osd_open != osd_open ||
        s_prev_show_fps != show_fps ||
        s_prev_res_mode != s_res_mode) {
        s_clear_frames_remaining = NUM_FRAMEBUFFERS;
        s_prev_ui_state = ui_state;
        s_prev_active_count = active_count;
        s_prev_audio_focus = audio_focus;
        s_prev_focus_highlight = focus_highlight;
        s_prev_osd_open = osd_open;
        s_prev_show_fps = show_fps;
        s_prev_res_mode = s_res_mode;
    }

    if (osd_open || s_clear_frames_remaining > 0) {
        memset(back_buffer, 0, FRAME_BYTES);
        if (!osd_open && s_clear_frames_remaining > 0) {
            s_clear_frames_remaining--;
        }
    }

    /* Cores correspondentes aos 4 controles DualShock 4 (P1: Azul, P2: Vermelho, P3: Verde, P4: Rosa) */
    static const uint32_t player_colors[GBA_MAX_INSTANCES] = {
        0xFF0066FFU, /* P1: Azul (Controle 1) */
        0xFFFF1744U, /* P2: Vermelho (Controle 2) */
        0xFF00E676U, /* P3: Verde (Controle 3) */
        0xFFFF1493U  /* P4: Rosa (Controle 4) */
    };

    if (s_res_mode == PS4_RES_1X_NATIVE) {
        if (active_count == 1) {
            if (ctx->instances[0].active && ctx->instances[0].video_buffer) {
                blit_6x_wc64(back_buffer, ctx->instances[0].video_buffer, 240, 60);
                int border_thick = (focus_highlight && audio_focus == 0) ? 4 : 2;
                draw_viewport_border(back_buffer, 240, 60, 1440, 960, border_thick, player_colors[0]);
            }
        } else if (active_count == 2) {
            static const int p2_x[2] = { 0, 960 };
            int view_idx = 0;
            for (int i = 0; i < GBA_MAX_INSTANCES && view_idx < 2; ++i) {
                if (ctx->instances[i].active && ctx->instances[i].video_buffer) {
                    blit_4x_wc64(back_buffer, ctx->instances[i].video_buffer, p2_x[view_idx], 220);
                    int border_thick = (focus_highlight && audio_focus == i) ? 4 : 2;
                    draw_viewport_border(back_buffer, p2_x[view_idx], 220, 960, 640, border_thick, player_colors[i]);
                    view_idx++;
                }
            }
        } else {
            static const struct {
                int x;
                int y;
            } quadrant_origins[GBA_MAX_INSTANCES] = {
                { 120,  30 },
                { 1080, 30 },
                { 120,  570 },
                { 1080, 570 }
            };
            for (int i = 0; i < GBA_MAX_INSTANCES; ++i) {
                if (ctx && ctx->instances[i].active && ctx->instances[i].video_buffer) {
                    blit_3x_wc64(back_buffer, ctx->instances[i].video_buffer,
                                 quadrant_origins[i].x, quadrant_origins[i].y);
                    int border_thick = (focus_highlight && audio_focus == i) ? 4 : 2;
                    draw_viewport_border(back_buffer,
                                         quadrant_origins[i].x, quadrant_origins[i].y,
                                         720, 480, border_thick, player_colors[i]);
                }
            }
        }
    } else {
        bool full_16_9 = (s_res_mode == PS4_RES_FULL_16_9);

        if (active_count == 1) {
            /* 1P: 4x Ultra HD (960x640 internal) -> 1620x1080 (3:2 Max HD) or 1920x1080 (16:9 Full) */
            int internal_scale = (s_res_mode == PS4_RES_2X_HD) ? 2 : 4;
            int dst_w = full_16_9 ? 1920 : 1620;
            int dst_h = 1080;
            int dst_x = (PS4_SCREEN_WIDTH - dst_w) / 2;
            if (ctx->instances[0].active && ctx->instances[0].video_buffer) {
                render_instance_hd(back_buffer, ctx->instances[0].video_buffer,
                                   internal_scale, dst_x, 0, dst_w, dst_h);
                int border_thick = (focus_highlight && audio_focus == 0) ? 4 : 2;
                draw_viewport_border(back_buffer, dst_x, 0, dst_w, dst_h, border_thick, player_colors[0]);
            }
        } else if (active_count == 2) {
            /* 2P: 4x Ultra HD (960x640 internal) -> 960x640 (3:2) or 960x720 (16:9) */
            int internal_scale = (s_res_mode == PS4_RES_2X_HD) ? 2 : 4;
            int dst_w = 960;
            int dst_h = full_16_9 ? 720 : 640;
            int dst_y = (PS4_SCREEN_HEIGHT - dst_h) / 2;
            static const int p2_x[2] = { 0, 960 };
            int view_idx = 0;
            for (int i = 0; i < GBA_MAX_INSTANCES && view_idx < 2; ++i) {
                if (ctx->instances[i].active && ctx->instances[i].video_buffer) {
                    render_instance_hd(back_buffer, ctx->instances[i].video_buffer,
                                       internal_scale, p2_x[view_idx], dst_y, dst_w, dst_h);
                    int border_thick = (focus_highlight && audio_focus == i) ? 4 : 2;
                    draw_viewport_border(back_buffer, p2_x[view_idx], dst_y, dst_w, dst_h, border_thick, player_colors[i]);
                    view_idx++;
                }
            }
        } else {
            /* 3P/4P: 2x HD (480x320) or 4x UHD -> 804x532 (3:2 Max) or 960x540 (16:9 Full Quadrant) */
            int internal_scale = (s_res_mode == PS4_RES_4X_UHD) ? 4 : 2;
            int dst_w = full_16_9 ? 960 : 804;
            int dst_h = full_16_9 ? 540 : 532;
            int pad_x = (960 - dst_w) / 2;
            int pad_y = (540 - dst_h) / 2;
            const int quad_x[GBA_MAX_INSTANCES] = {
                pad_x, 960 + pad_x, pad_x, 960 + pad_x
            };
            const int quad_y[GBA_MAX_INSTANCES] = {
                pad_y, pad_y, 540 + pad_y, 540 + pad_y
            };

            for (int i = 0; i < GBA_MAX_INSTANCES; ++i) {
                if (ctx && ctx->instances[i].active && ctx->instances[i].video_buffer) {
                    render_instance_hd(back_buffer, ctx->instances[i].video_buffer,
                                       internal_scale, quad_x[i], quad_y[i], dst_w, dst_h);
                    int border_thick = (focus_highlight && audio_focus == i) ? 4 : 2;
                    draw_viewport_border(back_buffer,
                                         quad_x[i], quad_y[i],
                                         dst_w, dst_h, border_thick, player_colors[i]);
                }
            }
        }
    }

    if (show_fps) {
        ui_menu_render_fps_overlay(back_buffer, s_current_fps);
    }

    if (ui_state == UI_STATE_INGAME && !osd_open) {
        ui_menu_render_join_hint(back_buffer, ctx);
    }

    if (osd_open) {
        ui_menu_render_osd(back_buffer, ctx);
    }

    /* Wait for previous VSync flip before submitting current buffer so CPU emulation overlaps VSync wait */
    if (s_submitted_flip_arg > 0) {
        wait_for_flip(s_submitted_flip_arg);
    }
    int64_t flip_id = s_flip_arg++;
    sceVideoOutSubmitFlip(s_video_handle, s_buffer_index,
                          ORBIS_VIDEO_OUT_FLIP_VSYNC, flip_id);
    s_submitted_flip_arg = flip_id;
    s_buffer_index = (s_buffer_index + 1) % NUM_FRAMEBUFFERS;
}

void video_shutdown(void) {
    if (s_flip_queue) {
        sceKernelDeleteEqueue(s_flip_queue);
        s_flip_queue = 0;
    }
    if (s_video_handle >= 0) {
        sceVideoOutClose(s_video_handle);
        s_video_handle = -1;
    }
    if (s_framebuffers[0]) {
        sceKernelReleaseDirectMemory(s_direct_mem_offset, s_total_alloc_size);
        for (int i = 0; i < NUM_FRAMEBUFFERS; ++i) {
            s_framebuffers[i] = NULL;
        }
        s_direct_mem_offset = 0;
        s_total_alloc_size = 0;
    }
}
