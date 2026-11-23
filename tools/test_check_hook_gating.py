# Copyright(C) 2026 VP-V contributors
# SPDX-License-Identifier: GPL-3.0-only
import contextlib
import importlib.util
import io
import json
import os
import re
import subprocess
import tempfile
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
spec = importlib.util.spec_from_file_location("check_hook_gating", os.path.join(HERE, "check_hook_gating.py"))
chg = importlib.util.module_from_spec(spec)
spec.loader.exec_module(chg)

BILL = "src/VideoProcessor-Lib/vprenderer/Renderer.cpp"
SLOTS = """\
{ Id::MotionEstimation, "motion.estimation", Side::Input, 50, HookPoint::Prepass, Status::Built, Origin::Vp },
{ Id::ScalingGeometry, "scaling.geometry", Side::Processing, 150, HookPoint::Libplacebo, Status::Built, Origin::Mixed },
"""


def _w(root, rel, text):
    p = os.path.join(root, *rel.split("/"))
    os.makedirs(os.path.dirname(p), exist_ok=True)
    with open(p, "w", encoding="utf-8") as f:
        f.write(text)


def _run(root, files=(BILL,), extra=()):
    buf = io.StringIO()
    with contextlib.redirect_stdout(buf):
        rc = chg.main(["--root", root, "--no-git", "--files"] + list(files) + list(extra))
    return rc, buf.getvalue()


_REAL_LOAD_H2_BASELINE = chg.load_h2_baseline


def _fixture_baseline_patch(case):
    """The temp-repo fixtures below test other gates (sites, triggers, guards, untagged references) and carry no
    hook-lines baseline. G-CC1 SHOULD-1 makes a missing baseline exit 2 in git mode; for these fixtures only, a missing
    baseline keeps its --no-git meaning (no verdict text, h2_ceiling alone). A present baseline (the real repo) is read
    unchanged. HonestH2MetricTest tests the real function, unpatched."""
    chg.load_h2_baseline = lambda tree, no_git=False: _REAL_LOAD_H2_BASELINE(tree, True)
    case.addCleanup(setattr, chg, "load_h2_baseline", _REAL_LOAD_H2_BASELINE)


def _bill(body):
    """A Bill file with one function whose body is `body` (the hook lines are tagged by the caller)."""
    return "#include <x.h>\nstruct Impl\n{\n\tPipelineSlots::Registry pipelineSlots;\n\tvoid Render()\n\t{\n" + body + "\t}\n};\n"


class CheckHookGatingTest(unittest.TestCase):
    def setUp(self):
        _fixture_baseline_patch(self)
        self.td = tempfile.TemporaryDirectory()
        self.root = self.td.name
        _w(self.root, chg.SLOTS_HEADER, SLOTS)

    def tearDown(self):
        self.td.cleanup()

    def test_clean_on_the_current_tree(self):
        buf = io.StringIO()
        with contextlib.redirect_stdout(buf):
            rc = chg.main(["--root", ROOT])
        out = buf.getvalue()
        self.assertEqual(rc, 0, out)
        self.assertIn(" 0 unguarded, 0 unverified markers", out)
        self.assertNotIn("stale allow-list entry", out, "an allow-list entry nothing uses must be deleted")

    def test_red_an_unguarded_vp_call_in_a_bill_file_fails_with_file_and_line(self):
        _w(self.root, BILL, _bill("\t\tvectorEngine.PlanAhead(frame);   // VPV-HOOK\n"))
        rc, out = _run(self.root)
        self.assertEqual(rc, 1, out)
        self.assertIn("UNGUARDED %s:7 " % BILL, out)
        self.assertIn("1 unguarded", out)

    def test_green_the_canonical_guards(self):
        cases = {
            "enclosing if": "\t\tif (!pipelineSlots.OriginalPathway())\n\t\t{   // VPV-HOOK\n\t\t\tvectorEngine.PlanAhead(frame);   // VPV-HOOK\n\t\t}   // VPV-HOOK\n",
            "brace-less if": "\t\tif (!pipelineSlots.OriginalPathway())   // VPV-HOOK\n\t\t\tvectorEngine.PlanAhead(frame);   // VPV-HOOK\n",
            "inline if": "\t\tif (!activeSettings.pipelineOriginalPathway) vectorEngine.PlanAhead(frame);   // VPV-HOOK\n",
            "conjunction": "\t\tif (luma && !pipelineSlots.OriginalPathway() && wanted)   // VPV-HOOK\n\t\t\tvectorEngine.PlanAhead(frame);   // VPV-HOOK\n",
            "outer pointer": "\t\tif (m_impl && !m_impl->pipelineSlots.OriginalPathway())   // VPV-HOOK\n\t\t\tMotionAhead::Collect(m_impl.get());   // VPV-HOOK\n",
            "Vp slot Runs": "\t\tif (pipelineSlots.Runs(PipelineSlots::Id::MotionEstimation))   // VPV-HOOK\n\t\t\tvectorEngine.PlanAhead(frame);   // VPV-HOOK\n",
            "early return": "\t\tif (pipelineSlots.OriginalPathway())\n\t\t\treturn;\n\t\tdoBill();\n\t\tvectorEngine.PlanAhead(frame);   // VPV-HOOK\n",
            "else of a positive check": "\t\tif (pipelineSlots.OriginalPathway())\n\t\t{\n\t\t\tdoBill();\n\t\t}\n\t\telse\n\t\t{   // VPV-HOOK\n\t\t\tvectorEngine.PlanAhead(frame);   // VPV-HOOK\n\t\t}   // VPV-HOOK\n",
            "short circuit": "\t\tconst bool drawn = !pipelineSlots.OriginalPathway() && DrawVpPhasePicture(frame);   // VPV-HOOK\n",
            "if short circuit with return": "\t\tif (!pipelineSlots.OriginalPathway() && VpStageBFrame(seq))   // VPV-HOOK\n\t\t\treturn true;\n",
        }
        for name, body in cases.items():
            with self.subTest(name=name):
                _w(self.root, BILL, _bill(body))
                rc, out = _run(self.root)
                self.assertEqual(rc, 0, out)

    def test_red_the_wrong_guards_do_not_count(self):
        cases = {
            "positive check guards nothing": "\t\tif (pipelineSlots.OriginalPathway())   // VPV-HOOK\n\t\t\tvectorEngine.PlanAhead(frame);   // VPV-HOOK\n",
            "disjunction": "\t\tif (!pipelineSlots.OriginalPathway() || late)   // VPV-HOOK\n\t\t\tvectorEngine.PlanAhead(frame);   // VPV-HOOK\n",
            "mixed slot": "\t\tif (pipelineSlots.Runs(PipelineSlots::Id::ScalingGeometry))   // VPV-HOOK\n\t\t\tvectorEngine.PlanAhead(frame);   // VPV-HOOK\n",
            "early return after the hook": "\t\tvectorEngine.PlanAhead(frame);   // VPV-HOOK\n\t\tif (pipelineSlots.OriginalPathway())\n\t\t\treturn;\n",
            "a non-pathway condition": "\t\tif (dejudder)   // VPV-HOOK\n\t\t\tdejudder->InvalidateSnapshot();   // VPV-HOOK\n",
        }
        for name, body in cases.items():
            with self.subTest(name=name):
                _w(self.root, BILL, _bill(body))
                rc, out = _run(self.root)
                self.assertEqual(rc, 1, out)

    def test_gated_callee_verified_by_its_early_return(self):
        _w(self.root, "src/VP-V/modules/motion/estimation/Lease.h",
           "struct State\n{\n\tbool EndLease(bool originalPathway)   // VPV-GATED\n\t{\n\t\tif (originalPathway) return false;\n\t\tReset();\n\t\treturn true;\n\t}\n};\n")
        _w(self.root, BILL, _bill("\t\tvectorEstimate.EndLease(pipelineSlots.OriginalPathway());   // VPV-HOOK\n"))
        rc, out = _run(self.root)
        self.assertEqual(rc, 0, out)
        self.assertIn("VPV-GATED EndLease (src/VP-V/modules/motion/estimation/Lease.h:3) verified", out)

    def test_red_a_gated_marker_without_the_early_return_is_a_hit(self):
        _w(self.root, "src/VP-V/modules/motion/estimation/Lease.h",
           "// VPV-GATED\nvoid EndLease(bool originalPathway)\n{\n\tif (dejudder) Reset();\n}\n")
        _w(self.root, BILL, _bill("\t\tEndLease(pipelineSlots.OriginalPathway());   // VPV-HOOK\n"))
        rc, out = _run(self.root)
        self.assertEqual(rc, 1, out)
        self.assertIn("UNVERIFIED VPV-GATED marker", out)
        self.assertIn("1 unguarded, 1 unverified markers", out)

    def _gated_body(self, body):
        _w(self.root, "src/VP-V/modules/motion/estimation/Lease.h",
           "struct State\n{\n\tbool EndLease(Ctx& c)   // VPV-GATED\n\t{\n" + body + "\t}\n};\n")
        _w(self.root, BILL, _bill("\t\tvectorEstimate.EndLease(ctx);   // VPV-HOOK\n"))
        return _run(self.root)

    def test_red_hg2_an_early_return_after_effects_is_unverified(self):
        for name, body in (
                ("calls first", "\t\tDoWork();\n\t\tResetAll();\n\t\tif (originalPathway) return false;\n\t\treturn true;\n"),
                ("conditional effect first", "\t\tif (x) DoWork();\n\t\tif (originalPathway) return false;\n\t\treturn true;\n"),
                ("block first", "\t\tif (x)\n\t\t{\n\t\t\tn = 1;\n\t\t}\n\t\tif (originalPathway) return false;\n\t\treturn true;\n"),
                ("assignment first", "\t\tcount = 0;\n\t\tif (originalPathway) return false;\n\t\treturn true;\n"),
                ("non-pathway call in a declaration", "\t\tconst bool originalPathway = DoWork(c.host);\n\t\tif (originalPathway) return false;\n\t\treturn true;\n"),
                ("pathway query plus an effect", "\t\tconst bool originalPathway = OriginalPathwayOf(c.host); DoWork();\n\t\tif (originalPathway) return false;\n\t\treturn true;\n")):
            with self.subTest(name):
                rc, out = self._gated_body(body)
                self.assertEqual(rc, 1, out)
                self.assertIn("UNVERIFIED VPV-GATED marker", out)
                self.assertIn("1 unguarded, 1 unverified markers", out)

    def test_hg2_guard_first_after_inert_declarations_is_verified(self):
        for name, body in (
                ("guard first", "\t\tif (originalPathway) return false;\n\t\tDoWork();\n\t\treturn true;\n"),
                ("after declarations without calls", "\t\tint n = 0;\n\t\tbool ok;\n\t\tif (originalPathway) return false;\n\t\tDoWork();\n\t\treturn true;\n"),
                ("braced return", "\t\tif (originalPathway)\n\t\t{\n\t\t\treturn false;\n\t\t}\n\t\tDoWork();\n\t\treturn true;\n"),
                ("predicate from the pathway query", "\t\tconst bool originalPathway = OriginalPathwayOf(c.host);\n\t\tif (originalPathway) return false;\n\t\tDoWork();\n\t\treturn true;\n"),
                ("predicate from the hub query", "\t\tconst bool originalPathway = Vpv::Hub::ExtensionHub::Instance().OriginalPathway(c.host);\n\t\tif (originalPathway) return false;\n\t\treturn true;\n")):
            with self.subTest(name):
                rc, out = self._gated_body(body)
                self.assertEqual(rc, 0, out)
                self.assertIn("VPV-GATED EndLease (src/VP-V/modules/motion/estimation/Lease.h:3) verified", out)

    def test_red_hg2_receiver_chains_in_the_predicate_declaration_are_unverified(self):
        # HG2-RECEIVER-CHAIN (gate C-T3): before the guard only the two house forms of the predicate declaration are
        # admitted (the plain OriginalPathwayOf and the hub instance, verified above); any other receiver, call or
        # qualifier could hide an effect inside the declaration. The previous pattern verified every one of these.
        for name, query in (
                ("a zero-argument call as the receiver", "ResetAll().OriginalPathway(c.host)"),
                ("a two-link receiver chain", "Flush()->Drain().OriginalPathway(c.host)"),
                ("another call on the hub class", "Vpv::Hub::ExtensionHub::Reset().OriginalPathway(c.host)"),
                ("an extra link after the hub instance", "Vpv::Hub::ExtensionHub::Instance().Flush().OriginalPathway(c.host)"),
                ("a foreign singleton", "Other::Instance().OriginalPathway(c.host)"),
                ("a call before the free form", "Make().OriginalPathwayOf(c.host)"),
                ("a member receiver", "hub->OriginalPathway(c.host)"),
                ("a qualified free form", "Other::OriginalPathwayOf(c.host)")):
            with self.subTest(name):
                rc, out = self._gated_body("\t\tconst bool originalPathway = %s;\n\t\tif (originalPathway) return false;\n"
                                           "\t\treturn true;\n" % query)
                self.assertEqual(rc, 1, out)
                self.assertIn("UNVERIFIED VPV-GATED marker", out)
                self.assertIn("1 unguarded, 1 unverified markers", out)

    def test_red_hg_same_named_markers_in_two_files_are_each_verified(self):
        # HG-GATED-NAME-COLLISION: the markers were keyed by the short name, so of two same-named VPV-GATED definitions in
        # two files the one read last hid the other (in the tree: OnQueue in DejudderExtension.cpp and in
        # VectorEngineExtension.cpp). Each definition is now verified and listed, and a call of the name is gated only when
        # every definition is. Both file orders run, so in one of them the hidden definition is the unverified one.
        bad = "struct A\n{\n\tbool EndLease(Ctx& c)   // VPV-GATED\n\t{\n\t\tDoWork();\n\t\tif (originalPathway) return false;\n\t\treturn true;\n\t}\n};\n"
        good = "struct B\n{\n\tbool EndLease(Ctx& c)   // VPV-GATED\n\t{\n\t\tif (originalPathway) return false;\n\t\tDoWork();\n\t\treturn true;\n\t}\n};\n"
        files = ("src/VP-V/modules/a/LeaseA.h", "src/VP-V/modules/b/LeaseB.h")
        _w(self.root, BILL, _bill("\t\tvectorEstimate.EndLease(ctx);   // VPV-HOOK\n"))
        for name, texts in (("the unverified definition in the first file", (bad, good)),
                            ("the unverified definition in the second file", (good, bad)),
                            ("both definitions verified", (good, good))):
            with self.subTest(name):
                for rel, text in zip(files, texts):
                    _w(self.root, rel, text)
                rc, out = _run(self.root)
                for rel, text in zip(files, texts):
                    self.assertIn("VPV-GATED EndLease (%s:3) %s" % (rel, "verified" if text is good else "NOT VERIFIED"), out)
                if bad in texts:
                    self.assertEqual(rc, 1, out)
                    self.assertIn("UNVERIFIED VPV-GATED marker (no `if (<original>) return` found) %s:3 EndLease"
                                  % files[texts.index(bad)], out)
                    self.assertIn("1 unguarded, 1 unverified markers", out)
                else:
                    self.assertEqual(rc, 0, out)
                    self.assertIn("0 unguarded, 0 unverified markers", out)

    def test_declarations_includes_and_context_fills_are_inert(self):
        body = ("\t\tRendererExtension::IngressContext ic; ic.host = this; ic.seq = static_cast<int64_t>(seq);   // VPV-HOOK\n"
                "\t\tif (RendererExtension::DecideIngress(ic) && ic.drop) return;   // VPV-HOOK\n"
                "\t\tMotionValidity::Pending pending;   // VPV-HOOK\n")
        text = ("#include <VP-V/x.h>   // VPV-HOOK\nnamespace Vpv { class Bridge; }   // VPV-HOOK\n"
                "struct Impl\n{\n\tVpv::Resync::HostBinding resyncBinding{ this, resync };   // VPV-HOOK\n"
                "\tvoid Ingress();   // VPV-HOOK\n\tPipelineSlots::Registry pipelineSlots;\n\tvoid Render()\n\t{\n" + body + "\t}\n};\n")
        _w(self.root, BILL, text)
        rc, out = _run(self.root)
        self.assertEqual(rc, 0, out)
        self.assertIn("allowed", out)

    def test_hook_files_non_pathway_files_and_tests_never_fail(self):
        _w(self.root, "src/VideoProcessor-Lib/vprenderer/RendererExtension.h", "// VPV-HOOK-FILE\ninline void Go() { vectorEngine.PlanAhead(); }\n")
        _w(self.root, "src/VideoProcessor-Config/ConfigEditorWindow.cpp", "void Build()\n{\n\tvpvPages_.append(pages_);   // VPV-HOOK\n}\n")
        _w(self.root, "src/VideoProcessor-Test/SomeTests.cpp", "void T()\n{\n\tVpv::PluginGuard::Attach(p);   // VPV-HOOK\n}\n")
        rc, out = _run(self.root, files=("src/VideoProcessor-Lib/vprenderer/RendererExtension.h",
                                         "src/VideoProcessor-Config/ConfigEditorWindow.cpp", "src/VideoProcessor-Test/SomeTests.cpp"))
        self.assertEqual(rc, 0, out)
        self.assertIn("non-pathway", out)

    def test_allow_list_entries_carry_a_reason_and_are_reported(self):
        for e in chg.ALLOW_CALLEES:
            self.assertGreater(len(e["reason"]), 60, e["callee"])
            self.assertIn("Proof", e["reason"], e["callee"])
        for e in chg.ALLOW_SITES:
            self.assertGreater(len(e["reason"]), 40, e["pattern"])
            self.assertTrue(os.path.isfile(os.path.join(ROOT, *e["file"].split("/"))), e["file"])
        _w(self.root, BILL, _bill("\t\tRendererExtension::NotifyQueue(RendererExtension::QueueEvent::Reset, m_impl.get());   // VPV-HOOK\n"))
        rc, out = _run(self.root)
        self.assertEqual(rc, 0, out)
        self.assertIn("allow-list callee [1 sites] RendererExtension::", out)

    def test_hook_lines_with_a_trailing_note_count_as_hooks(self):
        self.assertEqual(chg.hook_line_set("a   // VPV-HOOK: resync\nb   // VPV-HOOK M2 P5b\n// VPV-HOOK-END\nd\n"), {1, 2, 3})
        self.assertEqual(chg.hook_line_set("// VPV-HOOK\nx\n// VPV-HOOK-END\ny\n"), {1, 2, 3})

    def test_statement_parser_handles_strings_comments_and_lambdas(self):
        tree = chg.parse_statements(chg.strip_code(
            'void f()\n{\n\tauto g = [&](int a) { return a; };   // a { comment }\n\tLog("a ; { b", g(1));\n\tif (x) y();\n}\n'))
        f = tree.children[0]
        self.assertTrue(chg.is_function_header(f.text))
        texts = [c.text for c in f.children]
        self.assertIn('Log("", g(1));', texts)
        self.assertIn("if (x) y();", texts)

    def _seed(self, body):
        _w(self.root, BILL, _bill(body))
        buf = io.StringIO()
        with contextlib.redirect_stdout(buf):
            rc = chg.main(["--root", self.root, "--no-git", "--files", BILL, "--write-manifest", "--write-budget"])
        self.assertEqual(rc, 0, buf.getvalue())

    def test_sites_manifest_matches_whitespace_insensitively_and_counts_runs(self):
        self._seed("\t\tif (!pipelineSlots.OriginalPathway())   // VPV-HOOK\n\t\t\tvectorEngine.PlanAhead(frame);   // VPV-HOOK\n\t\tdoBill();\n\t\tint keep = 0;   // VPV-HOOK\n")
        manifest = chg.load_json(chg.Tree(self.root), chg.MANIFEST_PATH)
        self.assertEqual(len(manifest["sites"]), 2)
        self.assertEqual(manifest["sites"][0]["anchor"]["symbol"], "Render")
        self.assertEqual(chg.load_json(chg.Tree(self.root), chg.BUDGET_PATH)["files"][BILL], 2)
        # re-indented and re-commented: still the same two sites
        _w(self.root, BILL, _bill("\tif (!pipelineSlots.OriginalPathway())  // VPV-HOOK: moved\n\t\t\t\tvectorEngine.PlanAhead( frame );   // VPV-HOOK\n\t\tdoBill();\n\t\tint keep = 0;   // VPV-HOOK\n"))
        rc, out = _run(self.root)
        self.assertEqual(rc, 0, out)
        self.assertIn("2 sites, 0 new (0 approved), 0 removed, 0 over budget", out)

    def _regen_manifest(self):
        buf = io.StringIO()
        with contextlib.redirect_stdout(buf):
            rc = chg.main(["--root", self.root, "--no-git", "--files", BILL, "--write-manifest"])
        self.assertEqual(rc, 0, buf.getvalue())
        with open(os.path.join(self.root, *chg.MANIFEST_PATH.split("/")), encoding="utf-8") as fh:
            return fh.read()

    def _hand_edit_manifest(self):
        """Seed three sites (two with identical text), then edit the manifest the way a person does: rename one id,
        fill seam / expiry and a hand-added key, reorder the entries, back-date generated_at."""
        self._seed("\t\tint keep = 0;   // VPV-HOOK\n\t\tdoBill();\n\t\tint keep = 0;   // VPV-HOOK\n\t\tdoBill();\n"
                   "\t\tint other = 1;   // VPV-HOOK\n")
        p = os.path.join(self.root, *chg.MANIFEST_PATH.split("/"))
        with open(p, encoding="utf-8") as fh:
            doc = json.load(fh)
        self.assertEqual([s["id"] for s in doc["sites"]], ["R.cpp-0001", "R.cpp-0002", "R.cpp-0003"])
        doc["generated_at"] = "2000-01-01"
        doc["sites"][2]["id"] = "P5c-77"
        doc["sites"][2]["seam"], doc["sites"][2]["expiry"] = "S3", "re-graft"
        doc["sites"][1]["seam"], doc["sites"][1]["owner_note"] = "S1", "kept by hand"
        doc["sites"] = [doc["sites"][2], doc["sites"][0], doc["sites"][1]]
        with open(p, "w", encoding="utf-8") as fh:
            json.dump(doc, fh, indent=1)
            fh.write("\n")
        with open(p, encoding="utf-8") as fh:
            return fh.read()

    def test_regenerating_an_unchanged_tree_is_a_no_op_apart_from_generated_at(self):
        before = self._hand_edit_manifest()
        after = self._regen_manifest()
        changed = [(a, b) for a, b in zip(before.splitlines(), after.splitlines()) if a != b]
        self.assertEqual(len(before.splitlines()), len(after.splitlines()))
        self.assertEqual(len(changed), 1, changed)
        self.assertIn('"generated_at": "2000-01-01"', changed[0][0])
        self.assertNotIn("2000-01-01", changed[0][1])
        doc = json.loads(after)
        self.assertEqual([s["id"] for s in doc["sites"]], ["P5c-77", "R.cpp-0001", "R.cpp-0002"])
        self.assertEqual((doc["sites"][0]["seam"], doc["sites"][0]["expiry"]), ("S3", "re-graft"))
        self.assertEqual(doc["sites"][2]["owner_note"], "kept by hand")
        self.assertEqual(self._regen_manifest(), after, "a second regeneration changes nothing")

    def test_a_single_added_site_gives_a_single_added_entry(self):
        old = json.loads(self._hand_edit_manifest())["sites"]
        # appended after the existing sites: no line moves, so every old entry is identical except the Bill lines
        # after the last old site (its anchor), which now end at the new site
        _w(self.root, BILL, _bill("\t\tint keep = 0;   // VPV-HOOK\n\t\tdoBill();\n\t\tint keep = 0;   // VPV-HOOK\n\t\tdoBill();\n"
                                  "\t\tint other = 1;   // VPV-HOOK\n\t\tdoBill();\n\t\tint added = 2;   // VPV-HOOK\n"))
        mid = json.loads(self._regen_manifest())["sites"]
        self.assertEqual(mid[1:3], old[1:3])
        self.assertEqual({k: v for k, v in mid[0].items() if k != "anchor"}, {k: v for k, v in old[0].items() if k != "anchor"})
        self.assertEqual(mid[0]["id"], "P5c-77")
        self.assertEqual(len(mid), 4)
        self.assertEqual((mid[3]["id"], mid[3]["text"], mid[3]["seam"]), ("R.cpp-0004", "int added = 2;", None))
        # inserted before them: the old entries keep id, order and hand fields and only move; the new site's
        # positional id (R.cpp-0001) is taken, so it is renumbered past the highest in use
        _w(self.root, BILL, _bill("\t\tint first = 3;   // VPV-HOOK\n\t\tdoBill();\n\t\tint keep = 0;   // VPV-HOOK\n\t\tdoBill();\n"
                                  "\t\tint keep = 0;   // VPV-HOOK\n\t\tdoBill();\n\t\tint other = 1;   // VPV-HOOK\n\t\tdoBill();\n"
                                  "\t\tint added = 2;   // VPV-HOOK\n"))
        new = json.loads(self._regen_manifest())["sites"]
        self.assertEqual(len(new), 5)
        for a, b in zip(mid, new):
            self.assertEqual({k: v for k, v in a.items() if k not in ("start", "end", "anchor")},
                             {k: v for k, v in b.items() if k not in ("start", "end", "anchor")})
            self.assertEqual((b["start"], b["end"]), (a["start"] + 2, a["end"] + 2))
        self.assertEqual((new[4]["id"], new[4]["text"]), ("R.cpp-0005", "int first = 3;"))
        self.assertEqual(len(set(s["id"] for s in new)), 5)

    def test_a_removed_site_is_dropped_and_its_id_never_reused(self):
        self._hand_edit_manifest()
        # the second `keep` (R.cpp-0002) is removed and a new site takes its run index: its positional id
        # R.cpp-0002 belonged to the dropped entry, so it is renumbered
        _w(self.root, BILL, _bill("\t\tint keep = 0;   // VPV-HOOK\n\t\tdoBill();\n\t\tint added = 2;   // VPV-HOOK\n\t\tdoBill();\n"
                                  "\t\tint other = 1;   // VPV-HOOK\n"))
        new = json.loads(self._regen_manifest())["sites"]
        self.assertEqual([(s["id"], s["text"]) for s in new],
                         [("P5c-77", "int other = 1;"), ("R.cpp-0001", "int keep = 0;"), ("R.cpp-0003", "int added = 2;")])
        self.assertEqual(new[1]["seam"], None)

    def test_one_of_two_duplicates_removed_the_survivor_keeps_its_own_id(self):
        old = {s["id"]: s for s in json.loads(self._hand_edit_manifest())["sites"]}
        self.assertNotEqual(old["R.cpp-0001"]["anchor"], old["R.cpp-0002"]["anchor"])
        # the FIRST `keep` is removed (its Bill line stays): pairing in order would hand the survivor R.cpp-0001;
        # the anchor and the line say it is R.cpp-0002, which carries the hand-filled seam and key
        _w(self.root, BILL, _bill("\t\tdoBill();\n\t\tdoBill();\n\t\tint keep = 0;   // VPV-HOOK\n\t\tdoBill();\n"
                                  "\t\tint other = 1;   // VPV-HOOK\n"))
        new = json.loads(self._regen_manifest())["sites"]
        self.assertEqual([s["id"] for s in new], ["P5c-77", "R.cpp-0002"])
        self.assertEqual((new[1]["seam"], new[1]["owner_note"]), ("S1", "kept by hand"))
        # again from a fresh manifest, with the survivor also moved down a line: the anchor still decides
        os.remove(os.path.join(self.root, *chg.MANIFEST_PATH.split("/")))
        self._hand_edit_manifest()
        _w(self.root, BILL, _bill("\t\tbill1();\n\t\tdoBill();\n\t\tdoBill();\n\t\tint keep = 0;   // VPV-HOOK\n\t\tdoBill();\n"
                                  "\t\tint other = 1;   // VPV-HOOK\n"))
        new = json.loads(self._regen_manifest())["sites"]
        self.assertEqual([s["id"] for s in new], ["P5c-77", "R.cpp-0002"])

    def test_red_a_new_site_fails_until_a_hook_approval_names_it(self):
        self._seed("\t\tint keep = 0;   // VPV-HOOK\n")
        _w(self.root, BILL, _bill("\t\tint keep = 0;   // VPV-HOOK\n\t\tdoBill();\n\t\tif (!pipelineSlots.OriginalPathway()) vectorEngine.PlanAhead(frame);   // VPV-HOOK\n"))
        rc, out = _run(self.root)
        self.assertEqual(rc, 1, out)
        self.assertIn("NEW SITE", out)
        self.assertIn("OVER BUDGET %s 2 sites > budget 1" % BILL, out)
        _w(self.root, chg.APPROVALS_PATH, '{"approvals": [{"id": "HOOK-APPROVAL-1", "file": "%s", "text_contains": "vectorEngine.PlanAhead(frame)", "seam": "S2", "who": "coordinator", "when": "2026-11-11", "expiry": null}]}' % BILL)
        rc, out = _run(self.root)
        self.assertEqual(rc, 1, out)
        self.assertIn("approved by HOOK-APPROVAL-1", out)
        self.assertIn("1 new (1 approved)", out)
        self.assertIn("OVER BUDGET", out, "an approval does not raise the ceiling: the budget file does")
        _w(self.root, chg.BUDGET_PATH, '{"files": {"%s": 2}}' % BILL)
        rc, out = _run(self.root)
        self.assertEqual(rc, 0, out)

    def test_a_removed_site_is_reported_and_the_ceiling_ratchets_down(self):
        self._seed("\t\tint keep = 0;   // VPV-HOOK\n\t\tdoBill();\n\t\tint other = 1;   // VPV-HOOK\n")
        _w(self.root, BILL, _bill("\t\tint keep = 0;   // VPV-HOOK\n\t\tdoBill();\n"))
        rc, out = _run(self.root)
        self.assertEqual(rc, 0, out)
        self.assertIn("1 removed", out)
        self.assertIn("ratchet the ceiling down", out)

    def test_since_a_merge_base_reports_only_added_lines_and_base_ref_refuses_our_tips(self):
        buf = io.StringIO()
        with contextlib.redirect_stdout(buf):
            rc = chg.main(["--root", ROOT, "--since", "HEAD"])
        out = buf.getvalue()
        self.assertEqual(rc, 0, out)
        self.assertIn("only lines added since HEAD", out)
        self.assertIn(" 0 unguarded, 0 unverified markers", out)
        self.assertIn(" 0 untagged", out)
        with self.assertRaises(SystemExit) as cm:
            with contextlib.redirect_stdout(io.StringIO()):
                chg.main(["--root", ROOT, "--base-ref", "HEAD"])
        self.assertIn("not Bill's baseline", str(cm.exception))

    def test_the_real_manifest_and_budget_match_the_tree(self):
        buf = io.StringIO()
        with contextlib.redirect_stdout(buf):
            rc = chg.main(["--root", ROOT])
        out = buf.getvalue()
        self.assertEqual(rc, 0, out)
        self.assertIn(" 0 new (0 approved), 0 removed, 0 over budget; manifest docs/audit/hook-sites.json, budget docs/audit/hook-budget.json", out)
        self.assertNotIn("ratchet the ceiling down", out, "regenerate docs/audit/hook-budget.json with --write-budget")
        self.assertNotIn("removed sites", out, "regenerate docs/audit/hook-sites.json with --write-manifest")


    # --- transitional approvals: expiry triggers and temporary ceilings (refs/107 s5.2; Lead 4 2026-11-11) ---

    SITE = "\t\tint keep = 0;   // VPV-HOOK\n\t\tdoBill();\n\t\tif (!pipelineSlots.OriginalPathway()) vectorEngine.PlanAhead(frame);   // VPV-HOOK\n"

    def _transitional(self, expiry, triggers):
        """Two known sites; the second carries an approval with `expiry`; hook-triggers.json = `triggers`."""
        self._seed(self.SITE)
        _w(self.root, chg.APPROVALS_PATH, json.dumps({"approvals": [{"id": "HA-T1", "file": BILL, "text_contains": "vectorEngine.PlanAhead(frame)",
                                                                     "seam": "S3", "approver": "owner via coordinator", "when": "2026-11-11",
                                                                     "expiry": expiry}]}))
        _w(self.root, chg.TRIGGERS_PATH, json.dumps(triggers))

    def test_expiry_not_landed_passes(self):
        self._transitional("P4-e", {"_note": "x", "P4-e": {"landed": None}})
        rc, out = _run(self.root)
        self.assertEqual(rc, 0, out)
        self.assertIn("; 0 expired, 0 unknown triggers", out)

    def test_red_expiry_landed_by_json_with_the_site_present_fails(self):
        self._transitional("P4-e", {"P4-e": {"landed": "abc123"}})
        rc, out = _run(self.root)
        self.assertEqual(rc, 1, out)
        self.assertIn("EXPIRED HA-T1 (trigger P4-e landed at abc123) still present", out)
        self.assertIn("; 1 expired, 0 unknown triggers", out)

    def test_expiry_landed_with_the_site_removed_passes(self):
        self._transitional("P4-e", {"P4-e": {"landed": "abc123"}})
        _w(self.root, BILL, _bill("\t\tint keep = 0;   // VPV-HOOK\n\t\tdoBill();\n"))
        rc, out = _run(self.root)
        self.assertEqual(rc, 0, out)
        self.assertIn("; 0 expired, 0 unknown triggers", out)
        self.assertIn("1 removed", out)

    def test_red_expiry_naming_an_unknown_trigger_fails(self):
        self._transitional("P9-nowhere", {"P4-e": {"landed": None}})
        rc, out = _run(self.root)
        self.assertEqual(rc, 1, out)
        self.assertIn("UNKNOWN TRIGGER approval HA-T1 names trigger P9-nowhere", out)
        self.assertIn("; 0 expired, 1 unknown triggers", out)

    def test_list_expiry_is_any_of(self):
        self._transitional(["P5c-extras", "P6-move"], {"P5c-extras": {"landed": None}, "P6-move": {"landed": None}})
        rc, out = _run(self.root)
        self.assertEqual(rc, 0, out)
        _w(self.root, chg.TRIGGERS_PATH, json.dumps({"P5c-extras": {"landed": None}, "P6-move": {"landed": "def456"}}))
        rc, out = _run(self.root)
        self.assertEqual(rc, 1, out)
        self.assertIn("EXPIRED HA-T1 (trigger P6-move landed at def456) still present", out)
        _w(self.root, chg.TRIGGERS_PATH, json.dumps({"P5c-extras": {"landed": "aaa111"}, "P6-move": {"landed": None}}))
        rc, out = _run(self.root)
        self.assertEqual(rc, 1, out)
        self.assertIn("EXPIRED HA-T1 (trigger P5c-extras landed at aaa111) still present", out)

    def test_red_expiry_landed_by_a_commit_trailer_fails(self):
        self._transitional("P4-e", {"P4-e": {"landed": None}})
        env = dict(os.environ, GIT_CONFIG_NOSYSTEM="1", GIT_AUTHOR_NAME="t", GIT_AUTHOR_EMAIL="t@example.invalid",
                   GIT_COMMITTER_NAME="t", GIT_COMMITTER_EMAIL="t@example.invalid")
        for args in (["init", "-q"], ["add", "-A"], ["commit", "-q", "-m", "base"],
                     ["commit", "-q", "--allow-empty", "-m", "P4 e lands\n\nHook-Trigger: P4-e"]):
            subprocess.run(["git", "-C", self.root] + args, check=True, env=env, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        known, landed = chg.landed_triggers(chg.Tree(self.root), self.root, "HEAD", no_git=False)
        self.assertIn("P4-e", landed)
        self.assertEqual(chg.landed_triggers(chg.Tree(self.root), self.root, "HEAD~1", no_git=False)[1], {}, "not an ancestor of HEAD~1")
        res = chg.run(self.root, chg.DEFAULT_BASE, [BILL], no_git=True)
        sc = chg.site_checks(chg.Tree(self.root), res["files"], self.root, "HEAD", no_git=False)
        self.assertEqual(len(sc["expired"]), 1, sc)
        self.assertIn("EXPIRED HA-T1 (trigger P4-e landed at %s) still present" % landed["P4-e"], sc["expired"][0])
        sc = chg.site_checks(chg.Tree(self.root), res["files"], self.root, "HEAD~1", no_git=False)
        self.assertEqual(sc["expired"], [])

    def test_temporary_budget_holds_until_all_its_triggers_land(self):
        self._transitional(None, {"P4-e": {"landed": None}, "P4-f": {"landed": None}})
        budget = {"files": {BILL: 1}, "temporary": {BILL: {"limit": 2, "reverts_to": 1, "until": ["P4-e", "P4-f"]}}}
        _w(self.root, chg.BUDGET_PATH, json.dumps(budget))
        rc, out = _run(self.root)
        self.assertEqual(rc, 0, out)
        self.assertIn("0 over budget", out)
        self.assertIn("; temporary Renderer.cpp 2 <= 2 (reverts to 1)", out)
        self.assertIn("temporary ceiling %s 2 (reverts to 1 when P4-e, P4-f land)" % BILL, out)
        _w(self.root, chg.TRIGGERS_PATH, json.dumps({"P4-e": {"landed": "abc123"}, "P4-f": {"landed": None}}))
        rc, out = _run(self.root)
        self.assertEqual(rc, 0, out)
        self.assertIn("reverts to 1 when P4-f land", out)
        _w(self.root, chg.TRIGGERS_PATH, json.dumps({"P4-e": {"landed": "abc123"}, "P4-f": {"landed": "abc124"}}))
        rc, out = _run(self.root)
        self.assertEqual(rc, 1, out)
        self.assertIn("OVER BUDGET %s 2 sites > budget 1" % BILL, out)
        self.assertNotIn("temporary ceiling", out)
        budget["temporary"][BILL]["until"] = ["P4-e", "P9-nowhere"]
        _w(self.root, chg.BUDGET_PATH, json.dumps(budget))
        rc, out = _run(self.root)
        self.assertEqual(rc, 1, out)
        self.assertIn("UNKNOWN TRIGGER temporary budget %s names trigger P9-nowhere" % BILL, out)


    # --- a missed trigger never passes silently (Lead 4 opus gate 2026-11-11; no-silent-fallbacks) ---

    GIT_ENV = dict(os.environ, GIT_CONFIG_NOSYSTEM="1", GIT_AUTHOR_NAME="t", GIT_AUTHOR_EMAIL="t@example.invalid",
                   GIT_COMMITTER_NAME="t", GIT_COMMITTER_EMAIL="t@example.invalid")

    def _git(self, *args):
        return subprocess.run(["git", "-C", self.root] + list(args), check=True, env=self.GIT_ENV,
                              stdout=subprocess.PIPE, stderr=subprocess.PIPE).stdout.decode("utf-8", "replace").strip()

    def _repo_with(self, message):
        """A repo with the transitional fixture as its base commit, then an empty commit with `message`."""
        self._transitional("P4-e", {"P4-e": {"landed": None}, "P4-f": {"landed": None}})
        self._git("init", "-q")
        self._git("add", "-A")
        self._git("commit", "-q", "-m", "base")
        self._git("commit", "-q", "--allow-empty", "-m", message)
        return chg.trigger_scan(chg.Tree(self.root), self.root, "HEAD", no_git=False)

    def test_hook_trigger_in_the_body_text_is_not_a_landing(self):
        t = self._repo_with("P4 e work\n\nHook-Trigger: P4-e\nis mentioned here, in the body.\n\nMore body text.\n\n"
                            "Co-Authored-By: t <t@example.invalid>")
        self.assertEqual(t["landed"], {}, t)
        self.assertEqual(t["errors"], [], t)

    def test_trailer_key_and_value_match_in_any_case(self):
        t = self._repo_with("P4 e lands\n\nhook-trigger:  p4-E ,P4-F")
        sha = self._git("rev-parse", "--short=12", "HEAD")
        self.assertEqual(t["landed"], {"P4-e": sha, "P4-f": sha}, t)
        self.assertEqual(t["errors"], [], t)

    def test_trailer_in_both_layouts_standard_block_and_own_paragraph_before_coauthored_by(self):
        co = "Co-Authored-By: t <t@example.invalid>"
        for name, msg in (("standard block", "P4 e lands\n\nbody\n\nHook-Trigger: P4-e\n" + co),
                          ("own paragraph (867594f19)", "P4 e lands\n\nbody\n\nHook-Trigger: P4-e\n\n" + co)):
            with self.subTest(name):
                t = self._repo_with(msg)
                sha = self._git("rev-parse", "--short=12", "HEAD")
                self.assertEqual(t["landed"], {"P4-e": sha}, t)
                self.assertEqual(t["errors"], [], t)
            self.tearDown()
            self.setUp()
        self.assertEqual(chg.trailer_values("s\n\nb\n\nHook-Trigger: P4-e\n\n" + co), ["P4-e"])
        self.assertEqual(chg.trailer_values("s\n\nHook-Trigger: P4-e\nmentioned in the body.\n\nMore.\n\n" + co), [])
        self.assertEqual(chg.trailer_values("Hook-Trigger: P4-e"), [], "the subject never counts")

    def test_red_a_landed_trigger_not_in_the_json_fails(self):
        t = self._repo_with("lands\n\nHook-Trigger: P4-e, P9-nowhere")
        sha = self._git("rev-parse", "--short=12", "HEAD")
        self.assertEqual(t["errors"], ["UNKNOWN LANDED TRIGGER P9-nowhere at %s" % sha], t)
        self.assertEqual(t["landed"], {"P4-e": sha})
        res = chg.run(self.root, chg.DEFAULT_BASE, [BILL], no_git=True)
        sc = chg.site_checks(chg.Tree(self.root), res["files"], self.root, "HEAD", no_git=False)
        self.assertIn("UNKNOWN LANDED TRIGGER P9-nowhere at %s" % sha, sc["trigger_errors"])

    def test_red_git_history_unavailable_fails_and_no_git_says_json_only(self):
        self._repo_with("nothing lands")
        t = chg.trigger_scan(chg.Tree(self.root), self.root, "no-such-rev-r2p5c", no_git=False)
        self.assertEqual(len(t["errors"]), 1, t)
        self.assertTrue(t["errors"][0].startswith("TRIGGER SOURCE FAILED: git history of no-such-rev-r2p5c is unavailable"), t)
        self.assertEqual(t["source"], "json + trailers FAILED")
        res = chg.run(self.root, chg.DEFAULT_BASE, [BILL], no_git=True)
        res["site_checks"] = chg.site_checks(chg.Tree(self.root), res["files"], self.root, "no-such-rev-r2p5c", no_git=False)
        res["rc"] = 1 if res["site_checks"]["trigger_errors"] else res["rc"]
        out = chg.render(res)
        self.assertIn("  TRIGGER SOURCE FAILED: git history of no-such-rev-r2p5c", out)
        self.assertIn("1 trigger errors", out)
        self.assertIn("FAIL (rule 32 expiry inputs)", out)
        rc, out = _run(self.root)
        self.assertEqual(rc, 0, out)
        self.assertIn("; trigger source: json only", out)

    def test_red_an_approval_whose_file_is_missing_fails(self):
        self._transitional("P4-e", {"P4-e": {"landed": "abc123"}})
        _w(self.root, chg.APPROVALS_PATH, json.dumps({"approvals": [{"id": "HA-T1", "file": "src/VideoProcessor-Lib/vprenderer/Moved.cpp",
                                                                     "text_contains": "vectorEngine.PlanAhead(frame)", "seam": "S3",
                                                                     "approver": "owner via coordinator", "when": "2026-11-11", "expiry": "P4-e"}]}))
        rc, out = _run(self.root)
        self.assertEqual(rc, 1, out)
        self.assertIn("  APPROVAL FILE MISSING HA-T1 src/VideoProcessor-Lib/vprenderer/Moved.cpp", out)
        self.assertIn("1 approval files missing", out)
        self.assertIn("FAIL (rule 32 expiry inputs)", out)

    def test_red_two_identical_sites_with_one_removed_still_expire(self):
        twin = "\t\tif (!pipelineSlots.OriginalPathway()) vectorEngine.PlanAhead(frame);   // VPV-HOOK\n"
        self._seed(twin + "\t\tdoBill();\n" + twin)
        _w(self.root, chg.APPROVALS_PATH, json.dumps({"approvals": [{"id": "HA-T1", "file": BILL, "text_contains": "vectorEngine.PlanAhead(frame)",
                                                                     "seam": "S3", "approver": "owner via coordinator", "when": "2026-11-11",
                                                                     "expiry": "P4-e"}]}))
        _w(self.root, chg.TRIGGERS_PATH, json.dumps({"P4-e": {"landed": "abc123"}}))
        _w(self.root, BILL, _bill("\t\tdoBill();\n" + twin))
        rc, out = _run(self.root)
        self.assertEqual(rc, 1, out)
        self.assertIn("EXPIRED HA-T1 (trigger P4-e landed at abc123) still present", out)
        _w(self.root, BILL, _bill("\t\tdoBill();\n"))
        rc, out = _run(self.root)
        self.assertEqual(rc, 0, out)
        self.assertIn("; 0 expired, 0 unknown triggers", out)

    def test_end_to_end_cli_with_rev_and_a_real_trailer_commit(self):
        _w(self.root, BILL, _bill("\t\tdoBill();\n"))
        self._git("init", "-q")
        self._git("add", BILL)
        self._git("commit", "-q", "-m", "bill")
        base = self._git("rev-parse", "HEAD")
        self._transitional("P4-e", {"P4-e": {"landed": None}})
        self._git("add", "-A")
        self._git("commit", "-q", "-m", "vp")
        self._git("commit", "-q", "--allow-empty", "-m", "P4 e lands\n\nHook-Trigger: P4-e")
        sha = self._git("rev-parse", "--short=12", "HEAD")
        def cli(rev):
            buf = io.StringIO()
            with contextlib.redirect_stdout(buf):
                rc = chg.main(["--root", self.root, "--base-ref", base, "--rev", rev, "--files", BILL])
            return rc, buf.getvalue()
        rc, out = cli("HEAD")
        self.assertEqual(rc, 1, out)
        self.assertIn("EXPIRED HA-T1 (trigger P4-e landed at %s) still present" % sha, out)
        self.assertIn("; trigger source: json + trailers of HEAD", out)
        rc, out = cli("HEAD~1")
        self.assertEqual(rc, 0, out)
        self.assertIn("; 0 expired, 0 unknown triggers, 0 trigger errors, 0 approval files missing, 0 budget errors", out)
        self.assertIn("; trigger source: json + trailers of HEAD~1", out)

    def test_red_temporary_reverts_to_above_the_files_ceiling_fails(self):
        self._transitional(None, {"P4-e": {"landed": None}})
        _w(self.root, chg.BUDGET_PATH, json.dumps({"files": {BILL: 1}, "temporary": {BILL: {"limit": 2, "reverts_to": 2, "until": ["P4-e"]}}}))
        rc, out = _run(self.root)
        self.assertEqual(rc, 1, out)
        self.assertIn("  BUDGET INCONSISTENT temporary %s reverts_to 2 > files 1" % BILL, out)
        self.assertIn("1 budget errors", out)
        _w(self.root, chg.BUDGET_PATH, json.dumps({"files": {BILL: 1}, "temporary": {BILL: {"limit": 2, "reverts_to": 1, "until": ["P4-e"]}}}))
        rc, out = _run(self.root)
        self.assertEqual(rc, 0, out)
        self.assertIn("0 budget errors", out)

    def _gate_numbers(self, out):
        import re
        classes = re.search(r"^  classes \(every bucket incl\. tests, whole files, before the --since filter; not the gate\): (.*)$", out, re.M)
        gate = re.search(r"^  gate: (\d+) unguarded \(production files", out, re.M)
        final = re.search(r"^hook gating: \d+ hook statements, (\d+) unguarded,", out, re.M)
        self.assertTrue(classes and gate and final, out)
        return classes.group(1), int(gate.group(1)), int(final.group(1))

    def test_the_breakdown_and_the_final_line_report_one_gate_count(self):
        tests_file = "src/VideoProcessor-Test/SomeTests.cpp"
        _w(self.root, BILL, _bill("\t\tif (!pipelineSlots.OriginalPathway()) vectorEngine.PlanAhead(frame);   // VPV-HOOK\n"))
        _w(self.root, tests_file, "void T()\n{\n\tvectorEngine.PlanAhead(frame);   // VPV-HOOK\n}\n")
        rc, out = _run(self.root, files=(BILL, tests_file))
        self.assertEqual(rc, 0, out)
        classes, gate, final = self._gate_numbers(out)
        self.assertIn("unguarded 1", classes, "the breakdown counts the tests file too, and says so")
        self.assertEqual((gate, final), (0, 0))
        _w(self.root, BILL, _bill("\t\tvectorEngine.PlanAhead(frame);   // VPV-HOOK\n"))
        rc, out = _run(self.root, files=(BILL, tests_file))
        self.assertEqual(rc, 1, out)
        classes, gate, final = self._gate_numbers(out)
        self.assertIn("unguarded 2", classes)
        self.assertEqual((gate, final), (1, 1))

    # --- a pending temporary ceiling ratchets against its base (Lead 4 ruling 2026-11-11, option a) ---

    def test_pending_temporary_with_headroom_is_informational(self):
        self._transitional(None, {"re-graft": {"landed": None}})
        _w(self.root, chg.BUDGET_PATH, json.dumps({"files": {BILL: 2}, "temporary": {BILL: {"limit": 3, "reverts_to": 2, "until": ["re-graft"]}}}))
        rc, out = _run(self.root)
        self.assertEqual(rc, 0, out)
        self.assertIn("  temporary headroom %s 2 < 3 (pending re-graft)" % BILL, out)
        self.assertNotIn("ratchet the ceiling down", out)
        self.assertIn("; temporary Renderer.cpp 2 <= 3 (reverts to 2)", out)

    def test_pending_temporary_below_its_base_still_ratchets_against_the_base(self):
        self._transitional(None, {"re-graft": {"landed": None}})
        _w(self.root, chg.BUDGET_PATH, json.dumps({"files": {BILL: 3}, "temporary": {BILL: {"limit": 4, "reverts_to": 3, "until": ["re-graft"]}}}))
        rc, out = _run(self.root)
        self.assertEqual(rc, 0, out)
        self.assertIn("%s 2 sites < budget 3 (ratchet the ceiling down)" % BILL, out)
        self.assertNotIn("temporary headroom", out)

    def test_without_a_temporary_the_ratchet_is_unchanged(self):
        self._transitional(None, {"re-graft": {"landed": None}})
        _w(self.root, chg.BUDGET_PATH, json.dumps({"files": {BILL: 3}}))
        rc, out = _run(self.root)
        self.assertEqual(rc, 0, out)
        self.assertIn("%s 2 sites < budget 3 (ratchet the ceiling down)" % BILL, out)
        self.assertNotIn("temporary headroom", out)
        _w(self.root, chg.BUDGET_PATH, json.dumps({"files": {BILL: 2}, "temporary": {BILL: {"limit": 3, "reverts_to": 2, "until": ["re-graft"]}}}))
        _w(self.root, chg.TRIGGERS_PATH, json.dumps({"re-graft": {"landed": "abc123"}}))
        rc, out = _run(self.root)
        self.assertEqual(rc, 0, out)
        self.assertNotIn("temporary headroom", out, "a landed temporary behaves as today: the base budget applies")
        self.assertNotIn("ratchet the ceiling down", out)


class UntaggedVpReferenceTest(unittest.TestCase):
    """T-HOOKGATE-UNTAGGED (coordinator 2026-11-12): under --since an added line of a Bill-owned production file that
    names a symbol declared under src/VP-V/ (and unknown to Bill's baseline) fails unless it is a tagged hook line."""

    GIT_ENV = CheckHookGatingTest.GIT_ENV
    VPV_HEADER = "src/VP-V/modules/feature/Feature.h"
    HEADER = ("#pragma once\nnamespace FeatureCatalog\n{\n\tstruct Decision {};\n\tDecision HeldNotice(int why);\n}\n"
              "struct FeatureMonitor\n{\n\tvoid SetNovelMode(int mode);\n\tint size() const;\n\tint frame;\n\tfloat detailPulse;\n};\n")
    # Bill's baseline already names `size` and `frame`: declared in VP-V too, but they are Bill's own names.
    BASE = ("struct Impl\n{\n\tFeatureMonitor mon;\n\tvoid Render(int frame)\n\t{\n\t\tdoBill(frame);\n\t\tint n = list.size();\n\t}\n};\n")

    def setUp(self):
        _fixture_baseline_patch(self)
        self.td = tempfile.TemporaryDirectory()
        self.root = self.td.name
        _w(self.root, BILL, self.BASE)
        self._git("init", "-q")
        self._git("add", BILL)
        self._git("commit", "-q", "-m", "bill")     # Bill's baseline: no src/VP-V/ in its tree
        self.base = self._git("rev-parse", "HEAD")
        _w(self.root, chg.SLOTS_HEADER, SLOTS)
        _w(self.root, self.VPV_HEADER, self.HEADER)

    def tearDown(self):
        self.td.cleanup()

    def _git(self, *args):
        return subprocess.run(["git", "-C", self.root] + list(args), check=True, env=self.GIT_ENV,
                              stdout=subprocess.PIPE, stderr=subprocess.PIPE).stdout.decode("utf-8", "replace").strip()

    def _cli(self, *extra):
        buf = io.StringIO()
        with contextlib.redirect_stdout(buf):
            rc = chg.main(["--root", self.root, "--base-ref", self.base, "--files", BILL] + list(extra))
        return rc, buf.getvalue()

    def _added(self, *lines):
        """Commit the VP-V header plus a Bill file with `lines` added inside Render() (file lines 8..); --since the Bill commit."""
        body = "".join("\t\t%s\n" % l for l in lines)
        _w(self.root, BILL, self.BASE.replace("\t\tint n = list.size();\n", "\t\tint n = list.size();\n" + body))
        self._git("add", "-A")
        self._git("commit", "-q", "-m", "branch")
        return self._cli("--rev", "HEAD", "--since", self.base)

    def test_granted_name_collision_site_is_honoured_and_only_for_its_symbol(self):
        entry = {"file": BILL, "pattern": r"\bdetailPulse\b", "symbol": "detailPulse",
                 "reason": "name collision granted by the coordinator; a local shares the spelling of a VP-V member. Proof: this test."}
        rc, out = self._added("float detailPulse = 1.f;")
        self.assertEqual(rc, 1, out)                      # no grant: flagged
        self.assertIn("detailPulse", out)
        old = chg.ALLOW_SITES
        chg.ALLOW_SITES = old + [entry]
        try:
            rc, out = self._cli("--rev", "HEAD", "--since", self.base)
            self.assertEqual(rc, 0, out)
            self.assertIn("0 new untagged VP-V references", out)
            self.tearDown()
            self.setUp()
            rc, out = self._added("float detailPulse = mon.SetNovelMode(1);")   # another VP-V name on the line still fails
            self.assertEqual(rc, 1, out)
            self.assertIn("[SetNovelMode]", out)
        finally:
            chg.ALLOW_SITES = old

    def test_waived_untagged_line_is_keyed_by_full_text_and_must_match_exactly_one_added_line(self):
        def entry(text):
            return {"file": BILL, "text": text, "reason": "WAIVER-TEST granted for this test. Expires at the re-graft."}
        ok = "\t\tmon.SetNovelMode(2);"
        old = chg.ALLOW_UNTAGGED_REFS
        try:
            chg.ALLOW_UNTAGGED_REFS = [entry(ok)]
            rc, out = self._added("mon.SetNovelMode(2);", "SyncFallback(FeatureCatalog::HeldNotice(1));")
            self.assertEqual(rc, 1, out)                  # the waiver covers only the SetNovelMode line
            self.assertIn("1 new untagged VP-V references", out)
            self.assertIn("allow-list untagged ref [1]", out)
            self.tearDown()
            self.setUp()
            chg.ALLOW_UNTAGGED_REFS = [entry(ok)]
            rc, out = self._added("mon.SetNovelMode(2);")
            self.assertEqual(rc, 0, out)
            self.tearDown()
            self.setUp()
            chg.ALLOW_UNTAGGED_REFS = [entry(ok)]
            rc, out = self._added("mon.SetNovelMode(2);", "mon.SetNovelMode(2);")   # two matches: ambiguous, nothing honoured
            self.assertEqual(rc, 1, out)
            self.assertIn("2 new untagged VP-V references", out)
        finally:
            chg.ALLOW_UNTAGGED_REFS = old

    def test_red_a_changed_or_extended_waived_line_is_flagged_again(self):
        old = chg.ALLOW_UNTAGGED_REFS
        try:
            for name, line in (("extended with more VP-V code", "mon.SetNovelMode(2); SyncFallback(FeatureCatalog::HeldNotice(1));"),
                               ("changed argument", "mon.SetNovelMode(3);"),
                               ("second call appended", "mon.SetNovelMode(2); mon.SetNovelMode(9);")):
                with self.subTest(name):
                    chg.ALLOW_UNTAGGED_REFS = [{"file": BILL, "text": "\t\tmon.SetNovelMode(2);",
                                                "reason": "WAIVER-TEST for this test. Expires at the re-graft."}]
                    rc, out = self._added(line)
                    self.assertEqual(rc, 1, out)
                    self.assertIn("1 new untagged VP-V references", out)
                self.tearDown()
                self.setUp()
        finally:
            chg.ALLOW_UNTAGGED_REFS = old

    def test_red_a_symbol_keyed_allow_site_never_approves_a_hook_statement(self):
        entry = {"file": BILL, "pattern": r"\bdetailPulse\b", "symbol": "detailPulse",
                 "reason": "name collision granted by the coordinator; applies to the symbol report only. Proof: this test."}
        old = chg.ALLOW_SITES
        chg.ALLOW_SITES = old + [entry]
        try:
            _w(self.root, BILL, _bill("\t\tvectorEngine.PlanAhead(detailPulse);   // VPV-HOOK\n"))
            rc, out = _run(self.root)
            self.assertEqual(rc, 1, out)
            self.assertIn("UNGUARDED %s:7 " % BILL, out)
        finally:
            chg.ALLOW_SITES = old

    def test_waiver_entries_name_the_waiver_and_expiry(self):
        for e in chg.ALLOW_UNTAGGED_REFS:
            self.assertIn("WAIVER-", e["reason"], e["text"])
            self.assertIn("Expires", e["reason"], e["text"])
            self.assertEqual(e["text"], chg.norm_line(e["text"]), "the stored text is already normalised")
            self.assertTrue(os.path.isfile(os.path.join(ROOT, *e["file"].split("/"))), e["file"])

    def test_p4_relocated_host_members_collect_frame_hooks_and_draw_frame_are_allowed_callees(self):
        rx = [e["callee"] for e in chg.ALLOW_CALLEES if "OrderFrameSlotHooks" in e["callee"]]
        self.assertEqual(len(rx), 1)
        for name in ("CollectFrameHooks", "DrawFrame", "m_impl->DrawFrame"):
            self.assertTrue(re.search(r"\b(?:" + rx[0] + r")\s*\(", "x = " + name + "(a);"), name)
        self.assertFalse(re.search(r"\b(?:" + rx[0] + r")\s*\(", "DrawFrameNow(a);"))

    def test_trailer_walk_back_stops_after_the_last_two_key_value_paragraphs(self):
        co = "Co-Authored-By: t <t@e.invalid>"
        self.assertEqual(chg.trailer_values("s\n\nb\n\nHook-Trigger: P4-e\n\n" + co), ["P4-e"])
        self.assertEqual(chg.trailer_values("s\n\nHook-Trigger: P4-e\n\nX-A: 1\n\n" + co), [])

    def test_red_an_added_untagged_vp_call_is_flagged_with_file_and_line(self):
        rc, out = self._added("mon.SetNovelMode(2); SyncFallback(FeatureCatalog::HeldNotice(1));")
        self.assertEqual(rc, 1, out)
        self.assertIn("untagged VP-V reference %s:8 " % BILL, out)
        self.assertIn("[FeatureCatalog, HeldNotice, SetNovelMode]", out)
        self.assertIn("1 new untagged VP-V references in Bill files (lines added since %s)" % self.base, out)
        self.assertIn("FAIL (rule 32): an added line in a Bill-owned production file names a symbol declared under src/VP-V/", out)

    def test_red_an_unreadable_since_ref_fails_closed_with_exit_2(self):
        _w(self.root, BILL, self.BASE + "// more\n")
        self._git("add", "-A")
        self._git("commit", "-q", "-m", "branch")
        err = io.StringIO()
        with contextlib.redirect_stderr(err), contextlib.redirect_stdout(io.StringIO()):
            with self.assertRaises(SystemExit) as cm:
                chg.main(["--root", self.root, "--base-ref", self.base, "--files", BILL, "--rev", "HEAD", "--since", "deadbeef123"])
        self.assertEqual(cm.exception.code, 2)
        self.assertIn("--since deadbeef123 cannot be read", err.getvalue())

    def test_green_the_same_line_with_a_hook_tag_is_not_an_untagged_reference(self):
        rc, out = self._added("mon.SetNovelMode(2);   // VPV-HOOK")
        self.assertIn("0 new untagged VP-V references in Bill files", out)
        self.assertNotIn("untagged VP-V reference " + BILL, out, "a tagged line is the hook gate's business, not this report's")

    def test_green_an_added_line_without_a_vp_reference_and_bills_own_names_are_not_flagged(self):
        # `size` and `frame` are declared in VP-V too, but Bill's baseline already uses them; the comment names a VP-V symbol.
        rc, out = self._added("int m = list.size() + frame;", "doBill(frame);   // FeatureCatalog is only named in this comment")
        self.assertEqual(rc, 0, out)
        self.assertIn("0 new untagged VP-V references in Bill files", out)

    def test_pre_existing_vp_references_are_not_flagged_since_only_added_lines_count(self):
        _w(self.root, BILL, self.BASE.replace("doBill(frame);\n", "doBill(frame);\n\t\tmon.SetNovelMode(1);\n"))
        self._git("add", "-A")
        self._git("commit", "-q", "-m", "older untagged call")
        older = self._git("rev-parse", "HEAD")
        _w(self.root, BILL, self.BASE.replace("doBill(frame);\n", "doBill(frame);\n\t\tmon.SetNovelMode(1);\n\t\tint kk = 1;\n"))
        self._git("add", "-A")
        self._git("commit", "-q", "-m", "unrelated edit")
        rc, out = self._cli("--rev", "HEAD", "--since", older)
        self.assertEqual(rc, 0, out)
        self.assertIn("0 new untagged VP-V references in Bill files", out)

    def test_without_since_the_output_and_exit_semantics_are_unchanged(self):
        _w(self.root, BILL, self.BASE.replace("doBill(frame);\n", "doBill(frame);\n\t\tmon.SetNovelMode(1);\n"))
        rc, out = self._cli()
        self.assertNotIn("untagged VP-V reference", out)
        self.assertNotIn("new untagged VP-V references", out)
        self.assertEqual(rc, 0, out)

    def test_the_symbol_set_is_derived_from_the_vp_v_headers(self):
        _w(self.root, "src/VP-V/modules/feature/Other.inl", "void NotAHeaderName(int x);\n")
        syms = chg.vp_symbols(chg.Tree(self.root))
        for name in ("FeatureCatalog", "Decision", "HeldNotice", "FeatureMonitor", "SetNovelMode"):
            self.assertIn(name, syms)
        self.assertNotIn("NotAHeaderName", syms, "only .h / .hpp under src/VP-V/ declare symbols")
        self.assertNotIn("return", syms)


class HookLinesTest(unittest.TestCase):
    def test_hook_lines_metric_blocks_above_baseline(self):
        # The gated metric is the honest H2 (WAIVER-CC-H2-METRIC): under --no-git there is no Bill baseline, so every
        # non-blank line of the file counts (fail closed), tagged or not. _bill("") has 8 non-blank lines.
        NL = chr(10)
        body = chr(9) * 2 + "if (!pipelineSlots.OriginalPathway()) { pipelineSlots.Prepass(); } // VPV-HOOK" + NL
        untagged = chr(9) * 2 + "doBill();" + NL
        with tempfile.TemporaryDirectory() as root:
            _w(root, BILL, _bill(body))
            _w(root, "src/VP-V/core/PipelineSlots.h", SLOTS)
            rc, out = _run(root)
            self.assertIn("hook LINES (honest H2): 9 (no baseline", out)
            rc, out = _run(root, extra=["--write-hook-lines-baseline"])
            self.assertEqual(rc, 1, "seeding a missing baseline is a migration: --migrate WAIVER-<ID> (G-CC1 SHOULD-2)")
            rc, out = _run(root, extra=["--write-hook-lines-baseline", "--migrate", "WAIVER-T-HL"])
            self.assertEqual(rc, 0, out)
            rc, out = _run(root)
            self.assertIn("hook LINES (honest H2): 9 (baseline 9, trend +0)", out)
            _w(root, BILL, _bill(body + untagged))
            rc, out = _run(root)
            self.assertEqual(rc, 1, out)
            self.assertIn("OVER HOOK-LINES BASELINE", out, "an UNTAGGED added line counts too")
            rc, out = _run(root, extra=["--write-hook-lines-baseline"])
            self.assertEqual(rc, 1)
            _w(root, BILL, _bill(""))
            rc, out = _run(root)
            self.assertIn("hook LINES (honest H2): 8 (baseline 9, trend -1)", out)
            self.assertEqual(rc, 1, "a value below its ceiling fails until the ratchet commit (G-CC1 SHOULD-7)")


class GateTwoMutationTest(unittest.TestCase):
    """G-CC2 (lead20/CC-gate-2.md) MUST-A, SHOULD-A, SHOULD-B: each test reproduces the gaming attempt the gate found and
    asserts that the checker now refuses it (WAIVER-CC-H2-METRIC, commit 1c)."""

    BODY = "".join("void Impl::f%d() {}\n" % i for i in range(10))
    MOVED = "struct Impl\n{\n\tPipelineSlots::Registry pipelineSlots;\n};\n#include <VP-V/host/Body.h>\n"

    def _tu(self, root):
        return chg.run(root, chg.DEFAULT_BASE, [BILL], no_git=True)["site_checks"]["hook_lines"]["tu"]

    def _relocated(self, root):
        """A Bill file whose 10 inline bodies moved to a FILE-scope VP header, baseline seeded before the move and
        re-seeded under a bounded relocation after it (h2 14 -> 5, tu 0 -> 10, combined 14 -> 15)."""
        _w(root, "src/VP-V/core/PipelineSlots.h", SLOTS)
        _w(root, BILL, "struct Impl\n{\n\tPipelineSlots::Registry pipelineSlots;\n" +
           "".join("\tvoid f%d() {}\n" % i for i in range(10)) + "};\n")
        self.assertEqual(_run(root, extra=["--write-hook-lines-baseline", "--migrate", "WAIVER-T-SEED"])[0], 0)
        _w(root, "src/VP-V/host/Body.h", self.BODY)
        _w(root, BILL, self.MOVED)

    def test_must_a_a_dead_or_unlisted_include_never_makes_a_moved_body_shared(self):
        with tempfile.TemporaryDirectory() as root:
            self._relocated(root)
            self.assertEqual(self._tu(root), {BILL: 10})
            # the gate's probe: a dummy VP-V .cpp with the include switched off
            for dummy in ("#if 0\n#include \"Body.h\"\n#endif\n", "#ifdef VPV_NEVER\n#include \"Body.h\"\n#endif\n",
                          "#if defined(VPV_NEVER)\n#include \"Body.h\"\n#endif\n"):
                _w(root, "src/VP-V/host/Dummy.cpp", dummy)
                self.assertEqual(self._tu(root), {BILL: 10}, dummy)
            # a live include from a .cpp that no build file lists
            _w(root, "src/VP-V/host/Dummy.cpp", "#include \"Body.h\"\n")
            self.assertEqual(self._tu(root), {BILL: 10})
            # a live include from a build-listed .cpp: Body.h defines Impl members, never shared
            _w(root, "src/VP-V/build/T.items.props",
               '<Project><ItemGroup><ClCompile Include="$(MSBuildThisFileDirectory)..\\host\\Dummy.cpp" /></ItemGroup></Project>\n')
            self.assertEqual(self._tu(root), {BILL: 10})
            # control: a genuine interface header behind the same listed .cpp is shared, so the exclusion still works
            _w(root, "src/VP-V/host/Api.h", "struct Api { int a; };\n")
            _w(root, "src/VP-V/host/Dummy.cpp", "#include \"Api.h\"\n")
            _w(root, BILL, self.MOVED + "#include <VP-V/host/Api.h>\n")
            self.assertEqual(self._tu(root), {BILL: 10})
            _w(root, "src/VP-V/host/Dummy.cpp", "#if 0\n#include \"Api.h\"\n#endif\n")
            self.assertEqual(self._tu(root), {BILL: 11}, "the same header behind #if 0 is not shared")

    def test_should_a_a_v1_swap_under_migrate_never_raises_h2(self):
        with tempfile.TemporaryDirectory() as root:
            _w(root, "src/VP-V/core/PipelineSlots.h", SLOTS)
            _w(root, BILL, _bill(""))   # 8 non-blank lines
            self.assertEqual(_run(root, extra=["--write-hook-lines-baseline", "--migrate", "WAIVER-T-SEED"])[0], 0)
            _w(root, chg.BUDGET_PATH, json.dumps({"h2_ceiling": {BILL: 8}}))
            _w(root, BILL, _bill("\t\tmore();\n\t\tevenMore();\n"))   # h2 8 -> 10
            # the gate's probe: replace the baseline with a v1 file and migrate
            _w(root, chg.HOOK_LINES_PATH, json.dumps({"base_ref": chg.DEFAULT_BASE, "total": 99, "files": {BILL: 99}}))
            rc, out = _run(root, extra=["--write-hook-lines-baseline", "--migrate", "WAIVER-GAME"])
            self.assertEqual(rc, 1, out)
            self.assertIn("h2 10 > ceiling 8", out)
            # a v2 baseline with an old tu measure (a tu re-seed) keeps its own h2 ceilings even without h2_ceiling
            _w(root, chg.BUDGET_PATH, json.dumps({}))
            _w(root, chg.HOOK_LINES_PATH, json.dumps({"version": 2, "metric": "H2", "base_ref": chg.DEFAULT_BASE, "total": 8,
                                                     "totals": {"production": 8}, "files": {BILL: 8}, "tu": {}, "combined": {}}))
            rc, out = _run(root, extra=["--write-hook-lines-baseline", "--migrate", "WAIVER-GAME"])
            self.assertEqual(rc, 1, out)
            self.assertIn("h2 10 > ceiling 8", out)
            # ... while the same re-seed without h2 growth passes and records the new tu measure
            _w(root, BILL, _bill(""))
            self.assertEqual(_run(root, extra=["--write-hook-lines-baseline", "--migrate", "WAIVER-T-TU"])[0], 0)
            base = chg.vp_footprint.read_json(os.path.join(root, *chg.HOOK_LINES_PATH.split("/")))
            self.assertEqual((base["files"][BILL], base["tu_measure"]), (8, chg.TU_MEASURE_VERSION))

    def test_should_b_a_relocation_is_bounded_by_its_stated_allowance(self):
        with tempfile.TemporaryDirectory() as root:
            self._relocated(root)
            reloc = ["--write-hook-lines-baseline", "--relocation", "WAIVER-T-RELOC"]
            rc, out = _run(root, extra=reloc)
            self.assertEqual(rc, 1, "a relocation states its allowance")
            self.assertIn("--allowance", out)
            rc, out = _run(root, extra=reloc + ["--allowance", "0"])
            self.assertEqual(rc, 1, out)
            self.assertIn("combined +1", out)
            # the gate's probe: NEW VP code added straight into the file-scope header passes as a "relocation"
            _w(root, "src/VP-V/host/Body.h", self.BODY + "".join("void Impl::g%d() {}\n" % i for i in range(20)))
            rc, out = _run(root, extra=reloc + ["--allowance", "2"])
            self.assertEqual(rc, 1, out)
            self.assertIn("tu +30", out)
            rc, out = _run(root, extra=reloc + ["--allowance", str(chg.RELOCATION_MAX_ALLOWANCE + 1)])
            self.assertEqual(rc, 1, "an allowance above the cap is refused")
            # the real move inside its allowance passes and is recorded with its deltas
            _w(root, "src/VP-V/host/Body.h", self.BODY)
            rc, out = _run(root, extra=reloc + ["--allowance", "1"])
            self.assertEqual(rc, 0, out)
            base = chg.vp_footprint.read_json(os.path.join(root, *chg.HOOK_LINES_PATH.split("/")))
            self.assertIn("RELOCATION allowance 1", base["_updates"][0])
            self.assertIn("tu +10, h2 -9, combined +1", base["_updates"][0])


class HonestH2MetricTest(unittest.TestCase):
    """CC-audit-1 CHECKER-TIGHTENING SPEC (WAIVER-CC-H2-METRIC): T1-T13 and the dbbef8c07 -> a68b5e7f7 regression fixture."""

    def test_selftests_t1_to_t13(self):
        buf = io.StringIO()
        with contextlib.redirect_stdout(buf):
            self.assertEqual(chg.selftest(), 0)
            chg.vp_footprint.selftest()
        self.assertEqual(buf.getvalue().count("selftest OK"), 2)

    def test_v2_baseline_and_budget_agree(self):
        base = chg.vp_footprint.read_json(os.path.join(ROOT, *chg.HOOK_LINES_PATH.split("/")))
        self.assertEqual((base["version"], base["metric"], base["base_ref"]), (2, "H2", chg.DEFAULT_BASE))
        self.assertEqual(base["total"], sum(base["totals"][b] for b in chg.H2_BUCKETS))
        self.assertEqual(base["total"], sum(base["files"].values()))
        budget = chg.vp_footprint.read_json(os.path.join(ROOT, *chg.BUDGET_PATH.split("/")))
        for f, n in budget.get("h2_ceiling", {}).items():
            self.assertLessEqual(base["files"].get(f, 0), n, f)
        self.assertNotEqual(base["total"], 393, "never a baseline at the fragment-gamed 393")

    def test_missing_baseline_in_git_mode_gives_no_verdict_and_h2_ceiling_holds_alone(self):
        """G-CC1 SHOULD-1: deleting the baseline never switches the H2 gate off."""
        with tempfile.TemporaryDirectory() as root:
            tree = chg.Tree(root)
            base, err = chg.load_h2_baseline(tree, no_git=False)
            self.assertIsNone(base)
            self.assertIn("missing", err)
            self.assertEqual(chg.load_h2_baseline(tree, no_git=True), (None, None))
            _w(root, chg.BUDGET_PATH, json.dumps({"files": {}, "h2_ceiling": {BILL: 3}}))
            h2 = {BILL: {"bucket": "production", "h1": 4, "inc": 0, "h2": 4, "tu": 0, "fragments": [], "errors": []}}
            hl = chg.hook_lines_metric(tree, [], h2, {}, False, root)
            self.assertTrue(hl["baseline_error"])
            self.assertEqual(hl["over"], ["%s h2 4 > ceiling 3" % BILL])
            _w(root, chg.HOOK_LINES_PATH, json.dumps({"version": 2, "metric": "H2", "files": {}}))
            self.assertIn("tu / combined", chg.load_h2_baseline(tree)[1], "a v2 baseline without tu ceilings is refused")

    def test_real_baseline_carries_tu_and_combined(self):
        base = chg.vp_footprint.read_json(os.path.join(ROOT, *chg.HOOK_LINES_PATH.split("/")))
        self.assertTrue(base["tu"] and base["combined"])
        for f, t in base["tu"].items():
            self.assertEqual(base["combined"][f], base["files"].get(f, 0) + t, f)

    @staticmethod
    def _have(sha):
        return subprocess.run(["git", "-C", ROOT, "cat-file", "-e", sha + "^{commit}"],
                              stdout=subprocess.PIPE, stderr=subprocess.PIPE).returncode == 0

    def test_regression_fixture_fragment_moves_raise_renderer_h2(self):
        """r2/cc-hooks 12.1-12.4 moved Bill-file lines into fragments included in class bodies: the narrow count fell
        451 -> 393, the honest H2 must RISE: renderer 5523 -> 5583 (CC-audit-1 HONEST COUNT)."""
        for sha in ("dbbef8c07", "a68b5e7f7"):
            if not self._have(sha):
                self.skipTest("fixture commit %s not in this clone" % sha)
        got = {}
        for sha in ("dbbef8c07", "a68b5e7f7"):
            res = chg.run(ROOT, chg.DEFAULT_BASE, rev=sha)
            got[sha] = res["site_checks"]["hook_lines"]
        self.assertEqual(got["dbbef8c07"]["totals"]["renderer"], 5523)
        self.assertEqual(got["a68b5e7f7"]["totals"]["renderer"], 5583)
        self.assertEqual((got["dbbef8c07"]["totals"]["production"], got["dbbef8c07"]["total"]), (18645, 23279))
        self.assertEqual((got["a68b5e7f7"]["totals"]["production"], got["a68b5e7f7"]["total"]), (18705, 23339))
        self.assertEqual((got["dbbef8c07"]["tagged_total"], got["a68b5e7f7"]["tagged_total"]), (451, 393))
        frags = sorted((x["includer"].rsplit("/", 1)[-1], x["fragment"].rsplit("/", 1)[-1], x["lines"]) for x in got["a68b5e7f7"]["fragments"])
        self.assertEqual(frags, [("LibplaceboVideoRenderer.cpp", "LvrImplDejudderState.h", 33),
                                 ("LibplaceboVideoRenderer.cpp", "LvrImplMembers.h", 140),
                                 ("LibplaceboVideoRenderer.cpp", "LvrImplRenderAheadState.h", 24),
                                 ("LibplaceboVideoRenderer.cpp", "LvrImplVectorEngineState.h", 36),
                                 ("LibplaceboVideoRenderer.h", "LvrClassMembers.h", 20)])
        self.assertEqual(got["dbbef8c07"]["fragments"], [])
        self.assertEqual(len(got["a68b5e7f7"]["fragment_hits"]), 5, "five unregistered class-scope fragments")


class SinceKeepsUnverifiedMarkersTest(unittest.TestCase):
    """HOOKGATE-CHECKER-HOLES (1): a VPV-GATED body in src/VP-V that loses its Original early return after the merge
    base changes no Bill line, yet under --since the marker and its call site must still fail the gate."""

    GIT_ENV = CheckHookGatingTest.GIT_ENV
    GATED = "src/VP-V/modules/motion/estimation/Lease.h"
    WITH_RETURN = ("struct State\n{\n\tbool EndLease(bool originalPathway)   // VPV-GATED\n\t{\n"
                   "\t\tif (originalPathway) return false;\n\t\tReset();\n\t\treturn true;\n\t}\n};\n")

    def setUp(self):
        _fixture_baseline_patch(self)
        self.td = tempfile.TemporaryDirectory()
        self.root = self.td.name
        _w(self.root, BILL, _bill("\t\tdoBill();\n"))
        self._git("init", "-q")
        self._git("add", "-A")
        self._git("commit", "-q", "-m", "bill")     # Bill's baseline: no src/VP-V/ in its tree
        self.bill = self._git("rev-parse", "HEAD")
        _w(self.root, chg.SLOTS_HEADER, SLOTS)

    def tearDown(self):
        self.td.cleanup()

    def _git(self, *args):
        return subprocess.run(["git", "-C", self.root] + list(args), check=True, env=self.GIT_ENV,
                              stdout=subprocess.PIPE, stderr=subprocess.PIPE).stdout.decode("utf-8", "replace").strip()

    def _commit(self, gated_text):
        _w(self.root, self.GATED, gated_text)
        _w(self.root, BILL, _bill("\t\tdoBill();\n\t\tvectorEstimate.EndLease(pipelineSlots.OriginalPathway());   // VPV-HOOK\n"))
        self._git("add", "-A")
        self._git("commit", "-q", "-m", "vp")
        return self._git("rev-parse", "HEAD")

    def _cli(self, since):
        buf = io.StringIO()
        with contextlib.redirect_stdout(buf):
            rc = chg.main(["--root", self.root, "--base-ref", self.bill, "--files", BILL, "--rev", "HEAD", "--since", since])
        return rc, buf.getvalue()

    def test_red_a_marker_that_loses_its_early_return_after_the_merge_base_fails_under_since(self):
        first = self._commit(self.WITH_RETURN)
        rc, out = self._cli(first)
        self.assertIn("0 unguarded, 0 unverified markers", out)
        self._commit(self.WITH_RETURN.replace("\t\tif (originalPathway) return false;\n", ""))
        rc, out = self._cli(first)
        self.assertEqual(rc, 1, out)
        self.assertIn("UNVERIFIED VPV-GATED marker", out)
        self.assertIn("EndLease", out)
        self.assertIn("1 unguarded, 1 unverified markers", out)
        self.assertIn(BILL + ":", out)


class HookgateHolesTest(unittest.TestCase):
    """HOOKGATE-CHECKER-HOLES S1 / S4 / S5 (102b; lead17 opus-audit-hookgate SHOULDs): planted fixtures."""

    GATED = "src/VP-V/modules/motion/estimation/Lease.h"
    PARAM_MARKER = ("struct State\n{\n\tbool EndLease(bool originalPathway)   // VPV-GATED\n\t{\n"
                    "\t\tif (originalPathway) return false;\n\t\tReset();\n\t\treturn true;\n\t}\n"
                    "\ttemplate <typename F> bool Drop(bool originalPathway, F f)   // VPV-GATED\n\t{\n"
                    "\t\tif (originalPathway) return false;\n\t\treturn f();\n\t}\n};\n")

    def setUp(self):
        self.td = tempfile.TemporaryDirectory()
        self.root = self.td.name
        _w(self.root, chg.SLOTS_HEADER, SLOTS)

    def tearDown(self):
        self.td.cleanup()

    # S1: an argument passed to a gated call's predicate parameter
    def test_red_s1_a_constant_passed_for_the_gated_predicate_parameter_fails(self):
        _w(self.root, self.GATED, self.PARAM_MARKER)
        for arg in ("false", "0", "wanted", "pipelineSlots.OriginalPathway() && false || x"):
            with self.subTest(arg):
                _w(self.root, BILL, _bill("\t\tvectorEstimate.EndLease(%s);   // VPV-HOOK\n" % arg))
                rc, out = _run(self.root)
                self.assertEqual(rc, 1, out)
                self.assertIn("UNGUARDED %s:7 " % BILL, out)
                self.assertIn("gated-arg-unverified: EndLease", out)
                self.assertIn("parameter 0", out)

    def test_green_s1_a_predicate_form_passed_for_the_parameter(self):
        _w(self.root, self.GATED, self.PARAM_MARKER)
        for arg in ("pipelineSlots.OriginalPathway()", "activeSettings.pipelineOriginalPathway", "originalPathwayOn()",
                    "m_impl->pipelineSlots.OriginalPathway()"):
            with self.subTest(arg):
                _w(self.root, BILL, _bill("\t\tvectorEstimate.EndLease(%s);   // VPV-HOOK\n" % arg))
                rc, out = _run(self.root)
                self.assertEqual(rc, 0, out)
                self.assertIn(" 0 unguarded, 0 unverified markers", out)

    def test_s1_a_call_cut_at_a_lambda_body_still_judges_the_completed_argument(self):
        # the statement ends at the lambda's `{` (ConfigEditorWindow QueueDelayShowsOff form): parameter 0 is complete
        _w(self.root, self.GATED, self.PARAM_MARKER)
        call = "\t\tvectorEstimate.Drop(%s,   // VPV-HOOK\n\t\t\t[&] { return n > 0; });\n"
        _w(self.root, BILL, _bill(call % "originalPathwayOn()"))
        rc, out = _run(self.root)
        self.assertEqual(rc, 0, out)
        _w(self.root, BILL, _bill(call % "true"))
        rc, out = _run(self.root)
        self.assertEqual(rc, 1, out)
        self.assertIn("gated-arg-unverified: Drop", out)

    def test_s1_a_member_predicate_marker_does_not_judge_arguments(self):
        _w(self.root, self.GATED, "struct State\n{\n\tbool originalPathway;\n\tbool EndLease(int n)   // VPV-GATED\n\t{\n"
                                  "\t\tif (originalPathway) return false;\n\t\treturn n > 0;\n\t}\n};\n")
        _w(self.root, BILL, _bill("\t\tvectorEstimate.EndLease(0);   // VPV-HOOK\n"))
        rc, out = _run(self.root)
        self.assertEqual(rc, 0, out)

    # S5: a local that shadows a predicate name
    def test_red_s5_a_shadowing_local_is_not_a_guard(self):
        for name, body in (
                ("constant local", "\t\tbool originalPathway = false;\n\t\tif (!originalPathway) vectorEngine.PlanAhead(frame);   // VPV-HOOK\n"),
                ("call local", "\t\tconst bool originalPathway = Decide(frame);\n\t\tif (!originalPathway) vectorEngine.PlanAhead(frame);   // VPV-HOOK\n"),
                ("reassigned", "\t\tconst bool originalPathway = pipelineSlots.OriginalPathway();\n\t\toriginalPathway = false;\n"
                               "\t\tif (!originalPathway) vectorEngine.PlanAhead(frame);   // VPV-HOOK\n"),
                ("early return on a shadow", "\t\tbool originalPathway = false;\n\t\tif (originalPathway)\n\t\t\treturn;\n"
                                             "\t\tvectorEngine.PlanAhead(frame);   // VPV-HOOK\n")):
            with self.subTest(name):
                _w(self.root, BILL, _bill(body))
                rc, out = _run(self.root)
                self.assertEqual(rc, 1, out)
                self.assertIn("UNGUARDED %s:" % BILL, out)

    def test_green_s5_the_pathway_query_declaration_still_guards(self):
        for decl in ("const bool originalPathway = pipelineSlots.OriginalPathway();",
                     "const bool originalPathway = OriginalPathwayOf(frame);"):
            with self.subTest(decl):
                _w(self.root, BILL, _bill("\t\t%s\n\t\tif (!originalPathway) vectorEngine.PlanAhead(frame);   // VPV-HOOK\n" % decl))
                rc, out = _run(self.root)
                self.assertEqual(rc, 0, out)

    def test_red_s5_a_gated_body_with_a_shadowing_local_is_unverified(self):
        _w(self.root, self.GATED, "struct State\n{\n\tbool EndLease(Ctx& c)   // VPV-GATED\n\t{\n"
                                  "\t\tbool originalPathway = false;\n\t\tif (originalPathway) return false;\n\t\treturn true;\n\t}\n};\n")
        _w(self.root, BILL, _bill("\t\tvectorEstimate.EndLease(ctx);   // VPV-HOOK\n"))
        rc, out = _run(self.root)
        self.assertEqual(rc, 1, out)
        self.assertIn("UNVERIFIED VPV-GATED marker", out)

    # S4: one approval row admits at most max_sites new sites
    def _two_new_sites(self):
        _w(self.root, BILL, _bill("\t\tint keep = 0;   // VPV-HOOK\n"))
        buf = io.StringIO()
        with contextlib.redirect_stdout(buf):
            self.assertEqual(chg.main(["--root", self.root, "--no-git", "--files", BILL, "--write-manifest", "--write-budget"]), 0)
        site = "\t\tif (!pipelineSlots.OriginalPathway()) vectorEngine.PlanAhead(frame);   // VPV-HOOK\n"
        _w(self.root, BILL, _bill("\t\tint keep = 0;   // VPV-HOOK\n\t\tdoBill();\n" + site + "\t\tdoBill();\n" + site))
        _w(self.root, chg.BUDGET_PATH, '{"files": {"%s": 3}}' % BILL)

    def _approval(self, extra=""):
        _w(self.root, chg.APPROVALS_PATH, '{"approvals": [{"id": "HOOK-APPROVAL-1", "file": "%s", "text_contains": '
                                          '"vectorEngine.PlanAhead(frame)", "seam": "S2", "who": "coordinator", '
                                          '"when": "2026-11-21", "expiry": null%s}]}' % (BILL, extra))

    def test_red_s4_one_approval_row_admitting_two_sites_fails(self):
        self._two_new_sites()
        self._approval()
        rc, out = _run(self.root)
        self.assertEqual(rc, 1, out)
        self.assertIn("APPROVAL OVER HOOK-APPROVAL-1 approves 2 sites > max_sites 1", out)
        self.assertIn("FAIL (rule 32 approvals)", out)

    def test_green_s4_an_explicit_max_sites_admits_them(self):
        self._two_new_sites()
        self._approval(', "max_sites": 2')
        rc, out = _run(self.root)
        self.assertEqual(rc, 0, out)
        self.assertIn("2 new (2 approved)", out)

    def test_red_s4_an_invalid_max_sites_fails(self):
        self._two_new_sites()
        for bad in ('0', '"2"', 'true', '-1'):
            with self.subTest(bad):
                self._approval(', "max_sites": %s' % bad)
                rc, out = _run(self.root)
                self.assertEqual(rc, 1, out)
                self.assertIn("APPROVAL INVALID HOOK-APPROVAL-1", out)


class HeaderEditDoesNotExemptTest(unittest.TestCase):
    """HOOKGATE-CHECKER-HOLES S2: an added header line of a Bill function no longer exempts its whole body."""

    GIT_ENV = CheckHookGatingTest.GIT_ENV

    def setUp(self):
        self.td = tempfile.TemporaryDirectory()
        self.root = self.td.name
        _w(self.root, BILL, _bill("\t\tdoBill();\n"))
        self._git("init", "-q")
        self._git("add", "-A")
        self._git("commit", "-q", "-m", "bill")
        self.bill = self._git("rev-parse", "HEAD")
        _w(self.root, chg.SLOTS_HEADER, SLOTS)

    def tearDown(self):
        self.td.cleanup()

    def _git(self, *args):
        return subprocess.run(["git", "-C", self.root] + list(args), check=True, env=self.GIT_ENV,
                              stdout=subprocess.PIPE, stderr=subprocess.PIPE).stdout.decode("utf-8", "replace").strip()

    def _check(self, text):
        _w(self.root, BILL, text)
        self._git("add", "-A")
        self._git("commit", "-q", "-m", "vp")
        with contextlib.redirect_stdout(io.StringIO()):
            chg.main(["--root", self.root, "--base-ref", self.bill, "--files", BILL,
                      "--write-hook-lines-baseline", "--migrate", "WAIVER-T-S2"])
        self._git("add", "-A")
        self._git("commit", "-q", "-m", "seed baseline")
        buf = io.StringIO()
        with contextlib.redirect_stdout(buf):
            rc = chg.main(["--root", self.root, "--base-ref", self.bill, "--files", BILL, "--rev", "HEAD"])
        return rc, buf.getvalue()

    def test_red_s2_an_edited_bill_header_does_not_exempt_the_body(self):
        text = _bill("\t\tdoBill();\n\t\tvectorEngine.PlanAhead(frame);   // VPV-HOOK\n").replace(
            "\tvoid Render()\n", "\tvoid Render(int extra)\n")
        rc, out = self._check(text)
        self.assertEqual(rc, 1, out)
        self.assertIn("UNGUARDED %s:" % BILL, out)
        self.assertIn("PlanAhead", out)

    def test_red_s2_a_new_overload_reusing_a_bill_name_is_judged(self):
        text = _bill("\t\tdoBill();\n").replace(
            "};\n", "\tvoid doBill(int n)\n\t{\n\t\tvectorEngine.PlanAhead(frame);   // VPV-HOOK\n\t}\n};\n")
        rc, out = self._check(text)
        self.assertEqual(rc, 1, out)
        self.assertIn("UNGUARDED %s:" % BILL, out)

    def test_green_s2_a_wholly_new_function_is_still_vp_function(self):
        text = _bill("\t\tdoBill();\n").replace(
            "};\n", "\tvoid VpPlanAhead()\n\t{\n\t\tvectorEngine.PlanAhead(frame);   // VPV-HOOK\n\t}\n};\n")
        rc, out = self._check(text)
        self.assertEqual(rc, 0, out)
        self.assertIn(" 0 unguarded, 0 unverified markers", out)

    def test_green_s2_a_new_member_initialiser_lambda_is_vp_function(self):
        # LibplaceboVideoRenderer.cpp dejudderBinding form: a new member whose lambda body is VP-V's
        text = _bill("\t\tdoBill();\n").replace(
            "};\n", "\tVpMotion::Binding vpBinding{ this,   // VPV-HOOK\n"
                    "\t\t[](void* self) { static_cast<Impl*>(self)->vpReset.store(true); } };   // VPV-HOOK\n};\n")
        rc, out = self._check(text)
        self.assertEqual(rc, 0, out)
        self.assertIn(" 0 unguarded, 0 unverified markers", out)

    def test_red_s2_a_lambda_inside_an_edited_bill_function_is_judged(self):
        text = _bill("\t\tdoBill();\n\t\tauto f = [&]()\n\t\t{\n\t\t\tvectorEngine.PlanAhead(frame);   // VPV-HOOK\n\t\t};\n").replace(
            "\tvoid Render()\n", "\tvoid Render(int extra)\n")
        rc, out = self._check(text)
        self.assertEqual(rc, 1, out)
        self.assertIn("UNGUARDED %s:" % BILL, out)


if __name__ == "__main__":
    unittest.main()
