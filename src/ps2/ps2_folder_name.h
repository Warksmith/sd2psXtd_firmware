#pragma once

/* Reversible mapping between raw PS2 names and host (FAT/exFAT) file names.
 *
 * Decoding is a pure function, so a host tree never needs metadata to recover a
 * PS2 name. Encoding escapes anything a host cannot store as %XX (upper-case hex):
 *
 *  - bytes outside 0x20..0x7E and any of  " * / : < > ? \ | %
 *  - a trailing '.' or ' '
 *  - the first byte of names with a meaning to the host (CON, .ps2meta, ...)
 *
 * The "strict" form additionally escapes lower-case letters. It is used only where
 * two siblings differ by case alone, which a host could not otherwise tell apart. */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "ps2_folder_fs.h"

#define PFN_HOST_NAME_MAX (PFS_NAME_LEN * 3 + 1)

/* Fills out with a NUL-terminated name of at most PFN_HOST_NAME_MAX bytes. */
void pfn_encode(const char raw[PFS_NAME_LEN], bool strict, char out[PFN_HOST_NAME_MAX]);

/* Returns false for names that cannot be a PS2 entry, and for spellings encoding
 * would not produce: such a file could not be written back under the same name. */
bool pfn_decode(const char *host, char raw[PFS_NAME_LEN], bool *strict);

/* True for names that are valid as a stored PS2 entry. */
bool pfn_valid_raw(const char raw[PFS_NAME_LEN]);

bool pfn_has_lower(const char raw[PFS_NAME_LEN]);
uint32_t pfn_fold_hash(const char raw[PFS_NAME_LEN]);
bool pfn_fold_equal(const char a[PFS_NAME_LEN], const char b[PFS_NAME_LEN]);

/* Host-created clutter that is never imported into the card. */
bool pfn_host_junk(const char *host);
