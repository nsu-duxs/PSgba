#ifndef AUDIO_PS4_H
#define AUDIO_PS4_H

#include <stdint.h>
#include <stdbool.h>
#include "emulator_manager.h"

#define PS4_AUDIO_SAMPLE_RATE 48000
#define PS4_AUDIO_GRANULARITY 256
#define GBA_AUDIO_BUFFER_SIZE 8192

typedef enum PS4AudioMode {
    PS4_AUDIO_MODE_FOCUS = 0,
    PS4_AUDIO_MODE_MIX   = 1
} PS4AudioMode;

typedef enum UISFX {
    /* Efeitos da Tela de Inicializacao (Splash / Title Screen) */
    UI_SFX_SPLASH_IMPACT     = 0, /* Impacto inicial do '4' (~110Hz -> ~55Hz) */
    UI_SFX_SPLASH_QUAD_CHIME = 1, /* Arpejo GBA 4 notas (D4, A4, D5, F#5) */
    UI_SFX_SPLASH_SHIMMER    = 2, /* Cintilacao suave criadores (~2.4kHz) */
    UI_SFX_SPLASH_CONFIRM    = 3, /* Confirmacao Title Screen -> Launcher (880Hz -> 1760Hz) */

    /* Efeitos de Navegacao da Interface (Menu SFX) */
    UI_SFX_TAB_SWITCH        = 4, /* Troca de Abas (L1 / R1): blip deslizante ~600Hz (60ms) */
    UI_SFX_VALUE_CHANGE      = 5, /* Alterar Valor (D-PAD Esq/Dir): clique sutil ~440Hz (40ms) */
    UI_SFX_CONFIRM           = 6, /* Confirmar / Selecionar (Cruz / X): toque positivo ~880Hz -> 1320Hz (90ms) */
    UI_SFX_TOUCHPAD          = 7, /* Touchpad: clique holografico ~1200Hz -> 800Hz (80ms) */
    UI_SFX_COUNT             = 8
} UISFX;

/* Aliases de compatibilidade */
typedef UISFX SplashSFX;
#define SPLASH_SFX_IMPACT     UI_SFX_SPLASH_IMPACT
#define SPLASH_SFX_QUAD_CHIME UI_SFX_SPLASH_QUAD_CHIME
#define SPLASH_SFX_SHIMMER    UI_SFX_SPLASH_SHIMMER
#define SPLASH_SFX_CONFIRM    UI_SFX_SPLASH_CONFIRM
#define SPLASH_SFX_COUNT      UI_SFX_COUNT

bool audio_init(void);
void audio_set_mode(PS4AudioMode mode);
PS4AudioMode audio_get_mode(void);
void audio_set_focus_player(int player_id);
int audio_get_focus_player(void);
void audio_process(struct PS4GBA_Context* ctx);
void audio_shutdown(void);

void audio_play_sfx(UISFX type);
void audio_play_splash_sfx(SplashSFX type);
void audio_stop_splash_sfx(void);

void audio_set_menu_sfx_enabled(bool enabled);
bool audio_is_menu_sfx_enabled(void);

#endif /* AUDIO_PS4_H */
