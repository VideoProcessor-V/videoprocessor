# Copyright(C) 2026 VP-V contributors
# SPDX-License-Identifier: GPL-3.0-only
"""Master settings checker and ratchet (W1-MS).

Usage: python -I tools/check_master_settings.py [--json] [--root PATH] [--strict]
                                                [--write-baseline] [--baseline FILE]
                                                [--write-upstream GIT-REF] [--base-ref GIT-REF]

Compares the keys that exist in code (Bill-style schema files, [vpv] keys, VP-V
field tables in either layout, [vpv.*] namespaced sections) with the rows of
the module README '## Settings' tables.

Hard errors (always exit 1): malformed or duplicate rows, a row naming the
owner's display, a stale docs/settings/MASTER-SETTINGS.md.
Gaps (ratcheted by tools/master_settings_baseline.json):
  undocumented      key in code without a row (keys of Bill's schema lists, in
                    docs/settings/UPSTREAM-KEYS.txt, are out of scope)
  stale             documented key not in code (rows marked planned/declared exempt)
  default_mismatch  documented default differs from the defaults golden

Default run reports and fails only on hard errors. --strict also fails when a
gap is not in the baseline. --write-baseline records the current gaps but never
adds one (the gap count may only go down); a missing baseline is created once.
--write-upstream REF rebuilds the upstream key list from Bill's schema files at REF.
"""
import argparse
import json
import os
import subprocess
import sys

sys.path.append(os.path.dirname(os.path.abspath(__file__)))
import master_settings_common as C  # noqa: E402

BASELINE_REL = "tools/master_settings_baseline.json"
GAP_KINDS = ("undocumented", "stale", "default_mismatch")


def load_baseline(path):
    if not os.path.isfile(path):
        return None
    data = json.loads(C.read_text(path))
    return {k: sorted(data.get("gaps", {}).get(k, [])) for k in GAP_KINDS}


def new_gaps(gaps, baseline):
    base = baseline or {k: [] for k in GAP_KINDS}
    return {k: sorted(set(gaps[k]) - set(base.get(k, []))) for k in GAP_KINDS}


def baseline_text(gaps):
    data = {
        "schema": 1,
        "note": "Ratchet for tools/check_master_settings.py: gaps may only be removed, never added.",
        "counts": {k: len(gaps[k]) for k in GAP_KINDS},
        "gaps": {k: sorted(gaps[k]) for k in GAP_KINDS},
    }
    return json.dumps(data, indent=1, sort_keys=True) + "\n"


def write_upstream(root, ref):
    keys = set()
    for rel in C.SCHEMA_FILES:
        p = subprocess.run(["git", "-C", root, "show", "%s:%s" % (ref, rel)],
                           capture_output=True, text=True, encoding="utf-8", errors="replace")
        if p.returncode != 0:
            print("git show %s:%s failed: %s" % (ref, rel, p.stderr.strip()), file=sys.stderr)
            return 1
        keys |= C.extract_schema_keys(p.stdout)
    path = os.path.join(root, *C.UPSTREAM_REL.split("/"))
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, "w", encoding="utf-8", newline="\n") as fh:
        fh.write(C.write_upstream_text(keys, ref))
    print("wrote %s (%d keys)" % (C.UPSTREAM_REL, len(keys)))
    return 0


def git_show(root, ref, rel):
    p = subprocess.run(["git", "-C", root, "show", "%s:%s" % (ref, rel)],
                       capture_output=True, text=True, encoding="utf-8-sig", errors="replace")
    return p.stdout if p.returncode == 0 else None


def base_ref_errors(root, ref, baseline):
    """Ratchet against a git ref (merge base): the baseline and upstream list may only shrink."""
    errs = []
    old = git_show(root, ref, BASELINE_REL)
    if old is not None and baseline is not None:
        og = json.loads(old).get("gaps", {})
        for k in GAP_KINDS:
            added = sorted(set(baseline.get(k, [])) - set(og.get(k, [])))
            if added:
                errs.append("baseline gained %s gap(s) vs %s: %s" % (k, ref, ", ".join(added)))
    up = git_show(root, ref, C.UPSTREAM_REL)
    if up is not None:
        oldkeys = {ln.strip() for ln in up.splitlines() if ln.strip() and not ln.startswith("#")}
        added = sorted(C.load_upstream(root) - oldkeys)
        if added:
            errs.append("upstream key list gained keys vs %s (a VP-V key hidden from scope?): %s" % (ref, ", ".join(added)))
    return errs


def master_stale(root):
    path = os.path.join(root, *C.MASTER_REL.split("/"))
    if not os.path.isfile(path):
        return "%s is missing: run python -I tools/gen_master_settings.py" % C.MASTER_REL
    with open(path, encoding="utf-8") as fh:   # universal newlines: a CRLF checkout compares equal
        cur = fh.read()
    if cur != C.render_master(root):
        return "%s is out of date: run python -I tools/gen_master_settings.py" % C.MASTER_REL
    return None


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--root", default=os.path.join(os.path.dirname(os.path.abspath(__file__)), ".."))
    ap.add_argument("--json", action="store_true")
    ap.add_argument("--strict", action="store_true")
    ap.add_argument("--write-baseline", action="store_true")
    ap.add_argument("--baseline", default=None)
    ap.add_argument("--write-upstream", metavar="GIT_REF")
    ap.add_argument("--base-ref", metavar="GIT_REF",
                    help="merge-base ref: fail if the baseline or upstream list grew relative to it")
    args = ap.parse_args(argv)
    root = os.path.abspath(args.root)
    bpath = args.baseline or os.path.join(root, *BASELINE_REL.split("/"))

    if args.write_upstream:
        return write_upstream(root, args.write_upstream)

    an = C.analyse(root)
    gaps, errors = an["gaps"], list(an["errors"])
    if not args.write_baseline:
        stale_master = master_stale(root)
        if stale_master:
            errors.append(stale_master)
    baseline = load_baseline(bpath)
    if baseline is not None and os.path.isfile(bpath):
        counts = json.loads(C.read_text(bpath)).get("counts", {})
        if any(counts.get(k) != len(baseline[k]) for k in GAP_KINDS):
            errors.append("baseline counts do not match its gap lists")
    if args.base_ref:
        errors += base_ref_errors(root, args.base_ref, baseline)
    fresh = new_gaps(gaps, baseline)
    shrinkable = {k: sorted(set((baseline or {}).get(k, [])) - set(gaps[k])) for k in GAP_KINDS}
    n_new = sum(len(v) for v in fresh.values())

    if args.write_baseline:
        if baseline is not None and n_new:
            print("refusing to raise the baseline: %d new gap(s): %s" % (
                n_new, json.dumps(fresh, sort_keys=True)), file=sys.stderr)
            return 1
        os.makedirs(os.path.dirname(bpath), exist_ok=True)
        with open(bpath, "w", encoding="utf-8", newline="\n") as fh:
            fh.write(baseline_text(gaps))
        print("wrote %s (%s)" % (BASELINE_REL, ", ".join("%s=%d" % (k, len(gaps[k])) for k in GAP_KINDS)))
        return 1 if errors else 0

    report = {
        "keys_in_code": len(an["code"]), "keys_in_scope": len(an["scope"]),
        "keys_upstream": len([k for k in an["code"] if k in an["upstream"]]),
        "documented_rows": len(an["rows"]),
        "documented_in_scope": len([k for k in an["scope"] if k in an["by_id"]]),
        "gaps": {k: len(gaps[k]) for k in GAP_KINDS},
        "baseline": {k: len((baseline or {}).get(k, [])) for k in GAP_KINDS} if baseline else None,
        "new_gaps": fresh, "baseline_can_shrink": shrinkable, "errors": errors,
    }
    if args.json:
        print(json.dumps(report, indent=1, sort_keys=True))
    else:
        print("keys in code: %d (upstream %d, VP-V scope %d); documented rows: %d (in scope %d)" % (
            report["keys_in_code"], report["keys_upstream"], report["keys_in_scope"],
            report["documented_rows"], report["documented_in_scope"]))
        print("gaps: " + ", ".join("%s=%d" % (k, len(gaps[k])) for k in GAP_KINDS) +
              ("  baseline: " + ", ".join("%s=%d" % (k, len(baseline[k])) for k in GAP_KINDS) if baseline else "  (no baseline)"))
        for k in GAP_KINDS:
            for g in fresh[k]:
                print("NEW GAP %s: %s" % (k, g))
        for k in GAP_KINDS:
            if shrinkable[k]:
                print("baseline can shrink (%s): %s" % (k, ", ".join(shrinkable[k])))
        for e in errors:
            print("ERROR: " + e)
    if errors:
        return 1
    if args.strict and (n_new or baseline is None):
        if baseline is None:
            print("no baseline file: run --write-baseline once", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
