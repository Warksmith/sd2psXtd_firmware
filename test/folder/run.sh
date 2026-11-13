#!/bin/sh
# Builds and runs the folder-card host tests. Needs only a C compiler.
#   test/folder/run.sh [test name filter]
# PF_VERBOSE=1 prints the engine log, PF_LONG=1 runs longer randomized sessions.
# PF_HOST_FOLDER=<dir> also imports a real save folder, leaving that folder untouched.
set -e

ROOT=$(cd "$(dirname "$0")/../.." && pwd)
OUT=${TMPDIR:-/tmp}/ps2-folder-test
INCLUDES="-I$ROOT/test/folder -I$ROOT/src/ps2 -I$ROOT/ext/ESP8266SdFatWrapper/include -I$ROOT/ext/mcfat/inc -I$ROOT/ext/mcfat/src -I$ROOT/ext/inih -I$ROOT/src"

mkdir -p "$OUT"

# mcfat is third-party and shifts into the sign bit, so UBSan stays on our code only.
for src in mcio mcfat util; do
    cc -std=gnu11 -O1 -g -w -fsanitize=address -DMAX_FDHANDLES=12 $INCLUDES \
        -c "$ROOT/ext/mcfat/src/$src.c" -o "$OUT/$src.o"
done

# The real configuration path, down to the ini reader the firmware uses on SD.
cc -std=gnu11 -O1 -g -w -fsanitize=address $INCLUDES -c "$ROOT/ext/inih/ini.c" -o "$OUT/ini.o"
cc -std=gnu11 -O1 -g -w -fsanitize=address $INCLUDES -c "$ROOT/src/card_config.c" -o "$OUT/card_config.o"

cc -std=gnu11 -O1 -g -Wall -Wextra -Wno-unused-parameter -Wno-format-truncation -Wno-sign-compare \
    -fsanitize=address,undefined -fno-sanitize-recover=undefined \
    -DPS2_FOLDER_HOST_TEST $INCLUDES \
    "$ROOT/test/folder/test_folder.c" "$ROOT/test/folder/sim_sd.c" "$ROOT/test/folder/sim_console.c" \
    "$ROOT/test/folder/sim_settings.c" \
    "$ROOT/src/ps2/ps2_card_format.c" "$ROOT/src/ps2/ps2_folder_fs.c" "$ROOT/src/ps2/ps2_folder_name.c" \
    "$ROOT/src/ps2/ps2_folder_meta.c" "$ROOT/src/ps2/ps2_folder_import.c" "$ROOT/src/ps2/ps2_folder_sync.c" \
    "$ROOT/src/ps2/ps2_folder_card.c" "$ROOT/src/ps2/ps2_folder_fill.c" "$ROOT/src/ps2/ps2_folder_snapshot.c" \
    "$OUT/mcio.o" "$OUT/mcfat.o" "$OUT/util.o" "$OUT/ini.o" "$OUT/card_config.o" \
    -o "$OUT/test_folder"

exec "$OUT/test_folder" "$@"
