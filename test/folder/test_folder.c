/* Host tests for the folder-card engine. The console is the real mcfat driver, the SD
 * card and PSRAM are simulated; see sim_sd.c for the durability model.
 *
 * Passing here shows the logic holds under that model. It says nothing about real SD
 * cards, timing on the console or actual power loss. */

#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "card_config.h"
#include "mcio.h"
#include "ps2_folder_card.h"
#include "ps2_folder_fs.h"
#include "ps2_folder_internal.h"
#include "ps2_folder_meta.h"
#include "ps2_folder_name.h"
#include "ps2_folder_port.h"
#include "sd.h"
#include "settings.h"
#include "sim.h"

#define BASE        "MemoryCards/PS2/Card1/Card1-1.folder"
#define META        BASE "/.ps2meta"
#define MAX_TREE    2400
#define MAX_FILE    (8 * 1024 * 1024)
#define PATH_LEN    220

static int failures;
static int checks;
static const char *current_test = "";

#define CHECK(cond) do { \
        checks++; \
        if (!(cond)) { \
            failures++; \
            printf("  FAIL %s:%d [%s]: %s\n", __FILE__, __LINE__, current_test, #cond); \
        } \
    } while (0)

/* --- comparing what the host and the console see --- */

typedef struct {
    char path[PATH_LEN];
    bool dir;
    uint32_t len;
    uint32_t hash;
} item_t;

typedef struct {
    item_t *items;
    int n;
} tree_t;

static uint8_t file_buf[MAX_FILE];

static tree_t tree_new(void) {
    tree_t t = {malloc(sizeof(item_t) * MAX_TREE), 0};
    return t;
}

static int item_cmp(const void *a, const void *b) {
    return strcmp(((const item_t *)a)->path, ((const item_t *)b)->path);
}

static void tree_add(tree_t *t, const char *path, bool dir, const void *data, uint32_t len) {
    item_t *it = &t->items[t->n++];

    if (t->n > MAX_TREE) {
        printf("tree overflow\n");
        exit(2);
    }
    snprintf(it->path, sizeof(it->path), "%s", path);
    it->dir = dir;
    it->len = dir ? 0 : len;
    it->hash = dir ? 0 : pfm_crc32(0, data, len);
}

/* Host names decoded back to PS2 names, so the two views compare directly. */
static void host_walk_cb(const char *path, bool is_dir, const uint8_t *data, size_t len, void *ctx) {
    char decoded[PATH_LEN] = "";
    char copy[1024];

    if (strncmp(path, PFM_DIR, strlen(PFM_DIR)) == 0)
        return;

    snprintf(copy, sizeof(copy), "%s", path);
    for (char *part = strtok(copy, "/"); part; part = strtok(NULL, "/")) {
        char raw[PFS_NAME_LEN];
        bool strict;
        size_t at = strlen(decoded);

        if (!pfn_decode(part, raw, &strict)) {
            printf("  undecodable host name %s\n", part);
            failures++;
            return;
        }
        snprintf(&decoded[at], sizeof(decoded) - at, "/%s", raw);
    }
    tree_add(ctx, decoded, is_dir, data, (uint32_t)len);
}

static tree_t tree_host(void) {
    tree_t t = tree_new();

    sim_sd_walk(BASE, host_walk_cb, &t);
    qsort(t.items, (size_t)t.n, sizeof(item_t), item_cmp);
    return t;
}

static void card_walk(tree_t *t, const char *dir) {
    struct io_dirent de;
    char name[PATH_LEN];
    char (*children)[PATH_LEN] = malloc(PATH_LEN * 1100);
    bool *is_dir = malloc(1100);
    int count = 0;
    int fd;

    snprintf(name, sizeof(name), "%s", dir[0] ? dir : "/");
    fd = mcio_mcDopen(name);
    CHECK(fd >= 0);
    if (fd >= 0) {
        while (mcio_mcDread(fd, &de) > 0) {
            if (strcmp(de.name, ".") == 0 || strcmp(de.name, "..") == 0)
                continue;
            snprintf(children[count], PATH_LEN, "%s/%s", dir, de.name);
            is_dir[count] = (de.stat.mode & sceMcFileAttrSubdir) != 0;
            count++;
        }
        mcio_mcDclose(fd);
    }

    for (int i = 0; i < count; i++) {
        if (is_dir[i]) {
            tree_add(t, children[i], true, NULL, 0);
            card_walk(t, children[i]);
        } else {
            int len = con_read(children[i], file_buf, MAX_FILE);

            CHECK(len >= 0);
            tree_add(t, children[i], false, file_buf, len < 0 ? 0 : (uint32_t)len);
        }
    }
    free(children);
    free(is_dir);
}

static tree_t tree_card(void) {
    tree_t t = tree_new();

    card_walk(&t, "");
    qsort(t.items, (size_t)t.n, sizeof(item_t), item_cmp);
    return t;
}

static bool tree_equal(const tree_t *a, const tree_t *b) {
    if (a->n != b->n)
        return false;
    for (int i = 0; i < a->n; i++) {
        if (strcmp(a->items[i].path, b->items[i].path) != 0 || a->items[i].dir != b->items[i].dir
            || a->items[i].len != b->items[i].len || a->items[i].hash != b->items[i].hash)
            return false;
    }
    return true;
}

static void tree_dump(const char *label, const tree_t *t) {
    printf("  %s (%d):\n", label, t->n);
    for (int i = 0; i < t->n && i < 30; i++)
        printf("    %s%s %u %08x\n", t->items[i].path, t->items[i].dir ? "/" : "", t->items[i].len, t->items[i].hash);
}

static void tree_free(tree_t *t) {
    free(t->items);
    t->items = NULL;
}

/* True when both views agree; reports a difference otherwise. */
static bool views_match(void) {
    tree_t host = tree_host();
    tree_t card = tree_card();
    bool same = tree_equal(&host, &card);

    if (!same) {
        tree_dump("host", &host);
        tree_dump("card", &card);
    }
    tree_free(&host);
    tree_free(&card);
    return same;
}

/* --- driving the card --- */

static void fresh(void) {
    sim_sd_reset();
    sim_console_reset();
    sim_sd_mkdirs(BASE);
}

/* Runs until the console could see the card. */
static void pump_publish(void) {
    for (int i = 0; i < 2000000 && ps2_folder_card_status() == PS2_FOLDER_LOADING; i++)
        ps2_folder_card_task();
}

static void pump_fill(void) {
    for (int i = 0; i < 2000000 && ps2_folder_card_status() == PS2_FOLDER_FILLING; i++)
        ps2_folder_card_task();
}

static void pump_mount(void) {
    pump_publish();
    pump_fill();
}

static ps2_folder_status_t mount_at(const char *path) {
    /* Stale PSRAM from the previous card must never leak into the next one. */
    sim_console_reset();
    ps2_folder_card_open(path, false);
    pump_mount();
    con_refresh();
    return ps2_folder_card_status();
}

static ps2_folder_status_t mount(void) {
    return mount_at(BASE);
}

static ps2_folder_status_t settle(void) {
    ps2_folder_status_t st = ps2_folder_card_status();

    sim_time_advance_ms(2000);
    for (int i = 0, idle = 0; i < 2000000 && idle < 60; i++) {
        ps2_folder_card_task();
        st = ps2_folder_card_status();
        if (st == PS2_FOLDER_CLEAN || st == PS2_FOLDER_ERROR)
            break;
        if (st == PS2_FOLDER_DIRTY || st == PS2_FOLDER_SYNC_FAILED) {
            sim_time_advance_ms(200);
            idle++;
        }
    }
    return st;
}

static void settle_void(void) {
    settle();
}

static bool unmount(void) {
    bool ok = ps2_folder_card_flush();

    ps2_folder_card_close();
    return ok;
}

/* Cut the power at op n while fn runs. Returns true if the cut happened. */
static bool cut_power_during(long n, void (*fn)(void)) {
    sim_op_count = 0;
    sim_crash_at = n;
    if (setjmp(sim_crash_point) == 0) {
        fn();
        sim_crash_at = 0;
        return false;
    }
    sim_sd_power_cycle();
    ps2_folder_card_power_loss();
    return true;
}

static void fill(uint8_t *buf, size_t len, uint32_t seed) {
    uint32_t x = seed * 2654435761U + 1;

    for (size_t i = 0; i < len; i++) {
        x ^= x << 13;
        x ^= x >> 17;
        x ^= x << 5;
        buf[i] = (uint8_t)x;
    }
}

static void put_host(const char *rel, size_t len, uint32_t seed) {
    char path[512];

    snprintf(path, sizeof(path), "%s/%s", BASE, rel);
    fill(file_buf, len, seed);
    sim_sd_put(path, file_buf, len);
}

static void put_host_dir(const char *rel) {
    char path[512];

    snprintf(path, sizeof(path), "%s/%s", BASE, rel);
    sim_sd_mkdirs(path);
}

static int con_put(const char *path, int len, uint32_t seed) {
    static uint8_t data[MAX_FILE];

    fill(data, (size_t)len, seed);
    return con_write(path, data, len);
}

static bool host_has(const char *rel) {
    char path[512];

    snprintf(path, sizeof(path), "%s/%s", BASE, rel);
    return sd_exists(path) != 0;
}

static bool host_content_is(const char *rel, size_t len, uint32_t seed) {
    static uint8_t want[MAX_FILE];
    char path[512];
    size_t actual = 0;
    const uint8_t *data;

    snprintf(path, sizeof(path), "%s/%s", BASE, rel);
    data = sim_sd_get(path, &actual);
    fill(want, len, seed);
    return data ? (actual == len && memcmp(data, want, len) == 0) : len == 0 && sd_exists(path);
}

static bool console_content_is(const char *path, int len, uint32_t seed) {
    static uint8_t want[MAX_FILE];
    int got = con_read(path, file_buf, MAX_FILE);

    fill(want, (size_t)len, seed);
    return got == len && memcmp(file_buf, want, (size_t)len) == 0;
}

static bool dir_empty(const char *path) {
    int fd = sd_open(path, O_RDONLY);
    int it = -1;
    bool empty;

    if (fd < 0)
        return true;
    it = sd_iterate_dir(fd, it);
    empty = it < 0;
    if (it >= 0)
        sd_close(it);
    sd_close(fd);
    return empty;
}

static bool no_transaction_left(void) {
    return !sd_exists(META "/journal.bin") && dir_empty(META "/stage");
}

/* Finds a directory entry by walking the on-card structures. */
static bool locate(const char *path, pfs_t *fs, uint32_t *out_cluster, uint32_t *out_slot, pfs_entry_t *out) {
    char copy[256];
    uint32_t dir = 0, length = 0;
    pfs_entry_t e;

    if (!pfs_mount(fs))
        return false;
    dir = fs->root_cluster;
    pfs_entry_read(fs, dir, 0, &e);
    length = e.length;

    snprintf(copy, sizeof(copy), "%s", path);
    for (char *part = strtok(copy, "/"); part; part = strtok(NULL, "/")) {
        uint32_t cluster = dir;
        bool found = false;

        for (uint32_t k = 0; k < length; k++) {
            if (k && k % PFS_ENTRIES_PER_CLUSTER == 0 && !pfs_fat_next(fs, cluster, &cluster))
                return false;
            pfs_entry_read(fs, cluster, k % PFS_ENTRIES_PER_CLUSTER, &e);
            if ((e.mode & PFS_MODE_EXISTS) && strcmp(e.name, part) == 0) {
                *out_cluster = cluster;
                *out_slot = k % PFS_ENTRIES_PER_CLUSTER;
                *out = e;
                dir = e.cluster;
                length = e.length;
                found = true;
                break;
            }
        }
        if (!found)
            return false;
    }
    return true;
}

static void edit_written(const pfs_t *fs, uint32_t cluster, uint32_t slot) {
    con_touch_dirty((fs->alloc_offset + cluster) * 2 + slot);
    con_refresh(); /* the driver caches directory entries */
}

/* --- A: import --- */

static void test_empty_folder(void) {
    int free_kb;

    fresh();
    CHECK(mount() == PS2_FOLDER_CLEAN);
    tree_t t = tree_card();
    CHECK(t.n == 0);
    tree_free(&t);
    free_kb = con_free_clusters();
    CHECK(free_kb > 7900);
    CHECK(sd_exists(META "/index.bin"));
    CHECK(no_transaction_left());
    CHECK(unmount());
}

static void test_missing_folder_is_created(void) {
    sim_sd_reset();
    sim_console_reset();
    CHECK(mount() == PS2_FOLDER_CLEAN);
    CHECK(sd_exists(BASE));
    CHECK(unmount());
}

static void test_import_tree(void) {
    fresh();
    put_host_dir("BASLUS-20001");
    put_host("BASLUS-20001/icon.sys", 964, 1);
    put_host("BASLUS-20001/BASLUS-20001", 65536, 2);
    put_host_dir("BESCES-50001");
    put_host("BESCES-50001/save.dat", 1024, 3);
    put_host("ROOTFILE", 300, 4);
    CHECK(mount() == PS2_FOLDER_CLEAN);
    CHECK(views_match());
    CHECK(console_content_is("/BASLUS-20001/BASLUS-20001", 65536, 2));
    CHECK(console_content_is("/ROOTFILE", 300, 4));
    CHECK(unmount());
}

static void test_import_is_hidden_until_complete(void) {
    int loading_steps = 0;

    fresh();
    put_host("A", 200000, 5);
    sim_time_step = 50000;
    ps2_folder_card_open(BASE, false);
    while (ps2_folder_card_status() == PS2_FOLDER_LOADING && loading_steps < 100000) {
        loading_steps++;
        CHECK(ps2_folder_card_progress() >= 0 && ps2_folder_card_progress() <= 100);
        ps2_folder_card_task();
    }
    sim_time_step = 10;
    CHECK(loading_steps > 1); /* it really is sliced */
    CHECK(ps2_folder_card_status() == PS2_FOLDER_FILLING);
    pump_fill();
    CHECK(ps2_folder_card_status() == PS2_FOLDER_CLEAN);
    con_refresh();
    CHECK(unmount());
}

static void test_import_boundary_sizes(void) {
    static const int sizes[] = {0, 1, 511, 512, 513, 1023, 1024, 1025, 2048, 8192, 16385};
    char name[32];
    uint32_t i;

    fresh();
    for (i = 0; i < sizeof(sizes) / sizeof(sizes[0]); i++) {
        snprintf(name, sizeof(name), "F%d", sizes[i]);
        put_host(name, (size_t)sizes[i], i + 10);
    }
    CHECK(mount() == PS2_FOLDER_CLEAN);
    CHECK(views_match());
    CHECK(unmount());
}

static void test_import_names(void) {
    static const char *const names[] = {
        "BESLES-12345 Save (1)", "lower.sys", "with space", "trailing.", "CON", "NUL.txt", "a%b",
        "quote\"name", "star*name", "col:on", "back\\slash", "\x82\xA0\x82\xA2", "1234567890123456789012345678901",
        " leading", "trailing ", ".dotted", "..double", ".ps2meta", ".DS_Store", "._resource", "Thumbs.db", "COM1.DAT",
    };
    char raw[PFS_NAME_LEN];
    char host[PFN_HOST_NAME_MAX];
    char path[64];
    uint32_t i;

    fresh();
    for (i = 0; i < sizeof(names) / sizeof(names[0]); i++) {
        memset(raw, 0, sizeof(raw));
        strncpy(raw, names[i], sizeof(raw) - 1);
        pfn_encode(raw, false, host);
        put_host(host, 100 + i, i + 20);
    }
    CHECK(mount() == PS2_FOLDER_CLEAN);
    CHECK(views_match());
    for (i = 0; i < sizeof(names) / sizeof(names[0]); i++) {
        snprintf(path, sizeof(path), "/%s", names[i]);
        CHECK(console_content_is(path, (int)(100 + i), i + 20));
    }
    CHECK(unmount());
}

static void test_unsupported_host_name_is_rejected(void) {
    fresh();
    put_host("100%.dat", 10, 1);
    put_host("GOOD", 10, 2);
    CHECK(mount() == PS2_FOLDER_ERROR);
    CHECK(strcmp(ps2_folder_card_error_text(), "Bad file name") == 0);
    CHECK(host_content_is("100%.dat", 10, 1));
    CHECK(host_content_is("GOOD", 10, 2));
    CHECK(unmount());
}

static void test_capacity_failure_changes_nothing(void) {
    void *before;

    fresh();
    for (int i = 0; i < 9; i++) {
        char name[16];
        snprintf(name, sizeof(name), "BIG%d", i);
        put_host(name, 1024 * 1024, (uint32_t)i);
    }
    before = sim_sd_snapshot();
    CHECK(mount() == PS2_FOLDER_ERROR);
    CHECK(strcmp(ps2_folder_card_error_text(), "Too big for 8MB") == 0);
    for (int i = 0; i < 9; i++) {
        char name[16];
        snprintf(name, sizeof(name), "BIG%d", i);
        CHECK(host_content_is(name, 1024 * 1024, (uint32_t)i));
    }
    CHECK(unmount());
    sim_sd_snapshot_free(before);
}

static void test_single_oversized_file(void) {
    fresh();
    put_host("HUGE", 8U * 1024U * 1024U, 3);
    CHECK(mount() == PS2_FOLDER_ERROR);
    CHECK(unmount());
}

static void test_card_filled_to_the_last_cluster(void) {
    fresh();
    /* The firmware format offers 8001 clusters; the root holds one, and its second one
     * holds the entry for this file. */
    put_host("FULL", (8001U - 2U) * 1024U, 9);
    CHECK(mount() == PS2_FOLDER_CLEAN);
    CHECK(console_content_is("/FULL", (8001 - 2) * 1024, 9));
    CHECK(con_put("/MORE", 2048, 1) < 0);
    CHECK(unmount());

    fresh();
    put_host("FULL", (8001U - 1U) * 1024U, 9);
    CHECK(mount() == PS2_FOLDER_ERROR);
    CHECK(unmount());
}

static void test_entry_limit(void) {
    char name[32];

    fresh();
    for (int d = 0; d < 3; d++) {
        snprintf(name, sizeof(name), "D%d", d);
        put_host_dir(name);
        for (int i = 0; i < 700; i++) {
            snprintf(name, sizeof(name), "D%d/F%d", d, i);
            put_host(name, 1, 1);
        }
    }
    CHECK(mount() == PS2_FOLDER_ERROR);
    CHECK(strcmp(ps2_folder_card_error_text(), "Too many files") == 0);
    CHECK(unmount());
}

static void test_many_entries_in_one_directory(void) {
    char name[32];

    fresh();
    for (int i = 0; i < 1000; i++) {
        snprintf(name, sizeof(name), "F%04d", i);
        put_host(name, 4, (uint32_t)i);
    }
    CHECK(mount() == PS2_FOLDER_CLEAN);
    CHECK(views_match());
    CHECK(con_remove("/F0500") == 0);
    CHECK(settle() == PS2_FOLDER_CLEAN);
    CHECK(!host_has("F0500"));
    CHECK(views_match());
    CHECK(unmount());
}

static void test_depth_limit(void) {
    char path[128] = "";

    fresh();
    for (int i = 0; i < 8; i++) {
        strcat(path, i ? "/D" : "D");
        put_host_dir(path);
    }
    put_host("D/D/D/D/D/D/D/D/LEAF", 5, 1);
    CHECK(mount() == PS2_FOLDER_CLEAN);
    CHECK(console_content_is("/D/D/D/D/D/D/D/D/LEAF", 5, 1));
    CHECK(unmount());

    strcat(path, "/D");
    put_host_dir(path);
    CHECK(mount() == PS2_FOLDER_ERROR);
    CHECK(unmount());
}

static void test_internal_files_are_not_exposed(void) {
    tree_t t;

    fresh();
    put_host("REAL", 8, 1);
    put_host(".DS_Store", 8, 2);
    put_host("Thumbs.db", 8, 3);
    CHECK(mount() == PS2_FOLDER_CLEAN);
    CHECK(unmount());
    CHECK(mount() == PS2_FOLDER_CLEAN);
    t = tree_card();
    CHECK(t.n == 1);
    CHECK(strcmp(t.items[0].path, "/REAL") == 0);
    tree_free(&t);
    CHECK(unmount());
    CHECK(host_content_is(".DS_Store", 8, 2));
}

static void test_coexists_with_image(void) {
    static const char image[] = "MemoryCards/PS2/Card1/Card1-1.mcd";
    size_t len = 0;
    const uint8_t *data;

    fresh();
    fill(file_buf, 4096, 77);
    sim_sd_put(image, file_buf, 4096);
    CHECK(mount() == PS2_FOLDER_CLEAN);
    CHECK(con_put("/NEW", 2000, 5) == 2000);
    CHECK(settle() == PS2_FOLDER_CLEAN);
    CHECK(unmount());
    data = sim_sd_get(image, &len);
    fill(file_buf, 4096, 77);
    CHECK(data && len == 4096 && memcmp(data, file_buf, 4096) == 0);
}

/* --- metadata --- */

static void test_metadata_survives_remount(void) {
    pfs_t fs;
    uint32_t c, s, c2, s2;
    pfs_entry_t e, after;

    fresh();
    CHECK(mount() == PS2_FOLDER_CLEAN);
    CHECK(con_mkdir("/SAVE") == 0);
    CHECK(con_put("/SAVE/DATA", 3000, 8) == 3000);
    CHECK(locate("/SAVE/DATA", &fs, &c, &s, &e));

    e.mode |= PFS_MODE_HIDDEN;
    e.mode &= (uint16_t)~PFS_MODE_WRITE;
    e.attr = 0x12345678;
    e.created[3] = 7;
    e.modified[3] = 9;
    pfs_entry_write(&fs, c, s, &e);
    edit_written(&fs, c, s);

    CHECK(settle() == PS2_FOLDER_CLEAN);
    CHECK(unmount());

    CHECK(mount() == PS2_FOLDER_CLEAN);
    CHECK(locate("/SAVE/DATA", &fs, &c2, &s2, &after));
    CHECK(after.mode == e.mode);
    CHECK(after.attr == e.attr);
    CHECK(memcmp(after.created, e.created, 8) == 0);
    CHECK(memcmp(after.modified, e.modified, 8) == 0);
    CHECK(unmount());
}

static void test_directory_order_survives_remount(void) {
    static const char *const order[] = {"ZED", "ALPHA", "MIKE", "BRAVO"};
    struct io_dirent de;
    int fd, i = 0;

    fresh();
    CHECK(mount() == PS2_FOLDER_CLEAN);
    for (int k = 0; k < 4; k++) {
        char path[16];
        snprintf(path, sizeof(path), "/%s", order[k]);
        CHECK(con_put(path, 10, (uint32_t)k) == 10);
    }
    CHECK(unmount());
    CHECK(mount() == PS2_FOLDER_CLEAN);
    fd = mcio_mcDopen((char *)"/");
    while (fd >= 0 && mcio_mcDread(fd, &de) > 0) {
        if (de.name[0] != '.' && i < 4)
            CHECK(strcmp(de.name, order[i++]) == 0);
    }
    mcio_mcDclose(fd);
    CHECK(i == 4);
    CHECK(unmount());
}

static void test_offline_host_changes(void) {
    pfs_t fs;
    uint32_t c, s;
    pfs_entry_t kept, edited;
    pfs_entry_t kept_before;

    fresh();
    put_host("KEPT", 1000, 1);
    put_host("EDITED", 1000, 2);
    put_host("DELETED", 1000, 3);
    CHECK(mount() == PS2_FOLDER_CLEAN);
    CHECK(locate("/KEPT", &fs, &c, &s, &kept_before));
    kept_before.modified[3] = 5;
    pfs_entry_write(&fs, c, s, &kept_before);
    edit_written(&fs, c, s);
    CHECK(settle() == PS2_FOLDER_CLEAN);
    CHECK(unmount());

    sim_mtime[3] = 22;
    put_host("EDITED", 1500, 12);
    sim_sd_del(BASE "/DELETED");
    put_host("ADDED", 700, 4);

    CHECK(mount() == PS2_FOLDER_CLEAN);
    CHECK(views_match());
    CHECK(console_content_is("/EDITED", 1500, 12));
    CHECK(console_content_is("/ADDED", 700, 4));
    CHECK(con_read("/DELETED", file_buf, 10) < 0);
    CHECK(locate("/KEPT", &fs, &c, &s, &kept));
    CHECK(kept.modified[3] == 5);
    CHECK(locate("/EDITED", &fs, &c, &s, &edited));
    CHECK(edited.modified[3] == 22);
    sim_mtime[3] = 10;
    CHECK(unmount());
}

/* --- B: write-back --- */

static void test_write_back_create(void) {
    fresh();
    CHECK(mount() == PS2_FOLDER_CLEAN);
    CHECK(con_mkdir("/BASLUS-1") == 0);
    CHECK(con_put("/BASLUS-1/icon.sys", 964, 1) == 964);
    CHECK(con_put("/BASLUS-1/DATA", 50000, 2) == 50000);
    CHECK(ps2_folder_card_status() == PS2_FOLDER_DIRTY);
    CHECK(settle() == PS2_FOLDER_CLEAN);
    CHECK(host_content_is("BASLUS-1/icon.sys", 964, 1));
    CHECK(host_content_is("BASLUS-1/DATA", 50000, 2));
    CHECK(no_transaction_left());
    CHECK(views_match());
    CHECK(unmount());
}

static void test_write_back_modify_truncate_extend(void) {
    fresh();
    put_host("SAME", 4000, 1);
    put_host("SHRINK", 4000, 2);
    put_host("GROW", 4000, 3);
    put_host("EMPTIED", 4000, 4);
    CHECK(mount() == PS2_FOLDER_CLEAN);
    CHECK(con_put("/SAME", 4000, 11) == 4000);
    CHECK(con_put("/SHRINK", 1000, 12) == 1000);
    CHECK(con_put("/GROW", 9000, 13) == 9000);
    CHECK(con_put("/EMPTIED", 0, 14) == 0);
    CHECK(settle() == PS2_FOLDER_CLEAN);
    CHECK(host_content_is("SAME", 4000, 11));
    CHECK(host_content_is("SHRINK", 1000, 12));
    CHECK(host_content_is("GROW", 9000, 13));
    CHECK(host_content_is("EMPTIED", 0, 14));
    CHECK(views_match());
    CHECK(unmount());
    CHECK(mount() == PS2_FOLDER_CLEAN);
    CHECK(views_match());
    CHECK(unmount());
}

static void test_empty_file_with_stray_cluster_value(void) {
    pfs_t fs;
    uint32_t c, s;
    pfs_entry_t e;

    fresh();
    put_host("EMPTY", 0, 1);
    put_host("OTHER", 2000, 2);
    CHECK(mount() == PS2_FOLDER_CLEAN);
    CHECK(locate("/EMPTY", &fs, &c, &s, &e));
    e.cluster = 0;
    e.mode |= PFS_MODE_HIDDEN;
    pfs_entry_write(&fs, c, s, &e);
    edit_written(&fs, c, s);
    CHECK(con_put("/NEW", 500, 3) == 500);
    CHECK(settle() == PS2_FOLDER_CLEAN);
    CHECK(host_content_is("NEW", 500, 3));
    CHECK(host_content_is("EMPTY", 0, 1));
    CHECK(unmount());
}

static void test_write_back_in_place(void) {
    char name[] = "/PATCH";
    uint8_t patch[100];
    int fd;

    fresh();
    put_host("PATCH", 5000, 1);
    CHECK(mount() == PS2_FOLDER_CLEAN);
    fd = mcio_mcOpen(name, sceMcFileAttrWriteable | sceMcFileAttrReadable);
    CHECK(fd >= 0);
    mcio_mcSeek(fd, 2000, 0);
    memset(patch, 0x5A, sizeof(patch));
    CHECK(mcio_mcWrite(fd, patch, sizeof(patch)) == sizeof(patch));
    mcio_mcClose(fd);
    CHECK(settle() == PS2_FOLDER_CLEAN);
    CHECK(views_match());
    CHECK(unmount());
}

static void test_write_back_delete(void) {
    fresh();
    put_host_dir("GONE");
    put_host("GONE/A", 100, 1);
    put_host("GONE/B", 100, 2);
    put_host("KEEP", 100, 3);
    CHECK(mount() == PS2_FOLDER_CLEAN);
    CHECK(con_remove("/GONE/A") == 0);
    CHECK(con_remove("/GONE/B") == 0);
    CHECK(con_rmdir("/GONE") == 0);
    CHECK(settle() == PS2_FOLDER_CLEAN);
    CHECK(!host_has("GONE"));
    CHECK(host_content_is("KEEP", 100, 3));
    CHECK(views_match());
    CHECK(unmount());
}

static void test_host_droppings_do_not_resurrect_directories(void) {
    fresh();
    put_host_dir("GONE");
    put_host("GONE/A", 100, 1);
    put_host("GONE/.DS_Store", 50, 2);
    CHECK(mount() == PS2_FOLDER_CLEAN);
    CHECK(con_remove("/GONE/A") == 0);
    CHECK(con_rmdir("/GONE") == 0);
    CHECK(settle() == PS2_FOLDER_CLEAN);
    CHECK(!host_has("GONE"));
    CHECK(unmount());
}

static void test_write_back_rename_directory(void) {
    pfs_t fs;
    uint32_t c, s;
    pfs_entry_t e;

    fresh();
    put_host_dir("OLDNAME");
    put_host("OLDNAME/DATA", 3000, 1);
    CHECK(mount() == PS2_FOLDER_CLEAN);
    CHECK(locate("/OLDNAME", &fs, &c, &s, &e));
    memset(e.name, 0, sizeof(e.name));
    strcpy(e.name, "NEWNAME");
    pfs_entry_write(&fs, c, s, &e);
    edit_written(&fs, c, s);
    CHECK(settle() == PS2_FOLDER_CLEAN);
    CHECK(!host_has("OLDNAME"));
    CHECK(host_content_is("NEWNAME/DATA", 3000, 1));
    CHECK(views_match());
    CHECK(unmount());
}

static void test_write_back_case_only_rename(void) {
    pfs_t fs;
    uint32_t c, s;
    pfs_entry_t e;
    tree_t t;

    fresh();
    put_host("readme", 3000, 1);
    CHECK(mount() == PS2_FOLDER_CLEAN);
    CHECK(locate("/readme", &fs, &c, &s, &e));
    strcpy(e.name, "README");
    pfs_entry_write(&fs, c, s, &e);
    edit_written(&fs, c, s);
    CHECK(settle() == PS2_FOLDER_CLEAN);
    CHECK(unmount());
    CHECK(mount() == PS2_FOLDER_CLEAN);
    t = tree_card();
    CHECK(t.n == 1 && strcmp(t.items[0].path, "/README") == 0);
    tree_free(&t);
    CHECK(unmount());
}

static void test_names_differing_only_in_case(void) {
    tree_t t;

    fresh();
    CHECK(mount() == PS2_FOLDER_CLEAN);
    CHECK(con_put("/Save", 500, 1) == 500);
    CHECK(con_put("/SAVE", 600, 2) == 600);
    CHECK(con_put("/save", 700, 3) == 700);
    CHECK(settle() == PS2_FOLDER_CLEAN);
    CHECK(unmount());
    CHECK(mount() == PS2_FOLDER_CLEAN);
    t = tree_card();
    CHECK(t.n == 3);
    tree_free(&t);
    CHECK(console_content_is("/Save", 500, 1));
    CHECK(console_content_is("/SAVE", 600, 2));
    CHECK(console_content_is("/save", 700, 3));

    CHECK(con_remove("/SAVE") == 0);
    CHECK(con_remove("/Save") == 0);
    CHECK(settle() == PS2_FOLDER_CLEAN);
    CHECK(unmount());
    CHECK(mount() == PS2_FOLDER_CLEAN);
    t = tree_card();
    CHECK(t.n == 1 && strcmp(t.items[0].path, "/save") == 0);
    tree_free(&t);
    CHECK(console_content_is("/save", 700, 3));
    CHECK(unmount());
}

static void test_file_replaced_by_directory(void) {
    fresh();
    put_host("THING", 1000, 1);
    CHECK(mount() == PS2_FOLDER_CLEAN);
    CHECK(con_remove("/THING") == 0);
    CHECK(con_mkdir("/THING") == 0);
    CHECK(con_put("/THING/IN", 100, 2) == 100);
    CHECK(settle() == PS2_FOLDER_CLEAN);
    CHECK(views_match());
    CHECK(unmount());
}

static void test_no_writes_means_no_sd_activity(void) {
    fresh();
    put_host("FILE", 1000, 1);
    CHECK(mount() == PS2_FOLDER_CLEAN);
    for (int i = 0; i < 100000 && pf_tree_stale(); i++) {
        sim_time_advance_ms(100);
        ps2_folder_card_task();
    }
    CHECK(!pf_tree_stale());
    sim_op_count = 0;
    sim_time_advance_ms(60000);
    for (int i = 0; i < 100; i++)
        ps2_folder_card_task();
    CHECK(sim_op_count == 0);
    CHECK(unmount());
}

static void test_rewrite_with_same_content(void) {
    size_t before_ops;

    fresh();
    put_host("FILE", 1000, 1);
    put_host("OTHER", 1000, 2);
    CHECK(mount() == PS2_FOLDER_CLEAN);
    CHECK(con_put("/OTHER", 1000, 2) == 1000);
    sim_op_count = 0;
    CHECK(settle() == PS2_FOLDER_CLEAN);
    before_ops = (size_t)sim_op_count;
    CHECK(before_ops > 0);
    CHECK(host_content_is("OTHER", 1000, 2));
    CHECK(unmount());
}

/* Commits nothing while the console keeps writing. */
static void hook_console_write(long op) {
    static int serial;
    char path[32];

    if (op == 3) {
        snprintf(path, sizeof(path), "/LATE%d", serial++ % 4);
        con_put(path, 900, 90);
    }
}

static void test_console_write_during_scan(void) {
    fresh();
    CHECK(mount() == PS2_FOLDER_CLEAN);
    CHECK(con_put("/FIRST", 1500, 1) == 1500);
    sim_op_count = 0;
    sim_op_hook = hook_console_write;
    settle();
    sim_op_hook = NULL;
    CHECK(settle() == PS2_FOLDER_CLEAN);
    CHECK(views_match());
    CHECK(no_transaction_left());
    CHECK(unmount());
}

/* A write landing at every possible moment of a sync must end up on SD. */
static long interrupt_at;

static void hook_interrupt(long op) {
    if (op == interrupt_at)
        con_put("/LATE", 1100, 91);
}

static void test_console_write_at_every_moment(void) {
    long total, ops = 0;

    fresh();
    put_host("OLD", 3000, 1);
    put_host("VICTIM", 3000, 2);
    CHECK(mount() == PS2_FOLDER_CLEAN);
    CHECK(unmount());
    void *base = sim_sd_snapshot();

    CHECK(mount() == PS2_FOLDER_CLEAN);
    CHECK(con_put("/NEW", 3000, 3) == 3000);
    CHECK(con_remove("/VICTIM") == 0);
    sim_op_count = 0;
    CHECK(settle() == PS2_FOLDER_CLEAN);
    total = sim_op_count;
    CHECK(unmount());

    for (interrupt_at = 1; interrupt_at <= total; interrupt_at++) {
        sim_sd_restore(base);
        CHECK(mount() == PS2_FOLDER_CLEAN);
        CHECK(con_put("/NEW", 3000, 3) == 3000);
        CHECK(con_remove("/VICTIM") == 0);
        sim_op_count = 0;
        sim_op_hook = hook_interrupt;
        settle();
        sim_op_hook = NULL;
        CHECK(settle() == PS2_FOLDER_CLEAN);
        sim_time_advance_ms(3000);
        CHECK(settle() == PS2_FOLDER_CLEAN);
        CHECK(host_content_is("NEW", 3000, 3));
        CHECK(host_content_is("LATE", 1100, 91));
        CHECK(!host_has("VICTIM"));
        CHECK(views_match());
        CHECK(unmount());
        ops++;
    }
    printf("  (%ld interleavings)\n", ops);
    sim_sd_snapshot_free(base);
}

/* The console rewrites two files as a pair in the middle of a sync. Whatever gets
 * committed must be a snapshot from a single moment, never half of each. */
static long torn_at;
static long torn_sealed_at;

static void hook_torn(long op) {
    if (op == torn_at) {
        con_put("/PAIR_A", 3000, 2);
        con_put("/PAIR_Z", 3000, 2);
    } else if (op > torn_at) {
        if (torn_sealed_at && op > torn_sealed_at)
            sim_crash_at = op;
        else if (!torn_sealed_at && sd_exists(META "/journal.bin") && pfj_validate(META "/journal.bin"))
            torn_sealed_at = op;
    }
}

static int pair_generation(const char *path) {
    for (int g = 0; g <= 2; g++) {
        if (console_content_is(path, 3000, (uint32_t)g))
            return g;
    }
    return -1;
}

static void test_commit_is_a_single_snapshot(void) {
    long total;
    void *base;
    int cuts = 0;

    fresh();
    put_host("PAIR_A", 3000, 0);
    put_host("PAIR_Z", 3000, 0);
    CHECK(mount() == PS2_FOLDER_CLEAN);
    CHECK(unmount());
    base = sim_sd_snapshot();

    CHECK(mount() == PS2_FOLDER_CLEAN);
    CHECK(con_put("/PAIR_A", 3000, 1) == 3000);
    CHECK(con_put("/PAIR_Z", 3000, 1) == 3000);
    sim_op_count = 0;
    CHECK(settle() == PS2_FOLDER_CLEAN);
    total = sim_op_count;
    CHECK(unmount());

    for (torn_at = 1; torn_at <= total; torn_at++) {
        sim_sd_restore(base);
        CHECK(mount() == PS2_FOLDER_CLEAN);
        CHECK(con_put("/PAIR_A", 3000, 1) == 3000);
        CHECK(con_put("/PAIR_Z", 3000, 1) == 3000);
        torn_sealed_at = 0;
        sim_op_hook = hook_torn;
        if (cut_power_during(0, settle_void))
            cuts++;
        sim_op_hook = NULL;
        sim_crash_at = 0;

        if (ps2_folder_card_active())
            CHECK(unmount());
        CHECK(mount() == PS2_FOLDER_CLEAN);
        int a = pair_generation("/PAIR_A");
        int z = pair_generation("/PAIR_Z");
        CHECK(a >= 0 && a == z);
        if (a != z)
            printf("  op %ld: PAIR_A is generation %d, PAIR_Z %d\n", torn_at, a, z);
        CHECK(unmount());
    }
    printf("  (%ld moments, %d power cuts after a commit)\n", total, cuts);
    CHECK(cuts > 0);
    sim_sd_snapshot_free(base);
}

static void test_file_open_on_console_is_deferred(void) {
    uint8_t data[1000];

    fresh();
    CHECK(mount() == PS2_FOLDER_CLEAN);
    fill(data, sizeof(data), 3);
    CHECK(con_write_open("/OPEN", data, sizeof(data)) == sizeof(data));
    sim_time_advance_ms(2000);
    for (int i = 0; i < 5000; i++)
        ps2_folder_card_task();
    CHECK(ps2_folder_card_status() != PS2_FOLDER_CLEAN);
    con_write_close();
    CHECK(settle() == PS2_FOLDER_CLEAN);
    CHECK(host_content_is("OPEN", 1000, 3));
    CHECK(unmount());
}

static void test_flush_on_unmount(void) {
    fresh();
    CHECK(mount() == PS2_FOLDER_CLEAN);
    CHECK(con_put("/QUICK", 2000, 4) == 2000);
    CHECK(unmount());
    CHECK(host_content_is("QUICK", 2000, 4));
    CHECK(no_transaction_left());
}

static void test_two_cards_are_isolated(void) {
    static const char other[] = "MemoryCards/PS2/Card2/Card2-1.folder";
    char path[128];

    fresh();
    put_host("CARD1", 100, 1);
    snprintf(path, sizeof(path), "%s/CARD2", other);
    fill(file_buf, 200, 2);
    sim_sd_mkdirs(other);
    sim_sd_put(path, file_buf, 200);

    CHECK(mount() == PS2_FOLDER_CLEAN);
    CHECK(con_put("/WRITTEN1", 300, 3) == 300);
    CHECK(unmount());

    CHECK(mount_at(other) == PS2_FOLDER_CLEAN);
    CHECK(con_read("/CARD1", file_buf, 10) < 0);
    CHECK(con_read("/WRITTEN1", file_buf, 10) < 0);
    CHECK(console_content_is("/CARD2", 200, 2));
    CHECK(unmount());

    CHECK(mount() == PS2_FOLDER_CLEAN);
    CHECK(con_read("/CARD2", file_buf, 10) < 0);
    CHECK(console_content_is("/WRITTEN1", 300, 3));
    CHECK(unmount());
}

static void test_path_protection(void) {
    fresh();
    CHECK(mount() == PS2_FOLDER_CLEAN);
    CHECK(ps2_folder_card_path_protected(BASE));
    CHECK(ps2_folder_card_path_protected("/" BASE "/X"));
    CHECK(ps2_folder_card_path_protected("memorycards/ps2/card1/CARD1-1.FOLDER/.ps2meta/index.bin"));
    CHECK(!ps2_folder_card_path_protected("MemoryCards/PS2/Card1/Card1-1.folder2/X"));
    CHECK(!ps2_folder_card_path_protected("MemoryCards/PS2/Card1/Card1-1.mcd"));
    CHECK(unmount());
    CHECK(!ps2_folder_card_path_protected(BASE));
}

/* --- invalid filesystems never reach SD --- */


static void invalid_fs_case(void (*damage)(void)) {
    uint8_t *clean = malloc(SIM_PSRAM_SIZE);
    tree_t before, after;

    fresh();
    put_host_dir("SAVE");
    put_host("SAVE/DATA", 5000, 1);
    put_host("SAVE/SYS", 3000, 2);
    put_host("OTHER", 3000, 3);
    CHECK(mount() == PS2_FOLDER_CLEAN);
    CHECK(con_put("/NEWER", 2000, 4) == 2000);
    CHECK(settle() == PS2_FOLDER_CLEAN);
    before = tree_host();
    memcpy(clean, sim_psram, SIM_PSRAM_SIZE);

    damage();
    con_touch_dirty(0);
    CHECK(settle() == PS2_FOLDER_SYNC_FAILED);
    CHECK(strcmp(ps2_folder_card_error_text(), "Card data invalid") == 0);
    after = tree_host();
    CHECK(tree_equal(&before, &after));
    CHECK(no_transaction_left());
    CHECK(!ps2_folder_card_flush());

    /* The console repaired it (or was rewound): the backlog drains. */
    memcpy(sim_psram, clean, SIM_PSRAM_SIZE);
    con_touch_dirty(0);
    sim_time_advance_ms(6000);
    CHECK(settle() == PS2_FOLDER_CLEAN);
    CHECK(ps2_folder_card_flush());
    ps2_folder_card_close();
    tree_free(&before);
    tree_free(&after);
    free(clean);
}

static void damage_superblock(void) {
    memset(sim_psram, 0xFF, 512);
}

static void damage_directory_chain(void) {
    pfs_t fs;
    uint32_t c, s, v;
    pfs_entry_t e;

    CHECK(locate("/SAVE", &fs, &c, &s, &e));
    CHECK(pfs_fat_get(&fs, e.cluster, &v));
    pfs_fat_set(&fs, e.cluster, 0xFFFFFFFFU);
    pfs_fat_flush(&fs);
}

static void damage_file_cycle(void) {
    pfs_t fs;
    uint32_t c, s;
    pfs_entry_t e;

    CHECK(locate("/SAVE/DATA", &fs, &c, &s, &e));
    pfs_fat_set(&fs, e.cluster, e.cluster | PFS_FAT_USED);
    pfs_fat_flush(&fs);
}

static void damage_cross_link(void) {
    pfs_t fs;
    uint32_t c, s;
    pfs_entry_t a, b;

    CHECK(locate("/SAVE/DATA", &fs, &c, &s, &a));
    CHECK(locate("/OTHER", &fs, &c, &s, &b));
    b.cluster = a.cluster;
    pfs_entry_write(&fs, c, s, &b);
}

static void damage_short_chain(void) {
    pfs_t fs;
    uint32_t c, s;
    pfs_entry_t e;

    CHECK(locate("/OTHER", &fs, &c, &s, &e));
    e.length = 6000;
    pfs_entry_write(&fs, c, s, &e);
}

static void damage_bad_entry(void) {
    pfs_t fs;
    uint32_t c, s;
    pfs_entry_t e;

    CHECK(locate("/OTHER", &fs, &c, &s, &e));
    e.mode = PFS_MODE_EXISTS | PFS_MODE_FILE | PFS_MODE_DIR;
    pfs_entry_write(&fs, c, s, &e);
}

static void damage_duplicate_name(void) {
    pfs_t fs;
    uint32_t c, s;
    pfs_entry_t e;

    CHECK(locate("/NEWER", &fs, &c, &s, &e));
    strcpy(e.name, "OTHER");
    pfs_entry_write(&fs, c, s, &e);
}

static void test_invalid_superblock(void) { invalid_fs_case(damage_superblock); }
static void test_invalid_directory_chain(void) { invalid_fs_case(damage_directory_chain); }
static void test_invalid_file_cycle(void) { invalid_fs_case(damage_file_cycle); }
static void test_invalid_cross_link(void) { invalid_fs_case(damage_cross_link); }
static void test_invalid_short_chain(void) { invalid_fs_case(damage_short_chain); }
static void test_invalid_bad_entry(void) { invalid_fs_case(damage_bad_entry); }
static void test_invalid_duplicate_name(void) { invalid_fs_case(damage_duplicate_name); }

/* --- format --- */

static void test_console_format_moves_saves_to_trash(void) {
    tree_t t;
    size_t len = 0;
    bool found = false;
    uint32_t crc = 0;

    fresh();
    put_host_dir("SAVE1");
    put_host("SAVE1/DATA", 3000, 1);
    put_host("ROOT", 2000, 2);
    CHECK(mount() == PS2_FOLDER_CLEAN);
    crc = pf.base_crc;
    CHECK(con_format() == 0);
    con_refresh();
    CHECK(settle() == PS2_FOLDER_CLEAN);

    t = tree_host();
    CHECK(t.n == 0);
    tree_free(&t);
    t = tree_card();
    CHECK(t.n == 0);
    tree_free(&t);

    char path[256];
    snprintf(path, sizeof(path), META "/trash/%08lX/ROOT", (unsigned long)crc);
    CHECK(sim_sd_get(path, &len) != NULL && len == 2000);
    snprintf(path, sizeof(path), META "/trash/%08lX/SAVE1/DATA", (unsigned long)crc);
    found = sim_sd_get(path, &len) != NULL && len == 3000;
    CHECK(found);

    CHECK(con_put("/AFTER", 500, 3) == 500);
    CHECK(settle() == PS2_FOLDER_CLEAN);
    CHECK(unmount());
    CHECK(mount() == PS2_FOLDER_CLEAN);
    CHECK(console_content_is("/AFTER", 500, 3));
    CHECK(views_match());
    CHECK(unmount());
}

static void test_partial_erase_is_not_a_format(void) {
    tree_t before, after;

    fresh();
    put_host("SAVE", 3000, 1);
    CHECK(mount() == PS2_FOLDER_CLEAN);
    before = tree_host();
    /* The first erased blocks of a format in progress. */
    memset(sim_psram, 0xFF, 16 * 512 * 8);
    con_touch_dirty(0);
    CHECK(settle() == PS2_FOLDER_SYNC_FAILED);
    after = tree_host();
    CHECK(tree_equal(&before, &after));
    CHECK(!ps2_folder_card_flush());
    ps2_folder_card_close();
    tree_free(&before);
    tree_free(&after);
}

/* --- C: power cuts and SD failures --- */

static void setup_basic(void) {
    put_host_dir("KEEP");
    put_host("KEEP/SAME", 2500, 1);
    put_host_dir("MODIFY");
    put_host("MODIFY/A", 3000, 2);
    put_host("MODIFY/B", 3000, 3);
    put_host_dir("REMOVE");
    put_host("REMOVE/X", 1500, 4);
    put_host("REMOVE/Y", 1500, 5);
    put_host("ROOTFILE", 900, 6);
}

static void mutate_mixed(void) {
    con_put("/MODIFY/A", 5000, 12);
    con_put("/MODIFY/B", 1000, 13);
    con_remove("/REMOVE/X");
    con_remove("/REMOVE/Y");
    con_rmdir("/REMOVE");
    con_mkdir("/CREATED");
    con_put("/CREATED/N", 4000, 14);
    con_put("/ROOTFILE", 900, 15);
}

static void mutate_create_only(void) {
    con_mkdir("/NEW1");
    con_put("/NEW1/A", 2000, 21);
    con_put("/NEW1/B", 2000, 22);
}

static void mutate_delete_only(void) {
    con_remove("/MODIFY/A");
    con_remove("/MODIFY/B");
    con_rmdir("/MODIFY");
}

static void mutate_format(void) {
    con_format();
    con_refresh();
}

static void rename_case(void) {
    pfs_t fs;
    uint32_t c, s;
    pfs_entry_t e;

    if (locate("/ROOTFILE", &fs, &c, &s, &e)) {
        strcpy(e.name, "rootfile");
        pfs_entry_write(&fs, c, s, &e);
        edit_written(&fs, c, s);
    }
}

static void mutate_rename_directory(void) {
    pfs_t fs;
    uint32_t c, s;
    pfs_entry_t e;

    if (locate("/MODIFY", &fs, &c, &s, &e)) {
        memset(e.name, 0, sizeof(e.name));
        strcpy(e.name, "MOVED");
        pfs_entry_write(&fs, c, s, &e);
        edit_written(&fs, c, s);
    }
}

static void build_base(void (*setup)(void), void **snapshot) {
    fresh();
    setup();
    CHECK(mount() == PS2_FOLDER_CLEAN);
    CHECK(unmount());
    *snapshot = sim_sd_snapshot();
}

static bool mounted_state_is(const tree_t *old_state, const tree_t *new_state) {
    tree_t card = tree_card();
    bool ok = tree_equal(&card, old_state) || tree_equal(&card, new_state);

    if (!ok) {
        tree_dump("expected old", old_state);
        tree_dump("expected new", new_state);
        tree_dump("got", &card);
    }
    tree_free(&card);
    return ok;
}

static void power_cut_matrix(const char *label, void (*setup)(void), void (*mutate)(void)) {
    void *base;
    tree_t old_state, new_state;
    long n;
    int saw_old = 0, saw_new = 0;

    build_base(setup, &base);
    CHECK(mount() == PS2_FOLDER_CLEAN);
    old_state = tree_card();
    mutate();
    CHECK(settle() == PS2_FOLDER_CLEAN);
    new_state = tree_card();
    CHECK(unmount());

    for (n = 1;; n++) {
        tree_t card;

        sim_sd_restore(base);
        CHECK(mount() == PS2_FOLDER_CLEAN);
        mutate();

        if (!cut_power_during(n, settle_void)) {
            CHECK(ps2_folder_card_status() == PS2_FOLDER_CLEAN);
            CHECK(unmount());
            break;
        }

        CHECK(mount() == PS2_FOLDER_CLEAN);
        CHECK(views_match());
        CHECK(mounted_state_is(&old_state, &new_state));
        CHECK(no_transaction_left());

        card = tree_card();
        if (tree_equal(&card, &new_state))
            saw_new++;
        else
            saw_old++;
        tree_free(&card);

        /* It still works afterwards. */
        CHECK(con_put("/PROBE", 700, 99) == 700);
        CHECK(settle() == PS2_FOLDER_CLEAN);
        CHECK(host_content_is("PROBE", 700, 99));
        CHECK(unmount());
        CHECK(sim_sd_open_fds() == 0);
    }
    printf("  (%s: %ld cut points, %d rolled back, %d completed)\n", label, n - 1, saw_old, saw_new);
    CHECK(saw_old > 0);
    CHECK(saw_new > 0);
    sim_sd_snapshot_free(base);
    tree_free(&old_state);
    tree_free(&new_state);
}

static void test_power_cut_mixed(void) { power_cut_matrix("mixed", setup_basic, mutate_mixed); }
static void test_power_cut_create(void) { power_cut_matrix("create", setup_basic, mutate_create_only); }
static void test_power_cut_delete(void) { power_cut_matrix("delete", setup_basic, mutate_delete_only); }
static void test_power_cut_format(void) { power_cut_matrix("format", setup_basic, mutate_format); }
static void test_power_cut_case_rename(void) { power_cut_matrix("case rename", setup_basic, rename_case); }
static void test_power_cut_dir_rename(void) { power_cut_matrix("dir rename", setup_basic, mutate_rename_directory); }

/* Cut during recovery from every cut in apply. */
static void mount_void(void) {
    mount();
}

static void test_power_cut_during_recovery(void) {
    void *base;
    tree_t new_state;
    long n, pairs = 0;

    build_base(setup_basic, &base);
    CHECK(mount() == PS2_FOLDER_CLEAN);
    mutate_create_only();
    CHECK(settle() == PS2_FOLDER_CLEAN);
    new_state = tree_card();
    CHECK(unmount());

    for (n = 1;; n++) {
        sim_sd_restore(base);
        CHECK(mount() == PS2_FOLDER_CLEAN);
        mutate_create_only();
        if (!cut_power_during(n, settle_void)) {
            CHECK(unmount());
            break;
        }
        if (!sd_exists(META "/journal.bin") || !pfj_validate(META "/journal.bin"))
            continue;

        void *crashed = sim_sd_snapshot();
        for (long m = 1;; m++) {
            sim_sd_restore(crashed);
            sim_console_reset();
            bool cut = cut_power_during(m, mount_void);
            sim_sd_clear_faults();
            if (!cut)
                ps2_folder_card_power_loss();
            sim_sd_power_cycle();
            CHECK(mount() == PS2_FOLDER_CLEAN);
            tree_t card = tree_card();
            CHECK(tree_equal(&card, &new_state));
            tree_free(&card);
            CHECK(views_match());
            CHECK(no_transaction_left());
            CHECK(unmount());
            pairs++;
            if (!cut)
                break;
        }
        sim_sd_snapshot_free(crashed);
    }
    printf("  (%ld double-fault cases)\n", pairs);
    CHECK(pairs > 0);
    sim_sd_snapshot_free(base);
    tree_free(&new_state);
}

static void sd_failure_matrix(bool sticky) {
    void *base;
    tree_t new_state;
    long n;
    int failed_runs = 0;

    build_base(setup_basic, &base);
    CHECK(mount() == PS2_FOLDER_CLEAN);
    mutate_mixed();
    CHECK(settle() == PS2_FOLDER_CLEAN);
    new_state = tree_card();
    CHECK(unmount());

    for (n = 1;; n++) {
        tree_t card;

        sim_sd_restore(base);
        CHECK(mount() == PS2_FOLDER_CLEAN);
        mutate_mixed();
        sim_op_count = 0;
        sim_fail_at = n;
        sim_fail_sticky = sticky;
        ps2_folder_status_t st = settle();
        bool triggered = sim_op_count >= n;

        /* The console keeps being served whatever the SD card does. */
        card = tree_card();
        CHECK(tree_equal(&card, &new_state));
        tree_free(&card);

        sim_fail_at = 0;
        sim_fail_sticky = false;
        if (st == PS2_FOLDER_SYNC_FAILED) {
            failed_runs++;
            CHECK(!sticky || !ps2_folder_card_flush() || no_transaction_left());
        }
        sim_time_advance_ms(6000);
        for (int i = 0; i < 5 && settle() != PS2_FOLDER_CLEAN; i++)
            sim_time_advance_ms(6000);
        CHECK(ps2_folder_card_status() == PS2_FOLDER_CLEAN);
        CHECK(views_match());
        CHECK(!sd_exists(META "/journal.bin"));
        CHECK(unmount());

        CHECK(mount() == PS2_FOLDER_CLEAN);
        card = tree_card();
        CHECK(no_transaction_left());
        CHECK(tree_equal(&card, &new_state));
        tree_free(&card);
        CHECK(unmount());

        if (!triggered)
            break;
    }
    printf("  (%ld failure points, %d surfaced as sync failures)\n", n - 1, failed_runs);
    CHECK(!sticky || failed_runs > 0);
    sim_sd_snapshot_free(base);
    tree_free(&new_state);
}

static void test_sd_error_once(void) { sd_failure_matrix(false); }
static void test_sd_error_persistent(void) { sd_failure_matrix(true); }

static void test_sd_full(void) {
    fresh();
    put_host("OLD", 2000, 1);
    CHECK(mount() == PS2_FOLDER_CLEAN);
    CHECK(con_put("/BIG", 100000, 2) == 100000);
    sim_free_bytes = 1000;
    CHECK(settle() == PS2_FOLDER_SYNC_FAILED);
    CHECK(strcmp(ps2_folder_card_error_text(), "SD error") == 0);
    CHECK(console_content_is("/BIG", 100000, 2));
    CHECK(host_content_is("OLD", 2000, 1));
    CHECK(!host_has("BIG"));
    CHECK(!ps2_folder_card_flush());

    sim_free_bytes = -1;
    sim_time_advance_ms(6000);
    CHECK(settle() == PS2_FOLDER_CLEAN);
    CHECK(host_content_is("BIG", 100000, 2));
    CHECK(unmount());
}

static void test_truncated_journal_is_discarded(void) {
    void *base;
    long n;

    build_base(setup_basic, &base);
    for (n = 1;; n++) {
        sim_sd_restore(base);
        CHECK(mount() == PS2_FOLDER_CLEAN);
        mutate_mixed();
        if (!cut_power_during(n, settle_void)) {
            CHECK(unmount());
            break;
        }
        if (sd_exists(META "/journal.bin"))
            break;
    }
    CHECK(sd_exists(META "/journal.bin"));
    CHECK(!pfj_validate(META "/journal.bin"));
    CHECK(mount() == PS2_FOLDER_CLEAN);
    CHECK(no_transaction_left());
    CHECK(console_content_is("/MODIFY/A", 3000, 2));
    CHECK(unmount());
    sim_sd_snapshot_free(base);
}

static void test_corrupt_journal_is_discarded(void) {
    size_t len = 0;
    uint8_t copy[8192];
    const uint8_t *data;

    fresh();
    setup_basic();
    CHECK(mount() == PS2_FOLDER_CLEAN);
    mutate_mixed();
    CHECK(settle() == PS2_FOLDER_CLEAN);
    CHECK(unmount());

    /* Sealed for a harmless path; one bit rot turns it into a well-formed journal that
     * removes a real save, and only the CRC can tell. */
    pfj_writer_t j;
    CHECK(pfj_create(&j, META "/journal.bin"));
    CHECK(pfj_add(&j, PFJ_REMOVE, 0, "KEEP/SAMG"));
    CHECK(pfj_commit(&j));
    data = sim_sd_get(META "/journal.bin", &len);
    memcpy(copy, data, len);
    CHECK(memcmp(&copy[16], "KEEP/SAMG", 9) == 0);
    copy[24] ^= 0x02;
    CHECK(copy[24] == 'E');
    sim_sd_put(META "/journal.bin", copy, len);

    CHECK(mount() == PS2_FOLDER_CLEAN);
    CHECK(host_content_is("KEEP/SAME", 2500, 1));
    CHECK(no_transaction_left());
    CHECK(unmount());
}

static void test_missing_stage_file_requires_recovery(void) {
    void *base;
    long n;
    void *before;

    build_base(setup_basic, &base);
    for (n = 1;; n++) {
        sim_sd_restore(base);
        CHECK(mount() == PS2_FOLDER_CLEAN);
        mutate_mixed();
        if (!cut_power_during(n, settle_void)) {
            CHECK(false);
            break;
        }
        if (sd_exists(META "/journal.bin") && pfj_validate(META "/journal.bin"))
            break;
    }

    sim_sd_del(META "/stage/00000000.bin");
    before = sim_sd_snapshot();
    CHECK(mount() == PS2_FOLDER_ERROR);
    CHECK(strcmp(ps2_folder_card_error_text(), "Recovery required") == 0);
    CHECK(sd_exists(META "/journal.bin"));
    CHECK(unmount());

    /* Documented way out: delete the journal, the old state comes back. */
    sim_sd_del(META "/journal.bin");
    CHECK(mount() == PS2_FOLDER_CLEAN);
    CHECK(views_match());
    CHECK(unmount());
    sim_sd_snapshot_free(before);
    sim_sd_snapshot_free(base);
}

static void test_damaged_index_keeps_all_files(void) {
    size_t len = 0;
    uint8_t copy[4096];
    const uint8_t *data;

    fresh();
    setup_basic();
    CHECK(mount() == PS2_FOLDER_CLEAN);
    CHECK(unmount());

    data = sim_sd_get(META "/index.bin", &len);
    memcpy(copy, data, len);
    copy[100] ^= 0xFF;
    sim_sd_put(META "/index.bin", copy, len);

    CHECK(mount() == PS2_FOLDER_CLEAN);
    CHECK(views_match());
    CHECK(console_content_is("/KEEP/SAME", 2500, 1));
    CHECK(con_put("/AFTER", 100, 7) == 100);
    CHECK(settle() == PS2_FOLDER_CLEAN);
    CHECK(host_content_is("KEEP/SAME", 2500, 1));
    CHECK(host_content_is("AFTER", 100, 7));
    CHECK(unmount());
}

static void test_missing_metadata_keeps_all_files(void) {
    fresh();
    setup_basic();
    CHECK(mount() == PS2_FOLDER_CLEAN);
    CHECK(unmount());
    sim_sd_del(META);
    CHECK(mount() == PS2_FOLDER_CLEAN);
    CHECK(views_match());
    CHECK(unmount());
}

static void test_leftover_stage_files_are_removed(void) {
    fresh();
    setup_basic();
    CHECK(mount() == PS2_FOLDER_CLEAN);
    CHECK(unmount());
    sim_sd_put(META "/stage/00000042.bin", "leftover", 8);
    CHECK(mount() == PS2_FOLDER_CLEAN);
    CHECK(no_transaction_left());
    CHECK(unmount());
}

static void test_trash_never_overwrites(void) {
    uint32_t crc;
    char path[256];
    size_t len = 0;

    fresh();
    put_host("SAVE", 3000, 1);
    CHECK(mount() == PS2_FOLDER_CLEAN);
    crc = pf.base_crc;
    snprintf(path, sizeof(path), META "/trash/%08lX", (unsigned long)crc);
    sim_sd_mkdirs(path);
    snprintf(path, sizeof(path), META "/trash/%08lX/SAVE", (unsigned long)crc);
    sim_sd_put(path, "existing", 8);

    CHECK(con_format() == 0);
    con_refresh();
    settle();
    CHECK(sim_sd_get(path, &len) != NULL && len == 8);
    CHECK(host_content_is("SAVE", 3000, 1)); /* left in place, logged, nothing removed */
    CHECK(unmount());
}

/* --- D: round trips --- */

#define MODEL_MAX 200

typedef struct {
    char path[64];
    bool dir;
    int len;
    uint32_t seed;
    int parent;
    bool live;
} model_t;

static model_t model[MODEL_MAX];

static tree_t tree_model(void) {
    static uint8_t data[MAX_FILE];
    tree_t t = tree_new();

    for (int i = 0; i < MODEL_MAX; i++) {
        if (model[i].live) {
            fill(data, (size_t)model[i].len, model[i].seed);
            tree_add(&t, model[i].path, model[i].dir, data, (uint32_t)model[i].len);
        }
    }
    qsort(t.items, (size_t)t.n, sizeof(item_t), item_cmp);
    return t;
}

static uint32_t rng_state;

static uint32_t rnd(uint32_t limit) {
    rng_state = rng_state * 1664525U + 1013904223U;
    return (rng_state >> 8) % limit;
}

static int model_pick(bool dir) {
    int start = (int)rnd(MODEL_MAX);

    for (int i = 0; i < MODEL_MAX; i++) {
        int k = (start + i) % MODEL_MAX;
        if (model[k].live && model[k].dir == dir)
            return k;
    }
    return -1;
}

static int model_free(void) {
    for (int i = 0; i < MODEL_MAX; i++) {
        if (!model[i].live)
            return i;
    }
    return -1;
}

static bool model_has_child(int dir) {
    for (int i = 0; i < MODEL_MAX; i++) {
        if (model[i].live && model[i].parent == dir)
            return true;
    }
    return false;
}

static void model_put(const model_t *m) {
    int r = con_put(m->path, m->len, m->seed);

    if (r != m->len)
        printf("  writing %s (%d bytes) returned %d, %d KB free\n", m->path, m->len, r, con_free_clusters());
    CHECK(r == m->len);
}

static void random_op(uint32_t *serial) {
    int slot = model_free();
    uint32_t kind = rnd(10);

    if (kind < 4 && slot >= 0) {
        int dir = rnd(3) ? model_pick(true) : -1;
        model_t *m = &model[slot];

        snprintf(m->path, sizeof(m->path), "%s/%sF%u", dir >= 0 ? model[dir].path : "", rnd(2) ? "low" : "UP", (*serial)++);
        m->len = (int)rnd(4) == 0 ? 0 : (int)rnd(20000);
        m->seed = rng_state;
        m->dir = false;
        m->parent = dir;
        m->live = true;
        model_put(m);
    } else if (kind < 6) {
        int f = model_pick(false);
        if (f >= 0) {
            model[f].len = (int)rnd(30000);
            model[f].seed = rng_state;
            model_put(&model[f]);
        }
    } else if (kind < 8) {
        int f = model_pick(false);
        if (f >= 0) {
            CHECK(con_remove(model[f].path) == 0);
            model[f].live = false;
        }
    } else if (kind < 9 && slot >= 0) {
        model_t *m = &model[slot];
        int parent = rnd(4) ? -1 : model_pick(true);
        int depth = 0;

        for (int p = parent; p >= 0; p = model[p].parent)
            depth++;
        if (depth < 5) {
            snprintf(m->path, sizeof(m->path), "%s/DIR%u", parent >= 0 ? model[parent].path : "", (*serial)++);
            m->dir = true;
            m->len = 0;
            m->parent = parent;
            m->live = true;
            CHECK(con_mkdir(m->path) == 0);
        }
    } else {
        int d = model_pick(true);
        if (d >= 0 && !model_has_child(d)) {
            CHECK(con_rmdir(model[d].path) == 0);
            model[d].live = false;
        }
    }
}

static void check_model(void) {
    tree_t want = tree_model();
    tree_t got = tree_card();

    CHECK(tree_equal(&want, &got));
    if (!tree_equal(&want, &got)) {
        tree_dump("model", &want);
        tree_dump("card", &got);
    }
    tree_free(&want);
    tree_free(&got);
}

static void test_random_sessions(void) {
    long rounds = getenv("PF_LONG") ? 400 : 60;

    for (uint32_t seed = 1; seed <= 6; seed++) {
        uint32_t serial = 0;

        rng_state = seed * 7919;
        memset(model, 0, sizeof(model));
        fresh();
        CHECK(mount() == PS2_FOLDER_CLEAN);

        for (long r = 0; r < rounds; r++) {
            uint32_t ops = 1 + rnd(8);

            for (uint32_t i = 0; i < ops; i++)
                random_op(&serial);

            switch (rnd(5)) {
                case 0:
                case 1:
                    CHECK(settle() == PS2_FOLDER_CLEAN);
                    CHECK(views_match());
                    break;
                case 2:
                    CHECK(unmount());
                    CHECK(mount() == PS2_FOLDER_CLEAN);
                    check_model();
                    CHECK(views_match());
                    break;
                case 3: {
                    /* Cut at a random moment: what is on SD is coherent, one of the
                     * committed states; resynchronize the model from the console. */
                    CHECK(settle() == PS2_FOLDER_CLEAN);
                    tree_t before = tree_card();
                    con_put("/CUTFILE", 800, serial);
                    bool cut = cut_power_during(1 + rnd(40), settle_void);
                    if (cut) {
                        CHECK(mount() == PS2_FOLDER_CLEAN);
                        CHECK(views_match());
                        CHECK(no_transaction_left());
                    }
                    con_remove("/CUTFILE");
                    CHECK(settle() == PS2_FOLDER_CLEAN);
                    tree_t after = tree_card();
                    CHECK(tree_equal(&before, &after));
                    tree_free(&before);
                    tree_free(&after);
                    break;
                }
                default:
                    break;
            }
        }
        CHECK(unmount());
        CHECK(mount() == PS2_FOLDER_CLEAN);
        check_model();
        CHECK(views_match());
        CHECK(unmount());
        CHECK(sim_sd_open_fds() == 0);
    }
}

static void test_import_export_is_lossless(void) {
    tree_t first, second;

    fresh();
    setup_basic();
    put_host("lower", 100, 8);
    put_host("Mixed Case.sys", 1024, 9);
    CHECK(mount() == PS2_FOLDER_CLEAN);
    first = tree_card();
    CHECK(unmount());
    for (int i = 0; i < 3; i++) {
        CHECK(mount() == PS2_FOLDER_CLEAN);
        second = tree_card();
        CHECK(tree_equal(&first, &second));
        tree_free(&second);
        CHECK(unmount());
    }
    tree_free(&first);
}

static void test_name_codec(void) {
    char raw[PFS_NAME_LEN], back[PFS_NAME_LEN], host[PFN_HOST_NAME_MAX];
    bool strict;

    /* every single-byte name, both encodings */
    for (int c = 1; c < 256; c++) {
        for (int mode = 0; mode < 2; mode++) {
            memset(raw, 0, sizeof(raw));
            raw[0] = (char)c;
            raw[1] = 'X';
            pfn_encode(raw, mode != 0, host);
            CHECK(sim_sd_name_ok(host, strlen(host)));
            CHECK(pfn_decode(host, back, &strict));
            CHECK(memcmp(raw, back, sizeof(raw)) == 0);

            /* first, last and only position have rules of their own */
            memset(raw, 0, sizeof(raw));
            raw[0] = 'X';
            raw[1] = (char)c;
            pfn_encode(raw, mode != 0, host);
            CHECK(sim_sd_name_ok(host, strlen(host)));
            CHECK(pfn_decode(host, back, &strict));
            CHECK(memcmp(raw, back, sizeof(raw)) == 0);

            memset(raw, 0, sizeof(raw));
            raw[0] = (char)c;
            if (pfn_valid_raw(raw)) {
                pfn_encode(raw, mode != 0, host);
                CHECK(sim_sd_name_ok(host, strlen(host)));
                CHECK(pfn_decode(host, back, &strict));
                CHECK(memcmp(raw, back, sizeof(raw)) == 0);
            }
        }
    }

    memset(raw, 0, sizeof(raw));
    strcpy(raw, "abc");
    pfn_encode(raw, true, host);
    CHECK(pfn_decode(host, back, &strict) && strict && strcmp(back, "abc") == 0);
    pfn_encode(raw, false, host);
    CHECK(pfn_decode(host, back, &strict) && !strict && strcmp(back, "abc") == 0);

    CHECK(!pfn_decode("100%.dat", back, &strict));
    CHECK(!pfn_decode("bad%zz", back, &strict));
    CHECK(!pfn_decode("nul%00", back, &strict));
    CHECK(!pfn_decode(".", back, &strict));
    CHECK(!pfn_decode("..", back, &strict));
    CHECK(!pfn_decode("12345678901234567890123456789012", back, &strict));
    CHECK(!pfn_decode("%41", back, &strict)); /* not the canonical spelling of A */
}

typedef struct {
    const char *name;
    void (*fn)(void);
} test_t;

/* --- card configuration --- */

static void check_mapping(const char *ini, const char *id, const char *expected) {
    char folder[MAX_FOLDER_NAME_LENGTH] = "stale previous card";
    sim_sd_mkdirs(".sd2psx");
    sim_sd_put(".sd2psx/Game2Folder.ini", ini, strlen(ini));
    card_config_get_card_folder(id, folder, sizeof(folder));
    CHECK(strcmp(folder, expected) == 0);
}

static void test_config_game2folder(void) {
    fresh();
    sim_mode = MODE_PS2;
    sim_ps2_variant = PS2_VARIANT_RETAIL;
    char folder[32] = "stale";
    card_config_get_card_folder("SLUS-12345", folder, sizeof(folder));
    CHECK(folder[0] == '\0'); /* Missing file clears previous selection. */
    check_mapping("[PS2]\nSLUS-12345=Exact\nSLUS-123**=Narrow\nSLUS-*=Wide\n*=All",
                  "SLUS-12345", "Exact");
    check_mapping("[PS2]\nSLUS-*=Wide\nSLUS-123**=Narrow", "SLUS-12399", "Narrow");
    check_mapping("[PS2]\nSLUS-123**=Narrow\nSLUS-*=Wide", "SLUS-12399", "Narrow");
    check_mapping("[PS2]\nSLUS-*=Wide\nSLUS-12345=Exact", "SLUS-12345", "Exact");
    check_mapping("[PS2]\nSLUS-123*=First\nSLUS-123**=Last", "SLUS-12345", "Last");
    check_mapping("[PS2]\nSLUS-12345=First\nSLUS-12345=Last", "SLUS-12345", "Last");
    check_mapping("[PS2]\nSLUS-*45=Invalid\nSLUS-123?" "?=Invalid", "SLUS-12345", "");
    check_mapping("[PS2]\nSLUS-123*=No", "SLUS-12", "");
    check_mapping("[PS2]\nSLUS-*=No", "slus-12345", "");
    check_mapping("[PS2]\nDefaultCard=Shared", "SLES-12345", "");
    check_mapping("[PS2]\nUseDefaultCard=OFF\nDefaultCard=Shared", "SLES-12345", "");
    check_mapping("[PS2]\nUseDefaultCard=ON", "SLES-12345", "");
    check_mapping("[PS2]\nUseDefaultCard=ON\nDefaultCard=", "SLES-12345", "");
    check_mapping("\xEF\xBB\xBF[PS2]\r\nDefaultCard=Shared\r\nUseDefaultCard=on", "SLES-12345", "Shared");
    check_mapping("[PS2]\nUseDefaultCard=ON\nDefaultCard=Shared\nSLUS-*=Wide", "SLUS-12345", "Wide");
    check_mapping("[PS2]\nSLUS-12345=\nUseDefaultCard=ON\nDefaultCard=Shared", "SLUS-12345", "");
    check_mapping("[PS2]\nUseDefaultCard=ON\nUseDefaultCard=TYPO\nDefaultCard=Shared", "SLES-12345", "");
    check_mapping("[PS1]\nUseDefaultCard=ON\nDefaultCard=Other\nSLUS-*=Other", "SLUS-12345", "");
    check_mapping("[PS2]\nUseDefaultCard=ON\nDefaultCard=Shared\n*=All", "", "");
    sim_mode = MODE_PS1;
    check_mapping("[PS2]\nSLUS-*=Other\n[PS1]\nSLUS-*=PS1Card", "SLUS-12345", "PS1Card");
    sim_mode = MODE_PS2;
    const int variants[] = {PS2_VARIANT_PROTO, PS2_VARIANT_COH, PS2_VARIANT_SC2};
    const char *expected[] = {"Proto", "Coh", "Conquest"};
    for (size_t i = 0; i < 3; i++) {
        sim_ps2_variant = variants[i];
        check_mapping("[PROT]\nUseDefaultCard=ON\nDefaultCard=Proto\n[COH]\nUseDefaultCard=ON\nDefaultCard=Coh\n[CONQUEST]\nUseDefaultCard=ON\nDefaultCard=Conquest", "SLUS-12345", expected[i]);
    }
    sim_ps2_variant = PS2_VARIANT_RETAIL;
    check_mapping("[PS2]\n*=All", "SLUS-12345", "All");
    char tiny[3] = "xx";
    card_config_get_card_folder("SLUS-12345", tiny, sizeof(tiny));
    CHECK(strcmp(tiny, "Al") == 0);
    card_config_get_card_folder("SLUS-12345", tiny, 0);
    CHECK(strcmp(tiny, "Al") == 0);
    card_config_get_card_folder("SLUS-12345", NULL, 0);
    card_config_get_card_folder(NULL, tiny, sizeof(tiny));
    CHECK(tiny[0] == '\0');
}

static void put_config(const char *text) {
    sim_sd_put("MemoryCards/PS2/Card1/Card1.ini", text, strlen(text));
}

#define CARD1_IMAGE "MemoryCards/PS2/Card1/Card1-2.mcd"
#define CARD1_INI   "MemoryCards/PS2/Card1/Card1.ini"
#define BOOT_IMAGE  "MemoryCards/PS2/BOOT/BootCard-1.mcd"

static bool backend_is_folder(const char *folder, const char *base, const char *image, bool boot) {
    char path[256];
    bool result;

    snprintf(path, sizeof(path), "%s", image);
    result = card_config_get_ps2_folder_backend(folder, base, path, boot);
    CHECK(strcmp(path, image) == 0);
    return result;
}

static bool card1_is_folder(void) {
    return backend_is_folder("Card1", "Card1", CARD1_IMAGE, false);
}

static bool boot_is_folder(void) {
    return backend_is_folder("BOOT", "BootCard", BOOT_IMAGE, true);
}

static bool config_selects_folder(const char *text) {
    put_config(text);
    return card1_is_folder();
}

static void test_config_storage_backend(void) {
    fresh();

    CHECK(!card1_is_folder());

    /* What Notepad saves: CRLF, and no newline after the last line. */
    CHECK(config_selects_folder("[Settings]\r\nStorageBackend=FOLDER"));
    CHECK(config_selects_folder("[Settings]\r\nStorageBackend=FOLDER\r\n"));
    CHECK(config_selects_folder("[Settings]\nStorageBackend=FOLDER"));
    CHECK(config_selects_folder("[Settings]\nStorageBackend=FOLDER\n"));
    CHECK(config_selects_folder("[Settings]\nstoragebackend=x\nStorageBackend=folder"));
    CHECK(config_selects_folder("\xEF\xBB\xBF[Settings]\r\nStorageBackend=FOLDER"));

    CHECK(!config_selects_folder(""));
    CHECK(!config_selects_folder("[Settings]"));
    CHECK(!config_selects_folder("[Settings]\nStorageBackend=IMAGE"));
    CHECK(!config_selects_folder("[Settings]\nStorageBackend=TAPE"));
    CHECK(!config_selects_folder("StorageBackend=FOLDER"));
    CHECK(!config_selects_folder("[Other]\nStorageBackend=FOLDER"));
    CHECK(sim_sd_open_fds() == 0);
}

static void test_config_last_line_of_other_settings(void) {
    fresh();

    put_config("[Settings]\nStorageBackend=FOLDER\nCardSize=8");
    CHECK(card_config_get_ps2_cardsize("Card1", "Card1") == 8);
    put_config("[Settings]\nStorageBackend=FOLDER\nMaxChannels=4");
    CHECK(card_config_get_max_channels("Card1", "Card1") == 4);
    CHECK(card1_is_folder());
}

/* cardman passes folder "BOOT" and base "BootCard", so the name is fixed. */
static void test_config_boot_card_name(void) {
    static const char text[] = "[Settings]\r\nStorageBackend=FOLDER\r\n";

    fresh();
    sim_sd_mkdirs("MemoryCards/PS2/BOOT");

    sim_sd_put("MemoryCards/PS2/BOOT/Boot.ini", text, strlen(text));
    sim_sd_put("MemoryCards/PS2/BOOT/BootCard-1.ini", text, strlen(text));
    CHECK(!boot_is_folder());

    sim_sd_put("MemoryCards/PS2/BOOT/BootCard.ini", text, strlen(text));
    CHECK(boot_is_folder());
}

static void test_new_card_backend(void) {
    fresh();
    sim_sd_mkdirs("MemoryCards/PS2/Card1");
    sim_sd_mkdirs("MemoryCards/PS2/BOOT");

    CHECK(!card1_is_folder());
    sim_new_card_folder = true;
    CHECK(card1_is_folder());

    /* The boot card has to answer at power-on. */
    CHECK(!boot_is_folder());

    /* Folder cards are 8 MB, and the card's own size comes first. */
    put_config("[Settings]\nCardSize=16");
    CHECK(!card1_is_folder());
    sim_ps2_cardsize = 16;
    put_config("[Settings]\nCardSize=8");
    CHECK(card1_is_folder());
    sim_sd_del(CARD1_INI);
    CHECK(!card1_is_folder());
    sim_ps2_cardsize = 8;

    sim_ps2_variant = PS2_VARIANT_COH;
    CHECK(!card1_is_folder());
    sim_ps2_variant = PS2_VARIANT_RETAIL;

    /* The card's ini beats the global setting both ways. */
    put_config("[Settings]\nStorageBackend=IMAGE");
    CHECK(!card1_is_folder());
    sim_sd_del(CARD1_INI);

    /* An existing image keeps its saves in view. */
    sim_sd_put(CARD1_IMAGE, "x", 1);
    CHECK(!card1_is_folder());
    put_config("[Settings]\nStorageBackend=FOLDER");
    CHECK(card1_is_folder());
    sim_sd_del(CARD1_INI);
    sim_sd_del(CARD1_IMAGE);

    /* A folder made by this setting outlives it. */
    sim_sd_mkdirs("MemoryCards/PS2/Card1/Card1-2.folder");
    sim_sd_mkdirs("MemoryCards/PS2/BOOT/BootCard-1.folder");
    sim_new_card_folder = false;
    CHECK(card1_is_folder());
    CHECK(boot_is_folder());

    /* Side by side, the image it always was. */
    sim_sd_put(CARD1_IMAGE, "x", 1);
    CHECK(!card1_is_folder());
    sim_sd_del(CARD1_IMAGE);

    put_config("[Settings]\nStorageBackend=IMAGE");
    CHECK(!card1_is_folder());
    CHECK(sim_sd_open_fds() == 0);
}

/* --- load speed --- */

static void test_free_space_is_erased_over_stale_psram(void) {
    static uint8_t clean[SIM_PSRAM_SIZE];

    fresh();
    /* The third entry starts a second directory cluster and leaves its second page unused.
     * The big file puts that cluster past the part of the card the format writes. */
    put_host("BIG", 40000, 2);
    put_host_dir("DIR");
    put_host("DIR/SAVE", 3000, 1);
    CHECK(mount() == PS2_FOLDER_CLEAN);
    memcpy(clean, sim_psram, sizeof(clean));
    CHECK(unmount());

    sim_console_reset();
    memset(sim_psram, 0xA5, sizeof(sim_psram));
    ps2_folder_card_open(BASE, false);
    pump_mount();
    CHECK(ps2_folder_card_status() == PS2_FOLDER_CLEAN);
    CHECK(memcmp(sim_psram, clean, sizeof(clean)) == 0);
    CHECK(unmount());
}

static void test_index_crc_needs_no_second_read(void) {
    uint32_t count = 0, crc = 0, cluster, slot;
    pfs_entry_t entry;
    pfs_t fs;
    int fd;

    fresh();
    put_host_dir("DIR");
    put_host("DIR/SAVE", 3000, 1);
    put_host("ROOT", 2048, 2);
    sim_sd_put(BASE "/EMPTY", "", 0);
    CHECK(mount() == PS2_FOLDER_CLEAN);

    fd = sd_open(META "/index.bin", O_RDONLY);
    CHECK(fd >= 0);
    CHECK(pfm_index_validate(fd, &count, &crc));
    CHECK(count == 5);
    for (uint32_t i = 1; i < count; i++) {
        pfm_record_t rec;
        char rel[256], path[512];
        size_t len = 0;
        const uint8_t *data;

        CHECK(pfm_index_read(fd, i, &rec));
        if (rec.mode & PFS_MODE_DIR)
            continue;
        CHECK(pfm_record_path(fd, i, rel, sizeof(rel)));
        snprintf(path, sizeof(path), "%s/%s", BASE, rel);
        data = sim_sd_get(path, &len);
        CHECK(rec.crc == (len ? pfm_crc32(0, data, len) : 0));
    }
    sd_close(fd);

    /* The field that carried the CRC must not reach the console. */
    CHECK(locate("DIR/SAVE", &fs, &cluster, &slot, &entry));
    CHECK(entry.stash == 0);
    CHECK(locate("ROOT", &fs, &cluster, &slot, &entry));
    CHECK(entry.stash == 0);
    CHECK(unmount());
}

static void test_failed_open_is_not_a_removed_file(void) {
    fresh();
    put_host("KEEP", 1500, 1);
    put_host("GONE", 1500, 2);
    CHECK(mount() == PS2_FOLDER_CLEAN);
    CHECK(unmount());

    sim_sd_del(BASE "/GONE");
    sim_open_at_fails = true;
    CHECK(mount() == PS2_FOLDER_CLEAN);
    sim_open_at_fails = false;
    CHECK(console_content_is("/KEEP", 1500, 1));
    CHECK(con_read("/GONE", file_buf, MAX_FILE) < 0);
    CHECK(unmount());
    CHECK(host_content_is("KEEP", 1500, 1));
}

/* A lookup moves the directory's position, which must not hide what comes before it. */
static void test_new_file_before_a_known_one(void) {
    fresh();
    put_host("KNOWN", 1500, 1);
    CHECK(mount() == PS2_FOLDER_CLEAN);
    CHECK(unmount());

    sim_sd_del(BASE "/KNOWN");
    put_host("NEW", 1500, 2);
    put_host("KNOWN", 1500, 1);
    CHECK(mount() == PS2_FOLDER_CLEAN);
    CHECK(console_content_is("/NEW", 1500, 2));
    CHECK(console_content_is("/KNOWN", 1500, 1));
    CHECK(views_match());
    CHECK(unmount());
}

/* --- loading behind the console --- */

static ps2_folder_status_t publish(void) {
    sim_console_reset();
    ps2_folder_card_open(BASE, false);
    pump_publish();
    con_refresh();
    return ps2_folder_card_status();
}

static void pump_loaded(void) {
    for (int i = 0; i < 2000000 && ps2_folder_card_filling(); i++)
        ps2_folder_card_task();
}

static void test_card_is_served_before_its_files_load(void) {
    struct io_dirent de;
    pfs_t fs;
    pfs_entry_t entry;
    uint32_t cluster, slot;
    int fd, seen = 0;

    fresh();
    put_host_dir("DIR");
    put_host("DIR/BIG", 300000, 1);
    put_host("ROOT", 5000, 2);

    CHECK(publish() == PS2_FOLDER_FILLING);
    CHECK(locate("DIR/BIG", &fs, &cluster, &slot, &entry));
    CHECK(entry.length == 300000);
    CHECK(!sim_page_ready((fs.alloc_offset + entry.cluster) * 2));

    /* Everything about the card but what its files hold is there already. */
    sim_pages_waited = 0;
    fd = mcio_mcDopen("/");
    CHECK(fd >= 0);
    while (mcio_mcDread(fd, &de) > 0)
        seen++;
    mcio_mcDclose(fd);
    CHECK(seen == 4);
    CHECK(sim_pages_waited == 0);

    CHECK(console_content_is("/ROOT", 5000, 2));
    CHECK(sim_pages_waited > 0);
    CHECK(ps2_folder_card_status() == PS2_FOLDER_FILLING);
    CHECK(console_content_is("/DIR/BIG", 300000, 1));

    pump_fill();
    CHECK(ps2_folder_card_status() == PS2_FOLDER_CLEAN);
    CHECK(views_match());
    CHECK(no_transaction_left());
    CHECK(unmount());
    CHECK(sim_sd_open_fds() == 0);
}

/* Read in order, a file should not cost the console a wait for every cluster. */
static void test_read_ahead_keeps_up_with_the_console(void) {
    pfs_t fs;
    pfs_entry_t entry;
    uint32_t cluster, slot, page;
    uint8_t buf[512];

    fresh();
    put_host("FIRST", 60000, 1);
    put_host("SECOND", 60000, 2);
    CHECK(publish() == PS2_FOLDER_FILLING);
    CHECK(locate("SECOND", &fs, &cluster, &slot, &entry));
    page = (fs.alloc_offset + entry.cluster + 10) * 2;

    con_read_page(page, buf);
    CHECK(sim_page_ready(page));
    CHECK(!sim_page_ready(page + 8));

    sim_time_step = 100000;
    for (int i = 0; i < 8; i++)
        ps2_folder_card_task();
    sim_time_step = 10;
    CHECK(sim_page_ready(page + 8));
    CHECK(!sim_page_ready((fs.alloc_offset + entry.cluster) * 2)); /* the sweep is nowhere near */

    pump_fill();
    CHECK(console_content_is("/SECOND", 60000, 2));
    CHECK(unmount());
}

static void test_console_writes_while_files_load(void) {
    fresh();
    put_host_dir("DIR");
    put_host("DIR/SAVE", 6000, 1);
    put_host("KEEP", 4000, 2);
    put_host("OVER", 4000, 3);
    put_host("GONE", 3000, 4);
    CHECK(mount() == PS2_FOLDER_CLEAN);
    CHECK(unmount());

    CHECK(publish() == PS2_FOLDER_FILLING);
    CHECK(con_put("/OVER", 4000, 11) == 4000);
    CHECK(con_put("/NEW", 9000, 12) == 9000);
    CHECK(con_remove("/GONE") == 0);
    CHECK(ps2_folder_card_status() == PS2_FOLDER_DIRTY);

    CHECK(settle() == PS2_FOLDER_CLEAN);
    CHECK(console_content_is("/OVER", 4000, 11));
    CHECK(console_content_is("/KEEP", 4000, 2));
    CHECK(host_content_is("OVER", 4000, 11));
    CHECK(host_content_is("NEW", 9000, 12));
    CHECK(host_content_is("KEEP", 4000, 2));
    CHECK(host_content_is("DIR/SAVE", 6000, 1));
    CHECK(!sd_exists(BASE "/GONE"));
    CHECK(views_match());
    CHECK(no_transaction_left());
    CHECK(unmount());

    CHECK(mount() == PS2_FOLDER_CLEAN);
    CHECK(views_match());
    CHECK(unmount());
}

/* The index has to say what is on SD, or a later save of the old content looks like no
 * change at all. */
static void test_same_size_edit_on_the_host(void) {
    fresh();
    put_host("SAVE", 3000, 1);
    CHECK(mount() == PS2_FOLDER_CLEAN);
    CHECK(unmount());

    put_host("SAVE", 3000, 2);
    CHECK(mount() == PS2_FOLDER_CLEAN);
    CHECK(console_content_is("/SAVE", 3000, 2));
    CHECK(con_put("/SAVE", 3000, 1) == 3000);
    CHECK(settle() == PS2_FOLDER_CLEAN);
    CHECK(host_content_is("SAVE", 3000, 1));
    CHECK(unmount());
}

static void test_importer_index_matches_a_scan(void) {
    fresh();
    put_host_dir("A");
    put_host_dir("A/B");
    put_host_dir("HOLLOW");
    put_host("A/B/C", 3000, 1);
    put_host("A/D", 1100, 2);
    put_host("ROOT", 1500, 3);
    put_host("CASE", 100, 4);
    put_host("%63%61%73%65", 200, 5);
    sim_sd_put(BASE "/EMPTY", "", 0);
    CHECK(mount() == PS2_FOLDER_CLEAN);
    CHECK(views_match());

    /* What a sync works from. A disagreement would rewrite files on every mount. */
    pf_sync_start(PF_SCAN_SYNC, true);
    while (pf_sync_step() != PF_STEP_DONE) {
    }
    CHECK(pf_sync_result() == PF_SYNC_NO_CHANGE);
    pf_sync_abort();
    CHECK(unmount());
    CHECK(no_transaction_left());

    CHECK(mount() == PS2_FOLDER_CLEAN);
    CHECK(views_match());
    CHECK(unmount());
}

static void test_free_space_reads_as_erased_while_loading(void) {
    uint8_t page[512];
    bool erased = true;

    fresh();
    put_host("A", 3000, 1);
    CHECK(mount() == PS2_FOLDER_CLEAN);
    CHECK(unmount());

    sim_console_reset();
    memset(sim_psram, 0xA5, sizeof(sim_psram));
    ps2_folder_card_open(BASE, false);
    pump_publish();
    CHECK(ps2_folder_card_status() == PS2_FOLDER_FILLING);
    CHECK(!sim_page_ready(8000));
    con_read_page(8000, page);
    for (size_t i = 0; i < sizeof(page); i++)
        erased = erased && page[i] == 0xFF;
    CHECK(erased);
    pump_fill();
    CHECK(unmount());
}

static void test_read_error_while_loading(void) {
    fresh();
    put_host("A", 4000, 1);
    put_host("B", 4000, 2);
    CHECK(mount() == PS2_FOLDER_CLEAN);
    CHECK(unmount());

    CHECK(publish() == PS2_FOLDER_FILLING);
    CHECK(con_put("/NEW", 1000, 3) == 1000);

    sim_op_count = 0;
    sim_fail_at = 1;
    sim_fail_sticky = true;
    pump_loaded();
    CHECK(ps2_folder_card_status() == PS2_FOLDER_ERROR);
    CHECK(ps2_folder_card_take_remount());
    CHECK(!ps2_folder_card_take_remount());
    sim_fail_at = 0;
    sim_fail_sticky = false;
    sim_sd_clear_faults();

    CHECK(unmount());
    CHECK(host_content_is("A", 4000, 1));
    CHECK(host_content_is("B", 4000, 2));

    /* What the console wrote is gone, which is the cost of showing the card early. */
    CHECK(mount() == PS2_FOLDER_CLEAN);
    CHECK(con_read("/NEW", file_buf, 10) < 0);
    CHECK(console_content_is("/A", 4000, 1));
    CHECK(unmount());
}

static void test_unmount_while_loading(void) {
    fresh();
    put_host("A", 40000, 1);
    CHECK(mount() == PS2_FOLDER_CLEAN);
    CHECK(unmount());

    /* Nothing to save, so this does not wait. */
    CHECK(publish() == PS2_FOLDER_FILLING);
    CHECK(unmount());
    CHECK(sim_sd_open_fds() == 0);
    CHECK(host_content_is("A", 40000, 1));

    /* Unsaved writes need the rest of the card first. */
    CHECK(publish() == PS2_FOLDER_FILLING);
    CHECK(con_put("/NEW", 1000, 2) == 1000);
    CHECK(!ps2_folder_card_flush());
    pump_loaded();
    CHECK(unmount());
    CHECK(sim_sd_open_fds() == 0);
    CHECK(host_content_is("NEW", 1000, 2));
    CHECK(host_content_is("A", 40000, 1));
    CHECK(no_transaction_left());
}

static void power_cut_while_loading(bool writes) {
    void *base;
    tree_t old_state, new_state;
    long n;

    build_base(setup_basic, &base);
    CHECK(mount() == PS2_FOLDER_CLEAN);
    old_state = tree_card();
    mutate_create_only();
    CHECK(settle() == PS2_FOLDER_CLEAN);
    new_state = tree_card();
    CHECK(unmount());

    /* A file changed on a computer, so that the load has something to commit. */
    sim_sd_restore(base);
    put_host("ROOTFILE", 900, 77);
    sim_sd_snapshot_free(base);
    base = sim_sd_snapshot();

    for (n = 1;; n++) {
        sim_sd_restore(base);
        CHECK(publish() == PS2_FOLDER_FILLING);
        if (writes)
            mutate_create_only();

        if (!cut_power_during(n, settle_void)) {
            CHECK(ps2_folder_card_status() == PS2_FOLDER_CLEAN);
            CHECK(unmount());
            break;
        }

        CHECK(mount() == PS2_FOLDER_CLEAN);
        CHECK(views_match());
        CHECK(console_content_is("/ROOTFILE", 900, 77));
        CHECK(no_transaction_left());
        CHECK(con_put("/PROBE", 700, 99) == 700);
        CHECK(settle() == PS2_FOLDER_CLEAN);
        CHECK(host_content_is("PROBE", 700, 99));
        CHECK(unmount());
        CHECK(sim_sd_open_fds() == 0);
    }
    printf("  (%ld cut points)\n", n - 1);
    CHECK(n > 10);
    sim_sd_snapshot_free(base);
    tree_free(&old_state);
    tree_free(&new_state);
}

static void test_power_cut_while_loading(void) { power_cut_while_loading(false); }
static void test_power_cut_while_loading_with_writes(void) { power_cut_while_loading(true); }

/* --- building from the index --- */

#define TREE_STAMP  META "/tree.ok"

static void tree_setup(void) {
    fresh();
    put_host_dir("DIR");
    put_host("DIR/SAVE", 5000, 1);
    put_host("ROOT", 1200, 2);
    sim_sd_put(BASE "/EMPTY", "", 0);
    CHECK(mount() == PS2_FOLDER_CLEAN);
    CHECK(unmount());
}

static bool mount_is_trusted(void) {
    bool trusted;

    CHECK(publish() == PS2_FOLDER_FILLING);
    trusted = pf_import_trusted();
    pump_fill();
    CHECK(ps2_folder_card_status() == PS2_FOLDER_CLEAN);
    CHECK(views_match());
    return trusted;
}

static void test_unchanged_folder_builds_from_the_index(void) {
    tree_setup();
    CHECK(sd_exists(TREE_STAMP));
    CHECK(mount_is_trusted());
    CHECK(console_content_is("/DIR/SAVE", 5000, 1));
    CHECK(unmount());
    CHECK(sim_sd_open_fds() == 0);
    CHECK(no_transaction_left());
}

static void test_build_from_index_needs_the_stamp(void) {
    size_t len = 0;
    uint8_t stamp[64];

    tree_setup();
    CHECK(sim_sd_get(TREE_STAMP, &len) != NULL);
    CHECK(len <= sizeof(stamp));
    memcpy(stamp, sim_sd_get(TREE_STAMP, &len), len);

    sim_sd_del(TREE_STAMP);
    CHECK(!mount_is_trusted());
    CHECK(unmount());
    CHECK(sd_exists(TREE_STAMP));

    stamp[10] ^= 1;
    sim_sd_put(TREE_STAMP, stamp, len);
    CHECK(!mount_is_trusted());
    CHECK(unmount());

    sim_sd_put(TREE_STAMP, stamp, 7);
    CHECK(!mount_is_trusted());
    CHECK(unmount());
    CHECK(mount_is_trusted());
    CHECK(unmount());
}

static void test_changed_folder_is_walked(void) {
    tree_setup();
    put_host("ADDED", 700, 3);
    CHECK(!mount_is_trusted());
    CHECK(console_content_is("/ADDED", 700, 3));
    CHECK(unmount());

    put_host("ROOT", 1300, 4);
    CHECK(!mount_is_trusted());
    CHECK(console_content_is("/ROOT", 1300, 4));
    CHECK(unmount());

    sim_sd_del(BASE "/ADDED");
    CHECK(!mount_is_trusted());
    CHECK(con_read("/ADDED", file_buf, 10) < 0);
    CHECK(unmount());

    sim_sd_del(BASE "/ROOT");
    put_host("MOVED", 1300, 4);
    CHECK(!mount_is_trusted());
    CHECK(con_read("/ROOT", file_buf, 10) < 0);
    CHECK(console_content_is("/MOVED", 1300, 4));
    CHECK(unmount());

    CHECK(mount_is_trusted());
    CHECK(unmount());
}

/* The same blind spot as a snapshot, except that nothing old is served. */
static void test_build_from_index_reads_same_size_edits(void) {
    tree_setup();
    put_host("ROOT", 1200, 5);
    CHECK(mount_is_trusted());
    CHECK(console_content_is("/ROOT", 1200, 5));
    CHECK(con_put("/ROOT", 1200, 2) == 1200);
    CHECK(settle() == PS2_FOLDER_CLEAN);
    CHECK(host_content_is("ROOT", 1200, 2));
    CHECK(unmount());
}

static void test_build_from_index_follows_saves(void) {
    tree_setup();
    CHECK(mount() == PS2_FOLDER_CLEAN);
    CHECK(con_put("/DIR/SAVE", 7000, 6) == 7000);
    CHECK(con_put("/NEW", 900, 7) == 900);
    CHECK(settle() == PS2_FOLDER_CLEAN);
    CHECK(unmount());

    CHECK(mount_is_trusted());
    CHECK(console_content_is("/DIR/SAVE", 7000, 6));
    CHECK(console_content_is("/NEW", 900, 7));
    CHECK(unmount());
}

static void test_stamp_written_while_idle(void) {
    tree_setup();
    sim_sd_del(TREE_STAMP);
    CHECK(mount() == PS2_FOLDER_CLEAN);
    CHECK(pf_tree_stale());
    for (int i = 0; i < 100000 && pf_tree_stale(); i++) {
        sim_time_advance_ms(100);
        ps2_folder_card_task();
    }
    CHECK(sd_exists(TREE_STAMP));
    CHECK(unmount());
    CHECK(mount_is_trusted());
    CHECK(unmount());
}

/* --- snapshot --- */

#define SNAP_IMAGE  META "/snapshot.mcd"
#define SNAP_STAMP  META "/snapshot.ok"

static bool snap_served;

static void snap_setup(void) {
    put_host_dir("DIR");
    put_host("DIR/SAVE", 5000, 1);
    put_host("DIR/ICON", 900, 2);
    put_host("ROOT", 1200, 3);
}

static void snap_load(int fd) {
    for (uint32_t pos = 0; pos < SIM_PSRAM_SIZE; pos += 512)
        CHECK(sd_read(fd, &sim_psram[pos], 512) == 512);
    pf_pages_ready(0, SIM_PSRAM_SIZE / 512);
}

/* What cardman does at open, with the sector loader in one go. */
static ps2_folder_status_t mount_snapshot(void) {
    sim_console_reset();
    ps2_folder_card_open(BASE, true);

    int fd = ps2_folder_card_snapshot_open();

    snap_served = fd >= 0;
    if (snap_served) {
        snap_load(fd);
        ps2_folder_card_snapshot_loaded();
        for (int i = 0; i < 100000 && ps2_folder_card_status() == PS2_FOLDER_VERIFYING; i++)
            ps2_folder_card_task();
        CHECK(!ps2_folder_card_take_remount());
    } else {
        pump_mount();
    }
    con_refresh();
    return ps2_folder_card_status();
}

static void cache(void) {
    for (int i = 0; i < 2000000 && (pf_snapshot_outdated() || ps2_folder_card_status() == PS2_FOLDER_CACHING); i++) {
        sim_time_advance_ms(100);
        ps2_folder_card_task();
    }
}

static void cache_void(void) {
    cache();
}

static void settle_and_cache(void) {
    settle();
    cache();
}

/* A card whose next mount can be served from its snapshot. */
static void snap_prepare(void) {
    fresh();
    snap_setup();
    CHECK(mount_snapshot() == PS2_FOLDER_CLEAN);
    CHECK(!snap_served);
    cache();
    CHECK(sd_exists(SNAP_STAMP));
    CHECK(unmount());
}

static void test_snapshot_off_by_default(void) {
    fresh();
    snap_setup();
    CHECK(mount() == PS2_FOLDER_CLEAN);
    CHECK(con_put("/NEW", 700, 4) == 700);
    CHECK(settle() == PS2_FOLDER_CLEAN);
    for (int i = 0; i < 200; i++) {
        sim_time_advance_ms(100);
        ps2_folder_card_task();
    }
    CHECK(unmount());
    CHECK(!sd_exists(SNAP_IMAGE));
    CHECK(!sd_exists(SNAP_STAMP));

    /* A snapshot left from when it was on is not used, and costs nothing else. */
    snap_prepare();
    ps2_folder_card_open(BASE, false);
    CHECK(ps2_folder_card_snapshot_open() == -1);
    pump_mount();
    CHECK(ps2_folder_card_status() == PS2_FOLDER_CLEAN);
    CHECK(unmount());
    CHECK(sim_sd_open_fds() == 0);
}

static void test_snapshot_served_as_it_was(void) {
    static uint8_t before[SIM_PSRAM_SIZE];
    size_t len = 0;
    tree_t want;

    fresh();
    snap_setup();
    CHECK(mount_snapshot() == PS2_FOLDER_CLEAN);
    CHECK(!snap_served);
    want = tree_card();
    cache();
    CHECK(ps2_folder_card_status() == PS2_FOLDER_CLEAN);
    CHECK(sd_exists(SNAP_STAMP));
    CHECK(sim_sd_get(SNAP_IMAGE, &len) != NULL);
    CHECK(len == SIM_PSRAM_SIZE);
    memcpy(before, sim_psram, sizeof(before));
    CHECK(unmount());
    CHECK(sim_sd_open_fds() == 0);

    for (int i = 0; i < 2; i++) {
        CHECK(mount_snapshot() == PS2_FOLDER_CLEAN);
        CHECK(snap_served);
        CHECK(memcmp(before, sim_psram, sizeof(before)) == 0);
        CHECK(views_match());

        tree_t card = tree_card();
        CHECK(tree_equal(&card, &want));
        tree_free(&card);
        CHECK(!pf_snapshot_outdated());
        CHECK(unmount());
        CHECK(sim_sd_open_fds() == 0);
    }
    tree_free(&want);
}

static void test_snapshot_keeps_up_with_saves(void) {
    snap_prepare();

    CHECK(mount_snapshot() == PS2_FOLDER_CLEAN);
    CHECK(snap_served);
    CHECK(con_put("/DIR/SAVE", 5000, 11) == 5000);
    CHECK(settle() == PS2_FOLDER_CLEAN);
    CHECK(host_content_is("DIR/SAVE", 5000, 11));

    sim_op_count = 0;
    cache();
    CHECK(sim_op_count < 100); /* the changed clusters, not the whole card */
    CHECK(sd_exists(SNAP_STAMP));
    CHECK(unmount());

    CHECK(mount_snapshot() == PS2_FOLDER_CLEAN);
    CHECK(snap_served);
    CHECK(console_content_is("/DIR/SAVE", 5000, 11));

    /* The next synchronization starts from the snapshot's layout. */
    CHECK(con_remove("/ROOT") == 0);
    CHECK(settle() == PS2_FOLDER_CLEAN);
    CHECK(!host_has("ROOT"));
    CHECK(host_content_is("DIR/SAVE", 5000, 11));
    CHECK(views_match());
    CHECK(unmount());
}

typedef void (*edit_t)(void);

static void edit_grow(void) { put_host("ROOT", 1300, 3); }
static void edit_same_size_new_date(void) { sim_mtime[1]++; }
static void edit_add_file(void) { put_host("DIR/NEW", 100, 5); }
static void edit_add_dir(void) { put_host_dir("DIR2"); }
static void edit_add_nested(void) { put_host_dir("DIR2"); put_host("DIR2/NEW", 100, 5); }
static void edit_remove(void) { sim_sd_del(BASE "/DIR/ICON"); }
static void edit_new_tree(void) { sim_sd_del(BASE "/DIR/ICON"); sim_sd_del(BASE "/DIR/SAVE"); sim_sd_del(BASE "/DIR"); }

static void test_snapshot_never_hides_folder_edits(void) {
    static const edit_t edits[] = {edit_grow, edit_same_size_new_date, edit_add_file, edit_add_dir,
                                   edit_add_nested, edit_remove, edit_new_tree};
    const uint8_t mtime[8];
    void *base;

    memcpy((void *)mtime, sim_mtime, sizeof(mtime));
    snap_prepare();
    base = sim_sd_snapshot();

    for (size_t i = 0; i < sizeof(edits) / sizeof(edits[0]); i++) {
        sim_sd_restore(base);
        edits[i]();

        CHECK(mount_snapshot() == PS2_FOLDER_CLEAN);
        CHECK(!snap_served);
        CHECK(views_match());
        cache();
        CHECK(unmount());

        /* ...and the snapshot is good again once it has caught up. */
        CHECK(mount_snapshot() == PS2_FOLDER_CLEAN);
        CHECK(snap_served);
        CHECK(views_match());
        CHECK(unmount());
        memcpy(sim_mtime, mtime, sizeof(mtime));
    }

    /* A computer leaving droppings in the folder changes nothing on the card. */
    sim_sd_restore(base);
    put_host("Thumbs.db", 100, 6);
    CHECK(mount_snapshot() == PS2_FOLDER_CLEAN);
    CHECK(snap_served);
    CHECK(unmount());
    sim_sd_snapshot_free(base);
}

static void test_snapshot_after_power_cuts_outside_it(void) {
    /* Saved, but the power went before the snapshot caught up. */
    snap_prepare();
    CHECK(mount_snapshot() == PS2_FOLDER_CLEAN);
    CHECK(con_put("/NEW", 700, 4) == 700);
    sim_time_advance_ms(2000);
    for (int i = 0; i < 100000 && (!host_has("NEW") || sd_exists(META "/journal.bin")); i++)
        ps2_folder_card_task();
    ps2_folder_card_power_loss();
    sim_sd_power_cycle();
    CHECK(mount_snapshot() == PS2_FOLDER_CLEAN);
    CHECK(!snap_served);
    CHECK(console_content_is("/NEW", 700, 4));
    CHECK(unmount());

    /* Written but never saved: both the snapshot and the folder are the state before it. */
    snap_prepare();
    CHECK(mount_snapshot() == PS2_FOLDER_CLEAN);
    CHECK(con_put("/NEW", 700, 4) == 700);
    ps2_folder_card_power_loss();
    sim_sd_power_cycle();
    CHECK(mount_snapshot() == PS2_FOLDER_CLEAN);
    CHECK(snap_served);
    CHECK(views_match());
    CHECK(unmount());
}

static void snap_save_mixed(void) {
    CHECK(con_put("/DIR/SAVE", 5000, 21) == 5000);
    CHECK(con_put("/NEW", 700, 22) == 700);
}

/* What games do. Size and date stay the same, so only the index tells it apart. */
static void snap_save_same_size(void) {
    CHECK(con_put("/DIR/SAVE", 5000, 23) == 5000);
}

static void save_settle_unmount(void) {
    settle();
    unmount();
}

/* Every cut leaves either the old card or the new one, and never a stale snapshot. */
static void snap_power_matrix(const char *label, void (*save)(void), void (*during)(void)) {
    void *base;
    tree_t old_state, new_state;
    long n, slow = 0, fast = 0;

    snap_prepare();
    base = sim_sd_snapshot();
    CHECK(mount_snapshot() == PS2_FOLDER_CLEAN);
    old_state = tree_card();
    save();
    CHECK(settle() == PS2_FOLDER_CLEAN);
    new_state = tree_card();
    CHECK(unmount());

    for (n = 1;; n++) {
        tree_t card;

        sim_sd_restore(base);
        CHECK(mount_snapshot() == PS2_FOLDER_CLEAN);
        CHECK(snap_served);
        save();

        if (!cut_power_during(n, during)) {
            ps2_folder_card_close();
            break;
        }

        CHECK(mount_snapshot() == PS2_FOLDER_CLEAN);
        if (snap_served)
            fast++;
        else
            slow++;
        CHECK(views_match());
        CHECK(mounted_state_is(&old_state, &new_state));
        CHECK(no_transaction_left());

        card = tree_card();
        if (snap_served)
            CHECK(tree_equal(&card, &old_state)); /* it may only ever be the last clean state */
        tree_free(&card);

        CHECK(con_put("/PROBE", 700, 99) == 700);
        CHECK(settle() == PS2_FOLDER_CLEAN);
        cache();
        CHECK(sd_exists(SNAP_STAMP));
        CHECK(unmount());

        /* A snapshot written after the cut is as good as any. */
        CHECK(mount_snapshot() == PS2_FOLDER_CLEAN);
        CHECK(snap_served);
        CHECK(console_content_is("/PROBE", 700, 99));
        CHECK(unmount());
        CHECK(sim_sd_open_fds() == 0);
    }
    printf("  (%s: %ld cut points, %ld from the folder, %ld from the snapshot)\n", label, n - 1, slow, fast);
    CHECK(slow > 0);
    sim_sd_snapshot_free(base);
    tree_free(&old_state);
    tree_free(&new_state);
}

static void test_snapshot_power_cut_while_caching(void) {
    snap_power_matrix("caching", snap_save_mixed, settle_and_cache);
    snap_power_matrix("caching, same size", snap_save_same_size, settle_and_cache);
}

static void test_snapshot_power_cut_while_closing(void) {
    snap_power_matrix("closing", snap_save_mixed, save_settle_unmount);
    snap_power_matrix("closing, same size", snap_save_same_size, save_settle_unmount);
}

/* Clusters that change no file leave the index alone, so here the stamp itself is what
 * stops a half-updated snapshot from being served. */
static void test_snapshot_stamp_goes_first(void) {
    static uint8_t before[SIM_PSRAM_SIZE];
    uint32_t page = 7000 * PFS_PAGES_PER_CLUSTER;
    void *base;
    long n;

    snap_prepare();
    base = sim_sd_snapshot();

    for (n = 1;; n++) {
        sim_sd_restore(base);
        CHECK(mount_snapshot() == PS2_FOLDER_CLEAN);
        CHECK(snap_served);
        memcpy(before, sim_psram, sizeof(before));

        memset(&sim_psram[page * 512], 0x11, 512);
        memset(&sim_psram[(page + 2) * 512], 0x22, 512);
        con_touch_dirty(page);
        con_touch_dirty(page + 2);

        if (!cut_power_during(n, settle_and_cache)) {
            ps2_folder_card_close();
            break;
        }

        CHECK(mount_snapshot() == PS2_FOLDER_CLEAN);
        CHECK(views_match());
        CHECK(unmount());
    }
    printf("  (%ld cut points)\n", n - 1);
    CHECK(n > 3);
    sim_sd_snapshot_free(base);
}

static void test_snapshot_power_cut_while_created(void) {
    static const long at[] = {1, 2, 3, 4, 5, 6, 7, 8, 100, 4000, 8190, 8192, 8193, 8194, 8195, 8196, 8197, 8198, 8199, 8200};
    void *base;

    fresh();
    snap_setup();
    base = sim_sd_snapshot();

    for (size_t i = 0; i < sizeof(at) / sizeof(at[0]); i++) {
        sim_sd_restore(base);
        CHECK(mount_snapshot() == PS2_FOLDER_CLEAN);
        if (!cut_power_during(at[i], cache_void)) {
            CHECK(at[i] >= 8192);
            CHECK(unmount());
            continue;
        }

        CHECK(mount_snapshot() == PS2_FOLDER_CLEAN);
        CHECK(!snap_served || at[i] >= 8192);
        CHECK(views_match());
        cache();
        CHECK(unmount());

        CHECK(mount_snapshot() == PS2_FOLDER_CLEAN);
        CHECK(snap_served);
        CHECK(views_match());
        CHECK(unmount());
    }
    sim_sd_snapshot_free(base);
}

static void corrupt_snapshot(size_t offset, size_t len, uint8_t value) {
    size_t size = 0;
    uint8_t *data = (uint8_t *)sim_sd_get(SNAP_IMAGE, &size);

    CHECK(data != NULL && offset + len <= size);
    memset(&data[offset], value, len);
}

/* A valid stamp only says what was written. */
static void test_snapshot_damage_is_found(void) {
    tree_t want;

    snap_prepare();
    corrupt_snapshot(6 * 1024 * 1024, 1, 0x5A);

    sim_console_reset();
    ps2_folder_card_open(BASE, true);
    int fd = ps2_folder_card_snapshot_open();
    CHECK(fd >= 0);
    snap_load(fd);
    ps2_folder_card_snapshot_loaded();
    for (int i = 0; i < 100000 && ps2_folder_card_status() == PS2_FOLDER_VERIFYING; i++)
        ps2_folder_card_task();
    CHECK(ps2_folder_card_take_remount());
    pump_mount();
    con_refresh();
    CHECK(ps2_folder_card_status() == PS2_FOLDER_CLEAN);
    CHECK(views_match());
    CHECK(!sd_exists(SNAP_STAMP));
    want = tree_card();

    cache();
    CHECK(unmount());
    CHECK(mount_snapshot() == PS2_FOLDER_CLEAN);
    CHECK(snap_served);

    tree_t card = tree_card();
    CHECK(tree_equal(&card, &want));
    tree_free(&card);
    tree_free(&want);
    CHECK(unmount());
}

/* The dangerous case: a damaged root directory looks like every save was deleted, and
 * synchronizing that to the folder would act on it. */
static void damaged_snapshot_with_writes(bool flush) {
    tree_t want;

    snap_prepare();
    want = tree_host();
    corrupt_snapshot(49 * 1024, 1024, 0);

    sim_console_reset();
    ps2_folder_card_open(BASE, true);
    int fd = ps2_folder_card_snapshot_open();
    CHECK(fd >= 0);
    snap_load(fd);
    con_touch_dirty(49 * 2);
    ps2_folder_card_snapshot_loaded();

    if (flush) {
        CHECK(ps2_folder_card_flush());
    } else {
        for (int i = 0; i < 100000 && !ps2_folder_card_take_remount(); i++) {
            sim_time_advance_ms(100);
            ps2_folder_card_task();
        }
    }
    if (!flush) {
        pump_mount();
        con_refresh();
        CHECK(ps2_folder_card_status() == PS2_FOLDER_CLEAN);
        sim_time_advance_ms(5000);
        CHECK(settle() == PS2_FOLDER_CLEAN);
    }
    CHECK(unmount());

    tree_t host = tree_host();
    CHECK(tree_equal(&host, &want));
    tree_free(&host);
    tree_free(&want);
    CHECK(sim_sd_open_fds() == 0);

    /* Either rebuilt from the folder or gone, but never the damaged card stamped again. */
    if (flush)
        CHECK(!sd_exists(SNAP_STAMP));
    CHECK(mount_snapshot() == PS2_FOLDER_CLEAN);
    CHECK(views_match());
    CHECK(unmount());
}

static void test_snapshot_damage_never_reaches_the_folder(void) {
    damaged_snapshot_with_writes(false);
    damaged_snapshot_with_writes(true);
}

static void test_snapshot_read_error_loads_from_folder(void) {
    snap_prepare();

    sim_console_reset();
    ps2_folder_card_open(BASE, true);
    CHECK(ps2_folder_card_snapshot_open() >= 0);
    ps2_folder_card_snapshot_failed();
    pump_mount();
    con_refresh();
    CHECK(ps2_folder_card_status() == PS2_FOLDER_CLEAN);
    CHECK(views_match());
    CHECK(unmount());
    CHECK(sim_sd_open_fds() == 0);
}

static void test_snapshot_needs_its_stamp(void) {
    static uint8_t bytes[64];
    size_t len = 0;
    const uint8_t *stamp;
    void *base;

    snap_prepare();
    base = sim_sd_snapshot();
    stamp = sim_sd_get(SNAP_STAMP, &len);
    CHECK(stamp != NULL && len <= sizeof(bytes));
    memcpy(bytes, stamp, len);

    sim_sd_del(SNAP_STAMP);
    CHECK(mount_snapshot() == PS2_FOLDER_CLEAN);
    CHECK(!snap_served);
    CHECK(unmount());

    for (size_t i = 0; i < len; i++) {
        sim_sd_restore(base);
        bytes[i] ^= 0x10;
        sim_sd_put(SNAP_STAMP, bytes, len);
        bytes[i] ^= 0x10;
        CHECK(mount_snapshot() == PS2_FOLDER_CLEAN);
        CHECK(!snap_served);
        CHECK(views_match());
        CHECK(unmount());
    }

    sim_sd_restore(base);
    sim_sd_put(SNAP_STAMP, bytes, len - 1);
    CHECK(mount_snapshot() == PS2_FOLDER_CLEAN);
    CHECK(!snap_served);
    CHECK(unmount());

    sim_sd_restore(base);
    sim_sd_put(SNAP_IMAGE, bytes, len);
    CHECK(mount_snapshot() == PS2_FOLDER_CLEAN);
    CHECK(!snap_served);
    CHECK(unmount());

    sim_sd_restore(base);
    sim_sd_del(SNAP_IMAGE);
    CHECK(mount_snapshot() == PS2_FOLDER_CLEAN);
    CHECK(!snap_served);
    CHECK(unmount());
    sim_sd_snapshot_free(base);
}

static void test_snapshot_gives_way_on_a_full_card(void) {
    fresh();
    snap_setup();
    CHECK(mount_snapshot() == PS2_FOLDER_CLEAN);

    sim_free_bytes = 1024 * 1024;
    cache();
    sim_free_bytes = -1;
    CHECK(!pf_snapshot_outdated());
    CHECK(!sd_exists(SNAP_IMAGE));
    CHECK(!sd_exists(SNAP_STAMP));

    /* The save it would have been in the way of. */
    CHECK(con_put("/NEW", 700, 4) == 700);
    CHECK(settle() == PS2_FOLDER_CLEAN);
    CHECK(host_content_is("NEW", 700, 4));
    CHECK(unmount());
}

static void test_snapshot_gives_way_to_the_console(void) {
    long ops = 0;

    fresh();
    snap_setup();
    CHECK(mount_snapshot() == PS2_FOLDER_CLEAN);

    sim_time_advance_ms(2000);
    for (int i = 0; i < 100000 && ps2_folder_card_status() != PS2_FOLDER_CACHING; i++)
        ps2_folder_card_task();
    sim_op_count = 0;
    CHECK(ps2_folder_card_status() == PS2_FOLDER_CACHING);
    ps2_folder_card_task();
    ps2_folder_card_task();
    ops = sim_op_count;
    CHECK(ops > 0);

    sim_console_busy = true;
    ps2_folder_card_task();
    CHECK(sim_op_count == ops);
    sim_console_busy = false;

    CHECK(con_put("/NEW", 700, 4) == 700);
    ps2_folder_card_task();
    CHECK(ps2_folder_card_status() != PS2_FOLDER_CACHING);
    CHECK(settle() == PS2_FOLDER_CLEAN);
    CHECK(host_content_is("NEW", 700, 4));

    /* What was copied before the interruption is not copied again. */
    cache();
    CHECK(sim_op_count < 8192 + 100);
    CHECK(unmount());

    CHECK(mount_snapshot() == PS2_FOLDER_CLEAN);
    CHECK(snap_served);
    CHECK(console_content_is("/NEW", 700, 4));
    CHECK(unmount());
}

static void test_snapshot_writes_while_loading_are_saved(void) {
    snap_prepare();

    sim_console_reset();
    ps2_folder_card_open(BASE, true);
    int fd = ps2_folder_card_snapshot_open();
    CHECK(fd >= 0);
    CHECK(ps2_folder_card_status() == PS2_FOLDER_VERIFYING);

    /* The load cannot be abandoned once something was written to it. */
    snap_load(fd);
    con_refresh();
    CHECK(con_put("/NEW", 700, 4) == 700);
    CHECK(ps2_folder_card_status() == PS2_FOLDER_DIRTY);
    CHECK(!ps2_folder_card_flush());

    ps2_folder_card_snapshot_loaded();
    CHECK(ps2_folder_card_flush());
    CHECK(host_content_is("NEW", 700, 4));
    ps2_folder_card_close();

    /* Otherwise there is nothing to wait for. */
    sim_console_reset();
    ps2_folder_card_open(BASE, true);
    CHECK(ps2_folder_card_snapshot_open() == -1 || ps2_folder_card_flush());
    ps2_folder_card_close();
    CHECK(sim_sd_open_fds() == 0);
}

static void test_snapshot_close_waits_only_for_little(void) {
    snap_prepare();

    /* One save is worth finishing before a card switch... */
    CHECK(mount_snapshot() == PS2_FOLDER_CLEAN);
    CHECK(snap_served);
    CHECK(con_put("/NEW", 700, 4) == 700);
    CHECK(unmount());
    CHECK(sd_exists(SNAP_STAMP));
    CHECK(mount_snapshot() == PS2_FOLDER_CLEAN);
    CHECK(snap_served);
    CHECK(console_content_is("/NEW", 700, 4));
    CHECK(unmount());

    /* ...writing the whole card is not. */
    fresh();
    snap_setup();
    CHECK(mount_snapshot() == PS2_FOLDER_CLEAN);
    CHECK(unmount());
    CHECK(!sd_exists(SNAP_STAMP));
}

static void test_config_folder_snapshot(void) {
    fresh();

    CHECK(!card_config_get_ps2_folder_snapshot("Card1", "Card1"));
    put_config("[Settings]\r\nStorageBackend=FOLDER\r\nFolderSnapshot=ON");
    CHECK(card_config_get_ps2_folder_snapshot("Card1", "Card1"));
    CHECK(card1_is_folder());
    put_config("[Settings]\r\nFolderSnapshot=on\r\n");
    CHECK(card_config_get_ps2_folder_snapshot("Card1", "Card1"));
    put_config("[Settings]\r\nFolderSnapshot=OFF");
    CHECK(!card_config_get_ps2_folder_snapshot("Card1", "Card1"));
    put_config("[Settings]\r\nFolderSnapshot=MAYBE");
    CHECK(!card_config_get_ps2_folder_snapshot("Card1", "Card1"));
    put_config("[Settings]\r\nStorageBackend=FOLDER");
    CHECK(!card_config_get_ps2_folder_snapshot("Card1", "Card1"));
    CHECK(sim_sd_open_fds() == 0);
}

/* --- a real folder, when one is given --- */

static long real_files;
static long real_bytes;

/* Loads a host directory into the simulated SD, or checks both sides against it. */
static void real_folder_walk(const char *host, const char *rel, bool load) {
    static uint8_t want[MAX_FILE];
    static uint8_t got[MAX_FILE];
    DIR *dir = opendir(host);
    struct dirent *e;

    CHECK(dir != NULL);
    while (dir && (e = readdir(dir)) != NULL) {
        char child[1024], next[PATH_LEN], sd[PATH_LEN + 64];
        struct stat st;

        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..") || !strcmp(e->d_name, ".ps2meta"))
            continue;
        snprintf(child, sizeof(child), "%s/%s", host, e->d_name);
        snprintf(next, sizeof(next), "%s/%s", rel, e->d_name);
        snprintf(sd, sizeof(sd), "%s%s", BASE, next);
        CHECK(stat(child, &st) == 0);

        if (S_ISDIR(st.st_mode)) {
            if (load)
                sim_sd_mkdirs(sd);
            real_folder_walk(child, next, load);
            continue;
        }

        FILE *f = fopen(child, "rb");
        CHECK(f != NULL);
        size_t len = f ? fread(want, 1, sizeof(want), f) : 0;
        if (f)
            fclose(f);

        if (load) {
            sim_sd_put(sd, want, len);
            real_files++;
            real_bytes += (long)len;
        } else {
            size_t host_len = 0;
            const uint8_t *data = sim_sd_get(sd, &host_len);
            int n = con_read(next, got, MAX_FILE);

            if (n != (int)len || memcmp(got, want, len) != 0) {
                failures++;
                printf("  FAIL console view of %s: %d of %zu bytes\n", next, n, len);
            }
            if (host_len != len || (len && (!data || memcmp(data, want, len) != 0))) {
                failures++;
                printf("  FAIL SD view of %s: %zu of %zu bytes\n", next, host_len, len);
            }
            checks += 2;
        }
    }
    if (dir)
        closedir(dir);
}

static void test_real_folder(void) {
    const char *host = getenv("PF_HOST_FOLDER");

    if (!host)
        return;

    fresh();
    real_files = real_bytes = 0;
    real_folder_walk(host, "", true);
    printf("  %ld files, %ld bytes\n", real_files, real_bytes);

    CHECK(mount() == PS2_FOLDER_CLEAN);
    printf("  %d clusters free after the import\n", con_free_clusters());
    real_folder_walk(host, "", false);

    /* Saving must leave every other save exactly as it was. */
    CHECK(con_put("/PFTEST", 3000, 77) == 3000);
    CHECK(settle() == PS2_FOLDER_CLEAN);
    CHECK(host_content_is("PFTEST", 3000, 77));
    real_folder_walk(host, "", false);

    CHECK(unmount());
    CHECK(mount() == PS2_FOLDER_CLEAN);
    CHECK(console_content_is("/PFTEST", 3000, 77));
    real_folder_walk(host, "", false);

    CHECK(con_remove("/PFTEST") == 0);
    CHECK(settle() == PS2_FOLDER_CLEAN);
    CHECK(!host_has("PFTEST"));
    CHECK(unmount());
    CHECK(mount() == PS2_FOLDER_CLEAN);
    real_folder_walk(host, "", false);
    CHECK(unmount());
}

#define T(fn) {#fn, fn}

static const test_t tests[] = {
    T(test_name_codec),
    T(test_empty_folder),
    T(test_missing_folder_is_created),
    T(test_import_tree),
    T(test_import_is_hidden_until_complete),
    T(test_import_boundary_sizes),
    T(test_import_names),
    T(test_unsupported_host_name_is_rejected),
    T(test_capacity_failure_changes_nothing),
    T(test_single_oversized_file),
    T(test_card_filled_to_the_last_cluster),
    T(test_entry_limit),
    T(test_many_entries_in_one_directory),
    T(test_depth_limit),
    T(test_internal_files_are_not_exposed),
    T(test_coexists_with_image),
    T(test_metadata_survives_remount),
    T(test_directory_order_survives_remount),
    T(test_offline_host_changes),
    T(test_write_back_create),
    T(test_write_back_modify_truncate_extend),
    T(test_empty_file_with_stray_cluster_value),
    T(test_write_back_in_place),
    T(test_write_back_delete),
    T(test_host_droppings_do_not_resurrect_directories),
    T(test_write_back_rename_directory),
    T(test_write_back_case_only_rename),
    T(test_names_differing_only_in_case),
    T(test_file_replaced_by_directory),
    T(test_no_writes_means_no_sd_activity),
    T(test_rewrite_with_same_content),
    T(test_console_write_during_scan),
    T(test_console_write_at_every_moment),
    T(test_commit_is_a_single_snapshot),
    T(test_file_open_on_console_is_deferred),
    T(test_flush_on_unmount),
    T(test_two_cards_are_isolated),
    T(test_path_protection),
    T(test_invalid_superblock),
    T(test_invalid_directory_chain),
    T(test_invalid_file_cycle),
    T(test_invalid_cross_link),
    T(test_invalid_short_chain),
    T(test_invalid_bad_entry),
    T(test_invalid_duplicate_name),
    T(test_console_format_moves_saves_to_trash),
    T(test_partial_erase_is_not_a_format),
    T(test_power_cut_mixed),
    T(test_power_cut_create),
    T(test_power_cut_delete),
    T(test_power_cut_format),
    T(test_power_cut_case_rename),
    T(test_power_cut_dir_rename),
    T(test_power_cut_during_recovery),
    T(test_sd_error_once),
    T(test_sd_error_persistent),
    T(test_sd_full),
    T(test_truncated_journal_is_discarded),
    T(test_corrupt_journal_is_discarded),
    T(test_missing_stage_file_requires_recovery),
    T(test_damaged_index_keeps_all_files),
    T(test_missing_metadata_keeps_all_files),
    T(test_leftover_stage_files_are_removed),
    T(test_trash_never_overwrites),
    T(test_import_export_is_lossless),
    T(test_random_sessions),
    T(test_config_storage_backend),
    T(test_config_game2folder),
    T(test_config_last_line_of_other_settings),
    T(test_config_boot_card_name),
    T(test_new_card_backend),
    T(test_free_space_is_erased_over_stale_psram),
    T(test_index_crc_needs_no_second_read),
    T(test_failed_open_is_not_a_removed_file),
    T(test_new_file_before_a_known_one),
    T(test_card_is_served_before_its_files_load),
    T(test_read_ahead_keeps_up_with_the_console),
    T(test_console_writes_while_files_load),
    T(test_same_size_edit_on_the_host),
    T(test_importer_index_matches_a_scan),
    T(test_free_space_reads_as_erased_while_loading),
    T(test_read_error_while_loading),
    T(test_unmount_while_loading),
    T(test_power_cut_while_loading),
    T(test_power_cut_while_loading_with_writes),
    T(test_unchanged_folder_builds_from_the_index),
    T(test_build_from_index_needs_the_stamp),
    T(test_changed_folder_is_walked),
    T(test_build_from_index_reads_same_size_edits),
    T(test_build_from_index_follows_saves),
    T(test_stamp_written_while_idle),
    T(test_snapshot_off_by_default),
    T(test_snapshot_served_as_it_was),
    T(test_snapshot_keeps_up_with_saves),
    T(test_snapshot_never_hides_folder_edits),
    T(test_snapshot_after_power_cuts_outside_it),
    T(test_snapshot_power_cut_while_caching),
    T(test_snapshot_power_cut_while_closing),
    T(test_snapshot_stamp_goes_first),
    T(test_snapshot_power_cut_while_created),
    T(test_snapshot_damage_is_found),
    T(test_snapshot_damage_never_reaches_the_folder),
    T(test_snapshot_read_error_loads_from_folder),
    T(test_snapshot_needs_its_stamp),
    T(test_snapshot_gives_way_on_a_full_card),
    T(test_snapshot_gives_way_to_the_console),
    T(test_snapshot_writes_while_loading_are_saved),
    T(test_snapshot_close_waits_only_for_little),
    T(test_config_folder_snapshot),
    T(test_real_folder),
};

int main(int argc, char **argv) {
    int ran = 0;

    setvbuf(stdout, NULL, _IOLBF, 0);

    for (size_t i = 0; i < sizeof(tests) / sizeof(tests[0]); i++) {
        int before = failures;

        if (argc > 1 && !strstr(tests[i].name, argv[1]))
            continue;
        current_test = tests[i].name;
        printf("%s\n", tests[i].name);
        fflush(stdout);
        tests[i].fn();
        ps2_folder_card_power_loss();
        ran++;
        if (failures != before)
            printf("  -> %d failure(s)\n", failures - before);
    }

    printf("\n%d tests, %d checks, %d failures\n", ran, checks, failures);
    return failures ? 1 : 0;
}
