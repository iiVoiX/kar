import struct, sys, collections
REC = struct.Struct('<5Q')
LO, HI = 0x7005859540, 0x7005859f40
want = set(int(x) for x in sys.argv[1:])
views = collections.defaultdict(list); cur = {}
with open('E:/kirby/trace.bin', 'rb') as f:
    while True:
        b = f.read(REC.size * 65536)
        if not b: break
        for fn, lr, x0, x1, ft in REC.iter_unpack(b[:len(b) // REC.size * REC.size]):
            fr, tid = ft & 0xffffffff, ft >> 32
            if fr not in want: continue
            if fn == 0x7005859540:
                v = [x1, []]; views[fr].append(v); cur[tid] = v
            elif LO <= lr < HI and tid in cur:
                cur[tid][1].append((fn, lr, x0, x1))
for fr in sorted(views):
    print('frame', fr)
    for x1, ch in views[fr]:
        keep = [c for c in ch if c[0] in (0x700581bf94, 0x700581c13c, 0x700584b660, 0x700581d520, 0x700581bd60)]
        if len(ch) < 10: continue
        print('  n%d ' % len(ch) + ' '.join('%x(x0 %x x1 %x)' % (fn & 0xffffff, a0, a1) for fn, lr, a0, a1 in keep[:5]))
