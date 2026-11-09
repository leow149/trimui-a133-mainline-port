import sys, struct, os
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from pmmap import load
def is_pt(p):
    q = struct.unpack_from('<512Q', p); nz = [x for x in q if x]
    return nz and all(x & 1 and x < (1 << 40) and not (x & 0xffe) for x in nz)
def spaces(pages):
    # catalogue pages: few bytes, pfn<<4|1 entries -> follow PC -> PD -> PT -> data pages
    out = []
    for phys, (fl, i, p) in pages.items():
        w = struct.unpack_from('<1024I', p); nz = [x for x in w if x]
        if len(nz) == 1 and (nz[0] & 0xf) == 1 and ((nz[0] >> 4) << 12) in pages:
            pd = pages[(nz[0] >> 4) << 12][2]
            if not is_pt(pd): continue
            pts = [x & ~0xfff for x in struct.unpack_from('<512Q', pd) if x]
            data = []
            for pt in pts:
                if pt in pages:
                    data += [x & ~0xfff for x in struct.unpack_from('<512Q', pages[pt][2]) if x]
            out.append(data)
    return out
for n in sys.argv[2:]:
    pages = load(os.path.join(sys.argv[1], f'pm2_{n}.bin'))
    sp = spaces(pages)
    vce = cs = None
    for data in sp:
        if not data or data[0] not in pages: continue
        p = pages[data[0]][2]
        w = struct.unpack_from('<4I', p)
        if (w[0] & 0x3) == 0 and (w[0] >> 8) < 0x100 and w[1] & 1 and w[0] < 0x10000 and (w[0] & 0xff) >= 4: cs = p
        elif (w[0] & 0x3f) in (0x01, 0x21) or w[0] & 0x80: vce = p
    print(f'== {n}')
    if cs:
        w = struct.unpack_from('<32I', cs, 0)
        last = max(j for j, x in enumerate(w) if x)
        print('  tile0 cs: ' + ' '.join(f'{x:08x}' for x in w[:last + 1]))
        w = struct.unpack_from('<32I', cs, 15 * 0x80)
        last = max(j for j, x in enumerate(w) if x)
        print('  tile15 cs: ' + ' '.join(f'{x:08x}' for x in w[:last + 1]))
        # print each referenced block header
        ws = struct.unpack_from('<32I', cs, 0)
        j = 0
        while j < 32 and (ws[j] & 3) == 0 and ws[j]:
            addr = (ws[j + 1] & ~1) << 2
            if vce:
                h = struct.unpack_from('<10I', vce, addr)
                print(f'    entry dw0={ws[j]:08x} block@{addr:#x}: ' + ' '.join(f'{x:08x}' for x in h))
            j += 2

def blocks(D, n):
    pages = load(os.path.join(D, f'pm2_{n}.bin'))
    sp = spaces(pages); cs = vce = None
    for data in sp:
        if not data or data[0] not in pages: continue
        p = pages[data[0]][2]; w = struct.unpack_from('<4I', p)
        if (w[0] & 0x3) == 0 and w[0] < 0x100000 and w[1] & 1 and (w[0] & 0xff) >= 4: cs = p
        else: vce = p
    ws = struct.unpack_from('<32I', cs, 0); j = 0; res = []
    while (ws[j] & 3) == 0 and ws[j]:
        addr = (ws[j + 1] & ~1) << 2
        q = struct.unpack_from('<40Q', vce, addr)
        def isv(x): return 0x7000 <= (x & 0x1ffff) <= 0x9800 and 0x7000 <= ((x >> 17) & 0x1ffff) <= 0x9800 and (x >> 34) == 0
        vstart = next(k for k in range(1, 40) if isv(q[k]) and isv(q[k+1]))
        nv = 0
        while isv(q[vstart + nv]): nv += 1
        res.append((ws[j], addr, q[0], vstart, nv, q[vstart+nv: vstart+nv+4]))
        j += 2
    return res
