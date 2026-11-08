#!/bin/sh
# Build libpvrcap.so and vkxfer_stock against the TrimUI stock rootfs (glibc 2.33). Build-only.
set -eu
D=$(cd "$(dirname "$0")" && pwd)
SB=$D/..
R=${STOCK_ROOTFS:-/home/user/claude/trimui_a133_mainline_port/gpu_video/stock/rootfs}
CC=aarch64-linux-gnu-gcc
cd "$D"
CF="-mno-outline-atomics -std=gnu99 -O2 -Wall -U_FORTIFY_SOURCE -D_FORTIFY_SOURCE=0 -fno-stack-protector -fno-asynchronous-unwind-tables"
LD="-Wl,--allow-shlib-undefined -Wl,-rpath-link,$R/lib:$R/usr/lib"
$CC $CF -shared -fPIC -nostdlib -o libpvrcap.so pvrcap.c -Wl,-soname,libpvrcap.so $LD \
  "$R/lib/libdl.so.2" "$R/lib/libc.so.6"
$CC $CF -I"$SB" -I"$SB/inc" -nostdlib -o vkxfer_stock vkxfer.c "$SB/start.S" \
  -Wl,--dynamic-linker=/lib/ld-linux-aarch64.so.1 $LD \
  "$R/usr/lib/libvulkan.so.1" "$R/lib/libdl.so.2" "$R/lib/libm.so.6" "$R/lib/libc.so.6" \
  $($CC -print-libgcc-file-name)
for f in libpvrcap.so vkxfer_stock; do
  echo "== $f"; file "$f"; readelf -d "$f" | grep NEEDED
  echo -n "max GLIBC: "; readelf -V "$f" | grep -o 'GLIBC_[0-9.]*' | sort -uV | tail -1
done
