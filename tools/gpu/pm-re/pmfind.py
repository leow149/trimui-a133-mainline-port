import struct, sys, glob, os
def regions(d):
    off = 0; r = []
    while off + 64 <= len(d):
        magic, fl, idx, npg, sz = struct.unpack_from('<IIIIQ', d, off)
        if magic != 0x444e4d50: break
        r.append((fl, off + 64, sz)); off += 64 + sz
    return r
def isvert(q):
    x, y = q & 0x1ffff, (q >> 17) & 0x1ffff
    return 0x7000 <= x <= 0x9800 and 0x7000 <= y <= 0x9800
for f in sorted(glob.glob(os.path.join(sys.argv[1], 'pm_*.bin'))):
    d = open(f, 'rb').read(); seen = set()
    for fl, base, sz in regions(d):
        for pg in range(0, sz, 4096):
            p = d[base+pg: base+pg+4096]
            if not any(p): continue
            # a primitive block: >=3 consecutive vertex-like qwords at 8-byte aligned offset >= 0x20
            for o in range(0x20, 0x200, 8):
                qs = [struct.unpack_from('<Q', p, o + 8*k)[0] for k in range(3)]
                if all(isvert(q) for q in qs) and not isvert(struct.unpack_from('<Q', p, o - 8)[0]):
                    hdr = struct.unpack_from('<8I', p, o - 0x20)
                    nv = 0
                    while isvert(struct.unpack_from('<Q', p, o + 8*nv)[0]): nv += 1
                    tail = struct.unpack_from('<8I', p, o + 8*nv)
                    key = (hdr, tail)
                    if key in seen: break
                    seen.add(key)
                    print(f'{os.path.basename(f):16s} pg{pg//4096:3d} vtx@{o:#x} n={nv} hdr=' + ' '.join(f'{x:08x}' for x in hdr))
                    print(f'{"":16s}       pds=' + ' '.join(f'{x:08x}' for x in tail))
                    break
