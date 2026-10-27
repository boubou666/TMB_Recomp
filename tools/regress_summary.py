#!/usr/bin/env python3
"""regress_summary.py <out-dir> <exit-code> <duration-s> <ps2recomp-hash>: <out-dir>/summary.txt from a scripts/regress.sh
run, in the fork's port regression format (key=value lines, NA when TMB can't tell).

The match is the TMB_TRACE_FRAMES windows (100 frames each) where the VIF1 thread works 3 ms a frame or more: menus and
loading screens barely use it, the match draws its 3D through it. Hang: the presented frame (a capture every 4 s) stayed
the same for 20 s or more after the match started.
"""
import glob, hashlib, os, re, statistics, sys

out, code, duration, ps2recomp = sys.argv[1], int(sys.argv[2]), float(sys.argv[3]), sys.argv[4]
log = open(os.path.join(out, 'run.log'), encoding='utf-8', errors='replace').read().splitlines()

window = re.compile(r'\[tmb:frames\] W avg ([\d.]+) max [\d.]+ ms, B avg ([\d.]+) max .*?(?:over 25 ms (\d+) )?\| cpu ms/frame:(.*)')
fps, over25, windows, first_match = [], 0, 0, None
for line in log:
    m = window.search(line)
    if not m:
        continue
    windows += 1
    vif1 = re.search(r'vif1 ([\d.]+)', m[4])
    if not vif1 or float(vif1[1]) < 3.0:
        continue
    if first_match is None:
        first_match = windows
    fps.append(1000.0 / (float(m[1]) + float(m[2])))
    over25 = over25 + int(m[3]) if m[3] is not None and over25 is not None else None

# Captures are named by their order; identical ones in a row mean the presented frame stopped changing.
captures = sorted(glob.glob(os.path.join(out, 'frame_*.png')))
hang = False
if fps and captures:
    start = len(captures) * (first_match - 1) // max(windows, 1)  # roughly where the match begins
    same = 0
    previous = None
    for path in captures[start:]:
        digest = hashlib.md5(open(path, 'rb').read()).hexdigest()
        same = same + 1 if digest == previous else 0
        previous = digest
        hang |= same * 4 >= 20
if captures:
    with open(captures[-1], 'rb') as src, open(os.path.join(out, 'last_frame.png'), 'wb') as dst:
        dst.write(src.read())

crashed = code not in (0, 124) or any(re.search(r'segfault|access violation|terminate called|Assertion', l) for l in log)
exit_kind = 'crash' if crashed else 'hang' if hang else 'ok' if code == 124 else 'timeout'
sorted_fps = sorted(fps)
summary = {
    'port': 'tmb',
    'ps2recomp': ps2recomp,
    'route_reached': int(len(fps) >= 10),  # at least 1000 frames of the match
    'exit': exit_kind,
    'duration_s': f'{duration:.0f}',
    'frames_presented': windows * 100,
    'fps_mean': f'{statistics.mean(fps):.1f}' if fps else 'NA',
    'fps_p10': f'{sorted_fps[len(sorted_fps) // 10]:.1f}' if fps else 'NA',
    'frames_over_25ms': over25 if fps and over25 is not None else 'NA',
    'gs_frames_per_5s_median': 'NA',
    'gs_frames_per_5s_min': 'NA',
    'spu2_underruns': sum('Sound stalled' in l for l in log),
    'error_lines': sum(bool(re.search(r' error|exception|segfault|crash', l, re.I)) for l in log),
    'warning_lines': sum(bool(re.search(r'warn', l, re.I)) for l in log),
}
with open(os.path.join(out, 'summary.txt'), 'w') as f:
    f.writelines(f'{k}={v}\n' for k, v in summary.items())
print(' '.join(f'{k}={v}' for k, v in summary.items()))
