#ifndef EMULATOR_MANAGER_H
#define EMULATOR_MANAGER_H

#include <stdint.h>
#include <stdbool.h>
#include <orbis/UserService.h>
#include <mgba/core/core.h>
#include <mgba/gba/core.h>

#define GBA_MAX_INSTANCES 4
#define GBA_SCREEN_WIDTH  240
#define GBA_SCREEN_HEIGHT 160

typedef struct __attribute__((aligned(64))) PS4GBA_Instance {
    struct mCore* core;
    uint32_t* video_buffer;
    bool active;
    OrbisUserServiceUserId user_id;
    char rom_name[128];
} PS4GBA_Instance;

typedef struct PS4GBA_Context {
    PS4GBA_Instance instances[GBA_MAX_INSTANCES];
    int active_count;
    char rom_name[128];
    char rom_path[256];
} PS4GBA_Context;

extern PS4GBA_Context g_gba_ctx;

bool emulator_init_session(const char* const rom_paths[GBA_MAX_INSTANCES], int num_players);
bool emulator_init_all(const char* rom_path);
bool emulator_join_player(int slot_index, OrbisUserServiceUserId user_id);
void emulator_step_lockstep_frame(void);
void emulator_parallel_render_video(uint32_t* back_buffer, struct PS4GBA_Context* ctx);
void emulator_destroy_all(void);

#endif /* EMULATOR_MANAGER_H */
