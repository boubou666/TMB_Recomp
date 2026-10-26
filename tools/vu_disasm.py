#!/usr/bin/env python3
"""Disassembles PS2 VU microcode: a code image as `tmb_vu1_replay <trace> dumpcode <dir>` writes it (pairs of
lower word, upper word, little-endian), or any raw dump of VU micro memory.

  tools/vu_disasm.py <image.bin> [start_pc [end_pc]]     pcs in bytes, e.g. 0x1d40

Branch targets are labelled; an `[E]` marks the end bit (the program stops after the next pair), `[I]` a pair whose
lower word is the float for the I register (LOI).
"""
import struct
import sys

BC = 'xyzw'


def dest(word):
    d = (word >> 21) & 0xf
    return ''.join(c for c, bit in zip('xyzw', (8, 4, 2, 1)) if d & bit)


def upper(word):
    ft, fs, fd = (word >> 16) & 31, (word >> 11) & 31, (word >> 6) & 31
    op = word & 0x3f
    dd = dest(word)
    if op >= 0x3c:
        sub = (((word >> 6) & 31) << 2) | (word & 3)
        names = {0x00: 'ADDA', 0x04: 'SUBA', 0x08: 'MADDA', 0x0c: 'MSUBA', 0x18: 'MULA'}
        base = sub & ~3
        if base in names:
            return f'{names[base]}{BC[sub & 3]}.{dd} ACC, vf{fs}, vf{ft}{BC[sub & 3]}'
        if 0x10 <= sub <= 0x17:
            kind = 'ITOF' if sub < 0x14 else 'FTOI'
            return f'{kind}{(0, 4, 12, 15)[sub & 3]}.{dd} vf{ft}, vf{fs}'
        acc_q = {0x1c: 'MULAq', 0x20: 'ADDAq', 0x21: 'MADDAq', 0x24: 'SUBAq', 0x25: 'MSUBAq'}
        acc_i = {0x1e: 'MULAi', 0x22: 'ADDAi', 0x23: 'MADDAi', 0x26: 'SUBAi', 0x27: 'MSUBAi'}
        acc_v = {0x28: 'ADDA', 0x29: 'MADDA', 0x2a: 'MULA', 0x2c: 'SUBA', 0x2d: 'MSUBA', 0x2e: 'OPMULA'}
        if sub in acc_q:
            return f'{acc_q[sub]}.{dd} ACC, vf{fs}, Q'
        if sub in acc_i:
            return f'{acc_i[sub]}.{dd} ACC, vf{fs}, I'
        if sub in acc_v:
            return f'{acc_v[sub]}.{dd} ACC, vf{fs}, vf{ft}'
        if sub == 0x1d:
            return f'ABS.{dd} vf{ft}, vf{fs}'
        if sub == 0x1f:
            return f'CLIP.xyz vf{fs}, vf{ft}w'
        if sub == 0x2f:
            return 'NOP'
        return f'upper? 0x{word:08x}'
    names = {0x00: 'ADD', 0x04: 'SUB', 0x08: 'MADD', 0x0c: 'MSUB', 0x10: 'MAX', 0x14: 'MINI', 0x18: 'MUL'}
    if op < 0x1c:
        return f'{names[op & ~3]}{BC[op & 3]}.{dd} vf{fd}, vf{fs}, vf{ft}{BC[op & 3]}'
    q_i = {0x1c: ('MULq', 'Q'), 0x1d: ('MAXi', 'I'), 0x1e: ('MULi', 'I'), 0x1f: ('MINIi', 'I'), 0x20: ('ADDq', 'Q'),
           0x21: ('MADDq', 'Q'), 0x22: ('ADDi', 'I'), 0x23: ('MADDi', 'I'), 0x24: ('SUBq', 'Q'), 0x25: ('MSUBq', 'Q'),
           0x26: ('SUBi', 'I'), 0x27: ('MSUBi', 'I')}
    if op in q_i:
        name, reg = q_i[op]
        return f'{name}.{dd} vf{fd}, vf{fs}, {reg}'
    vv = {0x28: 'ADD', 0x29: 'MADD', 0x2a: 'MUL', 0x2b: 'MAX', 0x2c: 'SUB', 0x2d: 'MSUB', 0x2e: 'OPMSUB', 0x2f: 'MINI'}
    if op in vv:
        return f'{vv[op]}.{dd} vf{fd}, vf{fs}, vf{ft}'
    return f'upper? 0x{word:08x}'


def simm(value, bits):
    return value - (1 << bits) if value & (1 << (bits - 1)) else value


def lower(word, pc, targets):
    op = word >> 25
    it, is_, imm11 = (word >> 16) & 31, (word >> 11) & 31, simm(word & 0x7ff, 11)
    ft, fs = it, is_
    dd = dest(word)
    fsf, ftf = BC[(word >> 21) & 3], BC[(word >> 23) & 3]

    def branch(name, regs):
        target = (pc + 8 + imm11 * 8) & 0x3fff
        targets.add(target)
        return f'{name} {regs}L_{target:04x}' if regs else f'{name} L_{target:04x}'

    if op == 0x00:
        return f'LQ.{dd} vf{ft}, {imm11}(vi{is_})'
    if op == 0x01:
        return f'SQ.{dd} vf{fs}, {imm11}(vi{it})'
    if op == 0x04:
        return f'ILW.{dd} vi{it}, {imm11}(vi{is_})'
    if op == 0x05:
        return f'ISW.{dd} vi{it}, {imm11}(vi{is_})'
    if op in (0x08, 0x09):
        imm15 = ((word >> 10) & 0x7800) | (word & 0x7ff)
        return f'{"IADDIU" if op == 0x08 else "ISUBIU"} vi{it}, vi{is_}, 0x{imm15:x}'
    imm24 = word & 0xffffff
    flag24 = {0x10: 'FCEQ vi1,', 0x11: 'FCSET', 0x12: 'FCAND vi1,', 0x13: 'FCOR vi1,'}
    if op in flag24:
        return f'{flag24[op]} 0x{imm24:06x}'
    if op == 0x1c:
        return f'FCGET vi{it}'
    imm12 = ((word >> 10) & 0x800) | (word & 0x7ff)
    if op in (0x14, 0x15, 0x16, 0x17):
        name = {0x14: 'FSEQ', 0x15: 'FSSET', 0x16: 'FSAND', 0x17: 'FSOR'}[op]
        return f'{name} vi{it}, 0x{imm12:03x}' if op != 0x15 else f'FSSET 0x{imm12:03x}'
    if op in (0x18, 0x1a, 0x1b):
        return f'{ {0x18: "FMEQ", 0x1a: "FMAND", 0x1b: "FMOR"}[op]} vi{it}, vi{is_}'
    if op == 0x20:
        return branch('B', '')
    if op == 0x21:
        return branch('BAL', f'vi{it}, ')
    if op == 0x24:
        return f'JR vi{is_}'
    if op == 0x25:
        return f'JALR vi{it}, vi{is_}'
    if op in (0x28, 0x29):
        return branch('IBEQ' if op == 0x28 else 'IBNE', f'vi{it}, vi{is_}, ')
    if op in (0x2c, 0x2d, 0x2e, 0x2f):
        return branch({0x2c: 'IBLTZ', 0x2d: 'IBGTZ', 0x2e: 'IBLEZ', 0x2f: 'IBGEZ'}[op], f'vi{is_}, ')
    if op != 0x40:
        return f'lower? 0x{word:08x}'
    low6 = word & 0x3f
    id_ = (word >> 6) & 31
    if low6 < 0x3c:
        ops = {0x30: 'IADD', 0x31: 'ISUB', 0x34: 'IAND', 0x35: 'IOR'}
        if low6 in ops:
            return f'{ops[low6]} vi{id_}, vi{is_}, vi{it}'
        if low6 == 0x32:
            return f'IADDI vi{it}, vi{is_}, {simm(id_, 5)}'
        return f'lower? 0x{word:08x}'
    sub = (((word >> 6) & 31) << 2) | (word & 3)
    if sub == 0x30:
        return 'NOP' if word == 0x8000033c else f'MOVE.{dd} vf{ft}, vf{fs}'
    simple = {
        0x31: f'MR32.{dd} vf{ft}, vf{fs}', 0x34: f'LQI.{dd} vf{ft}, (vi{is_}++)', 0x35: f'SQI.{dd} vf{fs}, (vi{it}++)',
        0x36: f'LQD.{dd} vf{ft}, (--vi{is_})', 0x37: f'SQD.{dd} vf{fs}, (--vi{it})',
        0x38: f'DIV Q, vf{fs}{fsf}, vf{ft}{ftf}', 0x39: f'SQRT Q, vf{ft}{ftf}', 0x3a: f'RSQRT Q, vf{fs}{fsf}, vf{ft}{ftf}',
        0x3b: 'WAITQ', 0x3c: f'MTIR vi{it}, vf{fs}{fsf}', 0x3d: f'MFIR.{dd} vf{ft}, vi{is_}',
        0x3e: f'ILWR.{dd} vi{it}, (vi{is_})', 0x3f: f'ISWR.{dd} vi{it}, (vi{is_})',
        0x40: f'RNEXT.{dd} vf{ft}, R', 0x41: f'RGET.{dd} vf{ft}, R', 0x42: f'RINIT R, vf{fs}{fsf}',
        0x43: f'RXOR R, vf{fs}{fsf}', 0x64: f'MFP.{dd} vf{ft}, P', 0x68: f'XTOP vi{it}', 0x69: f'XITOP vi{it}',
        0x6c: f'XGKICK vi{is_}', 0x70: f'ESADD P, vf{fs}', 0x71: f'ERSADD P, vf{fs}', 0x72: f'ELENG P, vf{fs}',
        0x73: f'ERLENG P, vf{fs}', 0x74: f'EATANxy P, vf{fs}', 0x75: f'EATANxz P, vf{fs}', 0x76: f'ESUM P, vf{fs}',
        0x78: f'ESQRT P, vf{fs}{fsf}', 0x79: f'ERSQRT P, vf{fs}{fsf}', 0x7a: f'ERCPR P, vf{fs}{fsf}', 0x7b: 'WAITP',
        0x7c: f'ESIN P, vf{fs}{fsf}', 0x7d: f'EATAN P, vf{fs}{fsf}', 0x7e: f'EEXP P, vf{fs}{fsf}',
    }
    return simple.get(sub, f'lower? 0x{word:08x}')


def main():
    data = open(sys.argv[1], 'rb').read()
    start = int(sys.argv[2], 0) if len(sys.argv) > 2 else 0
    end = int(sys.argv[3], 0) if len(sys.argv) > 3 else len(data)
    pairs = [struct.unpack_from('<II', data, pc) for pc in range(0, len(data) - 7, 8)]
    targets = set()
    lines = []
    for index, (lo, up) in enumerate(pairs):
        pc = index * 8
        flags = ''.join(f for f, bit in (('I', 31), ('E', 30), ('M', 29), ('D', 28), ('T', 27)) if up >> bit & 1)
        low = f'LOI {struct.unpack("<f", struct.pack("<I", lo))[0]:.6g} (0x{lo:08x})' if up >> 31 & 1 else lower(lo, pc, targets)
        lines.append((pc, f'{upper(up & 0x07ffffff):40s} {low}' + (f'   [{flags}]' if flags else '')))
    for pc, text in lines:
        if start <= pc < end:
            if pc in targets:
                print(f'L_{pc:04x}:')
            print(f'  {pc:04x}  {text}')


main()
