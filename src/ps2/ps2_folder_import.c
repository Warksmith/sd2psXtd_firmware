#include <string.h>

#include "ps2_card_format.h"
#include "ps2_folder_internal.h"
#include "ps2_folder_name.h"
#include "ps2_folder_port.h"
#include "sd.h"

/* Builds the PS2 filesystem in PSRAM from the host tree, except for what files contain.
 * Every file gets contiguous clusters and a FAT chain from its size alone, so a card
 * is ready for the console after a walk over the names; ps2_folder_fill.c loads the
 * contents behind it. Runs as a state machine so the main loop keeps running.
 *
 * The directory being built doubles as the work list: subdirectories are descended
 * into after all of a directory's entries exist, by re-reading its entries from
 * PSRAM, so no name list has to be kept in SRAM.
 *
 * If the folder still matches its stamp, the names and sizes come from index.bin and the
 * folder is not opened at all, which is most of what this costs on an SD card.
 *
 * Each entry also becomes a record in the stage index, which is how the fill finds its
 * files once PSRAM belongs to the console. It is written in batches: alternating
 * between the directory being walked and this file costs an SD write per entry. */

#define FORMAT_PAGES_PER_STEP   128U
#define FORMAT_META_PAGES       128U    /* superblock to root directory on an 8 MB card */
#define HOST_NAME_MAX_LEN       256U
#define STASH_STRICT            0x0001U
#define STASH_OLD_SHIFT         16
#define LIST_BATCH              (PFS_CLUSTER_SIZE / sizeof(pfm_record_t))
#define TREE_CHECK_US           (2000U * 1000U)

typedef enum {
    IMP_FORMAT,
    IMP_DIR_OPEN,
    IMP_KNOWN,
    IMP_HOST_OPEN,
    IMP_HOST,
    IMP_DIR_CLOSE,
    IMP_DESCEND,
    IMP_DONE,
} imp_state_t;

typedef struct {
    uint32_t first_cluster;
    uint32_t tail_cluster;
    uint32_t walk_cluster;
    uint32_t parent_cluster;
    uint32_t parent_slot;
    uint32_t count;             /* entries so far, including . and .. */
    uint32_t walk_slot;
    uint16_t old_idx;
    uint16_t old_next;
    uint16_t old_end;
    uint16_t old_first;
    uint16_t path_len;
    uint16_t record;
    uint16_t first_child;
} frame_t;

static struct {
    imp_state_t state;
    uint32_t format_page;
    uint32_t next_cluster;
    uint32_t total_entries;
    int depth;
    frame_t frames[PFM_MAX_DEPTH + 1];

    int old_fd;
    int dir_fd;
    int it;
    int list_fd;
    uint32_t records;
    bool trusted;

    /* the entry being created */
    pfs_entry_t entry;
    pfm_record_t old;
    bool has_old;
    bool strict;
    uint32_t chain;
    char host_name[HOST_NAME_MAX_LEN];
} imp;

static const uint8_t default_time[8] = {0, 0, 0, 12, 1, 1, 0xD0, 0x07}; /* 2000-01-01 12:00 */

static pf_step_t fail(pf_error_t error) {
    pf.error = error;
    return PF_STEP_FAILED;
}

static void close_all(void) {
    pfm_close(&imp.old_fd);
    pfm_close(&imp.dir_fd);
    if (imp.it >= 0)
        pfm_close(&imp.it);
}

void pf_import_abort(void) {
    close_all();
    pfm_close(&imp.list_fd);
}

#ifdef PS2_FOLDER_HOST_TEST
void pf_import_power_loss(void) {
    imp.old_fd = imp.dir_fd = imp.it = imp.list_fd = -1;
}
#endif

void pf_import_start(void) {
    memset(&imp, 0, sizeof(imp));
    imp.state = IMP_FORMAT;
    imp.old_fd = imp.dir_fd = imp.it = imp.list_fd = -1;
    imp.next_cluster = 1; /* the root directory owns cluster 0 */
    pf_pages_reset();

    if (pf.base_valid && pf_abs(PFM_INDEX))
        imp.old_fd = pfm_open(pf.path, O_RDONLY);
}

bool pf_import_trusted(void) {
    return imp.trusted;
}

int pf_import_progress(void) {
    if (imp.state == IMP_FORMAT)
        return 0;
    return (int)(imp.next_cluster * 100 / pf.fs.max_alloc);
}

static frame_t *top(void) {
    return &imp.frames[imp.depth];
}

static bool alloc_cluster(uint32_t *cluster) {
    if (imp.next_cluster >= pf.fs.max_alloc)
        return false;
    *cluster = imp.next_cluster++;
    return pfs_fat_set(&pf.fs, *cluster, PFS_FAT_END);
}

static bool link_cluster(uint32_t from, uint32_t to) {
    return pfs_fat_set(&pf.fs, from, to | PFS_FAT_USED);
}

/* Only directory clusters are complete as soon as they are written. */
static bool alloc_dir_cluster(uint32_t *cluster) {
    if (!alloc_cluster(cluster))
        return false;
    pf_pages_ready((pf.fs.alloc_offset + *cluster) * PFS_PAGES_PER_CLUSTER, PFS_PAGES_PER_CLUSTER);
    return true;
}

static bool list_flush(size_t records) {
    int len = (int)(records * sizeof(pfm_record_t));

    return sd_write(imp.list_fd, pf.io, (size_t)len) == len;
}

static bool list_add(const pfm_record_t *rec) {
    uint32_t slot = imp.records % LIST_BATCH;

    memcpy(&pf.io[slot * sizeof(*rec)], rec, sizeof(*rec));
    imp.records++;
    return slot + 1 < LIST_BATCH || list_flush(LIST_BATCH);
}

/* Lists imp.entry under the record of its directory. */
static bool list_entry(uint32_t parent) {
    pfm_record_t rec;

    pf_fill_record(&rec, &imp.entry, parent);
    rec.chain = imp.chain;
    if (imp.strict)
        rec.flags |= PFM_FLAG_STRICT;
    pf.map[imp.records] = imp.entry.cluster == PFS_NO_CLUSTER ? PFM_NONE : (uint16_t)imp.entry.cluster;
    return list_add(&rec);
}

static void slot_location(const frame_t *f, uint32_t slot, uint32_t *cluster) {
    uint32_t c = f->first_cluster;

    for (uint32_t i = 0; i < slot / PFS_ENTRIES_PER_CLUSTER; i++) {
        uint32_t next = PFS_NO_CLUSTER;
        pfs_fat_next(&pf.fs, c, &next);
        c = next;
    }
    *cluster = c;
}

/* Appends the finished imp.entry to the directory on top of the stack. */
static pf_step_t emit_entry(void) {
    frame_t *f = top();
    uint32_t slot = f->count;
    uint32_t live = slot - 2;
    uint32_t fold = pfn_fold_hash(imp.entry.name);

    if (live >= PF_MAX_DIR_ENTRIES || imp.total_entries + 1 >= PF_MAX_ENTRIES) {
        PF_LOG("Import failed: too many entries");
        return fail(PF_ERR_LIMIT);
    }

    for (uint32_t i = 0; i < live; i++) {
        if (pf.fold[i] == fold) {
            pfs_entry_t other;
            uint32_t cluster;

            slot_location(f, i + 2, &cluster);
            pfs_entry_read(&pf.fs, cluster, (i + 2) % PFS_ENTRIES_PER_CLUSTER, &other);
            if (memcmp(other.name, imp.entry.name, PFS_NAME_LEN) == 0) {
                PF_LOG("Import failed: duplicate name %s", imp.entry.name);
                return fail(PF_ERR_NAME);
            }
        }
    }

    if (slot % PFS_ENTRIES_PER_CLUSTER == 0) {
        uint32_t cluster;
        uint8_t blank[PFS_PAGE_SIZE];

        if (!alloc_dir_cluster(&cluster)) {
            PF_LOG("Import failed: insufficient capacity");
            return fail(PF_ERR_CAPACITY);
        }
        link_cluster(f->tail_cluster, cluster);
        f->tail_cluster = cluster;

        /* Its second page may stay unused, and PSRAM still holds the previous card. */
        memset(blank, 0xFF, sizeof(blank));
        pf_psram_write(pfs_page_addr(&pf.fs, cluster, 1), blank, sizeof(blank));
    }

    if (!list_entry(f->record))
        return fail(PF_ERR_IO);

    pfs_entry_write(&pf.fs, f->tail_cluster, slot % PFS_ENTRIES_PER_CLUSTER, &imp.entry);
    pf.fold[live] = fold;
    f->count++;
    imp.total_entries++;
    return PF_STEP_RUN;
}

static uint16_t sanitized_mode(bool is_dir) {
    uint16_t type = is_dir ? PFS_MODE_DIR : PFS_MODE_FILE;

    if (imp.has_old && (imp.old.mode & PFS_MODE_EXISTS)
        && (imp.old.mode & (PFS_MODE_DIR | PFS_MODE_FILE)) == type)
        return imp.old.mode;
    return is_dir ? PFS_MODE_DEFAULT_DIR : PFS_MODE_DEFAULT_FILE;
}

/* Creates imp.entry. stat may only be NULL for an entry whose size the index has right. */
static pf_step_t add_entry(bool is_dir, uint32_t size, const ps2_fileio_stat_t *stat, const char raw[PFS_NAME_LEN],
                           bool strict) {
    uint32_t prev = PFS_NO_CLUSTER;

    memset(&imp.entry, 0, sizeof(imp.entry));
    memcpy(imp.entry.name, raw, PFS_NAME_LEN);
    imp.strict = strict;

    imp.entry.mode = sanitized_mode(is_dir);
    if (imp.has_old) {
        memcpy(imp.entry.created, imp.old.created, 8);
        memcpy(imp.entry.modified, imp.old.modified, 8);
        imp.entry.attr = imp.old.attr;
    } else {
        memcpy(imp.entry.created, stat->ctime, 8);
        memcpy(imp.entry.modified, stat->mtime, 8);
    }

    if (is_dir) {
        uint16_t old_idx = imp.has_old ? (uint16_t)(top()->old_next - 1) : PFM_NONE;

        imp.entry.cluster = PFS_NO_CLUSTER;
        imp.entry.stash = (strict ? STASH_STRICT : 0U) | ((uint32_t)old_idx << STASH_OLD_SHIFT);
        imp.chain = 0;
        return emit_entry();
    }

    if (size / PFS_CLUSTER_SIZE >= pf.fs.max_alloc) {
        PF_LOG("Import failed: insufficient capacity");
        return fail(PF_ERR_CAPACITY);
    }

    /* The same size can still be an edit, which only the fill can see. */
    if (imp.has_old && imp.old.size != size)
        memcpy(imp.entry.modified, stat->mtime, 8);

    imp.entry.length = size;
    imp.entry.cluster = PFS_NO_CLUSTER;
    imp.chain = PF_CHAIN_START;
    for (uint32_t left = (size + PFS_CLUSTER_SIZE - 1) / PFS_CLUSTER_SIZE; left > 0; left--) {
        uint32_t cluster;

        if (!alloc_cluster(&cluster)) {
            PF_LOG("Import failed: insufficient capacity");
            return fail(PF_ERR_CAPACITY);
        }
        if (prev == PFS_NO_CLUSTER)
            imp.entry.cluster = cluster;
        else
            link_cluster(prev, cluster);
        prev = cluster;
        imp.chain = pf_chain_add(imp.chain, cluster);
    }
    return emit_entry();
}

/* Prepare imp.entry for a host object opened as fd. */
static pf_step_t begin_entry(int fd, bool owned, const char raw[PFS_NAME_LEN], bool strict) {
    ps2_fileio_stat_t stat;
    bool is_dir = sd_is_dir(fd);
    uint32_t size = is_dir ? 0 : (uint32_t)sd_filesize(fd);

    memset(&stat, 0, sizeof(stat));
    sd_get_stat(fd, &stat);
    if (owned)
        pfm_close(&fd);
    return add_entry(is_dir, size, &stat, raw, strict);
}

static void write_dot_entries(frame_t *f, const pfs_entry_t *self) {
    pfs_entry_t dot, dotdot;

    memset(&dot, 0, sizeof(dot));
    memset(&dotdot, 0, sizeof(dotdot));

    dot.mode = PFS_MODE_DEFAULT_DIR;
    memcpy(dot.created, self->created, 8);
    memcpy(dot.modified, self->modified, 8);
    dot.attr = self->attr;
    dot.name[0] = '.';

    dotdot.name[0] = dotdot.name[1] = '.';

    if (imp.depth == 0) {
        dot.mode = self->mode;
        dot.length = 2;
        dotdot.mode = PFS_MODE_ROOT_DOTDOT;
        memcpy(dotdot.created, self->created, 8);
        memcpy(dotdot.modified, self->modified, 8);
    } else {
        pfs_entry_t parent_dot;
        pfs_entry_read(&pf.fs, f->parent_cluster, 0, &parent_dot);

        dot.cluster = f->parent_cluster;
        dot.dir_entry = f->parent_slot;
        dotdot.mode = PFS_MODE_DEFAULT_DIR;
        dotdot.cluster = parent_dot.cluster;
        dotdot.dir_entry = parent_dot.dir_entry;
        memcpy(dotdot.created, parent_dot.created, 8);
        memcpy(dotdot.modified, parent_dot.modified, 8);
    }

    pfs_entry_write(&pf.fs, f->first_cluster, 0, &dot);
    pfs_entry_write(&pf.fs, f->first_cluster, 1, &dotdot);
}

static void frame_set_old(frame_t *f, uint16_t old_idx) {
    pfm_record_t rec;

    f->old_idx = old_idx;
    f->old_next = f->old_end = f->old_first = 0;
    if (old_idx != PFM_NONE && imp.old_fd >= 0 && pfm_index_read(imp.old_fd, old_idx, &rec)
        && (rec.mode & PFS_MODE_DIR)) {
        f->old_first = f->old_next = rec.first_child;
        f->old_end = (uint16_t)(rec.first_child + rec.child_count);
    } else {
        f->old_idx = PFM_NONE;
    }
}

static pf_step_t step_dir_open(void) {
    frame_t *f = top();
    pfs_entry_t self;

    if (imp.depth == 0) {
        pfm_record_t rec;

        memset(&self, 0, sizeof(self));
        self.mode = PFS_MODE_DEFAULT_DIR;
        memcpy(self.created, default_time, 8);
        memcpy(self.modified, default_time, 8);
        if (imp.old_fd >= 0 && pfm_index_read(imp.old_fd, 0, &rec)) {
            self.mode = rec.mode;
            self.attr = rec.attr;
            memcpy(self.created, rec.created, 8);
            memcpy(self.modified, rec.modified, 8);
        }
        f->first_cluster = f->tail_cluster = pf.fs.root_cluster;
        f->path_len = (uint16_t)strlen(pf.path);

        imp.entry = self;
        imp.entry.cluster = pf.fs.root_cluster;
        imp.chain = 0;
        imp.strict = false;
        if (!list_entry(0))
            return fail(PF_ERR_IO);
    } else {
        uint32_t cluster;

        if (!alloc_dir_cluster(&cluster)) {
            PF_LOG("Import failed: insufficient capacity");
            return fail(PF_ERR_CAPACITY);
        }
        f->first_cluster = f->tail_cluster = cluster;
        pf.map[f->record] = (uint16_t)cluster;
    }

    f->first_child = (uint16_t)imp.records;
    f->count = 2;
    imp.state = IMP_KNOWN;

    if (imp.depth > 0) {
        frame_t *parent = &imp.frames[imp.depth - 1];
        uint32_t cluster;

        slot_location(parent, f->parent_slot, &cluster);
        pfs_entry_read(&pf.fs, cluster, f->parent_slot % PFS_ENTRIES_PER_CLUSTER, &self);
        self.cluster = f->first_cluster;
        self.stash = 0;
        pfs_entry_write(&pf.fs, cluster, f->parent_slot % PFS_ENTRIES_PER_CLUSTER, &self);
        f->parent_cluster = parent->first_cluster;
    }

    write_dot_entries(f, &self);
    return PF_STEP_RUN;
}

/* Entries the index knows, in their stored order, so a remount preserves ordering. */
static pf_step_t step_known(void) {
    frame_t *f = top();

    if (f->old_idx == PFM_NONE || f->old_next >= f->old_end) {
        pfm_close(&imp.dir_fd); /* iterating it needs a fresh start */
        imp.state = imp.trusted ? IMP_DIR_CLOSE : IMP_HOST_OPEN;
        return PF_STEP_RUN;
    }

    if (!imp.trusted && imp.dir_fd < 0) {
        pf.path[f->path_len] = 0;
        imp.dir_fd = pfm_open(pf.path, O_RDONLY);
        if (imp.dir_fd < 0)
            return fail(PF_ERR_IO);
    }

    if (!pfm_index_read(imp.old_fd, f->old_next, &imp.old)) {
        /* the index was validated when it was loaded */
        return fail(PF_ERR_IO);
    }
    f->old_next++;

    if (imp.trusted) {
        imp.has_old = true;
        return add_entry((imp.old.mode & PFS_MODE_DIR) != 0, imp.old.size, NULL, imp.old.name,
                         (imp.old.flags & PFM_FLAG_STRICT) != 0);
    }

    char host[PFN_HOST_NAME_MAX];
    pfn_encode(imp.old.name, (imp.old.flags & PFM_FLAG_STRICT) != 0, host);

    int fd = pfm_open_at(imp.dir_fd, host, O_RDONLY);
    if (fd < 0) {
        /* Usually removed on the host while unmounted, but a failed open says nothing
         * about why, and dropping a save over a read error would delete it on SD. */
        pf.path[f->path_len] = 0;
        if (f->path_len + 1 + strlen(host) + 1 > sizeof(pf.path))
            return fail(PF_ERR_LIMIT);
        pf.path[f->path_len] = '/';
        strcpy(&pf.path[f->path_len + 1], host);

        bool removed = !sd_exists(pf.path);

        if (!removed)
            fd = pfm_open(pf.path, O_RDONLY);
        pf.path[f->path_len] = 0;
        if (removed)
            return PF_STEP_RUN;
        if (fd < 0)
            return fail(PF_ERR_IO);
    }

    imp.has_old = true;
    return begin_entry(fd, true, imp.old.name, (imp.old.flags & PFM_FLAG_STRICT) != 0);
}

static bool known_child(const frame_t *f, const char raw[PFS_NAME_LEN]) {
    pfm_record_t rec;
    uint32_t key;

    if (f->old_idx == PFM_NONE)
        return false;

    key = pf_base_key(f->old_idx, pfn_fold_hash(raw));
    for (uint32_t i = f->old_first; i < f->old_end; i++) {
        if (pf.base_hash[i] == key && pfm_index_read(imp.old_fd, i, &rec) && pfn_fold_equal(rec.name, raw))
            return true;
    }
    return false;
}

/* Everything the index does not know: new saves copied in from a computer. */
static pf_step_t step_host(void) {
    frame_t *f = top();
    char *name = imp.host_name;
    char raw[PFS_NAME_LEN];
    bool strict;

    pfm_iter_next(imp.dir_fd, &imp.it);
    if (imp.it < 0) {
        pfm_close(&imp.dir_fd);
        imp.state = IMP_DIR_CLOSE;
        return PF_STEP_RUN;
    }

    if (sd_get_name(imp.it, name, sizeof(imp.host_name)) == 0)
        return fail(PF_ERR_NAME);

    if ((imp.depth == 0 && strcmp(name, PFM_DIR) == 0) || pfn_host_junk(name))
        return PF_STEP_RUN;

    if (!pfn_decode(name, raw, &strict)) {
        PF_LOG("Import failed: unsupported host name %s", name);
        return fail(PF_ERR_NAME);
    }

    if (known_child(f, raw))
        return PF_STEP_RUN;

    imp.has_old = false;
    return begin_entry(imp.it, false, raw, strict);
}

static void finish_directory(void) {
    frame_t *f = top();
    pfs_entry_t self;

    if (imp.depth == 0) {
        pfs_entry_read(&pf.fs, f->first_cluster, 0, &self);
        self.length = f->count;
        pfs_entry_write(&pf.fs, f->first_cluster, 0, &self);
    } else {
        frame_t *parent = &imp.frames[imp.depth - 1];
        uint32_t cluster;

        slot_location(parent, f->parent_slot, &cluster);
        pfs_entry_read(&pf.fs, cluster, f->parent_slot % PFS_ENTRIES_PER_CLUSTER, &self);
        self.length = f->count;
        pfs_entry_write(&pf.fs, cluster, f->parent_slot % PFS_ENTRIES_PER_CLUSTER, &self);
    }

    f->walk_slot = 2;
    slot_location(f, 2, &f->walk_cluster);
}

static pf_step_t step_descend(void) {
    frame_t *f = top();

    if (f->walk_slot >= f->count) {
        pf.path[f->path_len] = 0;
        if (imp.depth == 0) {
            imp.state = IMP_DONE;
            return PF_STEP_RUN;
        }
        imp.depth--;
        pf.path[top()->path_len] = 0;
        return PF_STEP_RUN;
    }

    pfs_entry_t entry;
    uint32_t slot = f->walk_slot;

    pfs_entry_read(&pf.fs, f->walk_cluster, slot % PFS_ENTRIES_PER_CLUSTER, &entry);
    f->walk_slot++;
    if (f->walk_slot % PFS_ENTRIES_PER_CLUSTER == 0 && f->walk_slot < f->count) {
        if (!pfs_fat_next(&pf.fs, f->walk_cluster, &f->walk_cluster))
            return fail(PF_ERR_INVALID_FS);
    }

    if (!(entry.mode & PFS_MODE_DIR))
        return PF_STEP_RUN;

    if (imp.depth >= PFM_MAX_DEPTH) {
        PF_LOG("Import failed: directories nested too deeply");
        return fail(PF_ERR_LIMIT);
    }

    char host[PFN_HOST_NAME_MAX];
    size_t len = strlen(pf.path);

    pfn_encode(entry.name, (entry.stash & STASH_STRICT) != 0, host);
    if (len + 1 + strlen(host) + 1 > sizeof(pf.path))
        return fail(PF_ERR_LIMIT);
    pf.path[len] = '/';
    strcpy(&pf.path[len + 1], host);

    imp.depth++;
    frame_t *child = top();
    memset(child, 0, sizeof(*child));
    child->parent_slot = slot;
    child->record = (uint16_t)(f->first_child + slot - 2);
    child->path_len = (uint16_t)strlen(pf.path);
    frame_set_old(child, (uint16_t)(entry.stash >> STASH_OLD_SHIFT));
    imp.state = IMP_DIR_OPEN;
    return PF_STEP_RUN;
}

pf_step_t pf_import_step(void) {
    switch (imp.state) {
        case IMP_FORMAT:
            /* Only what precedes the first file: what the files leave over is erased
             * by the fill, once it is known how little that is. */
            for (uint32_t i = 0; i < FORMAT_PAGES_PER_STEP && imp.format_page < FORMAT_META_PAGES; i++) {
                ps2_card_format_page(PFS_CARD_SIZE, imp.format_page * PFS_PAGE_SIZE, pf.io);
                pf_psram_write(imp.format_page * PFS_PAGE_SIZE, pf.io, PFS_PAGE_SIZE);
                imp.format_page++;
            }
            if (imp.format_page >= FORMAT_META_PAGES) {
                if (!pfs_mount(&pf.fs))
                    return fail(PF_ERR_INVALID_FS);
                if (pf.fs.alloc_offset + pf.fs.root_cluster + 1 > FORMAT_META_PAGES / PFS_PAGES_PER_CLUSTER)
                    return fail(PF_ERR_INVALID_FS);
                pf_pages_ready(0, (pf.fs.alloc_offset + pf.fs.root_cluster + 1) * PFS_PAGES_PER_CLUSTER);
                if (!pf_meta_dirs())
                    return fail(PF_ERR_IO);
                if (!pf_abs(PFM_STAGE_INDEX) || (imp.list_fd = pfm_index_create(pf.path)) < 0)
                    return fail(PF_ERR_IO);
                imp.trusted = imp.old_fd >= 0 && pf_tree_unchanged(TREE_CHECK_US);
                if (!pf_abs(""))
                    return fail(PF_ERR_LIMIT);
                frame_set_old(&imp.frames[0], 0);
                imp.state = IMP_DIR_OPEN;
            }
            return PF_STEP_RUN;

        case IMP_DIR_OPEN:
            return step_dir_open();
        case IMP_KNOWN:
            return step_known();

        case IMP_HOST_OPEN:
            pf.path[top()->path_len] = 0;
            imp.dir_fd = pfm_open(pf.path, O_RDONLY);
            if (imp.dir_fd < 0)
                return fail(PF_ERR_IO);
            imp.it = -1;
            imp.state = IMP_HOST;
            return PF_STEP_RUN;

        case IMP_HOST:
            return step_host();

        case IMP_DIR_CLOSE:
            finish_directory();
            imp.state = IMP_DESCEND;
            return PF_STEP_RUN;

        case IMP_DESCEND:
            return step_descend();

        case IMP_DONE:
            pfs_fat_flush(&pf.fs);
            close_all();
            if (!list_flush(imp.records % LIST_BATCH))
                return fail(PF_ERR_IO);
            pf_fill_start(imp.list_fd, imp.records, imp.next_cluster);
            imp.list_fd = -1;
            return PF_STEP_DONE;
    }
    return fail(PF_ERR_IO);
}
