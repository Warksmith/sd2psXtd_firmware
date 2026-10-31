#pragma once

/* Everything the folder-card engine needs from the platform. Firmware builds use
 * ps2_folder_port_pico.c; the host test harness provides a simulated PSRAM. */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Bounded PSRAM transfers (at most one page) that are safe against console DMA. */
void pf_psram_read(uint32_t addr, void *buf, size_t len);
void pf_psram_write(uint32_t addr, const void *buf, size_t len);

/* Which card pages the console can read. Until it wants one that is not ready, a
 * console write is the only other thing that makes a page ready. */
void pf_pages_reset(void);
void pf_pages_ready(uint32_t first, uint32_t count);
bool pf_page_ready(uint32_t page);
/* Stores a page, unless the console has written it in the meantime. */
void pf_page_fill(uint32_t page, const void *buf);
/* The page the console is waiting for, or -1. */
int pf_page_wanted(void);

uint64_t pf_time_us(void);

/* Consistent view of the console write counter and of writes not yet drained
 * into the folder-card change tracking. */
void pf_write_snapshot(uint32_t *generation, bool *pending);

/* False while the console is actively using the card. */
bool pf_console_quiet(void);
/* Puts what the console has done so far in the log, to compare with the mount's timing. */
void pf_log_console_polls(void);

/* False while an MMCEMAN file operation owns the SD interface. */
bool pf_sd_available(void);

#ifdef PS2_FOLDER_HOST_TEST
#include <stdio.h>
#include <stdlib.h>
#define PF_LOG(fmt, ...) do { if (getenv("PF_VERBOSE")) printf("[FOLDER] " fmt "\n", ##__VA_ARGS__); } while (0)
#else
#include "debug.h"
#define PF_LOG(fmt, x...) DPRINTF("[FOLDER] " fmt "\n", ##x)
#endif
