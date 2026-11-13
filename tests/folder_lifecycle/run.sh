#!/bin/sh
set -eu
cd "$(dirname "$0")/../.."
out=$(mktemp -d)
trap 'rm -rf "$out"' EXIT
cc -std=gnu11 -Wall -Wextra -Wno-unused-variable -Wno-unused-parameter -ffunction-sections -fdata-sections -fsanitize=address,undefined -fno-sanitize-recover=undefined -Itests/folder_lifecycle/stubs -Itests/mca/command_stubs -Iext/ESP8266SdFatWrapper/include -Isrc -Isrc/ps2 -Itests/mca/stubs src/ps2.c tests/folder_lifecycle/test_ps2.c -Wl,--gc-sections -o "$out/lifecycle"
"$out/lifecycle"
cc -std=c11 -Wall -Wextra -Werror -fsanitize=address,undefined -pthread -Itests/mca/stubs -Isrc -Isrc/usb -Isrc/ps2 src/usb/mca_bridge.c src/ps2/ps2_card_backend_mca.c tests/folder_lifecycle/test_mca_visibility.c -o "$out/visibility"
"$out/visibility"
