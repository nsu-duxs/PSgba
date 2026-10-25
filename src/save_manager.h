#ifndef SAVE_MANAGER_H
#define SAVE_MANAGER_H

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include <orbis/UserService.h>
#include "emulator_manager.h"

#define PS4_DATA_ROOT_DIR   "/data/gba4p"
#define PS4_ROMS_DIR        "/data/gba4p/roms"
#define PS4_SAVES_DIR       "/data/gba4p/saves"
#define PS4_GUEST_SAVES_DIR "/data/gba4p/saves/guests"
#define PS4_STATES_DIR      "/data/gba4p/states"

bool save_manager_init(void);
void save_manager_extract_rom_name(const char* rom_path, char* out_name, size_t max_len);
bool save_manager_attach_battery_save(struct mCore* core, const char* rom_name, int player_id,
                                      OrbisUserServiceUserId user_id);
void save_manager_flush_battery_save(struct mCore* core);

bool save_manager_quick_save_state(struct PS4GBA_Context* ctx, int player_id);
bool save_manager_quick_load_state(struct PS4GBA_Context* ctx, int player_id);

bool save_manager_group_save_state(struct PS4GBA_Context* ctx);
bool save_manager_group_load_state(struct PS4GBA_Context* ctx);

#endif /* SAVE_MANAGER_H */
