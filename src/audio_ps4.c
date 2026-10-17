#include "audio_ps4.h"
#include <string.h>
#include <orbis/AudioOut.h>
#include <orbis/UserService.h>
#include <mgba-util/audio-buffer.h>

#define MAX_RESAMPLED_FRAME 2048
#define FIFO_CAPACITY_FRAMES 4096

static int32_t s_audio_handle = -1;
static PS4AudioMode s_audio_mode = PS4_AUDIO_MODE_FOCUS;
static int s_focused_player = 0;

static int16_t s_raw_buf[GBA_AUDIO_BUFFER_SIZE * 2];
static int16_t s_inst_resampled[GBA_MAX_INSTANCES][MAX_RESAMPLED_FRAME * 2];
static size_t s_inst_frames[GBA_MAX_INSTANCES];

static int16_t s_fifo[FIFO_CAPACITY_FRAMES * 2];
static size_t s_fifo_frames = 0;

static int16_t s_out_buffers[2][PS4_AUDIO_GRANULARITY * 2];
static int s_out_flip = 0;

static inline int16_t clamp_s16(int32_t val) {
    if (val > 32767) {
        return 32767;
    }
    if (val < -32768) {
        return -32768;
    }
    return (int16_t)val;
}

static size_t resample_stereo_to_48k(
    const int16_t* __restrict src,
    size_t src_frames,
    unsigned src_rate,
    int16_t* __restrict dst,
    size_t max_dst_frames
) {
    if (src_frames == 0 || src_rate == 0) {
        return 0;
    }

    size_t dst_frames = (src_frames * PS4_AUDIO_SAMPLE_RATE) / src_rate;
    if (dst_frames > max_dst_frames) {
        dst_frames = max_dst_frames;
    }

    uint32_t step_fp = (uint32_t)(((uint64_t)src_rate << 16) / PS4_AUDIO_SAMPLE_RATE);
    uint32_t pos_fp = 0;

    for (size_t i = 0; i < dst_frames; ++i) {
        size_t idx = pos_fp >> 16;
        if (idx >= src_frames) {
            idx = src_frames - 1;
        }
        dst[i * 2 + 0] = src[idx * 2 + 0];
        dst[i * 2 + 1] = src[idx * 2 + 1];
        pos_fp += step_fp;
    }

    return dst_frames;
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
    s_fifo_frames = 0;
    s_out_flip = 0;
    memset(s_fifo, 0, sizeof(s_fifo));
    memset(s_out_buffers, 0, sizeof(s_out_buffers));

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

void audio_process(struct PS4GBA_Context* ctx) {
    if (!ctx || s_audio_handle < 0) {
        return;
    }

    if (s_audio_mode == PS4_AUDIO_MODE_FOCUS) {
        for (int i = 0; i < GBA_MAX_INSTANCES; ++i) {
            if (!ctx->instances[i].active || !ctx->instances[i].core) {
                continue;
            }

            struct mCore* core = ctx->instances[i].core;
            struct mAudioBuffer* abuf = core->getAudioBuffer(core);
            if (!abuf) {
                continue;
            }

            if (i != s_focused_player) {
                mAudioBufferClear(abuf);
                continue;
            }

            size_t avail = mAudioBufferAvailable(abuf);
            if (avail > GBA_AUDIO_BUFFER_SIZE) {
                avail = GBA_AUDIO_BUFFER_SIZE;
            }
            if (avail > 0) {
                size_t read_frames = mAudioBufferRead(abuf, s_raw_buf, avail);
                unsigned rate = core->audioSampleRate(core);
                size_t resampled = resample_stereo_to_48k(
                    s_raw_buf, read_frames, rate,
                    s_inst_resampled[0], MAX_RESAMPLED_FRAME
                );

                if (s_fifo_frames + resampled <= FIFO_CAPACITY_FRAMES) {
                    memcpy(&s_fifo[s_fifo_frames * 2],
                           s_inst_resampled[0],
                           resampled * 2 * sizeof(int16_t));
                    s_fifo_frames += resampled;
                }
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
                unsigned rate = core->audioSampleRate(core);
                s_inst_frames[i] = resample_stereo_to_48k(
                    s_raw_buf, read_frames, rate,
                    s_inst_resampled[i], MAX_RESAMPLED_FRAME
                );
                if (s_inst_frames[i] > max_frames) {
                    max_frames = s_inst_frames[i];
                }
            }
        }

        if (max_frames > 0 && (s_fifo_frames + max_frames <= FIFO_CAPACITY_FRAMES)) {
            for (size_t f = 0; f < max_frames; ++f) {
                int32_t mix_l = 0;
                int32_t mix_r = 0;

                for (int i = 0; i < GBA_MAX_INSTANCES; ++i) {
                    if (f < s_inst_frames[i]) {
                        /* 25% gain (attenuation per channel) */
                        mix_l += ((int32_t)s_inst_resampled[i][f * 2 + 0]) >> 2;
                        mix_r += ((int32_t)s_inst_resampled[i][f * 2 + 1]) >> 2;
                    }
                }

                s_fifo[(s_fifo_frames + f) * 2 + 0] = clamp_s16(mix_l);
                s_fifo[(s_fifo_frames + f) * 2 + 1] = clamp_s16(mix_r);
            }
            s_fifo_frames += max_frames;
        }
    }

    while (s_fifo_frames >= PS4_AUDIO_GRANULARITY) {
        int16_t* out_ptr = s_out_buffers[s_out_flip];
        memcpy(out_ptr, s_fifo, PS4_AUDIO_GRANULARITY * 2 * sizeof(int16_t));

        sceAudioOutOutput(s_audio_handle, out_ptr);
        s_out_flip ^= 1;

        s_fifo_frames -= PS4_AUDIO_GRANULARITY;
        if (s_fifo_frames > 0) {
            memmove(s_fifo,
                    &s_fifo[PS4_AUDIO_GRANULARITY * 2],
                    s_fifo_frames * 2 * sizeof(int16_t));
        }
    }
}

void audio_shutdown(void) {
    if (s_audio_handle >= 0) {
        sceAudioOutOutput(s_audio_handle, NULL);
        sceAudioOutClose(s_audio_handle);
        s_audio_handle = -1;
    }
    s_fifo_frames = 0;
}

