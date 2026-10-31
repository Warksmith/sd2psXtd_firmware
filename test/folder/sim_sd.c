/* In-memory stand-in for the SD wrapper: case-insensitive like FAT, with the same
 * return conventions as ext/ESP8266SdFatWrapper/src/sd.cpp, plus power-cut and fault
 * injection for the recovery tests.
 *
 * Durability model: directory operations and O_TRUNC reach the card immediately, file
 * growth only on flush or close. A power cut drops unflushed growth but keeps in-place
 * overwrites, which is the pessimistic reading of how FAT behaves. */

#include "sim.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "sd.h"

#define NUM_FILES 16

typedef struct node {
    char name[256];
    bool is_dir;
    uint8_t *data;
    size_t size;
    size_t cap;
    size_t durable;
    struct node *parent;
    struct node *child;
    struct node *next;
} node_t;

typedef struct {
    node_t *node;
    size_t pos;
    node_t *cursor;
    bool started;
    bool writable;
    bool dirty;
} file_t;

static node_t root = {.is_dir = true};
static file_t files[NUM_FILES];

long sim_op_count;
long sim_crash_at;
long sim_fail_at;
bool sim_fail_sticky;
bool sim_open_at_fails;
long sim_free_bytes = -1;
void (*sim_op_hook)(long op);
jmp_buf sim_crash_point;
uint8_t sim_mtime[8] = {0, 30, 20, 10, 15, 6, 0xE8, 0x07};

/* Counts a state-changing operation. Returns false to inject a failure. */
static bool op(void) {
    sim_op_count++;
    if (sim_op_hook)
        sim_op_hook(sim_op_count);
    if (sim_crash_at && sim_op_count >= sim_crash_at) {
        sim_crash_at = 0;
        longjmp(sim_crash_point, 1);
    }
    if (sim_fail_at && sim_op_count >= sim_fail_at) {
        if (!sim_fail_sticky)
            sim_fail_at = 0;
        return false;
    }
    return true;
}

static node_t *find_child(node_t *dir, const char *name, size_t len) {
    for (node_t *c = dir->child; c; c = c->next) {
        if (strlen(c->name) == len && strncasecmp(c->name, name, len) == 0)
            return c;
    }
    return NULL;
}

static node_t *lookup(const char *path, node_t **parent, const char **leaf) {
    node_t *cur = &root;
    node_t *up = NULL;
    const char *p = path;
    const char *last = NULL;

    while (*p) {
        while (*p == '/')
            p++;
        if (!*p)
            break;
        const char *end = strchr(p, '/');
        size_t len = end ? (size_t)(end - p) : strlen(p);

        last = p;
        up = cur;
        cur = (cur && cur->is_dir) ? find_child(cur, p, len) : NULL;
        p += len;
        if (!cur && strchr(p, '/') && p[strspn(p, "/")]) {
            up = NULL;
            break;
        }
    }
    if (parent)
        *parent = up;
    if (leaf)
        *leaf = last;
    return cur;
}

bool sim_sd_name_ok(const char *name, size_t len) {
    if (len == 0 || len > 255 || name[0] == ' ' || name[len - 1] == ' ' || name[len - 1] == '.')
        return false;
    for (size_t i = 0; i < len; i++) {
        unsigned char c = (unsigned char)name[i];

        /* Bytes above 0x7E only stay what they are when they happen to be valid UTF-8. */
        if (c < 0x20 || c >= 0x7F || strchr("\"*/:<>?\\|", c))
            return false;
    }
    return true;
}

static node_t *add_node(node_t *dir, const char *name, size_t len, bool is_dir) {
    node_t *n = calloc(1, sizeof(*n));
    node_t **tail = &dir->child;

    memcpy(n->name, name, len);
    n->is_dir = is_dir;
    n->parent = dir;
    while (*tail)
        tail = &(*tail)->next;
    *tail = n;
    return n;
}

static void free_tree(node_t *n) {
    node_t *c = n->child;

    while (c) {
        node_t *next = c->next;
        free_tree(c);
        c = next;
    }
    free(n->data);
    if (n != &root)
        free(n);
}

static void unlink_node(node_t *n) {
    node_t **p = &n->parent->child;

    while (*p != n)
        p = &(*p)->next;
    *p = n->next;
}

static void reserve(node_t *n, size_t size) {
    if (size > n->cap) {
        n->cap = size * 2 + 64;
        n->data = realloc(n->data, n->cap);
    }
}

static void durable_tree(node_t *n) {
    if (!n->is_dir && n->size > n->durable)
        n->size = n->durable;
    for (node_t *c = n->child; c; c = c->next)
        durable_tree(c);
}

void sim_sd_reset(void) {
    memset(files, 0, sizeof(files));
    for (node_t *c = root.child; c;) {
        node_t *next = c->next;
        free_tree(c);
        c = next;
    }
    root.child = NULL;
    sim_sd_clear_faults();
}

void sim_sd_clear_faults(void) {
    sim_crash_at = 0;
    sim_fail_at = 0;
    sim_fail_sticky = false;
    sim_free_bytes = -1;
    sim_op_hook = NULL;
    sim_op_count = 0;
}

void sim_sd_power_cycle(void) {
    memset(files, 0, sizeof(files));
    durable_tree(&root);
}

int sim_sd_open_fds(void) {
    int n = 0;

    for (int i = 0; i < NUM_FILES; i++)
        n += files[i].node != NULL;
    return n;
}

static node_t *clone_node(const node_t *n, node_t *parent) {
    node_t *copy = malloc(sizeof(*copy));

    *copy = *n;
    copy->parent = parent;
    copy->child = copy->next = NULL;
    copy->cap = n->size;
    copy->data = NULL;
    if (n->size) {
        copy->data = malloc(n->size);
        memcpy(copy->data, n->data, n->size);
    }

    node_t **tail = &copy->child;
    for (const node_t *c = n->child; c; c = c->next) {
        *tail = clone_node(c, copy);
        tail = &(*tail)->next;
    }
    return copy;
}

void *sim_sd_snapshot(void) {
    return clone_node(&root, NULL);
}

void sim_sd_restore(void *snapshot) {
    const node_t *snap = snapshot;

    sim_sd_reset();
    node_t **tail = &root.child;
    for (const node_t *c = snap->child; c; c = c->next) {
        *tail = clone_node(c, &root);
        tail = &(*tail)->next;
    }
}

void sim_sd_snapshot_free(void *snapshot) {
    free_tree(snapshot);
}

static void walk(const node_t *n, char *path, size_t len, sim_walk_cb cb, void *ctx) {
    for (const node_t *c = n->child; c; c = c->next) {
        size_t extra = (size_t)snprintf(&path[len], 1024 - len, "%s%s", len ? "/" : "", c->name);

        cb(path, c->is_dir, c->data, c->size, ctx);
        if (c->is_dir)
            walk(c, path, len + extra, cb, ctx);
        path[len] = 0;
    }
}

void sim_sd_walk(const char *base, sim_walk_cb cb, void *ctx) {
    char path[1024] = "";
    node_t *n = lookup(base, NULL, NULL);

    if (n)
        walk(n, path, 0, cb, ctx);
}

void sim_sd_put(const char *path, const void *data, size_t len) {
    long count = sim_op_count;
    long crash = sim_crash_at, fail = sim_fail_at;
    int fd;

    sim_crash_at = sim_fail_at = 0;
    fd = sd_open(path, O_WRONLY | O_CREAT | O_TRUNC);
    if (fd < 0) {
        printf("sim_sd_put: cannot create %s\n", path);
        exit(2);
    }
    sd_write(fd, (void *)data, len);
    sd_close(fd);
    sim_op_count = count;
    sim_crash_at = crash;
    sim_fail_at = fail;
}

const uint8_t *sim_sd_get(const char *path, size_t *len) {
    node_t *n = lookup(path, NULL, NULL);

    if (!n || n->is_dir)
        return NULL;
    *len = n->size;
    return n->data;
}

void sim_sd_del(const char *path) {
    node_t *n = lookup(path, NULL, NULL);

    if (n) {
        unlink_node(n);
        free_tree(n);
    }
}

void sim_sd_mkdirs(const char *path) {
    long count = sim_op_count;

    sd_mkdir(path);
    sim_op_count = count;
}

int sd_exists(const char *path) {
    return lookup(path, NULL, NULL) != NULL;
}

int sd_open(const char *path, int oflag) {
    node_t *parent;
    const char *leaf;
    node_t *n = lookup(path, &parent, &leaf);
    int fd;

    if (!n && !(oflag & O_CREAT))
        return -1;

    for (fd = 0; fd < NUM_FILES; fd++) {
        if (!files[fd].node)
            break;
    }
    if (fd >= NUM_FILES)
        return -1;

    if (!n) {
        if (!parent || !parent->is_dir || !sim_sd_name_ok(leaf, strcspn(leaf, "/")) || !op())
            return -1;
        n = add_node(parent, leaf, strcspn(leaf, "/"), false);
    } else if (n != &root && n->is_dir && (oflag & O_ACCMODE) != O_RDONLY) {
        return -1;
    }

    if ((oflag & O_TRUNC) && !n->is_dir) {
        if (!op())
            return -1;
        n->size = n->durable = 0;
    }

    memset(&files[fd], 0, sizeof(files[fd]));
    files[fd].node = n;
    files[fd].writable = (oflag & O_ACCMODE) != O_RDONLY;
    return fd;
}

#define CHECK_FD(fd) if ((fd) < 0 || (fd) >= NUM_FILES || !files[fd].node) return -1

int sd_open_at(int dir, const char *name, int oflag) {
    int fd;
    node_t *n;

    (void)oflag;
    CHECK_FD(dir);
    n = files[dir].node->is_dir ? find_child(files[dir].node, name, strlen(name)) : NULL;
    if (!n || sim_open_at_fails)
        return -1;
    for (fd = 0; fd < NUM_FILES; fd++) {
        if (!files[fd].node)
            break;
    }
    if (fd >= NUM_FILES)
        return -1;
    memset(&files[fd], 0, sizeof(files[fd]));
    files[fd].node = n;
    /* Like SdFat, a lookup leaves the directory positioned at what it found. */
    files[dir].started = true;
    files[dir].cursor = n;
    return fd;
}

void sd_flush(int fd) {
    if (fd < 0 || fd >= NUM_FILES || !files[fd].node || !files[fd].dirty)
        return;
    if (op()) {
        files[fd].node->durable = files[fd].node->size;
        files[fd].dirty = false;
    }
}

int sd_close(int fd) {
    bool ok = true;

    CHECK_FD(fd);
    if (files[fd].dirty) {
        ok = op();
        if (ok)
            files[fd].node->durable = files[fd].node->size;
    }
    memset(&files[fd], 0, sizeof(files[fd]));
    return ok ? 0 : 1;
}

int sd_read(int fd, void *buf, size_t count) {
    CHECK_FD(fd);
    node_t *n = files[fd].node;
    size_t avail = files[fd].pos < n->size ? n->size - files[fd].pos : 0;

    if (count > avail)
        count = avail;
    if (count)
        memcpy(buf, &n->data[files[fd].pos], count);
    files[fd].pos += count;
    return (int)count;
}

int sd_write(int fd, void *buf, size_t count) {
    CHECK_FD(fd);
    node_t *n = files[fd].node;
    size_t end = files[fd].pos + count;
    size_t grow = end > n->size ? end - n->size : 0;

    if (!files[fd].writable || !op())
        return -1;
    if (sim_free_bytes >= 0) {
        if ((long)grow > sim_free_bytes)
            return 0;
        sim_free_bytes -= (long)grow;
    }
    reserve(n, end);
    if (files[fd].pos > n->size)
        memset(&n->data[n->size], 0, files[fd].pos - n->size);
    if (count)
        memcpy(&n->data[files[fd].pos], buf, count);
    files[fd].pos = end;
    if (end > n->size)
        n->size = end;
    files[fd].dirty = true;
    return (int)count;
}

int sd_seek(int fd, int32_t offset, int whence) {
    CHECK_FD(fd);
    size_t origin = whence == SEEK_SET ? 0 : whence == SEEK_CUR ? files[fd].pos : files[fd].node->size;

    if ((int64_t)origin + offset < 0)
        return 1;
    files[fd].pos = origin + (size_t)offset;
    return 0;
}

uint32_t sd_tell(int fd) {
    CHECK_FD(fd);
    return (uint32_t)files[fd].pos;
}

int sd_filesize(int fd) {
    CHECK_FD(fd);
    return (int)files[fd].node->size;
}

int sd_mkdir(const char *path) {
    node_t *cur = &root;
    const char *p = path;

    if (sd_exists(path))
        return 0;

    while (*p) {
        while (*p == '/')
            p++;
        if (!*p)
            break;
        size_t len = strcspn(p, "/");
        node_t *next = find_child(cur, p, len);

        if (!next) {
            if (!sim_sd_name_ok(p, len) || !op())
                return 1;
            next = add_node(cur, p, len, true);
        } else if (!next->is_dir) {
            return 1;
        }
        cur = next;
        p += len;
    }
    return 0;
}

int sd_remove(const char *path) {
    node_t *n = lookup(path, NULL, NULL);

    if (!n || n->is_dir || !op())
        return 1;
    unlink_node(n);
    free_tree(n);
    return 0;
}

int sd_rmdir(const char *path) {
    node_t *n = lookup(path, NULL, NULL);

    if (!n || !n->is_dir || n->child || !op())
        return 1;
    unlink_node(n);
    free_tree(n);
    return 0;
}

int sd_rename(const char *old_path, const char *new_path) {
    node_t *n = lookup(old_path, NULL, NULL);
    node_t *parent;
    const char *leaf;

    if (!n || lookup(new_path, &parent, &leaf) || !parent || !op())
        return -1;
    unlink_node(n);
    size_t len = strlen(leaf);
    memset(n->name, 0, sizeof(n->name));
    memcpy(n->name, leaf, len);
    n->parent = parent;
    n->next = NULL;
    node_t **tail = &parent->child;
    while (*tail)
        tail = &(*tail)->next;
    *tail = n;
    return 0;
}

int sd_get_stat(int fd, ps2_fileio_stat_t *const stat) {
    CHECK_FD(fd);
    stat->size = (unsigned int)files[fd].node->size;
    memcpy(stat->ctime, sim_mtime, 8);
    memcpy(stat->atime, sim_mtime, 8);
    memcpy(stat->mtime, sim_mtime, 8);
    return 0;
}

int sd_iterate_dir(int dir, int it) {
    CHECK_FD(dir);
    file_t *d = &files[dir];
    node_t *next = d->started ? (d->cursor ? d->cursor->next : NULL) : d->node->child;

    d->started = true;
    d->cursor = next;

    if (it == -1) {
        for (it = 0; it < NUM_FILES; it++) {
            if (!files[it].node)
                break;
        }
    } else {
        memset(&files[it], 0, sizeof(files[it]));
    }

    if (!next || it >= NUM_FILES)
        return -1;
    memset(&files[it], 0, sizeof(files[it]));
    files[it].node = next;
    return it;
}

size_t sd_get_name(int fd, char *name, size_t size) {
    size_t len = strlen(files[fd].node->name);

    if (len >= size)
        return 0;
    memcpy(name, files[fd].node->name, len + 1);
    return len;
}

bool sd_is_dir(int fd) {
    return files[fd].node->is_dir;
}
