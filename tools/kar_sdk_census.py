"""Census of a game's SDK boundary against this runtime: what the modules import, who exports it, what the host replaces.

    .venv\\Scripts\\python tools\\kar_sdk_census.py --exefs build\\extracted\\kar458752\\exefs --out build\\kar458752-census.txt

The full report (symbol names only, no code or data) goes to --out under build/, which git ignores; a summary prints.
It answers the first porting question for a new game: which of the runtime's host functions still find their symbol in
this game's sdk, which nn:: namespaces the game calls that the runtime never replaces, which IPC services main.npdm
grants, and what the extra modules (subsdk*) are.
"""
import argparse
import re
import struct
import sys
from collections import Counter, defaultdict
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / 'recomp'))
from nso import Nso  # noqa: E402

HOST_PATTERNS = [
    re.compile(r'HOST_OVERRIDE\(\s*"([^"]+)"'),
    re.compile(r'HOST_OBSERVER\(\s*"([^"]+)"'),
    re.compile(r'hostfn_register(?:_observer)?\(\s*"([^"]+)"'),
    re.compile(r'hostfn_add_override\(\s*"([^"]+)"'),
]


def host_names():
    """Symbol names the runtime replaces or observes, found by scanning runtime/ for the registration forms."""
    names = defaultdict(set)
    for p in sorted((ROOT / 'runtime').rglob('*')):
        if p.suffix not in ('.c', '.h', '.cpp', '.inc') or 'third_party' in p.parts:
            continue
        text = p.read_text(encoding='utf-8', errors='replace')
        for pat in HOST_PATTERNS:
            for m in pat.finditer(text):
                names[m.group(1)].add(p.name)
    return names


def namespace(sym):
    """Short group for a symbol: nn::fs, nn::os, nvn, cuda, C library and so on."""
    m = re.match(r'_ZN(?:K)?2nn(\d+)', sym)
    if m:
        n = int(m.group(1))
        start = m.end()
        return 'nn::' + sym[start:start + n]
    m = re.match(r'_ZN(?:K)?(\d+)', sym)
    if m:
        n = int(m.group(1))
        return sym[m.end():m.end() + n] + '::'
    for prefix in ('nvn', 'cu', 'NVSDK', 'nnMain', 'nninit', 'glslc', 'pthread', '__cxa', '__gxx', '_Unwind'):
        if sym.startswith(prefix):
            return prefix
    return '(C/other)'


def services(npdm_path):
    """Service names in main.npdm's ACI0 service access control, and whether each is a client or a server entry."""
    data = npdm_path.read_bytes()
    off, size = struct.unpack_from('<II', data, 0x70)
    aci = data[off:off + size]
    sac_off, sac_size = struct.unpack_from('<II', aci, 0x28)
    sac = aci[sac_off:sac_off + sac_size]
    out, i = [], 0
    while i < len(sac):
        ctrl = sac[i]
        n = (ctrl & 7) + 1
        out.append((sac[i + 1:i + 1 + n].decode('ascii', 'replace'), bool(ctrl & 0x80)))
        i += 1 + n
    return out


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument('--exefs', type=Path, required=True)
    ap.add_argument('--out', type=Path, required=True, help='full report; keep it under build/')
    args = ap.parse_args()

    mods = {}
    for name in ('main', 'sdk', 'subsdk0', 'subsdk1', 'subsdk2', 'subsdk3'):
        p = args.exefs / name
        if p.is_file():
            mods[name] = Nso(p)

    exporter = {}
    for mname, m in mods.items():
        for s in m.exports():
            exporter.setdefault(s[0], mname)

    host = host_names()
    lines = []
    w = lines.append

    w('== Modules')
    for mname, m in mods.items():
        w(f'{mname:8} build {m.build_id[:16]}  imports {len(m.imports()):6}  exports {len(m.exports()):6}  '
          f'needed {m.needed}')

    w('\n== What each subsdk exports (top namespaces)')
    for mname, m in mods.items():
        if not mname.startswith('subsdk'):
            continue
        c = Counter(namespace(s[0]) for s in m.exports())
        w(f'{mname}: ' + ', '.join(f'{k} {v}' for k, v in c.most_common(8)))
        for s in sorted(m.exports())[:15]:
            w(f'    {s[0]}')

    w('\n== Imports by providing module')
    unresolved = []
    by_provider = defaultdict(lambda: defaultdict(set))
    for mname, m in mods.items():
        for s in m.imports():
            prov = exporter.get(s[0])
            if prov is None:
                unresolved.append((mname, s[0]))
            else:
                by_provider[mname][prov].add(s[0])
    for mname, provs in by_provider.items():
        w(f'{mname}: ' + ', '.join(f'from {p} {len(v)}' for p, v in sorted(provs.items())))
    w(f'unresolved imports (weak or provided by the host): {len(unresolved)}')
    for mname, s in sorted(unresolved):
        w(f'    {mname}: {s}  {"[host]" if s in host else ""}')

    # Everything any game module takes from sdk: the SDK surface the port must run, lifted or replaced.
    sdk_used = set()
    for mname, provs in by_provider.items():
        if mname != 'sdk':
            sdk_used |= provs.get('sdk', set())

    w('\n== SDK functions the game calls, by namespace (replaced by the host / total)')
    groups = defaultdict(list)
    for s in sdk_used:
        groups[namespace(s)].append(s)
    rows = []
    for ns, syms in groups.items():
        covered = sum(1 for s in syms if s in host)
        rows.append((ns, covered, len(syms)))
    for ns, covered, total in sorted(rows, key=lambda r: (-r[2], r[0])):
        w(f'{ns:32} {covered:4} / {total:4}{"   <- no host code" if covered == 0 else ""}')

    w('\n== Runtime host functions whose symbol this game no longer has')
    missing = sorted(n for n in host if n not in exporter and n not in {s for _, s in unresolved})
    w(f'{len(missing)} of {len(host)} (renamed or removed in this SDK, or specific to another game):')
    for n in missing:
        w(f'    {n}   ({", ".join(sorted(host[n]))})')

    w('\n== Runtime host functions this game does use')
    used_host = sorted(n for n in host if n in sdk_used or n in {s for _, s in unresolved})
    w(f'{len(used_host)}')

    w('\n== IPC services granted in main.npdm')
    svcs = services(args.exefs / 'main.npdm')
    for name, server in svcs:
        w(f'    {name}{"  (server)" if server else ""}')

    args.out.parent.mkdir(parents=True, exist_ok=True)
    args.out.write_text('\n'.join(lines) + '\n', encoding='utf-8')

    print(f'report: {args.out}')
    print(f'host functions in runtime: {len(host)}; used by this game: {len(used_host)}; '
          f'symbol gone: {len(missing)}')
    print(f'SDK functions the game calls: {len(sdk_used)} in {len(groups)} namespaces; '
          f'namespaces with no host code: {sum(1 for r in rows if r[1] == 0)}')
    print(f'unresolved imports: {len(unresolved)}; services granted: {len(svcs)}')


if __name__ == '__main__':
    main()
