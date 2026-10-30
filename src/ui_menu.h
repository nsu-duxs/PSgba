#ifndef UI_MENU_H
#define UI_MENU_H

#include <stdint.h>
#include <stdbool.h>
#include "emulator_manager.h"
#include "audio_ps4.h"

#define UI_MAX_ROMS      512
#define UI_MAX_FILENAME  128
#define UI_MAX_PATH      256

typedef enum UIAppState {
    UI_STATE_SPLASH   = 0,
    UI_STATE_LAUNCHER = 1,
    UI_STATE_INGAME   = 2,
    UI_STATE_OSD      = 3
} UIAppState;

typedef enum LauncherTab {
    LAUNCHER_TAB_GAMES = 0,
    LAUNCHER_TAB_VIDEO_AUDIO = 1,
    LAUNCHER_TAB_CONTROLS = 2,
    LAUNCHER_TAB_COUNT = 3
} LauncherTab;

typedef enum OsdTab {
    OSD_TAB_SAVES = 0,
    OSD_TAB_VIDEO_AUDIO = 1,
    OSD_TAB_SETTINGS = 1, /* Alias de compatibilidade */
    OSD_TAB_LINK_CABLE = 2,
    OSD_TAB_CONTROLS = 3,
    OSD_TAB_COUNT = 4
} OsdTab;

void ui_menu_init(void);
void ui_menu_scan_roms(void);

UIAppState ui_menu_get_state(void);
void ui_menu_set_state(UIAppState state);
void ui_menu_open_osd(int trigger_player);
bool ui_menu_is_osd_open(void);
bool ui_menu_is_fps_visible(void);
void ui_menu_set_fps_visible(bool visible);
void ui_menu_trigger_join_hint(void);
void ui_menu_render_join_hint(uint32_t* fb, struct PS4GBA_Context* ctx);

void ui_menu_update_splash(uint32_t buttons_pressed);
void ui_menu_update_launcher(struct PS4GBA_Context* ctx, uint32_t buttons_pressed);
void ui_menu_update_osd(struct PS4GBA_Context* ctx, uint32_t buttons_pressed);

void ui_menu_render_splash(uint32_t* fb);
void ui_menu_render_launcher(uint32_t* fb);
void ui_menu_render_osd(uint32_t* fb, struct PS4GBA_Context* ctx);
void ui_menu_render_fps_overlay(uint32_t* fb, int fps);

#endif /* UI_MENU_H */
