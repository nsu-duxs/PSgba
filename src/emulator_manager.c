#include "emulator_manager.h"
#include "link_cable_hub.h"
#include "audio_ps4.h"
#include "save_manager.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <orbis/libkernel.h>
#include <mgba/internal/arm/arm.h>
#include <mgba/internal/gba/gba.h>
#include <mgba/internal/gba/video.h>

PS4GBA_Context g_gba_ctx = {0};

typedef struct SpinBarrier {
    volatile int count;
    volatile int sense;
    int total;
} SpinBarrier;

static OrbisPthread s_worker_threads[GBA_MAX_INSTANCES];
static bool s_workers_initialized = false;
static volatile int s_workers_shutdown = 0;
static volatile uint32_t s_frame_generation = 0;
static volatile int s_parallel_scanline = 1;

static SpinBarrier s_scanline_barrier;
static int s_coord_sense = 0;
static struct GBA* s_frame_gbas[GBA_MAX_INSTANCES];
static uint32_t s_start_frames[GBA_MAX_INSTANCES];
static uint32_t s_start_cycles[GBA_MAX_INSTANCES];

static inline void cpu_relax_pause(void) {
#if defined(__x86_64__) || defined(__i386__)
    __builtin_ia32_pause();
#endif
}

static void spin_barrier_init(SpinBarrier* b, int total_threads) {
    b->count = 0;
    b->sense = 0;
    b->total = total_threads;
    s_coord_sense = 0;
}

static inline void spin_barrier_wait(SpinBarrier* b, int* local_sense) {
    *local_sense = !(*local_sense);
    if (__atomic_add_fetch(&b->count, 1, __ATOMIC_ACQ_REL) == b->total) {
        __atomic_store_n(&b->count, 0, __ATOMIC_RELAXED);
        __atomic_store_n(&b->sense, *local_sense, __ATOMIC_RELEASE);
    } else {
        while (__atomic_load_n(&b->sense, __ATOMIC_ACQUIRE) != *local_sense) {
            cpu_relax_pause();
        }
    }
}

static inline void step_instance_slice(int idx, uint32_t slice_target) {
    struct GBA* gba = s_frame_gbas[idx];
    if (!gba) {
        return;
    }
    uint32_t start_frame = s_start_frames[idx];
    uint32_t start_cycle = s_start_cycles[idx];

    while (gba->video.frameCounter == start_frame &&
           (uint32_t)(mTimingCurrentTime(&gba->timing) - start_cycle) < slice_target) {
        ARMRunLoop(gba->cpu);
    }
}

static inline void finish_instance_frame(int idx) {
    struct GBA* gba = s_frame_gbas[idx];
    if (!gba) {
        return;
    }
    uint32_t start_frame = s_start_frames[idx];
    uint32_t start_cycle = s_start_cycles[idx];

    while (gba->video.frameCounter == start_frame &&
           (uint32_t)(mTimingCurrentTime(&gba->timing) - start_cycle) <
               VIDEO_TOTAL_LENGTH + VIDEO_HORIZONTAL_LENGTH) {
        ARMRunLoop(gba->cpu);
    }
}

static void* gba_worker_entry(void* arg) {
    int worker_id = (int)(intptr_t)arg;
    uint32_t seen_gen = 0;
    int local_sense = 0;

    while (!__atomic_load_n(&s_workers_shutdown, __ATOMIC_ACQUIRE)) {
        uint32_t cur_gen = __atomic_load_n(&s_frame_generation, __ATOMIC_ACQUIRE);
        if (cur_gen == seen_gen) {
            cpu_relax_pause();
            continue;
        }
        seen_gen = cur_gen;

        if (__atomic_load_n(&s_workers_shutdown, __ATOMIC_ACQUIRE)) {
            break;
        }

        for (uint32_t slice_target = VIDEO_HORIZONTAL_LENGTH;
             slice_target <= VIDEO_TOTAL_LENGTH;
             slice_target += VIDEO_HORIZONTAL_LENGTH) {

            if (__atomic_load_n(&s_parallel_scanline, __ATOMIC_ACQUIRE)) {
                step_instance_slice(worker_id, slice_target);
            } else if (worker_id == 0) {
                /* Active Link Cable transfer: step all instances sequentially on worker 0 */
                for (int i = 0; i < GBA_MAX_INSTANCES; ++i) {
                    step_instance_slice(i, slice_target);
                }
            }

            /* Scanline barrier across all 4 workers + main coordinator */
            spin_barrier_wait(&s_scanline_barrier, &local_sense);
        }

        if (__atomic_load_n(&s_parallel_scanline, __ATOMIC_ACQUIRE)) {
            finish_instance_frame(worker_id);
        } else if (worker_id == 0) {
            for (int i = 0; i < GBA_MAX_INSTANCES; ++i) {
                finish_instance_frame(i);
            }
        }

        /* Frame completion barrier */
        spin_barrier_wait(&s_scanline_barrier, &local_sense);
    }

    return NULL;
}

static void workers_init_pool(void) {
    if (s_workers_initialized) {
        return;
    }

    s_workers_shutdown = 0;
    s_frame_generation = 0;
    s_parallel_scanline = 1;
    /* 4 worker threads + 1 coordinator thread = 5 participants at barrier */
    spin_barrier_init(&s_scanline_barrier, GBA_MAX_INSTANCES + 1);

    for (int i = 0; i < GBA_MAX_INSTANCES; ++i) {
        char thread_name[32];
        snprintf(thread_name, sizeof(thread_name), "GBA_Worker_P%d", i + 1);
        if (scePthreadCreate(&s_worker_threads[i], NULL,
                             gba_worker_entry, (void*)(intptr_t)i,
                             thread_name) == 0) {
            /* Distribute workers across PS4 AMD Jaguar cores 2..5 */
            uint64_t affinity_mask = (1ULL << (2 + i));
            scePthreadSetaffinity(s_worker_threads[i], affinity_mask);
        }
    }

    s_workers_initialized = true;
}

static void workers_shutdown_pool(void) {
    if (!s_workers_initialized) {
        return;
    }

    __atomic_store_n(&s_workers_shutdown, 1, __ATOMIC_RELEASE);
    __atomic_add_fetch(&s_frame_generation, 1, __ATOMIC_ACQ_REL);

    for (int i = 0; i < GBA_MAX_INSTANCES; ++i) {
        scePthreadJoin(s_worker_threads[i], NULL);
    }

    s_workers_initialized = false;
}

bool emulator_init_session(const char* const rom_paths[GBA_MAX_INSTANCES], int num_players) {
    if (!rom_paths || num_players < 1 || num_players > GBA_MAX_INSTANCES) {
        return false;
    }

    emulator_destroy_all();
    memset(&g_gba_ctx, 0, sizeof(g_gba_ctx));
    save_manager_init();
    save_manager_extract_rom_name(rom_paths[0], g_gba_ctx.rom_name, sizeof(g_gba_ctx.rom_name));
    link_hub_init();

    for (int i = 0; i < num_players; ++i) {
        const char* path = rom_paths[i] ? rom_paths[i] : rom_paths[0];
        if (!path || !*path) {
            emulator_destroy_all();
            return false;
        }

        struct mCore* core = GBACoreCreate();
        if (!core) {
            emulator_destroy_all();
            return false;
        }

        if (!core->init(core)) {
            core->deinit(core);
            emulator_destroy_all();
            return false;
        }

        uint32_t* vbuf = (uint32_t*)malloc(GBA_SCREEN_WIDTH * GBA_SCREEN_HEIGHT * sizeof(uint32_t));
        if (!vbuf) {
            core->deinit(core);
            emulator_destroy_all();
            return false;
        }
        memset(vbuf, 0, GBA_SCREEN_WIDTH * GBA_SCREEN_HEIGHT * sizeof(uint32_t));

        core->setVideoBuffer(core, (mColor*)vbuf, GBA_SCREEN_WIDTH);
        core->setAudioBufferSize(core, GBA_AUDIO_BUFFER_SIZE);

        if (!mCoreLoadFile(core, path)) {
            free(vbuf);
            core->deinit(core);
            emulator_destroy_all();
            return false;
        }

        save_manager_extract_rom_name(path, g_gba_ctx.instances[i].rom_name,
                                      sizeof(g_gba_ctx.instances[i].rom_name));
        save_manager_attach_battery_save(core, g_gba_ctx.instances[i].rom_name, i);
        link_hub_attach_instance(i, core);
        core->reset(core);

        g_gba_ctx.instances[i].core = core;
        g_gba_ctx.instances[i].video_buffer = vbuf;
        g_gba_ctx.instances[i].active = true;
        g_gba_ctx.active_count++;
    }

    link_hub_reset();
    workers_init_pool();
    return (g_gba_ctx.active_count > 0);
}

bool emulator_init_all(const char* rom_path) {
    const char* paths[GBA_MAX_INSTANCES] = {
        rom_path, rom_path, rom_path, rom_path
    };
    return emulator_init_session(paths, GBA_MAX_INSTANCES);
}

void emulator_step_lockstep_frame(void) {
    for (int i = 0; i < GBA_MAX_INSTANCES; ++i) {
        if (g_gba_ctx.instances[i].active && g_gba_ctx.instances[i].core) {
            struct GBA* gba = (struct GBA*)g_gba_ctx.instances[i].core->board;
            s_frame_gbas[i] = gba;
            s_start_frames[i] = gba->video.frameCounter;
            s_start_cycles[i] = mTimingCurrentTime(&gba->timing);
        } else {
            s_frame_gbas[i] = NULL;
            s_start_frames[i] = 0;
            s_start_cycles[i] = 0;
        }
    }

    if (!s_workers_initialized) {
        workers_init_pool();
    }

    __atomic_store_n(&s_parallel_scanline, !link_hub_is_transfer_active(), __ATOMIC_RELEASE);

    /* Wake all 4 persistent worker threads to begin the frame */
    __atomic_add_fetch(&s_frame_generation, 1, __ATOMIC_ACQ_REL);

    for (uint32_t slice_target = VIDEO_HORIZONTAL_LENGTH;
         slice_target <= VIDEO_TOTAL_LENGTH;
         slice_target += VIDEO_HORIZONTAL_LENGTH) {
        spin_barrier_wait(&s_scanline_barrier, &s_coord_sense);
        link_hub_notify_scanline();
        __atomic_store_n(&s_parallel_scanline, !link_hub_is_transfer_active(), __ATOMIC_RELEASE);
    }

    /* Wait for all workers to complete the end-of-frame boundary */
    spin_barrier_wait(&s_scanline_barrier, &s_coord_sense);
}

void emulator_destroy_all(void) {
    workers_shutdown_pool();
    link_hub_destroy();

    for (int i = 0; i < GBA_MAX_INSTANCES; ++i) {
        if (g_gba_ctx.instances[i].core) {
            g_gba_ctx.instances[i].core->deinit(g_gba_ctx.instances[i].core);
            g_gba_ctx.instances[i].core = NULL;
        }
        if (g_gba_ctx.instances[i].video_buffer) {
            free(g_gba_ctx.instances[i].video_buffer);
            g_gba_ctx.instances[i].video_buffer = NULL;
        }
        g_gba_ctx.instances[i].active = false;
    }
    g_gba_ctx.active_count = 0;
}
