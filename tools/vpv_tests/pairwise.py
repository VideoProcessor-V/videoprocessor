#!/usr/bin/env python
"""All-pairs (pairwise) combination generator over tests/vpv/dimensions.json.

Greedy AETG-style, deterministic for a given --seed (default 1).
Output line format: name=pw-0001;dim=value;dim=value;...

Usage:
  python -I tools/vpv_tests/pairwise.py [--seed N] [--out tests/vpv/pairwise.txt]
  python -I tools/vpv_tests/pairwise.py --verify FILE [--dims dimensions.json]
  python -I tools/vpv_tests/pairwise.py --stats
  python -I tools/vpv_tests/pairwise.py --selftest

CONSTRAINTS: predicates over a dict {dim: value} that return False for a
forbidden combination. The dict may be PARTIAL while a row is built, so use
.get() and only reject when every key you test is present. Pairs that no
valid row can cover are reported as infeasible (and ignored by --verify only
if the constraints exclude them).
"""
import json, random, sys, itertools
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
DIMS = ROOT / "tests/vpv/dimensions.json"
OUT = ROOT / "tests/vpv/pairwise.txt"

# Product rules go here. Deliberately empty: do not invent rules.
CONSTRAINTS = [
    # Example (disabled):
    # lambda c: not (c.get("signal") == "DV_tunnel" and c.get("geometry") == "NLS"),
]


def valid(c):
    return all(p(c) for p in CONSTRAINTS)


def load_dims(path=DIMS):
    d = json.loads(Path(path).read_text(encoding="utf-8"))["dimensions"]
    return [(x["name"], list(x["values"])) for x in d]


def generate(dims, seed=1, tries=12):
    rng = random.Random(seed)
    n = len(dims)
    uncovered = {((i, a), (j, b)) for i in range(n) for j in range(i + 1, n)
                 for a in range(len(dims[i][1])) for b in range(len(dims[j][1]))}
    infeasible = set()
    rows = []

    def as_dict(row):
        return {dims[i][0]: dims[i][1][v] for i, v in row.items()}

    while uncovered:
        pair = min(uncovered)                     # deterministic seed pair
        (i, a), (j, b) = pair
        best, best_gain = None, -1
        for _ in range(tries):
            row = {i: a, j: b}
            if not valid(as_dict(row)):
                break
            order = [k for k in range(n) if k not in row]
            rng.shuffle(order)
            for k in order:
                scored = []
                for v in range(len(dims[k][1])):
                    row[k] = v
                    if not valid(as_dict(row)):
                        continue
                    g = sum(1 for m, w in row.items() if m != k and
                            ((min(m, k), row[min(m, k)] if m < k else v),
                             (max(m, k), v if m < k else row[m])) in uncovered)
                    scored.append((g, rng.random(), v))
                if not scored:
                    row = None; break
                row[k] = max(scored)[2]
            if row is None:
                continue
            gain = sum(1 for p in itertools.combinations(sorted(row), 2)
                       if ((p[0], row[p[0]]), (p[1], row[p[1]])) in uncovered)
            if gain > best_gain:
                best, best_gain = dict(row), gain
        if best is None:
            uncovered.discard(pair); infeasible.add(pair); continue
        for p in itertools.combinations(sorted(best), 2):
            uncovered.discard(((p[0], best[p[0]]), (p[1], best[p[1]])))
        rows.append(best)
    lines = []
    for r, row in enumerate(rows, 1):
        lines.append(f"name=pw-{r:04d};" + ";".join(f"{dims[i][0]}={dims[i][1][row[i]]}" for i in range(n)))
    return lines, [(dims[i][0], dims[i][1][a], dims[j][0], dims[j][1][b]) for (i, a), (j, b) in sorted(infeasible)]


def parse_line(line):
    kv = dict(f.split("=", 1) for f in line.strip().split(";") if f)
    kv.pop("name", None)
    return kv


def verify(dims, lines):
    covered = set()
    for ln in lines:
        if not ln.strip() or ln.startswith("#"): continue
        kv = parse_line(ln)
        for p in itertools.combinations(sorted(kv), 2):
            covered.add((p[0], kv[p[0]], p[1], kv[p[1]]))
    missing = []
    for (an, av), (bn, bv) in itertools.combinations([(n, v) for n, vs in dims for v in vs], 2):
        if an == bn: continue
        k = (an, av, bn, bv) if an < bn else (bn, bv, an, av)
        if k not in covered:
            if valid({an: av, bn: bv}): missing.append(k)
    return missing


def selftest():
    dims = [("a", ["0", "1", "2"]), ("b", ["x", "y"]), ("c", ["p", "q", "r", "s"]), ("d", ["u", "v"]), ("e", ["m", "n", "o"])]
    lines, inf = generate(dims, 1)
    miss = verify(dims, lines)
    assert not inf and not miss, (inf, miss)
    assert generate(dims, 1)[0] == lines, "not deterministic"
    print(f"selftest ok: {len(lines)} rows cover all pairs")


def main():
    a = sys.argv[1:]
    def opt(name, default=None):
        return a[a.index(name) + 1] if name in a else default
    if "--selftest" in a: return selftest()
    dims = load_dims(opt("--dims", DIMS))
    if "--verify" in a:
        lines = Path(opt("--verify")).read_text(encoding="utf-8").splitlines()
        miss = verify(dims, lines)
        for m in miss: print("UNCOVERED:", *m)
        print("verify:", "FAIL, %d uncovered" % len(miss) if miss else "all pairs covered")
        sys.exit(1 if miss else 0)
    lines, inf = generate(dims, int(opt("--seed", 1)))
    if "--stats" in a:
        total = sum(len(x[1]) * len(y[1]) for x, y in itertools.combinations(dims, 2))
        print(f"dimensions={len(dims)} pairs={total} rows={len(lines)} infeasible={len(inf)}")
        return
    for p in inf: print("INFEASIBLE:", *p)
    out = Path(opt("--out", OUT)); out.parent.mkdir(parents=True, exist_ok=True)
    out.write_text("\n".join(lines) + "\n", encoding="utf-8", newline="\n")
    print(f"wrote {len(lines)} rows to {out}")

if __name__ == "__main__":
    main()
