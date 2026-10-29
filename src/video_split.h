#ifndef VIDEO_SPLIT_H
#define VIDEO_SPLIT_H

#include <stdint.h>
#include <stdbool.h>
#include "emulator_manager.h"

#define PS4_SCREEN_WIDTH  1920
#define PS4_SCREEN_HEIGHT 1080

/*
 * Pipeline: [viewport] e [buffer de upscale] sao independentes.
 *  - Viewport: area da TV ocupada pelo jogo. Depende SOMENTE de PS4VideoDisplay e do numero
 *    de jogadores (1P 1620x1080 / 2P 960x640 / 4P 810x540 em 3:2; ou a area inteira em 16:9).
 *  - Upscale: filtro + fator interno decidem como 240x160 e ampliado ANTES de ser ajustado
 *    ao viewport. Nao altera o tamanho da imagem na TV.
 */
typedef enum PS4VideoFilter {
    PS4_FILTER_NEAREST = 0, /* Pixel puro                         */
    PS4_FILTER_SCALENX = 1, /* Scale2x / Scale3x / Scale4x         */
    PS4_FILTER_HQX     = 2, /* HQx-like (aproximacao SIMD)         */
    PS4_FILTER_XBR     = 3, /* xBR-lite (aproximacao de xBRZ)      */
    PS4_FILTER_COUNT   = 4
} PS4VideoFilter;

typedef enum PS4VideoScale {
    PS4_SCALE_1X    = 0, /* 240x160   */
    PS4_SCALE_2X    = 1, /* 480x320   */
    PS4_SCALE_3X    = 2, /* 720x480   */
    PS4_SCALE_4X    = 3, /* 960x640   */
    PS4_SCALE_MAX   = 4, /* 1440x960 (6x, super-sampling no 1P) */
    PS4_SCALE_COUNT = 5
} PS4VideoScale;

typedef enum PS4VideoDisplay {
    PS4_DISPLAY_ASPECT_3_2   = 0, /* 3:2 perfeito, pillarbox centralizado */
    PS4_DISPLAY_STRETCH_16_9 = 1, /* preenche 100% da area do jogador     */
    PS4_DISPLAY_COUNT        = 2
} PS4VideoDisplay;

typedef enum PS4FpsLimit {
    PS4_FPS_60 = 0,
    PS4_FPS_30 = 1,
    PS4_FPS_LIMIT_COUNT = 2
} PS4FpsLimit;

bool video_init(void);

void video_set_filter(PS4VideoFilter filter);
PS4VideoFilter video_get_filter(void);
void video_cycle_filter(int delta);
const char* video_get_filter_name(PS4VideoFilter filter);

void video_set_scale(PS4VideoScale scale);
PS4VideoScale video_get_scale(void);
void video_cycle_scale(int delta);
const char* video_get_scale_name(PS4VideoScale scale);

void video_set_smoothing(bool enabled);
bool video_get_smoothing(void);
void video_toggle_smoothing(void);
const char* video_get_smoothing_name(bool enabled);

void video_set_display_mode(PS4VideoDisplay mode);
PS4VideoDisplay video_get_display_mode(void);
void video_cycle_display_mode(int delta);
const char* video_get_display_mode_name(PS4VideoDisplay mode);

/* 0 = qualidade configurada; 1..3 = reducao automatica por tempo de frame > 14 ms */
int video_get_auto_degrade_level(void);

void video_set_fps_limit(PS4FpsLimit limit);
PS4FpsLimit video_get_fps_limit(void);
void video_toggle_fps_limit(void);
const char* video_get_fps_limit_name(PS4FpsLimit limit);
bool video_begin_frame_should_skip_render(void);

/* Jogador N inteiro (conversao + upscale + ajuste + borda) em uma unica thread */
void video_render_instance_slot(int slot_idx, uint32_t* back_buffer, struct PS4GBA_Context* ctx);

/* 1P em varias threads: prepare (1x) -> bandas de linhas (N threads) -> border (1x) */
void video_prepare_instance(int slot_idx, struct PS4GBA_Context* ctx);
void video_render_instance_band(int slot_idx, int band, int band_count,
                                uint32_t* back_buffer, struct PS4GBA_Context* ctx);
void video_draw_instance_border(int slot_idx, uint32_t* back_buffer, struct PS4GBA_Context* ctx);

void video_render_split_screen(struct PS4GBA_Context* ctx);
void video_request_clear_framebuffers(void);
void video_shutdown(void);

#endif /* VIDEO_SPLIT_H */
