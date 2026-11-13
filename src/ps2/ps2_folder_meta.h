#pragma once

/* On-SD metadata of a folder card. See doc/folder-cards.md for the format.
 *
 *   <card>.folder/.ps2meta/index.bin    PS2 metadata of the last committed state
 *   <card>.folder/.ps2meta/journal.bin  redo log of a transaction being applied
 *   <card>.folder/.ps2meta/stage/       replacement content for that transaction
 *   <card>.folder/.ps2meta/trash/       saves removed by a console format
 *   <card>.folder/.ps2meta/snapshot.mcd copy of the card, with FolderSnapshot=ON
 *   <card>.folder/.ps2meta/snapshot.ok  says what that copy is and that it is complete
 *   <card>.folder/.ps2meta/tree.ok      hash of the folder that goes with index.bin
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "ps2_folder_fs.h"

#define PFM_DIR             ".ps2meta"
#define PFM_INDEX           PFM_DIR "/index.bin"
#define PFM_JOURNAL         PFM_DIR "/journal.bin"
#define PFM_STAGE_DIR       PFM_DIR "/stage"
#define PFM_STAGE_INDEX     PFM_STAGE_DIR "/index.bin"
#define PFM_TRASH_DIR       PFM_DIR "/trash"
#define PFM_SNAPSHOT        PFM_DIR "/snapshot.mcd"
#define PFM_SNAPSHOT_STAMP  PFM_DIR "/snapshot.ok"
#define PFM_TREE_STAMP      PFM_DIR "/tree.ok"

#define PFM_PATH_MAX        512
#define PFM_MAX_DEPTH       8
#define PFM_NONE            0xFFFFU

#define PFM_FLAG_STRICT     0x0001U     /* the host name uses the strict encoding */

/* Record 0 is the root; the children of a directory are contiguous. */
typedef struct __attribute__((packed)) {
    char name[PFS_NAME_LEN];
    uint8_t created[8];
    uint8_t modified[8];
    uint32_t attr;
    uint32_t size;          /* bytes, or the on-card entry count of a directory */
    uint32_t crc;           /* CRC-32 of the file contents */
    uint32_t chain;         /* hash of the cluster chain, detects re-pointed files */
    uint32_t cluster;
    uint16_t parent;
    uint16_t slot;
    uint16_t mode;
    uint16_t first_child;
    uint16_t child_count;
    uint16_t flags;
} pfm_record_t;

_Static_assert(sizeof(pfm_record_t) == 80, "pfm_record_t is an on-disk format");

uint32_t pfm_crc32(uint32_t crc, const void *buf, size_t len);

/* fds registered here survive MMCEMAN_FS_RESET, which closes everything it does not
 * know about. */
int pfm_open(const char *path, int oflag);
/* Opens name inside an open directory without walking the whole path again. Fails
 * for a missing file and for an SD error alike, and moves any directory iteration. */
int pfm_open_at(int dir_fd, const char *name, int oflag);
void pfm_close(int *fd);
/* Also reports whether everything written reached the card. */
bool pfm_close_checked(int *fd);
void pfm_iter_next(int dir_fd, int *it);
bool pfm_owns_fd(int fd);
#ifdef PS2_FOLDER_HOST_TEST
void pfm_forget_fds(void);
#endif

bool pfm_join(char *out, size_t size, const char *base, const char *rel);

/* Index file: header, records, trailer. */
int pfm_index_create(const char *path);
bool pfm_index_read(int fd, uint32_t idx, pfm_record_t *rec);
bool pfm_index_write(int fd, uint32_t idx, const pfm_record_t *rec);
/* CRC is computed here because directory records are updated while building. */
bool pfm_index_seal(int fd, uint32_t count, uint32_t *crc_out);
/* Returns the record count and CRC of an intact index, or false. */
bool pfm_index_validate(int fd, uint32_t *count, uint32_t *crc);

/* Card-root-relative host path of a record, by walking its parents. */
bool pfm_record_path(int fd, uint32_t idx, char *out, size_t size);
/* Same, for a child of record `parent` that is not stored in this index. */
bool pfm_child_path(int fd, uint32_t parent, const char *child, char *out, size_t size);

typedef enum {
    PFJ_WRITE = 1,      /* replace path with stage/<id>.bin */
    PFJ_MKDIR = 2,
    PFJ_REMOVE = 3,
    PFJ_RMDIR = 4,
    PFJ_TRASH = 5,      /* move path to trash/<id>/ */
} pfj_op_t;

#define PFJ_STAGE_INDEX 0xFFFFFFFFU

typedef struct {
    int fd;
    uint32_t crc;
    uint32_t ops;
} pfj_writer_t;

typedef struct {
    pfj_op_t type;
    uint32_t id;
    char path[PFM_PATH_MAX];
} pfj_entry_t;

bool pfj_create(pfj_writer_t *j, const char *path);
bool pfj_add(pfj_writer_t *j, pfj_op_t type, uint32_t id, const char *rel_path);
/* The commit point: once this returns true the transaction is redone on recovery. */
bool pfj_commit(pfj_writer_t *j);
void pfj_abandon(pfj_writer_t *j, const char *path);

/* True only for a complete journal with a matching CRC. */
bool pfj_validate(const char *path);
/* Opens a validated journal, positioned at its first op. */
int pfj_open_ops(const char *path);
bool pfj_next(int fd, pfj_entry_t *entry);
