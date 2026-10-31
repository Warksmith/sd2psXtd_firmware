# Folder-backed PS2 memory cards

**Status: experimental. Tested on a host simulator and in first use on a PSXMemCard Gen2, where a card mounted, saves loaded and one game save reached the SD card. A `FolderSnapshot` boot card has also started FreeMcBoot from power-on and passed its content check. A boot card without a snapshot has started FreeMcBoot from a cold boot once, with `.ps2meta` deleted first, after an earlier version failed to. That has not been repeated, and building from `index.bin` has not been confirmed on a console. Power loss and most of the checklist are untested on hardware.**
Work through the [hardware validation checklist](#hardware-validation-checklist) before trusting it with saves you care about.

A folder card keeps its saves as ordinary files on the SD card instead of inside one `.mcd` image. While the card is mounted, the firmware keeps an 8 MB PS2 filesystem in PSRAM and the console uses it like any other card, with the stock driver and no homebrew or patches. Changes are written back to the files in the background.

Folder cards need PSRAM. On the BitFunX PSXMemCard Gen2, use the `SD2PSX` firmware build.

## Using folder cards

### Enabling

Folder mode is opt-in per card. A directory named `*.folder` never turns it on by itself.

Add this to the card's ini file:

```ini
[Settings]
StorageBackend=FOLDER
```

| Card | Ini file | Folder used for channel 1 |
|------|----------|---------------------------|
| Numbered | `MemoryCards/PS2/Card1/Card1.ini` | `MemoryCards/PS2/Card1/Card1-1.folder/` |
| Boot | `MemoryCards/PS2/BOOT/BootCard.ini` | `MemoryCards/PS2/BOOT/BootCard-1.folder/` |
| Game ID or named | `MemoryCards/PS2/SLUS-12345/SLUS-12345.ini` | `MemoryCards/PS2/SLUS-12345/SLUS-12345-1.folder/` |

- The ini name must match the table exactly. The boot card reads `BootCard.ini`, not `Boot.ini` or `BootCard-1.ini`. Without `StorageBackend` in a matching ini, what is on the SD card decides, as described under `NewCardBackend` below.
- The setting applies to every channel of the card, and each channel has its own folder.
- Accepted values are `IMAGE` (the default) and `FOLDER`, in either case. Unknown values are ignored, as if the line were not there.
- On a firmware build without PSRAM, `FOLDER` is ignored and the image is used.
- A folder card is always 8 MB. If the ini also sets a `CardSize` other than `8`, the card refuses to mount.
- The folder is created if it does not exist.

Existing `.mcd` files are never read, changed, converted or deleted by folder mode. A folder and an image can sit side by side, and removing the setting brings the image back if there is one. To move saves from an image to a folder, copy them with a save manager on the console.

### New cards: `NewCardBackend`

To have cards that do not exist yet start as folder cards, including the ones Game ID creates, set this in `.sd2psx/settings.ini`:

```ini
[PS2]
NewCardBackend=FOLDER
```

A card with no `StorageBackend` in its ini is then decided per channel, in this order:

1. If it has a `.mcd`, it stays an image, so its saves do not vanish behind a blank folder.
2. If it has a `.folder`, it stays a folder. This holds with the setting off too, so turning it off does not hide saves.
3. If it has neither, it is created as a folder when `NewCardBackend=FOLDER`, and as an image otherwise.

Step 3 still creates an image for the boot card, which has to answer at power-on; for a card whose `CardSize`, or the global `CardSize` if it has none, is not 8; and for variants other than `RETAIL`. The value is `IMAGE` (the default) or `FOLDER`, in either case. It does not turn on `FolderSnapshot`.

**A Game ID card is switched to just as the game starts, and is hidden until its directories are built.** This has not been tried on hardware, and a game that looks for its card early may not find it.

### How a card loads

A card loads in two steps:

1. **Directories.** The firmware builds the card's directories and FAT from the names and sizes alone. The console sees no card yet. The display shows `Loading folder..`. If the folder is unchanged since the last time, the names and sizes come from `index.bin` and the folder is only compared with `tree.ok`. Otherwise every file is opened, which is slower. A card mounted for the first time, or after a change from a computer, always goes the slow way. So does a card whose `tree.ok` was never written, which happens the first time it sits idle or is unmounted.
2. **Files.** The card is now visible, and the file contents are read from the SD card behind it. What the console asks for goes first, so a file that has not been read yet comes off the SD card as the console reads it. The display shows `Reading files...`.

On a PSXMemCard Gen2, with a boot card holding FreeMcBoot:

- When this work waited for the display to start, **FreeMcBoot did not start.** The card became visible 972 ms after its mount began, and the console asked for no file data during the 5.8 seconds of loading, so it had probably stopped looking for the card by then.
- With this work moved to before the display starts, **FreeMcBoot started** from one cold boot, with `.ps2meta` deleted first, so the slow way above. On the same card without a console attached, the card was ready 655 ms after its mount began and 1,361 ms after the debug firmware started.

That is one success, and how long a console waits for its card is still not known. Whether games cope with the slower first read of each file is also unknown. `FolderSnapshot` below is the more tested route.

While `Reading files...` shows:

- The console can read and write as usual, but nothing it writes is saved until all files are read. The display shows `Unsaved changes` in that case.
- **If a file cannot be read from the SD card, the card disappears and shows `SD error`, and anything the console wrote since the card appeared is dropped.** Nothing is changed in the folder.
- Switching cards goes ahead at once if nothing was written. If something was, the rest of the files are read first.

A file edited on a computer takes the file's date on the card if its size changed. If the size stayed the same, its new content is used, but the card keeps showing the old date.

### Ready at power-on: `FolderSnapshot`

Building the directories means walking the whole folder, and that can still be too late for a console looking for its boot card. To have a card available straight away, and without reading files on demand through the file system, turn on its snapshot:

```ini
[Settings]
StorageBackend=FOLDER
FolderSnapshot=ON
```

The firmware then keeps an 8 MB copy of the card in `.ps2meta/snapshot.mcd`. If nothing changed since that copy was made, the next mount serves it the way an image is served: the card is visible at once, and whatever the console asks for first is read on demand while the rest loads. The folder stays the master copy. The snapshot is only a cache and is never needed to get your saves back.

- It is off by default and set per card, with `ON` or `OFF` in either case. Unknown values leave it off.
- The first mount still loads from the folder, and the copy is then written in the background once all files are read. **Let `Caching card...` finish once** before relying on it: a card switch or power-off before then means the next mount loads from the folder again.
- After every save the copy is updated in the background, which needs the console to stay quiet for a moment. If the power goes before it finishes, the next mount loads from the folder. Nothing is lost.
- Switching cards finishes the update if it is small. If it is not, the switch goes ahead and that card's next mount loads from the folder.
- Adding, deleting or resizing files on a computer is noticed from the file names, sizes and dates, and the next mount loads from the folder.
- The console may start writing before the snapshot's contents have been checked, which takes a few seconds after loading. If the check finds it damaged, the card briefly disappears, comes back built from the folder, and **anything written since power-on is dropped**. A damaged snapshot is never saved over the folder.
- `snapshot.mcd` is an ordinary 8 MB image. While `snapshot.ok` exists it is a complete copy of the card, and you can copy it elsewhere. Without `snapshot.ok` it may be half-written. **Changing it in place does not change the card:** the check fails and the card is rebuilt from the folder.

What it costs:

- 8 MB of SD space per channel that uses it, and more SD writes, because every save is also written to the copy. If the SD card has no room, the snapshot is removed and the card keeps working without it.
- **Replacing a file with one of the same size and the same date is not noticed.** This applies only to a mount that uses the snapshot. The device has no clock, so a rewritten file keeps its old date, and restoring a save from a backup can leave size and date exactly as they were. The card then keeps showing the old content, and the restored file is overwritten the next time the console saves over it. After restoring a save, delete `.ps2meta/snapshot.ok`.

Turning the setting off leaves the two files in place, unused. Delete `snapshot.mcd` and `snapshot.ok` to get the space back.

### What the folder looks like

```text
Card1-1.folder/
  BASLUS-20001/
    icon.sys
    BASLUS-20001
  BESCES-50001/
    ...
  .ps2meta/          firmware metadata, leave it alone
                     (safe to delete: snapshot.mcd, snapshot.ok and tree.ok, with the card unmounted)
```

You can add, replace or delete saves from a computer **while the card is not mounted**. The changes appear the next time the card is mounted. An edit made with the card mounted is not seen, and may be overwritten.

`.DS_Store`, `Thumbs.db`, `desktop.ini` and `._*` files are ignored, and they are removed along with a save directory if the console deletes it.

File dates on the SD card do not show when a save last changed. The device has no clock, so a rewritten file keeps its old date and a new file gets a fixed one. The dates the console sets are kept in `.ps2meta/index.bin`.

### Status on the display

The top line of the main screen shows the state of a folder card when there is something to report. While a status is showing, the main screen stays up instead of giving way to the card's image.

The screen with the progress bar says which way the card is loading:

| Text | Meaning |
|------|---------|
| `Loading folder..` | Its directories are being built. The console sees no card yet. |
| `Reading files...` | The console can use it. Its files are still being read from SD. |
| `Snap 1234 kB/s` | Served from its snapshot. The console can already use it. |

The main screen's own texts are:

| Text | Meaning |
|------|---------|
| `Loading card...` | Being rebuilt after a snapshot was rejected. The console sees no card yet. |
| `Reading files...` | Served while its files are read from SD. Saves wait until this is done. |
| `Checking card...` | Served from its snapshot, which is still loading or being checked. Saves wait until this is done. |
| `Caching card...` | Updating the snapshot. Everything is already saved, so it is safe to power off, at the cost of a slower next mount. |
| `Unsaved changes` | Changes are in PSRAM only, waiting for the console to go quiet. |
| `Saving card...` | Writing to SD. |
| `Save failed!` | SD is behind PSRAM. The card keeps working and saving retries every few seconds. |
| `Too big for 8MB` | The folder's content does not fit. The card stays hidden. |
| `Bad file name` | A file has a name the card cannot represent. The card stays hidden. |
| `Too many files` | Over the limits below. The card stays hidden. |
| `SD error` | The SD card failed during mounting or saving. If it was while reading files, the card is hidden. |
| `Card data invalid` | The on-card filesystem failed validation, so nothing is being saved. |
| `Recovery required` | See [Recovering a card](#recovering-a-card). |
| `Needs 8MB card` | The ini sets a different `CardSize`. |

### When your data is safe

PSRAM loses its contents when the power goes. Anything not yet on SD is lost in a power cut.

- A sync starts about 1.5 seconds after the console stops writing, and takes as long as the changed files need. While `Reading files...` or `Checking card...` is in progress it waits for that as well.
- **Wait for `Unsaved changes` and `Saving card...` to clear before turning the console off.** On a build without a display you have no indication, so allow a few seconds, more after a large save.
- Switching cards saves first. If that fails, the switch is refused and retried every 5 seconds, so the unsaved data is not thrown away. Fix the cause, or power-cycle to give up on those changes.
- A power cut during a sync leaves either the old or the new version of the whole sync after the next mount, as far as the simulator can show. See [Power loss](#power-loss).

### Limits

- 8 MB card, about 7.8 MB usable. The firmware's format has 8001 allocatable clusters of 1 KB, and each directory uses some of them.
- 2047 files and directories per card, 1024 entries in one directory.
- 8 directory levels below the root.
- PS2 names are at most 31 bytes.
- The SD card temporarily needs free space for a copy of every changed file.

The console itself can exceed the entry and depth limits. If it does, the card keeps working but shows `Save failed!` until the extra entries are removed.

### Things that do not work

- **Cross-linked files**, as used by multi-version FreeMcBoot installs. Two files sharing clusters cannot be turned into separate host files, so the card reports `Card data invalid`, nothing is saved, and card switching is blocked until a power cycle. Use an image card for these.
- **Booting from the card right after power-on has only been tried repeatedly with `FolderSnapshot=ON`** and a current snapshot. Without one it has worked once and failed once, on different firmware. An 8 MB image card is visible while it loads, which is what lets FreeMcBoot or PS2BBL start at power-on. A folder card without a usable snapshot is hidden until its directories are built, so a console that reads the card earlier finds no card. Check this on your console with your card contents before relying on it.
- **Renames are expensive.** The console has no rename the firmware can see, so renaming a directory appears as a delete plus a create and rewrites every file in it.
- **Host names the card cannot represent are refused, not renamed.** `100%.dat` and most non-ASCII names stop the card from mounting. Rename the file on the computer. See [Filename encoding](#filename-encoding).

### Recovering a card

- `Bad file name`, `Too big for 8MB`, `Too many files`: fix the folder on a computer and remount. Nothing was changed.
- `Save failed!`: check the SD card for free space and errors. Saving retries by itself.
- `Card data invalid`: something on the console wrote an inconsistent filesystem, or a format is in progress. Nothing is written to SD until it is consistent again. A power cycle returns to the last saved state.
- `Recovery required`: a committed sync cannot be finished because its stage file is missing, which should only follow external damage to `.ps2meta`. Nothing is changed automatically. Back up the folder, then delete `.ps2meta/journal.bin` and remount. Files from that interrupted sync may be a mix of old and new.
- **After a console format**, the old saves are moved to `.ps2meta/trash/<id>/` rather than deleted. To restore them, copy them back into the card folder with the card unmounted. The firmware never empties the trash, so delete it yourself when you no longer need it.

## How it works

```text
           SD card                         PSRAM (8 MB)             console
 ┌────────────────────────┐   import   ┌──────────────────┐ existing ┌───────┐
 │ Card1-1.folder/        │ ─────────► │ PS2 filesystem   │ ◄──────► │ mcman │
 │   saves as files       │            │ (authoritative   │ protocol └───────┘
 │   .ps2meta/            │ ◄───────── │  while mounted)  │
 └────────────────────────┘ reconcile  └──────────────────┘
```

**Authority.** SD is authoritative while unmounted, PSRAM while mounted. There is no merging: external changes made during a mount are not detected.

**Demand loading stops at core 0.** The time-critical protocol code on core 1 only ever deals with PSRAM and is unchanged: for a page that is not ready, it asks core 0 for it and waits, exactly as it does while an image loads. Core 0 does the rest. All SD and folder work runs there, in slices of about 20 ms, or 100 ms while the card is hidden, and only when no MMCEMAN file operation is using the SD card.

### Modules

| File | Purpose |
|------|---------|
| `src/ps2/ps2_folder_card.c` | Lifecycle: mount phases, sync triggering, retries, final flush, status |
| `src/ps2/ps2_folder_import.c` | Builds the PS2 filesystem in PSRAM from the host tree, without file contents |
| `src/ps2/ps2_folder_fill.c` | Loads file contents behind the published card, and finishes its index |
| `src/ps2/ps2_folder_sync.c` | Validating scan, stage, journal, apply and recovery |
| `src/ps2/ps2_folder_snapshot.c` | Writing, validating and checking the optional snapshot |
| `src/ps2/ps2_folder_meta.c` | `index.bin` and `journal.bin` formats, fd tracking |
| `src/ps2/ps2_folder_fs.c` | Superblock, FAT and directory entries in PSRAM |
| `src/ps2/ps2_folder_name.c` | Reversible filename encoding |
| `src/ps2/ps2_folder_port_pico.c` | Platform layer; `test/folder/sim_console.c` replaces it in tests |
| `src/ps2/ps2_card_format.c` | Empty-card generator, moved out of `ps2_cardman.c` unchanged |

Integration points:

- `ps2_cardman.c` picks the backend and hides the card. Hiding happens at the `0x81` identifier, because erase and write commands are not gated by accessibility. It also runs the sector loader over a snapshot, and hides the card again if the snapshot turns out to be unusable or a file cannot be read. It stays busy while files load, as it does while an image loads, because a read on core 0 would wait for itself.
- `ps2_dirty.c` counts console writes in `ps2_dirty_generation` and routes drained pages to the change tracker instead of an image.
- `ps2_mmceman.c` runs the final flush before a switch.
- `ps2_mmceman_fs.c` refuses writes to the mounted card's folder from MMCEMAN clients. Reads stay allowed. `MMCEMAN_FS_RESET` leaves the engine's files open.

The Game ID history tracker still runs, since it reads through the same PSRAM path.

`ext/mcfat` is deliberately not used by the engine. It keeps global state shared with the history tracker and does no chain validation.

### Mounting

Up to publishing, this runs inside the card open, as loading a snapshot does, for at most 2 seconds. The main loop does not start until the display is up, which at power-on is too late.

1. **Recover.** Finish or discard a transaction left by a power cut, then load the base index.
2. **Compare.** If `tree.ok` matches the index, hash the names, sizes and dates in the folder as a snapshot does. If that matches too, the host is not looked at again before the fill. This only decides where the names and sizes come from: the fill reads every file either way, so unlike a snapshot it cannot show old content for a file replaced at the same size and date.
3. **Import.** Write the superblock, FAT and root directory to PSRAM, then add directories depth-first. Entries known to the index come first, in their stored order and with their stored PS2 metadata. New host entries follow with defaults, unless the comparison matched, when the index is all there is. If a file's size differs from the index, it was edited on the host and takes the host's modification time. Each file gets contiguous clusters and a FAT chain from its size, without being read. Every entry is also written to `.ps2meta/stage/index.bin`, in batches, as a record of the new index.
4. **Publish.** Only the pages written so far count as available: the metadata and the directory clusters. Stop hiding the card.
5. **Fill.** Sweep over the records, reading each file from start to end and storing its pages in PSRAM, then erase what is left of the card, since PSRAM still holds the previous card. Before every step, a page the console is waiting for goes first: it finds the file by its first cluster, reads that cluster, and reads up to 32 more ahead. A page the console has written counts as available, and is not loaded over.
6. **Commit.** Add what the sweep learned to the records: the CRC of each file as read from SD, whatever the console has done since, and each directory's children. If the result differs from `index.bin`, replace it with the same journal a sync uses. PSRAM plays no part in this, so the console can keep running. Writes made since publishing are then synchronized against the new index as usual.

A failure during import leaves the card hidden. A failed read during the fill hides it and drops what the console wrote, because PSRAM is not a complete card that could be synchronized. The host files are untouched either way. A power cut before the commit leaves only a stage file, which the next mount removes.

The sweep reads every file even where pages were served already, because the CRC has to be that of the whole file on SD. A wrong CRC in the index would make a later save of the old content look like no change.

### Serving a snapshot

The stamp `snapshot.ok` is what makes `snapshot.mcd` count. It records the index the copy belongs to, a hash of the host tree and the CRC of the whole 8 MB.

**Using it.** After recovery, and blocking for at most 2 seconds so the card can appear before the console looks, the firmware reads the stamp and compares it with the index. It then walks the directories the index knows and hashes every name, plus size and date for files, skipping `.ps2meta` and the ignored names. Anything that does not match, and any error, falls back to the normal mount.

The loaded sectors go through the same on-demand loader as an image, except that a read error falls back to a mount instead of stopping the device. Afterwards the whole file is read again and checked against the stamp's CRC. Nothing is synchronized until that has passed, because a damaged directory looks exactly like saves the console deleted. A failed check removes the stamp and hides the card, which is then built from the folder.

**Writing it.** When the card is clean and the console has been quiet for 1.5 s:

1. Remove the stamp. From here on a power cut leaves no valid snapshot.
2. Pass over all 8192 clusters in order, hashing each and writing those changed since the last pass to `snapshot.mcd`. A file being created grows in the same order, so it never seeks past its end.
3. Close the file, then hash the host tree.
4. If the console has not written since step 1, write the stamp.

Any console write stops the pass. Clusters already copied stay copied, and the pass restarts after the next sync. An SD error or a full card removes the snapshot for the rest of the mount, so it cannot take the space a save needs.

A sync that commits replaces the index, so a stamp that is still there no longer matches it. That covers a power cut between the commit and step 1.

### Syncing

A sync starts when the console has written since the last one, at least 1.5 s have passed since the last write, the dirty queue is empty and the console is quiet.

1. **Scan.** Walk the card breadth-first using the index being written as the queue, so no tree is held in SRAM. Every directory chain, file chain and entry is checked: valid clusters, no cycles, no cluster with two owners, chains long enough for the length, unique names, sane modes.
2. **Stage.** A file is unchanged if it has the same size and chain hash as the index and none of its clusters were written since the last commit. Anything else is copied to `.ps2meta/stage/`, and dropped again if its CRC shows the content is the same.
3. **Plan deletions.** Entries in the index that were not seen become remove operations, children first.
4. **Commit.** If the generation counter moved at any point, the scan is thrown away and retried later. Otherwise the journal trailer is written and the file closed. That write is the commit point.
5. **Apply.** Deletions first, so a case-only rename or a file replaced by a directory cannot collide with itself. Then directories and files. The index is the last operation.
6. **Finish.** Remove the journal, then clean the stage directory.

Rules that keep this safe:

- **Stage files are copied over their targets, never renamed.** SdFat's rename writes the new directory entry before removing the old one, and a cut between the two leaves two entries on one cluster chain.
- **Operations are idempotent**, so an interrupted apply is simply run again.
- **A format is not trusted to mean delete.** If the root is empty but the index was not, only the top-level entries are moved to `.ps2meta/trash/<index crc>/`. Trash moves never overwrite: on a collision, both copies stay and the source remains in the card folder.
- **An invalid filesystem writes nothing.** A partial erase has no valid superblock, so it is not mistaken for a format.
- **A file being written is left alone.** The console clears the Closed bit while a file is open, so sync waits and retries every second. After 30 s it accepts the file anyway, in case the writer crashed. A final flush always accepts it.
- **An empty, unchanged result commits nothing**, so an idle card causes no SD writes.

### Recovery rules

| Found at mount | Action |
|----------------|--------|
| No journal | Remove leftover stage files |
| Journal with a bad CRC or no trailer | Never committed, nothing applied: discard it and the stage |
| Valid journal, stage complete | Redo it |
| Valid journal, stage file missing | `Recovery required`: card hidden, nothing changed |
| Missing or damaged `index.bin` | Mount with default metadata, no files lost |

### Power loss

Tests cut power at every state-changing SD operation of several sync scenarios, of a load with and without console writes, and again during recovery, and always found the complete old or complete new state after the next mount. That result holds **only for the simulator's model** of the SD card:

- Directory operations and truncation are immediately durable.
- File growth is durable after flush or close.
- Overwrites in place survive, even if not flushed.
- Operations happen in the order issued.

Real SD cards have internal write caching and wear levelling, and may not honour that ordering. Real FAT updates are not atomic either: a rename into the trash is modelled as atomic here but is not on a real volume. **No power-loss guarantee is claimed for real hardware.**

Other limits:

- Changes newer than the last commit are always lost in a power cut.
- A power cut while a snapshot is being updated was tested the same way, and only ever cost the shortcut. The same model applies, and in particular a real SD card that reorders the write of the stamp ahead of the writes to `snapshot.mcd` is not modelled. The content check after loading is what would catch that.
- Between a commit and the end of apply, the folder can show a mix of old and new files. Mounting completes it. Editing the folder on a computer in that state can be overwritten by the redo.

### Metadata format

All integers are little-endian. CRCs are CRC-32 (reflected, polynomial `0xEDB88320`). Any change to these layouts must bump the version.

**`.ps2meta/index.bin`**

| Part | Content |
|------|---------|
| Header, 16 B | `PS2FIDX1`, version `u32` = 1, record size `u32` = 80 |
| Records, 80 B each | Record 0 is the root. The children of a directory are contiguous, in on-card order. A mount writes the directories depth-first and a sync breadth-first, and nothing depends on which. |
| Trailer, 16 B | Record count `u32`, CRC of header and records `u32`, `PS2FEND1` |

| Record field | Size | Meaning |
|--------------|------|---------|
| `name` | 32 | PS2 name, NUL padded |
| `created` | 8 | PS2 timestamp |
| `modified` | 8 | PS2 timestamp |
| `attr` | 4 | PS2 attribute word |
| `size` | 4 | Bytes, or on-card entry count for a directory |
| `crc` | 4 | CRC-32 of the file |
| `chain` | 4 | Hash of the cluster chain |
| `cluster` | 4 | First cluster when written; informational |
| `parent` | 2 | Parent record, always lower than this record |
| `slot` | 2 | Reserved |
| `mode` | 2 | PS2 mode bits |
| `first_child` | 2 | First child record of a directory |
| `child_count` | 2 | Number of child records |
| `flags` | 2 | Bit 0: the host name uses strict encoding |

**`.ps2meta/snapshot.ok`**, 28 B

| Field | Size | Meaning |
|-------|------|---------|
| `magic` | 8 | `PS2FSNP1` |
| `index_crc` | 4 | CRC of the `index.bin` this copy belongs to |
| `index_count` | 4 | Records in that index |
| `host_hash` | 4 | Sum over the host tree of a CRC of record number, name, size and date |
| `card_crc` | 4 | CRC-32 of `snapshot.mcd` |
| `crc` | 4 | CRC-32 of the fields above |

`.ps2meta/snapshot.mcd` is the raw 8 MB card, in the same format as an image card.

**`.ps2meta/tree.ok`**, 24 B

| Field | Bytes | Notes |
|---|---|---|
| `magic` | 8 | `PS2FTRE1` |
| `index_crc` | 4 | CRC of the `index.bin` this belongs to |
| `index_count` | 4 | Records in that index |
| `host_hash` | 4 | Names, sizes and dates in the folder, as in `snapshot.ok` |
| `crc` | 4 | CRC-32 of the fields above |

It is written once a card is idle or unmounted, whenever the index has changed. Writing it is not protected by the journal, because a missing or damaged one only costs the next mount a look at every file.

**`.ps2meta/journal.bin`**

| Part | Content |
|------|---------|
| Header, 8 B | `PS2FJRN1` |
| Operation | Type `u8`, reserved `u8`, path length `u16`, id `u32`, then the path |
| Trailer, 16 B | Operation count `u32`, CRC of everything before it `u32`, `PS2FJEND` |

Paths are relative to the card folder.

| Type | Operation | `id` |
|------|-----------|------|
| 1 | Replace path with stage content | Stage file `<id as %08X>.bin`; `0xFFFFFFFF` means `stage/index.bin` |
| 2 | Create directory | |
| 3 | Remove file | |
| 4 | Remove directory | |
| 5 | Move to trash | Trash directory name, as `%08X` |

### Filename encoding

Host names are PS2 names with some bytes written as `%XX` in upper-case hex. The mapping is exact in both directions.

Escaped:

- bytes below `0x20` and from `0x7F` up, including Shift-JIS
- `"` `*` `/` `:` `<` `>` `?` `\` `|` `%`
- a leading space, a trailing space and a trailing dot
- the first character of names a host must not see literally: `CON`, `PRN`, `AUX`, `NUL`, `COM1`-`9` and `LPT1`-`9` with or without an extension, `.ps2meta`, and the ignored names such as `Thumbs.db`

| PS2 name | Host name |
|----------|-----------|
| `icon.sys` | `icon.sys` |
| `CON` | `%43ON` |
| `What?` | `What%3F` |
| `100%` | `100%25` |

PS2 names are case-sensitive and FAT is not. When two entries in a directory differ only by case, the ones with lower-case letters also escape those letters (strict encoding), so `save` next to `SAVE` is stored as `%73%61%76%65`.

The decoder only accepts the spelling the encoder would produce. A host name that decodes to something else, such as `100%.dat` or `%41`, is an error rather than a guess, because guessing could map two host files onto one PS2 name.

### Memory use

Static SRAM on the `SD2PSX` build, measured from the ELF:

| Item | Bytes |
|------|-------|
| Engine context (`pf`) | 21,988 |
| Sync state | 1,392 |
| Import state | about 850 |
| Other buffers | about 2,700 |
| Snapshot change bitmap, and CRC table | 1,024 each |
| Whole firmware before → after | 191,880 → 220,156 of 262,144 |

The largest parts of the context are 8 KB of per-record hashes, 4 KB of name hashes and 4 KB of record mapping. The change bitmaps take 1 KB each. While files load, the record mapping holds first clusters and the hashes hold what the sweep learns, so loading needs no tables of its own. Nothing is allocated dynamically, and builds without PSRAM carry none of this. Large work buffers are static because the core 0 stack is small.

## Developer notes

### Host tests

The tests need only a C compiler. They run the real engine sources under ASan and UBSan. The console side is the real `ext/mcfat` driver, so test filesystems are shaped by a real driver rather than by this project's assumptions.

```bash
test/folder/run.sh
```

```bash
test/folder/run.sh test_power_cut
```

`PF_VERBOSE=1` prints the engine log and `PF_LONG=1` lengthens the randomized sessions. In Docker, using the image built from `misc/docker/Dockerfile`:

```bash
docker run --rm -v "${PWD}:/src" sd2psx-build sh /src/test/folder/run.sh
```

| Area | Coverage |
|------|----------|
| Import | Empty and missing folders, sizes around cluster boundaries, name encoding, capacity and entry limits, depth limit, hidden-until-complete, image coexistence |
| Write-back | Create, overwrite, truncate, extend, in-place patch, delete, directory and case-only rename, attributes and timestamps, names differing by case, file replaced by directory |
| Concurrency | A console write at every SD operation of a sync; a two-file update at every moment of a sync, with a power cut after each commit, so a commit never mixes two moments; a file left open |
| Validation | Bad superblock, directory and file chains, cycles, cross-links, short chains, bad entries, duplicate names, partial erase; an empty file with a stray cluster value is accepted |
| Recovery | Power cut at every operation of six scenarios and of a load with and without console writes, a second cut during recovery, one-off and persistent SD failure at every operation, SD full, damaged journal, index and stage |
| Round trip | Randomized console sessions checked against a model across remounts and power cuts |
| Configuration | `StorageBackend` and `FolderSnapshot` parsing through the real `card_config.c` and SD ini reader, including files with CRLF, a byte order mark or no newline after the last line; which cards follow `NewCardBackend`, and that the ini, an image or a folder overrides it |
| Loading | Directories readable before any file is, files served on demand and read ahead, console writes not loaded over, free space erased over stale PSRAM and on demand, index CRCs correct, an importer index a scan has nothing to add to, a same-size host edit, a read error, unmounting mid-load, no `stash` left on the card, a failed open not taken for a removed file |
| Snapshot | Off by default; served byte for byte; follows saves by rewriting only changed clusters; host edits, stale and damaged stamps, wrong-size files; a damaged copy with console writes pending, during a task and during a flush; power cut at every operation of an update and of a close, and at sampled points of a first write; SD full; console interrupting a pass |

To try a real save folder, point `PF_HOST_FOLDER` at it. The test works on a copy in the simulated SD and does not change the folder. In Docker, mount the folder read-only and pass `-e PF_HOST_FOLDER=/card`.

Breaking safety properties one at a time on a copy of the sources showed the suite notices nine of the ten tried for syncing. The tenth removes the stale-scan check just before the commit. It goes unnoticed because the check before staging finishes still protects the commit, and removing both makes `test_commit_is_a_single_snapshot` fail. The same exercise on the load speedups and the snapshot found 30 of 32. The other two change nothing observable: the stamp's magic is also covered by its CRC, and `.ps2meta` adds only a constant to the host hash.

`test/folder/sim_sd.c` documents the durability model. Its volume rejects names FAT or SdFat would reject. The tests do not cover real SD cards, real console timing, `mcman` behaviour that differs from `mcfat`, the protocol code on core 1, or `ps2_cardman.c`, `ps2_dirty.c`, `settings.c`, GUI and MMCEMAN integration, which only compile. For loading behind the console, the simulator's page read waits for core 0 the way core 1 does, one task slice at a time, which says nothing about how long a real console tolerates. For the snapshot, the tests stand in for `ps2_cardman.c` by copying the file into simulated PSRAM. The sector loader and the check at open have run on hardware on a good snapshot, but the card reappearing after a failed check has not been run anywhere as a whole.

The bundled `mcio` port stores a FAT link in its free-cluster hint when removing a file, and then reports a full card. The firmware only reads through it, so `con_remove()` in the simulator works around this by restarting the driver.

### Building

```bash
docker build -t sd2psx-build misc/docker
```

```bash
docker run --rm -v "${PWD}:/src" -v sd2psx-build:/build sd2psx-build bash /src/misc/docker/build.sh
```

Run both from the repository root. This builds only the PSXMemCard Gen2 firmware: `sd2psx.uf2`, and `sd2psx-debug.uf2` with `[FOLDER]` log lines on USB serial. Both end up in `build/docker/` in the repository, which git ignores. The Docker volume `sd2psx-build` holds only the intermediate files, so later builds are faster. To build another variant, change `VARIANT` and the `cp` line in `misc/docker/build.sh` to match. Other variants name their files differently, for example `psxmemcard.uf2`.

The folder engine is compiled only when `SD2PSX_WITH_PSRAM` is set. The build downloads the game database, so it needs network access.

## Hardware validation checklist

First use on a PSXMemCard Gen2 covered steps 6 and 7 for a 6.5 MB card, without a timing, and part of step 10: one rewritten game save was confirmed on SD by matching its checksum in `.ps2meta/index.bin`. Everything else is still to do, including all of the power-loss steps. Use a spare SD card, build with `-DDEBUG_USB_UART=ON` to get `[FOLDER]` log lines on USB serial, and keep a copy of any saves used.

**Setup**

1. Build `SD2PSX` and flash it to the PSXMemCard Gen2 by holding BOOTSEL and copying the `.uf2`.
2. On the SD card, create `MemoryCards/PS2/Card1/Card1.ini` with `[Settings]` and `StorageBackend=FOLDER`.
3. Copy two real save directories into `MemoryCards/PS2/Card1/Card1-1.folder/`.

**Regression: image cards unchanged**

4. On a card without the setting, confirm an existing `.mcd` loads and saves as before, its checksum changes only after a write, and switching between image cards works.
5. Confirm Game ID switching and history tracking work on image cards.

**Mounting**

6. Boot with the card in place. Confirm `Loading folder..` and then `Reading files...` show and go away, and record how long each takes. The debug log gives the time of each phase.
7. In the browser, confirm both saves appear with correct icons, sizes and dates, and that they load in their games.
8. Confirm `.ps2meta` is not visible on the console.
9. Check free space on the console against the expected value.

**Writing**

10. Save in a game. Confirm `Unsaved changes`, then `Saving card...`, then a normal header, and that the file on SD changed.
11. Copy a save, delete a save and overwrite a save in the browser. After the header clears, compare the SD card to the console's view with the card powered off.
12. Remount by switching cards and back, and confirm everything is still there with the same dates.
13. Format the card in the browser. Confirm the folder empties, the saves are in `.ps2meta/trash/`, and new saves work.

**Switching**

14. Switch cards while `Unsaved changes` is showing. Confirm the data reached SD before the new card appears.
15. Switch channels, and switch to and from the BOOT and a Game ID card. Confirm cards do not leak into each other.
16. Pull the SD card's free space near zero, save, and confirm `Save failed!`, a refused switch, and recovery after freeing space.

**Failure handling**

17. Put a 9 MB file in a folder. Confirm `Too big for 8MB`, a hidden card, and untouched files.
18. Add `100%.dat`. Confirm `Bad file name`.
19. Write a save, then cut console power during `Saving card...`, at least 20 times with a large save. After each, remount and confirm every save is complete and either old or new. **This is the test that decides whether the power-loss claims can be made at all.** Repeat with two different SD card brands.
20. Repeat with the power cut while the save is being written by the game.

**Snapshot**

These need `FolderSnapshot=ON`. Steps 21 and 22 have been tried on a PSXMemCard Gen2, but not the ten power cycles of step 22, and the other steps have not been run on hardware.

21. Mount a card, wait for `Caching card...` to clear, and check `.ps2meta/snapshot.mcd` is 8 MB and `snapshot.ok` exists. Record how long it took.
22. Set it on the boot card with FreeMcBoot or PS2BBL on it. Power-cycle, confirm `Snap` and a speed show while it loads, and confirm it starts without a soft reset. Repeat at least 10 times.
23. Save in a game, wait for `Caching card...` to clear, power-cycle, and confirm the save is there and `Loading folder..` does not show.
24. Save and power off during `Caching card...`. Confirm the next mount shows `Loading folder..` and the save is there.
25. With the card unmounted, add a save and delete another on a computer. Confirm the next mount shows `Loading folder..` and both changes appear.
26. With the card unmounted, change a byte in the middle of `snapshot.mcd`. Confirm the card appears, then disappears and returns built from the folder, and that the folder is unchanged.
27. Fill the SD card. Confirm the snapshot goes away and saving still works.
28. Switch cards during `Checking card...` and during `Caching card...`, with and without having written to the card.

**New cards**

These need `NewCardBackend=FOLDER`, and nothing in this section has been run on hardware yet.

29. With Game ID on, start a game that has no card yet. Confirm a `.folder` appears, no `.mcd` is created, and the game can save.
30. Start that game again, and confirm it finds its save while starting.
31. Confirm an existing `.mcd` card and a new boot card stay images, and that a folder card stays a folder after setting `NewCardBackend=IMAGE`.

**Loading behind the console**

Nothing in this section has been run on hardware yet. In the debug log, `Card ready after` is when the card appeared, `Files loaded at` and `Card complete at` mark the end of the load, and `Console waited for N reads, longest M us` gives the demand-read cost.

32. Set `StorageBackend=FOLDER` on the boot card with FreeMcBoot or PS2BBL on it, without `FolderSnapshot`. Power-cycle at least 10 times, and record whether it starts without a soft reset, the `Card ready after` time and `Built from`. Do this both with `.ps2meta` deleted and with a current `tree.ok`, since the two take different paths. So far: one cold boot started FreeMcBoot with `.ps2meta` deleted, and none have been checked with `tree.ok`. The `first console poll` line shows how long the console waits, but only with USB serial connected while a console powers the card. Check that this is electrically safe for your setup first, as it has not been tried.
33. During `Reading files...`, open a save in the browser and load one in a game. Confirm icons and data are right, and record the longest wait from the log.
34. During `Reading files...`, save in a game. Confirm `Unsaved changes` shows, and that the save reaches SD after the files are read.
35. Switch cards during `Reading files...`, with and without having written to the card.
36. Cut the power during `Reading files...` and during the sync after it. Confirm the folder is unchanged or complete, as in step 19.
37. With a current snapshot, confirm the card still comes up with `Snap` and not `Reading files...`.

**Compatibility and performance**

38. Test OPL, FreeMcBoot and wLaunchELF using the card, and confirm with MMCEMAN tools that writes into the mounted folder are refused and that FS reset does not break a sync or a snapshot being loaded.
39. Try a game that writes often, and confirm audio and video do not stutter during a sync and that the OLED, buttons and USB stay responsive.
40. Record these benchmarks:

| Measurement | Result |
|-------------|--------|
| Time until visible, empty card | |
| Time until visible, full 8 MB, 20 saves | |
| Time until visible, 2000 small files | |
| Time until all files are read, full 8 MB, 20 saves | |
| Time until all files are read, 2000 small files | |
| Longest demand read during a load | |
| Time until the card is visible with a current snapshot | 178 ms (boot card, PSXMemCard Gen2) |
| Time until `Checking card...` clears | 6.4 s (same card, loaded at 3.6 s) |
| First snapshot write, full 8 MB | |
| Sync time, 1 KB change on a full card | |
| Sync time, 8 MB rewrite | |
| Longest pause of the main loop | |
| Card switch time, clean card | |

The main-loop pause is the most important of these. The slice target is 20 ms, but single SD operations can take much longer on slow cards. The console is not affected by it, while the display and MMCEMAN are.
