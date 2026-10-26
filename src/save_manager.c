#include "save_manager.h"
#include "link_cable_hub.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <mgba-util/vfs.h>
#include <mgba/internal/gba/gba.h>
#include <mgba/internal/gba/savedata.h>

#define GROUP_STATE_MAGIC 0x34534750U /* 'PGS4' */

typedef struct PS4GroupStateHeader {
    uint32_t magic;
    int32_t active_count;
    uint32_t single_state_size;
    uint16_t multi_data[LINK_MAX_PLAYERS];
    uint8_t normal8_data[LINK_MAX_PLAYERS];
    uint32_t normal32_data[LINK_MAX_PLAYERS];
    int32_t sio_mode[LINK_MAX_PLAYERS];
    uint16_t sio_rcnt[LINK_MAX_PLAYERS];
    uint16_t sio_siocnt[LINK_MAX_PLAYERS];
} PS4GroupStateHeader;

static bool is_registered_ps4_user(OrbisUserServiceUserId user_id) {
    if (user_id <= 0 || user_id == ORBIS_USER_SERVICE_USER_ID_SYSTEM) {
        return false;
    }

    char user_name[ORBIS_USER_SERVICE_MAX_USER_NAME_LENGTH + 1];
    memset(user_name, 0, sizeof(user_name));
    if (sceUserServiceGetUserName(user_id, user_name, sizeof(user_name)) < 0 ||
        user_name[0] == '\0') {
        return false;
    }

    if (strncasecmp(user_name, "Guest", 5) == 0 ||
        strncasecmp(user_name, "Convidado", 9) == 0) {
        return false;
    }

    return true;
}

static void build_user_save_path(const char* rom_name, int player_id,
                                 OrbisUserServiceUserId user_id,
                                 char* out_path, size_t max_len) {
    mkdir(PS4_DATA_ROOT_DIR, 0777);
    mkdir(PS4_SAVES_DIR, 0777);
    mkdir(PS4_GUEST_SAVES_DIR, 0777);

    char profile_dir[256];
    if (is_registered_ps4_user(user_id)) {
        snprintf(profile_dir, sizeof(profile_dir),
                 "%s/user_%08x", PS4_SAVES_DIR, (unsigned int)user_id);
        mkdir(profile_dir, 0777);
    } else {
        int slot_num = (player_id >= 0 && player_id < GBA_MAX_INSTANCES) ? (player_id + 1) : 1;
        snprintf(profile_dir, sizeof(profile_dir),
                 "%s/guest_%d", PS4_GUEST_SAVES_DIR, slot_num);
        mkdir(profile_dir, 0777);
    }

    snprintf(out_path, max_len, "%s/%s.sav", profile_dir, rom_name);
}

static void build_user_state_path(const char* rom_name, int player_id,
                                  OrbisUserServiceUserId user_id,
                                  char* out_path, size_t max_len) {
    mkdir(PS4_DATA_ROOT_DIR, 0777);
    mkdir(PS4_STATES_DIR, 0777);

    char profile_dir[256];
    if (is_registered_ps4_user(user_id)) {
        snprintf(profile_dir, sizeof(profile_dir),
                 "%s/user_%08x", PS4_STATES_DIR, (unsigned int)user_id);
        mkdir(profile_dir, 0777);
    } else {
        char guests_state_root[256];
        snprintf(guests_state_root, sizeof(guests_state_root), "%s/guests", PS4_STATES_DIR);
        mkdir(guests_state_root, 0777);

        int slot_num = (player_id >= 0 && player_id < GBA_MAX_INSTANCES) ? (player_id + 1) : 1;
        snprintf(profile_dir, sizeof(profile_dir),
                 "%s/guest_%d", guests_state_root, slot_num);
        mkdir(profile_dir, 0777);
    }

    snprintf(out_path, max_len, "%s/%s.ss0", profile_dir, rom_name);
}

bool save_manager_init(void) {
    mkdir(PS4_DATA_ROOT_DIR, 0777);
    mkdir(PS4_ROMS_DIR, 0777);
    mkdir(PS4_SAVES_DIR, 0777);
    mkdir(PS4_GUEST_SAVES_DIR, 0777);
    mkdir(PS4_STATES_DIR, 0777);
    return true;
}

void save_manager_extract_rom_name(const char* rom_path, char* out_name, size_t max_len) {
    if (!out_name || max_len == 0) {
        return;
    }
    if (!rom_path || !*rom_path) {
        strncpy(out_name, "rom", max_len - 1);
        out_name[max_len - 1] = '\0';
        return;
    }

    const char* slash = strrchr(rom_path, '/');
    const char* bslash = strrchr(rom_path, '\\');
    const char* base = rom_path;

    if (slash && bslash) {
        base = (slash > bslash) ? (slash + 1) : (bslash + 1);
    } else if (slash) {
        base = slash + 1;
    } else if (bslash) {
        base = bslash + 1;
    }

    const char* dot = strrchr(base, '.');
    size_t len = dot ? (size_t)(dot - base) : strlen(base);
    if (len >= max_len) {
        len = max_len - 1;
    }

    memcpy(out_name, base, len);
    out_name[len] = '\0';
}

bool save_manager_attach_battery_save(struct mCore* core, const char* rom_name, int player_id,
                                      OrbisUserServiceUserId user_id) {
    if (!core || !rom_name || player_id < 0 || player_id >= GBA_MAX_INSTANCES) {
        return false;
    }

    char save_path[256];
    build_user_save_path(rom_name, player_id, user_id, save_path, sizeof(save_path));

    /*
     * Se o save ainda nao existe no PS4, verifica se ha um .sav pre-existente no Pendrive
     * ao lado da ROM para importar (ex: save trazido do PC). Apos importado, todas as
     * leituras e gravacoes permanecem estritamente no armazenamento interno do PS4.
     */
    struct stat st;
    if (stat(save_path, &st) != 0 || st.st_size == 0) {
        if (g_gba_ctx.rom_path[0] != '\0') {
            char usb_sav_path[256];
            strncpy(usb_sav_path, g_gba_ctx.rom_path, sizeof(usb_sav_path) - 1);
            usb_sav_path[sizeof(usb_sav_path) - 1] = '\0';
            char* dot = strrchr(usb_sav_path, '.');
            if (dot) {
                *dot = '\0';
                strncat(usb_sav_path, ".sav", sizeof(usb_sav_path) - strlen(usb_sav_path) - 1);
                FILE* src_f = fopen(usb_sav_path, "rb");
                if (src_f) {
                    FILE* dst_f = fopen(save_path, "wb");
                    if (dst_f) {
                        char buf[4096];
                        size_t n;
                        while ((n = fread(buf, 1, sizeof(buf), src_f)) > 0) {
                            fwrite(buf, 1, n, dst_f);
                        }
                        fclose(dst_f);
                    }
                    fclose(src_f);
                }
            }
        }
    }

    struct VFile* vf = VFileOpen(save_path, O_CREAT | O_RDWR);
    if (!vf) {
        return false;
    }

    return core->loadSave(core, vf);
}

void save_manager_flush_battery_save(struct mCore* core) {
    if (!core || !core->board) {
        return;
    }

    struct GBA* gba = (struct GBA*)core->board;
    struct GBASavedata* savedata = &gba->memory.savedata;
    if (!savedata->vf || !savedata->data) {
        return;
    }

    GBASavedataClean(savedata, 0);
    size_t size = GBASavedataSize(savedata);
    if (size > 0) {
        savedata->vf->sync(savedata->vf, savedata->data, size);
    }
}

bool save_manager_quick_save_state(struct PS4GBA_Context* ctx, int player_id) {
    if (!ctx || player_id < 0 || player_id >= GBA_MAX_INSTANCES) {
        return false;
    }

    struct mCore* core = ctx->instances[player_id].core;
    if (!ctx->instances[player_id].active || !core) {
        return false;
    }

    save_manager_flush_battery_save(core);

    size_t state_size = core->stateSize(core);
    if (state_size == 0) {
        return false;
    }

    void* state_buf = malloc(state_size);
    if (!state_buf) {
        return false;
    }

    if (!core->saveState(core, state_buf)) {
        free(state_buf);
        return false;
    }

    const char* rname = ctx->instances[player_id].rom_name[0]
                            ? ctx->instances[player_id].rom_name
                            : ctx->rom_name;
    char state_path[256];
    build_user_state_path(rname, player_id, ctx->instances[player_id].user_id,
                          state_path, sizeof(state_path));

    struct VFile* vf = VFileOpen(state_path, O_CREAT | O_TRUNC | O_RDWR);
    if (!vf) {
        free(state_buf);
        return false;
    }

    ssize_t written = vf->write(vf, state_buf, state_size);
    vf->close(vf);
    free(state_buf);

    return (written == (ssize_t)state_size);
}

bool save_manager_quick_load_state(struct PS4GBA_Context* ctx, int player_id) {
    if (!ctx || player_id < 0 || player_id >= GBA_MAX_INSTANCES) {
        return false;
    }

    struct mCore* core = ctx->instances[player_id].core;
    if (!ctx->instances[player_id].active || !core) {
        return false;
    }

    size_t state_size = core->stateSize(core);
    if (state_size == 0) {
        return false;
    }

    const char* rname = ctx->instances[player_id].rom_name[0]
                            ? ctx->instances[player_id].rom_name
                            : ctx->rom_name;
    char state_path[256];
    build_user_state_path(rname, player_id, ctx->instances[player_id].user_id,
                          state_path, sizeof(state_path));

    struct VFile* vf = VFileOpen(state_path, O_RDONLY);
    if (!vf) {
        return false;
    }

    void* state_buf = malloc(state_size);
    if (!state_buf) {
        vf->close(vf);
        return false;
    }

    ssize_t bytes_read = vf->read(vf, state_buf, state_size);
    vf->close(vf);

    if (bytes_read != (ssize_t)state_size) {
        free(state_buf);
        return false;
    }

    bool ok = core->loadState(core, state_buf);
    free(state_buf);

    /* Restore custom SIO link driver state after core deserialization */
    if (ok && g_link_hub.gba[player_id]) {
        GBASIOSetDriver(&g_link_hub.gba[player_id]->sio, &g_link_hub.drivers[player_id].d);
    }

    return ok;
}

bool save_manager_group_save_state(struct PS4GBA_Context* ctx) {
    if (!ctx || ctx->active_count <= 0 || !ctx->instances[0].core) {
        return false;
    }

    size_t state_size = ctx->instances[0].core->stateSize(ctx->instances[0].core);
    if (state_size == 0) {
        return false;
    }

    void* state_buf = malloc(state_size);
    if (!state_buf) {
        return false;
    }

    char group_path[256];
    snprintf(group_path, sizeof(group_path),
             "%s/%s_group.ss0", PS4_STATES_DIR, ctx->rom_name);

    struct VFile* vf = VFileOpen(group_path, O_CREAT | O_TRUNC | O_RDWR);
    if (!vf) {
        free(state_buf);
        return false;
    }

    PS4GroupStateHeader hdr;
    memset(&hdr, 0, sizeof(hdr));
    hdr.magic = GROUP_STATE_MAGIC;
    hdr.active_count = ctx->active_count;
    hdr.single_state_size = (uint32_t)state_size;

    for (int i = 0; i < LINK_MAX_PLAYERS; ++i) {
        hdr.multi_data[i] = g_link_hub.multi_data[i];
        hdr.normal8_data[i] = g_link_hub.normal8_data[i];
        hdr.normal32_data[i] = g_link_hub.normal32_data[i];
        if (g_link_hub.gba[i]) {
            hdr.sio_mode[i] = (int32_t)g_link_hub.gba[i]->sio.mode;
            hdr.sio_rcnt[i] = g_link_hub.gba[i]->sio.rcnt;
            hdr.sio_siocnt[i] = g_link_hub.gba[i]->sio.siocnt;
        }
    }

    if (vf->write(vf, &hdr, sizeof(hdr)) != (ssize_t)sizeof(hdr)) {
        vf->close(vf);
        free(state_buf);
        return false;
    }

    for (int i = 0; i < GBA_MAX_INSTANCES; ++i) {
        struct mCore* core = ctx->instances[i].core;
        if (!ctx->instances[i].active || !core) {
            memset(state_buf, 0, state_size);
        } else {
            save_manager_flush_battery_save(core);
            if (!core->saveState(core, state_buf)) {
                vf->close(vf);
                free(state_buf);
                return false;
            }
        }

        if (vf->write(vf, state_buf, state_size) != (ssize_t)state_size) {
            vf->close(vf);
            free(state_buf);
            return false;
        }
    }

    vf->close(vf);
    free(state_buf);
    return true;
}

bool save_manager_group_load_state(struct PS4GBA_Context* ctx) {
    if (!ctx || ctx->active_count <= 0 || !ctx->instances[0].core) {
        return false;
    }

    size_t state_size = ctx->instances[0].core->stateSize(ctx->instances[0].core);
    if (state_size == 0) {
        return false;
    }

    char group_path[256];
    snprintf(group_path, sizeof(group_path),
             "%s/%s_group.ss0", PS4_STATES_DIR, ctx->rom_name);

    struct VFile* vf = VFileOpen(group_path, O_RDONLY);
    if (!vf) {
        return false;
    }

    PS4GroupStateHeader hdr;
    if (vf->read(vf, &hdr, sizeof(hdr)) != (ssize_t)sizeof(hdr) ||
        hdr.magic != GROUP_STATE_MAGIC ||
        hdr.single_state_size != (uint32_t)state_size) {
        vf->close(vf);
        return false;
    }

    void* state_buf = malloc(state_size);
    if (!state_buf) {
        vf->close(vf);
        return false;
    }

    for (int i = 0; i < GBA_MAX_INSTANCES; ++i) {
        if (vf->read(vf, state_buf, state_size) != (ssize_t)state_size) {
            vf->close(vf);
            free(state_buf);
            return false;
        }

        struct mCore* core = ctx->instances[i].core;
        if (ctx->instances[i].active && core) {
            if (!core->loadState(core, state_buf)) {
                vf->close(vf);
                free(state_buf);
                return false;
            }
        }
    }

    vf->close(vf);
    free(state_buf);

    for (int i = 0; i < LINK_MAX_PLAYERS; ++i) {
        g_link_hub.multi_data[i] = hdr.multi_data[i];
        g_link_hub.normal8_data[i] = hdr.normal8_data[i];
        g_link_hub.normal32_data[i] = hdr.normal32_data[i];
        if (g_link_hub.gba[i]) {
            GBASIOSetDriver(&g_link_hub.gba[i]->sio, &g_link_hub.drivers[i].d);
            g_link_hub.gba[i]->sio.mode = (enum GBASIOMode)hdr.sio_mode[i];
            g_link_hub.gba[i]->sio.rcnt = hdr.sio_rcnt[i];
            g_link_hub.gba[i]->sio.siocnt = hdr.sio_siocnt[i];
        }
    }

    return true;
}
