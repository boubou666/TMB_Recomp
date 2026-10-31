#!/usr/bin/env python3
"""Converts TMB's texture files from the user's own disc into the native texture pack the port's renderer loads
(runtime/tmb_textures.cpp): done once, offline, into the build tree (never committed). While the game runs, nothing is
read from its texture records: the runtime only counts the records TMB's texture code sets up and loads, and looks
the textures up here by file and position.

  tools/tmb_texture_pack.py <game dir> <ELF> <out.pack>

What it reads:
- .TEX files (a zip entry, "IE" instead of "PK": raw deflate after the local header): a header (+0 class count,
  +4 its size in quadwords), then records chained by their size (+0, in words). texmLoadTexture skips a record with
  bit 7 of its format word (+0xc) set and sets up the others in order (texmSetupTexture): those are the pack's
  records, in that order. Pixels at +0x80 (a plain raster in the record's format).
- .RTX files (raw): records from the offset its +4 gives in quadwords, each loaded in order by texmLoadResTexture:
  the T8H / T4HL / T4HH textures (pixels at +0x80) and the palettes (entries at +0x10).
- the font (fontDmaFontData's sources in the ELF: a 128 x 128 T4 raster, a 16-entry CT32 palette).

The pack: "TMBTEX1\\0", a file count, then per file its name (upper case, no directory), its record count and each
record: psm, width, height, then its texels unpacked (indexed formats one byte per texel, CT32 four bytes, CT24 four
bytes with alpha 0, CT16 two bytes), each record's byte count before them.
"""
import os
import struct
import sys
import zlib

CT32, CT24, CT16, CT16S, T8, T4, T8H, T4HL, T4HH = 0x00, 0x01, 0x02, 0x0a, 0x13, 0x14, 0x1b, 0x24, 0x2c
FONT = {'SCUS_971.01': (0x22aba0, 0x22ab60), 'SCES_503.60': (0x22be20, 0x22bde0)}


def u32(data, at):
    return struct.unpack_from('<I', data, at)[0]


def unpack_texels(psm, raw, width, height):
    count = width * height
    if psm in (T8, T8H):
        return bytes(raw[:count])
    if psm in (T4, T4HL, T4HH):
        out = bytearray(count)
        for i in range(count):
            byte = raw[i >> 1]
            out[i] = (byte >> 4) if i & 1 else (byte & 0xf)
        return bytes(out)
    if psm == CT32:
        return bytes(raw[:count * 4])
    if psm == CT24:
        out = bytearray(count * 4)
        for i in range(count):
            out[i * 4:i * 4 + 3] = raw[i * 3:i * 3 + 3]
        return bytes(out)
    if psm in (CT16, CT16S):
        return bytes(raw[:count * 2])
    raise ValueError(f'psm {psm:#x}')


def bits(psm):
    return {CT32: 32, CT24: 24, CT16: 16, CT16S: 16, T8: 8, T8H: 8}.get(psm, 4)


def record_size(data, rec, texels_at):
    """A record's size in bytes: its +0 (in words), or for a file's last record, whose +0 the game fills in when it
    links the files (texmRelocate / texmResRelocate), its header and raster."""
    size = u32(data, rec) * 4
    if size:
        return size
    fmt = u32(data, rec + 0xc)
    width, height = 1 << ((fmt >> 8) & 0xf), 1 << ((fmt >> 12) & 0xf)
    return texels_at + (width * height * bits(fmt & 0x3f) + 7) // 8


def record(data, rec, texels_at):
    fmt = u32(data, rec + 0xc)
    psm = fmt & 0x3f
    width, height = 1 << ((fmt >> 8) & 0xf), 1 << ((fmt >> 12) & 0xf)
    size = record_size(data, rec, texels_at)
    raw = data[rec + texels_at:rec + size]
    need = (width * height * bits(psm) + 7) // 8
    if len(raw) < need:  # a palette smaller than its format's size: what is there
        raw = raw + bytes(need - len(raw))
    return psm, width, height, unpack_texels(psm, raw, width, height)


def walk(data, first, texels_at):
    """The records from first to the end of the file (texels_at: where a record's texels start, for the last one's
    size)."""
    rec = first
    while rec + 0x10 <= len(data):
        if u32(data, rec) == 0 and u32(data, rec + 0xc) == 0:
            return  # the file's padding
        yield rec
        rec += (record_size(data, rec, texels_at(rec)) + 15) // 16 * 16


def tex_records(path):
    blob = open(path, 'rb').read()
    if blob[:4] == b'IE\x03\x04':
        name_len, extra_len = struct.unpack_from('<HH', blob, 26)
        data = zlib.decompressobj(-15).decompress(blob[30 + name_len + extra_len:])
    else:
        data = blob
    out = []
    for rec in walk(data, u32(data, 4) * 16, lambda rec: 0x80):
        if (u32(data, rec + 0xc) >> 7) & 1:
            continue  # texmLoadTexture skips it
        out.append(record(data, rec, 0x80))
    return out


def rtx_records(path):
    data = open(path, 'rb').read()
    out = []
    def texels_at(rec):
        return 0x80 if u32(data, rec + 0xc) & 0x3f in (T8H, T4HL, T4HH) else 0x10
    for rec in walk(data, u32(data, 4) * 16, texels_at):
        psm = u32(data, rec + 0xc) & 0x3f
        out.append(record(data, rec, 0x80 if psm in (T8H, T4HL, T4HH) else 0x10))
    return out


def elf_bytes(path, address, size):
    elf = open(path, 'rb').read()
    phoff, phentsize, phnum = u32(elf, 0x1c), struct.unpack_from('<H', elf, 0x2a)[0], struct.unpack_from('<H', elf, 0x2c)[0]
    for i in range(phnum):
        p = phoff + i * phentsize
        offset, vaddr, filesz = u32(elf, p + 4), u32(elf, p + 8), u32(elf, p + 16)
        if vaddr <= address and address + size <= vaddr + filesz:
            return elf[offset + address - vaddr:offset + address - vaddr + size]
    raise ValueError(f'{address:#x} not in the ELF')


def main():
    game, elf, out_path = sys.argv[1:4]
    files = []
    for root, _, names in os.walk(game):
        for name in sorted(names):
            ext = os.path.splitext(name)[1].upper()
            if ext == '.TEX':
                files.append((name.upper(), tex_records(os.path.join(root, name))))
            elif ext == '.RTX':
                files.append((name.upper(), rtx_records(os.path.join(root, name))))
    pixels_at, palette_at = FONT[os.path.basename(elf).upper()]
    font = unpack_texels(T4, elf_bytes(elf, pixels_at, 128 * 64), 128, 128)
    files.append(('FONT', [(T4, 128, 128, font), (CT32, 8, 2, elf_bytes(elf, palette_at, 64))]))
    with open(out_path, 'wb') as out:
        out.write(b'TMBTEX1\0')
        out.write(struct.pack('<I', len(files)))
        for name, records in files:
            encoded = name.encode()
            out.write(struct.pack('<H', len(encoded)) + encoded + struct.pack('<I', len(records)))
            for psm, width, height, texels in records:
                out.write(struct.pack('<IIII', psm, width, height, len(texels)) + texels)
    print(f'{out_path}: {len(files)} files, {sum(len(r) for _, r in files)} records')


if __name__ == '__main__':
    main()
