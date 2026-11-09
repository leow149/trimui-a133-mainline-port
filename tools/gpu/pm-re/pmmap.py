import struct, sys
def load(path):
    d = open(path, 'rb').read(); off = 0; pages = {}  # phys -> bytes
    while off + 64 <= len(d):
        magic, fl, idx, npg, sz = struct.unpack_from('<IIIIQ', d, off)
        if magic != 0x444e4d50: break
        base = off + 64; npgs = sz >> 12
        tab = struct.unpack_from(f'<{npgs}Q', d, base + sz)
        for i, a in enumerate(tab):
            pages[a] = (fl, i, d[base + i*4096: base + (i+1)*4096])
        off = base + sz + npgs * 8
    return pages
if __name__ == '__main__':
    pages = load(sys.argv[1])
    print(len(pages), 'pages; phys range', hex(min(pages)), hex(max(pages)))
    for phys, (fl, i, p) in sorted(pages.items(), key=lambda x: (x[1][0], x[1][1])):
        if not any(p): continue
        n = max(k for k in range(4096) if p[k]) + 1
        q = struct.unpack_from(f'<{(n+7)//8}Q', p + b'\0'*8)
        pte = [x for x in q if x & 1 and (x & ~0xfff) in pages]
        kind = 'PT' if len(pte) >= max(1, len(q) - 1) and len(pte) == len([x for x in q if x]) else ''
        print(f'fl{fl} pg{i:3d} phys {phys:#x} used {n:4d} {kind} ' + ' '.join(f'{x:016x}' for x in q[:6]))
