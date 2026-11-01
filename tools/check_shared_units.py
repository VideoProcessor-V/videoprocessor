# Copyright(C) 2026 VP-V contributors
# SPDX-License-Identifier: GPL-3.0-only
"""Non-blocking duplicate-computation checker for shared units.

Usage: python -I tools/check_shared_units.py [--json] [--root PATH]

Always exits 0. Reports hits of registered shared computations that live
outside the unit that should own the single implementation.
"""
import argparse
import json
import os
import re
import sys

SCAN_ROOTS = ["src/VP-V", "src/VideoProcessor-Lib/vprenderer"]
SKIP_DIRS = {"tests", "VideoProcessor-Test", "3rdparty", "x64"}
EXTS = (".h", ".hpp", ".cpp", ".cc", ".cxx", ".glsl", ".inl")

_F = re.IGNORECASE
REGISTRY = [
    {"name": "gaussian_band_split",
     "owning_unit": "src/VP-V/modules/repair/grain_split",
     "signatures": [re.compile(p, _F) for p in (
         r"gauss(ian)?[_ ]?(blur|split|sigma|kernel)",
         r"\bsigma\s*=\s*(1|2\.5|6)\b",
         r"band[_ ]?split")]},
    {"name": "grain_estimate",
     "owning_unit": "src/VP-V/modules/repair/grain_split",
     "signatures": [re.compile(p, _F) for p in (
         r"grain[_ ]?(rms|level|estimate|noise)",
         r"wiener",
         r"noise[_ ]?(level|sigma|estimate)")]},
    {"name": "cut_fade_flash_judge",
     "owning_unit": "src/VP-V/modules/input/source_analysis",
     "signatures": [re.compile(p, _F) for p in (
         r"cut[_ ]?(fade|detect|judge)",
         r"FrameCutJudge",
         r"fade[_ ]?(detect|judge|gain)",
         r"scene[_ ]?cut")]},
    {"name": "half_luma_downsample",
     "owning_unit": "src/VP-V/modules/input/source_analysis/lane",
     "signatures": [re.compile(p, _F) for p in (
         r"half[_ ]?luma", r"HalfLuma", r"LOW_HALF", r"downsample")]},
    {"name": "block_statistics",
     "owning_unit": "src/VP-V/modules/input/source_analysis/regions",
     "signatures": [re.compile(p, _F) for p in (
         r"block[_ ]?(mean|var|stats|stat)", r"structure[_ ]?tensor")]},
    {"name": "histogram_apl",
     "owning_unit": "src/VP-V/modules/input/source_analysis",
     "signatures": [re.compile(p, _F) for p in (
         r"histogram", r"\bAPL\b", r"average[_ ]?picture[_ ]?level")]},
    {"name": "source_quality_read",
     "owning_unit": "src/VP-V/modules/input/source_analysis/identity",
     "signatures": [re.compile(p, _F) for p in (
         r"snap(shot)?\.bitrateQuality", r"\.bitrateQuality.*(strength|allowance|tier)")]},
    {"name": "global_motion",
     "owning_unit": "src/VP-V/modules/motion/estimation",
     "signatures": [re.compile(p, _F) for p in (
         r"global[_ ]?motion", r"FlowGlobal", r"engineGlobal")]},
]

KNOWN_DUPLICATES = [
    "half/quarter luma built 6-8 times",
    "full-resolution band split implemented 7 times (grain_split, TD fallback "
    "split, CfL x3 planes, DetailPulse, TextureRestore, VpDebander, "
    "interpolator grain bands)",
    "grain/noise estimated in 7 places",
    "cut/fade decided in about 10 places",
    "block statistics 8",
    "histogram/APL/peak 6",
    "global motion 3",
    "source quality read straight from the snapshot (P14 SourceQualityView is the one read API; LVR call sites move with P11)",
]


def _iter_files(root):
    for sr in SCAN_ROOTS:
        base = os.path.join(root, *sr.split("/"))
        for dp, dns, fns in os.walk(base):
            dns[:] = [d for d in dns if d not in SKIP_DIRS]
            for fn in fns:
                if fn.lower().endswith(EXTS):
                    yield os.path.join(dp, fn)


def _inside(rel, unit):
    return rel == unit or rel.startswith(unit.rstrip("/") + "/")


def scan(root, registry=None):
    """Return {computations:[{name,owner,hits,inside_owner}], total_outside}."""
    registry = REGISTRY if registry is None else registry
    out = [{"name": e["name"], "owner": e["owning_unit"], "hits": [],
            "inside_owner": 0} for e in registry]
    for path in _iter_files(root):
        rel = os.path.relpath(path, root).replace(os.sep, "/")
        try:
            with open(path, "r", encoding="utf-8", errors="replace") as f:
                lines = f.read().splitlines()
        except OSError:
            continue
        for i, line in enumerate(lines, 1):
            for entry, res in zip(registry, out):
                if any(s.search(line) for s in entry["signatures"]):
                    if _inside(rel, entry["owning_unit"]):
                        res["inside_owner"] += 1
                    else:
                        res["hits"].append(
                            {"file": rel, "line": i, "text": line.strip()[:200]})
    total = sum(len(c["hits"]) for c in out)
    return {"computations": out, "total_outside": total}


def format_text(result):
    L = ["Shared-unit duplicate-computation check (non-blocking, always exits 0)"]
    for c in result["computations"]:
        files = {h["file"] for h in c["hits"]}
        L.append("")
        L.append("== %s: %d hits outside owner in %d files "
                 "(%d inside owner; owner: %s)" % (
                     c["name"], len(c["hits"]), len(files),
                     c["inside_owner"], c["owner"]))
        for h in c["hits"]:
            L.append("%s:%d  %s  (owner: %s)" % (
                h["file"], h["line"], c["name"], c["owner"]))
    L.append("")
    L.append("Total hits outside owning units: %d" % result["total_outside"])
    L.append("")
    L.append("Known duplicates (doc 51 section 2):")
    L.extend("  - " + k for k in KNOWN_DUPLICATES)
    return "\n".join(L)


def main(argv=None):
    here = os.path.dirname(os.path.abspath(__file__))
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--json", action="store_true")
    ap.add_argument("--root", default=os.path.dirname(here))
    try:
        args = ap.parse_args(argv)
        result = scan(os.path.abspath(args.root))
        if args.json:
            print(json.dumps(result, indent=1))
        else:
            print(format_text(result))
    except SystemExit:
        pass
    except Exception as ex:  # non-blocking by contract
        print("check_shared_units (non-blocking): error: %s" % ex)
    return 0


if __name__ == "__main__":
    main()
    sys.exit(0)
