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
    char rom_path[256];
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

/* Suporte nativo a Trapacas / Cheats (.cht) */
bool emulator_find_cheat_file(const char* rom_path, const char* rom_name, char* out_path, size_t max_len);
bool emulator_load_cheats(int slot_index, const char* rom_path);
bool emulator_has_cheat_file(int slot_index);
int emulator_get_cheat_count(int slot_index);
int emulator_get_cheat_code_count(int slot_index, int cheat_index);
bool emulator_is_cheat_master(int slot_index, int cheat_index);
const char* emulator_get_cheat_name(int slot_index, int cheat_index);
bool emulator_is_cheat_enabled(int slot_index, int cheat_index);
void emulator_toggle_cheat(int slot_index, int cheat_index);
const char* emulator_get_cheat_file_path(int slot_index);
const char* emulator_get_cheat_status(int slot_index);
void emulator_apply_cheats_frame(int slot_index);

#endif /* EMULATOR_MANAGER_H */
