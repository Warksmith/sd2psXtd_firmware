#include <stdio.h>
#include <string.h>

#include "ps2_folder_internal.h"
#include "ps2_folder_name.h"
#include "ps2_folder_port.h"
#include "sd.h"

/* Reconciles the live PS2 filesystem in PSRAM against the last committed state.
 *
 * Scan: walk the card breadth-first, using the index being written as the work
 * queue. Every structure is validated and nothing else is trusted, because the
 * console may have stopped writing halfway through an update. Changed files are
 * copied into the stage directory and the required host operations go into a journal.
 *
 * Commit: only if no console write happened during the whole scan, the journal is
 * sealed with a CRC. That single write is the commit point.
 *
 * Apply: journal ops are idempotent, so an interrupted apply is simply redone. Stage
 * content is copied over its target rather than renamed: SdFat's rename writes the
 * new directory entry before dropping the old one, and a cut between the two leaves
 * two entries on one cluster chain, where deleting either destroys the data. */

#define PREPASS_ENTRIES_PER_STEP    16U
#define MAX_DIR_LENGTH              32768U
#define HOST_NAME_MAX_LEN           256U

typedef enum {
    SC_IDLE,
    SC_BEGIN,
    SC_NEXT_DIR,
    SC_PREPASS,
    SC_ENTRIES,
    SC_STAGE,
    SC_DELETES,
    SC_COMMIT,
    SC_APPLY_OPEN,
    SC_APPLY_REMOVE,
    SC_APPLY_WRITE,
    SC_APPLY_COPY,
    SC_APPLY_DONE,
    SC_CLEAN,
    SC_FINISHED,
} sc_state_t;

static struct {
    sc_state_t state;
    pf_scan_mode_t mode;
    pf_sync_result_t result;
    bool accept_open;
    bool committed;

    uint32_t start_gen;
    uint32_t count;
    uint32_t cursor;

    int index_fd;
    int base_fd;
    int stage_fd;
    int jr_fd;
    int src_fd;
    int dst_fd;
    pfj_writer_t journal;
    bool journal_open;

    /* directory being walked */
    pfm_record_t dir;
    uint16_t dir_base;
    uint32_t cluster;
    uint32_t k;
    uint32_t live;
    uint32_t live_i;

    /* entry being emitted */
    pfm_record_t rec;
    uint16_t rec_base;
    bool rec_is_new;
    pfm_record_t base_rec;
    uint32_t stage_id;
    uint32_t stage_cluster;
    uint32_t stage_left;
    uint32_t stage_crc;
    char rel[PFM_PATH_MAX];

    uint32_t del;
    bool changed;
    uint32_t n_new, n_mod, n_del;

    pfj_entry_t op;
    uint32_t total_ops;
} s;

static uint8_t host_name[HOST_NAME_MAX_LEN];
static char other_path[PFM_PATH_MAX];

pf_ctx_t pf;

uint32_t pf_base_key(uint32_t parent, uint32_t fold) {
    return (fold ^ (parent * 0x9E3779B1U)) | 1U; /* never 0, which marks an empty slot */
}

bool pf_abs(const char *rel) {
    return pfm_join(pf.path, sizeof(pf.path), pf.base, rel);
}

void pf_base_load(void) {
    int fd;
    pfm_record_t rec;
    uint32_t count, crc;

    pf.base_valid = false;
    pf.base_count = 0;

    if (!pf_abs(PFM_INDEX) || !sd_exists(pf.path))
        return;
    fd = pfm_open(pf.path, O_RDONLY);
    if (fd < 0)
        return;

    if (pfm_index_validate(fd, &count, &crc) && count <= PF_MAX_ENTRIES) {
        bool ok = true;

        for (uint32_t i = 0; ok && i < count; i++) {
            ok = pfm_index_read(fd, i, &rec) && (i == 0 || rec.parent < i);
            if (ok)
                pf.base_hash[i] = pf_base_key(rec.parent, pfn_fold_hash(rec.name));
        }
        if (ok) {
            pf.base_valid = true;
            pf.base_count = count;
            pf.base_crc = crc;
        }
    }
    if (!pf.base_valid) {
        PF_LOG("Metadata missing or damaged, using defaults");
    }
    pfm_close(&fd);
}

static void stage_path(uint32_t id, char *out, size_t size) {
    char rel[32];

    if (id == PFJ_STAGE_INDEX)
        snprintf(rel, sizeof(rel), "%s", PFM_STAGE_INDEX);
    else
        snprintf(rel, sizeof(rel), "%s/%08lX.bin", PFM_STAGE_DIR, (unsigned long)id);
    pfm_join(out, size, pf.base, rel);
}

/* Removes everything in the stage directory, one entry per pass of the iterator
 * because the directory cannot change while it is being iterated. */
static void stage_clean(void) {
    char dir[PF_BASE_PATH_MAX + 32];

    pfm_join(dir, sizeof(dir), pf.base, PFM_STAGE_DIR);
    while (true) {
        int dir_fd = pfm_open(dir, O_RDONLY);
        int it = -1;
        bool found = false;

        if (dir_fd < 0)
            return;
        pfm_iter_next(dir_fd, &it);
        if (it >= 0) {
            found = sd_get_name(it, (char *)host_name, sizeof(host_name)) != 0;
            pfm_close(&it);
        }
        pfm_close(&dir_fd);

        if (!found || !pfm_join(pf.path, sizeof(pf.path), dir, (char *)host_name) || sd_remove(pf.path) != 0)
            return;
    }
}

bool pf_meta_dirs(void) {
    if (sd_mkdir(pf.base) != 0)
        return false;
    if (!pf_abs(PFM_DIR) || sd_mkdir(pf.path) != 0)
        return false;
    return pf_abs(PFM_STAGE_DIR) && sd_mkdir(pf.path) == 0;
}

static void close_scan_files(void) {
    pfm_close(&s.index_fd);
    pfm_close(&s.base_fd);
    pfm_close(&s.stage_fd);
    pfm_close(&s.src_fd);
    pfm_close(&s.dst_fd);
    pfm_close(&s.jr_fd);
}

static void drop_scan(void) {
    close_scan_files();
    if (s.journal_open) {
        pf_abs(PFM_JOURNAL);
        pfj_abandon(&s.journal, pf.path);
        s.journal_open = false;
    }
    stage_clean();
}

static pf_step_t finish(pf_sync_result_t result) {
    s.result = result;
    s.state = SC_FINISHED;
    return PF_STEP_DONE;
}

static pf_step_t give_up(pf_sync_result_t result, pf_error_t error) {
    drop_scan();
    if (error != PF_OK)
        pf.error = error;
    return finish(result);
}

static pf_step_t invalid_fs(const char *why) {
    (void)why;
    PF_LOG("Invalid filesystem: %s", why);
    return give_up(PF_SYNC_FAILED, PF_ERR_INVALID_FS);
}

static bool journal_add(pfj_op_t type, uint32_t id, const char *rel) {
    if (!s.journal_open) {
        if (!pf_abs(PFM_JOURNAL) || !pfj_create(&s.journal, pf.path))
            return false;
        s.journal_open = true;
    }
    return pfj_add(&s.journal, type, id, rel);
}

/* True when the scan must be abandoned because the snapshot is no longer coherent. */
static bool scan_stale(void) {
    uint32_t gen;
    bool pending;

    if (s.mode == PF_SCAN_IMPORT)
        return false;
    pf_write_snapshot(&gen, &pending);
    return gen != s.start_gen || pending;
}

static bool must_wait(void) {
    return s.mode == PF_SCAN_SYNC && !pf_console_quiet();
}

static bool visit(uint32_t cluster) {
    if (pf_bit_get(pf.visited, cluster))
        return false; /* cycle, or two owners of one cluster */
    pf_bit_set(pf.visited, cluster);
    return true;
}

static uint16_t base_find(uint16_t parent, const char raw[PFS_NAME_LEN], uint32_t fold, bool strict, bool is_dir) {
    pfm_record_t rec;
    uint32_t key;

    if (parent == PFM_NONE || s.base_fd < 0)
        return PFM_NONE;

    /* Children of one directory are contiguous, so its range bounds the search. */
    if (!pfm_index_read(s.base_fd, parent, &rec))
        return PFM_NONE;
    key = pf_base_key(parent, fold);

    for (uint32_t i = rec.first_child; i < (uint32_t)rec.first_child + rec.child_count && i < pf.base_count; i++) {
        if (pf.base_hash[i] != key || pf_bit_get(pf.base_seen, i) || !pfm_index_read(s.base_fd, i, &s.base_rec))
            continue;
        if (memcmp(s.base_rec.name, raw, PFS_NAME_LEN) != 0)
            continue;
        if (((s.base_rec.mode & PFS_MODE_DIR) != 0) != is_dir)
            continue;
        if (((s.base_rec.flags & PFM_FLAG_STRICT) != 0) != strict)
            continue;
        return (uint16_t)i;
    }
    return PFM_NONE;
}

static bool metadata_differs(const pfm_record_t *a, const pfm_record_t *b) {
    return a->mode != b->mode || a->attr != b->attr || memcmp(a->created, b->created, 8) != 0
           || memcmp(a->modified, b->modified, 8) != 0;
}

void pf_fill_record(pfm_record_t *rec, const pfs_entry_t *e, uint32_t parent) {
    memset(rec, 0, sizeof(*rec));
    memcpy(rec->name, e->name, PFS_NAME_LEN);
    memcpy(rec->created, e->created, 8);
    memcpy(rec->modified, e->modified, 8);
    rec->attr = e->attr;
    rec->size = e->length;
    rec->cluster = e->cluster;
    rec->parent = (uint16_t)parent;
    rec->mode = e->mode;
}

static pf_step_t step_begin(void) {
    pfs_entry_t root;
    pfm_record_t rec;
    bool pending = false;

    memset(pf.visited, 0, sizeof(pf.visited));
    memset(pf.base_seen, 0, sizeof(pf.base_seen));
    s.count = s.cursor = 0;
    s.changed = false;
    s.n_new = s.n_mod = s.n_del = 0;
    s.stage_id = 0;

    pf_write_snapshot(&s.start_gen, &pending);
    if (pending)
        return finish(PF_SYNC_INTERRUPTED);

    for (size_t i = 0; i < sizeof(pf.touched); i++) {
        pf.scan_touched[i] |= pf.touched[i];
        pf.touched[i] = 0;
    }

    if (!pf_meta_dirs())
        return give_up(PF_SYNC_FAILED, PF_ERR_IO);
    stage_clean();

    if (!pfs_mount(&pf.fs))
        return invalid_fs("superblock");

    if (pf.base_valid) {
        if (!pf_abs(PFM_INDEX) || (s.base_fd = pfm_open(pf.path, O_RDONLY)) < 0)
            return give_up(PF_SYNC_FAILED, PF_ERR_IO);
    }

    if (!pf_abs(PFM_STAGE_INDEX) || (s.index_fd = pfm_index_create(pf.path)) < 0)
        return give_up(PF_SYNC_FAILED, PF_ERR_IO);

    pfs_entry_read(&pf.fs, pf.fs.root_cluster, 0, &root);
    if (!(root.mode & PFS_MODE_EXISTS) || !(root.mode & PFS_MODE_DIR) || root.length < 2 || root.length > MAX_DIR_LENGTH)
        return invalid_fs("root directory");

    root.cluster = pf.fs.root_cluster;
    pf_fill_record(&rec, &root, 0);
    memset(rec.name, 0, sizeof(rec.name));

    pf.map[0] = pf.base_valid ? 0 : PFM_NONE;
    if (pf.base_valid) {
        pf_bit_set(pf.base_seen, 0);
        if (!pfm_index_read(s.base_fd, 0, &s.base_rec))
            return give_up(PF_SYNC_FAILED, PF_ERR_IO);
        if (metadata_differs(&rec, &s.base_rec))
            s.changed = true;
    } else {
        s.changed = true;
    }

    if (!pfm_index_write(s.index_fd, 0, &rec))
        return give_up(PF_SYNC_FAILED, PF_ERR_IO);
    s.count = 1;
    s.state = SC_NEXT_DIR;
    return PF_STEP_RUN;
}

static pf_step_t step_next_dir(void) {
    if (s.cursor >= s.count) {
        s.del = pf.base_count;
        s.state = SC_DELETES;
        return PF_STEP_RUN;
    }

    if (!pfm_index_read(s.index_fd, s.cursor, &s.dir))
        return give_up(PF_SYNC_FAILED, PF_ERR_IO);

    if (!(s.dir.mode & PFS_MODE_DIR)) {
        s.cursor++;
        return PF_STEP_RUN;
    }

    if (!pfs_cluster_valid(&pf.fs, s.dir.cluster) || s.dir.size < 2 || s.dir.size > MAX_DIR_LENGTH)
        return invalid_fs("directory geometry");

    s.dir_base = pf.map[s.cursor];
    s.cluster = s.dir.cluster;
    s.k = 0;
    s.live = 0;
    s.state = SC_PREPASS;
    return PF_STEP_RUN;
}

static bool walk_advance(void) {
    uint32_t next;

    s.k++;
    if (s.k % PFS_ENTRIES_PER_CLUSTER != 0 || s.k >= s.dir.size)
        return true;
    if (!pfs_fat_next(&pf.fs, s.cluster, &next) || next == PFS_NO_CLUSTER)
        return false;
    s.cluster = next;
    return true;
}

/* First pass over a directory: validate it and collect what deciding the host names
 * of its children needs. */
static pf_step_t step_prepass(void) {
    pfs_entry_t e;

    if (must_wait())
        return PF_STEP_WAIT;

    for (uint32_t n = 0; n < PREPASS_ENTRIES_PER_STEP; n++) {
        if (s.k >= s.dir.size) {
            s.cluster = s.dir.cluster;
            s.k = 0;
            s.live_i = 0;
            s.dir.first_child = (uint16_t)s.count;
            s.state = SC_ENTRIES;
            return PF_STEP_RUN;
        }

        if (s.k % PFS_ENTRIES_PER_CLUSTER == 0 && !visit(s.cluster))
            return invalid_fs("directory chain");

        pfs_entry_read(&pf.fs, s.cluster, s.k % PFS_ENTRIES_PER_CLUSTER, &e);

        if (s.k >= 2 && (e.mode & PFS_MODE_EXISTS)) {
            if (s.live >= PF_MAX_DIR_ENTRIES)
                return give_up(PF_SYNC_FAILED, PF_ERR_LIMIT);
            pf.fold[s.live++] = pfn_fold_hash(e.name);
        }

        if (!walk_advance())
            return invalid_fs("directory shorter than its length");
    }
    return PF_STEP_RUN;
}

static bool sibling_shares_fold(uint32_t i) {
    for (uint32_t j = 0; j < s.live; j++) {
        if (j != i && pf.fold[j] == pf.fold[i])
            return true;
    }
    return false;
}

static pf_step_t emit_record(void) {
    if (s.count >= PF_MAX_ENTRIES)
        return give_up(PF_SYNC_FAILED, PF_ERR_LIMIT);

    pf.map[s.count] = s.rec_base;
    if (s.rec_base != PFM_NONE)
        pf_bit_set(pf.base_seen, s.rec_base);
    if (!pfm_index_write(s.index_fd, s.count, &s.rec))
        return give_up(PF_SYNC_FAILED, PF_ERR_IO);
    s.count++;
    s.live_i++;
    s.state = SC_ENTRIES;
    return PF_STEP_RUN;
}

static pf_step_t process_file(void) {
    uint32_t needed = (s.rec.size + PFS_CLUSTER_SIZE - 1) / PFS_CLUSTER_SIZE;
    uint32_t chain = PF_CHAIN_START;
    uint32_t clusters = 0;
    bool touched = false;
    uint32_t c = s.rec.cluster;

    if (s.rec.size > PFS_CARD_SIZE)
        return invalid_fs("file size");

    /* An empty file has no content that could depend on what its entry points at. */
    if (s.rec.size != 0) {
        if (c == PFS_NO_CLUSTER)
            return invalid_fs("file without clusters");
        while (c != PFS_NO_CLUSTER) {
            if (!pfs_cluster_valid(&pf.fs, c) || !visit(c))
                return invalid_fs("file chain");
            touched = touched || pf_bit_get(pf.scan_touched, pf.fs.alloc_offset + c);
            chain = pf_chain_add(chain, c);
            clusters++;
            if (!pfs_fat_next(&pf.fs, c, &c))
                return invalid_fs("file chain link");
        }
    }
    if (clusters < needed)
        return invalid_fs("file shorter than its length");

    s.rec.chain = chain;

    bool known = s.rec_base != PFM_NONE;
    bool content_same = known && !touched && s.rec.size == s.base_rec.size && chain == s.base_rec.chain;

    if (content_same) {
        s.rec.crc = s.base_rec.crc;
        if (metadata_differs(&s.rec, &s.base_rec))
            s.changed = true;
        return emit_record();
    }

    char path[PF_BASE_PATH_MAX + 32];

    s.stage_cluster = s.rec.cluster;
    s.stage_left = s.rec.size;
    s.stage_crc = 0;
    stage_path(s.stage_id, path, sizeof(path));
    s.stage_fd = pfm_open(path, O_WRONLY | O_CREAT | O_TRUNC);
    if (s.stage_fd < 0)
        return give_up(PF_SYNC_FAILED, PF_ERR_IO);
    s.state = SC_STAGE;
    return PF_STEP_RUN;
}

static pf_step_t step_entries(void) {
    pfs_entry_t e;

    if (must_wait())
        return PF_STEP_WAIT;
    if (scan_stale())
        return give_up(PF_SYNC_INTERRUPTED, PF_OK);

    if (s.k >= s.dir.size) {
        s.dir.child_count = (uint16_t)(s.count - s.dir.first_child);
        if (!pfm_index_write(s.index_fd, s.cursor, &s.dir))
            return give_up(PF_SYNC_FAILED, PF_ERR_IO);
        s.cursor++;
        s.state = SC_NEXT_DIR;
        return PF_STEP_RUN;
    }

    pfs_entry_read(&pf.fs, s.cluster, s.k % PFS_ENTRIES_PER_CLUSTER, &e);
    bool live = s.k >= 2 && (e.mode & PFS_MODE_EXISTS);

    if (!walk_advance())
        return invalid_fs("directory shorter than its length");
    if (!live)
        return PF_STEP_RUN;

    bool is_dir = (e.mode & PFS_MODE_DIR) != 0;
    bool is_file = (e.mode & PFS_MODE_FILE) != 0;
    if (is_dir == is_file || !pfn_valid_raw(e.name))
        return invalid_fs("directory entry");

    if (is_file && !(e.mode & PFS_MODE_CLOSED) && s.mode == PF_SCAN_SYNC && !s.accept_open) {
        give_up(PF_SYNC_BUSY, PF_OK);
        return PF_STEP_DONE;
    }

    uint32_t fold = pf.fold[s.live_i];
    bool strict = false;

    if (sibling_shares_fold(s.live_i)) {
        /* Names equal under case folding: an exact match is corruption, anything else
         * is told apart on the host by the strict encoding. */
        for (uint32_t j = 0; j < s.live_i; j++) {
            pfm_record_t other;
            if (pf.fold[j] == fold) {
                if (!pfm_index_read(s.index_fd, s.dir.first_child + j, &other))
                    return give_up(PF_SYNC_FAILED, PF_ERR_IO);
                if (memcmp(other.name, e.name, PFS_NAME_LEN) == 0)
                    return invalid_fs("duplicate name");
            }
        }
        strict = pfn_has_lower(e.name);
    }

    pf_fill_record(&s.rec, &e, s.cursor);
    if (strict)
        s.rec.flags |= PFM_FLAG_STRICT;

    pfn_encode(e.name, strict, (char *)host_name);
    if (!pfm_child_path(s.index_fd, s.cursor, (char *)host_name, s.rel, sizeof(s.rel)))
        return give_up(PF_SYNC_FAILED, PF_ERR_LIMIT);

    s.rec_base = base_find(s.dir_base, e.name, fold, strict, is_dir);
    s.rec_is_new = s.rec_base == PFM_NONE;

    if (is_dir) {
        if (!pfs_cluster_valid(&pf.fs, e.cluster) || e.length < 2 || e.length > MAX_DIR_LENGTH)
            return invalid_fs("directory geometry");
        if (s.rec_is_new) {
            s.changed = true;
            s.n_new++;
            if (!journal_add(PFJ_MKDIR, 0, s.rel))
                return give_up(PF_SYNC_FAILED, PF_ERR_IO);
        } else if (metadata_differs(&s.rec, &s.base_rec)) {
            s.changed = true;
        }
        return emit_record();
    }
    return process_file();
}

static pf_step_t step_stage(void) {
    if (must_wait())
        return PF_STEP_WAIT;

    if (s.stage_left > 0) {
        uint32_t chunk = s.stage_left > PFS_CLUSTER_SIZE ? PFS_CLUSTER_SIZE : s.stage_left;

        pf_psram_read(pfs_page_addr(&pf.fs, s.stage_cluster, 0), pf.io, PFS_PAGE_SIZE);
        pf_psram_read(pfs_page_addr(&pf.fs, s.stage_cluster, 1), &pf.io[PFS_PAGE_SIZE], PFS_PAGE_SIZE);

        s.stage_crc = pfm_crc32(s.stage_crc, pf.io, chunk);
        if (sd_write(s.stage_fd, pf.io, chunk) != (int)chunk)
            return give_up(PF_SYNC_FAILED, PF_ERR_IO);

        s.stage_left -= chunk;
        if (s.stage_left > 0 && (!pfs_fat_next(&pf.fs, s.stage_cluster, &s.stage_cluster) || s.stage_cluster == PFS_NO_CLUSTER))
            return invalid_fs("file chain link");
        return PF_STEP_RUN;
    }

    s.rec.crc = s.stage_crc;

    if (!pfm_close_checked(&s.stage_fd))
        return give_up(PF_SYNC_FAILED, PF_ERR_IO);

    if (!s.rec_is_new && s.rec.size == s.base_rec.size && s.rec.crc == s.base_rec.crc) {
        /* Rewritten with identical content. */
        char path[PF_BASE_PATH_MAX + 32];

        stage_path(s.stage_id, path, sizeof(path));
        sd_remove(path);
        if (metadata_differs(&s.rec, &s.base_rec) || s.rec.chain != s.base_rec.chain)
            s.changed = true;
        return emit_record();
    }

    if (!journal_add(PFJ_WRITE, s.stage_id, s.rel))
        return give_up(PF_SYNC_FAILED, PF_ERR_IO);
    s.stage_id++;
    s.changed = true;
    if (s.rec_is_new)
        s.n_new++;
    else
        s.n_mod++;
    return emit_record();
}

static pf_step_t step_deletes(void) {
    /* A console format leaves nothing behind, so a mistaken read of an empty card would
     * otherwise erase everything. Those saves are moved aside rather than deleted. */
    bool wipe = s.count == 1 && pf.base_count > 1;

    if (s.del <= 1) {
        s.state = SC_COMMIT;
        return PF_STEP_RUN;
    }

    uint32_t i = --s.del;
    if (pf_bit_get(pf.base_seen, i))
        return PF_STEP_RUN;

    if (!pfm_index_read(s.base_fd, i, &s.base_rec) || !pfm_record_path(s.base_fd, i, s.rel, sizeof(s.rel)))
        return give_up(PF_SYNC_FAILED, PF_ERR_IO);

    s.changed = true;
    s.n_del++;

    bool ok = true;
    if (!wipe)
        ok = journal_add((s.base_rec.mode & PFS_MODE_DIR) ? PFJ_RMDIR : PFJ_REMOVE, 0, s.rel);
    else if (s.base_rec.parent == 0)
        ok = journal_add(PFJ_TRASH, pf.base_crc, s.rel);

    return ok ? PF_STEP_RUN : give_up(PF_SYNC_FAILED, PF_ERR_IO);
}

static pf_step_t step_commit(void) {
    uint32_t crc;

    pfm_close(&s.base_fd);

    if (!pfm_index_seal(s.index_fd, s.count, &crc))
        return give_up(PF_SYNC_FAILED, PF_ERR_IO);

    bool nothing_to_do;
    if (s.mode == PF_SCAN_IMPORT)
        nothing_to_do = pf.base_valid && pf.base_count == s.count && pf.base_crc == crc;
    else
        nothing_to_do = !s.changed;

    if (nothing_to_do) {
        if (s.mode != PF_SCAN_IMPORT)
            memset(pf.scan_touched, 0, sizeof(pf.scan_touched));
        drop_scan();
        return finish(PF_SYNC_NO_CHANGE);
    }

    if (!pfm_close_checked(&s.index_fd))
        return give_up(PF_SYNC_FAILED, PF_ERR_IO);
    if (!journal_add(PFJ_WRITE, PFJ_STAGE_INDEX, PFM_INDEX))
        return give_up(PF_SYNC_FAILED, PF_ERR_IO);

    if (scan_stale()) {
        PF_LOG("Synchronization interrupted by new writes");
        return give_up(PF_SYNC_INTERRUPTED, PF_OK);
    }

    bool committed = pfj_commit(&s.journal);
    s.journal_open = false;
    if (!committed) {
        pf_abs(PFM_JOURNAL);
        sd_remove(pf.path);
        return give_up(PF_SYNC_FAILED, PF_ERR_IO);
    }

    PF_LOG("Changes: %lu modified, %lu created, %lu deleted", (unsigned long)s.n_mod, (unsigned long)s.n_new,
           (unsigned long)s.n_del);

    if (s.mode != PF_SCAN_IMPORT)
        memset(pf.scan_touched, 0, sizeof(pf.scan_touched));
    s.committed = true;
    s.state = SC_APPLY_OPEN;
    return PF_STEP_RUN;
}

static pf_step_t apply_failed(pf_error_t error) {
    close_scan_files();
    pf.error = error;
    return finish(PF_SYNC_FAILED);
}

static pf_step_t step_apply_open(sc_state_t next) {
    if (!pf_abs(PFM_JOURNAL) || (s.jr_fd = pfj_open_ops(pf.path)) < 0)
        return apply_failed(PF_ERR_IO);
    s.state = next;
    return PF_STEP_RUN;
}

static bool trash_entry(const pfj_entry_t *op) {
    char *dst = other_path;
    char rel[64];
    const char *leaf = strrchr(op->path, '/');

    leaf = leaf ? leaf + 1 : op->path;

    if (!pf_abs(PFM_TRASH_DIR) || sd_mkdir(pf.path) != 0)
        return false;
    snprintf(rel, sizeof(rel), "%s/%08lX", PFM_TRASH_DIR, (unsigned long)op->id);
    if (!pf_abs(rel) || sd_mkdir(pf.path) != 0)
        return false;
    if (!pfm_join(dst, sizeof(other_path), pf.path, leaf) || !pf_abs(op->path))
        return false;

    /* Never delete on a collision: a repeated apply must not lose a half-moved save. */
    if (sd_exists(pf.path) && !sd_exists(dst))
        return sd_rename(pf.path, dst) == 0;
    return true;
}

/* The console removed this directory. What the host left in it decides what happens:
 * OS droppings are not worth keeping, or the directory would come back on the next
 * mount, while anything else is not ours to delete. */
static bool remove_directory(void) {
    while (sd_rmdir(pf.path) != 0) {
        int dir_fd = pfm_open(pf.path, O_RDONLY);
        int it = -1;
        bool found = false;

        if (dir_fd < 0)
            return false;
        pfm_iter_next(dir_fd, &it);
        if (it >= 0) {
            found = sd_get_name(it, (char *)host_name, sizeof(host_name)) != 0;
            pfm_close(&it);
        }
        pfm_close(&dir_fd);

        if (!found)
            return false; /* empty, so the rmdir failure was the SD card's */
        if (!pfn_host_junk((char *)host_name)) {
            PF_LOG("Directory kept, not empty: %s", s.op.path);
            return true;
        }
        if (!pfm_join(other_path, sizeof(other_path), pf.path, (char *)host_name) || sd_remove(other_path) != 0)
            return false;
    }
    return true;
}

/* Deletions run first so that a case-only rename, or a file replaced by a directory,
 * cannot collide with its own predecessor. */
static pf_step_t step_apply_remove(void) {
    if (!pfj_next(s.jr_fd, &s.op)) {
        pfm_close(&s.jr_fd);
        return step_apply_open(SC_APPLY_WRITE);
    }

    if (!pf_abs(s.op.path))
        return apply_failed(PF_ERR_LIMIT);

    switch (s.op.type) {
        case PFJ_REMOVE:
            if (sd_exists(pf.path) && sd_remove(pf.path) != 0)
                return apply_failed(PF_ERR_IO);
            break;
        case PFJ_RMDIR:
            if (sd_exists(pf.path) && !remove_directory())
                return apply_failed(PF_ERR_IO);
            break;
        case PFJ_TRASH:
            if (!trash_entry(&s.op))
                return apply_failed(PF_ERR_IO);
            break;
        default:
            break;
    }
    return PF_STEP_RUN;
}

static pf_step_t step_apply_write(void) {
    if (!pfj_next(s.jr_fd, &s.op)) {
        pfm_close(&s.jr_fd);
        s.state = SC_APPLY_DONE;
        return PF_STEP_RUN;
    }

    if (!pf_abs(s.op.path))
        return apply_failed(PF_ERR_LIMIT);

    if (s.op.type == PFJ_MKDIR) {
        if (sd_mkdir(pf.path) != 0)
            return apply_failed(PF_ERR_IO);
    } else if (s.op.type == PFJ_WRITE) {
        char src[PF_BASE_PATH_MAX + 32];

        stage_path(s.op.id, src, sizeof(src));
        if (!sd_exists(src)) {
            /* Stage content outlives the journal, so a committed op without it means
             * the metadata is damaged. Keep everything for the user to resolve. */
            PF_LOG("Recovery required: stage file missing for %s", s.op.path);
            return apply_failed(PF_ERR_RECOVERY);
        }
        s.src_fd = pfm_open(src, O_RDONLY);
        s.dst_fd = pfm_open(pf.path, O_WRONLY | O_CREAT | O_TRUNC);
        if (s.src_fd < 0 || s.dst_fd < 0)
            return apply_failed(PF_ERR_IO);
        s.state = SC_APPLY_COPY;
    }
    return PF_STEP_RUN;
}

static pf_step_t step_apply_copy(void) {
    int n = sd_read(s.src_fd, pf.io, sizeof(pf.io));

    if (n < 0)
        return apply_failed(PF_ERR_IO);
    if (n > 0 && sd_write(s.dst_fd, pf.io, (size_t)n) != n)
        return apply_failed(PF_ERR_IO);

    if (n < (int)sizeof(pf.io)) {
        pfm_close(&s.src_fd);
        if (!pfm_close_checked(&s.dst_fd))
            return apply_failed(PF_ERR_IO);
        s.state = SC_APPLY_WRITE;
    }
    return PF_STEP_RUN;
}

static pf_step_t step_apply_done(void) {
    /* Removing the journal ends the transaction; stage files go after it, so a journal
     * can never point at content that was cleaned up. */
    if (!pf_abs(PFM_JOURNAL) || (sd_exists(pf.path) && sd_remove(pf.path) != 0))
        return apply_failed(PF_ERR_IO);

    s.committed = false;
    s.state = SC_CLEAN;
    return PF_STEP_RUN;
}

void pf_sync_abort(void) {
    if (s.state == SC_IDLE)
        return;
    if (s.committed)
        close_scan_files(); /* the journal stays and is completed by the next mount */
    else if (s.state != SC_FINISHED)
        drop_scan();
    s.committed = false;
    s.state = SC_IDLE;
}

void pf_sync_start(pf_scan_mode_t mode, bool accept_open_files) {
    s.index_fd = s.base_fd = s.stage_fd = s.jr_fd = s.src_fd = s.dst_fd = -1;
    s.journal_open = false;
    s.journal.fd = -1;
    s.mode = mode;
    s.accept_open = accept_open_files;
    s.committed = false;
    s.state = SC_BEGIN;
}

void pf_sync_commit_index(int index_fd, uint32_t count) {
    pf_sync_start(PF_SCAN_IMPORT, true);
    s.index_fd = index_fd;
    s.count = count;
    s.state = SC_COMMIT;
}

void pf_sync_resume(void) {
    s.index_fd = s.base_fd = s.stage_fd = s.jr_fd = s.src_fd = s.dst_fd = -1;
    s.state = SC_APPLY_OPEN;
}

void pf_recover_start(void) {
    pf_sync_start(PF_SCAN_FINAL, true);
    s.state = SC_CLEAN;

    if (!pf_abs(PFM_JOURNAL) || !sd_exists(pf.path))
        return;

    if (pfj_validate(pf.path)) {
        PF_LOG("Completing interrupted transaction");
        s.committed = true;
        s.state = SC_APPLY_OPEN;
    } else {
        /* Never sealed: nothing was applied, so the host tree is the last good state. */
        PF_LOG("Discarding unfinished transaction");
        sd_remove(pf.path);
    }
}

pf_step_t pf_sync_step(void) {
    switch (s.state) {
        case SC_BEGIN: return step_begin();
        case SC_NEXT_DIR: return step_next_dir();
        case SC_PREPASS: return step_prepass();
        case SC_ENTRIES: return step_entries();
        case SC_STAGE: return step_stage();
        case SC_DELETES: return step_deletes();
        case SC_COMMIT: return step_commit();
        case SC_APPLY_OPEN: return step_apply_open(SC_APPLY_REMOVE);
        case SC_APPLY_REMOVE: return step_apply_remove();
        case SC_APPLY_WRITE: return step_apply_write();
        case SC_APPLY_COPY: return step_apply_copy();
        case SC_APPLY_DONE: return step_apply_done();
        case SC_CLEAN:
            stage_clean();
            pf_base_load();
            return finish(PF_SYNC_COMMITTED);
        case SC_FINISHED:
        case SC_IDLE:
            return PF_STEP_DONE;
    }
    return PF_STEP_FAILED;
}

#ifdef PS2_FOLDER_HOST_TEST
void pf_sync_power_loss(void) {
    memset(&s, 0, sizeof(s));
    pfm_forget_fds();
}
#endif

pf_sync_result_t pf_sync_result(void) {
    return s.result;
}

bool pf_sync_committed(void) {
    return s.committed;
}

uint32_t pf_sync_generation(void) {
    return s.start_gen;
}
