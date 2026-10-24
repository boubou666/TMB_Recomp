# tools/falls.py pos.txt (TMB_TRACE_POS output): per vehicle, the falls under the arena (height going under -100;
# the quarry floor is at -86) and the vblank fields between two traced calls (0: substeps of one frame).
import collections, sys
d = collections.defaultdict(list)
for line in open(sys.argv[1]):
    p = line.split()
    if len(p) != 5: continue
    try: d[p[1]].append((int(p[0]), float(p[4])))
    except ValueError: pass
total = 0; span = 0; hist = collections.Counter()
for v, s in sorted(d.items()):
    falls = [s[i][0] for i in range(1, len(s)) if s[i][1] < -100 <= s[i - 1][1]]
    total += len(falls); span = max(span, s[-1][0] - s[0][0])
    hist.update(min(s[i][0] - s[i - 1][0], 9) for i in range(1, len(s)))
    print(v, len(s), 'zmin %.1f' % min(z for _, z in s), 'falls at', falls)
print('falls', total, 'over', span, 'vblanks; fields/frame', sorted(hist.items()))
