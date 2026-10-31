#include <string.h>

#include "ps2_folder_internal.h"
#include "ps2_folder_port.h"
#include "sd.h"

/* Loads the file contents the importer left out, while the console is already using the
 * card. A page that is not ready yet has one possible content, what its file holds or
 * erased, so the console sees the card as it would an image being loaded. It can only
 * add to that by writing, and a page the console wrote is never loaded over.
 *
 * A page the console is waiting for goes first, read on its own. The sweep reads every
 * file from start to end whatever has been served already, because the CRC of what is
 * on SD is what the index must hold, however much of it the console has replaced.
 *
 * The stage index holds the work list: it lists every entry the card started with, and
 * PSRAM cannot serve for that because the console can change it. pf.map gives each
 * record its first cluster. It stays free until the first scan, which cannot start
 * before the card is complete. pf.base_hash is used for what the sweep learns, and is
 * rebuilt once the index is committed:
 *
 *   directory: first child << 16 | number of children
 *   file:      CRC of its contents */

#define CHASE_CLUSTERS      32U     /* read ahead of a console reading a file in order */
#define TAIL_PAGES_PER_STEP 32U

typedef enum {
    FL_FILES,
    FL_TAIL,
    FL_SEAL,
} fl_state_t;

static struct {
    fl_state_t state;
    int list_fd;
    int src_fd;
    int serve_fd;
    uint32_t count;
    uint32_t free_cluster;  /* the first one no file or directory uses */

    /* sweep */
    uint32_t record;
    uint32_t cluster;
    uint32_t left;
    uint32_t crc;
    uint32_t tail_page;

    /* console */
    uint32_t serve_record;
    uint32_t serve_first;
    uint32_t serve_size;
    uint32_t chase_cluster;
    uint32_t chase_left;
    uint32_t served;
    uint32_t longest_us;
} fl = {.list_fd = -1, .src_fd = -1, .serve_fd = -1};

static char rel[PFM_PATH_MAX];

void pf_fill_start(int list_fd, uint32_t count, uint32_t free_cluster) {
    memset(&fl, 0, sizeof(fl));
    fl.state = FL_FILES;
    fl.list_fd = list_fd;
    fl.src_fd = fl.serve_fd = -1;
    fl.count = count;
    fl.free_cluster = free_cluster;
    memset(pf.base_hash, 0, sizeof(pf.base_hash));
}

void pf_fill_abort(void) {
    pfm_close(&fl.list_fd);
    pfm_close(&fl.src_fd);
    pfm_close(&fl.serve_fd);
}

#ifdef PS2_FOLDER_HOST_TEST
void pf_fill_power_loss(void) {
    fl.list_fd = fl.src_fd = fl.serve_fd = -1;
}
#endif

int pf_fill_progress(void) {
    uint32_t cluster;

    if (fl.state == FL_FILES)
        cluster = pf.fs.alloc_offset + fl.cluster;
    else if (fl.state == FL_TAIL)
        cluster = fl.tail_page / PFS_PAGES_PER_CLUSTER;
    else
        return 100;
    return (int)(cluster * 100U / PFS_TOTAL_CLUSTERS);
}

static pf_step_t fail(void) {
    pf.error = PF_ERR_IO;
    return PF_STEP_FAILED;
}

static uint32_t first_page(uint32_t cluster) {
    return (pf.fs.alloc_offset + cluster) * PFS_PAGES_PER_CLUSTER;
}

/* Takes chunk bytes of content in pf.io. */
static void store_cluster(uint32_t cluster, uint32_t chunk) {
    uint32_t page = first_page(cluster);

    memset(&pf.io[chunk], 0xFF, PFS_CLUSTER_SIZE - chunk);
    pf_page_fill(page, pf.io);
    pf_page_fill(page + 1, &pf.io[PFS_PAGE_SIZE]);
}

static int open_record(uint32_t record) {
    if (!pfm_record_path(fl.list_fd, record, rel, sizeof(rel)) || !pf_abs(rel))
        return -1;
    return pfm_open(pf.path, O_RDONLY);
}

/* Files are contiguous and no directory cluster lies inside one, so this is the file
 * with the nearest start. */
static uint32_t record_of(uint32_t cluster) {
    uint32_t found = 0;

    for (uint32_t i = 1; i < fl.count; i++) {
        if (pf.map[i] != PFM_NONE && pf.map[i] <= cluster && (found == 0 || pf.map[i] > pf.map[found]))
            found = i;
    }
    return found;
}

static bool serve_open(uint32_t cluster) {
    uint32_t record = record_of(cluster);
    pfm_record_t rec;

    if (record == fl.serve_record && fl.serve_fd >= 0)
        return true;

    pfm_close(&fl.serve_fd);
    if (record == 0 || !pfm_index_read(fl.list_fd, record, &rec))
        return false;
    fl.serve_fd = open_record(record);
    fl.serve_record = record;
    fl.serve_first = rec.cluster;
    fl.serve_size = rec.size;
    return fl.serve_fd >= 0;
}

static bool serve_cluster(uint32_t cluster) {
    uint32_t offset = (cluster - fl.serve_first) * PFS_CLUSTER_SIZE;
    uint32_t chunk = fl.serve_size - offset;

    if (chunk > PFS_CLUSTER_SIZE)
        chunk = PFS_CLUSTER_SIZE;
    if (sd_seek(fl.serve_fd, (int32_t)offset, SEEK_SET) != 0 || sd_read(fl.serve_fd, pf.io, chunk) != (int)chunk)
        return false;
    store_cluster(cluster, chunk);
    return true;
}

static pf_step_t serve_page(uint32_t page) {
    uint32_t cluster = page / PFS_PAGES_PER_CLUSTER - pf.fs.alloc_offset;
    uint64_t start = pf_time_us();

    fl.served++;
    if (cluster >= fl.free_cluster) {
        memset(pf.io, 0xFF, PFS_PAGE_SIZE);
        pf_page_fill(page, pf.io);
    } else {
        if (!serve_open(cluster) || !serve_cluster(cluster))
            return fail();
        fl.chase_cluster = cluster + 1;
        fl.chase_left = CHASE_CLUSTERS;
    }

    uint32_t took = (uint32_t)(pf_time_us() - start);

    if (took > fl.longest_us)
        fl.longest_us = took;
    return PF_STEP_RUN;
}

static pf_step_t chase_step(void) {
    uint32_t cluster = fl.chase_cluster++;

    fl.chase_left--;
    if ((cluster - fl.serve_first) * PFS_CLUSTER_SIZE >= fl.serve_size) {
        fl.chase_left = 0;
        return PF_STEP_RUN;
    }
    if (pf_page_ready(first_page(cluster)) && pf_page_ready(first_page(cluster) + 1))
        return PF_STEP_RUN;
    return serve_cluster(cluster) ? PF_STEP_RUN : fail();
}

static pf_step_t next_file(void) {
    pfm_record_t rec;

    if (fl.record >= fl.count) {
        fl.tail_page = first_page(fl.free_cluster);
        fl.state = FL_TAIL;
        return PF_STEP_RUN;
    }

    if (!pfm_index_read(fl.list_fd, fl.record, &rec))
        return fail();

    /* Children are contiguous and come in order. */
    if (fl.record > 0) {
        if (pf.base_hash[rec.parent] == 0)
            pf.base_hash[rec.parent] = fl.record << 16;
        pf.base_hash[rec.parent]++;
    }

    if ((rec.mode & PFS_MODE_DIR) || rec.size == 0) {
        fl.record++;
        return PF_STEP_RUN;
    }

    fl.src_fd = open_record(fl.record);
    if (fl.src_fd < 0)
        return fail();
    fl.cluster = rec.cluster;
    fl.left = rec.size;
    fl.crc = 0;
    return PF_STEP_RUN;
}

static pf_step_t files_step(void) {
    if (fl.src_fd < 0)
        return next_file();

    uint32_t chunk = fl.left > PFS_CLUSTER_SIZE ? PFS_CLUSTER_SIZE : fl.left;

    if (sd_read(fl.src_fd, pf.io, chunk) != (int)chunk)
        return fail();
    fl.crc = pfm_crc32(fl.crc, pf.io, chunk);
    store_cluster(fl.cluster, chunk);

    fl.cluster++;
    fl.left -= chunk;
    if (fl.left == 0) {
        pf.base_hash[fl.record] = fl.crc;
        pfm_close(&fl.src_fd);
        fl.record++;
    }
    return PF_STEP_RUN;
}

static pf_step_t tail_step(void) {
    uint32_t end = PFS_CARD_SIZE / PFS_PAGE_SIZE;

    memset(pf.io, 0xFF, PFS_PAGE_SIZE);
    for (uint32_t i = 0; i < TAIL_PAGES_PER_STEP && fl.tail_page < end; i++)
        pf_page_fill(fl.tail_page++, pf.io);

    if (fl.tail_page >= end) {
        fl.record = 0;
        fl.state = FL_SEAL;
    }
    return PF_STEP_RUN;
}

/* The records now say what was read from SD, whatever the console has done since. */
static pf_step_t seal_step(void) {
    pfm_record_t rec;
    uint32_t info;

    if (fl.record >= fl.count) {
        PF_LOG("Console waited for %lu reads, longest %lu us", (unsigned long)fl.served, (unsigned long)fl.longest_us);
        pfm_close(&fl.serve_fd);
        pf_sync_commit_index(fl.list_fd, fl.count);
        fl.list_fd = -1;
        return PF_STEP_DONE;
    }

    if (!pfm_index_read(fl.list_fd, fl.record, &rec))
        return fail();
    info = pf.base_hash[fl.record];
    if (rec.mode & PFS_MODE_DIR) {
        rec.first_child = (uint16_t)(info >> 16);
        rec.child_count = (uint16_t)info;
        rec.size = rec.child_count + 2U;
        rec.cluster = pf.map[fl.record];
    } else {
        rec.crc = info;
    }
    if (!pfm_index_write(fl.list_fd, fl.record, &rec))
        return fail();
    fl.record++;
    return PF_STEP_RUN;
}

pf_step_t pf_fill_step(void) {
    int page = pf_page_wanted();

    if (page >= 0)
        return serve_page((uint32_t)page);
    if (fl.chase_left > 0)
        return chase_step();

    switch (fl.state) {
        case FL_FILES:
            return files_step();
        case FL_TAIL:
            return tail_step();
        case FL_SEAL:
            return seal_step();
    }
    return fail();
}
