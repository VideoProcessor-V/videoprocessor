# Copyright(C) 2026 VP-V contributors
# SPDX-License-Identifier: GPL-3.0-only
"""P15b of tools/m2/move_tests.py: split VP-V tests out of Bill's CppUnitTest files."""
import json
import posixpath
import re
import sys
from collections import Counter, OrderedDict
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import m2common as C  # noqa: E402
import cppscan as S  # noqa: E402

SUITE_DIR = "src/VP-V/tests/suite"
RENAMES = "tools/m2/test-renames.json"
CATEGORIES = "tools/test-categories.json"
TEST_PROPS = C.props_path("src/VideoProcessor-Test/VideoProcessor-Test.vcxproj.vcxproj")
TEST_PROJECT = "src/VideoProcessor-Test/VideoProcessor-Test.vcxproj.vcxproj"

CPP_KEYWORDS = set("""if for while switch return sizeof operator static_cast dynamic_cast reinterpret_cast const_cast
decltype alignof noexcept catch throw new delete else do case default typeid assert main""".split())
INC_RE = re.compile(r'^[ \t]*#[ \t]*include[ \t]*([<"])([^">\r\n]+)[">]')


def decode(raw):
    return raw.decode("utf-8", "surrogateescape")


def encode(text):
    return text.encode("utf-8", "surrogateescape")


def nl_style(text):
    return "\r\n" if "\r\n" in text else "\n"


def tokens(mask):
    return Counter(S.IDENT.findall(mask))


class FileInfo:
    def __init__(self, text):
        self.text = text
        self.top, self.mask, self.match = S.build_tree(text)
        self.classes = [m for m in S.walk(self.top) if m.kind == "class"]

    def methods(self):
        return [(m.parent.name, m) for m in S.walk(self.top) if m.kind == "method"]


def method_attr(cls_node, name):
    for ch in cls_node.children:
        if ch.kind == "attr" and ch.name == name:
            return ch
    return None


def vp_inventory(head, bill):
    """-> (vp_methods [(cls, name)], vp_classes [name], bill_classes_with_vp {cls: [names]},
    modified_bill_methods count)"""
    bill_methods = {(m.parent.name, m.name) for _, m in bill.methods()}
    bill_methods = {(c, m.name) for c, m in bill.methods()}
    bill_classes = {c.name for c in bill.classes}
    vp_methods, vp_classes = [], []
    per_class = OrderedDict()
    for c in head.classes:
        if c.name not in bill_classes:
            vp_classes.append(c.name)
            continue
        for ch in c.children:
            if ch.kind == "method" and (c.name, ch.name) not in bill_methods:
                vp_methods.append((c.name, ch.name))
                per_class.setdefault(c.name, []).append(ch.name)
    return vp_methods, vp_classes, per_class


def count_modified(head, bill):
    """Bill methods whose block differs from Bill's (diff -w semantics via whitespace squash)."""
    bmap = {}
    for c, m in bill.methods():
        bmap[(c, m.name)] = re.sub(r"\s+", "", bill.text[m.start:m.end])
    n = 0
    for c, m in head.methods():
        k = (c, m.name)
        if k in bmap and re.sub(r"\s+", "", head.text[m.start:m.end]) != bmap[k]:
            n += 1
    return n


# ------------------------------------------------------------------ edits on text

def apply_edits(text, edits):
    """edits: list of (start, end, replacement); non-overlapping after sorting; deletions use
    delete_spans' blank-line handling separately."""
    out, pos = [], 0
    for s, e, r in sorted(edits):
        out.append(text[pos:s])
        out.append(r)
        pos = e
    out.append(text[pos:])
    return "".join(out)


def class_name_span(info, cls):
    m = S.TEST_CLASS.search(info.mask, cls.start)
    return m.start(1), m.end(1)


def build_bill_text(head, vp_methods, vp_classes):
    """HEAD text with the VP methods/attributes and VP classes removed."""
    spans = []
    vpm = set(vp_methods)
    for c in head.classes:
        if c.name in vp_classes:
            spans.append(S.line_span(head.text, head.mask, c))
            continue
        for ch in c.children:
            if ch.kind == "method" and (c.name, ch.name) in vpm:
                spans.append(S.line_span(head.text, head.mask, ch))
                a = method_attr(c, ch.name)
                if a:
                    spans.append(S.line_span(head.text, head.mask, a))
    return S.delete_spans(head.text, spans)


def build_vpv_text(head, bill_class_names, vp_methods, vp_classes):
    """HEAD text reduced to the preamble, the VP classes and the Bill classes' VP methods."""
    spans, renames = [], []
    vpm = set(vp_methods)
    with_vp = {c for c, _ in vp_methods}
    for c in head.classes:
        if c.name in vp_classes:
            continue
        if c.name not in with_vp:
            spans.append(S.line_span(head.text, head.mask, c))
            continue
        a, b = class_name_span(head, c)
        renames.append((a, b, c.name + "Vpv"))
        for ch in c.children:
            if ch.kind == "method" and (c.name, ch.name) not in vpm:
                spans.append(S.line_span(head.text, head.mask, ch))
                at = method_attr(c, ch.name)
                if at:
                    spans.append(S.line_span(head.text, head.mask, at))
    # attr blocks of methods whose name is not a VP method but in a with-VP class were handled above;
    text = S.delete_spans(head.text, spans)
    # renames positions shift after deletion: redo by re-locating class names
    return rename_classes(text, [c for c in with_vp]), None


def rename_classes(text, names):
    def repl(m):
        return m.group(0).replace(m.group(1), m.group(1) + "Vpv") if m.group(1) in names else m.group(0)
    mask = S.mask_cpp(text)
    out, pos = [], 0
    for m in S.TEST_CLASS.finditer(mask):
        if m.group(1) in names:
            out.append(text[pos:m.start(1)])
            out.append(m.group(1) + "Vpv")
            pos = m.end(1)
    out.append(text[pos:])
    return "".join(out)


# ------------------------------------------------------------------ pruning

def prune(text, protected=None, label=""):
    """Remove helper members (file/namespace scope or TEST_CLASS member) whose defined identifier
    is referenced nowhere else; fixpoint. protected(ident) False => never removed."""
    removed = []
    for _ in range(20):
        top, mask, match = S.build_tree(text)
        freq = tokens(mask)
        spans = []
        for m in S.walk(top):
            if m.kind != "other":
                continue
            if m.parent is not None and m.parent.kind not in ("namespace", "class"):
                continue
            ident = S.defined_ident(m)
            if not ident:
                continue
            if protected is not None and not protected(ident):
                continue
            own = tokens(mask[m.start:m.end])
            if freq[ident] - own[ident] <= 0:
                spans.append(S.line_span(text, mask, m))
                removed.append(ident)
        # empty namespaces left behind by pruning
        for m in S.walk(top):
            if m.kind == "namespace" and m.children is not None and not m.children:
                if protected is None or True:
                    body = mask[m.body[0] + 1:m.body[1]].strip()
                    if body == "" and text[m.body[0] + 1:m.body[1]].strip() == "":
                        spans.append(S.line_span(text, mask, m))
        if not spans:
            break
        text = S.delete_spans(text, spans)
    return text, removed


def wrap_external_helpers(vpv_text, bill_text):
    """Wrap external-linkage function/variable definitions of the VPV text in an unnamed namespace
    when Bill's remaining text defines the same identifier (avoids duplicate symbols)."""
    b_top, b_mask, _ = S.build_tree(bill_text)
    bill_defs = set()

    def collect(members, unnamed):
        for m in members:
            if m.kind == "namespace":
                collect(m.children or [], unnamed or m.unnamed)
            elif S.external_linkage_definition(m, unnamed):
                bill_defs.add(S.defined_ident(m))
    collect(b_top, False)
    top, mask, _ = S.build_tree(vpv_text)
    spans = []
    wrapped = []

    def scan(members, unnamed):
        for m in members:
            if m.kind == "namespace":
                scan(m.children or [], unnamed or m.unnamed)
            elif S.external_linkage_definition(m, unnamed):
                ident = S.defined_ident(m)
                if ident in bill_defs:
                    spans.append(m)
                    wrapped.append(ident)
    scan(top, False)
    if not spans:
        return vpv_text, []
    nl = nl_style(vpv_text)
    edits = []
    for m in spans:
        s, e = S.line_span(vpv_text, mask, m)
        edits.append((s, s, "namespace {" + nl))
        edits.append((e, e, "}" + nl))
    return apply_edits_insert(vpv_text, edits), sorted(set(wrapped))


def apply_edits_insert(text, edits):
    out, pos = [], 0
    for s, e, r in sorted(edits, key=lambda x: (x[0], 0 if x[2].startswith("namespace") else 1)):
        out.append(text[pos:s])
        out.append(r)
        pos = e
    out.append(text[pos:])
    return "".join(out)


# ------------------------------------------------------------------ include pruning (Bill file)

_tracked_cache = {}


def resolve_header(inc):
    if not _tracked_cache:
        for p in C.ls_files("src", "3rdparty"):
            _tracked_cache.setdefault(posixpath.basename(p).lower(), []).append(p)
    cands = _tracked_cache.get(posixpath.basename(inc).lower(), [])
    suffix = inc.replace("\\", "/").lower()
    hit = [p for p in cands if p.lower().endswith("/" + suffix) or p.lower() == suffix]
    return hit[0] if hit else (cands[0] if len(cands) == 1 and "/" not in suffix else None)


def declared_names(path):
    raw = C.read_working(path)
    if raw is None:
        return None
    t = decode(raw)
    m = S.mask_cpp(t)
    names = set()
    for r in (r"\b(?:struct|class|union|namespace)\s+(\w+)", r"\benum(?:\s+class|\s+struct)?\s+(\w+)",
              r"\busing\s+(\w+)\s*=", r"\bIDENT\b"):
        names.update(re.findall(r, m))
    names.update(re.findall(r"#\s*define\s+(\w+)", t))
    names.update(re.findall(r"\b(\w+)\s*\(", m))
    return names - CPP_KEYWORDS


def prune_includes(text, bill_text, report):
    bill_lines = {l.strip() for l in bill_text.split("\n")}
    mask = S.mask_cpp(text)
    # remaining-text tokens exclude the include lines themselves (preprocessor lines are masked)
    toks = set(S.IDENT.findall(mask))
    out, removed = [], []
    for ln in text.split("\n"):
        s = ln.strip()
        m = INC_RE.match(ln)
        if m and s not in bill_lines:
            inc = m.group(2)
            path = resolve_header(inc) if m.group(1) in "<\"" else None
            names = declared_names(path) if path else None
            stem = posixpath.splitext(posixpath.basename(inc))[0]
            if names is not None and stem not in toks and not (names & toks):
                removed.append(s)
                continue
            report.append("kept VP include (unsure): %s" % s)
        out.append(ln)
    return "\n".join(out), removed


# ------------------------------------------------------------------ append mode

def append_to_vpv(existing_text, head, vp_methods, vp_classes, per_class, notes):
    info = FileInfo(existing_text)
    nl = nl_style(existing_text)
    edits = []
    for cname, names in per_class.items():
        tgt = [c for c in info.classes if c.name == cname + "Vpv"]
        hc = [c for c in head.classes if c.name == cname][0]
        if not tgt:
            notes.append("append: no %sVpv class in existing file; methods %s not appended" % (cname, names))
            continue
        chunks = []
        for ch in hc.children:
            if ch.kind == "method" and ch.name in names:
                a = method_attr(hc, ch.name)
                for node in ([a] if a else []) + [ch]:
                    s, e = S.line_span(head.text, head.mask, node)
                    chunks.append(head.text[s:e])
        pos = tgt[0].body[1]
        ls = existing_text.rfind("\n", 0, pos) + 1
        edits.append((ls, ls, nl + "".join(chunks)))
    for cname in vp_classes:
        hc = [c for c in head.classes if c.name == cname][0]
        s, e = S.line_span(head.text, head.mask, hc)
        parent = hc.parent
        tgt = None
        if parent is not None:
            for m in S.walk(info.top):
                if m.kind == "namespace" and m.name == parent.name:
                    tgt = m
        if tgt is None:
            notes.append("append: namespace for VP class %s not found; class not appended" % cname)
            continue
        ls = existing_text.rfind("\n", 0, tgt.body[1]) + 1
        edits.append((ls, ls, nl + head.text[s:e]))
    return apply_edits_insert(existing_text, edits)


# ------------------------------------------------------------------ project / renames / categories

def cond_depth(text, pos):
    depth = 0
    for ln in text[:pos].split("\n"):
        t = ln.strip()
        if re.match(r"#\s*if", t):
            depth += 1
        elif re.match(r"#\s*endif", t):
            depth -= 1
    return depth


def update_props(plan, bill_path, new_path):
    praw = plan.get(TEST_PROPS) or C.read_working(TEST_PROPS)
    items, pnl, _ = C.parse_props(praw)
    key = ("ClCompile", C.norm_include(posixpath.relpath(new_path, "src/VideoProcessor-Test").replace("/", "\\")))
    if any(i.key == key for i in items):
        return
    rel = posixpath.relpath(new_path, "src/VideoProcessor-Test").replace("/", "\\")
    # copy metadata / Condition from Bill's own item in the Test project
    proj_text, _ = C.decode(C.read_working(TEST_PROJECT))
    groups = C.parse_project(proj_text)
    bname = posixpath.basename(bill_path).lower()
    tmpl = None
    for it in C.all_items(groups):
        if it.tag == "ClCompile" and C.norm_include(it.include) == bname:
            tmpl = it
    if tmpl is not None:
        text = re.sub(r'(\bInclude\s*=\s*")[^"]*(")', lambda m: m.group(1) + rel + m.group(2), tmpl.text, count=1)
        norm = C.norm_item_text(text, proj_text, tmpl.start)
    else:
        norm = '<ClCompile Include="%s" />' % rel
    items.append(C.PropsItem("ClCompile", rel, norm, key))
    plan[TEST_PROPS] = C.render_props(C.props_name(TEST_PROJECT), items, pnl or "\r\n")


def update_renames(renames_new, plan):
    cur = []
    raw = C.read_working(RENAMES)
    if raw:
        cur = json.loads(raw.decode("utf-8"))
    d = {r["old"]: r["new"] for r in cur}
    d.update(renames_new)
    out = [{"old": k, "new": d[k]} for k in sorted(d)]
    text = json.dumps(out, indent=1, ensure_ascii=False) + "\n"
    plan[RENAMES] = text.encode("utf-8")


def update_categories(moved, lost_classes, new_file_for_class, plan):
    """moved: {(Class, Method): (NewClass, newfile)}; lost_classes: {Class: newfile}."""
    raw = C.read_working(CATEGORIES)
    d = json.loads(raw.decode("utf-8"), object_pairs_hook=OrderedDict)
    changed = False
    entries = d["entries"]
    have = {(e["kind"], e["value"]) for e in entries}
    for e in list(entries):
        if e["kind"] == "method":
            c, m = e["value"].split("::")
            if (c, m) in moved:
                nc, nf = moved[(c, m)]
                e["value"] = "%s::%s" % (nc, m)
                e["file"] = nf
                changed = True
        elif e["kind"] == "class" and e["value"] in lost_classes:
            nv = e["value"] + "Vpv"
            if ("class", nv) not in have:
                ne = OrderedDict(e)
                ne["value"] = nv
                ne["file"] = lost_classes[e["value"]]
                ne["note"] = (e.get("note", "") + " (VP-V split)").strip()
                entries.append(ne)
                have.add(("class", nv))
                changed = True
    if changed:
        # keep the manifest's own formatting convention: 1-space indent, original text for untouched keys
        text = json.dumps(d, indent=1, ensure_ascii=False) + "\n"
        plan[CATEGORIES] = text.encode("utf-8")


# ------------------------------------------------------------------ driver

def plan_file(f, bill_ref, cfg, plan, report):
    stem = Path(f).stem
    stem_no = stem[:-len("Tests")] if stem.endswith("Tests") else stem
    new_path = "%s/%sVpvTests.cpp" % (SUITE_DIR, stem_no)
    head_raw = C.read_working(f)
    bill_raw = C.bill_bytes(bill_ref, f)
    if head_raw is None or bill_raw is None:
        report.append("%s: missing at HEAD or bill" % f)
        return None
    head_text = decode(head_raw)
    bill_text = decode(bill_raw)
    head, bill = FileInfo(head_text), FileInfo(bill_text)
    vp_methods, vp_classes, per_class = vp_inventory(head, bill)
    res = {"file": f, "new": new_path, "vp_methods": vp_methods, "vp_classes": vp_classes,
           "modified": count_modified(head, bill), "pruned_bill": [], "pruned_vpv": [], "wrapped": [],
           "inc_removed": [], "notes": [], "lines_before": head_text.count("\n"), "bill_lines": bill_text.count("\n")}
    if not vp_methods and not vp_classes:
        res["lines_after"] = head_text.count("\n")
        if C.read_working(new_path) is not None:
            update_props(plan, f, new_path)
        return res
    # --- conditional-compilation guard: VP methods inside #if/#ifdef regions are only reported
    for c in head.classes:
        for ch in c.children or []:
            if ch.kind == "method" and (c.name, ch.name) in set(vp_methods):
                if cond_depth(head_text, ch.start) > 0:
                    res["notes"].append("VP method %s::%s lies inside a conditional-compilation block "
                                        "(moved without it: review)" % (c.name, ch.name))
    # --- Bill-side text
    new_bill = build_bill_text(head, vp_methods, vp_classes)
    bill_idents = set(S.IDENT.findall(bill.mask))
    new_bill, pr = prune(new_bill, protected=lambda i: i not in bill_idents)
    res["pruned_bill"] = pr
    new_bill, inc_rm = prune_includes(new_bill, bill_text, res["notes"])
    res["inc_removed"] = inc_rm
    # --- Vpv text
    existing = C.read_working(new_path)
    nl = nl_style(head_text)
    if existing is not None:
        vtext = append_to_vpv(decode(existing), head, vp_methods, vp_classes, per_class, res["notes"])
        res["append"] = True
    else:
        vtext, _ = build_vpv_text(head, {c.name for c in bill.classes}, vp_methods, vp_classes)
        vtext, pv = prune(vtext)
        res["pruned_vpv"] = pv
        vtext, wr = wrap_external_helpers(vtext, new_bill)
        res["wrapped"] = wr
        bom = "﻿" if vtext.startswith("﻿") else ""
        vtext = vtext[len(bom):]
        header = ("// Copyright(C) 2026 VP-V contributors" + nl +
                  "// SPDX-License-Identifier: GPL-3.0-only" + nl +
                  "// VP-V tests split out of Bill's %s by tools/m2/move_tests.py." % Path(f).name + nl)
        vtext = bom + header + vtext
        res["append"] = False
    res["lines_after"] = new_bill.count("\n")
    plan[f] = encode(new_bill)
    plan[new_path] = encode(vtext)
    update_props(plan, f, new_path)
    res["per_class"] = per_class
    res["class_renames"] = {c: c + "Vpv" for c in per_class}
    return res


def run(args, cfg):
    bill_ref = args.bill
    plan, report, results = {}, [], []
    excluded = cfg.get("exclude", {})
    renames_new, moved_map, lost = {}, {}, {}
    for f in cfg["split"]:
        if f in excluded:
            report.append("%s: excluded (%s)" % (f, excluded[f]))
            continue
        r = plan_file(f, bill_ref, cfg, plan, report)
        if r is None:
            continue
        results.append(r)
        for c, names in r.get("per_class", {}).items():
            for n in names:
                renames_new["%s::%s" % (c, n)] = "%sVpv::%s" % (c, n)
                moved_map[(c, n)] = (c + "Vpv", r["new"])
            lost[c] = r["new"]
    if renames_new or C.read_working(RENAMES) is None:
        update_renames(renames_new, plan)
    if moved_map or lost:
        update_categories(moved_map, lost, None, plan)
    # --- report
    print("== P15b plan ==")
    tot = Counter()
    for r in sorted(results, key=lambda x: x["file"]):
        print("\n%s -> %s%s" % (r["file"], r["new"], " (append)" if r.get("append") else ""))
        print("  VP methods to move: %d in classes %s; VP classes to move: %s" % (
            len(r["vp_methods"]), sorted({c for c, _ in r["vp_methods"]}), r["vp_classes"]))
        print("  modified Bill methods (residual footprint): %d" % r["modified"])
        print("  Bill-side helpers pruned: %s" % (sorted(set(r["pruned_bill"])) or "-"))
        print("  Vpv-side helpers pruned: %s; wrapped in unnamed namespace: %s" % (
            sorted(set(r["pruned_vpv"])) or "-", r["wrapped"] or "-"))
        print("  VP includes removed from Bill file: %s" % (r["inc_removed"] or "-"))
        print("  lines: HEAD %d -> %d (Bill %d)" % (r["lines_before"], r["lines_after"], r["bill_lines"]))
        for n in r["notes"]:
            print("  NOTE %s" % n)
        tot["methods"] += len(r["vp_methods"])
        tot["classes"] += len(r["vp_classes"])
        tot["modified"] += r["modified"]
    for line in sorted(report):
        print("REPORT %s" % line)
    for f, why in sorted(cfg.get("split_excluded_open_items", {}).items()):
        print("OPEN ITEM (not split): %s : %s" % (f, why))
    print("\n== totals: %d method(s), %d class(es), %d renames, %d modified Bill methods ==" % (
        tot["methods"], tot["classes"], len(renames_new), tot["modified"]))
    changes = [p for p in sorted(plan) if C.read_working(p) != plan[p]]
    print("files that would change: %d" % len(changes))
    for c in changes:
        print("  " + c)
    if args.verify:
        print("verify: %d file(s) would change" % len(changes))
        return 1 if changes else 0
    if args.apply:
        for c in changes:
            C.write_if_changed(c, plan[c])
        print("applied: %d file(s) written" % len(changes))
    else:
        print("(dry run: nothing written)")
    return 0
