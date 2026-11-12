# Offline: compute-shader stored inputs in shader-cache.pack, grouped by program code (variants per program).
import struct, sys, hashlib, collections
p = sys.argv[1]
f = open(p, 'rb'); head = f.read(16); n = struct.unpack_from('<I', head, 8)[0]
ix = f.read(48 * n)
ents = [struct.unpack_from('<32sIIQ', ix, 48 * i) for i in range(n)]
CONTROL = 0x880
groups = collections.defaultdict(list); stages = collections.Counter()
for name, kind, size, off in ents:
    if kind != 4: continue
    f.seek(off); b = f.read(size)
    magic, ver, stage, length, c1, ans = struct.unpack_from('<6I', b, 0)
    stages[stage] += 1
    if stage != 5: continue
    o = 24 + ans
    code = b[o + CONTROL:o + CONTROL + length]
    groups[hashlib.sha1(b[o:o + CONTROL] + code).hexdigest()].append((b[24:24 + ans], length))
print('stored inputs by stage', dict(stages))
v = sorted(groups.values(), key=len, reverse=True)
print('cs programs', len(v), 'cs inputs', sum(map(len, v)))
for g in v[:8]:
    # which answer bytes differ inside the group
    diff = sorted({i for a, _ in g for i in range(len(a)) if a[i] != g[0][0][i]})
    print(len(g), 'variants, code', g[0][1], 'bytes, differing answer bytes', diff[:20])
