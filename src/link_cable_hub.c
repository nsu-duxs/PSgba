#include "link_cable_hub.h"
#include <string.h>
#include <mgba/internal/gba/gba.h>
#include <mgba/internal/gba/io.h>

PS4LinkHub g_link_hub = {0};

/*
 * Transferência síncrona de Multiplayer (GBA_SIO_MULTI):
 * O Master (Instância 0) aciona a transferência quando o bit de Start/Busy é ativado.
 * Os dados enviados (SIOMLT_SEND) de todas as instâncias ativas são lidos para o hub,
 * escritos em SIOMULTI0..3 de cada GBA, o bit Busy é limpo e a interrupção serial (IRQ_SIO)
 * é disparada em todos os núcleos.
 */
static void _link_hub_transfer_multi(PS4LinkHub* hub) {
    if (!hub || hub->active_count < 2) {
        return;
    }

    int count = hub->active_count;
    if (count > LINK_MAX_PLAYERS) {
        count = LINK_MAX_PLAYERS;
    }

    /* 1. Copia dados enviados (SIOMLT_SEND) de cada instância para os buffers do hub */
    for (int i = 0; i < LINK_MAX_PLAYERS; ++i) {
        if (i < count && hub->gba[i]) {
            hub->multi_data[i] = hub->gba[i]->memory.io[GBA_REG(SIOMLT_SEND)];
        } else {
            hub->multi_data[i] = 0xFFFF;
        }
    }

    /* 2. Distribui para os registradores de recepção (SIOMULTI0..3) de cada instância */
    for (int i = 0; i < count; ++i) {
        struct GBA* g = hub->gba[i];
        if (!g) {
            continue;
        }

        g->memory.io[GBA_REG(SIOMULTI0)] = hub->multi_data[0];
        g->memory.io[GBA_REG(SIOMULTI1)] = hub->multi_data[1];
        g->memory.io[GBA_REG(SIOMULTI2)] = hub->multi_data[2];
        g->memory.io[GBA_REG(SIOMULTI3)] = hub->multi_data[3];

        /* Limpa o bit de Busy (0x80), garante bit Ready (0x08) ativado e atualiza ID/Slave */
        uint16_t scnt = g->sio.siocnt;
        scnt &= ~0x0080;
        scnt |= 0x0008;
        scnt = GBASIOMultiplayerSetId(scnt, i);
        scnt = GBASIOMultiplayerSetSlave(scnt, i != 0);
        scnt = GBASIOMultiplayerClearError(scnt);
        g->sio.siocnt = scnt;
        g->memory.io[GBA_REG(SIOCNT)] = scnt;

        /* Atualiza RCNT: SC=1, SD=1 (Ready), SI=(i!=0) */
        g->sio.rcnt = GBASIORegisterRCNTFillSc(g->sio.rcnt);
        g->sio.rcnt = GBASIORegisterRCNTSetSi(g->sio.rcnt, i != 0);
        g->sio.rcnt = GBASIORegisterRCNTSetSd(g->sio.rcnt, 1);
        g->memory.io[GBA_REG(RCNT)] = g->sio.rcnt;

        /* Dispara interrupção serial no registrador IF e agenda IRQ */
        g->memory.io[GBA_REG(IF)] |= (1 << GBA_IRQ_SIO);
        if (GBASIOMultiplayerIsIrq(scnt)) {
            GBARaiseIRQ(g, GBA_IRQ_SIO, 0);
        }
    }

    hub->transfer_count++;
    __atomic_store_n(&hub->active_transfer_lines, 4, __ATOMIC_RELEASE);
}

/*
 * Transferência síncrona Normal 32-bit (GBA_SIO_NORMAL_32)
 */
static void _link_hub_transfer_normal32(PS4LinkHub* hub) {
    if (!hub || hub->active_count < 2) {
        return;
    }

    struct GBA* g0 = hub->gba[0];
    struct GBA* g1 = hub->gba[1];
    if (!g0 || !g1) {
        return;
    }

    uint32_t d0 = (uint32_t)g0->memory.io[GBA_REG(SIODATA32_LO)] |
                  ((uint32_t)g0->memory.io[GBA_REG(SIODATA32_HI)] << 16);
    uint32_t d1 = (uint32_t)g1->memory.io[GBA_REG(SIODATA32_LO)] |
                  ((uint32_t)g1->memory.io[GBA_REG(SIODATA32_HI)] << 16);

    hub->normal32_data[0] = d0;
    hub->normal32_data[1] = d1;

    /* Instância 0 recebe d1 */
    g0->memory.io[GBA_REG(SIODATA32_LO)] = (uint16_t)(d1 & 0xFFFF);
    g0->memory.io[GBA_REG(SIODATA32_HI)] = (uint16_t)(d1 >> 16);
    g0->sio.siocnt &= ~0x0080;
    g0->memory.io[GBA_REG(SIOCNT)] = g0->sio.siocnt;
    g0->memory.io[GBA_REG(IF)] |= (1 << GBA_IRQ_SIO);
    if (GBASIONormalIsIrq(g0->sio.siocnt)) {
        GBARaiseIRQ(g0, GBA_IRQ_SIO, 0);
    }

    /* Instância 1 recebe d0 */
    g1->memory.io[GBA_REG(SIODATA32_LO)] = (uint16_t)(d0 & 0xFFFF);
    g1->memory.io[GBA_REG(SIODATA32_HI)] = (uint16_t)(d0 >> 16);
    g1->sio.siocnt &= ~0x0080;
    g1->memory.io[GBA_REG(SIOCNT)] = g1->sio.siocnt;
    g1->memory.io[GBA_REG(IF)] |= (1 << GBA_IRQ_SIO);
    if (GBASIONormalIsIrq(g1->sio.siocnt)) {
        GBARaiseIRQ(g1, GBA_IRQ_SIO, 0);
    }

    hub->transfer_count++;
    __atomic_store_n(&hub->active_transfer_lines, 4, __ATOMIC_RELEASE);
}

/*
 * Transferência síncrona Normal 8-bit (GBA_SIO_NORMAL_8)
 */
static void _link_hub_transfer_normal8(PS4LinkHub* hub) {
    if (!hub || hub->active_count < 2) {
        return;
    }

    struct GBA* g0 = hub->gba[0];
    struct GBA* g1 = hub->gba[1];
    if (!g0 || !g1) {
        return;
    }

    uint8_t d0 = (uint8_t)(g0->memory.io[GBA_REG(SIODATA8)] & 0xFF);
    uint8_t d1 = (uint8_t)(g1->memory.io[GBA_REG(SIODATA8)] & 0xFF);

    hub->normal8_data[0] = d0;
    hub->normal8_data[1] = d1;

    g0->memory.io[GBA_REG(SIODATA8)] = d1;
    g0->sio.siocnt &= ~0x0080;
    g0->memory.io[GBA_REG(SIOCNT)] = g0->sio.siocnt;
    g0->memory.io[GBA_REG(IF)] |= (1 << GBA_IRQ_SIO);
    if (GBASIONormalIsIrq(g0->sio.siocnt)) {
        GBARaiseIRQ(g0, GBA_IRQ_SIO, 0);
    }

    g1->memory.io[GBA_REG(SIODATA8)] = d0;
    g1->sio.siocnt &= ~0x0080;
    g1->memory.io[GBA_REG(SIOCNT)] = g1->sio.siocnt;
    g1->memory.io[GBA_REG(IF)] |= (1 << GBA_IRQ_SIO);
    if (GBASIONormalIsIrq(g1->sio.siocnt)) {
        GBARaiseIRQ(g1, GBA_IRQ_SIO, 0);
    }

    hub->transfer_count++;
    __atomic_store_n(&hub->active_transfer_lines, 4, __ATOMIC_RELEASE);
}

/* --- Callbacks de GBASIODriver --- */

static bool _link_driver_init(struct GBASIODriver* driver) {
    PS4LinkDriver* self = (PS4LinkDriver*)driver;
    if (driver->p) {
        driver->p->rcnt = GBASIORegisterRCNTFillSc(driver->p->rcnt);
        driver->p->rcnt = GBASIORegisterRCNTSetSi(driver->p->rcnt, self->player_id != 0);
        if (self->hub && self->hub->active_count >= 2) {
            driver->p->rcnt = GBASIORegisterRCNTSetSd(driver->p->rcnt, 1);
        }
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
    bool has_peers = (self->hub && self->hub->active_count >= 2);

    gba->memory.io[GBA_REG(SIOMULTI0)] = 0xFFFF;
    gba->memory.io[GBA_REG(SIOMULTI1)] = 0xFFFF;
    gba->memory.io[GBA_REG(SIOMULTI2)] = 0xFFFF;
    gba->memory.io[GBA_REG(SIOMULTI3)] = 0xFFFF;

    uint16_t scnt = 0;
    if (has_peers) {
        scnt |= 0x0008; /* Ready / SD */
    }
    scnt = GBASIOMultiplayerSetSlave(scnt, self->player_id != 0);
    scnt = GBASIOMultiplayerSetId(scnt, self->player_id);
    driver->p->siocnt = scnt;
    gba->memory.io[GBA_REG(SIOCNT)] = scnt;

    uint16_t rcnt = driver->p->rcnt;
    rcnt = GBASIORegisterRCNTFillSc(rcnt);
    rcnt = GBASIORegisterRCNTSetSi(rcnt, self->player_id != 0);
    if (has_peers) {
        rcnt = GBASIORegisterRCNTSetSd(rcnt, 1);
    }
    driver->p->rcnt = rcnt;
    gba->memory.io[GBA_REG(RCNT)] = rcnt;
}

static void _link_driver_set_mode(struct GBASIODriver* driver, enum GBASIOMode mode) {
    PS4LinkDriver* self = (PS4LinkDriver*)driver;
    if (!driver->p || !driver->p->p || !self->hub) {
        return;
    }

    struct GBA* gba = driver->p->p;
    bool has_peers = (self->hub->active_count >= 2);

    if (mode == GBA_SIO_MULTI) {
        uint16_t scnt = driver->p->siocnt;
        if (has_peers) {
            scnt |= 0x0008; /* Ready */
        }
        scnt = GBASIOMultiplayerSetSlave(scnt, self->player_id != 0);
        scnt = GBASIOMultiplayerSetId(scnt, self->player_id);
        scnt = GBASIOMultiplayerClearError(scnt);
        scnt &= ~0x0080;
        driver->p->siocnt = scnt;
        gba->memory.io[GBA_REG(SIOCNT)] = scnt;

        uint16_t rcnt = driver->p->rcnt;
        rcnt = GBASIORegisterRCNTFillSc(rcnt);
        rcnt = GBASIORegisterRCNTSetSi(rcnt, self->player_id != 0);
        if (has_peers) {
            rcnt = GBASIORegisterRCNTSetSd(rcnt, 1);
        }
        driver->p->rcnt = rcnt;
        gba->memory.io[GBA_REG(RCNT)] = rcnt;
    }
}

static bool _link_driver_handles_mode(struct GBASIODriver* driver, enum GBASIOMode mode) {
    (void)driver;
    switch (mode) {
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
    value = GBASIORegisterRCNTFillSc(value);
    value = GBASIORegisterRCNTSetSi(value, self->player_id != 0);
    if (self->hub && self->hub->active_count >= 2) {
        value = GBASIORegisterRCNTSetSd(value, 1);
    }
    return value;
}

static uint16_t _link_driver_write_siocnt(struct GBASIODriver* driver, uint16_t value) {
    PS4LinkDriver* self = (PS4LinkDriver*)driver;
    PS4LinkHub* hub = self->hub;
    if (!hub || !driver->p) {
        return value;
    }

    bool has_peers = (hub->active_count >= 2);
    enum GBASIOMode mode = driver->p->mode;

    if (mode == GBA_SIO_MULTI) {
        if (has_peers) {
            value |= 0x0008; /* Ready (SD) ativo para todos os jogadores conectados */
        }
        value = GBASIOMultiplayerSetSlave(value, self->player_id != 0);
        value = GBASIOMultiplayerSetId(value, self->player_id);
        value = GBASIOMultiplayerClearError(value);

        /* Se o Master (Instância 0) acionar a transferência (bit Start/Busy ativo) */
        if (self->player_id == 0 && (value & 0x0080)) {
            _link_hub_transfer_multi(hub);
            value &= ~0x0080; /* Limpa Busy após transferência síncrona */
        } else if (self->player_id != 0) {
            value &= ~0x0080; /* Slaves não acionam clock de transferência */
        }

        /* Mantém RCNT em conformidade */
        driver->p->rcnt = GBASIORegisterRCNTFillSc(driver->p->rcnt);
        driver->p->rcnt = GBASIORegisterRCNTSetSi(driver->p->rcnt, self->player_id != 0);
        if (has_peers) {
            driver->p->rcnt = GBASIORegisterRCNTSetSd(driver->p->rcnt, 1);
        }
        if (driver->p->p) {
            driver->p->p->memory.io[GBA_REG(RCNT)] = driver->p->rcnt;
        }
        return value;
    }

    if (mode == GBA_SIO_NORMAL_32) {
        value = GBASIONormalSetSi(value, !has_peers);
        if (self->player_id == 0 && (value & 0x0080)) {
            _link_hub_transfer_normal32(hub);
            value &= ~0x0080;
        } else if (self->player_id != 0) {
            value &= ~0x0080;
        }
        return value;
    }

    if (mode == GBA_SIO_NORMAL_8) {
        value = GBASIONormalSetSi(value, !has_peers);
        if (self->player_id == 0 && (value & 0x0080)) {
            _link_hub_transfer_normal8(hub);
            value &= ~0x0080;
        } else if (self->player_id != 0) {
            value &= ~0x0080;
        }
        return value;
    }

    return value;
}

static bool _link_driver_start(struct GBASIODriver* driver) {
    (void)driver;
    /* Retorna false para que o mGBA não agende eventos assíncronos (conclusão em writeSIOCNT) */
    return false;
}

static void _link_driver_finish_multiplayer(struct GBASIODriver* driver, uint16_t data[4]) {
    PS4LinkDriver* self = (PS4LinkDriver*)driver;
    if (!self || !self->hub) {
        return;
    }
    for (int i = 0; i < LINK_MAX_PLAYERS; ++i) {
        data[i] = self->hub->multi_data[i];
    }
}

static uint8_t _link_driver_finish_normal8(struct GBASIODriver* driver) {
    PS4LinkDriver* self = (PS4LinkDriver*)driver;
    if (!self || !self->hub || self->player_id == 0) {
        return 0xFF;
    }
    return self->hub->normal8_data[self->player_id - 1];
}

static uint32_t _link_driver_finish_normal32(struct GBASIODriver* driver) {
    PS4LinkDriver* self = (PS4LinkDriver*)driver;
    if (!self || !self->hub || self->player_id == 0) {
        return 0xFFFFFFFFU;
    }
    return self->hub->normal32_data[self->player_id - 1];
}

/* --- Funções Públicas do Link Hub --- */

void link_hub_init(void) {
    memset(&g_link_hub, 0, sizeof(g_link_hub));
    for (int i = 0; i < LINK_MAX_PLAYERS; ++i) {
        g_link_hub.multi_data[i] = 0xFFFF;
        g_link_hub.normal8_data[i] = 0xFF;
        g_link_hub.normal32_data[i] = 0xFFFFFFFFU;
    }
}

/*
 * Anexa todos os núcleos ao hub e configura o GBASIODriver em gba->sio.driver.
 */
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

        memset(drv, 0, sizeof(*drv));
        drv->hub = &g_link_hub;
        drv->player_id = i;

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
    for (int i = 0; i < LINK_MAX_PLAYERS; ++i) {
        if (g_link_hub.gba[i]) {
            _link_driver_reset(&g_link_hub.drivers[i].d);
            _link_driver_set_mode(&g_link_hub.drivers[i].d, g_link_hub.gba[i]->sio.mode);
        }
    }
}

void link_hub_begin_frame(void) {
    /* No-op: Lockstep em mesma thread opera de forma síncrona nos callbacks */
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
