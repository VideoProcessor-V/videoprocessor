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
        # X2 left this list at C6-1 U25 C10: the copy was REMOVED by D8 (d51e276c3, L-B6), see test_x2_removed_* below.
        # D4 detects 0 at the tip after its retarget (gate-15a-D4: K2 FitEntryOf); proven by Synthetic.test_d4_* instead.
        # D6 detects 0 at the tip after 15c C6 (the decode moved to the one decode owner); proven by Synthetic.test_d6_* instead.
        for sid in ["D1", "D2", "D3", "D5", "X1", "X3", "X4", "X5", "X6", "X7", "X8"]:
            self.assertIn(sid, self.sites, sid + " not detected at the tip")

    def test_x2_removed_at_the_tip(self):
        # D8 (d51e276c3) deleted kCutShare / kFadeStepShare / FadeCodes and the GLSL tile-share cut and two-step fade
        # laws from DetailPulse and TextureRestore; both read the shared judge's verdict (SceneRestart.h, U7 via P4).
        self.assertNotIn("X2", self.sites)
        tr = os.path.join(REPO, M + "repair/texture_restore")
        for name in ("DetailPulse.h", "DetailPulse.cpp", "TextureRestore.h", "TextureRestore.cpp"):
            with open(os.path.join(tr, name), encoding="utf-8") as fh:
                text = fh.read()
            for law in ("kCutShare", "kFadeStepShare", "FadeCodes", "cut_share", "fade_codes"):
                self.assertNotRegex(text, r"\b" + law + r"\b(?!::)", name + ": " + law)
        with open(os.path.join(tr, "DetailPulse.h"), encoding="utf-8") as fh:
            self.assertIn("SceneRestart.h", fh.read())

    def test_x2_pattern_still_detects_a_private_judge(self):
        for f in ("DetailPulse.h", "TextureRestore.cpp"):
            root = make_tree({M + "repair/texture_restore/" + f: "constexpr double kCutShare = 0.5;\nfloat s = kFadeStepShare;\n"})
            self.assertEqual(len([h for h in N1.scan(root) if h["site"] == "X2"]), 2, f)

    def test_x1_is_file_level_inside_the_owner_folder(self):
        x1 = [h for h in self.hits if h["site"] == "X1"]
        self.assertTrue(all("matcher/DejudderWarp" in h["file"] for h in x1))
        self.assertTrue(x1)

    def test_x8_names_texture_restore_p10(self):
        self.assertTrue(any(h["rule"] == "b" and h["product"] == "kP10GrainSplit" and "texture_restore" in h["file"]
                            for h in self.hits))

    def test_rule_c_flags_td_grain_split_compute(self):
        # Anchored by the site's text, not its line number (C6-1 U25 C9: the site moved 141 -> 142 with an include).
        site = [h for h in self.hits if h["rule"] == "c" and h["file"].endswith("temporal_detail/TemporalDetailModel.h")
                and "GrainSplit::Compute(y, scale, B, SplitParams())" in h["text"]]
        self.assertEqual(len(site), 1, site)
        with open(os.path.join(REPO, site[0]["file"]), encoding="utf-8") as fh:
            self.assertIn("GrainSplit::Compute(y, scale, B, SplitParams())", fh.read().splitlines()[site[0]["line"] - 1])

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

    def test_d4_second_field_read_in_the_label_fill_hits(self):
        # gate-15a-D4 fixtures: a field read other than the fit entry's own field in the label fill / producer files hits;
        # the one pool read FitEntryOf does not; FlowOf outside the four files (CameraExtension.cpp) is not D4.
        fill = M + "motion/estimation/global_model/link/CameraLabelFill.cpp"
        prod = M + "motion/estimation/global_model/link/CameraLabelProducer.h"
        for text in ("auto f = m_link.FieldOf(seq);",
                     "VerifiedField v = vectorEngine.FieldOf(seq, f);",
                     "const auto* fld = dejudder->Field();"):
            root = make_tree({fill: text})
            self.assertEqual([h["file"] for h in N1.scan(root) if h["site"] == "D4"], [fill], text)
        root = make_tree({prod: "auto p = engine.Acquire(VpSkeleton::kP3AnalysisField, form);"})
        self.assertEqual([h["file"] for h in N1.scan(root) if h["site"] == "D4"], [prod])
        root = make_tree({fill: "const bool ok = m_link.FitEntryOf(seq, rec, field);",
                          M + "motion/estimation/global_model/link/CameraExtension.cpp": "auto fl = engine.FlowOf(a, b);"})
        self.assertFalse([h for h in N1.scan(root) if h["site"].startswith("D4")])

    def test_d6_planted_private_decode_hits(self):
        # D6 planted fixtures (15c C6): a private RGB / full-range decode returning to the producer hits D6; the old wrapper name
        # anywhere else hits D6~moved; asking the owner's query API (MapsToLimitedDomain) does not.
        vfa = M + "input/source_analysis/VpFrameAnalysis.cpp"
        host = M + "input/source_analysis/VpFrameAnalysisHost.h"
        root = make_tree({vfa: "static bool SamplerCodesAreFullRange(const S& s){ return s.CodesFullRange(); }\n"
                               "const bool full = source.fullRangeYuv;",
                          host: "const bool f = VpFrameAnalysis::SamplerCodesAreFullRange(s);"})
        hs = sorted((h["site"], h["file"]) for h in N1.scan(root) if h["site"].startswith("D6"))
        self.assertEqual(hs, [("D6", vfa), ("D6", vfa), ("D6~moved", host)])
        root = make_tree({vfa: "if (fmt == P010 && source.MapsToLimitedDomain()) {}", host: "const bool f = s.CodesFullRange();"})
        self.assertFalse([h for h in N1.scan(root) if h["site"].startswith("D6")])

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

    def test_rule_b_acquire_of_the_product_is_a_read(self):
        cat = ('inline const std::vector<ConsumerRow>& ConsumerRows(){ r.push_back(MakeRow(1, "texture restore", '
               '"VP-V/modules/repair/texture_restore", PathwayUnit::Repair, 30, { kP10GrainSplit }, own, "k")); }')
        for read in ("auto* g = memo.Acquire(kP10GrainSplit, form);", "auto* g = memo.Acquire(VpSkeleton::kP10GrainSplit, form);"):
            root = make_tree({"src/VP-V/core/ProductCatalog.h": cat, M + "repair/texture_restore/T.cpp": read})
            self.assertFalse([h for h in N1.scan(root) if h["rule"] == "b"], read)
        root = make_tree({"src/VP-V/core/ProductCatalog.h": cat, M + "repair/texture_restore/T.cpp": "int kP10GrainSplit = 0;"})
        self.assertTrue([h for h in N1.scan(root) if h["rule"] == "b"])   # naming the id is not a read

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

    def test_d5_private_msb_shift_outside_the_owner_law_is_moved(self):
        # gate-15c-D5 (b): a planted private MSB-align shift in a module file is D5~moved; the owner law is not.
        root = make_tree({M + "scaling/chroma_upscale/Copy.cpp": "dst[x] = uint16_t(src[x] << view.msbShift);",
                          "src/VP-V/core/LaneInterfaces.h": "uint16_t MsbAlign(uint16_t raw) const { return uint16_t(uint32_t(raw) << msbShift); }"})
        hs = [(h["site"], h["file"]) for h in N1.scan(root) if h["site"].startswith("D5")]
        self.assertEqual(hs, [("D5~moved", M + "scaling/chroma_upscale/Copy.cpp")])

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

    def test_d3s_search_pass_outside_the_owner_file(self):
        # gate-15a-D3s fixtures: a SAVE MV / LUMAQ / LUMAC pass in a shader or in a non-owner header raw string hits;
        # the old comment wording and the owner file do not.
        owner = M + "motion/estimation/fallback/OutputDomainSearchGlsl.h"
        root = make_tree({
            "shaders/Fixture.glsl": "//!HOOK OUTPUT\n//!SAVE MV\nvoid hook(){}\n",
            M + "output/dither/Copy.h": 'const char* k = R"(\n//!SAVE LUMAQ\n//!DESC copy\n)";\n',
            "shaders/Comment.glsl": "/* full search within the block */\n//!SAVE SIGMA_DELTA_STATE\n",
            owner: 'const char* p = R"(\n//!SAVE LUMAQ\n)";\nconst char* q = R"(\n//!SAVE LUMAC\n//!SAVE MV\n)";\n',
        })
        hs = sorted(h["file"] for h in N1.scan(root) if h["site"] == "D3s")
        self.assertEqual(hs, sorted(["shaders/Fixture.glsl", M + "output/dither/Copy.h"]))

    def test_x8_owner_edge_excuses_only_its_row_and_product(self):
        # gate-15a-X8-row14-P2 fixtures: the producer row's P2 edge holds only with the computed-flow publish in the named file,
        # under the product's owner folder; it never excuses the row's other products or another row.
        prod = M + "motion/estimation/MotionAheadProducer.cpp"
        pub = "void R(){ slot->Publish(previousSequence, job.sequence, job.generation, flow); }"

        def cat(row_id=14, folder="VP-V/modules/motion/estimation/MotionAheadProducer", owner="motion/estimation"):
            return ('inline const std::vector<ProductRow>& ProductRows(){ static const std::vector<ProductRow> rows = {'
                    ' { kP2VectorPair, "' + owner + '", kExpectedVectorUnit, "NVOF producer (MotionAheadProducer)" } }; }\n'
                    'r.push_back(MakeRow(' + str(row_id) + ', "optical-flow producer", "' + folder + '", PathwayUnit::Input, 5, '
                    '{ kP1HalfLuma, kP2VectorPair }, own, "product.nvof_producer"));')

        def x8(files):
            return sorted(h["product"] for h in N1.scan(make_tree(files)) if h["site"] == "X8")
        self.assertEqual(x8({"src/VP-V/core/ProductCatalog.h": cat(), prod: pub}), ["kP1HalfLuma"])   # P1 still governed
        self.assertEqual(x8({"src/VP-V/core/ProductCatalog.h": cat(),
                             prod: "void R(){ slot->Publish(a, b, c, std::make_shared<Flow>()); }"}),
                         ["kP1HalfLuma", "kP2VectorPair"])                                              # empty publish only
        self.assertEqual(x8({"src/VP-V/core/ProductCatalog.h": cat(), prod: "// " + pub}),
                         ["kP1HalfLuma", "kP2VectorPair"])                                              # writer in a comment
        self.assertEqual(x8({"src/VP-V/core/ProductCatalog.h": cat(),
                             M + "motion/estimation/MotionAheadProducerNew.cpp": pub}),
                         ["kP1HalfLuma", "kP2VectorPair"])                                              # file renamed
        self.assertEqual(x8({"src/VP-V/core/ProductCatalog.h": cat(row_id=13), prod: pub}),
                         ["kP1HalfLuma", "kP2VectorPair"])                                              # another row id
        self.assertEqual(x8({"src/VP-V/core/ProductCatalog.h": cat(owner="input/capture"), prod: pub}),
                         ["kP1HalfLuma", "kP2VectorPair"])                                              # not under the owner folder

    def test_x8_p2_read_through_set_external_flow(self):
        # gate-15a-X8-row13-P2 fixtures: SetExternalFlow( is a P2 read; using only the MotionAhead::Flow type is not.
        cat = ('r.push_back(MakeRow(13, "matcher / motion warp", "VP-V/modules/motion/estimation/matcher/DejudderWarp", '
               'PathwayUnit::Motion, 20, { kP2VectorPair }, own, "product.motion_warp"));')
        w = M + "motion/estimation/matcher/DejudderWarp.h"

        def x8(text):
            return [h for h in N1.scan(make_tree({"src/VP-V/core/ProductCatalog.h": cat, w: text})) if h["site"] == "X8"]
        self.assertFalse(x8("void SetExternalFlow(std::shared_ptr<const MotionAhead::Flow> flow);"))
        self.assertTrue(x8("std::shared_ptr<MotionAhead::Flow> own = std::make_shared<MotionAhead::Flow>();"))

    @staticmethod
    def _view_tree(row_id, prod, flag_kind, folder, files, note="n"):
        cat = ('r.push_back(MakeRow(' + str(row_id) + ', "row", "VP-V/modules/' + folder + '", PathwayUnit::Repair, 30, '
               '{ ' + prod + ' }, own, "product.x"));\n')
        if flag_kind:
            cat += ('inline const std::vector<ConsumerFlag>& ConsumerFlags(){ static const std::vector<ConsumerFlag> flags = {\n'
                    '  { uint16_t(kCatalogConsumerIdBase + ' + str(row_id) + '), ' + prod + ', FlagKind::' + flag_kind
                    + ', "D8", "' + note + '" },\n }; return flags; }\n')
        tree = {"src/VP-V/core/ProductCatalog.h": cat}
        tree.update({M + folder + "/" + k: v for k, v in files.items()})
        return [h for h in N1.scan(make_tree(tree)) if h["site"] == "X8"]

    def test_c7_p4_view_accessor_needs_a_view_flag(self):
        # gate-15c-X8-rows0-1-P4 fixtures: the call-site form is accepted only with a ViewIsActual / ExternalView flag of that
        # (row, product); NotRead, no flag or a flag of another row never excuse; the judge's own definition is not a call site.
        call = {"T.cpp": "auto s = VpDetailRestore::SceneRestartOf(params.sequence, params.sceneReader);"}
        self.assertFalse(self._view_tree(1, "kP4SourceStats", "ViewIsActual", "repair/texture_restore", call))
        self.assertFalse(self._view_tree(1, "kP4SourceStats", "ExternalView", "repair/texture_restore", call))
        self.assertTrue(self._view_tree(1, "kP4SourceStats", "NotRead", "repair/texture_restore", call))
        self.assertTrue(self._view_tree(1, "kP4SourceStats", None, "repair/texture_restore", call))
        self.assertTrue(self._view_tree(1, "kP4SourceStats", "ViewIsActual", "repair/texture_restore",
                                        {"SceneRestart.h": "inline SceneRestartAnswer SceneRestartOf(uint64_t sequence, F r);"
                                                           " bool p = store->PeekScene(sequence, scene);"}))
        self.assertTrue(self._view_tree(1, "kP4SourceStats", "ViewIsActual", "repair/texture_restore",
                                        {"T.cpp": "// SceneRestartOf(params.sequence, r);"}))   # a comment is not a call

    def test_c7_p13_view_accessor_only_in_the_note_file(self):
        # gate-15c-X8-row11-P13 fixtures: the broad pl_frame form is accepted only with the ViewIsActual flag AND in the file the
        # flag note names; the same signature in another member file, a NotRead flag or no flag leave the X8 hit.
        sig = "bool CflModule::Apply(pl_gpu gpu, pl_dispatch dispatch, const pl_frame& source, const Params& p, pl_frame& r);"
        note = "render-time source pl_frame (scaling/chroma_upscale/ChromaCflGpu.cpp CflModule::Apply)"
        f = "scaling/chroma_upscale"
        self.assertFalse(self._view_tree(11, "kP13SourceRing", "ViewIsActual", f, {"ChromaCflGpu.cpp": sig}, note))
        self.assertTrue(self._view_tree(11, "kP13SourceRing", "ViewIsActual", f, {"basic/BasicChromaGpu.cpp": sig}, note))
        self.assertTrue(self._view_tree(11, "kP13SourceRing", "NotRead", f, {"ChromaCflGpu.cpp": sig}, note))
        self.assertTrue(self._view_tree(11, "kP13SourceRing", None, f, {"ChromaCflGpu.cpp": sig}, note))
        self.assertTrue(self._view_tree(11, "kP13SourceRing", "ViewIsActual", f, {"ChromaCflGpu.cpp": sig}, "no file named"))

    def test_c7_p3_params_field_only_with_the_view_flag_in_the_note_file(self):
        # gate-15c-X8-row2-P3-r2 fixtures (a)-(d).
        use = {"VpDebander.cpp": "const bool haveField = params.field != nullptr;"}
        note = "live matcher field read at render (repair/deband/VpDebander.cpp Params.field)"
        f = "repair/deband"
        self.assertTrue(self._view_tree(2, "kP3AnalysisField", None, f, use, note))                          # (a) no flag
        self.assertTrue(self._view_tree(2, "kP3AnalysisField", "NotRead", f, use, note))                     # (b) NotRead
        self.assertTrue(self._view_tree(2, "kP3AnalysisField", "ViewIsActual", f, use,
                                        "live matcher field (repair/deband/Other.cpp)"))                     # (c) other file
        self.assertFalse(self._view_tree(2, "kP3AnalysisField", "ViewIsActual", f, use, note))               # (d)

    def test_bad_root_is_exit_2(self):
        self.assertEqual(N1.main(["--root", tempfile.mkdtemp()]), 2)


if __name__ == "__main__":
    unittest.main(verbosity=1)
