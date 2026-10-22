#!/usr/bin/env python3
"""List every file of an ISO9660 image with its start LBN and size.

usage: tools/iso_lbn.py <image.iso> [lbn_hex ...]
With LBNs given, print only the file that contains each one.
"""
import struct
import sys

SECTOR = 2048


def read_dir(f, lbn, size, prefix, out):
    f.seek(lbn * SECTOR)
    data = f.read(size)
    pos = 0
    while pos < len(data):
        length = data[pos]
        if length == 0:
            pos = (pos // SECTOR + 1) * SECTOR
            continue
        rec = data[pos:pos + length]
        ext = struct.unpack_from('<I', rec, 2)[0]
        sz = struct.unpack_from('<I', rec, 10)[0]
        flags = rec[25]
        name_len = rec[32]
        name = rec[33:33 + name_len]
        pos += length
        if name in (b'\x00', b'\x01'):
            continue
        name = name.decode('ascii', 'replace').split(';')[0]
        path = prefix + name
        if flags & 2:
            read_dir(f, ext, sz, path + '/', out)
        else:
            out.append((ext, sz, path))


def main():
    if len(sys.argv) < 2:
        print(__doc__)
        return 1
    with open(sys.argv[1], 'rb') as f:
        f.seek(16 * SECTOR)
        pvd = f.read(SECTOR)
        root = pvd[156:156 + 34]
        files = []
        read_dir(f, struct.unpack_from('<I', root, 2)[0], struct.unpack_from('<I', root, 10)[0], '', files)
    files.sort()
    if len(sys.argv) == 2:
        for lbn, sz, path in files:
            print(f'0x{lbn:06x} {sz:10d} {path}')
        return 0
    for arg in sys.argv[2:]:
        want = int(arg, 16)
        hit = [(l, s, p) for l, s, p in files if l <= want < l + (s + SECTOR - 1) // SECTOR]
        for l, s, p in hit:
            print(f'0x{want:x} -> {p} (start 0x{l:x}, +{want - l} sectors of {(s + SECTOR - 1) // SECTOR})')
        if not hit:
            print(f'0x{want:x} -> not inside any file')
    return 0


if __name__ == '__main__':
    sys.exit(main())
