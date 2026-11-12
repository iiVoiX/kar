"""Builds a stand-in for the Switch's Nintendo extension font (button symbols in the Unicode private use area), which
Kirby Air Riders asks for as built-in font type 5 (runtime/ov_system.c ov_builtin_font). The console font is system
data and is not used; these glyphs are original geometric shapes.

  --probe   every code point E000-E3FF draws as a ring with its last two hex digits, to read off the codes the game uses
  default   button glyphs for the codes in BUTTONS/ARROWS/STICKS; every other code keeps the probe look

Writes <out>/5.ttf (default build/kar458752/fonts). Needs fontTools:  python tools/make_kar_buttonfont.py [--probe]
"""
import argparse
import math
from pathlib import Path

from fontTools.fontBuilder import FontBuilder
from fontTools.pens.ttGlyphPen import TTGlyphPen

UPM = 1000
ROOT = Path(__file__).resolve().parent.parent

# 7-segment digits for hex: segments a b c d e f g
SEG = {'0': 'abcdef', '1': 'bc', '2': 'abged', '3': 'abgcd', '4': 'fgbc', '5': 'afgcd', '6': 'afgedc', '7': 'abc',
       '8': 'abcdefg', '9': 'abcdfg', 'A': 'abcefg', 'B': 'fgcde', 'C': 'afed', 'D': 'bcdeg', 'E': 'afged', 'F': 'afge'}


def rect(pen, x0, y0, x1, y1):
    pen.moveTo((x0, y0)); pen.lineTo((x0, y1)); pen.lineTo((x1, y1)); pen.lineTo((x1, y0)); pen.closePath()


def circle(pen, cx, cy, r, ccw):
    n = 32
    pts = [(cx + r * math.cos(2 * math.pi * i / n), cy + r * math.sin(2 * math.pi * i / n)) for i in range(n)]
    if not ccw:
        pts.reverse()
    pen.moveTo(pts[0])
    for p in pts[1:]:
        pen.lineTo(p)
    pen.closePath()


def digit(pen, ch, x, y, w, h, t):
    s = SEG[ch]
    m = y + h / 2
    if 'a' in s: rect(pen, x, y + h - t, x + w, y + h)
    if 'g' in s: rect(pen, x, m - t / 2, x + w, m + t / 2)
    if 'd' in s: rect(pen, x, y, x + w, y + t)
    if 'f' in s: rect(pen, x, m, x + t, y + h)
    if 'b' in s: rect(pen, x + w - t, m, x + w, y + h)
    if 'e' in s: rect(pen, x, y, x + t, m)
    if 'c' in s: rect(pen, x + w - t, y, x + w, m)


def ring(pen):   # a ring: outer clockwise, inner counter-clockwise (TrueType winding)
    circle(pen, 500, 380, 470, False)
    circle(pen, 500, 380, 400, True)


def probe_glyph(code):
    pen = TTGlyphPen(None)
    ring(pen)
    hx = f'{code & 0xff:02X}'
    digit(pen, hx[0], 260, 200, 200, 360, 50)
    digit(pen, hx[1], 540, 200, 200, 360, 50)
    return pen.glyph()


def poly(pen, pts):   # one closed contour, turned clockwise (TrueType filled winding, y up)
    area = sum(pts[i][0] * pts[(i + 1) % len(pts)][1] - pts[(i + 1) % len(pts)][0] * pts[i][1] for i in range(len(pts)))
    if area > 0:
        pts = pts[::-1]
    pen.moveTo(pts[0])
    for p in pts[1:]:
        pen.lineTo(p)
    pen.closePath()


def stroke(pen, pts, t):   # a polyline of width t: a quad per segment plus a square at each joint
    for (x0, y0), (x1, y1) in zip(pts, pts[1:]):
        dx, dy = x1 - x0, y1 - y0
        n = math.hypot(dx, dy) or 1
        ox, oy = -dy / n * t / 2, dx / n * t / 2
        poly(pen, [(x0 + ox, y0 + oy), (x1 + ox, y1 + oy), (x1 - ox, y1 - oy), (x0 - ox, y0 - oy)])
    for x, y in pts:
        poly(pen, [(x - t / 2, y - t / 2), (x - t / 2, y + t / 2), (x + t / 2, y + t / 2), (x + t / 2, y - t / 2)])


# Letters as polylines in a unit box (x, y in 0..1, y up).
LETTERS = {
    'A': [[(0, 0), (0.5, 1), (1, 0)], [(0.22, 0.4), (0.78, 0.4)]],
    'B': [[(0, 0), (0, 1), (0.72, 1), (0.9, 0.86), (0.9, 0.66), (0.72, 0.52), (0, 0.52)],
          [(0.72, 0.52), (1, 0.36), (1, 0.14), (0.82, 0), (0, 0)]],
    'X': [[(0, 0), (1, 1)], [(0, 1), (1, 0)]],
    'Y': [[(0, 1), (0.5, 0.5), (1, 1)], [(0.5, 0.5), (0.5, 0)]],
    'L': [[(0, 1), (0, 0), (0.9, 0)]],
    'R': [[(0, 0), (0, 1), (0.72, 1), (0.95, 0.85), (0.95, 0.65), (0.72, 0.5), (0, 0.5)], [(0.4, 0.5), (1, 0)]],
    'Z': [[(0, 1), (1, 1), (0, 0), (1, 0)]],
    'S': [[(1, 1), (0, 1), (0, 0.5), (1, 0.5), (1, 0), (0, 0)]],
    '+': [[(0, 0.5), (1, 0.5)], [(0.5, 0), (0.5, 1)]],
    '-': [[(0, 0.5), (1, 0.5)]],
}


def label_glyph(text):   # ring with one or two letters
    pen = TTGlyphPen(None)
    ring(pen)
    n = len(text)
    w, h, t = (300, 400, 70) if n == 1 else (200, 320, 56)
    gap = 60
    x = 500 - (n * w + (n - 1) * gap) / 2
    y = 380 - h / 2
    for ch in text:
        for line in LETTERS[ch]:
            stroke(pen, [(x + px * w, y + py * h) for px, py in line], t)
        x += w + gap
    return pen.glyph()


def arrow_glyph(direction):   # ring with a filled triangle (d-pad direction)
    pen = TTGlyphPen(None)
    ring(pen)
    tri = {'up': [(500, 620), (300, 240), (700, 240)], 'down': [(500, 140), (300, 520), (700, 520)],
           'left': [(260, 380), (640, 180), (640, 580)], 'right': [(740, 380), (360, 180), (360, 580)]}[direction]
    poly(pen, tri)
    return pen.glyph()


def stick_glyph(text):   # filled disc inside the ring with the stick's letter
    pen = TTGlyphPen(None)
    ring(pen)
    circle(pen, 500, 380, 330, False)
    circle(pen, 500, 380, 260, True)
    for line in LETTERS[text]:
        stroke(pen, [(420 + px * 160, 280 + py * 200) for px, py in line], 50)
    return pen.glyph()


# Nintendo extension font layout as homebrew documents it: E0E0.. buttons, E0EA.. d-pad, E0EF/E0F0 plus/minus; the
# E0A0 block repeats the buttons in another style. E0E1 (B) was confirmed in game with --probe ("Turbo-Aufladen" tip).
BUTTONS = {0: 'A', 1: 'B', 2: 'X', 3: 'Y', 4: 'L', 5: 'R', 6: 'ZL', 7: 'ZR', 8: 'SL', 9: 'SR', 0xF: '+', 0x10: '-'}
ARROWS = {0xA: 'up', 0xB: 'down', 0xC: 'left', 0xD: 'right'}
STICKS = {0xE101: 'L', 0xE102: 'R'}


def button_glyph(code):   # None when the code's meaning is unknown (it keeps the probe look)
    for base in (0xE0E0, 0xE0A0):
        k = code - base
        if k in BUTTONS:
            return label_glyph(BUTTONS[k])
        if k in ARROWS:
            return arrow_glyph(ARROWS[k])
    if code in STICKS:
        return stick_glyph(STICKS[code])
    return None


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--probe', action='store_true')
    ap.add_argument('--out', type=Path, default=ROOT / 'build' / 'kar458752' / 'fonts')
    args = ap.parse_args()
    codes = list(range(0xE000, 0xE400))
    names = ['.notdef'] + [f'u{c:04X}' for c in codes]
    glyphs = {'.notdef': TTGlyphPen(None).glyph()}
    for c in codes:
        g = None if args.probe else button_glyph(c)
        glyphs[f'u{c:04X}'] = g if g is not None else probe_glyph(c)
    fb = FontBuilder(UPM, isTTF=True)
    fb.setupGlyphOrder(names)
    fb.setupCharacterMap({c: f'u{c:04X}' for c in codes})
    fb.setupGlyf(glyphs)
    fb.setupHorizontalMetrics({n: (1000, 0) for n in names})
    fb.setupHorizontalHeader(ascent=880, descent=-120)
    fb.setupNameTable({'familyName': 'KarButtons', 'styleName': 'Regular'})
    fb.setupOS2(sTypoAscender=880, sTypoDescender=-120, usWinAscent=880, usWinDescent=120)
    fb.setupPost()
    args.out.mkdir(parents=True, exist_ok=True)
    fb.save(str(args.out / '5.ttf'))
    print('wrote', args.out / '5.ttf', 'probe' if args.probe else '')


if __name__ == '__main__':
    main()
