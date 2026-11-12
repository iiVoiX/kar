"""Scores Kirby Air Riders test screenshots (TOTK_SHOTS PNGs) for the known picture failures, without image libraries.

For each PNG: mean saturation, mean brightness and the share of near-black pixels. Per folder (one run) the median
brightness decides: rainbow runs (colour textures under a depth compare, fix 34) wash the whole race out, median
0.79-0.87 over the race shots of known bad runs against 0.29-0.55 for clean ones (single shots overlap: a clean sky
view reaches 0.82). A black 3D view shows as a high dark share.
Usage:  python tools/kar_shot_check.py <run folder or png> [...]   (exit 1 when any run is flagged)
"""
import struct
import sys
import zlib
from pathlib import Path

RUN_BRIGHT, DARK_SHARE = 0.62, 0.92   # run median brightness for a rainbow run; near-black share for a black shot


def read_png(path):
    data = Path(path).read_bytes()
    if data[:8] != b'\x89PNG\r\n\x1a\n':
        raise ValueError('not a PNG')
    pos, idat, w = 8, b'', 0
    while pos < len(data):
        n, kind = struct.unpack('>I4s', data[pos:pos + 8])
        body = data[pos + 8:pos + 8 + n]
        if kind == b'IHDR':
            w, h, depth, ctype, _, _, inter = struct.unpack('>IIBBBBB', body)
            if depth != 8 or ctype not in (2, 6) or inter:
                raise ValueError('only 8-bit RGB/RGBA, not interlaced')
            bpp = 3 if ctype == 2 else 4
        elif kind == b'IDAT':
            idat += body
        pos += 12 + n
    raw, stride = zlib.decompress(idat), w * bpp
    out, prev = bytearray(), bytearray(stride)
    i = 0
    for _ in range(h):
        f, line = raw[i], bytearray(raw[i + 1:i + 1 + stride])
        i += 1 + stride
        for x in range(stride):
            a = line[x - bpp] if x >= bpp else 0
            b = prev[x]
            c = prev[x - bpp] if x >= bpp else 0
            if f == 1: line[x] = (line[x] + a) & 255
            elif f == 2: line[x] = (line[x] + b) & 255
            elif f == 3: line[x] = (line[x] + ((a + b) >> 1)) & 255
            elif f == 4:
                p = a + b - c
                pa, pb, pc = abs(p - a), abs(p - b), abs(p - c)
                line[x] = (line[x] + (a if pa <= pb and pa <= pc else b if pb <= pc else c)) & 255
        out += line
        prev = line
    return w, h, bpp, out


def score(path, step=2):
    w, h, bpp, px = read_png(path)
    n = sat_sum = bright = dark = 0
    for y in range(0, h, step):
        row = y * w * bpp
        for x in range(0, w, step):
            r, g, b = px[row + x * bpp], px[row + x * bpp + 1], px[row + x * bpp + 2]
            mx, mn = max(r, g, b), min(r, g, b)
            s = (mx - mn) / mx if mx else 0.0
            n += 1
            sat_sum += s
            bright += mx
            dark += mx < 20
    return sat_sum / n, bright / n / 255, dark / n


def main():
    bad = 0
    for a in sys.argv[1:]:
        p = Path(a)
        files = sorted(p.glob('*.png')) if p.is_dir() else [p]
        brights, blacks = [], 0
        for f in files:
            sat, bri, dark = score(f)
            brights.append(bri)
            blacks += dark > DARK_SHARE
            print(f'  sat {sat:.2f} bright {bri:.2f} dark {dark:.2f}  {f.name}')
        if not brights:
            continue
        med = sorted(brights)[len(brights) // 2]
        verdict = 'RAINBOW' if med > RUN_BRIGHT else 'BLACK' if blacks > len(files) // 2 else 'ok'
        bad += verdict != 'ok'
        print(f'{verdict:7s} {p}: {len(files)} shots, median brightness {med:.2f}, black shots {blacks}')
    return 1 if bad else 0


if __name__ == '__main__':
    sys.exit(main())
