#!/usr/bin/env python
"""Option model for combination testing -> tests/vpv/dimensions.json.

Sources: docs/design/vpv-tiers.json (unit tiers + toggles) plus fixed extra
dimensions (signal, rate, queue_frames, low_latency, source_size, output,
geometry). Planned keys are not dimensions. slots.json is cross-checked
(every unit slot/toggle slot must exist).

Usage:  python -I tools/vpv_tests/dimensions.py [--check]
"""
import json, sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
TIERS = ROOT / "docs/design/vpv-tiers.json"
SLOTS = ROOT / "docs/design/slots.json"
OUT = ROOT / "tests/vpv/dimensions.json"

FIXED = [
    ("signal", ["SDR", "HDR10", "LLDV", "DV_tunnel"]),
    ("rate", ["23.976", "24", "25", "29.97", "30", "50", "59.94", "60"]),
    ("queue_frames", ["4", "3", "2"]),
    ("low_latency", ["off", "on"]),
    ("source_size", ["1080p", "1440p", "2160p"]),
    ("output", ["2560x1600", "3840x2160"]),
    ("geometry", ["none", "crop", "A-lens", "NLS"]),
]


def build():
    t = json.loads(TIERS.read_text(encoding="utf-8"))
    slot_ids = {s["id"] for s in json.loads(SLOTS.read_text(encoding="utf-8"))["slots"]}
    dims, problems = [], []
    dims += [{"name": n, "values": v, "source": "fixed"} for n, v in FIXED]
    for u in t["units"]:
        for sl in u.get("slots", []):
            if sl not in slot_ids: problems.append(f"unit {u['id']}: unknown slot {sl}")
        if u.get("tiers"):
            dims.append({"name": f"{u['id']}.tier", "values": list(u["tiers"]), "source": "tiers"})
        for g in u.get("toggles", []):
            if g.get("slot") not in slot_ids: problems.append(f"toggle {g['key']}: unknown slot {g.get('slot')}")
            dims.append({"name": g["key"], "values": [str(x) for x in g["values"]], "source": f"toggle:{u['id']}"})
    names = [d["name"] for d in dims]
    for n in {x for x in names if names.count(x) > 1}: problems.append(f"duplicate dimension {n}")
    for d in dims:
        for x in [d["name"]] + d["values"]:
            if any(c in x for c in ";= "): problems.append(f"bad character in {x!r}")
    return {"schema": "vpv-dimensions/1", "dimensions": dims}, problems


def main():
    model, problems = build()
    text = json.dumps(model, indent=1) + "\n"
    if "--check" in sys.argv:
        for p in problems: print("ERROR:", p)
        cur = OUT.read_text(encoding="utf-8").replace("\r\n", "\n") if OUT.exists() else None
        if cur != text: problems.append("stale"); print("ERROR: stale", OUT.name)
        print("dimensions check:", "FAIL" if problems else "ok")
        sys.exit(1 if problems else 0)
    for p in problems: print("WARNING:", p)
    OUT.parent.mkdir(parents=True, exist_ok=True)
    OUT.write_text(text, encoding="utf-8", newline="\n")
    print(f"wrote {len(model['dimensions'])} dimensions")

if __name__ == "__main__":
    main()
