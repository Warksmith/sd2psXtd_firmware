#pragma once

/* Folder-backed PS2 memory card: the card lives in PSRAM while mounted and persists
 * as ordinary files on SD. See doc/folder-cards.md. */

#include <stdbool.h>
#include <stdint.h>

typedef enum {
    PS2_FOLDER_OFF,         /* no folder card mounted */
    PS2_FOLDER_LOADING,     /* being built, hidden from the console */
    PS2_FOLDER_FILLING,     /* served while its files are still being read from SD */
    PS2_FOLDER_VERIFYING,   /* served from its snapshot, which is still being checked */
    PS2_FOLDER_CLEAN,       /* SD matches PSRAM */
    PS2_FOLDER_CACHING,     /* as CLEAN, while the snapshot is brought up to date */
    PS2_FOLDER_DIRTY,       /* PSRAM holds changes that are not on SD yet */
    PS2_FOLDER_SYNCING,
    PS2_FOLDER_SYNC_FAILED, /* still served from PSRAM; SD lags behind and retries */
    PS2_FOLDER_ERROR,       /* could not be mounted, stays hidden */
} ps2_folder_status_t;

/* Starts mounting the folder card at path, keeping a snapshot of it if asked to. */
void ps2_folder_card_open(const char *path, bool snapshot);
/* Tries to skip the mount. On success the caller loads the returned fd into PSRAM
 * and reports back; the card may be served meanwhile. Otherwise returns -1, and
 * ps2_folder_card_task() goes on building the card. */
int ps2_folder_card_snapshot_open(void);
void ps2_folder_card_snapshot_loaded(void);
/* Runs the mount as far as it can be published, within reason. Worth doing rather than
 * leaving it to ps2_folder_card_task(), which may not run for a while during boot. */
void ps2_folder_card_build(void);
/* Gives up on the snapshot and builds the card from its folder after all. */
void ps2_folder_card_snapshot_failed(void);
/* True once after the card stopped being what the console was served. */
bool ps2_folder_card_take_remount(void);
/* Refuses the mount, for a configuration that does not allow folder mode. */
void ps2_folder_card_reject(void);
/* One bounded slice of whatever the card needs. Call regularly on core 0. */
void ps2_folder_card_task(void);

ps2_folder_status_t ps2_folder_card_status(void);
int ps2_folder_card_progress(void);
/* True while PSRAM lacks file contents, which only ps2_folder_card_task() provides. */
bool ps2_folder_card_filling(void);
const char *ps2_folder_card_error_text(void);
bool ps2_folder_card_active(void);

/* Records a page drained from the dirty heap. Core 0 only. */
void ps2_folder_card_note_dirty(uint32_t page);

/* Blocking final synchronization once the console is stopped. Returns false if
 * unsaved data remains, in which case the card must not be replaced. */
bool ps2_folder_card_flush(void);
void ps2_folder_card_close(void);

#ifdef PS2_FOLDER_HOST_TEST
/* Everything in RAM is gone: nothing on SD is touched on the way down. */
void ps2_folder_card_power_loss(void);
#endif

bool ps2_folder_card_owns_fd(int fd);
/* True if path is inside the mounted card's folder, which only the firmware may modify. */
bool ps2_folder_card_path_protected(const char *path);
