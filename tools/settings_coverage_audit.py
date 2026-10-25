"""Settings coverage audit: every config key VP accepts, where Config lets you change it, and the flags.

    VideoProcessorConfigTests.exe --inventory inventory.tsv        # (QT_QPA_PLATFORM=offscreen)
    python -I tools/settings_coverage_audit.py inventory.tsv       # rewrites the generated part of
                                                                   # docs/settings/coverage-audit.md
    python -I tools/settings_coverage_audit.py inventory.tsv --summary   # numbers only

Sources (no hand-kept key list):
- the inventory the Config test exe writes (--inventory): every schema key the coverage test walks
  (renderer validators, [general]/[queue]/[directshow]/[decklink]/[lldv]/[logging]/[shortcuts]/
  [ambient]/[calibration], [led_drive]/[projector], [vpv]) with its allow-list reason, every
  config.* / vpv.* control with its page, tab, tooltip and what it shows for an unset key, and the
  compiled shortcut defaults;
- the tone-mapping tuning table (ToneMappingTuning.h) and the retired keys (RetiredSettings.h),
  which the renderer validates through tables the coverage test does not see;
- runtime reads: the key as a string literal in the player/renderer sources outside the validators;
- docs/design/vpv-tiers.json (VP-V unit, tier and toggle placement) and
  docs/settings/VP-settings-reference.md (guide section, retirement candidates).
"""
import json
import pathlib
import re
import sys
from collections import defaultdict

ROOT = pathlib.Path(__file__).resolve().parent.parent
AUDIT = ROOT / "docs" / "settings" / "coverage-audit.md"
GUIDE = ROOT / "docs" / "settings" / "VP-settings-reference.md"
TIERS = ROOT / "docs" / "design" / "vpv-tiers.json"
SRC = ROOT / "src"
RUNTIME_DIRS = ["VideoProcessor-Lib", "VideoProcessor-GUI", "VideoProcessor-VPRenderer",
                "VideoProcessor-VPRendererFilter", "VP-V/core"]
BEGIN = "<!-- BEGIN GENERATED (tools/settings_coverage_audit.py) -->"
END = "<!-- END GENERATED -->"
UI_SUFFIXES = {"auto", "value", "unset", "unit"}
# Key lists that are not reads: the live-apply classifier, retired-key table and profile migrations.
NOT_READERS = {"ConfigurationApplyPolicy.h", "RetiredSettings.h", "ColorOutputProfileMigration.h",
               "CalibrationProfileMigration.h", "ConfigSchema.h"}
VALIDATOR = re.compile(r"\b(\w*Rules|ValidateProfileSetting|ValidateBaseSetting|ValidateSection)\s*\([^;{]*\)\s*(const\s*)?\{")

# Allow-list reasons that mean "accepted, but changes nothing at runtime".
NO_EFFECT = re.compile(r"retired|ignored|never read|never takes effect|never changes|compatibility editing only", re.I)
# Sources whose keys the guide's unit sections describe (a same-named key elsewhere, such as
# [logging] enabled, is not the guide's LED 'enabled').
GUIDE_SOURCES = {"renderer", "led_drive", "projector", "queue", "queue_recovery", "general", "vpv"}
# Config controls that are not named after a key on purpose (checked by hand, 2026-10-24).
UI_HELPERS = {
    "config.dv.mode.lldv": "DV mode button: writes dolby_vision_tunnel (allow-list)",
    "config.dv.mode.tunneling": "DV mode button: writes dolby_vision_tunnel (allow-list)",
    "config.vprenderer.viewport.anamorphic_enabled": "check box that enables / clears anamorphic_scale",
    "config.vprenderer.color.sdr_response": "preset drop-down that fills the SDR response keys",
    "config.led_drive.default.ftb_experimental": "unlocks the experimental fade-to-black fields",
}

UNITS = ["Input", "Motion", "Repair", "Scaling", "Colour", "Dynamic Drive", "Output"]
GUIDE_UNIT = {2: "Input", 3: "Motion", 4: "Repair", 5: "Scaling", 6: "Colour", 7: "Dynamic Drive", 8: "Output"}


def strip_validators(text):
    """Removes the bodies of validator / rule-list functions (brace matched)."""
    out, pos = [], 0
    for m in VALIDATOR.finditer(text):
        if m.start() < pos:
            continue
        depth, i = 0, m.end() - 1
        while i < len(text):
            if text[i] == "{":
                depth += 1
            elif text[i] == "}":
                depth -= 1
                if depth == 0:
                    break
            i += 1
        out.append(text[pos:m.start()])
        pos = i + 1
    out.append(text[pos:])
    return "".join(out)


def runtime_text():
    parts = []
    for d in RUNTIME_DIRS:
        for p in (SRC / d).rglob("*"):
            if p.suffix not in (".h", ".cpp") or "test" in p.name.lower() or p.name in NOT_READERS:
                continue
            t = p.read_text(encoding="utf-8", errors="replace")
            t = re.sub(r"//[^\n]*", "", t)
            parts.append(strip_validators(t))
    return "\n".join(parts)


def read_inventory(path):
    keys, controls, shortcuts = [], [], []
    for line in pathlib.Path(path).read_text(encoding="utf-8").splitlines():
        f = line.split("\t")
        if f[0] == "K":
            keys.append({"source": f[1], "sections": f[2].split("|"), "key": f[3], "allow": f[4] if len(f) > 4 else ""})
        elif f[0] == "C":
            f += [""] * (10 - len(f))
            controls.append({"name": f[1], "cls": f[2], "shown": f[3], "tip": f[4] == "1", "enabled": f[5] == "1",
                             "tooltip": f[6], "page": f[7], "tab": f[8]})
        elif f[0] == "S":
            shortcuts.append((f[1], f[2] if len(f) > 2 else ""))
    return keys, controls, shortcuts


def table_keys(header, pattern):
    text = (SRC / "VideoProcessor-Lib" / header).read_text(encoding="utf-8")
    return re.findall(pattern, text)


def shown_default(shown):
    s = shown.strip()
    for pat in (r"^Not set \(default: (.*)\)$", r"^Not set \((.*)\)$", r"^Default: (.*)$", r"^Inherited: (.*)$",
                r"^When unset: (.*)$", r"^(.*) \(default[^)]*\)$"):
        m = re.match(pat, s)
        if m:
            return m.group(1)
    if s in ("Inherited / not set", ""):
        return ""
    return s


def guide_info():
    text = GUIDE.read_text(encoding="utf-8")
    section_of, retire = {}, {}
    current = 0
    for line in text.splitlines():
        m = re.match(r"^## (\d+)\.", line)
        if m:
            current = int(m.group(1))
        for k in re.findall(r"`([a-z0-9_]+)(?:=[a-z0-9_]+)?`", line):
            if current and current != 12:
                section_of.setdefault(k, current)
    m = re.search(r"## 12\. Settings to retire.*", text, re.S)
    if m:
        group = ""
        for line in m.group(0).splitlines():
            g = re.match(r"^\*\*([A-D])\. (.*?)\*\*", line)
            if g:
                group = g.group(1) + ". " + g.group(2).rstrip(".:")
            for k in re.findall(r"`([a-z0-9_]+)(?:\([a-z_]+\))?(?:=[a-z0-9_]+)?`", line):
                retire.setdefault(k, group)
        # `dejudder_verify(_share)` names two keys.
        if "dejudder_verify" in retire:
            retire.setdefault("dejudder_verify_share", retire["dejudder_verify"])
    return section_of, retire


def tiers_info():
    data = json.loads(TIERS.read_text(encoding="utf-8"))
    tier_of, toggle_of, planned_of = defaultdict(list), {}, {}
    for u in data["units"]:
        for tier, p in u["presets"].items():
            for k in p["set"]:
                tier_of[k].append((u["name"], tier, p["set"][k]))
        for t in u.get("toggles", []):
            toggle_of[t["key"]] = (u["name"], t["label"])
        for p in u.get("planned", []):
            planned_of[p["key"]] = (u["name"], p["label"])
    return data, tier_of, toggle_of, planned_of


def control_key(name):
    """(section, key) for a config.<section>.<key> control, else None."""
    if not name.startswith("config."):
        return None
    parts = name[7:].split(".")
    if len(parts) > 2 and parts[-1] in UI_SUFFIXES:
        parts = parts[:-1]
    if len(parts) < 2:
        return None
    return ".".join(parts[:-1]), parts[-1]


def in_source(section, prefixes):
    return any(section == p or section.startswith(p + ".") for p in prefixes)


def where(c):
    return c["page"] + (" > " + c["tab"] if c["tab"] and c["tab"] != c["page"] else "")


def md(s):
    return str(s).replace("|", "\\|")


def analyse(inventory_path):
    keys, controls, shortcuts = read_inventory(inventory_path)
    runtime = runtime_text()
    section_of, retire = guide_info()
    tiers, tier_of, toggle_of, planned_of = tiers_info()
    # The retirement list the VP-V page shows (vpv-tiers.json), by schema source.
    source_of_section = {"vprenderer": "renderer", "general": "general", "led_drive": "led_drive",
                         "projector": "projector", "queue": "queue"}
    groups = tiers.get("proposed_retirement", {}).get("groups", {})
    retire_json = {(source_of_section[r["section"]], r["key"]): r["group"] + ". " + groups.get(r["group"], "")
                   for r in tiers.get("proposed_retirement", {}).get("keys", [])}
    tone = table_keys("ToneMappingTuning.h", r'\{"([a-z_]+)", "')
    retired = table_keys("RetiredSettings.h", r'\{ "([a-z_]+)", "[^"]*(?:"\s*"[^"]*)*"\s*\},')
    retired = [k for k in retired if k in ("fix_scene_cooldown", "dejudder_picture_area", "dither_vectors_new_frame")] or retired
    known = {k["key"] for k in keys}
    for k in tone:
        if k not in known:
            keys.append({"source": "renderer", "sections": ["vprenderer"], "key": k, "allow": "", "via": "ToneMappingTuning"})
    for k in retired:
        if k not in known:
            keys.append({"source": "renderer", "sections": ["vprenderer"], "key": k,
                         "allow": "retired (RetiredSettings.h): accepted with any value, ignored", "via": "RetiredSettings"})

    by_key = defaultdict(list)
    for c in controls:
        ck = control_key(c["name"])
        if ck:
            by_key[ck[1]].append((ck[0], c))
    vpv_controls = {}
    for c in controls:
        n = c["name"]
        if not n.startswith("vpv."):
            continue
        parts = n.split(".")
        if parts[1] == "pathway" and len(parts) == 2:
            vpv_controls.setdefault("pipeline_original_pathway", []).append(("Pathway", c))
        elif len(parts) == 3 and parts[1] == "expert":
            vpv_controls.setdefault(parts[2], []).append(("Pipeline (expert)", c))
        elif len(parts) == 4 and parts[2] == "planned":
            vpv_controls.setdefault(parts[3], []).append((c["tab"] + " (planned, greyed)", c))
        elif len(parts) == 3 and parts[2] not in ("tier", "settings", "slots", "slots_heading", "untiered", "delay"):
            vpv_controls.setdefault(parts[2], []).append((c["tab"], c))

    rows = []
    for k in keys:
        key = k["key"]
        own, elsewhere = [], []
        for section, c in by_key.get(key, []):
            (own if in_source(section, k["sections"]) else elsewhere).append(c)
        vpv = vpv_controls.get(key, [])
        live_vpv = [v for v in vpv if "planned" not in v[0]]
        tiers_here = tier_of.get(key, [])
        literal = '"' + key + '"'
        reads = runtime.count(literal)
        via = k.get("via", "")
        if via == "ToneMappingTuning":
            reads = max(reads, 1)   # read through the tuning table by index
        row = dict(k)
        row.update(own=own, elsewhere=elsewhere, vpv=vpv, live_vpv=live_vpv, tiers=tiers_here, reads=reads,
                   guide=section_of.get(key), retire=retire_json.get((k["source"], key), ""))
        # Default as Config shows it (first upstream control with a readable default), else VP-V's table.
        default = ""
        for c in own:
            default = shown_default(c["shown"])
            if default:
                break
        if not default and key in tiers["defaults"]:
            default = tiers["defaults"][key]
        row["default"] = default
        # Unit: VP-V placement, then the guide's unit section, else the upstream page.
        unit = ""
        if key in toggle_of:
            unit = toggle_of[key][0]
        elif tiers_here:
            unit = tiers_here[0][0]
        elif key in planned_of:
            unit = planned_of[key][0]
        elif row["guide"] in GUIDE_UNIT and k["source"] in GUIDE_SOURCES:
            unit = GUIDE_UNIT[row["guide"]]
        row["unit"] = unit
        flags = []
        has_control = bool(own or live_vpv or tiers_here)
        if not has_control:
            flags.append("NO CONTROL" if not k["allow"] else "allow-listed")
        if k["allow"] and NO_EFFECT.search(k["allow"]):
            flags.append("NO EFFECT")
        elif reads == 0 and via != "ToneMappingTuning":
            flags.append("ORPHAN?")
        if (own and (live_vpv or tiers_here)):
            flags.append("DUAL (managed)")
        pages = {where(c) for c in own}
        if len({c["page"] for c in own}) > 1:
            flags.append("DUP pages")
        if row["retire"]:
            flags.append("retire " + row["retire"][0])
        row["flags"] = flags
        rows.append(row)

    # Controls whose key no schema accepts (the "no effect" check), skipping UI-only widgets.
    keyset = {r["key"] for r in rows}
    ui_words = re.compile(r"(add|remove|move_up|move_down|profiles|name|use_rule|live_toggle|selected_profile_status|"
                          r"status|note|label|heading|buttons|button|row|check|reset|toggle|preview|picker|help|error|"
                          r"range|slider|control|fine|title|box|description|default|problem|browse|items|details|"
                          r"run|on|open|result|summary|warning|notice|report|mode|fps|rule|engine|matcher|off|always|"
                          r"blend|hold|libplacebo|sigma_delta|one_d|lut|vp|frames|time|rate5060|rules|"
                          r"compensate|internal_script|none|display|metadata|visibility|ease_fast|ease_slow|led_protect|"
                          r"inherit|compatibility|reset_defaults|restore_defaults|open_folder|edit_fallback|"
                          r"connection_test|advanced_toggle|profile_browse|profile_builtin|profile_status|settings|"
                          r"settings_note|settings_reset|settings_title|addons|stage|glsl_file|hlsl_file|parameters|"
                          r"parameters_toggle|modes|clear|coalesce_role|delay_seconds)$")
    orphan_controls = []
    for c in controls:
        ck = control_key(c["name"])
        if not ck:
            continue
        section, key = ck
        if key in keyset or ui_words.search(c["name"]) or section.startswith(("shader", "actions", "shortcuts")):
            continue
        if c["cls"] in ("QLabel", "QWidget", "QFrame", "QGroupBox", "QPushButton", "QListWidget", "QStackedWidget"):
            continue
        orphan_controls.append(c)
    shortcut_rows = []
    for action, chord in shortcuts:
        ctl = [c for c in controls if c["name"] == "config.shortcuts." + action]
        shortcut_rows.append((action, chord, where(ctl[0]) if ctl else ""))
    return rows, orphan_controls, shortcut_rows, controls


def summary(rows, orphan_controls, shortcut_rows):
    total = len(rows)
    with_control = sum(1 for r in rows if r["own"] or r["live_vpv"] or r["tiers"])
    allow = sum(1 for r in rows if "allow-listed" in r["flags"])
    missing = sum(1 for r in rows if "NO CONTROL" in r["flags"])
    orphans = sum(1 for r in rows if "ORPHAN?" in r["flags"])
    no_effect = sum(1 for r in rows if "NO EFFECT" in r["flags"])
    dual = sum(1 for r in rows if "DUAL (managed)" in r["flags"])
    dup = sum(1 for r in rows if "DUP pages" in r["flags"])
    retire = sum(1 for r in rows if r["retire"])
    vpv_keys = sum(1 for r in rows if r["live_vpv"] or r["tiers"])
    guide = sum(1 for r in rows if r["guide"])
    return dict(total=total, with_control=with_control, allow=allow, missing=missing, orphans=orphans,
                no_effect=no_effect, dual=dual,
                dup=dup, retire=retire, vpv=vpv_keys, guide=guide, shortcuts=len(shortcut_rows),
                shortcuts_with_control=sum(1 for s in shortcut_rows if s[2]),
                orphan_controls=sum(1 for c in orphan_controls if c["name"] not in UI_HELPERS))


def render(rows, orphan_controls, shortcut_rows):
    s = summary(rows, orphan_controls, shortcut_rows)
    out = [BEGIN, "", "## A. Generated inventory", "",
           "Generated from the Config test exe's `--inventory` dump and the source; do not edit by hand. "
           "Re-run: `VideoProcessorConfigTests.exe --inventory inv.tsv` (offscreen) then "
           "`python -I tools/settings_coverage_audit.py inv.tsv`.", "",
           "| Measure | Count |", "|---|---|",
           f"| Keys VP accepts (schemas + tone tuning + retired) | {s['total']} |",
           f"| ... with a Config control (upstream page, VP-V toggle or VP-V tier) | {s['with_control']} |",
           f"| ... without a control, allow-listed with a reason (alias / legacy location / retired / diagnostic) | {s['allow']} |",
           f"| ... without a control and NOT allow-listed (violates the rule) | {s['missing']} |",
           f"| ... accepted but changing nothing (allow-list says retired / ignored / never read) | {s['no_effect']} |",
           f"| ... accepted but never read as a literal by the player/renderer (orphan candidates) | {s['orphans']} |",
           f"| ... on the VP-V page (toggle or tier) | {s['vpv']} |",
           f"| ... on the VP-V page AND an upstream page (managed duplicates: the upstream control greys under VP-V) | {s['dual']} |",
           f"| ... with controls on two or more upstream pages | {s['dup']} |",
           f"| ... in the settings guide | {s['guide']} |",
           f"| ... retirement candidates (guide section 12) | {s['retire']} |",
           f"| Upstream controls that write a key no schema accepts (no effect; UI helpers excluded) | {s['orphan_controls']} |",
           f"| Shortcut actions (compiled defaults) / with a Shortcuts-page control | {s['shortcuts']} / {s['shortcuts_with_control']} |",
           "",
           "Flags: **NO CONTROL** = no Config control and no allow-list reason; **allow-listed** = no control by design "
           "(reason in the Notes column); **NO EFFECT** = accepted but ignored at runtime; **ORPHAN?** = accepted but no literal read in the player/renderer (see section 3 "
           "for the hand check); **DUAL (managed)** = on the VP-V page and an upstream page (the upstream one is greyed "
           "'Managed by VP-V' while the VP-V pathway is on); **DUP pages** = controls on two upstream pages; "
           "**retire X** = guide section 12 group X.", ""]
    order = UNITS + [""]
    groups = defaultdict(list)
    for r in rows:
        if r["unit"]:
            groups[r["unit"]].append(r)
        elif r["live_vpv"]:
            groups["VP-V: global keys ([vpv])"].append(r)
        elif r["own"]:
            groups["Upstream: " + (r["own"][0]["page"] or "(no page)")].append(r)
        elif r["allow"]:
            groups["No control of its own (allow-listed)"].append(r)
        else:
            groups["NO CONTROL"].append(r)
    names = [u for u in UNITS if u in groups] + sorted(g for g in groups if g not in UNITS)
    for g in names:
        out += [f"### {g} ({len(groups[g])})", "",
                "| Key | Section | Upstream control | VP-V | Tier | Default | Guide | Flags / notes |",
                "|---|---|---|---|---|---|---|---|"]
        for r in sorted(groups[g], key=lambda r: r["key"]):
            up = "; ".join(sorted({where(c) for c in r["own"]})) or ("only in " + ", ".join(sorted({where(c) for c in r['elsewhere']})) if r["elsewhere"] else "-")
            vp = "; ".join(sorted({v[0] for v in r["vpv"]})) or "-"
            tier = ", ".join(f"{t[1]}={t[2]}" for t in r["tiers"]) or "-"
            notes = ", ".join(r["flags"])
            if r["allow"]:
                notes += (": " if notes else "") + r["allow"]
            guide = f"s{r['guide']}" if r["guide"] else "-"
            out.append(f"| `{r['key']}` | {md('/'.join(r['sections'][:1]))} | {md(up)} | {md(vp)} | {md(tier)} | "
                       f"{md(r['default'][:40]) or '-'} | {guide} | {md(notes)} |")
        out.append("")
    out += ["### Shortcut actions", "", "| Action | Default chord | Control |", "|---|---|---|"]
    for a, chord, w in shortcut_rows:
        out.append(f"| `{a}` | {chord or '(none)'} | {md(w) or 'NO CONTROL'} |")
    out.append("")
    if orphan_controls:
        out += ["### Controls not named after an accepted key", "", "| Control | Page | Checked |", "|---|---|---|"]
        for c in orphan_controls:
            out.append(f"| `{c['name']}` | {md(where(c))} | {md(UI_HELPERS.get(c['name'], 'NO EFFECT: writes a key VP does not accept'))} |")
        out.append("")
    out.append(END)
    return "\n".join(out), s


def main(argv):
    if len(argv) < 2:
        print(__doc__)
        return 2
    rows, orphan_controls, shortcut_rows, _ = analyse(argv[1])
    text, s = render(rows, orphan_controls, shortcut_rows)
    if "--summary" in argv:
        print(json.dumps(s, indent=1))
        for flag in ("NO CONTROL", "NO EFFECT", "ORPHAN?", "DUP pages", "DUAL (managed)"):
            print(flag, sorted(f"{r['source']}:{r['key']}" for r in rows if flag in r["flags"]))
        print("orphan controls", [c["name"] for c in orphan_controls])
        return 0
    doc = AUDIT.read_text(encoding="utf-8") if AUDIT.exists() else "# Settings coverage audit\n\n" + BEGIN + "\n" + END + "\n"
    if BEGIN not in doc:
        doc += "\n" + BEGIN + "\n" + END + "\n"
    head, rest = doc.split(BEGIN, 1)
    tail = rest.split(END, 1)[1] if END in rest else ""
    AUDIT.write_text(head + text + tail, encoding="utf-8", newline="\n")
    print(json.dumps(s))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
