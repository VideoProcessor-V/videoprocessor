#!/usr/bin/env python3
# Copyright(C) 2026 VP-V contributors
# SPDX-License-Identifier: GPL-3.0-only
"""Tests for tools/producer_chain.py. Run: python -I tools/test_producer_chain.py"""
import importlib.util
import json
import os
import shutil
import sys
import tempfile
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(HERE)


def load():
    spec = importlib.util.spec_from_file_location("producer_chain", os.path.join(HERE, "producer_chain.py"))
    m = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(m)
    return m


PC = load()

PRODUCTS_H = """
constexpr const char* kTokenSix = "six_token";
constexpr uint8_t kGuideDefaultLag = 0;
inline void A() {
    ProductDecl a = Make(kPa, "pa", "alpha", Backend::Cpu, 1, Tier::T0, c, s);
    ProductDecl b = Make(kPb, kTokenSix, "beta", Backend::GpuCompute, 1, Tier::T0, c, s);
    AddInput(b, kPa, 0);
    AddInput(b, kPa, 1);
    ProductDecl c2 = Make(kPc, "pc", "gamma", Backend::Cpu, 0, Tier::T0, c, s);
    AddInput(c2, kPa, 0);
    AddInput(c2, kPb, kGuideDefaultLag);
    AddLead(c2, kPb, 2);
    AddInput(c2, kPc, 1);
    c2.variants = Variants::NonIdentical;
    c2.shortQueueSubstitute = kPb;
    ProductDecl d = Make(kPd, "pd", "delta", Backend::Cpu, 0, Tier::T0, c, s);
    AddInput(d, kPa, 0);
    ProductDecl d2 = Make(kPe, "pe", "eps", Backend::Cpu, 0, Tier::T0, c, s);
    AddInput(d2, kPd, 0);
}
"""
CATALOG_H = """
inline const std::vector<ProductRow>& ProductRows() {
    static const std::vector<ProductRow> rows = {
        { kPa, "u/a", kExpectedA, "A producer" },
        { kPb, "u/b", kExpectedB, "B producer" },
        { kPc, "u/c", kExpectedC, "C producer" },
        { kPd, "u/d", kExpectedD, "reserved (no producer)" },
        { kPe, "u/e", kExpectedE, "E" },
    };
    return rows;
}
inline const ProductRow* RowOf(ProductId id) { return nullptr; }
inline const std::vector<ProductFlag>& ProductFlags() {
    static const std::vector<ProductFlag> flags = {
        { kPc, kPb, FlagKind::ViewIsActual, "C9", "a view" },
    };
    return flags;
}
inline const std::vector<ConsumerFlag>& ConsumerFlags() { return {}; }
"""
MAP = {
    "lanes": {"kPb": "L-X"},
    "default_read": ["\\bInput\\(\\s*(VpSkeleton::)?{k}\\b", "\\bAcquire\\(\\s*(VpSkeleton::)?{k}\\b"],
    "products": {"kPb": {"files": ["m/b.cpp"]}, "kPc": {"files": ["m/c.cpp"]}, "kPa": {"files": ["m/a.cpp"]}},
    "edges": {},
    "no_producer_owners": {"kPd": "102b FIXTURE-D", "kPe": "L-X"},
    "bypass": [{"input": "kPa", "products": ["kPb"], "regex": "\\bPrivateCopy\\(", "kind": "recompute"}],
}


def make_tree(b_text, c_text, mp=None):
    root = tempfile.mkdtemp(prefix="pchain_")
    for rel, text in (("src/VP-V/core/Products.h", PRODUCTS_H), ("src/VP-V/core/ProductCatalog.h", CATALOG_H),
                      ("src/VP-V/modules/m/b.cpp", b_text), ("src/VP-V/modules/m/c.cpp", c_text),
                      ("src/VP-V/modules/m/a.cpp", "// nothing\n"),
                      ("tools/producer_chain_map.json", json.dumps(mp or MAP))):
        full = os.path.join(root, rel)
        os.makedirs(os.path.dirname(full), exist_ok=True)
        with open(full, "w", encoding="utf-8") as f:
            f.write(text)
    return root


def edge(doc, pid, inp):
    for t in doc["products"]:
        if t["id"] == pid:
            for e in t["edges"]:
                if e["input"] == inp:
                    return e
    raise KeyError(pid + "<-" + inp)


class FixtureTests(unittest.TestCase):
    def setUp(self):
        self.dirs = []

    def tearDown(self):
        for d in self.dirs:
            shutil.rmtree(d, ignore_errors=True)

    def doc(self, b, c, mp=None):
        r = make_tree(b, c, mp)
        self.dirs.append(r)
        return PC.build(r)

    def test_parse_declarations(self):
        d = self.doc("", "")
        self.assertEqual([t["id"] for t in d["products"]], ["kPa", "kPb", "kPc", "kPd", "kPe"])
        self.assertEqual(next(t for t in d["products"] if t["id"] == "kPb")["token"], "six_token")
        e = edge(d, "kPb", "kPa")
        self.assertEqual(sorted(e["lags"]), [0, 1])
        c = edge(d, "kPc", "kPb")
        self.assertEqual(c["leads"], [2])
        t = next(t for t in d["products"] if t["id"] == "kPc")
        self.assertEqual(t["variants"], "NonIdentical")
        self.assertEqual(t["shortQueueSubstitute"], "kPb")

    def test_read_is_R(self):
        d = self.doc("void f(C& c){ c.Input(VpSkeleton::kPa); }", "void g(){ Acquire(kPb); }")
        self.assertEqual(edge(d, "kPb", "kPa")["status"], "R")
        self.assertEqual(edge(d, "kPc", "kPb")["status"], "R")

    def test_bypass_wins_over_read_is_X(self):
        d = self.doc("c.Input(kPa); PrivateCopy(x);", "")
        e = edge(d, "kPb", "kPa")
        self.assertEqual(e["status"], "X")
        self.assertEqual(e["kind"], "recompute")
        self.assertEqual(e["evidence"][0][1], 1)

    def test_unread_is_N_and_comments_ignored(self):
        d = self.doc("// c.Input(kPa); PrivateCopy(x)\n/* Acquire(kPa) */\n", "")
        self.assertEqual(edge(d, "kPb", "kPa")["status"], "N")
        self.assertEqual(edge(d, "kPc", "kPa")["status"], "N")

    def test_reserved_and_fileless_is_NP(self):
        d = self.doc("", "")
        self.assertEqual(edge(d, "kPe", "kPd")["status"], "NP")   # kPe has no files
        self.assertEqual(edge(d, "kPd", "kPa")["status"], "NP")   # kPd is reserved

    def test_flag_note_joined(self):
        d = self.doc("", "")
        self.assertIn("ViewIsActual", edge(d, "kPc", "kPb")["note"])

    def test_counts_and_lane(self):
        d = self.doc("c.Input(kPa);", "")
        self.assertEqual(sum(d["counts"].values()),
                         sum(1 for t in d["products"] for e in t["edges"] if e["declared"]))
        self.assertEqual(next(t for t in d["products"] if t["id"] == "kPb")["lane"], "L-X")

    def test_owner_fill_registration_auto_discovered(self):
        root = make_tree("", "")
        self.dirs.append(root)
        full = os.path.join(root, "src/VP-V/modules/m/e.cpp")
        with open(full, "w", encoding="utf-8") as f:
            f.write("void Reg(){ memo.RegisterOwnerFill(kPe, form, unit, fill); Acquire(kPd, s); }\n")
        d = PC.build(root)
        t = next(t for t in d["products"] if t["id"] == "kPe")
        self.assertEqual(t["producer_files"], ["m/e.cpp"])
        self.assertEqual(edge(d, "kPe", "kPd")["status"], "R")

    def test_scheduler_order_and_temporal(self):
        d = self.doc("", "")
        s = d["scheduler"]
        self.assertTrue(s["acyclic"])
        pos = {k: i for i, k in enumerate(s["topological_order"])}
        for a, b in s["ordering_edges"]:
            self.assertLess(pos[a], pos[b])
        self.assertNotIn(["kPc", "kPc"], s["ordering_edges"])         # own previous frame is temporal
        kinds = {(x["from"], x["to"], x["kind"], x["n"]) for x in s["temporal_edges"]}
        self.assertIn(("kPb", "kPc", "lead", 2), kinds)
        self.assertIn(("kPa", "kPb", "lag", 1), kinds)
        self.assertIn(("kPc", "kPc", "lag", 1), kinds)

    def test_cycle_detected(self):
        table = [{"id": "x", "edges": [{"input": "y", "lags": [0], "leads": [], "declared": True}]},
                 {"id": "y", "edges": [{"input": "x", "lags": [0], "leads": [], "declared": True}]},
                 {"id": "z", "edges": []}]
        s = PC.schedule(table)
        self.assertFalse(s["acyclic"])
        self.assertEqual(s["cycle_members"], ["x", "y"])
        self.assertEqual(s["topological_order"], ["z"])

    def test_lag_breaks_cycle(self):
        table = [{"id": "x", "edges": [{"input": "y", "lags": [1], "leads": [], "declared": True}]},
                 {"id": "y", "edges": [{"input": "x", "lags": [0], "leads": [], "declared": True}]}]
        self.assertTrue(PC.schedule(table)["acyclic"])

    def test_check_flag_detects_stale(self):
        root = make_tree("", "")
        self.dirs.append(root)
        os.makedirs(os.path.join(root, "docs/design"))
        self.assertEqual(PC.main(["--root", root, "--check"]), 1)
        self.assertEqual(PC.main(["--root", root, "--write"]), 0)
        self.assertEqual(PC.main(["--root", root, "--check"]), 0)
        with open(os.path.join(root, "docs/design/producer-chain.md"), "a", encoding="utf-8") as f:
            f.write("x")
        self.assertEqual(PC.main(["--root", root, "--check"]), 1)

    def test_unowned_no_producer_fails_check(self):
        mp = dict(MAP)
        mp["no_producer_owners"] = {"kPd": "102b FIXTURE-D"}
        root = make_tree("", "", mp)
        self.dirs.append(root)
        os.makedirs(os.path.join(root, "docs/design"))
        self.assertEqual(PC.main(["--root", root, "--write"]), 0)
        self.assertEqual(PC.main(["--root", root, "--check"]), 1)
        self.assertEqual(PC.build(root)["unowned_no_producer"], ["kPe"])

    def test_interim_edge_is_tagged(self):
        mp = dict(MAP)
        mp["edges"] = {"kPc<-kPa": {"interim": "tap plane"}}
        doc = self.doc("", "", mp)
        self.assertEqual(edge(doc, "kPc", "kPa")["interim"], "tap plane")
        self.assertIn("INTERIM (tap plane)", edge(doc, "kPc", "kPa")["note"])

    def test_edge_read_override_is_scoped_not_a_dodge(self):
        # jprod-a audit SHOULD 1: an edge override narrows the read shape; the same edge still reads R when the narrowed
        # shape is present (the override never hides a real read).
        mp = dict(MAP)
        mp["edges"] = {"kPc<-kPc": {"read": ["\\bAcquire\\(\\s*kPc\\s*,\\s*req\\.seq\\s*-\\s*1\\b"]}}
        self.assertEqual(edge(self.doc("", "void g(){ Acquire(kPc, seq); }"), "kPc", "kPc")["status"], "R")
        self.assertEqual(edge(self.doc("", "void g(){ Acquire(kPc, seq); }", mp), "kPc", "kPc")["status"], "N")
        self.assertEqual(edge(self.doc("", "void g(){ m.Acquire(kPc, req.seq - 1); }", mp), "kPc", "kPc")["status"], "R")


class RepoTests(unittest.TestCase):
    """Invariants of the real tree that lane fixes must not break (the X / N rows are NOT pinned)."""

    @classmethod
    def setUpClass(cls):
        cls.doc = PC.build(REPO)

    def test_all_declared_products_parsed(self):
        ids = {t["id"] for t in self.doc["products"]}
        for k in ("kP1HalfLuma", "kP2VectorPair", "kP2mMatcherPair", "kP3AnalysisField", "kP4SourceStats", "kP7CameraFit",
                  "kP8CameraLabels", "kP9CameraSmoothed", "kP10GrainSplit", "kP11RegionBlockStats", "kP12LightLevel",
                  "kTdBlocks", "kSourceIdentity", "kSourceGeometry", "kCodingEvidence", "kQualityPattern", "kRefinedField"):
            self.assertIn(k, ids)
        self.assertGreaterEqual(len(ids), 30)

    def test_scheduler_acyclic_and_complete(self):
        s = self.doc["scheduler"]
        self.assertTrue(s["acyclic"], s["cycle_members"])
        self.assertEqual(len(s["topological_order"]), len(self.doc["products"]))
        pos = {k: i for i, k in enumerate(s["topological_order"])}
        for a, b in s["ordering_edges"]:
            self.assertLess(pos[a], pos[b])

    def test_every_declared_edge_has_a_verdict(self):
        for t in self.doc["products"]:
            for e in t["edges"]:
                self.assertIn(e["status"], ("R", "X", "N", "NP"), t["id"] + "<-" + e["input"])
                if e["status"] == "X":
                    self.assertTrue(e["evidence"], t["id"] + "<-" + e["input"])

    def test_stable_rows(self):
        self.assertEqual(edge(self.doc, "kP4SourceStats", "kP1HalfLuma")["status"], "R")
        self.assertEqual(edge(self.doc, "kP11RegionBlockStats", "kP10GrainSplit")["status"], "R")
        self.assertEqual(edge(self.doc, "kP12LightLevel", "kP11RegionBlockStats")["status"], "R")

    def test_declared_unread_today(self):
        # refs/128: the lane jobs drop P13 (the tap view is read instead). The day a lane reads it this row becomes R.
        self.assertIn(edge(self.doc, "kP10GrainSplit", "kP13SourceRing")["status"], ("N", "R"))

    def test_p16_is_a_real_producer(self):
        # L-P16 (r2/job1-p16): kCodingEvidence owner fill is registered, so its edges are verdicts, never NP.
        self.assertEqual(sum(1 for t in self.doc["products"] if t["id"] == "kCodingEvidence"), 1,
                         "exactly one kCodingEvidence row (the loop below must not pass vacuously)")
        for t in self.doc["products"]:
            if t["id"] == "kCodingEvidence":
                self.assertFalse(t["no_producer"])
                self.assertTrue(any(f.endswith("CodingEvidence.cpp") for f in t["producer_files"]))
                self.assertTrue(all(e["status"] != "NP" for e in t["edges"]))

    def test_p16_self_edge_is_not_R_while_the_fill_ignores_its_memo(self):
        # jprod-a audit SHOULD 1: the declared P16 lag-1 self edge is read only if the owner fill asks its memo for the
        # previous frame. MakeOwnerFill's fill takes the SkeletonMemo unnamed (ignored); Hub::AcquireTiles is the consumer.
        path = os.path.join(REPO, "src", "VP-V", "modules", "input", "source_analysis", "regions", "CodingEvidence.cpp")
        with open(path, encoding="utf-8") as f:
            text = f.read()
        self.assertRegex(text, r"OwnerFillRequest&\s*req\s*,\s*VpSkeleton::SkeletonMemo&\s*,",
                         "the fill now names its memo: re-check the P16 lag-1 verdict and update this pin")
        self.assertEqual(edge(self.doc, "kCodingEvidence", "kCodingEvidence")["status"], "N")

    def test_every_no_producer_product_has_an_owner_or_trigger(self):
        self.assertEqual(self.doc["unowned_no_producer"], [])

    def test_p13_edges_of_p10_p11_are_interim(self):
        for pid in ("kP10GrainSplit", "kP11RegionBlockStats"):
            self.assertTrue(edge(self.doc, pid, "kP13SourceRing")["interim"], pid)

    def test_committed_outputs_fresh(self):
        self.assertEqual(PC.main(["--root", REPO, "--check"]), 0)

    def test_generation_is_deterministic(self):
        self.assertEqual(PC.render_json(PC.build(REPO)), PC.render_json(self.doc))


if __name__ == "__main__":
    unittest.main(verbosity=1)
