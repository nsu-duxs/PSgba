#include "link_cable_hub.h"
#include <string.h>
#include <mgba/internal/gba/gba.h>
#include <mgba/internal/gba/io.h>

PS4LinkHub g_link_hub = {0};

/*
 * Verifica se TODAS as instâncias ativas já entraram no modo GBA_SIO_MULTI.
 * No hardware real do GBA, a linha SD (Bit 3 de SIOCNT - "All GBAs Ready")
 * só fica em nível alto (1) quando todos os consoles conectados na cadeia
 * configuraram RCNT com bit 15 = 0 e SIOCNT com bits 12..13 = 10 (Multi).
 */
static bool _link_hub_all_in_multi(const PS4LinkHub* hub) {
    if (!hub || hub->active_count < 2) {
        return false;
    }
    for (int i = 0; i < hub->active_count; ++i) {
        struct GBA* g = hub->gba[i];
        if (!g || g->sio.mode != GBA_SIO_MULTI) {
            return false;
        }
    }
    return true;
}

/*
 * Verifica se ao menos 2 instâncias estão em modos compatíveis de Link.
 */
static bool _link_hub_has_peers(const PS4LinkHub* hub) {
    return (hub && hub->active_count >= 2);
}

/*
 * Propaga a cadeia GPIO (Daisy-Chain) quando os consoles estão em modo RCNT GPIO (0x8000):
 * O pino SO (Bit 3) do Jogador (i - 1) alimenta o pino SI (Bit 2) do Jogador (i).
 * O Jogador 0 (Master) tem seu pino SI permanentemente em GND (0).
 */
static void _link_hub_sync_gpio_chain(PS4LinkHub* hub) {
    if (!hub || hub->active_count < 2) {
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
                /* Se o console anterior estiver em GPIO e tiver SO como saída/alto, propaga para SI */
                uint16_t prev_rcnt = prev->sio.rcnt;
                if (prev->sio.mode == GBA_SIO_GPIO) {
                    si_level = (prev_rcnt & 0x0008) ? 1 : 0;
                } else {
                    si_level = 1;
                }
            } else {
                si_level = 1;
            }
        }

        if (g->sio.mode == GBA_SIO_GPIO) {
            if (si_level) {
                g->sio.rcnt |= 0x0004;
            } else {
                g->sio.rcnt &= ~0x0004;
            }
            g->memory.io[GBA_REG(RCNT)] = g->sio.rcnt;
        }
    }
}

/*
 * Sincroniza o estado dos pinos SI, SD e Player ID de todas as instâncias
 * que estão atualmente em modo GBA_SIO_MULTI.
 */
static void _link_hub_refresh_multi_status(PS4LinkHub* hub) {
    if (!hub) {
        return;
    }

    bool all_multi = _link_hub_all_in_multi(hub);

    for (int i = 0; i < hub->active_count; ++i) {
        struct GBA* g = hub->gba[i];
        if (!g) {
            continue;
        }

        /* Atualiza RCNT: SC=1, SD=(all_multi), SI=(i != 0) */
        uint16_t rcnt = g->sio.rcnt;
        if (g->sio.mode != GBA_SIO_GPIO) {
            rcnt = GBASIORegisterRCNTFillSc(rcnt);
            rcnt = GBASIORegisterRCNTSetSi(rcnt, i != 0);
            rcnt = GBASIORegisterRCNTSetSd(rcnt, all_multi ? 1 : 0);
            g->sio.rcnt = rcnt;
            g->memory.io[GBA_REG(RCNT)] = rcnt;
        }

        if (g->sio.mode == GBA_SIO_MULTI) {
            uint16_t scnt = g->sio.siocnt;
            if (all_multi) {
                scnt = GBASIOMultiplayerFillReady(scnt); /* Bit 3 (SD) = 1 */
            } else {
                scnt = GBASIOMultiplayerClearReady(scnt); /* Bit 3 (SD) = 0 */
            }
            scnt = GBASIOMultiplayerSetSlave(scnt, i != 0); /* Bit 2 (SI): 0=Master, 1=Slave */
            scnt = GBASIOMultiplayerSetId(scnt, i);         /* Bits 4..5: ID 0..3 */
            scnt = GBASIOMultiplayerClearError(scnt);       /* Bit 6 (Error) = 0 */

            if (hub->transfer_in_progress) {
                scnt = GBASIOMultiplayerFillBusy(scnt);
            }

            g->sio.siocnt = scnt;
            g->memory.io[GBA_REG(SIOCNT)] = scnt;
        }
    }
}

/* --- Callbacks de GBASIODriver --- */

static bool _link_driver_init(struct GBASIODriver* driver) {
    PS4LinkDriver* self = (PS4LinkDriver*)driver;
    if (driver->p) {
        bool all_multi = _link_hub_all_in_multi(self->hub);
        driver->p->rcnt = GBASIORegisterRCNTFillSc(driver->p->rcnt);
        driver->p->rcnt = GBASIORegisterRCNTSetSi(driver->p->rcnt, self->player_id != 0);
        driver->p->rcnt = GBASIORegisterRCNTSetSd(driver->p->rcnt, all_multi ? 1 : 0);
    }
    return true;
}

static void _link_driver_deinit(struct GBASIODriver* driver) {
    (void)driver;
}

static void _link_driver_reset(struct GBASIODriver* driver) {
    PS4LinkDriver* self = (PS4LinkDriver*)driver;
    if (!driver->p || !driver->p->p) {
        return;
    }

    struct GBA* gba = driver->p->p;

    gba->memory.io[GBA_REG(SIOMULTI0)] = 0xFFFF;
    gba->memory.io[GBA_REG(SIOMULTI1)] = 0xFFFF;
    gba->memory.io[GBA_REG(SIOMULTI2)] = 0xFFFF;
    gba->memory.io[GBA_REG(SIOMULTI3)] = 0xFFFF;
    gba->memory.io[GBA_REG(SIOMLT_SEND)] = 0xFFFF;

    if (self->hub) {
        _link_hub_refresh_multi_status(self->hub);
    }
}

static void _link_driver_set_mode(struct GBASIODriver* driver, enum GBASIOMode mode) {
    PS4LinkDriver* self = (PS4LinkDriver*)driver;
    if (!driver->p || !driver->p->p || !self->hub) {
        return;
    }

    if (mode == GBA_SIO_GPIO) {
        _link_hub_sync_gpio_chain(self->hub);
    }
    /* Sempre que qualquer GBA muda de modo, atualiza o bit SD (All Ready) de todos */
    _link_hub_refresh_multi_status(self->hub);
    __atomic_store_n(&self->hub->active_transfer_lines, 8, __ATOMIC_RELEASE);
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
    if (!self->hub || self->hub->active_count <= 1) {
        return 0;
    }
    return self->hub->active_count - 1;
}

static int _link_driver_device_id(struct GBASIODriver* driver) {
    PS4LinkDriver* self = (PS4LinkDriver*)driver;
    return self->player_id;
}

static uint16_t _link_driver_write_rcnt(struct GBASIODriver* driver, uint16_t value) {
    PS4LinkDriver* self = (PS4LinkDriver*)driver;
    PS4LinkHub* hub = self->hub;

    if (driver->p && driver->p->mode == GBA_SIO_GPIO) {
        driver->p->rcnt = value;
        if (hub) {
            _link_hub_sync_gpio_chain(hub);
            __atomic_store_n(&hub->active_transfer_lines, 8, __ATOMIC_RELEASE);
        }
        return driver->p->rcnt;
    }

    bool all_multi = _link_hub_all_in_multi(hub);
    value = GBASIORegisterRCNTFillSc(value);
    value = GBASIORegisterRCNTSetSi(value, self->player_id != 0);
    value = GBASIORegisterRCNTSetSd(value, all_multi ? 1 : 0);

    if (hub) {
        _link_hub_refresh_multi_status(hub);
    }
    return value;
}

static uint16_t _link_driver_write_siocnt(struct GBASIODriver* driver, uint16_t value) {
    PS4LinkDriver* self = (PS4LinkDriver*)driver;
    PS4LinkHub* hub = self->hub;
    if (!hub || !driver->p) {
        return value;
    }

    enum GBASIOMode mode = driver->p->mode;

    if (mode == GBA_SIO_MULTI) {
        bool all_multi = _link_hub_all_in_multi(hub);
        if (all_multi) {
            value = GBASIOMultiplayerFillReady(value);
        } else {
            value = GBASIOMultiplayerClearReady(value);
        }

        value = GBASIOMultiplayerSetSlave(value, self->player_id != 0);
        value = GBASIOMultiplayerSetId(value, self->player_id);
        value = GBASIOMultiplayerClearError(value);

        if (self->player_id != 0) {
            /* Slaves não iniciam o clock; refletem apenas o estado Busy do Master */
            if (hub->transfer_in_progress) {
                value = GBASIOMultiplayerFillBusy(value);
            } else {
                value = GBASIOMultiplayerClearBusy(value);
            }
        }

        /* Mantém RCNT sincronizado */
        driver->p->rcnt = GBASIORegisterRCNTFillSc(driver->p->rcnt);
        driver->p->rcnt = GBASIORegisterRCNTSetSi(driver->p->rcnt, self->player_id != 0);
        driver->p->rcnt = GBASIORegisterRCNTSetSd(driver->p->rcnt, all_multi ? 1 : 0);
        if (driver->p->p) {
            driver->p->p->memory.io[GBA_REG(RCNT)] = driver->p->rcnt;
        }

        __atomic_store_n(&hub->active_transfer_lines, 16, __ATOMIC_RELEASE);
        return value;
    }

    if (mode == GBA_SIO_NORMAL_32 || mode == GBA_SIO_NORMAL_8) {
        bool has_peers = _link_hub_has_peers(hub);
        value = GBASIONormalSetSi(value, self->player_id != 0 ? 1 : (!has_peers ? 1 : 0));
        if (self->player_id != 0 && !hub->transfer_in_progress) {
            value = GBASIONormalClearStart(value);
        }
        __atomic_store_n(&hub->active_transfer_lines, 16, __ATOMIC_RELEASE);
        return value;
    }

    return value;
}

/*
 * _link_driver_start é invocado pelo mGBA quando o bit Start/Busy (0x0080) é ativado em SIOCNT.
 * Retornando `true` para o Master (player_id == 0), o mGBA agenda `completeEvent` no mTiming
 * com a latência exata de ciclos do Baud Rate selecionado, evitando o bug de conclusão em 0 ciclos!
 */
static bool _link_driver_start(struct GBASIODriver* driver) {
    PS4LinkDriver* self = (PS4LinkDriver*)driver;
    PS4LinkHub* hub = self->hub;
    if (!hub || !driver->p) {
        return false;
    }

    /* Apenas o Master (Instância 0 / Internal Clock) comanda o início da transferência */
    if (self->player_id != 0) {
        return false;
    }

    enum GBASIOMode mode = driver->p->mode;

    if (mode == GBA_SIO_MULTI) {
        hub->transfer_in_progress = true;
        __atomic_store_n(&hub->active_transfer_lines, 16, __ATOMIC_RELEASE);

        /*
         * No início da transferência no hardware real:
         * 1. Os registradores SIOMULTI0..3 de todos os GBAs são resetados para 0xFFFF.
         * 2. O bit Busy (Bit 7) de SIOCNT sobe para 1 em todos os Slaves conectados.
         */
        for (int i = 0; i < hub->active_count; ++i) {
            struct GBA* g = hub->gba[i];
            if (!g) {
                continue;
            }
            if (g->sio.mode == GBA_SIO_MULTI) {
                g->memory.io[GBA_REG(SIOMULTI0)] = 0xFFFF;
                g->memory.io[GBA_REG(SIOMULTI1)] = 0xFFFF;
                g->memory.io[GBA_REG(SIOMULTI2)] = 0xFFFF;
                g->memory.io[GBA_REG(SIOMULTI3)] = 0xFFFF;

                g->sio.siocnt = GBASIOMultiplayerFillBusy(g->sio.siocnt);
                g->sio.siocnt = GBASIOMultiplayerSetId(g->sio.siocnt, i);
                g->sio.siocnt = GBASIOMultiplayerSetSlave(g->sio.siocnt, i != 0);
                g->memory.io[GBA_REG(SIOCNT)] = g->sio.siocnt;
            }
        }
        return true; /* Deixa o mTiming do Master agendar _GBASIOComplete */
    }

    if (mode == GBA_SIO_NORMAL_32 || mode == GBA_SIO_NORMAL_8) {
        if (!GBASIONormalIsInternalSc(driver->p->siocnt)) {
            return false;
        }
        hub->transfer_in_progress = true;
        __atomic_store_n(&hub->active_transfer_lines, 16, __ATOMIC_RELEASE);
        return true;
    }

    return false;
}

/*
 * Chamado pelo mTiming do Master (Player 0) após decorrerem os ciclos reais da transferência.
 * Nesse instante, os Slaves já avançaram em lockstep e escreveram suas respostas em SIOMLT_SEND.
 */
static void _link_driver_finish_multiplayer(struct GBASIODriver* driver, uint16_t data[4]) {
    PS4LinkDriver* self = (PS4LinkDriver*)driver;
    PS4LinkHub* hub = self ? self->hub : NULL;
    if (!hub) {
        data[0] = 0xFFFF;
        data[1] = 0xFFFF;
        data[2] = 0xFFFF;
        data[3] = 0xFFFF;
        return;
    }

    bool all_multi = _link_hub_all_in_multi(hub);

    /* 1. Coleta o SIOMLT_SEND atual de cada GBA que está em modo Multiplayer */
    for (int i = 0; i < LINK_MAX_PLAYERS; ++i) {
        if (i < hub->active_count && hub->gba[i] && hub->gba[i]->sio.mode == GBA_SIO_MULTI) {
            hub->multi_data[i] = hub->gba[i]->memory.io[GBA_REG(SIOMLT_SEND)];
        } else {
            hub->multi_data[i] = 0xFFFF;
        }
        data[i] = hub->multi_data[i];
    }

    hub->transfer_in_progress = false;

    /*
     * 2. O mGBA preenche SIOMULTI0..3, limpa Busy e dispara a IRQ para o Master (i == 0)
     *    imediatamente no retorno desta função.
     *    Para os Slaves (i >= 1), atualizamos os registradores e disparamos a IRQ agora!
     */
    for (int i = 1; i < hub->active_count; ++i) {
        struct GBA* g = hub->gba[i];
        if (!g || g->sio.mode != GBA_SIO_MULTI) {
            continue;
        }

        g->memory.io[GBA_REG(SIOMULTI0)] = data[0];
        g->memory.io[GBA_REG(SIOMULTI1)] = data[1];
        g->memory.io[GBA_REG(SIOMULTI2)] = data[2];
        g->memory.io[GBA_REG(SIOMULTI3)] = data[3];

        uint16_t scnt = g->sio.siocnt;
        scnt = GBASIOMultiplayerClearBusy(scnt);
        if (all_multi) {
            scnt = GBASIOMultiplayerFillReady(scnt);
        } else {
            scnt = GBASIOMultiplayerClearReady(scnt);
        }
        scnt = GBASIOMultiplayerSetId(scnt, i);
        scnt = GBASIOMultiplayerSetSlave(scnt, 1);
        scnt = GBASIOMultiplayerClearError(scnt);
        g->sio.siocnt = scnt;
        g->memory.io[GBA_REG(SIOCNT)] = scnt;

        g->sio.rcnt = GBASIORegisterRCNTFillSc(g->sio.rcnt);
        g->sio.rcnt = GBASIORegisterRCNTSetSi(g->sio.rcnt, 1);
        g->sio.rcnt = GBASIORegisterRCNTSetSd(g->sio.rcnt, all_multi ? 1 : 0);
        g->memory.io[GBA_REG(RCNT)] = g->sio.rcnt;

        if (GBASIOMultiplayerIsIrq(scnt)) {
            GBARaiseIRQ(g, GBA_IRQ_SIO, 0);
        }
    }

    /* Garante que o Master também mantenha SD/ID/Slave corretos após o retorno */
    if (hub->gba[0]) {
        struct GBA* g0 = hub->gba[0];
        if (all_multi) {
            g0->sio.siocnt = GBASIOMultiplayerFillReady(g0->sio.siocnt);
        }
        g0->sio.siocnt = GBASIOMultiplayerSetId(g0->sio.siocnt, 0);
        g0->sio.siocnt = GBASIOMultiplayerSetSlave(g0->sio.siocnt, 0);
        g0->sio.siocnt = GBASIOMultiplayerClearError(g0->sio.siocnt);
    }

    hub->transfer_count++;
    __atomic_store_n(&hub->active_transfer_lines, 16, __ATOMIC_RELEASE);
}

static uint8_t _link_driver_finish_normal8(struct GBASIODriver* driver) {
    PS4LinkDriver* self = (PS4LinkDriver*)driver;
    PS4LinkHub* hub = self ? self->hub : NULL;
    if (!hub || hub->active_count < 2 || !hub->gba[0] || !hub->gba[1]) {
        if (hub) {
            hub->transfer_in_progress = false;
        }
        return 0xFF;
    }

    struct GBA* g0 = hub->gba[0];
    struct GBA* g1 = hub->gba[1];

    uint8_t d0 = (uint8_t)(g0->memory.io[GBA_REG(SIODATA8)] & 0xFF);
    uint8_t d1 = (uint8_t)(g1->memory.io[GBA_REG(SIODATA8)] & 0xFF);

    hub->normal8_data[0] = d0;
    hub->normal8_data[1] = d1;
    hub->transfer_in_progress = false;

    /* Atualiza Slave (Player 1) */
    g1->memory.io[GBA_REG(SIODATA8)] = d0;
    g1->sio.siocnt = GBASIONormalClearStart(g1->sio.siocnt);
    g1->memory.io[GBA_REG(SIOCNT)] = g1->sio.siocnt;
    if (GBASIONormalIsIrq(g1->sio.siocnt)) {
        GBARaiseIRQ(g1, GBA_IRQ_SIO, 0);
    }

    hub->transfer_count++;
    __atomic_store_n(&hub->active_transfer_lines, 16, __ATOMIC_RELEASE);
    return d1; /* Master recebe d1 */
}

static uint32_t _link_driver_finish_normal32(struct GBASIODriver* driver) {
    PS4LinkDriver* self = (PS4LinkDriver*)driver;
    PS4LinkHub* hub = self ? self->hub : NULL;
    if (!hub || hub->active_count < 2 || !hub->gba[0] || !hub->gba[1]) {
        if (hub) {
            hub->transfer_in_progress = false;
        }
        return 0xFFFFFFFFU;
    }

    struct GBA* g0 = hub->gba[0];
    struct GBA* g1 = hub->gba[1];

    uint32_t d0 = (uint32_t)g0->memory.io[GBA_REG(SIODATA32_LO)] |
                  ((uint32_t)g0->memory.io[GBA_REG(SIODATA32_HI)] << 16);
    uint32_t d1 = (uint32_t)g1->memory.io[GBA_REG(SIODATA32_LO)] |
                  ((uint32_t)g1->memory.io[GBA_REG(SIODATA32_HI)] << 16);

    hub->normal32_data[0] = d0;
    hub->normal32_data[1] = d1;
    hub->transfer_in_progress = false;

    /* Atualiza Slave (Player 1) */
    g1->memory.io[GBA_REG(SIODATA32_LO)] = (uint16_t)(d0 & 0xFFFF);
    g1->memory.io[GBA_REG(SIODATA32_HI)] = (uint16_t)(d0 >> 16);
    g1->sio.siocnt = GBASIONormalClearStart(g1->sio.siocnt);
    g1->memory.io[GBA_REG(SIOCNT)] = g1->sio.siocnt;
    if (GBASIONormalIsIrq(g1->sio.siocnt)) {
        GBARaiseIRQ(g1, GBA_IRQ_SIO, 0);
    }

    hub->transfer_count++;
    __atomic_store_n(&hub->active_transfer_lines, 16, __ATOMIC_RELEASE);
    return d1; /* Master recebe d1 */
}

/* --- Funções Públicas do Link Hub --- */

void link_hub_init(void) {
    memset(&g_link_hub, 0, sizeof(g_link_hub));
    for (int i = 0; i < LINK_MAX_PLAYERS; ++i) {
        g_link_hub.multi_data[i] = 0xFFFF;
        g_link_hub.pending_multi_send[i] = 0xFFFF;
        g_link_hub.normal8_data[i] = 0xFF;
        g_link_hub.normal32_data[i] = 0xFFFFFFFFU;
    }
}

static void _setup_driver_callbacks(PS4LinkDriver* drv, int player_id) {
    memset(drv, 0, sizeof(*drv));
    drv->hub = &g_link_hub;
    drv->player_id = player_id;

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

        _setup_driver_callbacks(drv, i);
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

    _setup_driver_callbacks(drv, player_id);

    g_link_hub.gba[player_id] = gba;
    if (player_id >= g_link_hub.active_count) {
        g_link_hub.active_count = player_id + 1;
    }

    GBASIOSetDriver(&gba->sio, &drv->d);
    _link_driver_reset(&drv->d);
    return true;
}

void link_hub_reset(void) {
    g_link_hub.active_transfer_lines = 0;
    g_link_hub.transfer_in_progress = false;
    for (int i = 0; i < LINK_MAX_PLAYERS; ++i) {
        if (g_link_hub.gba[i]) {
            _link_driver_reset(&g_link_hub.drivers[i].d);
            _link_driver_set_mode(&g_link_hub.drivers[i].d, g_link_hub.gba[i]->sio.mode);
        }
    }
}

void link_hub_begin_frame(void) {
    _link_hub_refresh_multi_status(&g_link_hub);
}

void link_hub_destroy(void) {
    for (int i = 0; i < LINK_MAX_PLAYERS; ++i) {
        if (g_link_hub.gba[i]) {
            GBASIOSetDriver(&g_link_hub.gba[i]->sio, NULL);
            g_link_hub.gba[i] = NULL;
        }
    }
    memset(&g_link_hub, 0, sizeof(g_link_hub));
}

bool link_hub_is_transfer_active(void) {
    if (g_link_hub.transfer_in_progress) {
        return true;
    }
    return (__atomic_load_n(&g_link_hub.active_transfer_lines, __ATOMIC_ACQUIRE) > 0);
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