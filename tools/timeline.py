#!/usr/bin/env python3
"""Reads a TMB_TRACE_TIMELINE file: per frame (end of real-time processing to the next), what the game thread waited
for and when the vif1 / gs threads were busy, then a 1 ms chart of a few frames.

  tools/timeline.py <timeline.txt> [first-frame-to-chart] [frames-to-chart]

Chart rows: game / vif1 / gs threads, '#' busy most of the millisecond, '+' part of it; markers: B vblank handler,
U TheGame::Update, H hier, P sceGsSyncPath wait (p its end), V sceGsSyncV wait (v its end), E end of processing.
"""
import sys
from bisect import bisect_left

END_SYNC_RA = 0x15B49C  # sceGsSyncPath after drawOverlay, just before timerEndOfRealTimeProcessing


def load(path):
    events, samples = [], []
    for line in open(path):
        f = line.split()
        if f[0] == 'e':
            events.append((int(f[1]) / 1e6, f[2], int(f[3], 16)))
        else:
            samples.append((int(f[1]) / 1e6, int(f[2]) / 1e6, int(f[3]) / 1e6, int(f[4]) / 1e6))
    events.sort()
    samples.sort()
    return events, samples


def cpu_at(samples, times, t, thread):
    i = min(max(bisect_left(times, t), 1), len(samples) - 1)
    (t0, *c0), (t1, *c1) = samples[i - 1], samples[i]
    a = (t - t0) / (t1 - t0) if t1 > t0 else 0
    return c0[thread] + (c1[thread] - c0[thread]) * a


def main():
    events, samples = load(sys.argv[1])
    times = [s[0] for s in samples]
    ends = [t for t, k, _ in events if k == 'E']
    names = ('game', 'vif1', 'gs')
    rows = []
    for start, end in zip(ends, ends[1:]):
        inside = [(t, k, a) for t, k, a in events if start <= t < end]
        r = {'start': start, 'dur': end - start,
             'vblanks': sum(1 for _, k, _ in inside if k == 'B'),
             'fields': next((a for _, k, a in inside if k == 'U'), 0)}
        for i, n in enumerate(names):
            r[n] = cpu_at(samples, times, end, i) - cpu_at(samples, times, start, i)
        # Waits: sceGsSyncPath (by call site) and sceGsSyncV.
        r['endsync'] = r['syncpath'] = r['syncv'] = 0.0
        open_ = {}
        for t, k, a in inside:
            if k in 'PV':
                open_[(k, a)] = t
            elif k in 'pv' and (k.upper(), a) in open_:
                d = t - open_.pop((k.upper(), a))
                if k == 'v':
                    r['syncv'] += d
                elif a == END_SYNC_RA:
                    r['endsync'] += d
                else:
                    r['syncpath'] += d
        h = next((t for t, k, _ in inside if k == 'H'), None)
        r['hier_at'] = h - start if h is not None else -1
        e_sync = next((t for t, k, a in inside if k == 'P' and a == END_SYNC_RA), None)
        r['endsync_at'] = e_sync - start if e_sync is not None else -1
        rows.append(r)

    if not rows:
        print('no frames')
        return
    n = len(rows)
    avg = lambda key: sum(r[key] for r in rows) / n
    print(f'{n} frames, {avg("dur"):.1f} ms each, {avg("vblanks"):.2f} vblanks, {avg("fields"):.2f} fields')
    print(f'CPU per frame: game {avg("game"):.1f}  vif1 {avg("vif1"):.1f}  gs {avg("gs"):.1f} ms')
    print(f'game waits per frame: end-of-frame sceGsSyncPath {avg("endsync"):.1f}, other sceGsSyncPath '
          f'{avg("syncpath"):.1f}, sceGsSyncV {avg("syncv"):.1f} ms')
    print(f'hier starts {avg("hier_at"):.1f} ms into the frame, the end-of-frame sync {avg("endsync_at"):.1f} ms')
    hist = {}
    for r in rows:
        hist[round(r['dur'])] = hist.get(round(r['dur']), 0) + 1
    print('frame times (ms: count):', ', '.join(f'{k}: {v}' for k, v in sorted(hist.items())))

    first = int(sys.argv[2]) if len(sys.argv) > 2 else n // 2
    count = int(sys.argv[3]) if len(sys.argv) > 3 else 4
    chart_start = rows[min(first, n - 1)]['start']
    chart_end = rows[min(first + count, n) - 1]['start'] + rows[min(first + count, n) - 1]['dur']
    width = int(chart_end - chart_start) + 1
    print(f'\nchart from frame {first}, 1 column = 1 ms')
    for i, name in enumerate(names):
        line = ''
        for ms in range(width):
            t = chart_start + ms
            busy = cpu_at(samples, times, t + 1, i) - cpu_at(samples, times, t, i)
            line += '#' if busy > 0.6 else '+' if busy > 0.15 else '.'
        print(f'{name:>5} {line}')
    marks = [' '] * width
    for t, k, _ in events:
        if chart_start <= t < chart_start + width:
            ms = int(t - chart_start)
            if marks[ms] == ' ' or k in 'BE':
                marks[ms] = k
    print(f'{"":>5} {"".join(marks)}')


if __name__ == '__main__':
    main()
