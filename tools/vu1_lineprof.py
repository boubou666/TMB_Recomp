#!/usr/bin/env python3
"""Where the compiled VU1 microcode spends its time, by jit helper, from a PS2X_PROFILE output of tmb_vu1_replay
built with line tables in the microcode (-DTMB_VU1_AOT_LINES=ON, best in a build tree of its own).

  cmake -S . -B build/prof -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo -DTMB_VU1_AOT_LINES=ON
  cmake --build build/prof --target tmb_vu1_replay
  PS2X_PROFILE=25 PS2X_PROFILE_DELAY=6 PS2X_PROFILE_OUT=build/prof.txt build/prof/tmb_vu1_replay <trace> 1500
  tools/vu1_lineprof.py build/prof.txt build/prof/tmb_vu1_replay.exe [top]

The replay has to outlast delay + duration, or the profile is never written. Prints, for the replay thread, the
samples by helper called from a generated vu1_aot_* line (Vu1Jit::fmac, leave, blockReady...) or by function outside
the microcode, then by innermost source line. Needs binutils' addr2line on the PATH.
"""
import collections
import re
import subprocess
import sys

IMAGE_BASE = 0x140000000  # PE image base of the MinGW executables (no ASLR offset in the profile: rvas)


def main():
    profile, exe = sys.argv[1], sys.argv[2]
    top = int(sys.argv[3]) if len(sys.argv) > 3 else 40

    # Every thread gets the same number of samples; the replay thread is the one with the most inside the exe.
    per_thread = collections.defaultdict(collections.Counter)
    for line in open(profile):
        tid, leaf, _caller, count = line.split()
        per_thread[tid][int(leaf, 16)] += int(count)
    tid, leaves = max(per_thread.items(), key=lambda kv: sum(c for a, c in kv[1].items() if a))
    total = sum(leaves.values())

    addresses = [a for a in leaves if a]
    out = subprocess.run(['addr2line', '-a', '-f', '-i', '-C', '-e', exe],
                         input='\n'.join(hex(IMAGE_BASE + a) for a in addresses) + '\n',
                         capture_output=True, text=True).stdout.splitlines()
    frames = {}  # rva -> [(function, file:line)], innermost first
    current = None
    index = 0
    while index < len(out):
        if out[index].startswith('0x'):
            current = int(out[index], 16) - IMAGE_BASE
            frames[current] = []
            index += 1
            continue
        location = out[index + 1] if index + 1 < len(out) else '??:0'
        frames[current].append((out[index], location.split(' ')[0]))
        index += 2

    helper = collections.Counter()
    inner = collections.Counter()
    for address, count in leaves.items():
        chain = frames.get(address)
        if not chain:
            helper['[outside the exe]'] += count
            continue
        inner[f'{chain[0][0][:60]} @ {chain[0][1].split("/")[-1]}'] += count
        for level, (_function, location) in enumerate(chain):
            if 'vu1_aot_' in location or 'vu0_aot_' in location:
                callee = re.sub(r'<.*', '', chain[level - 1][0]) if level > 0 else '(generated code itself)'
                helper[callee[:70]] += count
                break
        else:
            helper[f'[outside microcode] {chain[-1][0][:60]}'] += count

    print(f'thread {tid}: {total} samples')
    print('\n-- by helper called from the microcode --')
    for name, count in helper.most_common(top):
        print(f'{100.0 * count / total:6.2f}%  {name}')
    print('\n-- by innermost function @ file:line --')
    for name, count in inner.most_common(top):
        print(f'{100.0 * count / total:6.2f}%  {name}')


if __name__ == '__main__':
    main()
