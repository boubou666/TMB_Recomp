#!/usr/bin/env python3
"""Minimal MIPS R3000 disassembler for IRX modules (enough to read IOP wait loops).

usage: tools/irx_disasm.py <module.irx> <load_base_hex> <start_hex> <count>
Addresses are IOP addresses (load base + offset into the first PT_LOAD segment).
Relocations are not applied: lui/addiu pairs and jal targets show unrelocated values.
"""
import struct
import sys

REG = ['zero', 'at', 'v0', 'v1', 'a0', 'a1', 'a2', 'a3', 't0', 't1', 't2', 't3', 't4', 't5', 't6', 't7',
       's0', 's1', 's2', 's3', 's4', 's5', 's6', 's7', 't8', 't9', 'k0', 'k1', 'gp', 'sp', 'fp', 'ra']
I_OPS = {0x08: 'addi', 0x09: 'addiu', 0x0a: 'slti', 0x0b: 'sltiu', 0x0c: 'andi', 0x0d: 'ori', 0x0e: 'xori',
         0x20: 'lb', 0x21: 'lh', 0x23: 'lw', 0x24: 'lbu', 0x25: 'lhu', 0x28: 'sb', 0x29: 'sh', 0x2b: 'sw'}
R_OPS = {0x20: 'add', 0x21: 'addu', 0x22: 'sub', 0x23: 'subu', 0x24: 'and', 0x25: 'or', 0x26: 'xor', 0x27: 'nor',
         0x2a: 'slt', 0x2b: 'sltu', 0x00: 'sll', 0x02: 'srl', 0x03: 'sra', 0x08: 'jr', 0x09: 'jalr',
         0x10: 'mfhi', 0x12: 'mflo', 0x18: 'mult', 0x19: 'multu', 0x1a: 'div', 0x1b: 'divu', 0x0c: 'syscall'}


def disasm(word, pc):
    op = word >> 26
    rs, rt, rd = (word >> 21) & 31, (word >> 16) & 31, (word >> 11) & 31
    sa, fn = (word >> 6) & 31, word & 63
    imm = word & 0xffff
    simm = imm - 0x10000 if imm & 0x8000 else imm
    if word == 0:
        return 'nop'
    if op == 0:
        name = R_OPS.get(fn, f'special.{fn:#x}')
        if fn in (0, 2, 3):
            return f'{name} {REG[rd]}, {REG[rt]}, {sa}'
        if fn == 8:
            return f'jr {REG[rs]}'
        if fn == 9:
            return f'jalr {REG[rd]}, {REG[rs]}'
        if fn in (0x10, 0x12):
            return f'{name} {REG[rd]}'
        if fn in (0x18, 0x19, 0x1a, 0x1b):
            return f'{name} {REG[rs]}, {REG[rt]}'
        return f'{name} {REG[rd]}, {REG[rs]}, {REG[rt]}'
    if op in (2, 3):
        return f"{'j' if op == 2 else 'jal'} {((pc & 0xf0000000) | ((word & 0x3ffffff) << 2)):#x}"
    if op in (4, 5, 6, 7):
        target = pc + 4 + (simm << 2)
        name = {4: 'beq', 5: 'bne', 6: 'blez', 7: 'bgtz'}[op]
        return f'{name} {REG[rs]}, {REG[rt]}, {target:#x}' if op in (4, 5) else f'{name} {REG[rs]}, {target:#x}'
    if op == 1:
        target = pc + 4 + (simm << 2)
        return f"{ {0: 'bltz', 1: 'bgez', 16: 'bltzal', 17: 'bgezal'}.get(rt, 'regimm') } {REG[rs]}, {target:#x}"
    if op == 0x0f:
        return f'lui {REG[rt]}, {imm:#x}'
    if op in I_OPS:
        name = I_OPS[op]
        if op >= 0x20:
            return f'{name} {REG[rt]}, {simm:#x}({REG[rs]})'
        shown = imm if op in (0x0c, 0x0d, 0x0e) else simm
        return f'{name} {REG[rt]}, {REG[rs]}, {shown:#x}'
    if op == 0x10:
        return f'cop0 {word:#010x}'
    return f'.word {word:#010x}'


def main():
    if len(sys.argv) != 5:
        print(__doc__)
        return 1
    data = open(sys.argv[1], 'rb').read()
    base, start, count = int(sys.argv[2], 16), int(sys.argv[3], 16), int(sys.argv[4])
    phoff, = struct.unpack_from('<I', data, 0x1c)
    phentsize, phnum = struct.unpack_from('<HH', data, 0x2a)
    seg_off = None
    for i in range(phnum):
        p_type, p_offset = struct.unpack_from('<II', data, phoff + i * phentsize)
        if p_type == 1:
            seg_off = p_offset
            break
    if seg_off is None:
        print('no PT_LOAD segment')
        return 1
    for k in range(count):
        pc = start + 4 * k
        off = seg_off + (pc - base)
        word, = struct.unpack_from('<I', data, off)
        print(f'{pc:08x}: {word:08x}  {disasm(word, pc)}')
    return 0


if __name__ == '__main__':
    sys.exit(main())
