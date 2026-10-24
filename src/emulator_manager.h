#ifndef EMULATOR_MANAGER_H
#define EMULATOR_MANAGER_H

#include <stdint.h>
#include <stdbool.h>
#include <mgba/core/core.h>
#include <mgba/gba/core.h>

#define GBA_MAX_INSTANCES 4
#define GBA_SCREEN_WIDTH  240
#define GBA_SCREEN_HEIGHT 160

typedef struct PS4GBA_Instance {
    struct mCore* core;
    uint32_t* video_buffer;
    bool active;
    char rom_name[128];
} PS4GBA_Instance;

typedef struct PS4GBA_Context {
    PS4GBA_Instance instances[GBA_MAX_INSTANCES];
    int active_count;
    char rom_name[128];
} PS4GBA_Context;

extern PS4GBA_Context g_gba_ctx;

bool emulator_init_session(const char* const rom_paths[GBA_MAX_INSTANCES], int num_players);
bool emulator_init_all(const char* rom_path);
void emulator_step_lockstep_frame(void);
void emulator_destroy_all(void);

#endif /* EMULATOR_MANAGER_H */

