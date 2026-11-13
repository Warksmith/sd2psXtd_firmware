#pragma once

/* State shared by the folder-card importer, synchronizer and lifecycle manager.
 * One card is mounted at a time, so a single static context keeps the SRAM budget
 * explicit; see doc/folder-cards.md for the breakdown. */

#include <stdbool.h>
#include <stdint.h>

#include "ps2_folder_fs.h"
#include "ps2_folder_meta.h"

#define PF_MAX_ENTRIES      2048U   /* files and directories on a card */
#define PF_MAX_DIR_ENTRIES  1024U   /* live entries in one directory */
#define PF_BASE_PATH_MAX    128U
#define PF_CHAIN_START      0x811C9DC5U

typedef enum {
    PF_OK = 0,
    PF_ERR_CAPACITY,    /* the host tree does not fit on an 8 MB card */
    PF_ERR_NAME,        /* a host name has no valid PS2 mapping */
    PF_ERR_LIMIT,       /* entry count or depth limit exceeded */
    PF_ERR_IO,          /* SD failure */
    PF_ERR_INVALID_FS,  /* the live filesystem is not coherent */
    PF_ERR_RECOVERY,    /* an interrupted transaction cannot be completed */
    PF_ERR_CONFIG,      /* the card configuration does not allow folder mode */
} pf_error_t;

typedef enum {
    PF_STEP_RUN,        /* more work, call again */
    PF_STEP_WAIT,       /* blocked on console activity, call again later */
    PF_STEP_DONE,
    PF_STEP_FAILED,     /* see pf.error */
} pf_step_t;

typedef struct {
    char base[PF_BASE_PATH_MAX];
    char path[PFM_PATH_MAX];
    pfs_t fs;
    pf_error_t error;

    /* Last committed state: index.bin on SD, plus one hash per record here. */
    bool base_valid;
    uint32_t base_count;
    uint32_t base_crc;
    uint32_t base_hash[PF_MAX_ENTRIES];

    uint32_t fold[PF_MAX_DIR_ENTRIES];
    uint16_t map[PF_MAX_ENTRIES];                   /* scanned record -> base record */
    uint8_t base_seen[PF_MAX_ENTRIES / 8];
    uint8_t touched[PFS_TOTAL_CLUSTERS / 8];        /* written since the last scan */
    uint8_t scan_touched[PFS_TOTAL_CLUSTERS / 8];   /* written since the last commit */
    uint8_t visited[PFS_TOTAL_CLUSTERS / 8];

    uint8_t io[PFS_CLUSTER_SIZE];
} pf_ctx_t;

extern pf_ctx_t pf;

static inline bool pf_bit_get(const uint8_t *map, uint32_t bit) {
    return (map[bit / 8] >> (bit % 8)) & 1U;
}

static inline void pf_bit_set(uint8_t *map, uint32_t bit) {
    map[bit / 8] |= (uint8_t)(1U << (bit % 8));
}

/* Hash of a cluster chain, see pfm_record_t. */
static inline uint32_t pf_chain_add(uint32_t chain, uint32_t cluster) {
    return (chain ^ cluster) * 0x01000193U;
}

uint32_t pf_base_key(uint32_t parent, uint32_t fold);
/* Rebuilds the base table from index.bin; a missing or damaged index leaves it invalid. */
void pf_base_load(void);
/* Absolute path of a card-relative path in pf.path. */
bool pf_abs(const char *rel);
/* Creates the directories metadata goes into. */
bool pf_meta_dirs(void);
void pf_fill_record(pfm_record_t *rec, const pfs_entry_t *e, uint32_t parent);

void pf_import_start(void);
/* Done means the card can be published, and pf_fill_start() has been called. */
pf_step_t pf_import_step(void);
int pf_import_progress(void);
void pf_import_abort(void);
/* True if the card came from the index alone, because the folder matched its stamp. */
bool pf_import_trusted(void);
#ifdef PS2_FOLDER_HOST_TEST
void pf_import_power_loss(void);
#endif

/* Loads what the importer left out, see ps2_folder_fill.c. Done means PSRAM is complete
 * and pf_sync_commit_index() has been called. */
void pf_fill_start(int list_fd, uint32_t count, uint32_t free_cluster);
pf_step_t pf_fill_step(void);
int pf_fill_progress(void);
void pf_fill_abort(void);
#ifdef PS2_FOLDER_HOST_TEST
void pf_fill_power_loss(void);
#endif

typedef enum {
    PF_SCAN_IMPORT, /* no scan: the importer's index, committed unless it changed nothing */
    PF_SCAN_SYNC,   /* background: yields to the console, restarts on new writes */
    PF_SCAN_FINAL,  /* console stopped: runs to completion */
} pf_scan_mode_t;

typedef enum {
    PF_SYNC_COMMITTED,
    PF_SYNC_NO_CHANGE,
    PF_SYNC_INTERRUPTED,    /* the console wrote during the scan */
    PF_SYNC_BUSY,           /* a file is open for writing on the console */
    PF_SYNC_FAILED,         /* pf.error says why; nothing was lost */
} pf_sync_result_t;

/* Resolve whatever an earlier power cycle left behind. */
void pf_recover_start(void);
void pf_sync_start(pf_scan_mode_t mode, bool accept_open_files);
/* Makes an unsealed index in the stage directory the committed state, taking over its
 * fd. It describes the folder, so PSRAM plays no part in this and the console may run. */
void pf_sync_commit_index(int index_fd, uint32_t count);
pf_step_t pf_sync_step(void);
pf_sync_result_t pf_sync_result(void);
/* True between the commit point and the end of apply: must be completed, not dropped. */
bool pf_sync_committed(void);
/* Restart applying a committed transaction after a failure. */
void pf_sync_resume(void);
/* Drop an uncommitted scan. */
void pf_sync_abort(void);
uint32_t pf_sync_generation(void);
#ifdef PS2_FOLDER_HOST_TEST
void pf_sync_power_loss(void);
#endif

/* Optional copy of the card, see ps2_folder_snapshot.c. */
void pf_snapshot_reset(bool enabled);
void pf_snapshot_close(void);
/* The snapshot of the committed state, opened for reading, or -1 if there is none.
 * Blocks while it compares the folder, for at most budget_us. */
int pf_snapshot_open(uint64_t budget_us);
void pf_snapshot_check_start(void);
pf_step_t pf_snapshot_check_step(void);
bool pf_snapshot_check_passed(void);
/* Stop trusting what is on SD. */
void pf_snapshot_discard(void);
void pf_snapshot_mark(uint32_t cluster);
void pf_snapshot_mark_all(void);
bool pf_snapshot_outdated(void);
uint32_t pf_snapshot_backlog(void);
void pf_snapshot_write_start(bool final);
pf_step_t pf_snapshot_write_step(void);
void pf_snapshot_write_abort(void);

/* A stamp saying the folder still is what index.bin describes, which spares a mount
 * from looking at every file. Blocks while it compares, for at most budget_us. */
void pf_tree_reset(void);
bool pf_tree_unchanged(uint64_t budget_us);
bool pf_tree_stale(void);
void pf_tree_write_start(void);
pf_step_t pf_tree_write_step(void);
void pf_tree_write_abort(void);
#ifdef PS2_FOLDER_HOST_TEST
void pf_snapshot_power_loss(void);
#endif
