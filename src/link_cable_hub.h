#ifndef LINK_CABLE_HUB_H
#define LINK_CABLE_HUB_H

#include <stdint.h>
#include <stdbool.h>
#include <mgba/core/core.h>
#include <mgba/internal/gba/gba.h>
#include <mgba/internal/gba/sio.h>

#define LINK_MAX_PLAYERS 4

typedef struct PS4LinkHub PS4LinkHub;

typedef struct PS4LinkDriver {
    struct GBASIODriver d;
    PS4LinkHub* hub;
    int player_id;
} PS4LinkDriver;

struct PS4LinkHub {
    struct GBA* gba[LINK_MAX_PLAYERS];
    PS4LinkDriver drivers[LINK_MAX_PLAYERS];
    int active_count;
    uint16_t multi_data[LINK_MAX_PLAYERS];
    uint8_t normal8_data[LINK_MAX_PLAYERS];
    uint32_t normal32_data[LINK_MAX_PLAYERS];
    volatile int lock;
    volatile int active_transfer_lines;
};

extern PS4LinkHub g_link_hub;

void link_hub_init(void);
bool link_hub_attach_instance(int player_id, struct mCore* core);
void link_hub_reset(void);
void link_hub_destroy(void);
bool link_hub_is_transfer_active(void);
void link_hub_notify_scanline(void);

#endif /* LINK_CABLE_HUB_H */
