#!/bin/sh
P=/root/roms/psp
both() { tag=$1; shift; env -u VK_ICD_FILENAMES sh /root/app.sh old_$tag "$@"; VK_ICD_FILENAMES=/root/new_icd.json sh /root/app.sh new_$tag "$@"; }
{
both vkcube 10 vkcube --wsi wayland
both simple_egl 10 weston-simple-egl -f
both es2gears 10 es2gears_wayland
both simple_vk 10 weston-simple-vulkan
both dmabuf_egl 10 weston-simple-dmabuf-egl
both ppsspp_ff_vk 40 PPSSPPQt --graphics=vulkan "$P/Final Fantasy - 20th Anniversary Edition (Europe) (En,Ja).iso"
both ppsspp_ff_gl 40 PPSSPPQt --graphics=opengl "$P/Final Fantasy - 20th Anniversary Edition (Europe) (En,Ja).iso"
both ppsspp_nfs_vk 50 PPSSPPQt --graphics=vulkan "$P/Need for Speed - Most Wanted - 5-1-0 (Europe) (En,Fr,De,Es,It).iso"
both ppsspp_nfs_gl 50 PPSSPPQt --graphics=opengl "$P/Need for Speed - Most Wanted - 5-1-0 (Europe) (En,Fr,De,Es,It).iso"
both moonlight 15 moonlight
echo ALLDONE
} > /root/ax_summary.txt 2>&1
