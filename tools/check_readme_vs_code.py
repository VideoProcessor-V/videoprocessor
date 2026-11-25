# Copyright(C) 2026 VP-V contributors
# SPDX-License-Identifier: GPL-3.0-only
"""N8 README-vs-code consistency checker (doc 124 s2.2 N8).

Usage: python -I tools/check_readme_vs_code.py [--root P] [--baseline FILE]
                                               [--write-baseline] [--json] [--strict] [--selftest]

Checks every module folder under src/VP-V/modules/<unit>/<slot>/ that contains a MANIFEST.json:
  1. MANIFEST.json parses cleanly and agrees with its folder path (<unit>/<slot>) and valid status
     (built, partial, placeholder, draft).
  2. Every module with a MANIFEST.json has a README.md.
  3. The README.md header/metadata lines (first 15 lines) agree with MANIFEST.json on:
       - slot id (must appear in the README header)
       - unit (if declared as `Unit: **<unit>**`, must match; required for non-draft modules)
       - order (if declared as `order: <N>`, must match)
       - status (if declared as `status: **<status>**`, must match — explicitly catching a README
         claiming `status: **built**` while MANIFEST.json says `placeholder` or vice versa; required
         for non-draft modules)
  4. Every entry in MANIFEST.json `live_files` exists on disk, and every explicit source path
     (*.h, *.hpp, *.cpp, *.cc, *.cxx, *.c, *.inl, *.glsl, *.hlsl) exists as a regular file.

Shrink-only baseline (tools/readme-vs-code-baseline.json): records pre-existing drifts so they stay
visible and can only shrink (never grow). When no default baseline exists in --root (e.g. a clean
synthetic test tree), zero drifts are allowed. Exit 0 clean, 1 violation, 2 error.
"""
import argparse
import importlib.util
import json
import os
import re
import shutil
import sys
import tempfile


def _lib():
    p = os.path.join(os.path.dirname(os.path.abspath(__file__)), "compliance_lib.py")
    s = importlib.util.spec_from_file_location("compliance_lib", p)
    m = importlib.util.module_from_spec(s)
    s.loader.exec_module(m)
    return m


L = _lib()
BASELINE = "tools/readme-vs-code-baseline.json"
VALID_STATUSES = {"built", "partial", "placeholder", "draft"}
SOURCE_EXTS = (".h", ".hpp", ".cpp", ".cc", ".cxx", ".c", ".inl", ".glsl", ".hlsl")
HEADER_LINES = 15
UNIT_RX = re.compile(r"\bUnit:?\s*\*\*([A-Za-z0-9_]+)\*\*")
ORDER_RX = re.compile(r"\border:?\s*(\d+)\b")
STATUS_RX = re.compile(r"\bstatus:?\s*\*\*([A-Za-z0-9_]+)\*\*", re.I)


def drift_key(item):
    return "%s|%s|%s" % (item["slot"], item["kind"], item["detail"])


def scan_module(root, unit_dir, slot_dir, folder):
    """Return list of drift dicts {slot, kind, file, detail} for one module folder."""
    drifts = []
    rel_dir = os.path.relpath(folder, root).replace(os.sep, "/")
    man_rel = rel_dir + "/MANIFEST.json"
    readme_rel = rel_dir + "/README.md"
    expected_slot = "%s.%s" % (unit_dir, slot_dir)

    man = L.load_json(os.path.join(folder, "MANIFEST.json"))
    if not isinstance(man, dict):
        drifts.append({
            "slot": expected_slot,
            "kind": "manifest-invalid",
            "file": man_rel,
            "detail": "MANIFEST.json is not a valid JSON object",
        })
        return drifts

    slot = str(man.get("slot") or "")
    unit = str(man.get("unit") or (slot.split(".")[0] if "." in slot else ""))
    order = man.get("order")
    status = str(man.get("status") or "")

    if slot != expected_slot:
        drifts.append({
            "slot": expected_slot,
            "kind": "manifest-slot",
            "file": man_rel,
            "detail": "MANIFEST slot=%r differs from folder %r" % (slot, expected_slot),
        })
    if unit != unit_dir:
        drifts.append({
            "slot": expected_slot,
            "kind": "manifest-unit",
            "file": man_rel,
            "detail": "MANIFEST unit=%r differs from folder unit %r" % (unit, unit_dir),
        })
    if status not in VALID_STATUSES:
        drifts.append({
            "slot": expected_slot,
            "kind": "manifest-status",
            "file": man_rel,
            "detail": "MANIFEST status=%r not in %s" % (status, sorted(VALID_STATUSES)),
        })

    # Check live_files on disk.
    live_files = man.get("live_files", [])
    if not isinstance(live_files, list):
        drifts.append({
            "slot": expected_slot,
            "kind": "manifest-live-files",
            "file": man_rel,
            "detail": "MANIFEST live_files is not a list",
        })
    else:
        for lf in live_files:
            if not isinstance(lf, str) or not lf.strip():
                drifts.append({
                    "slot": expected_slot,
                    "kind": "live-file-invalid",
                    "file": man_rel,
                    "detail": "invalid live_files entry %r" % (lf,),
                })
                continue
            full = os.path.join(root, *lf.split("/"))
            if lf.lower().endswith(SOURCE_EXTS):
                if not os.path.isfile(full):
                    drifts.append({
                        "slot": expected_slot,
                        "kind": "live-file-missing",
                        "file": man_rel,
                        "detail": "missing source file: %s" % lf,
                    })
            elif not os.path.exists(full):
                drifts.append({
                    "slot": expected_slot,
                    "kind": "live-file-missing",
                    "file": man_rel,
                    "detail": "missing live path: %s" % lf,
                })

    # Check README.md presence and header metadata agreement.
    readme_full = os.path.join(folder, "README.md")
    if not os.path.isfile(readme_full):
        drifts.append({
            "slot": expected_slot,
            "kind": "readme-missing",
            "file": readme_rel,
            "detail": "README.md is missing",
        })
        return drifts

    text = L.read(readme_full)
    head = "\n".join(text.splitlines()[:HEADER_LINES])
    is_draft = (status == "draft")

    if slot and slot not in head:
        drifts.append({
            "slot": expected_slot,
            "kind": "readme-slot",
            "file": readme_rel,
            "detail": "README header does not name slot %r" % slot,
        })

    mu = UNIT_RX.search(head)
    if mu:
        if mu.group(1) != unit:
            drifts.append({
                "slot": expected_slot,
                "kind": "readme-unit",
                "file": readme_rel,
                "detail": "README unit=%r differs from MANIFEST unit=%r" % (mu.group(1), unit),
            })
    elif not is_draft:
        drifts.append({
            "slot": expected_slot,
            "kind": "readme-unit-missing",
            "file": readme_rel,
            "detail": "README header missing Unit metadata (MANIFEST unit=%r)" % unit,
        })

    mo = ORDER_RX.search(head)
    if mo and isinstance(order, int):
        if int(mo.group(1)) != order:
            drifts.append({
                "slot": expected_slot,
                "kind": "readme-order",
                "file": readme_rel,
                "detail": "README order=%s differs from MANIFEST order=%s" % (mo.group(1), order),
            })

    ms = STATUS_RX.search(head)
    if ms:
        rstatus = ms.group(1).lower()
        if rstatus != status.lower():
            drifts.append({
                "slot": expected_slot,
                "kind": "readme-status",
                "file": readme_rel,
                "detail": "README status=%r differs from MANIFEST status=%r" % (rstatus, status),
            })
    elif not is_draft:
        drifts.append({
            "slot": expected_slot,
            "kind": "readme-status-missing",
            "file": readme_rel,
            "detail": "README header missing status metadata (MANIFEST status=%r)" % status,
        })

    return drifts


def scan(root):
    """Scan all src/VP-V/modules/<unit>/<slot>/ folders with MANIFEST.json. Returns (modules_checked, drifts)."""
    modules_dir = os.path.join(root, "src", "VP-V", "modules")
    if not os.path.isdir(modules_dir):
        return 0, []
    checked = 0
    drifts = []
    for unit in sorted(os.listdir(modules_dir)):
        udir = os.path.join(modules_dir, unit)
        if not os.path.isdir(udir):
            continue
        for slot in sorted(os.listdir(udir)):
            sdir = os.path.join(udir, slot)
            if not os.path.isdir(sdir):
                continue
            if not os.path.isfile(os.path.join(sdir, "MANIFEST.json")):
                continue
            checked += 1
            drifts.extend(scan_module(root, unit, slot, sdir))
    drifts.sort(key=lambda d: (d["slot"], d["kind"], d["detail"]))
    return checked, drifts


def evaluate(drifts, baseline):
    """Return (new_drifts, baselined_drifts, shrunk_keys) against shrink-only baseline."""
    base_keys = set((baseline or {}).get("drifts", []))
    current_by_key = {drift_key(d): d for d in drifts}
    new_drifts = [current_by_key[k] for k in sorted(current_by_key) if k not in base_keys]
    baselined_drifts = [current_by_key[k] for k in sorted(current_by_key) if k in base_keys]
    shrunk_keys = sorted(base_keys - set(current_by_key))
    return new_drifts, baselined_drifts, shrunk_keys


def build_baseline(drifts):
    keys = sorted({drift_key(d) for d in drifts})
    return {
        "tool": "N8 readme-vs-code",
        "total": len(keys),
        "drifts": keys,
    }


def selftest_failures():
    """Names of the failed selftest checks (empty when all pass)."""
    failed = []

    def check(name, cond):
        if not cond:
            failed.append(name)

    root = tempfile.mkdtemp()
    try:
        def put(rel, text):
            path = os.path.join(root, *rel.split("/"))
            os.makedirs(os.path.dirname(path), exist_ok=True)
            with open(path, "w", encoding="utf-8", newline="\n") as fh:
                fh.write(text)

        def write_mod(unit, slot, order, status, live_files, readme_text):
            man = {
                "schema": "vp-module-manifest/1",
                "slot": "%s.%s" % (unit, slot),
                "unit": unit,
                "order": order,
                "status": status,
                "live_files": live_files,
            }
            put("src/VP-V/modules/%s/%s/MANIFEST.json" % (unit, slot), json.dumps(man, indent=2) + "\n")
            if readme_text is not None:
                put("src/VP-V/modules/%s/%s/README.md" % (unit, slot), readme_text)

        # 1. Clean built, placeholder, and draft modules pass with exit 0.
        put("src/VP-V/modules/input/alpha/Alpha.h", "#pragma once\n")
        put("src/VP-V/modules/input/alpha/Alpha.cpp", "// impl\n")
        put("shaders/alpha.glsl", "// shader\n")
        write_mod(
            "input", "alpha", 10, "built",
            ["src/VP-V/modules/input/alpha/Alpha.h", "src/VP-V/modules/input/alpha/Alpha.cpp", "shaders/alpha.glsl"],
            "# Alpha (`input.alpha`)\n\nUnit: **input** | side: input | order: 10 | status: **built** | hook point: HOST\n",
        )
        write_mod(
            "colour", "beta", 85, "placeholder", [],
            "# Beta (`colour.beta`)\n\nUnit: **colour** | side: input | order: 85 | status: **placeholder** | hook point: HOST\n",
        )
        write_mod(
            "repair", "gamma", 90, "draft", [],
            "# Gamma (`repair.gamma`, draft)\n\nDraft owner module.\n",
        )
        check("clean synthetic modules exit 0", main(["--root", root, "--strict"]) == 0)

        # 2. Missing README.md fails.
        write_mod("input", "noreadme", 15, "placeholder", [], None)
        _, d = scan(root)
        check("missing README caught", any(x["slot"] == "input.noreadme" and x["kind"] == "readme-missing" for x in d))
        check("missing README exits 1", main(["--root", root, "--strict"]) == 1)
        shutil.rmtree(os.path.join(root, "src", "VP-V", "modules", "input", "noreadme"))

        # 3. README claims built while MANIFEST says placeholder (and vice versa).
        write_mod(
            "colour", "beta", 85, "placeholder", [],
            "# Beta (`colour.beta`)\n\nUnit: **colour** | order: 85 | status: **built**\n",
        )
        check("README built vs MANIFEST placeholder exits 1", main(["--root", root, "--strict"]) == 1)
        write_mod(
            "colour", "beta", 85, "built", [],
            "# Beta (`colour.beta`)\n\nUnit: **colour** | order: 85 | status: **placeholder**\n",
        )
        check("README placeholder vs MANIFEST built exits 1", main(["--root", root, "--strict"]) == 1)

        # 4. Unit and order mismatches fail.
        write_mod(
            "colour", "beta", 85, "placeholder", [],
            "# Beta (`colour.beta`)\n\nUnit: **input** | order: 99 | status: **placeholder**\n",
        )
        _, d = scan(root)
        kinds = {x["kind"] for x in d if x["slot"] == "colour.beta"}
        check("unit and order mismatches caught", {"readme-unit", "readme-order"}.issubset(kinds))

        # 5. Missing slot in README header fails.
        write_mod(
            "colour", "beta", 85, "placeholder", [],
            "# Beta\n\nUnit: **colour** | order: 85 | status: **placeholder**\n",
        )
        _, d = scan(root)
        check("missing slot in header caught", any(x["kind"] == "readme-slot" for x in d))

        # 6. Missing source file in live_files (*.h, *.cpp, *.glsl) fails.
        write_mod(
            "colour", "beta", 85, "placeholder",
            ["src/VP-V/modules/colour/beta/Missing.cpp", "shaders/missing.glsl"],
            "# Beta (`colour.beta`)\n\nUnit: **colour** | order: 85 | status: **placeholder**\n",
        )
        _, d = scan(root)
        missing_srcs = [x for x in d if x["slot"] == "colour.beta" and x["kind"] == "live-file-missing"]
        check("missing live_files *.cpp and *.glsl caught", len(missing_srcs) == 2)

        # 7. Shrink-only baseline: seed, refuse growth, accept shrink.
        check("--write-baseline seeds baseline", main(["--root", root, "--write-baseline"]) == 0)
        check("seeded baseline exits 0", main(["--root", root, "--strict"]) == 0)
        write_mod(
            "colour", "beta", 85, "placeholder",
            ["src/VP-V/modules/colour/beta/Missing.cpp", "shaders/missing.glsl", "src/VP-V/modules/colour/beta/Extra.h"],
            "# Beta (`colour.beta`)\n\nUnit: **colour** | order: 85 | status: **placeholder**\n",
        )
        check("new drift beyond baseline exits 1", main(["--root", root, "--strict"]) == 1)
        check("--write-baseline refuses growth", main(["--root", root, "--write-baseline"]) == 1)
        write_mod(
            "colour", "beta", 85, "placeholder", [],
            "# Beta (`colour.beta`)\n\nUnit: **colour** | order: 85 | status: **placeholder**\n",
        )
        check("--write-baseline accepts shrink", main(["--root", root, "--write-baseline"]) == 0)
        check("shrunk baseline total is 0", L.load_json(os.path.join(root, BASELINE))["total"] == 0)

        # 8. Missing explicit --baseline exits 2.
        check("missing explicit baseline exits 2", main(["--root", root, "--baseline", os.path.join(root, "nope.json")]) == 2)
    finally:
        shutil.rmtree(root, True)
    return failed


def main(argv=None):
    ap = argparse.ArgumentParser(description="N8 README-vs-code consistency checker")
    ap.add_argument("--root", default=L.repo_root())
    ap.add_argument("--baseline", help="path to shrink-only baseline JSON")
    ap.add_argument("--write-baseline", action="store_true", help="write baseline; refused if any new drift appeared")
    ap.add_argument("--json", action="store_true")
    ap.add_argument("--strict", action="store_true", help="accepted for gate uniformity (always strict)")
    ap.add_argument("--selftest", action="store_true")
    a = ap.parse_args(argv)
    if a.selftest:
        failed = selftest_failures()
        for name in failed:
            print("selftest FAILED check: " + name)
        return L.selftest_ok("readme vs code", not failed)

    root = os.path.abspath(a.root)
    bpath = a.baseline or os.path.join(root, *BASELINE.split("/"))
    if a.baseline and not a.write_baseline and not os.path.isfile(bpath):
        print("N8 readme-vs-code: error: baseline not found at %s" % bpath)
        return 2

    base = L.load_json(bpath) if os.path.isfile(bpath) else None
    if os.path.isfile(bpath) and base is None:
        print("N8 readme-vs-code: error: invalid JSON baseline at %s" % bpath)
        return 2

    checked, drifts = scan(root)
    if a.write_baseline:
        if base is not None:
            new_drifts, _, _ = evaluate(drifts, base)
            if new_drifts:
                print("N8 readme-vs-code: refusing to write baseline with %d new drift(s)" % len(new_drifts))
                return 1
        data = build_baseline(drifts)
        L.save_json(bpath, data)
        print("N8 readme-vs-code: baseline written (%d drift(s) across %d module(s)) -> %s" % (
            data["total"], checked, bpath))
        return 0

    new_drifts, baselined_drifts, shrunk_keys = evaluate(drifts, base or {})
    print("N8 readme-vs-code: %d module(s), %d drift(s) (%d baselined, %d new, %d shrunk)" % (
        checked, len(drifts), len(baselined_drifts), len(new_drifts), len(shrunk_keys)))
    for d in baselined_drifts:
        print("  BASELINE %s [%s] %s: %s" % (d["slot"], d["kind"], d["file"], d["detail"]))
    for d in new_drifts:
        print("  DRIFT %s [%s] %s: %s" % (d["slot"], d["kind"], d["file"], d["detail"]))
    for k in shrunk_keys:
        print("  SHRUNK (may shrink baseline): %s" % k)
    if a.json:
        print(json.dumps({
            "modules": checked,
            "drifts": drifts,
            "baselined": baselined_drifts,
            "new": new_drifts,
            "shrunk": shrunk_keys,
        }, indent=1, sort_keys=True))
    if new_drifts:
        print("FAIL: module README.md and MANIFEST.json / live_files disagree; reconcile the README or MANIFEST.")
    return 1 if new_drifts else 0


if __name__ == "__main__":
    sys.exit(main())
