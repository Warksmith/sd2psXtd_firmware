#!/bin/sh
set -eu
cd "$(dirname "$0")/../.."
out=$(mktemp -d)
trap 'rm -rf "$out"' EXIT
flags="-std=c11 -Wall -Wextra -Werror -fsanitize=address,undefined -pthread -ffunction-sections -fdata-sections"
for debug in '' -DDEBUG_USB_UART; do
 cc $flags $debug -Itests/mca/stubs -Isrc/usb src/usb/mca_protocol.c src/usb/mca_bridge.c tests/mca_current/test_protocol.c -Wl,--gc-sections -o "$out/protocol"
 "$out/protocol"
done
cc $flags -Wno-unused-parameter -Wno-empty-body -Itests/mca/command_stubs -Itests/mca/stubs -Isrc -Isrc/usb -Isrc/ps2 src/usb/mca_protocol.c src/usb/mca_bridge.c src/usb/mca_page.c src/ps2/ps2_card_backend_mca.c src/ps2/card_emu/ps2_mc_commands.c tests/mca_current/test_ps2_commands.c -Wl,--gc-sections -o "$out/commands"
"$out/commands"
cc $flags -Wno-unused-parameter -Wno-unused-function -Itests/mca/command_stubs -Isrc -Isrc/ps1 -Isrc/usb -Isrc/wear_leveling -Iext/inih -Iext/ESP8266SdFatWrapper/include -include stdio.h src/settings.c src/ps1/ps1_mmce.c tests/mca_current/test_mode_reset.c -Wl,--gc-sections -o "$out/mode"
"$out/mode"
cc $flags -Itests/mca/stubs -Isrc src/ps2/card_emu/ps2_card_backend.c tests/mca_current/test_selector.c -Wl,--gc-sections -o "$out/selector"
"$out/selector"
