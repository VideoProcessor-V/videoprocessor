# Copyright(C) 2026 VP-V contributors
# SPDX-License-Identifier: GPL-3.0-only
"""Tests for tools/check_private_copies.py (N1). Run: python -I tools/test_check_private_copies.py"""
import json
import os
import sys
import tempfile
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import check_private_copies as N1  # noqa: E402

REPO = os.path.abspath(os.path.join(HERE, ".."))
M = "src/VP-V/modules/"


def make_tree(files):
    root = tempfile.mkdtemp()
    for rel, text in files.items():
        p = os.path.join(root, rel)
        os.makedirs(os.path.dirname(p), exist_ok=True)
        with open(p, "w", encoding="utf-8") as fh:
            fh.write(text)
    return root


class RepoSeeds(unittest.TestCase):
    """The checker must see every known copy at the tip (D1-D6, X1-X8)."""

    @classmethod
    def setUpClass(cls):
        if not os.path.isfile(os.path.join(REPO, "src/VP-V/core/Products.h")):
            raise unittest.SkipTest("no repository tree")
        cls.hits = N1.scan(REPO)
        cls.sites = {h["site"] for h in cls.hits}

    def test_every_d_and_x_site_detected(self):
        for sid in ["D1", "D2", "D3", "D4", "D5", "D6", "X1", "X2", "X3", "X4", "X5", "X6", "X7", "X8"]:
            self.assertIn(sid, self.sites, sid + " not detected at the tip")

    def test_x1_is_file_level_inside_the_owner_folder(self):
        x1 = [h for h in self.hits if h["site"] == "X1"]
        self.assertTrue(all("matcher/DejudderWarp" in h["file"] for h in x1))
        self.assertTrue(x1)

    def test_x8_names_texture_restore_p10(self):
        self.assertTrue(any(h["rule"] == "b" and h["product"] == "kP10GrainSplit" and "texture_restore" in h["file"]
                            for h in self.hits))

    def test_rule_c_flags_td_grain_split_compute(self):
        self.assertTrue(any(h["rule"] == "c" and h["file"].endswith("temporal_detail/TemporalDetailModel.h") and h["line"] == 141
                            for h in self.hits))

    def test_strict_passes_with_the_baseline(self):
        self.assertEqual(N1.main(["--strict", "--root", REPO]), 0)

    def test_baseline_entries_have_reason_and_refs(self):
        base = json.load(open(os.path.join(REPO, N1.BASELINE), encoding="utf-8"))
        for e in base["entries"]:
            self.assertTrue(e.get("reason") and e.get("refs"), e["key"])


class Synthetic(unittest.TestCase):
    def test_private_call_outside_owner_is_hit_and_owner_is_not(self):
        root = make_tree({
            M + "motion/estimation/OpticalFlowTrust.h": "inline float CellTrust(){ return OpticalFlowTrust::CellTrust(1); }",
            M + "repair/x/Use.h": "float t = OpticalFlowTrust::CellTrust(2);",
        })
        hs = [h for h in N1.scan(root) if h["site"] == "D2"]
        self.assertEqual([h["file"] for h in hs], [M + "repair/x/Use.h"])

    def test_comments_do_not_hit(self):
        root = make_tree({M + "repair/temporal_detail/TemporalDetailModel.h": "// m_prevGin\n/* double guard */\nint ok;"})
        self.assertFalse([h for h in N1.scan(root) if h["site"] == "D1"])

    def test_owner_compute_without_publish_is_rule_c(self):
        own = M + "input/source_analysis/regions/RegionEvidence.h"
        root = make_tree({own: "auto s = GrainSplit::Compute(y, 1, 8);"})
        self.assertTrue([h for h in N1.scan(root) if h["rule"] == "c"])
        root = make_tree({own: "auto s = GrainSplit::Compute(y, 1, 8); Publish(s);"})
        self.assertFalse([h for h in N1.scan(root) if h["rule"] == "c"])

    def test_declared_unread_product_is_rule_b(self):
        cat = ('inline const std::vector<ConsumerRow>& ConsumerRows(){ r.push_back(MakeRow(1, "texture restore", '
               '"VP-V/modules/repair/texture_restore", PathwayUnit::Repair, 30, { kP10GrainSplit }, own, "k")); }')
        root = make_tree({"src/VP-V/core/ProductCatalog.h": cat,
                          M + "repair/texture_restore/T.cpp": "int x;"})
        self.assertTrue([h for h in N1.scan(root) if h["rule"] == "b"])
        root = make_tree({"src/VP-V/core/ProductCatalog.h": cat,
                          M + "repair/texture_restore/T.cpp": "auto* g = params.grain.Gpu(p);"})
        self.assertFalse([h for h in N1.scan(root) if h["rule"] == "b"])

    def test_rule_d_owner_folders_come_from_the_product_rows(self):
        cat = ('inline const std::vector<ProductRow>& ProductRows(){ static const std::vector<ProductRow> rows = {'
               ' { kP13SourceRing, "input/capture (queue + early upload)", 0, "" },'
               ' { kTdBlocks, "repair/temporal_detail", kX, "reserved (engine: reset 2)" } }; }')
        reg = "d->RegisterOwnerFill(kP13SourceRing, 1);"
        root = make_tree({"src/VP-V/core/ProductCatalog.h": cat, M + "input/capture/SourcePlanes.h": reg,
                          M + "repair/temporal_detail/Stray.cpp": reg, M + "repair/other/Stray2.cpp": reg})
        ds = sorted(h["file"] for h in N1.scan(root) if h["rule"] == "d")
        self.assertEqual(ds, [M + "repair/other/Stray2.cpp", M + "repair/temporal_detail/Stray.cpp"])   # capture ok; reserved row and unknown folder are not owners
        root = make_tree({M + "input/capture/SourcePlanes.h": reg})   # no catalog: the fixed list only
        self.assertEqual([h["file"] for h in N1.scan(root) if h["rule"] == "d"], [M + "input/capture/SourcePlanes.h"])

    def test_rule_c_outside_owner_folder_only(self):
        call = "auto s = GrainSplit::Compute(y, 1, 8);"
        root = make_tree({M + "repair/temporal_detail/T.h": call, M + "repair/grain_split/G.h": call})
        cs = [h["file"] for h in N1.scan(root) if h["rule"] == "c"]
        self.assertEqual(cs, [M + "repair/temporal_detail/T.h"])
        root = make_tree({M + "repair/temporal_detail/T.h": "auto s = GrainSplit::Compute(y, 1, 8);"+chr(10)+"Publish(s);"})
        self.assertFalse([h for h in N1.scan(root) if h["rule"] == "c"])

    def test_rule_d_register_only_from_owner_folders(self):
        root = make_tree({M + "repair/deband/D.cpp": "lane.RegisterOwnerFill(k, f);",
                          M + "control/skeleton/S.cpp": "lane.RegisterOwnerFill(k, f);"})
        self.assertEqual([h["file"] for h in N1.scan(root) if h["rule"] == "d"], [M + "repair/deband/D.cpp"])

    def test_moved_copy_is_found_in_a_new_file(self):
        root = make_tree({M + "repair/temporal_detail/TemporalDetailModel.h": "int ok;",
                          M + "repair/temporal_detail/Other.h": "double v = m_prevGin;"})
        self.assertEqual([h["site"] for h in N1.scan(root) if "~moved" in h["site"]], ["D1~moved"])

    def test_update_baseline_refuses_a_new_key(self):
        root = make_tree({M + "repair/deband/VpDebander.cpp": "SPLIT_TENSOR_BODY;"})
        os.makedirs(os.path.join(root, "tools"), exist_ok=True)
        json.dump({"entries": [{"key": "a|X9|f", "rule": "a", "site": "X9", "max_hits": 1, "reason": "r", "refs": "x"}]},
                  open(os.path.join(root, N1.BASELINE), "w"))
        self.assertEqual(N1.main(["--update-baseline", "--root", root]), 1)

    def test_ratchet_rise_new_and_blind(self):
        h = [{"rule": "a", "site": "D1", "file": "f", "line": 1, "text": "", "product": "p"}] * 2
        base = {"entries": [{"key": "a|D1|f", "site": "D1", "max_hits": 1, "seed": True}]}
        self.assertTrue(any(v.startswith("RISE") for v in N1.compare(h, base)[0]))
        self.assertTrue(any(v.startswith("NEW") for v in N1.compare(h, {"entries": []})[0]))
        self.assertTrue(any(v.startswith("BLIND") for v in N1.compare([], base)[0]))
        self.assertEqual(N1.compare([], {"entries": [{"key": "a|D1|f", "site": "D1", "max_hits": 1}]})[0], [])

    def test_update_baseline_refuses_a_rise(self):
        root = make_tree({M + "repair/deband/VpDebander.cpp": "const char* SPLIT_TENSOR_BODY = 1;\nSPLIT_TENSOR_BODY;"})
        os.makedirs(os.path.join(root, "tools"), exist_ok=True)
        key = "a|X4|" + M + "repair/deband/VpDebander.cpp"
        json.dump({"entries": [{"key": key, "rule": "a", "site": "X4", "max_hits": 1, "reason": "r", "refs": "x"}]},
                  open(os.path.join(root, N1.BASELINE), "w"))
        self.assertEqual(N1.main(["--update-baseline", "--root", root]), 1)

    def test_bad_root_is_exit_2(self):
        self.assertEqual(N1.main(["--root", tempfile.mkdtemp()]), 2)


if __name__ == "__main__":
    unittest.main(verbosity=1)
