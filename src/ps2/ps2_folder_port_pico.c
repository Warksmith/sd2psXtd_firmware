#include "ps2_folder_port.h"

#include "card_emu/ps2_memory_card.h"
#include "hardware/timer.h"
#include "mmceman/ps2_mmceman_fs.h"
#include "ps2_cardman.h"
#include "ps2_dirty.h"
#include "psram/psram.h"

void pf_psram_read(uint32_t addr, void *buf, size_t len) {
    ps2_dirty_lock();
    psram_read_dma(addr, buf, len, NULL);
    psram_wait_for_dma();
    ps2_dirty_unlock();
}

void pf_psram_write(uint32_t addr, const void *buf, size_t len) {
    ps2_dirty_lock();
    psram_write_dma(addr, (void *)buf, len, NULL);
    psram_wait_for_dma();
    ps2_dirty_unlock();
}

void pf_pages_reset(void) {
    ps2_dirty_lock();
    ps2_cardman_forget_sectors();
    ps2_dirty_unlock();
}

void pf_pages_ready(uint32_t first, uint32_t count) {
    ps2_dirty_lock();
    for (uint32_t page = first; page < first + count; page++)
        ps2_cardman_mark_sector_available((int)page);
    ps2_dirty_unlock();
}

bool pf_page_ready(uint32_t page) {
    return ps2_cardman_is_sector_available((int)page);
}

void pf_page_fill(uint32_t page, const void *buf) {
    ps2_dirty_lock();
    if (!ps2_cardman_is_sector_available((int)page)) {
        psram_write_dma(page * 512U, (void *)buf, 512U, NULL);
        psram_wait_for_dma();
        ps2_cardman_mark_sector_available((int)page);
    }
    ps2_dirty_unlock();
}

int pf_page_wanted(void) {
    return ps2_cardman_get_priority_sector();
}

uint64_t pf_time_us(void) {
    return time_us_64();
}

void pf_write_snapshot(uint32_t *generation, bool *pending) {
    ps2_dirty_lock();
    *generation = ps2_dirty_generation;
    *pending = ps2_dirty_pending() != 0;
    ps2_dirty_unlock();
}

void pf_log_console_polls(void) {
    uint32_t first_ms, ignored;

    ps2_memory_card_polls(&first_ms, &ignored);
    if (first_ms == 0) {
        PF_LOG("Up for %lu ms, no console poll yet", (unsigned long)(time_us_32() / 1000U));
    } else {
        PF_LOG("Up for %lu ms, first console poll at %lu ms, %lu ignored", (unsigned long)(time_us_32() / 1000U),
               (unsigned long)first_ms, (unsigned long)ignored);
    }
}

bool pf_console_quiet(void) {
    return ps2_dirty_lockout_expired();
}

bool pf_sd_available(void) {
    return ps2_mmceman_fs_idle();
}
