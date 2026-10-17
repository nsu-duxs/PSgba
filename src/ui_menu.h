#ifndef UI_MENU_H
#define UI_MENU_H

#include <stdint.h>
#include <stdbool.h>
#include "emulator_manager.h"

#define UI_MAX_ROMS      128
#define UI_MAX_FILENAME  128
#define UI_MAX_PATH      256

typedef enum UIAppState {
    UI_STATE_LAUNCHER = 0,
    UI_STATE_INGAME   = 1,
    UI_STATE_OSD      = 2
} UIAppState;

typedef enum UIRomMode {
    UI_ROM_MODE_SHARED     = 0, /* Same ROM for all active players */
    UI_ROM_MODE_INDIVIDUAL = 1  /* Individual ROM selected per player */
} UIRomMode;

void ui_menu_init(void);
void ui_menu_scan_roms(void);

UIAppState ui_menu_get_state(void);
void ui_menu_set_state(UIAppState state);
void ui_menu_open_osd(int trigger_player);
bool ui_menu_is_osd_open(void);
bool ui_menu_is_fps_visible(void);
void ui_menu_set_fps_visible(bool visible);

void ui_menu_update_launcher(struct PS4GBA_Context* ctx, uint32_t buttons_pressed);
void ui_menu_update_osd(struct PS4GBA_Context* ctx, uint32_t buttons_pressed);

void ui_menu_render_launcher(uint32_t* fb);
void ui_menu_render_osd(uint32_t* fb, struct PS4GBA_Context* ctx);
void ui_menu_render_fps_overlay(uint32_t* fb, int fps);

#endif /* UI_MENU_H */

