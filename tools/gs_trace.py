#!/usr/bin/env python3
"""Reads a PS2X_GS_RECORD trace (the fork's gs_trace.h format): VRAM and drawing state at the start, then the GS
commands (GIF packets, native image uploads). Follows the GIF tags to report image uploads (where they land and in
which format) and the textures (TEX0) the draws use: the data a native texture cache works from.

  tools/gs_trace.py <trace> uploads      every image upload: destination base, width, format, rectangle, bytes
  tools/gs_trace.py <trace> textures     every distinct TEX0 the draws set, with the uploads at its base

As a module: commands(path) yields (kind, registers, bytes); gif_writes(packet) yields ('reg', addr, value) and
('image', bytes) events.
"""
import collections
import struct
import sys

MAGIC = 0x31525347
PSM = {0: 'CT32', 1: 'CT24', 2: 'CT16', 10: 'CT16S', 19: 'T8', 20: 'T4', 27: 'T8H', 36: 'T4HL', 44: 'T4HH'}
REG_TEX0_1, REG_TEX0_2, REG_BITBLTBUF, REG_TRXPOS, REG_TRXREG, REG_TRXDIR = 0x06, 0x07, 0x50, 0x51, 0x52, 0x53


def commands(path):
    with open(path, 'rb') as f:
        magic, vram_size = struct.unpack('<2I', f.read(8))
        if magic != MAGIC:
            raise ValueError('not a GS trace')
        f.seek(vram_size, 1)
        (state_size,) = struct.unpack('<I', f.read(4))
        f.seek(state_size, 1)
        while True:
            head = f.read(1 + 32 + 4)
            if len(head) < 37:
                return
            kind = head[0]
            registers = struct.unpack_from('<4Q', head, 1)
            (size,) = struct.unpack_from('<I', head, 33)
            yield kind, registers, f.read(size)


def gif_writes(packet):
    """The register writes and image data of a GIF packet (PACKED A+D and REGLIST register writes, IMAGE data)."""
    i = 0
    while i + 16 <= len(packet):
        lo, hi = struct.unpack_from('<2Q', packet, i)
        i += 16
        nloop = lo & 0x7fff
        flg = (lo >> 58) & 3
        nreg = (lo >> 60) & 0xf or 16
        regs = [(hi >> (4 * k)) & 0xf for k in range(nreg)]
        if (lo >> 46) & 1:  # PRE: PRIM written from the tag
            yield ('reg', 0x00, (lo >> 47) & 0x7ff)
        if flg == 0:  # PACKED
            for _ in range(nloop):
                for reg in regs:
                    if i + 16 > len(packet):
                        return
                    data, addr = struct.unpack_from('<2Q', packet, i)
                    i += 16
                    if reg == 0xe:  # A+D
                        yield ('reg', addr & 0xff, data)
                    else:
                        yield ('reg', reg, data)
        elif flg == 1:  # REGLIST
            count = nloop * nreg
            for k in range(count):
                if i + 8 > len(packet):
                    return
                yield ('reg', regs[k % nreg], struct.unpack_from('<Q', packet, i)[0])
                i += 8
            if count & 1:
                i += 8
        else:  # IMAGE
            size = nloop * 16
            yield ('image', packet[i:i + size])
            i += size


def uploads(path):
    """Yields (bitbltbuf, trxpos, trxreg, data) per image upload; data is the raw pixels in the destination format."""
    state = {}
    for kind, registers, data in commands(path):
        if kind == 2:  # NativeImageUpload: BITBLTBUF, TRXPOS, TRXREG, TRXDIR in the registers
            yield registers[0], registers[1], registers[2], data
            continue
        pending = None
        for event in gif_writes(data):
            if event[0] == 'reg':
                state[event[1]] = event[2]
                if event[1] == REG_TRXDIR and (event[2] & 3) == 0:
                    pending = bytearray()
            elif pending is not None:
                pending += event[1]
                width, height = state.get(REG_TRXREG, 0) & 0xfff, (state.get(REG_TRXREG, 0) >> 32) & 0xfff
                bpp = {0: 32, 1: 24, 2: 16, 10: 16, 19: 8, 20: 4}.get((state.get(REG_BITBLTBUF, 0) >> 56) & 0x3f, 32)
                if len(pending) * 8 >= width * height * bpp:
                    yield state.get(REG_BITBLTBUF, 0), state.get(REG_TRXPOS, 0), state.get(REG_TRXREG, 0), bytes(pending)
                    pending = None


def describe_upload(bitbltbuf, trxpos, trxreg):
    dbp, dbw, dpsm = (bitbltbuf >> 32) & 0x3fff, (bitbltbuf >> 48) & 0x3f, (bitbltbuf >> 56) & 0x3f
    x, y = (trxpos >> 32) & 0x7ff, (trxpos >> 48) & 0x7ff
    w, h = trxreg & 0xfff, (trxreg >> 32) & 0xfff
    return dbp, dbw, PSM.get(dpsm, dpsm), x, y, w, h


def describe_tex0(tex0):
    return {'tbp0': tex0 & 0x3fff, 'tbw': (tex0 >> 14) & 0x3f, 'psm': PSM.get((tex0 >> 20) & 0x3f, (tex0 >> 20) & 0x3f),
            'tw': 1 << ((tex0 >> 26) & 0xf), 'th': 1 << ((tex0 >> 30) & 0xf), 'tcc': (tex0 >> 34) & 1,
            'tfx': (tex0 >> 35) & 3, 'cbp': (tex0 >> 37) & 0x3fff, 'cpsm': PSM.get((tex0 >> 51) & 0xf, (tex0 >> 51) & 0xf),
            'csm': (tex0 >> 55) & 1, 'csa': (tex0 >> 56) & 0x1f, 'cld': (tex0 >> 61) & 7}


if __name__ == '__main__':
    path, mode = sys.argv[1], sys.argv[2]
    if mode == 'uploads':
        count = collections.Counter()
        for bitbltbuf, trxpos, trxreg, data in uploads(path):
            d = describe_upload(bitbltbuf, trxpos, trxreg)
            count[(d[2], d[5], d[6])] += 1
            print(f'base {d[0]:5d} width {d[1]:2d} {d[2]:5s} at ({d[3]},{d[4]}) {d[5]}x{d[6]}  {len(data)} bytes')
        print('by format and size:', count.most_common(12))
    elif mode == 'textures':
        tex = collections.Counter()
        for kind, registers, data in commands(path):
            if kind == 2:
                continue
            for event in gif_writes(data):
                if event[0] == 'reg' and event[1] in (REG_TEX0_1, REG_TEX0_2):
                    tex[event[2] & ((1 << 61) - 1)] += 1  # without CLD
        for value, n in tex.most_common(30):
            print(f'{n:5d} x {describe_tex0(value)}')
        print(len(tex), 'distinct TEX0')
