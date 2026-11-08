import re, sys
L = open(sys.argv[1]).read().splitlines()
rx = re.compile(r"^\s*(.+?)-(\d+)\s+\[\d+\]\s+\S+\s+([\d.]+):\s+(\w+):\s*(.*)$")
jt = {}; out = []; ctxname = {}
for l in L:
    m = rx.match(l)
    if not m: continue
    comm, t, ev, rest = m.group(1).strip(), float(m.group(3)), m.group(4), m.group(5)
    if ev == "dma_fence_init":
        d = re.search(r"driver=(\S+) timeline=(\S+) context=(\d+) seqno=(\d+)", rest)
        if d: ctxname[d.group(3)] = f"{d.group(1)}/{d.group(2)}"; out.append((t, f"   init  {d.group(1)}/{d.group(2)} {d.group(3)}:{d.group(4)} by {comm[:14]}"))
    elif ev == "dma_fence_signaled":
        d = re.search(r"driver=(\S+) timeline=(\S+) context=(\d+) seqno=(\d+)", rest)
        if d: out.append((t, f"   SIG   {d.group(1)}/{d.group(2)} {d.group(3)}:{d.group(4)} in {comm[:14]}"))
    elif ev == "pvr_job_create":
        j = re.search(r"job=(\w+)", rest).group(1); jt[j] = (comm.split(":")[0], re.search(r"job_type=(\w+)", rest).group(1)[:4])
    elif ev == "pvr_job_submit_ioctl":
        out.append((t, f"{comm.split(':')[0]:9s} SUBMIT"))
    elif ev == "pvr_job_submit_fw":
        j = re.search(r"job=(\w+)", rest).group(1); c, ty = jt.get(j, ("?", "?")); out.append((t, f"{c:9s} ->fw {ty} seq={re.search(r'done_seqno=(\d+)', rest).group(1)}"))
    elif ev == "pvr_job_done":
        j = re.search(r"job=(\w+)", rest).group(1); c, ty = jt.get(j, ("?", "?")); out.append((t, f"{c:9s} done {ty}"))
    elif ev == "drm_vblank_event":
        out.append((t, "----------- VBLANK"))
    elif ev in ("dma_fence_wait_start", "dma_fence_wait_end"):
        d = re.search(r"driver=(\S+) timeline=(\S+) context=(\d+) seqno=(\d+)", rest)
        out.append((t, f"{comm[:16]:16s} {ev[11:]} {d.group(1)+'/'+d.group(2)+' '+d.group(3)+':'+d.group(4) if d else ''}"))
a, b = float(sys.argv[2]), float(sys.argv[3]); base = out[0][0]
for t, s in out:
    r = 1e3 * (t - base)
    if a <= r <= b and "dma_fence_chain" not in s and "stub" not in s: print(f"{r:8.2f}  {s}")
