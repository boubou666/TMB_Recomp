#!/usr/bin/env python3
"""Writes the hand-written recompiler settings shared by both regions (recomp/tmb_manual.toml, by symbol name) into a
region's config, with its executable's addresses: the manual block at the top of its stub list, replacing the one
there (between '# --- Manual' and '# --- Analyzer output below').

  tools/tmb_manual_toml.py <ELF> recomp/tmb_manual.toml <region config>
"""
import os, sys, tomllib

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import tmb_elf

BEGIN = '  # --- Manual (recomp/tmb_manual.toml, tools/tmb_manual_toml.py)'
END = '  # --- Analyzer output below'


def main():
    elf, manual_path, config_path = sys.argv[1:4]
    table = tmb_elf.by_name(tmb_elf.symbols(elf))
    manual = tomllib.load(open(manual_path, 'rb'))
    lines = [BEGIN]
    for stub in manual.get('stub', []):
        for name in stub['symbols']:
            s = table.get(name)
            if s is None:
                sys.exit(f'tmb_manual_toml: no {name} in {elf}')
            lines.append(f'  "{stub["handler"]}@{s.address:#010X}", # {name}'.replace('0X', '0x', 1))
    lines.append(END)

    text = open(config_path, encoding='utf-8').read().split('\n')
    start = text.index('stubs = [') + 1
    # Drop the block a previous run wrote (or the hand-written one before this tool), up to the analyzer's part.
    end = start
    while end < len(text) and text[end].strip().startswith(('#', '"ret')) and text[end] != END:
        end += 1
    if end < len(text) and text[end] == END:
        end += 1
    text[start:end] = lines
    open(config_path, 'w', encoding='utf-8', newline='\n').write('\n'.join(text))
    print(f'tmb_manual_toml: {len(lines) - 2} manual stubs written into {config_path}')


if __name__ == '__main__':
    main()
