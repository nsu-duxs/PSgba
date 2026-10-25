#ifndef VIDEO_SPLIT_H
#define VIDEO_SPLIT_H

#include <stdint.h>
#include <stdbool.h>
#include "emulator_manager.h"

#define PS4_SCREEN_WIDTH  1920
#define PS4_SCREEN_HEIGHT 1080

typedef enum PS4VideoResMode {
    PS4_RES_AUTO_HD    = 0, /* Auto: 4x Ultra HD em 1P/2P, 2x HD em 3P/4P (3:2 Max) */
    PS4_RES_4X_UHD     = 1, /* 4x Ultra HD (960x640 Scale4x EPX+AA -> 3:2 Max)      */
    PS4_RES_2X_HD      = 2, /* 2x HD (480x320 Scale2x EPX+AA -> 3:2 Max)            */
    PS4_RES_FULL_16_9  = 3, /* Tela Cheia 16:9 HD (1920x1080 / 960x540 sem bordas)  */
    PS4_RES_1X_NATIVE  = 4, /* 1x Nativa (240x160 Pixel Puro Inteiro)               */
    PS4_RES_MODE_COUNT = 5
} PS4VideoResMode;

bool video_init(void);
void video_set_res_mode(PS4VideoResMode mode);
PS4VideoResMode video_get_res_mode(void);
void video_cycle_res_mode(int delta);
const char* video_get_res_mode_name(PS4VideoResMode mode);
void video_render_split_screen(struct PS4GBA_Context* ctx);
void video_request_clear_framebuffers(void);
void video_shutdown(void);

#endif /* VIDEO_SPLIT_H */
