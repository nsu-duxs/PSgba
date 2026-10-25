#include "emulator_manager.h"
#include "link_cable_hub.h"
#include "audio_ps4.h"
#include "save_manager.h"
#include "input_ds4.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <mgba/core/log.h>
#include <mgba-util/vfs.h>
#include <mgba/internal/arm/arm.h>
#include <mgba/internal/gba/gba.h>
#include <mgba/internal/gba/video.h>

PS4GBA_Context g_gba_ctx = {0};

static struct GBA* s_frame_gbas[GBA_MAX_INSTANCES];
static uint32_t s_start_frames[GBA_MAX_INSTANCES];
static uint32_t s_start_cycles[GBA_MAX_INSTANCES];

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
    save_manager_attach_battery_save(core, g_gba_ctx.instances[slot_index].rom_name,
                                     slot_index, user_id);

    /* Reset core ANTES de anexar o driver de SIO do Link Cable para que GBASIOReset não o sobrescreva */
    core->reset(core);

    g_gba_ctx.instances[slot_index].core = core;
    g_gba_ctx.instances[slot_index].video_buffer = vbuf;
    g_gba_ctx.instances[slot_index].user_id = user_id;
    g_gba_ctx.instances[slot_index].active = true;
    g_gba_ctx.active_count++;
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
            gba->cpu->cycles = gba->cpu->nextEvent;
        }

        ARMRunLoop(gba->cpu);

        if (gba->cpu->halted) {
            gba->cpu->cycles = 0;
            gba->cpu->nextEvent = 0;
        }

        if (idx == 0 && link_hub_consume_just_started()) {
            master_started = true;
            break;
        }
    }

    mTimingDeschedule(&gba->timing, lockstep_ev);
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
            gba->cpu->nextEvent = 0;
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
    int active_instances = 0;
    int first_active = -1;
    for (int i = 0; i < GBA_MAX_INSTANCES; ++i) {
        if (g_gba_ctx.instances[i].active && g_gba_ctx.instances[i].core &&
            g_gba_ctx.instances[i].core->board) {
            struct GBA* gba = (struct GBA*)g_gba_ctx.instances[i].core->board;
            if (gba->cpu && gba->cpu->halted) {
                gba->cpu->cycles = 0;
                gba->cpu->nextEvent = 0;
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

    link_hub_begin_frame();

    uint32_t slice_target = 0;
    while (slice_target < VIDEO_TOTAL_LENGTH) {
        bool xfer_active = link_hub_is_transfer_active();
        uint32_t step = xfer_active ? 64U : 256U;
        slice_target += step;
        if (slice_target > VIDEO_TOTAL_LENGTH) {
            slice_target = VIDEO_TOTAL_LENGTH;
        }

        if (s_frame_gbas[0]) {
            bool master_started = step_instance_slice(0, slice_target);
            if (master_started) {
                /*
                 * O Mestre (Player 0) acabou de iniciar uma transferência no ciclo `master_elapsed`.
                 * 1. Avança todos os Escravos (Players 1..3) exatamente até `master_elapsed`.
                 * 2. Agenda o evento de conclusão da transferência nos Escravos no ciclo exato.
                 * 3. Conclui o restante da fatia `slice_target` para todas as instâncias.
                 */
                uint32_t master_elapsed =
                    (uint32_t)(mTimingCurrentTime(&s_frame_gbas[0]->timing) - s_start_cycles[0]);
                for (int j = 1; j < GBA_MAX_INSTANCES; ++j) {
                    if (s_frame_gbas[j]) {
                        step_instance_slice(j, master_elapsed);
                    }
                }
                link_hub_begin_transfer_on_slaves();
                step_instance_slice(0, slice_target);
            }
        }

        for (int i = 1; i < GBA_MAX_INSTANCES; ++i) {
            if (s_frame_gbas[i]) {
                step_instance_slice(i, slice_target);
            }
        }

        link_hub_sync_pins();

        if ((slice_target % VIDEO_HORIZONTAL_LENGTH) < step) {
            link_hub_notify_scanline();
        }
    }

    for (int i = 0; i < GBA_MAX_INSTANCES; ++i) {
        if (s_frame_gbas[i]) {
            finish_instance_frame(i);
        }
    }
}

void emulator_destroy_all(void) {
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