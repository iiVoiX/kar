"""Which nn:: functions a module imports, and how many of them a host override answers by name.

The ratio is not a crash list: lifted SDK code runs the rest, which works for os, movie, audio and the like and only
fails when it reaches an IPC service the host does not serve (runtime/ipc.c, sm_handler). Use it to find features
that are stubbed or never exercised (RELEASE_PLAN 3.11).

    .venv/Scripts/python tools/kar_service_audit.py build/extracted/kar458752/exefs/main [namespace]

With a namespace, lists that namespace's imports and marks the overridden ones.
"""
import collections
import pathlib
import re
import sys

ROOT = pathlib.Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / "recomp"))
from nso import Nso  # noqa: E402


def overrides():
    names = set()
    for f in (ROOT / "runtime").rglob("*.c"):
        names.update(re.findall(r'HOST_OVERRIDE\("([^"]+)"', f.read_text(encoding="utf-8", errors="ignore")))
    return names


def namespace(sym):
    m = re.match(r"_ZN(?:K)?2nn(\d+)", sym)
    return sym[m.end():m.end() + int(m.group(1))] if m else None


def main():
    imports = [s[0] for s in Nso(sys.argv[1]).imports()]
    ovr = overrides()
    if len(sys.argv) > 2:
        for s in sorted(s for s in imports if namespace(s) == sys.argv[2]):
            print(("ovr  " if s in ovr else "     ") + s)
        return
    total, covered = collections.Counter(), collections.Counter()
    for s in imports:
        ns = namespace(s)
        if ns:
            total[ns] += 1
            covered[ns] += s in ovr
    for ns, n in total.most_common():
        print(f"{ns:12} {covered[ns]:3}/{n:3}")


if __name__ == "__main__":
    main()
