# Analyse vendor (stock) transfer captures: FAST_2D register sets + control stream entries + block headers.
import struct, sys, os, glob
def regsets(log):
    d = open(log, 'rb').read(); o = 0; out = set()
    while o + 56 <= len(d):
        mg, k, sq, fd, a, b, n = struct.unpack_from('<7Q', d, o); o += 56
        data = d[o:o+n]; o += n
        if k not in (3, 4) or n < 64: continue
        w = struct.unpack_from(f'<{n//4}I', data)
        # pattern: ISP_BGOBJVALS USC_POC CLR0..3 MTILE_SIZE ORIGIN ISP_CTL AA EPDS_INFO CODE DATA ISP_RENDER ISP_RGN
        for i in range(len(w) - 16):
            if w[i+1] == 0x1ffc and w[i+13] in (0, 1, 2, 3) and (w[i+10] & 0xfff) == 0x202:
                out.add(tuple(w[i:i+16]))
    return out
def entries(mapf):
    d = open(mapf, 'rb').read()
    res = []
    for o in range(0, min(len(d), 0x200), 8):
        dw0, dw1 = struct.unpack_from('<2I', d, o)
        if dw1 & 1 and dw1 < 0x100000 and (dw0 & 0xff) and (dw0 >> 8) and dw0 < 0x1000000:
            blk = (dw1 & ~1) << 2
            if blk + 16 <= len(d):
                fmt = struct.unpack_from('<4I', d, blk)
                if (fmt[0] & 0xfff) in range(0, 0x400) and fmt[2] & 0x00200000 or fmt[2] in (0x002e0000,):
                    res.append((o, dw0, dw1, blk, fmt))
    return res
for cap in sorted(glob.glob(os.path.join(sys.argv[1], 'cap_*')) + [os.path.join(sys.argv[1], 'pvrcap')]):
    print('=====', os.path.basename(cap))
    for r in sorted(regsets(os.path.join(cap, 'ioctl.log'))):
        print('  regs bg=%08x mtile=%08x origin=%08x ispctl=%08x aa=%08x epds=%08x render=%x rgn=%08x' %
              (r[0], r[6], r[7], r[8], r[9], r[10], r[13], r[14]))
    for m in sorted(glob.glob(os.path.join(cap, '*_map_*.bin'))):
        for o, dw0, dw1, blk, fmt in entries(m):
            n = (dw0 >> 8).bit_length()
            print(f'  {os.path.basename(m)} entry@{o:#x}: {dw0:08x} {dw1:08x} low={dw0 & 0xff:#04x} prims={n} -> block@{blk:#x} fmt={fmt[0]:08x} {fmt[1]:08x} isp={fmt[2]:08x} {fmt[3]:08x} nvtx={((fmt[0]>>5)&0x1f)+1} tspdw={fmt[0]>>12}')
