#include "link_cable_hub.h"
#include <string.h>
#include <mgba/internal/arm/arm.h>
#include <mgba/internal/gba/gba.h>
#include <mgba/internal/gba/io.h>

#define LINK_GPIO_GRACE_FRAMES 600U
#define LINK_MIN_TRANSFER_CYCLES 64

PS4LinkHub g_link_hub = {0};

static volatile unsigned char s_link_lock = 0;

static inline void link_lock(void) {
    while (__atomic_test_and_set(&s_link_lock, __ATOMIC_ACQUIRE)) {
        __builtin_ia32_pause();
    }
}

static inline void link_unlock(void) {
    __atomic_clear(&s_link_lock, __ATOMIC_RELEASE);
}

static inline bool _link_is_player_xfer_busy(const PS4LinkHub* hub, int i) {
    if (!hub || !__atomic_load_n(&hub->transfer_in_progress, __ATOMIC_ACQUIRE)) {
        return false;
    }
    if (i == 0 && __atomic_load_n(&hub->transfer_just_started, __ATOMIC_ACQUIRE)) {
        return true;
    }
    uint32_t sched = __atomic_load_n(&hub->transfer_scheduled_mask, __ATOMIC_ACQUIRE);
    uint32_t fin = __atomic_load_n(&hub->transfer_finished_mask, __ATOMIC_ACQUIRE);
    return ((sched & ~fin) & (1U << i)) != 0;
}

static inline void _link_hub_request_slice_sync(PS4LinkHub* hub) {
    __atomic_store_n(&hub->slice_interrupt, true, __ATOMIC_RELEASE);
    for (int i = 0; i < hub->active_count; ++i) {
        if (hub->gba[i]) {
            __atomic_store_n(&hub->gba[i]->earlyExit, true, __ATOMIC_RELEASE);
        }
    }
}

static void _link_transfer_event_cb(struct mTiming* timing, void* user, uint32_t cyclesLate);
static void _link_lockstep_event_cb(struct mTiming* timing, void* user, uint32_t cyclesLate);

/*
 * Retorna true se o cabo estiver fisicamente conectado e fora da janela
 * de desconexão temporária do Hotplug.
 */
static inline bool _link_hub_is_physically_linked(const PS4LinkHub* hub) {
    if (!hub || hub->active_count < 2) {
        return false;
    }
    if (!hub->cable_enabled || hub->hotplug_disconnect_frames > 0) {
        return false;
    }
    return true;
}

/*
 * Verifica se o pino SD (Bit 3 de SIOCNT / Bit 1 de RCNT = All GBAs Ready)
 * deve estar em nível lógico alto (1).
 * Permanece em 1 quando todas as instâncias ativas estão em GBA_SIO_MULTI ou
 * dentro da janela de tolerância de transição GPIO/Baud Rate (como ocorre no
 * Pokémon Quetzal / Emerald e Mario Kart: Super Circuit).
 */
static bool _link_hub_is_sd_ready(const PS4LinkHub* hub) {
    if (!_link_hub_is_physically_linked(hub)) {
        return false;
    }

    int ready_count = 0;
    for (int i = 0; i < hub->active_count; ++i) {
        struct GBA* g = hub->gba[i];
        if (!g) {
            return false;
        }
        if (g->sio.mode == GBA_SIO_MULTI ||
            (hub->drivers[i].has_entered_multi && hub->drivers[i].gpio_grace_frames > 0)) {
            ready_count++;
        }
    }

    return (ready_count == hub->active_count);
}

/*
 * Aborta de forma limpa qualquer transferência em andamento (ex: quando o
 * Mestre muda o modo de SIO escrevendo RCNT = 0x8000 antes do fim dos ciclos
 * da transferência). Isso impede que `transfer_in_progress` ou o bit Busy (bit 7)
 * fiquem travados em 1 permanentemente.
 */
static void _link_hub_abort_transfer_ex(PS4LinkHub* hub, bool workers_stopped) {
    if (!hub) {
        return;
    }

    __atomic_store_n(&hub->transfer_in_progress, false, __ATOMIC_RELEASE);
    __atomic_store_n(&hub->transfer_just_started, false, __ATOMIC_RELEASE);
    __atomic_store_n(&hub->transfer_data_latched, false, __ATOMIC_RELEASE);
    __atomic_store_n(&hub->transfer_scheduled_mask, 0, __ATOMIC_RELEASE);
    __atomic_store_n(&hub->transfer_finished_mask, 0, __ATOMIC_RELEASE);

    for (int i = 0; i < hub->active_count; ++i) {
        struct GBA* g = hub->gba[i];
        if (!g) {
            continue;
        }
        if (workers_stopped || i == 0) {
            mTimingDeschedule(&g->timing, &hub->drivers[i].transfer_event);
            mTimingDeschedule(&g->timing, &g->sio.completeEvent);
        }

        if (g->sio.mode == GBA_SIO_MULTI) {
            g->sio.siocnt = GBASIOMultiplayerClearBusy(g->sio.siocnt);
            g->memory.io[GBA_REG(SIOCNT)] = g->sio.siocnt;
        } else if (g->sio.mode == GBA_SIO_NORMAL_8 || g->sio.mode == GBA_SIO_NORMAL_32) {
            g->sio.siocnt = GBASIONormalClearStart(g->sio.siocnt);
            g->memory.io[GBA_REG(SIOCNT)] = g->sio.siocnt;
        }

        if (g->sio.mode != GBA_SIO_GPIO) {
            g->sio.rcnt = GBASIORegisterRCNTFillSc(g->sio.rcnt);
            g->memory.io[GBA_REG(RCNT)] = g->sio.rcnt;
        }
    }
}

static void _link_hub_abort_transfer(PS4LinkHub* hub) {
    _link_hub_abort_transfer_ex(hub, true);
}

/*
 * Propaga os níveis elétricos quando o cabo está DESCONECTADO:
 * - SI = 1 (Pull-up flutuante em todos os consoles)
 * - SD = 0 (Sem link ready)
 * - ID = 0
 */
static void _link_hub_apply_disconnected_state(PS4LinkHub* hub) {
    if (!hub) {
        return;
    }

    _link_hub_abort_transfer(hub);

    for (int i = 0; i < hub->active_count; ++i) {
        struct GBA* g = hub->gba[i];
        if (!g) {
            continue;
        }

        if (g->sio.mode != GBA_SIO_GPIO) {
            uint16_t rcnt = g->sio.rcnt;
            rcnt = GBASIORegisterRCNTFillSc(rcnt);
            rcnt = GBASIORegisterRCNTSetSi(rcnt, 1);
            rcnt = GBASIORegisterRCNTSetSd(rcnt, 0);
            g->sio.rcnt = rcnt;
            g->memory.io[GBA_REG(RCNT)] = rcnt;
        }

        if (g->sio.mode == GBA_SIO_MULTI) {
            uint16_t scnt = g->sio.siocnt;
            scnt = GBASIOMultiplayerClearReady(scnt);
            scnt = GBASIOMultiplayerSetSlave(scnt, 1);
            scnt = GBASIOMultiplayerSetId(scnt, 0);
            scnt = GBASIOMultiplayerClearBusy(scnt);
            g->sio.siocnt = scnt;
            g->memory.io[GBA_REG(SIOCNT)] = scnt;
        }
    }
}

/*
 * Propaga a cadeia GPIO (Daisy-Chain) quando os consoles estão em modo RCNT GPIO (0x8000):
 * - Mestre (Player 0) tem SI aterrado em GND (0) pelo conector físico do cabo.
 * - Escravos (Players 1..3) leem no seu pino SI (bit 2) o estado do pino SO (bit 3) do console anterior.
 */
static void _link_hub_sync_gpio_chain(PS4LinkHub* hub) {
    if (!_link_hub_is_physically_linked(hub)) {
        _link_hub_apply_disconnected_state(hub);
        return;
    }

    for (int i = 0; i < hub->active_count; ++i) {
        struct GBA* g = hub->gba[i];
        if (!g) {
            continue;
        }

        int si_level = 0;
        if (i > 0) {
            struct GBA* prev = hub->gba[i - 1];
            if (prev) {
                uint16_t prev_rcnt = prev->sio.rcnt;
                if (prev->sio.mode == GBA_SIO_GPIO) {
                    /* Se SO está configurado como saída (bit 7=1), propaga bit 3 (SO) */
                    if (prev_rcnt & 0x0080) {
                        si_level = (prev_rcnt & 0x0008) ? 1 : 0;
                    } else {
                        si_level = 0;
                    }
                } else {
                    si_level = 1;
                }
            }
        }

        if (g->sio.mode == GBA_SIO_GPIO) {
            if (si_level) {
                g->sio.rcnt |= 0x0004;
            } else {
                g->sio.rcnt &= ~0x0004;
            }
            /* Bit 1 (SD) e Bit 0 (SC) em nível alto quando fisicamente conectado */
            g->sio.rcnt |= 0x0003;
            g->memory.io[GBA_REG(RCNT)] = g->sio.rcnt;
        }
    }
}

/*
 * Sincroniza os pinos de status (Ready/SD, Slave/SI, ID, SC) nos registradores
 * `sio.siocnt`, `sio.rcnt` e nos espelhos `memory.io[GBA_REG(SIOCNT)]` e
 * `memory.io[GBA_REG(RCNT)]`.
 * Não força Busy=1 arbitrariamente: o bit Busy é controlado exclusivamente pelo
 * agendamento real de `transfer_event` no `mTiming` de cada GBA.
 */
static void _link_hub_refresh_multi_status(PS4LinkHub* hub) {
    if (!hub) {
        return;
    }

    if (!_link_hub_is_physically_linked(hub)) {
        _link_hub_apply_disconnected_state(hub);
        return;
    }

    bool sd_ready = _link_hub_is_sd_ready(hub);

    for (int i = 0; i < hub->active_count; ++i) {
        struct GBA* g = hub->gba[i];
        if (!g) {
            continue;
        }

        bool is_xfer_scheduled = _link_is_player_xfer_busy(hub, i);

        if (g->sio.mode == GBA_SIO_MULTI) {
            hub->drivers[i].has_entered_multi = true;
            hub->drivers[i].gpio_grace_frames = LINK_GPIO_GRACE_FRAMES;
        }

        if (g->sio.mode != GBA_SIO_GPIO) {
            uint16_t rcnt = g->sio.rcnt;
            if (g->sio.mode == GBA_SIO_MULTI && is_xfer_scheduled) {
                rcnt = GBASIORegisterRCNTClearSc(rcnt);
            } else {
                rcnt = GBASIORegisterRCNTFillSc(rcnt);
            }
            rcnt = GBASIORegisterRCNTSetSi(rcnt, i != 0);
            rcnt = GBASIORegisterRCNTSetSd(rcnt, sd_ready ? 1 : 0);
            g->sio.rcnt = rcnt;
            g->memory.io[GBA_REG(RCNT)] = rcnt;
        }

        if (g->sio.mode == GBA_SIO_MULTI) {
            uint16_t scnt = g->sio.siocnt;
            if (sd_ready) {
                scnt = GBASIOMultiplayerFillReady(scnt);
            } else {
                scnt = GBASIOMultiplayerClearReady(scnt);
            }
            scnt = GBASIOMultiplayerSetSlave(scnt, i != 0);
            scnt = GBASIOMultiplayerSetId(scnt, i);
            scnt = GBASIOMultiplayerClearError(scnt);

            if (is_xfer_scheduled) {
                scnt = GBASIOMultiplayerFillBusy(scnt);
            } else {
                scnt = GBASIOMultiplayerClearBusy(scnt);
            }

            g->sio.siocnt = scnt;
            g->memory.io[GBA_REG(SIOCNT)] = scnt;
        } else if (g->sio.mode == GBA_SIO_NORMAL_8 || g->sio.mode == GBA_SIO_NORMAL_32) {
            uint16_t scnt = g->sio.siocnt;
            scnt = GBASIONormalSetSi(scnt, i != 0 ? 1 : 0);
            if (!is_xfer_scheduled) {
                scnt = GBASIONormalClearStart(scnt);
            }
            g->sio.siocnt = scnt;
            g->memory.io[GBA_REG(SIOCNT)] = scnt;
        }
    }
}

/*
 * Captura os dados de saída de todas as instâncias ativas uma única vez por
 * transferência quando os ciclos da linha serial se completam.
 */
static void _link_hub_latch_transfer_data(PS4LinkHub* hub) {
    if (!hub || __atomic_load_n(&hub->transfer_data_latched, __ATOMIC_ACQUIRE)) {
        return;
    }
    __atomic_store_n(&hub->transfer_data_latched, true, __ATOMIC_RELEASE);

    if (hub->transfer_mode == GBA_SIO_MULTI) {
        for (int i = 0; i < LINK_MAX_PLAYERS; ++i) {
            if (i < hub->active_count && hub->gba[i] &&
                (hub->gba[i]->sio.mode == GBA_SIO_MULTI || hub->drivers[i].has_entered_multi)) {
                hub->multi_data[i] = hub->gba[i]->memory.io[GBA_REG(SIOMLT_SEND)];
            } else {
                hub->multi_data[i] = 0xFFFFU;
            }
        }
    } else if (hub->transfer_mode == GBA_SIO_NORMAL_8) {
        for (int i = 0; i < LINK_MAX_PLAYERS; ++i) {
            if (i < hub->active_count && hub->gba[i]) {
                hub->normal8_data[i] = (uint8_t)(hub->gba[i]->memory.io[GBA_REG(SIODATA8)] & 0xFFU);
            } else {
                hub->normal8_data[i] = 0xFFU;
            }
        }
    } else if (hub->transfer_mode == GBA_SIO_NORMAL_32) {
        for (int i = 0; i < LINK_MAX_PLAYERS; ++i) {
            if (i < hub->active_count && hub->gba[i]) {
                hub->normal32_data[i] =
                    (uint32_t)hub->gba[i]->memory.io[GBA_REG(SIODATA32_LO)] |
                    ((uint32_t)hub->gba[i]->memory.io[GBA_REG(SIODATA32_HI)] << 16);
            } else {
                hub->normal32_data[i] = 0xFFFFFFFFU;
            }
        }
    }

    hub->transfer_count++;
    __atomic_store_n(&hub->active_transfer_lines, 16, __ATOMIC_RELEASE);
}

void link_hub_latch_transfer_data(void) {
    link_lock();
    _link_hub_latch_transfer_data(&g_link_hub);
    link_unlock();
}

/*
 * Callback de evento `mTiming` disparado individualmente em CADA GBA (Mestre e Escravos)
 * no ciclo exato de término da transferência serial (`GBASIOTransferCycles`).
 */
static void _link_transfer_event_cb(struct mTiming* timing, void* user, uint32_t cyclesLate) {
    (void)timing;
    PS4LinkDriver* self = (PS4LinkDriver*)user;
    if (!self || !self->hub || !self->d.p || !self->d.p->p) {
        return;
    }

    PS4LinkHub* hub = self->hub;
    struct GBASIO* sio = self->d.p;
    struct GBA* gba = sio->p;
    int id = self->player_id;

    link_lock();
    if (!_link_hub_is_physically_linked(hub)) {
        _link_hub_abort_transfer_ex(hub, false);
        link_unlock();
        return;
    }

    if (!__atomic_load_n(&hub->transfer_in_progress, __ATOMIC_ACQUIRE) ||
        !(__atomic_load_n(&hub->transfer_scheduled_mask, __ATOMIC_ACQUIRE) & (1U << id))) {
        link_unlock();
        return;
    }

    _link_hub_latch_transfer_data(hub);
    bool sd_ready = _link_hub_is_sd_ready(hub);
    enum GBASIOMode xfer_mode = hub->transfer_mode;
    uint16_t local_multi[LINK_MAX_PLAYERS];
    for (int i = 0; i < LINK_MAX_PLAYERS; ++i) {
        local_multi[i] = hub->multi_data[i];
    }
    uint8_t local_norm8 = (id == 0) ? hub->normal8_data[1] : hub->normal8_data[0];
    uint32_t local_norm32 = (id == 0) ? hub->normal32_data[1] : hub->normal32_data[0];
    link_unlock();

    if (xfer_mode == GBA_SIO_MULTI) {
        if (sio->mode == GBA_SIO_MULTI || id == 0) {
            GBASIOMultiplayerFinishTransfer(sio, local_multi, cyclesLate);

            uint16_t scnt = sio->siocnt;
            scnt = GBASIOMultiplayerClearBusy(scnt);
            if (sd_ready) {
                scnt = GBASIOMultiplayerFillReady(scnt);
            } else {
                scnt = GBASIOMultiplayerClearReady(scnt);
            }
            scnt = GBASIOMultiplayerSetSlave(scnt, id != 0);
            scnt = GBASIOMultiplayerSetId(scnt, id);
            scnt = GBASIOMultiplayerClearError(scnt);
            sio->siocnt = scnt;
            gba->memory.io[GBA_REG(SIOCNT)] = scnt;

            uint16_t rcnt = sio->rcnt;
            rcnt = GBASIORegisterRCNTFillSc(rcnt);
            rcnt = GBASIORegisterRCNTSetSi(rcnt, id != 0);
            rcnt = GBASIORegisterRCNTSetSd(rcnt, sd_ready ? 1 : 0);
            sio->rcnt = rcnt;
            gba->memory.io[GBA_REG(RCNT)] = rcnt;
        }
    } else if (xfer_mode == GBA_SIO_NORMAL_8) {
        if (id == 0 || id == 1) {
            if (sio->mode == GBA_SIO_NORMAL_8 || id == 0) {
                GBASIONormal8FinishTransfer(sio, local_norm8, cyclesLate);
                sio->siocnt = GBASIONormalSetSi(sio->siocnt, id != 0 ? 1 : 0);
                gba->memory.io[GBA_REG(SIOCNT)] = sio->siocnt;
            }
        }
    } else if (xfer_mode == GBA_SIO_NORMAL_32) {
        if (id == 0 || id == 1) {
            if (sio->mode == GBA_SIO_NORMAL_32 || id == 0) {
                GBASIONormal32FinishTransfer(sio, local_norm32, cyclesLate);
                sio->siocnt = GBASIONormalSetSi(sio->siocnt, id != 0 ? 1 : 0);
                gba->memory.io[GBA_REG(SIOCNT)] = sio->siocnt;
            }
        }
    }

    uint32_t fin = __atomic_or_fetch(&hub->transfer_finished_mask, (1U << id), __ATOMIC_ACQ_REL);
    uint32_t sched = __atomic_load_n(&hub->transfer_scheduled_mask, __ATOMIC_ACQUIRE);
    if ((fin & sched) == sched) {
        __atomic_store_n(&hub->transfer_in_progress, false, __ATOMIC_RELEASE);
    }
}

/*
 * Callback de evento `mTiming` para limitar o avanço da fatia de ciclos (slice),
 * garantindo que CPUs em `Halt` / `IntrWait` / `VBlankIntrWait` saiam de
 * `GBAProcessEvents` exatamente no limite da fatia (`earlyExit = true`) em vez
 * de avançarem até o VBlank.
 */
static void _link_lockstep_event_cb(struct mTiming* timing, void* user, uint32_t cyclesLate) {
    (void)timing;
    (void)cyclesLate;
    struct GBA* gba = (struct GBA*)user;
    if (gba) {
        GBAInterrupt(gba);
    }
}

/* --- Callbacks de GBASIODriver --- */

static bool _link_driver_init(struct GBASIODriver* driver) {
    PS4LinkDriver* self = (PS4LinkDriver*)driver;
    if (driver->p) {
        bool sd_ready = _link_hub_is_sd_ready(self->hub);
        driver->p->rcnt = GBASIORegisterRCNTFillSc(driver->p->rcnt);
        driver->p->rcnt = GBASIORegisterRCNTSetSi(driver->p->rcnt, self->player_id != 0);
        driver->p->rcnt = GBASIORegisterRCNTSetSd(driver->p->rcnt, sd_ready ? 1 : 0);
    }
    return true;
}

static void _link_driver_deinit(struct GBASIODriver* driver) {
    PS4LinkDriver* self = (PS4LinkDriver*)driver;
    if (self && driver->p && driver->p->p) {
        struct GBA* gba = driver->p->p;
        mTimingDeschedule(&gba->timing, &self->transfer_event);
        mTimingDeschedule(&gba->timing, &self->lockstep_event);
    }
}

static void _link_driver_reset(struct GBASIODriver* driver) {
    PS4LinkDriver* self = (PS4LinkDriver*)driver;
    if (!driver->p || !driver->p->p) {
        return;
    }

    struct GBA* gba = driver->p->p;
    mTimingDeschedule(&gba->timing, &self->transfer_event);
    mTimingDeschedule(&gba->timing, &self->lockstep_event);

    if (self->hub && self->player_id == 0 && self->hub->transfer_in_progress) {
        _link_hub_abort_transfer(self->hub);
    }

    gba->memory.io[GBA_REG(SIOMULTI0)] = 0xFFFFU;
    gba->memory.io[GBA_REG(SIOMULTI1)] = 0xFFFFU;
    gba->memory.io[GBA_REG(SIOMULTI2)] = 0xFFFFU;
    gba->memory.io[GBA_REG(SIOMULTI3)] = 0xFFFFU;
    gba->memory.io[GBA_REG(SIOMLT_SEND)] = 0xFFFFU;

    if (self->hub) {
        _link_hub_refresh_multi_status(self->hub);
    }
}

static void _link_driver_set_mode(struct GBASIODriver* driver, enum GBASIOMode mode) {
    PS4LinkDriver* self = (PS4LinkDriver*)driver;
    if (!driver->p || !driver->p->p || !self->hub) {
        return;
    }

    PS4LinkHub* hub = self->hub;

    link_lock();
    /*
     * Idêntico ao `GBASIOLockstepDriverSetMode` do mGBA (`lockstep.c` linha 611):
     * Se o Mestre (Player 0) trocar de modo enquanto uma transferência estiver
     * ativa, abortamos/desagendamos a transferência imediatamente para nunca
     * deixar o estado Busy preso.
     */
    if (hub->transfer_in_progress && self->player_id == 0 && mode != hub->transfer_mode) {
        _link_hub_abort_transfer_ex(hub, false);
    }

    if (mode == GBA_SIO_MULTI) {
        self->has_entered_multi = true;
        self->gpio_grace_frames = LINK_GPIO_GRACE_FRAMES;
    } else if (mode == GBA_SIO_GPIO) {
        _link_hub_sync_gpio_chain(hub);
    }

    _link_hub_refresh_multi_status(hub);
    __atomic_store_n(&hub->active_transfer_lines, 16, __ATOMIC_RELEASE);
    _link_hub_request_slice_sync(hub);
    link_unlock();
}

static bool _link_driver_handles_mode(struct GBASIODriver* driver, enum GBASIOMode mode) {
    (void)driver;
    switch (mode) {
    case GBA_SIO_GPIO:
    case GBA_SIO_MULTI:
    case GBA_SIO_NORMAL_8:
    case GBA_SIO_NORMAL_32:
        return true;
    default:
        return false;
    }
}

static int _link_driver_connected_devices(struct GBASIODriver* driver) {
    PS4LinkDriver* self = (PS4LinkDriver*)driver;
    if (!_link_hub_is_physically_linked(self->hub)) {
        return 0;
    }
    return self->hub->active_count - 1;
}

static int _link_driver_device_id(struct GBASIODriver* driver) {
    PS4LinkDriver* self = (PS4LinkDriver*)driver;
    if (!_link_hub_is_physically_linked(self->hub)) {
        return 0;
    }
    return self->player_id;
}

static uint16_t _link_driver_write_rcnt(struct GBASIODriver* driver, uint16_t value) {
    PS4LinkDriver* self = (PS4LinkDriver*)driver;
    PS4LinkHub* hub = self->hub;

    if (driver->p && driver->p->mode == GBA_SIO_GPIO) {
        link_lock();
        driver->p->rcnt = value;
        if (hub) {
            _link_hub_sync_gpio_chain(hub);
            _link_hub_refresh_multi_status(hub);
            __atomic_store_n(&hub->active_transfer_lines, 16, __ATOMIC_RELEASE);
            _link_hub_request_slice_sync(hub);
        }
        uint16_t ret_rcnt = driver->p->rcnt;
        link_unlock();
        return ret_rcnt;
    }

    /*
     * Nota crítica sobre `GBASIOWriteRCNT` em `mgba/src/gba/sio.c` (linha 132):
     * `sio->rcnt = (sio->driver->writeRCNT(sio->driver, value) & 0x01F0) | (sio->rcnt & 0xC00F);`
     * O `libmgba` descarta os bits 0..3 (SC, SD, SI, SO) do valor retornado por `writeRCNT`
     * e preserva apenas os bits 0..3 que já estavam em `sio->rcnt` (`driver->p->rcnt`)!
     * Portanto, devemos atualizar os bits 0..3 diretamente em `driver->p->rcnt` antes de retornar.
     */
    link_lock();
    if (!_link_hub_is_physically_linked(hub)) {
        value = GBASIORegisterRCNTFillSc(value);
        value = GBASIORegisterRCNTSetSi(value, 1);
        value = GBASIORegisterRCNTSetSd(value, 0);
        if (driver->p) {
            driver->p->rcnt = (driver->p->rcnt & ~0x000FU) | (value & 0x000FU);
        }
        link_unlock();
        return value;
    }

    bool sd_ready = _link_hub_is_sd_ready(hub);
    value = GBASIORegisterRCNTFillSc(value);
    value = GBASIORegisterRCNTSetSi(value, self->player_id != 0);
    value = GBASIORegisterRCNTSetSd(value, sd_ready ? 1 : 0);

    if (driver->p) {
        driver->p->rcnt = (driver->p->rcnt & ~0x000FU) | (value & 0x000FU);
    }

    if (hub) {
        _link_hub_refresh_multi_status(hub);
    }
    link_unlock();
    return value;
}

static uint16_t _link_driver_write_siocnt(struct GBASIODriver* driver, uint16_t value) {
    PS4LinkDriver* self = (PS4LinkDriver*)driver;
    PS4LinkHub* hub = self->hub;
    if (!hub || !driver->p) {
        return value;
    }

    link_lock();
    if (!_link_hub_is_physically_linked(hub)) {
        value = GBASIOMultiplayerClearReady(value);
        value = GBASIOMultiplayerSetSlave(value, 1);
        value = GBASIOMultiplayerSetId(value, 0);
        value = GBASIOMultiplayerClearBusy(value);
        link_unlock();
        return value;
    }

    enum GBASIOMode mode = driver->p->mode;

    /*
     * Se o Mestre (Player 0) acabou de iniciar uma transferência em `_link_driver_start`,
     * agora temos o `value` completo recém-escrito em `SIOCNT` (incluindo os novos bits
     * de Baud Rate 0..1). Calculamos os ciclos exatos da transferência e agendamos o
     * evento de conclusão do Mestre no `mTiming`.
     */
    if (self->player_id == 0 && __atomic_load_n(&hub->transfer_just_started, __ATOMIC_ACQUIRE) && driver->p->p) {
        struct GBA* gba0 = driver->p->p;
        int connected = hub->active_count - 1;
        int32_t cycles = GBASIOTransferCycles(mode, value, connected);
        if (cycles < LINK_MIN_TRANSFER_CYCLES) {
            cycles = LINK_MIN_TRANSFER_CYCLES;
        }
        hub->transfer_cycles = cycles;
        __atomic_store_n(&hub->transfer_scheduled_mask, (1U << 0), __ATOMIC_RELEASE);
        __atomic_store_n(&hub->transfer_finished_mask, 0, __ATOMIC_RELEASE);

        mTimingDeschedule(&gba0->timing, &self->transfer_event);
        mTimingSchedule(&gba0->timing, &self->transfer_event, cycles);

        /* Força saída imediata do `ARMRunLoop` do Mestre e dos Escravos para alinhar no ciclo exato */
        if (gba0->cpu) {
            gba0->cpu->nextEvent = gba0->cpu->cycles;
        }
        GBAInterrupt(gba0);
        _link_hub_request_slice_sync(hub);
    }

    if (mode == GBA_SIO_MULTI) {
        self->has_entered_multi = true;
        self->gpio_grace_frames = LINK_GPIO_GRACE_FRAMES;

        bool sd_ready = _link_hub_is_sd_ready(hub);
        if (sd_ready) {
            value = GBASIOMultiplayerFillReady(value);
        } else {
            value = GBASIOMultiplayerClearReady(value);
        }

        value = GBASIOMultiplayerSetSlave(value, self->player_id != 0);
        value = GBASIOMultiplayerSetId(value, self->player_id);
        value = GBASIOMultiplayerClearError(value);

        bool is_busy = _link_is_player_xfer_busy(hub, self->player_id);
        if (is_busy) {
            value = GBASIOMultiplayerFillBusy(value);
            driver->p->rcnt = GBASIORegisterRCNTClearSc(driver->p->rcnt);
            __atomic_store_n(&hub->active_transfer_lines, 16, __ATOMIC_RELEASE);
        } else {
            value = GBASIOMultiplayerClearBusy(value);
            driver->p->rcnt = GBASIORegisterRCNTFillSc(driver->p->rcnt);
        }

        driver->p->rcnt = GBASIORegisterRCNTSetSi(driver->p->rcnt, self->player_id != 0);
        driver->p->rcnt = GBASIORegisterRCNTSetSd(driver->p->rcnt, sd_ready ? 1 : 0);
        if (driver->p->p) {
            driver->p->p->memory.io[GBA_REG(RCNT)] = driver->p->rcnt;
        }

        link_unlock();
        return value;
    }

    if (mode == GBA_SIO_NORMAL_32 || mode == GBA_SIO_NORMAL_8) {
        value = GBASIONormalSetSi(value, self->player_id != 0 ? 1 : 0);
        bool is_busy = _link_is_player_xfer_busy(hub, self->player_id);
        if (self->player_id != 0 && !is_busy) {
            value = GBASIONormalClearStart(value);
        }
        if (is_busy) {
            __atomic_store_n(&hub->active_transfer_lines, 16, __ATOMIC_RELEASE);
        }
        link_unlock();
        return value;
    }

    link_unlock();
    return value;
}

static bool _link_driver_start(struct GBASIODriver* driver) {
    PS4LinkDriver* self = (PS4LinkDriver*)driver;
    PS4LinkHub* hub = self->hub;
    if (!hub || !driver->p || !driver->p->p || !_link_hub_is_physically_linked(hub)) {
        return false;
    }

    if (self->player_id != 0) {
        return false;
    }

    enum GBASIOMode mode = driver->p->mode;
    if (mode != GBA_SIO_MULTI && mode != GBA_SIO_NORMAL_8 && mode != GBA_SIO_NORMAL_32) {
        return false;
    }

    link_lock();
    if (hub->transfer_in_progress) {
        _link_hub_abort_transfer_ex(hub, false);
    }

    hub->transfer_mode = mode;
    __atomic_store_n(&hub->transfer_in_progress, true, __ATOMIC_RELEASE);
    __atomic_store_n(&hub->transfer_just_started, true, __ATOMIC_RELEASE);
    __atomic_store_n(&hub->transfer_data_latched, false, __ATOMIC_RELEASE);
    __atomic_store_n(&hub->transfer_scheduled_mask, (1U << 0), __ATOMIC_RELEASE);
    __atomic_store_n(&hub->transfer_finished_mask, 0, __ATOMIC_RELEASE);
    __atomic_store_n(&hub->active_transfer_lines, 16, __ATOMIC_RELEASE);
    link_unlock();

    /*
     * Retorna `false` para informar a `_startTransfer` (`mgba/src/gba/sio.c` linha 145)
     * que o agendamento e a conclusão da transferência são gerenciados internamente
     * pelo nosso driver via `self->transfer_event` (com o Baud Rate atualizado).
     */
    return false;
}

static void _link_driver_finish_multiplayer(struct GBASIODriver* driver, uint16_t data[4]) {
    PS4LinkDriver* self = (PS4LinkDriver*)driver;
    PS4LinkHub* hub = self ? self->hub : NULL;
    if (!hub || !_link_hub_is_physically_linked(hub)) {
        data[0] = 0xFFFFU;
        data[1] = 0xFFFFU;
        data[2] = 0xFFFFU;
        data[3] = 0xFFFFU;
        return;
    }

    link_lock();
    _link_hub_latch_transfer_data(hub);
    for (int i = 0; i < LINK_MAX_PLAYERS; ++i) {
        data[i] = hub->multi_data[i];
    }
    link_unlock();
}

static uint8_t _link_driver_finish_normal8(struct GBASIODriver* driver) {
    PS4LinkDriver* self = (PS4LinkDriver*)driver;
    PS4LinkHub* hub = self ? self->hub : NULL;
    if (!hub || !_link_hub_is_physically_linked(hub)) {
        return 0xFFU;
    }

    link_lock();
    _link_hub_latch_transfer_data(hub);
    uint8_t res = (self->player_id == 0) ? hub->normal8_data[1] : hub->normal8_data[0];
    link_unlock();
    return res;
}

static uint32_t _link_driver_finish_normal32(struct GBASIODriver* driver) {
    PS4LinkDriver* self = (PS4LinkDriver*)driver;
    PS4LinkHub* hub = self ? self->hub : NULL;
    if (!hub || !_link_hub_is_physically_linked(hub)) {
        return 0xFFFFFFFFU;
    }

    link_lock();
    _link_hub_latch_transfer_data(hub);
    uint32_t res = (self->player_id == 0) ? hub->normal32_data[1] : hub->normal32_data[0];
    link_unlock();
    return res;
}

/* --- Funções Públicas do Link Hub --- */

/*
 * Equivalente direto ao `SIO_EV_TRANSFER_START` em `mgba/src/gba/sio/lockstep.c` (linhas 965-971):
 * Chamada por `emulator_step_lockstep_frame` na barreira quando todos os núcleos
 * estão sincronizados após o início da transferência pelo Mestre (Player 0).
 * Ativa o bit Busy (0x80) nos Escravos (sem apagar `SIOMULTI0..3` anteriores!)
 * e agenda `transfer_event` no `mTiming` de cada Escravo para disparar no ciclo exato `finish_elapsed`.
 */
void link_hub_begin_transfer_on_slaves(uint32_t master_elapsed, const uint32_t start_cycles[LINK_MAX_PLAYERS]) {
    PS4LinkHub* hub = &g_link_hub;
    if (!__atomic_load_n(&hub->transfer_in_progress, __ATOMIC_ACQUIRE) || !_link_hub_is_physically_linked(hub)) {
        return;
    }

    int32_t cycles = hub->transfer_cycles;
    if (cycles < LINK_MIN_TRANSFER_CYCLES) {
        cycles = LINK_MIN_TRANSFER_CYCLES;
    }

    uint32_t finish_elapsed = master_elapsed + (uint32_t)cycles;

    for (int i = 1; i < hub->active_count; ++i) {
        struct GBA* g = hub->gba[i];
        if (!g) {
            continue;
        }
        uint32_t slave_elapsed = (uint32_t)(mTimingCurrentTime(&g->timing) - start_cycles[i]);
        if (slave_elapsed + 16U > finish_elapsed) {
            finish_elapsed = slave_elapsed + 32U;
        }
    }

    if (finish_elapsed > master_elapsed + (uint32_t)cycles && hub->gba[0]) {
        int32_t master_rem = (int32_t)(finish_elapsed - master_elapsed);
        mTimingDeschedule(&hub->gba[0]->timing, &hub->drivers[0].transfer_event);
        mTimingSchedule(&hub->gba[0]->timing, &hub->drivers[0].transfer_event, master_rem);
    }

    hub->transfer_finish_elapsed = finish_elapsed;
    bool sd_ready = _link_hub_is_sd_ready(hub);

    for (int i = 0; i < hub->active_count; ++i) {
        if (hub->gba[i]) {
            hub->pending_multi_send[i] = hub->gba[i]->memory.io[GBA_REG(SIOMLT_SEND)];
        }
    }

    uint32_t sched_mask = (1U << 0);

    for (int i = 1; i < hub->active_count; ++i) {
        struct GBA* g = hub->gba[i];
        if (!g) {
            continue;
        }

        uint32_t slave_elapsed = (uint32_t)(mTimingCurrentTime(&g->timing) - start_cycles[i]);
        int32_t slave_cycles = (int32_t)(finish_elapsed - slave_elapsed);
        if (slave_cycles < 16) {
            slave_cycles = 16;
        }

        if (hub->transfer_mode == GBA_SIO_MULTI) {
            if (g->sio.mode == GBA_SIO_MULTI) {
                uint16_t scnt = g->sio.siocnt;
                scnt = GBASIOMultiplayerFillBusy(scnt);
                if (sd_ready) {
                    scnt = GBASIOMultiplayerFillReady(scnt);
                } else {
                    scnt = GBASIOMultiplayerClearReady(scnt);
                }
                scnt = GBASIOMultiplayerSetSlave(scnt, 1);
                scnt = GBASIOMultiplayerSetId(scnt, i);
                scnt = GBASIOMultiplayerClearError(scnt);
                g->sio.siocnt = scnt;
                g->memory.io[GBA_REG(SIOCNT)] = scnt;

                g->sio.rcnt = GBASIORegisterRCNTClearSc(g->sio.rcnt);
                g->sio.rcnt = GBASIORegisterRCNTSetSi(g->sio.rcnt, 1);
                g->sio.rcnt = GBASIORegisterRCNTSetSd(g->sio.rcnt, sd_ready ? 1 : 0);
                g->memory.io[GBA_REG(RCNT)] = g->sio.rcnt;
            }

            sched_mask |= (1U << i);
            mTimingDeschedule(&g->timing, &hub->drivers[i].transfer_event);
            mTimingSchedule(&g->timing, &hub->drivers[i].transfer_event, slave_cycles);
        } else if ((hub->transfer_mode == GBA_SIO_NORMAL_8 || hub->transfer_mode == GBA_SIO_NORMAL_32) && i == 1) {
            sched_mask |= (1U << 1);
            mTimingDeschedule(&g->timing, &hub->drivers[1].transfer_event);
            mTimingSchedule(&g->timing, &hub->drivers[1].transfer_event, slave_cycles);
        }
    }

    __atomic_store_n(&hub->transfer_scheduled_mask, sched_mask, __ATOMIC_RELEASE);
}

void link_hub_init(void) {
    memset(&g_link_hub, 0, sizeof(g_link_hub));
    g_link_hub.cable_enabled = true;
    g_link_hub.hotplug_disconnect_frames = 0;
    g_link_hub.transfer_mode = GBA_SIO_GPIO;
    for (int i = 0; i < LINK_MAX_PLAYERS; ++i) {
        g_link_hub.multi_data[i] = 0xFFFFU;
        g_link_hub.pending_multi_send[i] = 0xFFFFU;
        g_link_hub.normal8_data[i] = 0xFFU;
        g_link_hub.normal32_data[i] = 0xFFFFFFFFU;
    }
}

static void _setup_driver_callbacks(PS4LinkDriver* drv, int player_id, struct GBA* gba) {
    memset(drv, 0, sizeof(*drv));
    drv->hub = &g_link_hub;
    drv->player_id = player_id;

    drv->transfer_event.name = "PS4 GBA SIO Transfer Complete";
    drv->transfer_event.callback = _link_transfer_event_cb;
    drv->transfer_event.context = drv;
    drv->transfer_event.priority = 0x80;

    drv->lockstep_event.name = "PS4 GBA Lockstep Slice";
    drv->lockstep_event.callback = _link_lockstep_event_cb;
    drv->lockstep_event.context = gba;
    drv->lockstep_event.priority = 0x81;

    drv->d.init = _link_driver_init;
    drv->d.deinit = _link_driver_deinit;
    drv->d.reset = _link_driver_reset;
    drv->d.setMode = _link_driver_set_mode;
    drv->d.handlesMode = _link_driver_handles_mode;
    drv->d.connectedDevices = _link_driver_connected_devices;
    drv->d.deviceId = _link_driver_device_id;
    drv->d.writeSIOCNT = _link_driver_write_siocnt;
    drv->d.writeRCNT = _link_driver_write_rcnt;
    drv->d.start = _link_driver_start;
    drv->d.finishMultiplayer = _link_driver_finish_multiplayer;
    drv->d.finishNormal8 = _link_driver_finish_normal8;
    drv->d.finishNormal32 = _link_driver_finish_normal32;
}

bool link_hub_attach(struct mCore* cores[], int num_cores) {
    if (!cores || num_cores <= 0) {
        return false;
    }
    if (num_cores > LINK_MAX_PLAYERS) {
        num_cores = LINK_MAX_PLAYERS;
    }

    link_hub_init();
    g_link_hub.active_count = num_cores;

    for (int i = 0; i < num_cores; ++i) {
        if (!cores[i] || !cores[i]->board) {
            continue;
        }

        struct GBA* gba = (struct GBA*)cores[i]->board;
        PS4LinkDriver* drv = &g_link_hub.drivers[i];

        _setup_driver_callbacks(drv, i, gba);
        g_link_hub.gba[i] = gba;
        GBASIOSetDriver(&gba->sio, &drv->d);
    }

    link_hub_reset();
    return true;
}

bool link_hub_attach_instance(int player_id, struct mCore* core) {
    if (player_id < 0 || player_id >= LINK_MAX_PLAYERS || !core || !core->board) {
        return false;
    }

    struct GBA* gba = (struct GBA*)core->board;
    PS4LinkDriver* drv = &g_link_hub.drivers[player_id];

    _setup_driver_callbacks(drv, player_id, gba);

    g_link_hub.gba[player_id] = gba;
    if (player_id >= g_link_hub.active_count) {
        g_link_hub.active_count = player_id + 1;
    }

    GBASIOSetDriver(&gba->sio, &drv->d);
    _link_driver_reset(&drv->d);
    return true;
}

void link_hub_reset(void) {
    _link_hub_abort_transfer(&g_link_hub);
    g_link_hub.active_transfer_lines = 16;
    for (int i = 0; i < LINK_MAX_PLAYERS; ++i) {
        if (g_link_hub.gba[i]) {
            _link_driver_reset(&g_link_hub.drivers[i].d);
            _link_driver_set_mode(&g_link_hub.drivers[i].d, g_link_hub.gba[i]->sio.mode);
        }
    }
}

void link_hub_set_cable_enabled(bool enabled) {
    g_link_hub.cable_enabled = enabled;
    g_link_hub.hotplug_disconnect_frames = 0;
    if (!enabled) {
        _link_hub_apply_disconnected_state(&g_link_hub);
    } else {
        _link_hub_refresh_multi_status(&g_link_hub);
    }
}

bool link_hub_is_cable_enabled(void) {
    return g_link_hub.cable_enabled;
}

/*
 * Simula o ato físico de desplugar o Link Cable por 20 frames (~0.33s) e
 * plugá-lo novamente, forçando todos os jogos (como Mario Kart e Pokémon)
 * a detectarem a transição elétrica de conexão.
 */
void link_hub_trigger_hotplug(void) {
    g_link_hub.cable_enabled = true;
    g_link_hub.hotplug_disconnect_frames = 20;
    for (int i = 0; i < LINK_MAX_PLAYERS; ++i) {
        g_link_hub.drivers[i].has_entered_multi = true;
        g_link_hub.drivers[i].gpio_grace_frames = LINK_GPIO_GRACE_FRAMES;
    }
    _link_hub_apply_disconnected_state(&g_link_hub);
}

bool link_hub_is_hotplugging(void) {
    return (g_link_hub.hotplug_disconnect_frames > 0);
}

void link_hub_begin_frame(void) {
    __atomic_store_n(&g_link_hub.slice_interrupt, false, __ATOMIC_RELEASE);
    if (g_link_hub.hotplug_disconnect_frames > 0) {
        g_link_hub.hotplug_disconnect_frames--;
        if (g_link_hub.hotplug_disconnect_frames == 0) {
            for (int i = 0; i < g_link_hub.active_count; ++i) {
                g_link_hub.drivers[i].has_entered_multi = true;
                g_link_hub.drivers[i].gpio_grace_frames = LINK_GPIO_GRACE_FRAMES;
            }
            link_hub_reset();
        } else {
            _link_hub_apply_disconnected_state(&g_link_hub);
            return;
        }
    }

    for (int i = 0; i < g_link_hub.active_count; ++i) {
        struct GBA* g = g_link_hub.gba[i];
        if (!g) {
            continue;
        }
        if (g->sio.mode == GBA_SIO_MULTI) {
            g_link_hub.drivers[i].has_entered_multi = true;
            g_link_hub.drivers[i].gpio_grace_frames = LINK_GPIO_GRACE_FRAMES;
        } else if (g_link_hub.drivers[i].gpio_grace_frames > 0) {
            g_link_hub.drivers[i].gpio_grace_frames--;
            if (g_link_hub.drivers[i].gpio_grace_frames == 0) {
                g_link_hub.drivers[i].has_entered_multi = false;
            }
        }
    }
    _link_hub_refresh_multi_status(&g_link_hub);
}

void link_hub_sync_pins(void) {
    _link_hub_refresh_multi_status(&g_link_hub);
}

void link_hub_destroy(void) {
    _link_hub_abort_transfer(&g_link_hub);
    for (int i = 0; i < LINK_MAX_PLAYERS; ++i) {
        if (g_link_hub.gba[i]) {
            mTimingDeschedule(&g_link_hub.gba[i]->timing, &g_link_hub.drivers[i].transfer_event);
            mTimingDeschedule(&g_link_hub.gba[i]->timing, &g_link_hub.drivers[i].lockstep_event);
            GBASIOSetDriver(&g_link_hub.gba[i]->sio, NULL);
            g_link_hub.gba[i] = NULL;
        }
    }
    memset(&g_link_hub, 0, sizeof(g_link_hub));
}

bool link_hub_is_transfer_active(void) {
    if (__atomic_load_n(&g_link_hub.transfer_in_progress, __ATOMIC_ACQUIRE)) {
        return true;
    }
    return (__atomic_load_n(&g_link_hub.active_transfer_lines, __ATOMIC_ACQUIRE) > 0);
}

bool link_hub_consume_just_started(void) {
    return __atomic_exchange_n(&g_link_hub.transfer_just_started, false, __ATOMIC_ACQ_REL);
}

void link_hub_notify_scanline(void) {
    int lines = __atomic_load_n(&g_link_hub.active_transfer_lines, __ATOMIC_RELAXED);
    if (lines > 0) {
        __atomic_sub_fetch(&g_link_hub.active_transfer_lines, 1, __ATOMIC_RELEASE);
    }
}

uint32_t link_hub_get_transfer_count(void) {
    return g_link_hub.transfer_count;
}

int link_hub_get_player_mode(int player_id) {
    if (player_id < 0 || player_id >= LINK_MAX_PLAYERS || !g_link_hub.gba[player_id]) {
        return -1;
    }
    return (int)g_link_hub.gba[player_id]->sio.mode;
}