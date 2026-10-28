#include "audio_ps4.h"
#include <string.h>
#include <orbis/libkernel.h>
#include <orbis/AudioOut.h>
#include <orbis/UserService.h>
#include <mgba-util/audio-buffer.h>

#define MAX_RESAMPLED_FRAME   2048
#define RING_CAPACITY_FRAMES  8192U
#define RING_FRAME_MASK       (RING_CAPACITY_FRAMES - 1U)
#define RING_TARGET_FILL      1536U

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
 * Dedicated PS4 Audio Output Thread:
 * Blocks ONLY this worker thread inside sceAudioOutOutput(), freeing the main thread
 * to hit 60 Hz VSync every frame without audio/video synchronization stalls.
 */
static void* audio_output_thread_main(void* arg) {
    (void)arg;
    /* Fixa a thread de áudio no Core 4 (segundo cluster Jaguar) para nunca disputar
     * os Cores 0..3 onde rodam as 4 instâncias do mGBA em paralelo! */
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
    /* 48000 Hz / 60 fps = 800 frames por quadro de vídeo; ajuste suave pelo nível do ring buffer */
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

