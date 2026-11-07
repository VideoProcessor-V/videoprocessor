# Copyright(C) 2026 VP-V contributors
# SPDX-License-Identifier: GPL-3.0-only
"""Shared-unit duplicate-computation checker and ratchet (SU0).

Usage: python -I tools/check_shared_units.py [--json] [--root PATH] [--strict]
                                             [--write-baseline] [--baseline FILE]

Default run: report only, always exits 0. Every hit of a registered shared
computation (units U1..U14 of doc 62) gets a role:

  owner               inside the owning unit's folder (counted, not listed)
  declaration         catalogs, manifests, config, diagnostics, comment-only lines
  consumer            the line uses the unit's accessor (allow list)
  output_domain       LED / output code that deliberately keeps its own law
  bill                Bill's files (src/VideoProcessor-Lib non-VP code): reported, never blocking
  known_false_positive  listed in tools/shared_units_baseline.json with a reason
  duplicate           everything else: a private copy of a shared computation

--strict exits 1 when a blocking unit's duplicate count exceeds its baseline
(tools/shared_units_baseline.json). --write-baseline records the current
duplicate counts but refuses to raise the baseline of a blocking unit.
Baselines may only shrink (tools/test_check_shared_units.py asserts it).
"""
import argparse
import json
import os
import re
import sys

SCAN_ROOTS = ["src/VP-V", "src/VideoProcessor-Lib/vprenderer"]
SKIP_DIRS = {"tests", "VideoProcessor-Test", "3rdparty", "x64"}
EXTS = (".h", ".hpp", ".cpp", ".cc", ".cxx", ".glsl", ".inl")
BASELINE_REL = "tools/shared_units_baseline.json"

_F = re.IGNORECASE


def _c(*patterns, flags=_F):
    return [re.compile(p, flags) for p in patterns]


# id, legacy name, owner folder(s), signatures, allow (consumer accessors).
REGISTRY = [
    {"id": "U1", "name": "grain_estimate",
     "owning_unit": ["src/VP-V/modules/repair/grain_split"],
     "signatures": _c(r"grain[_ ]?(rms|level|estimate|noise)", r"wiener",
                      r"noise[_ ]?(level|sigma|estimate)"),
     "allow": _c(r"vpu_grain_view\w*", r"GrainSplit::Gpu::ViewGlsl")},
    {"id": "U2", "name": "gaussian_band_split",
     "owning_unit": ["src/VP-V/modules/repair/grain_split"],
     "signatures": _c(r"gauss(ian)?[_ ]?(blur|split|sigma|kernel)",
                      r"\bsigma\s*=\s*(1|2\.5|6)\b", r"band[_ ]?split",
                      r"exp\(\s*-0\.5", r"ceil\(\s*3(\.5)?(\.0)?\s*\*",
                      r"GAUSS_", r"HalfSigma", r"\bgp_w\b", r"LowWeightsGlsl",
                      r"GrainPreserve::kSplitSigma", r"separable\s+gauss",
                      r"#\s*include\s*[<\"].*GrainSplit\.h"),
     "allow": _c(r"VpUnit::BandFromBase::\w+", r"vpu_band_from_base\w*",
                 r"BandFromBase\.h")},
    {"id": "U3", "name": "half_luma_downsample",
     "owning_unit": ["src/VP-V/modules/input/source_analysis/lane"],
     "signatures": _c(r"half[_ ]?luma", r"HalfLuma", r"LOW_HALF", r"downsample",
                      r"sum\s*>>\s*4", r"\+\s*2u?\)\s*>>\s*2",
                      r"2x2\s+box|box\s+average",
                      r"0\.25\s*\*\s*\(", r"\*\s*0\.25"),
     "allow": _c(r"VpUnit::HalfLuma::\w+", r"vpu_half_luma\w*", r"vpu_quarter_luma\w*",
                 r"shared/HalfLuma\.h")},
    {"id": "U4", "name": "block_statistics",
     "owning_unit": ["src/VP-V/modules/input/source_analysis/regions"],
     "signatures": _c(r"block[_ ]?(mean|var|stats|stat)", r"structure[_ ]?tensor"),
     "allow": _c(r"vpu_block_stats")},
    {"id": "U5", "name": "laplacian_ringing",
     "owning_unit": ["src/VP-V/modules/input/source_analysis/regions"],
     "signatures": _c(r"laplac"),
     "allow": _c(r"vpu_band_energy")},
    {"id": "U6", "name": "histogram_apl",
     "owning_unit": ["src/VP-V/modules/input/source_analysis"],
     "signatures": _c(r"histogram", r"\bAPL\b", r"average[_ ]?picture[_ ]?level"),
     "allow": _c(r"SourceStats::")},
    {"id": "U7", "name": "cut_fade_flash_judge",
     "owning_unit": ["src/VP-V/modules/input/source_analysis"],
     "signatures": _c(r"cut[_ ]?(fade|detect|judge)", r"FrameCutJudge",
                      r"fade[_ ]?(detect|judge|gain)", r"scene[_ ]?cut"),
     "allow": _c(r"CutFadeJudge::")},
    {"id": "U8", "name": "active_picture_rect",
     "owning_unit": ["src/VP-V/modules/input/source_analysis/picture"],
     "signatures": _c(r"\bPictureArea\b"),
     "allow": _c(r"vpu_in_picture")},
    {"id": "U9", "name": "light_level",
     "owning_unit": ["src/VP-V/modules/input/source_analysis/light_level"],
     "signatures": [],
     "allow": _c(r"vpu_light_")},
    {"id": "U10", "name": "global_motion",
     "owning_unit": ["src/VP-V/modules/motion/estimation"],
     "signatures": _c(r"global[_ ]?motion", r"FlowGlobal", r"engineGlobal"),
     "allow": _c(r"MotionValidity::")},
    {"id": "U11", "name": "camera_fit",
     "owning_unit": ["src/VP-V/modules/motion/estimation/global_model"],
     "signatures": [], "allow": []},
    {"id": "U12", "name": "source_quality",
     "owning_unit": ["src/VP-V/modules/input/source_analysis/identity"],
     "signatures": _c(r"snap(shot)?\.bitrateQuality", r"\.bitrateQuality.*(strength|allowance|tier)"),
     "allow": _c(r"SourceQuality::")},
    # R1-W1-RES: the reserved temporal-detail engine products (one read API, TdProducts::Read; the engine itself is reset 2).
    {"id": "U13", "name": "temporal_detail_products",
     "owning_unit": ["src/VP-V/modules/repair/temporal_detail/products"],
     "signatures": _c(r"trail[_ ]?confidence", r"real[_ ]?detail[_ ]?(map|product)", r"i[-_ ]to[-_ ]i[_ ]drift",
                      r"quality[_ ]?pattern", r"reference[_ ]?(set[_ ]?)?mirror", r"refined[_ ]?vectors?",
                      r"\btd\.(residual|validity|stability|lighting|drift|mirror|base_long|base_short|refine_pixel)\b"),
     "allow": _c(r"TdProducts::\w+")},
    # R1-W1-RES: the basic polyphase chroma upsample (a stand-in for the hybrid CfL + MV chroma of reset 2).
    {"id": "U14", "name": "chroma_upsample_polyphase",
     "owning_unit": ["src/VP-V/modules/scaling/chroma_upscale/basic"],
     "signatures": _c(r"polyphase", r"\blanczos\w*\s*\(", r"chroma[_ ]?siting"),
     "allow": _c(r"BasicChroma::\w+")},
]

DECLARATION_PATHS = _c(
    r"/core/[^/]*Catalog[^/]*$", r"/core/Products\.h$", r"FallbackCatalog",
    r"MANIFEST", r"/config/", r"/diagnostics?/", r"Diagnostics?[^/]*$",
    r"Settings?[^/]*Catalog")
OUTPUT_DOMAIN_PATHS = _c(r"^src/VP-V/modules/drive/led_", r"^src/VP-V/modules/output/")
ROLES = ("declaration", "consumer", "output_domain", "bill",
         "known_false_positive", "duplicate")

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


def _comment_flags(lines):
    """Per line: True when the line is comment-only. A leading '*' counts only inside a /* ... */ block."""
    flags, in_block = [], False
    for line in lines:
        t = line.strip()
        if in_block:
            flags.append(True)
            if "*/" in t:
                in_block = False
        elif t.startswith("//"):
            flags.append(True)
        elif t.startswith("/*"):
            flags.append(True)
            in_block = "*/" not in t
        else:
            flags.append(False)
    return flags


def _is_bill(rel):
    if not rel.startswith("src/VideoProcessor-Lib/"):
        return False
    name = rel.rsplit("/", 1)[-1]
    return "/VP-V/" not in rel and not name.lower().startswith("vp")


def load_baseline(root, path=None):
    p = path or os.path.join(root, *BASELINE_REL.split("/"))
    try:
        with open(p, "r", encoding="utf-8") as f:
            return json.load(f)
    except (OSError, ValueError):
        return {"schema": 1, "units": {}}


def _kfp_reason(kfps, rel, text):
    """Return (kind, reason) when a baseline known_false_positive entry covers the hit."""
    for e in kfps:
        f = e.get("file", "")
        if not (rel == f or (f.endswith("/") and rel.startswith(f))):
            continue
        pat = e.get("pattern", "")
        if pat and not re.search(pat, text):
            continue
        return e.get("kind", "false_positive"), e.get("reason", "")
    return None, None


def _strip_allowed(entry, text):
    """(text with every allow-listed accessor removed, whether any was removed)."""
    out, hit = text, False
    for a in entry["allow"]:
        out, n = a.subn(" ", out)
        hit = hit or n > 0
    return out, hit


def classify(entry, rel, text, kfps, comment=False):
    if _is_bill(rel):
        return "bill", ""
    if comment or any(p.search(rel) for p in DECLARATION_PATHS):
        return "declaration", ""
    stripped, used = _strip_allowed(entry, text)
    # A line that holds an accessor AND a private signature is still a duplicate.
    if used and not any(sg.search(stripped) for sg in entry["signatures"]):
        return "consumer", ""
    if any(p.search(rel) for p in OUTPUT_DOMAIN_PATHS):
        return "output_domain", ""
    kind, reason = _kfp_reason(kfps, rel, text)
    if kind == "residual":
        return "duplicate", reason
    if kind is not None:
        return "known_false_positive", reason
    return "duplicate", ""


def scan(root, registry=None, baseline=None):
    """Return {computations:[...], total_outside, total_by_role, total_duplicates}."""
    registry = REGISTRY if registry is None else registry
    baseline = load_baseline(root) if baseline is None else baseline
    units = baseline.get("units", {})
    out = []
    for e in registry:
        b = units.get(e["id"], {})
        owners = e["owning_unit"] if isinstance(e["owning_unit"], list) else [e["owning_unit"]]
        out.append({"id": e["id"], "name": e["name"], "owner": ", ".join(owners),
                    "hits": [], "inside_owner": 0,
                    "blocking": bool(b.get("blocking", False)),
                    "baseline": b.get("baseline_duplicates"),
                    "roles": {r: 0 for r in ROLES}, "duplicates": 0})
    for path in _iter_files(root):
        rel = os.path.relpath(path, root).replace(os.sep, "/")
        try:
            with open(path, "r", encoding="utf-8", errors="replace") as f:
                lines = f.read().splitlines()
        except OSError:
            continue
        cflags = _comment_flags(lines)
        for i, line in enumerate(lines, 1):
            for entry, res in zip(registry, out):
                if not any(s.search(line) for s in entry["signatures"]):
                    continue
                owners = entry["owning_unit"] if isinstance(entry["owning_unit"], list) \
                    else [entry["owning_unit"]]
                if any(_inside(rel, o) for o in owners):
                    res["inside_owner"] += 1
                    continue
                kfps = units.get(entry["id"], {}).get("known_false_positive", [])
                role, reason = classify(entry, rel, line, kfps, cflags[i - 1])
                hit = {"file": rel, "line": i, "text": line.strip()[:200], "role": role}
                if reason:
                    hit["reason"] = reason
                res["hits"].append(hit)
                res["roles"][role] += 1
                if role == "duplicate":
                    res["duplicates"] += 1
    by_role = {r: sum(c["roles"][r] for c in out) for r in ROLES}
    return {"computations": out,
            "total_outside": sum(len(c["hits"]) for c in out),
            "total_by_role": by_role,
            "total_duplicates": by_role["duplicate"]}


def strict_violations(result):
    """Blocking units whose duplicate count exceeds the baseline (missing baseline = 0)."""
    bad = []
    for c in result["computations"]:
        if c["blocking"]:
            base = c["baseline"] if c["baseline"] is not None else 0
            if c["duplicates"] > base:
                bad.append((c["id"], c["duplicates"], base))
    return bad


def build_baseline(result, old):
    """The new baseline document, or raise ValueError when a blocking baseline would rise."""
    new = {k: v for k, v in old.items() if k != "units"}
    new.setdefault("schema", 1)
    new["units"] = {}
    old_units = old.get("units", {})
    for c in result["computations"]:
        o = old_units.get(c["id"], {})
        blocking = bool(o.get("blocking", False))
        prev = o.get("baseline_duplicates")
        if blocking and prev is None:
            prev = 0
        if blocking and c["duplicates"] > prev:
            raise ValueError("refusing to raise blocking baseline of %s: %d -> %d" % (
                c["id"], prev, c["duplicates"]))
        new["units"][c["id"]] = {
            "name": c["name"], "blocking": blocking,
            "baseline_duplicates": c["duplicates"],
            "known_false_positive": o.get("known_false_positive", [])}
    return new


def format_text(result, strict_bad=None):
    L = ["Shared-unit duplicate-computation check "
         "(report only unless --strict; default exits 0)"]
    for c in result["computations"]:
        files = {h["file"] for h in c["hits"]}
        roles = ", ".join("%s %d" % (r, n) for r, n in c["roles"].items() if n)
        L.append("")
        L.append("== %s %s: %d hits outside owner in %d files (%d inside owner; "
                 "owner: %s) blocking=%s baseline=%s duplicates=%d" % (
                     c["id"], c["name"], len(c["hits"]), len(files), c["inside_owner"],
                     c["owner"], c["blocking"], c["baseline"], c["duplicates"]))
        if roles:
            L.append("   roles: " + roles)
        for h in c["hits"]:
            if h["role"] in ("duplicate", "bill", "known_false_positive"):
                L.append("%s:%d  [%s] %s  (owner: %s)" % (
                    h["file"], h["line"], h["role"], c["name"], c["owner"]))
    L.append("")
    L.append("Total hits outside owning units: %d (by role: %s)" % (
        result["total_outside"],
        ", ".join("%s %d" % (r, n) for r, n in result["total_by_role"].items())))
    L.append("Total duplicates: %d" % result["total_duplicates"])
    if strict_bad is not None:
        L.append("STRICT: %s" % ("FAIL " + ", ".join(
            "%s %d > baseline %d" % b for b in strict_bad) if strict_bad else "pass"))
    L.append("")
    L.append("Known duplicates (doc 51 section 2):")
    L.extend("  - " + k for k in KNOWN_DUPLICATES)
    return "\n".join(L)


def main(argv=None):
    here = os.path.dirname(os.path.abspath(__file__))
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--json", action="store_true")
    ap.add_argument("--strict", action="store_true")
    ap.add_argument("--write-baseline", action="store_true")
    ap.add_argument("--baseline", default=None)
    ap.add_argument("--root", default=os.path.dirname(here))
    try:
        args = ap.parse_args(argv)
    except SystemExit:
        return 2 if "--strict" in (argv if argv is not None else sys.argv[1:]) else 0
    try:
        root = os.path.abspath(args.root)
        bpath = args.baseline or os.path.join(root, *BASELINE_REL.split("/"))
        old = load_baseline(root, bpath)
        result = scan(root, baseline=old)
        bad = strict_violations(result) if args.strict else None
        if args.write_baseline:
            try:
                new = build_baseline(result, old)
            except ValueError as ex:
                print("check_shared_units: %s" % ex)
                return 1
            with open(bpath, "w", encoding="utf-8", newline="\n") as f:
                json.dump(new, f, indent=1)
                f.write("\n")
        if args.json:
            print(json.dumps(result, indent=1))
        else:
            print(format_text(result, bad))
        if args.strict and bad:
            return 1
    except Exception as ex:
        print("check_shared_units: error: %s" % ex)
        return 2 if args.strict else 0
    return 0


if __name__ == "__main__":
    sys.exit(main())
