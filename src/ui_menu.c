#include "ui_menu.h"
#include "font8x8.h"
#include "video_split.h"
#include "audio_ps4.h"
#include "save_manager.h"
#include "link_cable_hub.h"
#include "ps4_jailbreak.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <dirent.h>
#include <sys/stat.h>
#include <orbis/Pad.h>

#define LAUNCHER_ROM_START_IDX 2

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
static bool s_show_fps = true;
static int s_join_hint_frames = 0;

static char s_rom_files[UI_MAX_ROMS][UI_MAX_FILENAME];
static char s_rom_paths[UI_MAX_ROMS][UI_MAX_PATH];
static int s_rom_count = 0;
static int s_usb_rom_count = 0;
static char s_usb_status_str[128] = "USB: Verificando portas /mnt/usb0..7...";

static int s_launcher_cursor = 0; /* 0 = Resolution, 1 = FPS, 2+ = ROM list */
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
    return (strcasecmp(dot, ".gba") == 0 ||
            strcasecmp(dot, ".agb") == 0 ||
            strcasecmp(dot, ".bin") == 0 ||
            strcasecmp(dot, ".zip") == 0 ||
            strcasecmp(dot, ".7z") == 0);
}

static bool is_system_or_junk_dir(const char* name) {
    if (!name || name[0] == '.') {
        return true; /* Ignora '.', '..' e arquivos/pastas ocultas */
    }
    if (strcasecmp(name, "$RECYCLE.BIN") == 0 ||
        strcasecmp(name, "System Volume Information") == 0 ||
        strcasecmp(name, "LOST.DIR") == 0 ||
        strcasecmp(name, "Android") == 0 ||
        strcasecmp(name, ".Spotlight-V100") == 0 ||
        strcasecmp(name, ".Trashes") == 0 ||
        strcasecmp(name, ".fseventsd") == 0) {
        return true;
    }
    return false;
}

static void scan_directory_recursive(const char* dir_path, int depth) {
    if (!dir_path || depth > 5 || s_rom_count >= UI_MAX_ROMS) {
        return;
    }

    DIR* dir = opendir(dir_path);
    if (!dir) {
        return;
    }

    size_t dlen = strlen(dir_path);
    bool has_trailing_slash = (dlen > 0 && dir_path[dlen - 1] == '/');

    struct dirent* entry;
    while ((entry = readdir(dir)) != NULL && s_rom_count < UI_MAX_ROMS) {
        if (!entry->d_name[0] || entry->d_name[0] == '.') {
            continue;
        }

        char full_path[UI_MAX_PATH];
        int len;
        if (has_trailing_slash) {
            len = snprintf(full_path, sizeof(full_path), "%s%s", dir_path, entry->d_name);
        } else {
            len = snprintf(full_path, sizeof(full_path), "%s/%s", dir_path, entry->d_name);
        }
        if (len <= 0 || len >= (int)sizeof(full_path)) {
            continue;
        }

        /* Determina se e diretorio para busca recursiva */
        bool is_dir = false;
        if (entry->d_type == DT_DIR) {
            is_dir = true;
        } else if (entry->d_type == DT_UNKNOWN || entry->d_type == DT_LNK) {
            /* Tenta opendir para deteccao fiavel de subpastas em exFAT/FAT32 sem depender de stat */
            DIR* sub = opendir(full_path);
            if (sub) {
                is_dir = true;
                closedir(sub);
            }
        }

        if (is_dir) {
            if (!is_system_or_junk_dir(entry->d_name)) {
                scan_directory_recursive(full_path, depth + 1);
            }
            continue;
        }

        /* E um arquivo: verifica se possui extensao compativel */
        if (has_gba_extension(entry->d_name)) {
            /* Evita duplicatas se o mesmo caminho for percorrido */
            bool duplicate = false;
            for (int i = 0; i < s_rom_count; ++i) {
                if (strcmp(s_rom_paths[i], full_path) == 0) {
                    duplicate = true;
                    break;
                }
            }
            if (!duplicate) {
                strncpy(s_rom_paths[s_rom_count], full_path, UI_MAX_PATH - 1);
                s_rom_paths[s_rom_count][UI_MAX_PATH - 1] = '\0';

                /* Rotula ROMs originarias do pendrive com [USB] para clareza visual */
                if (strncmp(full_path, "/mnt/usb", 8) == 0 ||
                    strncmp(full_path, "/usb", 4) == 0 ||
                    strncmp(full_path, "/mnt/ext", 8) == 0) {
                    snprintf(s_rom_files[s_rom_count], UI_MAX_FILENAME, "[USB] %s", entry->d_name);
                } else {
                    strncpy(s_rom_files[s_rom_count], entry->d_name, UI_MAX_FILENAME - 1);
                }
                s_rom_files[s_rom_count][UI_MAX_FILENAME - 1] = '\0';
                s_rom_count++;
            }
        }
    }
    closedir(dir);
}

static void sort_rom_list(void) {
    for (int i = 0; i < s_rom_count - 1; ++i) {
        for (int j = i + 1; j < s_rom_count; ++j) {
            if (strcasecmp(s_rom_files[i], s_rom_files[j]) > 0) {
                char tmp_f[UI_MAX_FILENAME];
                char tmp_p[UI_MAX_PATH];

                memcpy(tmp_f, s_rom_files[i], sizeof(tmp_f));
                memcpy(tmp_p, s_rom_paths[i], sizeof(tmp_p));

                memcpy(s_rom_files[i], s_rom_files[j], sizeof(tmp_f));
                memcpy(s_rom_paths[i], s_rom_paths[j], sizeof(tmp_p));

                memcpy(s_rom_files[j], tmp_f, sizeof(tmp_f));
                memcpy(s_rom_paths[j], tmp_p, sizeof(tmp_p));
            }
        }
    }
}

void ui_menu_scan_roms(void) {
    ps4_jailbreak_system();
    s_rom_count = 0;
    memset(s_rom_files, 0, sizeof(s_rom_files));
    memset(s_rom_paths, 0, sizeof(s_rom_paths));

    /* 1. Diretorios internos do PS4 */
    scan_directory_recursive(PS4_ROMS_DIR, 0);
    scan_directory_recursive("/data/roms", 0);
    scan_directory_recursive("/data/gba", 0);
    scan_directory_recursive("/data/homebrew/roms", 0);
    scan_directory_recursive("/data/homebrew/gba", 0);

    int ps4_roms = s_rom_count;
    char active_usb_ports[64] = "";
    int usb_ports_found = 0;

    /* 2. Todas as portas USB montadas pelo PS4 Orbis OS (/mnt/usb0 ate /mnt/usb7) */
    for (int u = 0; u < 8 && s_rom_count < UI_MAX_ROMS; ++u) {
        char usb_path[32];
        bool mounted = false;

        /* Teste 1: /mnt/usbX */
        snprintf(usb_path, sizeof(usb_path), "/mnt/usb%d", u);
        DIR* test = opendir(usb_path);
        if (test) {
            closedir(test);
            mounted = true;
        } else {
            /* Teste 2: /mnt/usbX/ */
            snprintf(usb_path, sizeof(usb_path), "/mnt/usb%d/", u);
            test = opendir(usb_path);
            if (test) {
                closedir(test);
                mounted = true;
            } else {
                /* Teste 3: Fallback raiz /usbX */
                snprintf(usb_path, sizeof(usb_path), "/usb%d", u);
                test = opendir(usb_path);
                if (test) {
                    closedir(test);
                    mounted = true;
                }
            }
        }

        if (mounted) {
            if (usb_ports_found == 0) {
                snprintf(active_usb_ports, sizeof(active_usb_ports), "/mnt/usb%d", u);
            }
            usb_ports_found++;
            scan_directory_recursive(usb_path, 0);
        }
    }

    /* 3. Disco externo USB (/mnt/ext0) se configurado */
    DIR* ext = opendir("/mnt/ext0");
    if (!ext) {
        ext = opendir("/mnt/ext0/");
    }
    if (ext) {
        closedir(ext);
        if (usb_ports_found == 0) {
            snprintf(active_usb_ports, sizeof(active_usb_ports), "/mnt/ext0");
        }
        usb_ports_found++;
        scan_directory_recursive("/mnt/ext0", 0);
    }

    s_usb_rom_count = s_rom_count - ps4_roms;

    if (usb_ports_found > 0) {
        if (s_usb_rom_count > 0) {
            snprintf(s_usb_status_str, sizeof(s_usb_status_str),
                     "USB: %s conectado (%d ROMs USB detectadas)",
                     active_usb_ports, s_usb_rom_count);
        } else {
            snprintf(s_usb_status_str, sizeof(s_usb_status_str),
                     "USB: %s montado, mas 0 ROMs (.gba/.zip) na raiz ou subpastas",
                     active_usb_ports);
        }
    } else {
        snprintf(s_usb_status_str, sizeof(s_usb_status_str),
                 "USB: Nenhum pendrive detectado em /mnt/usb0..7 (Formate FAT32/exFAT)");
    }

    /* Ordena a lista alfabeticamente para facil navegacao */
    if (s_rom_count > 1) {
        sort_rom_list();
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
    s_launcher_cursor = LAUNCHER_ROM_START_IDX;
    s_osd_cursor = 0;
    s_osd_player = 0;
    s_osd_status[0] = '\0';
    s_join_hint_frames = 0;
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

void ui_menu_trigger_join_hint(void) {
    s_join_hint_frames = 360; /* ~6 segundos a 60 FPS */
}

void ui_menu_render_join_hint(uint32_t* fb, struct PS4GBA_Context* ctx) {
    if (!fb || s_join_hint_frames <= 0) {
        return;
    }
    if (ctx && ctx->active_count >= GBA_MAX_INSTANCES) {
        s_join_hint_frames = 0;
        video_request_clear_framebuffers();
        return;
    }

    s_join_hint_frames--;
    if (s_join_hint_frames == 0) {
        video_request_clear_framebuffers();
        return;
    }

    /* Banner inferior: "Aperte [OPTIONS] para entrar com novo jogador" */
    int banner_w = 780;
    int banner_h = 44;
    int banner_x = (PS4_SCREEN_WIDTH - banner_w) / 2;
    int banner_y = 1010;

    draw_rect_translucent(fb, banner_x, banner_y, banner_w, banner_h, 0x00080E18U);
    draw_rect_border(fb, banner_x, banner_y, banner_w, banner_h, 2, 0xFF3A86FFU);

    int cur_x = banner_x + 24;
    int text_y = banner_y + 14;

    ui_draw_text(fb, cur_x, text_y, "Aperte ", 0xFFE2E8F0U, 2);
    cur_x += 7 * 16; /* "Aperte " = 112 px */

    /* Badge visual do botao OPTIONS estilo PS4 */
    int btn_w = 126;
    int btn_h = 26;
    int btn_x = cur_x;
    int btn_y = banner_y + 9;

    draw_rect_solid(fb, btn_x, btn_y, btn_w, btn_h, 0xFF1E293BU);
    draw_rect_border(fb, btn_x, btn_y, btn_w, btn_h, 1, 0xFF00F5D4U);
    ui_draw_text(fb, btn_x + 7, btn_y + 5, "OPTIONS", 0xFF00F5D4U, 2);
    cur_x += btn_w + 8;

    ui_draw_text(fb, cur_x, text_y, " para entrar com novo jogador", 0xFFE2E8F0U, 2);
}

void ui_menu_update_launcher(struct PS4GBA_Context* ctx, uint32_t buttons_pressed) {
    (void)ctx;

    /* Deteccao automatica periodica de insercao/remocao de pendrive a cada ~1s (60 frames) */
    static uint32_t s_usb_poll_tick = 0;
    static uint32_t s_last_usb_mask = 0xFFFFFFFFU;
    if ((++s_usb_poll_tick % 60U) == 0U) {
        uint32_t cur_mask = 0;
        for (int u = 0; u < 8; ++u) {
            char u_path[32];
            snprintf(u_path, sizeof(u_path), "/mnt/usb%d", u);
            DIR* d = opendir(u_path);
            if (!d) {
                snprintf(u_path, sizeof(u_path), "/mnt/usb%d/", u);
                d = opendir(u_path);
            }
            if (d) {
                cur_mask |= (1U << u);
                closedir(d);
            }
        }
        DIR* dext = opendir("/mnt/ext0");
        if (dext) {
            cur_mask |= (1U << 8);
            closedir(dext);
        }
        if (s_last_usb_mask == 0xFFFFFFFFU) {
            s_last_usb_mask = cur_mask;
        } else if (cur_mask != s_last_usb_mask) {
            s_last_usb_mask = cur_mask;
            ui_menu_scan_roms();
            snprintf(s_launcher_status, sizeof(s_launcher_status),
                     "Alteracao no USB detectada: %d ROM(s) encontradas", s_rom_count);
        }
    }

    if (buttons_pressed & ORBIS_PAD_BUTTON_TRIANGLE) {
        ui_menu_scan_roms();
        s_launcher_cursor = LAUNCHER_ROM_START_IDX;
        snprintf(s_launcher_status, sizeof(s_launcher_status),
                 "Varredura concluida: %d ROM(s) (%d USB, %d PS4 interno)",
                 s_rom_count, s_usb_rom_count, s_rom_count - s_usb_rom_count);
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
            video_cycle_res_mode(-1);
        }
        if (buttons_pressed & (ORBIS_PAD_BUTTON_RIGHT | ORBIS_PAD_BUTTON_CROSS)) {
            video_cycle_res_mode(1);
        }
    } else if (s_launcher_cursor == 1) {
        if (buttons_pressed & (ORBIS_PAD_BUTTON_LEFT | ORBIS_PAD_BUTTON_RIGHT | ORBIS_PAD_BUTTON_CROSS)) {
            s_show_fps = !s_show_fps;
        }
    } else if (s_launcher_cursor >= LAUNCHER_ROM_START_IDX) {
        if (buttons_pressed & ORBIS_PAD_BUTTON_CROSS) {
            int rom_idx = s_launcher_cursor - LAUNCHER_ROM_START_IDX;
            if (rom_idx >= 0 && rom_idx < s_rom_count) {
                const char* path = s_rom_paths[rom_idx];
                const char* paths[GBA_MAX_INSTANCES] = { path, path, path, path };
                /* Sessao sempre inicia em Tela Cheia 1P (Controles 2..4 entram dinamicamente via OPTIONS) */
                bool ok = emulator_init_session(paths, 1);
                if (ok) {
                    s_launcher_status[0] = '\0';
                    s_app_state = UI_STATE_INGAME;
                    ui_menu_trigger_join_hint();
                } else {
                    ui_menu_scan_roms();
                    snprintf(s_launcher_status, sizeof(s_launcher_status),
                             "Erro ao carregar ROM. Verifique os arquivos no Pendrive ou PS4");
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

    ui_draw_text(fb, 120, 76, "PS4 GBA 4-WAY SPLIT-SCREEN EMULATOR v1.04", 0xFFFFFFFFU, 3);

    char line_buf[256];
    snprintf(line_buf, sizeof(line_buf), "%s Resolucao / Qualidade: < %s >",
             (s_launcher_cursor == 0) ? ">" : " ",
             video_get_res_mode_name(video_get_res_mode()));
    ui_draw_text(fb, 120, 142, line_buf,
                 (s_launcher_cursor == 0) ? 0xFF00F5D4U : 0xFFCCCCCCU, 2);

    snprintf(line_buf, sizeof(line_buf), "%s Mostrar Contador de FPS: < %s >",
             (s_launcher_cursor == 1) ? ">" : " ",
             s_show_fps ? "Ligado (Canto Superior)" : "Desligado");
    ui_draw_text(fb, 120, 180, line_buf,
                 (s_launcher_cursor == 1) ? 0xFF00F5D4U : 0xFFCCCCCCU, 2);

    snprintf(line_buf, sizeof(line_buf),
             "ROMS disponiveis (%d encontradas - [TRIANGULO] Recarregar USB):", s_rom_count);
    ui_draw_text(fb, 120, 234, line_buf, 0xFFFFBE0BU, 2);

    int max_visible = 16;
    int selected_rom = (s_launcher_cursor >= LAUNCHER_ROM_START_IDX)
                           ? (s_launcher_cursor - LAUNCHER_ROM_START_IDX)
                           : 0;
    int start_idx = (selected_rom >= max_visible) ? (selected_rom - max_visible + 1) : 0;

    for (int i = 0; i < max_visible && (start_idx + i) < s_rom_count; ++i) {
        int idx = start_idx + i;
        bool is_sel = (s_launcher_cursor == idx + LAUNCHER_ROM_START_IDX);
        bool is_usb = (strncmp(s_rom_files[idx], "[USB]", 5) == 0);

        snprintf(line_buf, sizeof(line_buf), "%s [%02d] %s",
                 is_sel ? "->" : "  ", idx + 1, s_rom_files[idx]);

        uint32_t text_color = is_sel ? 0xFF00F5D4U : (is_usb ? 0xFF64DFDFU : 0xFFCBD5E1U);
        ui_draw_text(fb, 140, 276 + i * 36, line_buf, text_color, 2);
    }

    /* Informacoes de status do USB e varredura */
    uint32_t usb_color = (s_usb_rom_count > 0) ? 0xFF00F5D4U : 0xFFFFBE0BU;
    ui_draw_text(fb, 120, 874, s_usb_status_str, usb_color, 2);

    if (s_launcher_status[0]) {
        ui_draw_text(fb, 120, 914, s_launcher_status, 0xFFFF595EU, 2);
    }

    ui_draw_text(fb, 120, 960,
                 "[X] Iniciar 1P   [TRIANGULO] Forçar Varredura USB   [OPTIONS] Menu OSD",
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
    snprintf(buf, sizeof(buf), "MENU OSD v1.04 - CONTROLE P%d (%s)",
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
                 "[X / SETAS] Selecionar   [OPTIONS no Controle 2/3/4] Entrar no Jogo   [CIRCLE] Fechar",
                 0xFF8899AAU, 2);
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