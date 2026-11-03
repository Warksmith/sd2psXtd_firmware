#pragma once

#include <setjmp.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* --- simulated SD card --- */

extern bool sim_new_card_folder; /* NewCardBackend=FOLDER */
extern uint8_t sim_ps2_cardsize;
extern int sim_ps2_variant;
extern int sim_mode;
extern long sim_op_count;
extern long sim_crash_at;       /* cut power at this operation number */
extern long sim_fail_at;        /* fail this operation */
extern bool sim_fail_sticky;    /* ...and every one after it */
extern bool sim_open_at_fails;  /* a relative open fails although the file is there */
extern long sim_free_bytes;     /* negative: unlimited */
extern void (*sim_op_hook)(long op);
extern jmp_buf sim_crash_point;
extern uint8_t sim_mtime[8];

typedef void (*sim_walk_cb)(const char *path, bool is_dir, const uint8_t *data, size_t len, void *ctx);

void sim_sd_reset(void);
void sim_sd_clear_faults(void);
void sim_sd_power_cycle(void);
int sim_sd_open_fds(void);
void *sim_sd_snapshot(void);
void sim_sd_restore(void *snapshot);
void sim_sd_snapshot_free(void *snapshot);
void sim_sd_walk(const char *base, sim_walk_cb cb, void *ctx);
void sim_sd_put(const char *path, const void *data, size_t len);
const uint8_t *sim_sd_get(const char *path, size_t *len);
void sim_sd_del(const char *path);
void sim_sd_mkdirs(const char *path);
bool sim_sd_name_ok(const char *name, size_t len);

/* --- simulated PSRAM and console --- */

#define SIM_PSRAM_SIZE (8U * 1024U * 1024U)

extern uint8_t sim_psram[SIM_PSRAM_SIZE];
extern uint32_t sim_generation;
extern bool sim_pending;
extern bool sim_console_busy;
extern uint32_t sim_pages_waited;   /* console reads that had to wait for the fill */

void sim_console_reset(void);
bool sim_page_ready(uint32_t page);
void con_read_page(uint32_t page, void *buf);
void sim_time_advance_ms(uint32_t ms);
extern uint32_t sim_time_step;      /* us per clock read */

/* The console side, through the real mcfat driver. */
void con_refresh(void);
int con_write(const char *path, const void *data, int len);
int con_write_open(const char *path, const void *data, int len);
void con_write_close(void);
int con_read(const char *path, void *buf, int cap);
int con_mkdir(const char *path);
int con_remove(const char *path);
int con_rmdir(const char *path);
int con_format(void);
int con_free_clusters(void);
void con_touch_dirty(uint32_t page);
