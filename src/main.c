#include <stdio.h>
#include <stdbool.h>
#include <stddef.h>
#include <wchar.h>
#include "emulator_manager.h"
#include "video_split.h"
#include "input_ds4.h"
#include "audio_ps4.h"
#include "ui_menu.h"

/* Overrides para evitar que musl_current_locale.o (PT_TLS) seja puxado da libc.a */
char* strerror(int errnum) {
    (void)errnum;
    return (char*)"Orbis error";
}

size_t wcrtomb(char* restrict s, wchar_t wc, mbstate_t* restrict st) {
    (void)st;
    if (!s) {
        return 1;
    }
    *s = (char)(wc & 0xFF);
    return 1;
}

int main(int argc, char* argv[]) {
    (void)argc;
    (void)argv;

    if (!video_init()) {
        return -1;
    }

    input_init();
    audio_init();
    ui_menu_init();

    bool running = true;
    while (running) {
        input_poll_all(&g_gba_ctx);

        if (ui_menu_get_state() == UI_STATE_INGAME) {
            emulator_step_lockstep_frame();
            audio_process(&g_gba_ctx);
        }

        video_render_split_screen(&g_gba_ctx);
    }

    emulator_destroy_all();
    audio_shutdown();
    input_shutdown();
    video_shutdown();

    return 0;
}
