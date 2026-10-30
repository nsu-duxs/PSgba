#include "audio_ps4.h"
#include <string.h>
#include <math.h>
#include <orbis/libkernel.h>
#include <orbis/AudioOut.h>
#include <orbis/UserService.h>
#include <mgba-util/audio-buffer.h>

#define MAX_RESAMPLED_FRAME   2048
#define RING_CAPACITY_FRAMES  8192U
#define RING_FRAME_MASK       (RING_CAPACITY_FRAMES - 1U)
#define RING_TARGET_FILL      1536U

#define PI_F 3.14159265358979323846f

/* ==============================================================================
 * PROCEDURAL RETRO SFX SYNTHESIS (PCM 16-BIT STEREO @ 48kHz, ZERO DISK I/O)
 * ============================================================================== */
#define SFX_IMPACT_SAMPLES      19200U /* 0.40s: Tom grave encorpado 110Hz -> 55Hz */
#define SFX_QUAD_CHIME_SAMPLES  21120U /* 0.44s: Arpejo GBA 4 notas (D4, A4, D5, F#5) */
#define SFX_SHIMMER_SAMPLES     10560U /* 0.22s: Cintilacao suave ~2.4kHz com pan */
#define SFX_CONFIRM_SAMPLES      7200U /* 0.15s: Confirmacao Title Screen (880Hz -> 1760Hz) */
#define SFX_TAB_SWITCH_SAMPLES   2880U /* 0.06s: Troca de Abas L1/R1 ~600Hz descendo */
#define SFX_VALUE_CHANGE_SAMPLES 1920U /* 0.04s: Alterar Valor D-Pad ~440Hz */
#define SFX_CONFIRM_ACT_SAMPLES  4320U /* 0.09s: Confirmar Cruz/X 880Hz -> 1320Hz */
#define SFX_TOUCHPAD_SAMPLES     3840U /* 0.08s: Touchpad holografico 1200Hz -> 800Hz */

typedef struct SFXBuffer {
    int16_t* samples;
    uint32_t num_frames;
} SFXBuffer;

typedef struct SFXVoice {
    volatile uint32_t play_pos;
    volatile int active;
} SFXVoice;

static int16_t s_sfx_impact_buf[SFX_IMPACT_SAMPLES * 2] __attribute__((aligned(64)));
static int16_t s_sfx_quad_buf[SFX_QUAD_CHIME_SAMPLES * 2] __attribute__((aligned(64)));
static int16_t s_sfx_shimmer_buf[SFX_SHIMMER_SAMPLES * 2] __attribute__((aligned(64)));
static int16_t s_sfx_confirm_buf[SFX_CONFIRM_SAMPLES * 2] __attribute__((aligned(64)));
static int16_t s_sfx_tab_switch_buf[SFX_TAB_SWITCH_SAMPLES * 2] __attribute__((aligned(64)));
static int16_t s_sfx_value_change_buf[SFX_VALUE_CHANGE_SAMPLES * 2] __attribute__((aligned(64)));
static int16_t s_sfx_confirm_act_buf[SFX_CONFIRM_ACT_SAMPLES * 2] __attribute__((aligned(64)));
static int16_t s_sfx_touchpad_buf[SFX_TOUCHPAD_SAMPLES * 2] __attribute__((aligned(64)));

static SFXBuffer s_sfx_buffers[UI_SFX_COUNT];
static SFXVoice s_sfx_voices[UI_SFX_COUNT];

static bool s_menu_sfx_enabled = true;

static int32_t s_audio_handle = -1;
static PS4AudioMode s_audio_mode = PS4_AUDIO_MODE_FOCUS;
static int s_focused_player = 0;

static int16_t s_raw_buf[GBA_AUDIO_BUFFER_SIZE * 2] __attribute__((aligned(64)));
static int16_t s_inst_resampled[GBA_MAX_INSTANCES][MAX_RESAMPLED_FRAME * 2] __attribute__((aligned(64)));
static size_t s_inst_frames[GBA_MAX_INSTANCES];
static int16_t s_prev_src_sample[GBA_MAX_INSTANCES][2];

/* Lock-free Single-Producer Single-Consumer stereo ring buffer */
static int16_t s_ring[RING_CAPACITY_FRAMES * 2] __attribute__((aligned(64)));
static volatile uint32_t s_ring_write __attribute__((aligned(64))) = 0;
static volatile uint32_t s_ring_read __attribute__((aligned(64))) = 0;

static int16_t s_out_buffers[2][PS4_AUDIO_GRANULARITY * 2] __attribute__((aligned(64)));
static int s_out_flip = 0;

static OrbisPthread s_audio_thread;
static volatile int s_audio_thread_running = 0;
static bool s_audio_thread_created = false;

static inline int16_t clamp_s16(int32_t val) {
    if (val > 32767) {
        return 32767;
    }
    if (val < -32768) {
        return -32768;
    }
    return (int16_t)val;
}

static inline uint32_t ring_available_to_read(void) {
    uint32_t w = __atomic_load_n(&s_ring_write, __ATOMIC_ACQUIRE);
    uint32_t r = __atomic_load_n(&s_ring_read, __ATOMIC_RELAXED);
    return w - r;
}

static inline uint32_t ring_available_to_write(void) {
    uint32_t w = __atomic_load_n(&s_ring_write, __ATOMIC_RELAXED);
    uint32_t r = __atomic_load_n(&s_ring_read, __ATOMIC_ACQUIRE);
    uint32_t used = w - r;
    if (used >= RING_CAPACITY_FRAMES) {
        return 0;
    }
    return RING_CAPACITY_FRAMES - used;
}

static inline void ring_push_frames(const int16_t* __restrict frames, size_t count) {
    uint32_t avail = ring_available_to_write();
    if (count > avail) {
        count = avail;
    }
    uint32_t w = __atomic_load_n(&s_ring_write, __ATOMIC_RELAXED);
    for (size_t i = 0; i < count; ++i) {
        uint32_t idx = (w + (uint32_t)i) & RING_FRAME_MASK;
        s_ring[idx * 2 + 0] = frames[i * 2 + 0];
        s_ring[idx * 2 + 1] = frames[i * 2 + 1];
    }
    __atomic_store_n(&s_ring_write, w + (uint32_t)count, __ATOMIC_RELEASE);
}

/*
 * Sintese 1: Impacto do "4" (UI_SFX_SPLASH_IMPACT)
 * Tom grave e encorpado: 110 Hz descendo suavemente para 55 Hz em 0.4s.
 */
static void sfx_synth_impact(int16_t* dst, uint32_t count) {
    float phase = 0.0f;
    float sub_phase = 0.0f;
    for (uint32_t i = 0; i < count; ++i) {
        float tau = (float)i / (float)count;
        float freq = 110.0f - 55.0f * tau;
        float delta = (2.0f * PI_F * freq) / 48000.0f;
        phase += delta;
        sub_phase += delta * 0.5f;

        float env_att = (i < 384) ? ((float)i / 384.0f) : 1.0f;
        float decay = 1.0f - tau;
        float env_dec = decay * decay * (1.0f - 0.2f * tau);

        float s_main = sinf(phase);
        float s_sub = sinf(sub_phase);
        float s_harm = sinf(phase) - 0.11f * sinf(3.0f * phase);

        float wave = 0.65f * s_main + 0.25f * s_sub + 0.10f * s_harm;
        int32_t val = (int32_t)(24000.0f * env_att * env_dec * wave);
        int16_t s = clamp_s16(val);
        dst[i * 2 + 0] = s;
        dst[i * 2 + 1] = s;
    }
}

/*
 * Sintese 2: Divisao dos 4 Quadrantes / Arpejo GBA (UI_SFX_SPLASH_QUAD_CHIME)
 * Homenagem ao chime de inicializacao do GBA: 4 notas em onda quadrada (Re4, La4, Re5, Fa#5).
 */
static void sfx_synth_quad_chime(int16_t* dst, uint32_t count) {
    memset(dst, 0, count * 2 * sizeof(int16_t));

    float note_freqs[4] = { 293.66f, 440.00f, 587.33f, 739.99f };
    uint32_t note_starts[4] = { 0, 3120, 6240, 9360 };
    uint32_t note_lengths[4] = { 5280, 5280, 5280, 11760 };
    float pan_l[4] = { 0.90f, 0.45f, 0.85f, 0.70f };
    float pan_r[4] = { 0.45f, 0.90f, 0.50f, 0.90f };

    for (int n = 0; n < 4; ++n) {
        float freq = note_freqs[n];
        uint32_t start = note_starts[n];
        uint32_t len = note_lengths[n];
        float phase = 0.0f;

        for (uint32_t j = 0; j < len; ++j) {
            uint32_t idx = start + j;
            if (idx >= count) {
                break;
            }

            float tau = (float)j / (float)len;
            float cur_f = freq;
            if (n == 3 && j > 2400) {
                cur_f += 3.5f * sinf(2.0f * PI_F * 6.0f * (float)(j - 2400) / 48000.0f);
            }

            phase += (2.0f * PI_F * cur_f) / 48000.0f;

            float sq = 0.72f * sinf(phase) + 0.20f * sinf(3.0f * phase) + 0.08f * sinf(5.0f * phase);
            float att = (j < 144) ? ((float)j / 144.0f) : 1.0f;
            float d = 1.0f - tau;
            float dec = (n < 3) ? (d * d) : (d * d * (1.0f - 0.4f * tau));

            float amp = 16000.0f * att * dec;
            int32_t samp_l = (int32_t)(amp * sq * pan_l[n]);
            int32_t samp_r = (int32_t)(amp * sq * pan_r[n]);

            int32_t out_l = (int32_t)dst[idx * 2 + 0] + samp_l;
            int32_t out_r = (int32_t)dst[idx * 2 + 1] + samp_r;
            dst[idx * 2 + 0] = clamp_s16(out_l);
            dst[idx * 2 + 1] = clamp_s16(out_r);
        }
    }
}

/*
 * Sintese 3: Shimmer / Brilho dos Criadores (UI_SFX_SPLASH_SHIMMER)
 * Cintilacao suave de alta frequencia ~2.4 kHz com varredura estereo esquerda-para-direita.
 */
static void sfx_synth_shimmer(int16_t* dst, uint32_t count) {
    float phase_a = 0.0f;
    float phase_b = 0.0f;
    float phase_c = 0.0f;
    float f_a = 2400.0f;
    float f_b = 2418.0f;
    float f_c = 4800.0f;

    for (uint32_t i = 0; i < count; ++i) {
        float tau = (float)i / (float)count;
        phase_a += (2.0f * PI_F * f_a) / 48000.0f;
        phase_b += (2.0f * PI_F * f_b) / 48000.0f;
        phase_c += (2.0f * PI_F * f_c) / 48000.0f;

        float wave = 0.55f * sinf(phase_a) + 0.35f * sinf(phase_b) + 0.10f * sinf(phase_c);

        float att = (i < 480) ? ((float)i / 480.0f) : 1.0f;
        float d = 1.0f - tau;
        float dec = d * d * d;

        float pan_l = 0.90f - 0.65f * tau;
        float pan_r = 0.25f + 0.65f * tau;

        float amp = 12000.0f * att * dec;
        dst[i * 2 + 0] = clamp_s16((int32_t)(amp * wave * pan_l));
        dst[i * 2 + 1] = clamp_s16((int32_t)(amp * wave * pan_r));
    }
}

/*
 * Sintese 4: Confirmacao Title Screen (UI_SFX_SPLASH_CONFIRM)
 * Dois tons harmonicos (880 Hz seguido de 1760 Hz em 150ms).
 */
static void sfx_synth_confirm_splash(int16_t* dst, uint32_t count) {
    memset(dst, 0, count * 2 * sizeof(int16_t));

    /* Tom 1: 880 Hz */
    float phase1 = 0.0f;
    for (uint32_t i = 0; i < 3360 && i < count; ++i) {
        float tau = (float)i / 3360.0f;
        phase1 += (2.0f * PI_F * 880.0f) / 48000.0f;
        float w1 = 0.75f * sinf(phase1) + 0.25f * sinf(3.0f * phase1);
        float att1 = (i < 96) ? ((float)i / 96.0f) : 1.0f;
        float d1 = 1.0f - tau;
        float dec1 = d1 * d1;
        int32_t s1 = (int32_t)(16000.0f * att1 * dec1 * w1);
        dst[i * 2 + 0] = clamp_s16(s1);
        dst[i * 2 + 1] = clamp_s16(s1);
    }

    /* Tom 2: 1760 Hz */
    float phase2 = 0.0f;
    for (uint32_t j = 0; j < 4320; ++j) {
        uint32_t idx = 2880 + j;
        if (idx >= count) {
            break;
        }
        float tau = (float)j / 4320.0f;
        phase2 += (2.0f * PI_F * 1760.0f) / 48000.0f;
        float w2 = 0.80f * sinf(phase2) + 0.20f * sinf(3.0f * phase2);
        float att2 = (j < 96) ? ((float)j / 96.0f) : 1.0f;
        float d2 = 1.0f - tau;
        float dec2 = d2 * d2;
        int32_t s2 = (int32_t)(18000.0f * att2 * dec2 * w2);
        int32_t out_l = (int32_t)dst[idx * 2 + 0] + s2;
        int32_t out_r = (int32_t)dst[idx * 2 + 1] + s2;
        dst[idx * 2 + 0] = clamp_s16(out_l);
        dst[idx * 2 + 1] = clamp_s16(out_r);
    }
}

/*
 * Sintese 5: Troca de Abas L1 / R1 (UI_SFX_TAB_SWITCH)
 * Blip deslizante rapido e limpo: onda quadrada ~600 Hz descendo para ~520 Hz em 60ms.
 */
static void sfx_synth_tab_switch(int16_t* dst, uint32_t count) {
    float phase = 0.0f;
    for (uint32_t i = 0; i < count; ++i) {
        float tau = (float)i / (float)count;
        float freq = 620.0f - 100.0f * tau;
        phase += (2.0f * PI_F * freq) / 48000.0f;

        float sq = 0.70f * sinf(phase) + 0.22f * sinf(3.0f * phase) + 0.08f * sinf(5.0f * phase);
        float att = (i < 72) ? ((float)i / 72.0f) : 1.0f;
        float dec = (1.0f - tau);
        float env = att * dec * dec;

        int32_t val = (int32_t)(14000.0f * env * sq);
        int16_t s = clamp_s16(val);
        dst[i * 2 + 0] = s;
        dst[i * 2 + 1] = s;
    }
}

/*
 * Sintese 6: Alterar Valor de Configuracao (UI_SFX_VALUE_CHANGE)
 * Clique sutil e curto: pulso senoidal ~440 Hz de 40ms.
 */
static void sfx_synth_value_change(int16_t* dst, uint32_t count) {
    float phase = 0.0f;
    for (uint32_t i = 0; i < count; ++i) {
        float tau = (float)i / (float)count;
        phase += (2.0f * PI_F * 440.0f) / 48000.0f;

        float wave = 0.85f * sinf(phase) + 0.15f * sinf(2.0f * phase);
        float att = (i < 48) ? ((float)i / 48.0f) : 1.0f;
        float dec = (1.0f - tau) * (1.0f - tau) * (1.0f - tau);
        float env = att * dec;

        int32_t val = (int32_t)(13000.0f * env * wave);
        int16_t s = clamp_s16(val);
        dst[i * 2 + 0] = s;
        dst[i * 2 + 1] = s;
    }
}

/*
 * Sintese 7: Confirmar / Selecionar Algo (UI_SFX_CONFIRM)
 * Toque agudo e positivo: dois tons harmonicos rapidos (~880 Hz -> 1320 Hz em 90ms).
 */
static void sfx_synth_confirm_action(int16_t* dst, uint32_t count) {
    memset(dst, 0, count * 2 * sizeof(int16_t));

    /* Tom 1: 880 Hz (0 a 2160 samples = 45ms) */
    float phase1 = 0.0f;
    for (uint32_t i = 0; i < 2160 && i < count; ++i) {
        float tau = (float)i / 2160.0f;
        phase1 += (2.0f * PI_F * 880.0f) / 48000.0f;
        float w1 = 0.80f * sinf(phase1) + 0.20f * sinf(3.0f * phase1);
        float att = (i < 48) ? ((float)i / 48.0f) : 1.0f;
        float dec = (1.0f - tau) * (1.0f - tau);
        int32_t s = (int32_t)(17000.0f * att * dec * w1);
        dst[i * 2 + 0] = clamp_s16(s);
        dst[i * 2 + 1] = clamp_s16(s);
    }

    /* Tom 2: 1320 Hz (1680 a 4320 samples = 55ms com overlap) */
    float phase2 = 0.0f;
    for (uint32_t j = 0; j < 2640; ++j) {
        uint32_t idx = 1680 + j;
        if (idx >= count) {
            break;
        }
        float tau = (float)j / 2640.0f;
        phase2 += (2.0f * PI_F * 1320.0f) / 48000.0f;
        float w2 = 0.85f * sinf(phase2) + 0.15f * sinf(2.0f * phase2);
        float att = (j < 48) ? ((float)j / 48.0f) : 1.0f;
        float dec = (1.0f - tau) * (1.0f - tau);
        int32_t s2 = (int32_t)(17000.0f * att * dec * w2);
        int32_t out_l = (int32_t)dst[idx * 2 + 0] + s2;
        int32_t out_r = (int32_t)dst[idx * 2 + 1] + s2;
        dst[idx * 2 + 0] = clamp_s16(out_l);
        dst[idx * 2 + 1] = clamp_s16(out_r);
    }
}

/*
 * Sintese 8: Acionar o Touchpad (UI_SFX_TOUCHPAD)
 * Clique eletronico holografico: varredura de frequencia ~1200 Hz -> 800 Hz em 80ms.
 */
static void sfx_synth_touchpad(int16_t* dst, uint32_t count) {
    float phase = 0.0f;
    for (uint32_t i = 0; i < count; ++i) {
        float tau = (float)i / (float)count;
        float freq = 1200.0f - 400.0f * tau;
        phase += (2.0f * PI_F * freq) / 48000.0f;

        float wave = 0.70f * sinf(phase) + 0.20f * sinf(2.0f * phase) + 0.10f * sinf(3.0f * phase);
        float att = (i < 48) ? ((float)i / 48.0f) : 1.0f;
        float dec = (1.0f - tau) * (1.0f - tau);
        float amp = 16000.0f * att * dec;

        float pan_l = 0.85f - 0.15f * tau;
        float pan_r = 0.70f + 0.15f * tau;

        dst[i * 2 + 0] = clamp_s16((int32_t)(amp * wave * pan_l));
        dst[i * 2 + 1] = clamp_s16((int32_t)(amp * wave * pan_r));
    }
}

static void sfx_synthesize_all(void) {
    s_sfx_buffers[UI_SFX_SPLASH_IMPACT].samples = s_sfx_impact_buf;
    s_sfx_buffers[UI_SFX_SPLASH_IMPACT].num_frames = SFX_IMPACT_SAMPLES;
    sfx_synth_impact(s_sfx_impact_buf, SFX_IMPACT_SAMPLES);

    s_sfx_buffers[UI_SFX_SPLASH_QUAD_CHIME].samples = s_sfx_quad_buf;
    s_sfx_buffers[UI_SFX_SPLASH_QUAD_CHIME].num_frames = SFX_QUAD_CHIME_SAMPLES;
    sfx_synth_quad_chime(s_sfx_quad_buf, SFX_QUAD_CHIME_SAMPLES);

    s_sfx_buffers[UI_SFX_SPLASH_SHIMMER].samples = s_sfx_shimmer_buf;
    s_sfx_buffers[UI_SFX_SPLASH_SHIMMER].num_frames = SFX_SHIMMER_SAMPLES;
    sfx_synth_shimmer(s_sfx_shimmer_buf, SFX_SHIMMER_SAMPLES);

    s_sfx_buffers[UI_SFX_SPLASH_CONFIRM].samples = s_sfx_confirm_buf;
    s_sfx_buffers[UI_SFX_SPLASH_CONFIRM].num_frames = SFX_CONFIRM_SAMPLES;
    sfx_synth_confirm_splash(s_sfx_confirm_buf, SFX_CONFIRM_SAMPLES);

    s_sfx_buffers[UI_SFX_TAB_SWITCH].samples = s_sfx_tab_switch_buf;
    s_sfx_buffers[UI_SFX_TAB_SWITCH].num_frames = SFX_TAB_SWITCH_SAMPLES;
    sfx_synth_tab_switch(s_sfx_tab_switch_buf, SFX_TAB_SWITCH_SAMPLES);

    s_sfx_buffers[UI_SFX_VALUE_CHANGE].samples = s_sfx_value_change_buf;
    s_sfx_buffers[UI_SFX_VALUE_CHANGE].num_frames = SFX_VALUE_CHANGE_SAMPLES;
    sfx_synth_value_change(s_sfx_value_change_buf, SFX_VALUE_CHANGE_SAMPLES);

    s_sfx_buffers[UI_SFX_CONFIRM].samples = s_sfx_confirm_act_buf;
    s_sfx_buffers[UI_SFX_CONFIRM].num_frames = SFX_CONFIRM_ACT_SAMPLES;
    sfx_synth_confirm_action(s_sfx_confirm_act_buf, SFX_CONFIRM_ACT_SAMPLES);

    s_sfx_buffers[UI_SFX_TOUCHPAD].samples = s_sfx_touchpad_buf;
    s_sfx_buffers[UI_SFX_TOUCHPAD].num_frames = SFX_TOUCHPAD_SAMPLES;
    sfx_synth_touchpad(s_sfx_touchpad_buf, SFX_TOUCHPAD_SAMPLES);
}

void audio_set_menu_sfx_enabled(bool enabled) {
    s_menu_sfx_enabled = enabled;
}

bool audio_is_menu_sfx_enabled(void) {
    return s_menu_sfx_enabled;
}

void audio_play_sfx(UISFX type) {
    if (type < 0 || type >= UI_SFX_COUNT) {
        return;
    }

    /* Se desativado, silencia sons de navegacao e menu */
    if (!s_menu_sfx_enabled && type >= UI_SFX_TAB_SWITCH) {
        return;
    }

    if (type == UI_SFX_SPLASH_CONFIRM) {
        /* Interrompe caudas dos outros sons da splash para transicao limpa */
        for (int v = 0; v < UI_SFX_COUNT; ++v) {
            if (v != UI_SFX_SPLASH_CONFIRM) {
                __atomic_store_n(&s_sfx_voices[v].active, 0, __ATOMIC_RELEASE);
            }
        }
    }

    __atomic_store_n(&s_sfx_voices[type].play_pos, 0, __ATOMIC_RELEASE);
    __atomic_store_n(&s_sfx_voices[type].active, 1, __ATOMIC_RELEASE);
}

void audio_play_splash_sfx(SplashSFX type) {
    audio_play_sfx(type);
}

void audio_stop_splash_sfx(void) {
    for (int v = 0; v < UI_SFX_COUNT; ++v) {
        __atomic_store_n(&s_sfx_voices[v].active, 0, __ATOMIC_RELEASE);
    }
}

/*
 * Dedicated PS4 Audio Output Thread:
 * Blocks ONLY this worker thread inside sceAudioOutOutput(), freeing the main thread
 * to hit 60 Hz VSync every frame without audio/video synchronization stalls.
 */
static void* audio_output_thread_main(void* arg) {
    (void)arg;
    /* Fixa a thread de audio no Core 4 (segundo cluster Jaguar) para nunca disputar
     * os Cores 0..3 onde rodam as 4 instancias do mGBA em paralelo! */
    scePthreadSetaffinity(s_audio_thread, 1ULL << 4);

    int32_t last_l = 0;
    int32_t last_r = 0;

    while (__atomic_load_n(&s_audio_thread_running, __ATOMIC_ACQUIRE)) {
        int16_t* out_ptr = s_out_buffers[s_out_flip];
        uint32_t avail = ring_available_to_read();
        uint32_t r = __atomic_load_n(&s_ring_read, __ATOMIC_RELAXED);

        if (avail >= PS4_AUDIO_GRANULARITY) {
            for (uint32_t i = 0; i < PS4_AUDIO_GRANULARITY; ++i) {
                uint32_t idx = (r + i) & RING_FRAME_MASK;
                int16_t l = s_ring[idx * 2 + 0];
                int16_t rt = s_ring[idx * 2 + 1];
                out_ptr[i * 2 + 0] = l;
                out_ptr[i * 2 + 1] = rt;
            }
            last_l = out_ptr[(PS4_AUDIO_GRANULARITY - 1) * 2 + 0];
            last_r = out_ptr[(PS4_AUDIO_GRANULARITY - 1) * 2 + 1];
            __atomic_store_n(&s_ring_read, r + PS4_AUDIO_GRANULARITY, __ATOMIC_RELEASE);
        } else {
            /* Smoothly drain any remaining samples and decay DC to zero to avoid pops/crackles */
            for (uint32_t i = 0; i < PS4_AUDIO_GRANULARITY; ++i) {
                if (i < avail) {
                    uint32_t idx = (r + i) & RING_FRAME_MASK;
                    last_l = s_ring[idx * 2 + 0];
                    last_r = s_ring[idx * 2 + 1];
                } else {
                    last_l = (last_l * 63) >> 6;
                    last_r = (last_r * 63) >> 6;
                }
                out_ptr[i * 2 + 0] = (int16_t)last_l;
                out_ptr[i * 2 + 1] = (int16_t)last_r;
            }
            __atomic_store_n(&s_ring_read, r + avail, __ATOMIC_RELEASE);
        }

        /* Mistura das vozes ativas de efeitos sonoros procedurais (Splash / UI SFX) */
        for (int v = 0; v < UI_SFX_COUNT; ++v) {
            if (__atomic_load_n(&s_sfx_voices[v].active, __ATOMIC_ACQUIRE)) {
                uint32_t pos = __atomic_load_n(&s_sfx_voices[v].play_pos, __ATOMIC_RELAXED);
                uint32_t total = s_sfx_buffers[v].num_frames;
                const int16_t* sfx_src = s_sfx_buffers[v].samples;

                uint32_t frames_to_mix = PS4_AUDIO_GRANULARITY;
                if (pos + frames_to_mix > total) {
                    frames_to_mix = (pos < total) ? (total - pos) : 0;
                }

                for (uint32_t i = 0; i < frames_to_mix; ++i) {
                    int32_t mix_l = (int32_t)out_ptr[i * 2 + 0] + (int32_t)sfx_src[(pos + i) * 2 + 0];
                    int32_t mix_r = (int32_t)out_ptr[i * 2 + 1] + (int32_t)sfx_src[(pos + i) * 2 + 1];
                    out_ptr[i * 2 + 0] = clamp_s16(mix_l);
                    out_ptr[i * 2 + 1] = clamp_s16(mix_r);
                }

                uint32_t next_pos = pos + PS4_AUDIO_GRANULARITY;
                if (next_pos >= total) {
                    __atomic_store_n(&s_sfx_voices[v].active, 0, __ATOMIC_RELEASE);
                    __atomic_store_n(&s_sfx_voices[v].play_pos, total, __ATOMIC_RELEASE);
                } else {
                    __atomic_store_n(&s_sfx_voices[v].play_pos, next_pos, __ATOMIC_RELEASE);
                }
            }
        }

        if (s_audio_handle >= 0) {
            sceAudioOutOutput(s_audio_handle, out_ptr);
        }
        s_out_flip ^= 1;
    }

    return NULL;
}

/*
 * Linear-interpolated stereo resampler with frame-to-frame continuity and dynamic rate control.
 * Stretches/compresses the frames generated in 1 GBA video frame to match the target 48kHz
 * frame count (~800 samples/frame), preventing buffer drift and 60Hz boundary clicks.
 */
static size_t resample_stereo_linear(
    int inst_id,
    const int16_t* __restrict src,
    size_t src_frames,
    size_t target_dst_frames,
    int16_t* __restrict dst,
    size_t max_dst_frames
) {
    if (src_frames == 0 || target_dst_frames == 0) {
        return 0;
    }
    if (target_dst_frames > max_dst_frames) {
        target_dst_frames = max_dst_frames;
    }

    int32_t prev_l = s_prev_src_sample[inst_id][0];
    int32_t prev_r = s_prev_src_sample[inst_id][1];

    uint32_t step_fp = (uint32_t)(((uint64_t)src_frames << 16) / target_dst_frames);
    uint32_t pos_fp = 0;

    for (size_t i = 0; i < target_dst_frames; ++i) {
        uint32_t idx = pos_fp >> 16;
        int32_t frac = (int32_t)(pos_fp & 0xFFFFU);

        int32_t l0, r0, l1, r1;
        if (idx == 0) {
            l0 = prev_l;
            r0 = prev_r;
            l1 = src[0];
            r1 = src[1];
        } else {
            size_t i0 = (size_t)(idx - 1);
            if (i0 >= src_frames) {
                i0 = src_frames - 1;
            }
            size_t i1 = (size_t)idx;
            if (i1 >= src_frames) {
                i1 = src_frames - 1;
            }
            l0 = src[i0 * 2 + 0];
            r0 = src[i0 * 2 + 1];
            l1 = src[i1 * 2 + 0];
            r1 = src[i1 * 2 + 1];
        }

        dst[i * 2 + 0] = (int16_t)(l0 + (((l1 - l0) * frac) >> 16));
        dst[i * 2 + 1] = (int16_t)(r0 + (((r1 - r0) * frac) >> 16));
        pos_fp += step_fp;
    }

    s_prev_src_sample[inst_id][0] = src[(src_frames - 1) * 2 + 0];
    s_prev_src_sample[inst_id][1] = src[(src_frames - 1) * 2 + 1];
    return target_dst_frames;
}

bool audio_init(void) {
    sceAudioOutInit();

    s_audio_handle = sceAudioOutOpen(
        ORBIS_USER_SERVICE_USER_ID_SYSTEM,
        ORBIS_AUDIO_OUT_PORT_TYPE_MAIN,
        0,
        PS4_AUDIO_GRANULARITY,
        PS4_AUDIO_SAMPLE_RATE,
        ORBIS_AUDIO_OUT_PARAM_FORMAT_S16_STEREO
    );

    if (s_audio_handle < 0) {
        OrbisUserServiceLoginUserIdList user_list;
        memset(&user_list, 0, sizeof(user_list));
        if (sceUserServiceGetLoginUserIdList(&user_list) >= 0 && user_list.userId[0] > 0) {
            s_audio_handle = sceAudioOutOpen(
                user_list.userId[0],
                ORBIS_AUDIO_OUT_PORT_TYPE_MAIN,
                0,
                PS4_AUDIO_GRANULARITY,
                PS4_AUDIO_SAMPLE_RATE,
                ORBIS_AUDIO_OUT_PARAM_FORMAT_S16_STEREO
            );
        }
    }

    s_audio_mode = PS4_AUDIO_MODE_FOCUS;
    s_focused_player = 0;
    s_ring_write = RING_TARGET_FILL;
    s_ring_read = 0;
    s_out_flip = 0;
    memset(s_ring, 0, sizeof(s_ring));
    memset(s_out_buffers, 0, sizeof(s_out_buffers));
    memset(s_prev_src_sample, 0, sizeof(s_prev_src_sample));

    sfx_synthesize_all();
    for (int v = 0; v < UI_SFX_COUNT; ++v) {
        s_sfx_voices[v].play_pos = 0;
        s_sfx_voices[v].active = 0;
    }

    if (s_audio_handle >= 0 && !s_audio_thread_created) {
        __atomic_store_n(&s_audio_thread_running, 1, __ATOMIC_RELEASE);
        if (scePthreadCreate(&s_audio_thread, NULL, audio_output_thread_main, NULL, "gba4p_audio") == 0) {
            s_audio_thread_created = true;
            scePthreadSetaffinity(s_audio_thread, 1ULL << 4);
        } else {
            __atomic_store_n(&s_audio_thread_running, 0, __ATOMIC_RELEASE);
        }
    }

    return true;
}

void audio_set_mode(PS4AudioMode mode) {
    s_audio_mode = mode;
}

PS4AudioMode audio_get_mode(void) {
    return s_audio_mode;
}

void audio_set_focus_player(int player_id) {
    if (player_id >= 0 && player_id < GBA_MAX_INSTANCES) {
        s_focused_player = player_id;
    }
}

int audio_get_focus_player(void) {
    return s_focused_player;
}

static inline size_t compute_target_48k_frames(void) {
    /* 48000 Hz / 60 fps = 800 frames por quadro de video; ajuste suave pelo nivel do ring buffer */
    uint32_t fill = ring_available_to_read();
    int32_t diff = (int32_t)RING_TARGET_FILL - (int32_t)fill;
    int32_t adj = diff / 64;
    if (adj > 16) {
        adj = 16;
    } else if (adj < -16) {
        adj = -16;
    }
    return (size_t)(800 + adj);
}

void audio_process(struct PS4GBA_Context* ctx) {
    if (!ctx || s_audio_handle < 0) {
        return;
    }

    size_t target_frames = compute_target_48k_frames();

    if (s_audio_mode == PS4_AUDIO_MODE_FOCUS) {
        int focus = s_focused_player;
        if (focus < 0 || focus >= ctx->active_count) {
            focus = 0;
        }

        for (int i = 0; i < GBA_MAX_INSTANCES; ++i) {
            if (!ctx->instances[i].active || !ctx->instances[i].core) {
                continue;
            }

            struct mCore* core = ctx->instances[i].core;
            struct mAudioBuffer* abuf = core->getAudioBuffer(core);
            if (!abuf) {
                continue;
            }

            if (i != focus) {
                mAudioBufferClear(abuf);
                continue;
            }

            size_t avail = mAudioBufferAvailable(abuf);
            if (avail > GBA_AUDIO_BUFFER_SIZE) {
                avail = GBA_AUDIO_BUFFER_SIZE;
            }
            if (avail > 0) {
                size_t read_frames = mAudioBufferRead(abuf, s_raw_buf, avail);
                size_t resampled = resample_stereo_linear(
                    i, s_raw_buf, read_frames, target_frames,
                    s_inst_resampled[0], MAX_RESAMPLED_FRAME
                );
                ring_push_frames(s_inst_resampled[0], resampled);
            }
        }
    } else {
        size_t max_frames = 0;

        for (int i = 0; i < GBA_MAX_INSTANCES; ++i) {
            s_inst_frames[i] = 0;
            if (!ctx->instances[i].active || !ctx->instances[i].core) {
                continue;
            }

            struct mCore* core = ctx->instances[i].core;
            struct mAudioBuffer* abuf = core->getAudioBuffer(core);
            if (!abuf) {
                continue;
            }

            size_t avail = mAudioBufferAvailable(abuf);
            if (avail > GBA_AUDIO_BUFFER_SIZE) {
                avail = GBA_AUDIO_BUFFER_SIZE;
            }
            if (avail > 0) {
                size_t read_frames = mAudioBufferRead(abuf, s_raw_buf, avail);
                s_inst_frames[i] = resample_stereo_linear(
                    i, s_raw_buf, read_frames, target_frames,
                    s_inst_resampled[i], MAX_RESAMPLED_FRAME
                );
                if (s_inst_frames[i] > max_frames) {
                    max_frames = s_inst_frames[i];
                }
            }
        }

        if (max_frames > 0) {
            for (size_t f = 0; f < max_frames; ++f) {
                int32_t mix_l = 0;
                int32_t mix_r = 0;

                for (int i = 0; i < GBA_MAX_INSTANCES; ++i) {
                    if (f < s_inst_frames[i]) {
                        mix_l += ((int32_t)s_inst_resampled[i][f * 2 + 0]) >> 2;
                        mix_r += ((int32_t)s_inst_resampled[i][f * 2 + 1]) >> 2;
                    }
                }

                s_inst_resampled[0][f * 2 + 0] = clamp_s16(mix_l);
                s_inst_resampled[0][f * 2 + 1] = clamp_s16(mix_r);
            }
            ring_push_frames(s_inst_resampled[0], max_frames);
        }
    }
}

void audio_shutdown(void) {
    audio_stop_splash_sfx();
    if (s_audio_thread_created) {
        __atomic_store_n(&s_audio_thread_running, 0, __ATOMIC_RELEASE);
        scePthreadJoin(s_audio_thread, NULL);
        s_audio_thread_created = false;
    }
    if (s_audio_handle >= 0) {
        sceAudioOutOutput(s_audio_handle, NULL);
        sceAudioOutClose(s_audio_handle);
        s_audio_handle = -1;
    }
    s_ring_write = 0;
    s_ring_read = 0;
}
