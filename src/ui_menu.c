#include "ui_menu.h"
#include "font8x8.h"
#include "video_split.h"
#include "audio_ps4.h"
#include "save_manager.h"
#include "link_cable_hub.h"
#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <dirent.h>
#include <orbis/Pad.h>

#define LAUNCHER_ROM_START_IDX 4

enum {
    OSD_ITEM_RESUME = 0,
    OSD_ITEM_LINK_POWER,
    OSD_ITEM_LINK_HOTPLUG,
    OSD_ITEM_VIDEO_RES,
    OSD_ITEM_SHOW_FPS,
    OSD_ITEM_AUDIO_FOCUS,
    OSD_ITEM_QUICK_SAVE,
    OSD_ITEM_QUICK_LOAD,
    OSD_ITEM_GROUP_SAVE,
    OSD_ITEM_GROUP_LOAD,
    OSD_ITEM_RETURN_LAUNCHER,
    OSD_ITEM_COUNT
};

static UIAppState s_app_state = UI_STATE_LAUNCHER;
static int s_num_players = 4;
static UIRomMode s_rom_mode = UI_ROM_MODE_SHARED;
static bool s_show_fps = true;

static char s_rom_files[UI_MAX_ROMS][UI_MAX_FILENAME];
static char s_rom_paths[UI_MAX_ROMS][UI_MAX_PATH];
static int s_rom_count = 0;

static int s_launcher_cursor = 0; /* 0 = Players, 1 = ROM Mode, 2 = Resolution, 3 = FPS, 4+ = ROM list */
static int s_selecting_player = 0;
static char s_selected_roms[GBA_MAX_INSTANCES][UI_MAX_PATH];
static char s_launcher_status[128] = "";

static int s_osd_cursor = 0;
static int s_osd_player = 0;
static char s_osd_status[64] = "";

static void draw_rect_solid(uint32_t* fb, int x, int y, int w, int h, uint32_t color) {
    for (int py = y; py < y + h; ++py) {
        if (py < 0 || py >= PS4_SCREEN_HEIGHT) {
            continue;
        }
        uint32_t* row = &fb[py * PS4_SCREEN_WIDTH];
        for (int px = x; px < x + w; ++px) {
            if (px >= 0 && px < PS4_SCREEN_WIDTH) {
                row[px] = color;
            }
        }
    }
}

static void draw_rect_translucent(uint32_t* fb, int x, int y, int w, int h, uint32_t tint) {
    for (int py = y; py < y + h; ++py) {
        if (py < 0 || py >= PS4_SCREEN_HEIGHT) {
            continue;
        }
        uint32_t* row = &fb[py * PS4_SCREEN_WIDTH];
        for (int px = x; px < x + w; ++px) {
            if (px >= 0 && px < PS4_SCREEN_WIDTH) {
                uint32_t bg = (row[px] >> 2) & 0x3F3F3F3FU;
                row[px] = 0xFF000000U | (bg + tint);
            }
        }
    }
}

static void draw_rect_border(uint32_t* fb, int x, int y, int w, int h, int thickness, uint32_t color) {
    draw_rect_solid(fb, x, y, w, thickness, color);
    draw_rect_solid(fb, x, y + h - thickness, w, thickness, color);
    draw_rect_solid(fb, x, y, thickness, h, color);
    draw_rect_solid(fb, x + w - thickness, y, thickness, h, color);
}

static bool has_gba_extension(const char* name) {
    if (!name) {
        return false;
    }
    const char* dot = strrchr(name, '.');
    if (!dot) {
        return false;
    }
    return (strcasecmp(dot, ".gba") == 0);
}

static void scan_rom_directory(const char* dir_path) {
    DIR* dir = opendir(dir_path);
    if (!dir) {
        return;
    }

    struct dirent* entry;
    while ((entry = readdir(dir)) != NULL && s_rom_count < UI_MAX_ROMS) {
        if (entry->d_name[0] == '.') {
            continue;
        }
        if (has_gba_extension(entry->d_name)) {
            snprintf(s_rom_paths[s_rom_count], UI_MAX_PATH, "%s/%s", dir_path, entry->d_name);
            strncpy(s_rom_files[s_rom_count], entry->d_name, UI_MAX_FILENAME - 1);
            s_rom_files[s_rom_count][UI_MAX_FILENAME - 1] = '\0';
            s_rom_count++;
        }
    }
    closedir(dir);
}

void ui_menu_scan_roms(void) {
    s_rom_count = 0;
    memset(s_rom_files, 0, sizeof(s_rom_files));
    memset(s_rom_paths, 0, sizeof(s_rom_paths));

    static const char* const k_search_dirs[] = {
        PS4_ROMS_DIR,
        "/data/roms",
        "/mnt/usb0/gba4p/roms",
        "/mnt/usb0/roms",
        "/mnt/usb0",
        "/mnt/usb1/gba4p/roms",
        "/mnt/usb1/roms",
        "/mnt/usb1"
    };

    for (size_t i = 0; i < sizeof(k_search_dirs) / sizeof(k_search_dirs[0]); ++i) {
        if (s_rom_count < UI_MAX_ROMS) {
            scan_rom_directory(k_search_dirs[i]);
        }
    }

    if (s_rom_count == 0) {
        strncpy(s_rom_files[0], "test.gba (Demo 4P Link Integrada)", UI_MAX_FILENAME - 1);
        snprintf(s_rom_paths[0], UI_MAX_PATH, "%s/test.gba", PS4_ROMS_DIR);
        s_rom_count = 1;
    }
}

void ui_menu_init(void) {
    save_manager_init();
    ui_menu_scan_roms();
    s_app_state = UI_STATE_LAUNCHER;
    s_num_players = 4;
    s_rom_mode = UI_ROM_MODE_SHARED;
    s_launcher_cursor = LAUNCHER_ROM_START_IDX;
    s_selecting_player = 0;
    s_osd_cursor = 0;
    s_osd_player = 0;
    s_osd_status[0] = '\0';
}

UIAppState ui_menu_get_state(void) {
    return s_app_state;
}

void ui_menu_set_state(UIAppState state) {
    s_app_state = state;
}

void ui_menu_open_osd(int trigger_player) {
    s_osd_player = (trigger_player >= 0 && trigger_player < GBA_MAX_INSTANCES) ? trigger_player : 0;
    s_osd_cursor = 0;
    s_osd_status[0] = '\0';
    s_app_state = UI_STATE_OSD;
}

bool ui_menu_is_osd_open(void) {
    return (s_app_state == UI_STATE_OSD);
}

bool ui_menu_is_fps_visible(void) {
    return s_show_fps;
}

void ui_menu_set_fps_visible(bool visible) {
    s_show_fps = visible;
}

void ui_menu_update_launcher(struct PS4GBA_Context* ctx, uint32_t buttons_pressed) {
    (void)ctx;
    if (buttons_pressed & ORBIS_PAD_BUTTON_TRIANGLE) {
        ui_menu_scan_roms();
        s_launcher_cursor = LAUNCHER_ROM_START_IDX;
        snprintf(s_launcher_status, sizeof(s_launcher_status),
                 "Lista de ROMs atualizada (/data/gba4p/roms e /mnt/usb0)");
        return;
    }

    int total_items = LAUNCHER_ROM_START_IDX + s_rom_count;

    if (buttons_pressed & ORBIS_PAD_BUTTON_UP) {
        s_launcher_cursor = (s_launcher_cursor - 1 + total_items) % total_items;
    }
    if (buttons_pressed & ORBIS_PAD_BUTTON_DOWN) {
        s_launcher_cursor = (s_launcher_cursor + 1) % total_items;
    }

    if (s_launcher_cursor == 0) {
        if (buttons_pressed & ORBIS_PAD_BUTTON_LEFT) {
            s_num_players = (s_num_players > 1) ? (s_num_players - 1) : 4;
            s_selecting_player = 0;
        }
        if (buttons_pressed & (ORBIS_PAD_BUTTON_RIGHT | ORBIS_PAD_BUTTON_CROSS)) {
            s_num_players = (s_num_players < 4) ? (s_num_players + 1) : 1;
            s_selecting_player = 0;
        }
    } else if (s_launcher_cursor == 1) {
        if (buttons_pressed & (ORBIS_PAD_BUTTON_LEFT | ORBIS_PAD_BUTTON_RIGHT | ORBIS_PAD_BUTTON_CROSS)) {
            s_rom_mode = (s_rom_mode == UI_ROM_MODE_SHARED)
                             ? UI_ROM_MODE_INDIVIDUAL
                             : UI_ROM_MODE_SHARED;
            s_selecting_player = 0;
        }
    } else if (s_launcher_cursor == 2) {
        if (buttons_pressed & ORBIS_PAD_BUTTON_LEFT) {
            video_cycle_res_mode(-1);
        }
        if (buttons_pressed & (ORBIS_PAD_BUTTON_RIGHT | ORBIS_PAD_BUTTON_CROSS)) {
            video_cycle_res_mode(1);
        }
    } else if (s_launcher_cursor == 3) {
        if (buttons_pressed & (ORBIS_PAD_BUTTON_LEFT | ORBIS_PAD_BUTTON_RIGHT | ORBIS_PAD_BUTTON_CROSS)) {
            s_show_fps = !s_show_fps;
        }
    } else if (s_launcher_cursor >= LAUNCHER_ROM_START_IDX) {
        if ((buttons_pressed & ORBIS_PAD_BUTTON_CIRCLE) &&
            s_rom_mode == UI_ROM_MODE_INDIVIDUAL && s_selecting_player > 0) {
            s_selecting_player--;
        }

        if (buttons_pressed & ORBIS_PAD_BUTTON_CROSS) {
            int rom_idx = s_launcher_cursor - LAUNCHER_ROM_START_IDX;
            if (rom_idx >= 0 && rom_idx < s_rom_count) {
                if (s_rom_mode == UI_ROM_MODE_SHARED) {
                    const char* path = s_rom_paths[rom_idx];
                    const char* paths[GBA_MAX_INSTANCES] = { path, path, path, path };
                    bool ok = emulator_init_session(paths, s_num_players);
                    if (ok) {
                        s_launcher_status[0] = '\0';
                        s_app_state = UI_STATE_INGAME;
                    } else {
                        ui_menu_scan_roms();
                        snprintf(s_launcher_status, sizeof(s_launcher_status),
                                 "Coloque arquivos .gba em /data/gba4p/roms/ ou /mnt/usb0/roms/");
                    }
                } else {
                    strncpy(s_selected_roms[s_selecting_player],
                            s_rom_paths[rom_idx],
                            sizeof(s_selected_roms[s_selecting_player]) - 1);
                    s_selected_roms[s_selecting_player][sizeof(s_selected_roms[s_selecting_player]) - 1] = '\0';
                    s_selecting_player++;
                    if (s_selecting_player >= s_num_players) {
                        const char* paths[GBA_MAX_INSTANCES] = {
                            s_selected_roms[0], s_selected_roms[1],
                            s_selected_roms[2], s_selected_roms[3]
                        };
                        s_selecting_player = 0;
                        if (emulator_init_session(paths, s_num_players)) {
                            s_launcher_status[0] = '\0';
                            s_app_state = UI_STATE_INGAME;
                        } else {
                            ui_menu_scan_roms();
                            snprintf(s_launcher_status, sizeof(s_launcher_status),
                                     "Erro ao abrir ROM. Verifique /data/gba4p/roms/ ou USB");
                        }
                    }
                }
            }
        }
    }
}

void ui_menu_update_osd(struct PS4GBA_Context* ctx, uint32_t buttons_pressed) {
    if (buttons_pressed & (ORBIS_PAD_BUTTON_OPTIONS | ORBIS_PAD_BUTTON_CIRCLE)) {
        s_app_state = UI_STATE_INGAME;
        return;
    }

    if (buttons_pressed & ORBIS_PAD_BUTTON_UP) {
        s_osd_cursor = (s_osd_cursor - 1 + OSD_ITEM_COUNT) % OSD_ITEM_COUNT;
    }
    if (buttons_pressed & ORBIS_PAD_BUTTON_DOWN) {
        s_osd_cursor = (s_osd_cursor + 1) % OSD_ITEM_COUNT;
    }

    if (s_osd_cursor == OSD_ITEM_LINK_POWER &&
        (buttons_pressed & (ORBIS_PAD_BUTTON_LEFT | ORBIS_PAD_BUTTON_RIGHT | ORBIS_PAD_BUTTON_CROSS))) {
        bool next_state = !link_hub_is_cable_enabled();
        link_hub_set_cable_enabled(next_state);
        snprintf(s_osd_status, sizeof(s_osd_status),
                 next_state ? "Link Cable LIGADO e Conectado!" : "Link Cable DESLIGADO!");
        return;
    }

    if (s_osd_cursor == OSD_ITEM_VIDEO_RES &&
        (buttons_pressed & (ORBIS_PAD_BUTTON_LEFT | ORBIS_PAD_BUTTON_RIGHT | ORBIS_PAD_BUTTON_CROSS))) {
        int delta = (buttons_pressed & ORBIS_PAD_BUTTON_LEFT) ? -1 : 1;
        video_cycle_res_mode(delta);
        return;
    }

    if (s_osd_cursor == OSD_ITEM_SHOW_FPS &&
        (buttons_pressed & (ORBIS_PAD_BUTTON_LEFT | ORBIS_PAD_BUTTON_RIGHT | ORBIS_PAD_BUTTON_CROSS))) {
        s_show_fps = !s_show_fps;
        return;
    }

    if (s_osd_cursor == OSD_ITEM_AUDIO_FOCUS &&
        (buttons_pressed & (ORBIS_PAD_BUTTON_LEFT | ORBIS_PAD_BUTTON_RIGHT | ORBIS_PAD_BUTTON_CROSS))) {
        int active = (ctx && ctx->active_count > 0) ? ctx->active_count : 1;
        if (audio_get_mode() == PS4_AUDIO_MODE_MIX) {
            audio_set_mode(PS4_AUDIO_MODE_FOCUS);
            audio_set_focus_player(0);
        } else {
            int next_p = audio_get_focus_player() + 1;
            if (next_p >= active) {
                audio_set_mode(PS4_AUDIO_MODE_MIX);
            } else {
                audio_set_focus_player(next_p);
            }
        }
        return;
    }

    if (!(buttons_pressed & ORBIS_PAD_BUTTON_CROSS)) {
        return;
    }

    switch (s_osd_cursor) {
    case OSD_ITEM_RESUME:
        s_app_state = UI_STATE_INGAME;
        break;
    case OSD_ITEM_LINK_HOTPLUG:
        link_hub_trigger_hotplug();
        snprintf(s_osd_status, sizeof(s_osd_status),
                 "Plugging Link Cable... Volte ao jogo!");
        s_app_state = UI_STATE_INGAME;
        break;
    case OSD_ITEM_QUICK_SAVE:
        if (save_manager_quick_save_state(ctx, s_osd_player)) {
            snprintf(s_osd_status, sizeof(s_osd_status), "State P%d salvo!", s_osd_player + 1);
        } else {
            snprintf(s_osd_status, sizeof(s_osd_status), "Erro ao salvar P%d", s_osd_player + 1);
        }
        break;
    case OSD_ITEM_QUICK_LOAD:
        if (save_manager_quick_load_state(ctx, s_osd_player)) {
            snprintf(s_osd_status, sizeof(s_osd_status), "State P%d carregado!", s_osd_player + 1);
        } else {
            snprintf(s_osd_status, sizeof(s_osd_status), "State P%d nao encontrado", s_osd_player + 1);
        }
        break;
    case OSD_ITEM_GROUP_SAVE:
        if (save_manager_group_save_state(ctx)) {
            snprintf(s_osd_status, sizeof(s_osd_status), "Group State 4P salvo!");
        } else {
            snprintf(s_osd_status, sizeof(s_osd_status), "Erro no Group Save");
        }
        break;
    case OSD_ITEM_GROUP_LOAD:
        if (save_manager_group_load_state(ctx)) {
            snprintf(s_osd_status, sizeof(s_osd_status), "Group State 4P carregado!");
        } else {
            snprintf(s_osd_status, sizeof(s_osd_status), "Group State nao encontrado");
        }
        break;
    case OSD_ITEM_RETURN_LAUNCHER:
        emulator_destroy_all();
        ui_menu_scan_roms();
        s_selecting_player = 0;
        s_app_state = UI_STATE_LAUNCHER;
        break;
    default:
        break;
    }
}

void ui_menu_render_launcher(uint32_t* fb) {
    if (!fb) {
        return;
    }

    draw_rect_solid(fb, 0, 0, PS4_SCREEN_WIDTH, PS4_SCREEN_HEIGHT, 0xFF10141EU);
    draw_rect_border(fb, 60, 40, PS4_SCREEN_WIDTH - 120, PS4_SCREEN_HEIGHT - 80, 3, 0xFF3A86FFU);

    ui_draw_text(fb, 120, 76, "PS4 GBA 4-WAY SPLIT-SCREEN EMULATOR v1.02", 0xFFFFFFFFU, 3);

    static const char* const layout_labels[5] = {
        "",
        "1P (Tela Unica HD 1620x1080 / 1920x1080)",
        "2P (Split Side-by-Side HD 960x640 / 960x720)",
        "3P (Quadrantes 2x2 HD 804x532 / 960x540)",
        "4P (Quadrantes 2x2 HD 804x532 / 960x540)"
    };

    char line_buf[256];
    snprintf(line_buf, sizeof(line_buf), "%s Jogadores Ativos: < %s >",
             (s_launcher_cursor == 0) ? ">" : " ", layout_labels[s_num_players]);
    ui_draw_text(fb, 120, 142, line_buf,
                 (s_launcher_cursor == 0) ? 0xFF00F5D4U : 0xFFCCCCCCU, 2);

    snprintf(line_buf, sizeof(line_buf), "%s Modo de ROM: < %s >",
             (s_launcher_cursor == 1) ? ">" : " ",
             (s_rom_mode == UI_ROM_MODE_SHARED)
                 ? "Mesma ROM para todos"
                 : "ROM individual por jogador");
    ui_draw_text(fb, 120, 178, line_buf,
                 (s_launcher_cursor == 1) ? 0xFF00F5D4U : 0xFFCCCCCCU, 2);

    snprintf(line_buf, sizeof(line_buf), "%s Resolucao / Qualidade: < %s >",
             (s_launcher_cursor == 2) ? ">" : " ",
             video_get_res_mode_name(video_get_res_mode()));
    ui_draw_text(fb, 120, 214, line_buf,
                 (s_launcher_cursor == 2) ? 0xFF00F5D4U : 0xFFCCCCCCU, 2);

    snprintf(line_buf, sizeof(line_buf), "%s Mostrar Contador de FPS: < %s >",
             (s_launcher_cursor == 3) ? ">" : " ",
             s_show_fps ? "Ligado (Canto Superior)" : "Desligado");
    ui_draw_text(fb, 120, 250, line_buf,
                 (s_launcher_cursor == 3) ? 0xFF00F5D4U : 0xFFCCCCCCU, 2);

    if (s_rom_mode == UI_ROM_MODE_INDIVIDUAL) {
        snprintf(line_buf, sizeof(line_buf),
                 "Selecione a ROM para o JOGADOR %d de %d (/data/gba4p/roms/ ou USB):",
                 s_selecting_player + 1, s_num_players);
    } else {
        snprintf(line_buf, sizeof(line_buf),
                 "ROMS disponiveis (%d encontradas - [TRIANGULO] Atualizar):", s_rom_count);
    }
    ui_draw_text(fb, 120, 300, line_buf, 0xFFFFBE0BU, 2);

    int max_visible = 15;
    int selected_rom = (s_launcher_cursor >= LAUNCHER_ROM_START_IDX)
                           ? (s_launcher_cursor - LAUNCHER_ROM_START_IDX)
                           : 0;
    int start_idx = (selected_rom >= max_visible) ? (selected_rom - max_visible + 1) : 0;

    for (int i = 0; i < max_visible && (start_idx + i) < s_rom_count; ++i) {
        int idx = start_idx + i;
        bool is_sel = (s_launcher_cursor == idx + LAUNCHER_ROM_START_IDX);
        snprintf(line_buf, sizeof(line_buf), "%s [%02d] %s",
                 is_sel ? "->" : "  ", idx + 1, s_rom_files[idx]);
        ui_draw_text(fb, 140, 344 + i * 36, line_buf,
                     is_sel ? 0xFFFFFFFFU : 0xFF99A3B8U, 2);
    }

    if (s_launcher_status[0]) {
        ui_draw_text(fb, 120, 910, s_launcher_status, 0xFFFF595EU, 2);
    }

    ui_draw_text(fb, 120, 960,
                 "[X] Confirmar   [D-PAD] Navegar   [OPTIONS] Menu In-Game   [L3+R3] Foco Audio",
                 0xFF708090U, 2);
}

void ui_menu_render_osd(uint32_t* fb, struct PS4GBA_Context* ctx) {
    if (!fb || s_app_state != UI_STATE_OSD) {
        return;
    }

    int box_x = 360;
    int box_y = 120;
    int box_w = 1200;
    int box_h = 840;

    draw_rect_translucent(fb, box_x, box_y, box_w, box_h, 0x000A101AU);
    draw_rect_border(fb, box_x, box_y, box_w, box_h, 3, 0xFF00F5D4U);

    char buf[160];
    snprintf(buf, sizeof(buf), "MENU OSD v1.02 - CONTROLE P%d (%s)",
             s_osd_player + 1, ctx ? ctx->rom_name : "");
    ui_draw_text(fb, box_x + 50, box_y + 32, buf, 0xFFFFBE0BU, 3);

    char item_link_power[128];
    const char* cable_state_str = link_hub_is_hotplugging()
                                      ? "RECONECTANDO (HOTPLUG)..."
                                      : (link_hub_is_cable_enabled() ? "LIGADO / CONECTADO" : "DESLIGADO");
    snprintf(item_link_power, sizeof(item_link_power), "Estado do Link Cable: < %s >", cable_state_str);

    char item_res[140];
    snprintf(item_res, sizeof(item_res), "Resolucao / Qualidade: < %s >",
             video_get_res_mode_name(video_get_res_mode()));

    char item_fps[96];
    snprintf(item_fps, sizeof(item_fps), "Contador de FPS na Tela: < %s >",
             s_show_fps ? "Ligado" : "Desligado");

    char focus_label[64];
    if (audio_get_mode() == PS4_AUDIO_MODE_MIX) {
        snprintf(focus_label, sizeof(focus_label), "Modo Mix (Todas 25%%)");
    } else {
        snprintf(focus_label, sizeof(focus_label), "Foco Jogador P%d",
                 audio_get_focus_player() + 1);
    }

    char item_audio[128];
    snprintf(item_audio, sizeof(item_audio), "Trocar Foco de Audio: < %s >", focus_label);

    char item_qsave[128];
    snprintf(item_qsave, sizeof(item_qsave), "Salvar State Individual (P%d)", s_osd_player + 1);

    char item_qload[128];
    snprintf(item_qload, sizeof(item_qload), "Carregar State Individual (P%d)", s_osd_player + 1);

    const char* items[OSD_ITEM_COUNT] = {
        "Continuar Jogo",
        item_link_power,
        "Conectar / Reconectar Link Cable (Simular Hotplug)",
        item_res,
        item_fps,
        item_audio,
        item_qsave,
        item_qload,
        "Group Save State (4P + Link Cable)",
        "Group Load State (4P + Link Cable)",
        "Voltar ao Seletor de ROMs"
    };

    for (int i = 0; i < OSD_ITEM_COUNT; ++i) {
        bool sel = (s_osd_cursor == i);
        snprintf(buf, sizeof(buf), "%s %s", sel ? "->" : "  ", items[i]);
        ui_draw_text(fb, box_x + 50, box_y + 92 + i * 46, buf,
                     sel ? 0xFF00F5D4U : 0xFFE0E0E0U, 2);
    }

    /* Telemetria do Link Cable */
    uint32_t xfer_pkts = link_hub_get_transfer_count();
    int m0 = link_hub_get_player_mode(0);
    int m1 = link_hub_get_player_mode(1);
    const char* mode0_str = (m0 == GBA_SIO_MULTI) ? "MULTI" : (m0 == GBA_SIO_NORMAL_32) ? "NORM32" : (m0 == GBA_SIO_NORMAL_8) ? "NORM8" : (m0 == GBA_SIO_GPIO) ? "GPIO" : "IDLE";
    const char* mode1_str = (m1 == GBA_SIO_MULTI) ? "MULTI" : (m1 == GBA_SIO_NORMAL_32) ? "NORM32" : (m1 == GBA_SIO_NORMAL_8) ? "NORM8" : (m1 == GBA_SIO_GPIO) ? "GPIO" : "IDLE";

    char link_info[140];
    snprintf(link_info, sizeof(link_info), "Status Link: P1 [%s]  P2 [%s]  |  Cabo: %s  |  Pacotes: %u",
             mode0_str, mode1_str, link_hub_is_cable_enabled() ? "ON" : "OFF", xfer_pkts);
    ui_draw_text(fb, box_x + 50, box_y + 620, link_info, 0xFF00F5D4U, 2);

    if (s_osd_status[0]) {
        ui_draw_text(fb, box_x + 50, box_y + 668, s_osd_status, 0xFF38B000U, 2);
    }

    ui_draw_text(fb, box_x + 50, box_y + 720,
                 "[X / SETAS] Selecionar/Alterar   [CIRCLE/OPTIONS] Fechar OSD", 0xFF8899AAU, 2);
}

void ui_menu_render_fps_overlay(uint32_t* fb, int fps) {
    if (!fb || !s_show_fps) {
        return;
    }

    int badge_x = 24;
    int badge_y = 14;
    int badge_w = 156;
    int badge_h = 34;

    uint32_t color = 0xFF38B000U; /* Green (>= 55 FPS) */
    if (fps < 35) {
        color = 0xFFFF595EU;      /* Red   (< 35 FPS)  */
    } else if (fps < 55) {
        color = 0xFFFFBE0BU;      /* Gold  (35..54 FPS)*/
    }

    draw_rect_solid(fb, badge_x, badge_y, badge_w, badge_h, 0xFF10141EU);
    draw_rect_border(fb, badge_x, badge_y, badge_w, badge_h, 2, color);

    char fps_text[32];
    snprintf(fps_text, sizeof(fps_text), "FPS: %2d", fps);
    ui_draw_text(fb, badge_x + 18, badge_y + 9, fps_text, color, 2);
}