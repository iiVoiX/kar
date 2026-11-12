"""Write tools/data/expected-<name>.json for a Donkey Kong Bananza ExeFS: hashes and title id only, no game data.

    .venv\\Scripts\\python tools\\dkb_expected.py --exefs build\\extracted\\dkb100\\exefs --name dkb100 --display 1.0.0
"""
import argparse
import hashlib
import json
import struct
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent


def title_id(npdm):
    """Program id from the ACI0 block of main.npdm."""
    data = npdm.read_bytes()
    if data[:4] != b'META':
        raise SystemExit(f'{npdm}: not an NPDM')
    off, size = struct.unpack_from('<II', data, 0x70)
    aci = data[off:off + size]
    if aci[:4] != b'ACI0':
        raise SystemExit(f'{npdm}: no ACI0 block')
    return f'{struct.unpack_from("<Q", aci, 0x10)[0]:016X}'


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument('--exefs', type=Path, required=True)
    ap.add_argument('--name', required=True, help='build version name, e.g. dkb100')
    ap.add_argument('--display', required=True, help='display version, e.g. 1.0.0')
    ap.add_argument('--game', default='Donkey Kong Bananza', help='game name for the comment line')
    args = ap.parse_args()
    files = sorted(p for p in args.exefs.iterdir() if p.is_file() and not p.name.startswith('.'))
    out = {
        '_comment': f'{args.game} {args.display} ExeFS. Hashes only; no game data.',
        'title_id': title_id(args.exefs / 'main.npdm'),
        'display_version': args.display,
        'exefs_sha256': {p.name: hashlib.sha256(p.read_bytes()).hexdigest() for p in files},
    }
    dst = ROOT / 'tools' / 'data' / f'expected-{args.name}.json'
    dst.write_text(json.dumps(out, indent=2) + '\n', encoding='utf-8')
    print(dst, out['title_id'], ', '.join(out['exefs_sha256']))


if __name__ == '__main__':
    main()
