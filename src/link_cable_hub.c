#include "link_cable_hub.h"
#include <string.h>
#include <mgba/internal/gba/io.h>

PS4LinkHub g_link_hub = {0};

static bool _link_driver_init(struct GBASIODriver* driver) {
    PS4LinkDriver* self = (PS4LinkDriver*)driver;
    if (driver->p) {
        driver->p->rcnt = GBASIORegisterRCNTFillSc(driver->p->rcnt);
        driver->p->rcnt = GBASIORegisterRCNTSetSi(driver->p->rcnt, self->player_id != 0);
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

    driver->p->rcnt = GBASIORegisterRCNTFillSc(driver->p->rcnt);
    driver->p->rcnt = GBASIORegisterRCNTSetSi(driver->p->rcnt, self->player_id != 0);
}

static void _link_driver_set_mode(struct GBASIODriver* driver, enum GBASIOMode mode) {
    PS4LinkDriver* self = (PS4LinkDriver*)driver;
    if (!driver->p || !driver->p->p) {
        return;
    }

    if (mode == GBA_SIO_MULTI) {
        uint16_t siocnt = driver->p->siocnt;
        siocnt = GBASIOMultiplayerFillReady(siocnt);
        siocnt = GBASIOMultiplayerSetSlave(siocnt, self->player_id != 0);
        siocnt = GBASIOMultiplayerSetId(siocnt, self->player_id);
        siocnt = GBASIOMultiplayerClearError(siocnt);

        driver->p->siocnt = siocnt;
        driver->p->p->memory.io[GBA_REG(SIOCNT)] = siocnt;
        driver->p->rcnt = GBASIORegisterRCNTFillSc(driver->p->rcnt);
        driver->p->rcnt = GBASIORegisterRCNTSetSi(driver->p->rcnt, self->player_id != 0);
    } else if (mode == GBA_SIO_NORMAL_8 || mode == GBA_SIO_NORMAL_32) {
        int peer_id = self->player_id ^ 1;
        bool has_peer = (self->hub && self->hub->gba[peer_id] != NULL);
        driver->p->siocnt = GBASIONormalSetSi(driver->p->siocnt, !has_peer);
        driver->p->p->memory.io[GBA_REG(SIOCNT)] = driver->p->siocnt;
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
    return value;
}

static bool _link_driver_start(struct GBASIODriver* driver) {
    (void)driver;
    /* Returning false handles completion synchronously inside writeSIOCNT */
    return false;
}

static inline void link_hub_lock(PS4LinkHub* hub) {
    while (__atomic_test_and_set(&hub->lock, __ATOMIC_ACQUIRE)) {
#if defined(__x86_64__) || defined(__i386__)
        __builtin_ia32_pause();
#endif
    }
}

static inline void link_hub_unlock(PS4LinkHub* hub) {
    __atomic_clear(&hub->lock, __ATOMIC_RELEASE);
}

static uint16_t _link_driver_write_siocnt(struct GBASIODriver* driver, uint16_t value) {
    PS4LinkDriver* self = (PS4LinkDriver*)driver;
    PS4LinkHub* hub = self->hub;
    if (!hub || !driver->p || !driver->p->p) {
        return value;
    }

    link_hub_lock(hub);
    enum GBASIOMode mode = driver->p->mode;

    if (mode == GBA_SIO_MULTI) {
        value = GBASIOMultiplayerFillReady(value);
        value = GBASIOMultiplayerSetSlave(value, self->player_id != 0);
        value = GBASIOMultiplayerSetId(value, self->player_id);
        value = GBASIOMultiplayerClearError(value);

        if (self->player_id == 0 && GBASIOMultiplayerIsBusy(value)) {
            hub->active_transfer_lines = 4;
            uint16_t send_words[LINK_MAX_PLAYERS];
            for (int i = 0; i < LINK_MAX_PLAYERS; ++i) {
                struct GBA* g = hub->gba[i];
                if (g) {
                    send_words[i] = g->memory.io[GBA_REG(SIOMLT_SEND)];
                } else {
                    send_words[i] = 0xFFFF;
                }
                hub->multi_data[i] = send_words[i];
            }

            for (int i = 0; i < LINK_MAX_PLAYERS; ++i) {
                struct GBA* g = hub->gba[i];
                if (!g) {
                    continue;
                }

                g->memory.io[GBA_REG(SIOMULTI0)] = send_words[0];
                g->memory.io[GBA_REG(SIOMULTI1)] = send_words[1];
                g->memory.io[GBA_REG(SIOMULTI2)] = send_words[2];
                g->memory.io[GBA_REG(SIOMULTI3)] = send_words[3];

                uint16_t inst_siocnt = (i == 0) ? value : g->sio.siocnt;
                inst_siocnt = GBASIOMultiplayerClearBusy(inst_siocnt);
                inst_siocnt = GBASIOMultiplayerFillReady(inst_siocnt);
                inst_siocnt = GBASIOMultiplayerSetSlave(inst_siocnt, i != 0);
                inst_siocnt = GBASIOMultiplayerSetId(inst_siocnt, i);
                inst_siocnt = GBASIOMultiplayerClearError(inst_siocnt);

                g->sio.siocnt = inst_siocnt;
                g->memory.io[GBA_REG(SIOCNT)] = inst_siocnt;
                g->sio.rcnt = GBASIORegisterRCNTFillSc(g->sio.rcnt);
                g->sio.rcnt = GBASIORegisterRCNTSetSi(g->sio.rcnt, i != 0);

                if (GBASIOMultiplayerIsIrq(inst_siocnt)) {
                    GBARaiseIRQ(g, GBA_IRQ_SIO, 0);
                }
            }

            value = hub->gba[0]->sio.siocnt;
        } else if (self->player_id != 0) {
            value = GBASIOMultiplayerClearBusy(value);
        }

        link_hub_unlock(hub);
        return value;
    }

    if (mode == GBA_SIO_NORMAL_8) {
        int peer_id = self->player_id ^ 1;
        struct GBA* self_gba = hub->gba[self->player_id];
        struct GBA* peer_gba = hub->gba[peer_id];

        value = GBASIONormalSetSi(value, peer_gba == NULL);

        bool peer_waiting_master = peer_gba &&
                                   GBASIONormalIsStart(peer_gba->sio.siocnt) &&
                                   GBASIONormalIsSc(peer_gba->sio.siocnt);

        if (GBASIONormalIsStart(value)) {
            hub->active_transfer_lines = 4;
        }

        if (GBASIONormalIsStart(value) && (GBASIONormalIsSc(value) || peer_waiting_master)) {
            uint8_t self_out = (uint8_t)(self_gba->memory.io[GBA_REG(SIODATA8)] & 0xFF);
            uint8_t peer_out = peer_gba ? (uint8_t)(peer_gba->memory.io[GBA_REG(SIODATA8)] & 0xFF) : 0xFF;

            hub->normal8_data[self->player_id] = peer_out;
            self_gba->memory.io[GBA_REG(SIODATA8)] = peer_out;
            value = GBASIONormalClearStart(value);
            self_gba->sio.siocnt = value;
            self_gba->memory.io[GBA_REG(SIOCNT)] = value;

            if (GBASIONormalIsIrq(value)) {
                GBARaiseIRQ(self_gba, GBA_IRQ_SIO, 0);
            }

            if (peer_gba) {
                hub->normal8_data[peer_id] = self_out;
                peer_gba->memory.io[GBA_REG(SIODATA8)] = self_out;
                peer_gba->sio.siocnt = GBASIONormalClearStart(peer_gba->sio.siocnt);
                peer_gba->memory.io[GBA_REG(SIOCNT)] = peer_gba->sio.siocnt;

                if (GBASIONormalIsIrq(peer_gba->sio.siocnt)) {
                    GBARaiseIRQ(peer_gba, GBA_IRQ_SIO, 0);
                }
            }
        }

        link_hub_unlock(hub);
        return value;
    }

    if (mode == GBA_SIO_NORMAL_32) {
        int peer_id = self->player_id ^ 1;
        struct GBA* self_gba = hub->gba[self->player_id];
        struct GBA* peer_gba = hub->gba[peer_id];

        value = GBASIONormalSetSi(value, peer_gba == NULL);

        bool peer_waiting_master = peer_gba &&
                                   GBASIONormalIsStart(peer_gba->sio.siocnt) &&
                                   GBASIONormalIsSc(peer_gba->sio.siocnt);

        if (GBASIONormalIsStart(value)) {
            hub->active_transfer_lines = 4;
        }

        if (GBASIONormalIsStart(value) && (GBASIONormalIsSc(value) || peer_waiting_master)) {
            uint32_t self_out = (uint32_t)self_gba->memory.io[GBA_REG(SIODATA32_LO)] |
                                ((uint32_t)self_gba->memory.io[GBA_REG(SIODATA32_HI)] << 16);
            uint32_t peer_out = 0xFFFFFFFFU;
            if (peer_gba) {
                peer_out = (uint32_t)peer_gba->memory.io[GBA_REG(SIODATA32_LO)] |
                           ((uint32_t)peer_gba->memory.io[GBA_REG(SIODATA32_HI)] << 16);
            }

            hub->normal32_data[self->player_id] = peer_out;
            self_gba->memory.io[GBA_REG(SIODATA32_LO)] = (uint16_t)(peer_out & 0xFFFF);
            self_gba->memory.io[GBA_REG(SIODATA32_HI)] = (uint16_t)(peer_out >> 16);
            value = GBASIONormalClearStart(value);
            self_gba->sio.siocnt = value;
            self_gba->memory.io[GBA_REG(SIOCNT)] = value;

            if (GBASIONormalIsIrq(value)) {
                GBARaiseIRQ(self_gba, GBA_IRQ_SIO, 0);
            }

            if (peer_gba) {
                hub->normal32_data[peer_id] = self_out;
                peer_gba->memory.io[GBA_REG(SIODATA32_LO)] = (uint16_t)(self_out & 0xFFFF);
                peer_gba->memory.io[GBA_REG(SIODATA32_HI)] = (uint16_t)(self_out >> 16);
                peer_gba->sio.siocnt = GBASIONormalClearStart(peer_gba->sio.siocnt);
                peer_gba->memory.io[GBA_REG(SIOCNT)] = peer_gba->sio.siocnt;

                if (GBASIONormalIsIrq(peer_gba->sio.siocnt)) {
                    GBARaiseIRQ(peer_gba, GBA_IRQ_SIO, 0);
                }
            }
        }

        link_hub_unlock(hub);
        return value;
    }

    link_hub_unlock(hub);
    return value;
}

static void _link_driver_finish_multiplayer(struct GBASIODriver* driver, uint16_t data[4]) {
    PS4LinkDriver* self = (PS4LinkDriver*)driver;
    if (!self->hub) {
        return;
    }
    for (int i = 0; i < LINK_MAX_PLAYERS; ++i) {
        data[i] = self->hub->multi_data[i];
    }
}

static uint8_t _link_driver_finish_normal8(struct GBASIODriver* driver) {
    PS4LinkDriver* self = (PS4LinkDriver*)driver;
    if (!self->hub) {
        return 0xFF;
    }
    return self->hub->normal8_data[self->player_id];
}

static uint32_t _link_driver_finish_normal32(struct GBASIODriver* driver) {
    PS4LinkDriver* self = (PS4LinkDriver*)driver;
    if (!self->hub) {
        return 0xFFFFFFFFU;
    }
    return self->hub->normal32_data[self->player_id];
}

void link_hub_init(void) {
    memset(&g_link_hub, 0, sizeof(g_link_hub));
    for (int i = 0; i < LINK_MAX_PLAYERS; ++i) {
        g_link_hub.multi_data[i] = 0xFFFF;
        g_link_hub.normal8_data[i] = 0xFF;
        g_link_hub.normal32_data[i] = 0xFFFFFFFFU;
    }
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
    g_link_hub.active_count++;

    GBASIOSetDriver(&gba->sio, &drv->d);
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

