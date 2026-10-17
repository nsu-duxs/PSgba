#ifndef AUDIO_PS4_H
#define AUDIO_PS4_H

#include <stdint.h>
#include <stdbool.h>
#include "emulator_manager.h"

#define PS4_AUDIO_SAMPLE_RATE 48000
#define PS4_AUDIO_GRANULARITY 1024
#define GBA_AUDIO_BUFFER_SIZE 2048

typedef enum PS4AudioMode {
    PS4_AUDIO_MODE_FOCUS = 0,
    PS4_AUDIO_MODE_MIX   = 1
} PS4AudioMode;

bool audio_init(void);
void audio_set_mode(PS4AudioMode mode);
PS4AudioMode audio_get_mode(void);
void audio_set_focus_player(int player_id);
int audio_get_focus_player(void);
void audio_process(struct PS4GBA_Context* ctx);
void audio_shutdown(void);

#endif /* AUDIO_PS4_H */

