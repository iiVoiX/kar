"""Guest address layout of the loaded modules, computed from their sizes.

Guest addresses equal host addresses, and the lifter writes absolute addresses into the generated C, so the
generator and the runtime must agree on every module base. This is the single place that decides them: the generator
calls layout() and writes the result into generated/<version>/layout.h and layout.json, which the runtime and the
build read.

Modules are placed in Horizon's load order (rtld is dropped; the host links the modules):
main, subsdk0..9, sdk. Each base is aligned to 2 MiB. Everything must stay below 2^39, the limit of the addresses
IPC buffers can encode.
"""
import json
from pathlib import Path

from nso import Nso

FIRST_BASE = 0x7000000000
ALIGN = 0x200000
ADDRESS_LIMIT = 1 << 39
ORDER = ['main'] + [f'subsdk{i}' for i in range(10)] + ['sdk']


def align_up(v, a=ALIGN):
    return (v + a - 1) & ~(a - 1)


def layout(exefs):
    """[(name, base, size)] for the modules present in the ExeFS directory, in load order."""
    exefs = Path(exefs)
    out, base = [], FIRST_BASE
    for name in ORDER:
        path = exefs / name
        if not path.exists():
            continue
        size = Nso(str(path)).size
        out.append((name, base, size))
        base = align_up(base + size)
    if base > ADDRESS_LIMIT:
        raise ValueError(f'modules end at {base:#x}, above 2^39')
    return out


def layout_nro(mods, nros):
    """[(name, base, size)] for NROs the game maps at run time through nn::ro, placed after the ExeFS modules in the
    order given. nros: [(name, path)]. The names become C identifiers (MOD_<NAME>_BASE)."""
    base = align_up(max((b + s for _, b, s in mods), default=FIRST_BASE))
    out = []
    for name, path in nros:
        if not name.isidentifier():
            raise ValueError(f'NRO name {name!r} is not a C identifier')
        size = Nso(str(path)).size
        out.append((name, base, size))
        base = align_up(base + size)
    if base > ADDRESS_LIMIT:
        raise ValueError(f'NROs end at {base:#x}, above 2^39')
    return out


def write_nro(nros, paths, outdir):
    """nro_layout.json and nro_layout.h, apart from layout.json/layout.h: the runtime loads the ExeFS modules from
    MOD_LIST at boot, while an NRO is mapped only when the game calls nn::ro::LoadModule (NRO_LIST)."""
    outdir = Path(outdir)
    outdir.mkdir(parents=True, exist_ok=True)
    (outdir / 'nro_layout.json').write_text(json.dumps(
        [{'name': n, 'base': f'{b:#x}', 'size': f'{s:#x}', 'romfs_name': Path(paths[n]).name} for n, b, s in nros],
        indent=1) + '\n')
    lines = ['// NRO layout chosen by recomp/layout.py. Generated; do not edit.', '#pragma once', '']
    for n, b, s in nros:
        lines.append(f'#define MOD_{n.upper()}_BASE {b:#x}ull')
        lines.append(f'#define MOD_{n.upper()}_SIZE {s:#x}ull')
    lines += ['', f'#define NRO_COUNT {len(nros)}',
              '#define NRO_LIST(X) ' + ' '.join(f'X({n}, {n.upper()})' for n, _, _ in nros), '']
    (outdir / 'nro_layout.h').write_text('\n'.join(lines))


def write(mods, outdir):
    """layout.json for Python tools and layout.h for the runtime."""
    outdir = Path(outdir)
    outdir.mkdir(parents=True, exist_ok=True)
    (outdir / 'layout.json').write_text(json.dumps(
        [{'name': n, 'base': f'{b:#x}', 'size': f'{s:#x}'} for n, b, s in mods], indent=1) + '\n')
    lines = ['// Module layout chosen by recomp/layout.py. Generated; do not edit.', '#pragma once', '']
    for n, b, s in mods:
        lines.append(f'#define MOD_{n.upper()}_BASE {b:#x}ull')
        lines.append(f'#define MOD_{n.upper()}_SIZE {s:#x}ull')
    lines += ['', f'#define MOD_COUNT {len(mods)}',
              '#define MOD_LIST(X) ' + ' '.join(f'X({n}, {n.upper()})' for n, _, _ in mods), '']
    (outdir / 'layout.h').write_text('\n'.join(lines))


def read(outdir):
    """[(name, base, size)] from a layout.json written by write()."""
    data = json.loads((Path(outdir) / 'layout.json').read_text())
    return [(m['name'], int(m['base'], 16), int(m['size'], 16)) for m in data]
