#include "input_ds4.h"
#include "audio_ps4.h"
#include "ui_menu.h"
#include <string.h>
#include <orbis/UserService.h>
#include <orbis/Pad.h>

#ifndef GBA_KEY_A
#define GBA_KEY_A      (1 << 0)
#define GBA_KEY_B      (1 << 1)
#define GBA_KEY_SELECT (1 << 2)
#define GBA_KEY_START  (1 << 3)
#define GBA_KEY_RIGHT  (1 << 4)
#define GBA_KEY_LEFT   (1 << 5)
#define GBA_KEY_UP     (1 << 6)
#define GBA_KEY_DOWN   (1 << 7)
#define GBA_KEY_R      (1 << 8)
#define GBA_KEY_L      (1 << 9)
#endif

/* Cores oficiais do DualShock 4 para cada jogador (P1: Azul, P2: Vermelho, P3: Verde, P4: Rosa) */
static const OrbisPadColor s_ds4_colors[GBA_MAX_INSTANCES] = {
    {   0, 102, 255, 255 }, /* P1: Azul */
    { 255,  23,  68, 255 }, /* P2: Vermelho */
    {   0, 230, 118, 255 }, /* P3: Verde */
    { 255,  20, 147, 255 }  /* P4: Rosa */
};

static int s_pad_handles[GBA_MAX_INSTANCES] = { -1, -1, -1, -1 };
static OrbisUserServiceUserId s_user_ids[GBA_MAX_INSTANCES] = { -1, -1, -1, -1 };
static bool s_prev_l3_r3[GBA_MAX_INSTANCES] = { false, false, false, false };
static uint32_t s_prev_buttons[GBA_MAX_INSTANCES] = { 0, 0, 0, 0 };
static uint32_t s_hotplug_tick = 0;

static void refresh_pad_handles(void) {
    OrbisUserServiceLoginUserIdList user_list;
    memset(&user_list, 0, sizeof(user_list));
    int ret = sceUserServiceGetLoginUserIdList(&user_list);

    for (int i = 0; i < GBA_MAX_INSTANCES; ++i) {
        if (ret >= 0 && user_list.userId[i] > 0) {
            if (s_pad_handles[i] >= 0 && s_user_ids[i] != user_list.userId[i]) {
                scePadResetLightBar(s_pad_handles[i]);
                scePadClose(s_pad_handles[i]);
                s_pad_handles[i] = -1;
            }
            s_user_ids[i] = user_list.userId[i];
            if (s_pad_handles[i] < 0) {
                s_pad_handles[i] = scePadOpen(s_user_ids[i], ORBIS_PAD_PORT_TYPE_STANDARD, 0, NULL);
                if (s_pad_handles[i] >= 0) {
                    scePadSetLightBar(s_pad_handles[i], (OrbisPadColor*)&s_ds4_colors[i]);
                }
            }
        }
    }

    if (s_pad_handles[0] < 0) {
        OrbisUserServiceUserId initial_user = -1;
        if (sceUserServiceGetInitialUser(&initial_user) >= 0 && initial_user > 0) {
            s_user_ids[0] = initial_user;
            s_pad_handles[0] = scePadOpen(initial_user, ORBIS_PAD_PORT_TYPE_STANDARD, 0, NULL);
            if (s_pad_handles[0] >= 0) {
                scePadSetLightBar(s_pad_handles[0], (OrbisPadColor*)&s_ds4_colors[0]);
            }
        }
    }
}

OrbisUserServiceUserId input_get_user_id(int slot_index) {
    if (slot_index < 0 || slot_index >= GBA_MAX_INSTANCES) {
        return -1;
    }
    return s_user_ids[slot_index];
}

bool input_init(void) {
    OrbisUserServiceInitializeParams param;
    memset(&param, 0, sizeof(param));
    param.priority = ORBIS_KERNEL_PRIO_FIFO_LOWEST;
    sceUserServiceInitialize(&param);
    scePadInit();

    for (int i = 0; i < GBA_MAX_INSTANCES; ++i) {
        s_pad_handles[i] = -1;
        s_user_ids[i] = -1;
        s_prev_l3_r3[i] = false;
        s_prev_buttons[i] = 0;
    }

    refresh_pad_handles();
    return true;
}

void input_poll_all(struct PS4GBA_Context* ctx) {
    if (!ctx) {
        return;
    }

    if ((++s_hotplug_tick & 63U) == 0U) {
        refresh_pad_handles();
    }

    UIAppState app_state = ui_menu_get_state();

    for (int i = 0; i < GBA_MAX_INSTANCES; ++i) {
        uint32_t b = 0;

        if (s_pad_handles[i] >= 0) {
            OrbisPadData pad_data;
            memset(&pad_data, 0, sizeof(pad_data));
            if (scePadReadState(s_pad_handles[i], &pad_data) >= 0) {
                b = pad_data.buttons;

                if (pad_data.leftStick.x < 64) {
                    b |= ORBIS_PAD_BUTTON_LEFT;
                } else if (pad_data.leftStick.x > 192) {
                    b |= ORBIS_PAD_BUTTON_RIGHT;
                }

                if (pad_data.leftStick.y < 64) {
                    b |= ORBIS_PAD_BUTTON_UP;
                } else if (pad_data.leftStick.y > 192) {
                    b |= ORBIS_PAD_BUTTON_DOWN;
                }
            }
        }

        uint32_t newly_pressed = b & ~s_prev_buttons[i];
        s_prev_buttons[i] = b;

        bool l3_r3_pressed = ((b & (ORBIS_PAD_BUTTON_L3 | ORBIS_PAD_BUTTON_R3)) ==
                              (ORBIS_PAD_BUTTON_L3 | ORBIS_PAD_BUTTON_R3));
        if (l3_r3_pressed && !s_prev_l3_r3[i] && ctx->instances[i].active) {
            audio_set_mode(PS4_AUDIO_MODE_FOCUS);
            audio_set_focus_player(i);
        }
        s_prev_l3_r3[i] = l3_r3_pressed;

        if (app_state == UI_STATE_SPLASH) {
            if (newly_pressed) {
                ui_menu_update_splash(newly_pressed);
                app_state = ui_menu_get_state();
            }
            continue;
        }

        if (app_state == UI_STATE_DISCLAIMER) {
            if (newly_pressed) {
                ui_menu_update_disclaimer(newly_pressed);
                app_state = ui_menu_get_state();
            }
            continue;
        }

        if (app_state == UI_STATE_LAUNCHER) {
            if (newly_pressed) {
                ui_menu_update_launcher(ctx, newly_pressed);
                app_state = ui_menu_get_state();
            }
            continue;
        }

        if (app_state == UI_STATE_OSD) {
            if (newly_pressed) {
                ui_menu_update_osd(ctx, newly_pressed);
                app_state = ui_menu_get_state();
            }
            if (ctx->instances[i].active && ctx->instances[i].core) {
                ctx->instances[i].core->setKeys(ctx->instances[i].core, 0);
            }
            continue;
        }

        /* UI_STATE_INGAME */
        if (newly_pressed & ORBIS_PAD_BUTTON_OPTIONS) {
            /* Drop-In / Hot-Join: se um controle de slot inativo (P2..P4) pressionar OPTIONS, entra na partida */
            if (!ctx->instances[i].active) {
                refresh_pad_handles();
                emulator_join_player(i, s_user_ids[i]);
                continue;
            }

            /* Jogador ja ativo pressionou OPTIONS -> abre o menu OSD */
            ui_menu_open_osd(i);
            app_state = UI_STATE_OSD;
            if (ctx->instances[i].active && ctx->instances[i].core) {
                ctx->instances[i].core->setKeys(ctx->instances[i].core, 0);
            }
            continue;
        }

        if (!ctx->instances[i].active || !ctx->instances[i].core) {
            continue;
        }

        uint16_t gba_keys = 0;

        if (b & ORBIS_PAD_BUTTON_CROSS) {
            gba_keys |= GBA_KEY_A;
        }
        if (b & (ORBIS_PAD_BUTTON_CIRCLE | ORBIS_PAD_BUTTON_SQUARE)) {
            gba_keys |= GBA_KEY_B;
        }
        if (b & ORBIS_PAD_BUTTON_R3) {
            gba_keys |= GBA_KEY_START;
        }
        if (b & (ORBIS_PAD_BUTTON_TOUCH_PAD | ORBIS_PAD_BUTTON_TRIANGLE)) {
            gba_keys |= GBA_KEY_SELECT;
        }

        if (b & ORBIS_PAD_BUTTON_UP) {
            gba_keys |= GBA_KEY_UP;
        }
        if (b & ORBIS_PAD_BUTTON_DOWN) {
            gba_keys |= GBA_KEY_DOWN;
        }
        if (b & ORBIS_PAD_BUTTON_LEFT) {
            gba_keys |= GBA_KEY_LEFT;
        }
        if (b & ORBIS_PAD_BUTTON_RIGHT) {
            gba_keys |= GBA_KEY_RIGHT;
        }

        if (b & (ORBIS_PAD_BUTTON_L1 | ORBIS_PAD_BUTTON_L2)) {
            gba_keys |= GBA_KEY_L;
        }
        if (b & (ORBIS_PAD_BUTTON_R1 | ORBIS_PAD_BUTTON_R2)) {
            gba_keys |= GBA_KEY_R;
        }

        ctx->instances[i].core->setKeys(ctx->instances[i].core, gba_keys);
    }
}

void input_shutdown(void) {
    for (int i = 0; i < GBA_MAX_INSTANCES; ++i) {
        if (s_pad_handles[i] >= 0) {
            scePadResetLightBar(s_pad_handles[i]);
            scePadClose(s_pad_handles[i]);
            s_pad_handles[i] = -1;
        }
        s_user_ids[i] = -1;
    }
}
