#!/usr/bin/env python3
"""A PS2X_PAD_SCRIPT for one Challenge-mode match: ONE PLAYER, Challenge, the first vehicle, the arena `rights` RIGHT
presses after Suburbs (0 Suburbs, 1 Junkyard, 4 Skyscrapers, 5 Downtown, 6 Highway Loop, 7 Snowy Roads, 8 Drive-In;
2 Freeway is locked and 3 Prison Passage can't be picked in Challenge), two opponents, then driving and firing as
tools/play_script.py from 205 s (the match starts about 200 s in) until `end` seconds.

  PS2X_PAD_SCRIPT="$(tools/arena_script.py 4 380)" timeout 380 build/game/PS2Recomp/ps2xRuntime/ps2EntryRunner game/SCES_503.60
"""
import sys

rights = int(sys.argv[1]) if len(sys.argv) > 1 else 4
end = float(sys.argv[2]) if len(sys.argv) > 2 else 380
entries = ['25:CROSS/300', '62:CROSS/250', '72:DOWN/200', '80:CROSS/250', '92:CROSS/250']
t = 112
for _ in range(rights):
    entries.append(f'{t}:RIGHT/200')
    t += 6
entries += ['162:CROSS/250', '174:CROSS/250', '180:RIGHT/200', '186:CROSS/250', '194:SQUARE/250']
t = 205.0
while t < end:
    entries.append(f'{t:.1f}:CROSS/5200')  # accelerate, held
    t += 5.0
t, k = 208.0, 0
while t < end:
    entries.append(f'{t:.1f}:{"LEFT" if k % 2 else "RIGHT"}/{450 + 150 * (k % 3)}')  # steer
    t += 1.3 + 0.4 * (k % 4)
    k += 1
for period, button, ms in ((0.6, 'R1', 150), (1.7, 'L1', 150), (2.9, 'CIRCLE', 150), (7.0, 'TRIANGLE', 120),
                           (13.0, 'SQUARE', 700)):
    t = 211.0
    while t < end:
        entries.append(f'{t:.1f}:{button}/{ms}')
        t += period
print(','.join(entries))
