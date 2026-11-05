#!/bin/sh
# Run a program fullscreen in a cage (wlroots kiosk) Wayland session on the panel, in landscape.
# OpenGL / OpenGL ES go through Zink on the PowerVR Vulkan driver; Vulkan apps use it directly.
# Rendering happens on the GPU (renderD128), scan-out on the display engine (card1).
# Usage: wayland-session.sh <program> [args...]   (inside the edge chroot, see edge-chroot.sh)
# Example: wayland-session.sh weston-simple-egl -f
set -eu
R=/tmp/edge
[ $# -ge 1 ] || { echo "usage: $0 <program> [args...]"; exit 1; }
mkdir -p $R/tmp/xdg
chmod 700 $R/tmp/xdg
pidof seatd >/dev/null || { chroot $R /usr/bin/seatd -g root >/tmp/seatd.log 2>&1 & sleep 1; }
# The panel is mounted in portrait; transform 90 gives landscape.
exec chroot $R /usr/bin/env \
	XDG_RUNTIME_DIR=/tmp/xdg \
	LIBSEAT_BACKEND=seatd \
	WLR_LIBINPUT_NO_DEVICES=1 \
	WLR_DRM_DEVICES=/dev/dri/card1 \
	WLR_RENDER_DRM_DEVICE=/dev/dri/renderD128 \
	MESA_LOADER_DRIVER_OVERRIDE=zink \
	GALLIUM_DRIVER=zink \
	PVR_I_WANT_A_BROKEN_VULKAN_DRIVER=1 \
	/usr/bin/cage -- /bin/sh -c 'wlr-randr --output DSI-1 --transform 90; exec "$@"' sh "$@"
