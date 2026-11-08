#!/bin/sh
# Start RetroArch fullscreen in landscape: edge chroot + udevd (RetroArch's udev input only
# accepts devices udev has tagged as joystick/keyboard) + a cage Wayland session that rotates
# the portrait panel. Config: rootfs/edge/retroarch.cfg, gamepad: rootfs/edge/TrimUI_Smart_Pro_Gamepad.cfg.
# Usage: retroarch.sh [retroarch args...]      Stop: retroarch.sh stop
set -eu
[ "${1:-}" = stop ] && exec /root/wayland-session.sh stop
/root/edge-chroot.sh >/dev/null
R=/tmp/edge
if ! pidof udevd >/dev/null; then
	chroot $R /sbin/udevd --daemon
	chroot $R /bin/udevadm trigger --action=add
	chroot $R /bin/udevadm settle -t 10 || true
fi
# Device overrides go last (see rootfs/edge/retroarch-trimui.cfg).
exec /root/wayland-session.sh /usr/bin/env HOME=/root retroarch \
	--appendconfig=/root/.config/retroarch/trimui.cfg "$@"
