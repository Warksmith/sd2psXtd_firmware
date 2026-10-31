#!/bin/bash
# Builds the firmware for the PSXMemCard Gen2 (VARIANT=SD2PSX), with and without USB
# serial logging, into build/docker/. Keep /build in a volume so that later runs only
# recompile what changed. See doc/folder-cards.md.
set -eo pipefail

git config --global --add safe.directory '*'

for debug in OFF ON; do
    dir=/build/SD2PSX-$debug
    echo "=== SD2PSX debug=$debug"
    cmake -S /src -B "$dir" -DCMAKE_BUILD_TYPE=Release -DVARIANT=SD2PSX -DDEBUG_USB_UART="$debug" -G 'Unix Makefiles' >/dev/null
    cmake --build "$dir" -j8 2>&1 | grep -E ' error |FLASH:|RAM:|ps2_folder.*warning|\*\*\*'
done

mkdir -p /src/build/docker
cp /build/SD2PSX-OFF/sd2psx.uf2 /build/SD2PSX-OFF/sd2psx.elf /build/SD2PSX-ON/sd2psx-debug.uf2 /src/build/docker/
sha256sum /src/build/docker/sd2psx.uf2 /src/build/docker/sd2psx-debug.uf2
