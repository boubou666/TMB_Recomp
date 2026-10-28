#!/usr/bin/env python3
"""Writes tmb_region.h for a region's build: the runtime's hand-written code (runtime/) is written against the PAL
executable's addresses, and this maps each one it uses to the same place in the region's executable, by symbol name
(both releases ship their symbol tables).

  tools/tmb_region_map.py <PAL ELF> <region ELF> <runtime dir> > generated_<region>/tmb_region.h

What it maps, as the sources mark it:
- TMB_ADDR(0x...): an address in the PAL executable (a function, an instruction inside one, a global or a field of
  it), mapped to the symbol holding it plus the same offset.
- TMB_GP(-0x...): a gp-relative offset (small data), mapped through the global at PAL _gp + offset.
- name_0x<addr>: a recompiled function's generated name, defined to the region's name of the same function.
A symbol of another size in the region's executable is listed in the header (and on stderr) for review: the offset
into it may not hold the same thing. An address no symbol holds is an error.
"""
import glob, os, re, sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import tmb_elf

STATIC_SUFFIX = re.compile(r'\.\d+$')  # a function's static local: 'MOVIE_FILE_PATH.1033'
# Loads, stores and addiu: the instructions that reach a global through a base register and an offset.
MEMORY_OPS = {0x08, 0x09, 0x0d, 0x1e, 0x1f, 0x20, 0x21, 0x23, 0x24, 0x25, 0x27, 0x28, 0x29, 0x2b, 0x31, 0x37, 0x39, 0x3f}


def simm(word):
    return (word & 0xffff) - 0x10000 if word & 0x8000 else word & 0xffff


def code_references(image, functions, target=None, gp_offset=None):
    """(function, instruction index, partner index) for each instruction of the functions that reaches target (a lui
    and the load / store / addiu using its register) or gp + gp_offset (one instruction, partner None)."""
    for f in functions:
        count = f.size // 4
        words = [image.word(f.address + 4 * i) or 0 for i in range(count)]
        for i, w in enumerate(words):
            op, rs, rt = w >> 26, (w >> 21) & 31, (w >> 16) & 31
            if gp_offset is not None and op in MEMORY_OPS and rs == 28 and simm(w) == gp_offset:
                yield f, i, None
            if target is not None and op == 0x0f:  # lui rt, hi
                for j in range(i + 1, min(i + 24, count)):
                    w2 = words[j]
                    if w2 >> 26 in MEMORY_OPS and (w2 >> 21) & 31 == rt:
                        lo = (w2 & 0xffff) if w2 >> 26 == 0x0d else simm(w2)  # ori is unsigned
                        if (((w & 0xffff) << 16) + lo) & 0xffffffff == target:
                            yield f, i, j
                        break


def main():
    pal_path, region_path, runtime = sys.argv[1:4]
    pal = [s for s in tmb_elf.symbols(pal_path)]
    region = tmb_elf.symbols(region_path)
    region_by_name = tmb_elf.by_name(region)
    region_by_base = {}
    for s in region:
        region_by_base.setdefault(STATIC_SUFFIX.sub('', s.name), []).append(s)
    pal_gp, region_gp = tmb_elf.gp(pal_path), tmb_elf.gp(region_path)
    same = os.path.abspath(pal_path) == os.path.abspath(region_path)

    sources = sorted(glob.glob(os.path.join(runtime, '**', '*.cpp'), recursive=True) +
                     glob.glob(os.path.join(runtime, '**', '*.h'), recursive=True))
    addresses, gp_offsets, functions = set(), set(), set()
    for path in sources:
        text = open(path, encoding='utf-8', errors='replace').read()
        addresses.update(int(v, 16) for v in re.findall(r'TMB_ADDR\(\s*0x([0-9A-Fa-f]+)u?\s*\)', text))
        gp_offsets.update(int(v.replace('u', ''), 16) * (-1 if sign == '-' else 1)
                          for sign, v in re.findall(r'TMB_GP\(\s*(-?)\s*0x([0-9A-Fa-f]+u?)\s*\)', text))
        functions.update(re.findall(r'\b([A-Za-z_][A-Za-z0-9_]*)_0x([0-9a-f]{5,8})\b', text))

    errors, reviews = [], []

    def holder(address):
        """The PAL symbol holding address: the smallest one that covers it, else a function ending right at it."""
        covering = [s for s in pal if s.size and s.address <= address < s.address + s.size]
        if covering:
            return min(covering, key=lambda s: s.size)
        exact = [s for s in pal if s.address == address]
        return exact[0] if exact else None

    def counterpart(s):
        found = region_by_name.get(s.name)
        if found is None:
            candidates = region_by_base.get(STATIC_SUFFIX.sub('', s.name), [])
            found = candidates[0] if len(candidates) == 1 else None
        return found

    pal_image, region_image = tmb_elf.Image(pal_path), tmb_elf.Image(region_path)
    pal_functions = [s for s in pal if s.kind == 'func' and s.size]

    def by_code(target=None, gp_offset=None):
        """An unnamed global (a file's static) found through the code that uses it: the same instruction in the
        region's function of the same name. Returns the region's address (or gp offset)."""
        for f, i, j in code_references(pal_image, pal_functions, target, gp_offset):
            r = counterpart(f)
            if r is None or r.size != f.size:
                continue
            w = region_image.word(r.address + 4 * i)
            pw = pal_image.word(f.address + 4 * i)
            if w is None or w >> 26 != pw >> 26 or (w >> 16) & 0x3ff != (pw >> 16) & 0x3ff:
                continue
            if j is None:
                return simm(w)
            w2, pw2 = region_image.word(r.address + 4 * j), pal_image.word(f.address + 4 * j)
            if w2 is None or w2 >> 26 != pw2 >> 26:
                continue
            lo = (w2 & 0xffff) if w2 >> 26 == 0x0d else simm(w2)
            return (((w & 0xffff) << 16) + lo) & 0xffffffff
        return None

    def map_address(address, what):
        s = holder(address)
        if s is None:
            # Right before a function (a hook on the instruction before it): the region's function, as far before it.
            for before in (4, 8):
                f = next((p for p in pal_functions if p.address == address + before), None)
                r = counterpart(f) if f else None
                if r is not None:
                    return r.address - before
            found = by_code(target=address)
            if found is not None:
                return found
            errors.append(f'{what} {address:#x}: no PAL symbol holds it, and no code reaches it')
            return None
        r = counterpart(s)
        if r is None:
            errors.append(f'{what} {address:#x} ({s.name}+{address - s.address:#x}): no {s.name} in the region ELF')
            return None
        if r.size != s.size and not same:
            reviews.append(f'{what} {address:#x}: {s.name}+{address - s.address:#x} is {s.size:#x} bytes in PAL, '
                           f'{r.size:#x} here')
        return r.address + (address - s.address)

    address_map = {a: map_address(a, 'TMB_ADDR') for a in sorted(addresses)}
    gp_map = {}
    for offset in sorted(gp_offsets):
        address = (pal_gp + offset) & 0xffffffff
        if holder(address) is None:
            found = by_code(gp_offset=offset)
            if found is None:
                errors.append(f'TMB_GP({offset:#x}) {address:#x}: no PAL symbol holds it, and no code reaches it')
            gp_map[offset] = found
            continue
        target = map_address(address, f'TMB_GP({offset:#x})')
        gp_map[offset] = None if target is None else target - region_gp
    function_map = {}
    for name, hexaddr in sorted(functions):
        address = int(hexaddr, 16)
        s = next((p for p in pal if p.address == address and p.kind == 'func' and p.name.replace('$', '_') == name), None)
        if s is None:
            continue  # not a generated function name (or another region's)
        r = counterpart(s)
        if r is None:
            errors.append(f'function {name}_0x{hexaddr}: no {s.name} in the region ELF')
            continue
        function_map[f'{name}_0x{hexaddr}'] = f'{name}_0x{r.address:x}'

    if errors:
        for e in errors:
            print('tmb_region_map: ' + e, file=sys.stderr)
        sys.exit(1)
    for r in reviews:
        print('tmb_region_map: review: ' + r, file=sys.stderr)

    region_name = os.path.basename(region_path)
    out = ['// Generated by tools/tmb_region_map.py for ' + region_name + ': the PAL addresses runtime/ uses, mapped',
           '// to this executable by symbol. Derived from the game: never committed.',
           '#pragma once', '#include <cstdint>', '']
    out.append('#define TMB_REGION_US 1' if region_name.startswith('SCUS') else '#define TMB_REGION_PAL 1')
    out += ['', 'namespace tmb::region', '{',
            f'    constexpr const char *kExecutable = "{region_name}";',
            '    constexpr const char *kGameName = "Twisted Metal - Black (' +
            ('USA' if region_name.startswith('SCUS') else 'Europe') + ')";',
            '    constexpr uint32_t addr(uint32_t pal)', '    {', '        switch (pal)', '        {']
    out += [f'        case {a:#x}u: return {m:#x}u;' for a, m in address_map.items()]
    out += ['        default: return 0u; // not listed: tools/tmb_region_map.py did not see it marked', '        }', '    }',
            '    constexpr int32_t gp(int32_t pal)', '    {', '        switch (pal)', '        {']
    out += [f'        case {o}: return {m};' for o, m in gp_map.items()]
    out += ['        default: return 0;', '        }', '    }', '}', '',
            '#define TMB_ADDR(a) (::tmb::region::addr(a))', '#define TMB_GP(o) (::tmb::region::gp(o))', '']
    for r in reviews:
        out.append('// review: ' + r)
    for pal_name, region_name_ in function_map.items():
        if pal_name != region_name_:
            out.append(f'#define {pal_name} {region_name_}')
    print('\n'.join(out))


if __name__ == '__main__':
    main()
