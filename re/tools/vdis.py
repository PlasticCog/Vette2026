"""Quick real-mode disassembler for the unpacked VETTE.EXE (load segment 0).

Usage:
  python vdis.py SEG:OFF [count]        disassemble `count` instructions (default 40)
  python vdis.py --find HEXBYTES        list seg-agnostic linear offsets of a byte pattern

Addresses are image-relative: linear = SEG*16 + OFF, with the MZ header stripped.
"""
import re
import struct
import sys
from pathlib import Path

from capstone import Cs, CS_ARCH_X86, CS_MODE_16

EXE = Path(__file__).resolve().parents[1] / 'bin' / 'VETTE_unpacked.exe'


def load_image(path=EXE):
    data = path.read_bytes()
    hdr = struct.unpack_from('<14H', data, 0)
    return data[hdr[4] * 16:]


def disasm(image, seg, off, count=40):
    md = Cs(CS_ARCH_X86, CS_MODE_16)
    base = seg * 16
    code = image[base + off:base + off + count * 8]
    lines = []
    for ins in md.disasm(code, off):
        ops = re.sub(r'0xffff([0-9a-f]{4})', r'0x', ins.op_str)  # near targets wrap at 64K
        lines.append(f'{seg:04X}:{ins.address:04X}  {ins.bytes.hex():<14} {ins.mnemonic} {ops}')
        if len(lines) >= count:
            break
    return lines


def main(argv):
    image = load_image()
    if argv[0] == '--find':
        pat = bytes.fromhex(argv[1])
        i = image.find(pat)
        while i >= 0:
            print(f'{i:05X}')
            i = image.find(pat, i + 1)
        return
    seg, off = (int(x, 16) for x in argv[0].split(':'))
    count = int(argv[1]) if len(argv) > 1 else 40
    print('\n'.join(disasm(image, seg, off, count)))


if __name__ == '__main__':
    main(sys.argv[1:])
