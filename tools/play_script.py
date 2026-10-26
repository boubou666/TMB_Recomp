#!/usr/bin/env python3
"""A PS2X_PAD_SCRIPT that plays: the menu presses into the first match (it starts about 150 s in), then accelerating,
steering both ways, firing (R1, L1, CIRCLE), switching weapons and braking until `end` seconds.

  PS2X_PAD_SCRIPT="$(tools/play_script.py 600)" timeout 600 build/game/PS2Recomp/ps2xRuntime/ps2EntryRunner game/SCES_503.60

A car standing still sends few sound commands; this one made the 989snd freeze (fixed in the fork, RECOMP_NOTES
"Freeze while playing") happen within 5 minutes.
"""
import sys

end = float(sys.argv[1]) if len(sys.argv) > 1 else 600
entries = ['25:CROSS/300'] + [f'{t}:CROSS/250' for t in range(60, 141, 6)]
t = 152.0
while t < end:
    entries.append(f'{t:.1f}:CROSS/5200')  # accelerate, held
    t += 5.0
t, k = 155.0, 0
while t < end:
    entries.append(f'{t:.1f}:{"LEFT" if k % 2 else "RIGHT"}/{450 + 150 * (k % 3)}')  # steer
    t += 1.3 + 0.4 * (k % 4)
    k += 1
for period, button, ms in ((0.6, 'R1', 150), (1.7, 'L1', 150), (2.9, 'CIRCLE', 150), (7.0, 'TRIANGLE', 120),
                           (13.0, 'SQUARE', 700)):
    t = 158.0
    while t < end:
        entries.append(f'{t:.1f}:{button}/{ms}')
        t += period
print(','.join(entries))
