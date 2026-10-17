#include "video_split.h"
#include "audio_ps4.h"
#include "ui_menu.h"
#include <string.h>
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
static UIAppState s_prev_ui_state = UI_STATE_LAUNCHER;

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

static void draw_viewport_border(
    uint32_t* fb,
    int x,
    int y,
    int w,
    int h,
    int thickness,
    uint32_t color
) {
    for (int t = 0; t < thickness; ++t) {
        int top_y = y - thickness + t;
        int bot_y = y + h + t;
        int left_x = x - thickness;
        int span_w = w + thickness * 2;

        if (top_y >= 0 && top_y < PS4_SCREEN_HEIGHT) {
            for (int px = left_x; px < left_x + span_w; ++px) {
                if (px >= 0 && px < PS4_SCREEN_WIDTH) {
                    fb[top_y * PS4_SCREEN_WIDTH + px] = color;
                }
            }
        }
        if (bot_y >= 0 && bot_y < PS4_SCREEN_HEIGHT) {
            for (int px = left_x; px < left_x + span_w; ++px) {
                if (px >= 0 && px < PS4_SCREEN_WIDTH) {
                    fb[bot_y * PS4_SCREEN_WIDTH + px] = color;
                }
            }
        }
    }

    for (int py = y; py < y + h; ++py) {
        if (py < 0 || py >= PS4_SCREEN_HEIGHT) {
            continue;
        }
        for (int t = 0; t < thickness; ++t) {
            int lx = x - thickness + t;
            int rx = x + w + t;
            if (lx >= 0 && lx < PS4_SCREEN_WIDTH) {
                fb[py * PS4_SCREEN_WIDTH + lx] = color;
            }
            if (rx >= 0 && rx < PS4_SCREEN_WIDTH) {
                fb[py * PS4_SCREEN_WIDTH + rx] = color;
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

void video_render_split_screen(struct PS4GBA_Context* ctx) {
    uint32_t* back_buffer = (uint32_t*)s_framebuffers[s_buffer_index];
    if (!back_buffer) {
        return;
    }

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

    if (s_prev_ui_state != ui_state ||
        s_prev_active_count != active_count ||
        s_prev_audio_focus != audio_focus ||
        s_prev_focus_highlight != focus_highlight ||
        s_prev_osd_open != osd_open) {
        s_clear_frames_remaining = NUM_FRAMEBUFFERS;
        s_prev_ui_state = ui_state;
        s_prev_active_count = active_count;
        s_prev_audio_focus = audio_focus;
        s_prev_focus_highlight = focus_highlight;
        s_prev_osd_open = osd_open;
    }

    if (osd_open || s_clear_frames_remaining > 0) {
        memset(back_buffer, 0, FRAME_BYTES);
        if (!osd_open && s_clear_frames_remaining > 0) {
            s_clear_frames_remaining--;
        }
    }

    static const uint32_t player_colors[GBA_MAX_INSTANCES] = {
        0xFF00F5D4U, /* P1: Cyan   */
        0xFFFFBE0BU, /* P2: Gold   */
        0xFFFF006EU, /* P3: Pink   */
        0xFF38B000U  /* P4: Green  */
    };

    if (active_count == 1) {
        if (ctx->instances[0].active && ctx->instances[0].video_buffer) {
            blit_6x_wc64(back_buffer, ctx->instances[0].video_buffer, 240, 60);
        }
    } else if (active_count == 2) {
        static const int p2_x[2] = { 0, 960 };
        for (int i = 0; i < 2; ++i) {
            if (ctx->instances[i].active && ctx->instances[i].video_buffer) {
                blit_4x_wc64(back_buffer, ctx->instances[i].video_buffer, p2_x[i], 220);
                if (focus_highlight && audio_focus == i) {
                    draw_viewport_border(back_buffer, p2_x[i], 220, 960, 640, 3, player_colors[i]);
                }
            }
        }
    } else {
        static const struct {
            int x;
            int y;
        } quadrant_origins[GBA_MAX_INSTANCES] = {
            { 120,  30 },   /* Quadrant 0: Top-Left     */
            { 1080, 30 },   /* Quadrant 1: Top-Right    */
            { 120,  570 },  /* Quadrant 2: Bottom-Left  */
            { 1080, 570 }   /* Quadrant 3: Bottom-Right */
        };

        for (int i = 0; i < GBA_MAX_INSTANCES; ++i) {
            if (ctx && ctx->instances[i].active && ctx->instances[i].video_buffer) {
                blit_3x_wc64(back_buffer, ctx->instances[i].video_buffer,
                             quadrant_origins[i].x, quadrant_origins[i].y);
                if (focus_highlight && audio_focus == i) {
                    draw_viewport_border(back_buffer,
                                         quadrant_origins[i].x, quadrant_origins[i].y,
                                         720, 480, 3, player_colors[i]);
                }
            }
        }
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
