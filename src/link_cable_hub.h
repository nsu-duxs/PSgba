#ifndef LINK_CABLE_HUB_H
#define LINK_CABLE_HUB_H

#include <stdint.h>
#include <stdbool.h>
#include <mgba/core/core.h>
#include <mgba/core/timing.h>
#include <mgba/internal/gba/gba.h>
#include <mgba/internal/gba/sio.h>

#define LINK_MAX_PLAYERS 4

typedef struct PS4LinkHub PS4LinkHub;

typedef struct PS4LinkDriver {
    struct GBASIODriver d;
    PS4LinkHub* hub;
    int player_id;
    bool has_entered_multi;
    uint32_t gpio_grace_frames;
    struct mTimingEvent transfer_event;
    struct mTimingEvent lockstep_event;
} PS4LinkDriver;

struct PS4LinkHub {
    struct GBA* gba[LINK_MAX_PLAYERS];
    PS4LinkDriver drivers[LINK_MAX_PLAYERS];
    int active_count;
    uint32_t transfer_count;
    uint16_t multi_data[LINK_MAX_PLAYERS];
    uint16_t pending_multi_send[LINK_MAX_PLAYERS];
    uint8_t normal8_data[LINK_MAX_PLAYERS];
    uint32_t normal32_data[LINK_MAX_PLAYERS];
    enum GBASIOMode transfer_mode;
    int32_t transfer_cycles;
    uint32_t transfer_scheduled_mask;
    uint32_t transfer_finished_mask;
    bool transfer_data_latched;
    bool cable_enabled;
    uint32_t hotplug_disconnect_frames;
    bool transfer_in_progress;
    bool transfer_just_started;
    volatile int active_transfer_lines;
};

extern PS4LinkHub g_link_hub;

void link_hub_init(void);
bool link_hub_attach(struct mCore* cores[], int num_cores);
bool link_hub_attach_instance(int player_id, struct mCore* core);
void link_hub_reset(void);
void link_hub_begin_frame(void);
void link_hub_sync_pins(void);
void link_hub_begin_transfer_on_slaves(void);
void link_hub_destroy(void);

/* Controle de Conexão Física / Hotplug do Link Cable */
void link_hub_set_cable_enabled(bool enabled);
bool link_hub_is_cable_enabled(void);
void link_hub_trigger_hotplug(void);
bool link_hub_is_hotplugging(void);

bool link_hub_is_transfer_active(void);
bool link_hub_consume_just_started(void);
void link_hub_notify_scanline(void);
uint32_t link_hub_get_transfer_count(void);
int link_hub_get_player_mode(int player_id);

#endif /* LINK_CABLE_HUB_H */