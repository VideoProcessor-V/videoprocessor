#!/usr/bin/env python
"""Seeded random sampler over tests/vpv/dimensions.json.

Usage: python -I tools/vpv_tests/random_combos.py --seed N --count K --out FILE
Writes K lines  name=rnd-<seed>-<i>;dim=value;...  Honors pairwise.CONSTRAINTS
(rows violating them are redrawn).
"""
import random, sys
from pathlib import Path
sys.path.insert(0, str(Path(__file__).resolve().parent))
import pairwise


def main():
    a = sys.argv[1:]
    opt = lambda n, d=None: a[a.index(n) + 1] if n in a else d
    seed, count = int(opt("--seed", 1)), int(opt("--count", 500))
    out = Path(opt("--out", pairwise.ROOT / "tests/vpv/random.txt"))
    dims = pairwise.load_dims()
    rng = random.Random(seed)
    lines = []
    for i in range(1, count + 1):
        for _ in range(1000):
            row = {n: rng.choice(v) for n, v in dims}
            if pairwise.valid(row): break
        else:
            sys.exit("constraints reject every draw")
        lines.append(f"name=rnd-{seed}-{i};" + ";".join(f"{k}={v}" for k, v in row.items()))
    out.parent.mkdir(parents=True, exist_ok=True)
    out.write_text("\n".join(lines) + "\n", encoding="utf-8", newline="\n")
    print(f"wrote {count} rows to {out}")

if __name__ == "__main__":
    main()
