# vendor-notes: measurements from the stock firmware

Register dumps and notes recorded on a TrimUI Smart Pro running its stock firmware (booted from
eMMC with the SD card removed, accessed over ADB as root), most of them next to the same reads
on mainline. They are what the comparisons in the patches and in `docs/` are based on.

Only our own measurements and notes are here. Vendor binaries (firmware blobs, kernel image,
eMMC dumps, decompiled vendor code) are not; see the main README for why.

| Path | Content |
|---|---|
| `stock_firmware_notes.md` | What the stock system uses: input (GPIO, ADC and two UART microcontrollers), GPU, Wi-Fi/BT, camera, audio, RGB LEDs, firmware files, partition layout |
| `live_reg_dump_20260823.md` | TCON0/DSI/D-PHY registers on the running stock system vs mainline; found the TCON interrupt number |
| `live_reg_dump_20260823_mixer.md` | The same for the display engine mixer, blender and TCON-TOP |
| `live_reg_dump_20260823_full/` | The raw dumps of that session (register ranges, clock rates, interrupts, stock dmesg) |
| `live_reg_dump.sh`, `mmapread.c` | How they were taken: the stock kernel's `/dev/mem` only supports mmap() for MMIO, `mmapread` is a minimal static reader |
| `blind_regdump_20260823/` | A wider register sweep, stock (`vendor.txt`) vs mainline (`ours.txt`) and their `diff.txt` |
| `regsnap_20261031/` | AXP717 PMIC and pin controller state, stock vs mainline (`tools/regsnap.c`) |
| `gpu_regdump/` | GPU registers during rendering, CCU (clocks) and memory bus registers, stock vs mainline (`tools/gpu/regdump.c`, `regsample.c`) |

Some of these notes were written during the investigation and record hypotheses that were later
corrected; the conclusions that held are in the patches and in `docs/`.
