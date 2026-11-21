# Copyright(C) 2026 VP-V contributors
# SPDX-License-Identifier: GPL-3.0-only
"""TQ-RG-P1: check_module_manifests.py had no tests. Each case builds a small synthetic repo (slots.json, module
folders with MANIFEST.json, source files) in a temp dir, points the checker's path globals at it, and asserts the
exact DRIFT line (or none). migration_path_map is replaced by a stub: its own check reads the real repository.
"""
import contextlib
import importlib.util
import io
import json
import os
import shutil
import sys
import tempfile
import types
import unittest

_dir = os.path.dirname(os.path.abspath(__file__))


def _load(name):
    s = importlib.util.spec_from_file_location(name, os.path.join(_dir, name + ".py"))
    m = importlib.util.module_from_spec(s)
    s.loader.exec_module(m)
    return m


M = _load("check_module_manifests")

SLOT = {"id": "unit.alpha", "order": 10, "status": "built", "settings_keys": ["a_key"], "planned_keys": [], "upstream": []}
LIVE = "src/VP-V/modules/unit/alpha/Alpha.h"


def manifest(slot="unit.alpha", **over):
    m = {"slot": slot, "order": 10, "status": "built", "code_location": "module", "live_files": [LIVE],
         "components": {"config_keys": ["a_key"], "upstream_modules": []}, "fallback_when_missing": "none"}
    m.update(over)
    return m


class Tree:
    """A synthetic repository root the checker is pointed at."""

    def __init__(self, test):
        self.root = tempfile.mkdtemp()
        test.addCleanup(shutil.rmtree, self.root, True)
        self.slots = [dict(SLOT)]
        self.write_slots()
        self.module("unit", "alpha", manifest())
        self.file(LIVE, "// alpha\n")

    def path(self, rel):
        return os.path.join(self.root, *rel.split("/"))

    def file(self, rel, text):
        os.makedirs(os.path.dirname(self.path(rel)), exist_ok=True)
        with open(self.path(rel), "w", encoding="utf-8", newline="\n") as f:
            f.write(text)

    def write_slots(self):
        self.file("docs/design/slots.json", json.dumps({"slots": self.slots}))

    def module(self, unit, slot, man, readme=True, tests=True):
        base = "src/VP-V/modules/%s/%s" % (unit, slot)
        self.file(base + "/MANIFEST.json", json.dumps(man))
        if readme:
            self.file(base + "/README.md", "# %s.%s\n" % (unit, slot))
        if tests:
            os.makedirs(self.path(base + "/tests"), exist_ok=True)

    def run(self, write=False):
        """(rc, output) of check(write) with the module globals pointing at this tree."""
        saved = (M.ROOT, M.SLOTS, M.MASTER, M.MODULES, sys.modules.get("migration_path_map"))
        M.ROOT = self.root
        M.SLOTS = self.path("docs/design/slots.json")
        M.MASTER = self.path("docs/design/components-master.json")
        M.MODULES = self.path("src/VP-V/modules")
        sys.modules["migration_path_map"] = types.SimpleNamespace(problems=lambda: [])
        out = io.StringIO()
        try:
            with contextlib.redirect_stdout(out):
                rc = M.check(write)
        finally:
            M.ROOT, M.SLOTS, M.MASTER, M.MODULES = saved[:4]
            if saved[4] is None:
                sys.modules.pop("migration_path_map", None)
            else:
                sys.modules["migration_path_map"] = saved[4]
        return rc, out.getvalue()

    def clean(self):
        """Write the master list, then a plain check must pass."""
        self.run(write=True)
        return self.run()


class ConsistentTree(unittest.TestCase):
    def test_a_consistent_tree_passes_after_the_master_is_written(self):
        rc, out = Tree(self).clean()
        self.assertEqual(rc, 0, out)
        self.assertIn("1 slots, 1 manifests, 0 problems", out)

    def test_write_creates_the_master_from_the_manifests(self):
        t = Tree(self)
        rc, out = t.run(write=True)
        self.assertEqual(rc, 0, out)
        with open(t.path("docs/design/components-master.json"), encoding="utf-8") as f:
            master = json.load(f)
        self.assertEqual([m["slot"] for m in master["modules"]], ["unit.alpha"])
        self.assertEqual(master["schema"], "vp-components-master/1")

    def test_a_missing_master_is_drift(self):
        rc, out = Tree(self).run()
        self.assertEqual(rc, 1)
        self.assertIn("DRIFT: components-master.json missing (run with --write)", out)

    def test_a_stale_master_is_drift(self):
        t = Tree(self)
        t.run(write=True)
        t.module("unit", "alpha", manifest(code_location="somewhere else"))
        rc, out = t.run()
        self.assertEqual(rc, 1)
        self.assertIn("components-master.json is out of date with the manifests", out)


class SlotAndManifestDrift(unittest.TestCase):
    def assertDrift(self, tree, fragment):
        rc, out = tree.clean()
        self.assertEqual(rc, 1, out)
        self.assertIn("DRIFT: " + fragment, out)

    def test_duplicate_slot_ids_and_orders(self):
        t = Tree(self)
        t.slots.append(dict(SLOT))
        t.write_slots()
        rc, out = t.clean()
        self.assertEqual(rc, 1)
        self.assertIn("slots.json: duplicate slot ids", out)
        self.assertIn("slots.json: duplicate order values", out)

    def test_missing_readme_manifest_and_tests(self):
        t = Tree(self)
        os.remove(t.path("src/VP-V/modules/unit/alpha/README.md"))
        shutil.rmtree(t.path("src/VP-V/modules/unit/alpha/tests"))
        rc, out = t.clean()
        self.assertEqual(rc, 1)
        self.assertIn("DRIFT: unit.alpha: missing README.md", out)
        self.assertIn("DRIFT: unit.alpha: missing tests", out)
        os.remove(t.path("src/VP-V/modules/unit/alpha/MANIFEST.json"))
        rc, out = t.run()
        self.assertIn("DRIFT: unit.alpha: missing MANIFEST.json", out)

    def test_manifest_fields_must_equal_slots_json(self):
        for key, value, fragment in (("order", 99, "unit.alpha: manifest order=99, slots.json has 10"),
                                     ("status", "draft", "unit.alpha: manifest status='draft', slots.json has 'built'")):
            with self.subTest(key=key):
                t = Tree(self)
                t.module("unit", "alpha", manifest(**{key: value}))
                self.assertDrift(t, fragment)

    def test_config_keys_and_upstream_must_equal_slots_json(self):
        t = Tree(self)
        t.module("unit", "alpha", manifest(components={"config_keys": ["a_key", "extra"], "upstream_modules": ["x.y"]}))
        rc, out = t.clean()
        self.assertEqual(rc, 1)
        self.assertIn("unit.alpha: manifest config_keys differ from slots.json", out)
        self.assertIn("unit.alpha: manifest upstream_modules differ from slots.json upstream", out)

    def test_unknown_upstream_slot(self):
        t = Tree(self)
        t.slots[0]["upstream"] = ["no.such"]
        t.write_slots()
        t.module("unit", "alpha", manifest(components={"config_keys": ["a_key"], "upstream_modules": ["no.such"]}))
        self.assertDrift(t, "unit.alpha: unknown upstream no.such")

    def test_a_missing_live_file(self):
        t = Tree(self)
        os.remove(t.path(LIVE))
        self.assertDrift(t, "unit.alpha: live file missing: " + LIVE)


class StrayFoldersAndDrafts(unittest.TestCase):
    def draft(self, **over):
        m = manifest(slot="unit.beta", status="draft", successor_of="unit.alpha", live_files=[])
        m.update(over)
        return m

    def test_a_folder_without_a_slot_and_without_a_manifest_is_drift(self):
        t = Tree(self)
        os.makedirs(t.path("src/VP-V/modules/unit/orphan"))
        rc, out = t.clean()
        self.assertEqual(rc, 1)
        self.assertIn("DRIFT: src/VP-V/modules/unit/orphan: no slot in slots.json", out)

    def test_a_non_draft_manifest_without_a_slot_is_drift(self):
        t = Tree(self)
        t.module("unit", "beta", manifest(slot="unit.beta"))
        rc, out = t.clean()
        self.assertEqual(rc, 1)
        self.assertIn("a folder without a slot must be a draft manifest", out)

    def test_a_valid_draft_passes(self):
        t = Tree(self)
        t.module("unit", "beta", self.draft())
        rc, out = t.clean()
        self.assertEqual(rc, 0, out)

    def test_a_draft_needs_an_existing_successor_readme_tests_and_its_own_slot_name(self):
        t = Tree(self)
        bad = self.draft()
        bad["successor_of"] = "no.such"
        bad["slot"] = "unit.other"
        t.module("unit", "beta", bad, readme=False, tests=False)
        rc, out = t.clean()
        self.assertEqual(rc, 1)
        for fragment in ("draft successor_of 'no.such' is not an existing slot id", "draft is missing README.md",
                         "draft is missing tests", "draft manifest slot='unit.other', want 'unit.beta'"):
            self.assertIn(fragment, out)

    def test_draft_problem_reports_a_missing_live_file(self):
        t = Tree(self)
        t.module("unit", "beta", self.draft(live_files=["src/VP-V/modules/unit/beta/Gone.h"]))
        rc, out = t.clean()
        self.assertEqual(rc, 1)
        self.assertIn("draft live file missing: src/VP-V/modules/unit/beta/Gone.h", out)


class DuplicateSourceAndForwarders(unittest.TestCase):
    def test_a_copy_of_a_moved_source_elsewhere_in_src_is_drift(self):
        t = Tree(self)
        t.file("src/VideoProcessor-Lib/Alpha.h", "// a legacy copy\n")
        rc, out = t.clean()
        self.assertEqual(rc, 1)
        self.assertIn("DRIFT: duplicate source: src\\VideoProcessor-Lib\\Alpha.h and src\\VP-V\\modules\\unit\\alpha\\Alpha.h"
                      if os.sep == "\\" else "DRIFT: duplicate source: src/VideoProcessor-Lib/Alpha.h", out)

    def test_a_listed_pure_forwarder_is_not_a_duplicate(self):
        t = Tree(self)
        t.module("unit", "alpha", manifest(forwarding_headers=["src/VideoProcessor-Lib/Alpha.h"]))
        t.file("src/VideoProcessor-Lib/Alpha.h",
               "// VP-V forwarding header (removed in M2)\n#pragma once\n#include <VP-V/modules/unit/alpha/Alpha.h>\n")
        rc, out = t.clean()
        self.assertEqual(rc, 0, out)

    def test_an_unlisted_forwarder_is_still_a_duplicate(self):
        t = Tree(self)
        t.file("src/VideoProcessor-Lib/Alpha.h",
               "// VP-V forwarding header\n#pragma once\n#include <VP-V/modules/unit/alpha/Alpha.h>\n")
        rc, out = t.clean()
        self.assertEqual(rc, 1)
        self.assertIn("duplicate source", out)

    def test_a_listed_forwarder_that_holds_more_than_the_include_is_drift(self):
        t = Tree(self)
        t.module("unit", "alpha", manifest(forwarding_headers=["src/VideoProcessor-Lib/Alpha.h"]))
        t.file("src/VideoProcessor-Lib/Alpha.h",
               "// VP-V forwarding header\n#pragma once\n#include <VP-V/modules/unit/alpha/Alpha.h>\nint extra;\n")
        rc, out = t.clean()
        self.assertEqual(rc, 1)
        self.assertIn("forwarding header src/VideoProcessor-Lib/Alpha.h: must contain only '#pragma once' and "
                      "'#include <VP-V/modules/unit/alpha/Alpha.h>'", out)

    def test_a_listed_forwarder_that_is_missing_or_unmarked(self):
        t = Tree(self)
        t.module("unit", "alpha", manifest(forwarding_headers=["src/VideoProcessor-Lib/Alpha.h"]))
        rc, out = t.clean()
        self.assertEqual(rc, 1)
        self.assertIn("unit.alpha: forwarding header missing: src/VideoProcessor-Lib/Alpha.h", out)
        t.file("src/VideoProcessor-Lib/Alpha.h", "#pragma once\n#include <VP-V/modules/unit/alpha/Alpha.h>\n")
        rc, out = t.run()
        self.assertIn("missing the marker 'VP-V forwarding header'", out)

    def test_forwarder_problem_without_a_moved_target(self):
        t = Tree(self)
        t.file("src/Legacy.h", "// VP-V forwarding header\n#pragma once\n")
        self.assertEqual(M.forwarder_problem(t.path("src/Legacy.h"), None), "no moved file of that name under src/VP-V/modules")


class HeaderMirror(unittest.TestCase):
    """PipelineSlots.h mirrors slots.json (id, order, status, origin) and its hook points give the zones."""

    HEADER = ('{ Id::Alpha, "unit.alpha", Side::Input, 10, HookPoint::Capture, Status::Built, Origin::Vp },\n')

    def test_a_matching_header_passes_and_a_differing_one_is_drift(self):
        t = Tree(self)
        t.slots[0]["origin"] = "vp"
        t.write_slots()
        t.file("src/VP-V/core/PipelineSlots.h", self.HEADER)
        rc, out = t.clean()
        self.assertEqual(rc, 0, out)
        t.file("src/VP-V/core/PipelineSlots.h", self.HEADER.replace("10,", "11,"))
        rc, out = t.run()
        self.assertEqual(rc, 1)
        self.assertIn("PipelineSlots.h vs slots.json: unit.alpha: (11, 'built', 'vp') != (10, 'built', 'vp')", out)

    def test_a_zone_that_disagrees_with_the_hook_point(self):
        t = Tree(self)
        t.slots[0]["origin"] = "vp"
        t.slots[0]["zone"] = "present"
        t.write_slots()
        t.file("src/VP-V/core/PipelineSlots.h", self.HEADER)
        rc, out = t.clean()
        self.assertEqual(rc, 1)
        self.assertIn("unit.alpha: zone present in slots.json, host in PipelineSlots.h", out)


class Repository(unittest.TestCase):
    def test_the_repository_passes_its_own_check(self):
        out = io.StringIO()
        with contextlib.redirect_stdout(out):
            rc = M.check(False)
        self.assertEqual(rc, 0, out.getvalue())
        self.assertIn(" 0 problems", out.getvalue())


if __name__ == "__main__":
    unittest.main()
