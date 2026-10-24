#!/usr/bin/env python3
"""Drift check for the module folder tree (ADR-0017).

Checks that docs/design/slots.json, every src/VP-V/modules/<unit>/<slot>/MANIFEST.json and the
master component list docs/design/components-master.json agree, that every live file exists,
and that no source file exists both inside src/VP-V/modules and in a legacy location (no duplicate
source during the migration). A moved header may leave a pure forwarding header at its legacy
path while other active branches still include that path; it must be listed in its module's
"forwarding_headers" and is removed in M2 (docs/design/migration-early-inventory.md).

  python tools/check_module_manifests.py          # check; exit 1 on any mismatch
  python tools/check_module_manifests.py --write  # regenerate the master list from the manifests

Stdlib only; run from anywhere.
"""
import json
import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SLOTS = os.path.join(ROOT, "docs", "design", "slots.json")
MASTER = os.path.join(ROOT, "docs", "design", "components-master.json")
MODULES = os.path.join(ROOT, "src", "VP-V", "modules")
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


FORWARDER_MARKER = "VP-V forwarding header"


def forwarder_problem(path, target):
    """Why a legacy-path forwarder is not a pure forwarder to the moved file (None when it is)."""
    if target is None:
        return "no moved file of that name under src/VP-V/modules"
    with open(path, encoding="utf-8") as f:
        text = f.read()
    if FORWARDER_MARKER not in text:
        return f"missing the marker '{FORWARDER_MARKER}'"
    want = os.path.relpath(target, os.path.join(ROOT, "src")).replace(os.sep, "/")
    code = []
    for line in text.splitlines():
        s = line.strip()
        if s and not s.startswith("//"):
            code.append(s)
    if code != ["#pragma once", f"#include <{want}>"]:
        return f"must contain only '#pragma once' and '#include <{want}>' (plus comments)"
    return None


def check(write):
    errors = []
    slots = load(SLOTS)["slots"]
    ids = [s["id"] for s in slots]
    if len(ids) != len(set(ids)):
        errors.append("slots.json: duplicate slot ids")
    orders = [s["order"] for s in slots]
    if len(orders) != len(set(orders)):
        errors.append("slots.json: duplicate order values")

    # The renderer's slot table (PipelineSlots.h) mirrors slots.json: same ids, order, status, origin.
    header = os.path.join(ROOT, "src", "VP-V", "core", "PipelineSlots.h")
    if os.path.exists(header):
        pattern = re.compile(r'\{\s*Id::\w+,\s*"([\w.]+)",\s*Side::\w+,\s*(\d+),\s*HookPoint::\w+,\s*Status::(\w+),\s*Origin::(\w+)\s*\}')
        with open(header, encoding="utf-8") as f:
            table = {m.group(1): (int(m.group(2)), m.group(3).lower(), m.group(4).lower())
                     for m in pattern.finditer(f.read())}
        want = {s["id"]: (s["order"], s["status"], s.get("origin")) for s in slots}
        if table != want:
            for sid in sorted(set(table) | set(want)):
                if table.get(sid) != want.get(sid):
                    errors.append(f"PipelineSlots.h vs slots.json: {sid}: {table.get(sid)} != {want.get(sid)}")

    # Zones (ADR-0021) must agree with the header's hook points.
    zone_of = {"Capture": "host", "Host": "host", "Prepass": "front_end", "Libplacebo": "libplacebo",
               "PreOutput": "output_hooks", "Output": "quantise", "Present": "present"}
    if os.path.exists(header):
        hp = re.compile(r'\{\s*Id::\w+,\s*"([\w.]+)",\s*Side::\w+,\s*\d+,\s*HookPoint::(\w+),')
        with open(header, encoding="utf-8") as f:
            header_zone = {m.group(1): zone_of[m.group(2)] for m in hp.finditer(f.read())}
        for s in slots:
            if "zone" in s and header_zone.get(s["id"]) != s["zone"]:
                errors.append(f"{s['id']}: zone {s['zone']} in slots.json, {header_zone.get(s['id'])} in PipelineSlots.h")

    # The Original pathway (ADR-0019): one list (core/OriginalPathway.h), the renderer sets exactly
    # those keys ("// key=" comments in ApplyOriginalPathwaySettings), and slots.json names no other.
    op_header = os.path.join(ROOT, "src", "VP-V", "core", "OriginalPathway.h")
    renderer = os.path.join(ROOT, "src", "VideoProcessor-Lib", "vprenderer", "LibplaceboVideoRenderer.cpp")
    if os.path.exists(op_header) and os.path.exists(renderer):
        with open(op_header, encoding="utf-8") as f:
            entries = re.findall(r'\{\s*"([\w.]+)",\s*"([^"]*)",\s*"([\w.]+)"\s*\}', f.read())
        core_keys = {k for k, _, _ in entries}
        for k, _, slot in entries:
            if slot not in ids:
                errors.append(f"OriginalPathway.h: {k} names unknown slot {slot}")
        with open(renderer, encoding="utf-8") as f:
            text = f.read()
        start = text.find("static void ApplyOriginalPathwaySettings")
        end = text.find("\n\t}\n", start)
        body = text[start:end] if start >= 0 and end > start else ""
        renderer_keys = set(re.findall(r"//\s*key=([\w.]+)", body))
        if not body:
            errors.append("ApplyOriginalPathwaySettings not found in the renderer")
        for k in sorted(core_keys - renderer_keys):
            errors.append(f"Original pathway: {k} is in OriginalPathway.h but the renderer does not set it")
        for k in sorted(renderer_keys - core_keys):
            errors.append(f"Original pathway: the renderer sets {k}, which OriginalPathway.h does not list")
        for slot_entry in slots:
            for k in slot_entry.get("original_pathway", {}).get("settings", {}):
                if k.startswith("["):
                    continue   # a non-renderer setting (the LED drive's own section)
                if k not in core_keys:
                    errors.append(f"slots.json {slot_entry['id']}: original_pathway key {k} is not in OriginalPathway.h")

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
                    errors.append(f"src/VP-V/modules/{unit}/{slot}: no slot in slots.json")

    # No duplicate source: a source file name inside src/VP-V/modules must not also exist elsewhere.
    moved = {}
    for dirpath, _, files in os.walk(MODULES):
        for name in files:
            if os.path.splitext(name)[1].lower() in SOURCE_EXT:
                moved[name.lower()] = os.path.join(dirpath, name)
    # Forwarding headers (early migrations, docs/design/migration-early-inventory.md): a moved
    # header may leave a one-line forwarder at its legacy path, so files on other active branches
    # that still include the old path build unchanged. Each one is listed in its module's
    # "forwarding_headers", carries FORWARDER_MARKER, contains nothing but #pragma once and one
    # #include of the moved file, and is removed in M2. Anything else with a moved name is a copy.
    forwarders = {}
    for m in manifests:
        for f in m.get("forwarding_headers", []):
            forwarders[os.path.normcase(os.path.abspath(os.path.join(ROOT, f)))] = (m["slot"], f)
    for path, (slot, rel) in sorted(forwarders.items()):
        name = os.path.basename(path).lower()
        if not os.path.exists(path):
            errors.append(f"{slot}: forwarding header missing: {rel}")
            continue
        problem = forwarder_problem(path, moved.get(name))
        if problem:
            errors.append(f"{slot}: forwarding header {rel}: {problem}")
    if moved:
        for base in (os.path.join(ROOT, "src"), os.path.join(ROOT, "shaders")):
            for dirpath, dirnames, files in os.walk(base):
                if os.path.abspath(dirpath).startswith(os.path.abspath(MODULES)):
                    dirnames[:] = []
                    continue
                dirnames[:] = [d for d in dirnames if d not in ("x64", "Debug", "Release", ".vs")]
                for name in files:
                    if name.lower() in moved:
                        full = os.path.normcase(os.path.abspath(os.path.join(dirpath, name)))
                        if full in forwarders:
                            continue   # checked above
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
