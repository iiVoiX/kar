"""Builds a stand-in for the Switch's Nintendo extension font (button symbols in the Unicode private use area), which
Kirby Air Riders asks for as built-in font type 5 (runtime/ov_system.c ov_builtin_font). The console font is system
data and is not used; these glyphs are original geometric shapes.

  --probe   every code point E000-E3FF draws as a ring with its last two hex digits, to read off the codes the game uses
  default   button glyphs for the codes in BUTTONS/ARROWS/STICKS; every other code keeps the probe look

Button symbols per pad and keyboard (RELEASE_PLAN 17.11): every game code with a known meaning is a composite glyph of
one component, by default its Switch glyph. The font also holds the other styles as plain glyphs: x_<pad input> (Xbox
labels), p_<pad input> (PlayStation shapes) and k_<key> (a keycap per bindable key, mouse buttons as a mouse), named
after runtime/input_map.c's pad_names and key_names. <out>/5.map lists, per code, the Switch button it stands for and the
file offset of its component's glyph index ("slot <code> <button> <offset>"), and the glyph id of every style glyph
("glyph <name> <id>"). runtime/ov_system.c points each component at the glyph for the pad or key the button is bound to
(TOTK_BUTTON_GLYPHS), so the prompts follow the bindings without rebuilding the font.

Writes <out>/5.ttf (default build/kar458752/fonts). Needs fontTools:  python tools/make_kar_buttonfont.py [--probe]
"""
import argparse
import math
import re
import struct
from pathlib import Path

from fontTools.fontBuilder import FontBuilder
from fontTools.pens.ttGlyphPen import TTGlyphPen
from fontTools.ttLib import TTFont

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
    'C': [[(1, 1), (0, 1), (0, 0), (1, 0)]],
    'D': [[(0, 0), (0, 1), (0.6, 1), (1, 0.7), (1, 0.3), (0.6, 0), (0, 0)]],
    'E': [[(1, 1), (0, 1), (0, 0), (1, 0)], [(0, 0.5), (0.7, 0.5)]],
    'F': [[(1, 1), (0, 1), (0, 0)], [(0, 0.5), (0.7, 0.5)]],
    'G': [[(1, 1), (0, 1), (0, 0), (1, 0), (1, 0.45), (0.55, 0.45)]],
    'H': [[(0, 0), (0, 1)], [(1, 0), (1, 1)], [(0, 0.5), (1, 0.5)]],
    'I': [[(0.5, 0), (0.5, 1)], [(0.2, 1), (0.8, 1)], [(0.2, 0), (0.8, 0)]],
    'J': [[(1, 1), (1, 0), (0, 0), (0, 0.3)]],
    'K': [[(0, 0), (0, 1)], [(1, 1), (0, 0.45), (1, 0)]],
    'M': [[(0, 0), (0, 1), (0.5, 0.45), (1, 1), (1, 0)]],
    'N': [[(0, 0), (0, 1), (1, 0), (1, 1)]],
    'O': [[(0, 0), (0, 1), (1, 1), (1, 0), (0, 0)]],
    'P': [[(0, 0), (0, 1), (1, 1), (1, 0.5), (0, 0.5)]],
    'Q': [[(0, 0), (0, 1), (1, 1), (1, 0), (0, 0)], [(0.6, 0.3), (1, -0.05)]],
    'T': [[(0, 1), (1, 1)], [(0.5, 1), (0.5, 0)]],
    'U': [[(0, 1), (0, 0), (1, 0), (1, 1)]],
    'V': [[(0, 1), (0.5, 0), (1, 1)]],
    'W': [[(0, 1), (0.25, 0), (0.5, 0.6), (0.75, 0), (1, 1)]],
    '0': [[(0, 0), (0, 1), (1, 1), (1, 0), (0, 0)], [(0, 0), (1, 1)]],
    '1': [[(0.25, 0.8), (0.55, 1), (0.55, 0)], [(0.2, 0), (0.9, 0)]],
    '2': [[(0, 1), (1, 1), (1, 0.5), (0, 0.5), (0, 0), (1, 0)]],
    '3': [[(0, 1), (1, 1), (1, 0), (0, 0)], [(0.3, 0.5), (1, 0.5)]],
    '4': [[(0, 1), (0, 0.5), (1, 0.5)], [(0.8, 1), (0.8, 0)]],
    '5': [[(1, 1), (0, 1), (0, 0.5), (1, 0.5), (1, 0), (0, 0)]],
    '6': [[(1, 1), (0, 1), (0, 0), (1, 0), (1, 0.5), (0, 0.5)]],
    '7': [[(0, 1), (1, 1), (0.4, 0)]],
    '8': [[(0, 0), (0, 1), (1, 1), (1, 0), (0, 0)], [(0, 0.5), (1, 0.5)]],
    '9': [[(1, 0.5), (0, 0.5), (0, 1), (1, 1), (1, 0), (0, 0)]],
    '.': [[(0.45, 0), (0.55, 0), (0.55, 0.1), (0.45, 0.1), (0.45, 0)]],
    ',': [[(0.55, 0.15), (0.4, -0.15)]],
    '/': [[(0, 0), (1, 1)]],
    '*': [[(0.5, 0.15), (0.5, 0.85)], [(0.2, 0.3), (0.8, 0.7)], [(0.2, 0.7), (0.8, 0.3)]],
    '=': [[(0, 0.35), (1, 0.35)], [(0, 0.65), (1, 0.65)]],
    '[': [[(0.8, 1), (0.3, 1), (0.3, 0), (0.8, 0)]],
    ']': [[(0.2, 1), (0.7, 1), (0.7, 0), (0.2, 0)]],
    ';': [[(0.5, 0.6), (0.5, 0.7)], [(0.55, 0.15), (0.4, -0.15)]],
    "'": [[(0.5, 1), (0.5, 0.7)]],
    '`': [[(0.35, 1), (0.6, 0.75)]],
    '\\': [[(0, 1), (1, 0)]],
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


def text_strokes(pen, text, cx, cy, h, t):   # letters centred at (cx, cy), cap height h, stroke width t
    w = h * 0.7
    gap = h * 0.18
    x = cx - (len(text) * w + (len(text) - 1) * gap) / 2
    y = cy - h / 2
    for ch in text:
        for line in LETTERS[ch]:
            stroke(pen, [(x + px * w, y + py * h) for px, py in line], t)
        x += w + gap


def ring_text(text):   # ring with up to three letters (Xbox labels, shoulder and trigger names)
    pen = TTGlyphPen(None)
    ring(pen)
    h, t = {1: (400, 70), 2: (300, 56), 3: (210, 44)}[min(len(text), 3)]
    text_strokes(pen, text, 500, 380, h, t)
    return pen.glyph()


def ps_glyph(shape):   # ring with one of the four face-button shapes
    pen = TTGlyphPen(None)
    ring(pen)
    if shape == 'cross':
        stroke(pen, [(330, 210), (670, 550)], 70)
        stroke(pen, [(330, 550), (670, 210)], 70)
    elif shape == 'circle':
        circle(pen, 500, 380, 190, False)
        circle(pen, 500, 380, 130, True)
    elif shape == 'square':
        stroke(pen, [(330, 210), (330, 550), (670, 550), (670, 210), (330, 210)], 60)
    else:   # triangle
        stroke(pen, [(500, 590), (310, 230), (690, 230), (500, 590)], 60)
    return pen.glyph()


def lines_glyph():   # ring with three short bars (a menu or options button)
    pen = TTGlyphPen(None)
    ring(pen)
    for y in (270, 380, 490):
        rect(pen, 330, y - 28, 670, y + 28)
    return pen.glyph()


def squares_glyph():   # ring with two overlapping frames (a view or share button)
    pen = TTGlyphPen(None)
    ring(pen)
    stroke(pen, [(300, 300), (300, 560), (560, 560)], 44)
    stroke(pen, [(420, 200), (420, 460), (700, 460), (700, 200), (420, 200)], 44)
    return pen.glyph()


def keycap_outline(pen):   # a rounded square frame: outer clockwise, inner counter-clockwise
    def rounded(x0, y0, x1, y1, r, cw):
        pts = []
        for cx, cy, a0 in ((x1 - r, y1 - r, 0), (x0 + r, y1 - r, 90), (x0 + r, y0 + r, 180), (x1 - r, y0 + r, 270)):
            for i in range(7):
                a = math.radians(a0 + 15 * i)
                pts.append((cx + r * math.cos(a), cy + r * math.sin(a)))
        if cw:
            pts.reverse()
        pen.moveTo(pts[0])
        for q in pts[1:]:
            pen.lineTo(q)
        pen.closePath()
    rounded(40, -80, 960, 840, 140, True)
    rounded(100, -20, 900, 780, 90, False)


def keycap_glyph(text):   # keycap with up to four characters
    pen = TTGlyphPen(None)
    keycap_outline(pen)
    h, t = {1: (380, 66), 2: (290, 56), 3: (200, 44), 4: (150, 36)}[min(len(text), 4)]
    text_strokes(pen, text, 500, 380, h, t)
    return pen.glyph()


def keycap_arrow(direction):
    pen = TTGlyphPen(None)
    keycap_outline(pen)
    tri = {'up': [(500, 600), (310, 220), (690, 220)], 'down': [(500, 160), (310, 540), (690, 540)],
           'left': [(270, 380), (650, 190), (650, 570)], 'right': [(730, 380), (350, 190), (350, 570)]}[direction]
    poly(pen, tri)
    return pen.glyph()


def mouse_glyph(which):   # a mouse outline with the pressed button filled (l, r, m) or a side button number (x1, x2)
    pen = TTGlyphPen(None)
    stroke(pen, [(250, 380), (250, 80), (320, -40), (680, -40), (750, 80), (750, 380), (750, 620), (680, 760),
                 (320, 760), (250, 620), (250, 380)], 50)
    stroke(pen, [(250, 420), (750, 420)], 40)
    stroke(pen, [(500, 420), (500, 760)], 40)
    if which == 'l':
        poly(pen, [(290, 450), (290, 610), (340, 720), (470, 720), (470, 450)])
    elif which == 'r':
        poly(pen, [(530, 450), (530, 720), (660, 720), (710, 610), (710, 450)])
    elif which == 'm':
        rect(pen, 460, 500, 540, 680)
    else:
        text_strokes(pen, '1' if which == 'x1' else '2', 500, 200, 220, 44)
    return pen.glyph()


# Pad inputs (runtime/input_map.c pad_names) as each style labels them. Face buttons by position: Xbox south A, east B,
# west X, north Y; PlayStation cross, circle, square, triangle. A Steam Deck carries the Xbox labels.
XBOX = {'south': 'A', 'east': 'B', 'west': 'X', 'north': 'Y', 'l': 'LB', 'r': 'RB', 'zl': 'LT', 'zr': 'RT',
        'lstick': 'LS', 'rstick': 'RS', 'lpaddle1': 'P1', 'rpaddle1': 'P2', 'lpaddle2': 'P3', 'rpaddle2': 'P4'}
PS = {'l': 'L1', 'r': 'R1', 'zl': 'L2', 'zr': 'R2', 'lstick': 'L3', 'rstick': 'R3', 'lpaddle1': 'P1',
      'rpaddle1': 'P2', 'lpaddle2': 'P3', 'rpaddle2': 'P4'}
PS_SHAPES = {'south': 'cross', 'east': 'circle', 'west': 'square', 'north': 'triangle'}
DPAD = {'dpad_left': 'left', 'dpad_up': 'up', 'dpad_right': 'right', 'dpad_down': 'down'}
# Keycap labels for key names longer than one character (runtime/input_map.c key_names)
KEY_TEXT = {'space': 'SPC', 'tab': 'TAB', 'lshift': 'SHF', 'rshift': 'SHF', 'lctrl': 'CTL', 'rctrl': 'CTL',
            'enter': 'ENT', 'backspace': 'BKS', 'lalt': 'ALT', 'capslock': 'CAPS', 'insert': 'INS', 'delete': 'DEL',
            'home': 'HOME', 'end': 'END', 'pageup': 'PGUP', 'pagedown': 'PGDN', 'num_add': 'N+', 'num_sub': 'N-',
            'num_mul': 'N*', 'num_div': 'N/', 'num_decimal': 'N.', 'minus': '-', 'equals': '=', 'lbracket': '[',
            'rbracket': ']', 'semicolon': ';', 'apostrophe': "'", 'comma': ',', 'period': '.', 'slash': '/',
            'backslash': chr(92), 'grave': '`'}


def c_names(array):   # the string literals of a name table in runtime/input_map.c, in id order from 1
    src = (ROOT / 'runtime' / 'input_map.c').read_text(encoding='utf-8')
    body = src[src.index(f'static const char *const {array}['):]
    body = body[body.index('{') + 1:body.index('};')]
    return re.findall(r'"([^"]*)"', body)


def style_glyphs():   # name -> glyph for every pad input in both pad styles and every key
    out = {}
    for name in c_names('pad_names'):
        if name in DPAD:
            out['x_' + name] = arrow_glyph(DPAD[name])
            out['p_' + name] = arrow_glyph(DPAD[name])
        elif name == 'start':
            out['x_' + name], out['p_' + name] = lines_glyph(), lines_glyph()
        elif name == 'back':
            out['x_' + name], out['p_' + name] = squares_glyph(), squares_glyph()
        else:
            out['x_' + name] = ring_text(XBOX[name])
            out['p_' + name] = ps_glyph(PS_SHAPES[name]) if name in PS_SHAPES else ring_text(PS[name])
    for name in c_names('key_names'):
        if name in ('up', 'down', 'left', 'right'):
            g = keycap_arrow(name)
        elif name.startswith('mouse_'):
            g = mouse_glyph(name[6:])
        elif name.startswith('num') and name[3:].isdigit():
            g = keycap_glyph('N' + name[3:])
        elif len(name) == 1 or (name[0] == 'f' and name[1:].isdigit()):
            g = keycap_glyph(name.upper())
        else:
            g = keycap_glyph(KEY_TEXT[name])
        out['k_' + name] = g
    return out


# Nintendo extension font layout as homebrew documents it: E0E0.. buttons, E0EA.. d-pad, E0EF/E0F0 plus/minus; the
# E0A0 block repeats the buttons in another style. E0E1 (B) was confirmed in game with --probe ("Turbo-Aufladen" tip).
BUTTONS = {0: 'A', 1: 'B', 2: 'X', 3: 'Y', 4: 'L', 5: 'R', 6: 'ZL', 7: 'ZR', 8: 'SL', 9: 'SR', 0xF: '+', 0x10: '-'}
ARROWS = {0xA: 'up', 0xB: 'down', 0xC: 'left', 0xD: 'right'}
STICKS = {0xE101: 'L', 0xE102: 'R'}
# The Switch button (runtime/input_map.h B_*) each code offset or stick code stands for; SL and SR have none.
B_IDS = {'A': 0, 'B': 1, 'X': 2, 'Y': 3, 'L': 6, 'R': 7, 'ZL': 8, 'ZR': 9, '+': 10, '-': 11, 'left': 12, 'up': 13,
         'right': 14, 'down': 15}
STICK_IDS = {0xE101: 4, 0xE102: 5}


def button_of(code):   # the B_* id a code stands for, or None
    for base in (0xE0E0, 0xE0A0):
        k = code - base
        if k in BUTTONS:
            return B_IDS.get(BUTTONS[k])
        if k in ARROWS:
            return B_IDS[ARROWS[k]]
    return STICK_IDS.get(code)


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
    slots = {}   # code -> B_* id: drawn as a composite of one component
    for c in codes:
        g = None if args.probe else button_glyph(c)
        if g is not None and button_of(c) is not None:
            names.append(f's{c:04X}')   # the Switch glyph, which the code's composite shows by default
            glyphs[f's{c:04X}'] = g
            pen = TTGlyphPen(None)
            pen.addComponent(f's{c:04X}', (1, 0, 0, 1, 0, 0))
            g = pen.glyph()
            slots[c] = button_of(c)
        glyphs[f'u{c:04X}'] = g if g is not None else probe_glyph(c)
    styles = {} if args.probe else style_glyphs()
    names += list(styles)
    glyphs.update(styles)
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
    write_map(args.out / '5.ttf', slots, list(styles))
    print('wrote', args.out / '5.ttf', 'probe' if args.probe else f'{len(slots)} slots, {len(styles)} style glyphs')


def write_map(path, slots, style_names):   # <font>.map beside the font: see the module docstring
    font = TTFont(str(path))
    glyf_at = font.reader.tables['glyf'].offset
    loca = font['loca']
    data = path.read_bytes()
    lines = []
    for code, button in sorted(slots.items()):
        gid = font.getGlyphID(f'u{code:04X}')
        at = glyf_at + loca[gid]
        if struct.unpack('>h', data[at:at + 2])[0] != -1:
            raise SystemExit(f'u{code:04X} is not a composite glyph in {path}')
        lines.append(f'slot {code:04X} {button} {at + 12}')   # header 10 bytes, flags 2, then the glyph index
    for name in style_names:
        lines.append(f'glyph {name} {font.getGlyphID(name)}')
    path.with_suffix('.map').write_text(chr(10).join(lines) + chr(10), encoding='ascii', newline=chr(10))


if __name__ == '__main__':
    main()
