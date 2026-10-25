#!/usr/bin/env python3
# Copyright(C) 2026 VP-V contributors
# SPDX-License-Identifier: GPL-3.0-only
"""Synthetic DV tunnel validation: writes the GPU render jobs and runs the analysis.

  python -I run_dv_synth.py jobs    <corpus-root> <jobs.txt> [id-substring]
  python -I run_dv_synth.py analyze <corpus-root> <jobs.txt> <render-out> <report.json> [id-substring]

The jobs (DvSynthGpuRendersJobs renders them through VP's whole renderer):
  acc_*     decode accuracy: each accuracy segment with tone mapping out of the way
            (source range and L1 = the 1000-nit target, BT.2020 target, pure 2.2
            power, full range, dither/deband off), CPU and GPU unpack;
  btb_*     BTB/WTW-like codes below the source minimum / above the source maximum,
            under the cinema mapping;
  pol_*     policy: the 4000-nit master's scenes under the cinema profile at 100 nits
            (sdr_target_nits in docs/cinema/VideoProcessor-test.cfg) and 48 nits,
            L1 only / the nearest trim / dv_trims' interpolation, and at 300 nits
            where nearest and blend differ;
  lldv_*    LLDV-like companions of the same content (PQ BT.2020 V210).
"""
import json
import os
import sys

import numpy as np

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import dvsynth as dv  # noqa: E402
import dm_model  # noqa: E402

# Owner direction 2026-10-24: (a) accuracy, (c) alignment, (d) policy and (e) LLDV vs tunnel measure the
# FAITHFUL DV path: only the capture unpack (CPU / GPU), the DM packet extraction and libplacebo's
# Dolby Vision decode and tone mapping run. Every VP stage is switched off here, and the GPU test
# asserts from the renderer's log and status that each one stayed inactive (faithful=1 jobs).
# Kept identical to kDvSynthFaithful in RendererCaptureHarnessTests.cpp.
FAITHFUL = ("source_resolution: native\\nsource_resolution_crossfade_s: 0\\ndeband: off\\ndeband_vp: off\\n"
            "detail_pulse_stabilise: off\\ntexture_restore: off\\ndejudder_strength: 0\\ninterpolation: off\\n"
            "interpolation_fill_gaps: off\\nintent_motion: off\\nintent_content: off\\n"
            "intent_black: off\\nintent_display: off\\ndithering: off\\ntemporal_dither: off\\n"
            "adaptive_upscale: off\\nadaptive_downscale: off\\nadaptive_grain_map: off\\nadaptive_chroma_ssim: off\\n")
# (automatic_crop / subtitle_fit are viewport and subtitle settings, rejected in renderer profiles: the
# harness has no viewport or subtitle profile, so neither is active.)

NO_TM = ("[vprenderer.Default]\\n" + FAITHFUL + "tone_mapping: spline\\npeak_detection: off\\n"
         "sdr_target_nits: 1000\\nsdr_black_nits: 0\\ncontrast_recovery: 0\\noutput_gamma: 2.2\\n"
         "target_primaries: BT2020\\noutput_range: full\\n")


def cinema(target, tm="spline", faithful=True, dolby_style=False):
    # docs/cinema/VideoProcessor-test.cfg: spline, peak_detection off, gamut auto, contrast
    # recovery 0.3, P3 D65 target, 2.2 power, 100 nits. Dither off for measurement.
    # dolby_style: dv_tone_mapping dolby_style (DvDolbyStyle.h). Settings use "\\n" escapes only: a
    # real new line ends the job line (2026-10-24 the Dolby-style jobs lost every setting after
    # tone_mapping that way and rendered bt2390 at the 203-nit sRGB default; the harness now rejects it).
    return ("[vprenderer.Default]\\n" + (FAITHFUL if faithful else "dithering: off\\n") +
            "tone_mapping: %s\\n%speak_detection: off\\n"
            "gamut_mapping: auto\\ncontrast_recovery: 0.3\\nsdr_target_nits: %g\\nsdr_black_nits: 0\\n"
            "output_gamma: 2.2\\ntarget_primaries: p3_d65\\noutput_range: full\\n" %
            (tm, "dv_tone_mapping: dolby_style\\n" if dolby_style else "", target))


# The separate, later, clearly labelled FULL-PIPELINE check (VP's stages at their defaults).
FULL_NO_TM = ("[vprenderer.Default]\\ndithering: off\\ntone_mapping: spline\\npeak_detection: off\\n"
              "sdr_target_nits: 1000\\nsdr_black_nits: 0\\ncontrast_recovery: 0\\noutput_gamma: 2.2\\n"
              "target_primaries: BT2020\\noutput_range: full\\n")


def jobs(root):
    out = []
    seg = lambda name: os.path.join(root, name)
    for name in sorted(os.listdir(root)):
        if not name.startswith("acc-"):
            continue
        for unpack in ("off", "on"):
            out.append(("acc_%s_%s" % (name[4:], "gpu" if unpack == "on" else "cpu"), seg(name), "frame=0 seconds=3.0 capture=2.2",
                        "dv_tunnel_gpu_unpack: %s\\n" % unpack + NO_TM))
    for unpack in ("off", "on"):
        out.append(("btb_cinema100_%s" % ("gpu" if unpack == "on" else "cpu"), seg("btb-1080p24"), "frame=0 seconds=3.0 capture=2.2",
                    "dv_tunnel_gpu_unpack: %s\\n" % unpack + cinema(100)))
    out.append(("btb_notm", seg("btb-1080p24"), "frame=0 seconds=3.0 capture=2.2", "dv_tunnel_gpu_unpack: on\\n" + NO_TM))
    for target in (100, 48, 300):
        for segname, trims_on, label in (("policy-1080p24-real", False, "L1only"), ("policy-1080p24-100only", True, "nearest100"),
                                         ("policy-1080p24-real", True, "blend"), ("policy-1080p24-neutral", True, "neutraltrims")):
            if target != 300 and label == "neutraltrims":
                continue
            for tm in ("spline", "bt2390"):
                if tm == "bt2390" and label not in ("L1only", "blend"):
                    continue
                for k, scene in enumerate(("bright", "mid", "dim")):
                    out.append(("pol_%d_%s%s_%s" % (target, label, "" if tm == "spline" else "-bt2390", scene), seg(segname),
                                "frame=%d seconds=3.0 capture=2.2" % k,
                                "dv_tunnel_gpu_unpack: on\\ndv_trims: %s\\n" % ("on" if trims_on else "off") + cinema(target, tm)))
    # The Dolby-style DV tone mapping (fix/dv-dolby-style-dark): the policy scenes at 100 nits, trims off and on.
    for trims_on, label in ((False, "L1only"), (True, "blend")):
        for k, scene in enumerate(("bright", "mid", "dim")):
            out.append(("pol_100_%s-dolby_%s" % (label, scene), seg("policy-1080p24-real"), "frame=%d seconds=3.0 capture=2.2" % k,
                        "dv_tunnel_gpu_unpack: on\\ndv_trims: %s\\n" % ("on" if trims_on else "off") +
                        cinema(100, "bt2390", dolby_style=True)))
    # The tunnel's SDR passthrough (plain RGB8 in r210 before any DV lock): it must be shown.
    robust = load_truth(seg("robust-1080p24"))
    sdr_frame = next(f["index"] for f in robust["frames"] if f["expect"] == "sdr")
    out.append(("sdr_passthrough", seg("robust-1080p24"), "frame=%d seconds=3.0 capture=2.2" % sdr_frame,
                "[vprenderer.Default]\\ndithering: off\\ndeband: off\\n"))
    out.append(("lldv_unmapped_notm", seg("lldv-1080p24-unmapped"), "frame=0 seconds=3.0 capture=2.2 pq=1000", NO_TM))
    out.append(("lldv_bright100_cinema100", seg("lldv-1080p24-bright-100"), "frame=0 seconds=3.0 capture=2.2 pq=100", cinema(100)))
    out.append(("lldv_bright1000_cinema100", seg("lldv-1080p24-bright-1000"), "frame=0 seconds=3.0 capture=2.2 pq=1000", cinema(100)))
    out.append(("lldv_bright100_cinema48", seg("lldv-1080p24-bright-100"), "frame=0 seconds=3.0 capture=2.2 pq=100", cinema(48)))
    out.append(("lldv_bright1000_cinema48", seg("lldv-1080p24-bright-1000"), "frame=0 seconds=3.0 capture=2.2 pq=1000", cinema(48)))
    # FULL PIPELINE (separate, labelled): the same accuracy content and a policy scene with VP's
    # stages at their defaults; reported apart from the faithful measurements.
    for unpack in ("off", "on"):
        out.append(("full_acc_1080p24-p5-r210-low8_%s" % ("gpu" if unpack == "on" else "cpu"), seg("acc-1080p24-p5-r210-low8"),
                    "frame=0 seconds=3.0 capture=2.2", "dv_tunnel_gpu_unpack: %s\\n" % unpack + FULL_NO_TM))
    out.append(("full_pol_100_blend_mid", seg("policy-1080p24-real"), "frame=1 seconds=3.0 capture=2.2",
                "dv_tunnel_gpu_unpack: on\\ndv_trims: on\\n" + cinema(100, faithful=False)))
    out.append(("lldv_bright1000_bt2390-100", seg("lldv-1080p24-bright-1000"), "frame=0 seconds=3.0 capture=2.2 pq=1000", cinema(100, "bt2390")))
    return out


def write_jobs(root, path, only=None):
    # only: keep the job ids containing this text (e.g. "-dolby_" for the Dolby-style confirmation).
    with open(path, "w", newline="\n") as f:
        for jid, folder, keys, settings in jobs(root):
            if only and only not in jid:
                continue
            if "\n" in settings or "\r" in settings:   # not assert: it must hold under python -O (audit LOW-3)
                raise ValueError(jid + ": a real new line inside the settings")
            faithful = not jid.startswith("full_") and jid != "sdr_passthrough"
            f.write("job id=%s segment=%s %s%s settings=%s\n" % (jid, folder, keys, " faithful=1" if faithful else "", settings))
    print("wrote", path)


# ------------------------------------------------------------------------------ analysis
def load_truth(folder):
    with open(os.path.join(folder, "truth.json")) as f:
        return json.load(f)


def load_capture(out, jid):
    meta_path = os.path.join(out, jid + ".json")
    raw_path = os.path.join(out, jid + ".r10")
    if not os.path.exists(meta_path) or not os.path.exists(raw_path):
        return None, None
    with open(meta_path) as f:
        meta = json.load(f)
    codes = np.fromfile(raw_path, dtype="<u2").astype(np.float64).reshape(meta["height"], meta["width"], 3)
    return meta, codes


def patch_mean(codes, rect):
    x0, y0, x1, y1 = rect
    return codes[y0:y1, x0:x1].reshape(-1, 3).mean(axis=0)


def output_to_rgb2020(codes, target, primaries):
    lin = dv.decode_output(codes, target)
    if primaries == "p3":
        lin = lin @ dv.rgb_convert_matrix(dv.PRIM_P3D65, dv.PRIM_2020).T
    return lin


def analyze_accuracy(truth, meta, codes, target=1000.0):
    pic = next(iter(truth["pictures"].values()))
    rows, errs, dEs = [], [], []
    for p in pic["patches"]:
        if "reference_rgb2020_nits" not in p:
            continue
        ref = np.array(p["reference_rgb2020_nits"])
        if ref.max() > target or ref.min() < 0:
            continue        # outside the no-tone-mapping window (clipped by design)
        want = dv.encode_output(ref, target)
        got = patch_mean(codes, p["rect"])
        err = got - want
        dE = float(dv.delta_e_itp(dv.decode_output(got, target), ref))
        rows.append({"name": p["name"], "want": want.round(2).tolist(), "got": got.round(2).tolist(),
                     "err_codes": err.round(2).tolist(), "dE_itp": round(dE, 3)})
        errs.append(np.abs(err).max())
        dEs.append(dE)
    # I ramp, per pixel (pixels whose reference stays within the target).
    L = dv.Layout(truth["width"], truth["height"])
    rep = dv.REPS["p5" if truth["representation"].startswith("p5") else "p8"]
    w = truth["width"]
    y0, y1 = L.ramp[0] + 6, L.ramp[1] - 6
    x = np.arange(w)
    I = np.clip(np.rint(x * 4095.0 / (w - 1)), 0, 4095)
    nc = dv.neutral_codes(rep, 100.0)
    Pm, Tm = np.rint(nc[1]), np.rint(nc[2])
    ref = rep.decode(np.stack([I, np.full(w, Pm), np.full(w, Tm)], axis=-1))
    ok = (ref.max(axis=1) <= target) & (ref.min(axis=1) >= 0)
    want = dv.encode_output(ref, target)
    got = codes[y0:y1].mean(axis=0)
    d = (got - want)[ok]
    mse = float((d ** 2).mean())
    psnr = 10 * np.log10(1023.0 ** 2 / mse) if mse > 0 else float("inf")
    return {"patches": rows, "patch_max_err_codes": round(float(max(errs)), 3), "patch_mean_dE_itp": round(float(np.mean(dEs)), 3),
            "patch_max_dE_itp": round(float(max(dEs)), 3), "ramp_pixels": int(ok.sum()),
            "ramp_max_err_codes": round(float(np.abs(d).max()), 3), "ramp_psnr_db": round(float(psnr), 2),
            "transfer": meta.get("actual_pixel_transfer"), "target_transfer": meta.get("target_transfer_effective"),
            "source_transfer": meta.get("source_transfer"), "source_primaries": meta.get("source_primaries")}


def analyze_extremes(truth, codes):
    pic = next(iter(truth["pictures"].values()))
    out = []
    for p in pic["patches"]:
        if p["kind"] == "extreme":
            out.append({"name": p["name"], "I": p["codes"][0], "got": patch_mean(codes, p["rect"]).round(2).tolist()})
    g = [o["got"][1] for o in out]
    below = [o for o in out if o["I"] <= 62]
    above = [o for o in out if o["I"] >= 3500]
    distinct_below = len(set(round(o["got"][1]) for o in below))
    distinct_above = len(set(round(o["got"][1]) for o in above))
    return {"patches": out, "monotonic": all(b >= a - 0.5 for a, b in zip(g, g[1:])),
            "distinct_levels_at_or_below_source_min": "%d of %d" % (distinct_below, len(below)),
            "distinct_levels_at_or_above_3500": "%d of %d" % (distinct_above, len(above))}


def analyze_policy(truth, codes, target, scene_index, mode, trims_list):
    pic_id = "scene_%s" % ("bright", "mid", "dim")[scene_index]
    pic = truth["pictures"][pic_id]
    scene = truth["scenes"][scene_index]
    l1 = scene["l1"]
    dEs, lum = [], []
    for p in pic["patches"]:
        if "reference_rgb2020_nits" not in p or p["kind"] not in ("wedge", "colour") or p["name"].startswith("2020"):
            continue
        ref = np.array(p["reference_rgb2020_nits"])
        intent = dm_model.reference_dm(ref, l1=l1, target_nits=target, l2=trims_list, trim_mode=mode,
                                       apply_trims=mode is not None)
        got = output_to_rgb2020(patch_mean(codes, p["rect"]), target, "p3")
        dEs.append(float(dv.delta_e_itp(np.clip(got, 0, None), np.clip(intent, 0, None))))
        if p["kind"] == "wedge":
            yi = float(intent @ np.array([0.2627, 0.6780, 0.0593]))
            yg = float(got @ np.array([0.2627, 0.6780, 0.0593]))
            if yi > 0.05:
                lum.append(yg / yi)
    return {"mean_dE_itp": round(float(np.mean(dEs)), 2), "max_dE_itp": round(float(np.max(dEs)), 2),
            "median_luminance_ratio": round(float(np.median(lum)), 3) if lum else None}


def trims_of(truth):
    t = truth["frames"][0]["dm"]
    return t["l2"] if t else []


def analyze(root, jobs_path, out, report_path, only=None):
    report = {"accuracy": {}, "extremes": {}, "policy": {}, "lldv": {}, "missing": []}
    # only: the same id filter the jobs file was written with (audit D-1, 2026-10-25): a filtered
    # run must not list every other job as a missing capture.
    jl = [j for j in jobs(root) if not only or only in j[0]]
    for jid, folder, keys, settings in jl:
        meta, codes = load_capture(out, jid)
        if meta is None:
            report["missing"].append(jid)
            continue
        truth = load_truth(folder)
        if jid.startswith("full_acc_"):
            report.setdefault("full_pipeline", {})[jid] = analyze_accuracy(truth, meta, codes)
        elif jid.startswith("full_pol_"):
            real = load_truth(os.path.join(root, "policy-1080p24-real"))
            report.setdefault("full_pipeline", {})[jid] = {
                "vs_intent_nearest_trim": analyze_policy(truth, codes, 100.0, 1, "nearest", trims_of(real))}
        elif jid.startswith("acc_"):
            report["accuracy"][jid] = analyze_accuracy(truth, meta, codes)
        elif jid.startswith("btb_"):
            report["extremes"][jid] = analyze_extremes(truth, codes)
        elif jid.startswith("pol_"):
            _, target, label, scene = jid.split("_")
            target = float(target)
            k = ("bright", "mid", "dim").index(scene)
            real = load_truth(os.path.join(root, "policy-1080p24-real"))
            # The intent: the modelled DM with the colourist's trims (the 'real' set), nearest trim.
            res = analyze_policy(truth, codes, target, k, "nearest", trims_of(real))
            res_blend = analyze_policy(truth, codes, target, k, "blend", trims_of(real))
            res_l1 = analyze_policy(truth, codes, target, k, None, [])
            report["policy"][jid] = {"vs_intent_nearest_trim": res, "vs_intent_blend": res_blend, "vs_untrimmed_model": res_l1}
        elif jid == "sdr_passthrough":
            # The picture: R = B = 61, G alternating 181 / 61 by column (RGB8, full range).
            g_even, g_odd = codes[100:900, 100:1800:2, 1].mean(), codes[100:900, 101:1800:2, 1].mean()
            r = codes[100:900, 100:1800, 0].mean()
            report["sdr_passthrough"] = {"mean_R": round(float(r), 1), "mean_G_even": round(float(g_even), 1),
                                         "mean_G_odd": round(float(g_odd), 1), "shown": bool(g_even > r + 50)}
        elif jid.startswith("lldv_"):
            if "notm" in jid:
                report["lldv"][jid] = analyze_accuracy(truth, meta, codes)
            else:
                target = 100.0 if ("cinema100" in jid or jid.endswith("-100")) else 48.0
                real = load_truth(os.path.join(root, "policy-1080p24-real"))
                tr = {"scenes": real["scenes"], "pictures": {"scene_bright": truth["pictures"]["lldv"]}}
                report["lldv"][jid] = {"vs_intent_nearest_trim": analyze_policy(tr, codes, target, 0, "nearest", trims_of(real)),
                                       "vs_untrimmed_model": analyze_policy(tr, codes, target, 0, None, [])}
    with open(report_path, "w", newline="\n") as f:
        json.dump(report, f, indent=1)
    summarize(report)


def summarize(r):
    print("== DECODE ACCURACY (no tone mapping, 1000-nit 2.2 BT.2020 output; vs the independent reference decode)")
    for jid, a in sorted(r["accuracy"].items()):
        print("  %-42s patches: max %.2f codes, mean dE_ITP %.3f (max %.3f); I ramp: max %.2f codes, PSNR %.1f dB [%s]" %
              (jid, a["patch_max_err_codes"], a["patch_mean_dE_itp"], a["patch_max_dE_itp"], a["ramp_max_err_codes"],
               a["ramp_psnr_db"], a["transfer"]))
    print("== BTB / WTW-like extremes")
    for jid, e in sorted(r["extremes"].items()):
        print("  %-24s monotonic %s; at/below source min %s distinct; >= I 3500 %s distinct; G codes %s" %
              (jid, e["monotonic"], e["distinct_levels_at_or_below_source_min"], e["distinct_levels_at_or_above_3500"],
               [round(p["got"][1]) for p in e["patches"]]))
    print("== POLICY (mean / max dE_ITP vs the modelled intent with the 100-nit trim; luminance ratio)")
    for jid, p in sorted(r["policy"].items()):
        a, b = p["vs_intent_nearest_trim"], p["vs_untrimmed_model"]
        print("  %-32s intent(trimmed) %5.2f / %5.2f  lum x%s | untrimmed model %5.2f" %
              (jid, a["mean_dE_itp"], a["max_dE_itp"], a["median_luminance_ratio"], b["mean_dE_itp"]))
    print("== LLDV-like")
    for jid, p in sorted(r["lldv"].items()):
        if "patch_max_err_codes" in p:
            print("  %-32s patches max %.2f codes, mean dE %.3f; ramp max %.2f codes PSNR %.1f dB" %
                  (jid, p["patch_max_err_codes"], p["patch_mean_dE_itp"], p["ramp_max_err_codes"], p["ramp_psnr_db"]))
        else:
            print("  %-32s intent(trimmed) %5.2f / %5.2f | untrimmed model %5.2f" %
                  (jid, p["vs_intent_nearest_trim"]["mean_dE_itp"], p["vs_intent_nearest_trim"]["max_dE_itp"],
                   p["vs_untrimmed_model"]["mean_dE_itp"]))
    if r.get("full_pipeline"):
        print("== FULL PIPELINE (separate check: VP's stages at their defaults)")
        for jid, p in sorted(r["full_pipeline"].items()):
            if "patch_max_err_codes" in p:
                print("  %-36s patches max %.2f codes, mean dE %.3f; ramp max %.2f codes PSNR %.1f dB" %
                      (jid, p["patch_max_err_codes"], p["patch_mean_dE_itp"], p["ramp_max_err_codes"], p["ramp_psnr_db"]))
            else:
                print("  %-36s intent(trimmed) %5.2f / %5.2f" % (jid, p["vs_intent_nearest_trim"]["mean_dE_itp"],
                                                            p["vs_intent_nearest_trim"]["max_dE_itp"]))
    if "sdr_passthrough" in r:
        print("== SDR passthrough on the tunnel input:", r["sdr_passthrough"])
    if r["missing"]:
        print("MISSING captures:", r["missing"])


if __name__ == "__main__":
    if len(sys.argv) >= 4 and sys.argv[1] == "jobs":
        write_jobs(sys.argv[2], sys.argv[3], sys.argv[4] if len(sys.argv) >= 5 else None)
    elif len(sys.argv) >= 6 and sys.argv[1] == "analyze":
        analyze(sys.argv[2], sys.argv[3], sys.argv[4], sys.argv[5], sys.argv[6] if len(sys.argv) >= 7 else None)
    else:
        print(__doc__)
        sys.exit(2)
