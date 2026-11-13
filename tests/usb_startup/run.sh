#!/bin/sh
set -eu
cd "$(dirname "$0")/../.."
out=$(mktemp -d)
trap 'rm -rf "$out"' EXIT
cc -std=c11 -Wall -Wextra -Werror -fsanitize=address,undefined -Itests/usb_startup/stubs -Itests/msc/stubs -Isrc -Isrc/usb src/usb/usb_startup.c tests/usb_startup/test_startup.c -o "$out/startup"
"$out/startup"
