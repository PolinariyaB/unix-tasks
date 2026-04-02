#!/bin/sh
set -eu

FILE_A="fileA"
SIZE=$((4 * 1024 * 1024 + 1))

rm -f "$FILE_A"

truncate -s "$SIZE" "$FILE_A"

printf '\001' | dd of="$FILE_A" bs=1 seek=0 conv=notrunc status=none 2>/dev/null
printf '\001' | dd of="$FILE_A" bs=1 seek=10000 conv=notrunc status=none 2>/dev/null
printf '\001' | dd of="$FILE_A" bs=1 seek=$((SIZE - 1)) conv=notrunc status=none 2>/dev/null
