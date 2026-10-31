#include <stddef.h>
#include <string.h>

#include "ps2_folder_internal.h"
#include "ps2_folder_name.h"
#include "ps2_folder_port.h"
#include "sd.h"

/* An optional 8 MB copy of the card, so that it can be served while it loads the way
 * an image is, instead of staying hidden until it has been rebuilt from the folder.
 *
 * The copy is only ever a cache: the folder and index stay the master, and the worst a
 * lost snapshot costs is one mount from the folder. snapshot.mcd counts only while
 * snapshot.ok describes it, and that stamp is removed before the first byte of
 * snapshot.mcd changes and written after the last, so a power cut anywhere leaves no
 * stamp. The stamp names the index the copy belongs to, which a committed sync replaces,
 * and a hash of the host tree, which notices files changed on a computer.
 *
 * A valid stamp says what was written, not that it can still be read back, so the
 * content CRC is checked once the card is loaded and before anything is synchronized
 * from it. */

#define STAMP_MAGIC     "PS2FSNP1"
#define MAGIC_LEN       8U
#define HOST_NAME_LEN   256U

typedef struct __attribute__((packed)) {
    char magic[MAGIC_LEN];
    uint32_t index_crc;
    uint32_t index_count;
    uint32_t host_hash;
    uint32_t card_crc;
    uint32_t crc;           /* of everything above */
} stamp_t;

typedef enum {
    SN_BEGIN,
    SN_COPY,
    SN_HOST,
    SN_STAMP,
} sn_state_t;

static struct {
    bool enabled;
    bool outdated;          /* snapshot.mcd and the stamp do not describe the card */
    bool given_up;          /* until the next mount: the SD card would not take it */
    bool check_ok;
    bool final;             /* the console is stopped */

    int fd;
    sn_state_t state;
    uint32_t cluster;
    uint32_t crc;
    uint32_t expected_crc;
    uint32_t start_gen;
    uint32_t backlog;
} sn = {.fd = -1};

static struct {
    int index_fd;
    int dir_fd;
    int it;
    uint32_t record;
    uint32_t hash;
} host = {.index_fd = -1, .dir_fd = -1, .it = -1};

#define TREE_MAGIC      "PS2FTRE1"

typedef struct __attribute__((packed)) {
    char magic[MAGIC_LEN];
    uint32_t index_crc;
    uint32_t index_count;
    uint32_t host_hash;
    uint32_t crc;           /* of everything above */
} tree_stamp_t;

static struct {
    uint32_t index_crc;     /* the index the stamp on SD goes with */
    uint32_t index_count;
    bool given_up;          /* until the next mount */
} tree;

static uint8_t dirty[PFS_TOTAL_CLUSTERS / 8];   /* differs from snapshot.mcd */
static char rel[PFM_PATH_MAX];
static char name[HOST_NAME_LEN];

static void host_close(void) {
    if (host.it >= 0)
        pfm_close(&host.it);
    pfm_close(&host.dir_fd);
    pfm_close(&host.index_fd);
}

static bool host_start(void) {
    host_close();
    host.record = 0;
    host.hash = 0;
    if (!pf_abs(PFM_INDEX))
        return false;
    host.index_fd = pfm_open(pf.path, O_RDONLY);
    return host.index_fd >= 0;
}

/* The directories come from the index, so the walk needs no stack of open directories.
 * One that appeared later is found as an entry of its parent, and that is enough:
 * anything unexpected only has to change the hash. */
static pf_step_t host_step(void) {
    if (host.dir_fd < 0) {
        pfm_record_t rec;

        if (host.record >= pf.base_count) {
            pfm_close(&host.index_fd);
            return PF_STEP_DONE;
        }
        if (!pfm_index_read(host.index_fd, host.record, &rec))
            return PF_STEP_FAILED;
        if (!(rec.mode & PFS_MODE_DIR)) {
            host.record++;
            return PF_STEP_RUN;
        }
        if (!pfm_record_path(host.index_fd, host.record, rel, sizeof(rel)) || !pf_abs(rel))
            return PF_STEP_FAILED;
        host.dir_fd = pfm_open(pf.path, O_RDONLY);
        host.it = -1;
        return host.dir_fd >= 0 ? PF_STEP_RUN : PF_STEP_FAILED;
    }

    pfm_iter_next(host.dir_fd, &host.it);
    if (host.it < 0) {
        pfm_close(&host.dir_fd);
        host.record++;
        return PF_STEP_RUN;
    }

    if (sd_get_name(host.it, name, sizeof(name)) == 0)
        return PF_STEP_FAILED;
    if ((host.record == 0 && strcmp(name, PFM_DIR) == 0) || pfn_host_junk(name))
        return PF_STEP_RUN;

    uint32_t hash = pfm_crc32(host.record, name, strlen(name));

    if (!sd_is_dir(host.it)) {
        ps2_fileio_stat_t stat;
        uint32_t size = (uint32_t)sd_filesize(host.it);

        memset(&stat, 0, sizeof(stat));
        sd_get_stat(host.it, &stat);
        hash = pfm_crc32(hash, &size, sizeof(size));
        hash = pfm_crc32(hash, stat.mtime, sizeof(stat.mtime));
    }
    host.hash += hash; /* the order of a directory is not part of its content */
    return PF_STEP_RUN;
}

static bool host_hash_within(uint64_t start, uint64_t budget_us) {
    pf_step_t step;

    if (!host_start())
        return false;
    do {
        step = host_step();
    } while (step == PF_STEP_RUN && pf_time_us() - start < budget_us);
    host_close();
    return step == PF_STEP_DONE;
}

static bool stamp_read(stamp_t *stamp) {
    int fd;
    bool ok;

    if (!pf_abs(PFM_SNAPSHOT_STAMP) || !sd_exists(pf.path))
        return false;
    fd = pfm_open(pf.path, O_RDONLY);
    if (fd < 0)
        return false;
    ok = sd_read(fd, stamp, sizeof(*stamp)) == (int)sizeof(*stamp);
    pfm_close(&fd);

    return ok && memcmp(stamp->magic, STAMP_MAGIC, MAGIC_LEN) == 0
           && stamp->crc == pfm_crc32(0, stamp, offsetof(stamp_t, crc));
}

static bool stamp_write(void) {
    stamp_t stamp;
    int fd;
    bool ok;

    memcpy(stamp.magic, STAMP_MAGIC, MAGIC_LEN);
    stamp.index_crc = pf.base_crc;
    stamp.index_count = pf.base_count;
    stamp.host_hash = host.hash;
    stamp.card_crc = sn.crc;
    stamp.crc = pfm_crc32(0, &stamp, offsetof(stamp_t, crc));

    if (!pf_abs(PFM_SNAPSHOT_STAMP))
        return false;
    fd = pfm_open(pf.path, O_WRONLY | O_CREAT | O_TRUNC);
    if (fd < 0)
        return false;
    ok = sd_write(fd, &stamp, sizeof(stamp)) == (int)sizeof(stamp);
    return pfm_close_checked(&fd) && ok;
}

static bool stamp_remove(void) {
    return pf_abs(PFM_SNAPSHOT_STAMP) && (!sd_exists(pf.path) || sd_remove(pf.path) == 0);
}

void pf_snapshot_reset(bool enabled) {
    pf_snapshot_close();
    memset(dirty, 0, sizeof(dirty));
    sn.enabled = enabled;
    sn.outdated = false;
    sn.given_up = false;
    sn.backlog = 0;
}

void pf_snapshot_close(void) {
    pfm_close(&sn.fd);
    host_close();
}

int pf_snapshot_open(uint64_t budget_us) {
    stamp_t stamp;
    uint64_t start = pf_time_us();

    if (!sn.enabled || !pf.base_valid || !stamp_read(&stamp))
        return -1;
    if (stamp.index_crc != pf.base_crc || stamp.index_count != pf.base_count) {
        PF_LOG("Snapshot is older than the last save");
        return -1;
    }

    if (!host_hash_within(start, budget_us) || host.hash != stamp.host_hash) {
        PF_LOG("Folder differs from the snapshot");
        return -1;
    }

    if (!pf_abs(PFM_SNAPSHOT) || !sd_exists(pf.path))
        return -1;
    sn.fd = pfm_open(pf.path, O_RDONLY);
    if (sn.fd >= 0 && (uint32_t)sd_filesize(sn.fd) != PFS_CARD_SIZE)
        pfm_close(&sn.fd);
    sn.expected_crc = stamp.card_crc;
    return sn.fd;
}

void pf_snapshot_check_start(void) {
    sn.cluster = 0;
    sn.crc = 0;
    sn.check_ok = sn.fd >= 0 && sd_seek(sn.fd, 0, SEEK_SET) == 0;
}

pf_step_t pf_snapshot_check_step(void) {
    if (sn.check_ok && sn.cluster < PFS_TOTAL_CLUSTERS) {
        if (sd_read(sn.fd, pf.io, sizeof(pf.io)) == (int)sizeof(pf.io)) {
            sn.crc = pfm_crc32(sn.crc, pf.io, sizeof(pf.io));
            sn.cluster++;
            return PF_STEP_RUN;
        }
        sn.check_ok = false;
    }

    sn.check_ok = sn.check_ok && sn.crc == sn.expected_crc;
    pfm_close(&sn.fd);
    return PF_STEP_DONE;
}

bool pf_snapshot_check_passed(void) {
    return sn.check_ok;
}

void pf_snapshot_discard(void) {
    pf_snapshot_close();
    stamp_remove();
}

void pf_snapshot_mark(uint32_t cluster) {
    if (!sn.enabled || cluster >= PFS_TOTAL_CLUSTERS)
        return;
    if (!pf_bit_get(dirty, cluster)) {
        pf_bit_set(dirty, cluster);
        sn.backlog++;
    }
    sn.outdated = true;
}

void pf_snapshot_mark_all(void) {
    if (!sn.enabled)
        return;
    memset(dirty, 0xFF, sizeof(dirty));
    sn.backlog = PFS_TOTAL_CLUSTERS;
    sn.outdated = true;
}

bool pf_snapshot_outdated(void) {
    return sn.enabled && sn.outdated && !sn.given_up && pf.base_valid;
}

uint32_t pf_snapshot_backlog(void) {
    return sn.backlog;
}

void pf_snapshot_write_start(bool final) {
    bool pending;

    sn.state = SN_BEGIN;
    sn.final = final;
    pf_write_snapshot(&sn.start_gen, &pending);
}

void pf_snapshot_write_abort(void) {
    pf_snapshot_close();
}

/* A snapshot that will not fit is the one thing here that could hurt: it would take
 * the room a save needs. */
static pf_step_t write_failed(void) {
    PF_LOG("Snapshot not written, this card keeps loading from its folder");
    pf_snapshot_close();
    sn.given_up = true;
    if (stamp_remove() && pf_abs(PFM_SNAPSHOT) && sd_exists(pf.path))
        sd_remove(pf.path);
    return PF_STEP_DONE;
}

static pf_step_t write_begin(void) {
    /* From here until the new stamp, snapshot.mcd is not what any stamp says it is. */
    if (!stamp_remove() || !pf_abs(PFM_SNAPSHOT))
        return write_failed();

    /* Shorter is a copy the console interrupted. Everything past its end is still
     * marked, because a mount that did not start from a snapshot marks every cluster. */
    if (sd_exists(pf.path)) {
        sn.fd = pfm_open(pf.path, O_RDWR);
        if (sn.fd >= 0 && (uint32_t)sd_filesize(sn.fd) > PFS_CARD_SIZE)
            pfm_close(&sn.fd);
    }
    if (sn.fd < 0) {
        sn.fd = pfm_open(pf.path, O_RDWR | O_CREAT | O_TRUNC);
        if (sn.fd < 0)
            return write_failed();
        pf_snapshot_mark_all();
    }

    sn.cluster = 0;
    sn.crc = 0;
    sn.state = SN_COPY;
    return PF_STEP_RUN;
}

/* Every cluster goes through the CRC, but only the changed ones reach the SD card.
 * Going up in order means a new file never has to seek past its end. */
static pf_step_t write_copy(void) {
    uint32_t c = sn.cluster;

    if (c >= PFS_TOTAL_CLUSTERS) {
        if (!pfm_close_checked(&sn.fd) || !host_start())
            return write_failed();
        sn.state = SN_HOST;
        return PF_STEP_RUN;
    }

    pf_psram_read(c * PFS_CLUSTER_SIZE, pf.io, PFS_PAGE_SIZE);
    pf_psram_read(c * PFS_CLUSTER_SIZE + PFS_PAGE_SIZE, &pf.io[PFS_PAGE_SIZE], PFS_PAGE_SIZE);
    sn.crc = pfm_crc32(sn.crc, pf.io, sizeof(pf.io));

    if (pf_bit_get(dirty, c)) {
        if (sd_seek(sn.fd, (int32_t)(c * PFS_CLUSTER_SIZE), SEEK_SET) != 0
            || sd_write(sn.fd, pf.io, sizeof(pf.io)) != (int)sizeof(pf.io))
            return write_failed();
        dirty[c / 8] &= (uint8_t)~(1U << (c % 8));
        sn.backlog--;
    }
    sn.cluster++;
    return PF_STEP_RUN;
}

pf_step_t pf_snapshot_write_step(void) {
    uint32_t gen;
    bool pending;

    if (!sn.final && !pf_console_quiet())
        return PF_STEP_WAIT;

    /* The console moved on: what was copied stays copied, the rest is still marked. */
    pf_write_snapshot(&gen, &pending);
    if (gen != sn.start_gen || pending) {
        pf_snapshot_close();
        return PF_STEP_DONE;
    }

    switch (sn.state) {
        case SN_BEGIN:
            return write_begin();
        case SN_COPY:
            return write_copy();
        case SN_HOST: {
            pf_step_t step = host_step();

            if (step == PF_STEP_FAILED)
                return write_failed();
            if (step == PF_STEP_DONE)
                sn.state = SN_STAMP;
            return PF_STEP_RUN;
        }
        case SN_STAMP:
            if (sn.backlog != 0) {
                pf_snapshot_close();
                return PF_STEP_DONE;
            }
            if (!stamp_write())
                return write_failed();
            sn.outdated = false;
            PF_LOG("Snapshot written");
            return PF_STEP_DONE;
    }
    return PF_STEP_FAILED;
}

void pf_tree_reset(void) {
    memset(&tree, 0, sizeof(tree));
}

/* Only says which files to expect. What they hold is read in any case, so unlike a
 * snapshot this cannot show old content for a file replaced at the same size and date. */
bool pf_tree_unchanged(uint64_t budget_us) {
    tree_stamp_t stamp;
    uint64_t start = pf_time_us();
    bool ok = false;
    int fd;

    if (!pf.base_valid || !pf_abs(PFM_TREE_STAMP) || !sd_exists(pf.path))
        return false;
    fd = pfm_open(pf.path, O_RDONLY);
    if (fd < 0)
        return false;
    ok = sd_read(fd, &stamp, sizeof(stamp)) == (int)sizeof(stamp);
    pfm_close(&fd);

    if (!ok || memcmp(stamp.magic, TREE_MAGIC, MAGIC_LEN) != 0
        || stamp.crc != pfm_crc32(0, &stamp, offsetof(tree_stamp_t, crc))
        || stamp.index_crc != pf.base_crc || stamp.index_count != pf.base_count)
        return false;

    ok = host_hash_within(start, budget_us) && host.hash == stamp.host_hash;
    PF_LOG("Folder compared in %lu ms: %s", (unsigned long)((pf_time_us() - start) / 1000U), ok ? "same" : "changed");
    if (ok) {
        tree.index_crc = stamp.index_crc;
        tree.index_count = stamp.index_count;
    }
    return ok;
}

bool pf_tree_stale(void) {
    return pf.base_valid && !tree.given_up && (tree.index_crc != pf.base_crc || tree.index_count != pf.base_count);
}

void pf_tree_write_start(void) {
    if (!host_start())
        tree.given_up = true;
}

void pf_tree_write_abort(void) {
    host_close();
}

pf_step_t pf_tree_write_step(void) {
    tree_stamp_t stamp;
    pf_step_t step;
    bool ok;
    int fd;

    if (tree.given_up)
        return PF_STEP_DONE;

    step = host_step();
    if (step == PF_STEP_RUN)
        return PF_STEP_RUN;
    if (step == PF_STEP_FAILED) {
        host_close();
        tree.given_up = true;
        return PF_STEP_DONE;
    }

    memcpy(stamp.magic, TREE_MAGIC, MAGIC_LEN);
    stamp.index_crc = pf.base_crc;
    stamp.index_count = pf.base_count;
    stamp.host_hash = host.hash;
    stamp.crc = pfm_crc32(0, &stamp, offsetof(tree_stamp_t, crc));

    ok = pf_abs(PFM_TREE_STAMP) && (fd = pfm_open(pf.path, O_WRONLY | O_CREAT | O_TRUNC)) >= 0;
    if (ok) {
        ok = sd_write(fd, &stamp, sizeof(stamp)) == (int)sizeof(stamp);
        ok = pfm_close_checked(&fd) && ok;
    }

    /* Half a stamp fails its CRC, and costs one mount a look at every file. */
    if (ok) {
        tree.index_crc = stamp.index_crc;
        tree.index_count = stamp.index_count;
    } else {
        tree.given_up = true;
    }
    return PF_STEP_DONE;
}

#ifdef PS2_FOLDER_HOST_TEST
void pf_snapshot_power_loss(void) {
    sn.fd = -1;
    host.index_fd = host.dir_fd = host.it = -1;
}
#endif
