#!/bin/sh
set -eu
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
OUT=$(mktemp -d)
trap 'rm -rf "$OUT"' EXIT
cc -std=gnu11 -O1 -g -fsanitize=address,undefined -fno-sanitize-recover=undefined -I"$ROOT/test/ini" -I"$ROOT/ext/inih" -I"$ROOT/ext/ESP8266SdFatWrapper/include" -I"$ROOT/src" "$ROOT/test/ini/test_ini.c" "$ROOT/ext/inih/ini.c" -o "$OUT/test_ini"
"$OUT/test_ini"
