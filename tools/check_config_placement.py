# Copyright(C) 2026 VP-V contributors
# SPDX-License-Identifier: GPL-3.0-only
"""One placement table for the Config editor (P8-3, consolidation plan row 218).

Usage: python -I tools/check_config_placement.py [--root PATH] [--write] [--all] [--list]

The ONE placement table is `Organisation()` in
src/VP-V/modules/control/config/config_pages/VpvOrganisation.h: the card (unit tab, Main or
Advanced sub-page, card title) every Rendering-page row, whole section or VP-V toggle lives on.
This tool derives, from that table alone, the placement every key must be documented with in a
module README (the doc-70 `## Settings` tables, template: docs/settings/README.md) and checks:

  1. a key (Row / Toggle item) sits on one card only, a Section id on one card only;
  2. a documented key that sits on a card carries that card's placement text:
         VP-V > <Unit> > <Main|Advanced> > <card title>
     (the Proposed retirement tab has no sub-page: VP-V > Proposed retirement > <card title>).
     Text after the canonical placement ("; ...", " (...)") is allowed: it names a second,
     Bill-owned place for a managed duplicate;
  3. a key on a card of unit U is owned by a module of unit U (the owner row is in that module's
     README); Proposed retirement cards are exempt (a retired key keeps its unit's owner);
  4. every key on a Motion card that code accepts has exactly one owner row (doc-70) in a motion
     module, so de-judder slider rows, estimation rows, experiments and the intent_motion* rows
     are all owned by motion.

Rows of keys inside a whole Section (the key is built by Bill's ConfigEditorWindow.cpp inside that
section, not listed one by one) are resolved from the section's source span there: a key named
inside the section's span and on no card of its own takes the section's card.

Hard errors (exit 1) are limited to the Motion unit (the unit this check is enforced for first,
P8-3). The same differences in the other units are reported as advisory counts (--all makes them
errors); the lanes that move those units (P8-4, P8-5) turn them on.
--write rewrites the Config placement cell of every documented key whose card is known.
--list prints the resolved table. Pure text parsing: no Qt, no build.
"""
import argparse
import json
import os
import re
import sys

sys.path.append(os.path.dirname(os.path.abspath(__file__)))
import master_settings_common as C  # noqa: E402

ORG_REL = "src/VP-V/modules/control/config/config_pages/VpvOrganisation.h"
CE_REL = "src/VideoProcessor-Config/ConfigEditorWindow.cpp"
RETIREMENT_UNIT = "retirement"
RETIREMENT_NAME = "Proposed retirement"
ENFORCED_UNITS = ("motion",)
# A card may sit in another unit's tab than the module that owns the key (the viewer finds the
# control where it is used; the module that reads it owns it). Every such case is listed here, with
# the owner's slot; the owner's MANIFEST.json config_keys must list the key.
CROSS_UNIT_OWNER = {
    "occlusion_mask": "motion.consumers",          # card: Output (presentation of the picture); code: OcclusionMaskHook
    "render_ahead": "output.presentation",         # card: Motion > Render ahead (under test); code: the render-ahead planner
    "render_ahead_queue_credit": "output.presentation",
    "render_ahead_utilisation": "output.presentation",
    "dejudder_gate": "output.presentation",
    "dejudder_pulldown": "input.queue_timing",     # card: Motion > De-judder shaping; row documented there by r2/docs
}
# Keys whose owner row r2/docs put in a module whose MANIFEST does not list them ("read here, owned elsewhere"):
# the row location is accepted as is; the manifest-lists-the-key rule is not applied to them.
MANIFEST_EXEMPT = ("dejudder_pulldown",)
KEY_ITEMS = ("Row", "Toggle")


def read(root, rel):
    return C.read_text(os.path.join(root, *rel.split("/")))


def parse_organisation(text):
    """-> [card dict] in file order: unit, id, title, advanced, items [(kind, id)]."""
    start = text.index("Organisation()")
    end = text.find("\tinline ", start + 20)
    body = text[start:end if end > 0 else len(text)]
    marks = [(m.start(), m.group(1), m.group(2), m.group(3))
             for m in re.finditer(r'\n\t\t\t\{ "(\w+)", "(\w+)", "([^"]*)"', body)]
    marks.append((len(body), None, None, None))
    cards = []
    for (s, unit, cid, title), (e, _u, _i, _t) in zip(marks, marks[1:]):
        chunk = body[s:e]
        flags = re.search(r',\s*(true|false)\s*,\s*(true|false)\s*,\s*\{', chunk)
        items = [(m.group(1), m.group(2))
                 for m in re.finditer(r'\{\s*K::(Row|Toggle|Section|Elsewhere)\s*,\s*"([^"]+)"', chunk)]
        cards.append({"unit": unit, "id": cid, "title": title,
                      "advanced": bool(flags and flags.group(1) == "true"), "items": items})
    return cards


def unit_names(root):
    names = {}
    tiers = json.loads(read(root, C.TIERS_REL))
    for u in tiers.get("units", []):
        names[u["id"]] = u.get("name", u["id"])
    names[RETIREMENT_UNIT] = RETIREMENT_NAME
    return names


def section_spans(ce_lines):
    """-> {section id: (first line, end line)} of each addCollapsibleSection(QStringLiteral("id") ... call."""
    marks = []
    for i, ln in enumerate(ce_lines):
        m = re.search(r'addCollapsibleSection\(\s*QStringLiteral\("([A-Za-z0-9_]+)"\)', ln)
        if not m and "addCollapsibleSection(" in ln and i + 1 < len(ce_lines):
            m = re.search(r'^\s*QStringLiteral\("([A-Za-z0-9_]+)"\)', ce_lines[i + 1])
        if m:
            marks.append((i, m.group(1)))
    spans = {}
    for n, (i, sid) in enumerate(marks):
        spans[sid] = (i, marks[n + 1][0] if n + 1 < len(marks) else len(ce_lines))
    return spans


def build_table(root, known_keys):
    """-> (cards, placement{key: card}, section_card{section id: card}, errors)."""
    errors = []
    cards = parse_organisation(read(root, ORG_REL))
    names = unit_names(root)
    placement = {}
    section_card = {}
    for card in cards:
        card["name"] = names.get(card["unit"], card["unit"])
        if card["unit"] == RETIREMENT_UNIT:
            card["text"] = "VP-V > %s > %s" % (RETIREMENT_NAME, card["title"])
        else:
            card["text"] = "VP-V > %s > %s > %s" % (card["name"], "Advanced" if card["advanced"] else "Main", card["title"])
        for kind, ident in card["items"]:
            if kind in KEY_ITEMS:
                if ident in placement:
                    errors.append("%s: key '%s' is on two cards: %s/%s and %s/%s" % (
                        ORG_REL, ident, placement[ident]["unit"], placement[ident]["id"], card["unit"], card["id"]))
                else:
                    placement[ident] = card
            elif kind == "Section":
                if ident in section_card:
                    errors.append("%s: section '%s' is on two cards" % (ORG_REL, ident))
                else:
                    section_card[ident] = card
    # keys built inside a whole Section by Bill's page: the section's source span
    ce_path = os.path.join(root, *CE_REL.split("/"))
    if os.path.isfile(ce_path):
        lines = read(root, CE_REL).splitlines()
        spans = section_spans(lines)
        for sid, card in section_card.items():
            if sid not in spans:
                errors.append("%s: section '%s' (card %s/%s) not found in %s" % (ORG_REL, sid, card["unit"], card["id"], CE_REL))
                continue
            first, last = spans[sid]
            for ln in lines[first:last]:
                for tok in re.findall(r'"([a-z][a-z0-9_]*)"', ln):
                    if tok in known_keys and "_" in tok and tok not in placement:
                        placement[tok] = card
    return cards, placement, section_card, errors


def allowed(cell, canonical):
    return cell == canonical or cell.startswith((canonical + " (", canonical + ";"))


def cell_split(line):
    """A table line -> its cells (unescaped pipes), keeping each cell's exact text for rewriting."""
    body = line.rstrip("\n")
    return [c for c in re.split(r"(?<!\\)\|", body)]


def rewrite_placements(root, rows, placement):
    """--write: set the placement cell (6th column) of the documented rows whose card is known."""
    changed = 0
    by_readme = {}
    for r in rows:
        card = placement.get(r["key"])
        if card is None or allowed(r["placement"], card["text"]):
            continue
        by_readme.setdefault(r["readme"], []).append((r["line"], r["key"], card["text"]))
    for rel, edits in by_readme.items():
        path = os.path.join(root, *rel.split("/"))
        with open(path, encoding="utf-8", newline="") as fh:
            text = fh.read()
        lines = text.split("\n")
        for ln, key, new in edits:
            cells = cell_split(lines[ln - 1])
            if len(cells) < 8 or cells[1].strip() != "`%s`" % key:
                print("skipped %s:%d (row layout changed)" % (rel, ln))
                continue
            cells[6] = " " + new.replace("|", "\\|") + " "
            lines[ln - 1] = "|".join(cells)
            changed += 1
        with open(path, "w", encoding="utf-8", newline="") as fh:
            fh.write("\n".join(lines))
    return changed


def analyse(root):
    an = C.analyse(root)
    known = set(an["code"]) | {r["key"] for r in an["rows"]}
    cards, placement, section_card, errors = build_table(root, known)
    rows = an["rows"]
    issues = {"error": list(errors), "advisory": []}

    def flag(units, msg):
        issues["error" if any(u in ENFORCED_UNITS for u in units) else "advisory"].append(msg)

    # 2. canonical placement text, 3. owner unit of the row
    for r in rows:
        card = placement.get(r["key"])
        if card is None:
            continue
        where = "%s:%d" % (r["readme"], r["line"])
        if not allowed(r["placement"], card["text"]):
            flag((r["unit"], card["unit"]),
                 "%s: %s is on card %s/%s: placement should read '%s', not '%s'" % (
                     where, r["key"], card["unit"], card["id"], card["text"], r["placement"]))
        if r["key"] in CROSS_UNIT_OWNER:
            if r["slot"] != CROSS_UNIT_OWNER[r["key"]]:
                flag((r["unit"], card["unit"]), "%s: %s is owned by %s (CROSS_UNIT_OWNER), its row is in %s" % (
                    where, r["key"], CROSS_UNIT_OWNER[r["key"]], r["slot"]))
        elif card["unit"] != RETIREMENT_UNIT and r["unit"] != card["unit"]:
            flag((r["unit"], card["unit"]),
                 "%s: %s sits on a %s card but its owner row is in %s (%s)" % (
                     where, r["key"], card["unit"], r["unit"], r["slot"]))

    for key, slot in sorted(CROSS_UNIT_OWNER.items()):
        if key not in placement or key in MANIFEST_EXEMPT:
            continue
        mod = next((m for m in an["modules"] if m["slot"] == slot), None)
        if mod is None or key not in mod["manifest_keys"]:
            issues["error"].append("CROSS_UNIT_OWNER: %s names %s but that MANIFEST.json does not list the key" % (key, slot))

    # 4. every motion-card key that code accepts has exactly one owner row in a motion module
    owner = {}
    for r in rows:
        owner.setdefault(r["key"], []).append(r)
    code = set(an["code"])
    for key, card in sorted(placement.items()):
        if card["unit"] not in ENFORCED_UNITS or key not in code:
            continue
        rs = owner.get(key, [])
        if key in CROSS_UNIT_OWNER:
            mod = next((m for m in an["modules"] if m["slot"] == CROSS_UNIT_OWNER[key]), None)
            if mod is not None and mod["unit"] not in ENFORCED_UNITS:
                continue
        if len(rs) != 1:
            issues["error"].append("%s/%s: %s has %d owner rows (need exactly 1 in a %s module)" % (
                card["unit"], card["id"], key, len(rs), card["unit"]))
    return an, cards, placement, issues


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--root", default=os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
    ap.add_argument("--write", action="store_true", help="rewrite the placement cells of documented keys")
    ap.add_argument("--all", action="store_true", help="advisory differences in the other units are errors too")
    ap.add_argument("--list", action="store_true", help="print the resolved table")
    args = ap.parse_args(argv)
    an, cards, placement, issues = analyse(args.root)
    if args.write:
        n = rewrite_placements(args.root, an["rows"], placement)
        print("placement cells rewritten: %d" % n)
        an, cards, placement, issues = analyse(args.root)
    if args.list:
        for key, card in sorted(placement.items()):
            print("%-40s %s" % (key, card["text"]))
    for msg in issues["error"]:
        print("ERROR: " + msg)
    adv = issues["advisory"]
    for msg in adv[:20]:
        print("advisory: " + msg)
    if len(adv) > 20:
        print("advisory: ... and %d more" % (len(adv) - 20))
    print("cards: %d; keys placed: %d; documented rows: %d; errors (%s): %d; advisory (other units): %d" % (
        len(cards), len(placement), len(an["rows"]), "/".join(ENFORCED_UNITS), len(issues["error"]), len(adv)))
    return 1 if issues["error"] or (args.all and adv) else 0


if __name__ == "__main__":
    sys.exit(main())
