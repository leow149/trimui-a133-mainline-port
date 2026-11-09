# Parameter-memory reverse engineering (debug only)

Used to decode the GE8300's simple internal parameter format v1 (docs/gpu-v1-transfer.md).

- `DEBUG-ONLY-pvr-pmdump-debugfs.patch`: kernel patch adding `/sys/kernel/debug/dri/0/pvr_pm/dump`,
  a snapshot of all free-list pages with their DMA addresses. Never ship it.
- `pmcap2.sh`: render with `vkpass2 hold=N warm=0` and dump parameter memory + region headers
  (the region header dump needs a Mesa debug hook, see the doc).
- `pmmap.py` (pages by physical address), `pmchain.py` (PM catalogue -> directory -> table -> data,
  control streams and their primitive blocks), `pmfind.py` (primitive blocks by vertex content).

The vendor-driver side (captures on stock firmware) is in `tools/gpu/stock_build/capture/` and
`vendor_data/stockcap_20261108/analyze.py`.
