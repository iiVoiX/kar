"""Finds the GPU-made textures whose content differs in a broken run (vk_dump images.txt with TOTK_DUMP_TEX=gpu).

A texture the game computes once (at load) has the same content in every good run, whatever the race position. This
lists, per size and format, the content hashes found in all good runs that the bad run lacks, and the textures of the
bad run with that size and format, with the first pass that sampled them.
    python tools/kar_dump_diff.py --good <dump dir> <dump dir> --bad <dump dir>
"""
import argparse
import collections
import re
from pathlib import Path

LINE = re.compile(r'^pass_(\d+)_(\w+?)_(\d+x\d+x\d+)_(\d+)_(\w+) .* hash ([0-9a-f]{16})$')


def load(d):
    by_kind = collections.defaultdict(dict)   # (size, format) -> {hash: (first pass, name)}
    for line in (Path(d) / 'images.txt').read_text(errors='replace').splitlines():
        m = LINE.match(line.strip())
        if not m:
            continue
        pas, label, size, fmt_code, fmt, h = m.groups()
        key = (size, fmt)
        if h not in by_kind[key] or int(pas) < by_kind[key][h][0]:
            by_kind[key][h] = (int(pas), f'{label}')
    return by_kind


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--good', nargs='+', required=True)
    ap.add_argument('--bad', required=True)
    a = ap.parse_args()
    goods = [load(g) for g in a.good]
    bad = load(a.bad)
    found = 0
    for key in sorted(set().union(*[g.keys() for g in goods])):
        common = set.intersection(*[set(g.get(key, {})) for g in goods])
        if not common:
            continue
        missing = common - set(bad.get(key, {}))
        if not missing:
            continue
        found += 1
        size, fmt = key
        print(f'{size} {fmt}: {len(missing)} of {len(common)} stable contents missing in the bad run')
        for h in sorted(missing):
            print(f'    good: hash {h} first pass {goods[0][key][h][0]} ({goods[0][key][h][1]})')
        for h, (pas, label) in sorted(bad.get(key, {}).items(), key=lambda kv: kv[1][0])[:6]:
            print(f'    bad:  hash {h} first pass {pas} ({label})')
    print(f'{found} size/format groups differ')


if __name__ == '__main__':
    main()
