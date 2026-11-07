# Copyright(C) 2026 VP-V contributors
# SPDX-License-Identifier: GPL-3.0-only
"""Shared parsing for the master settings list (W1-MS).

Used by tools/gen_master_settings.py and tools/check_master_settings.py.
Everything takes a repository root so the unit tests can run it on fixtures.

Key identity ("key id"): the bare key name, except for keys of a namespaced
VP-V section ([vpv.<name>]), which are "vpv.<name>.<key>" (as in the module
MANIFEST config_keys).
"""
import glob
import json
import os
import re

UNIT_ORDER = ["input", "motion", "repair", "scaling", "colour", "drive", "output", "control"]
UNIT_TITLE = {
    "input": "Input", "motion": "Motion", "repair": "Repair", "scaling": "Scaling",
    "colour": "Colour", "drive": "Dynamic drive", "output": "Output", "control": "Control",
}

# Sources of keys that exist in code. Bill's key lists live in the first group;
# the VP-V field tables (either layout) in the second.
SCHEMA_FILES = [
    "src/VideoProcessor-Lib/RendererProfileConfig.h",
    "src/VideoProcessor-Lib/MainConfigSchema.h",
    "src/VideoProcessor-Lib/ConfigSchema.h",
]
FIELD_TABLE_GLOBS = [
    "src/VP-V/modules/control/config/VpvRenderer*.h",
    "src/VP-V/modules/control/config/VpvRenderer*.cpp",
    "src/VP-V/**/VpvRendererFields.h",
]
SCAN_SKIP_DIRS = {"tests", "x64", "3rdparty"}

UPSTREAM_REL = "docs/settings/UPSTREAM-KEYS.txt"
GOLDEN_REL = "src/VP-V/tests/golden/cpu-defaults.golden.txt"
TIERS_REL = "docs/design/vpv-tiers.json"
COMPONENTS_REL = "docs/design/components-master.json"
MASTER_REL = "docs/settings/MASTER-SETTINGS.md"

HEADER_COLS = ["Key", "Section", "Type", "Range / values", "Default", "Config placement",
               "Original (Bill)", "Fallbacks / status", "Tiers"]
VALID_TYPES = {"bool", "int", "float", "enum", "string"}
KEY_RE = r"[A-Za-z0-9_.]+"

_NAME = re.compile(r'"(' + KEY_RE + r')"')


def read_text(path):
    with open(path, encoding="utf-8-sig", errors="replace") as fh:
        return fh.read()


def strip_comments(text):
    text = re.sub(r"/\*.*?\*/", "", text, flags=re.S)
    return re.sub(r"//[^\n]*", "", text)


# ---------------------------------------------------------------- code keys

def extract_schema_keys(text):
    """Bare keys named by the validators / schema entries of Bill-style schema files."""
    t = strip_comments(text)
    keys = set(re.findall(r'\bkey\s*==\s*"(' + KEY_RE + r')"', t))
    keys |= set(re.findall(r'ConfigSchema::\w+\(\s*"(' + KEY_RE + r')"', t))
    for m in re.finditer(r'(?:vpvKeys\s*\[\s*\]\s*=|for\s*\(\s*const\s+char\s*\*\s*key\s*:)\s*\{([^}]*)\}', t):
        keys |= set(_NAME.findall(m.group(1)))
    return keys


def extract_field_table_keys(text):
    """Keys of a VP-V settings field table: validators plus `{ "key", Kind::... }` entries."""
    t = strip_comments(text)
    keys = extract_schema_keys(text)
    keys |= set(re.findall(r'\{\s*"([a-z][a-z0-9_]*)"\s*,\s*(?:Vpv\w*|Field\w*|Kind|Type|Setting\w*)::', t))
    return keys


def extract_namespaced_keys(text):
    """Keys of a VP-V namespaced section: `kSection = "vpv.x"` files (SkeletonSettings.h style)."""
    t = strip_comments(text)
    out = set()
    m = re.search(r'kSection\s*=\s*"(vpv\.[A-Za-z0-9_.]+)"', t)
    if not m:
        return out
    sec = m.group(1)
    for arr in re.finditer(r'\bk\w*Keys\s*\[\s*\]\s*=\s*\{([^}]*)\}', t):
        out |= {sec + "." + k for k in _NAME.findall(arr.group(1))}
    out |= {sec + "." + k for k in re.findall(r'TryGet\w+\(\s*kSection\s*,\s*"(' + KEY_RE + r')"', t)}
    return out


def _iter_vpv_sources(root):
    base = os.path.join(root, "src", "VP-V")
    for dp, dns, fns in os.walk(base):
        dns[:] = [d for d in dns if d not in SCAN_SKIP_DIRS]
        for fn in fns:
            if fn.lower().endswith((".h", ".hpp", ".cpp")):
                yield os.path.join(dp, fn)


def extract_code_keys(root):
    """key id -> sorted list of repo-relative source files that name it."""
    found = {}

    def add(keys, rel):
        for k in keys:
            found.setdefault(k, set()).add(rel)

    for rel in SCHEMA_FILES:
        p = os.path.join(root, *rel.split("/"))
        if os.path.isfile(p):
            add(extract_schema_keys(read_text(p)), rel)
    tables = set()
    for pat in FIELD_TABLE_GLOBS:
        tables |= set(glob.glob(os.path.join(root, *pat.split("/")), recursive=True))
    for p in sorted(tables):
        rel = os.path.relpath(p, root).replace(os.sep, "/")
        add(extract_field_table_keys(read_text(p)), rel)
    for p in sorted(_iter_vpv_sources(root)):
        text = read_text(p)
        if "kSection" in text and '"vpv.' in text:
            add(extract_namespaced_keys(text), os.path.relpath(p, root).replace(os.sep, "/"))
    return {k: sorted(v) for k, v in found.items()}


def load_upstream(root):
    p = os.path.join(root, *UPSTREAM_REL.split("/"))
    if not os.path.isfile(p):
        return set()
    return {ln.strip() for ln in read_text(p).splitlines() if ln.strip() and not ln.startswith("#")}


def write_upstream_text(keys, ref):
    head = [
        "# Keys that belong to Bill's (upstream) schema files; the master settings list scopes itself to the others.",
        "# Generated: python -I tools/check_master_settings.py --write-upstream <git-ref>  (reference used: %s)" % ref,
        "# One key per line, sorted. A key Bill adds later shows up as an undocumented gap until it is added here.",
    ]
    return "\n".join(head + sorted(keys)) + "\n"


# ------------------------------------------------------------------ golden

def load_golden(root):
    """Defaults: 'case empty' first, then the '### tier defaults' block for keys it lacks."""
    p = os.path.join(root, *GOLDEN_REL.split("/"))
    out = {}
    if not os.path.isfile(p):
        return out
    block, empty, tiers = None, {}, {}
    for ln in read_text(p).splitlines():
        if ln.startswith("###"):
            block = "empty" if ln.strip() == "### case empty" else ("tiers" if ln.strip() == "### tier defaults" else None)
            continue
        if block is None or "=" not in ln or ln.startswith("#"):
            continue
        k, v = ln.split("=", 1)
        if k.startswith(("fp[", "matcher.")) or k == "rule":
            continue
        (empty if block == "empty" else tiers)[k] = v
    out.update(tiers)
    out.update(empty)
    return out


def load_tiers(root):
    p = os.path.join(root, *TIERS_REL.split("/"))
    if not os.path.isfile(p):
        return {}
    return json.loads(read_text(p))


# ---------------------------------------------------------------- modules

def discover_modules(root):
    """Every module with a MANIFEST.json: slot, unit, submodule, dir, README path, status, keys."""
    comp = {}
    cp = os.path.join(root, *COMPONENTS_REL.split("/"))
    if os.path.isfile(cp):
        for m in json.loads(read_text(cp)).get("modules", []):
            comp[m.get("slot")] = m
    mods = []
    base = os.path.join(root, "src", "VP-V", "modules")
    for dp, dns, fns in os.walk(base):
        dns[:] = [d for d in dns if d not in SCAN_SKIP_DIRS]
        if "MANIFEST.json" not in fns:
            continue
        man = json.loads(read_text(os.path.join(dp, "MANIFEST.json")))
        slot = man.get("slot", "")
        unit = man.get("unit") or (slot.split(".")[0] if slot else "")
        rel = os.path.relpath(dp, root).replace(os.sep, "/")
        readme = rel + "/README.md"
        c = comp.get(slot, {})
        mods.append({
            "slot": slot, "unit": unit,
            "sub": slot.split(".", 1)[1] if "." in slot else slot,
            "dir": rel,
            "readme": readme if os.path.isfile(os.path.join(root, *readme.split("/"))) else None,
            "status": man.get("status") or c.get("status", ""),
            "order": man.get("order", c.get("order", 0)),
            "manifest_keys": sorted(man.get("components", {}).get("config_keys", [])),
            "in_components_master": slot in comp,
        })
    mods.sort(key=lambda m: (unit_rank(m["unit"]), m["order"], m["slot"]))
    return mods


def unit_rank(unit):
    return UNIT_ORDER.index(unit) if unit in UNIT_ORDER else len(UNIT_ORDER)


# ----------------------------------------------------------- README tables

def _split_row(line):
    line = line.strip()
    if line.startswith("|"):
        line = line[1:]
    if line.endswith("|"):
        line = line[:-1]
    return [c.strip() for c in re.split(r"(?<!\\)\|", line)]


def _unquote(cell):
    m = re.fullmatch(r"`([^`]*)`", cell.strip())
    return m.group(1) if m else cell.strip()


def row_key_id(key, section):
    sec = re.search(r"vpv\.[A-Za-z0-9_.]+", section or "")
    if sec and not key.startswith("vpv."):
        return sec.group(0) + "." + key
    return key


def parse_readme_settings(text, readme_rel):
    """-> (rows, errors). Rows come from tables with the template header under any '## Settings...' heading."""
    rows, errors = [], []
    in_settings = False
    lines = text.splitlines()
    i = 0
    fence = False
    while i < len(lines):
        ln = lines[i]
        if ln.lstrip().startswith("```"):
            fence = not fence
            i += 1
            continue
        if fence:
            i += 1
            continue
        if ln.startswith("## "):
            in_settings = ln[3:].strip().lower().startswith("settings")
            i += 1
            continue
        if in_settings and ln.strip().startswith("|"):
            hdr = _split_row(ln)
            if [h.lower() for h in hdr] == [h.lower() for h in HEADER_COLS]:
                i += 1
                if i < len(lines) and re.fullmatch(r"\|?[\s:\-|]+\|?", lines[i].strip()):
                    i += 1
                else:
                    errors.append("%s:%d: header row not followed by a separator row" % (readme_rel, i))
                while i < len(lines) and lines[i].strip().startswith("|"):
                    cells = _split_row(lines[i])
                    if len(cells) != len(HEADER_COLS):
                        errors.append("%s:%d: row has %d cells, template has %d" % (readme_rel, i + 1, len(cells), len(HEADER_COLS)))
                    elif cells[0].strip().lower() in ("(none)", "`(none)`"):
                        pass
                    else:
                        key = _unquote(cells[0])
                        if cells[0] == key or not re.fullmatch(KEY_RE, key):
                            errors.append("%s:%d: key must be one name in backticks, got %r" % (readme_rel, i + 1, cells[0]))
                        else:
                            row = dict(zip(["key", "section", "type", "range", "default", "placement",
                                            "original", "status", "tiers"], cells))
                            row["key"] = key
                            row["id"] = row_key_id(key, row["section"])
                            row["readme"] = readme_rel
                            row["line"] = i + 1
                            if row["type"].strip("` ").lower() not in VALID_TYPES:
                                errors.append("%s:%d: type %r is not one of %s" % (
                                    readme_rel, i + 1, row["type"], "/".join(sorted(VALID_TYPES))))
                            rows.append(row)
                    i += 1
                continue
            if len(hdr) == len(HEADER_COLS) and hdr[0].strip().lower() == "key":   # 9-column near miss = typo; older bullet/3-column tables are legacy
                errors.append("%s:%d: Settings table header differs from the template" % (readme_rel, i + 1))
        i += 1
    return rows, errors


def collect_documented(root, modules):
    """-> (rows, errors); each row also carries slot and unit of its module."""
    rows, errors = [], []
    for m in modules:
        if not m["readme"]:
            continue
        got, errs = parse_readme_settings(read_text(os.path.join(root, *m["readme"].split("/"))), m["readme"])
        for r in got:
            r["slot"], r["unit"] = m["slot"], m["unit"]
        rows += got
        errors += errs
    return rows, errors


def is_planned(row):
    return row["status"].strip().strip("*_ ").lower().startswith(("planned", "declared"))


# --------------------------------------------------------------- defaults

_TRUE = {"on", "true", "1", "yes"}
_FALSE = {"off", "false", "0", "no"}


def norm_default(text):
    t = text.strip().strip("`").strip()
    if t.lower() in _TRUE:
        return "1"
    if t.lower() in _FALSE:
        return "0"
    try:
        return repr(float(t))
    except ValueError:
        return t


def doc_default_literal(text):
    """The documented default when it is one plain token (else None: not comparable)."""
    t = text.strip()
    m = re.fullmatch(r"`([^`]*)`", t)
    if m:
        return m.group(1)
    if re.fullmatch(r"[A-Za-z0-9_.+\-]+", t):
        return t
    return None


# ----------------------------------------------------------------- compare

FL35 = re.compile(r"fl\s*-?\s*35", re.I)


def analyse(root):
    mods = discover_modules(root)
    rows, struct_errors = collect_documented(root, mods)
    code = extract_code_keys(root)
    upstream = load_upstream(root)
    golden = load_golden(root)

    errors = list(struct_errors)
    by_id = {}
    for r in rows:
        by_id.setdefault(r["id"], []).append(r)
    for kid, rs in sorted(by_id.items()):
        if len(rs) > 1:
            errors.append("duplicate row for key %s: %s" % (kid, ", ".join("%s:%d" % (r["readme"], r["line"]) for r in rs)))
    for r in rows:
        if any(FL35.search(v) for v in r.values() if isinstance(v, str)):
            errors.append("%s:%d: row for %s names the owner's display; use generic wording" % (r["readme"], r["line"], r["id"]))

    scope = {k for k in code if k not in upstream}
    documented = set(by_id)
    gaps = {
        "undocumented": sorted(scope - documented),
        "stale": sorted(k for k, rs in by_id.items()
                        if k not in code and not all(is_planned(r) for r in rs)),
        "default_mismatch": [],
    }
    for k, rs in sorted(by_id.items()):
        if k in golden and (k in code or not is_planned(rs[0])):
            lit = doc_default_literal(rs[0]["default"])
            if lit is not None and norm_default(lit) != norm_default(golden[k]):
                gaps["default_mismatch"].append(k)
    return {
        "modules": mods, "rows": rows, "by_id": by_id, "code": code, "upstream": upstream,
        "scope": scope, "golden": golden, "errors": errors, "gaps": gaps,
    }


# --------------------------------------------------------------- rendering

def _cell(text):
    return (text or "").replace("|", "\\|").replace("\n", " ").strip()


def _tier_hint(tiers, key):
    """VpvSection placement hint from vpv-tiers.json: unit tab + toggle label."""
    for u in tiers.get("units", []):
        for t in u.get("toggles", []):
            if t.get("key") == key:
                return "VP-V > %s > %s" % (u.get("name", u.get("id", "")), t.get("label", key))
        for t in u.get("planned", []):
            if t.get("key") == key:
                return "VP-V > %s > %s (planned)" % (u.get("name", u.get("id", "")), t.get("label", key))
    return ""


def render_master(root, an=None):
    an = an or analyse(root)
    tiers = load_tiers(root)
    mods, rows, by_id = an["modules"], an["rows"], an["by_id"]
    golden = an["golden"]
    out = []
    w = out.append
    w("<!-- GENERATED by tools/gen_master_settings.py - do not edit by hand. -->")
    w("<!-- Edit the module README '## Settings' tables (template: docs/settings/README.md), then regenerate. -->")
    w("")
    w("# Master settings list")
    w("")
    w("One generated reference of every VP-V setting, by pipeline unit. Sources: the module README `## Settings` tables,")
    w("module `MANIFEST.json` files, `docs/design/vpv-tiers.json` and the defaults golden (`%s`)." % GOLDEN_REL)
    w("Checked against the code by `tools/check_master_settings.py` (see `docs/settings/README.md`).")
    w("")
    n_scope = len(an["scope"])
    n_doc_scope = len([k for k in an["scope"] if k in by_id])
    w("## Summary")
    w("")
    w("| Count | Value |")
    w("|---|---|")
    w("| Modules | %d |" % len(mods))
    w("| Documented rows | %d |" % len(rows))
    w("| VP-V keys in code (outside Bill's schema lists) | %d |" % n_scope)
    w("| of which documented | %d |" % n_doc_scope)
    w("| of which undocumented (appendix A) | %d |" % len(an["gaps"]["undocumented"]))
    w("| Documented keys with no code entry (appendix B) | %d |" % len(an["gaps"]["stale"]))
    w("")
    w("## Modules index")
    w("")
    w("| Unit | Module | Status | README | Documented keys | Keys in manifest | Documented / manifest |")
    w("|---|---|---|---|---|---|---|")
    for m in mods:
        mine = sorted(r["id"] for r in rows if r["slot"] == m["slot"])
        cover = len(set(mine) & set(m["manifest_keys"]))
        link = ("[README](../../%s)" % m["readme"]) if m["readme"] else "none"
        w("| %s | `%s` | %s | %s | %d | %d | %d/%d |" % (
            UNIT_TITLE.get(m["unit"], m["unit"]), m["slot"], _cell(m["status"]), link,
            len(mine), len(m["manifest_keys"]), cover, len(m["manifest_keys"])))
    w("")

    for unit in UNIT_ORDER + sorted({m["unit"] for m in mods} - set(UNIT_ORDER)):
        urows = sorted((r for r in rows if r["unit"] == unit), key=lambda r: (r["id"], r["slot"]))
        umods = [m for m in mods if m["unit"] == unit]
        w("## %s" % UNIT_TITLE.get(unit, unit))
        w("")
        w("Modules: %s." % (", ".join("`%s`" % m["slot"] for m in umods) or "none"))
        w("")
        w("| Key | Module | Section | Type | Range / values | Default | Golden default | Config placement | Original (Bill) | Fallbacks / status | Tiers |")
        w("|---|---|---|---|---|---|---|---|---|---|---|")
        if not urows:
            w("| (none documented yet) | | | | | | | | | | |")
        for r in urows:
            w("| `%s` | `%s` | %s | %s | %s | %s | %s | %s | %s | %s | %s |" % (
                r["id"], r["slot"], _cell(r["section"]), _cell(r["type"]), _cell(r["range"]),
                _cell(r["default"]), _cell(golden.get(r["id"], "")), _cell(r["placement"]),
                _cell(r["original"]), _cell(r["status"]), _cell(r["tiers"])))
        w("")

    w("## Tier presets")
    w("")
    w("From `%s`: the keys each tier sets (blank = tier leaves the key alone)." % TIERS_REL)
    w("")
    any_tier = False
    for u in tiers.get("units", []):
        names = u.get("tiers") or []
        presets = u.get("presets") or {}
        if not names:
            continue
        any_tier = True
        keys = sorted({k for t in names for k in (presets.get(t, {}).get("set", {}) if isinstance(presets.get(t), dict) else {})})
        w("### %s" % u.get("name", u.get("id", "")))
        w("")
        w("| Key | " + " | ".join(names) + " |")
        w("|---|" + "---|" * len(names))
        for k in keys:
            cells = []
            for t in names:
                p = presets.get(t)
                cells.append(_cell(str(p.get("set", {}).get(k, ""))) if isinstance(p, dict) else "")
            w("| `%s` | %s |" % (k, " | ".join(cells)))
        w("")
    if not any_tier:
        w("(no tiered units found)")
        w("")

    w("## Appendix A: keys in code without a documented row")
    w("")
    w("Each needs a row in its module README `## Settings` table (then regenerate). Hints come from module manifests,")
    w("the defaults golden and `vpv-tiers.json`; they are not documentation.")
    w("")
    w("| Key | Module hint (manifest) | Golden default | VpvSection hint | Found in |")
    w("|---|---|---|---|---|")
    hint = {}
    for m in mods:
        for k in m["manifest_keys"]:
            hint.setdefault(k, []).append(m["slot"])
    for k in an["gaps"]["undocumented"]:
        w("| `%s` | %s | %s | %s | %s |" % (
            k, _cell(", ".join(hint.get(k, [])) or "unassigned"), _cell(golden.get(k, "")),
            _cell(_tier_hint(tiers, k)), _cell(", ".join(os.path.basename(f) for f in an["code"].get(k, [])))))
    if not an["gaps"]["undocumented"]:
        w("| (none) | | | | |")
    w("")
    w("## Appendix B: documented keys with no entry in code")
    w("")
    w("| Key | Module | Row |")
    w("|---|---|---|")
    for k in an["gaps"]["stale"]:
        r = by_id[k][0]
        w("| `%s` | `%s` | %s:%d |" % (k, r["slot"], r["readme"], r["line"]))
    if not an["gaps"]["stale"]:
        w("| (none) | | |")
    w("")
    w("## Appendix C: Original (Bill) keys")
    w("")
    w("%d key(s) belong to Bill's schema files (`%s`) and are not part of this list." % (
        len({k for k in an["code"] if k in an["upstream"]}), UPSTREAM_REL))
    w("")
    return "\n".join(out)
