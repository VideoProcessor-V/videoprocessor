#!/usr/bin/env python
# Copyright(C) 2026 VP-V contributors
# SPDX-License-Identifier: GPL-3.0-only
"""P1 (H7): move VP-V item elements out of Bill's *.vcxproj files into src/VP-V/build/<name>.items.props.

Modes: --dry-run (default), --apply, --verify (exit 1 if a dry-run would change anything).
Everything is recomputed from the current tree and the --bill ref on every run; the script is
idempotent. See tools/m2/README.md.
"""
import argparse
import re
import sys
from collections import Counter, OrderedDict
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import m2common as C  # noqa: E402

CPP_TARGETS_RE = re.compile(r"^([ \t]*)<Import\s+Project=\"\$\(VCTargetsPath\)\\Microsoft\.Cpp\.targets\"\s*/>[ \t]*(\r?\n)",
                            re.M)


def bill_items_multiset(bill_groups):
    cnt = Counter()
    for it in C.all_items(bill_groups):
        cnt[it.key] += 1
    return cnt


def classify(head_groups, bill_groups, bill_text):
    """-> (moved items, modified bill items, removed bill items(list of key), staying count)."""
    bill_counts = bill_items_multiset(bill_groups)
    bill_text_by_key = {}
    for it in C.all_items(bill_groups):
        bill_text_by_key.setdefault(it.key, []).append(it)
    seen = Counter()
    moved, modified, stay = [], [], []
    for it in C.all_items(head_groups):
        seen[it.key] += 1
        if seen[it.key] <= bill_counts.get(it.key, 0):
            stay.append(it)
            b = bill_text_by_key[it.key][seen[it.key] - 1]
            if _squash(b.text) != _squash(it.text):
                modified.append(it)
        else:
            moved.append(it)
    head_counts = Counter(i.key for i in C.all_items(head_groups))
    removed = sorted(k for k, n in bill_counts.items() if head_counts.get(k, 0) < n)
    return moved, modified, removed, stay


def _squash(s):
    return re.sub(r"\s+", " ", s).strip()


def process_project(bill, path, plan, report):
    raw = C.read_working(path)
    braw = C.bill_bytes(bill, path)
    if raw is None or braw is None:
        report.append((path, "SKIP (missing at %s)" % ("HEAD" if raw is None else bill)))
        return None
    text, bom = C.decode(raw)
    btext, _ = C.decode(braw)
    nl = C.detect_newline(text)
    hg = C.parse_project(text)
    bg = C.parse_project(btext)
    moved, modified, removed, stay = classify(hg, bg, btext)
    name = C.props_name(path)
    pp = C.props_path(path)
    props_raw = C.read_working(pp)
    p_items, p_nl, _ = C.parse_props(props_raw)
    have = Counter(i.key for i in p_items)
    new_items = []
    for it in moved:
        # lead comments only travel when Bill's file does not contain the comment text
        lead = [c for c in it.lead if c[2] not in btext]
        s = lead[0][0] if lead else it.start
        norm = C.norm_item_text(text[s:it.end], text, s)
        new_items.append(C.PropsItem(it.tag, it.include, norm, it.key))
    merged = list(p_items)
    existing = {i.key for i in p_items}
    appended = 0
    for ni in new_items:
        if ni.key in existing:
            continue  # already in props: dedupe by key
        existing.add(ni.key)
        merged.append(ni)
        appended += 1

    # --- remove spans from the Bill project
    spans = []
    emptied_groups = 0
    moved_ids = {id(i) for i in moved}
    for g in hg:
        gm = [i for i in g.items if id(i) in moved_ids]
        if not gm:
            continue
        for i in gm:
            lead = [c for c in i.lead if c[2] not in btext]
            s = lead[0][0] if lead else i.start
            spans.append((s, i.end))
        remaining = [i for i in g.items if id(i) not in moved_ids]
        leftover_comments = [c for c in g.comments
                             if not any(s <= c[0] and c[1] <= e for s, e in spans)]
        if not remaining and not leftover_comments and g.attrs.get("Label") != "ProjectConfigurations":
            spans.append((g.start, g.end))
            emptied_groups += 1
    new_text = C.remove_spans(text, spans)

    # --- import line
    imp_marker = "VP-V\\build\\%s.items.props" % name
    have_import = imp_marker.lower() in new_text.lower() and "<import" in new_text.lower()
    props_will_exist = bool(merged)
    import_added = False
    import_reused = False
    if props_will_exist:
        if re.search(r"<Import\b[^>]*" + re.escape(imp_marker), new_text, re.I):
            import_reused = True
        else:
            m = CPP_TARGETS_RE.search(new_text)
            if not m:
                report.append((path, "ERROR no Microsoft.Cpp.targets import line found; import not added"))
            else:
                line = ("%s<Import Project=\"$(MSBuildThisFileDirectory)..\\VP-V\\build\\%s.items.props\" "
                        "Condition=\"Exists('$(MSBuildThisFileDirectory)..\\VP-V\\build\\%s.items.props')\" />%s"
                        % (m.group(1), name, name, m.group(2)))
                new_text = new_text[:m.start()] + line + new_text[m.start():]
                import_added = True

    if props_will_exist:
        use_nl = p_nl or nl
        plan[pp] = C.render_props(name, merged, use_nl)
    new_raw = C.encode(new_text, bom)
    plan[path] = new_raw

    info = OrderedDict()
    info["path"] = path
    info["props"] = pp
    info["moved"] = moved
    info["moved_by_tag"] = Counter(i.tag for i in moved)
    info["modified"] = modified
    info["removed"] = removed
    info["appended_to_props"] = appended
    info["props_total"] = len(merged)
    info["emptied_groups"] = emptied_groups
    info["import"] = "added" if import_added else ("H7 import reused" if import_reused else "none")
    info["lines_before"] = text.count("\n") + (0 if text.endswith("\n") or not text else 1)
    info["lines_after"] = new_text.count("\n") + (0 if new_text.endswith("\n") or not new_text else 1)
    info["bill_lines"] = btext.count("\n") + (0 if btext.endswith("\n") else 1)
    info["name"] = name

    # --- filters
    finfo = []
    moved_keys = {i.key for i in moved}
    for fpath in filters_for(bill, path):
        finfo.append(process_filters(bill, fpath, moved_keys, plan))
    info["filters"] = finfo
    return info


def filters_for(bill, project_path):
    d = Path(project_path).parent.as_posix()
    cands = sorted(set(p for p in C.bill_ls(bill, d + "/") if p.endswith(".filters") and p.startswith(d + "/")
                       and "/" not in p[len(d) + 1:]))
    base = Path(project_path).name
    out = []
    for p in cands:
        pn = Path(p).name
        stem = pn[: -len(".filters")]
        if stem == base or base.startswith(stem):
            out.append(p)
    return out


def process_filters(bill, fpath, moved_keys, plan):
    raw = C.read_working(fpath)
    braw = C.bill_bytes(bill, fpath)
    res = {"path": fpath, "entries_removed": 0, "filters_removed": 0, "groups_emptied": 0}
    if raw is None or braw is None:
        res["skip"] = True
        return res
    text, bom = C.decode(raw)
    btext, _ = C.decode(braw)
    hg = C.parse_project(text)
    bg = C.parse_project(btext)
    bill_filter_defs = {i.include.lower() for i in C.all_items(bg) if i.tag == "Filter"}
    entries = [i for i in C.all_items(hg) if i.tag != "Filter"]
    drop_entries = [i for i in entries if i.key in moved_keys]
    drop_ids = {id(i) for i in drop_entries}
    kept_refs = set()
    for i in entries:
        if id(i) not in drop_ids:
            f = C.item_filter_name(i)
            if f:
                kept_refs.add(f.lower())
    kept_defs = {i.include.lower() for i in C.all_items(hg) if i.tag == "Filter" and i.include.lower() in bill_filter_defs}

    def referenced_or_parent(name):
        n = name.lower()
        for r in kept_refs | kept_defs:
            if r == n or r.startswith(n + "\\"):
                return True
        return False

    # VP filter definitions: absent from Bill's filters and unreferenced; iterate (child before parent)
    drop_filters = []
    vp_filters = [i for i in C.all_items(hg) if i.tag == "Filter" and i.include.lower() not in bill_filter_defs]
    # a VP filter stays while a kept VP filter lies below it
    kept_vp = set()
    for i in sorted(vp_filters, key=lambda x: -len(x.include)):
        n = i.include.lower()
        if n in kept_refs or any(k.startswith(n + "\\") for k in kept_refs | kept_vp):
            kept_vp.add(n)
        else:
            drop_filters.append(i)
    spans = []
    for i in drop_entries + drop_filters:
        spans.append((i.start, i.end))
    dropped_ids = drop_ids | {id(i) for i in drop_filters}
    for g in hg:
        gm = [i for i in g.items if id(i) in dropped_ids]
        if gm and len(gm) == len(g.items) and not g.comments:
            spans.append((g.start, g.end))
            res["groups_emptied"] += 1
    new_text = C.remove_spans(text, spans)
    res["entries_removed"] = len(drop_entries)
    res["filters_removed"] = len(drop_filters)
    plan[fpath] = C.encode(new_text, bom)
    return res


def autodetect_others(bill, report):
    """Report Bill projects under src/ outside BILL_PROJECTS whose items differ from Bill's."""
    out = []
    for p in C.bill_ls(bill, "src/"):
        if not p.endswith(".vcxproj") or p in C.BILL_PROJECTS:
            continue
        raw = C.read_working(p)
        if raw is None:
            continue
        text, _ = C.decode(raw)
        btext, _ = C.decode(C.bill_bytes(bill, p))
        moved, modified, removed, _ = classify(C.parse_project(text), C.parse_project(btext), btext)
        if moved or modified or removed:
            out.append((p, len(moved), len(modified), len(removed)))
    return out


def build_plan(bill):
    plan = {}
    report = []
    infos = []
    names = {}
    for p in C.BILL_PROJECTS:
        if not C.bill_exists(bill, p):
            report.append((p, "SKIP (not present at bill ref)"))
            continue
        n = C.props_name(p)
        if n in names:
            raise SystemExit("props name collision: %s and %s -> %s" % (names[n], p, n))
        names[n] = p
        info = process_project(bill, p, plan, report)
        if info:
            infos.append(info)
    others = autodetect_others(bill, report)
    return plan, infos, report, others


def print_report(infos, report, others, plan):
    tot = Counter()
    print("== P1 plan (projects in sorted order) ==")
    for info in sorted(infos, key=lambda i: i["path"]):
        print("\n%s" % info["path"])
        print("  props: %s (items after merge: %d, newly appended: %d)" % (info["props"], info["props_total"],
                                                                         info["appended_to_props"]))
        print("  moved from project: %d  by tag: %s" % (len(info["moved"]),
              ", ".join("%s=%d" % kv for kv in sorted(info["moved_by_tag"].items())) or "-"))
        print("  import: %s; emptied ItemGroups removed: %d" % (info["import"], info["emptied_groups"]))
        print("  project lines: HEAD %d -> %d (Bill %d)" % (info["lines_before"], info["lines_after"],
                                                           info["bill_lines"]))
        for m in sorted(info["modified"], key=lambda i: i.key):
            print("  modified Bill item, stays: %s %s" % (m.tag, m.include))
        for k in info["removed"]:
            print("  removed Bill item: %s %s" % k)
        for f in info["filters"]:
            if f.get("skip"):
                continue
            print("  filters %s: entries removed %d, Filter defs removed %d, groups emptied %d" % (
                f["path"], f["entries_removed"], f["filters_removed"], f["groups_emptied"]))
            tot["f_entries"] += f["entries_removed"]
            tot["f_defs"] += f["filters_removed"]
        tot["moved"] += len(info["moved"])
        tot["modified"] += len(info["modified"])
        tot["removed"] += len(info["removed"])
        tot["lines_before"] += info["lines_before"]
        tot["lines_after"] += info["lines_after"]
    print("\n== moved items table (project | tag | include) ==")
    rows = []
    for info in infos:
        for m in info["moved"]:
            rows.append((info["path"], m.tag, m.include))
    for r in sorted(rows):
        print("  %s | %s | %s" % r)
    for p, msg in sorted(report):
        print("NOTE %s: %s" % (p, msg))
    for p, mv, md, rm in sorted(others):
        print("REPORT other Bill project differs: %s (VP items %d, modified %d, removed Bill items %d) - not touched" %
              (p, mv, md, rm))
    print("\n== totals: moved %d, modified-Bill-items %d, removed-Bill-items %d, filters entries %d, "
          "Filter defs %d; project lines %d -> %d ==" % (tot["moved"], tot["modified"], tot["removed"],
                                                         tot["f_entries"], tot["f_defs"], tot["lines_before"],
                                                         tot["lines_after"]))


def changed_files(plan):
    out = []
    for path in sorted(plan):
        cur = C.read_working(path)
        if cur != plan[path]:
            out.append(path)
    return out


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__)
    g = ap.add_mutually_exclusive_group()
    g.add_argument("--dry-run", action="store_true")
    g.add_argument("--apply", action="store_true")
    g.add_argument("--verify", action="store_true")
    ap.add_argument("--bill", default=C.DEFAULT_BILL)
    a = ap.parse_args(argv)
    plan, infos, report, others = build_plan(a.bill)
    changes = changed_files(plan)
    if a.verify:
        for c in changes:
            print("would change: %s" % c)
        print("verify: %d file(s) still to change" % len(changes))
        return 1 if changes else 0
    print_report(infos, report, others, plan)
    print("\nfiles that would change: %d" % len(changes))
    for c in changes:
        print("  " + c)
    if a.apply:
        for c in changes:
            C.write_if_changed(c, plan[c])
        print("applied: %d file(s) written" % len(changes))
    else:
        print("(dry run: nothing written)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
