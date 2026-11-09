import re, sys, os, glob
from PIL import Image, ImageChops
D = sys.argv[1]
summ = {}
for l in open(os.path.join(D, 'mx_summary.txt')):
    m = re.match(r'(\S+) secs=(\S+) vblanks=(\d+) uflow="([^"]*)" ss=(\S+) kmsg="([^"]*)"', l)
    if m: summ[m.group(1)] = m.groups()[1:]
rx = re.compile(r"^\s*(.+?)-(\d+)\s+\[\d+\]\s+\S+\s+([\d.]+):\s+(\w+):\s*(.*)$")
def frames(tag):
    p = os.path.join(D, tag + '.trace')
    if not os.path.exists(p): return None
    cnt = {}; vb = 0
    for l in open(p):
        m = rx.match(l)
        if not m: continue
        if m.group(4) == 'drm_vblank_event': vb += 1
        if m.group(4) == 'pvr_job_create' and 'job_type=GEOMETRY' in m.group(5):
            c = m.group(1).strip()[:10]; cnt[c] = cnt.get(c, 0) + 1
    return vb, cnt
def fps(tag):
    f = frames(tag)
    if not f or not f[0]: return '-'
    vb, cnt = f
    return ' '.join(f'{k}:{v * 59.4 / vb:.1f}' for k, v in sorted(cnt.items()) if v > 3)
def uf(s):
    m = re.search(r'events: (\d+)', s); return m.group(1) if m else '?'
tags = sorted({t.split('_', 1)[1] for t in summ})
print(f"{'test':16s} {'old fps':28s} {'new fps':28s} uf o/n  ss-diff  kmsg")
for t in tags:
    o, n = 'old_' + t, 'new_' + t
    d = '-'
    po, pn = os.path.join(D, 'img', o + '.png'), os.path.join(D, 'img', n + '.png')
    if os.path.exists(po) and os.path.exists(pn):
        a, b = Image.open(po).convert('RGB'), Image.open(pn).convert('RGB')
        if a.size != b.size: d = f'size {a.size}/{b.size}'
        else:
            diff = ImageChops.difference(a, b); bb = diff.getbbox()
            d = 'identical' if not bb else f'DIFF max={max(diff.getextrema(), key=lambda e: e[1])[1]} bbox={bb}'
    else: d = f'missing o={os.path.exists(po)} n={os.path.exists(pn)}'
    km = (summ.get(o, ['']*5)[4] + summ.get(n, ['']*5)[4])[:60]
    print(f"{t:16s} {fps(o):28s} {fps(n):28s} {uf(summ.get(o,['','',''])[1])}/{uf(summ.get(n,['','',''])[1])}  {d}  {km}")
