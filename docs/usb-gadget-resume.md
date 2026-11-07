# USB gadget re-enumeration after s2idle (fixed 2026-11-08)

Symptom: after suspend/resume the host logged `device descriptor read/64, error -71` and the UDC
stayed "not attached". Cause: `musb_suspend()` writes DEVCTL = 0 (ends the session) unless the glue
sets `MUSB_PRESERVE_SESSION`, and saves the context after that, so `musb_resume()` never restores
the session. The sunxi glue only starts the session from its mode-change work. Registers before and
after a failed resume: DEVCTL 0x99 -> 0x98 (SESSION lost), POWER HSMODE lost; the PHY, PMU, ISCR
(0x4303f000 -> 0x4703f000) and CCU gate/reset registers were intact, so no PHY re-init is needed.

Fix: patches/linux/0073 sets `MUSB_PRESERVE_SESSION` in the sunxi musb platform ops.
Results (tests/results-20261108/usb-resume/): before 0/6 re-enumerated, after 11/11, ISCR unchanged.
Harness: usbsusp.sh (runs on the device).
