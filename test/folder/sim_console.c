/* Simulated PSRAM and the console's view of it. The console side runs the real mcfat
 * driver, so the filesystems the tests produce are the ones an actual driver writes,
 * not ones shaped by this project's own assumptions. */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "mcfat.h"
#include "mcio.h"
#include "ps2_folder_card.h"
#include "ps2_folder_port.h"
#include "sim.h"

#define PAGE_SIZE 512U

uint8_t sim_psram[SIM_PSRAM_SIZE];
uint32_t sim_generation;
bool sim_pending;
bool sim_console_busy;
uint32_t sim_pages_waited;

#define SIM_PAGES (SIM_PSRAM_SIZE / PAGE_SIZE)

static uint8_t page_ready[SIM_PAGES / 8];
static int wanted_page = -1;
static uint64_t sim_time;
uint32_t sim_time_step = 10;
static int open_fd = -1;

void pf_psram_read(uint32_t addr, void *buf, size_t len) {
    if (addr + len > SIM_PSRAM_SIZE) {
        printf("PSRAM read out of range: %u\n", addr);
        abort();
    }
    memcpy(buf, &sim_psram[addr], len);
}

void pf_psram_write(uint32_t addr, const void *buf, size_t len) {
    if (addr + len > SIM_PSRAM_SIZE) {
        printf("PSRAM write out of range: %u\n", addr);
        abort();
    }
    memcpy(&sim_psram[addr], buf, len);
}

bool sim_page_ready(uint32_t page) {
    return (page_ready[page / 8] >> (page % 8)) & 1U;
}

static void mark_ready(uint32_t page) {
    page_ready[page / 8] |= (uint8_t)(1U << (page % 8));
}

void pf_pages_reset(void) {
    memset(page_ready, 0, sizeof(page_ready));
}

void pf_pages_ready(uint32_t first, uint32_t count) {
    for (uint32_t page = first; page < first + count; page++)
        mark_ready(page);
}

bool pf_page_ready(uint32_t page) {
    return sim_page_ready(page);
}

void pf_page_fill(uint32_t page, const void *buf) {
    if (!sim_page_ready(page)) {
        memcpy(&sim_psram[page * PAGE_SIZE], buf, PAGE_SIZE);
        mark_ready(page);
    }
}

int pf_page_wanted(void) {
    return (wanted_page >= 0 && !sim_page_ready((uint32_t)wanted_page)) ? wanted_page : -1;
}

uint64_t pf_time_us(void) {
    sim_time += sim_time_step;
    return sim_time;
}

void sim_time_advance_ms(uint32_t ms) {
    sim_time += (uint64_t)ms * 1000U;
}

void pf_write_snapshot(uint32_t *generation, bool *pending) {
    *generation = sim_generation;
    *pending = sim_pending;
}

void pf_log_console_polls(void) {
}

bool pf_console_quiet(void) {
    return !sim_console_busy;
}

bool pf_sd_available(void) {
    return true;
}

void con_touch_dirty(uint32_t page) {
    sim_generation++;
    ps2_folder_card_note_dirty(page);
}

static int page_erase(mcfat_cardspecs_t *info, uint32_t page) {
    (void)info;
    memset(&sim_psram[page * PAGE_SIZE], 0xFF, PAGE_SIZE);
    mark_ready(page);
    con_touch_dirty(page);
    return sceMcResSucceed;
}

static int page_write(mcfat_cardspecs_t *info, uint32_t page, void *buf) {
    (void)info;
    memcpy(&sim_psram[page * PAGE_SIZE], buf, PAGE_SIZE);
    mark_ready(page);
    con_touch_dirty(page);
    return sceMcResSucceed;
}

/* Core 1 spins here while core 0 gets on with it. */
static void wait_for_page(uint32_t page) {
    uint32_t step = sim_time_step;

    if (sim_page_ready(page))
        return;

    /* A slice is otherwise so long on this clock that it finishes a small card. */
    sim_time_step = 100U * 1000U;
    sim_pages_waited++;
    wanted_page = (int)page;
    for (int i = 0; i < 2000000 && !sim_page_ready(page); i++) {
        if (ps2_folder_card_status() == PS2_FOLDER_ERROR) {
            mark_ready(page);
            break;
        }
        ps2_folder_card_task();
    }
    wanted_page = -1;
    sim_time_step = step;
    if (!sim_page_ready(page)) {
        printf("Console stuck waiting for page %u\n", page);
        abort();
    }
}

static int page_read(mcfat_cardspecs_t *info, uint32_t page, uint32_t count, void *buf) {
    (void)info;
    for (uint32_t p = page; p * PAGE_SIZE < page * PAGE_SIZE + count; p++)
        wait_for_page(p);
    memcpy(buf, &sim_psram[page * PAGE_SIZE], count);
    return sceMcResSucceed;
}

void con_read_page(uint32_t page, void *buf) {
    page_read(NULL, page, PAGE_SIZE, buf);
}

static int ecc_write(mcfat_cardspecs_t *info, uint32_t page, void *buf) {
    (void)info;
    (void)page;
    (void)buf;
    return sceMcResSucceed;
}

static int ecc_read(mcfat_cardspecs_t *info, uint32_t page, uint32_t count, void *buf) {
    (void)info;
    (void)page;
    memset(buf, 0, count);
    return sceMcResSucceed;
}

void sim_console_reset(void) {
    memset(sim_psram, 0xFF, sizeof(sim_psram));
    memset(page_ready, 0xFF, sizeof(page_ready));
    wanted_page = -1;
    sim_pages_waited = 0;
    sim_generation = 0;
    sim_pending = false;
    sim_console_busy = false;
    open_fd = -1;
}

void con_refresh(void) {
    static const mcfat_mcops_t ops = {page_erase, page_write, page_read, ecc_write, ecc_read};
    mcfat_cardspecs_t specs = {.pagesize = PAGE_SIZE, .blocksize = 16, .cardsize = SIM_PSRAM_SIZE / PAGE_SIZE, .flags = 0x08};

    open_fd = -1;
    mcfat_setConfig(ops, specs);
    mcfat_setCardChanged(true);
    mcio_init();
}

int con_write_open(const char *path, const void *data, int len) {
    char name[256];

    snprintf(name, sizeof(name), "%s", path);
    open_fd = mcio_mcOpen(name, sceMcFileCreateFile | sceMcFileAttrWriteable | sceMcFileAttrReadable | sceMcFile0400);
    if (open_fd < 0)
        return open_fd;
    return mcio_mcWrite(open_fd, (void *)data, len);
}

void con_write_close(void) {
    mcio_mcClose(open_fd);
    open_fd = -1;
}

int con_write(const char *path, const void *data, int len) {
    int r = con_write_open(path, data, len);

    if (open_fd >= 0)
        con_write_close();
    return r;
}

int con_read(const char *path, void *buf, int cap) {
    char name[256];
    int fd, r;

    snprintf(name, sizeof(name), "%s", path);
    fd = mcio_mcOpen(name, sceMcFileAttrReadable);
    if (fd < 0)
        return fd;
    r = mcio_mcRead(fd, buf, cap);
    mcio_mcClose(fd);
    return r;
}

int con_mkdir(const char *path) {
    char name[256];

    snprintf(name, sizeof(name), "%s", path);
    return mcio_mcMkDir(name);
}

int con_remove(const char *path) {
    char name[256];
    int r;

    snprintf(name, sizeof(name), "%s", path);
    r = mcio_mcRemove(name);
    /* mcio's removal can leave a FAT link in its free-cluster hint, after which it
     * reports a full card. Real consoles run mcman, not this port: start it afresh. */
    con_refresh();
    return r;
}

int con_rmdir(const char *path) {
    char name[256];

    snprintf(name, sizeof(name), "%s", path);
    return mcio_mcRmDir(name);
}

int con_format(void) {
    int r = mcio_mcFormat();

    return r;
}

int con_free_clusters(void) {
    int free_kb = 0;

    if (mcio_mcGetAvailableSpace(&free_kb) != 0)
        return -1;
    return free_kb;
}
