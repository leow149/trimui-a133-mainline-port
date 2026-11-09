import sys, os
from PIL import Image
D = sys.argv[1]
for g in ['aw','gs','mk','fz','mf','kdl2','ladx','pkc','ka','cv3','mrb']:
    rp = os.path.join(D, f'raw_{g}.png')
    if not os.path.exists(rp): print(g, 'no raw'); continue
    raw = Image.open(rp).convert('RGB'); W, H = raw.size; rpx = raw.load()
    for vd in ['vulkan', 'glcore']:
        gp = os.path.join(D, f'new_{g}_{vd}.png')
        if not os.path.exists(gp): print(g, vd, 'no gpu shot'); continue
        im = Image.open(gp).convert('RGB'); GW, GH = im.size; gpx = im.load()
        bad = 0; tot = 0; maxd = 0; first = None
        for y in range(H):
            for x in range(W):
                gx = int((x + 0.5) * GW / W); gy = int((y + 0.5) * GH / H)
                if gy >= GH - 60 and gx < 220: continue   # FPS overlay box
                tot += 1
                a = rpx[x, y]; b = gpx[gx, gy]
                d = max(abs(a[i] - b[i]) for i in range(3))
                if d > 8:
                    bad += 1; first = first or (x, y, a, b)
                maxd = max(maxd, d)
        print(f'{g:5s} {vd:7s} raw {W}x{H} gpu {GW}x{GH}: {bad}/{tot} px differ (>8), max diff {maxd}, first {first}')
