# Copyright(C) 2026 VP-V contributors
# SPDX-License-Identifier: GPL-3.0-only
"""Unit tests for tools/check_master_settings.py and tools/gen_master_settings.py (fixture trees)."""
import contextlib
import importlib.util
import io
import json
import os
import tempfile
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)


def _load(name):
    spec = importlib.util.spec_from_file_location(name, os.path.join(HERE, name + ".py"))
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


import sys  # noqa: E402
sys.path.insert(0, HERE)
C = _load("master_settings_common")
chk = _load("check_master_settings")
gen = _load("gen_master_settings")

HDR = ("| Key | Section | Type | Range / values | Default | Config placement | Original (Bill) "
       "| Fallbacks / status | Tiers |\n|---|---|---|---|---|---|---|---|---|\n")


def row(key, section="profile (vprenderer)", typ="enum", rng="on, off", default="`on`",
        placement="VP-V > Colour > x", original="n/a (VP-only)", status="none", tiers="same on all"):
    return "| `%s` | %s | %s | %s | %s | %s | %s | %s | %s |\n" % (
        key, section, typ, rng, default, placement, original, status, tiers)


def _w(root, rel, text):
    p = os.path.join(root, *rel.split("/"))
    os.makedirs(os.path.dirname(p), exist_ok=True)
    with open(p, "w", encoding="utf-8", newline="\n") as f:
        f.write(text)


def _run(mod, argv):
    out, err = io.StringIO(), io.StringIO()
    with contextlib.redirect_stdout(out), contextlib.redirect_stderr(err):
        rc = mod.main(argv)
    return rc, out.getvalue() + err.getvalue()


SCHEMA = '''
inline bool V(const std::string& key, const std::string& value) {
    if (key == "bill_key") return true;      // Bill's
    if (key == "vp_key") return IsChoice(value, { "on", "off" });
    if (key == "other_key") return true;
    // if (key == "commented_out") return true;
    return false;
}
'''


class Fixture(unittest.TestCase):
    def setUp(self):
        self.td = tempfile.TemporaryDirectory()
        self.root = self.td.name
        _w(self.root, C.SCHEMA_FILES[0], SCHEMA)
        _w(self.root, C.UPSTREAM_REL, "# header\nbill_key\n")
        _w(self.root, C.GOLDEN_REL, "# golden\n### case empty\nrule=x\nvp_key=on\nmatcher.a=1\nfp[000]=1\n### tier defaults\nother_key=off\n")
        self.mod("colour", "extended", 10, ["vp_key"])

    def tearDown(self):
        self.td.cleanup()

    def mod(self, unit, sub, order, keys, readme=None):
        d = "src/VP-V/modules/%s/%s" % (unit, sub)
        _w(self.root, d + "/MANIFEST.json", json.dumps({
            "slot": "%s.%s" % (unit, sub), "unit": unit, "order": order, "status": "built",
            "components": {"config_keys": keys}}))
        if readme is not None:
            _w(self.root, d + "/README.md", readme)
        return d

    def readme(self, body, heading="## Settings"):
        self.mod("colour", "extended", 10, ["vp_key"],
                 "# M\n\n%s\n\n%s\n\n## Tests\n\n- none\n" % (heading, body))

    def gen_and_baseline(self):
        self.assertEqual(_run(gen, ["--root", self.root])[0], 0)
        return _run(chk, ["--root", self.root, "--write-baseline"])


class ExtractionTests(unittest.TestCase):
    def test_schema_ignores_comments_and_collects_ctor_and_lists(self):
        text = ('if (key == "a") x; // key == "b"\n/* key == "c" */\n'
                'ConfigSchema::Boolean("d"), ConfigSchema::Choice("e", {"x"}),\n'
                'static const char* const vpvKeys[] = { "f", "g" };\n'
                'for (const char* key : { "h", "i" }) k = k;\n')
        self.assertEqual(C.extract_schema_keys(text), {"a", "d", "e", "f", "g", "h", "i"})

    def test_namespaced_section_keys_are_prefixed(self):
        text = ('constexpr const char* kSection = "vpv.skeleton";\n'
                'static const char* const kLaneKeys[] = { "lane", "chunk" };\n'
                'config.TryGetString(kSection, "camera_link", s);\n')
        self.assertEqual(C.extract_namespaced_keys(text),
                         {"vpv.skeleton.lane", "vpv.skeleton.chunk", "vpv.skeleton.camera_link"})

    def test_field_table_layout_after_move(self):
        text = '{ "new_key", Vpv::Kind::Bool, 0 }, { "second", Field::Int }, if (key == "valid") x;'
        self.assertEqual(C.extract_field_table_keys(text), {"new_key", "second", "valid"})

    def test_row_key_id(self):
        self.assertEqual(C.row_key_id("lane", "[vpv.skeleton]"), "vpv.skeleton.lane")
        self.assertEqual(C.row_key_id("k", "[vpv]"), "k")
        self.assertEqual(C.row_key_id("k", "profile (vprenderer)"), "k")

    def test_default_normalisation(self):
        n = C.norm_default
        self.assertEqual(n("`on`"), n("1"))
        self.assertEqual(n("0.50"), n("0.5"))
        self.assertNotEqual(n("on"), n("off"))
        self.assertIsNone(C.doc_default_literal("computed from the frame period"))
        self.assertEqual(C.doc_default_literal("`auto`"), "auto")


class CheckerTests(Fixture):
    def test_undocumented_key_is_a_gap_and_bill_key_is_out_of_scope(self):
        an = C.analyse(self.root)
        self.assertEqual(an["gaps"]["undocumented"], ["other_key", "vp_key"])
        self.assertIn("bill_key", an["code"])
        self.assertNotIn("bill_key", an["scope"])

    def test_both_field_table_layouts_are_scanned(self):
        _w(self.root, "src/VP-V/modules/control/config/VpvRendererFields.h", '{ "moved_key", Vpv::Kind::Bool }')
        self.assertIn("moved_key", C.analyse(self.root)["gaps"]["undocumented"])
        _w(self.root, "src/VP-V/config/VpvRendererFields.h", '{ "old_layout_key", Vpv::Kind::Bool }')
        self.assertIn("old_layout_key", C.analyse(self.root)["gaps"]["undocumented"])

    def test_documented_rows_close_the_gap(self):
        self.readme(HDR + row("vp_key") + row("other_key", default="`off`"))
        an = C.analyse(self.root)
        self.assertEqual(an["gaps"], {"undocumented": [], "stale": [], "default_mismatch": []})
        self.assertEqual(an["errors"], [])

    def test_stale_row_and_planned_exemption(self):
        self.readme(HDR + row("vp_key") + row("ghost_key") + row("future_key", status="planned: not wired"))
        an = C.analyse(self.root)
        self.assertEqual(an["gaps"]["stale"], ["ghost_key"])

    def test_default_mismatch_against_golden(self):
        self.readme(HDR + row("vp_key", default="`off`") + row("other_key", default="`on`"))
        an = C.analyse(self.root)
        self.assertEqual(an["gaps"]["default_mismatch"], ["other_key", "vp_key"])

    def test_non_literal_default_is_not_compared(self):
        self.readme(HDR + row("vp_key", default="from the tier (see Tiers)") + row("other_key", default="`off`"))
        self.assertEqual(C.analyse(self.root)["gaps"]["default_mismatch"], [])

    def test_fl35_row_is_a_hard_error(self):
        self.readme(HDR + row("vp_key", status="tuned on the FL35"))
        an = C.analyse(self.root)
        self.assertTrue(any("display" in e for e in an["errors"]))

    def test_duplicate_row_and_malformed_rows_are_errors(self):
        self.readme(HDR + row("vp_key") + row("vp_key") + "| `short` | a | b |\n")
        errs = C.analyse(self.root)["errors"]
        self.assertTrue(any("duplicate" in e for e in errs))
        self.assertTrue(any("cells" in e for e in errs))

    def test_bad_type_and_unquoted_key_are_errors(self):
        self.readme(HDR + row("vp_key", typ="number") + row("other_key").replace("`other_key`", "other_key"))
        errs = C.analyse(self.root)["errors"]
        self.assertTrue(any("type" in e for e in errs))
        self.assertTrue(any("backticks" in e for e in errs))

    def test_none_row_and_other_headings_ignored(self):
        self.readme(HDR + "| (none) | | | | | | | | |\n")
        self.assertEqual(C.analyse(self.root)["rows"], [])
        self.readme(HDR + row("vp_key"), heading="## Not settings")
        self.assertEqual(C.analyse(self.root)["rows"], [])

    def test_extra_settings_heading_variants_are_read(self):
        self.readme(HDR + row("vp_key"), heading="## Settings (declared, not wired)")
        self.assertEqual([r["key"] for r in C.analyse(self.root)["rows"]], ["vp_key"])


class RatchetTests(Fixture):
    def test_strict_requires_baseline_then_passes_then_fails_on_new_gap(self):
        self.assertEqual(_run(gen, ["--root", self.root])[0], 0)
        self.assertEqual(_run(chk, ["--root", self.root, "--strict"])[0], 1)   # no baseline yet
        rc, _ = _run(chk, ["--root", self.root, "--write-baseline"])
        self.assertEqual(rc, 0)
        self.assertEqual(_run(chk, ["--root", self.root, "--strict"])[0], 0)
        # a new key lands in code: strict fails, default run (report) does not, baseline refuses to rise
        _w(self.root, C.SCHEMA_FILES[0], SCHEMA.replace('"other_key"', '"other_key") || key == "added_key"'))
        self.assertEqual(_run(gen, ["--root", self.root])[0], 0)
        rc, out = _run(chk, ["--root", self.root, "--strict"])
        self.assertEqual(rc, 1)
        self.assertIn("NEW GAP undocumented: added_key", out)
        self.assertEqual(_run(chk, ["--root", self.root])[0], 0)
        rc, out = _run(chk, ["--root", self.root, "--write-baseline"])
        self.assertEqual(rc, 1)
        self.assertIn("refusing to raise", out)

    def test_baseline_shrinks_when_a_gap_is_documented(self):
        self.gen_and_baseline()
        base = json.loads(C.read_text(os.path.join(self.root, chk.BASELINE_REL)))
        self.assertEqual(base["counts"]["undocumented"], 2)
        self.readme(HDR + row("vp_key"))
        self.assertEqual(_run(gen, ["--root", self.root])[0], 0)
        rc, out = _run(chk, ["--root", self.root, "--strict"])
        self.assertEqual(rc, 0)
        self.assertIn("baseline can shrink", out)
        self.assertEqual(_run(chk, ["--root", self.root, "--write-baseline"])[0], 0)
        base = json.loads(C.read_text(os.path.join(self.root, chk.BASELINE_REL)))
        self.assertEqual(base["gaps"]["undocumented"], ["other_key"])

    def test_stale_master_list_fails(self):
        self.gen_and_baseline()
        self.readme(HDR + row("vp_key"))   # README changed, master not regenerated
        rc, out = _run(chk, ["--root", self.root])
        self.assertEqual(rc, 1)
        self.assertIn("out of date", out)
        self.assertEqual(_run(gen, ["--root", self.root, "--check"])[0], 1)

    def test_swapped_gap_is_still_new(self):
        self.gen_and_baseline()
        # drop other_key from code, add a different key: the count is the same but the set changed
        _w(self.root, C.SCHEMA_FILES[0], SCHEMA.replace('"other_key"', '"swapped_key"'))
        self.assertEqual(_run(gen, ["--root", self.root])[0], 0)
        rc, out = _run(chk, ["--root", self.root, "--strict"])
        self.assertEqual(rc, 1)
        self.assertIn("swapped_key", out)


class TryGetExtractionTests(Fixture):
    def test_tryget_literal_constant_variable_and_table_keys(self):
        lines = [
            'constexpr const char* DRIVE_SECTION = "led_drive.default";',
            'constexpr const char* kKey = "const_key";',
            'void f(){ c.TryGetString(section, "var_key", v); c.TryGetBool("ambient", "lit_key", b);',
            ' c.TryGetString(DRIVE_SECTION, kKey, v); c.TryGetString("vprenderer", "profile_key", v); }',
            'static const X t[] = { {"table_key", "Label", 0.0, 1.0} };',
        ]
        _w(self.root, "src/VP-V/modules/drive/x/Cfg.h", chr(10).join(lines) + chr(10))
        ids = set(C.extract_code_keys(self.root))
        self.assertTrue({"led_drive.var_key", "ambient.lit_key", "led_drive.const_key", "profile_key", "table_key"} <= ids, ids)

    def test_local_variable_named_section_is_not_a_constant(self):
        self.assertEqual(C.collect_constants('const std::string section = "event_actions.";'), {})
        self.assertEqual(C.collect_constants('constexpr const char* kSection = "vpv.x";'), {"kSection": "vpv.x"})

    def test_qualified_row_ids(self):
        self.assertEqual(C.row_key_id("k", "[led_drive.default]"), "led_drive.k")
        self.assertEqual(C.row_key_id("k", "[calibration]"), "calibration.k")


LED_FIXTURE = """
namespace LedDrive {
	constexpr const char* PROJECTOR_SECTION = "projector.default";
	constexpr const char* LED_DRIVE_SECTION = "led_drive.default";
	constexpr const char* DYNAMIC_DRIVE_SECTION = "dynamic_drive.default";
	inline std::vector<KeyRule> ProjectorRules()
	{
		return { Choice("protocol", { "a", "b" }), Integer("baud", 1, 2), Custom("port", [](const std::string&) { return true; }, "x") };
	}
	inline std::vector<KeyRule> LedDriveRules()
	{
		return {
			Boolean("enabled"),
			Custom("listen_port", [](const std::string&) { return true; }, "retired (ignored)"),
			Custom("curve", [](const std::string& v) { return true; }, "pairs"),
			Number("rate_up", 1.0, 2.0),
		};
	}
	inline ProjectorConfig ReadProjectorConfig(const ConfigFile& config)
	{
		const std::string section = PROJECTOR_SECTION;
		config.TryGetString(section, "echo", value);
		return result;
	}
	inline bool Validate(const ConfigFile& config, std::string& error)
	{
		if (led && config.TryGetString(section, "led_response_ms", moved)) { return false; }
		for (const char* key : { "rate_min_up", "rate_preset" }) names = config.TryGetString(section, key, value);
		return true;
	}
	inline void ApplyLedDriveSection(const ConfigFile& config, const std::string& section, LedDriveConfig& result)
	{
		auto readInt = [&](const char* key, int minimum, int maximum, int& target) { config.TryGetString(section, key, value); };
		readInt("tick_ms", 50, 5000, result.tickMs);
		readNumber("knee", result.knee);
		config.TryGetBool(section, "cut_jump", result.cutJump);
	}
}
"""


class SectionAttributionTests(Fixture):
    LED = "src/VP-V/modules/drive/led_compensation/LedDriveConfig.h"

    def ids(self):
        return set(C.extract_code_keys(self.root))

    def test_led_file_is_attributed_by_function_not_first_constant(self):          # F1
        _w(self.root, self.LED, LED_FIXTURE)
        ids = self.ids()
        self.assertTrue({"projector.protocol", "projector.baud", "projector.port", "projector.echo"} <= ids, ids)
        self.assertTrue({"led_drive.cut_jump", "led_drive.tick_ms", "led_drive.rate_min_up",
                         "led_drive.rate_preset", "led_drive.enabled"} <= ids, ids)
        self.assertFalse({k for k in ids if k.startswith("projector.") and k.split(".", 1)[1] in
                          {"cut_jump", "tick_ms", "enabled", "curve", "rate_up", "knee"}})

    def test_dynamic_drive_is_an_alias_of_led_drive(self):                         # F1
        self.assertEqual(C.qualify("dynamic_drive.default", "k"), "led_drive.k")
        self.assertEqual(C.qualify("led_drive.default", "k"), "led_drive.k")
        self.assertEqual(C.row_key_id("k", "[dynamic_drive.default]"), "led_drive.k")

    def test_spec_builders_and_read_lambdas_are_keys(self):                        # F2
        _w(self.root, self.LED, LED_FIXTURE)
        ids = self.ids()
        self.assertTrue({"led_drive.curve", "led_drive.rate_up", "led_drive.knee", "projector.port"} <= ids, ids)

    def test_retired_keys_are_not_live(self):                                      # F2
        _w(self.root, self.LED, LED_FIXTURE)
        self.assertNotIn("led_drive.listen_port", self.ids())
        self.assertNotIn("led_drive.led_response_ms", self.ids())
        retired = C.extract_retired_keys(self.root)
        self.assertEqual(set(retired), {"led_drive.listen_port", "led_drive.led_response_ms"})
        self.assertIn("## Appendix D", C.render_master(self.root))

    def test_builders_are_ignored_outside_mapped_files(self):                      # F2
        _w(self.root, "src/VP-V/modules/drive/x/Other.h", 'auto r = { Boolean("not_a_key"), Integer("nor_this", 1, 2) };\n')
        self.assertFalse({"not_a_key", "nor_this"} & self.ids())

    def test_guard_unmapped_builder_file_with_keyrule_is_an_error(self):          # F-A (a)
        _w(self.root, "src/VP-V/modules/x/y/NewConfig.h",
           'inline auto R(){ KeyRule r; return { Boolean("sneaky_key"), Integer("other_sneaky", 1, 2) }; }\n')
        probs = []
        C.extract_code_keys(self.root, probs)
        self.assertTrue(any("add src/VP-V/modules/x/y/NewConfig.h to SECTION_MAP" in p for p in probs), probs)
        self.assertTrue(any("NewConfig.h" in e and "SECTION_MAP" in e for e in C.analyse(self.root)["errors"]))

    def test_guard_unmapped_multi_section_variable_read_is_an_error(self):        # F-A (b)
        _w(self.root, "src/VP-V/modules/x/y/TwoConfig.h",
           'constexpr const char* A_SECTION = "a";\n' 'constexpr const char* B_SECTION = "b";\n'
           'void f(const std::string& section){ c.TryGetString(section, "which_one", v); }\n')
        probs = []
        C.extract_code_keys(self.root, probs)
        self.assertTrue(any("add src/VP-V/modules/x/y/TwoConfig.h to SECTION_MAP" in p for p in probs), probs)

    def test_guard_quiet_when_builder_keys_are_otherwise_extracted(self):         # F-A (a) negative
        _w(self.root, "src/VP-V/modules/x/y/OkConfig.h",
           'KeyRule r; auto s = Boolean("fine_key"); void f(){ c.TryGetBool("sec", "fine_key", v); }\n'
           'constexpr const char* A_SECTION = "a";\n' 'void g(){ c.TryGetBool(A_SECTION, "z_key", v); }\n')
        probs = []
        C.extract_code_keys(self.root, probs)
        self.assertEqual(probs, [])

    def test_section_qualified_keys_are_kept_next_to_bare_schema_keys(self):       # F3
        _w(self.root, "src/VP-V/modules/control/diag/StateReportSettings.h",
           'void f(){ c.TryGetBool("logging", "auto_report", a); c.TryGetBool("logging", "enabled", b); }\n')
        _w(self.root, C.SCHEMA_FILES[1],
           'inline bool V(const std::string& key){ if (key == "enabled") return true; return false; }\n'
           'inline auto R(){ return { ConfigSchema::Boolean("auto_report") }; }\n')
        ids = self.ids()
        self.assertIn("logging.enabled", ids)        # bare twin is Bill's (upstream or not): both kept
        self.assertIn("logging.auto_report", ids)
        self.assertNotIn("auto_report", ids)          # the schema file could not name a section: not a second key

    def test_ui_key_lists_without_a_section_make_no_keys(self):                    # F3
        _w(self.root, "src/VP-V/config/Ui.cpp", 'void f(){ for (const char* key : { "ui_a", "ui_b" }) g(key); }\n')
        _w(self.root, "src/VP-V/nets/NetPackage.cpp", 'void f(){ for (const char* key : { "b_structure", "c_structure" }) g(key); }\n')
        self.assertFalse({"ui_a", "ui_b", "b_structure", "c_structure"} & self.ids())

    def test_ambient_gets_its_own_section_and_no_bare_twins(self):                 # F3
        _w(self.root, "src/VP-V/modules/colour/appearance/AmbientConfig.h",
           'inline auto R(){ return { ConfigSchema::Choice("mode", {"off"}), ConfigSchema::Integer("baud", 1, 2) }; }\n'
           'void g(){ config.TryGetString("ambient", "room", raw); }\n')
        ids = self.ids()
        self.assertTrue({"ambient.mode", "ambient.baud", "ambient.room"} <= ids, ids)
        self.assertFalse({"mode", "baud", "room"} & ids)

    def test_file_local_constant_wins_over_same_name_elsewhere(self):              # F3
        _w(self.root, "src/VP-V/modules/a/A.h", 'constexpr const char* kSection = "vpv";\n')
        _w(self.root, "src/VP-V/modules/b/B.h",
           'constexpr const char* kSection = "vpv.b";\nvoid f(){ c.TryGetString(kSection, "camera_link", v); }\n')
        ids = self.ids()
        self.assertIn("vpv.b.camera_link", ids)
        self.assertNotIn("camera_link", ids)

    def test_kv_map_getkey_and_extra_file_reads(self):                             # F4
        _w(self.root, "src/VP-V/modules/repair/td/TemporalDetail.h",
           'void f(){ auto it = kv.find("temporal_detail"); auto g = kv.find("temporal_detail_boost");\n'
           ' number("temporal_detail_strength", 0.0, 1.0, x); }\n')
        _w(self.root, "src/VP-V/modules/output/dither/DitherNeed.h", 'bool p(){ return getKey("dither_need", raw); }\n')
        _w(self.root, "src/VideoProcessor-Lib/microsoft_directshow/MadVRShaderLoader.cpp",
           'constexpr const char* CONFIG_SECTION = "shaders";\nvoid f(){ config.TryGetString(section, "cinema_only", v);'
           ' config.TryGetString(section, "bill_key_here", v); }\n')
        ids = self.ids()
        self.assertTrue({"temporal_detail", "temporal_detail_boost", "temporal_detail_strength", "dither_need",
                         "shaders.cinema_only"} <= ids, ids)
        self.assertNotIn("shaders.bill_key_here", ids)


class HardeningTests(Fixture):
    def test_fenced_table_is_ignored_and_bad_header_is_an_error(self):
        self.readme('```' + chr(10) + HDR + row("vp_key") + '```')
        self.assertEqual(C.analyse(self.root)["rows"], [])
        self.readme(HDR.replace("Range / values", "Range") + row("vp_key"))
        self.assertTrue(any("header" in e for e in C.analyse(self.root)["errors"]))

    def test_planned_row_for_live_key_still_default_checked(self):
        self.readme(HDR + row("vp_key", default="`off`", status="planned") + row("other_key", default="`off`"))
        self.assertEqual(C.analyse(self.root)["gaps"]["default_mismatch"], ["vp_key"])

    def test_defaults_are_case_sensitive_except_switch_words(self):
        self.assertNotEqual(C.norm_default("Auto"), C.norm_default("auto"))
        self.assertEqual(C.norm_default("ON"), C.norm_default("1"))

    def test_baseline_count_mismatch_is_an_error(self):
        self.gen_and_baseline()
        bp = os.path.join(self.root, chk.BASELINE_REL)
        data = json.loads(C.read_text(bp))
        data["counts"]["undocumented"] = 0
        _w(self.root, chk.BASELINE_REL, json.dumps(data))
        rc, out = _run(chk, ["--root", self.root])
        self.assertEqual(rc, 1)
        self.assertIn("counts do not match", out)

    def test_bom_json_is_read(self):
        d = self.mod("colour", "bom", 20, ["vp_key"])
        with open(os.path.join(self.root, d, "MANIFEST.json"), "wb") as f:
            f.write(bytes([0xEF, 0xBB, 0xBF]) + json.dumps({"slot": "colour.bom", "unit": "colour"}).encode())
        self.assertIn("colour.bom", [m["slot"] for m in C.discover_modules(self.root)])


class GeneratorTests(Fixture):
    def test_output_is_deterministic_and_complete(self):
        self.readme(HDR + row("vp_key") + row("other_key", default="`off`"))
        a = C.render_master(self.root)
        b = C.render_master(self.root)
        self.assertEqual(a, b)
        self.assertTrue(a.startswith("<!-- GENERATED by tools/gen_master_settings.py"))
        for heading in ("## Modules index", "## Colour", "## Input", "## Motion", "## Repair", "## Scaling",
                        "## Dynamic drive", "## Output", "## Control", "Original (Bill)",
                        "## Appendix A", "## Appendix B"):
            self.assertIn(heading, a)
        self.assertIn("`vp_key`", a)
        self.assertNotRegex(a, r"(?i)fl\s*-?\s*35")

    def test_generator_refuses_owner_display(self):
        self.readme(HDR + row("vp_key", original="the FL35 panel"))
        rc, out = _run(gen, ["--root", self.root])
        self.assertEqual(rc, 1)
        self.assertIn("owner's display", out)

    def test_rows_sorted_regardless_of_readme_order(self):
        self.readme(HDR + row("vp_key") + row("other_key", default="`off`"))
        a = C.render_master(self.root)
        self.readme(HDR + row("other_key", default="`off`") + row("vp_key"))
        self.assertEqual(a, C.render_master(self.root))


@unittest.skipUnless(os.environ.get("VP_MS_REAL") == "1", "set VP_MS_REAL=1 to run the repository's own gate")
class RealRepoTests(unittest.TestCase):
    """The committed tree must satisfy its own gate."""

    def test_repo_is_strict_clean_and_master_current(self):
        rc, out = _run(chk, ["--root", ROOT, "--strict"])
        self.assertEqual(rc, 0, out)
        self.assertEqual(_run(gen, ["--root", ROOT, "--check"])[0], 0)


if __name__ == "__main__":
    unittest.main()
