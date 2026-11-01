#!/bin/bash
# Builds one release firmware for every target into build/release/, for attaching to a
# release by hand. Takes much longer than build.sh. See doc/folder-cards.md.
set -eo pipefail

git config --global --add safe.directory '*'

mkdir -p /src/build/release
rm -f /src/build/release/*.uf2

for variant in SD2PSX SD2PSXlite PMC+ PMCZero PSXMemCard; do
    dir=/build/$variant-OFF
    name=$(echo "$variant" | tr '[:upper:]' '[:lower:]')
    echo "=== $variant"
    cmake -S /src -B "$dir" -DCMAKE_BUILD_TYPE=Release -DVARIANT="$variant" -DDEBUG_USB_UART=OFF -G 'Unix Makefiles' >/dev/null
    cmake --build "$dir" -j8 2>&1 | grep -E ' error |FLASH:|RAM:|\*\*\*'
    cp "$dir/$name.uf2" /src/build/release/

    symbols=$(arm-none-eabi-nm "$dir/$name.elf")
    usb=no
    folder=no
    grep -q ' usb_storage_run$' <<<"$symbols" && usb=yes
    grep -q ' ps2_folder_card_open$' <<<"$symbols" && folder=yes
    echo "$name: usb storage=$usb, folder cards=$folder"
done

sha256sum /src/build/release/*.uf2
