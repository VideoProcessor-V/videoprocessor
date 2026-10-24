#!/usr/bin/env python3
"""Tests for tools/net_stamp.py (stdlib unittest; run: python -B tools/test_net_stamp.py).

Synthetic exports are built in a temporary folder (never in a kit folder). When this machine has
the round-1 smoke export (D:\\nn-corpus\\VP-NN-TrainKit\\smoke\\staging), it is stamped read-only too.
"""
import copy
import json
import os
import shutil
import sys
import tempfile
import unittest

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import net_stamp as ns  # noqa: E402

ORDER = ("dv_reconstruction(nets never see raw DV IPT)>detect_on_original(kernel, labels)>crop>A_on_original_delivered_frame>"
         "per_axis[1D_rebase_pass(kernel-matched 2:1 or 1D Lanczos, r16)>C]>per_axis[x2_pass>B(H r16 hand-off)]>hybrid_ssim_downscale(w0.2, lanczos base, ortho)>D>rest_of_VP")
CFG = {"split_eps_codes": 6.0, "split_r2160": 2, "bound": {"b0": 1.5, "b1": 2.0, "b2": 0.35, "bmax": 24.0},
       "scale_bound": {"c0": 1.0, "c1": 0.25, "cmax": 32.0}, "delivered_bound": {"c0": 1.5, "c1": 0.35, "cmax": 32.0}}
SETTINGS = {"common": {"deband": "off", "deband_vp": "off", "texture_restore": "off", "detail_pulse_stabilise": "off"}, "down": {}, "up": {}, "adapter": "NVIDIA"}
SMOKE = r"D:\nn-corpus\VP-NN-TrainKit\smoke\staging"
SMOKE_MANIFEST = r"D:\nn-corpus\VP-NN-TrainKit\data\scaling\scaling_manifest.json"


def fingerprint(kind, cfg=CFG, settings=SETTINGS, order=ORDER, **extra):
    grain = dict(operator="self-guided filter", eps_codes=cfg["split_eps_codes"], r2160=cfg["split_r2160"], pad="reflect")
    fp = dict(vp_commit="test", renderer_dll_sha256="x", rebase_settings="r", grain_split=ns.h(grain), stage_order=ns.h(order),
              scaler_blend_settings=ns.h({k: v for k, v in settings.items() if k in ("common", "down", "up", "adapter")}),
              bounds=ns.h({"bound": cfg["bound"], "scale_bound": cfg["scale_bound"]}), input_stage=f"{kind}: test",
              stage_contract=f"test contract | {kind}",
              texture_formats="all net intermediates rgba32f (test); re-base pass outputs r16; x2 H->V hand-off r16 (quantised in-shader)")
    fp.update(extra)
    fp["fingerprint"] = ns.h(fp)
    fp["stage_order_text"] = order
    return fp


def a_shader(fid, c0="1.5", c1="0.3499999940395355", cmax="32.0", params=True):
    p = "//!PARAM vpnn_ry\n//!TYPE float\n//!MINIMUM 0.0\n//!MAXIMUM 1.0\n1.0\n\n" if params else ""
    return (f"// VP-NN T: test\n// VPNN pipeline fingerprint: {fid} input_stage=A: test\n{p}"
            "//!DESC VPNN A output (pixel shuffle, bounded; gate off: exact copy)\n//!HOOK LUMA\n//!BIND HOOKED\n//!COMPONENTS 1\n\n"
            f"vec4 hook() {{\n    float bound = clamp({c0} + {c1} * (mx - mn) * 1023.0, 0.0, {cmax}) / 1023.0;\n    return vec4(0.0);\n}}\n")


def plain_shader(fid, kind):
    return f"// VP-NN T: test\n// VPNN pipeline fingerprint: {fid} input_stage={kind}: test\n//!DESC x\n//!HOOK LUMA\n//!BIND HOOKED\n//!COMPONENTS 1\n\nvec4 hook() {{ return vec4(0.0); }}\n"


class StampTests(unittest.TestCase):
    def setUp(self):
        self.dir = tempfile.mkdtemp(prefix="vp_stamp_test_")
        os.makedirs(os.path.join(self.dir, "shaders"))

    def tearDown(self):
        shutil.rmtree(self.dir, ignore_errors=True)

    def write(self, name, fp, shaders, cfg=CFG, settings=SETTINGS):
        for sname, text in shaders.items():
            with open(os.path.join(self.dir, "shaders", sname), "w", newline="\n") as f:
                f.write(text)
        ex = os.path.join(self.dir, f"export_{name}.json")
        with open(ex, "w") as f:
            json.dump({"shaders": list(shaders), "fingerprint": fp}, f)
        cp = os.path.join(self.dir, "config.json")
        with open(cp, "w") as f:
            json.dump(cfg, f)
        mp = os.path.join(self.dir, "scaling_manifest.json")
        with open(mp, "w") as f:
            json.dump({"settings": settings}, f)
        return ex, cp, mp

    def test_c_stamps_from_the_records(self):
        fp = fingerprint("C", c_structure="single-axis refine; passes H then V, 16-bit UNORM pass outputs")
        ex, cp, _ = self.write("C", fp, {"C_h.glsl": plain_shader(fp["fingerprint"], "C"), "C_v.glsl": plain_shader(fp["fingerprint"], "C")})
        s = ns.stamp(ex, cp)
        self.assertEqual(s["fields"]["upstream_key"], "repair.encoding")
        self.assertEqual(s["fields"]["pass_order"], "H>V")
        self.assertEqual(s["fields"]["intermediate_format"], "rgba32f")
        self.assertEqual(s["fields"]["handoff_format"], "r16")
        self.assertEqual(s["fields"]["label_schema"], "none")
        self.assertEqual(s["fields"]["delivered_bound"], "n/a")
        self.assertEqual(sorted(s["shaders"]), ["C_h.glsl", "C_v.glsl"])
        self.assertEqual(s["export_fingerprint"], fp["fingerprint"])

    def test_a_takes_delivered_bound_only_when_the_shader_used_it(self):
        fp = fingerprint("A", label_schema="vpnn-labels-v1")
        ex, cp, _ = self.write("A", fp, {"A.glsl": a_shader(fp["fingerprint"])})
        s = ns.stamp(ex, cp)
        self.assertEqual(s["fields"]["upstream_key"], "")
        self.assertEqual(s["fields"]["delivered_bound"], {"c0": 1.5, "c1": 0.35, "cmax": 32.0})
        self.assertEqual(s["fields"]["label_schema"], "vpnn-labels-v1")
        ex, cp, _ = self.write("A", fp, {"A.glsl": a_shader(fp["fingerprint"], c1="0.25")})
        with self.assertRaisesRegex(ns.Refuse, "bound constants"):
            ns.stamp(ex, cp)
        cfg = copy.deepcopy(CFG)
        del cfg["delivered_bound"]
        ex, cp, _ = self.write("A", fp, {"A.glsl": a_shader(fp["fingerprint"])}, cfg=cfg)
        with self.assertRaisesRegex(ns.Refuse, "no delivered_bound"):
            ns.stamp(ex, cp)

    def test_labels_without_a_schema_refuse(self):
        fp = fingerprint("A")
        ex, cp, _ = self.write("A", fp, {"A.glsl": a_shader(fp["fingerprint"])})
        with self.assertRaisesRegex(ns.Refuse, "label_schema"):
            ns.stamp(ex, cp)

    def test_edited_fingerprint_refuses(self):
        fp = fingerprint("C")
        fp["stage_contract"] += " (edited)"
        ex, cp, _ = self.write("C", fp, {"C_h.glsl": plain_shader(fp["fingerprint"], "C")})
        with self.assertRaisesRegex(ns.Refuse, "not the recorded id"):
            ns.stamp(ex, cp)

    def test_a_different_config_refuses(self):
        fp = fingerprint("C")
        cfg = copy.deepcopy(CFG)
        cfg["scale_bound"]["cmax"] = 16.0
        ex, cp, _ = self.write("C", fp, {"C_h.glsl": plain_shader(fp["fingerprint"], "C")}, cfg=cfg)
        with self.assertRaisesRegex(ns.Refuse, "not the config"):
            ns.stamp(ex, cp)

    def test_shader_of_another_export_refuses(self):
        fp = fingerprint("C")
        ex, cp, _ = self.write("C", fp, {"C_h.glsl": plain_shader("0123456789abcdef", "C")})
        with self.assertRaisesRegex(ns.Refuse, "does not carry"):
            ns.stamp(ex, cp)

    def test_d_needs_the_render_settings(self):
        fp = fingerprint("D")
        ex, cp, mp = self.write("D", fp, {"D.glsl": plain_shader(fp["fingerprint"], "D")})
        with self.assertRaisesRegex(ns.Refuse, "--scaling-manifest"):
            ns.stamp(ex, cp)
        s = ns.stamp(ex, cp, mp)
        self.assertEqual(s["fields"]["upstream_key"], "repair.encoding>scaling.rebase>scaling.route_4k")
        on = copy.deepcopy(SETTINGS)
        on["common"]["deband"] = "on"
        fp2 = fingerprint("D", settings=on)
        ex, cp, mp = self.write("D", fp2, {"D.glsl": plain_shader(fp2["fingerprint"], "D")}, settings=on)
        with self.assertRaisesRegex(ns.Refuse, "deband"):
            ns.stamp(ex, cp, mp)
        ex, cp, mp = self.write("D", fp, {"D.glsl": plain_shader(fp["fingerprint"], "D")}, settings=on)
        with self.assertRaisesRegex(ns.Refuse, "scaler_blend_settings"):
            ns.stamp(ex, cp, mp)

    def test_unknown_stage_token_refuses(self):
        order = ORDER.replace("crop>", "crop>mystery_stage>")
        fp = fingerprint("C", order=order)
        ex, cp, _ = self.write("C", fp, {"C_h.glsl": plain_shader(fp["fingerprint"], "C")})
        with self.assertRaisesRegex(ns.Refuse, "mystery_stage"):
            ns.stamp(ex, cp)

    def test_missing_pass_order_refuses(self):
        fp = fingerprint("A", label_schema="vpnn-labels-v1", texture_formats="all net intermediates rgba32f")
        ex, cp, _ = self.write("A", fp, {"A.glsl": a_shader(fp["fingerprint"])})
        with self.assertRaisesRegex(ns.Refuse, "pass order"):
            ns.stamp(ex, cp)

    def test_cli_writes_only_the_sidecar(self):
        fp = fingerprint("C")
        ex, cp, _ = self.write("C", fp, {"C_h.glsl": plain_shader(fp["fingerprint"], "C")})
        out = os.path.join(self.dir, "out")
        self.assertEqual(ns.main(["--export", ex, "--config", cp, "--out", out]), 0)
        self.assertEqual(os.listdir(out), ["export_C.vpstamp.json"])
        fp["stage_contract"] += "x"
        ex, cp, _ = self.write("C", fp, {"C_h.glsl": plain_shader(fp["fingerprint"], "C")})
        self.assertEqual(ns.main(["--export", ex, "--config", cp, "--out", os.path.join(self.dir, "out2")]), 2)
        self.assertFalse(os.path.exists(os.path.join(self.dir, "out2")))

    @unittest.skipUnless(os.path.exists(os.path.join(SMOKE, "export_C_rebase_refine.json")), "no smoke export here")
    def test_real_smoke_exports(self):
        cfg = os.path.join(SMOKE, "config.json")
        want = {"A_single_small": "", "A_motion_small": "", "C_rebase_refine": "repair.encoding"}
        for net, key in want.items():
            self.assertEqual(ns.stamp(os.path.join(SMOKE, f"export_{net}.json"), cfg)["fields"]["upstream_key"], key)
        if os.path.exists(SMOKE_MANIFEST):
            s = ns.stamp(os.path.join(SMOKE, "export_D_hybrid_refine.json"), cfg, SMOKE_MANIFEST)
            self.assertEqual(s["fields"]["upstream_key"], "repair.encoding>scaling.rebase>scaling.route_4k")


if __name__ == "__main__":
    unittest.main()
