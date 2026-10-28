#!/usr/bin/env python3
"""Reads a VIF1 DMA chain from a dump of EE memory (TMB_DUMP_RDRAM) the way the hardware would hand it to VIF1, and
lists what it does: UNPACKs (where, how many quadwords, which format), microprogram starts (MSCAL / MSCNT), DIRECT
(GIF) transfers. The native port's adapters read their objects' data this way instead of running VU code.

  tools/vif_chain.py <rdram.bin> <chain address> [--all]
"""
import struct
import sys

RAM = 32 * 1024 * 1024
VN_NAMES = ['S', 'V2', 'V3', 'V4']
VL_BITS = [32, 16, 8, 5]


def dma_chain(mem, addr, limit=200000):
    """The VIF word stream of a source chain with tag transfer on: per tag its two VIF codes (upper 64 bits) then
    its data. Follows cnt, next, ref, refs, call (two levels), ret and end."""
    words = []
    stack = []
    for _ in range(limit):
        a = addr & (RAM - 1)
        lo, hi = struct.unpack_from('<2Q', mem, a)
        qwc, tag_id, ref = lo & 0xffff, (lo >> 28) & 7, (lo >> 32) & 0x7fffffff
        words += [hi & 0xffffffff, hi >> 32]
        if tag_id in (0, 3, 4):  # refe, ref, refs: data at ADDR
            data = mem[ref & (RAM - 1):(ref & (RAM - 1)) + qwc * 16]
            nxt = a + 16
        else:
            data = mem[a + 16:a + 16 + qwc * 16]
            nxt = a + 16 + qwc * 16
        words += list(struct.unpack('<%dI' % (len(data) // 4), data))
        if tag_id == 0 or tag_id == 7:  # refe, end
            return words
        if tag_id == 1 or tag_id == 3 or tag_id == 4:  # cnt, ref, refs
            addr = nxt
        elif tag_id == 2:  # next
            addr = ref
        elif tag_id == 5:  # call
            stack.append(nxt)
            addr = ref
        elif tag_id == 6:  # ret
            if not stack:
                return words
            addr = stack.pop()
    return words


def vif_commands(words):
    """Yields (name, imm, num, details, payload words) for each VIF command of the stream."""
    i = 0
    while i < len(words):
        code = words[i]
        cmd, num, imm = (code >> 24) & 0x7f, (code >> 16) & 0xff, code & 0xffff
        i += 1
        if cmd & 0x60 == 0x60:  # UNPACK
            vn, vl = (cmd >> 2) & 3, cmd & 3
            n = num or 256
            bits = (vn + 1) * VL_BITS[vl] if vl != 3 else 16
            size = (n * bits + 31) // 32
            yield ('UNPACK', imm, n, f'{VN_NAMES[vn]}-{VL_BITS[vl]} addr {imm & 0x3ff} usn {(imm >> 14) & 1} '
                                     f'flg {(imm >> 15) & 1} mask {(cmd >> 4) & 1}', words[i:i + size])
            i += size
        elif cmd == 0x50 or cmd == 0x51:  # DIRECT / DIRECTHL
            q = imm or 65536
            yield ('DIRECT' if cmd == 0x50 else 'DIRECTHL', imm, q, '', words[i:i + q * 4])
            i += q * 4
        elif cmd == 0x4a:  # MPG
            n = num or 256
            yield ('MPG', imm, n, f'load at {imm * 8:#x}', words[i:i + n * 2])
            i += n * 2
        elif cmd == 0x20:  # STMASK
            yield ('STMASK', imm, 0, '', words[i:i + 1])
            i += 1
        elif cmd == 0x30 or cmd == 0x31:  # STROW / STCOL
            yield ('STROW' if cmd == 0x30 else 'STCOL', imm, 0, '', words[i:i + 4])
            i += 4
        else:
            name = {0x00: 'NOP', 0x01: 'STCYCL', 0x02: 'OFFSET', 0x03: 'BASE', 0x04: 'ITOP', 0x05: 'STMOD',
                    0x06: 'MSKPATH3', 0x07: 'MARK', 0x10: 'FLUSHE', 0x11: 'FLUSH', 0x13: 'FLUSHA', 0x14: 'MSCAL',
                    0x15: 'MSCALF', 0x17: 'MSCNT'}.get(cmd, f'?{cmd:#x}')
            detail = f'pc {imm * 8:#x}' if name in ('MSCAL', 'MSCALF') else (
                f'cl {imm & 0xff} wl {imm >> 8}' if name == 'STCYCL' else '')
            yield (name, imm, num, detail, [])


if __name__ == '__main__':
    import collections
    mem = open(sys.argv[1], 'rb').read()
    words = dma_chain(mem, int(sys.argv[2], 0))
    counts = collections.Counter()
    shown = 0
    for name, imm, num, detail, payload in vif_commands(words):
        counts[name] += 1
        if '--all' in sys.argv or (shown < 80 and name != 'NOP'):
            shown += 1
            print(f'{name:8s} {detail} num {num}')
    print(len(words), 'words', dict(counts))
