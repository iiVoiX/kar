# Offline: functions (with their call sites) that appear only in some frames of trace.bin.
import struct, collections, sys
REC = struct.Struct('<5Q')
cnt = collections.defaultdict(collections.Counter)   # frame -> (fn, lr) -> n
with open('E:/kirby/trace.bin', 'rb') as f:
    while True:
        b = f.read(REC.size * 65536)
        if not b: break
        for fn, lr, x0, x1, ft in REC.iter_unpack(b[:len(b) // REC.size * REC.size]):
            cnt[ft & 0xffffffff][(fn, lr)] += 1
frames = sorted(cnt)
print('frames', frames[0], '..', frames[-1], len(frames))
import pickle; pickle.dump({k: dict(v) for k, v in cnt.items()}, open('E:/kirby/trace_counts.pkl', 'wb'))
