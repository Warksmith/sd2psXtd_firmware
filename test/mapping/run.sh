#!/bin/sh
set -eu
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
OUT=$(mktemp -d)
trap 'rm -rf "$OUT"' EXIT
cc -std=gnu11 -O1 -g -ffunction-sections -fdata-sections -fsanitize=address,undefined -fno-sanitize-recover=undefined -I"$ROOT/test/mapping" -I"$ROOT/src" -I"$ROOT/ext/inih" -I"$ROOT/ext/ESP8266SdFatWrapper/include" "$ROOT/test/mapping/test_mapping.c" "$ROOT/src/card_config.c" "$ROOT/ext/inih/ini.c" -Wl,--gc-sections -o "$OUT/test_mapping"
"$OUT/test_mapping"
