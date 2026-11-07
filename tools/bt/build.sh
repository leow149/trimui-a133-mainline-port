#!/bin/sh
# Cross-build static aarch64 binary on bodenserver (trimui-build container) and fetch it.
set -eu
cd "$(dirname "$0")"
scp -q xr829_bt_up.c bodenserver:/mnt/ext1/trimui_build/
ssh bodenserver 'docker exec trimui-build bash -c "cd /work && aarch64-linux-gnu-gcc -O2 -static -o xr829_bt_up xr829_bt_up.c"'
scp -q bodenserver:/mnt/ext1/trimui_build/xr829_bt_up .
