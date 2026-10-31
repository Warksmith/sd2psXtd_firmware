#pragma once

/* Minimal, self-contained access to the on-card PS2 filesystem.
 *
 * ext/mcfat is deliberately not used here: it keeps global state shared with the
 * history tracker, caches clusters across console writes and performs none of the
 * chain validation that reconciliation depends on. */

#include <stdbool.h>
#include <stdint.h>

#define PFS_CARD_SIZE           (8U * 1024U * 1024U)
#define PFS_PAGE_SIZE           512U
#define PFS_CLUSTER_SIZE        1024U
#define PFS_PAGES_PER_CLUSTER   2U
#define PFS_ENTRIES_PER_CLUSTER 2U
#define PFS_TOTAL_CLUSTERS      (PFS_CARD_SIZE / PFS_CLUSTER_SIZE)
#define PFS_FAT_CLUSTERS        32U
#define PFS_NAME_LEN            32U

#define PFS_FAT_FREE            0x7FFFFFFFU
#define PFS_FAT_END             0xFFFFFFFFU
#define PFS_FAT_USED            0x80000000U
#define PFS_NO_CLUSTER          0xFFFFFFFFU

#define PFS_MODE_READ           0x0001U
#define PFS_MODE_WRITE          0x0002U
#define PFS_MODE_EXEC           0x0004U
#define PFS_MODE_FILE           0x0010U
#define PFS_MODE_DIR            0x0020U
#define PFS_MODE_CLOSED         0x0080U
#define PFS_MODE_0400           0x0400U
#define PFS_MODE_HIDDEN         0x2000U
#define PFS_MODE_EXISTS         0x8000U
#define PFS_MODE_DEFAULT_FILE   0x8497U
#define PFS_MODE_DEFAULT_DIR    0x8427U
#define PFS_MODE_ROOT_DOTDOT    0xA426U

typedef struct {
    uint16_t mode;
    uint32_t length;
    uint8_t created[8];
    uint32_t cluster;
    uint32_t dir_entry;
    uint8_t modified[8];
    uint32_t attr;
    uint32_t stash;     /* importer scratch, zero on any finished card */
    char name[PFS_NAME_LEN];
} pfs_entry_t;

typedef struct {
    uint32_t alloc_offset;
    uint32_t alloc_end;
    uint32_t max_alloc;
    uint32_t root_cluster;
    uint16_t fat_clusters[PFS_FAT_CLUSTERS];

    uint32_t cache_addr;
    bool cache_valid;
    bool cache_dirty;
    uint8_t cache[PFS_PAGE_SIZE];
} pfs_t;

/* Parse and validate the superblock. All layout values come from the card, since a
 * console format uses a different layout from the firmware's own formatter. */
bool pfs_mount(pfs_t *fs);

/* Drop the FAT cache; must be called before reading after console activity. */
void pfs_invalidate(pfs_t *fs);

bool pfs_cluster_valid(const pfs_t *fs, uint32_t cluster);
uint32_t pfs_page_addr(const pfs_t *fs, uint32_t cluster, uint32_t page);

bool pfs_fat_get(pfs_t *fs, uint32_t cluster, uint32_t *value);
bool pfs_fat_set(pfs_t *fs, uint32_t cluster, uint32_t value);
void pfs_fat_flush(pfs_t *fs);

/* Follow one link. Returns false on a corrupt link; *next is PFS_NO_CLUSTER at the
 * end of the chain. */
bool pfs_fat_next(pfs_t *fs, uint32_t cluster, uint32_t *next);

void pfs_entry_read(const pfs_t *fs, uint32_t cluster, uint32_t slot, pfs_entry_t *entry);
void pfs_entry_write(const pfs_t *fs, uint32_t cluster, uint32_t slot, const pfs_entry_t *entry);
