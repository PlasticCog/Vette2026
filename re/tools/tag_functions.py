"""Heuristic tags for functions in the Ghidra export (re/out/<program>/).

  python re/tools/tag_functions.py 3009:0EF5 4021:0ED1 ...   tag specific functions
  python re/tools/tag_functions.py --callees-of 3009:0025 --range 0135-061B
        tag every function called from a range of another function, in call order

Tags: EGA (ports 3C4/3CE), fps (reads frame_rate [2CD3]), DOS (INT 21h), mouse (INT 33h),
vbios (INT 10h), joy (port 201h), sfx (calls the speaker driver), serial, math (IMUL/IDIV).
"own" = the function itself, "deep" = added by callees up to 3 levels down.
"""
import argparse
import re
from pathlib import Path

OUT = Path(__file__).resolve().parents[1] / 'out' / 'VETTE_unpacked.exe'
INS = re.compile(r'^[0-9A-F]{4}:[0-9A-F]{4}  ')
CALL = re.compile(r'CALLF? ([0-9A-F]{4}:[0-9A-F]{4})')
CHECKS = [
    ('EGA', lambda s: re.search(r'0x3c[e4]\b', s)),
    ('fps', lambda s: '[0x2cd3]' in s),
    ('DOS', lambda s: 'INT 0x21' in s),
    ('mouse', lambda s: 'INT 0x33' in s),
    ('vbios', lambda s: 'INT 0x10' in s),
    ('joy', lambda s: re.search(r'0x201\b', s)),
    ('sfx', lambda s: 'CALL 3009:933E' in s or 'CALL 3009:9352' in s),
    ('serial', lambda s: '0x3f8' in s or 'INT 0x14' in s),
    ('math', lambda s: re.search(r'IMUL|IDIV', s)),
]


def load():
    funcs = {}
    for f in (OUT / 'funcs').iterdir():
        addr = f.name[:9].replace('_', ':')
        funcs[addr] = [l for l in f.read_text().splitlines() if INS.match(l)]
    return funcs


def own_tags(funcs, a):
    text = '\n'.join(funcs.get(a, []))
    return {name for name, check in CHECKS if check(text)}


def deep_tags(funcs, a, depth=3, seen=None):
    seen = set() if seen is None else seen
    if a in seen or depth < 0:
        return set()
    seen.add(a)
    tags = own_tags(funcs, a)
    for line in funcs.get(a, []):
        m = CALL.search(line)
        if m:
            tags |= deep_tags(funcs, m.group(1), depth - 1, seen)
    return tags


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('addrs', nargs='*')
    ap.add_argument('--callees-of')
    ap.add_argument('--range', help='hex offset range within --callees-of, e.g. 0135-061B')
    args = ap.parse_args()
    funcs = load()
    addrs = list(args.addrs)
    if args.callees_of:
        lo, hi = (int(x, 16) for x in (args.range or '0000-FFFF').split('-'))
        for line in funcs[args.callees_of]:
            m = CALL.search(line)
            if m and lo <= int(line[5:9], 16) <= hi:
                addrs.append(m.group(1))
    for a in addrs:
        own = own_tags(funcs, a)
        deep = deep_tags(funcs, a) - own
        print(f'{a} {len(funcs.get(a, [])):4} ins  own:{",".join(sorted(own)) or "-":22} deep:{",".join(sorted(deep))}')


if __name__ == '__main__':
    main()
