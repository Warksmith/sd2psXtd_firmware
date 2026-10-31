#include "ps2_folder_fs.h"

#include <string.h>

#include "ps2_folder_port.h"

/* Page-sized work buffer; these run on the core 0 stack, which is only 2 KB. */
static uint8_t scratch[PFS_PAGE_SIZE];

#define SB_MAGIC            "Sony PS2 Memory Card Format "
#define SB_MAGIC_LEN        28
#define SB_PAGE_LEN         0x028
#define SB_PAGES_PER_CLUST  0x02A
#define SB_PAGES_PER_BLOCK  0x02C
#define SB_CLUSTERS         0x030
#define SB_ALLOC_OFFSET     0x034
#define SB_ALLOC_END        0x038
#define SB_ROOT_CLUSTER     0x03C
#define SB_IFC_LIST         0x050
#define SB_CARD_TYPE        0x150
#define SB_MAX_ALLOC        0x170

#define ENTRY_MODE          0
#define ENTRY_LENGTH        4
#define ENTRY_CREATED       8
#define ENTRY_CLUSTER       16
#define ENTRY_DIR_ENTRY     20
#define ENTRY_MODIFIED      24
#define ENTRY_ATTR          32
#define ENTRY_STASH         36
#define ENTRY_NAME          64
#define ENTRY_USED_BYTES    (ENTRY_NAME + PFS_NAME_LEN)

#define FAT_ENTRIES_PER_PAGE    (PFS_PAGE_SIZE / 4U)
#define FAT_ENTRIES_PER_CLUSTER (PFS_CLUSTER_SIZE / 4U)

static uint32_t rd32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static uint16_t rd16(const uint8_t *p) {
    return (uint16_t)(p[0] | (p[1] << 8));
}

static void wr32(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16);
    p[3] = (uint8_t)(v >> 24);
}

static void wr16(uint8_t *p, uint16_t v) {
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
}

bool pfs_mount(pfs_t *fs) {
    uint8_t *page = scratch;

    pfs_invalidate(fs);

    pf_psram_read(0, page, PFS_PAGE_SIZE);
    if (memcmp(page, SB_MAGIC, SB_MAGIC_LEN) != 0)
        return false;
    if (rd16(&page[SB_PAGE_LEN]) != PFS_PAGE_SIZE
        || rd16(&page[SB_PAGES_PER_CLUST]) != PFS_PAGES_PER_CLUSTER
        || rd16(&page[SB_PAGES_PER_BLOCK]) != 16
        || rd32(&page[SB_CLUSTERS]) != PFS_TOTAL_CLUSTERS
        || page[SB_CARD_TYPE] != 2)
        return false;

    fs->alloc_offset = rd32(&page[SB_ALLOC_OFFSET]);
    fs->alloc_end = rd32(&page[SB_ALLOC_END]);
    fs->root_cluster = rd32(&page[SB_ROOT_CLUSTER]);
    fs->max_alloc = rd32(&page[SB_MAX_ALLOC]);

    if (fs->alloc_offset >= PFS_TOTAL_CLUSTERS
        || fs->alloc_end == 0
        || fs->alloc_end > PFS_TOTAL_CLUSTERS - fs->alloc_offset
        || fs->root_cluster >= fs->alloc_end)
        return false;
    if (fs->max_alloc == 0 || fs->max_alloc > fs->alloc_end)
        fs->max_alloc = fs->alloc_end;

    /* An 8 MB card has one indirect FAT cluster, listing 32 FAT clusters that all fit
     * in its first page. */
    uint32_t ifc = rd32(&page[SB_IFC_LIST]);
    if (ifc >= PFS_TOTAL_CLUSTERS)
        return false;

    pf_psram_read(ifc * PFS_CLUSTER_SIZE, page, PFS_PAGE_SIZE);
    for (uint32_t i = 0; i < PFS_FAT_CLUSTERS; i++) {
        uint32_t cluster = rd32(&page[i * 4]);
        if (cluster >= PFS_TOTAL_CLUSTERS)
            return false;
        fs->fat_clusters[i] = (uint16_t)cluster;
    }

    return true;
}

void pfs_invalidate(pfs_t *fs) {
    fs->cache_valid = false;
    fs->cache_dirty = false;
}

bool pfs_cluster_valid(const pfs_t *fs, uint32_t cluster) {
    return cluster < fs->alloc_end;
}

uint32_t pfs_page_addr(const pfs_t *fs, uint32_t cluster, uint32_t page) {
    return (fs->alloc_offset + cluster) * PFS_CLUSTER_SIZE + page * PFS_PAGE_SIZE;
}

void pfs_fat_flush(pfs_t *fs) {
    if (fs->cache_valid && fs->cache_dirty) {
        pf_psram_write(fs->cache_addr, fs->cache, sizeof(fs->cache));
        fs->cache_dirty = false;
    }
}

static uint8_t *fat_slot(pfs_t *fs, uint32_t cluster) {
    uint32_t fat_cluster = cluster / FAT_ENTRIES_PER_CLUSTER;
    uint32_t in_cluster = cluster % FAT_ENTRIES_PER_CLUSTER;

    if (fat_cluster >= PFS_FAT_CLUSTERS)
        return NULL;

    uint32_t addr = fs->fat_clusters[fat_cluster] * PFS_CLUSTER_SIZE
                    + (in_cluster / FAT_ENTRIES_PER_PAGE) * PFS_PAGE_SIZE;

    if (!fs->cache_valid || fs->cache_addr != addr) {
        pfs_fat_flush(fs);
        pf_psram_read(addr, fs->cache, sizeof(fs->cache));
        fs->cache_addr = addr;
        fs->cache_valid = true;
    }

    return &fs->cache[(in_cluster % FAT_ENTRIES_PER_PAGE) * 4];
}

bool pfs_fat_get(pfs_t *fs, uint32_t cluster, uint32_t *value) {
    uint8_t *slot = fat_slot(fs, cluster);

    if (!slot)
        return false;
    *value = rd32(slot);
    return true;
}

bool pfs_fat_set(pfs_t *fs, uint32_t cluster, uint32_t value) {
    uint8_t *slot = fat_slot(fs, cluster);

    if (!slot)
        return false;
    wr32(slot, value);
    fs->cache_dirty = true;
    return true;
}

bool pfs_fat_next(pfs_t *fs, uint32_t cluster, uint32_t *next) {
    uint32_t value;

    if (!pfs_fat_get(fs, cluster, &value))
        return false;

    if (value == PFS_FAT_END) {
        *next = PFS_NO_CLUSTER;
        return true;
    }
    if ((value & PFS_FAT_USED) == 0)
        return false; /* free cluster inside a chain */

    *next = value & ~PFS_FAT_USED;
    return pfs_cluster_valid(fs, *next);
}

void pfs_entry_read(const pfs_t *fs, uint32_t cluster, uint32_t slot, pfs_entry_t *entry) {
    uint8_t raw[ENTRY_USED_BYTES];

    pf_psram_read(pfs_page_addr(fs, cluster, slot), raw, sizeof(raw));

    entry->mode = rd16(&raw[ENTRY_MODE]);
    entry->length = rd32(&raw[ENTRY_LENGTH]);
    memcpy(entry->created, &raw[ENTRY_CREATED], sizeof(entry->created));
    entry->cluster = rd32(&raw[ENTRY_CLUSTER]);
    entry->dir_entry = rd32(&raw[ENTRY_DIR_ENTRY]);
    memcpy(entry->modified, &raw[ENTRY_MODIFIED], sizeof(entry->modified));
    entry->attr = rd32(&raw[ENTRY_ATTR]);
    entry->stash = rd32(&raw[ENTRY_STASH]);
    memcpy(entry->name, &raw[ENTRY_NAME], sizeof(entry->name));
}

void pfs_entry_write(const pfs_t *fs, uint32_t cluster, uint32_t slot, const pfs_entry_t *entry) {
    uint8_t *raw = scratch;

    memset(raw, 0, PFS_PAGE_SIZE);
    wr16(&raw[ENTRY_MODE], entry->mode);
    wr32(&raw[ENTRY_LENGTH], entry->length);
    memcpy(&raw[ENTRY_CREATED], entry->created, sizeof(entry->created));
    wr32(&raw[ENTRY_CLUSTER], entry->cluster);
    wr32(&raw[ENTRY_DIR_ENTRY], entry->dir_entry);
    memcpy(&raw[ENTRY_MODIFIED], entry->modified, sizeof(entry->modified));
    wr32(&raw[ENTRY_ATTR], entry->attr);
    wr32(&raw[ENTRY_STASH], entry->stash);
    memcpy(&raw[ENTRY_NAME], entry->name, sizeof(entry->name));

    pf_psram_write(pfs_page_addr(fs, cluster, slot), raw, PFS_PAGE_SIZE);
}
