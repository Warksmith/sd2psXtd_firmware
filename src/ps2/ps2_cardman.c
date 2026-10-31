#include "ps2_cardman.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "card_emu/ps2_mc_data_interface.h"
#include "ps2_card_format.h"
#include "mmceman/ps2_mmceman.h"
#include "debug.h"
#include "game_db/game_db.h"
#include "hardware/timer.h"
#include "mmceman/ps2_mmceman_fs.h"
#include "pico/multicore.h"
#include "pico/platform.h"
#if WITH_PSRAM
    #include "ps2_dirty.h"
    #include "ps2_folder_card.h"
    #include "psram/psram.h"
#endif
#include "sd.h"
#include "settings.h"
#include "util.h"
#include "card_config.h"

#if LOG_LEVEL_PS2_CM == 0
    #define log(x...)
#else
    #define log(level, fmt, x...) LOG_PRINT(LOG_LEVEL_PS2_CM, level, fmt, ##x)
#endif

#define BLOCK_SIZE   (512)

#define CARD_HOME_ARCADE     "MemoryCards/COH"
#define CARD_HOME_CONQUEST   "MemoryCards/SC2"
#define CARD_HOME_PS2        "MemoryCards/PS2"
#define CARD_HOME_PROTO      "MemoryCards/PROT"
#define CARD_HOME_LENGTH    (17)

static int sector_count = -1;

#if WITH_PSRAM
#define SECTOR_COUNT_8MB (PS2_CARD_SIZE_8M / BLOCK_SIZE)
uint8_t available_sectors[SECTOR_COUNT_8MB / 8];  // bitmap
#define PSRAM_AVAILABLE true
#else
#define PSRAM_AVAILABLE false
#endif
static uint8_t flushbuf[BLOCK_SIZE];
int cardman_fd = -1;

int current_read_sector = 0, priority_sector = -1;

#define MAX_GAME_NAME_LENGTH (127)
#define MAX_PREFIX_LENGTH    (4)
#define MAX_PATH_LENGTH      (256)

#define MAX_SLICE_LENGTH     (30 * 1000)


static int card_variant;
static int card_idx;
static int card_chan;
static bool needs_update;
static uint32_t card_size;
static cardman_cb_t cardman_cb;
static char folder_name[MAX_FOLDER_NAME_LENGTH];
static char cardhome[CARD_HOME_LENGTH];
static uint64_t cardprog_start;
static int cardman_sectors_done;
static uint32_t cardprog_pos;

static ps2_cardman_state_t cardman_state;

static enum { CARDMAN_CREATE, CARDMAN_OPEN, CARDMAN_FOLDER_LOAD, CARDMAN_FOLDER_FILL, CARDMAN_IDLE, CARDMAN_UNKNOWN } cardman_operation = CARDMAN_UNKNOWN;

/* Until a folder card's directories exist, there is nothing to serve on demand. */
static volatile bool folder_hidden;

static bool try_set_boot_card() {
    if (!settings_get_ps2_autoboot())
        return false;

    card_idx = PS2_CARD_IDX_SPECIAL;
    card_chan = settings_get_ps2_boot_channel();
    cardman_state = PS2_CM_STATE_BOOT;
    snprintf(folder_name, sizeof(folder_name), "BOOT");
    return true;
}

static void set_default_card() {
    card_idx = settings_get_ps2_card();
    card_chan = settings_get_ps2_channel();
    cardman_state = PS2_CM_STATE_NORMAL;
    snprintf(folder_name, sizeof(folder_name), "Card%d", card_idx);
    uint8_t max_chan = card_config_get_max_channels(folder_name, folder_name);
    if (card_chan > max_chan)
        card_chan = max_chan;
}

static bool try_set_game_id_card() {
    if (!settings_get_ps2_game_id())
        return false;

    char parent_id[MAX_GAME_ID_LENGTH] = {};

    (void)game_db_get_current_parent(parent_id);

    if (!parent_id[0])
        return false;

    card_idx = PS2_CARD_IDX_SPECIAL;
    card_chan = CHAN_MIN;
    cardman_state = PS2_CM_STATE_GAMEID;
    card_config_get_card_folder(parent_id, folder_name, sizeof(folder_name));
    if (folder_name[0] == 0x00)
        snprintf(folder_name, sizeof(folder_name), "%s", parent_id);

    return true;
}

int ps2_cardman_read_sector(int sector, void *buf512) {
    if (cardman_fd < 0)
        return -1;

    if (sd_seek(cardman_fd, sector * BLOCK_SIZE, SEEK_SET) != 0)
        return -1;

    if (sd_read(cardman_fd, buf512, BLOCK_SIZE) != BLOCK_SIZE)
        return -1;

    return 0;
}

static bool try_set_next_named_card() {
    bool ret = false;
    if (cardman_state != PS2_CM_STATE_NAMED) {
        ret = try_set_named_card_folder(cardhome, 0, folder_name, sizeof(folder_name));
        if (ret)
            card_idx = 1;
    } else {
        ret = try_set_named_card_folder(cardhome, card_idx, folder_name, sizeof(folder_name));
        if (ret)
            card_idx++;
    }

    if (ret) {
        card_chan = CHAN_MIN;
        cardman_state = PS2_CM_STATE_NAMED;
    }

    return ret;
}

static bool try_set_prev_named_card() {
    bool ret = false;
    if (card_idx > 1) {
        ret = try_set_named_card_folder(cardhome, card_idx - 2, folder_name, sizeof(folder_name));
        if (ret) {
            card_idx--;
            card_chan = CHAN_MIN;
            cardman_state = PS2_CM_STATE_NAMED;
        }
    }
    return ret;
}

int ps2_cardman_write_sector(int sector, void *buf512) {
    if (cardman_fd < 0)
        return -1;

    if (sd_seek(cardman_fd, sector * BLOCK_SIZE, SEEK_SET) != 0)
        return -1;

    if (sd_write(cardman_fd, buf512, BLOCK_SIZE) != BLOCK_SIZE)
        return -1;

    return 0;
}

bool ps2_cardman_is_sector_available(int sector) {
#if WITH_PSRAM
    return available_sectors[sector / 8] & (1 << (sector % 8));
#else
    return true;
#endif
}

void ps2_cardman_mark_sector_available(int sector) {
#if WITH_PSRAM
    available_sectors[sector / 8] |= (1 << (sector % 8));
#endif
}

void ps2_cardman_forget_sectors(void) {
#if WITH_PSRAM
    memset(available_sectors, 0, sizeof(available_sectors));
#endif
}

void ps2_cardman_set_priority_sector(int sector) {
    priority_sector = sector;
}

int ps2_cardman_get_priority_sector(void) {
    int sector = priority_sector;

    return (sector != -1 && !ps2_cardman_is_sector_available(sector)) ? sector : -1;
}

void ps2_cardman_flush(void) {
    if (cardman_fd >= 0)
        sd_flush(cardman_fd);
}

static void ensuredirs(void) {
    char cardpath[CARD_HOME_LENGTH + MAX_FOLDER_NAME_LENGTH + 2];

    switch (settings_get_ps2_variant()) {
        case PS2_VARIANT_COH:
            snprintf(cardhome, sizeof(cardhome), CARD_HOME_ARCADE);
            break;
        case PS2_VARIANT_SC2:
            snprintf(cardhome, sizeof(cardhome), CARD_HOME_CONQUEST);
            break;
        case PS2_VARIANT_PROTO:
            snprintf(cardhome, sizeof(cardhome), CARD_HOME_PROTO);
            break;
        case PS2_VARIANT_RETAIL:
        default:
            snprintf(cardhome, sizeof(cardhome), CARD_HOME_PS2);
            break;
    }

    snprintf(cardpath, sizeof(cardpath), "%s/%s", cardhome, folder_name);

    sd_mkdir("MemoryCards");
    sd_mkdir(cardhome);
    sd_mkdir(cardpath);

    if (!sd_exists("MemoryCards") || !sd_exists(cardhome) || !sd_exists(cardpath))
        fatal(ERR_CARDMAN, "error creating directories");
}


static int next_sector_to_load() {
    if (priority_sector != -1) {
        if (ps2_cardman_is_sector_available(priority_sector))
            priority_sector = -1;
        else
            return priority_sector;
    }

    while (current_read_sector < sector_count) {
        if (!ps2_cardman_is_sector_available(current_read_sector))
            return current_read_sector++;
        else
            current_read_sector++;
    }

    return -1;
}

#if WITH_PSRAM
/* The snapshot being served cannot be trusted any more: the console has to be told it
 * has a different card, and that card has to be built from the folder. */
static void ps2_cardman_folder_remount(void) {
    folder_hidden = true;
    cardman_fd = -1;
    current_read_sector = 0;
    priority_sector = -1;
    cardprog_pos = 0;
    cardman_operation = CARDMAN_FOLDER_LOAD;
    ps2_mc_data_interface_card_changed();
}

static void ps2_cardman_continue_folder_load(void) {
    switch (ps2_folder_card_status()) {
        case PS2_FOLDER_LOADING:
            cardprog_pos = (uint32_t)ps2_folder_card_progress() * (card_size / 100U);
            if (cardman_cb)
                cardman_cb(ps2_folder_card_progress(), false);
            return;
        case PS2_FOLDER_ERROR:
            log(LOG_ERROR, "Folder card unavailable: %s\n", ps2_folder_card_error_text());
            /* Core 1 may be waiting for a sector that will never be loaded. */
            ps2_dirty_lock();
            memset(available_sectors, 0xFF, sizeof(available_sectors));
            ps2_dirty_unlock();
            break;
        default:
            folder_hidden = false;
            if (ps2_folder_card_filling()) {
                cardprog_pos = 0;
                cardman_operation = CARDMAN_FOLDER_FILL;
                return;
            }
            break;
    }

    cardman_operation = CARDMAN_IDLE;
    if (cardman_cb)
        cardman_cb(100, true);
}

/* Not idle, because whatever reads the card on core 0 would wait for itself. */
static void ps2_cardman_continue_folder_fill(void) {
    if (ps2_folder_card_filling()) {
        if (cardman_cb)
            cardman_cb(ps2_folder_card_progress(), false);
        return;
    }

    cardman_operation = CARDMAN_IDLE;
    if (cardman_cb)
        cardman_cb(100, true);
}
#endif

static void ps2_cardman_continue(void) {
#if WITH_PSRAM
    if (cardman_operation == CARDMAN_FOLDER_LOAD) {
        ps2_cardman_continue_folder_load();
        return;
    }
    if (cardman_operation == CARDMAN_FOLDER_FILL) {
        ps2_cardman_continue_folder_fill();
        return;
    }
#endif

    if (cardman_operation == CARDMAN_OPEN) {
        uint64_t slice_start = time_us_64();

        if (ps2_mc_data_interface_get_sdmode()) {
            uint64_t end = time_us_64();
            log(LOG_INFO, "took = %.2f s; SD read speed = %.2f kB/s\n", (end - cardprog_start) / 1e6, 1000000.0 * card_size / (end - cardprog_start) / 1024);
            if (cardman_cb)
                cardman_cb(100, true);
            cardman_operation = CARDMAN_IDLE;
        } else {
#if WITH_PSRAM
            uint8_t thisIter = 0;
            log(LOG_TRACE, "%s:%u\n", __func__, __LINE__);
            while ((ps2_mmceman_fs_idle()) && (time_us_64() - slice_start < MAX_SLICE_LENGTH)) {
                log(LOG_TRACE, "Slice!\n");

                ps2_dirty_lock();
                int sector_idx = next_sector_to_load();
                if (sector_idx == -1) {
                    ps2_dirty_unlock();
                    cardman_operation = CARDMAN_IDLE;
                    if (ps2_folder_card_active()) {
                        cardman_fd = -1;
                        ps2_folder_card_snapshot_loaded();
                    }
                    uint64_t end = time_us_64();
                    log(LOG_INFO, "took = %.2f s; SD read speed = %.2f kB/s\n", (end - cardprog_start) / 1e6,
                        1000000.0 * card_size / (end - cardprog_start) / 1024);
                    if (cardman_cb)
                        cardman_cb(100, true);
                    break;
                }

                size_t pos = sector_idx * BLOCK_SIZE;
                bool seek_ok = sd_seek(cardman_fd, pos, 0) == 0;
                bool read_ok = seek_ok && sd_read(cardman_fd, flushbuf, BLOCK_SIZE) == BLOCK_SIZE;

                if (!read_ok && ps2_folder_card_active()) {
                    ps2_dirty_unlock();
                    ps2_folder_card_snapshot_failed();
                    ps2_cardman_folder_remount();
                    return;
                }

                if (!seek_ok)
                    fatal(ERR_CARDMAN, "cannot read memcard\nseek");

                if (!read_ok)
                    fatal(ERR_CARDMAN, "cannot read memcard\nread %u", pos);

                log(LOG_TRACE, "Writing pos %u\n", pos);
                psram_write_dma(pos, flushbuf, BLOCK_SIZE, NULL);

                psram_wait_for_dma();

                ps2_cardman_mark_sector_available(sector_idx);
                ps2_dirty_unlock();

                cardprog_pos = cardman_sectors_done * BLOCK_SIZE;

                if (cardman_cb)
                    cardman_cb(100U * (uint64_t)cardprog_pos / (uint64_t)card_size, false);

                cardman_sectors_done++;
                thisIter++;
            }
            log(LOG_INFO, "ps2_cardman_continue: thisIter = %u\n", thisIter);
            log(LOG_TRACE, "%s:%u\n", __func__, __LINE__);

#endif
        }
    } else if (cardman_operation == CARDMAN_CREATE) {
        uint64_t slice_start = time_us_64();
        while ((ps2_mmceman_fs_idle()) && (time_us_64() - slice_start < MAX_SLICE_LENGTH)) {
            cardprog_pos = cardman_sectors_done * BLOCK_SIZE;
            if (cardprog_pos >= card_size) {
                sd_flush(cardman_fd);
                log(LOG_INFO, "OK!\n");

                cardman_operation = CARDMAN_IDLE;
                uint64_t end = time_us_64();

                log(LOG_INFO, "took = %.2f s; SD write speed = %.2f kB/s\n", (end - cardprog_start) / 1e6,
                    1000000.0 * card_size / (end - cardprog_start) / 1024);
                if (cardman_cb)
                    cardman_cb(100, true);

                break;
            }
            if (ps2_mc_data_interface_get_sdmode()) {
                ps2_card_format_page(card_size, cardprog_pos, flushbuf);
                sd_write(cardman_fd, flushbuf, BLOCK_SIZE);
            } else {
#if WITH_PSRAM
                ps2_dirty_lock();
                psram_wait_for_dma();

                // read back from PSRAM to make sure to retain already rewritten sectors, if any
                psram_read_dma(cardprog_pos, flushbuf, BLOCK_SIZE, NULL);
                psram_wait_for_dma();

                if (sd_write(cardman_fd, flushbuf, BLOCK_SIZE) != BLOCK_SIZE)
                    fatal(ERR_CARDMAN, "cannot init memcard");

                ps2_dirty_unlock();
#endif
            }

            if (cardman_cb)
                cardman_cb(100U * (uint64_t)cardprog_pos / (uint64_t)card_size, cardman_operation == CARDMAN_IDLE);

            cardman_sectors_done++;
        }
        sd_flush(cardman_fd);

    } else if (cardman_cb) {
        cardman_cb(100, true);
    }
}

static bool ps2_check_cardsize(uint32_t filesize) {
    switch (filesize) {
        case PS2_CARD_SIZE_512K:
        case PS2_CARD_SIZE_1M:
        case PS2_CARD_SIZE_2M:
        case PS2_CARD_SIZE_4M:
        case PS2_CARD_SIZE_8M:
        case PS2_CARD_SIZE_16M:
        case PS2_CARD_SIZE_32M:
        case PS2_CARD_SIZE_64M:
        case PS2_CARD_SIZE_128M:
        case PS2_CARD_SIZE_256M:
        case PS2_CARD_SIZE_512M:
        case PS2_CARD_SIZE_1G:
        case PS2_CARD_SIZE_2G: return true;
        default: return false;
    }
}

#if WITH_PSRAM
static void ps2_cardman_initializePSRAMCard(void) {
        // quickly generate and write an empty card into PSRAM so that it's immediately available, takes about ~0.6s
    for (size_t pos = 0; pos < card_size; pos += BLOCK_SIZE) {
        if (card_size == PS2_CARD_SIZE_8M)
            ps2_card_format_page(card_size, pos, flushbuf);
        else
            memset(flushbuf, 0xFF, BLOCK_SIZE);

        ps2_dirty_lock();
        psram_write_dma(pos, flushbuf, BLOCK_SIZE, NULL);
        psram_wait_for_dma();
        ps2_cardman_mark_sector_available(pos / BLOCK_SIZE);
        ps2_dirty_unlock();
    }
    log(LOG_TRACE, "%s created empty PSRAM image... \n", __func__);
}
#endif

static void ps2_cardman_createCard(char* path) {
    card_size = card_config_get_ps2_cardsize(folder_name, (cardman_state == PS2_CM_STATE_BOOT) ? "BootCard" : folder_name) * 1024 * 1024;
    if (card_size == 0U) {
        card_size = settings_get_ps2_cardsize() * 1024 * 1024;
    }
    cardman_fd = sd_open(path, O_RDWR | O_CREAT | O_TRUNC);
    cardman_sectors_done = 0;
    cardprog_pos = 0;
    if (card_size > PS2_CARD_SIZE_8M) {
        ps2_mc_data_interface_set_sdmode(true);
    } else {
        ps2_mc_data_interface_set_sdmode(!PSRAM_AVAILABLE);
    }

    if (cardman_fd < 0)
        fatal(ERR_CARDMAN, "cannot open for creating new card");

    log(LOG_INFO, "create new image at %s... ", path);

    if (cardman_cb)
        cardman_cb(0, false);
#if WITH_PSRAM
    if (card_size <= PS2_CARD_SIZE_8M) {
        ps2_cardman_initializePSRAMCard();
    }
#endif
}

static void ps2_cardman_resolveCardPath(char* path) {
    switch (cardman_state) {
        case PS2_CM_STATE_BOOT:
            if (card_chan == 1) {
                snprintf(path, MAX_PATH_LENGTH, "%s/%s/BootCard-%d.mcd", cardhome, folder_name, card_chan);
                if (!sd_exists(path)) {
                    // before boot card channels, boot card was located at BOOT/BootCard.mcd, for backwards compatibility check if it exists
                    snprintf(path, MAX_PATH_LENGTH, "%s/%s/BootCard.mcd", cardhome, folder_name);
                }
                if (!sd_exists(path)) {
                    // go back to BootCard-1.mcd if it doesn't
                    snprintf(path, MAX_PATH_LENGTH, "%s/%s/BootCard-%d.mcd", cardhome, folder_name, card_chan);
                }
            } else {
                snprintf(path, MAX_PATH_LENGTH, "%s/%s/BootCard-%d.mcd", cardhome, folder_name, card_chan);
            }

            settings_set_ps2_boot_channel(card_chan);
            break;
        case PS2_CM_STATE_NAMED:
        case PS2_CM_STATE_GAMEID: snprintf(path, MAX_PATH_LENGTH, "%s/%s/%s-%d.mcd", cardhome, folder_name, folder_name, card_chan); break;
        case PS2_CM_STATE_NORMAL:
            snprintf(path, MAX_PATH_LENGTH, "%s/%s/%s-%d.mcd", cardhome, folder_name, folder_name, card_chan);

            /* this is ok to do on every boot because it wouldn't update if the value is the same as currently stored */
            settings_set_ps2_card(card_idx);
            settings_set_ps2_channel(card_chan);
            break;
    }
}

static void ps2_cardman_move_card_crp(char* path) {
    char crp_path[MAX_PATH_LENGTH] = {};
    for (int i = 0; i < 512; i++) {
        snprintf(crp_path, sizeof(crp_path), "%s/%s/%s-%d.crp.%i", cardhome, folder_name, folder_name, card_chan, i);
        if (!sd_exists(crp_path))
            break;
    }
    if (!crp_path[0])
        fatal(ERR_CARDMAN, "Card %d Chann %d invalid size and cannot create .crp file", card_idx, card_chan);
    sd_close(cardman_fd);
    sd_rename(path, crp_path);
    log(LOG_ERROR, "Card %d Chann %d has invalid size and was renamed to %s\n", card_idx, card_chan, crp_path);
}

#if WITH_PSRAM
static void ps2_cardman_open_folder(void) {
    const char *base = (cardman_state == PS2_CM_STATE_BOOT) ? "BootCard" : folder_name;
    uint8_t configured_size = card_config_get_ps2_cardsize(folder_name, base);
    char path[MAX_PATH_LENGTH];

    folder_hidden = true;
    card_size = PS2_CARD_SIZE_8M;
    sector_count = card_size / BLOCK_SIZE;
    cardprog_pos = 0;
    cardprog_start = time_us_64();
    cardman_operation = CARDMAN_FOLDER_LOAD;
    ps2_mc_data_interface_set_sdmode(false);
    ps2_mc_data_interface_card_changed();

    if (cardman_cb)
        cardman_cb(0, false);

    if (configured_size != 0 && configured_size != 8) {
        log(LOG_ERROR, "Folder cards are always 8 MB, CardSize=%u refused\n", configured_size);
        ps2_folder_card_reject();
        return;
    }

    snprintf(path, sizeof(path), "%s/%s/%s-%d.folder", cardhome, folder_name, base, card_chan);
    log(LOG_INFO, "Switching to folder card path = %s\n", path);
    ps2_folder_card_open(path, card_config_get_ps2_folder_snapshot(folder_name, base));

    /* Same as opening an image from here on: what is not in PSRAM yet is read on demand. */
    int fd = ps2_folder_card_snapshot_open();
    if (fd >= 0) {
        ps2_dirty_lock();
        memset(available_sectors, 0, sizeof(available_sectors));
        ps2_dirty_unlock();
        cardman_fd = fd;
        current_read_sector = 0;
        priority_sector = -1;
        cardman_sectors_done = 0;
        cardman_operation = CARDMAN_OPEN;
        folder_hidden = false;
        return;
    }

    /* The task does not run until the GUI is up, which is a long time at power-on. */
    ps2_folder_card_build();
    ps2_cardman_continue_folder_load();
}

/* Writes made while a card loads cannot be saved from a half-filled PSRAM, and stay
 * queued because the dirty task waits for the load. */
static bool ps2_cardman_finish_folder_load(void) {
    if (cardman_operation != CARDMAN_OPEN && cardman_operation != CARDMAN_FOLDER_FILL)
        return true;
    while (cardman_operation == CARDMAN_OPEN || cardman_operation == CARDMAN_FOLDER_FILL) {
        if (!ps2_mmceman_fs_idle())
            return false;
        ps2_cardman_task();
    }
    ps2_dirty_activity = 1;
    ps2_mc_data_interface_flush();
    return true;
}
#endif

void ps2_cardman_open(void) {
    char path[MAX_PATH_LENGTH];

    needs_update = false;

    ensuredirs();

    ps2_cardman_resolveCardPath(path);

    if (card_config_get_ps2_folder_backend(folder_name, (cardman_state == PS2_CM_STATE_BOOT) ? "BootCard" : folder_name, path,
                                           cardman_state == PS2_CM_STATE_BOOT)) {
#if WITH_PSRAM
        ps2_cardman_open_folder();
        return;
#else
        log(LOG_ERROR, "StorageBackend=FOLDER needs PSRAM, using the image instead\n");
#endif
    }

    log(LOG_INFO, "Switching to card path = %s\n", path);

    ps2_mc_data_interface_card_changed();

    if (!sd_exists(path)) {
        cardman_operation = CARDMAN_CREATE;
        ps2_cardman_createCard(path);
    } else {
        cardman_fd = sd_open(path, O_RDWR);
        card_size = sd_filesize(cardman_fd);
        cardman_operation = CARDMAN_OPEN;
        cardprog_pos = 0;
        cardman_sectors_done = 0;

        if (cardman_fd < 0)
            fatal(ERR_CARDMAN, "cannot open card");

        if (ps2_check_cardsize(card_size)) {
            cardman_operation = CARDMAN_OPEN;
            cardprog_pos = 0;
            cardman_sectors_done = 0;
            if (card_size > PS2_CARD_SIZE_8M) {
                ps2_mc_data_interface_set_sdmode(true);
            } else {
                ps2_mc_data_interface_set_sdmode(!PSRAM_AVAILABLE);
            }
            log(LOG_INFO, "reading card (%lu KB).... ", (uint32_t)(card_size / 1024));
            if (cardman_cb)
                cardman_cb(0, false);
        } else {
            cardman_operation = CARDMAN_CREATE;
            ps2_cardman_move_card_crp(path);
            ps2_cardman_createCard(path);
        }
    }
    cardprog_start = time_us_64();

    sector_count = card_size / BLOCK_SIZE;

    log(LOG_INFO, "Open Finished!\n");
}

bool ps2_cardman_prepare_close(void) {
#if WITH_PSRAM
    if (ps2_folder_card_active()) {
        if (ps2_folder_card_status() == PS2_FOLDER_DIRTY && !ps2_cardman_finish_folder_load())
            return false;
        return ps2_folder_card_flush();
    }
#endif
    return true;
}

bool ps2_cardman_can_unload(void) {
#if WITH_PSRAM
    if (ps2_folder_card_active()) {
        ps2_folder_status_t status = ps2_folder_card_status();
        /* Nothing is lost by dropping a snapshot: it is only a shortcut for the next mount. */
        return status == PS2_FOLDER_CLEAN || status == PS2_FOLDER_ERROR || status == PS2_FOLDER_CACHING
               || status == PS2_FOLDER_VERIFYING || status == PS2_FOLDER_FILLING;
    }
#endif
    return true;
}

void ps2_cardman_close(void) {
#if WITH_PSRAM
    if (ps2_folder_card_active()) {
        folder_hidden = true;
        if (ps2_folder_card_status() == PS2_FOLDER_DIRTY)
            ps2_cardman_finish_folder_load();
        if (!ps2_folder_card_flush())
            log(LOG_ERROR, "Closing folder card with unsaved data\n");
        ps2_folder_card_close();
        if (cardman_operation == CARDMAN_OPEN || cardman_operation == CARDMAN_FOLDER_FILL)
            cardman_operation = CARDMAN_IDLE;
        cardman_fd = -1;
        current_read_sector = 0;
        priority_sector = -1;
        memset(available_sectors, 0, sizeof(available_sectors));
        folder_hidden = false;
        return;
    }
#endif

    if (cardman_fd < 0)
        return;
    ps2_cardman_flush();
    sd_close(cardman_fd);
    cardman_fd = -1;
    current_read_sector = 0;
    priority_sector = -1;
#if WITH_PSRAM
    memset(available_sectors, 0, sizeof(available_sectors));
#endif
}

void ps2_cardman_set_channel(uint16_t chan_num) {
    uint8_t max_chan = card_config_get_max_channels(folder_name, (cardman_state == PS2_CM_STATE_BOOT) ? "BootCard" : folder_name);
    if (chan_num <= max_chan && chan_num >= CHAN_MIN) {
        needs_update |= (chan_num != card_chan);
        card_chan = chan_num;
    }
}

void ps2_cardman_next_channel(void) {
    uint8_t max_chan = card_config_get_max_channels(folder_name, (cardman_state == PS2_CM_STATE_BOOT) ? "BootCard" : folder_name);
    card_chan += 1;
    if (card_chan > max_chan)
#if WITH_GUI
        card_chan = CHAN_MIN;
#else
        card_chan = max_chan; //dont jump to CHAN_MIN. Otherwise without display, you cant see where you actually are.
#endif
    needs_update = true;
}

void ps2_cardman_prev_channel(void) {
    uint8_t max_chan = card_config_get_max_channels(folder_name, (cardman_state == PS2_CM_STATE_BOOT) ? "BootCard" : folder_name);
    card_chan -= 1;
    if (card_chan < CHAN_MIN)
#if WITH_GUI
        card_chan = max_chan;
#else
        card_chan = CHAN_MIN; //dont jump to max_chan. Otherwise without display, you cant see where you actually are.
#endif
    needs_update = true;
}

//TEMP
void ps2_cardman_switch_bootcard(void) {
    if (try_set_boot_card())
        needs_update = true;
}

void ps2_cardman_set_idx(uint16_t idx_num) {
    if ((idx_num >= IDX_MIN) && (idx_num < UINT16_MAX)) {
        needs_update |= (idx_num != card_idx);
        card_idx = idx_num;
        card_chan = CHAN_MIN;
        cardman_state = PS2_CM_STATE_NORMAL;
    }
    snprintf(folder_name, sizeof(folder_name), "Card%d", card_idx);
}

void ps2_cardman_next_idx(void) {
    switch (cardman_state) {
        case PS2_CM_STATE_NAMED:
            if (!try_set_prev_named_card()
                && !try_set_boot_card()
                && !try_set_game_id_card())
                set_default_card();
            break;
        case PS2_CM_STATE_BOOT:
            if (!try_set_game_id_card())
                set_default_card();
            break;
        case PS2_CM_STATE_GAMEID: set_default_card(); break;
        case PS2_CM_STATE_NORMAL:
            card_idx += 1;
            card_chan = CHAN_MIN;
            if (card_idx > UINT16_MAX)
                card_idx = UINT16_MAX;
            uint8_t maxcards = settings_get_ps2_maxcardidx();
            if (maxcards != 0) //0 = unlimited cards UINT16_MAX
            {
                if (card_idx > maxcards)
                    card_idx = maxcards;
            }
            snprintf(folder_name, sizeof(folder_name), "Card%d", card_idx);
            break;
    }

    needs_update = true;
}

void ps2_cardman_prev_idx(void) {
    switch (cardman_state) {
        case PS2_CM_STATE_NAMED:
        case PS2_CM_STATE_BOOT:
            if (!try_set_next_named_card())
                set_default_card();
            break;
        case PS2_CM_STATE_GAMEID:
            if (!try_set_boot_card())
                if (!try_set_next_named_card())
                    set_default_card();
            break;
        case PS2_CM_STATE_NORMAL:
            card_idx -= 1;
            card_chan = CHAN_MIN;
            if (card_idx <= PS2_CARD_IDX_SPECIAL) {
                if (!try_set_game_id_card() && !try_set_boot_card() && !try_set_next_named_card())
                    set_default_card();
            } else {
                snprintf(folder_name, sizeof(folder_name), "Card%d", card_idx);
            }
            break;
    }

    needs_update = true;
}

int ps2_cardman_get_idx(void) {
    return card_idx;
}

int ps2_cardman_get_channel(void) {
    return card_chan;
}

void ps2_cardman_set_gameid(const char *const card_game_id) {
    if (!settings_get_ps2_game_id())
        return;

    char new_folder_name[MAX_FOLDER_NAME_LENGTH] = {};
    if (card_game_id[0]) {
        card_config_get_card_folder(card_game_id, new_folder_name, sizeof(new_folder_name));
        if (new_folder_name[0] == 0x00)
            snprintf(new_folder_name, sizeof(new_folder_name), "%s", card_game_id);
        log(LOG_TRACE, "Folder: %s\n", new_folder_name);
        if ((strcmp(new_folder_name, folder_name) != 0) || (PS2_CM_STATE_GAMEID != cardman_state)) {
            card_idx = PS2_CARD_IDX_SPECIAL;
            cardman_state = PS2_CM_STATE_GAMEID;
            card_chan = CHAN_MIN;
            memcpy(folder_name, new_folder_name, sizeof(folder_name));
            needs_update = true;
        }
    }
}

void ps2_cardman_set_progress_cb(cardman_cb_t func) {
    cardman_cb = func;
}

char *ps2_cardman_get_progress_text(void) {
    static char progress[32];

    /* The display fits 16 characters here. */
    if (cardman_operation == CARDMAN_FOLDER_LOAD)
        snprintf(progress, sizeof(progress), "Loading folder..");
    else if (cardman_operation == CARDMAN_FOLDER_FILL)
        snprintf(progress, sizeof(progress), "Reading files...");
#if WITH_PSRAM
    else if (cardman_operation == CARDMAN_OPEN && ps2_folder_card_active())
        snprintf(progress, sizeof(progress), "Snap %.0f kB/s", 1000000.0 * cardprog_pos / (time_us_64() - cardprog_start) / 1024);
#endif
    else if (cardman_operation != CARDMAN_IDLE)
        snprintf(progress, sizeof(progress), "%s %.2f kB/s", cardman_operation == CARDMAN_CREATE ? "Wr" : "Rd",
                 1000000.0 * cardprog_pos / (time_us_64() - cardprog_start) / 1024);
    else
        snprintf(progress, sizeof(progress), "Switching...");

    return progress;
}

uint32_t ps2_cardman_get_card_size(void) {
    return card_size;
}

const char *ps2_cardman_get_folder_name(void) {
    return folder_name;
}

ps2_cardman_state_t ps2_cardman_get_state(void) {
    return cardman_state;
}

void ps2_cardman_force_update(void) {
    needs_update = true;
}

void ps2_cardman_set_variant(int variant) {
    if (variant != card_variant) {
        settings_set_ps2_variant(variant);
        if (!try_set_boot_card()) {
            card_idx = 1;
            card_chan = 1;
            cardman_state = PS2_CM_STATE_NORMAL;
            snprintf(folder_name, sizeof(folder_name), "Card%d", card_idx);
        }
        card_variant = variant;
    }
    needs_update = true;
}

bool ps2_cardman_needs_update(void) {
    return needs_update;
}

bool __time_critical_func(ps2_cardman_is_accessible)(void) {
    // SD: X IDLE   => X
    // SD: X CREATE => /
    // SD: X OPEN =>   /
    // SD: / IDLE   => X
    // SD: / CREATE => X
    // SD: / OPEN   => X
    if (folder_hidden || cardman_operation == CARDMAN_UNKNOWN)
        return false;
    else if (ps2_mc_data_interface_get_sdmode())
        return (cardman_operation != CARDMAN_CREATE);
    else
        return true;
}

bool __time_critical_func(ps2_cardman_hidden_from_console)(void) {
    return folder_hidden || (!ps2_cardman_is_accessible() && (cardman_state == PS2_CM_STATE_GAMEID));
}

const char *ps2_cardman_get_storage_status(void) {
#if WITH_PSRAM
    switch (ps2_folder_card_status()) {
        case PS2_FOLDER_LOADING: return "Loading card...";
        case PS2_FOLDER_FILLING: return "Reading files...";
        case PS2_FOLDER_VERIFYING: return "Checking card...";
        case PS2_FOLDER_CACHING: return "Caching card...";
        case PS2_FOLDER_DIRTY: return "Unsaved changes";
        case PS2_FOLDER_SYNCING: return "Saving card...";
        case PS2_FOLDER_SYNC_FAILED: return "Save failed!";
        case PS2_FOLDER_ERROR: return ps2_folder_card_error_text();
        default: break;
    }
#endif
    return NULL;
}

bool ps2_cardman_owns_fd(int fd) {
#if WITH_PSRAM
    return ps2_folder_card_owns_fd(fd);
#else
    (void)fd;
    return false;
#endif
}

bool ps2_cardman_path_protected(const char *path) {
#if WITH_PSRAM
    return ps2_folder_card_path_protected(path);
#else
    (void)path;
    return false;
#endif
}

bool ps2_cardman_is_idle(void) {
    return cardman_operation == CARDMAN_IDLE;
}

void ps2_cardman_init(void) {
    card_variant = settings_get_ps2_variant();
    if (!try_set_boot_card())
        set_default_card();

    cardman_operation = CARDMAN_UNKNOWN;
}

void ps2_cardman_task(void) {
#if WITH_PSRAM
    ps2_folder_card_task();
    if (ps2_folder_card_take_remount())
        ps2_cardman_folder_remount();
#endif
    ps2_cardman_continue();
}
