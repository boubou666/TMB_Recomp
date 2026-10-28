"""The symbol table of a TMB executable (both regions ship theirs): names, addresses and sizes, for the tools that
carry the hand-written parts of the port from one region's ELF to the other's (tmb_manual_toml.py, tmb_region_map.py).
"""
import struct


class Symbol:
    __slots__ = ('name', 'address', 'size', 'kind')

    def __init__(self, name, address, size, kind):
        self.name, self.address, self.size, self.kind = name, address, size, kind  # kind: 'func', 'object', other


def symbols(path):
    """The ELF's named function and object symbols."""
    data = open(path, 'rb').read()
    assert data[:4] == b'\x7fELF' and data[4] == 1, f'{path}: not a 32-bit ELF'
    shoff, = struct.unpack_from('<I', data, 0x20)
    shentsize, shnum = struct.unpack_from('<HH', data, 0x2e)
    sections = [struct.unpack_from('<10I', data, shoff + i * shentsize) for i in range(shnum)]
    result = []
    for sh in sections:
        if sh[1] != 2:  # SHT_SYMTAB
            continue
        strtab = sections[sh[6]]
        names = data[strtab[4]:strtab[4] + strtab[5]]
        for offset in range(sh[4], sh[4] + sh[5], 16):
            name_off, value, size, info, _other, shndx = struct.unpack_from('<IIIBBH', data, offset)
            kind = {1: 'object', 2: 'func'}.get(info & 0xf)
            if not kind or shndx == 0:
                continue
            name = names[name_off:names.index(b'\0', name_off)].decode('latin-1')
            if name:
                result.append(Symbol(name, value, size, kind))
    return result


def by_name(syms):
    table = {}
    for s in syms:
        table.setdefault(s.name, s)
    return table


def containing(syms, address):
    """The symbol whose [address, address + size) holds address (the nearest one below it when sizes are missing)."""
    best = None
    for s in syms:
        if s.address <= address and (best is None or s.address > best.address):
            if s.size == 0 or address < s.address + s.size or s.address == address:
                best = s
    return best


def gp(path):
    """The executable's _gp (small data pointer)."""
    for s in symbols_any(path):
        if s.name == '_gp':
            return s.address
    return None


def symbols_any(path):
    """Every named symbol, of any kind (absolute ones like _gp included)."""
    data = open(path, 'rb').read()
    shoff, = struct.unpack_from('<I', data, 0x20)
    shentsize, shnum = struct.unpack_from('<HH', data, 0x2e)
    sections = [struct.unpack_from('<10I', data, shoff + i * shentsize) for i in range(shnum)]
    for sh in sections:
        if sh[1] != 2:
            continue
        strtab = sections[sh[6]]
        names = data[strtab[4]:strtab[4] + strtab[5]]
        for offset in range(sh[4], sh[4] + sh[5], 16):
            name_off, value, size, info, _other, _shndx = struct.unpack_from('<IIIBBH', data, offset)
            name = names[name_off:names.index(b'\0', name_off)].decode('latin-1')
            if name:
                yield Symbol(name, value, size, 'other')


class Image:
    """The executable's loaded bytes, by address (its PT_LOAD segments)."""

    def __init__(self, path):
        data = open(path, 'rb').read()
        phoff, = struct.unpack_from('<I', data, 0x1c)
        phentsize, phnum = struct.unpack_from('<HH', data, 0x2a)
        self.segments = []
        for i in range(phnum):
            ptype, offset, vaddr, _paddr, filesz, _memsz, _flags, _align = struct.unpack_from('<8I', data, phoff + i * phentsize)
            if ptype == 1:
                self.segments.append((vaddr, data[offset:offset + filesz]))

    def word(self, address):
        for vaddr, blob in self.segments:
            if vaddr <= address and address + 4 <= vaddr + len(blob):
                return struct.unpack_from('<I', blob, address - vaddr)[0]
        return None
