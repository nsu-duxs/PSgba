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

enum {
    OSD_ITEM_RESUME = 0,
    OSD_ITEM_AUDIO_FOCUS,
    OSD_ITEM_QUICK_SAVE,
    OSD_ITEM_QUICK_LOAD,
    OSD_ITEM_GROUP_SAVE,
    OSD_ITEM_GROUP_LOAD,
    OSD_ITEM_RESET_LINK,
    OSD_ITEM_RETURN_LAUNCHER,
    OSD_ITEM_COUNT
};

static UIAppState s_app_state = UI_STATE_LAUNCHER;
static int s_num_players = 4;
static UIRomMode s_rom_mode = UI_ROM_MODE_SHARED;

static char s_rom_files[UI_MAX_ROMS][UI_MAX_FILENAME];
static int s_rom_count = 0;

static int s_launcher_cursor = 0; /* 0 = Players, 1 = ROM Mode, 2+ = ROM list */
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

void ui_menu_scan_roms(void) {
    s_rom_count = 0;
    memset(s_rom_files, 0, sizeof(s_rom_files));

    DIR* dir = opendir(PS4_ROMS_DIR);
    if (dir) {
        struct dirent* entry;
        while ((entry = readdir(dir)) != NULL && s_rom_count < UI_MAX_ROMS) {
            if (entry->d_name[0] == '.') {
                continue;
            }
            if (has_gba_extension(entry->d_name)) {
                strncpy(s_rom_files[s_rom_count], entry->d_name, UI_MAX_FILENAME - 1);
                s_rom_files[s_rom_count][UI_MAX_FILENAME - 1] = '\0';
                s_rom_count++;
            }
        }
        closedir(dir);
    }

    if (s_rom_count == 0) {
        strncpy(s_rom_files[0], "test.gba", UI_MAX_FILENAME - 1);
        s_rom_count = 1;
    }
}

void ui_menu_init(void) {
    save_manager_init();
    ui_menu_scan_roms();
    s_app_state = UI_STATE_LAUNCHER;
    s_num_players = 4;
    s_rom_mode = UI_ROM_MODE_SHARED;
    s_launcher_cursor = 2;
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

static void build_rom_full_path(const char* filename, char* out_path, size_t max_len) {
    if (strcmp(filename, "test.gba") == 0) {
        /* Try /data/gba4p/roms/test.gba first, fallback to /data/roms/test.gba */
        snprintf(out_path, max_len, "%s/%s", PS4_ROMS_DIR, filename);
    } else {
        snprintf(out_path, max_len, "%s/%s", PS4_ROMS_DIR, filename);
    }
}

void ui_menu_update_launcher(struct PS4GBA_Context* ctx, uint32_t buttons_pressed) {
    (void)ctx;
    int total_items = 2 + s_rom_count;

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
    } else if (s_launcher_cursor >= 2) {
        if ((buttons_pressed & ORBIS_PAD_BUTTON_CIRCLE) &&
            s_rom_mode == UI_ROM_MODE_INDIVIDUAL && s_selecting_player > 0) {
            s_selecting_player--;
        }

        if (buttons_pressed & ORBIS_PAD_BUTTON_CROSS) {
            int rom_idx = s_launcher_cursor - 2;
            if (rom_idx >= 0 && rom_idx < s_rom_count) {
                if (s_rom_mode == UI_ROM_MODE_SHARED) {
                    char path[UI_MAX_PATH];
                    build_rom_full_path(s_rom_files[rom_idx], path, sizeof(path));
                    const char* paths[GBA_MAX_INSTANCES] = { path, path, path, path };
                    bool ok = emulator_init_session(paths, s_num_players);
                    if (!ok) {
                        const char* fallback[GBA_MAX_INSTANCES] = {
                            "/data/roms/test.gba", "/data/roms/test.gba",
                            "/data/roms/test.gba", "/data/roms/test.gba"
                        };
                        ok = emulator_init_session(fallback, s_num_players);
                    }
                    if (ok) {
                        s_launcher_status[0] = '\0';
                        s_app_state = UI_STATE_INGAME;
                    } else {
                        ui_menu_scan_roms();
                        snprintf(s_launcher_status, sizeof(s_launcher_status),
                                 "Coloque arquivos .gba em /data/gba4p/roms/ via FTP/USB e aperte [X]");
                    }
                } else {
                    build_rom_full_path(s_rom_files[rom_idx],
                                        s_selected_roms[s_selecting_player],
                                        sizeof(s_selected_roms[s_selecting_player]));
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
                                     "Erro ao abrir ROM. Verifique /data/gba4p/roms/");
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
    case OSD_ITEM_RESET_LINK:
        link_hub_reset();
        snprintf(s_osd_status, sizeof(s_osd_status), "Link Cable reiniciado!");
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

    ui_draw_text(fb, 120, 80, "PS4 GBA 4-WAY SPLIT-SCREEN EMULATOR", 0xFFFFFFFFU, 3);

    static const char* const layout_labels[5] = {
        "",
        "1P (Tela Cheia 1440x960 - 6x)",
        "2P (Split Side-by-Side 960x640 - 4x)",
        "3P (Quadrantes 2x2 720x480 - 3x)",
        "4P (Quadrantes 2x2 720x480 - 3x)"
    };

    char line_buf[256];
    snprintf(line_buf, sizeof(line_buf), "%s Jogadores Ativos: < %s >",
             (s_launcher_cursor == 0) ? ">" : " ", layout_labels[s_num_players]);
    ui_draw_text(fb, 120, 160, line_buf,
                 (s_launcher_cursor == 0) ? 0xFF00F5D4U : 0xFFCCCCCCU, 2);

    snprintf(line_buf, sizeof(line_buf), "%s Modo de ROM: < %s >",
             (s_launcher_cursor == 1) ? ">" : " ",
             (s_rom_mode == UI_ROM_MODE_SHARED)
                 ? "Mesma ROM para todos"
                 : "ROM individual por jogador");
    ui_draw_text(fb, 120, 200, line_buf,
                 (s_launcher_cursor == 1) ? 0xFF00F5D4U : 0xFFCCCCCCU, 2);

    if (s_rom_mode == UI_ROM_MODE_INDIVIDUAL) {
        snprintf(line_buf, sizeof(line_buf),
                 "Selecione a ROM para o JOGADOR %d de %d (/data/gba4p/roms/):",
                 s_selecting_player + 1, s_num_players);
    } else {
        snprintf(line_buf, sizeof(line_buf),
                 "ROMS disponiveis em /data/gba4p/roms/ (%d encontradas):", s_rom_count);
    }
    ui_draw_text(fb, 120, 260, line_buf, 0xFFFFBE0BU, 2);

    int max_visible = 15;
    int selected_rom = (s_launcher_cursor >= 2) ? (s_launcher_cursor - 2) : 0;
    int start_idx = (selected_rom >= max_visible) ? (selected_rom - max_visible + 1) : 0;

    for (int i = 0; i < max_visible && (start_idx + i) < s_rom_count; ++i) {
        int idx = start_idx + i;
        bool is_sel = (s_launcher_cursor == idx + 2);
        snprintf(line_buf, sizeof(line_buf), "%s [%02d] %s",
                 is_sel ? "->" : "  ", idx + 1, s_rom_files[idx]);
        ui_draw_text(fb, 140, 310 + i * 36, line_buf,
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

    int box_x = 460;
    int box_y = 200;
    int box_w = 1000;
    int box_h = 680;

    draw_rect_translucent(fb, box_x, box_y, box_w, box_h, 0x000A101AU);
    draw_rect_border(fb, box_x, box_y, box_w, box_h, 3, 0xFF00F5D4U);

    char buf[160];
    snprintf(buf, sizeof(buf), "MENU OSD - CONTROLE P%d (%s)",
             s_osd_player + 1, ctx ? ctx->rom_name : "");
    ui_draw_text(fb, box_x + 50, box_y + 40, buf, 0xFFFFBE0BU, 3);

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
        item_audio,
        item_qsave,
        item_qload,
        "Group Save State (4P + Link Cable)",
        "Group Load State (4P + Link Cable)",
        "Resetar Link Cable",
        "Voltar ao Seletor de ROMs"
    };

    for (int i = 0; i < OSD_ITEM_COUNT; ++i) {
        bool sel = (s_osd_cursor == i);
        snprintf(buf, sizeof(buf), "%s %s", sel ? "->" : "  ", items[i]);
        ui_draw_text(fb, box_x + 60, box_y + 120 + i * 54, buf,
                     sel ? 0xFF00F5D4U : 0xFFE0E0E0U, 2);
    }

    if (s_osd_status[0]) {
        ui_draw_text(fb, box_x + 60, box_y + 575, s_osd_status, 0xFF38B000U, 2);
    }

    ui_draw_text(fb, box_x + 60, box_y + 625,
                 "[X] Selecionar   [CIRCLE/OPTIONS] Fechar OSD", 0xFF8899AAU, 2);
}

