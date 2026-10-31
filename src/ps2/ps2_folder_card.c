#include "ps2_folder_card.h"

#include <stdio.h>
#include <string.h>

#include "ps2_folder_internal.h"
#include "ps2_folder_port.h"
#include "sd.h"

#define TASK_SLICE_US       (20U * 1000U)
#define MOUNT_SLICE_US      (100U * 1000U)      /* hidden from the console, so nothing is waiting */
#define SYNC_QUIET_US       (1500U * 1000U)     /* no console writes for this long */
#define SYNC_RETRY_US       (5000U * 1000U)     /* after a failed attempt */
#define SYNC_BUSY_RETRY_US  (1000U * 1000U)     /* a file is open on the console */
#define OPEN_FILE_LIMIT_US  (30U * 1000U * 1000U) /* stop waiting for a crashed writer */
#define SNAPSHOT_OPEN_US    (2000U * 1000U)     /* the console starts looking for its card */
#define BUILD_OPEN_US       (2000U * 1000U)     /* the rest is left to the task */
#define SNAPSHOT_FLUSH_MAX  512U                /* clusters a card switch will wait for */

typedef enum {
    PHASE_OFF,
    PHASE_RECOVER,
    PHASE_IMPORT,
    PHASE_FILL,
    PHASE_INDEX,
    PHASE_SNAPSHOT_LOAD,
    PHASE_SNAPSHOT_CHECK,
    PHASE_READY,
    PHASE_SYNC,
    PHASE_SNAPSHOT_WRITE,
    PHASE_TREE_WRITE,
    PHASE_APPLY_RETRY,
    PHASE_FAILED,
} phase_t;

static phase_t phase;
static uint32_t synced_gen;
static uint32_t seen_gen;
static uint64_t seen_gen_time;
static uint64_t retry_at;
static uint64_t busy_since;
static bool sync_failed;
static uint64_t mount_started;
static bool remount;
static bool snapshot_wanted;

static inline uint32_t mount_ms(void) {
    return (uint32_t)((pf_time_us() - mount_started) / 1000U);
}

static bool has_unsynced_writes(void);

/* Once the console has the card, nothing in PSRAM can be saved without a complete card,
 * so what it wrote since is gone. SD still holds everything from before. */
static void fail_mount(pf_error_t error) {
    bool published = phase == PHASE_FILL || phase == PHASE_INDEX;

    if (error != PF_OK)
        pf.error = error;
    if (published && has_unsynced_writes()) {
        PF_LOG("Writes made to this card are dropped");
    }
    pf_import_abort();
    pf_fill_abort();
    phase = PHASE_FAILED;
    if (published)
        remount = true;
    PF_LOG("Card unavailable: %s", ps2_folder_card_error_text());
}

void ps2_folder_card_open(const char *path, bool snapshot) {
    uint32_t gen;
    bool pending;

    snprintf(pf.base, sizeof(pf.base), "%s", path);
    pf.error = PF_OK;
    memset(pf.touched, 0, sizeof(pf.touched));
    memset(pf.scan_touched, 0, sizeof(pf.scan_touched));
    sync_failed = false;
    retry_at = 0;
    busy_since = 0;
    remount = false;
    mount_started = pf_time_us();
    snapshot_wanted = snapshot;
    pf_snapshot_reset(snapshot);
    pf_tree_reset();

    pf_write_snapshot(&gen, &pending);
    synced_gen = seen_gen = gen;
    seen_gen_time = pf_time_us();

    PF_LOG("Selected backend: FOLDER");
    PF_LOG("Card: %s", path);

    if (sd_mkdir(path) != 0) {
        fail_mount(PF_ERR_IO);
        return;
    }
    pf_recover_start();
    phase = PHASE_RECOVER;
}

void ps2_folder_card_reject(void) {
    pf.error = PF_ERR_CONFIG;
    pf.base[0] = 0;
    phase = PHASE_FAILED;
}

static void card_published(void) {
    uint32_t gen;
    bool pending;

    pf_write_snapshot(&gen, &pending);
    synced_gen = seen_gen = gen;
    seen_gen_time = pf_time_us();
    phase = PHASE_READY;
    PF_LOG("Card ready after %lu ms", (unsigned long)mount_ms());
    pf_log_console_polls();
}

static void mount_step(void) {
    pf_step_t step;

    if (phase == PHASE_RECOVER) {
        step = pf_sync_step();
        if (step == PF_STEP_DONE) {
            if (pf_sync_result() == PF_SYNC_FAILED) {
                fail_mount(PF_OK);
            } else {
                PF_LOG("Recovery done at %lu ms", (unsigned long)mount_ms());
                pf_import_start();
                phase = PHASE_IMPORT;
            }
        }
    } else if (phase == PHASE_IMPORT) {
        step = pf_import_step();
        if (step == PF_STEP_FAILED) {
            fail_mount(PF_OK);
        } else if (step == PF_STEP_DONE) {
            PF_LOG("Built from %s", pf_import_trusted() ? "the index" : "the folder");
            card_published();
            pf_snapshot_mark_all();
            phase = PHASE_FILL;
        }
    } else if (phase == PHASE_FILL) {
        step = pf_fill_step();
        if (step == PF_STEP_FAILED) {
            fail_mount(PF_OK);
        } else if (step == PF_STEP_DONE) {
            PF_LOG("Files loaded at %lu ms", (unsigned long)mount_ms());
            phase = PHASE_INDEX;
        }
    } else {
        step = pf_sync_step();
        if (step == PF_STEP_DONE) {
            if (pf_sync_result() == PF_SYNC_FAILED && pf.error != PF_ERR_IO) {
                fail_mount(PF_OK);
            } else {
                /* An index that could not be stored is only a lost optimization: the
                 * card is intact, the next sync compares file contents instead. */
                sync_failed = pf_sync_result() == PF_SYNC_FAILED;
                if (pf_sync_result() != PF_SYNC_COMMITTED)
                    pf_base_load(); /* the fill borrowed its table */
                phase = PHASE_READY;
                PF_LOG("Card complete at %lu ms", (unsigned long)mount_ms());
                pf_log_console_polls();
                if (pf_sync_committed()) {
                    retry_at = pf_time_us() + SYNC_RETRY_US;
                    phase = PHASE_APPLY_RETRY;
                }
            }
        }
    }
}

static void note_generation(uint64_t now) {
    uint32_t gen;
    bool pending;

    pf_write_snapshot(&gen, &pending);
    if (gen != seen_gen) {
        seen_gen = gen;
        seen_gen_time = now;
    }
}

static void sync_finished(uint64_t now) {
    switch (pf_sync_result()) {
        case PF_SYNC_COMMITTED:
            PF_LOG("Synchronization committed");
            /* fall through */
        case PF_SYNC_NO_CHANGE:
            synced_gen = pf_sync_generation();
            sync_failed = false;
            busy_since = 0;
            phase = PHASE_READY;
            break;
        case PF_SYNC_INTERRUPTED:
            phase = PHASE_READY;
            break;
        case PF_SYNC_BUSY:
            if (busy_since == 0)
                busy_since = now;
            retry_at = now + SYNC_BUSY_RETRY_US;
            phase = PHASE_READY;
            break;
        case PF_SYNC_FAILED:
            PF_LOG("Unsynchronized data remains: %s", ps2_folder_card_error_text());
            sync_failed = true;
            retry_at = now + SYNC_RETRY_US;
            phase = pf_sync_committed() ? PHASE_APPLY_RETRY : PHASE_READY;
            break;
    }
}

static bool has_unsynced_writes(void) {
    uint32_t gen;
    bool pending;

    pf_write_snapshot(&gen, &pending);
    return gen != synced_gen;
}

int ps2_folder_card_snapshot_open(void) {
    uint64_t start = pf_time_us();
    int fd;

    if (!snapshot_wanted)
        return -1;

    /* A transaction left by a power cut decides what the committed state is. */
    while (phase == PHASE_RECOVER) {
        if (!pf_sd_available() || pf_time_us() - start > SNAPSHOT_OPEN_US)
            return -1;
        mount_step();
    }
    if (phase != PHASE_IMPORT)
        return -1;

    fd = pf_snapshot_open(SNAPSHOT_OPEN_US);
    if (fd < 0)
        return -1;

    pf_import_abort();
    card_published();
    phase = PHASE_SNAPSHOT_LOAD;
    PF_LOG("Serving snapshot at %lu ms", (unsigned long)mount_ms());
    pf_log_console_polls();
    return fd;
}

void ps2_folder_card_build(void) {
    uint64_t start = pf_time_us();

    while ((phase == PHASE_RECOVER || phase == PHASE_IMPORT) && pf_sd_available()
           && pf_time_us() - start < BUILD_OPEN_US)
        mount_step();
}

void ps2_folder_card_snapshot_loaded(void) {
    PF_LOG("Snapshot loaded at %lu ms", (unsigned long)mount_ms());
    pf_snapshot_check_start();
    phase = PHASE_SNAPSHOT_CHECK;
}

void ps2_folder_card_snapshot_failed(void) {
    PF_LOG("Snapshot unusable, loading from the folder");
    if (has_unsynced_writes()) {
        PF_LOG("Writes made to the snapshot are dropped");
    }
    pf_snapshot_discard();
    sync_failed = false;
    pf_import_start();
    phase = PHASE_IMPORT;
}

static void snapshot_checked(void) {
    if (pf_snapshot_check_passed()) {
        PF_LOG("Snapshot verified at %lu ms", (unsigned long)mount_ms());
        pf_log_console_polls();
        phase = PHASE_READY;
    } else {
        ps2_folder_card_snapshot_failed();
        remount = true;
    }
}

bool ps2_folder_card_take_remount(void) {
    bool result = remount;

    remount = false;
    return result;
}

static void ready_step(uint64_t now) {
    bool unsynced = has_unsynced_writes() || sync_failed;
    bool tree = pf_tree_stale();

    if ((!unsynced && !tree && !pf_snapshot_outdated()) || now < retry_at || now - seen_gen_time < SYNC_QUIET_US)
        return;
    if (!pf_console_quiet())
        return;

    if (!unsynced && tree) {
        pf_tree_write_start();
        phase = PHASE_TREE_WRITE;
        return;
    }

    if (!unsynced) {
        pf_snapshot_write_start(false);
        phase = PHASE_SNAPSHOT_WRITE;
        return;
    }

    PF_LOG("Reconciliation started");
    pf_sync_start(PF_SCAN_SYNC, busy_since != 0 && now - busy_since > OPEN_FILE_LIMIT_US);
    phase = PHASE_SYNC;
}

void ps2_folder_card_task(void) {
    uint64_t start = pf_time_us();
    uint64_t slice = TASK_SLICE_US;

    if (phase == PHASE_OFF || phase == PHASE_FAILED || !pf_sd_available())
        return;
    if (ps2_folder_card_status() == PS2_FOLDER_LOADING)
        slice = MOUNT_SLICE_US;

    do {
        uint64_t now = pf_time_us();

        note_generation(now);

        switch (phase) {
            case PHASE_RECOVER:
            case PHASE_FILL:
            case PHASE_INDEX:
                mount_step();
                break;
            case PHASE_IMPORT:
                mount_step();
                if (phase == PHASE_FILL)
                    return; /* the sooner cardman publishes the card the better */
                break;
            case PHASE_READY:
                ready_step(now);
                if (phase == PHASE_READY)
                    return;
                break;
            case PHASE_SNAPSHOT_CHECK:
                if (pf_snapshot_check_step() == PF_STEP_DONE)
                    snapshot_checked();
                break;
            case PHASE_SNAPSHOT_WRITE: {
                pf_step_t step = pf_snapshot_write_step();
                if (step == PF_STEP_WAIT)
                    return;
                if (step != PF_STEP_RUN)
                    phase = PHASE_READY;
                break;
            }
            case PHASE_TREE_WRITE:
                if (pf_tree_write_step() != PF_STEP_RUN)
                    phase = PHASE_READY;
                break;
            case PHASE_SYNC: {
                pf_step_t step = pf_sync_step();
                if (step == PF_STEP_WAIT)
                    return;
                if (step == PF_STEP_DONE)
                    sync_finished(now);
                break;
            }
            case PHASE_APPLY_RETRY:
                if (now < retry_at)
                    return;
                pf_sync_resume();
                phase = PHASE_SYNC;
                break;
            default:
                return;
        }
    } while (pf_time_us() - start < slice);
}

ps2_folder_status_t ps2_folder_card_status(void) {
    switch (phase) {
        case PHASE_OFF:
            return PS2_FOLDER_OFF;
        case PHASE_RECOVER:
        case PHASE_IMPORT:
            return PS2_FOLDER_LOADING;
        case PHASE_FILL:
        case PHASE_INDEX:
            return has_unsynced_writes() ? PS2_FOLDER_DIRTY : PS2_FOLDER_FILLING;
        case PHASE_FAILED:
            return PS2_FOLDER_ERROR;
        case PHASE_APPLY_RETRY:
            return PS2_FOLDER_SYNC_FAILED;
        case PHASE_SYNC:
            return PS2_FOLDER_SYNCING;
        case PHASE_SNAPSHOT_LOAD:
        case PHASE_SNAPSHOT_CHECK:
            return has_unsynced_writes() ? PS2_FOLDER_DIRTY : PS2_FOLDER_VERIFYING;
        case PHASE_SNAPSHOT_WRITE:
        case PHASE_TREE_WRITE:
            return PS2_FOLDER_CACHING;
        case PHASE_READY:
            break;
    }
    if (sync_failed)
        return PS2_FOLDER_SYNC_FAILED;
    return has_unsynced_writes() ? PS2_FOLDER_DIRTY : PS2_FOLDER_CLEAN;
}

int ps2_folder_card_progress(void) {
    switch (phase) {
        case PHASE_RECOVER:
            return 0;
        case PHASE_IMPORT:
            return pf_import_progress();
        case PHASE_FILL:
            return pf_fill_progress();
        default:
            return 100;
    }
}

bool ps2_folder_card_filling(void) {
    return phase == PHASE_FILL;
}

const char *ps2_folder_card_error_text(void) {
    switch (pf.error) {
        case PF_OK:             return "OK";
        case PF_ERR_CAPACITY:   return "Too big for 8MB";
        case PF_ERR_NAME:       return "Bad file name";
        case PF_ERR_LIMIT:      return "Too many files";
        case PF_ERR_IO:         return "SD error";
        case PF_ERR_INVALID_FS: return "Card data invalid";
        case PF_ERR_RECOVERY:   return "Recovery required";
        case PF_ERR_CONFIG:     return "Needs 8MB card";
    }
    return "Unknown error";
}

bool ps2_folder_card_active(void) {
    return phase != PHASE_OFF;
}

void ps2_folder_card_note_dirty(uint32_t page) {
    uint32_t cluster = page / PFS_PAGES_PER_CLUSTER;

    if (cluster < PFS_TOTAL_CLUSTERS)
        pf_bit_set(pf.touched, cluster);
    pf_snapshot_mark(cluster);
}

static bool run_sync_to_completion(void) {
    pf_step_t step;

    do {
        step = pf_sync_step();
    } while (step == PF_STEP_RUN || step == PF_STEP_WAIT);

    sync_finished(pf_time_us());
    return pf_sync_result() == PF_SYNC_COMMITTED || pf_sync_result() == PF_SYNC_NO_CHANGE;
}

/* Finishing costs a pass over the card, which pays off when the next boot is instant
 * but is not worth holding up a card switch to write the whole snapshot. */
static void finish_snapshot(void) {
    pf_step_t step;

    if (pf_tree_stale()) {
        pf_tree_write_start();
        while (pf_tree_write_step() == PF_STEP_RUN) {
        }
    }

    if (!pf_snapshot_outdated() || pf_snapshot_backlog() > SNAPSHOT_FLUSH_MAX)
        return;
    pf_snapshot_write_start(true);
    do {
        step = pf_snapshot_write_step();
    } while (step == PF_STEP_RUN);
}

bool ps2_folder_card_flush(void) {
    uint64_t now = pf_time_us();

    note_generation(now);

    while (phase == PHASE_INDEX)
        mount_step();

    /* A card the console never saw has nothing to save, and any interrupted
     * transaction stays in place for the next mount. */
    switch (phase) {
        case PHASE_OFF:
        case PHASE_FAILED:
            return true;
        case PHASE_RECOVER:
        case PHASE_IMPORT:
            pf_sync_abort();
            pf_import_abort();
            return true;
        case PHASE_FILL:
            /* As below: the caller has to finish loading first. */
            return !has_unsynced_writes();
        case PHASE_APPLY_RETRY:
            pf_sync_resume();
            phase = PHASE_SYNC;
            break;
        case PHASE_SNAPSHOT_LOAD:
            /* PSRAM is incomplete, so only the caller finishing the load can save this. */
            return !has_unsynced_writes();
        case PHASE_SNAPSHOT_CHECK:
            if (!has_unsynced_writes()) {
                pf_snapshot_close();
                phase = PHASE_READY;
                return true;
            }
            while (pf_snapshot_check_step() != PF_STEP_DONE) {
            }
            if (!pf_snapshot_check_passed()) {
                PF_LOG("Snapshot damaged, writes made to it are dropped");
                pf_snapshot_discard();
                pf_snapshot_reset(false); /* PSRAM must not become the next snapshot */
                phase = PHASE_READY;
                synced_gen = seen_gen;
                return true;
            }
            phase = PHASE_READY;
            break;
        case PHASE_SNAPSHOT_WRITE:
            pf_snapshot_write_abort();
            phase = PHASE_READY;
            break;
        case PHASE_TREE_WRITE:
            pf_tree_write_abort();
            phase = PHASE_READY;
            break;
        default:
            break;
    }

    if (phase == PHASE_SYNC) {
        if (pf_sync_committed()) {
            if (!run_sync_to_completion())
                return false;
        } else {
            pf_sync_abort();
            phase = PHASE_READY;
        }
    }

    if (!has_unsynced_writes() && !sync_failed) {
        finish_snapshot();
        return true;
    }

    pf_sync_start(PF_SCAN_FINAL, true);
    phase = PHASE_SYNC;
    if (!run_sync_to_completion() || has_unsynced_writes()) {
        PF_LOG("Unsynchronized data remains");
        return false;
    }
    PF_LOG("Card unmounted cleanly");
    finish_snapshot();
    return true;
}

void ps2_folder_card_close(void) {
    pf_sync_abort();
    pf_import_abort();
    pf_fill_abort();
    pf_snapshot_close();
    phase = PHASE_OFF;
    pf.base[0] = 0;
}

#ifdef PS2_FOLDER_HOST_TEST
void ps2_folder_card_power_loss(void) {
    pf_sync_power_loss();
    pf_import_power_loss();
    pf_fill_power_loss();
    pf_snapshot_power_loss();
    phase = PHASE_OFF;
}
#endif

bool ps2_folder_card_owns_fd(int fd) {
    return phase != PHASE_OFF && pfm_owns_fd(fd);
}

bool ps2_folder_card_path_protected(const char *path) {
    size_t len = strlen(pf.base);

    if (phase == PHASE_OFF || len == 0)
        return false;

    while (*path == '/')
        path++;

    for (size_t i = 0; i < len; i++) {
        char a = path[i], b = pf.base[i];
        if (a >= 'a' && a <= 'z')
            a = (char)(a - 'a' + 'A');
        if (b >= 'a' && b <= 'z')
            b = (char)(b - 'a' + 'A');
        if (a != b)
            return false;
    }
    return path[len] == 0 || path[len] == '/';
}
