#include "ps2_folder_meta.h"

#include <stdio.h>
#include <string.h>

#include "ps2_folder_name.h"
#include "ps2_folder_port.h"
#include "sd.h"

#define INDEX_MAGIC     "PS2FIDX1"
#define INDEX_END_MAGIC "PS2FEND1"
#define JOURNAL_MAGIC   "PS2FJRN1"
#define JOURNAL_END     "PS2FJEND"
#define MAGIC_LEN       8U
#define INDEX_VERSION   1U
#define INDEX_HEADER    16U
#define INDEX_TRAILER   16U
#define JOURNAL_HEADER  MAGIC_LEN
#define JOURNAL_TRAILER 16U
#define OP_HEADER       8U
#define MAX_TRACKED_FDS 8

static int tracked_fds[MAX_TRACKED_FDS];
static bool tracked_ready;

static void track_init(void) {
    if (!tracked_ready) {
        for (int i = 0; i < MAX_TRACKED_FDS; i++)
            tracked_fds[i] = -1;
        tracked_ready = true;
    }
}

static void track_add(int fd) {
    track_init();
    for (int i = 0; i < MAX_TRACKED_FDS; i++) {
        if (tracked_fds[i] == fd)
            return;
    }
    for (int i = 0; i < MAX_TRACKED_FDS; i++) {
        if (tracked_fds[i] < 0) {
            tracked_fds[i] = fd;
            return;
        }
    }
}

static void track_remove(int fd) {
    track_init();
    for (int i = 0; i < MAX_TRACKED_FDS; i++) {
        if (tracked_fds[i] == fd)
            tracked_fds[i] = -1;
    }
}

#ifdef PS2_FOLDER_HOST_TEST
void pfm_forget_fds(void) {
    tracked_ready = false;
}
#endif

bool pfm_owns_fd(int fd) {
    track_init();
    for (int i = 0; i < MAX_TRACKED_FDS; i++) {
        if (tracked_fds[i] == fd)
            return true;
    }
    return false;
}

int pfm_open(const char *path, int oflag) {
    int fd = sd_open(path, oflag);

    if (fd >= 0)
        track_add(fd);
    return fd;
}

void pfm_close(int *fd) {
    if (*fd >= 0) {
        track_remove(*fd);
        sd_close(*fd);
        *fd = -1;
    }
}

int pfm_open_at(int dir_fd, const char *name, int oflag) {
    int fd = sd_open_at(dir_fd, name, oflag);

    if (fd >= 0)
        track_add(fd);
    return fd;
}

bool pfm_close_checked(int *fd) {
    bool ok = false;

    if (*fd >= 0) {
        track_remove(*fd);
        ok = sd_close(*fd) == 0;
        *fd = -1;
    }
    return ok;
}

void pfm_iter_next(int dir_fd, int *it) {
    int previous = *it;

    *it = sd_iterate_dir(dir_fd, previous);
    if (previous >= 0 && *it != previous)
        track_remove(previous);
    if (*it >= 0)
        track_add(*it);
}

bool pfm_join(char *out, size_t size, const char *base, const char *rel) {
    int n = rel[0] ? snprintf(out, size, "%s/%s", base, rel) : snprintf(out, size, "%s", base);

    return n > 0 && (size_t)n < size;
}

uint32_t pfm_crc32(uint32_t crc, const void *buf, size_t len) {
    /* Every byte of a card goes through here while mounting, and SRAM reads faster
     * than a table in flash. */
    static uint32_t table[256];
    const uint8_t *p = buf;

    if (table[1] == 0) {
        for (uint32_t i = 0; i < 256; i++) {
            uint32_t c = i;

            for (int bit = 0; bit < 8; bit++)
                c = (c >> 1) ^ ((c & 1) ? 0xEDB88320U : 0);
            table[i] = c;
        }
    }

    crc = ~crc;
    while (len--)
        crc = table[(crc ^ *p++) & 0xFF] ^ (crc >> 8);
    return ~crc;
}

static bool write_all(int fd, const void *buf, size_t len) {
    return sd_write(fd, (void *)buf, len) == (int)len;
}

static bool read_all(int fd, void *buf, size_t len) {
    return sd_read(fd, buf, len) == (int)len;
}

static void put32(uint8_t *p, uint32_t v) {
    memcpy(p, &v, sizeof(v));
}

static uint32_t get32(const uint8_t *p) {
    uint32_t v;

    memcpy(&v, p, sizeof(v));
    return v;
}

int pfm_index_create(const char *path) {
    uint8_t header[INDEX_HEADER];
    int fd = pfm_open(path, O_RDWR | O_CREAT | O_TRUNC);

    if (fd < 0)
        return -1;

    memcpy(header, INDEX_MAGIC, MAGIC_LEN);
    put32(&header[8], INDEX_VERSION);
    put32(&header[12], sizeof(pfm_record_t));
    if (!write_all(fd, header, sizeof(header)))
        pfm_close(&fd);
    return fd;
}

bool pfm_index_read(int fd, uint32_t idx, pfm_record_t *rec) {
    if (sd_seek(fd, INDEX_HEADER + idx * sizeof(pfm_record_t), SEEK_SET) != 0)
        return false;
    return read_all(fd, rec, sizeof(*rec));
}

bool pfm_index_write(int fd, uint32_t idx, const pfm_record_t *rec) {
    if (sd_seek(fd, INDEX_HEADER + idx * sizeof(pfm_record_t), SEEK_SET) != 0)
        return false;
    return write_all(fd, rec, sizeof(*rec));
}

static bool index_crc(int fd, uint32_t count, uint32_t *crc_out) {
    uint8_t chunk[256];
    uint32_t remaining = INDEX_HEADER + count * sizeof(pfm_record_t);
    uint32_t crc = 0;

    if (sd_seek(fd, 0, SEEK_SET) != 0)
        return false;
    while (remaining) {
        size_t n = remaining > sizeof(chunk) ? sizeof(chunk) : remaining;
        if (!read_all(fd, chunk, n))
            return false;
        crc = pfm_crc32(crc, chunk, n);
        remaining -= n;
    }
    *crc_out = crc;
    return true;
}

bool pfm_index_seal(int fd, uint32_t count, uint32_t *crc_out) {
    uint8_t trailer[INDEX_TRAILER];
    uint32_t crc;

    if (!index_crc(fd, count, &crc))
        return false;

    put32(&trailer[0], count);
    put32(&trailer[4], crc);
    memcpy(&trailer[8], INDEX_END_MAGIC, MAGIC_LEN);

    if (sd_seek(fd, INDEX_HEADER + count * sizeof(pfm_record_t), SEEK_SET) != 0
        || !write_all(fd, trailer, sizeof(trailer)))
        return false;
    sd_flush(fd);
    if (crc_out)
        *crc_out = crc;
    return true;
}

bool pfm_index_validate(int fd, uint32_t *count, uint32_t *crc) {
    uint8_t header[INDEX_HEADER];
    uint8_t trailer[INDEX_TRAILER];
    uint32_t size = (uint32_t)sd_filesize(fd);
    uint32_t actual;

    if (size < INDEX_HEADER + sizeof(pfm_record_t) + INDEX_TRAILER)
        return false;

    if (sd_seek(fd, 0, SEEK_SET) != 0 || !read_all(fd, header, sizeof(header)))
        return false;
    if (memcmp(header, INDEX_MAGIC, MAGIC_LEN) != 0 || get32(&header[8]) != INDEX_VERSION
        || get32(&header[12]) != sizeof(pfm_record_t))
        return false;

    if (sd_seek(fd, size - INDEX_TRAILER, SEEK_SET) != 0 || !read_all(fd, trailer, sizeof(trailer)))
        return false;
    if (memcmp(&trailer[8], INDEX_END_MAGIC, MAGIC_LEN) != 0)
        return false;

    *count = get32(&trailer[0]);
    if (*count == 0 || INDEX_HEADER + *count * sizeof(pfm_record_t) + INDEX_TRAILER != size)
        return false;
    if (!index_crc(fd, *count, &actual) || actual != get32(&trailer[4]))
        return false;

    *crc = actual;
    return true;
}

/* Paths are assembled backwards from the end of the buffer. */
static bool prepend(char *out, size_t *start, size_t end, const char *component, size_t len) {
    size_t need = len + ((*start == end) ? 0 : 1);

    if (need > *start)
        return false;
    if (*start != end)
        out[--(*start)] = '/';
    *start -= len;
    memcpy(&out[*start], component, len);
    return true;
}

static bool walk_path(int fd, uint32_t idx, const char *child, char *out, size_t size) {
    size_t end = size - 1;
    size_t start = end;
    pfm_record_t rec;
    char host[PFN_HOST_NAME_MAX];

    out[end] = 0;

    if (child && !prepend(out, &start, end, child, strlen(child)))
        return false;

    for (int depth = 0; idx != 0; depth++) {
        if (depth > PFM_MAX_DEPTH || !pfm_index_read(fd, idx, &rec) || rec.parent >= idx)
            return false;
        pfn_encode(rec.name, (rec.flags & PFM_FLAG_STRICT) != 0, host);
        if (!prepend(out, &start, end, host, strlen(host)))
            return false;
        idx = rec.parent;
    }

    memmove(out, &out[start], size - start);
    return true;
}

bool pfm_record_path(int fd, uint32_t idx, char *out, size_t size) {
    return walk_path(fd, idx, NULL, out, size);
}

bool pfm_child_path(int fd, uint32_t parent, const char *child, char *out, size_t size) {
    return walk_path(fd, parent, child, out, size);
}

bool pfj_create(pfj_writer_t *j, const char *path) {
    j->fd = pfm_open(path, O_RDWR | O_CREAT | O_TRUNC);
    j->crc = 0;
    j->ops = 0;

    if (j->fd < 0)
        return false;
    if (!write_all(j->fd, JOURNAL_MAGIC, JOURNAL_HEADER)) {
        pfm_close(&j->fd);
        return false;
    }
    j->crc = pfm_crc32(0, JOURNAL_MAGIC, JOURNAL_HEADER);
    return true;
}

bool pfj_add(pfj_writer_t *j, pfj_op_t type, uint32_t id, const char *rel_path) {
    uint8_t header[OP_HEADER] = {0};
    size_t len = strlen(rel_path);

    if (len >= PFM_PATH_MAX)
        return false;

    header[0] = (uint8_t)type;
    header[2] = (uint8_t)len;
    header[3] = (uint8_t)(len >> 8);
    put32(&header[4], id);

    if (!write_all(j->fd, header, sizeof(header)) || !write_all(j->fd, rel_path, len))
        return false;

    j->crc = pfm_crc32(j->crc, header, sizeof(header));
    j->crc = pfm_crc32(j->crc, rel_path, len);
    j->ops++;
    return true;
}

bool pfj_commit(pfj_writer_t *j) {
    uint8_t trailer[JOURNAL_TRAILER];

    put32(&trailer[0], j->ops);
    put32(&trailer[4], j->crc);
    memcpy(&trailer[8], JOURNAL_END, MAGIC_LEN);

    bool ok = write_all(j->fd, trailer, sizeof(trailer));
    return pfm_close_checked(&j->fd) && ok;
}

void pfj_abandon(pfj_writer_t *j, const char *path) {
    pfm_close(&j->fd);
    sd_remove(path);
}

bool pfj_validate(const char *path) {
    uint8_t chunk[256];
    uint8_t trailer[JOURNAL_TRAILER];
    int fd = pfm_open(path, O_RDONLY);

    if (fd < 0)
        return false;

    uint32_t size = (uint32_t)sd_filesize(fd);
    bool ok = false;

    if (size >= JOURNAL_HEADER + JOURNAL_TRAILER) {
        uint32_t remaining = size - JOURNAL_TRAILER;
        uint32_t crc = 0;
        bool first = true;

        ok = true;
        while (ok && remaining) {
            size_t n = remaining > sizeof(chunk) ? sizeof(chunk) : remaining;
            ok = read_all(fd, chunk, n);
            if (ok) {
                if (first && memcmp(chunk, JOURNAL_MAGIC, JOURNAL_HEADER) != 0)
                    ok = false;
                first = false;
                crc = pfm_crc32(crc, chunk, n);
                remaining -= n;
            }
        }
        ok = ok && read_all(fd, trailer, sizeof(trailer)) && memcmp(&trailer[8], JOURNAL_END, MAGIC_LEN) == 0
             && get32(&trailer[4]) == crc;
    }

    pfm_close(&fd);
    return ok;
}

int pfj_open_ops(const char *path) {
    int fd = pfm_open(path, O_RDONLY);

    if (fd >= 0 && sd_seek(fd, JOURNAL_HEADER, SEEK_SET) != 0)
        pfm_close(&fd);
    return fd;
}

bool pfj_next(int fd, pfj_entry_t *entry) {
    uint8_t header[OP_HEADER];

    if (sd_tell(fd) + JOURNAL_TRAILER >= (uint32_t)sd_filesize(fd))
        return false;
    if (!read_all(fd, header, sizeof(header)))
        return false;

    size_t len = (size_t)header[2] | ((size_t)header[3] << 8);
    if (len >= PFM_PATH_MAX || !read_all(fd, entry->path, len))
        return false;

    entry->path[len] = 0;
    entry->type = (pfj_op_t)header[0];
    entry->id = get32(&header[4]);
    return true;
}
