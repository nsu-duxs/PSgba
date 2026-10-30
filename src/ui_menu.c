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
#include <math.h>
#include <orbis/Pad.h>

/* ==============================================================================
 * PALETA RETRO PIXEL ART RIGOROSA
 * ============================================================================== */
#define COLOR_BG               0xFF0B0E14U /* Fundo escuro azul-noite #0B0E14    */
#define COLOR_CARD_BG          0xFF0E1A2DU /* Fundo das caixas azul-noite        */
#define COLOR_BORDER_CYAN      0xFF00F5D4U /* Ciano Neon #00F5D4                 */
#define COLOR_BORDER_BLUE      0xFF3A86FFU /* Azul Neon retro #3A86FF            */
#define COLOR_BORDER_INNER     0xFF1D3557U /* Azul escuro moldura RPG 16-bit     */
#define COLOR_TEXT_GOLD        0xFFFFBE0BU /* Amarelo Dourado Teclas/Titulos     */
#define COLOR_TEXT_WHITE       0xFFFFFFFFU /* Branco Puro Descricoes             */
#define COLOR_TEXT_LIGHT       0xFFE2E8F0U /* Cinza claro #E2E8F0                */
#define COLOR_TEXT_MUTED       0xFF8D99AEU /* Cinza ardosia inativo #8D99AE      */
#define COLOR_TEXT_GREEN       0xFF38B000U /* Verde esmeralda sucesso #38B000    */
#define COLOR_TEXT_RED         0xFFFF595EU /* Vermelho coral alerta #FF595E      */
#define COLOR_TEXT_USB         0xFF48CAE4U /* Azul ciano pendrive USB            */
#define COLOR_ITEM_SEL_BG      0xFF1B3B6FU /* Fundo azul selecao                 */
#define COLOR_TAB_ACTIVE_BG    0xFF1B3B6FU /* Fundo aba ativa                    */
#define COLOR_TAB_INACTIVE_BG  0xFF0B132BU /* Fundo aba inativa                  */
#define COLOR_TAB_INACTIVE_BDR 0xFF334155U /* Borda aba inativa                  */
#define COLOR_CIRCUIT_LINE     0xFF1B4965U /* Ciano Escuro / Azul Metalico        */

/* Cores originais dos botoes do controle DualShock 4 */
#define COLOR_PS_CROSS         0xFF2E86DEU /* Cruz Azul #2E86DE                  */
#define COLOR_PS_CIRCLE        0xFFEE5253U /* Circulo Vermelho #EE5253           */
#define COLOR_PS_SQUARE        0xFFFF9FF3U /* Quadrado Rosa #FF9FF3              */
#define COLOR_PS_TRIANGLE      0xFF10AC84U /* Triangulo Verde #10AC84            */
#define COLOR_PS_DPAD          0xFF576574U /* D-Pad Cinza #576574                */
#define COLOR_PS_CHASSIS       0xFF1E272EU /* Corpo do controle #1E272E          */
#define COLOR_PS_CHASSIS_BDR   0xFF485460U /* Borda do chassi #485460            */
#define COLOR_PS_TRIGGER       0xFF34495EU /* Gatilhos L1/L2/R1/R2 #34495E       */

/* Cores da carcaça do Game Boy Advance Clássico */
#define COLOR_GBA_BODY         0xFF4E5478U /* Indigo clássico AGB-001          */
#define COLOR_GBA_BODY_DARK    0xFF2B2D42U /* Borda exterior carcaça AGB-001    */
#define COLOR_GBA_SCREEN_BEZEL 0xFF1B263BU /* Moldura escura da tela LCD       */
#define COLOR_GBA_SCREEN_LCD   0xFF2D6A4FU /* Matriz LCD verde-oliva retro      */
#define COLOR_GBA_BUTTON       0xFF9E2A2BU /* Botões A e B vinho / bordô        */
#define COLOR_GBA_TRIGGER      0xFFD1D8E0U /* Gatilhos L e R salientes chanfro  */

/* Itens da aba Video & Audio no Launcher */
enum {
    LAUNCHER_VA_DISPLAY = 0,
    LAUNCHER_VA_FILTER,
    LAUNCHER_VA_SCALE,
    LAUNCHER_VA_SMOOTH,
    LAUNCHER_VA_SHOW_FPS,
    LAUNCHER_VA_FPS_LIMIT,
    LAUNCHER_VA_AUDIO_MODE,
    LAUNCHER_VA_MENU_SFX,
    LAUNCHER_VA_COUNT
};

/* Itens da aba Saves no Menu OSD */
enum {
    OSD_SAVE_RESUME = 0,
    OSD_SAVE_QUICK_SAVE,
    OSD_SAVE_QUICK_LOAD,
    OSD_SAVE_GROUP_SAVE,
    OSD_SAVE_GROUP_LOAD,
    OSD_SAVE_RETURN_LAUNCHER,
    OSD_SAVE_COUNT
};

/* Itens da aba Video & Audio no Menu OSD */
enum {
    OSD_CFG_DISPLAY = 0,
    OSD_CFG_FILTER,
    OSD_CFG_SCALE,
    OSD_CFG_SMOOTH,
    OSD_CFG_SHOW_FPS,
    OSD_CFG_FPS_LIMIT,
    OSD_CFG_AUDIO_FOCUS,
    OSD_CFG_MENU_SFX,
    OSD_CFG_COUNT
};

/* Itens da aba Link Cable no Menu OSD */
enum {
    OSD_LINK_POWER = 0,
    OSD_LINK_HOTPLUG,
    OSD_LINK_MODE,
    OSD_LINK_DEV_TELEMETRY,
    OSD_LINK_COUNT
};

#define ROM_PAGE_SIZE 12

static UIAppState s_app_state = UI_STATE_SPLASH;
static int s_splash_frame = 0;
static bool s_show_fps = true;
static int s_join_hint_frames = 0;

/* Painel Lateral de Informacoes Extras (Oculto por padrao + toggle Touchpad) */
static bool s_show_side_panel = false;
static int s_side_panel_anim = 0; /* 0 = 100% oculto, 10 = 100% visivel (slide-in) */

/* Alternancia da visualizacao na aba Controles: false = DualShock 4, true = GBA Classico */
static bool s_controls_view_gba = false;

/* Telemetria avancada de Link Cable (Oculta por padrao) */
static bool s_show_dev_telemetry = false;

static char s_rom_files[UI_MAX_ROMS][UI_MAX_FILENAME];
static char s_rom_paths[UI_MAX_ROMS][UI_MAX_PATH];
static int s_rom_count = 0;
static int s_usb_rom_count = 0;
static char s_usb_status_str[128] = "USB: Inicializando portas /mnt/usb0..7...";

/* Estado de navegacao do Launcher */
static int s_launcher_tab = LAUNCHER_TAB_GAMES;
static int s_launcher_rom_cursor = 0;
static int s_launcher_va_cursor = 0;
static char s_launcher_status[128] = "";

/* Estado de navegacao do Menu OSD */
static int s_osd_tab = OSD_TAB_SAVES;
static int s_osd_saves_cursor = 0;
static int s_osd_cfg_cursor = 0;
static int s_osd_link_cursor = 0;
static int s_osd_player = 0;
static char s_osd_status[128] = "";

/* ==============================================================================
 * PRIMITIVAS DE RENDERIZACAO E TIPOGRAFIA
 * ============================================================================== */

static inline void draw_rect_solid(uint32_t* fb, int x, int y, int w, int h, uint32_t color) {
    if (!fb || w <= 0 || h <= 0) {
        return;
    }
    if (x < 0) {
        w += x;
        x = 0;
    }
    if (y < 0) {
        h += y;
        y = 0;
    }
    if (x + w > PS4_SCREEN_WIDTH) {
        w = PS4_SCREEN_WIDTH - x;
    }
    if (y + h > PS4_SCREEN_HEIGHT) {
        h = PS4_SCREEN_HEIGHT - y;
    }
    if (w <= 0 || h <= 0) {
        return;
    }

    for (int py = y; py < y + h; ++py) {
        uint32_t* dst = &fb[py * PS4_SCREEN_WIDTH + x];
        int px = 0;
        for (; px <= w - 8; px += 8) {
            dst[px + 0] = color;
            dst[px + 1] = color;
            dst[px + 2] = color;
            dst[px + 3] = color;
            dst[px + 4] = color;
            dst[px + 5] = color;
            dst[px + 6] = color;
            dst[px + 7] = color;
        }
        for (; px < w; ++px) {
            dst[px] = color;
        }
    }
}

static inline void draw_rect_border(uint32_t* fb, int x, int y, int w, int h, int thickness, uint32_t color) {
    draw_rect_solid(fb, x, y, w, thickness, color);
    draw_rect_solid(fb, x, y + h - thickness, w, thickness, color);
    draw_rect_solid(fb, x, y, thickness, h, color);
    draw_rect_solid(fb, x + w - thickness, y, thickness, h, color);
}

static inline void draw_box_rpg(uint32_t* fb, int x, int y, int w, int h, uint32_t bg_color, uint32_t border_color) {
    draw_rect_solid(fb, x, y, w, h, bg_color);
    draw_rect_border(fb, x, y, w, h, 2, border_color);
    if (w > 12 && h > 12) {
        draw_rect_border(fb, x + 4, y + 4, w - 8, h - 8, 1, COLOR_BORDER_INNER);
    }
}

static inline void draw_line_h(uint32_t* fb, int x, int y, int len, int thickness, uint32_t color) {
    draw_rect_solid(fb, x, y, len, thickness, color);
}

static inline void draw_line_v(uint32_t* fb, int x, int y, int len, int thickness, uint32_t color) {
    draw_rect_solid(fb, x, y, thickness, len, color);
}

/* Truncamento seguro: garante que texto + reticencias caibam exatamente em max_width */
static void ui_draw_text_truncated(uint32_t* fb, int x, int y, const char* text,
                                   uint32_t color, int scale, int max_width) {
    if (!fb || !text || scale <= 0 || max_width <= 0) {
        return;
    }

    int char_w = 8 * scale;
    int len = (int)strlen(text);
    int total_px = len * char_w;

    if (total_px <= max_width) {
        ui_draw_text(fb, x, y, text, color, scale);
        return;
    }

    int dots_px = 3 * char_w;
    if (max_width < dots_px) {
        int fit_chars = max_width / char_w;
        if (fit_chars <= 0) {
            return;
        }
        char temp[64];
        if (fit_chars > (int)sizeof(temp) - 1) {
            fit_chars = (int)sizeof(temp) - 1;
        }
        strncpy(temp, text, fit_chars);
        temp[fit_chars] = '\0';
        ui_draw_text(fb, x, y, temp, color, scale);
        return;
    }

    int avail_chars = (max_width - dots_px) / char_w;
    if (avail_chars < 0) {
        avail_chars = 0;
    }

    char buf[256];
    if (avail_chars > (int)sizeof(buf) - 4) {
        avail_chars = (int)sizeof(buf) - 4;
    }
    strncpy(buf, text, avail_chars);
    buf[avail_chars] = '.';
    buf[avail_chars + 1] = '.';
    buf[avail_chars + 2] = '.';
    buf[avail_chars + 3] = '\0';

    ui_draw_text(fb, x, y, buf, color, scale);
}

/* Quebra de linha automatica em palavras para explicacoes completas (zero truncamento) */
static int ui_draw_text_wrapped(uint32_t* fb, int x, int y, const char* text,
                                uint32_t color, int scale, int max_width, int line_spacing) {
    if (!fb || !text || scale <= 0 || max_width <= 0) {
        return 0;
    }

    int char_w = 8 * scale;
    int max_chars_per_line = max_width / char_w;
    if (max_chars_per_line <= 0) {
        return 0;
    }

    const char* ptr = text;
    int cur_y = y;

    while (*ptr) {
        while (*ptr == ' ') {
            ptr++;
        }
        if (!*ptr) {
            break;
        }

        int line_len = 0;
        const char* line_start = ptr;
        const char* last_space = NULL;

        while (*ptr && *ptr != '\n' && line_len < max_chars_per_line) {
            if (*ptr == ' ') {
                last_space = ptr;
            }
            ptr++;
            line_len++;
        }

        if (*ptr == '\n') {
            int count = (int)(ptr - line_start);
            char buf[128];
            if (count > (int)sizeof(buf) - 1) {
                count = (int)sizeof(buf) - 1;
            }
            memcpy(buf, line_start, count);
            buf[count] = '\0';
            ui_draw_text(fb, x, cur_y, buf, color, scale);
            cur_y += line_spacing;
            ptr++;
            continue;
        }

        if (*ptr == '\0') {
            ui_draw_text(fb, x, cur_y, line_start, color, scale);
            cur_y += line_spacing;
            break;
        }

        if (last_space && last_space > line_start) {
            int count = (int)(last_space - line_start);
            char buf[128];
            if (count > (int)sizeof(buf) - 1) {
                count = (int)sizeof(buf) - 1;
            }
            memcpy(buf, line_start, count);
            buf[count] = '\0';
            ui_draw_text(fb, x, cur_y, buf, color, scale);
            cur_y += line_spacing;
            ptr = last_space + 1;
        } else {
            char buf[128];
            int count = max_chars_per_line;
            if (count > (int)sizeof(buf) - 1) {
                count = (int)sizeof(buf) - 1;
            }
            memcpy(buf, line_start, count);
            buf[count] = '\0';
            ui_draw_text(fb, x, cur_y, buf, color, scale);
            cur_y += line_spacing;
        }
    }

    return cur_y - y;
}

static void draw_button_badge(uint32_t* fb, int cx, int cy, int size, uint32_t bg_color,
                              uint32_t border_color, const char* label, uint32_t text_color) {
    int x = cx - size / 2;
    int y = cy - size / 2;
    draw_rect_solid(fb, x, y, size, size, bg_color);
    draw_rect_border(fb, x, y, size, size, 2, border_color);
    if (label && label[0]) {
        int tlen = (int)strlen(label);
        int tx = cx - (tlen * 8) / 2;
        int ty = cy - 4;
        ui_draw_text(fb, tx, ty, label, text_color, 1);
    }
}

/* ==============================================================================
 * BARRA DE ABAS RIGOROSA ([L1] E [R1] PERFEITAMENTE CENTRALIZADOS NAS SUAS CAIXAS)
 * ============================================================================== */

static void draw_tab_bar(uint32_t* fb, int x, int y, int total_w, const char* tabs[],
                         int count, int active_tab) {
    if (!fb || !tabs || count <= 0 || total_w <= 0) {
        return;
    }

    int btn_w = 84;
    int btn_h = 36;
    int tab_h = 36;
    int gap = 10;

    /* Botao [L1] alinhado a esquerda com texto rigorosamente centralizado */
    draw_rect_solid(fb, x, y, btn_w, btn_h, 0xFF1E293BU);
    draw_rect_border(fb, x, y, btn_w, btn_h, 1, COLOR_BORDER_CYAN);
    int l1_len = 4; /* "[L1]" possui 4 caracteres */
    int l1_tx = x + (btn_w - (l1_len * 16)) / 2;
    int l1_ty = y + (btn_h - 16) / 2;
    ui_draw_text(fb, l1_tx, l1_ty, "[L1]", COLOR_BORDER_CYAN, 2);

    /* Botao [R1] alinhado a extrema direita com folga garantida > 30px da borda */
    int r1_x = x + total_w - btn_w;
    draw_rect_solid(fb, r1_x, y, btn_w, btn_h, 0xFF1E293BU);
    draw_rect_border(fb, r1_x, y, btn_w, btn_h, 1, COLOR_BORDER_CYAN);
    int r1_len = 4; /* "[R1]" possui 4 caracteres */
    int r1_tx = r1_x + (btn_w - (r1_len * 16)) / 2;
    int r1_ty = y + (btn_h - 16) / 2;
    ui_draw_text(fb, r1_tx, r1_ty, "[R1]", COLOR_BORDER_CYAN, 2);

    /* Espaco util distribuido uniformemente entre as abas centrais */
    int tabs_start_x = x + btn_w + gap;
    int tabs_avail_w = (r1_x - gap) - tabs_start_x;
    int tab_w = (tabs_avail_w - (count - 1) * gap) / count;

    int cur_x = tabs_start_x;
    for (int i = 0; i < count; ++i) {
        bool is_active = (i == active_tab);
        uint32_t bg = is_active ? COLOR_TAB_ACTIVE_BG : COLOR_TAB_INACTIVE_BG;
        uint32_t bdr = is_active ? COLOR_BORDER_CYAN : COLOR_TAB_INACTIVE_BDR;
        uint32_t txt = is_active ? COLOR_TEXT_GOLD : COLOR_TEXT_MUTED;

        draw_rect_solid(fb, cur_x, y, tab_w, tab_h, bg);
        draw_rect_border(fb, cur_x, y, tab_w, tab_h, is_active ? 2 : 1, bdr);

        int text_len = (int)strlen(tabs[i]);
        int text_px = text_len * 16;
        int tx = cur_x + (tab_w - text_px) / 2;
        int ty = y + (tab_h - 16) / 2;
        if (tx < cur_x + 4) {
            tx = cur_x + 4;
        }
        ui_draw_text_truncated(fb, tx, ty, tabs[i], txt, 2, tab_w - 8);

        cur_x += tab_w + gap;
    }
}

/* ==============================================================================
 * TELA VISUAL DE CONTROLE E ATALHOS (PADRONIZACAO RETRO, CORES E CIRCUITOS 2D)
 * ============================================================================== */

/* Retorna a cor oficial da barra de luz (lightbar) por jogador */
static uint32_t get_player_lightbar_color(int player) {
    switch (player) {
    case 1:
        return 0xFFEE5253U; /* P2: Vermelho #EE5253 */
    case 2:
        return 0xFF10AC84U; /* P3: Verde    #10AC84 */
    case 3:
        return 0xFFE056FDU; /* P4: Rosa     #E056FD */
    case 0:
    default:
        return 0xFF2E86DEU; /* P1: Azul     #2E86DE */
    }
}

/* Desenha card de comando com padronizacao cromatica rigorosa (sem truncamento) */
static void draw_control_card(uint32_t* fb, int x, int y, int w, int h,
                              const char* key_title, const char* gba_desc) {
    /* Fundo das Caixas: Azul-noite com moldura solida de 2px em Ciano */
    draw_rect_solid(fb, x, y, w, h, COLOR_CARD_BG);
    draw_rect_border(fb, x, y, w, h, 2, COLOR_BORDER_CYAN);

    /* Linha 1: Titulo / Teclas do PS4 em Amarelo Dourado + Seta em Ciano Neon */
    int ty1 = y + 6;
    int cur_x = x + 12;
    ui_draw_text(fb, cur_x, ty1, key_title, COLOR_TEXT_GOLD, 2);
    cur_x += (int)strlen(key_title) * 16;

    ui_draw_text(fb, cur_x, ty1, " ->", COLOR_BORDER_CYAN, 2);

    /* Linha 2: Descricao da Funcao no GBA em Branco Puro integral */
    int ty2 = y + 25;
    ui_draw_text(fb, x + 12, ty2, gba_desc, COLOR_TEXT_WHITE, 2);
}

/* Tracado de linha ortogonal em angulo reto estilo circuito eletronico / blueprint */
static void draw_circuit_line(uint32_t* fb, int x1, int y1, int mid_x, int x2, int y2, uint32_t color) {
    /* 1. Segmento horizontal saindo do pino do botao na silhueta do DualShock 4 */
    int min_x1 = (x1 < mid_x) ? x1 : mid_x;
    int len1 = abs(mid_x - x1);
    draw_line_h(fb, min_x1, y1 - 1, len1 + 1, 2, color);

    /* 2. Segmento vertical ate a altura central da caixa correspondente */
    int min_y = (y1 < y2) ? y1 : y2;
    int len_v = abs(y2 - y1);
    draw_line_v(fb, mid_x - 1, min_y, len_v + 1, 2, color);

    /* 3. Segmento horizontal conectando diretamente na lateral da caixa */
    int min_x2 = (mid_x < x2) ? mid_x : x2;
    int len2 = abs(x2 - mid_x);
    draw_line_h(fb, min_x2, y2 - 1, len2 + 1, 2, color);

    /* Ponto terminal / pino de conexao na borda da caixa */
    int pin_x = (x2 < x1) ? (x2 - 2) : (x2 - 1);
    draw_rect_solid(fb, pin_x, y2 - 3, 4, 6, COLOR_BORDER_CYAN);
    /* Ponto terminal de origem no botao */
    draw_rect_solid(fb, x1 - 2, y1 - 2, 4, 4, COLOR_TEXT_GOLD);
}

/* Visualizacao 1: DualShock 4 -> GBA (Sem cruzamento de linhas) */
static void draw_graphical_dualshock4_tutorial(uint32_t* fb, int cx, int cy) {
    int card_w = 710;
    int card_h = 46;

    /* 1. Coluna Esquerda: Caixas alinhadas verticalmente com a posicao fisica dos botoes */
    int col_l_x = 40;
    int card_l1_y  = 390; /* 1. Topo: [L1 / L2] */
    int card_sel_y = 462; /* 2. Media-alta: [TOUCHPAD / TRIANGULO] */
    int card_dp_y  = 534; /* 3. Media-baixa: [D-PAD / ANALOG ESQ] */
    int card_aud_y = 606; /* 4. Base: [L3 + R3] */

    draw_control_card(fb, col_l_x, card_l1_y, card_w, card_h,
                      "[L1 / L2]", "Botao L");

    draw_control_card(fb, col_l_x, card_sel_y, card_w, card_h,
                      "[TOUCHPAD / TRIANGULO]", "Botao SELECT GBA");

    draw_control_card(fb, col_l_x, card_dp_y, card_w, card_h,
                      "[D-PAD / ANALOG ESQ]", "Movimento GBA");

    draw_control_card(fb, col_l_x, card_aud_y, card_w, card_h,
                      "[L3 + R3]", "Alternar Foco de Audio");

    /* 2. Coluna Direita: Caixas alinhadas verticalmente com a posicao fisica dos botoes */
    int col_r_x = 1170;
    int card_r1_y = 390; /* 1. Topo: [R1 / R2] */
    int card_op_y = 444; /* 2. Media-alta: [OPTIONS] */
    int card_b_y  = 498; /* 3. Botoes frontais: [QUADRADO / CIRCULO] */
    int card_a_y  = 552; /* 4. Abaixo frontais: [CRUZ (X)] */
    int card_st_y = 606; /* 5. Base: [R3 (CLICK DIR)] */

    draw_control_card(fb, col_r_x, card_r1_y, card_w, card_h,
                      "[R1 / R2]", "Botao R");

    draw_control_card(fb, col_r_x, card_op_y, card_w, card_h,
                      "[OPTIONS]", "Abrir Menu / Entrar no Jogo");

    draw_control_card(fb, col_r_x, card_b_y, card_w, card_h,
                      "[QUADRADO / BOLA]", "Botao B");

    draw_control_card(fb, col_r_x, card_a_y, card_w, card_h,
                      "[CRUZ (X)]", "Botao A");

    draw_control_card(fb, col_r_x, card_st_y, card_w, card_h,
                      "[R3]", "Botao START GBA");

    /* 3. Bloco Central: Silhueta Grafica 2D do DualShock 4 (Centralizado em cx = 960) */
    draw_rect_solid(fb, cx - 140, cy - 70, 280, 140, COLOR_PS_CHASSIS);
    draw_rect_border(fb, cx - 140, cy - 70, 280, 140, 2, COLOR_PS_CHASSIS_BDR);

    /* Empunhaduras laterais */
    draw_rect_solid(fb, cx - 190, cy - 45, 52, 90, COLOR_PS_CHASSIS);
    draw_rect_border(fb, cx - 190, cy - 45, 52, 90, 2, COLOR_PS_CHASSIS_BDR);
    draw_rect_solid(fb, cx - 180, cy + 45, 42, 80, COLOR_PS_CHASSIS);
    draw_rect_border(fb, cx - 180, cy + 45, 42, 80, 2, COLOR_PS_CHASSIS_BDR);

    draw_rect_solid(fb, cx + 138, cy - 45, 52, 90, COLOR_PS_CHASSIS);
    draw_rect_border(fb, cx + 138, cy - 45, 52, 90, 2, COLOR_PS_CHASSIS_BDR);
    draw_rect_solid(fb, cx + 138, cy + 45, 42, 80, COLOR_PS_CHASSIS);
    draw_rect_border(fb, cx + 138, cy + 45, 42, 80, 2, COLOR_PS_CHASSIS_BDR);

    /* Parte inferior central */
    draw_rect_solid(fb, cx - 50, cy + 50, 100, 25, COLOR_BG);
    draw_rect_border(fb, cx - 50, cy + 50, 100, 25, 2, COLOR_PS_CHASSIS_BDR);

    /* Touchpad e Barra de Luz DINAMICA por Jogador */
    draw_rect_solid(fb, cx - 55, cy - 65, 110, 52, 0xFF2C3E50U);
    draw_rect_border(fb, cx - 55, cy - 65, 110, 52, 1, 0xFF576574U);

    uint32_t lightbar_col = get_player_lightbar_color(s_osd_player);
    draw_rect_solid(fb, cx - 45, cy - 69, 90, 4, lightbar_col);
    ui_draw_text(fb, cx - 32, cy - 40, "TOUCHPAD", COLOR_TEXT_MUTED, 1);

    /* Gatilhos L1/L2 e R1/R2 */
    draw_rect_solid(fb, cx - 150, cy - 90, 55, 20, COLOR_PS_TRIGGER);
    draw_rect_border(fb, cx - 150, cy - 90, 55, 20, 2, 0xFF8395A7U);
    ui_draw_text(fb, cx - 138, cy - 85, "L1/L2", COLOR_TEXT_WHITE, 1);

    draw_rect_solid(fb, cx + 95, cy - 90, 55, 20, COLOR_PS_TRIGGER);
    draw_rect_border(fb, cx + 95, cy - 90, 55, 20, 2, 0xFF8395A7U);
    ui_draw_text(fb, cx + 107, cy - 85, "R1/R2", COLOR_TEXT_WHITE, 1);

    /* D-Pad direcional na esquerda */
    int dpad_cx = cx - 90, dpad_cy = cy - 8;
    draw_rect_solid(fb, dpad_cx - 9, dpad_cy - 28, 18, 56, COLOR_PS_DPAD);
    draw_rect_solid(fb, dpad_cx - 28, dpad_cy - 9, 56, 18, COLOR_PS_DPAD);
    draw_rect_border(fb, dpad_cx - 9, dpad_cy - 28, 18, 56, 1, 0xFF222F3EU);
    draw_rect_border(fb, dpad_cx - 28, dpad_cy - 9, 56, 18, 1, 0xFF222F3EU);
    draw_rect_solid(fb, dpad_cx - 5, dpad_cy - 5, 10, 10, COLOR_PS_CHASSIS);

    ui_draw_text(fb, dpad_cx - 3, dpad_cy - 24, "^", COLOR_TEXT_WHITE, 1);
    ui_draw_text(fb, dpad_cx - 3, dpad_cy + 16, "v", COLOR_TEXT_WHITE, 1);
    ui_draw_text(fb, dpad_cx - 23, dpad_cy - 4, "<", COLOR_TEXT_WHITE, 1);
    ui_draw_text(fb, dpad_cx + 17, dpad_cy - 4, ">", COLOR_TEXT_WHITE, 1);

    /* Botoes de Acao (Triangulo, Circulo, Cruz, Quadrado) com cores oficiais */
    int btn_cx = cx + 90, btn_cy = cy - 8;
    int r = 24;
    draw_button_badge(fb, btn_cx, btn_cy - r, 20, COLOR_PS_TRIANGLE, 0xFF1DD1A1U, "/\\", COLOR_TEXT_WHITE);
    draw_button_badge(fb, btn_cx + r, btn_cy, 20, COLOR_PS_CIRCLE, 0xFFFF6B6BU, "O", COLOR_TEXT_WHITE);
    draw_button_badge(fb, btn_cx, btn_cy + r, 20, COLOR_PS_CROSS, 0xFF54A0FFU, "X", COLOR_TEXT_WHITE);
    draw_button_badge(fb, btn_cx - r, btn_cy, 20, COLOR_PS_SQUARE, 0xFFFDA7DFU, "[]", COLOR_TEXT_WHITE);

    /* Alavancas analogicas L3 e R3 */
    draw_button_badge(fb, cx - 45, cy + 32, 34, 0xFF2F3542U, 0xFF576574U, "L3", COLOR_BORDER_CYAN);
    draw_button_badge(fb, cx + 45, cy + 32, 34, 0xFF2F3542U, 0xFF576574U, "R3", COLOR_BORDER_CYAN);

    /* Botoes Share e Options */
    draw_rect_solid(fb, cx - 75, cy - 40, 14, 20, 0xFF485460U);
    ui_draw_text(fb, cx - 87, cy - 52, "SHARE", COLOR_TEXT_MUTED, 1);

    draw_rect_solid(fb, cx + 60, cy - 40, 14, 20, 0xFF485460U);
    ui_draw_text(fb, cx + 48, cy - 52, "OPTIONS", COLOR_TEXT_MUTED, 1);

    /* 4. Linhas de Circuito SEM CRUZAMENTO (Em linha reta / degrau paralelo) */
    int card_edge_l = col_l_x + card_w; /* 750 */
    int card_edge_r = col_r_x;          /* 1170 */

    /* Conexao L1/L2 -> Card 1 (Quase horizontal reta no topo) */
    draw_circuit_line(fb, cx - 140, cy - 85, 780, card_edge_l, card_l1_y + card_h / 2, COLOR_CIRCUIT_LINE);

    /* Conexao Touchpad/Select -> Card 2 (Saindo do Touchpad) */
    draw_circuit_line(fb, cx - 55, cy - 40, 795, card_edge_l, card_sel_y + card_h / 2, COLOR_CIRCUIT_LINE);

    /* Conexao D-PAD -> Card 3 (Direcional) */
    draw_circuit_line(fb, dpad_cx - 28, dpad_cy, 785, card_edge_l, card_dp_y + card_h / 2, COLOR_CIRCUIT_LINE);

    /* Conexao L3 + R3 -> Card 4 (Ponte entre L3 e R3 na base) */
    draw_line_h(fb, cx - 45, cy + 32, 90, 2, COLOR_CIRCUIT_LINE);
    draw_circuit_line(fb, cx - 45 - 17, cy + 32, 775, card_edge_l, card_aud_y + card_h / 2, COLOR_CIRCUIT_LINE);

    /* Conexao R1/R2 -> Card 1 (Quase horizontal reta no topo) */
    draw_circuit_line(fb, cx + 140, cy - 85, 1135, card_edge_r, card_r1_y + card_h / 2, COLOR_CIRCUIT_LINE);

    /* Conexao Options -> Card 2 (Saindo direto do botao Options) */
    draw_circuit_line(fb, cx + 60 + 8, cy - 40, 1125, card_edge_r, card_op_y + card_h / 2, COLOR_CIRCUIT_LINE);

    /* Conexao Quadrado e Circulo -> Card 3 (Ponte visual entre Quadrado e Circulo) */
    draw_line_h(fb, btn_cx - r, btn_cy, 2 * r, 2, COLOR_CIRCUIT_LINE);
    draw_circuit_line(fb, btn_cx + r + 10, btn_cy, 1115, card_edge_r, card_b_y + card_h / 2, COLOR_CIRCUIT_LINE);

    /* Conexao Cruz (A) -> Card 4 */
    draw_circuit_line(fb, btn_cx, btn_cy + r + 10, 1130, card_edge_r, card_a_y + card_h / 2, COLOR_CIRCUIT_LINE);

    /* Conexao R3 (START GBA) -> Card 5 (Base) */
    draw_circuit_line(fb, cx + 45 + 17, cy + 32, 1140, card_edge_r, card_st_y + card_h / 2, COLOR_CIRCUIT_LINE);

    /* 5. Banner Informativo na Base */
    int banner_x = 40;
    int banner_y = 860;
    int banner_w = 1840;
    int banner_h = 64;

    draw_box_rpg(fb, banner_x, banner_y, banner_w, banner_h, 0xFF14243BU, COLOR_BORDER_CYAN);
    ui_draw_text_truncated(fb, banner_x + 24, banner_y + 11,
                           "DICA MULTIPLAYER: Conecte ate 4 Controles DualShock 4 ao PS4.",
                           COLOR_TEXT_GOLD, 2, banner_w - 48);
    ui_draw_text_truncated(fb, banner_x + 24, banner_y + 36,
                           "Nos Controles 2, 3 e 4, aperte OPTIONS para entrar na partida a qualquer momento via Drop-In.",
                           COLOR_TEXT_USB, 2, banner_w - 48);
}

/* Visualizacao 2: Game Boy Advance Classico -> Mapeamento PS4 */
static void draw_graphical_gba_classic_tutorial(uint32_t* fb, int cx, int cy) {
    int card_w = 710;
    int card_h = 48;

    /* 1. Coluna Esquerda: Botoes GBA -> PS4 */
    int col_l_x = 40;
    int card_l1_y  = 390; /* 1. Topo: [Botao L GBA] */
    int card_dp_y  = 498; /* 2. Meio: [D-Pad GBA] */
    int card_sel_y = 606; /* 3. Base: [SELECT GBA] */

    draw_control_card(fb, col_l_x, card_l1_y, card_w, card_h,
                      "Botao L GBA", "L1 / L2");

    draw_control_card(fb, col_l_x, card_dp_y, card_w, card_h,
                      "D-Pad GBA", "D-Pad / Analogico Esquerdo");

    draw_control_card(fb, col_l_x, card_sel_y, card_w, card_h,
                      "SELECT GBA", "Touchpad / Triangulo");

    /* 2. Coluna Direita: Botoes GBA -> PS4 */
    int col_r_x = 1170;
    int card_r1_y = 390; /* 1. Topo: [Botao R GBA] */
    int card_a_y  = 462; /* 2. Meio-alto: [Botao A GBA] */
    int card_b_y  = 534; /* 3. Meio-baixo: [Botao B GBA] */
    int card_st_y = 606; /* 4. Base: [START GBA] */

    draw_control_card(fb, col_r_x, card_r1_y, card_w, card_h,
                      "Botao R GBA", "R1 / R2");

    draw_control_card(fb, col_r_x, card_a_y, card_w, card_h,
                      "Botao A GBA", "Cruz (X)");

    draw_control_card(fb, col_r_x, card_b_y, card_w, card_h,
                      "Botao B GBA", "Quadrado / Bola");

    draw_control_card(fb, col_r_x, card_st_y, card_w, card_h,
                      "START GBA", "R3");

    /* 3. Bloco Central: Game Boy Advance Classico (AGB-001) em Pixel Art 2D Fiel */
    /* Camadas da carcaça arredondada/ergonômica do AGB-001 (Largura ~340px, Altura ~184px) */
    draw_rect_solid(fb, cx - 130, cy - 92, 260, 184, COLOR_GBA_BODY);
    draw_rect_solid(fb, cx - 152, cy - 84, 304, 168, COLOR_GBA_BODY);
    draw_rect_solid(fb, cx - 170, cy - 68, 340, 136, COLOR_GBA_BODY);

    /* Empunhaduras laterais arredondadas anatômicas com textura sutil */
    draw_rect_solid(fb, cx - 168, cy - 60, 20, 120, 0xFF3D405BU);
    draw_rect_solid(fb, cx + 148, cy - 60, 20, 120, 0xFF3D405BU);

    /* Contorno de borda stepped em pixel art */
    draw_line_h(fb, cx - 130, cy - 92, 260, 2, COLOR_GBA_BODY_DARK);
    draw_line_h(fb, cx - 130, cy + 90, 260, 2, COLOR_GBA_BODY_DARK);

    draw_line_v(fb, cx - 130, cy - 92, 9, 2, COLOR_GBA_BODY_DARK);
    draw_line_h(fb, cx - 152, cy - 84, 23, 2, COLOR_GBA_BODY_DARK);
    draw_line_v(fb, cx - 152, cy - 84, 17, 2, COLOR_GBA_BODY_DARK);
    draw_line_h(fb, cx - 170, cy - 68, 19, 2, COLOR_GBA_BODY_DARK);
    draw_line_v(fb, cx - 170, cy - 68, 136, 2, COLOR_GBA_BODY_DARK);
    draw_line_h(fb, cx - 170, cy + 67, 19, 2, COLOR_GBA_BODY_DARK);
    draw_line_v(fb, cx - 152, cy + 67, 18, 2, COLOR_GBA_BODY_DARK);
    draw_line_h(fb, cx - 152, cy + 84, 23, 2, COLOR_GBA_BODY_DARK);
    draw_line_v(fb, cx - 130, cy + 84, 8, 2, COLOR_GBA_BODY_DARK);

    draw_line_v(fb, cx + 129, cy - 92, 9, 2, COLOR_GBA_BODY_DARK);
    draw_line_h(fb, cx + 129, cy - 84, 24, 2, COLOR_GBA_BODY_DARK);
    draw_line_v(fb, cx + 151, cy - 84, 17, 2, COLOR_GBA_BODY_DARK);
    draw_line_h(fb, cx + 151, cy - 68, 20, 2, COLOR_GBA_BODY_DARK);
    draw_line_v(fb, cx + 169, cy - 68, 136, 2, COLOR_GBA_BODY_DARK);
    draw_line_h(fb, cx + 151, cy + 67, 20, 2, COLOR_GBA_BODY_DARK);
    draw_line_v(fb, cx + 151, cy + 67, 18, 2, COLOR_GBA_BODY_DARK);
    draw_line_h(fb, cx + 129, cy + 84, 24, 2, COLOR_GBA_BODY_DARK);
    draw_line_v(fb, cx + 129, cy + 84, 8, 2, COLOR_GBA_BODY_DARK);

    /* Gatilhos L e R de ombro salientes com chanfro em cinza claro */
    int trig_w = 52, trig_h = 18;
    int trig_y = cy - 108;
    int trig_l_x = cx - 146;
    int trig_r_x = cx + 94;

    draw_rect_solid(fb, trig_l_x, trig_y, trig_w, trig_h, COLOR_GBA_TRIGGER);
    draw_rect_border(fb, trig_l_x, trig_y, trig_w, trig_h, 2, 0xFF95A5A6U);
    draw_line_h(fb, trig_l_x + 2, trig_y + 2, trig_w - 4, 2, 0xFFFFFFFFU);
    ui_draw_text(fb, trig_l_x + 22, trig_y + 5, "L", 0xFF2B2D42U, 1);

    draw_rect_solid(fb, trig_r_x, trig_y, trig_w, trig_h, COLOR_GBA_TRIGGER);
    draw_rect_border(fb, trig_r_x, trig_y, trig_w, trig_h, 2, 0xFF95A5A6U);
    draw_line_h(fb, trig_r_x + 2, trig_y + 2, trig_w - 4, 2, 0xFFFFFFFFU);
    ui_draw_text(fb, trig_r_x + 22, trig_y + 5, "R", 0xFF2B2D42U, 1);

    /* Bezel de vidro frontal escuro da tela */
    int bezel_w = 176, bezel_h = 126;
    int bezel_x = cx - bezel_w / 2;
    int bezel_y = cy - 70;
    draw_rect_solid(fb, bezel_x, bezel_y, bezel_w, bezel_h, COLOR_GBA_SCREEN_BEZEL);
    draw_rect_border(fb, bezel_x, bezel_y, bezel_w, bezel_h, 2, 0xFF0D1B2AU);
    ui_draw_text(fb, cx - 64, cy + 44, "GAME BOY ADVANCE", 0xFF8D99AEU, 1);

    /* Tela LCD TFT Proporcao 3:2 Nativa (150 x 100 pixels) em tom esmeralda retro */
    int lcd_w = 150, lcd_h = 100;
    int lcd_x = cx - lcd_w / 2;
    int lcd_y = cy - 58;
    draw_rect_solid(fb, lcd_x, lcd_y, lcd_w, lcd_h, COLOR_GBA_SCREEN_LCD);
    draw_rect_border(fb, lcd_x, lcd_y, lcd_w, lcd_h, 2, 0xFF1B4332U);
    ui_draw_text(fb, cx - 48, cy - 20, "GBA 4P", COLOR_TEXT_GOLD, 2);
    ui_draw_text(fb, cx - 48, cy + 6, "SPLIT-SCREEN", COLOR_BORDER_CYAN, 1);

    /* LED indicador de energia (Power) verde aceso */
    draw_rect_solid(fb, cx + 72, cy - 64, 6, 6, 0xFF00FF66U);
    draw_rect_border(fb, cx + 72, cy - 64, 6, 6, 1, 0xFF009933U);

    /* D-Pad Direcional classico em cruz cinza escuro na esquerda */
    int dpad_cx = cx - 122, dpad_cy = cy - 6;
    draw_rect_solid(fb, dpad_cx - 9, dpad_cy - 27, 18, 54, 0xFF2B2D42U);
    draw_rect_solid(fb, dpad_cx - 27, dpad_cy - 9, 54, 18, 0xFF2B2D42U);
    draw_rect_border(fb, dpad_cx - 9, dpad_cy - 27, 18, 54, 1, 0xFF14171FU);
    draw_rect_border(fb, dpad_cx - 27, dpad_cy - 9, 54, 18, 1, 0xFF14171FU);
    draw_rect_solid(fb, dpad_cx - 4, dpad_cy - 4, 8, 8, 0xFF3D405BU);

    ui_draw_text(fb, dpad_cx - 3, dpad_cy - 23, "^", 0xFF8D99AEU, 1);
    ui_draw_text(fb, dpad_cx - 3, dpad_cy + 15, "v", 0xFF8D99AEU, 1);
    ui_draw_text(fb, dpad_cx - 22, dpad_cy - 4, "<", 0xFF8D99AEU, 1);
    ui_draw_text(fb, dpad_cx + 15, dpad_cy - 4, ">", 0xFF8D99AEU, 1);

    /* Botoes A e B circulares inclinados em diagonal na direita (Vinho/Bordô) */
    int b_cx = cx + 104, b_cy = cy + 12;
    int a_cx = cx + 138, a_cy = cy - 14;
    draw_button_badge(fb, b_cx, b_cy, 26, COLOR_GBA_BUTTON, 0xFF5C1D24U, "B", COLOR_TEXT_WHITE);
    draw_button_badge(fb, a_cx, a_cy, 26, COLOR_GBA_BUTTON, 0xFF5C1D24U, "A", COLOR_TEXT_WHITE);

    /* Botoes SELECT e START emborrachados abaixo da tela */
    int sel_x = cx - 35, sel_y = cy + 64;
    int st_x  = cx + 15, st_y  = cy + 64;

    draw_rect_solid(fb, sel_x, sel_y, 24, 8, 0xFF2B2D42U);
    draw_rect_border(fb, sel_x, sel_y, 24, 8, 1, 0xFF14171FU);
    ui_draw_text(fb, sel_x - 10, sel_y + 11, "SELECT", COLOR_TEXT_MUTED, 1);

    draw_rect_solid(fb, st_x, st_y, 24, 8, 0xFF2B2D42U);
    draw_rect_border(fb, st_x, st_y, 24, 8, 1, 0xFF14171FU);
    ui_draw_text(fb, st_x + 1, st_y + 11, "START", COLOR_TEXT_MUTED, 1);

    /* Grelha de alto-falante na parte inferior direita */
    for (int s = 0; s < 9; ++s) {
        draw_rect_solid(fb, cx + 98 + (s % 3) * 6, cy + 54 + (s / 3) * 6, 2, 2, 0xFF14171FU);
    }

    /* 4. Linhas de Circuito Ortogonais Sem Cruzamentos */
    int card_edge_l = col_l_x + card_w; /* 750 */
    int card_edge_r = col_r_x;          /* 1170 */

    /* Conexao L GBA -> Card 1 */
    draw_circuit_line(fb, trig_l_x + trig_w / 2, trig_y + trig_h / 2, 785, card_edge_l, card_l1_y + card_h / 2, COLOR_CIRCUIT_LINE);

    /* Conexao D-Pad GBA -> Card 2 */
    draw_circuit_line(fb, dpad_cx - 27, dpad_cy, 795, card_edge_l, card_dp_y + card_h / 2, COLOR_CIRCUIT_LINE);

    /* Conexao SELECT GBA -> Card 3 (4 segmentos continuos) */
    int sel_pin_x = sel_x + 12;
    int sel_pin_y = sel_y + 8;
    int sel_exit_y = 604;
    int sel_guide_x = 775;
    int card_sel_center_y = card_sel_y + card_h / 2;

    draw_rect_solid(fb, sel_pin_x - 1, sel_pin_y - 1, 3, 3, COLOR_TEXT_GOLD);
    draw_line_v(fb, sel_pin_x - 1, sel_pin_y, sel_exit_y - sel_pin_y + 1, 2, COLOR_CIRCUIT_LINE);
    draw_line_h(fb, sel_guide_x - 1, sel_exit_y - 1, sel_pin_x - sel_guide_x + 2, 2, COLOR_CIRCUIT_LINE);
    draw_line_v(fb, sel_guide_x - 1, sel_exit_y - 1, card_sel_center_y - sel_exit_y + 2, 2, COLOR_CIRCUIT_LINE);
    draw_line_h(fb, card_edge_l, card_sel_center_y - 1, sel_guide_x - card_edge_l + 1, 2, COLOR_CIRCUIT_LINE);
    draw_rect_solid(fb, card_edge_l - 3, card_sel_center_y - 3, 4, 6, COLOR_BORDER_CYAN);

    /* Conexao R GBA -> Card 1 */
    draw_circuit_line(fb, trig_r_x + trig_w / 2, trig_y + trig_h / 2, 1140, card_edge_r, card_r1_y + card_h / 2, COLOR_CIRCUIT_LINE);

    /* Conexao Botao A GBA -> Card 2 */
    draw_circuit_line(fb, a_cx + 13, a_cy, 1130, card_edge_r, card_a_y + card_h / 2, COLOR_CIRCUIT_LINE);

    /* Conexao Botao B GBA -> Card 3 */
    draw_circuit_line(fb, b_cx + 13, b_cy, 1140, card_edge_r, card_b_y + card_h / 2, COLOR_CIRCUIT_LINE);

    /* Conexao START GBA -> Card 4 (4 segmentos ortogonais continuos sem cruzamento) */
    int start_pin_x = st_x + 12;
    int start_pin_y = st_y + 8;
    int start_exit_y = 604;
    int start_guide_x = 1150;
    int card_st_center_y = card_st_y + card_h / 2;

    draw_rect_solid(fb, start_pin_x - 1, start_pin_y - 1, 3, 3, COLOR_TEXT_GOLD);
    draw_line_v(fb, start_pin_x - 1, start_pin_y, start_exit_y - start_pin_y + 1, 2, COLOR_CIRCUIT_LINE);
    draw_line_h(fb, start_pin_x - 1, start_exit_y - 1, start_guide_x - start_pin_x + 2, 2, COLOR_CIRCUIT_LINE);
    draw_line_v(fb, start_guide_x - 1, start_exit_y - 1, card_st_center_y - start_exit_y + 2, 2, COLOR_CIRCUIT_LINE);
    draw_line_h(fb, start_guide_x - 1, card_st_center_y - 1, card_edge_r - start_guide_x + 1, 2, COLOR_CIRCUIT_LINE);
    draw_rect_solid(fb, card_edge_r - 1, card_st_center_y - 3, 4, 6, COLOR_BORDER_CYAN);

    /* 5. Banner Informativo na Base */
    int banner_x = 40;
    int banner_y = 860;
    int banner_w = 1840;
    int banner_h = 64;

    draw_box_rpg(fb, banner_x, banner_y, banner_w, banner_h, 0xFF14243BU, COLOR_BORDER_CYAN);
    ui_draw_text_truncated(fb, banner_x + 24, banner_y + 11,
                           "DICA MULTIPLAYER: Conecte ate 4 Controles DualShock 4 ao PS4.",
                           COLOR_TEXT_GOLD, 2, banner_w - 48);
    ui_draw_text_truncated(fb, banner_x + 24, banner_y + 36,
                           "Nos Controles 2, 3 e 4, aperte OPTIONS para entrar na partida a qualquer momento via Drop-In.",
                           COLOR_TEXT_USB, 2, banner_w - 48);
}

/* ==============================================================================
 * SISTEMA DE VARREDURA DE ROMS (APENAS NA INICIALIZACAO OU VIA TRIANGULO)
 * ============================================================================== */

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
        return true;
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

        bool is_dir = false;
        if (entry->d_type == DT_DIR) {
            is_dir = true;
        } else if (entry->d_type == DT_UNKNOWN || entry->d_type == DT_LNK) {
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

        if (has_gba_extension(entry->d_name)) {
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

    /* 1. Pastas internas do PS4 */
    scan_directory_recursive(PS4_ROMS_DIR, 0);
    scan_directory_recursive("/data/roms", 0);
    scan_directory_recursive("/data/gba", 0);
    scan_directory_recursive("/data/homebrew/roms", 0);
    scan_directory_recursive("/data/homebrew/gba", 0);

    int ps4_roms = s_rom_count;
    char active_usb_ports[64] = "";
    int usb_ports_found = 0;

    /* 2. Portas USB montadas (/mnt/usb0 ate /mnt/usb7) */
    for (int u = 0; u < 8 && s_rom_count < UI_MAX_ROMS; ++u) {
        char usb_path[32];
        bool mounted = false;

        snprintf(usb_path, sizeof(usb_path), "/mnt/usb%d", u);
        DIR* test = opendir(usb_path);
        if (test) {
            closedir(test);
            mounted = true;
        } else {
            snprintf(usb_path, sizeof(usb_path), "/mnt/usb%d/", u);
            test = opendir(usb_path);
            if (test) {
                closedir(test);
                mounted = true;
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

    /* 3. Disco externo USB (/mnt/ext0) se presente */
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
                     "USB: %s (%d ROMs USB detectadas)", active_usb_ports, s_usb_rom_count);
        } else {
            snprintf(s_usb_status_str, sizeof(s_usb_status_str),
                     "USB: %s montado (0 ROMs encontradas na raiz/pastas)", active_usb_ports);
        }
    } else {
        snprintf(s_usb_status_str, sizeof(s_usb_status_str),
                 "USB: Nenhum pendrive detectado (Formate em FAT32 ou exFAT)");
    }

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
    s_app_state = UI_STATE_SPLASH;
    s_splash_frame = 0;
    s_launcher_tab = LAUNCHER_TAB_GAMES;
    s_launcher_rom_cursor = 0;
    s_launcher_va_cursor = 0;
    s_launcher_status[0] = '\0';
    s_show_side_panel = false;
    s_side_panel_anim = 0;
    s_controls_view_gba = false;
    s_show_dev_telemetry = false;
    s_osd_tab = OSD_TAB_SAVES;
    s_osd_saves_cursor = 0;
    s_osd_cfg_cursor = 0;
    s_osd_link_cursor = 0;
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
    s_osd_tab = OSD_TAB_SAVES;
    s_osd_saves_cursor = 0;
    s_osd_status[0] = '\0';
    s_controls_view_gba = false;
    s_app_state = UI_STATE_OSD;
    audio_play_sfx(UI_SFX_CONFIRM);
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
    s_join_hint_frames = 360;
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

    int banner_w = 780;
    int banner_h = 44;
    int banner_x = (PS4_SCREEN_WIDTH - banner_w) / 2;
    int banner_y = 1010;

    draw_rect_solid(fb, banner_x, banner_y, banner_w, banner_h, 0xFF14243BU);
    draw_rect_border(fb, banner_x, banner_y, banner_w, banner_h, 2, COLOR_BORDER_BLUE);

    int cur_x = banner_x + 24;
    int text_y = banner_y + 14;

    ui_draw_text(fb, cur_x, text_y, "Aperte ", COLOR_TEXT_LIGHT, 2);
    cur_x += 7 * 16;

    int btn_w = 126;
    int btn_h = 26;
    int btn_x = cur_x;
    int btn_y = banner_y + 9;

    draw_rect_solid(fb, btn_x, btn_y, btn_w, btn_h, 0xFF1E293BU);
    draw_rect_border(fb, btn_x, btn_y, btn_w, btn_h, 1, COLOR_BORDER_CYAN);
    ui_draw_text(fb, btn_x + 7, btn_y + 5, "OPTIONS", COLOR_BORDER_CYAN, 2);
    cur_x += btn_w + 8;

    ui_draw_text(fb, cur_x, text_y, " para entrar com novo jogador", COLOR_TEXT_LIGHT, 2);
}

/* ==============================================================================
 * TELA DE INICIALIZACAO ANIMADA (SPLASH SCREEN / INTRO): "DO PS4 AS 4 TELAS"
 * 180 Frames @ 60 FPS (~3 Segundos) - 100% Procedural e Zero Alocacao Dinamica
 * ============================================================================== */

#define COLOR_SPLASH_BG        0xFF050811U /* Preto Profundo */
#define COLOR_PS_CLASSIC_BLUE  0xFF003791U /* Azul PlayStation Oficial */
#define COLOR_PS_NEON_BLUE     0xFF2E86DEU /* Azul Neon vibrante */
#define COLOR_SPLASH_CYAN      0xFF00F5D4U /* Ciano Neon */

void ui_menu_update_splash(uint32_t buttons_pressed) {
    if (!buttons_pressed) {
        return;
    }

    if (s_splash_frame < 180) {
        /* Pula a animacao de introducao direto para a Title Screen pronta */
        s_splash_frame = 180;
        audio_play_splash_sfx(SPLASH_SFX_CONFIRM);
    } else {
        /* Qualquer botao pressionado no estado Title Screen inicia o Launcher */
        s_app_state = UI_STATE_LAUNCHER;
        s_splash_frame = 0;
        audio_play_splash_sfx(SPLASH_SFX_CONFIRM);
    }
}

void ui_menu_render_splash(uint32_t* fb) {
    if (!fb || s_app_state != UI_STATE_SPLASH) {
        return;
    }

    int frame = s_splash_frame;
    int cx = PS4_SCREEN_WIDTH / 2;  /* 960 */
    int cy = PS4_SCREEN_HEIGHT / 2; /* 540 */

    /* Fundo Preto Profundo Padronizado */
    draw_rect_solid(fb, 0, 0, PS4_SCREEN_WIDTH, PS4_SCREEN_HEIGHT, COLOR_SPLASH_BG);

    /* SFX 1: Impacto Inicial do "4" (Fotograma 1) */
    if (frame == 1) {
        audio_play_splash_sfx(SPLASH_SFX_IMPACT);
    }

    /* --------------------------------------------------------------------------
     * FASE 1 (Fotogramas 0 a 45): O "4" do PS4
     * -------------------------------------------------------------------------- */
    if (frame < 50) {
        int stem_x = cx + 20;
        int stem_y = cy - 100;
        int stem_w = 40;
        int stem_h = 190;

        int bar_x = cx - 80;
        int bar_y = cy + 10;
        int bar_w = 160;
        int bar_h = 36;

        int diag_top_x = cx + 20;
        int diag_top_y = cy - 100;
        int diag_bot_x = cx - 80;
        int diag_bot_y = cy + 20;

        /* Materializacao progressiva */
        int draw_limit_h = (frame < 15) ? ((frame * stem_h) / 15) : stem_h;

        /* Haste Vertical Direita do "4" */
        if (draw_limit_h > 0) {
            draw_rect_solid(fb, stem_x, stem_y, stem_w, draw_limit_h, COLOR_PS_CLASSIC_BLUE);
            draw_rect_border(fb, stem_x, stem_y, stem_w, draw_limit_h, 2, COLOR_PS_NEON_BLUE);
        }

        /* Barra Horizontal Central do "4" */
        if (frame >= 8) {
            int draw_bar_w = (frame < 20) ? ((frame - 8) * bar_w) / 12 : bar_w;
            draw_rect_solid(fb, bar_x, bar_y, draw_bar_w, bar_h, COLOR_PS_CLASSIC_BLUE);
            draw_rect_border(fb, bar_x, bar_y, draw_bar_w, bar_h, 2, COLOR_PS_NEON_BLUE);
        }

        /* Haste Diagonal Esquerda do "4" */
        if (frame >= 5) {
            int diag_steps = 28;
            int max_steps = (frame < 20) ? ((frame - 5) * diag_steps) / 15 : diag_steps;
            for (int s = 0; s < max_steps; ++s) {
                int px = diag_top_x - (s * (diag_top_x - diag_bot_x)) / diag_steps;
                int py = diag_top_y + (s * (diag_bot_y - diag_top_y)) / diag_steps;
                draw_rect_solid(fb, px - 30, py, 32, 6, COLOR_PS_CLASSIC_BLUE);
                draw_rect_border(fb, px - 30, py, 32, 6, 1, COLOR_PS_NEON_BLUE);
            }
        }

        /* Pulso de brilho neon percorre o número (Frames 15 a 45) */
        if (frame >= 15 && frame < 45) {
            int pulse_y = (cy - 100) + ((frame - 15) * 7) % 200;
            draw_line_h(fb, cx - 85, pulse_y, 170, 3, COLOR_SPLASH_CYAN);
            draw_line_h(fb, cx - 75, pulse_y + 1, 150, 1, 0xFFFFFFFFU);
        }

        /* Logotipo PS4 na base da fase inicial */
        if (frame >= 12) {
            ui_draw_text(fb, cx - 24, cy + 120, "PS4", COLOR_PS_NEON_BLUE, 2);
        }
    }

    /* --------------------------------------------------------------------------
     * FASE 2 (Fotogramas 45 a 90): A Metamorfose: O "4" se divide em 4 Telas
     * -------------------------------------------------------------------------- */
    if (frame >= 45) {
        /* SFX 2: Arpejo GBA 4 notas (D4, A4, D5, F#5) na divisao das 4 telas */
        if (frame == 58) {
            audio_play_splash_sfx(SPLASH_SFX_QUAD_CHIME);
        }
        int t = frame - 45; /* 0 a 135 */

        /* Transicao das hastes do "4" para a cruz divisoria dos 4 quadrantes */
        int vert_y1 = (t < 30) ? ((cy - 100) - ((cy - 100) * t) / 30) : 0;
        int vert_y2 = (t < 30) ? ((cy + 90) + ((PS4_SCREEN_HEIGHT - (cy + 90)) * t) / 30) : PS4_SCREEN_HEIGHT;
        int vert_w  = (t < 30) ? (40 - (36 * t) / 30) : 4;
        int vert_x  = (t < 30) ? ((cx + 20) - (20 * t) / 30) : (cx - vert_w / 2);

        int horiz_x1 = (t < 30) ? ((cx - 80) - ((cx - 80) * t) / 30) : 0;
        int horiz_x2 = (t < 30) ? ((cx + 80) + ((PS4_SCREEN_WIDTH - (cx + 80)) * t) / 30) : PS4_SCREEN_WIDTH;
        int horiz_h  = (t < 30) ? (36 - (32 * t) / 30) : 4;
        int horiz_y  = (t < 30) ? ((cy + 20) - (20 * t) / 30) : (cy - horiz_h / 2);

        /* Desenho das linhas divisorias da cruz 4-Way Split-Screen */
        draw_rect_solid(fb, vert_x, vert_y1, vert_w, vert_y2 - vert_y1, COLOR_PS_NEON_BLUE);
        draw_rect_solid(fb, horiz_x1, horiz_y, horiz_x2 - horiz_x1, horiz_h, COLOR_PS_NEON_BLUE);

        /* 4 Mini-Telas de GBA em proporcao 3:2 (Acendem nos 4 Quadrantes) */
        if (frame >= 55) {
            int qf = frame - 55;
            int max_w = 270;
            int max_h = 180;
            int cur_w = (qf < 20) ? (max_w * qf) / 20 : max_w;
            int cur_h = (qf < 20) ? (max_h * qf) / 20 : max_h;

            if (cur_w > 10 && cur_h > 10) {
                int quad_centers[4][2] = {
                    { 480, 270 },   /* Q1 (P1): Top-Left     */
                    { 1440, 270 },  /* Q2 (P2): Top-Right    */
                    { 480, 810 },   /* Q3 (P3): Bottom-Left  */
                    { 1440, 810 }   /* Q4 (P4): Bottom-Right */
                };

                uint32_t p_colors[4] = {
                    0xFF2E86DEU, /* P1: Azul     */
                    0xFFEE5253U, /* P2: Vermelho */
                    0xFF10AC84U, /* P3: Verde    */
                    0xFFE056FDU  /* P4: Rosa     */
                };

                uint32_t p_fills[4] = {
                    0xFF0A1E36U,
                    0xFF360A0AU,
                    0xFF0A361EU,
                    0xFF320A36U
                };

                const char* p_tags[4] = { "P1", "P2", "P3", "P4" };

                for (int p = 0; p < 4; ++p) {
                    int qx = quad_centers[p][0];
                    int qy = quad_centers[p][1];

                    /* Moldura externa do mini-GBA */
                    draw_rect_solid(fb, qx - cur_w / 2 - 10, qy - cur_h / 2 - 8, cur_w + 20, cur_h + 16, 0xFF0E1A2DU);
                    draw_rect_border(fb, qx - cur_w / 2 - 10, qy - cur_h / 2 - 8, cur_w + 20, cur_h + 16, 2, 0xFF1D3557U);

                    /* Display LCD 3:2 iluminado na cor do jogador */
                    draw_rect_solid(fb, qx - cur_w / 2, qy - cur_h / 2, cur_w, cur_h, p_fills[p]);
                    draw_rect_border(fb, qx - cur_w / 2, qy - cur_h / 2, cur_w, cur_h, 2, p_colors[p]);

                    /* Identificador de jogador */
                    if (cur_w >= 180) {
                        ui_draw_text(fb, qx - 16, qy - 12, p_tags[p], p_colors[p], 3);
                        ui_draw_text(fb, qx - 40, qy + 24, "GBA SCREEN", 0xFF8D99AEU, 1);
                    }
                }
            }
        }

        /* Cabo de circuito eletrico azul neon (Link Cable 4-Way Virtual) */
        if (frame >= 68) {
            /* Hub central de conexao */
            int hub_size = 24;
            draw_rect_solid(fb, cx - hub_size / 2, cy - hub_size / 2, hub_size, hub_size, COLOR_SPLASH_CYAN);
            draw_rect_border(fb, cx - hub_size / 2, cy - hub_size / 2, hub_size, hub_size, 2, 0xFFFFFFFFU);

            /* Tracado eletrico para cada mini tela */
            draw_circuit_line(fb, cx, cy, 480, 480, 370, COLOR_SPLASH_CYAN);
            draw_circuit_line(fb, cx, cy, 1440, 1440, 370, COLOR_SPLASH_CYAN);
            draw_circuit_line(fb, cx, cy, 480, 480, 710, COLOR_SPLASH_CYAN);
            draw_circuit_line(fb, cx, cy, 1440, 1440, 710, COLOR_SPLASH_CYAN);

            /* Pacotes eletricos em transito simulando o Link Cable Ring Buffer */
            int pkt_offset = ((frame - 68) * 16) % 480;
            draw_rect_solid(fb, cx - pkt_offset, cy - 1, 6, 3, 0xFFFFFFFFU);
            draw_rect_solid(fb, cx + pkt_offset, cy - 1, 6, 3, 0xFFFFFFFFU);
        }
    }

    /* --------------------------------------------------------------------------
     * IDLE EFFECT 1: Luz Percorrendo as Linhas / Telas (Running Light)
     * -------------------------------------------------------------------------- */
    if (frame >= 90) {
        /* Ponto luminoso contínuo nas bordas das 4 mini-telas */
        int quad_pos[4][2] = {
            { 480, 270 }, { 1440, 270 }, { 480, 810 }, { 1440, 810 }
        };
        int peri = 2 * (270 + 180); /* 900 pixels */
        int base_run = ((frame - 90) * 6) % peri;

        for (int p = 0; p < 4; ++p) {
            int qx0 = quad_pos[p][0] - 135;
            int qy0 = quad_pos[p][1] - 90;
            int p_run = (base_run + p * 225) % peri;

            for (int b = 0; b < 10; ++b) {
                int st = (p_run - b + peri) % peri;
                int lx, ly;
                if (st < 270) {
                    lx = qx0 + st;
                    ly = qy0;
                } else if (st < 450) {
                    lx = qx0 + 270;
                    ly = qy0 + (st - 270);
                } else if (st < 720) {
                    lx = qx0 + 270 - (st - 450);
                    ly = qy0 + 180;
                } else {
                    lx = qx0;
                    ly = qy0 + 180 - (st - 720);
                }

                uint32_t spark_col = (b == 0) ? 0xFFFFFFFFU : 0xFFE0FFFFU;
                if (lx >= 0 && lx < PS4_SCREEN_WIDTH && ly >= 0 && ly < PS4_SCREEN_HEIGHT) {
                    fb[ly * PS4_SCREEN_WIDTH + lx] = spark_col;
                    if (st < 270 || (st >= 450 && st < 720)) {
                        if (ly + 1 < PS4_SCREEN_HEIGHT) fb[(ly + 1) * PS4_SCREEN_WIDTH + lx] = spark_col;
                    } else {
                        if (lx + 1 < PS4_SCREEN_WIDTH) fb[ly * PS4_SCREEN_WIDTH + lx + 1] = spark_col;
                    }
                }
            }
        }

        /* Feixe elétrico percorrendo a cruz central */
        int v_run = ((frame - 90) * 8) % PS4_SCREEN_HEIGHT;
        draw_rect_solid(fb, cx - 1, v_run, 2, 10, 0xFFE0FFFFU);
        draw_rect_solid(fb, cx - 1, (v_run + 8) % PS4_SCREEN_HEIGHT, 2, 2, 0xFFFFFFFFU);

        int h_run = ((frame - 90) * 12) % PS4_SCREEN_WIDTH;
        draw_rect_solid(fb, h_run, cy - 1, 10, 2, 0xFFE0FFFFU);
        draw_rect_solid(fb, (h_run + 8) % PS4_SCREEN_WIDTH, cy - 1, 2, 2, 0xFFFFFFFFU);
    }

    /* --------------------------------------------------------------------------
     * FASE 3 (Fotogramas 85 a 140): Formacao da Marca PSgba
     * -------------------------------------------------------------------------- */
    if (frame >= 85) {
        int banner_w = 780;
        int banner_h = 96;
        int banner_x = cx - banner_w / 2;
        int banner_y = 50;

        /* Caixa superior do logotipo */
        draw_rect_solid(fb, banner_x, banner_y, banner_w, banner_h, 0xFF080F1EU);
        draw_rect_border(fb, banner_x, banner_y, banner_w, banner_h, 2, COLOR_PS_NEON_BLUE);
        draw_rect_border(fb, banner_x + 4, banner_y + 4, banner_w - 8, banner_h - 8, 1, 0xFF1D3557U);

        if (frame < 110) {
            /* Transicao com integracao do "4": "PS4GBA" */
            ui_draw_text(fb, cx - 110, banner_y + 16, "PS", 0xFFFFFFFFU, 4);
            uint32_t four_col = ((frame / 4) % 2 == 0) ? COLOR_TEXT_GOLD : COLOR_SPLASH_CYAN;
            ui_draw_text(fb, cx - 46, banner_y + 16, "4", four_col, 4);
            ui_draw_text(fb, cx - 14, banner_y + 16, "GBA", COLOR_SPLASH_CYAN, 4);

            ui_draw_text(fb, cx - 136, banner_y + 64, "CONECTANDO 4 JOGADORES...", 0xFF8D99AEU, 2);
        } else {
            /* Nome oficial consolidado: "PSgba" */
            ui_draw_text(fb, cx - 80, banner_y + 16, "PS", COLOR_PS_NEON_BLUE, 4);
            ui_draw_text(fb, cx - 16, banner_y + 16, "gba", COLOR_TEXT_GOLD, 4);

            /* Subtitulo em Amarelo Dourado */
            ui_draw_text(fb, cx - 216, banner_y + 64, "4-WAY SPLIT-SCREEN EMULATOR", COLOR_TEXT_GOLD, 2);
        }
    }

    /* --------------------------------------------------------------------------
     * FASE 4 (Fotogramas 100+): Creditos dos Criadores & Efeito Shimmer
     * -------------------------------------------------------------------------- */
    if (frame >= 100) {
        int cred_w = 640;
        int cred_h = 76;
        int cred_x = cx - cred_w / 2;
        int cred_y = 880;

        draw_rect_solid(fb, cred_x, cred_y, cred_w, cred_h, 0xFF080F1EU);
        draw_rect_border(fb, cred_x, cred_y, cred_w, cred_h, 2, COLOR_SPLASH_CYAN);
        draw_rect_border(fb, cred_x + 3, cred_y + 3, cred_w - 6, cred_h - 6, 1, 0xFF1D3557U);

        /* Texto guia superior */
        ui_draw_text(fb, cx - 60, cred_y + 14, "DESENVOLVIDO POR", 0xFF8899AAU, 1);

        /* Nomes em destaque: felipecjb (Azul P1) x nsu-duxs (Vermelho P2) */
        int names_x = cx - 160;
        int names_y = cred_y + 38;

        ui_draw_text(fb, names_x, names_y, "felipecjb", COLOR_PS_NEON_BLUE, 2);
        ui_draw_text(fb, names_x + 144, names_y, " x ", 0xFFFFFFFFU, 2);
        ui_draw_text(fb, names_x + 192, names_y, "nsu-duxs", 0xFFEE5253U, 2);

        /* IDLE EFFECT 2: Brilho Metalico / Shimmer nos Nomes dos Criadores
         * Feixe luminoso translúcido que varre suavemente a caixa a cada 2.5s (150 frames) */
        int shim_cycle = 150;
        int shim_t = (frame - 100) % shim_cycle;
        if (shim_t < 50) {
            /* SFX 3: Cintilacao suave ~2.4kHz ao passar sobre os nomes dos criadores (Fotograma ~120 / loop) */
            if (shim_t == 20) {
                audio_play_splash_sfx(SPLASH_SFX_SHIMMER);
            }

            int sweep_x = cred_x + (shim_t * (cred_w + 60)) / 50 - 30;
            int half_w = 6;

            for (int sy = cred_y + 4; sy < cred_y + cred_h - 4; ++sy) {
                int sx_center = sweep_x + ((sy - cred_y) * 12) / cred_h;

                for (int dx = -half_w; dx <= half_w; ++dx) {
                    int px = sx_center + dx;
                    if (px >= cred_x + 4 && px < cred_x + cred_w - 4) {
                        int p_idx = sy * PS4_SCREEN_WIDTH + px;
                        uint32_t orig = fb[p_idx];

                        int boost = (half_w + 1 - abs(dx)) * 14; /* 14 a 98 */
                        uint32_t r = ((orig >> 16) & 0xFF) + boost;
                        uint32_t g = ((orig >> 8) & 0xFF) + boost;
                        uint32_t b = (orig & 0xFF) + boost;

                        if (r > 255) r = 255;
                        if (g > 255) g = 255;
                        if (b > 255) b = 255;

                        fb[p_idx] = 0xFF000000U | (r << 16) | (g << 8) | b;
                    }
                }
            }
        }
    }

    /* --------------------------------------------------------------------------
     * FASE 5 (Fotogramas 165+): Moldura Exterior Estável
     * -------------------------------------------------------------------------- */
    if (frame >= 165) {
        draw_rect_border(fb, 40, 24, PS4_SCREEN_WIDTH - 80, PS4_SCREEN_HEIGHT - 48, 3, COLOR_BORDER_BLUE);
    }

    /* --------------------------------------------------------------------------
     * INTERAÇÃO & TEXTO RETRÔ PULSANTE / SALTITANTE (TITLE SCREEN)
     * -------------------------------------------------------------------------- */
    if (frame < 180) {
        /* Dica discreta no rodapé durante a animação inicial */
        ui_draw_text(fb, cx - 76, 1044, "[X / OPTIONS] Pular", 0xFF8D99AEU, 1);
    } else {
        /* Title Screen Interativa: Espera por qualquer botão do DualShock 4 */
        int offset_y = (int)(sinf((float)s_splash_frame * 0.1f) * 4.0f);
        uint32_t prompt_color = ((s_splash_frame / 30) % 2 == 0) ? COLOR_TEXT_GOLD : COLOR_TEXT_WHITE;

        /* Centralizado perfeitamente: 34 chars * 16px = 544px -> X = 960 - 272 = 688.
         * Y = 990 + offset_y fica entre a caixa de créditos (Y=956) e a borda inferior (Y=1056). */
        ui_draw_text(fb, 688, 990 + offset_y, "Aperte qualquer botao para comecar", prompt_color, 2);
    }

    /* Avanço contínuo do fotograma (loop infinito de animação idle na Title Screen) */
    s_splash_frame++;
}

/* ==============================================================================
 * LOGICA DE ATUALIZACAO DO LAUNCHER (ZERO DISK ACCESS)
 * ============================================================================== */

void ui_menu_update_launcher(struct PS4GBA_Context* ctx, uint32_t buttons_pressed) {
    (void)ctx;

    /* Na aba de controles, Touchpad alterna entre DualShock 4 e Game Boy Advance */
    if (s_launcher_tab == LAUNCHER_TAB_CONTROLS) {
        if (buttons_pressed & ORBIS_PAD_BUTTON_TOUCH_PAD) {
            s_controls_view_gba = !s_controls_view_gba;
            audio_play_sfx(UI_SFX_TOUCHPAD);
            return;
        }
    } else {
        /* Nas demais abas, Touchpad alterna o painel lateral de detalhes/ajuda */
        if (buttons_pressed & ORBIS_PAD_BUTTON_TOUCH_PAD) {
            s_show_side_panel = !s_show_side_panel;
            audio_play_sfx(UI_SFX_TOUCHPAD);
        }
    }

    /* Troca de abas via L1 / R1 */
    if (buttons_pressed & ORBIS_PAD_BUTTON_L1) {
        s_launcher_tab = (s_launcher_tab - 1 + LAUNCHER_TAB_COUNT) % LAUNCHER_TAB_COUNT;
        audio_play_sfx(UI_SFX_TAB_SWITCH);
        return;
    }
    if (buttons_pressed & ORBIS_PAD_BUTTON_R1) {
        s_launcher_tab = (s_launcher_tab + 1) % LAUNCHER_TAB_COUNT;
        audio_play_sfx(UI_SFX_TAB_SWITCH);
        return;
    }

    /* Atualizacao manual e explicita de ROMs via TRIANGULO */
    if (buttons_pressed & ORBIS_PAD_BUTTON_TRIANGLE) {
        ui_menu_scan_roms();
        s_launcher_rom_cursor = 0;
        snprintf(s_launcher_status, sizeof(s_launcher_status),
                 "Varredura concluida: %d ROM(s) encontradas (%d USB, %d Internas)",
                 s_rom_count, s_usb_rom_count, s_rom_count - s_usb_rom_count);
        return;
    }

    if (s_launcher_tab == LAUNCHER_TAB_GAMES) {
        if (s_rom_count > 0) {
            if (buttons_pressed & ORBIS_PAD_BUTTON_UP) {
                s_launcher_rom_cursor = (s_launcher_rom_cursor - 1 + s_rom_count) % s_rom_count;
            }
            if (buttons_pressed & ORBIS_PAD_BUTTON_DOWN) {
                s_launcher_rom_cursor = (s_launcher_rom_cursor + 1) % s_rom_count;
            }
            if (buttons_pressed & ORBIS_PAD_BUTTON_LEFT) {
                s_launcher_rom_cursor -= ROM_PAGE_SIZE;
                if (s_launcher_rom_cursor < 0) {
                    s_launcher_rom_cursor = 0;
                }
            }
            if (buttons_pressed & ORBIS_PAD_BUTTON_RIGHT) {
                s_launcher_rom_cursor += ROM_PAGE_SIZE;
                if (s_launcher_rom_cursor >= s_rom_count) {
                    s_launcher_rom_cursor = s_rom_count - 1;
                }
            }
        }

        if ((buttons_pressed & ORBIS_PAD_BUTTON_CROSS) && s_rom_count > 0) {
            audio_play_sfx(UI_SFX_CONFIRM);
            if (s_launcher_rom_cursor >= 0 && s_launcher_rom_cursor < s_rom_count) {
                const char* path = s_rom_paths[s_launcher_rom_cursor];
                const char* paths[GBA_MAX_INSTANCES] = { path, path, path, path };
                bool ok = emulator_init_session(paths, 1);
                if (ok) {
                    s_launcher_status[0] = '\0';
                    s_app_state = UI_STATE_INGAME;
                    audio_stop_splash_sfx();
                    ui_menu_trigger_join_hint();
                } else {
                    snprintf(s_launcher_status, sizeof(s_launcher_status),
                             "Erro ao carregar ROM. Aperte [TRIANGULO] para atualizar lista.");
                }
            }
        }
    } else if (s_launcher_tab == LAUNCHER_TAB_VIDEO_AUDIO) {
        if (buttons_pressed & ORBIS_PAD_BUTTON_UP) {
            s_launcher_va_cursor = (s_launcher_va_cursor - 1 + LAUNCHER_VA_COUNT) % LAUNCHER_VA_COUNT;
        }
        if (buttons_pressed & ORBIS_PAD_BUTTON_DOWN) {
            s_launcher_va_cursor = (s_launcher_va_cursor + 1) % LAUNCHER_VA_COUNT;
        }

        int delta = (buttons_pressed & ORBIS_PAD_BUTTON_LEFT) ? -1 : 1;
        bool trigger = (buttons_pressed & (ORBIS_PAD_BUTTON_LEFT | ORBIS_PAD_BUTTON_RIGHT | ORBIS_PAD_BUTTON_CROSS));

        if (trigger) {
            if (buttons_pressed & ORBIS_PAD_BUTTON_CROSS) {
                audio_play_sfx(UI_SFX_CONFIRM);
            } else {
                audio_play_sfx(UI_SFX_VALUE_CHANGE);
            }
            switch (s_launcher_va_cursor) {
            case LAUNCHER_VA_DISPLAY:
                video_cycle_display_mode(delta);
                break;
            case LAUNCHER_VA_FILTER:
                video_cycle_filter(delta);
                break;
            case LAUNCHER_VA_SCALE:
                video_cycle_scale(delta);
                break;
            case LAUNCHER_VA_SMOOTH:
                video_toggle_smoothing();
                break;
            case LAUNCHER_VA_SHOW_FPS:
                s_show_fps = !s_show_fps;
                break;
            case LAUNCHER_VA_FPS_LIMIT:
                video_toggle_fps_limit();
                break;
            case LAUNCHER_VA_AUDIO_MODE:
                if (audio_get_mode() == PS4_AUDIO_MODE_MIX) {
                    audio_set_mode(PS4_AUDIO_MODE_FOCUS);
                    audio_set_focus_player(0);
                } else {
                    audio_set_mode(PS4_AUDIO_MODE_MIX);
                }
                break;
            case LAUNCHER_VA_MENU_SFX:
                audio_set_menu_sfx_enabled(!audio_is_menu_sfx_enabled());
                break;
            default:
                break;
            }
        }
    } else if (s_launcher_tab == LAUNCHER_TAB_CONTROLS) {
        if (buttons_pressed & ORBIS_PAD_BUTTON_CIRCLE) {
            s_launcher_tab = LAUNCHER_TAB_GAMES;
            audio_play_sfx(UI_SFX_TAB_SWITCH);
        }
    }
}

/* ==============================================================================
 * LOGICA DE ATUALIZACAO DO MENU OSD (IN-GAME)
 * ============================================================================== */

void ui_menu_update_osd(struct PS4GBA_Context* ctx, uint32_t buttons_pressed) {
    if (buttons_pressed & (ORBIS_PAD_BUTTON_OPTIONS | ORBIS_PAD_BUTTON_CIRCLE)) {
        s_app_state = UI_STATE_INGAME;
        audio_play_sfx(UI_SFX_CONFIRM);
        return;
    }

    /* Na aba de controles, Touchpad alterna entre DualShock 4 e Game Boy Advance */
    if (s_osd_tab == OSD_TAB_CONTROLS) {
        if (buttons_pressed & ORBIS_PAD_BUTTON_TOUCH_PAD) {
            s_controls_view_gba = !s_controls_view_gba;
            audio_play_sfx(UI_SFX_TOUCHPAD);
            return;
        }
    } else {
        /* Nas demais abas, Touchpad alterna o painel lateral de detalhes/ajuda */
        if (buttons_pressed & ORBIS_PAD_BUTTON_TOUCH_PAD) {
            s_show_side_panel = !s_show_side_panel;
            audio_play_sfx(UI_SFX_TOUCHPAD);
        }
    }

    if (buttons_pressed & ORBIS_PAD_BUTTON_L1) {
        s_osd_tab = (s_osd_tab - 1 + OSD_TAB_COUNT) % OSD_TAB_COUNT;
        audio_play_sfx(UI_SFX_TAB_SWITCH);
        return;
    }
    if (buttons_pressed & ORBIS_PAD_BUTTON_R1) {
        s_osd_tab = (s_osd_tab + 1) % OSD_TAB_COUNT;
        audio_play_sfx(UI_SFX_TAB_SWITCH);
        return;
    }

    if (s_osd_tab == OSD_TAB_SAVES) {
        if (buttons_pressed & ORBIS_PAD_BUTTON_UP) {
            s_osd_saves_cursor = (s_osd_saves_cursor - 1 + OSD_SAVE_COUNT) % OSD_SAVE_COUNT;
        }
        if (buttons_pressed & ORBIS_PAD_BUTTON_DOWN) {
            s_osd_saves_cursor = (s_osd_saves_cursor + 1) % OSD_SAVE_COUNT;
        }

        if (buttons_pressed & ORBIS_PAD_BUTTON_CROSS) {
            audio_play_sfx(UI_SFX_CONFIRM);
            switch (s_osd_saves_cursor) {
            case OSD_SAVE_RESUME:
                s_app_state = UI_STATE_INGAME;
                break;
            case OSD_SAVE_QUICK_SAVE:
                if (save_manager_quick_save_state(ctx, s_osd_player)) {
                    snprintf(s_osd_status, sizeof(s_osd_status), "Quick Save P%d salvo com sucesso!", s_osd_player + 1);
                } else {
                    snprintf(s_osd_status, sizeof(s_osd_status), "Erro ao salvar Quick Save P%d", s_osd_player + 1);
                }
                break;
            case OSD_SAVE_QUICK_LOAD:
                if (save_manager_quick_load_state(ctx, s_osd_player)) {
                    snprintf(s_osd_status, sizeof(s_osd_status), "Quick Load P%d carregado com sucesso!", s_osd_player + 1);
                } else {
                    snprintf(s_osd_status, sizeof(s_osd_status), "Quick Save P%d nao encontrado", s_osd_player + 1);
                }
                break;
            case OSD_SAVE_GROUP_SAVE:
                if (save_manager_group_save_state(ctx)) {
                    snprintf(s_osd_status, sizeof(s_osd_status), "Group Save State (4P) salvo com sucesso!");
                } else {
                    snprintf(s_osd_status, sizeof(s_osd_status), "Erro ao salvar Group Save State");
                }
                break;
            case OSD_SAVE_GROUP_LOAD:
                if (save_manager_group_load_state(ctx)) {
                    snprintf(s_osd_status, sizeof(s_osd_status), "Group Save State (4P) carregado com sucesso!");
                } else {
                    snprintf(s_osd_status, sizeof(s_osd_status), "Group Save State nao encontrado");
                }
                break;
            case OSD_SAVE_RETURN_LAUNCHER:
                emulator_destroy_all();
                ui_menu_scan_roms();
                s_app_state = UI_STATE_LAUNCHER;
                break;
            default:
                break;
            }
        }
    } else if (s_osd_tab == OSD_TAB_VIDEO_AUDIO) {
        if (buttons_pressed & ORBIS_PAD_BUTTON_UP) {
            s_osd_cfg_cursor = (s_osd_cfg_cursor - 1 + OSD_CFG_COUNT) % OSD_CFG_COUNT;
        }
        if (buttons_pressed & ORBIS_PAD_BUTTON_DOWN) {
            s_osd_cfg_cursor = (s_osd_cfg_cursor + 1) % OSD_CFG_COUNT;
        }

        int delta = (buttons_pressed & ORBIS_PAD_BUTTON_LEFT) ? -1 : 1;
        bool trigger = (buttons_pressed & (ORBIS_PAD_BUTTON_LEFT | ORBIS_PAD_BUTTON_RIGHT | ORBIS_PAD_BUTTON_CROSS));

        if (trigger) {
            if (buttons_pressed & ORBIS_PAD_BUTTON_CROSS) {
                audio_play_sfx(UI_SFX_CONFIRM);
            } else {
                audio_play_sfx(UI_SFX_VALUE_CHANGE);
            }
            switch (s_osd_cfg_cursor) {
            case OSD_CFG_DISPLAY:
                video_cycle_display_mode(delta);
                break;
            case OSD_CFG_FILTER:
                video_cycle_filter(delta);
                break;
            case OSD_CFG_SCALE:
                video_cycle_scale(delta);
                break;
            case OSD_CFG_SMOOTH:
                video_toggle_smoothing();
                break;
            case OSD_CFG_SHOW_FPS:
                s_show_fps = !s_show_fps;
                break;
            case OSD_CFG_FPS_LIMIT:
                video_toggle_fps_limit();
                snprintf(s_osd_status, sizeof(s_osd_status), "Bloqueio de FPS: %s",
                         video_get_fps_limit_name(video_get_fps_limit()));
                break;
            case OSD_CFG_AUDIO_FOCUS: {
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
                break;
            }
            case OSD_CFG_MENU_SFX:
                audio_set_menu_sfx_enabled(!audio_is_menu_sfx_enabled());
                break;
            default:
                break;
            }
        }
    } else if (s_osd_tab == OSD_TAB_LINK_CABLE) {
        if (buttons_pressed & ORBIS_PAD_BUTTON_UP) {
            s_osd_link_cursor = (s_osd_link_cursor - 1 + OSD_LINK_COUNT) % OSD_LINK_COUNT;
        }
        if (buttons_pressed & ORBIS_PAD_BUTTON_DOWN) {
            s_osd_link_cursor = (s_osd_link_cursor + 1) % OSD_LINK_COUNT;
        }

        bool trigger = (buttons_pressed & (ORBIS_PAD_BUTTON_LEFT | ORBIS_PAD_BUTTON_RIGHT | ORBIS_PAD_BUTTON_CROSS));
        if (trigger) {
            if (buttons_pressed & ORBIS_PAD_BUTTON_CROSS) {
                audio_play_sfx(UI_SFX_CONFIRM);
            } else {
                audio_play_sfx(UI_SFX_VALUE_CHANGE);
            }
            if (s_osd_link_cursor == OSD_LINK_POWER) {
                bool next_state = !link_hub_is_cable_enabled();
                link_hub_set_cable_enabled(next_state);
                snprintf(s_osd_status, sizeof(s_osd_status),
                         next_state ? "Link Cable: CONECTADO" : "Link Cable: DESCONECTADO");
            } else if (s_osd_link_cursor == OSD_LINK_HOTPLUG && (buttons_pressed & ORBIS_PAD_BUTTON_CROSS)) {
                link_hub_trigger_hotplug();
                snprintf(s_osd_status, sizeof(s_osd_status),
                         "Reconectando Link Cable... Volte ao jogo!");
                s_app_state = UI_STATE_INGAME;
            } else if (s_osd_link_cursor == OSD_LINK_DEV_TELEMETRY) {
                s_show_dev_telemetry = !s_show_dev_telemetry;
            }
        }
    }
}

/* ==============================================================================
 * RENDERIZACAO DA TELA INICIAL (LAUNCHER) - FLUIDO E SEM COLISAO
 * ============================================================================== */

void ui_menu_render_launcher(uint32_t* fb) {
    if (!fb) {
        return;
    }

    /* Atualizacao da animacao do painel lateral (slide-in / slide-out em 10 frames) */
    if (s_show_side_panel) {
        if (s_side_panel_anim < 10) {
            s_side_panel_anim++;
        }
    } else {
        if (s_side_panel_anim > 0) {
            s_side_panel_anim--;
        }
    }

    draw_rect_solid(fb, 0, 0, PS4_SCREEN_WIDTH, PS4_SCREEN_HEIGHT, COLOR_BG);
    draw_rect_border(fb, 40, 24, PS4_SCREEN_WIDTH - 80, PS4_SCREEN_HEIGHT - 48, 3, COLOR_BORDER_CYAN);
    draw_rect_border(fb, 46, 30, PS4_SCREEN_WIDTH - 92, PS4_SCREEN_HEIGHT - 60, 1, COLOR_BORDER_INNER);

    ui_draw_text(fb, 70, 44, "PSgba - GBA 4-WAY SPLIT-SCREEN EMULATOR v1.08", COLOR_TEXT_GOLD, 3);

    const char* tabs[LAUNCHER_TAB_COUNT] = {
        "[ JOGOS ]",
        "[ VIDEO & AUDIO ]",
        "[ CONTROLE E ATALHOS ]"
    };

    /* Barra de abas com margem de respiro de 40px (> 30px) antes da moldura */
    draw_tab_bar(fb, 80, 94, 1760, tabs, LAUNCHER_TAB_COUNT, s_launcher_tab);
    draw_rect_solid(fb, 60, 142, PS4_SCREEN_WIDTH - 120, 2, COLOR_BORDER_INNER);

    /* Calculo de largura dinamica para eliminar colisao */
    int total_avail_w = 1780;
    int side_max_w = 560;
    int gap = (s_side_panel_anim > 0) ? 24 : 0;
    int side_w = (side_max_w * s_side_panel_anim) / 10;
    int left_w = total_avail_w - side_w - gap;
    int left_x = 70;
    int left_y = 160;
    int left_h = 780;
    int side_x = left_x + left_w + gap;

    if (s_launcher_tab == LAUNCHER_TAB_GAMES) {
        draw_box_rpg(fb, left_x, left_y, left_w, left_h, COLOR_BG, COLOR_BORDER_BLUE);

        int total_pages = (s_rom_count > 0) ? ((s_rom_count + ROM_PAGE_SIZE - 1) / ROM_PAGE_SIZE) : 1;
        int cur_page = (s_rom_count > 0) ? (s_launcher_rom_cursor / ROM_PAGE_SIZE) : 0;
        int page_start = cur_page * ROM_PAGE_SIZE;
        int page_end = page_start + ROM_PAGE_SIZE;
        if (page_end > s_rom_count) {
            page_end = s_rom_count;
        }

        char page_str[96];
        snprintf(page_str, sizeof(page_str), "Pagina %d de %d  (Total: %d ROMs)", cur_page + 1, total_pages, s_rom_count);

        ui_draw_text(fb, left_x + 30, left_y + 24, "LISTA DE JOGOS DISPONIVEIS", COLOR_TEXT_GOLD, 2);
        ui_draw_text_truncated(fb, left_x + left_w - 420, left_y + 24, page_str, COLOR_BORDER_CYAN, 2, 400);
        draw_rect_solid(fb, left_x + 20, left_y + 56, left_w - 40, 1, COLOR_BORDER_INNER);

        for (int i = 0; i < ROM_PAGE_SIZE && (page_start + i) < page_end; ++i) {
            int idx = page_start + i;
            bool is_sel = (s_launcher_rom_cursor == idx);
            bool is_usb = (strncmp(s_rom_files[idx], "[USB]", 5) == 0);
            int row_y = left_y + 76 + i * 44;

            if (is_sel) {
                draw_rect_solid(fb, left_x + 20, row_y - 4, left_w - 40, 38, COLOR_ITEM_SEL_BG);
                draw_rect_border(fb, left_x + 20, row_y - 4, left_w - 40, 38, 1, COLOR_BORDER_CYAN);
            }

            char line[180];
            snprintf(line, sizeof(line), "%s [%02d] %s", is_sel ? ">" : " ", idx + 1, s_rom_files[idx]);
            uint32_t txt_color = is_sel ? COLOR_TEXT_GOLD : (is_usb ? COLOR_TEXT_USB : COLOR_TEXT_LIGHT);
            ui_draw_text_truncated(fb, left_x + 30, row_y + 5, line, txt_color, 2, left_w - 60);
        }

        ui_draw_text_truncated(fb, left_x + 30, left_y + 668,
                               "[ESQUERDA / DIREITA] Pular Pagina   [TRIANGULO] Atualizar Lista",
                               COLOR_TEXT_MUTED, 2, left_w - 60);

        uint32_t usb_col = (s_usb_rom_count > 0) ? COLOR_BORDER_CYAN : COLOR_TEXT_GOLD;
        ui_draw_text_truncated(fb, left_x + 30, left_y + 700, s_usb_status_str, usb_col, 2, left_w - 60);

        if (s_launcher_status[0]) {
            ui_draw_text_truncated(fb, left_x + 30, left_y + 734, s_launcher_status, COLOR_TEXT_GREEN, 2, left_w - 60);
        }

        /* Renderiza painel lateral somente se expandido */
        if (side_w > 120) {
            draw_box_rpg(fb, side_x, left_y, side_w, left_h, COLOR_BG, COLOR_BORDER_BLUE);
            ui_draw_text_truncated(fb, side_x + 24, left_y + 24, "DETALHES DO JOGO", COLOR_TEXT_GOLD, 2, side_w - 48);
            draw_rect_solid(fb, side_x + 20, left_y + 56, side_w - 40, 1, COLOR_BORDER_INNER);

            if (s_rom_count > 0 && s_launcher_rom_cursor >= 0 && s_launcher_rom_cursor < s_rom_count) {
                int cur_idx = s_launcher_rom_cursor;
                bool is_usb = (strncmp(s_rom_files[cur_idx], "[USB]", 5) == 0);

                int ty = left_y + 80;
                ui_draw_text(fb, side_x + 24, ty, "NOME DO ARQUIVO:", COLOR_BORDER_CYAN, 2); ty += 30;
                ty += ui_draw_text_wrapped(fb, side_x + 24, ty, s_rom_files[cur_idx], COLOR_TEXT_WHITE, 2, side_w - 48, 24);
                ty += 16;

                ui_draw_text(fb, side_x + 24, ty, "CAMINHO COMPLETO:", COLOR_BORDER_CYAN, 2); ty += 30;
                ty += ui_draw_text_wrapped(fb, side_x + 24, ty, s_rom_paths[cur_idx], COLOR_TEXT_MUTED, 1, side_w - 48, 14);
                ty += 16;

                ui_draw_text(fb, side_x + 24, ty, "DISPOSITIVO DE ARMAZENAMENTO:", COLOR_BORDER_CYAN, 2); ty += 30;
                const char* stor_desc = is_usb ? "Pendrive ou HD Externo USB (/mnt/usbX)" : "Disco Rigido Interno do PS4 (/data)";
                ty += ui_draw_text_wrapped(fb, side_x + 24, ty, stor_desc, is_usb ? COLOR_TEXT_GREEN : COLOR_TEXT_LIGHT, 2, side_w - 48, 24);
                ty += 16;

                ui_draw_text(fb, side_x + 24, ty, "SUPORTE MULTIPLAYER:", COLOR_BORDER_CYAN, 2); ty += 30;
                const char* mp_desc = "Ate 4 Jogadores Simultaneos.\nLink Cable Virtual com Ring Buffer de alta velocidade em memoria RAM.";
                ui_draw_text_wrapped(fb, side_x + 24, ty, mp_desc, COLOR_TEXT_USB, 2, side_w - 48, 24);
            }
        }

    } else if (s_launcher_tab == LAUNCHER_TAB_VIDEO_AUDIO) {
        draw_box_rpg(fb, left_x, left_y, left_w, left_h, COLOR_BG, COLOR_BORDER_BLUE);

        ui_draw_text(fb, left_x + 30, left_y + 24, "CONFIGURACOES DE VIDEO & AUDIO", COLOR_TEXT_GOLD, 2);
        ui_draw_text_truncated(fb, left_x + 30, left_y + 54, "Ajuste imagem e som para televisores HD e 4K", COLOR_TEXT_MUTED, 2, left_w - 60);
        draw_rect_solid(fb, left_x + 20, left_y + 86, left_w - 40, 1, COLOR_BORDER_INNER);

        char opt_display[160], opt_filter[160], opt_scale[160], opt_smooth[160];
        char opt_fps[160], opt_fps_limit[160], opt_audio[160], opt_sfx[160];

        snprintf(opt_display, sizeof(opt_display), "Proporcao da Tela: < %s >",
                 video_get_display_mode_name(video_get_display_mode()));
        snprintf(opt_filter, sizeof(opt_filter), "Modo de Qualidade / Upscaling: < %s >",
                 video_get_filter_name(video_get_filter()));
        snprintf(opt_scale, sizeof(opt_scale), "Fator de Resolucao Interna: < %s >",
                 video_get_scale_name(video_get_scale()));
        snprintf(opt_smooth, sizeof(opt_smooth), "Suavizacao (Bilinear): < %s >",
                 video_get_smoothing_name(video_get_smoothing()));
        snprintf(opt_fps, sizeof(opt_fps), "Contador de FPS: < %s >",
                 s_show_fps ? "LIGADO" : "DESLIGADO");
        snprintf(opt_fps_limit, sizeof(opt_fps_limit), "Bloqueio de Framerate: < %s >",
                 video_get_fps_limit_name(video_get_fps_limit()));

        const char* audio_name = (audio_get_mode() == PS4_AUDIO_MODE_MIX)
                                     ? "Mix Todas as Telas"
                                     : "Foco P1";
        snprintf(opt_audio, sizeof(opt_audio), "Audio: < %s >", audio_name);
        snprintf(opt_sfx, sizeof(opt_sfx), "Sons do Menu: < %s >",
                 audio_is_menu_sfx_enabled() ? "LIGADO" : "DESLIGADO");

        const char* opts[LAUNCHER_VA_COUNT] = {
            opt_display, opt_filter, opt_scale, opt_smooth, opt_fps, opt_fps_limit, opt_audio, opt_sfx
        };

        for (int i = 0; i < LAUNCHER_VA_COUNT; ++i) {
            bool is_sel = (s_launcher_va_cursor == i);
            int row_y = left_y + 104 + i * 64;

            if (is_sel) {
                draw_rect_solid(fb, left_x + 20, row_y - 6, left_w - 40, 50, COLOR_ITEM_SEL_BG);
                draw_rect_border(fb, left_x + 20, row_y - 6, left_w - 40, 50, 1, COLOR_BORDER_CYAN);
            }

            char line[200];
            snprintf(line, sizeof(line), "%s %s", is_sel ? ">" : " ", opts[i]);
            ui_draw_text_truncated(fb, left_x + 30, row_y + 10, line, is_sel ? COLOR_TEXT_GOLD : COLOR_TEXT_LIGHT, 2, left_w - 60);
        }

        ui_draw_text_truncated(fb, left_x + 30, left_y + 710,
                               "[ESQUERDA / DIREITA / X] Alterar   [D-PAD CIMA/BAIXO] Navegar",
                               COLOR_TEXT_MUTED, 2, left_w - 60);

        if (side_w > 120) {
            draw_box_rpg(fb, side_x, left_y, side_w, left_h, COLOR_BG, COLOR_BORDER_BLUE);
            ui_draw_text_truncated(fb, side_x + 24, left_y + 24, "INFORMACAO DA OPCAO", COLOR_TEXT_GOLD, 2, side_w - 48);
            draw_rect_solid(fb, side_x + 20, left_y + 56, side_w - 40, 1, COLOR_BORDER_INNER);

            int ty = left_y + 80;
            switch (s_launcher_va_cursor) {
            case LAUNCHER_VA_DISPLAY:
                ui_draw_text(fb, side_x + 24, ty, "PROPORCAO DA TELA:", COLOR_BORDER_CYAN, 2); ty += 36;
                ui_draw_text_wrapped(fb, side_x + 24, ty,
                                     "3:2 Nativo: Preserva a geometria exata do Game Boy Advance com pillarboxing lateral.\n\n"
                                     "16:9 Esticado: Preenche toda a area da tela da TV sem bordas pretas.",
                                     COLOR_TEXT_WHITE, 2, side_w - 48, 24);
                break;
            case LAUNCHER_VA_FILTER:
                ui_draw_text(fb, side_x + 24, ty, "ALGORITMO DE QUALIDADE:", COLOR_BORDER_CYAN, 2); ty += 36;
                ui_draw_text_wrapped(fb, side_x + 24, ty,
                                     "Pixel Puro: Nearest-neighbor classico com pixels nitidos.\n\n"
                                     "ScaleNx: Realce inteligente de bordas sem desfoque.\n\n"
                                     "HQx / xBR: Interpolacao vetorial avancada para visual moderno.",
                                     COLOR_TEXT_WHITE, 2, side_w - 48, 24);
                break;
            case LAUNCHER_VA_SCALE:
                ui_draw_text(fb, side_x + 24, ty, "FATOR INTERNO:", COLOR_BORDER_CYAN, 2); ty += 36;
                ui_draw_text_wrapped(fb, side_x + 24, ty,
                                     "1X ate Max 4K: Define a resolucao interna de renderizacao antes da projecao na tela.\n\n"
                                     "Em 4 Jogadores, o emulador ajusta automaticamente para garantir 60 FPS cravados.",
                                     COLOR_TEXT_WHITE, 2, side_w - 48, 24);
                break;
            case LAUNCHER_VA_SMOOTH:
                ui_draw_text(fb, side_x + 24, ty, "SUAVIZACAO BILINEAR:", COLOR_BORDER_CYAN, 2); ty += 36;
                ui_draw_text_wrapped(fb, side_x + 24, ty,
                                     "Aplica interpolacao bilinear na apresentacao final, eliminando cintilacoes e suavizando gradientes.",
                                     COLOR_TEXT_WHITE, 2, side_w - 48, 24);
                break;
            case LAUNCHER_VA_SHOW_FPS:
                ui_draw_text(fb, side_x + 24, ty, "CONTADOR DE FPS:", COLOR_BORDER_CYAN, 2); ty += 36;
                ui_draw_text_wrapped(fb, side_x + 24, ty,
                                     "Exibe a taxa real de quadros por segundo no canto superior esquerdo da TV durante o jogo.",
                                     COLOR_TEXT_WHITE, 2, side_w - 48, 24);
                break;
            case LAUNCHER_VA_FPS_LIMIT:
                ui_draw_text(fb, side_x + 24, ty, "BLOQUEIO DE FPS:", COLOR_BORDER_CYAN, 2); ty += 36;
                ui_draw_text_wrapped(fb, side_x + 24, ty,
                                     "60 FPS: Velocidade nominal original do Game Boy Advance.\n\n"
                                     "30 FPS: Modo alternativo de sincronizacao para economia de processamento.",
                                     COLOR_TEXT_WHITE, 2, side_w - 48, 24);
                break;
            case LAUNCHER_VA_AUDIO_MODE:
                ui_draw_text(fb, side_x + 24, ty, "MODO DE AUDIO:", COLOR_BORDER_CYAN, 2); ty += 36;
                ui_draw_text_wrapped(fb, side_x + 24, ty,
                                     "Mix: Mistura o som de todos os jogadores ativos (25% cada).\n\n"
                                     "Foco: Concentra 100% do volume na tela do Jogador 1 (atalho L3+R3).",
                                     COLOR_TEXT_WHITE, 2, side_w - 48, 24);
                break;
            case LAUNCHER_VA_MENU_SFX:
                ui_draw_text(fb, side_x + 24, ty, "SONS DA INTERFACE:", COLOR_BORDER_CYAN, 2); ty += 36;
                ui_draw_text_wrapped(fb, side_x + 24, ty,
                                     "Ativa ou desativa os efeitos sonoros ao navegar pelas abas e opcoes dos menus. O audio do jogo em execucao continua funcionando normalmente.",
                                     COLOR_TEXT_WHITE, 2, side_w - 48, 24);
                break;
            default:
                break;
            }
        }

    } else if (s_launcher_tab == LAUNCHER_TAB_CONTROLS) {
        if (s_controls_view_gba) {
            draw_graphical_gba_classic_tutorial(fb, 960, 505);
        } else {
            draw_graphical_dualshock4_tutorial(fb, 960, 505);
        }
    }

    /* Rodape informativo fixo padronizado */
    if (s_launcher_tab == LAUNCHER_TAB_CONTROLS) {
        ui_draw_text_truncated(fb, 70, 970,
                               "[D-PAD] Navegar   [O] Voltar   [TOUCHPAD] Alternar Visualizacao (DualShock 4 / Game Boy Advance)",
                               COLOR_TEXT_MUTED, 2, 1780);
    } else {
        ui_draw_text_truncated(fb, 70, 970,
                               "[D-PAD] Navegar   [X] Confirmar   [O] Voltar   [TOUCHPAD] Ajuda/Detalhes   [TRIANGULO] Atualizar ROMs",
                               COLOR_TEXT_MUTED, 2, 1780);
    }
}

/* ==============================================================================
 * RENDERIZACAO DO MENU OSD SUSPENSO (IN-GAME)
 * ============================================================================== */

void ui_menu_render_osd(uint32_t* fb, struct PS4GBA_Context* ctx) {
    if (!fb || s_app_state != UI_STATE_OSD) {
        return;
    }

    if (s_show_side_panel) {
        if (s_side_panel_anim < 10) {
            s_side_panel_anim++;
        }
    } else {
        if (s_side_panel_anim > 0) {
            s_side_panel_anim--;
        }
    }

    int box_x = 40;
    int box_y = 24;
    int box_w = PS4_SCREEN_WIDTH - 80;   /* 1840 */
    int box_h = PS4_SCREEN_HEIGHT - 48;  /* 1032 */

    draw_rect_solid(fb, 0, 0, PS4_SCREEN_WIDTH, PS4_SCREEN_HEIGHT, COLOR_BG);
    draw_rect_border(fb, box_x, box_y, box_w, box_h, 3, COLOR_BORDER_CYAN);
    draw_rect_border(fb, box_x + 6, box_y + 6, box_w - 12, box_h - 12, 1, COLOR_BORDER_INNER);

    /* Linha superior: Titulo formatado estritamente como MENU - CONTROLE P%d (%s) */
    char title[160];
    snprintf(title, sizeof(title), "MENU - CONTROLE P%d (%s)",
             s_osd_player + 1, ctx ? ctx->rom_name : "");
    ui_draw_text_truncated(fb, box_x + 36, box_y + 24, title, COLOR_TEXT_GOLD, 3, 1150);

    char link_tag[64];
    snprintf(link_tag, sizeof(link_tag), "[LINK: %s]",
             link_hub_is_cable_enabled() ? "CONECTADO" : "DESCONECTADO");
    ui_draw_text(fb, box_x + box_w - 380, box_y + 28, link_tag, COLOR_BORDER_CYAN, 2);

    const char* osd_tabs[OSD_TAB_COUNT] = {
        "[ JOGO & SAVES ]",
        "[ VIDEO & AUDIO ]",
        "[ LINK CABLE ]",
        "[ CONTROLES ]"
    };

    /* Barra de abas do OSD alinhada e centrada com folga de respiro > 30px */
    draw_tab_bar(fb, 80, box_y + 70, 1760, osd_tabs, OSD_TAB_COUNT, s_osd_tab);
    draw_rect_solid(fb, 60, box_y + 116, box_w - 40, 2, COLOR_BORDER_INNER);

    int total_avail_w = 1780;
    int side_max_w = 560;
    int gap = (s_side_panel_anim > 0) ? 24 : 0;
    int side_w = (side_max_w * s_side_panel_anim) / 10;
    int left_w = total_avail_w - side_w - gap;
    int left_x = 70;
    int left_y = box_y + 130;
    int left_h = 740;
    int side_x = left_x + left_w + gap;

    if (s_osd_tab == OSD_TAB_SAVES) {
        draw_box_rpg(fb, left_x, left_y, left_w, left_h, COLOR_BG, COLOR_BORDER_BLUE);
        ui_draw_text(fb, left_x + 30, left_y + 24, "GERENCIAMENTO DE JOGO & SAVES", COLOR_TEXT_GOLD, 2);
        draw_rect_solid(fb, left_x + 20, left_y + 56, left_w - 40, 1, COLOR_BORDER_INNER);

        char item_qsave[128], item_qload[128];
        snprintf(item_qsave, sizeof(item_qsave), "Quick Save (Slot Individual P%d)", s_osd_player + 1);
        snprintf(item_qload, sizeof(item_qload), "Quick Load (Slot Individual P%d)", s_osd_player + 1);

        const char* save_items[OSD_SAVE_COUNT] = {
            "Continuar Jogo",
            item_qsave,
            item_qload,
            "Group Save State (Todas as 4 Telas Juntas)",
            "Group Load State (Todas as 4 Telas Juntas)",
            "Voltar ao Launcher (Fechar ROM)"
        };

        for (int i = 0; i < OSD_SAVE_COUNT; ++i) {
            bool is_sel = (s_osd_saves_cursor == i);
            int row_y = left_y + 86 + i * 86;

            if (is_sel) {
                draw_rect_solid(fb, left_x + 20, row_y - 6, left_w - 40, 58, COLOR_ITEM_SEL_BG);
                draw_rect_border(fb, left_x + 20, row_y - 6, left_w - 40, 58, 1, COLOR_BORDER_CYAN);
            }

            char line[180];
            snprintf(line, sizeof(line), "%s %s", is_sel ? ">" : " ", save_items[i]);
            ui_draw_text_truncated(fb, left_x + 30, row_y + 14, line, is_sel ? COLOR_TEXT_GOLD : COLOR_TEXT_LIGHT, 2, left_w - 50);
        }

        if (side_w > 120) {
            draw_box_rpg(fb, side_x, left_y, side_w, left_h, COLOR_BG, COLOR_BORDER_BLUE);
            ui_draw_text_truncated(fb, side_x + 24, left_y + 24, "STATUS DA SESSAO", COLOR_TEXT_GOLD, 2, side_w - 48);
            draw_rect_solid(fb, side_x + 20, left_y + 56, side_w - 40, 1, COLOR_BORDER_INNER);

            int ty = left_y + 80;
            ui_draw_text(fb, side_x + 24, ty, "SISTEMA DE SAVES:", COLOR_BORDER_CYAN, 2); ty += 36;
            ui_draw_text_wrapped(fb, side_x + 24, ty,
                                 "- Saves .sav gravados em flash no PS4.\n\n"
                                 "- Quick Save: Salva o estado individual do Jogador P#.\n\n"
                                 "- Group Save: Salva o estado de todas as 4 telas e da comunicacao Link simultaneamente.",
                                 COLOR_TEXT_WHITE, 2, side_w - 48, 24);

            ty = left_y + 360;
            ui_draw_text(fb, side_x + 24, ty, "JOGADORES ATIVOS:", COLOR_BORDER_CYAN, 2); ty += 36;
            for (int p = 0; p < GBA_MAX_INSTANCES; ++p) {
                bool active = (ctx && ctx->instances[p].active);
                char pstr[64];
                snprintf(pstr, sizeof(pstr), "Player %d: %s", p + 1, active ? "ATIVO NA PARTIDA" : "Slot Livre");
                ui_draw_text(fb, side_x + 24, ty + p * 32, pstr,
                             active ? COLOR_TEXT_GREEN : COLOR_TEXT_MUTED, 2);
            }
        }

    } else if (s_osd_tab == OSD_TAB_VIDEO_AUDIO) {
        draw_box_rpg(fb, left_x, left_y, left_w, left_h, COLOR_BG, COLOR_BORDER_BLUE);
        ui_draw_text(fb, left_x + 30, left_y + 24, "CONFIGURACOES DE VIDEO & AUDIO", COLOR_TEXT_GOLD, 2);
        draw_rect_solid(fb, left_x + 20, left_y + 56, left_w - 40, 1, COLOR_BORDER_INNER);

        char item_display[140], item_filter[140], item_scale[140], item_smooth[140];
        char item_fps[140], item_fps_limit[140], item_audio[140], item_sfx[140];

        snprintf(item_display, sizeof(item_display), "Proporcao da Tela: < %s >",
                 video_get_display_mode_name(video_get_display_mode()));
        snprintf(item_filter, sizeof(item_filter), "Modo de Qualidade / Upscaling: < %s >",
                 video_get_filter_name(video_get_filter()));
        snprintf(item_scale, sizeof(item_scale), "Fator Interno: < %s >%s",
                 video_get_scale_name(video_get_scale()),
                 video_get_auto_degrade_level() > 0 ? " [AUTO 60FPS]" : "");
        snprintf(item_smooth, sizeof(item_smooth), "Suavizacao (Bilinear): < %s >",
                 video_get_smoothing_name(video_get_smoothing()));
        snprintf(item_fps, sizeof(item_fps), "Contador de FPS: < %s >",
                 s_show_fps ? "LIGADO" : "DESLIGADO");
        snprintf(item_fps_limit, sizeof(item_fps_limit), "Bloqueio de FPS: < %s >",
                 video_get_fps_limit_name(video_get_fps_limit()));

        char focus_label[64];
        if (audio_get_mode() == PS4_AUDIO_MODE_MIX) {
            snprintf(focus_label, sizeof(focus_label), "Mix Todas as Telas");
        } else {
            snprintf(focus_label, sizeof(focus_label), "Foco Player P%d",
                     audio_get_focus_player() + 1);
        }
        snprintf(item_audio, sizeof(item_audio), "Audio dos Jogadores: < %s >", focus_label);
        snprintf(item_sfx, sizeof(item_sfx), "Sons do Menu: < %s >",
                 audio_is_menu_sfx_enabled() ? "LIGADO" : "DESLIGADO");

        const char* cfg_items[OSD_CFG_COUNT] = {
            item_display, item_filter, item_scale, item_smooth, item_fps, item_fps_limit, item_audio, item_sfx
        };

        for (int i = 0; i < OSD_CFG_COUNT; ++i) {
            bool is_sel = (s_osd_cfg_cursor == i);
            int row_y = left_y + 72 + i * 68;

            if (is_sel) {
                draw_rect_solid(fb, left_x + 20, row_y - 6, left_w - 40, 48, COLOR_ITEM_SEL_BG);
                draw_rect_border(fb, left_x + 20, row_y - 6, left_w - 40, 48, 1, COLOR_BORDER_CYAN);
            }

            char line[200];
            snprintf(line, sizeof(line), "%s %s", is_sel ? ">" : " ", cfg_items[i]);
            ui_draw_text_truncated(fb, left_x + 30, row_y + 10, line, is_sel ? COLOR_TEXT_GOLD : COLOR_TEXT_LIGHT, 2, left_w - 50);
        }

        ui_draw_text_truncated(fb, left_x + 30, left_y + 668,
                               "[ESQUERDA / DIREITA / X] Alterar em Tempo Real",
                               COLOR_TEXT_MUTED, 2, left_w - 50);

        if (side_w > 120) {
            draw_box_rpg(fb, side_x, left_y, side_w, left_h, COLOR_BG, COLOR_BORDER_BLUE);
            ui_draw_text_truncated(fb, side_x + 24, left_y + 24, "TELEMETRIA DE VIDEO", COLOR_TEXT_GOLD, 2, side_w - 48);
            draw_rect_solid(fb, side_x + 20, left_y + 56, side_w - 40, 1, COLOR_BORDER_INNER);

            int ty = left_y + 80;
            ui_draw_text(fb, side_x + 24, ty, "SAIDA DE VIDEO:", COLOR_BORDER_CYAN, 2); ty += 28;
            ui_draw_text(fb, side_x + 24, ty, "1920x1080p 60Hz", COLOR_TEXT_WHITE, 2); ty += 40;

            ui_draw_text(fb, side_x + 24, ty, "ACELERACAO GRAFICA:", COLOR_BORDER_CYAN, 2); ty += 28;
            ui_draw_text(fb, side_x + 24, ty, "SIMD SSE2 Zero-Copy", COLOR_TEXT_WHITE, 2); ty += 40;

            ui_draw_text(fb, side_x + 24, ty, "ATALHO DE AUDIO:", COLOR_BORDER_CYAN, 2); ty += 28;
            ui_draw_text(fb, side_x + 24, ty, "[L3 + R3] Foco no seu P#", COLOR_TEXT_USB, 2); ty += 40;

            ui_draw_text(fb, side_x + 24, ty, "INFORMACOES DA OPCAO:", COLOR_BORDER_CYAN, 2); ty += 30;
            switch (s_osd_cfg_cursor) {
            case OSD_CFG_DISPLAY:
                ui_draw_text_wrapped(fb, side_x + 24, ty, "Alterna entre proporcao 3:2 nativa e preenchimento 16:9 completo.", COLOR_TEXT_LIGHT, 2, side_w - 48, 22);
                break;
            case OSD_CFG_FILTER:
                ui_draw_text_wrapped(fb, side_x + 24, ty, "Filtro grafico de reconstrucao: Pixel Puro, ScaleNx ou HQx/xBR.", COLOR_TEXT_LIGHT, 2, side_w - 48, 22);
                break;
            case OSD_CFG_SCALE:
                ui_draw_text_wrapped(fb, side_x + 24, ty, "Multiplicador de escala interna para renderizacao de alta definicao.", COLOR_TEXT_LIGHT, 2, side_w - 48, 22);
                break;
            case OSD_CFG_SMOOTH:
                ui_draw_text_wrapped(fb, side_x + 24, ty, "Filtro bilinear para remocao de flicker em telas planas modernas.", COLOR_TEXT_LIGHT, 2, side_w - 48, 22);
                break;
            case OSD_CFG_SHOW_FPS:
                ui_draw_text_wrapped(fb, side_x + 24, ty, "Liga ou desliga o medidor de FPS no canto da tela.", COLOR_TEXT_LIGHT, 2, side_w - 48, 22);
                break;
            case OSD_CFG_FPS_LIMIT:
                ui_draw_text_wrapped(fb, side_x + 24, ty, "Trava a taxa de quadros em 60 FPS ou 30 FPS.", COLOR_TEXT_LIGHT, 2, side_w - 48, 22);
                break;
            case OSD_CFG_AUDIO_FOCUS:
                ui_draw_text_wrapped(fb, side_x + 24, ty, "Alterna audio entre mix geral e foco isolado em uma das telas.", COLOR_TEXT_LIGHT, 2, side_w - 48, 22);
                break;
            case OSD_CFG_MENU_SFX:
                ui_draw_text_wrapped(fb, side_x + 24, ty, "Ativa ou desativa os efeitos sonoros ao navegar pelas abas e opcoes dos menus. O audio do jogo em execucao continua funcionando normalmente.", COLOR_TEXT_LIGHT, 2, side_w - 48, 22);
                break;
            default:
                break;
            }
        }

    } else if (s_osd_tab == OSD_TAB_LINK_CABLE) {
        int cable_panel_h = s_show_dev_telemetry ? 320 : 660;
        draw_box_rpg(fb, left_x, left_y, total_avail_w, cable_panel_h, COLOR_BG, COLOR_BORDER_BLUE);

        ui_draw_text(fb, left_x + 30, left_y + 24, "CONEXAO E ESTADO DO LINK CABLE", COLOR_TEXT_GOLD, 2);
        draw_rect_solid(fb, left_x + 20, left_y + 56, total_avail_w - 40, 1, COLOR_BORDER_INNER);

        const char* cable_state_str = link_hub_is_hotplugging()
                                          ? "RECONECTANDO..."
                                          : (link_hub_is_cable_enabled() ? "CONECTADO" : "DESCONECTADO");

        char item_cable[140], item_dev[140];
        snprintf(item_cable, sizeof(item_cable), "Estado do Cabo: < %s >", cable_state_str);
        snprintf(item_dev, sizeof(item_dev), "[%c] Opcoes de Desenvolvedor / Telemetria",
                 s_show_dev_telemetry ? 'X' : ' ');

        const char* link_items[OSD_LINK_COUNT] = {
            item_cable,
            "[X] Reconectar Link Cable (Simular Hotplug)",
            "Modo do Cabo: < Multi-player (GBA Link) >",
            item_dev
        };

        for (int i = 0; i < OSD_LINK_COUNT; ++i) {
            bool is_sel = (s_osd_link_cursor == i);
            int row_y = left_y + 76 + i * 56;

            if (is_sel) {
                draw_rect_solid(fb, left_x + 20, row_y - 4, total_avail_w - 40, 44, COLOR_ITEM_SEL_BG);
                draw_rect_border(fb, left_x + 20, row_y - 4, total_avail_w - 40, 44, 1, COLOR_BORDER_CYAN);
            }

            char line[180];
            snprintf(line, sizeof(line), "%s %s", is_sel ? ">" : " ", link_items[i]);
            ui_draw_text_truncated(fb, left_x + 30, row_y + 8, line, is_sel ? COLOR_TEXT_GOLD : COLOR_TEXT_LIGHT, 2, total_avail_w - 50);
        }

        /* Se ativada a telemetria tecnica pelo desenvolvedor */
        if (s_show_dev_telemetry) {
            int tele_y = left_y + 350;
            int tele_h = 360;
            draw_box_rpg(fb, left_x, tele_y, total_avail_w, tele_h, COLOR_BG, COLOR_BORDER_CYAN);

            ui_draw_text(fb, left_x + 30, tele_y + 20, "TELEMETRIA DETALHADA DAS 4 PORTAS GBA SIO", COLOR_TEXT_GOLD, 2);
            draw_rect_solid(fb, left_x + 20, tele_y + 48, total_avail_w - 40, 1, COLOR_BORDER_INNER);

            uint32_t xfer_pkts = link_hub_get_transfer_count();
            char pkts_str[96];
            snprintf(pkts_str, sizeof(pkts_str), "Pacotes trocados: %u  |  Buffer: Lockless Ring Buffer em RAM", xfer_pkts);
            ui_draw_text(fb, left_x + 30, tele_y + 64, pkts_str, COLOR_TEXT_WHITE, 2);

            for (int p = 0; p < GBA_MAX_INSTANCES; ++p) {
                int mode = link_hub_get_player_mode(p);
                const char* mode_name = (mode == GBA_SIO_MULTI) ? "MULTI (Multiplayer 4P)" :
                                        (mode == GBA_SIO_NORMAL_32) ? "NORM32 (Transferencia 32-bit)" :
                                        (mode == GBA_SIO_NORMAL_8) ? "NORM8 (Transferencia 8-bit)" :
                                        (mode == GBA_SIO_GPIO) ? "GPIO (Linha)" : "IDLE";

                char p_info[140];
                snprintf(p_info, sizeof(p_info), "GBA Player %d (%s): Modo [%s]",
                         p + 1, (p == 0 ? "Master Clock" : "Slave"), mode_name);

                bool is_active_mode = (mode == GBA_SIO_MULTI || mode == GBA_SIO_NORMAL_32 ||
                                       mode == GBA_SIO_NORMAL_8 || mode == GBA_SIO_GPIO);
                uint32_t col = is_active_mode ? COLOR_BORDER_CYAN : COLOR_TEXT_MUTED;
                ui_draw_text(fb, left_x + 30, tele_y + 106 + p * 36, p_info, col, 2);
            }
        } else {
            /* Informacao limpa e formatada sobre o cabo */
            int tip_y = left_y + 350;
            draw_box_rpg(fb, left_x + 20, tip_y, total_avail_w - 40, 140, 0xFF14243BU, COLOR_BORDER_BLUE);
            ui_draw_text(fb, left_x + 40, tip_y + 24, "COMO FUNCIONA O LINK CABLE:", COLOR_TEXT_GOLD, 2);
            ui_draw_text(fb, left_x + 40, tip_y + 60, "- O cabo virtual conecta em memoria todas as portas GBA em tempo real.", COLOR_TEXT_LIGHT, 2);
            ui_draw_text(fb, left_x + 40, tip_y + 96, "- Se perder sincronia, selecione reconectar.", COLOR_TEXT_USB, 2);
        }

    } else if (s_osd_tab == OSD_TAB_CONTROLS) {
        if (s_controls_view_gba) {
            draw_graphical_gba_classic_tutorial(fb, 960, 505);
        } else {
            draw_graphical_dualshock4_tutorial(fb, 960, 505);
        }
    }

    if (s_osd_status[0]) {
        ui_draw_text_truncated(fb, box_x + 40, box_y + box_h - 76, s_osd_status, COLOR_TEXT_GREEN, 2, box_w - 80);
    }

    if (s_osd_tab == OSD_TAB_CONTROLS) {
        ui_draw_text_truncated(fb, box_x + 40, box_y + box_h - 40,
                               "[D-PAD] Navegar   [O / OPTIONS] Voltar ao Jogo   [TOUCHPAD] Alternar Visualizacao (DualShock 4 / Game Boy Advance)",
                               COLOR_TEXT_MUTED, 2, box_w - 80);
    } else {
        ui_draw_text_truncated(fb, box_x + 40, box_y + box_h - 40,
                               "[D-PAD] Navegar   [X] Confirmar   [O / OPTIONS] Voltar ao Jogo   [TOUCHPAD] Ajuda/Detalhes",
                               COLOR_TEXT_MUTED, 2, box_w - 80);
    }
}

/* ==============================================================================
 * RENDERIZACAO DO OVERLAY DE FPS
 * ============================================================================== */

void ui_menu_render_fps_overlay(uint32_t* fb, int fps) {
    if (!fb || !s_show_fps) {
        return;
    }

    int badge_x = 24;
    int badge_y = 14;
    int badge_w = 156;
    int badge_h = 34;

    uint32_t color = COLOR_TEXT_GREEN;
    if (fps < 35) {
        color = COLOR_TEXT_RED;
    } else if (fps < 55) {
        color = COLOR_TEXT_GOLD;
    }

    draw_rect_solid(fb, badge_x, badge_y, badge_w, badge_h, COLOR_BG);
    draw_rect_border(fb, badge_x, badge_y, badge_w, badge_h, 2, color);

    char fps_text[32];
    snprintf(fps_text, sizeof(fps_text), "FPS: %2d", fps);
    ui_draw_text(fb, badge_x + 18, badge_y + 9, fps_text, color, 2);
}
