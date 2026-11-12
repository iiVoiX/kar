# Offline: labels and texts of an MSBT (MsgStdBn) asset. Usage: kar_msbt.py <asset id hex> [label regex]
import struct, sys, re
sys.path.insert(0, 'E:/kirby/tools')
import kar_assets
def parse(d):
    assert d[:8] == b'MsgStdBn'
    bom = d[8:10]; le = bom == b'\xff\xfe'
    nsec, = struct.unpack_from('<H', d, 0x0e)
    o = 0x20; secs = {}
    for _ in range(nsec):
        mag = d[o:o + 4]; size, = struct.unpack_from('<I', d, o + 4)
        secs[mag] = d[o + 16:o + 16 + size]; o = (o + 16 + size + 15) & ~15
    labels = {}
    l = secs[b'LBL1']; n, = struct.unpack_from('<I', l, 0)
    for i in range(n):
        cnt, off = struct.unpack_from('<II', l, 4 + 8 * i); p = off
        for _ in range(cnt):
            ln = l[p]; name = l[p + 1:p + 1 + ln].decode(); idx, = struct.unpack_from('<I', l, p + 1 + ln); labels[idx] = name; p += 5 + ln
    t = secs[b'TXT2']; n, = struct.unpack_from('<I', t, 0)
    offs = list(struct.unpack_from('<%dI' % n, t, 4)) + [len(t)]
    texts = [t[offs[i]:offs[i + 1]].decode('utf-16-le', 'replace').rstrip('\0') for i in range(n)]
    return {labels[i]: texts[i] for i in labels}
if __name__ == '__main__':
    lo, hi, E = kar_assets.entries()
    e = [x for x in E if x[0].hex() == sys.argv[1]][0]
    m = parse(kar_assets.read(e))
    pat = re.compile(sys.argv[2] if len(sys.argv) > 2 else '.')
    print(len(m), 'messages')
    for k in sorted(m):
        if pat.search(k): print(k, '=', re.sub(r'[\x00-\x1f]', '·', m[k])[:90])
