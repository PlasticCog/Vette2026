"""Unpack a Microsoft EXEPACK-compressed DOS MZ executable.

Usage: python unexepack.py <packed.exe> <out.exe>

Format reference: https://www.bamsoftware.com/software/exepack/
"""
import struct
import sys


def unpack(data: bytes) -> bytes:
    (magic, cblp, cp, crlc, cparhdr, minalloc, maxalloc,
     ss, sp, csum, ip, cs, lfarlc, ovno) = struct.unpack_from('<14H', data, 0)
    if magic != 0x5A4D:
        raise ValueError('not an MZ executable')
    file_len = cp * 512 - (512 - cblp if cblp else 0)
    hdr_len = cparhdr * 16
    image = data[hdr_len:file_len]

    ep_off = cs * 16  # EXEPACK header lives at CS:0000
    if ip == 0x10:
        fields = struct.unpack_from('<8H', image, ep_off)
        real_ip, real_cs, mem_start, ep_size, real_sp, real_ss, dest_len, sig = fields
        skip_len = 1
    elif ip == 0x12:
        fields = struct.unpack_from('<9H', image, ep_off)
        real_ip, real_cs, mem_start, ep_size, real_sp, real_ss, dest_len, skip_len, sig = fields
    else:
        raise ValueError(f'unexpected EXEPACK header size (IP={ip:#x})')
    if sig != 0x4252:
        raise ValueError('EXEPACK "RB" signature not found')

    comp_len = ep_off - (skip_len - 1) * 16
    buf = bytearray(max(dest_len * 16, comp_len))
    buf[:comp_len] = image[:comp_len]

    # Decompression runs backwards. Trailing 0xFF padding (up to 15 bytes) is skipped.
    src = comp_len - 1
    for _ in range(16):
        if buf[src] != 0xFF:
            break
        src -= 1
    dst = dest_len * 16 - 1

    while True:
        cmd = buf[src]; src -= 1
        length = buf[src] << 8 | buf[src - 1]; src -= 2
        op = cmd & 0xFE
        if op == 0xB0:  # fill
            val = buf[src]; src -= 1
            for _ in range(length):
                buf[dst] = val; dst -= 1
        elif op == 0xB2:  # copy
            for _ in range(length):
                buf[dst] = buf[src]; dst -= 1; src -= 1
        else:
            raise ValueError(f'bad EXEPACK command {cmd:#x} at {src + 1:#x}')
        if cmd & 1:
            break
    out_image = bytes(buf[:dest_len * 16])

    # Packed relocation table follows the decompressor stub and its error message.
    stub = image[ep_off:ep_off + ep_size]
    msg = stub.find(b'Packed file is corrupt')
    if msg < 0:
        raise ValueError('could not locate packed relocation table')
    pos = msg + len(b'Packed file is corrupt')
    relocs = []
    for seg_index in range(16):
        count, = struct.unpack_from('<H', stub, pos); pos += 2
        for _ in range(count):
            off, = struct.unpack_from('<H', stub, pos); pos += 2
            relocs.append((off, seg_index * 0x1000))

    # Build a fresh MZ. Memory need is preserved: packed load size + minalloc.
    packed_paras = (len(image) + 15) // 16
    new_hdr_len = (0x1C + len(relocs) * 4 + 511) // 512 * 512
    new_cparhdr = new_hdr_len // 16
    total = new_hdr_len + len(out_image)
    new_cp = (total + 511) // 512
    new_cblp = total % 512
    new_min = max(0, packed_paras + minalloc - dest_len)
    hdr = bytearray(new_hdr_len)
    struct.pack_into('<14H', hdr, 0, 0x5A4D, new_cblp, new_cp, len(relocs), new_cparhdr,
                     new_min, maxalloc, real_ss, real_sp, 0, real_ip, real_cs, 0x1C, 0)
    for i, (off, seg) in enumerate(relocs):
        struct.pack_into('<HH', hdr, 0x1C + i * 4, off, seg)

    info = dict(real_cs=real_cs, real_ip=real_ip, real_ss=real_ss, real_sp=real_sp,
                image_len=len(out_image), relocs=len(relocs), minalloc=new_min)
    return bytes(hdr) + out_image, info


if __name__ == '__main__':
    src_path, dst_path = sys.argv[1], sys.argv[2]
    out, info = unpack(open(src_path, 'rb').read())
    open(dst_path, 'wb').write(out)
    print(' '.join(f'{k}={v:#x}' for k, v in info.items()))
