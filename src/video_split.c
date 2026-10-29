#include "video_split.h"
#include "video_scale.h"
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

/* Orcamento de frame (emulacao + video) acima do qual a qualidade cai automaticamente */
#define AUTO_BUDGET_US        14000ULL
#define AUTO_RECOVER_US       8000ULL
#define AUTO_DEGRADE_FRAMES   30
#define AUTO_RECOVER_FRAMES   600
#define AUTO_MAX_LEVEL        3

_Static_assert(GBA_SCREEN_WIDTH == VSCALE_SRC_W && GBA_SCREEN_HEIGHT == VSCALE_SRC_H,
               "resolucao GBA diverge do pipeline de upscale");
_Static_assert(PS4_SCREEN_WIDTH <= VSCALE_MAX_DST_W, "VSCALE_MAX_DST_W menor que o framebuffer");

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
static PS4VideoDisplay s_prev_display = PS4_DISPLAY_ASPECT_3_2;

static PS4VideoFilter s_filter = PS4_FILTER_SCALENX;
static PS4VideoScale s_scale = PS4_SCALE_4X;
static bool s_smooth = true;
static PS4VideoDisplay s_display = PS4_DISPLAY_ASPECT_3_2;

static PS4FpsLimit s_fps_limit = PS4_FPS_60;
static bool s_skip_frame_render = false;
static uint64_t s_frame_deadline_us = 0;
static uint64_t s_frame_start_us = 0;
static int s_fps_frame_count = 0;
static uint64_t s_fps_last_us = 0;
static int s_current_fps = 60;

static int s_degrade_level = 0;
static int64_t s_work_ema_us = 0;
static int s_over_frames = 0;
static int s_under_frames = 0;

static const int k_scale_factor[PS4_SCALE_COUNT] = { 1, 2, 3, 4, 6 };
static const VScaleAlgo k_filter_algo[PS4_FILTER_COUNT] = {
    VSCALE_NEAREST, VSCALE_SCALEX, VSCALE_HQ, VSCALE_XBR
};

/*
 * Buffers pre-alocados (BSS): nenhum malloc/calloc no laco grafico.
 * Cada slot tem seus proprios buffers 64B-alinhados para as 4 threads nao compartilharem linhas de cache.
 * 4x (960x640) cabe em todos os slots; o 6x (1440x960) so existe no 1P e usa um buffer unico.
 */
#define SMALL_FINAL_PIXELS ((GBA_SCREEN_WIDTH * 4) * (GBA_SCREEN_HEIGHT * 4))
static uint32_t s_buf_1x[GBA_MAX_INSTANCES][GBA_SCREEN_WIDTH * GBA_SCREEN_HEIGHT] __attribute__((aligned(64)));
static uint32_t s_buf_mid[GBA_MAX_INSTANCES][VSCALE_BUF_MID_PIXELS] __attribute__((aligned(64)));
static uint32_t s_buf_small[GBA_MAX_INSTANCES][SMALL_FINAL_PIXELS] __attribute__((aligned(64)));
static uint32_t s_buf_big[VSCALE_BUF_FINAL_PIXELS] __attribute__((aligned(64)));
static uint32_t s_scratch_a[GBA_MAX_INSTANCES][PS4_SCREEN_WIDTH] __attribute__((aligned(64)));
static uint32_t s_scratch_b[GBA_MAX_INSTANCES][PS4_SCREEN_WIDTH] __attribute__((aligned(64)));
static VScaleMap s_map;

typedef struct Rect {
    int x, y, w, h;
} Rect;

/* Snapshot da configuracao efetiva do frame (escrito na thread principal antes de acordar os workers) */
typedef struct FrameCfg {
    VScaleAlgo algo;
    int factor;
    int src_w;
    int src_h;
    bool smooth;
    int active_count;
    int vw;
    int vh;
} FrameCfg;

static FrameCfg s_cfg;
static const uint32_t* s_slot_img[GBA_MAX_INSTANCES];

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

static void reset_auto_quality(void) {
    s_degrade_level = 0;
    s_work_ema_us = 0;
    s_over_frames = 0;
    s_under_frames = 0;
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

    /* Mantém o hardware de vídeo em 60Hz constante; o modo 30 FPS faz frameskip 1 de vídeo
     * mantendo a emulação da CPU do GBA e o áudio em 60Hz (100% da velocidade real). */
    sceVideoOutSetFlipRate(s_video_handle, 0);

    memset(&s_map, 0, sizeof(s_map));
    reset_auto_quality();

    s_buffer_index = 0;
    s_flip_arg = 1;
    s_submitted_flip_arg = 0;
    s_skip_frame_render = false;
    s_frame_deadline_us = 0;
    s_frame_start_us = sceKernelGetProcessTime();
    s_clear_frames_remaining = NUM_FRAMEBUFFERS;
    return true;
}

/* ------------------------------------------------------------------ */
/* Configuracao de video (menus)                                       */
/* ------------------------------------------------------------------ */

static int wrap_index(int cur, int delta, int count) {
    int next = (cur + delta) % count;
    return (next < 0) ? next + count : next;
}

void video_set_filter(PS4VideoFilter filter) {
    if (filter >= 0 && filter < PS4_FILTER_COUNT) {
        s_filter = filter;
        reset_auto_quality();
    }
}

PS4VideoFilter video_get_filter(void) {
    return s_filter;
}

void video_cycle_filter(int delta) {
    video_set_filter((PS4VideoFilter)wrap_index((int)s_filter, delta, PS4_FILTER_COUNT));
}

const char* video_get_filter_name(PS4VideoFilter filter) {
    switch (filter) {
    case PS4_FILTER_NEAREST:
        return "Pixel Puro (Nearest Neighbor)";
    case PS4_FILTER_SCALENX:
        return "ScaleNx (Scale2x / Scale3x / Scale4x)";
    case PS4_FILTER_HQX:
        return "HQx (aprox. hq2x / hq3x / hq4x)";
    case PS4_FILTER_XBR:
    default:
        return "xBR-lite (aprox. xBRZ / Super-xBR)";
    }
}

void video_set_scale(PS4VideoScale scale) {
    if (scale >= 0 && scale < PS4_SCALE_COUNT) {
        s_scale = scale;
        reset_auto_quality();
    }
}

PS4VideoScale video_get_scale(void) {
    return s_scale;
}

void video_cycle_scale(int delta) {
    video_set_scale((PS4VideoScale)wrap_index((int)s_scale, delta, PS4_SCALE_COUNT));
}

const char* video_get_scale_name(PS4VideoScale scale) {
    switch (scale) {
    case PS4_SCALE_1X:
        return "1X (Nativo 240x160)";
    case PS4_SCALE_2X:
        return "2X (480x320)";
    case PS4_SCALE_3X:
        return "3X (720x480)";
    case PS4_SCALE_4X:
        return "4X (960x640)";
    case PS4_SCALE_MAX:
    default:
        return "6X Max (1440x960 Super-Sampling)";
    }
}

void video_set_smoothing(bool enabled) {
    s_smooth = enabled;
    reset_auto_quality();
}

bool video_get_smoothing(void) {
    return s_smooth;
}

void video_toggle_smoothing(void) {
    video_set_smoothing(!s_smooth);
}

const char* video_get_smoothing_name(bool enabled) {
    return enabled ? "LIGADO (Bilinear Suave)" : "DESLIGADO (Sharp / Raw Pixels)";
}

void video_set_display_mode(PS4VideoDisplay mode) {
    if (mode >= 0 && mode < PS4_DISPLAY_COUNT) {
        s_display = mode;
        s_clear_frames_remaining = NUM_FRAMEBUFFERS;
    }
}

PS4VideoDisplay video_get_display_mode(void) {
    return s_display;
}

void video_cycle_display_mode(int delta) {
    video_set_display_mode((PS4VideoDisplay)wrap_index((int)s_display, delta, PS4_DISPLAY_COUNT));
}

const char* video_get_display_mode_name(PS4VideoDisplay mode) {
    return (mode == PS4_DISPLAY_STRETCH_16_9) ? "Esticar Tela Inteira (Full 16:9)"
                                              : "Aspecto 3:2 Perfeito (Recomendado)";
}

int video_get_auto_degrade_level(void) {
    return s_degrade_level;
}

void video_set_fps_limit(PS4FpsLimit limit) {
    if (limit >= 0 && limit < PS4_FPS_LIMIT_COUNT) {
        s_fps_limit = limit;
        s_skip_frame_render = false;
        s_fps_last_us = 0;
        s_fps_frame_count = 0;
        s_current_fps = (limit == PS4_FPS_30) ? 30 : 60;
        if (s_video_handle >= 0) {
            sceVideoOutSetFlipRate(s_video_handle, 0);
        }
        s_clear_frames_remaining = NUM_FRAMEBUFFERS;
    }
}

PS4FpsLimit video_get_fps_limit(void) {
    return s_fps_limit;
}

void video_toggle_fps_limit(void) {
    video_set_fps_limit((s_fps_limit == PS4_FPS_60) ? PS4_FPS_30 : PS4_FPS_60);
}

const char* video_get_fps_limit_name(PS4FpsLimit limit) {
    return (limit == PS4_FPS_30) ? "30 FPS" : "60 FPS";
}

bool video_begin_frame_should_skip_render(void) {
    if (s_fps_limit == PS4_FPS_30 &&
        ui_menu_get_state() == UI_STATE_INGAME &&
        !ui_menu_is_osd_open()) {
        s_skip_frame_render = !s_skip_frame_render;
    } else {
        s_skip_frame_render = false;
    }
    return s_skip_frame_render;
}

/* ------------------------------------------------------------------ */
/* Layout: viewport (tamanho na TV) - independente do upscale           */
/* ------------------------------------------------------------------ */

static void layout_area_size(int active_count, int* aw, int* ah) {
    *aw = PS4_SCREEN_WIDTH;
    *ah = PS4_SCREEN_HEIGHT;
    if (active_count == 2) {
        *aw /= 2;
    } else if (active_count >= 3) {
        *aw /= 2;
        *ah /= 2;
    }
}

/* Maior retangulo 3:2 (ou a area inteira em 16:9) centralizado na area do jogador */
static Rect fit_viewport(Rect area, PS4VideoDisplay display) {
    Rect vp = area;
    if (display == PS4_DISPLAY_ASPECT_3_2) {
        if (area.w * 2 >= area.h * 3) {
            vp.h = area.h;
            vp.w = (area.h * 3) / 2;
        } else {
            vp.w = area.w;
            vp.h = (area.w * 2) / 3;
        }
        vp.x = area.x + (area.w - vp.w) / 2;
        vp.y = area.y + (area.h - vp.h) / 2;
    }
    return vp;
}

static bool slot_viewport(int slot, const struct PS4GBA_Context* ctx, Rect* out) {
    if (!ctx || slot < 0 || slot >= GBA_MAX_INSTANCES ||
        !ctx->instances[slot].active || !ctx->instances[slot].video_buffer) {
        return false;
    }

    int ac = ctx->active_count;
    Rect area;
    if (ac <= 1) {
        area = (Rect){ 0, 0, PS4_SCREEN_WIDTH, PS4_SCREEN_HEIGHT };
    } else if (ac == 2) {
        int view_idx = 0;
        for (int i = 0; i < slot; ++i) {
            if (ctx->instances[i].active && ctx->instances[i].video_buffer) {
                view_idx++;
            }
        }
        if (view_idx >= 2) {
            return false;
        }
        area = (Rect){ view_idx * (PS4_SCREEN_WIDTH / 2), 0, PS4_SCREEN_WIDTH / 2, PS4_SCREEN_HEIGHT };
    } else {
        area = (Rect){ (slot & 1) ? (PS4_SCREEN_WIDTH / 2) : 0,
                       (slot >= 2) ? (PS4_SCREEN_HEIGHT / 2) : 0,
                       PS4_SCREEN_WIDTH / 2, PS4_SCREEN_HEIGHT / 2 };
    }
    *out = fit_viewport(area, s_display);
    return true;
}

static int snap_factor(int f) {
    if (f >= 6) {
        return 6;
    }
    if (f >= 4) {
        return 4;
    }
    return (f < 1) ? 1 : f;
}

/*
 * Congela a configuracao efetiva do frame (menus + reducao automatica + limite util pelo viewport)
 * e pre-computa a tabela horizontal do ajuste. Roda na thread principal antes de acordar os workers.
 */
static void begin_frame_config(const struct PS4GBA_Context* ctx) {
    int ac = (ctx && ctx->active_count > 0) ? ctx->active_count : 1;
    int aw, ah;
    layout_area_size(ac, &aw, &ah);
    Rect vp = fit_viewport((Rect){ 0, 0, aw, ah }, s_display);

    PS4VideoFilter filter = s_filter;
    bool smooth = s_smooth;
    int factor = k_scale_factor[s_scale];

    if (s_degrade_level >= 1) {
        smooth = false;
    }
    if (s_degrade_level >= 2 && factor > 2) {
        factor = 2;
    }
    if (s_degrade_level >= 3) {
        filter = PS4_FILTER_NEAREST;
        factor = 1;
    }

    /* Ampliar alem da altura do viewport so gasta CPU */
    int useful = (vp.h + GBA_SCREEN_HEIGHT - 1) / GBA_SCREEN_HEIGHT;
    if (factor > useful) {
        factor = useful;
    }
    factor = snap_factor(factor);
    if (ac > 1 && factor > 4) {
        factor = 4;
    }

    s_cfg.algo = k_filter_algo[filter];
    s_cfg.factor = factor;
    s_cfg.src_w = GBA_SCREEN_WIDTH * factor;
    s_cfg.src_h = GBA_SCREEN_HEIGHT * factor;
    s_cfg.smooth = smooth;
    s_cfg.active_count = ac;
    s_cfg.vw = vp.w;
    s_cfg.vh = vp.h;

    vscale_map_build(&s_map, s_cfg.src_w, s_cfg.vw, smooth);
}

/* ------------------------------------------------------------------ */
/* Renderizacao por jogador                                              */
/* ------------------------------------------------------------------ */

void video_prepare_instance(int slot_idx, struct PS4GBA_Context* ctx) {
    if (!ctx || slot_idx < 0 || slot_idx >= GBA_MAX_INSTANCES ||
        !ctx->instances[slot_idx].active || !ctx->instances[slot_idx].video_buffer) {
        return;
    }

    uint32_t* final_buf = (s_cfg.factor > 4) ? s_buf_big : s_buf_small[slot_idx];
    vscale_convert_xbgr(ctx->instances[slot_idx].video_buffer, s_buf_1x[slot_idx]);
    s_slot_img[slot_idx] = vscale_build(s_cfg.algo, s_cfg.factor, s_buf_1x[slot_idx],
                                        final_buf, s_buf_mid[slot_idx]);
}

/* job = indice do scratch: o slot (multi-jogador) ou a banda (1P) */
static void fit_band(int slot_idx, int job, int band, int band_count,
                     uint32_t* back_buffer, const struct PS4GBA_Context* ctx) {
    Rect vp;
    if (!back_buffer || !s_slot_img[slot_idx] || !slot_viewport(slot_idx, ctx, &vp)) {
        return;
    }
    int row0 = (int)(((int64_t)vp.h * band) / band_count);
    int row1 = (int)(((int64_t)vp.h * (band + 1)) / band_count);
    vscale_fit_rows(s_slot_img[slot_idx], s_cfg.src_w, s_cfg.src_h,
                    &back_buffer[vp.y * PS4_SCREEN_WIDTH + vp.x], PS4_SCREEN_WIDTH,
                    vp.w, vp.h, row0, row1, &s_map,
                    s_scratch_a[job], s_scratch_b[job]);
}

void video_render_instance_band(int slot_idx, int band, int band_count,
                                uint32_t* back_buffer, struct PS4GBA_Context* ctx) {
    if (slot_idx < 0 || slot_idx >= GBA_MAX_INSTANCES || band < 0 ||
        band_count <= 0 || band >= band_count || band >= GBA_MAX_INSTANCES) {
        return;
    }
    fit_band(slot_idx, band, band, band_count, back_buffer, ctx);
}

void video_draw_instance_border(int slot_idx, uint32_t* back_buffer, struct PS4GBA_Context* ctx) {
    static const uint32_t player_colors[GBA_MAX_INSTANCES] = {
        0xFF0066FFU, /* P1: Azul (Controle 1) */
        0xFFFF1744U, /* P2: Vermelho (Controle 2) */
        0xFF00E676U, /* P3: Verde (Controle 3) */
        0xFFFF1493U  /* P4: Rosa (Controle 4) */
    };
    Rect vp;
    if (!back_buffer || !slot_viewport(slot_idx, ctx, &vp)) {
        return;
    }
    bool focus_highlight = (audio_get_mode() == PS4_AUDIO_MODE_FOCUS);
    int border_thick = (focus_highlight && audio_get_focus_player() == slot_idx) ? 4 : 2;
    draw_viewport_border(back_buffer, vp.x, vp.y, vp.w, vp.h, border_thick, player_colors[slot_idx]);
}

void video_render_instance_slot(int slot_idx, uint32_t* back_buffer, struct PS4GBA_Context* ctx) {
    if (!back_buffer || !ctx || slot_idx < 0 || slot_idx >= GBA_MAX_INSTANCES) {
        return;
    }
    video_prepare_instance(slot_idx, ctx);
    fit_band(slot_idx, slot_idx, 0, 1, back_buffer, ctx);
    video_draw_instance_border(slot_idx, back_buffer, ctx);
}

/* ------------------------------------------------------------------ */
/* Pacing, FPS e qualidade automatica                                    */
/* ------------------------------------------------------------------ */

/*
 * Limitador de quadros de precisão de microssegundos via contador monotônico de hardware do Orbis OS
 * (`sceKernelGetProcessTime`). Garante 60.00 Hz exatos (16.667 us/quadro) sem acelerar o jogo.
 */
static void pace_frame_60hz(void) {
    const uint64_t frame_period_us = 16667ULL;
    uint64_t now = sceKernelGetProcessTime();
    if (s_frame_deadline_us == 0 || now > s_frame_deadline_us + 100000ULL) {
        s_frame_deadline_us = now + frame_period_us;
        s_frame_start_us = now;
        return;
    }
    while (now < s_frame_deadline_us) {
        uint64_t rem = s_frame_deadline_us - now;
        if (rem > 1500ULL) {
            sceKernelUsleep((OrbisKernelUseconds)(rem - 1000ULL));
        } else {
            __builtin_ia32_pause();
        }
        now = sceKernelGetProcessTime();
    }
    s_frame_deadline_us += frame_period_us;
    if (now > s_frame_deadline_us + frame_period_us) {
        s_frame_deadline_us = now + frame_period_us;
    }
    s_frame_start_us = now;
}

static void update_fps_counter(void) {
    uint64_t now_us = sceKernelGetProcessTime();
    if (s_fps_last_us == 0 || now_us <= s_fps_last_us) {
        s_fps_last_us = now_us;
        s_fps_frame_count = 0;
        return;
    }
    s_fps_frame_count++;
    uint64_t elapsed_us = now_us - s_fps_last_us;
    if (elapsed_us >= 500000ULL) {
        int fps = (int)(((uint64_t)s_fps_frame_count * 1000000ULL + (elapsed_us / 2ULL)) / elapsed_us);
        int max_target = (s_fps_limit == PS4_FPS_30) ? 30 : 60;
        if (fps > max_target && fps <= max_target + 2) {
            fps = max_target;
        } else if (fps > 999) {
            fps = 999;
        }
        s_current_fps = fps;
        s_fps_frame_count = 0;
        s_fps_last_us = now_us;
    }
}

/*
 * work_us = tempo de CPU do frame (emulacao + video), medido ate antes da espera de VSync.
 * Acima de 14 ms sustentados reduz a qualidade em degraus; so volta quando houver folga clara.
 */
static void update_auto_quality(uint64_t work_us) {
    if (s_work_ema_us == 0) {
        s_work_ema_us = (int64_t)work_us;
    } else {
        s_work_ema_us = (s_work_ema_us * 7 + (int64_t)work_us) / 8;
    }

    if (s_work_ema_us > (int64_t)AUTO_BUDGET_US) {
        s_under_frames = 0;
        if (++s_over_frames >= AUTO_DEGRADE_FRAMES && s_degrade_level < AUTO_MAX_LEVEL) {
            s_degrade_level++;
            s_over_frames = 0;
            s_work_ema_us = 0;
        }
    } else if (s_work_ema_us < (int64_t)AUTO_RECOVER_US && s_degrade_level > 0) {
        s_over_frames = 0;
        if (++s_under_frames >= AUTO_RECOVER_FRAMES) {
            s_degrade_level--;
            s_under_frames = 0;
            s_work_ema_us = 0;
        }
    } else {
        s_over_frames = 0;
        s_under_frames = 0;
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

    UIAppState ui_state = ui_menu_get_state();
    if (ui_state == UI_STATE_LAUNCHER) {
        update_fps_counter();
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
        pace_frame_60hz();
        return;
    }

    bool osd_open = ui_menu_is_osd_open();

    /* No modo 30 FPS, pula a composição/apresentação de vídeo nos frames ímpares
     * mantendo a emulação do GBA e o áudio rodando a 60Hz constantes (velocidade 1x real). */
    if (s_skip_frame_render && !osd_open) {
        pace_frame_60hz();
        return;
    }

    update_fps_counter();

    int active_count = ctx ? ctx->active_count : 0;
    int audio_focus = audio_get_focus_player();
    bool focus_highlight = (audio_get_mode() == PS4_AUDIO_MODE_FOCUS);
    bool show_fps = ui_menu_is_fps_visible();

    if (s_prev_ui_state != ui_state ||
        s_prev_active_count != active_count ||
        s_prev_audio_focus != audio_focus ||
        s_prev_focus_highlight != focus_highlight ||
        s_prev_osd_open != osd_open ||
        s_prev_show_fps != show_fps ||
        s_prev_display != s_display) {
        s_clear_frames_remaining = NUM_FRAMEBUFFERS;
        s_prev_ui_state = ui_state;
        s_prev_active_count = active_count;
        s_prev_audio_focus = audio_focus;
        s_prev_focus_highlight = focus_highlight;
        s_prev_osd_open = osd_open;
        s_prev_show_fps = show_fps;
        s_prev_display = s_display;
    }

    if (osd_open || s_clear_frames_remaining > 0) {
        memset(back_buffer, 0, FRAME_BYTES);
        if (!osd_open && s_clear_frames_remaining > 0) {
            s_clear_frames_remaining--;
        }
    }

    begin_frame_config(ctx);

    /* Renderiza os viewports de todas as instâncias ativas em paralelo nos 4 núcleos Jaguar */
    emulator_parallel_render_video(back_buffer, ctx);

    if (show_fps) {
        ui_menu_render_fps_overlay(back_buffer, s_current_fps);
    }

    if (ui_state == UI_STATE_INGAME && !osd_open) {
        ui_menu_render_join_hint(back_buffer, ctx);
    }

    if (osd_open) {
        ui_menu_render_osd(back_buffer, ctx);
    } else {
        uint64_t t_work_end = sceKernelGetProcessTime();
        if (t_work_end > s_frame_start_us) {
            update_auto_quality(t_work_end - s_frame_start_us);
        }
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
    pace_frame_60hz();
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
