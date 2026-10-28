#!/usr/bin/env python3
"""Decodes TMB's textures from a dump of EE memory (TMB_DUMP_RDRAM, build/rdram.bin) into PNG files: the native PC
port's texture data, read where the game keeps it instead of from the GS.

  tools/tmb_textures.py <rdram.bin> list                 the texture table: each texture's uploads
  tools/tmb_textures.py <rdram.bin> png <out-dir> [ids]   the textures as PNG

TMB's texture table is at 0x4D4930, 16 bytes per texture number (texmGetTextureDMATagAddress): the first word points
at the texture's data, whose DMA chain starts 16 bytes further. The chain carries GIF packets uploading the pixels
(BITBLTBUF, TRXPOS, TRXREG, TRXDIR, then IMAGE data) and the palette (CLUT) in PS2 formats.
"""
import os
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import gs_trace as G

TABLE = 0x4D4930
RAM = 32 * 1024 * 1024


def u32(mem, addr):
    return struct.unpack_from('<I', mem, addr & (RAM - 1))[0]


def chain(mem, addr, limit=256):
    """The payload of a DMA source chain (cnt, next, ref, refe, end tags) starting at addr, as one bytes object."""
    out = bytearray()
    for _ in range(limit):
        lo, hi = struct.unpack_from('<2Q', mem, addr & (RAM - 1))
        qwc, tag_id, ref = lo & 0xffff, (lo >> 28) & 7, (lo >> 32) & 0x7fffffff
        if tag_id == 0:  # refe: data at ADDR, then end
            out += mem[ref & (RAM - 1):(ref & (RAM - 1)) + qwc * 16]
            return bytes(out)
        if tag_id == 1:  # cnt: data follows the tag
            out += mem[(addr + 16) & (RAM - 1):((addr + 16) & (RAM - 1)) + qwc * 16]
            addr += 16 + qwc * 16
        elif tag_id == 2:  # next: data follows, then ADDR
            out += mem[(addr + 16) & (RAM - 1):((addr + 16) & (RAM - 1)) + qwc * 16]
            addr = ref
        elif tag_id == 3 or tag_id == 4:  # ref / refs: data at ADDR, then the next tag
            out += mem[ref & (RAM - 1):(ref & (RAM - 1)) + qwc * 16]
            addr += 16
        elif tag_id == 7 or tag_id == 6:  # end / ret: data follows, then stop (TMB's chains end in a ret)
            out += mem[(addr + 16) & (RAM - 1):((addr + 16) & (RAM - 1)) + qwc * 16]
            return bytes(out)
        else:
            return bytes(out)
    return bytes(out)


def texture_uploads(mem, tex_id):
    """[(dbp, dbw, psm, x, y, w, h, pixels)] uploaded by texture tex_id's chain."""
    data = u32(mem, TABLE + tex_id * 16)
    if not data or data >= RAM:
        return []
    payload = chain(mem, data + 0x10)
    state, result, pending = {}, [], None
    for event in G.gif_writes(payload):
        if event[0] == 'reg':
            state[event[1]] = event[2]
            if event[1] == G.REG_TRXDIR and (event[2] & 3) == 0:
                pending = bytearray()
        elif pending is not None:
            pending += event[1]
            dbp, dbw, psm, x, y, w, h = G.describe_upload(state.get(G.REG_BITBLTBUF, 0), state.get(G.REG_TRXPOS, 0),
                                                         state.get(G.REG_TRXREG, 0))
            bpp = {'CT32': 32, 'CT24': 24, 'CT16': 16, 'CT16S': 16, 'T8': 8, 'T4': 4}.get(psm, 32)
            if len(pending) * 8 >= w * h * bpp:
                result.append((dbp, dbw, psm, x, y, w, h, bytes(pending)))
                pending = None
    return result


def csm1(index):
    """CSM1 palette order: within each 32 entries, entries 8-15 and 16-23 swap."""
    return (index & ~0x18) | ((index & 0x08) << 1) | ((index & 0x10) >> 1)


def to_rgba(texture, palette):
    """(w, h, RGBA bytes) for an indexed texture upload and its 32-bit palette upload (alpha 0x80 = opaque)."""
    _, _, psm, _, _, w, h, pixels = texture
    pal = [palette[7][i * 4:i * 4 + 4] for i in range(len(palette[7]) // 4)]
    out = bytearray()
    for i in range(w * h):
        if psm == 'T8':
            index = pixels[i]
            entry = pal[csm1(index)] if csm1(index) < len(pal) else b'\0\0\0\0'
        else:  # T4
            byte = pixels[i >> 1]
            index = byte >> 4 if i & 1 else byte & 0xf
            entry = pal[index] if index < len(pal) else b'\0\0\0\0'
        out += bytes((entry[0], entry[1], entry[2], min(255, entry[3] * 2)))
    return w, h, bytes(out)


if __name__ == '__main__':
    mem = open(sys.argv[1], 'rb').read()
    mode = sys.argv[2]
    ids = range(4096)
    if mode == 'list':
        n = 0
        for tex_id in ids:
            up = texture_uploads(mem, tex_id)
            if up:
                n += 1
                if n <= 40:
                    print(tex_id, [(u[2], u[5], u[6], f'base {u[0]}') for u in up])
        print(n, 'textures with uploads')
    elif mode == 'png':
        from PIL import Image
        out_dir = sys.argv[3]
        os.makedirs(out_dir, exist_ok=True)
        wanted = [int(x) for x in sys.argv[4:]] or ids
        written = 0
        for tex_id in wanted:
            up = texture_uploads(mem, tex_id)
            textures = [u for u in up if u[2] in ('T8', 'T4')]
            palettes = [u for u in up if u[2] == 'CT32']
            if not textures or not palettes:
                continue
            w, h, rgba = to_rgba(textures[0], palettes[0])
            Image.frombytes('RGBA', (w, h), rgba).save(os.path.join(out_dir, f'tex{tex_id:04d}_{textures[0][2]}.png'))
            written += 1
        print(written, 'textures written to', out_dir)
