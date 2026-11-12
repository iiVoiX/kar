"""Disassemble a range of a lifted module with call targets named, for reading boot crashes.

    .venv\\Scripts\\python tools\\kar_dis.py --exefs build\\extracted\\kar458752\\exefs main 0xe43f80 0xe44000

Addresses are module offsets as the runtime prints them (main+0x...). A `bl` is named from the module's own exported
symbols, or through its PLT stub and GOT relocation for an import. Prints nothing game-derived to files.
"""
import argparse
import sys
from pathlib import Path

import capstone

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / 'recomp'))
from nso import Nso  # noqa: E402


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument('--exefs', type=Path, required=True)
    ap.add_argument('module')
    ap.add_argument('lo', type=lambda s: int(s, 0))
    ap.add_argument('hi', type=lambda s: int(s, 0))
    args = ap.parse_args()
    n = Nso(str(args.exefs / args.module))
    md = capstone.Cs(capstone.CS_ARCH_ARM64, capstone.CS_MODE_ARM)
    md.detail = True
    got = {r[0]: n.syms[r[2]][0] for r in n.relocs if r[1] in (1025, 1026, 257) and r[2] < len(n.syms)}
    own = {s[3]: s[0] for s in n.syms if s[0] and s[2]}

    def plt_name(t):
        ins = list(md.disasm(bytes(n.image[t:t + 12]), t))
        if len(ins) >= 2 and ins[0].mnemonic == 'adrp' and ins[1].mnemonic == 'ldr':
            return got.get(ins[0].operands[1].imm + ins[1].operands[1].mem.disp)
        return None

    for x in md.disasm(bytes(n.image[args.lo:args.hi]), args.lo):
        note = ''
        if x.mnemonic in ('bl', 'b') and x.operands and x.operands[0].type == capstone.arm64.ARM64_OP_IMM:
            t = x.operands[0].imm
            name = plt_name(t) or own.get(t)
            if name:
                note = f'   <- {name}'
        print(f'{x.address:#9x}: {x.mnemonic:7} {x.op_str}{note}')


if __name__ == '__main__':
    main()
