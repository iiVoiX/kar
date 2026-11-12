# Offline: E:/kirby/trace.bin (main.c KAR_TRACE_FRAMES) -> per frame, each f_7005859540 call (view recorder) with x0/x1
# and its direct children (calls whose return address lies in f_7005859540).
import struct, sys, collections
REC = struct.Struct('<5Q')
LO, HI = 0x7005859540, 0x7005859f40
want = set(int(x) for x in sys.argv[1:])
views = collections.defaultdict(list)   # frame -> list of [x0, x1, tid, children]
cur = {}
with open('E:/kirby/trace.bin', 'rb') as f:
    while True:
        b = f.read(REC.size * 65536)
        if not b: break
        for fn, lr, x0, x1, ft in REC.iter_unpack(b[:len(b) // REC.size * REC.size]):
            fr, tid = ft & 0xffffffff, ft >> 32
            if fr not in want: continue
            if fn == 0x7005859540:
                v = [x0, x1, tid, []]
                views[fr].append(v); cur[tid] = v
            elif LO <= lr < HI and tid in cur:
                cur[tid][3].append((fn, lr, x1))
for fr in sorted(views):
    print('frame', fr, len(views[fr]), 'views')
    for x0, x1, tid, ch in views[fr]:
        sig = ' '.join('%x@%x' % (fn & 0xffffff, lr & 0xfff) for fn, lr, _ in ch[:14])
        print('  x0 %x x1 %x n%d: %s' % (x0, x1, len(ch), sig))
