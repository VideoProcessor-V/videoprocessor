# Copyright(C) 2026 VP-V contributors
# SPDX-License-Identifier: GPL-3.0-only
"""Unit tests for tools/check_config_placement.py (fixture trees; no Qt, no build)."""
import contextlib
import importlib.util
import io
import json
import os
import sys
import tempfile
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
sys.path.insert(0, HERE)


def _load(name):
    spec = importlib.util.spec_from_file_location(name, os.path.join(HERE, name + ".py"))
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


C = _load("master_settings_common")
chk = _load("check_config_placement")

HDR = ("| Key | Section | Type | Range / values | Default | Config placement | Original (Bill) "
       "| Fallbacks / status | Tiers |\n|---|---|---|---|---|---|---|---|---|\n")

SCHEMA = '''
inline bool V(const std::string& key, const std::string& value) {
    if (key == "dejudder_strength") return true;
    if (key == "dejudder_fade_match") return true;
    if (key == "intent_motion") return true;
    if (key == "intent_motion_min") return true;
    if (key == "repair_key") return true;
    return false;
}
'''

ORG = '''#pragma once
namespace Vpv {
	inline const std::vector<Card>& Organisation()
	{
		static const std::vector<Card> cards = {
			{ "motion", "easing", "Per-shot easing", "help", false, false, {
				{ K::Toggle, "intent_motion" } } },
			{ "motion", "taste_keys", "Set by the amount", "help", true, false, {
				{ K::Row, "dejudder_strength" }, { K::Hide, "some_heading" } } },
			{ "motion", "shaping", "Shaping", nullptr, true, false, {
				{ K::Section, "dejudder" } } },
			{ "repair", "deband", "Deband", "help", false, false, {
				{ K::Row, "repair_key" } } },
		};
		return cards;
	}
	inline const std::vector<int>& Other() { static const std::vector<int> v; return v; }
}
'''

CE = '''
form = addCollapsibleSection(QStringLiteral("dejudder"), QStringLiteral("De-judder"), QString(), false);
addRow("dejudder_strength");
addRow("dejudder_fade_match");
form = addCollapsibleSection(QStringLiteral("other"), QStringLiteral("Other"), QString(), false);
addRow("repair_key");
form = addCollapsibleSection(QStringLiteral("fixesUnderTest"), QStringLiteral("Fixes under test"), QString(), false);
addRow("intent_motion");
'''


def row(key, placement, section="profile (`vprenderer.<profile>`, input group)", typ="bool", rng="on, off", default="on"):
    return "| `%s` | %s | %s | %s | %s | %s | forced off | none | none |\n" % (key, section, typ, rng, default, placement)


def _w(root, rel, text):
    p = os.path.join(root, *rel.split("/"))
    os.makedirs(os.path.dirname(p), exist_ok=True)
    with open(p, "w", encoding="utf-8", newline="\n") as f:
        f.write(text)


def _run(argv):
    out = io.StringIO()
    with contextlib.redirect_stdout(out):
        rc = chk.main(argv)
    return rc, out.getvalue()


class Fixture(unittest.TestCase):
    def setUp(self):
        self.stays = chk.FIXES_CONTAINER_STAYS
        chk.FIXES_CONTAINER_STAYS = ()               # the fixture container holds no repository keys
        self.td = tempfile.TemporaryDirectory()
        self.root = self.td.name
        _w(self.root, C.SCHEMA_FILES[0], SCHEMA)
        _w(self.root, C.UPSTREAM_REL, "# header\n")
        _w(self.root, C.GOLDEN_REL, "# golden\n### case empty\nrule=x\n")
        _w(self.root, chk.ORG_REL, ORG)
        _w(self.root, chk.CE_REL, CE)
        _w(self.root, C.TIERS_REL, json.dumps({"units": [{"id": "motion", "name": "Motion"}, {"id": "repair", "name": "Repair"}]}))
        self.readmes()

    def tearDown(self):
        chk.FIXES_CONTAINER_STAYS = self.stays
        self.td.cleanup()

    def mod(self, unit, sub, keys, body):
        d = "src/VP-V/modules/%s/%s" % (unit, sub)
        _w(self.root, d + "/MANIFEST.json", json.dumps({
            "slot": "%s.%s" % (unit, sub), "unit": unit, "order": 10, "status": "built",
            "components": {"config_keys": keys}}))
        _w(self.root, d + "/README.md", "# M\n\n## Settings\n\n%s%s\n## Tests\n\n- none\n" % (HDR, body))

    def readmes(self, motion=None, repair=None):
        if motion is None:
            motion = (row("dejudder_fade_match", "VP-V > Motion > Advanced > Shaping") +
                      row("dejudder_strength", "VP-V > Motion > Advanced > Set by the amount", typ="float", rng="0..1", default="0.5") +
                      row("intent_motion", "VP-V > Motion > Main > Per-shot easing", typ="enum") +
                      row("intent_motion_min", "VP-V > Advanced", typ="float", rng="0..1", default="0.5"))
        if repair is None:
            repair = row("repair_key", "VP-V > Repair > Main > Deband")
        self.mod("motion", "consumers", ["dejudder_fade_match", "dejudder_strength", "intent_motion", "intent_motion_min"], motion)
        self.mod("repair", "deband", ["repair_key"], repair)


class OrganisationParseTests(unittest.TestCase):
    def test_cards_flags_items_and_function_boundary(self):
        cards = chk.parse_organisation(ORG)
        self.assertEqual([(c["unit"], c["id"], c["advanced"]) for c in cards],
                         [("motion", "easing", False), ("motion", "taste_keys", True),
                          ("motion", "shaping", True), ("repair", "deband", False)])
        self.assertEqual(cards[1]["items"], [("Row", "dejudder_strength")])   # Hide is not a key item
        self.assertEqual(cards[2]["items"], [("Section", "dejudder")])
        self.assertEqual(cards[2]["title"], "Shaping")

    def test_section_spans(self):
        spans = chk.section_spans(CE.splitlines())
        self.assertEqual(sorted(spans), ["dejudder", "fixesUnderTest", "other"])
        self.assertEqual(spans["dejudder"][1], spans["other"][0])
        self.assertEqual(spans["other"][1], spans["fixesUnderTest"][0])

    def test_own_organisation_parses_to_the_motion_cards(self):
        text = chk.read(ROOT, chk.ORG_REL)
        ids = [(c["unit"], c["id"]) for c in chk.parse_organisation(text)]
        for want in [("motion", "taste"), ("motion", "easing"), ("motion", "easing_tuning"), ("motion", "internals")]:
            self.assertIn(want, ids)


class CheckTests(Fixture):
    def test_clean_tree_passes_and_resolves_section_members(self):
        rc, out = _run(["--root", self.root, "--list"])
        self.assertEqual(rc, 0, out)
        self.assertIn("dejudder_fade_match", out)
        self.assertIn("VP-V > Motion > Advanced > Shaping", out)
        self.assertIn("VP-V > Repair > Main > Deband", out)

    def test_wrong_motion_placement_is_an_error_and_write_repairs_it(self):
        rc, out = _run(["--root", self.root])
        self.assertEqual(rc, 0, out)                      # intent_motion_min is on no card: not checked
        self.readmes(motion=row("dejudder_fade_match", "VP-V > Advanced") +
                     row("dejudder_strength", "VP-V > Motion > Advanced > Set by the amount", typ="float", rng="0..1", default="0.5") +
                     row("intent_motion", "VP-V > Motion > Main > Per-shot easing", typ="enum") +
                     row("intent_motion_min", "VP-V > Advanced", typ="float", rng="0..1", default="0.5"))
        rc, out = _run(["--root", self.root])
        self.assertEqual(rc, 1)
        self.assertIn("dejudder_fade_match is on card motion/shaping", out)
        rc, out = _run(["--root", self.root, "--write"])
        self.assertEqual(rc, 0, out)
        self.assertIn("rewritten: 1", out)
        text = chk.read(self.root, "src/VP-V/modules/motion/consumers/README.md")
        self.assertIn("| VP-V > Motion > Advanced > Shaping |", text)
        self.assertIn("| VP-V > Advanced |", text)       # intent_motion_min: no card, untouched

    def test_second_place_after_the_canonical_text_is_allowed(self):
        self.readmes(motion=row("dejudder_fade_match", "VP-V > Motion > Advanced > Shaping; Tone Mapping > Profile details") +
                     row("dejudder_strength", "VP-V > Motion > Advanced > Set by the amount (and Bill's page)", typ="float", rng="0..1", default="0.5") +
                     row("intent_motion", "VP-V > Motion > Main > Per-shot easing", typ="enum") +
                     row("intent_motion_min", "VP-V > Advanced", typ="float", rng="0..1", default="0.5"))
        self.assertEqual(_run(["--root", self.root])[0], 0)

    def test_key_on_two_cards_is_an_error(self):
        _w(self.root, chk.ORG_REL, ORG.replace('{ K::Row, "dejudder_strength" }', '{ K::Row, "dejudder_strength" }, { K::Row, "intent_motion" }'))
        rc, out = _run(["--root", self.root])
        self.assertEqual(rc, 1)
        self.assertIn("key 'intent_motion' is on two cards", out)

    def test_motion_card_key_needs_one_owner_row(self):
        self.readmes(motion=row("dejudder_fade_match", "VP-V > Motion > Advanced > Shaping") +
                     row("dejudder_strength", "VP-V > Motion > Advanced > Set by the amount", typ="float", rng="0..1", default="0.5") +
                     row("intent_motion_min", "VP-V > Advanced", typ="float", rng="0..1", default="0.5"))
        rc, out = _run(["--root", self.root])
        self.assertEqual(rc, 1)
        self.assertIn("motion/easing: intent_motion has 0 owner rows", out)

    def test_intent_row_owned_outside_motion_is_an_error(self):
        # the intent_motion row moved to the repair module: owner unit differs from its Motion card
        self.readmes(motion=row("dejudder_fade_match", "VP-V > Motion > Advanced > Shaping") +
                     row("dejudder_strength", "VP-V > Motion > Advanced > Set by the amount", typ="float", rng="0..1", default="0.5") +
                     row("intent_motion_min", "VP-V > Advanced", typ="float", rng="0..1", default="0.5"),
                     repair=row("repair_key", "VP-V > Repair > Main > Deband") +
                     row("intent_motion", "VP-V > Motion > Main > Per-shot easing", typ="enum"))
        rc, out = _run(["--root", self.root])
        self.assertEqual(rc, 1)
        self.assertIn("intent_motion sits on a motion card but its owner row is in repair", out)

    def test_repair_card_differences_are_errors_since_p8_4(self):
        self.readmes(repair=row("repair_key", "VP-V > Advanced"))
        rc, out = _run(["--root", self.root])
        self.assertEqual(rc, 1, out)
        self.assertIn("repair_key is on card repair/deband", out)
        self.assertEqual(_run(["--root", self.root, "--write"])[0], 0)

    def test_units_not_yet_enforced_stay_advisory_until_all(self):
        # a Drive card (no Drive card exists in the repository: the unit is not enforced): a wrong placement is advisory until --all
        _w(self.root, chk.ORG_REL, ORG.replace('{ "repair", "deband"', '{ "drive", "deband"'))
        _w(self.root, C.TIERS_REL, json.dumps({"units": [{"id": "motion", "name": "Motion"}, {"id": "drive", "name": "Drive"}]}))
        self.mod("drive", "deband", ["repair_key"], row("repair_key", "VP-V > Advanced"))
        _w(self.root, "src/VP-V/modules/repair/deband/MANIFEST.json", "{}")
        os.remove(os.path.join(self.root, "src", "VP-V", "modules", "repair", "deband", "README.md"))
        rc, out = _run(["--root", self.root])
        self.assertEqual(rc, 0, out)
        self.assertIn("advisory: ", out)
        self.assertEqual(_run(["--root", self.root, "--all"])[0], 1)

    def test_fixes_container_key_off_every_card_is_an_error(self):
        _w(self.root, chk.CE_REL, CE + 'addRow("intent_motion_min");\n')
        rc, out = _run(["--root", self.root])
        self.assertEqual(rc, 1, out)
        self.assertIn("'intent_motion_min' is built in the Fixes under test container", out)

    def test_fixes_container_stays_list_accepts_and_goes_stale(self):
        _w(self.root, chk.CE_REL, CE + 'addRow("intent_motion_min");\n')
        saved = chk.FIXES_CONTAINER_STAYS
        try:
            chk.FIXES_CONTAINER_STAYS = ("intent_motion_min",)
            self.assertEqual(_run(["--root", self.root])[0], 0)
            chk.FIXES_CONTAINER_STAYS = ("intent_motion_min", "intent_motion")      # placed on a card now
            rc, out = _run(["--root", self.root])
            self.assertEqual(rc, 1)
            self.assertIn("FIXES_CONTAINER_STAYS: intent_motion is now on card", out)
            _w(self.root, chk.CE_REL, CE)                                      # container no longer builds it
            chk.FIXES_CONTAINER_STAYS = ("intent_motion_min",)
            rc, out = _run(["--root", self.root])
            self.assertEqual(rc, 1)
            self.assertIn("intent_motion_min is no longer built in the container", out)
        finally:
            chk.FIXES_CONTAINER_STAYS = saved

    def test_span_ends_at_a_form_reassignment(self):
        # repair_key is built after `form = processingForm;` inside the dejudder section's text: it is not that section's key
        _w(self.root, chk.CE_REL, CE.replace('addRow("dejudder_fade_match");', 'addRow("dejudder_fade_match");\nform = processingForm;\naddRow("intent_motion_min");'))
        rc, out = _run(["--root", self.root, "--list"])
        self.assertEqual(rc, 0, out)
        self.assertIn("dejudder_fade_match", out)
        self.assertNotIn("intent_motion_min  ", out)        # not placed by span inference

    def test_elsewhere_item_is_placed_on_its_card_and_checked(self):
        _w(self.root, chk.ORG_REL, ORG.replace('{ K::Row, "repair_key" }', '{ K::Row, "repair_key" }, { K::Elsewhere, "intent_motion_min" }'))
        rc, out = _run(["--root", self.root])
        self.assertEqual(rc, 1, out)                         # its row says "VP-V > Advanced", the card is Repair > Main > Deband
        self.assertIn("intent_motion_min is on card repair/deband", out)

    def test_missing_fixes_container_is_an_error(self):
        _w(self.root, chk.CE_REL, CE.split("form = addCollapsibleSection(QStringLiteral(\"fixesUnderTest\")")[0])
        rc, out = _run(["--root", self.root])
        self.assertEqual(rc, 1)
        self.assertIn("container 'fixesUnderTest' not found", out)

    def test_cross_unit_owner_table_names_modules_that_list_the_key(self):
        for key, slot in chk.CROSS_UNIT_OWNER.items():
            if key in chk.MANIFEST_EXEMPT:
                continue
            man = os.path.join(ROOT, "src", "VP-V", "modules", *slot.split("."), "MANIFEST.json")
            with open(man, encoding="utf-8") as fh:
                keys = json.load(fh).get("components", {}).get("config_keys", [])
            self.assertIn(key, keys, "%s is not listed by %s" % (key, slot))


class RepositoryTests(unittest.TestCase):
    def test_repository_motion_unit_is_one_table(self):
        rc, out = _run(["--root", ROOT])
        self.assertEqual(rc, 0, out)

    def test_repository_p8_5_output_enforced_and_adaptive_motion_rows_are_scaling_owned(self):
        self.assertIn("output", chk.ENFORCED_UNITS)
        an, cards, placement, issues = chk.analyse(ROOT)
        self.assertEqual(issues["error"] + issues["advisory"], [])
        owners = {}
        for r in an["rows"]:
            owners.setdefault(r["key"], []).append(r["slot"])
        for key in ("adaptive_motion", "adaptive_motion_floor", "adaptive_motion_smooth_ms",
                    "adaptive_motion_speed_hi", "adaptive_motion_speed_lo"):
            self.assertEqual(owners.get(key), ["scaling.output"], key)
            self.assertEqual(placement[key]["unit"], "scaling", key)
        self.assertIn("output.dither", owners["occlusion_mask"])
        self.assertEqual(placement["occlusion_mask"]["unit"], "output")

    def test_repository_enforces_the_p8_4_units_and_the_fixes_container(self):
        for unit in ("motion", "input", "colour", "repair", "scaling", "retirement"):
            self.assertIn(unit, chk.ENFORCED_UNITS)
        an, cards, placement, issues = chk.analyse(ROOT)
        self.assertEqual(issues["error"], [])
        for key in chk.FIXES_CONTAINER_STAYS:
            self.assertNotIn(key, placement, key)
        # the source-resolution rows are owned by input.source_analysis on their Scaling picture-slider cards
        for key in ("source_resolution", "source_resolution_crossfade_s"):
            self.assertEqual(chk.CROSS_UNIT_OWNER[key], "input.source_analysis")
            self.assertEqual(placement[key]["unit"], "scaling", key)

    def test_intent_motion_keys_are_owned_by_a_motion_module(self):
        an, cards, placement, issues = chk.analyse(ROOT)
        owners = {}
        for r in an["rows"]:
            owners.setdefault(r["key"], []).append(r["slot"])
        for key in ("intent_motion", "intent_motion_policy", "intent_motion_min", "intent_motion_min_shot_s", "intent_ease_s"):
            self.assertEqual(owners.get(key), ["motion.consumers"], key)
            self.assertEqual(placement[key]["unit"], "motion", key)

    def test_de_judder_slider_and_estimation_rows_have_owner_rows(self):
        an, cards, placement, issues = chk.analyse(ROOT)
        owned = {r["key"] for r in an["rows"]}
        for card in cards:
            if card["id"] in ("taste_keys", "tier_keys") and card["unit"] == "motion":
                for kind, ident in card["items"]:
                    if kind == "Row":
                        self.assertIn(ident, owned, ident)


if __name__ == "__main__":
    unittest.main()
