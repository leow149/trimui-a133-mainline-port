#!/usr/bin/env python3
"""Analyze a gpu_trace.sh capture: per-process GPU job latencies, frame rates, vblanks."""
import re, sys, collections
L = open(sys.argv[1]).read().splitlines()
rx = re.compile(r"^\s*(.+?)-(\d+)\s+\[\d+\]\s+\S+\s+([\d.]+):\s+(\w+):\s*(.*)$")
jobs = {}            # fence -> dict(comm, queue, run, done, deps)
vbl = []
waits = collections.defaultdict(list)  # comm -> [(start, end)]
wstart = {}
for l in L:
    m = rx.match(l)
    if not m: continue
    comm, pid, t, ev, rest = m.group(1).strip(), m.group(2), float(m.group(3)), m.group(4), m.group(5)
    f = re.search(r"fence=(\d+:\d+)", rest)
    if ev == "drm_sched_job_queue":
        jobs[f.group(1)] = dict(comm=comm, q=t, deps=[])
    elif ev == "drm_sched_job_add_dep":
        d = re.search(r"depends on fence=(\d+:\d+)", rest).group(1)
        if f.group(1) in jobs: jobs[f.group(1)]["deps"].append(d)
    elif ev == "drm_sched_job_run" and f.group(1) in jobs:
        jobs[f.group(1)]["r"] = t
    elif ev == "drm_sched_job_done" and f.group(1) in jobs:
        jobs[f.group(1)]["d"] = t
    elif ev == "drm_vblank_event":
        vbl.append(t)
    elif ev == "dma_fence_wait_start":
        wstart[(comm, pid)] = t
    elif ev == "dma_fence_wait_end" and (comm, pid) in wstart:
        waits[comm].append((wstart.pop((comm, pid)), t))
dur = (vbl[-1] - vbl[0]) if len(vbl) > 1 else 0
print(f"vblanks: {len(vbl)} over {dur:.3f} s -> {(len(vbl)-1)/dur:.2f} Hz" if dur else "no vblanks")
by = collections.defaultdict(list)
for k, j in jobs.items():
    if "r" in j and "d" in j: by[j["comm"]].append((k, j))
ctx_owner = {}
for k, j in jobs.items(): ctx_owner[k.split(":")[0]] = j["comm"]
for comm, lst in sorted(by.items()):
    lst.sort(key=lambda x: x[1]["q"])
    qr = [j["r"]-j["q"] for _, j in lst]; rd = [j["d"]-j["r"] for _, j in lst]
    ctxs = collections.Counter(k.split(":")[0] for k, _ in lst)
    print(f"\n== {comm}: {len(lst)} jobs, contexts {dict(ctxs)}")
    print(f"   queue->run (waiting for deps): avg {1e3*sum(qr)/len(qr):.2f} ms, max {1e3*max(qr):.2f}")
    print(f"   run->done (GPU):               avg {1e3*sum(rd)/len(rd):.2f} ms, max {1e3*max(rd):.2f}")
    foreign = collections.Counter()
    for _, j in lst:
        for d in j["deps"]:
            o = ctx_owner.get(d.split(":")[0], "external:" + d.split(":")[0])
            if o != comm: foreign[o] += 1
    print(f"   deps on other processes/external: {dict(foreign)}")
for comm, w in waits.items():
    tot = sum(e - s for s, e in w)
    print(f"CPU fence waits {comm}: {len(w)} waits, total {1e3*tot:.1f} ms in {dur:.2f} s, max {1e3*max(e-s for s,e in w):.2f} ms")
# per-vblank-interval: which comms got jobs done
if len(sys.argv) > 2:
    t0 = vbl[0]
    ev = sorted([(j["q"], "Q", j["comm"]) for _, j in jobs.items()] + [(j["d"], "D", j["comm"]) for _, j in jobs.items() if "d" in j] + [(t, "V", "") for t in vbl])
    for t, k, c in ev[:int(sys.argv[2])]:
        print(f"{1e3*(t-t0):9.2f} {k} {c}")
