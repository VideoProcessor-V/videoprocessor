#!/usr/bin/env python
# Copyright(C) 2026 VP-V contributors
# SPDX-License-Identifier: GPL-3.0-only
"""Generates tests/vpv/settings-identity-corpus.txt, the corpus C of RendererSettingsIdentityTests (M2 P3-0).

One case per (renderer key, probe value) for every key the profile validator
(RendererProfileConfig.h ValidateProfileSettingBuiltin / ValidateBaseSetting) names, plus invalid
values, plus a few whole-file cases (every sample configuration, every key at its first / second
alternative). The key list is read from the source text AT GENERATION TIME and then frozen in the
checked-in corpus, so moving the validators later (P3-7) does not change what the tests probe.

Usage (repository root):  python -I tools/gen_settings_identity_corpus.py [--check]
Format of the corpus: blocks '### <case name>' followed by the configuration text.
"""
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
SOURCES = [
    ROOT / "src" / "VideoProcessor-Lib" / "RendererProfileConfig.h",
    ROOT / "src" / "VP-V" / "modules" / "control" / "config" / "VpvSchemaKeys.h",
]
OUT = ROOT / "tests" / "vpv" / "settings-identity-corpus.txt"
SAMPLES = [
    "VideoProcessor.cfg",
    "docs/cinema/VideoProcessor-test.cfg",
    "docs/cinema/led-profile-example.cfg",
    "docs/examples/VideoProcessorRenderer.from-legacy.proposed.cfg",
    "docs/examples/VideoProcessorRenderer.legacy.cfg",
    "docs/examples/VideoProcessorRenderer.unified.minimal.proposed.cfg",
    "docs/examples/VideoProcessorRenderer.unified.proposed.cfg",
    "test-fixtures/deployed-VideoProcessor-20260807-current.cfg",
    "test-fixtures/deployed-VideoProcessor-20260807.cfg",
    "test-fixtures/deployed-VideoProcessor-save-simulation.cfg",
]
# Keys that are structural (sections, shortcuts, paths) and would only add noise or touch the disk.
SKIP = {"type", "label", "when", "priority", "shortcut", "cycle_shortcut"}
KEY = re.compile(r'key == "([a-z0-9_.\-]+)"')
BASE = "[general]\npersist_profile_selection: false\n"


def statement_after(text, pos):
    ret = text.find("return", pos)
    if ret < 0:
        return text[pos:pos + 200]
    end = text.find(";", ret)
    return text[pos:end + 1 if end > 0 else ret + 200]


def values_for(statement):
    vals = []
    m = re.search(r"IsChoice\(value,\s*\{([^}]*)\}", statement)
    if m:
        vals += re.findall(r'"([^"]*)"', m.group(1))
    for m in re.finditer(r"(?:IsNumberInRange|IsAspectInRange)\(value,\s*([-0-9.e+a-zA-Z_:()<>]+),\s*([-0-9.e+a-zA-Z_:()<>]+)", statement):
        try:
            lo, hi = float(m.group(1)), float(m.group(2))
        except ValueError:
            continue
        vals += [repr(lo), repr(hi), repr((lo + hi) / 2), repr(hi + abs(hi) + 1)]
    for m in re.finditer(r"ParseInteger\(value,\s*(-?\d+),\s*(-?\d+)", statement):
        lo, hi = int(m.group(1)), int(m.group(2))
        vals += [str(lo), str(hi), str((lo + hi) // 2), str(hi + 1)]
    if "IsBoolean(" in statement:
        vals += ["on", "off", "true", "false", "1", "0"]
    if not vals:
        vals += ["on", "off", "auto", "1", "0", "2.5", "10", "100"]
    seen, out = set(), []
    for v in vals:
        if v not in seen:
            seen.add(v)
            out.append(v)
    return out[:10]


def collect():
    keys = {}
    for src in SOURCES:
        if not src.exists():
            continue
        text = src.read_text(encoding="utf-8", errors="replace")
        for m in KEY.finditer(text):
            key = m.group(1)
            if key in SKIP or key.startswith(("subtitle_", "led_", "ambient_")):
                continue
            vals = values_for(statement_after(text, m.end()))
            old = keys.get(key, [])
            keys[key] = old + [v for v in vals if v not in old]
    # The [vpv] keys (VpvKeys list) and the tier table keys are read by the loader too.
    for src in SOURCES:
        if not src.exists():
            continue
        text = src.read_text(encoding="utf-8", errors="replace")
        at = text.find("vpvKeys[] = {")
        if at >= 0:
            body = text[at:text.find("};", at)]
            for key in re.findall(r'"([a-z0-9_]+)"', body):
                keys.setdefault(key, ["on", "off", "auto", "1", "0"])
    # Keys the renderer's loader reads (LibplaceboVideoRenderer.cpp, VpvRendererSettings.cpp): the reads that
    # use rule.section, the readChoice/readToggle/tryChoice/readTone/readSeconds helpers and the flat reads.
    loaders = [ROOT / "src" / "VideoProcessor-Lib" / "vprenderer" / "LibplaceboVideoRenderer.cpp",
               ROOT / "src" / "VP-V" / "modules" / "control" / "config" / "VpvRendererSettings.cpp"]
    pat = re.compile(r'(?:rule\.section,\s*|readChoice\(\s*|readToggle\(\s*|tryChoice\(\s*|readTone\(\s*|readSeconds\(\s*|'
                     r'TryGetPipelineString\(config,\s*|TryGetDisplayString\(config,\s*|TryGetDisplayBool\(config,\s*|'
                     r'ReadChoice\(\s*config,\s*|readViewportString\(\s*)"([a-z0-9_]+)"')
    for src in loaders:
        if src.exists():
            for key in set(pat.findall(src.read_text(encoding="utf-8", errors="replace"))):
                if key in SKIP or key.startswith(("subtitle_", "led_", "ambient_")):
                    continue
                keys.setdefault(key, [])
    for key in list(keys):
        if not keys[key]:
            keys[key] = ["on", "off", "auto", "1", "0", "2.5", "10", "100"]
    tier = ROOT / "src" / "VP-V" / "config" / "VpvTierTable.generated.h"
    if tier.exists():
        for key in set(re.findall(r'\{"([a-z0-9_]+)",\s*"', tier.read_text(encoding="utf-8", errors="replace"))):
            keys.setdefault(key, ["on", "off", "auto", "1", "0"])
    return keys


def case(name, profile_lines, vpv_lines=None):
    text = BASE + "[vprenderer.Default]\nquality: high\n" + "".join(profile_lines)
    text += "[vpv]\n" + "".join(vpv_lines if vpv_lines is not None else profile_lines)
    return "### " + name + "\n" + text


def main():
    keys = collect()
    out = ["# settings identity corpus (tools/gen_settings_identity_corpus.py); frozen; regenerate only with review\n"]
    out.append("### empty\n" + BASE + "[vprenderer.Default]\nquality: high\n")
    for rel in SAMPLES:
        p = ROOT / rel
        if p.exists():
            out.append("### sample " + rel + "\n" + p.read_text(encoding="utf-8", errors="replace").replace("\r\n", "\n").rstrip("\n") + "\n")
    names = sorted(keys)
    for key in names:
        vals = keys[key]
        for v in vals:
            out.append(case("%s=%s" % (key, v), ["%s: %s\n" % (key, v)]))
        out.append(case("%s=zzz_invalid" % key, ["%s: zzz_invalid\n" % key]))
    # whole-set cases: every key at its n-th probe value
    for n in (0, 1, 2, 3):
        lines = ["%s: %s\n" % (k, keys[k][min(n, len(keys[k]) - 1)]) for k in names]
        out.append(case("all-keys-probe-%d" % n, lines))
    text = "\n".join(s.rstrip("\n") + "\n" for s in out)
    if "--check" in sys.argv:
        old = OUT.read_text(encoding="utf-8") if OUT.exists() else ""
        sys.exit(0 if old == text else 1)
    OUT.parent.mkdir(parents=True, exist_ok=True)
    OUT.write_text(text, encoding="utf-8", newline="\n")
    print("wrote %s: %d keys, %d cases" % (OUT, len(names), text.count("\n### ") + 1))


if __name__ == "__main__":
    main()
