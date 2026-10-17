#ifndef VIDEO_SPLIT_H
#define VIDEO_SPLIT_H

#include <stdint.h>
#include <stdbool.h>
#include "emulator_manager.h"

#define PS4_SCREEN_WIDTH  1920
#define PS4_SCREEN_HEIGHT 1080

bool video_init(void);
void video_render_split_screen(struct PS4GBA_Context* ctx);
void video_shutdown(void);

#endif /* VIDEO_SPLIT_H */
