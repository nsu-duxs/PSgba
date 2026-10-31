#include "emulator_manager.h"
#include "link_cable_hub.h"
#include "video_split.h"
#include "audio_ps4.h"
#include "save_manager.h"
#include "input_ds4.h"
#include "ps4_jailbreak.h"
#include <stdio.h>
#include <strings.h>
#include <stdlib.h>
#include <string.h>
#include <orbis/libkernel.h>
#include <mgba/core/log.h>
#include <mgba-util/vfs.h>
#include <mgba/internal/arm/arm.h>
#include <mgba/internal/gba/gba.h>
#include <mgba/internal/gba/video.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <dirent.h>
#include <ctype.h>
#include <mgba/core/cheats.h>
#include <mgba/internal/gba/cheats.h>

PS4GBA_Context g_gba_ctx = {0};

static struct GBA* s_frame_gbas[GBA_MAX_INSTANCES] __attribute__((aligned(64)));
static uint32_t s_start_frames[GBA_MAX_INSTANCES] __attribute__((aligned(64)));
static uint32_t s_start_cycles[GBA_MAX_INSTANCES] __attribute__((aligned(64)));

static uint32_t* s_video_back_buffer = NULL;
static struct PS4GBA_Context* s_video_ctx = NULL;
static int s_video_band_slot = 0;

static char s_cheat_file_paths[GBA_MAX_INSTANCES][256] = {{0}};
static bool s_cheat_file_found[GBA_MAX_INSTANCES] = {0};
static char s_cheat_status[GBA_MAX_INSTANCES][256] = {{0}};
static int s_last_usb_ports_mounted = 0;
static int s_last_usb_cht_count = 0;

typedef struct __attribute__((aligned(64))) GBAWorker {
    OrbisPthread thread;
    int player_id;
    volatile int command; /* 0 = idle, 1 = step_slice, 2 = finish_frame, 3 = terminate, 4 = render_video, 5 = render_band (1P) */
    volatile uint32_t target_slice;
    volatile bool running;
    bool thread_created;
} GBAWorker;

static GBAWorker s_workers[GBA_MAX_INSTANCES] __attribute__((aligned(64))) = {0};

void emulator_apply_cheats_frame(int slot_index);

static bool s_main_affinity_set = false;

static void silent_mgba_log(struct mLogger* logger, int category, enum mLogLevel level,
                            const char* format, va_list args) {
    (void)logger;
    (void)category;
    (void)level;
    (void)format;
    (void)args;
}

static struct mLogger s_silent_logger = {
    .log = silent_mgba_log,
    .filter = NULL
};

static bool load_builtin_demo_rom(struct mCore* core) {
    static const uint32_t k_demo_arm_code[] = {
        0xE3A04301U, 0xE3A05306U, 0xE3A00F01U, 0xE3800003U,
        0xE1C400B0U, 0xE3A00000U, 0xE2846F48U, 0xE1C601B4U,
        0xE3A00908U, 0xE3800003U, 0xE1C600B8U, 0xE3A07070U,
        0xE3A08048U, 0xE1D400B6U, 0xE35000A0U, 0x1AFFFFFCU,
        0xE1D601B0U, 0xE3100010U, 0x02877002U, 0xE3100020U,
        0x02477002U, 0xE3100040U, 0x02488002U, 0xE3100080U,
        0x02888002U, 0xE3570008U, 0xB3A07008U, 0xE35700DCU,
        0xC3A070DCU, 0xE3580008U, 0xB3A08008U, 0xE358008CU,
        0xC3A0808CU, 0xE1870408U, 0xE1C600BAU, 0xE3A00908U,
        0xE3800083U, 0xE1C600B8U, 0xE3A01B12U, 0xE2811C03U,
        0xE3A02C21U, 0xE3822004U, 0xE1822802U, 0xE1A03005U,
        0xE4832004U, 0xE2511001U, 0x1AFFFFFCU, 0xE3A09000U,
        0xE1A00089U, 0xE19610B0U, 0xE3710001U, 0x01871408U,
        0xE20120FFU, 0xE1A03421U, 0xE20330FFU, 0xE35200E0U,
        0xC3A02070U, 0xE3530090U, 0xC3A03048U, 0xE3590000U,
        0x03A0A01FU, 0xE3590001U, 0x03A0AC03U, 0x038AA0E0U,
        0xE3590002U, 0x03A0AC03U, 0x038AA0FFU, 0xE3590003U,
        0x03A0AA07U, 0x038AAFE0U, 0xE3A0B00CU, 0xE3A000F0U,
        0xE0010390U, 0xE0811002U, 0xE0851081U, 0xE3A0000CU,
        0xE0C1A0B2U, 0xE2500001U, 0x1AFFFFFCU, 0xE2833001U,
        0xE25BB001U, 0x1AFFFFF5U, 0xE2899001U, 0xE3590004U,
        0xBAFFFFDAU, 0xE1D400B6U, 0xE35000A0U, 0x0AFFFFFCU,
        0xEAFFFFB3U
    };

    const size_t rom_size = 0x80000U; /* 512 KB (> EWRAM so GBAIsMB == false) */
    struct VFile* vf = VFileMemChunk(NULL, rom_size);
    if (!vf) {
        return false;
    }

    uint8_t header[0xC0 + sizeof(k_demo_arm_code)];
    memset(header, 0, sizeof(header));
    header[0] = 0x2EU;
    header[1] = 0x00U;
    header[2] = 0x00U;
    header[3] = 0xEAU; /* b 0xC0 */
    memcpy(&header[0xA0], "PS4GBA4PDEMO", 12);
    memcpy(&header[0xAC], "G4PD01", 6);
    header[0xB2] = 0x96U;
    memcpy(&header[0xC0], k_demo_arm_code, sizeof(k_demo_arm_code));

    vf->seek(vf, 0, SEEK_SET);
    vf->write(vf, header, sizeof(header));
    vf->seek(vf, 0, SEEK_SET);

    if (!core->loadROM(core, vf)) {
        vf->close(vf);
        return false;
    }
    return true;
}

static bool instantiate_player_slot(int slot_index, const char* path, OrbisUserServiceUserId user_id) {
    if (slot_index < 0 || slot_index >= GBA_MAX_INSTANCES || !path || !*path) {
        return false;
    }

    if (g_gba_ctx.instances[slot_index].active && g_gba_ctx.instances[slot_index].core) {
        return true;
    }

    struct mCore* core = GBACoreCreate();
    if (!core) {
        return false;
    }

    if (!core->init(core)) {
        core->deinit(core);
        return false;
    }

    mCoreInitConfig(core, NULL);
    core->opts.skipBios = true;
    core->opts.useBios = false;
    core->opts.volume = 0x100;
    core->opts.mute = false;

    uint32_t* vbuf = (uint32_t*)calloc(GBA_SCREEN_WIDTH * GBA_SCREEN_HEIGHT, sizeof(uint32_t));
    if (!vbuf) {
        mCoreConfigDeinit(&core->config);
        core->deinit(core);
        return false;
    }

    core->setVideoBuffer(core, (mColor*)vbuf, GBA_SCREEN_WIDTH);
    core->setAudioBufferSize(core, GBA_AUDIO_BUFFER_SIZE);

    bool rom_loaded = mCoreLoadFile(core, path);
    if (!rom_loaded && strstr(path, "test.gba") != NULL) {
        rom_loaded = load_builtin_demo_rom(core);
    }

    if (!rom_loaded) {
        free(vbuf);
        mCoreConfigDeinit(&core->config);
        core->deinit(core);
        return false;
    }

    save_manager_extract_rom_name(path, g_gba_ctx.instances[slot_index].rom_name,
                                  sizeof(g_gba_ctx.instances[slot_index].rom_name));
    strncpy(g_gba_ctx.instances[slot_index].rom_path, path,
            sizeof(g_gba_ctx.instances[slot_index].rom_path) - 1);
    g_gba_ctx.instances[slot_index].rom_path[sizeof(g_gba_ctx.instances[slot_index].rom_path) - 1] = '\0';
    save_manager_attach_battery_save(core, g_gba_ctx.instances[slot_index].rom_name,
                                     slot_index, user_id);

    /* Reset core ANTES de anexar o driver de SIO do Link Cable para que GBASIOReset não o sobrescreva */
    core->reset(core);

    g_gba_ctx.instances[slot_index].core = core;
    g_gba_ctx.instances[slot_index].video_buffer = vbuf;
    g_gba_ctx.instances[slot_index].user_id = user_id;
    g_gba_ctx.instances[slot_index].active = true;
    g_gba_ctx.active_count++;

    emulator_load_cheats(slot_index, path);
    return true;
}

/*
 * Avança uma instância do GBA exatamente até `start_cycle + slice_target` usando
 * um evento `mTimingEvent` (`lockstep_event`) que aciona `GBAInterrupt(gba)`
 * (`gba->earlyExit = true`).
 * Isso impede que `GBAProcessEvents` avance CPUs em `Halt` / `IntrWait` / `VBlankIntrWait`
 * direto até o VBlank (fim do quadro), mantendo todas as 4 instâncias sincronizadas
 * ciclo a ciclo para que as ISRs de Serial (`GBA_IRQ_SIO`) dos Escravos respondam
 * imediatamente dentro da mesma fatia de tempo.
 */
static inline bool step_instance_slice(int idx, uint32_t slice_target) {
    struct GBA* gba = s_frame_gbas[idx];
    if (!gba || !gba->cpu) {
        return false;
    }
    uint32_t start_frame = s_start_frames[idx];
    uint32_t start_cycle = s_start_cycles[idx];
    uint32_t target_time = start_cycle + slice_target;
    struct mTimingEvent* lockstep_ev = &g_link_hub.drivers[idx].lockstep_event;
    bool master_started = false;

    while (gba->video.frameCounter == start_frame) {
        if (gba->cpu->halted) {
            gba->cpu->cycles = 0;
        }
        int32_t remaining = (int32_t)(target_time - (uint32_t)mTimingCurrentTime(&gba->timing));
        if (remaining <= 0) {
            break;
        }

        mTimingDeschedule(&gba->timing, lockstep_ev);
        mTimingSchedule(&gba->timing, lockstep_ev, remaining);

        if (gba->cpu->halted) {
            gba->cpu->nextEvent = mTimingNextEvent(&gba->timing);
            gba->cpu->cycles = gba->cpu->nextEvent;
        }

        ARMRunLoop(gba->cpu);

        if (gba->cpu->halted) {
            gba->cpu->cycles = 0;
            gba->cpu->nextEvent = mTimingNextEvent(&gba->timing);
        }

        if (idx == 0 && link_hub_consume_just_started()) {
            master_started = true;
            break;
        }
        if (__atomic_load_n(&g_link_hub.slice_interrupt, __ATOMIC_ACQUIRE)) {
            break;
        }
    }

    mTimingDeschedule(&gba->timing, lockstep_ev);
    gba->cpu->nextEvent = mTimingNextEvent(&gba->timing);
    return master_started;
}

static inline void finish_instance_frame(int idx) {
    struct GBA* gba = s_frame_gbas[idx];
    if (!gba || !gba->cpu) {
        return;
    }
    uint32_t start_frame = s_start_frames[idx];
    uint32_t start_cycle = s_start_cycles[idx];
    struct mTimingEvent* lockstep_ev = &g_link_hub.drivers[idx].lockstep_event;

    mTimingDeschedule(&gba->timing, lockstep_ev);
    gba->cpu->nextEvent = mTimingNextEvent(&gba->timing);

    while (gba->video.frameCounter == start_frame &&
           (uint32_t)(mTimingCurrentTime(&gba->timing) - start_cycle) <
               VIDEO_TOTAL_LENGTH + VIDEO_HORIZONTAL_LENGTH) {
        if (gba->cpu->halted) {
            gba->cpu->nextEvent = mTimingNextEvent(&gba->timing);
            gba->cpu->cycles = gba->cpu->nextEvent;
        }
        ARMRunLoop(gba->cpu);
        if (gba->cpu->halted) {
            gba->cpu->cycles = 0;
            gba->cpu->nextEvent = mTimingNextEvent(&gba->timing);
        }
    }
    emulator_apply_cheats_frame(idx);
}

static void* worker_thread_main(void* arg) {
    GBAWorker* w = (GBAWorker*)arg;
    int idx = w->player_id;

    /* Associa a thread de trabalho ao seu respectivo nucleo da CPU (P1->Core 1, P2->Core 2, P3->Core 3) */
    scePthreadSetaffinity(w->thread, 1ULL << idx);

    uint32_t idle_spins = 0;
    while (__atomic_load_n(&w->running, __ATOMIC_ACQUIRE)) {
        int cmd = __atomic_load_n(&w->command, __ATOMIC_ACQUIRE);
        if (cmd == 0) {
            __builtin_ia32_pause();
            if (++idle_spins > 2000000U) {
                sceKernelUsleep(200);
                idle_spins = 2000000U;
            }
            continue;
        }

        idle_spins = 0;

        if (cmd == 1) {
            uint32_t target = w->target_slice;
            step_instance_slice(idx, target);
            __atomic_store_n(&w->command, 0, __ATOMIC_RELEASE);
        } else if (cmd == 2) {
            finish_instance_frame(idx);
            __atomic_store_n(&w->command, 0, __ATOMIC_RELEASE);
        } else if (cmd == 3) {
            break;
        } else if (cmd == 4) {
            video_render_instance_slot(idx, s_video_back_buffer, s_video_ctx);
            __atomic_store_n(&w->command, 0, __ATOMIC_RELEASE);
        } else if (cmd == 5) {
            video_render_instance_band(s_video_band_slot, idx, GBA_MAX_INSTANCES,
                                       s_video_back_buffer, s_video_ctx);
            __atomic_store_n(&w->command, 0, __ATOMIC_RELEASE);
        }
    }
    return NULL;
}

static void init_workers(void) {
    if (!s_main_affinity_set) {
        typedef OrbisPthread (*ScePthreadSelfFn)(void);
        OrbisPthread self_thr = ((ScePthreadSelfFn)(uintptr_t)scePthreadSelf)();
        scePthreadSetaffinity(self_thr, 1ULL << 0);
        s_main_affinity_set = true;
    }
    for (int j = 1; j < GBA_MAX_INSTANCES; ++j) {
        if (!s_workers[j].thread_created) {
            memset(&s_workers[j], 0, sizeof(GBAWorker));
            s_workers[j].player_id = j;
            s_workers[j].running = true;
            s_workers[j].command = 0;
            s_workers[j].target_slice = 0;
            char name[32];
            snprintf(name, sizeof(name), "gba_worker_%d", j);
            int ret = scePthreadCreate(&s_workers[j].thread, NULL, worker_thread_main, &s_workers[j], name);
            if (ret == 0) {
                s_workers[j].thread_created = true;
                scePthreadSetaffinity(s_workers[j].thread, 1ULL << j);
            }
        }
    }
}

static void stop_workers(void) {
    for (int j = 1; j < GBA_MAX_INSTANCES; ++j) {
        if (s_workers[j].thread_created) {
            __atomic_store_n(&s_workers[j].running, false, __ATOMIC_RELEASE);
            __atomic_store_n(&s_workers[j].command, 3, __ATOMIC_RELEASE);
            scePthreadJoin(s_workers[j].thread, NULL);
            s_workers[j].thread_created = false;
        }
    }
}

static inline void dispatch_slaves_slice(uint32_t target) {
    for (int j = 1; j < GBA_MAX_INSTANCES; ++j) {
        if (s_frame_gbas[j]) {
            s_workers[j].target_slice = target;
            __atomic_store_n(&s_workers[j].command, 1, __ATOMIC_RELEASE);
        }
    }
}

static inline void dispatch_slaves_finish(void) {
    for (int j = 1; j < GBA_MAX_INSTANCES; ++j) {
        if (s_frame_gbas[j]) {
            __atomic_store_n(&s_workers[j].command, 2, __ATOMIC_RELEASE);
        }
    }
}

static inline void wait_slaves_done(void) {
    for (int j = 1; j < GBA_MAX_INSTANCES; ++j) {
        if (s_frame_gbas[j]) {
            while (__atomic_load_n(&s_workers[j].command, __ATOMIC_ACQUIRE) != 0) {
                __builtin_ia32_pause();
            }
        }
    }
}

void emulator_parallel_render_video(uint32_t* back_buffer, struct PS4GBA_Context* ctx) {
    if (!back_buffer || !ctx) {
        return;
    }
    if (ctx->active_count <= 1) {
        int slot = -1;
        for (int i = 0; i < GBA_MAX_INSTANCES; ++i) {
            if (ctx->instances[i].active && ctx->instances[i].video_buffer) {
                slot = i;
                break;
            }
        }
        if (slot < 0) {
            return;
        }

        /* 1P: upscale 1x na thread principal; ajuste ao viewport dividido em bandas de linhas
         * entre os 4 nucleos (banda j no worker j, banda 0 aqui) */
        video_prepare_instance(slot, ctx);

        init_workers();
        s_video_back_buffer = back_buffer;
        s_video_ctx = ctx;
        s_video_band_slot = slot;
        __atomic_thread_fence(__ATOMIC_RELEASE);

        for (int j = 1; j < GBA_MAX_INSTANCES; ++j) {
            if (s_workers[j].thread_created) {
                __atomic_store_n(&s_workers[j].command, 5, __ATOMIC_RELEASE);
            }
        }
        video_render_instance_band(slot, 0, GBA_MAX_INSTANCES, back_buffer, ctx);
        for (int j = 1; j < GBA_MAX_INSTANCES; ++j) {
            if (s_workers[j].thread_created) {
                while (__atomic_load_n(&s_workers[j].command, __ATOMIC_ACQUIRE) != 0) {
                    __builtin_ia32_pause();
                }
            } else {
                video_render_instance_band(slot, j, GBA_MAX_INSTANCES, back_buffer, ctx);
            }
        }
        video_draw_instance_border(slot, back_buffer, ctx);
        return;
    }

    init_workers();
    s_video_back_buffer = back_buffer;
    s_video_ctx = ctx;
    __atomic_thread_fence(__ATOMIC_RELEASE);

    for (int j = 1; j < GBA_MAX_INSTANCES; ++j) {
        if (ctx->instances[j].active && ctx->instances[j].video_buffer) {
            __atomic_store_n(&s_workers[j].command, 4, __ATOMIC_RELEASE);
        }
    }

    if (ctx->instances[0].active && ctx->instances[0].video_buffer) {
        video_render_instance_slot(0, back_buffer, ctx);
    }

    for (int j = 1; j < GBA_MAX_INSTANCES; ++j) {
        if (ctx->instances[j].active && ctx->instances[j].video_buffer) {
            while (__atomic_load_n(&s_workers[j].command, __ATOMIC_ACQUIRE) != 0) {
                __builtin_ia32_pause();
            }
        }
    }
}

bool emulator_init_session(const char* const rom_paths[GBA_MAX_INSTANCES], int num_players) {
    if (!rom_paths || !rom_paths[0] || !*rom_paths[0] ||
        num_players < 1 || num_players > GBA_MAX_INSTANCES) {
        return false;
    }

    mLogSetDefaultLogger(&s_silent_logger);
    emulator_destroy_all();
    memset(&g_gba_ctx, 0, sizeof(g_gba_ctx));
    for (int i = 0; i < GBA_MAX_INSTANCES; ++i) {
        g_gba_ctx.instances[i].user_id = -1;
    }

    save_manager_init();
    strncpy(g_gba_ctx.rom_path, rom_paths[0], sizeof(g_gba_ctx.rom_path) - 1);
    g_gba_ctx.rom_path[sizeof(g_gba_ctx.rom_path) - 1] = '\0';
    save_manager_extract_rom_name(rom_paths[0], g_gba_ctx.rom_name, sizeof(g_gba_ctx.rom_name));
    link_hub_init();

    for (int i = 0; i < num_players; ++i) {
        const char* path = rom_paths[i] ? rom_paths[i] : rom_paths[0];
        OrbisUserServiceUserId uid = input_get_user_id(i);
        if (!instantiate_player_slot(i, path, uid)) {
            emulator_destroy_all();
            return false;
        }
    }

    struct mCore* active_cores[GBA_MAX_INSTANCES];
    for (int i = 0; i < num_players; ++i) {
        active_cores[i] = g_gba_ctx.instances[i].core;
    }
    link_hub_attach(active_cores, num_players);

    return (g_gba_ctx.active_count > 0);
}

bool emulator_init_all(const char* rom_path) {
    const char* paths[GBA_MAX_INSTANCES] = {
        rom_path, rom_path, rom_path, rom_path
    };
    return emulator_init_session(paths, 1);
}

bool emulator_join_player(int slot_index, OrbisUserServiceUserId user_id) {
    if (slot_index < 0 || slot_index >= GBA_MAX_INSTANCES) {
        return false;
    }
    if (g_gba_ctx.instances[slot_index].active) {
        return true;
    }
    if (g_gba_ctx.rom_path[0] == '\0') {
        return false;
    }

    if (!instantiate_player_slot(slot_index, g_gba_ctx.rom_path, user_id)) {
        return false;
    }

    link_hub_attach_instance(slot_index, g_gba_ctx.instances[slot_index].core);
    return true;
}

void emulator_step_lockstep_frame(void) {
    bool skip_render = video_begin_frame_should_skip_render();
    int active_instances = 0;
    int first_active = -1;
    for (int i = 0; i < GBA_MAX_INSTANCES; ++i) {
        if (g_gba_ctx.instances[i].active && g_gba_ctx.instances[i].core &&
            g_gba_ctx.instances[i].core->board) {
            struct GBA* gba = (struct GBA*)g_gba_ctx.instances[i].core->board;
            gba->video.frameskip = 0;
            gba->video.frameskipCounter = skip_render ? 1 : 0;
            if (gba->cpu && gba->cpu->halted) {
                gba->cpu->cycles = 0;
                gba->cpu->nextEvent = mTimingNextEvent(&gba->timing);
            }
            s_frame_gbas[i] = gba;
            s_start_frames[i] = gba->video.frameCounter;
            s_start_cycles[i] = (uint32_t)mTimingCurrentTime(&gba->timing);
            if (first_active < 0) {
                first_active = i;
            }
            active_instances++;
        } else {
            s_frame_gbas[i] = NULL;
            s_start_frames[i] = 0;
            s_start_cycles[i] = 0;
        }
    }

    if (active_instances == 0) {
        return;
    }

    if (active_instances == 1) {
        finish_instance_frame(first_active);
        return;
    }

    init_workers();

    link_hub_begin_frame();

    uint32_t slice_target = 0;
    while (slice_target < VIDEO_TOTAL_LENGTH) {
        bool xfer_active = link_hub_is_transfer_active();
        uint32_t prev_target = slice_target;
        uint32_t step = xfer_active ? 64U : 256U;
        slice_target += step;
        if (g_link_hub.transfer_in_progress &&
            g_link_hub.transfer_finish_elapsed > prev_target &&
            g_link_hub.transfer_finish_elapsed < slice_target) {
            slice_target = g_link_hub.transfer_finish_elapsed;
        }
        if (slice_target > VIDEO_TOTAL_LENGTH) {
            slice_target = VIDEO_TOTAL_LENGTH;
        }

        __atomic_store_n(&g_link_hub.slice_interrupt, false, __ATOMIC_RELEASE);

        if (xfer_active) {
            /* Durante transferência ativa do Link Cable, avança o Mestre (Player 0) primeiro na
             * microfatia de 64 ciclos para que os Escravos nunca ultrapassem `master_elapsed`. */
            bool master_started = false;
            if (s_frame_gbas[0]) {
                master_started = step_instance_slice(0, slice_target);
            }
            if (master_started && s_frame_gbas[0]) {
                uint32_t master_elapsed =
                    (uint32_t)(mTimingCurrentTime(&s_frame_gbas[0]->timing) - s_start_cycles[0]);
                __atomic_store_n(&g_link_hub.slice_interrupt, false, __ATOMIC_RELEASE);
                dispatch_slaves_slice(master_elapsed);
                wait_slaves_done();

                link_hub_begin_transfer_on_slaves(master_elapsed, s_start_cycles);

                if (g_link_hub.transfer_finish_elapsed > master_elapsed &&
                    g_link_hub.transfer_finish_elapsed < slice_target) {
                    slice_target = g_link_hub.transfer_finish_elapsed;
                }

                __atomic_store_n(&g_link_hub.slice_interrupt, false, __ATOMIC_RELEASE);
                dispatch_slaves_slice(slice_target);
                step_instance_slice(0, slice_target);
                wait_slaves_done();
            } else {
                __atomic_store_n(&g_link_hub.slice_interrupt, false, __ATOMIC_RELEASE);
                dispatch_slaves_slice(slice_target);
                wait_slaves_done();
            }
        } else {
            /* Fora de transferência ativa, dispara todos os 4 núcleos Jaguar simultaneamente em paralelo! */
            dispatch_slaves_slice(slice_target);
            bool master_started = false;
            if (s_frame_gbas[0]) {
                master_started = step_instance_slice(0, slice_target);
            }
            wait_slaves_done();

            if (master_started && s_frame_gbas[0]) {
                uint32_t master_elapsed =
                    (uint32_t)(mTimingCurrentTime(&s_frame_gbas[0]->timing) - s_start_cycles[0]);
                __atomic_store_n(&g_link_hub.slice_interrupt, false, __ATOMIC_RELEASE);
                dispatch_slaves_slice(master_elapsed);
                wait_slaves_done();

                link_hub_begin_transfer_on_slaves(master_elapsed, s_start_cycles);

                if (g_link_hub.transfer_finish_elapsed > master_elapsed &&
                    g_link_hub.transfer_finish_elapsed < slice_target) {
                    slice_target = g_link_hub.transfer_finish_elapsed;
                }

                __atomic_store_n(&g_link_hub.slice_interrupt, false, __ATOMIC_RELEASE);
                dispatch_slaves_slice(slice_target);
                step_instance_slice(0, slice_target);
                wait_slaves_done();
            } else if (__atomic_load_n(&g_link_hub.slice_interrupt, __ATOMIC_ACQUIRE)) {
                link_hub_sync_pins();
                __atomic_store_n(&g_link_hub.slice_interrupt, false, __ATOMIC_RELEASE);
                dispatch_slaves_slice(slice_target);
                if (s_frame_gbas[0]) {
                    step_instance_slice(0, slice_target);
                }
                wait_slaves_done();
            }
        }

        if (g_link_hub.transfer_in_progress &&
            (g_link_hub.transfer_finished_mask == g_link_hub.transfer_scheduled_mask) &&
            g_link_hub.transfer_scheduled_mask != 0) {
            link_hub_latch_transfer_data();
        }

        link_hub_sync_pins();

        if ((slice_target % VIDEO_HORIZONTAL_LENGTH) < (slice_target - prev_target)) {
            link_hub_notify_scanline();
        }
    }

    /* Finaliza o frame em paralelo em todos os núcleos */
    dispatch_slaves_finish();
    if (s_frame_gbas[0]) {
        finish_instance_frame(0);
    }
    wait_slaves_done();

    for (int i = 0; i < GBA_MAX_INSTANCES; ++i) {
        if (g_gba_ctx.instances[i].active) {
            emulator_apply_cheats_frame(i);
        }
    }
}

void emulator_destroy_all(void) {
    stop_workers();
    memset(s_cheat_file_paths, 0, sizeof(s_cheat_file_paths));
    memset(s_cheat_file_found, 0, sizeof(s_cheat_file_found));
    link_hub_destroy();

    for (int i = 0; i < GBA_MAX_INSTANCES; ++i) {
        if (g_gba_ctx.instances[i].core) {
            struct mCore* core = g_gba_ctx.instances[i].core;
            save_manager_flush_battery_save(core);
            g_gba_ctx.instances[i].core = NULL;
            mCoreConfigDeinit(&core->config);
            core->deinit(core);
        }
        if (g_gba_ctx.instances[i].video_buffer) {
            free(g_gba_ctx.instances[i].video_buffer);
            g_gba_ctx.instances[i].video_buffer = NULL;
        }
        g_gba_ctx.instances[i].active = false;
        g_gba_ctx.instances[i].user_id = -1;
    }
    g_gba_ctx.active_count = 0;
}
/* ==============================================================================
 * SUBSISTEMA DE TRAPACAS / CHEATS (.cht) - INTEGRACAO LIBMGBA & ARMAZENAMENTO PS4
 * ============================================================================== */

static const char* safe_strcasestr(const char* haystack, const char* needle) {
    if (!haystack || !needle) return NULL;
    if (!*needle) return haystack;
    size_t nlen = strlen(needle);
    for (; *haystack; ++haystack) {
        if (strncasecmp(haystack, needle, nlen) == 0) {
            return haystack;
        }
    }
    return NULL;
}

static bool has_cht_extension(const char* name) {
    if (!name || !*name) {
        return false;
    }
    const char* dot = strrchr(name, '.');
    if (!dot) {
        return false;
    }
    if (strcasecmp(dot, ".cht") == 0 ||
        strcasecmp(dot, ".cheats") == 0 ||
        strcasecmp(dot, ".clt") == 0 ||
        strcasecmp(dot, ".cheat") == 0) {
        return true;
    }
    /* Suporte a arquivos .txt e extensoes compostas (ex: jogo.cht.txt, jogo.cheats.txt, cheats.txt) */
    if (strcasecmp(dot, ".txt") == 0) {
        size_t len = strlen(name);
        if (len >= 8 && strcasecmp(name + len - 8, ".cht.txt") == 0) return true;
        if (len >= 11 && strcasecmp(name + len - 11, ".cheats.txt") == 0) return true;
        if (safe_strcasestr(name, "cheat") != NULL) return true;
    }
    return false;
}

static void join_clean_path(char* dst, size_t max_len, const char* dir, const char* file) {
    if (!dst || max_len == 0) return;
    dst[0] = '\0';
    if (!dir || !*dir) {
        if (file) {
            strncpy(dst, file, max_len - 1);
            dst[max_len - 1] = '\0';
        }
        return;
    }
    size_t dlen = strlen(dir);
    bool has_slash = (dlen > 0 && (dir[dlen - 1] == '/' || dir[dlen - 1] == '\\'));
    if (has_slash) {
        snprintf(dst, max_len, "%s%s", dir, file ? file : "");
    } else {
        snprintf(dst, max_len, "%s/%s", dir, file ? file : "");
    }
}

static void extract_clean_rom_base(const char* input, char* clean_base, size_t max_len) {
    if (!clean_base || max_len == 0) {
        return;
    }
    clean_base[0] = '\0';
    if (!input || !*input) {
        return;
    }

    const char* slash = strrchr(input, '/');
    const char* bslash = strrchr(input, '\\');
    const char* base = input;
    if (slash && bslash) {
        base = (slash > bslash) ? (slash + 1) : (bslash + 1);
    } else if (slash) {
        base = slash + 1;
    } else if (bslash) {
        base = bslash + 1;
    }

    strncpy(clean_base, base, max_len - 1);
    clean_base[max_len - 1] = '\0';

    /* Remove multiplas extensoes encadeadas (ex: jogo.gba.cht.txt -> jogo) */
    for (int pass = 0; pass < 4; ++pass) {
        char* dot = strrchr(clean_base, '.');
        if (!dot) {
            break;
        }
        if (strcasecmp(dot, ".gba") == 0 ||
            strcasecmp(dot, ".agb") == 0 ||
            strcasecmp(dot, ".bin") == 0 ||
            strcasecmp(dot, ".zip") == 0 ||
            strcasecmp(dot, ".7z") == 0 ||
            strcasecmp(dot, ".cht") == 0 ||
            strcasecmp(dot, ".cheats") == 0 ||
            strcasecmp(dot, ".cheat") == 0 ||
            strcasecmp(dot, ".clt") == 0 ||
            strcasecmp(dot, ".txt") == 0) {
            *dot = '\0';
        } else {
            break;
        }
    }
}

/* Le Game Title (offset 0xA0) e Game Code (offset 0xAC) diretamente do cabecalho GBA */
static void extract_rom_header_info(const char* rom_path, char* out_game_code, size_t code_max, char* out_cart_title, size_t title_max) {
    if (out_game_code && code_max > 0) out_game_code[0] = '\0';
    if (out_cart_title && title_max > 0) out_cart_title[0] = '\0';
    if (!rom_path || !*rom_path) return;

    FILE* f = fopen(rom_path, "rb");
    if (!f) return;

    if (fseek(f, 0xA0, SEEK_SET) == 0) {
        char buf[16];
        /* Title: 12 bytes at 0xA0 */
        if (fread(buf, 1, 12, f) == 12 && out_cart_title && title_max > 0) {
            size_t w = 0;
            for (int i = 0; i < 12 && w + 1 < title_max; ++i) {
                unsigned char c = (unsigned char)buf[i];
                if (c >= 32 && c <= 126) {
                    out_cart_title[w++] = (char)c;
                } else if (c == 0) {
                    break;
                }
            }
            while (w > 0 && out_cart_title[w - 1] == ' ') w--;
            out_cart_title[w] = '\0';
        }
        /* Game Code: 4 bytes at 0xAC */
        if (fseek(f, 0xAC, SEEK_SET) == 0 && fread(buf, 1, 4, f) == 4 && out_game_code && code_max > 0) {
            size_t w = 0;
            for (int i = 0; i < 4 && w + 1 < code_max; ++i) {
                unsigned char c = (unsigned char)buf[i];
                if (isalnum(c)) {
                    out_game_code[w++] = (char)c;
                }
            }
            out_game_code[w] = '\0';
        }
    }
    fclose(f);
}

/* Normaliza o titulo removendo tags (USA), [!], numeracao inicial "0001 - " e pontuacao */
static void normalize_game_title(const char* base, char* norm, size_t max_len) {
    if (!norm || max_len == 0) {
        return;
    }
    norm[0] = '\0';
    if (!base || !*base) {
        return;
    }

    const char* p = base;
    /* Pula numeracao de ROM no inicio (ex: "1986 - Pokemon" ou "0123_Jogo") se houver letras depois */
    const char* q = p;
    while (*q && isdigit((unsigned char)*q)) {
        q++;
    }
    if (q > p && (q - p) <= 5) {
        while (*q == ' ' || *q == '-' || *q == '_' || *q == '.') {
            q++;
        }
        if (*q && isalpha((unsigned char)*q)) {
            p = q;
        }
    }

    size_t out_idx = 0;
    int paren_depth = 0;
    int bracket_depth = 0;

    for (const char* c = p; *c && out_idx + 1 < max_len; ++c) {
        if (*c == '(') {
            paren_depth++;
            continue;
        }
        if (*c == ')') {
            if (paren_depth > 0) paren_depth--;
            continue;
        }
        if (*c == '[') {
            bracket_depth++;
            continue;
        }
        if (*c == ']') {
            if (bracket_depth > 0) bracket_depth--;
            continue;
        }
        if (paren_depth > 0 || bracket_depth > 0) {
            continue;
        }
        if (isalnum((unsigned char)*c)) {
            norm[out_idx++] = (char)tolower((unsigned char)*c);
        }
    }
    norm[out_idx] = '\0';

    /* Fallback caso o nome inteiro estivesse entre parenteses/colchetes */
    if (out_idx == 0) {
        for (const char* c = base; *c && out_idx + 1 < max_len; ++c) {
            if (isalnum((unsigned char)*c)) {
                norm[out_idx++] = (char)tolower((unsigned char)*c);
            }
        }
        norm[out_idx] = '\0';
    }
}

static bool test_candidate_file(const char* cand, char* out_path, size_t max_len) {
    if (!cand || !*cand) {
        return false;
    }
    FILE* f = fopen(cand, "rb");
    if (f) {
        fclose(f);
        if (out_path && max_len > 0) {
            strncpy(out_path, cand, max_len - 1);
            out_path[max_len - 1] = '\0';
        }
        return true;
    }
    struct stat st;
    if (stat(cand, &st) == 0 && S_ISREG(st.st_mode)) {
        if (out_path && max_len > 0) {
            strncpy(out_path, cand, max_len - 1);
            out_path[max_len - 1] = '\0';
        }
        return true;
    }
    return false;
}

static bool test_dir_cheat(const char* dir, const char* name, const char* game_code, const char* cart_title, char* out_path, size_t max_len) {
    if (!dir || !*dir) return false;
    static const char* const k_exts[] = {
        ".cht", ".cheats", ".CHT", ".CHEATS", ".gba.cht", ".gba.cheats",
        ".clt", ".CLT", ".cheat", ".cht.txt", ".cheats.txt", ".txt"
    };
    const char* names[3] = { name, game_code, cart_title };
    for (int n = 0; n < 3; ++n) {
        const char* cur_n = names[n];
        if (!cur_n || !*cur_n) continue;
        for (size_t i = 0; i < sizeof(k_exts) / sizeof(k_exts[0]); ++i) {
            char cand[256];
            char fname[128];
            snprintf(fname, sizeof(fname), "%s%s", cur_n, k_exts[i]);
            join_clean_path(cand, sizeof(cand), dir, fname);
            if (test_candidate_file(cand, out_path, max_len)) {
                return true;
            }
        }
    }
    return false;
}

static bool is_junk_dir_name(const char* name) {
    if (!name || name[0] == '.') {
        return true;
    }
    if (strcasecmp(name, "$RECYCLE.BIN") == 0 ||
        strcasecmp(name, "System Volume Information") == 0 ||
        strcasecmp(name, "LOST.DIR") == 0 ||
        strcasecmp(name, "Android") == 0) {
        return true;
    }
    return false;
}

typedef struct CheatSearchCtx {
    char clean_base[128];
    char norm_base[128];
    char game_code[8];
    char cart_title[32];
    char norm_cart_title[32];
    char rom_dir[256];
    char best_path[256];
    int best_score;
    int total_cht_seen;
} CheatSearchCtx;

static int score_cheat_candidate(const char* full_path, const char* file_name, const CheatSearchCtx* sctx) {
    char cand_base[128];
    char cand_norm[128];
    extract_clean_rom_base(file_name, cand_base, sizeof(cand_base));
    normalize_game_title(cand_base, cand_norm, sizeof(cand_norm));

    bool in_rom_dir = false;
    if (sctx->rom_dir[0] != '\0' && strcmp(sctx->rom_dir, ".") != 0) {
        size_t rd_len = strlen(sctx->rom_dir);
        if (strncasecmp(full_path, sctx->rom_dir, rd_len) == 0) {
            in_rom_dir = true;
        }
    }

    /* 1. Nome base exato (case-insensitive) */
    if (cand_base[0] != '\0' && strcasecmp(cand_base, sctx->clean_base) == 0) {
        return in_rom_dir ? 1000 : 950;
    }

    /* 2. Game Code do cartucho (ex: BPRE.cht / BPRE.cheats) */
    if (sctx->game_code[0] != '\0' && strcasecmp(cand_base, sctx->game_code) == 0) {
        return in_rom_dir ? 1000 : 950;
    }

    /* 3. Titulo interno do cartucho (ex: POKEMON FIRE.cht) */
    if (sctx->cart_title[0] != '\0' && strcasecmp(cand_base, sctx->cart_title) == 0) {
        return in_rom_dir ? 980 : 930;
    }

    /* 4. Nome normalizado exato (ignora tags, pontuacao, numeracao) */
    if (cand_norm[0] != '\0' && sctx->norm_base[0] != '\0') {
        if (strcmp(cand_norm, sctx->norm_base) == 0) {
            return in_rom_dir ? 900 : 850;
        }
    }

    /* 5. Titulo normalizado do cartucho */
    if (cand_norm[0] != '\0' && sctx->norm_cart_title[0] != '\0') {
        if (strcmp(cand_norm, sctx->norm_cart_title) == 0) {
            return in_rom_dir ? 880 : 830;
        }
    }

    /* 6. Substring normalizada (ex: "firered" dentro de "pokemonfireredversion") */
    if (cand_norm[0] != '\0') {
        size_t clen = strlen(cand_norm);
        size_t rlen = strlen(sctx->norm_base);
        size_t min_len = (clen < rlen) ? clen : rlen;
        if (min_len >= 4 && sctx->norm_base[0] != '\0') {
            if (strstr(sctx->norm_base, cand_norm) != NULL ||
                strstr(cand_norm, sctx->norm_base) != NULL) {
                return in_rom_dir ? 750 : 700;
            }
        }
        if (sctx->norm_cart_title[0] != '\0') {
            size_t tlen = strlen(sctx->norm_cart_title);
            size_t min_t = (clen < tlen) ? clen : tlen;
            if (min_t >= 4) {
                if (strstr(sctx->norm_cart_title, cand_norm) != NULL ||
                    strstr(cand_norm, sctx->norm_cart_title) != NULL) {
                    return in_rom_dir ? 730 : 680;
                }
            }
        }
    }

    /* 7. Fallback: Arquivo no mesmo diretorio da ROM ou em subpasta 'cheat' */
    if (in_rom_dir) {
        return 500;
    }
    if (safe_strcasestr(full_path, "cheat") != NULL) {
        return 400;
    }
    return 200;
}

static void scan_cht_dir_recursive(const char* dir_path, int depth, CheatSearchCtx* sctx) {
    if (!dir_path || !sctx || depth > 5 || sctx->best_score >= 1000) {
        return;
    }

    DIR* dir = opendir(dir_path);
    if (!dir) {
        return;
    }

    struct dirent* entry;
    while ((entry = readdir(dir)) != NULL) {
        if (!entry->d_name[0] || entry->d_name[0] == '.') {
            continue;
        }

        char full_path[256];
        join_clean_path(full_path, sizeof(full_path), dir_path, entry->d_name);

        if (has_cht_extension(entry->d_name)) {
            sctx->total_cht_seen++;
            int sc = score_cheat_candidate(full_path, entry->d_name, sctx);
            if (sc > sctx->best_score) {
                sctx->best_score = sc;
                strncpy(sctx->best_path, full_path, sizeof(sctx->best_path) - 1);
                sctx->best_path[sizeof(sctx->best_path) - 1] = '\0';
            }
            if (sctx->best_score >= 1000) {
                break;
            }
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

        if (is_dir && !is_junk_dir_name(entry->d_name)) {
            scan_cht_dir_recursive(full_path, depth + 1, sctx);
            if (sctx->best_score >= 1000) {
                break;
            }
        }
    }

    closedir(dir);
}

bool emulator_find_cheat_file(const char* rom_path, const char* rom_name, char* out_path, size_t max_len) {
    if (!out_path || max_len == 0) {
        return false;
    }
    out_path[0] = '\0';

    ps4_jailbreak_system();

    CheatSearchCtx sctx;
    memset(&sctx, 0, sizeof(sctx));
    strcpy(sctx.rom_dir, ".");

    extract_clean_rom_base(rom_name, sctx.clean_base, sizeof(sctx.clean_base));
    if (sctx.clean_base[0] == '\0' && rom_path && *rom_path) {
        extract_clean_rom_base(rom_path, sctx.clean_base, sizeof(sctx.clean_base));
    }
    if (sctx.clean_base[0] == '\0') {
        return false;
    }

    normalize_game_title(sctx.clean_base, sctx.norm_base, sizeof(sctx.norm_base));

    /* Extrai Game Code (ex: BPRE) e Cart Title (ex: POKEMON FIRE) do cabecalho da ROM */
    if (rom_path && *rom_path) {
        extract_rom_header_info(rom_path, sctx.game_code, sizeof(sctx.game_code),
                                sctx.cart_title, sizeof(sctx.cart_title));
        if (sctx.cart_title[0] != '\0') {
            normalize_game_title(sctx.cart_title, sctx.norm_cart_title, sizeof(sctx.norm_cart_title));
        }

        const char* slash = strrchr(rom_path, '/');
        const char* bslash = strrchr(rom_path, '\\');
        const char* last_sep = (slash && bslash) ? ((slash > bslash) ? slash : bslash) : (slash ? slash : bslash);
        if (last_sep) {
            size_t dlen = (size_t)(last_sep - rom_path);
            if (dlen >= sizeof(sctx.rom_dir)) {
                dlen = sizeof(sctx.rom_dir) - 1;
            }
            memcpy(sctx.rom_dir, rom_path, dlen);
            sctx.rom_dir[dlen] = '\0';
        }
    }

    static const char* const k_cheat_subdirs[] = {
        "cheat", "Cheat", "cheats", "Cheats", "CHEATS", "CHT", "cht"
    };
    size_t num_subdirs = sizeof(k_cheat_subdirs) / sizeof(k_cheat_subdirs[0]);

    /* 1. Tentativa Direta Rapida: Mesma pasta da ROM e subpastas de cheat */
    if (strcmp(sctx.rom_dir, ".") != 0) {
        if (test_dir_cheat(sctx.rom_dir, sctx.clean_base, sctx.game_code, sctx.cart_title, out_path, max_len)) {
            s_last_usb_cht_count = 1;
            return true;
        }

        for (size_t s = 0; s < num_subdirs; ++s) {
            char sub_dir[256];
            join_clean_path(sub_dir, sizeof(sub_dir), sctx.rom_dir, k_cheat_subdirs[s]);
            if (test_dir_cheat(sub_dir, sctx.clean_base, sctx.game_code, sctx.cart_title, out_path, max_len)) {
                s_last_usb_cht_count = 1;
                return true;
            }
        }
    }

    /* 2. Tentativa Direta Rapida: Raiz e pastas cheat de todas as portas USB */
    for (int u = 0; u < 8; ++u) {
        char usb_root[32];
        snprintf(usb_root, sizeof(usb_root), "/mnt/usb%d", u);
        if (test_dir_cheat(usb_root, sctx.clean_base, sctx.game_code, sctx.cart_title, out_path, max_len)) {
            s_last_usb_cht_count = 1;
            return true;
        }
        for (size_t s = 0; s < num_subdirs; ++s) {
            char sub_dir[256];
            join_clean_path(sub_dir, sizeof(sub_dir), usb_root, k_cheat_subdirs[s]);
            if (test_dir_cheat(sub_dir, sctx.clean_base, sctx.game_code, sctx.cart_title, out_path, max_len)) {
                s_last_usb_cht_count = 1;
                return true;
            }
        }

        /* Teste com prefixo /usb%d caso esteja no chroot sandbox */
        snprintf(usb_root, sizeof(usb_root), "/usb%d", u);
        if (test_dir_cheat(usb_root, sctx.clean_base, sctx.game_code, sctx.cart_title, out_path, max_len)) {
            s_last_usb_cht_count = 1;
            return true;
        }
        for (size_t s = 0; s < num_subdirs; ++s) {
            char sub_dir[256];
            join_clean_path(sub_dir, sizeof(sub_dir), usb_root, k_cheat_subdirs[s]);
            if (test_dir_cheat(sub_dir, sctx.clean_base, sctx.game_code, sctx.cart_title, out_path, max_len)) {
                s_last_usb_cht_count = 1;
                return true;
            }
        }
    }

    /* /mnt/ext0 */
    if (test_dir_cheat("/mnt/ext0", sctx.clean_base, sctx.game_code, sctx.cart_title, out_path, max_len)) {
        s_last_usb_cht_count = 1;
        return true;
    }
    for (size_t s = 0; s < num_subdirs; ++s) {
        char sub_dir[256];
        join_clean_path(sub_dir, sizeof(sub_dir), "/mnt/ext0", k_cheat_subdirs[s]);
        if (test_dir_cheat(sub_dir, sctx.clean_base, sctx.game_code, sctx.cart_title, out_path, max_len)) {
            s_last_usb_cht_count = 1;
            return true;
        }
    }

    /* 3. Varredura Recursiva via opendir/readdir */
    if (strcmp(sctx.rom_dir, ".") != 0) {
        scan_cht_dir_recursive(sctx.rom_dir, 0, &sctx);
    }

    int usb_mounted = 0;
    for (int u = 0; u < 8; ++u) {
        char usb_path[32];
        bool ok_mount = false;

        snprintf(usb_path, sizeof(usb_path), "/mnt/usb%d", u);
        DIR* test = opendir(usb_path);
        if (test) {
            closedir(test);
            ok_mount = true;
        } else {
            snprintf(usb_path, sizeof(usb_path), "/mnt/usb%d/", u);
            test = opendir(usb_path);
            if (test) {
                closedir(test);
                ok_mount = true;
            } else {
                snprintf(usb_path, sizeof(usb_path), "/usb%d", u);
                test = opendir(usb_path);
                if (test) {
                    closedir(test);
                    ok_mount = true;
                }
            }
        }

        if (ok_mount) {
            usb_mounted++;
            scan_cht_dir_recursive(usb_path, 0, &sctx);
        }
    }

    DIR* ext = opendir("/mnt/ext0");
    if (!ext) ext = opendir("/mnt/ext0/");
    if (ext) {
        closedir(ext);
        usb_mounted++;
        scan_cht_dir_recursive("/mnt/ext0", 0, &sctx);
    }

    /* 4. Pastas internas do PS4 */
    static const char* const k_internal_dirs[] = {
        "/data/PSgba/cheats",
        "/data/PSgba",
        "/data/gba4p/cheats",
        "/data/cheats",
        "/data/roms",
        "/data/gba"
    };
    for (size_t i = 0; i < sizeof(k_internal_dirs) / sizeof(k_internal_dirs[0]); ++i) {
        scan_cht_dir_recursive(k_internal_dirs[i], 0, &sctx);
    }

    s_last_usb_ports_mounted = usb_mounted;
    s_last_usb_cht_count = sctx.total_cht_seen;

    if (sctx.best_score > 0 && sctx.best_path[0] != '\0') {
        strncpy(out_path, sctx.best_path, max_len - 1);
        out_path[max_len - 1] = '\0';
        printf("[CHEAT] Selecionado '%s' (score=%d, total_vistos=%d)\n",
               out_path, sctx.best_score, sctx.total_cht_seen);
        return true;
    }

    return false;
}

/* ==============================================================================
 * PARSER UNIVERSAL DE ARQUIVOS .CHT (LIBRETRO, MGBA, EZ-FLASH, VBA, INI, TEXTO)
 * ============================================================================== */

static bool is_hex_str(const char* s, size_t len) {
    if (len == 0) return false;
    for (size_t i = 0; i < len; ++i) {
        if (!isxdigit((unsigned char)s[i])) {
            return false;
        }
    }
    return true;
}

static int add_normalized_code_tokens(struct mCheatSet* set, const char* code_str) {
    if (!set || !code_str || !*code_str) {
        return 0;
    }

    /* Extrai tokens separados por espaco, '+', ';', ',', '-', '\t' */
    char tokens[64][32];
    int n_tok = 0;

    const char* p = code_str;
    while (*p && n_tok < 64) {
        while (*p && (*p == ' ' || *p == '\t' || *p == '+' || *p == ';' || *p == ',' || *p == '-' || *p == '\r' || *p == '\n')) {
            p++;
        }
        if (!*p) break;

        /* Remove prefixo 0x se existir */
        if (p[0] == '0' && (p[1] == 'x' || p[1] == 'X')) {
            p += 2;
        }

        size_t tlen = 0;
        while (p[tlen] && p[tlen] != ' ' && p[tlen] != '\t' && p[tlen] != '+' &&
               p[tlen] != ';' && p[tlen] != ',' && p[tlen] != '-' &&
               p[tlen] != '\r' && p[tlen] != '\n') {
            if (tlen + 1 < sizeof(tokens[0])) {
                tokens[n_tok][tlen] = p[tlen];
            }
            tlen++;
        }
        if (tlen >= sizeof(tokens[0])) {
            tlen = sizeof(tokens[0]) - 1;
        }
        tokens[n_tok][tlen] = '\0';
        n_tok++;
        p += tlen;
    }

    int added = 0;
    int i = 0;
    while (i < n_tok) {
        const char* t1 = tokens[i];
        size_t len1 = strlen(t1);

        /* Caso 1: Formato com ':' (ex: 020257C4:0001 ou 820257C4:0001) */
        const char* colon = strchr(t1, ':');
        if (colon) {
            if (mCheatAddLine(set, t1, 0)) {
                added++;
            } else {
                char alt[32];
                strncpy(alt, t1, sizeof(alt) - 1);
                alt[sizeof(alt) - 1] = '\0';
                char* c2 = strchr(alt, ':');
                if (c2) *c2 = ' ';
                if (mCheatAddLine(set, alt, 0)) {
                    added++;
                }
            }
            i++;
            continue;
        }

        /* Caso 2: 12 digitos hex continuos (XXXXXXXXYYYY -> XXXXXXXX:YYYY ou XXXXXXXX YYYY) */
        if (len1 == 12 && is_hex_str(t1, 12)) {
            char line[32];
            bool is_ram = (strncmp(t1, "02", 2) == 0 || strncmp(t1, "03", 2) == 0);
            if (is_ram) {
                snprintf(line, sizeof(line), "%.8s:%.4s", t1, t1 + 8);
                if (mCheatAddLine(set, line, 0)) {
                    added++;
                    i++;
                    continue;
                }
            }
            snprintf(line, sizeof(line), "%.8s %.4s", t1, t1 + 8);
            if (mCheatAddLine(set, line, 0)) {
                added++;
            } else {
                snprintf(line, sizeof(line), "%.8s:%.4s", t1, t1 + 8);
                if (mCheatAddLine(set, line, 0)) {
                    added++;
                }
            }
            i++;
            continue;
        }

        /* Caso 3: 16 digitos hex continuos (XXXXXXXXYYYYYYYY -> XXXXXXXX:YYYYYYYY ou XXXXXXXX YYYYYYYY) */
        if (len1 == 16 && is_hex_str(t1, 16)) {
            char line[32];
            bool is_ram = (strncmp(t1, "02", 2) == 0 || strncmp(t1, "03", 2) == 0);
            if (is_ram) {
                snprintf(line, sizeof(line), "%.8s:%.8s", t1, t1 + 8);
                if (mCheatAddLine(set, line, 0)) {
                    added++;
                    i++;
                    continue;
                }
            }
            snprintf(line, sizeof(line), "%.8s %.8s", t1, t1 + 8);
            if (mCheatAddLine(set, line, 0)) {
                added++;
            } else {
                snprintf(line, sizeof(line), "%.8s:%.8s", t1, t1 + 8);
                if (mCheatAddLine(set, line, 0)) {
                    added++;
                }
            }
            i++;
            continue;
        }

        /* Caso 4: 8 digitos hex seguidos por outro token hex (1 a 8 digitos) */
        if (len1 == 8 && is_hex_str(t1, 8) && (i + 1) < n_tok) {
            const char* t2 = tokens[i + 1];
            size_t len2 = strlen(t2);
            if (len2 >= 1 && len2 <= 8 && is_hex_str(t2, len2)) {
                char line[32];
                bool is_ram = (strncmp(t1, "02", 2) == 0 || strncmp(t1, "03", 2) == 0);

                /* Para enderecos diretos de RAM (0x02XXXXXX ou 0x03XXXXXX), formata prioritariamente
                 * no padrao VBA com dois-pontos (020257C4:423F ou 020257C4:000F423F). Isso garante
                 * atribuicao precisa em memoria (width 1, 2 ou 4) sem ser descartado como Game ID do CodeBreaker */
                if (is_ram) {
                    if (len2 <= 2) {
                        unsigned int val = (unsigned int)strtoul(t2, NULL, 16);
                        snprintf(line, sizeof(line), "%.8s:%02X", t1, val);
                    } else if (len2 <= 4) {
                        unsigned int val = (unsigned int)strtoul(t2, NULL, 16);
                        snprintf(line, sizeof(line), "%.8s:%04X", t1, val);
                    } else {
                        unsigned long val = strtoul(t2, NULL, 16);
                        snprintf(line, sizeof(line), "%.8s:%08lX", t1, val);
                    }
                    if (mCheatAddLine(set, line, 0)) {
                        added++;
                        i += 2;
                        continue;
                    }
                }

                /* Tenta formato com espaco (Action Replay / CodeBreaker / GameShark) */
                if (len2 <= 4) {
                    unsigned int val = (unsigned int)strtoul(t2, NULL, 16);
                    snprintf(line, sizeof(line), "%.8s %04X", t1, val);
                } else {
                    unsigned long val = strtoul(t2, NULL, 16);
                    snprintf(line, sizeof(line), "%.8s %08lX", t1, val);
                }
                if (mCheatAddLine(set, line, 0)) {
                    added++;
                    i += 2;
                    continue;
                }

                /* Fallback para formato com dois-pontos se ainda nao tentado */
                if (!is_ram) {
                    if (len2 <= 4) {
                        unsigned int val = (unsigned int)strtoul(t2, NULL, 16);
                        snprintf(line, sizeof(line), "%.8s:%04X", t1, val);
                    } else {
                        unsigned long val = strtoul(t2, NULL, 16);
                        snprintf(line, sizeof(line), "%.8s:%08lX", t1, val);
                    }
                    if (mCheatAddLine(set, line, 0)) {
                        added++;
                        i += 2;
                        continue;
                    }
                }
            }
        }

        i++;
    }

    return added;
}

static bool looks_like_code_line(const char* line) {
    while (*line == ' ' || *line == '\t') line++;
    if (line[0] == '0' && (line[1] == 'x' || line[1] == 'X')) {
        line += 2;
    }
    size_t hex_prefix = 0;
    while (isxdigit((unsigned char)line[hex_prefix])) {
        hex_prefix++;
    }
    if (hex_prefix == 8) {
        char next = line[8];
        if (next == ' ' || next == '\t' || next == ':' || next == '+' || next == '-') {
            const char* p2 = line + 9;
            while (*p2 == ' ' || *p2 == '\t' || *p2 == '+') p2++;
            if (isxdigit((unsigned char)p2[0])) {
                return true;
            }
        }
    } else if (hex_prefix == 12 || hex_prefix == 16) {
        char next = line[hex_prefix];
        if (next == '\0' || next == ' ' || next == '\t' || next == '+' || next == ';') {
            return true;
        }
    }
    return false;
}

static void unquote_and_trim(const char* src, char* dst, size_t max_len) {
    if (!dst || max_len == 0) return;
    dst[0] = '\0';
    if (!src) return;

    while (*src == ' ' || *src == '\t') src++;
    size_t len = strlen(src);
    while (len > 0 && (src[len - 1] == ' ' || src[len - 1] == '\t' || src[len - 1] == '\r' || src[len - 1] == '\n')) {
        len--;
    }
    if (len >= 2 && ((src[0] == '"' && src[len - 1] == '"') || (src[0] == '\'' && src[len - 1] == '\''))) {
        src++;
        len -= 2;
    }

    size_t out = 0;
    for (size_t i = 0; i < len && out + 1 < max_len; ++i) {
        if (src[i] == '\\' && i + 1 < len && (src[i + 1] == '"' || src[i + 1] == '\\')) {
            i++;
        }
        dst[out++] = src[i];
    }
    dst[out] = '\0';
}

#define MAX_PARSED_LIBRETRO_CHEATS 512

static void copy_cheat_seeds_no_hook(struct mCheatSet* dst, struct mCheatSet* src) {
    if (!dst || !src) return;
    struct GBACheatSet* newSet = (struct GBACheatSet*) dst;
    struct GBACheatSet* gbaset = (struct GBACheatSet*) src;
    newSet->gsaVersion = gbaset->gsaVersion;
    memcpy(newSet->gsaSeeds, gbaset->gsaSeeds, sizeof(newSet->gsaSeeds));
    newSet->cbRngState = gbaset->cbRngState;
    newSet->cbMaster = gbaset->cbMaster;
    memcpy(newSet->cbSeeds, gbaset->cbSeeds, sizeof(newSet->cbSeeds));
    memcpy(newSet->cbTable, gbaset->cbTable, sizeof(newSet->cbTable));
    /* NAO copia newSet->hook = gbaset->hook para nao marcar codigos normais de RAM como hooked */
}

static bool parse_libretro_cheats_mem(struct mCheatDevice* device, char* buf) {
    struct mCheatSet* sets[MAX_PARSED_LIBRETRO_CHEATS];
    memset(sets, 0, sizeof(sets));
    int max_idx = -1;

    char* saveptr = NULL;
    char* line = strtok_r(buf, "\n", &saveptr);
    while (line != NULL) {
        while (*line == ' ' || *line == '\t' || *line == '\r') {
            line++;
        }
        if (strncasecmp(line, "cheat", 5) == 0 && isdigit((unsigned char)line[5])) {
            char* endp = NULL;
            long idx = strtol(line + 5, &endp, 10);
            if (idx >= 0 && idx < MAX_PARSED_LIBRETRO_CHEATS && endp && *endp == '_') {
                const char* key = endp + 1;
                const char* eq = strchr(key, '=');
                if (eq) {
                    char val[512];
                    unquote_and_trim(eq + 1, val, sizeof(val));

                    if (!sets[idx]) {
                        sets[idx] = device->createSet(device, NULL);
                        if (sets[idx]) {
                            sets[idx]->enabled = false;
                        }
                    }
                    if (sets[idx]) {
                        if (idx > max_idx) max_idx = (int)idx;

                        if (strncasecmp(key, "desc", 4) == 0) {
                            mCheatSetRename(sets[idx], val);
                        } else if (strncasecmp(key, "enable", 6) == 0) {
                            sets[idx]->enabled = (strcasecmp(val, "true") == 0 || strcmp(val, "1") == 0);
                        } else if (strncasecmp(key, "code", 4) == 0) {
                            if (idx > 0 && sets[idx - 1]) {
                                copy_cheat_seeds_no_hook(sets[idx], sets[idx - 1]);
                            }
                            add_normalized_code_tokens(sets[idx], val);
                        }
                    }
                }
            }
        }
        line = strtok_r(NULL, "\n", &saveptr);
    }

    int total_added = 0;
    struct mCheatSet* prev_valid = NULL;
    for (int i = 0; i <= max_idx; ++i) {
        if (!sets[i]) continue;
        if (mCheatListSize(&sets[i]->list) > 0 || mCheatPatchListSize(&sets[i]->romPatches) > 0 || StringListSize(&sets[i]->lines) > 0) {
            if (prev_valid) {
                copy_cheat_seeds_no_hook(sets[i], prev_valid);
            }
            if (!sets[i]->name || !*sets[i]->name) {
                char def_name[32];
                snprintf(def_name, sizeof(def_name), "Trapaca #%d", total_added + 1);
                mCheatSetRename(sets[i], def_name);
            }
            mCheatAddSet(device, sets[i]);
            prev_valid = sets[i];
            total_added++;
        } else {
            mCheatSetDeinit(sets[i]);
        }
    }

    return (total_added > 0);
}

static bool parse_universal_cheats_mem(struct mCheatDevice* device, char* buf, size_t len) {
    /* Detecta formato Libretro (cheats = N / cheat0_desc / cheat0_code) */
    if (safe_strcasestr(buf, "cheat0_") != NULL || safe_strcasestr(buf, "cheats =") != NULL || safe_strcasestr(buf, "cheats=") != NULL) {
        char* copy = (char*)malloc(len + 1);
        if (copy) {
            memcpy(copy, buf, len + 1);
            bool ok = parse_libretro_cheats_mem(device, copy);
            free(copy);
            if (ok && mCheatSetsSize(&device->cheats) > 0) {
                return true;
            }
        }
    }

    /* Detecta formato EZ-Flash ([Nome] + ON=xxxx,yy) */
    if (strchr(buf, '[') != NULL && (strstr(buf, "\nON=") != NULL || strncmp(buf, "ON=", 3) == 0)) {
        struct VFile* vf = VFileFromConstMemory(buf, len);
        if (vf) {
            bool ok = mCheatParseEZFChtFile(device, vf);
            vf->close(vf);
            if (ok && mCheatSetsSize(&device->cheats) > 0) {
                return true;
            }
        }
    }

    /* Formato Geral: mGBA (# Nome), INI ([Nome]), Texto Puro (Nome seguido de Hex) ou Hex Puro */
    char* copy = (char*)malloc(len + 1);
    if (!copy) {
        return false;
    }
    memcpy(copy, buf, len + 1);

    struct mCheatSet* cur_set = NULL;
    struct mCheatSet* prev_set = NULL;
    bool next_disabled = true; /* Por seguranca, inicia trapacas desativadas para nao travar o jogo */
    int auto_idx = 1;

    char* saveptr = NULL;
    char* raw_line = strtok_r(copy, "\n", &saveptr);
    while (raw_line != NULL) {
        char* line = raw_line;
        while (*line == ' ' || *line == '\t' || *line == '\r') {
            line++;
        }
        size_t llen = strlen(line);
        while (llen > 0 && (line[llen - 1] == ' ' || line[llen - 1] == '\t' || line[llen - 1] == '\r')) {
            line[--llen] = '\0';
        }

        if (llen == 0 || line[0] == ';' || (line[0] == '/' && line[1] == '/')) {
            raw_line = strtok_r(NULL, "\n", &saveptr);
            continue;
        }

        /* Diretivas mGBA (!disabled, !enabled, !GSAv1, !PARv3, !reset) */
        if (line[0] == '!') {
            const char* dir = line + 1;
            while (*dir == ' ' || *dir == '\t') dir++;
            if (strcasecmp(dir, "disabled") == 0) {
                next_disabled = true;
            } else if (strcasecmp(dir, "enabled") == 0) {
                next_disabled = false;
            }
            raw_line = strtok_r(NULL, "\n", &saveptr);
            continue;
        }

        /* Cabecalho explicito (# Nome ou [Nome]) */
        if (line[0] == '#' || (line[0] == '[' && line[llen - 1] == ']')) {
            char title[128];
            if (line[0] == '#') {
                const char* t = line + 1;
                while (*t == ' ' || *t == '\t' || *t == '#') t++;
                strncpy(title, t, sizeof(title) - 1);
                title[sizeof(title) - 1] = '\0';
            } else {
                size_t tlen = llen - 2;
                if (tlen >= sizeof(title)) tlen = sizeof(title) - 1;
                memcpy(title, line + 1, tlen);
                title[tlen] = '\0';
            }

            if (cur_set) {
                if (mCheatListSize(&cur_set->list) > 0 || mCheatPatchListSize(&cur_set->romPatches) > 0 || StringListSize(&cur_set->lines) > 0) {
                    mCheatAddSet(device, cur_set);
                    prev_set = cur_set;
                    cur_set = NULL;
                } else {
                    mCheatSetRename(cur_set, title);
                }
            }
            if (!cur_set) {
                cur_set = device->createSet(device, title[0] ? title : "Trapaca");
                if (cur_set) {
                    cur_set->enabled = !next_disabled;
                    if (prev_set) {
                        copy_cheat_seeds_no_hook(cur_set, prev_set);
                    }
                }
            }
            next_disabled = true;
            raw_line = strtok_r(NULL, "\n", &saveptr);
            continue;
        }

        /* Verifica se a linha contem codigos hexadecimais */
        if (looks_like_code_line(line)) {
            if (!cur_set) {
                char def_title[64];
                snprintf(def_title, sizeof(def_title), "Trapaca #%d", auto_idx++);
                cur_set = device->createSet(device, def_title);
                if (cur_set) {
                    cur_set->enabled = false;
                    if (prev_set) {
                        copy_cheat_seeds_no_hook(cur_set, prev_set);
                    }
                }
            }
            if (cur_set) {
                add_normalized_code_tokens(cur_set, line);
            }
        } else {
            /* Linha de texto puro (ex: Nome do Cheat copiado de site sem '#') */
            if (cur_set) {
                if (mCheatListSize(&cur_set->list) > 0 || mCheatPatchListSize(&cur_set->romPatches) > 0 || StringListSize(&cur_set->lines) > 0) {
                    mCheatAddSet(device, cur_set);
                    prev_set = cur_set;
                    cur_set = NULL;
                } else {
                    mCheatSetRename(cur_set, line);
                }
            }
            if (!cur_set) {
                cur_set = device->createSet(device, line);
                if (cur_set) {
                    cur_set->enabled = false;
                    if (prev_set) {
                        copy_cheat_seeds_no_hook(cur_set, prev_set);
                    }
                }
            }
        }

        raw_line = strtok_r(NULL, "\n", &saveptr);
    }

    if (cur_set) {
        if (mCheatListSize(&cur_set->list) > 0 || mCheatPatchListSize(&cur_set->romPatches) > 0 || StringListSize(&cur_set->lines) > 0) {
            mCheatAddSet(device, cur_set);
        } else {
            mCheatSetDeinit(cur_set);
        }
    }

    free(copy);

    if (mCheatSetsSize(&device->cheats) > 0) {
        return true;
    }

    /* Fallback final para o parser nativo da libmgba */
    struct VFile* vf = VFileFromConstMemory(buf, len);
    if (vf) {
        bool ok = mCheatParseFile(device, vf);
        vf->close(vf);
        return ok && (mCheatSetsSize(&device->cheats) > 0);
    }
    return false;
}

static bool is_master_code_set(const struct mCheatSet* set) {
    if (!set) return false;
    if (set->name && *set->name) {
        if (safe_strcasestr(set->name, "master") != NULL ||
            safe_strcasestr(set->name, "(m)") != NULL ||
            safe_strcasestr(set->name, "must be on") != NULL ||
            safe_strcasestr(set->name, "enable code") != NULL) {
            return true;
        }
    }
    const struct GBACheatSet* gbaset = (const struct GBACheatSet*)set;
    if (gbaset->hook != NULL || gbaset->cbMaster != 0) {
        return true;
    }
    return false;
}

bool emulator_load_cheats(int slot_index, const char* rom_path) {
    if (slot_index < 0 || slot_index >= GBA_MAX_INSTANCES) {
        return false;
    }
    struct mCore* core = g_gba_ctx.instances[slot_index].core;
    if (!core || !core->cheatDevice) {
        return false;
    }

    ps4_jailbreak_system();

    char clean_base[128];
    extract_clean_rom_base(rom_path, clean_base, sizeof(clean_base));

    char cht_path[256];
    if (!emulator_find_cheat_file(rom_path, clean_base, cht_path, sizeof(cht_path))) {
        s_cheat_file_found[slot_index] = false;
        s_cheat_file_paths[slot_index][0] = '\0';
        snprintf(s_cheat_status[slot_index], sizeof(s_cheat_status[slot_index]),
                 "Buscado: '%s' (.cht/.cheats) | USBs: %d | Vistos: %d",
                 clean_base, s_last_usb_ports_mounted, s_last_usb_cht_count);
        return false;
    }

    /* Registra o caminho encontrado imediatamente */
    s_cheat_file_found[slot_index] = true;
    strncpy(s_cheat_file_paths[slot_index], cht_path, sizeof(s_cheat_file_paths[slot_index]) - 1);
    s_cheat_file_paths[slot_index][sizeof(s_cheat_file_paths[slot_index]) - 1] = '\0';

    struct mCheatDevice* device = core->cheatDevice(core);
    if (!device) {
        snprintf(s_cheat_status[slot_index], sizeof(s_cheat_status[slot_index]),
                 "Erro interno: dispositivo de cheat indisponivel");
        return false;
    }

    mCheatDeviceClear(device);

    FILE* f = fopen(cht_path, "rb");
    if (!f) {
        snprintf(s_cheat_status[slot_index], sizeof(s_cheat_status[slot_index]),
                 "Erro ao abrir '%s' (permissao/IO)", cht_path);
        return false;
    }

    fseek(f, 0, SEEK_END);
    long fsize = ftell(f);
    fseek(f, 0, SEEK_SET);

    if (fsize <= 0 || fsize > (2 * 1024 * 1024)) {
        fclose(f);
        snprintf(s_cheat_status[slot_index], sizeof(s_cheat_status[slot_index]),
                 "Arquivo '%s' vazio ou invalido (%ld bytes)", cht_path, fsize);
        return false;
    }

    char* raw_buf = (char*)malloc((size_t)fsize + 1);
    if (!raw_buf) {
        fclose(f);
        return false;
    }

    size_t read_bytes = fread(raw_buf, 1, (size_t)fsize, f);
    fclose(f);
    raw_buf[read_bytes] = '\0';

    /* Remove UTF-8 BOM (EF BB BF) se presente e normaliza CRLF (\r\n) para LF (\n) */
    char* text_start = raw_buf;
    if (read_bytes >= 3 &&
        (unsigned char)raw_buf[0] == 0xEF &&
        (unsigned char)raw_buf[1] == 0xBB &&
        (unsigned char)raw_buf[2] == 0xBF) {
        text_start += 3;
        read_bytes -= 3;
    }

    size_t w = 0;
    for (size_t r = 0; r < read_bytes; ++r) {
        if (text_start[r] == '\r') {
            if (r + 1 < read_bytes && text_start[r + 1] == '\n') {
                continue;
            }
            text_start[w++] = '\n';
        } else {
            text_start[w++] = text_start[r];
        }
    }
    text_start[w] = '\0';

    /* Verifica se o arquivo e INI puro ([Nome] sem linhas ON=) */
    bool has_bracket = (strchr(text_start, '[') != NULL);
    bool has_on_eq = (strstr(text_start, "\nON=") != NULL || strncmp(text_start, "ON=", 3) == 0);
    bool is_ini_cheat = (has_bracket && !has_on_eq);

    bool ok = false;
    if (!is_ini_cheat) {
        /* 1. Tenta PRIMEIRO o parser nativo da libmgba (suporta .cheats, libretro .cht e EZ-Flash .cht) */
        struct VFile* vf = VFileFromConstMemory(text_start, w);
        if (vf) {
            ok = mCheatParseFile(device, vf);
            vf->close(vf);
        }
    }

    /* 2. Verifica se a libmgba realmente compilou instrucoes/opcodes */
    size_t total_compiled = 0;
    size_t n_sets = mCheatSetsSize(&device->cheats);
    for (size_t s = 0; s < n_sets; ++s) {
        struct mCheatSet* set = *mCheatSetsGetPointer(&device->cheats, s);
        if (set) {
            total_compiled += mCheatListSize(&set->list);
            total_compiled += mCheatPatchListSize(&set->romPatches);
        }
    }

    /* 3. Se a libmgba nao compilou nenhum opcode ou se e arquivo INI, aplica o parser universal */
    if (total_compiled == 0 || is_ini_cheat) {
        mCheatDeviceClear(device);
        ok = parse_universal_cheats_mem(device, text_start, w);
    }

    free(raw_buf);

    size_t total = mCheatSetsSize(&device->cheats);
    total_compiled = 0;
    for (size_t c = 0; c < total; ++c) {
        struct mCheatSet* set = *mCheatSetsGetPointer(&device->cheats, c);
        if (set) {
            total_compiled += mCheatListSize(&set->list);
            total_compiled += mCheatPatchListSize(&set->romPatches);
            if (set->enabled) {
                mCheatRefresh(device, set);
            }
        }
    }

    if (total > 0) {
        snprintf(s_cheat_status[slot_index], sizeof(s_cheat_status[slot_index]),
                 "Carregado: %s (%zu trapacas, %zu codigos)", cht_path, total, total_compiled);
        return true;
    }

    snprintf(s_cheat_status[slot_index], sizeof(s_cheat_status[slot_index]),
             "Lido: %s (%zu bytes), 0 codigos reconhecidos", cht_path, (size_t)fsize);
    return false;
}

bool emulator_has_cheat_file(int slot_index) {
    if (slot_index < 0 || slot_index >= GBA_MAX_INSTANCES) {
        return false;
    }
    return s_cheat_file_found[slot_index];
}

int emulator_get_cheat_count(int slot_index) {
    if (slot_index < 0 || slot_index >= GBA_MAX_INSTANCES) {
        return 0;
    }
    struct mCore* core = g_gba_ctx.instances[slot_index].core;
    if (!core || !core->cheatDevice) {
        return 0;
    }
    struct mCheatDevice* device = core->cheatDevice(core);
    if (!device) {
        return 0;
    }
    return (int)mCheatSetsSize(&device->cheats);
}

int emulator_get_cheat_code_count(int slot_index, int cheat_index) {
    if (slot_index < 0 || slot_index >= GBA_MAX_INSTANCES) {
        return 0;
    }
    struct mCore* core = g_gba_ctx.instances[slot_index].core;
    if (!core || !core->cheatDevice) {
        return 0;
    }
    struct mCheatDevice* device = core->cheatDevice(core);
    if (!device) {
        return 0;
    }
    if (cheat_index < 0 || (size_t)cheat_index >= mCheatSetsSize(&device->cheats)) {
        return 0;
    }
    struct mCheatSet* set = *mCheatSetsGetPointer(&device->cheats, (size_t)cheat_index);
    if (!set) {
        return 0;
    }
    size_t count = mCheatListSize(&set->list) + mCheatPatchListSize(&set->romPatches);
    if (count == 0) {
        count = StringListSize(&set->lines);
    }
    return (int)count;
}

bool emulator_is_cheat_master(int slot_index, int cheat_index) {
    if (slot_index < 0 || slot_index >= GBA_MAX_INSTANCES) {
        return false;
    }
    struct mCore* core = g_gba_ctx.instances[slot_index].core;
    if (!core || !core->cheatDevice) {
        return false;
    }
    struct mCheatDevice* device = core->cheatDevice(core);
    if (!device) {
        return false;
    }
    if (cheat_index < 0 || (size_t)cheat_index >= mCheatSetsSize(&device->cheats)) {
        return false;
    }
    struct mCheatSet* set = *mCheatSetsGetPointer(&device->cheats, (size_t)cheat_index);
    return is_master_code_set(set);
}

const char* emulator_get_cheat_name(int slot_index, int cheat_index) {
    if (slot_index < 0 || slot_index >= GBA_MAX_INSTANCES) {
        return NULL;
    }
    struct mCore* core = g_gba_ctx.instances[slot_index].core;
    if (!core || !core->cheatDevice) {
        return NULL;
    }
    struct mCheatDevice* device = core->cheatDevice(core);
    if (!device) {
        return NULL;
    }

    if (cheat_index < 0 || (size_t)cheat_index >= mCheatSetsSize(&device->cheats)) {
        return NULL;
    }
    struct mCheatSet* set = *mCheatSetsGetPointer(&device->cheats, (size_t)cheat_index);
    if (!set) {
        return NULL;
    }
    return (set->name && *set->name) ? set->name : "Trapaca Sem Nome";
}

bool emulator_is_cheat_enabled(int slot_index, int cheat_index) {
    if (slot_index < 0 || slot_index >= GBA_MAX_INSTANCES) {
        return false;
    }
    struct mCore* core = g_gba_ctx.instances[slot_index].core;
    if (!core || !core->cheatDevice) {
        return false;
    }
    struct mCheatDevice* device = core->cheatDevice(core);
    if (!device) {
        return false;
    }

    if (cheat_index < 0 || (size_t)cheat_index >= mCheatSetsSize(&device->cheats)) {
        return false;
    }
    struct mCheatSet* set = *mCheatSetsGetPointer(&device->cheats, (size_t)cheat_index);
    return set ? set->enabled : false;
}

void emulator_apply_cheats_frame(int slot_index) {
    if (slot_index < 0 || slot_index >= GBA_MAX_INSTANCES) {
        return;
    }
    struct mCore* core = g_gba_ctx.instances[slot_index].core;
    if (!core || !core->cheatDevice) {
        return;
    }
    struct mCheatDevice* device = core->cheatDevice(core);
    if (!device) {
        return;
    }

    size_t total = mCheatSetsSize(&device->cheats);
    for (size_t c = 0; c < total; ++c) {
        struct mCheatSet* set = *mCheatSetsGetPointer(&device->cheats, c);
        if (set && set->enabled) {
            mCheatRefresh(device, set);
        }
    }
}

void emulator_toggle_cheat(int slot_index, int cheat_index) {
    if (slot_index < 0 || slot_index >= GBA_MAX_INSTANCES) {
        return;
    }
    struct mCore* core = g_gba_ctx.instances[slot_index].core;
    if (!core || !core->cheatDevice) {
        return;
    }
    struct mCheatDevice* device = core->cheatDevice(core);
    if (!device) {
        return;
    }

    if (cheat_index < 0 || (size_t)cheat_index >= mCheatSetsSize(&device->cheats)) {
        return;
    }
    struct mCheatSet* set = *mCheatSetsGetPointer(&device->cheats, (size_t)cheat_index);
    if (!set) {
        return;
    }

    set->enabled = !set->enabled;
    mCheatRefresh(device, set);

    /* Se ativou qualquer trapaça, auto-ativa e aplica qualquer Master Code da lista */
    if (set->enabled) {
        size_t total = mCheatSetsSize(&device->cheats);
        for (size_t i = 0; i < total; ++i) {
            struct mCheatSet* s = *mCheatSetsGetPointer(&device->cheats, i);
            if (s && !s->enabled && is_master_code_set(s)) {
                s->enabled = true;
                mCheatRefresh(device, s);
            }
        }
    }
}

const char* emulator_get_cheat_file_path(int slot_index) {
    if (slot_index < 0 || slot_index >= GBA_MAX_INSTANCES) {
        return "";
    }
    return s_cheat_file_paths[slot_index];
}

const char* emulator_get_cheat_status(int slot_index) {
    if (slot_index < 0 || slot_index >= GBA_MAX_INSTANCES) {
        return "";
    }
    return s_cheat_status[slot_index];
}
