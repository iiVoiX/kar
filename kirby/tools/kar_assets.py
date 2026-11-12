# Offline: HAL "PARC" asset catalog (Asset/assets.cat, extracted to E:/kirby/assets.cat) + Asset/assets_a000.dat inside
# romfs.bin. Entry (64 bytes): 16-byte asset id, u32 0, u32 compressed (zstd), u64 offset, u64 stored size,
# u64 size, 16-byte content hash. Usage: kar_assets.py search <bytes as text> [...]  (entries read in menu5's log)
import struct, sys, re, compression.zstd as zstd
CAT = open('E:/kirby/assets.cat', 'rb').read()
IMG = 'E:/kirby/dkb-pc-recomp/build/extracted/kar458752-packed/romfs/romfs.bin'
A000 = 0x113114c50
def entries():
    # the table: 64-byte rows whose u32 at +16 is 0 and +20 is 0/1, starting where a known row is, walking both ways
    known = 0x3caab4c
    lo = known
    while lo - 64 >= 0 and struct.unpack_from('<II', CAT, lo - 64 + 16)[0] == 0 and struct.unpack_from('<I', CAT, lo - 64 + 20)[0] in (0, 1): lo -= 64
    hi = known
    while hi + 64 <= len(CAT) and struct.unpack_from('<I', CAT, hi + 16)[0] == 0 and struct.unpack_from('<I', CAT, hi + 20)[0] in (0, 1): hi += 64
    out = []
    for o in range(lo, hi, 64):
        aid = CAT[o:o + 16]; comp, = struct.unpack_from('<I', CAT, o + 20); off, cs, us = struct.unpack_from('<QQQ', CAT, o + 24)
        out.append((aid, comp, off, cs, us))
    return lo, hi, out
def read(e):
    aid, comp, off, cs, us = e
    with open(IMG, 'rb') as f:
        f.seek(A000 + off); d = f.read(cs)
    return zstd.decompress(d) if comp else d
if __name__ == '__main__':
    lo, hi, E = entries()
    print('table', hex(lo), hex(hi), len(E), 'entries')
    if sys.argv[1:2] == ['search']:
        pats = [p.encode() for p in sys.argv[2:]] + [p.encode('utf-16-le') for p in sys.argv[2:]]
        reads = set(int(m.group(1), 16) for m in re.finditer(r'file 0x113114c50 off (\w+)', open('E:/kirby/menu5/run1.err', encoding='utf-8', errors='replace').read()))
        sel = [e for e in E if e[2] in reads]
        print(len(sel), 'entries read at boot or in the menu')
        for e in sel:
            if e[4] > 64 << 20: continue
            try: d = read(e)
            except Exception as x: continue
            hit = [p for p in pats if p in d]
            if hit: print(e[0].hex(), hex(e[2]), e[4], d[:8].hex(), [h[:30] for h in hit][:4])
