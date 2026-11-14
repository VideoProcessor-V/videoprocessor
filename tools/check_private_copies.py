# Copyright(C) 2026 VP-V contributors
# SPDX-License-Identifier: GPL-3.0-only
"""N1 private-copy checker (doc 124 s2.2; refs/126 s4; refs/122 s9).

Usage: python -I tools/check_private_copies.py [--strict] [--json] [--root PATH] [--update-baseline]

Every consumer of a declared skeleton product (src/VP-V/core/Products.h) must read it through the skeleton
accessor. This tool finds three things, comments stripped, owner scope by FILE (never by folder: the matcher
global search hides inside the motion owner folder, X1):

  a  private copy     a producer / recomputation signature outside the product owner files (sites D1-D6, X1-X7)
  c  owner no-publish a call of an owner compute (GrainSplit::Compute, BandFromBase::Blur, HalfLuma::*, CellTrust)
                      OUTSIDE the owning folder, in a file with no publish / Acquire / Seal evidence (refs/126 s1.3)
  d  register         RegisterOwnerFill / RegisterCpuLiveCompute called outside the owning folders
  moved               <site>~moved: a seeded copy signature found in a file other than the seeded ones
  b  declared-unread  a ConsumerRow in ProductCatalog.h that declares a product read, while no source file in the
                      row module folder references that product accessor (X8: the declaration is a lie)

Ratchet: tools/private_copies_baseline.json holds the allowed hit count per (rule, site, file) with a reason and a
refs id. A count may only shrink; --strict exits 1 on any rise, any new (rule, site, file), or a seeded site
(D1-D6, X1-X8) that detects nothing while the baseline still lists it (a blind checker). Exit 0 ok, 1 violation,
2 usage / IO error. Adding or raising an entry needs the coordinator approval (rule 21 style).
"""
import argparse
import json
import os
import re
import sys

SCAN_ROOTS = ["src/VP-V", "shaders"]
SKIP_DIRS = {"tests", "VideoProcessor-Test", "3rdparty", "x64", "fixtures", ".git", "Debug", "Release"}
EXTS = (".h", ".hpp", ".cpp", ".cc", ".cxx", ".inl", ".glsl", ".hlsl", ".comp", ".frag", ".fx")
BASELINE = "tools/private_copies_baseline.json"
_M = "src/VP-V/modules/"

# Rule a sites. id, product (Products.h enum name), regex over comment-stripped code (raw: raw text),
# only = restrict to these files (None: every scanned file), owner = files where the symbol is legitimate.
SITES = [
    {"id": "D1", "product": "kP10GrainSplit", "refs": "122 s9 D1",
     "what": "TD private grain guard / EMA of the grain",
     "re": r"\bm_prevGin\b|\bdouble\s+guard\b|\bema\[7\]",
     "wide": r"\bm_prevGin\b",
     "only": [_M + "repair/temporal_detail/TemporalDetailModel.h"]},
    {"id": "D2", "product": "kP2VectorPair", "refs": "122 s9 D2",
     "what": "per-consumer CellTrust (optical-flow trust recomputed outside the engine)",
     "re": r"\bOpticalFlowTrust::CellTrust\b|\bCellTrust\s*\(",
     "owner": [_M + "motion/estimation/VectorEngine.cpp", _M + "motion/estimation/OpticalFlowTrust.h",
               _M + "motion/estimation/VectorEngineInterface.h"]},
    {"id": "D3", "product": "kP2VectorPair", "refs": "122 s9 D3",
     "what": "sigma-delta own vector search (ditherVectorsOwn / l1Available)",
     "re": r"\bditherVectorsOwn\b|\bl1Available\b"},
    {"id": "D3s", "product": "kP2VectorPair", "refs": "122 s9 D3",
     "what": "sigma-delta shader own block search pass", "raw": True,
     "re": r"full search within",
     "only": ["shaders/Temporal sigma-delta.glsl"]},
    {"id": "D4", "product": "kP8CameraLabels", "refs": "122 s9 D4",
     "what": "camera labels read as a FieldCopy through the link instead of P3",
     "re": r"\bm_link\.FieldOf\s*\(",
     "wide": r"\bm_link\.FieldOf\s*\(",
     "only": [_M + "motion/estimation/global_model/link/CameraLabelFill.cpp"]},
    {"id": "D5", "product": "kP13SourceRing", "refs": "122 s9 D5",
     "what": "producer CPU luma decode (code >> 6) / GPU row upload shift",
     "re": r">>\s*6\b|<<\s*(view\.)?(msbShift|shift)\b",
     "only": [_M + "input/source_analysis/VpFrameAnalysis.cpp", _M + "input/source_analysis/lane/LaneGpuProducts.cpp"]},
    {"id": "D6", "product": "kP13SourceRing", "refs": "122 s9 D6 / doc 91 D C9",
     "what": "C9 producer own RGB / full-range luma decode (named exception until EPE is compared)",
     "re": r"\bSamplerCodesAreFullRange\b|\bfullRangeYuv\b|\bCodesFullRange\b",
     "wide": r"\bSamplerCodesAreFullRange\b",
     "only": [_M + "input/source_analysis/VpFrameAnalysis.cpp"]},
    {"id": "X1", "product": "kP2VectorPair", "refs": "126 s4 X1",
     "what": "matcher D1 global search inside the motion owner folder (file-level scope)",
     "re": r"global[_ ]?motion|\bFlowGlobal\b|\bengineGlobal\b", "ic": True,
     "only": [_M + "motion/estimation/matcher/DejudderWarp.cpp", _M + "motion/estimation/matcher/DejudderWarp.h"]},
    {"id": "X2", "product": "kP4SourceStats", "refs": "126 s4 X2",
     "what": "private cut / fade judge (kCutShare, kFadeStepShare) in DP and TR",
     "re": r"\bkCutShare\b|\bkFadeStepShare\b",
     "wide": r"\bkCutShare\b|\bkFadeStepShare\b",
     "only": [_M + "repair/texture_restore/DetailPulse.h", _M + "repair/texture_restore/DetailPulse.cpp",
              _M + "repair/texture_restore/TextureRestore.h", _M + "repair/texture_restore/TextureRestore.cpp"]},
    {"id": "X3", "product": "kP11RegionBlockStats", "refs": "126 s4 X3",
     "what": "TR STATS pass / DP tile energies recomputed (private block statistics)",
     "re": r"\bSTATS_HEADER\b|\bSTATS_BODY\b|\btileEnerg\w*",
     "only": [_M + "repair/texture_restore/TextureRestore.cpp", _M + "repair/texture_restore/DetailPulse.cpp"]},
    {"id": "X4", "product": "kP10GrainSplit", "refs": "126 s4 X4",
     "what": "debander own grain split (SPLIT_TENSOR_BODY)",
     "re": r"\bSPLIT_TENSOR_BODY\b",
     "wide": r"\bSPLIT_TENSOR_BODY\b",
     "only": [_M + "repair/deband/VpDebander.cpp"]},
    {"id": "X5", "product": "kP11RegionBlockStats", "refs": "126 s4 X5",
     "what": "adaptive downscale own Sobel / Laplacian maps",
     "re": r"\bsobel\w*|\blaplac\w*|\bgrain(Lo|Hi)\b|\bedge(Lo|Hi)\b", "ic": True,
     "only": [_M + "scaling/output/AdaptiveDownscale.h"]},
    {"id": "X6", "product": "kP11RegionBlockStats", "refs": "126 s4 X6",
     "what": "source-resolution detector own flat-block test",
     "re": r"\bhi\s*-\s*lo\s*<=\s*flat\b|\bflat\s*=",
     "only": [_M + "input/source_analysis/SourceResolutionDetector.cpp"]},
    {"id": "X7", "product": "kP10GrainSplit", "refs": "126 s4 X7",
     "what": "TD own grain split when the shared one does not fit (CheckFit fallback)",
     "re": r"\bGrainSplit::Gpu::CheckFit\b|\bown grain split\b", "raw": True,
     "wide": r"\bown grain split\b",
     "only": [_M + "repair/temporal_detail/TemporalDetailGpu.cpp"]},
]

# Rule c (refs/126 s1.3): a call of an owner compute symbol OUTSIDE the owning folder, in a file that shows no
# publish / Acquire path, computes a shared result privately. Owner scope is the owning FOLDER here; rule a is file-level.
OWNER_COMPUTE = [
    {"id": "C-grain", "product": "kP10GrainSplit", "sym": r"\bGrainSplit::Compute\s*\(",
     "owner": _M + "repair/grain_split/"},
    {"id": "C-band", "product": "kP10GrainSplit", "sym": r"\bBandFromBase::Blur\s*\(",
     "owner": _M + "repair/grain_split/"},
    {"id": "C-halfluma", "product": "kP1HalfLuma", "sym": r"\bHalfLuma::(Box4|FinishP1|FinishEngine8|MeanF|QuarterEngine8)\b",
     "owner": _M + "input/source_analysis/lane/"},
    {"id": "C-trust", "product": "kP2VectorPair", "sym": r"\bOpticalFlowTrust::CellTrust\s*\(",
     "owner": _M + "motion/estimation/"},
]
PUBLISH = re.compile(r"\b(Publish\w*|Acquire\s*\(|RegisterOwnerFill|SealProduct|Seal\s*\(|SetProduct\w*|StoreProduct\w*)")
# Rule d: the live-fill registration is made only from the owning folders (skeleton wiring and the producers).
REGISTER_SYM = re.compile(r"\b(RegisterOwnerFill|RegisterCpuLiveCompute)\b")
REGISTER_OWNERS = ("src/VP-V/core/", _M + "control/skeleton/", _M + "input/source_analysis/", _M + "motion/estimation/",
                   _M + "repair/grain_split/")

_PRODUCT_ROW = re.compile(r'\{\s*(k[A-Za-z0-9_]+)\s*,\s*"([^"]+)"\s*,[^,]*,\s*"([^"]*)"\s*\}')


def registration_owner_folders(root):
    """REGISTER_OWNERS plus the folders ProductRows() of ProductCatalog.h declares for the products (rule d). The catalog is
    the one place that says where each product lives, so a product whose owner is outside the fixed list (P13 SourceRing:
    input/capture) is recognised from its row. Rows marked 'reserved' have no producer and add nothing."""
    out = list(REGISTER_OWNERS)
    p = os.path.join(root, "src/VP-V/core/ProductCatalog.h")
    if os.path.isfile(p):
        for m in _PRODUCT_ROW.finditer(strip_comments(read(p))):
            if m.group(3).strip().lower().startswith("reserved"):
                continue
            folder = m.group(2).split()[0].strip("/")
            if folder and not folder.startswith(".."):
                f = _M + folder + "/"
                if f not in out:
                    out.append(f)
    return tuple(out)


# Rule b: product -> accessor regex (a consumer reads the product through one of these).
ACCESSOR = {
    "kP10GrainSplit": r"GrainSplit::Gpu|\bgrain\.(Gpu|Cpu)\b|GrainPlane|GrainSplit::View|ViewGlsl|\bgrainSplit\b|\bparams\.grain\b",
    "kP11RegionBlockStats": r"RegionBlockStats|\bblockStats\b|\bregionBlock\w*",
    "kP12LightLevel": r"LightLevel|\blightLevel\b",
    "kP13SourceRing": r"SourceRing|\bsourceRing\b|SourceFrameView",
    "kP3AnalysisField": r"AnalysisField|\banalysisField\b",
    "kP4SourceStats": r"SourceStats|\bsourceStats\b",
    "kP2VectorPair": r"VectorPair|\bP2\b|MotionIntent",
    "kP1HalfLuma": r"HalfLuma",
}
_ENUM_RE = re.compile(r"\b(k[A-Z]\w+)\b")


def strip_comments(text):
    out, i, n, blk = [], 0, len(text), False
    while i < n:
        c = text[i]
        if blk:
            if text.startswith("*/", i):
                blk, i = False, i + 2
            else:
                out.append("\n" if c == "\n" else " ")
                i += 1
        elif text.startswith("/*", i):
            blk, i = True, i + 2
        elif text.startswith("//", i):
            while i < n and text[i] != "\n":
                i += 1
        else:
            out.append(c)
            i += 1
    return "".join(out)


def iter_files(root):
    for sr in SCAN_ROOTS:
        base = os.path.join(root, sr)
        for d, dirs, files in os.walk(base):
            dirs[:] = [x for x in dirs if x not in SKIP_DIRS]
            for f in files:
                if f.lower().endswith(EXTS):
                    p = os.path.join(d, f)
                    yield os.path.relpath(p, root).replace("\\", "/"), p


def read(p):
    with open(p, "r", encoding="utf-8", errors="replace") as fh:
        return fh.read()


def products_enum(root):
    p = os.path.join(root, "src/VP-V/core/Products.h")
    names = set()
    if os.path.isfile(p):
        t = strip_comments(read(p))
        for m in re.finditer(r"\benum\b[^{]*\{([^}]*)\}", t):
            for n in re.finditer(r"\b(k[A-Z]\w*)\b", m.group(1)):
                names.add(n.group(1))
    return names


def consumer_rows(root):
    """[(id, name, folder, [declared products])] parsed from the MakeRow calls of ProductCatalog.h."""
    p = os.path.join(root, "src/VP-V/core/ProductCatalog.h")
    rows = []
    if not os.path.isfile(p):
        return rows
    t = strip_comments(read(p))
    for m in re.finditer(r'MakeRow\(\s*(\d+)\s*,\s*"([^"]*)"\s*,\s*"([^"]*)"\s*,[^{]*\{([^}]*)\}', t):
        rows.append((int(m.group(1)), m.group(2), m.group(3), _ENUM_RE.findall(m.group(4))))
    return rows


def scan(root):
    """-> list of findings {rule, site, file, line, text, product}."""
    hits = []
    files = dict(iter_files(root))
    cache = {}

    def code(rel, raw=False):
        k = (rel, raw)
        if k not in cache:
            t = read(files[rel])
            cache[k] = t if raw else strip_comments(t)
        return cache[k]

    for s in SITES:
        rx = re.compile(s["re"], re.IGNORECASE if s.get("ic") else 0)
        for rel in (s.get("only") or list(files)):
            if rel not in files or rel in s.get("owner", []):
                continue
            for ln, line in enumerate(code(rel, s.get("raw", False)).split("\n"), 1):
                if rx.search(line):
                    hits.append({"rule": "a", "site": s["id"], "file": rel, "line": ln,
                                 "text": line.strip()[:110], "product": s["product"]})
    for st in SITES:
        if not st.get("wide") or not st.get("only"):
            continue
        rx = re.compile(st["wide"])
        for rel in files:
            if rel in st["only"] or rel in st.get("owner", []):
                continue
            for ln, line in enumerate(code(rel).split(chr(10)), 1):
                if rx.search(line):
                    hits.append({"rule": "a", "site": st["id"] + "~moved", "file": rel, "line": ln,
                                 "text": "copy found outside the seeded file: " + line.strip()[:80], "product": st["product"]})
    for o in OWNER_COMPUTE:
        rx = re.compile(o["sym"])
        for rel in files:
            if rel.startswith(o["owner"]):
                continue
            lines = code(rel).split(chr(10))
            for ln, line in enumerate(lines, 1):
                if rx.search(line):
                    if PUBLISH.search(chr(10).join(lines[max(0, ln - 7):ln + 6])):
                        continue   # the result goes to a publish / Acquire path within the statement window
                    hits.append({"rule": "c", "site": o["id"], "file": rel, "line": ln,
                                 "text": "owner compute outside its folder, no publish path: " + line.strip()[:80],
                                 "product": o["product"]})
    reg_owners = registration_owner_folders(root)
    for rel in files:
        if REGISTER_SYM.search(code(rel)) and not rel.startswith(reg_owners):
            ln = next(i for i, l in enumerate(code(rel).split(chr(10)), 1) if REGISTER_SYM.search(l))
            hits.append({"rule": "d", "site": "REG", "file": rel, "line": ln,
                         "text": "live-fill registration outside the owning folders", "product": "-"})
    for rid, name, folder, prods in consumer_rows(root):
        fdir = "src/" + folder
        members = [r for r in files if r.startswith(fdir + "/")] or [r for r in files if r.startswith(fdir)]
        if not members:
            hits.append({"rule": "b", "site": "X8", "file": fdir, "line": 0, "product": "-",
                         "text": "row %d '%s': module folder absent, its declared reads cannot be matched to any code" % (rid, name)})
            continue
        blob = "\n".join(code(r) for r in members)
        for pr in prods:
            acc = ACCESSOR.get(pr)
            if acc and not re.search(acc, blob):
                hits.append({"rule": "b", "site": "X8", "file": fdir, "line": 0, "product": pr,
                             "text": "row %d '%s' declares %s but no file reads it" % (rid, name, pr)})
    return hits


def key(h):
    return "%s|%s|%s" % (h["rule"], h["site"], h["file"] + ("#" + h["product"] if h["rule"] == "b" else ""))


def counts(hits):
    c = {}
    for h in hits:
        c[key(h)] = c.get(key(h), 0) + 1
    return c


def load_baseline(root):
    p = os.path.join(root, BASELINE)
    return json.loads(read(p)) if os.path.isfile(p) else {"entries": []}


def compare(hits, base):
    cur = counts(hits)
    allowed = {e["key"]: e["max_hits"] for e in base.get("entries", [])}
    viol, stale = [], []
    for k, n in sorted(cur.items()):
        if k not in allowed:
            viol.append("NEW %s (%d hits)" % (k, n))
        elif n > allowed[k]:
            viol.append("RISE %s %d > baseline %d" % (k, n, allowed[k]))
    for k, m in allowed.items():
        if k not in cur:
            stale.append("%s (baseline %d, now 0: delete the entry)" % (k, m))
    seeded = {e["site"] for e in base.get("entries", []) if e.get("seed")}
    seen = {h["site"] for h in hits}
    for sid in sorted(seeded - seen):
        viol.append("BLIND seeded site %s detects nothing (checker regressed, or the copy is gone: delete its entries)" % sid)
    return viol, stale


def main(argv=None):
    ap = argparse.ArgumentParser()
    ap.add_argument("--strict", action="store_true")
    ap.add_argument("--json", action="store_true")
    ap.add_argument("--root", default=os.path.join(os.path.dirname(os.path.abspath(__file__)), ".."))
    ap.add_argument("--update-baseline", action="store_true", help="write the CURRENT counts (refuses a rise)")
    a = ap.parse_args(argv)
    root = os.path.abspath(a.root)
    if not os.path.isdir(os.path.join(root, "src")):
        print("no src/ under " + root, file=sys.stderr)
        return 2
    hits = scan(root)
    base = load_baseline(root)
    if a.update_baseline:
        old = {e["key"]: e for e in base.get("entries", [])}
        info = {s["id"]: s for s in SITES}
        ents = []
        for k, n in sorted(counts(hits).items()):
            rule, site = k.split("|")[0], k.split("|")[1]
            e = old.get(k) or {"key": k, "rule": rule, "site": site,
                               "refs": info.get(site, {}).get("refs", "126 s4 X8"),
                               "reason": info.get(site, {}).get("what", "declared product read never made, or owner compute without publish"),
                               "seed": site in info}
            if old and k not in old:
                print("refusing to add a new key " + k + " (edit the baseline by hand with the coordinator approval)", file=sys.stderr)
                return 1
            if k in old and n > old[k]["max_hits"]:
                print("refusing to raise " + k, file=sys.stderr)
                return 1
            e["max_hits"] = n
            ents.append(e)
        base["entries"] = ents
        with open(os.path.join(root, BASELINE), "w", encoding="utf-8", newline="\n") as fh:
            json.dump(base, fh, indent=1)
            fh.write("\n")
    viol, stale = compare(hits, base)
    if a.json:
        print(json.dumps({"hits": hits, "violations": viol, "stale": stale}, indent=1))
    else:
        per = {}
        for h in hits:
            per.setdefault(h["site"], []).append(h)
        for site, hs in sorted(per.items()):
            print("%-7s %3d  %s" % (site, len(hs), ", ".join(sorted({os.path.basename(h["file"]) for h in hs}))[:150]))
        for v in viol:
            print("VIOLATION " + v)
        for s in stale:
            print("stale " + s)
        print("total %d findings, %d violations" % (len(hits), len(viol)))
    return 1 if (viol and a.strict) else 0


if __name__ == "__main__":
    sys.exit(main())
