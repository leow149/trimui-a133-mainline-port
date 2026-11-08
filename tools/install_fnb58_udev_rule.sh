#!/bin/bash
# One-time: let the logged-in user read the FNIRSI FNB-58 USB power meter (2e3c:5558,
# HID) without root, for tools/fnb58_read.py. Run as root.
set -euo pipefail
cat > /etc/udev/rules.d/60-fnirsi-fnb58.rules <<'EOF'
# FNIRSI FNB-58 USB power meter: HID access for the seated user
SUBSYSTEM=="hidraw", ATTRS{idVendor}=="2e3c", ATTRS{idProduct}=="5558", MODE="0660", TAG+="uaccess"
EOF
udevadm control --reload-rules
udevadm trigger --subsystem-match=hidraw --action=change
ls -l /dev/hidraw*
