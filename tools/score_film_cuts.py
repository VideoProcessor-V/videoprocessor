"""Frame-accurate cut scoring for RealFilmCutDecisions.
usage: align_score.py <orig_gray.raw> <samp_gray.raw> <w> <h> <fps> <scores.txt> <cuts.csv> [exclude a-b,...] [extra cut times]
Each 2-fps sample is matched to its source frame (least squares among frames near k*0.5 s). A source
cut is a scene-score spike (score > 0.15 and > 3x the largest of the 3 frames each side), plus any extra
times given; a step k (sample k-1 -> k) holds a cut when frame(k-1) < cut frame <= frame(k).
"""
import sys, re, csv
import numpy as np

orig = np.fromfile(sys.argv[1], np.uint8)
samp = np.fromfile(sys.argv[2], np.uint8)
w, h = int(sys.argv[3]), int(sys.argv[4])
fps = float(sys.argv[5])
orig = orig.reshape(-1, h, w).astype(np.float32)
samp = samp.reshape(-1, h, w).astype(np.float32)
times, scores = [], []
t = None
for line in open(sys.argv[6]):
    m = re.search(r'pts_time:([0-9.]+)', line)
    if m: t = float(m.group(1)); continue
    m = re.search(r'scene_score=([0-9.]+)', line)
    if m: times.append(t); scores.append(float(m.group(1)))
scores = np.array(scores)
rows = [r for r in csv.DictReader(open(sys.argv[7]))]
excl = []
if len(sys.argv) > 8 and sys.argv[8]:
    for part in sys.argv[8].split(','):
        a, b = part.split('-'); excl.append((float(a), float(b)))
extra = [float(x) for x in sys.argv[9].split(',')] if len(sys.argv) > 9 and sys.argv[9] else []

n = min(len(samp), len(rows))
frame_of = []
for k in range(n):
    c = int(round(k * 0.5 * fps))
    lo, hi = max(0, c - 40), min(len(orig), c + 40)
    d = ((orig[lo:hi] - samp[k]) ** 2).mean(axis=(1, 2))
    frame_of.append(lo + int(np.argmin(d)))
offsets = [frame_of[k] - k * 0.5 * fps for k in range(n)]
print(f"sample -> frame offset (frames): median {np.median(offsets):.1f}, min {min(offsets):.1f}, max {max(offsets):.1f}")

cuts = []
for i in range(3, len(scores) - 3):
    nb = max(scores[i - 3:i].max(), scores[i + 1:i + 4].max())
    if scores[i] > 0.15 and scores[i] > 3 * nb:
        cuts.append(i)
for e in extra:
    cuts.append(int(round(e * fps)))
cuts = sorted(set(cuts))

def excluded(f):
    tt = f / fps
    return any(a <= tt <= b for a, b in excl)

cut_steps = {}
for c in cuts:
    if excluded(c): continue
    for k in range(1, n):
        if frame_of[k - 1] < c <= frame_of[k]:
            cut_steps.setdefault(k, []).append(c)
            break

def f(r, key): return float(r[key])
hits_on = hits_off = 0
misses = []
for k, cs in sorted(cut_steps.items()):
    r = rows[k]
    nxt = rows[k + 1] if k + 1 < n else None
    on = f(r, 'cut_fixes') > 0 or (nxt is not None and f(r, 'judged') == 0 and f(nxt, 'cut_fixes') > 0)
    off = f(r, 'cut_old') > 0 or (nxt is not None and f(r, 'judged') == 0 and f(nxt, 'cut_old') > 0)
    hits_on += on; hits_off += off
    if not on:
        misses.append((k, cs, f(r, 'decorrelation'), f(r, 'jump'), int(f(r, 'judged')), off, f(r, 'mean'), f(rows[k - 1], 'mean')))
false_on = [k for k in range(1, n) if f(rows[k], 'cut_fixes') > 0 and k not in cut_steps and (k - 1) not in cut_steps and not excluded(frame_of[k])]
false_off = [k for k in range(1, n) if f(rows[k], 'cut_old') > 0 and k not in cut_steps and (k - 1) not in cut_steps and not excluded(frame_of[k])]
N = len(cut_steps)
print(f"real cuts {N}: recall with latch fixes {hits_on}/{N} ({100.0 * hits_on / max(N, 1):.0f}%), old tile-mean jump {hits_off}/{N} ({100.0 * hits_off / max(N, 1):.0f}%)")
print(f"steps flagged as cuts with no source cut (not excluded): fixes {len(false_on)} {false_on}, old {len(false_off)}")
for k, cs, dec, jump, judged, off, mean, prevmean in misses:
    print(f"  MISSED step {k} (frames {frame_of[k-1]}->{frame_of[k]}, t {frame_of[k-1]/fps:.2f}->{frame_of[k]/fps:.2f}, cut frame {cs}): dec {dec:.3f} jump {jump:.3f} judged {judged} mean {prevmean:.3f}->{mean:.3f}; old {'hit' if off else 'missed'}")
for k in false_on:
    r = rows[k]
    print(f"  NO-SOURCE-CUT step {k} (t {frame_of[k-1]/fps:.2f}->{frame_of[k]/fps:.2f}): dec {f(r,'decorrelation'):.3f} jump {f(r,'jump'):.3f}")
