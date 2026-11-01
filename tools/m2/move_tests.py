#!/usr/bin/env python
# Copyright(C) 2026 VP-V contributors
# SPDX-License-Identifier: GPL-3.0-only
"""P15: get VP-V tests out of Bill's test projects.

  --phase move   P15a: git mv VP-only test source files to src/VP-V/tests/<area>/ and fix every reference.
  --phase split  P15b: split VP-V test methods out of Bill's CppUnitTest files into <Stem>VpvTests.cpp.
  --compare-lists BEFORE AFTER [RENAMES]   compare two sorted test inventories.

Modes: --dry-run (default), --apply, --verify. Idempotent; recomputes from the tree + --bill ref.
See tools/m2/README.md.
"""
import argparse
import fnmatch
import json
import os
import posixpath
import re
import subprocess
import sys
from collections import OrderedDict
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import m2common as C  # noqa: E402

CONFIG = "tools/m2/test_moves.json"
SRC_EXT = (".cpp", ".h", ".hpp", ".inc", ".c")
INCLUDE_RE = re.compile(r'^([ \t]*#[ \t]*include[ \t]*)(["<])([^">\r\n]+)([">])(.*)$', re.S)
INCLUDE_PROJ_EXT = (".vcxproj", ".props", ".filters")


def load_config():
    raw = (C.ROOT / CONFIG).read_text(encoding="utf-8")
    return json.loads(raw, object_pairs_hook=OrderedDict)


def save_config(cfg):
    text = json.dumps(cfg, indent=1, ensure_ascii=False) + "\n"
    C.write_if_changed(CONFIG, text.encode("utf-8"))


def glob_re(pattern):
    out = ""
    for ch in pattern:
        if ch == "*":
            out += "[^/]*"
        elif ch == "?":
            out += "[^/]"
        else:
            out += re.escape(ch)
    return re.compile("^" + out + "$", re.I)


def read_text(path):
    raw = C.read_working(path)
    return raw.decode("utf-8", "surrogateescape")


def write_text(path, text):
    return C.write_if_changed(path, text.encode("utf-8", "surrogateescape"))


# ======================================================================== P15a

def find_candidates(cfg, bill):
    keep = set(k.lower() for k in cfg["keep"])
    exts = tuple(cfg["extensions"])
    rules = [(glob_re(r["glob"]), r["dest"]) for r in cfg["rules"]]
    cands, reported = {}, []
    tracked = C.ls_files(*cfg["scan_dirs"])
    at_bill = set(C.bill_ls(bill, *[d + "/" for d in cfg["scan_dirs"]]))
    hist_paths = set(l for l in C.git("log", "--format=", "--name-only", bill, "--",
                                      *cfg["scan_dirs"], text=True).splitlines() if l)
    for p in tracked:
        if p in at_bill:
            continue
        if p in hist_paths:
            reported.append((p, "has history in Bill's ancestry"))
            continue
        if p.lower() in keep:
            reported.append((p, "keep-list"))
            continue
        if not p.lower().endswith(exts):
            reported.append((p, "not a source file (data/fixture/project)"))
            continue
        dest = None
        for rx, d in rules:
            if rx.match(p):
                dest = d
                break
        if dest is None:
            reported.append((p, "no destination rule matches"))
            continue
        cands[p] = posixpath.join(dest.rstrip("/"), posixpath.basename(p))
    return cands, reported


def has_header(path):
    head = read_text(path)[:600]
    return "VP-V contributors" in head


class Universe:
    """Case-insensitive file set with lookup."""

    def __init__(self, paths):
        self.map = {p.lower(): p for p in paths}

    def find(self, p):
        return self.map.get(posixpath.normpath(p).lower())


def resolve_include(uni, includer_dir, dirs, name, quoted):
    if quoted:
        r = uni.find(posixpath.join(includer_dir, name))
        if r:
            return r
    for d in dirs:
        r = uni.find(posixpath.join(d, name))
        if r:
            return r
    return None


def origin_dirs(cfg, loc_before, hist_dest_origin):
    origins = cfg["origins"]
    for name, o in origins.items():
        if loc_before.startswith(o["dir"] + "/"):
            return o["include_dirs"]
    d = posixpath.dirname(loc_before)
    if d in hist_dest_origin:
        return origins[hist_dest_origin[d]]["include_dirs"]
    # not in a test project: it may still be compiled into one (VP-V tests): union fallback chain
    out = []
    for o in origins.values():
        for x in o["include_dirs"]:
            if x not in out and x != "src":
                out.append(x)
    out.append("src")
    return out


def plan_move(cfg, bill):
    cands, reported = find_candidates(cfg, bill)
    resolved_hist = cfg.get("resolved", {})
    tracked = C.ls_files()
    files_now = set(tracked)
    virtual = {o: n for o, n in resolved_hist.items() if n in files_now and o not in files_now}
    # destination checks
    errors = []
    for old, new in cands.items():
        if new in files_now:
            if C.read_working(new) != C.read_working(old):
                errors.append("destination exists with different content: %s -> %s" % (old, new))
    dups = {}
    for old, new in cands.items():
        dups.setdefault(new.lower(), []).append(old)
    for k, v in dups.items():
        if len(v) > 1:
            errors.append("two files map to one destination %s: %s" % (k, v))
    before_set = files_now | set(virtual)
    after_set = (files_now - set(cands)) | set(cands.values())
    ub, ua = Universe(before_set), Universe(after_set)
    mapping = dict(virtual)
    mapping.update(cands)
    hist_origin = cfg.get("origin_by_dest", {})

    edits = {}       # final_path -> new text (only changed)
    stats = {"includes_rewritten": 0, "files_with_rewrites": 0, "quoted_out_of_src": 0}
    rewrites_log = []
    suspicious = []
    moved_names = {posixpath.basename(o).lower(): o for o in mapping}
    pch_checks = []
    cpp_files = [p for p in tracked if p.lower().endswith(SRC_EXT) and (p.startswith("src/") or p.startswith("tools/"))]
    for p in cpp_files:
        loc_before = p
        loc_after = cands.get(p, p)
        text = read_text(p)
        dirs_b = origin_dirs(cfg, loc_before, hist_origin)
        # after: origin stays that of the BEFORE location (project membership does not move)
        dirs_a = dirs_b
        lines = text.split("\n")
        changed = False
        for i, ln in enumerate(lines):
            m = INCLUDE_RE.match(ln)
            if not m:
                continue
            pre, o, name, c, post = m.groups()
            quoted = (o == '"')
            if name.lower() == "pch.h":
                rb = resolve_include(ub, posixpath.dirname(loc_before), dirs_b, name, quoted)
                ra = resolve_include(ua, posixpath.dirname(loc_after), dirs_a, name, quoted)
                if rb and (ra is None):
                    pch_checks.append((p, "pch.h no longer resolves after move"))
                continue
            rb = resolve_include(ub, posixpath.dirname(loc_before), dirs_b, name, quoted)
            ra = resolve_include(ua, posixpath.dirname(loc_after), dirs_a, name, quoted)
            if rb is None:
                if posixpath.basename(name).lower() in moved_names:
                    suspicious.append((p, i + 1, name))
                continue
            target_after = mapping.get(rb, rb)
            if ra is not None and ra == target_after:
                continue
            # rewrite
            if target_after.startswith("src/"):
                newname = target_after[len("src/"):]
                new_line = "%s<%s>%s" % (pre, newname, post)
            else:
                rel = posixpath.relpath(target_after, posixpath.dirname(loc_after))
                new_line = "%s\"%s\"%s" % (pre, rel, post)
                stats["quoted_out_of_src"] += 1
            if new_line != ln:
                rewrites_log.append((loc_after, i + 1, ln.strip(), new_line.strip()))
                lines[i] = new_line
                changed = True
                stats["includes_rewritten"] += 1
        if changed:
            stats["files_with_rewrites"] += 1
            edits[p] = "\n".join(lines)
    # project item rewrites
    proj_edits, proj_log, proj_errors, wildcard_hits = rewrite_project_items(cfg, tracked, mapping, cands, bill)
    errors.extend(proj_errors)
    # test-categories.json
    cat_text = read_text("tools/test-categories.json")
    new_cat = cat_text
    cat_hits = 0
    for old, new in sorted(mapping.items()):
        for fa, fb in ((old, new),):
            pat = '"file": "%s"' % fa
            if pat in new_cat:
                cat_hits += new_cat.count(pat)
                new_cat = new_cat.replace(pat, '"file": "%s"' % fb)
    if new_cat != cat_text:
        edits["tools/test-categories.json"] = new_cat
    return {
        "cands": cands, "reported": reported, "errors": errors, "edits": edits, "stats": stats,
        "rewrites_log": rewrites_log, "suspicious": suspicious, "pch": pch_checks,
        "proj_edits": proj_edits, "proj_log": proj_log, "wildcards": wildcard_hits, "cat_hits": cat_hits,
        "mapping": mapping, "virtual": virtual,
    }


_INC_ATTR = re.compile(r'(\bInclude\s*=\s*")([^"]*)(")')


def project_base_dirs():
    out = {}
    for p in C.BILL_PROJECTS:
        out[C.props_path(p)] = posixpath.dirname(p)
    return out


def rewrite_project_items(cfg, tracked, mapping, cands, bill):
    pbases = project_base_dirs()
    ub = Universe(list(mapping))
    edits, log, errors, wild = {}, [], [], []
    for p in tracked:
        if not p.lower().endswith(INCLUDE_PROJ_EXT):
            continue
        if p in pbases:
            base = pbases[p]
        else:
            base = posixpath.dirname(p)
        raw = C.read_working(p)
        if raw is None:
            continue
        text, bom = C.decode(raw)
        is_bill_proj = p in C.BILL_PROJECTS

        def repl(m):
            inc = m.group(2)
            if "$(" in inc:
                return m.group(0)
            if "*" in inc or "?" in inc:
                full = posixpath.normpath(posixpath.join(base, inc.replace("\\", "/")))
                rx = glob_re(full)
                for o in mapping:
                    if rx.match(o):
                        wild.append((p, inc, o))
                return m.group(0)
            full = posixpath.normpath(posixpath.join(base, inc.replace("\\", "/")))
            hit = ub.find(full)
            if hit is None:
                return m.group(0)
            if is_bill_proj:
                errors.append("%s still contains item %s (old test path): run move_project_items.py --apply first"
                              % (p, inc))
                return m.group(0)
            new = mapping[hit]
            rel = posixpath.relpath(new, base).replace("/", "\\")
            log.append((p, inc, rel))
            return m.group(1) + rel + m.group(3)

        new_text = _INC_ATTR.sub(repl, text)
        if new_text != text:
            edits[p] = C.encode(new_text, bom)
    return edits, log, errors, wild


def grep_old_paths(mapping, exclude_paths):
    pats = []
    for o in sorted(mapping):
        pats.append(o)
        pats.append(o.replace("/", "\\"))
    if not pats:
        return []
    args = ["grep", "-n", "-F", "-I"]
    for p in pats:
        args += ["-e", p]
    args += ["--", "tools", "tests", "docs", "scripts", ".github", "packaging", "src", "README.md", "AGENTS.md",
             "DEVELOPERS.md"]
    out = subprocess.run(["git", "-C", str(C.ROOT)] + args, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    lines = out.stdout.decode("utf-8", "replace").splitlines()
    res = []
    for ln in lines:
        path = ln.split(":", 1)[0]
        if path in exclude_paths or path.startswith("tools/m2/"):
            continue
        if path.lower().endswith(INCLUDE_PROJ_EXT) or path.lower().endswith((".cpp", ".h", ".hpp", ".inc", ".c")):
            continue
        res.append(ln)
    return sorted(res)


def scan_file_macro(cands):
    out = []
    for old in sorted(cands):
        if "__FILE__" in read_text(old):
            out.append(old)
    return out


def print_move_plan(plan, bill):
    cands = plan["cands"]
    print("== P15a plan: %d file(s) to move (bill=%s) ==" % (len(cands), bill))
    for old in sorted(cands):
        print("  %s -> %s   [VP-V header: %s]" % (old, cands[old], "yes" if has_header(old) else "no"))
    print("\n== reported, not moved ==")
    for p, why in sorted(plan["reported"]):
        print("  %s : %s" % (p, why))
    s = plan["stats"]
    print("\n== #include rewrites: %d line(s) in %d file(s) (quoted-relative outside src/: %d) ==" % (
        s["includes_rewritten"], s["files_with_rewrites"], s["quoted_out_of_src"]))
    for f, ln, a, b in sorted(plan["rewrites_log"]):
        print("  %s:%d  %s  =>  %s" % (f, ln, a, b))
    print("\n== project/props/filters Include rewrites: %d ==" % len(plan["proj_log"]))
    for p, a, b in sorted(plan["proj_log"]):
        print("  %s: %s => %s" % (p, a, b))
    print("\ntools/test-categories.json file fields rewritten: %d" % plan["cat_hits"])
    for w in sorted(plan["wildcards"]):
        print("REPORT wildcard Include %s in %s covers moved file %s" % (w[1], w[0], w[2]))
    for p, ln, n in sorted(plan["suspicious"]):
        print("REPORT unresolved include with a moved file's basename: %s:%d  %s" % (p, ln, n))
    for p, msg in sorted(plan["pch"]):
        print("REPORT %s: %s" % (p, msg))
    fm = scan_file_macro(cands)
    for f in fm:
        print("REPORT __FILE__ used in moved file: %s" % f)
    exclude = set(plan["edits"]) | set(plan["proj_edits"])
    for ln in grep_old_paths(plan["mapping"], exclude):
        print("REPORT textual reference (not edited): %s" % ln)
    for e in plan["errors"]:
        print("ERROR %s" % e)


def apply_move(plan, cfg, bill):
    cands = plan["cands"]
    for old in sorted(cands):
        new = cands[old]
        (C.ROOT / posixpath.dirname(new)).mkdir(parents=True, exist_ok=True)
        if (C.ROOT / new).exists():
            C.git("rm", "-q", "-f", "--", old)
        else:
            C.git("mv", "--", old, new)
    for p, text in plan["edits"].items():
        final = cands.get(p, p)
        write_text(final, text)
    for p, data in plan["proj_edits"].items():
        C.write_if_changed(p, data)
    res = OrderedDict(cfg.get("resolved", {}))
    res.update(cands)
    cfg["resolved"] = OrderedDict(sorted(res.items()))
    save_config(cfg)


def move_pending(plan):
    return bool(plan["cands"] or plan["edits"] or plan["proj_edits"])


# ======================================================================== compare lists

def compare_lists(before, after, renames):
    def load(p):
        return set(l.strip() for l in Path(p).read_text(encoding="utf-8", errors="replace").splitlines() if l.strip())
    b, a = load(before), load(after)
    ren = {}
    if renames:
        for r in json.loads(Path(renames).read_text(encoding="utf-8")):
            ren[r["old"]] = r["new"]

    expected = set()
    for x in b:
        if x in ren:
            expected.add(ren[x])
        else:
            # allow "Class::Method" style entries and "Class.Method" / fully qualified
            y = x
            for old, new in ren.items():
                oc, om = old.split("::")
                nc, nm = new.split("::")
                for sep in (".", "::"):
                    if x.endswith(oc + sep + om):
                        y = x[: -len(oc + sep + om)] + nc + sep + nm
            expected.add(y)
    missing = sorted(expected - a)
    extra = sorted(a - expected)
    print("before: %d, after: %d, renames applied: %d" % (len(b), len(a), len(ren)))
    print("missing after: %d, unexpected after: %d" % (len(missing), len(extra)))
    for m in missing[:200]:
        print("  missing: %s" % m)
    for m in extra[:200]:
        print("  unexpected: %s" % m)
    return 0 if not missing and not extra else 1


# ======================================================================== main

def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--phase", choices=["move", "split"])
    g = ap.add_mutually_exclusive_group()
    g.add_argument("--dry-run", action="store_true")
    g.add_argument("--apply", action="store_true")
    g.add_argument("--verify", action="store_true")
    ap.add_argument("--bill", default=C.DEFAULT_BILL)
    ap.add_argument("--compare-lists", nargs="+", metavar=("BEFORE", "AFTER"))
    a = ap.parse_args(argv)
    if a.compare_lists:
        cl = a.compare_lists
        return compare_lists(cl[0], cl[1], cl[2] if len(cl) > 2 else None)
    if not a.phase:
        ap.error("--phase is required")
    cfg = load_config()
    if a.phase == "move":
        plan = plan_move(cfg, a.bill)
        pending = move_pending(plan)
        if a.verify:
            print("verify: %d move(s), %d file edit(s), %d project edit(s) pending" % (
                len(plan["cands"]), len(plan["edits"]), len(plan["proj_edits"])))
            return 1 if pending else 0
        print_move_plan(plan, a.bill)
        if plan["errors"]:
            print("refusing: %d error(s)" % len(plan["errors"]))
            return 2
        if a.apply:
            apply_move(plan, cfg, a.bill)
            print("applied.")
        else:
            print("(dry run: nothing written)")
        return 0
    import move_tests_split as S
    return S.run(a, cfg)


if __name__ == "__main__":
    sys.exit(main())
