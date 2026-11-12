# Copyright(C) 2026 VP-V contributors
# SPDX-License-Identifier: GPL-3.0-only
"""Motion-bypass checker (INTEGRATOR-RULES rule 21: one motion source).

Usage: python -I tools/check_motion_bypass.py [--json] [--root PATH]

Greps the code under src/ and shaders/ OUTSIDE src/VP-V/modules/motion/ for a NEW private
motion estimation and exits 1 on any hit that is not on the allow-list below:

  nvof    NVOF session creation (NvOFAPICreateInstance, NvOFInit, new NvOpticalFlow::...,
          NvOpticalFlow::Engine / HintedEngine use, make_unique/make_shared of an engine; a static member read
          such as NvOpticalFlow::Engine::Grid is a constant, not a session, and is not a hit)
  sad     SAD / block-match search kernels (BestShift, BlockMatch*, SearchSad*, a sad() function
          or `sad* +=`, and `+= abs(<cur ... prev/ref/last/old>)` accumulation over a search window)
  phase   phase correlation (PhaseCorrelat*, FFT cross-power / cross-spectrum)

Comments are stripped before matching (// and /* */), so prose never hits. Tests, fixtures,
3rdparty and build output are skipped. Every allow-list entry names its file, the pattern
ids it covers, a hit ceiling, the row of doc 85 (D:\\codex\\vp-handover\\85-motion-bypass-scan.md)
and the reason. Entries are per FILE (line numbers drift); a ceiling stops a second private
search growing inside an allowed file. An entry that no longer hits is reported as stale (it can
be deleted) but does not fail. Adding an entry needs the coordinator's approval (rule 21).
"""
import argparse
import json
import os
import re
import sys

SCAN_ROOTS = ["src", "shaders"]
SKIP_DIRS = {"tests", "VideoProcessor-Test", "3rdparty", "x64", "fixtures", ".git", "Debug", "Release"}
EXTS = (".h", ".hpp", ".cpp", ".cc", ".cxx", ".inl", ".glsl", ".hlsl", ".comp", ".frag", ".fx")
EXEMPT_PREFIXES = ("src/VP-V/modules/motion/",)

_F = re.IGNORECASE
PATTERNS = [
    ("nvof", re.compile(r"NvOFAPICreateInstance|\bNvOFInit\b|new\s+NvOpticalFlow::|"
                        r"NvOpticalFlow::(Hinted)?Engine\b(?!\s*::)|\bHintedEngine\b|"
                        r"make_(unique|shared)\s*<\s*(NvOpticalFlow::)?(Hinted)?Engine\b")),
    ("sad", re.compile(r"\b(BestShift|BlockMatch\w*|SearchSad\w*|BlockSad\w*)\b|"
                       r"\bsad\w*\s*(\(|\+=)|\b(float|int|uint|double)\s+sad\w*\b|\bSumAbsDiff\w*|"
                       r"\+=\s*(std::)?f?abs\s*\((?=.*cur)(?=.*(prev|ref|last|old))", _F)),
    ("phase", re.compile(r"PhaseCorrelat\w*|cross[ _-]?power\w*|cross[ _-]?spectrum\w*", _F)),
]

# file -> entry. patterns: pattern ids the file may hit; max_hits: ceiling over those patterns.
ALLOW = [
    {"file": "src/VideoProcessor-Lib/ContentClassifier.h", "patterns": ["sad"], "max_hits": 4, "row": 1,
     "reason": "BestShift / Measure: CPU global pan for the content classifier (luma projections, MAD +-12.5%); the engine "
               "global overrides it later (VectorEngine ServeStatistics -> MotionIntent). Planned: skip when a P2 pair is ahead, "
               "no-pair path stays as a visible fallback (next reset)."},
]


def _strip_comments(text, glsl_like):
    """Remove // and /* */ comments, keeping line structure. Strings are not special-cased (none matter here)."""
    out = []
    i, n = 0, len(text)
    block = False
    while i < n:
        c = text[i]
        two = text[i:i + 2]
        if block:
            if two == "*/":
                block = False
                i += 2
            else:
                if c == "\n":
                    out.append("\n")
                i += 1
            continue
        if two == "/*":
            block = True
            i += 2
            continue
        if two == "//":
            while i < n and text[i] != "\n":
                i += 1
            continue
        out.append(c)
        i += 1
    return "".join(out)


def iter_files(root):
    for scan in SCAN_ROOTS:
        base = os.path.join(root, scan)
        for dp, dn, fn in os.walk(base):
            dn[:] = [d for d in dn if d not in SKIP_DIRS]
            for f in fn:
                if not f.lower().endswith(EXTS):
                    continue
                full = os.path.join(dp, f)
                rel = os.path.relpath(full, root).replace(os.sep, "/")
                if rel.startswith(EXEMPT_PREFIXES):
                    continue
                yield rel, full


def scan(root):
    hits = []
    for rel, full in iter_files(root):
        try:
            with open(full, encoding="utf-8", errors="replace") as fh:
                text = fh.read()
        except OSError:
            continue
        for lineno, line in enumerate(_strip_comments(text, True).splitlines(), 1):
            for pid, rx in PATTERNS:
                if rx.search(line):
                    hits.append({"file": rel, "line": lineno, "pattern": pid, "text": line.strip()[:140]})
    return hits


def evaluate(hits):
    by_file = {e["file"]: e for e in ALLOW}
    used = {e["file"]: 0 for e in ALLOW}
    new = []
    for h in hits:
        e = by_file.get(h["file"])
        if e and h["pattern"] in e["patterns"]:
            used[e["file"]] += 1
            if used[e["file"]] <= e["max_hits"]:
                continue
            h = dict(h, over_ceiling=True)
        new.append(h)
    stale = [f for f, n in used.items() if n == 0]
    return new, used, stale


def main(argv=None):
    ap = argparse.ArgumentParser(description="Fail on a new private motion estimation outside src/VP-V/modules/motion/.")
    ap.add_argument("--root", default=os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
    ap.add_argument("--json", action="store_true")
    args = ap.parse_args(argv)
    hits = scan(args.root)
    new, used, stale = evaluate(hits)
    if args.json:
        print(json.dumps({"hits": len(hits), "allow_list_entries": len(ALLOW), "allowed": sum(used.values()) - sum(1 for h in new if h.get("over_ceiling")),
                          "new": new, "stale_entries": stale}, indent=2))
    else:
        print("motion bypass: %d hit(s), %d allow-list entries, %d new" % (len(hits), len(ALLOW), len(new)))
        for f in stale:
            print("  stale allow-list entry (no hit any more, may be deleted): " + f)
        for h in new:
            print("  NEW %s %s:%d %s%s" % (h["pattern"], h["file"], h["line"], h["text"],
                                           "  (over the allowed ceiling)" if h.get("over_ceiling") else ""))
        if new:
            print("FAIL: a private motion estimation outside src/VP-V/modules/motion/ (rule 21): consume the shared views "
                  "(P2/P3, FrameAnalysisStore motion record), or ask the coordinator for an allow-list entry with a reason.")
    return 1 if new else 0


if __name__ == "__main__":
    sys.exit(main())
