"""Draws runtime/kar.ico, the icon of the Kirby Air Riders build: a yellow five-pointed star with a pink speed trail on a
sky-blue disc. Original artwork made from geometric shapes only (no game art), at 16 to 256 px.

    .venv\\Scripts\\python tools\\make_kar_icon.py
"""
import math
from pathlib import Path

from PIL import Image, ImageDraw, ImageFilter

ROOT = Path(__file__).resolve().parent.parent
SIZE = 1024   # drawn large, then downsampled per icon size


def star(cx, cy, r_out, r_in, rot):
    pts = []
    for i in range(10):
        r = r_out if i % 2 == 0 else r_in
        a = rot + i * math.pi / 5
        pts.append((cx + r * math.cos(a), cy + r * math.sin(a)))
    return pts


def draw():
    img = Image.new('RGBA', (SIZE, SIZE), (0, 0, 0, 0))
    d = ImageDraw.Draw(img)
    # sky disc with a darker rim
    d.ellipse((24, 24, SIZE - 24, SIZE - 24), fill=(38, 120, 214, 255))
    d.ellipse((64, 64, SIZE - 64, SIZE - 64), fill=(92, 176, 246, 255))
    # light streaks behind the star
    trail = Image.new('RGBA', (SIZE, SIZE), (0, 0, 0, 0))
    t = ImageDraw.Draw(trail)
    for k, (w, col) in enumerate(((110, (255, 120, 190, 235)), (70, (255, 255, 255, 220)), (40, (255, 214, 90, 230)))):
        off = (k - 1) * 120
        t.polygon([(140, 640 + off), (560, 470 + off - w // 2), (560, 470 + off + w // 2), (140, 640 + off + 20)], fill=col)
    trail = trail.filter(ImageFilter.GaussianBlur(6))
    img.alpha_composite(trail)
    # the star: dark outline, yellow body, light highlight
    rot = -math.pi / 2 - 0.25
    d = ImageDraw.Draw(img)
    d.polygon(star(610, 470, 330, 140, rot), fill=(120, 70, 10, 255))
    d.polygon(star(610, 470, 290, 120, rot), fill=(255, 208, 40, 255))
    d.polygon(star(585, 440, 150, 62, rot), fill=(255, 240, 150, 255))
    # clip everything to the outer disc
    mask = Image.new('L', (SIZE, SIZE), 0)
    ImageDraw.Draw(mask).ellipse((24, 24, SIZE - 24, SIZE - 24), fill=255)
    out = Image.new('RGBA', (SIZE, SIZE), (0, 0, 0, 0))
    out.paste(img, (0, 0), mask)
    return out


def main():
    big = draw()
    sizes = [(s, s) for s in (16, 24, 32, 48, 64, 128, 256)]
    big.resize((256, 256), Image.LANCZOS).save(ROOT / 'runtime' / 'kar.ico', sizes=sizes)
    big.resize((256, 256), Image.LANCZOS).save(ROOT / 'build' / 'kar_icon_preview.png')
    print('wrote runtime/kar.ico')


if __name__ == '__main__':
    main()
