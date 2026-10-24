#!/usr/bin/env python3
"""Drift check for the module folder tree (ADR-0017).

Checks that docs/design/slots.json, every src/modules/<unit>/<slot>/MANIFEST.json and the
master component list docs/design/components-master.json agree, that every live file exists,
and that no source file exists both inside src/modules and in a legacy location (no duplicate
source during the migration).

  python tools/check_module_manifests.py          # check; exit 1 on any mismatch
  python tools/check_module_manifests.py --write  # regenerate the master list from the manifests

Stdlib only; run from anywhere.
"""
import json
import os
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SLOTS = os.path.join(ROOT, "docs", "design", "slots.json")
MASTER = os.path.join(ROOT, "docs", "design", "components-master.json")
MODULES = os.path.join(ROOT, "src", "modules")
SOURCE_EXT = {".cpp", ".h", ".hpp", ".c", ".glsl", ".hlsl"}


def load(path):
    with open(path, encoding="utf-8") as f:
        return json.load(f)


def module_dir(slot_id):
    unit, slot = slot_id.split(".", 1)
    return os.path.join(MODULES, unit, slot)


def build_master(manifests):
    """The master list is a pure function of the manifests, so it can always be regenerated."""
    return {
        "schema": "vp-components-master/1",
        "generated_by": "tools/check_module_manifests.py --write",
        "modules": [
            {
                "slot": m["slot"],
                "order": m["order"],
                "status": m["status"],
                "code_location": m["code_location"],
                "live_files": m["live_files"],
                "components": m["components"],
                "fallback_when_missing": m["fallback_when_missing"],
            }
            for m in sorted(manifests, key=lambda m: m["order"])
        ],
    }


def check(write):
    errors = []
    slots = load(SLOTS)["slots"]
    ids = [s["id"] for s in slots]
    if len(ids) != len(set(ids)):
        errors.append("slots.json: duplicate slot ids")
    orders = [s["order"] for s in slots]
    if len(orders) != len(set(orders)):
        errors.append("slots.json: duplicate order values")

    manifests = []
    for s in slots:
        folder = module_dir(s["id"])
        for required in ("README.md", "MANIFEST.json", "tests"):
            if not os.path.exists(os.path.join(folder, required)):
                errors.append(f"{s['id']}: missing {required}")
        path = os.path.join(folder, "MANIFEST.json")
        if not os.path.exists(path):
            continue
        m = load(path)
        manifests.append(m)
        for key in ("slot", "order", "status"):
            want = s["id"] if key == "slot" else s[key]
            if m.get(key) != want:
                errors.append(f"{s['id']}: manifest {key}={m.get(key)!r}, slots.json has {want!r}")
        keys = sorted(set(s["settings_keys"] + s["planned_keys"]))
        if sorted(m["components"]["config_keys"]) != keys:
            errors.append(f"{s['id']}: manifest config_keys differ from slots.json settings_keys + planned_keys")
        if sorted(m["components"]["upstream_modules"]) != sorted(s["upstream"]):
            errors.append(f"{s['id']}: manifest upstream_modules differ from slots.json upstream")
        for up in s["upstream"]:
            if up not in ids:
                errors.append(f"{s['id']}: unknown upstream {up}")
        for f in m["live_files"]:
            if not os.path.exists(os.path.join(ROOT, f)):
                errors.append(f"{s['id']}: live file missing: {f}")

    # No stray module folders without a slot.
    if os.path.isdir(MODULES):
        for unit in os.listdir(MODULES):
            unit_dir = os.path.join(MODULES, unit)
            if not os.path.isdir(unit_dir):
                continue
            for slot in os.listdir(unit_dir):
                if os.path.isdir(os.path.join(unit_dir, slot)) and f"{unit}.{slot}" not in ids:
                    errors.append(f"src/modules/{unit}/{slot}: no slot in slots.json")

    # No duplicate source: a source file name inside src/modules must not also exist elsewhere.
    moved = {}
    for dirpath, _, files in os.walk(MODULES):
        for name in files:
            if os.path.splitext(name)[1].lower() in SOURCE_EXT:
                moved[name.lower()] = os.path.join(dirpath, name)
    if moved:
        for base in (os.path.join(ROOT, "src"), os.path.join(ROOT, "shaders")):
            for dirpath, dirnames, files in os.walk(base):
                if os.path.abspath(dirpath).startswith(os.path.abspath(MODULES)):
                    dirnames[:] = []
                    continue
                dirnames[:] = [d for d in dirnames if d not in ("x64", "Debug", "Release", ".vs")]
                for name in files:
                    if name.lower() in moved:
                        errors.append(f"duplicate source: {os.path.relpath(os.path.join(dirpath, name), ROOT)} "
                                      f"and {os.path.relpath(moved[name.lower()], ROOT)}")

    master = build_master(manifests)
    if write:
        with open(MASTER, "w", encoding="utf-8", newline="\n") as f:
            json.dump(master, f, indent=2, ensure_ascii=False)
            f.write("\n")
        print(f"wrote {os.path.relpath(MASTER, ROOT)} ({len(manifests)} modules)")
    elif not os.path.exists(MASTER):
        errors.append("components-master.json missing (run with --write)")
    elif load(MASTER) != master:
        errors.append("components-master.json is out of date with the manifests (run with --write)")

    for e in errors:
        print("DRIFT:", e)
    print(f"{len(slots)} slots, {len(manifests)} manifests, {len(errors)} problems")
    return 1 if errors else 0


if __name__ == "__main__":
    sys.exit(check("--write" in sys.argv[1:]))
