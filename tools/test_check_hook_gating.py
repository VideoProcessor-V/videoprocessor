# Copyright(C) 2026 VP-V contributors
# SPDX-License-Identifier: GPL-3.0-only
import contextlib
import importlib.util
import io
import os
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


def _bill(body):
    """A Bill file with one function whose body is `body` (the hook lines are tagged by the caller)."""
    return "#include <x.h>\nstruct Impl\n{\n\tPipelineSlots::Registry pipelineSlots;\n\tvoid Render()\n\t{\n" + body + "\t}\n};\n"


class CheckHookGatingTest(unittest.TestCase):
    def setUp(self):
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


if __name__ == "__main__":
    unittest.main()
