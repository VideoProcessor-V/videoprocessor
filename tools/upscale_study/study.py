#!/usr/bin/env python3
"""VP upscale quality study: job generator, runner and dry run (coordinator 2026-10-24).

Why: B (the x2 net) is dropped for now, so VP's classical upscaling must be excellent on its own.
This study compares, on realistic 1080p / 720p deliveries of the 4K masters, against the 4K
master's ideal downscale to the FL35 (fitted in 2560x1600):
  (a) the ROUTE: always-4K (upscale to 3840x2160, then VP's hybrid downscale; with / without D)
      vs a direct upscale to the output;
  (b) the libplacebo upscalers: separable (lanczos, spline36, catmull_rom) vs polar EWA
      (ewa_lanczos, ewa_lanczossharp, ewa_lanczos4sharpest), and anti-ringing strengths;
  (c) metrics: PSNR / SSIM on luma, the kit's ringing (near-edge error), halo overshoot, the
      kit's grain-keep ratio, the high-band error, and cost (pl_timer).
Design and run-time estimates: docs/design/upscale-study.md.

Stages (each resumable; --dry-run validates and prints without running anything):
  plan     build the matrix, check every input / tool / renderer key, write plan.json + job files
  prepare  (CPU, ffmpeg) master segments -> the encoded deliveries -> decoded / player-upscaled
           luma raws (gray10le) + the reference frames
  render   (GPU) VpScaleRender jobs (VP's real chain) and PlScaleSweep runs (the kernel sweep);
           waits while another agent's test / VideoProcessor process runs (never kills anything)
  score    (kit Python, torch) the metrics per output frame -> scores.csv (the kit's functions)
  report   scores.csv -> report.md

Read-only on D:\\nn-corpus and the kit; writes only under --work.
  python tools/upscale_study/study.py plan --work D:\\codex\\upscale-study --dry-run
"""
import argparse
import hashlib
import json
import os
import re
import shutil
import subprocess
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(os.path.dirname(HERE))

# ------------------------------------------------------------------------------------------ matrix
MASTERS = [  # 4K masters (read-only corpus)
    dict(id="boat", path=r"D:\nn-corpus\trimmed\boat4k\reference_ffv1.mkv", size=(4096, 2160)),
    dict(id="chimera", path=r"D:\nn-corpus\trimmed\chimera4k\reference_ffv1.mkv", size=(4096, 2160)),
    dict(id="foodmarket", path=r"D:\nn-corpus\trimmed\foodmarket4k\reference_ffv1.mkv", size=(4096, 2160)),
    dict(id="meridian", path=r"D:\nn-corpus\trimmed\meridian4k_existing\reference_ffv1.mkv", size=(3840, 2160)),
    dict(id="sintel", path=r"D:\nn-corpus\trimmed\sintel4k\reference_ffv1.mkv", size=(3840, 1744)),
    dict(id="sollevante", path=r"D:\nn-corpus\trimmed\sollevante4k\reference_ffv1.mkv", size=(3840, 2160)),
    dict(id="tos", path=r"D:\nn-corpus\trimmed\tos4k\reference_ffv1.mkv", size=(3840, 1714)),
]
SEGMENT_STARTS = [1.0, 3.0]        # seconds into each master (boat is 5 s long)
SEGMENT_FRAMES = 48                # encoded as one GOP-bearing segment, like a stream
EVAL_FRAMES = [12, 35]             # frames of each segment that are scored (a P and a B frame typically)
OUTPUT_BOX = (2560, 1600)          # the FL35
ROUTE_4K = (3840, 2160)

# Deliveries: encoded size, codec + bitrate (streaming-like), the player's upscale (None = native).
CASES = [
    dict(id="n1080", enc=(1920, 1080), codec="libx265", kbps=6000, player=None),
    dict(id="p1080_2160", enc=(1920, 1080), codec="libx265", kbps=6000, player=dict(size=(3840, 2160), kernel="catmull_rom")),
    dict(id="n720", enc=(1280, 720), codec="libx264", kbps=3000, player=None),
    dict(id="p720_1080", enc=(1280, 720), codec="libx264", kbps=3000, player=dict(size=(1920, 1080), kernel="lanczos")),
    dict(id="p720_2160", enc=(1280, 720), codec="libx264", kbps=3000, player=dict(size=(3840, 2160), kernel="catmull_rom")),
    # (d) the downscale A/B: the 4K master itself to the FL35 (no encode): AdaptiveDownscale's
    # faithful branch polar EWA vs separable, and fbo_float32 on/off. Plus a synthetic WTW ramp.
    dict(id="native2160", enc=(3840, 2160), codec=None, kbps=0, player=None, kind="down"),
]
# A synthetic still for the highlight / WTW check (codes 64 -> 1019 ramp with 1-code noise).
WTW_RAMP = dict(id="wtwramp", path=None, size=(3840, 2160), synthetic=True)
PLAYER_FLAGS = {"catmull_rom": "bicubic:param0=0:param1=0.5", "lanczos": "lanczos:param0=3", "bilinear": "bilinear"}

# VP renderer settings (keys and values VP's renderer accepts: LibplaceboVideoRenderer.cpp
# readChoice lists, checked by `plan`). Common = the kit's locked scaling set (scaling_render.json).
VP_COMMON = {
    "quality": "high", "dithering": "off", "deband": "off", "deband_vp": "off", "temporal_dither": "off",
    "adaptive_grain_map": "on", "adaptive_chroma_ssim": "off", "interpolation": "off", "interpolation_fill_gaps": "off",
    "dejudder_strength": "0", "fbo_float32": "on", "deband_strength": "off", "deband_grain": "off",
}
VP_DOWN = {"downscaler": "lanczos", "downscale_light": "linear", "adaptive_downscale": "on", "adaptive_w_base": "0.2",
           "adaptive_faithful": "ortho"}
VP_UP_HYBRID = {"upscaler": "auto", "upscale_light": "linear", "sigmoid": "auto", "adaptive_upscale": "on",
                "adaptive_upscale_w_base": "1.0", "adaptive_upscale_superres_scale": "on"}
SHADER_SECTIONS = (
    "[shader.standard]\ntype: multi\nlabel: Off\n"
    "[shader.standard.ssim_downscaler]\nshader_type: custom\nlabel: SSimDownscaler\nstage: post_resize\norder: 20\nglsl_file: SSimDownscaler.glsl\n"
    "[shader.standard.fsrcnnx]\nshader_type: custom\nlabel: FSRCNNX upscaler\nstage: pre_resize\norder: 15\nglsl_file: FSRCNNX_x2_16-0-4-1.glsl\n"
    "[shader.standard.ssim_superres]\nshader_type: custom\nlabel: SSimSuperRes (upscale refinement)\nstage: post_resize\norder: 25\nglsl_file: SSimSuperRes.glsl\n")
VP_UPSCALERS = ["lanczos", "catmull_rom", "ewa_lanczos", "ewa_lanczossharp"]
# AdaptiveDownscale's faithful branch: "auto" = polar ewa_lanczossharp when both axes scale alike
# (4K -> 2560x1440 does), "ortho" = two separable Lanczos passes (AdaptiveDownscale.cpp:631-636).
VP_FAITHFUL = {"polar": "auto", "separable": "ortho"}
VALID = {  # the renderer's accepted values (LibplaceboVideoRenderer.cpp readChoice)
    "upscaler": {"auto", "none", "nearest", "bilinear", "oversample", "bicubic", "gaussian", "catmull_rom", "lanczos", "ewa_lanczos",
                 "ewa_lanczossharp", "ewa_lanczos4sharpest"},
    "downscaler": {"auto", "gpu", "box", "hermite", "bilinear", "bicubic", "gaussian", "catmull_rom", "mitchell", "lanczos"},
    "upscale_light": {"linear", "gamma"}, "downscale_light": {"linear", "gamma"},
    "adaptive_faithful": {"auto", "ortho"}, "fbo_float32": {"auto", "on", "off"},
}

# The kernel sweep (PlScaleSweep, libplacebo directly): what VP's renderer does not expose
# (spline36, anti-ringing strength) next to what it does.
SWEEP_FILTERS = ["lanczos", "spline36", "catmull_rom", "ewa_lanczos", "ewa_lanczossharp", "ewa_lanczos4sharpest"]
SWEEP_ANTIRING = [0.0, 0.25, 0.5, 0.75, 1.0]


def fit(w, h, bw, bh):
    """The kit's fit_size: (w, h) fitted inside (bw, bh), aspect kept, even sizes."""
    s = min(bw / w, bh / h)
    ow, oh = int(round(w * s / 2)) * 2, int(round(h * s / 2)) * 2
    return min(ow, bw), min(oh, bh)


def variants(case=None):
    """VP renderer variants for a case. Each: id, stages [(window, settings, shader profiles)], d (bool).
    The downscale case gets (d); the upscale cases get (a) plus the fbo_float32 off check."""
    if case is not None and case.get("kind") == "down":
        v = []
        for name, faithful in VP_FAITHFUL.items():
            for fbo in ("on", "off"):
                v.append(dict(id=f"down_{name}_fbo32{fbo}", route="down",
                              stages=[dict(window=OUTPUT_BOX, settings=dict(VP_DOWN, adaptive_faithful=faithful, fbo_float32=fbo),
                                           shaders=["standard.ssim_downscaler"])]))
        return v
    v = [dict(id="direct_vp_hybrid", route="direct", stages=[dict(window=OUTPUT_BOX, settings=VP_UP_HYBRID, shaders=["standard.fsrcnnx", "standard.ssim_superres"])]),
         dict(id="direct_vp_hybrid_fbo32off", route="direct",
              stages=[dict(window=OUTPUT_BOX, settings=dict(VP_UP_HYBRID, fbo_float32="off"), shaders=["standard.fsrcnnx", "standard.ssim_superres"])])]
    for u in VP_UPSCALERS:
        v.append(dict(id=f"direct_{u}", route="direct",
                      stages=[dict(window=OUTPUT_BOX, settings=dict(VP_UP_HYBRID, upscaler=u, adaptive_upscale="off"), shaders=[])]))
    for u in ("lanczos", "catmull_rom", "ewa_lanczossharp"):
        s1 = dict(window=ROUTE_4K, settings=dict(VP_UP_HYBRID, upscaler=u, adaptive_upscale="off"), shaders=[])
        s2 = dict(window=OUTPUT_BOX, settings=VP_DOWN, shaders=["standard.ssim_downscaler"], source="stage1")
        v.append(dict(id=f"route4k_{u}", route="route4k", stages=[s1, s2]))
        v.append(dict(id=f"route4k_{u}+D", route="route4k", stages=[s1, s2], d=True, reuse=f"route4k_{u}"))
    return v


def cfg_text(settings, rebase_lines):
    kv = dict(VP_COMMON)
    kv.update(settings)
    if rebase_lines:
        kv.update(source_resolution=str(rebase_lines), source_resolution_latch_fixes="on", rebase_kernel_match="on")
    else:
        kv["source_resolution"] = "native"
    body = [f"{k}: {v}" for k, v in kv.items()]
    return "\n".join(["[general]", "persist_profile_selection: false", "[vprenderer]"] + body + ["[vprenderer.Default]"] + body) + "\n" + SHADER_SECTIONS


# ------------------------------------------------------------------------------------------ plan
class Plan:
    def __init__(self, a):
        self.a = a
        self.work = os.path.abspath(a.work)
        self.problems = []
        self.jobs = []          # VpScaleRender jobs
        self.sweeps = []        # PlScaleSweep commands
        self.frames = []        # (master, segment, frame) scored

    def check(self, cond, what):
        if not cond:
            self.problems.append(what)
        return cond

    def build(self):
        a = self.a
        for m in MASTERS:
            self.check(os.path.exists(m["path"]), f"master missing: {m['path']}")
        for t, p in (("ffmpeg", a.ffmpeg), ("VpScaleRender", a.render_tool), ("renderer DLL", a.dll), ("PlScaleSweep", a.sweep_tool)):
            self.check(bool(p) and os.path.exists(p), f"{t} not found: {p}")
        self.check(os.path.isdir(a.kit) and os.path.exists(os.path.join(a.kit, "trainkit", "common.py")), f"kit not found: {a.kit}")
        if a.d_staging:
            self.check(os.path.exists(os.path.join(a.d_staging, "weights", "D_hybrid_refine.pt")), f"D weights not found under {a.d_staging}")
        for v in variants() + variants(CASES[-1]):
            for st in v["stages"]:
                for k, val in st["settings"].items():
                    if k in VALID:
                        self.check(val in VALID[k], f"{v['id']}: {k}={val} is not a value VP's renderer accepts")
        for m in MASTERS:
            for si, start in enumerate(SEGMENT_STARTS):
                for f in EVAL_FRAMES:
                    self.frames.append((m["id"], si, f))
        self.frames.append((WTW_RAMP["id"], 0, 0))   # scored for the downscale case only (see job_text)
        for case in CASES:
            src = case["player"]["size"] if case["player"] else case["enc"]
            rebase = case["enc"][1] if case["player"] else None
            for v in variants(case):
                if v.get("reuse"):
                    continue   # +D reuses the route's renders (D runs at scoring)
                for si_, st in enumerate(v["stages"]):
                    jid = f"{case['id']}__{v['id']}__s{si_ + 1}"
                    d = os.path.join(self.work, "vp", jid)
                    feed = "stage1" if st.get("source") == "stage1" else "delivered"
                    s_size = ROUTE_4K if feed == "stage1" else src
                    self.jobs.append(dict(id=jid, dir=d, case=case["id"], variant=v["id"], stage=si_ + 1, feed=feed, source=list(s_size),
                                          window=list(st["window"]), shaders=st["shaders"], cfg=cfg_text(st["settings"], rebase if feed == "delivered" else None)))
            if case.get("kind") == "down":
                continue   # the kernel sweep is about upscaling
            for filt in SWEEP_FILTERS:
                for ar in SWEEP_ANTIRING:
                    for target in ("output", "route4k"):
                        self.sweeps.append(dict(case=case["id"], filter=filt, antiring=ar, target=target))
        return self

    def estimate(self):
        a = self.a
        pictures = len(self.frames)
        vp_s = sum(15 + (pictures + 1) * (a.hold + 0.3) for _ in self.jobs)
        sweep_renders = len(self.sweeps) * pictures
        return dict(scored_frames_per_case=pictures, vp_jobs=len(self.jobs), vp_wall_h=round(vp_s / 3600, 2),
                    sweep_runs=len(self.sweeps), sweep_renders=sweep_renders,
                    # per batch: ~0.6 s device + compile; per upscale: upload + 9 timed renders + readback
                    sweep_min_laptop=round((len(self.sweeps) * 0.6 + sweep_renders * 0.06) / 60, 1),
                    sweep_min_3090=round((len(self.sweeps) * 0.4 + sweep_renders * 0.03) / 60, 1),
                    score_items=(sum(len(variants(c)) for c in CASES) + len(self.sweeps)) * pictures)

    def write(self):
        os.makedirs(self.work, exist_ok=True)
        for j in self.jobs:
            os.makedirs(j["dir"], exist_ok=True)
            with open(os.path.join(j["dir"], "render.cfg"), "w", newline="\n") as f:
                f.write(j["cfg"])
            with open(os.path.join(j["dir"], "job.txt"), "w", newline="\n") as f:
                f.write(job_text(self.a, j, self.frames, self.work))
        with open(os.path.join(self.work, "plan.json"), "w", newline="\n") as f:
            json.dump(dict(jobs=[{k: v for k, v in j.items() if k != "cfg"} for j in self.jobs], sweeps=self.sweeps,
                           frames=self.frames, cases=CASES, masters=MASTERS + [WTW_RAMP], estimate=self.estimate()), f, indent=1)


def raw_path(work, case_id, master, seg, frame, kind):
    """kind: delivered (what the player hands VP), encoded (decoded at the encoded size), ref (the target)."""
    return os.path.join(work, "src", case_id if kind != "ref" else "_ref", f"{master}_s{seg}_f{frame:03d}_{kind}.y16")


def job_text(a, j, frames, work):
    lines = [f"dll {a.dll}", f"config {os.path.join(j['dir'], 'render.cfg')}", f"shots {os.path.join(j['dir'], 'shots')}",
             f"result {os.path.join(j['dir'], 'result.txt')}", f"log {os.path.join(j['dir'], 'vp.log')}",
             f"source {j['source'][0]} {j['source'][1]} 24000 1001", f"window {j['window'][0]} {j['window'][1]}", f"hold {a.hold}"]
    if j["shaders"]:
        lines.append("shaders " + " ".join(j["shaders"]))
    lines.append(f"require_adapter {a.adapter}")
    first = True
    down = j["case"] == "native2160"
    for (m, s, f) in frames:
        if m == WTW_RAMP["id"] and not down:
            continue   # the WTW ramp is a downscale-case still
        if j["feed"] == "stage1":
            p = os.path.join(work, "vp_stage1", j["case"], j["variant"], f"{m}_s{s}_f{f:03d}.y16")
        else:
            p = raw_path(work, j["case"], m, s, f, "delivered")
        if first:   # warm-up: VP's kernel identification and latch get their evidence first
            lines.append(f"picture warmup {p} 1")
            first = False
        lines.append(f"picture {m}_s{s}_f{f:03d} {p} 1")
    return "\n".join(lines) + "\n"


# ------------------------------------------------------------------------------------------ prepare
def prepare(a, plan):
    ff = a.ffmpeg
    for m in MASTERS:
        for si, start in enumerate(SEGMENT_STARTS):
            seg = os.path.join(plan.work, "seg", f"{m['id']}_s{si}.mkv")
            run_or_print(a, [ff, "-y", "-v", "error", "-ss", str(start), "-i", m["path"], "-frames:v", str(SEGMENT_FRAMES), "-c:v", "ffv1",
                             "-pix_fmt", "yuv420p10le", seg], seg)
            for f in EVAL_FRAMES:   # the 4K master luma (the reference is made from it at scoring)
                out = raw_path(plan.work, None, m["id"], si, f, "ref").replace(".y16", "_master.y16")
                run_or_print(a, [ff, "-y", "-v", "error", "-i", seg, "-vf", f"select=eq(n\\,{f})", "-frames:v", "1", "-pix_fmt", "gray16le",
                                 "-f", "rawvideo", out], out)
            for case in CASES:
                if case.get("kind") == "down":
                    for f in EVAL_FRAMES:   # the master itself, fitted into 3840x2160 (bars padded), 10-bit
                        out_d = raw_path(plan.work, case["id"], m["id"], si, f, "delivered")
                        run_or_print(a, [ff, "-y", "-v", "error", "-i", seg, "-vf",
                                         f"select=eq(n\\,{f}),scale=3840:2160:force_original_aspect_ratio=decrease:flags=lanczos,pad=3840:2160:(ow-iw)/2:(oh-ih)/2:color=black",
                                         "-frames:v", "1", "-pix_fmt", "gray10le", "-f", "rawvideo", out_d], out_d)
                    continue
                ew, eh = fit(m["size"][0], m["size"][1], *case["enc"])
                enc = os.path.join(plan.work, "enc", case["id"], f"{m['id']}_s{si}.mp4")
                run_or_print(a, [ff, "-y", "-v", "error", "-i", seg, "-vf", f"scale={ew}:{eh}:flags=lanczos,format=yuv420p" + ("10le" if "265" in case["codec"] else ""),
                                 "-c:v", case["codec"], "-b:v", f"{case['kbps']}k", "-maxrate", f"{int(case['kbps'] * 1.5)}k",
                                 "-bufsize", f"{case['kbps'] * 2}k", "-g", "48", "-bf", "3", enc], enc)
                for f in EVAL_FRAMES:
                    sel = f"select=eq(n\\,{f})"
                    out_e = raw_path(plan.work, case["id"], m["id"], si, f, "encoded")
                    run_or_print(a, [ff, "-y", "-v", "error", "-i", enc, "-vf", sel, "-frames:v", "1", "-pix_fmt", "gray10le", "-f", "rawvideo", out_e], out_e)
                    out_d = raw_path(plan.work, case["id"], m["id"], si, f, "delivered")
                    if case["player"]:
                        pw, ph = case["player"]["size"]
                        # the player fits the picture into its output frame and pads (bars) like a TV app
                        vf = (f"{sel},scale={pw}:{ph}:force_original_aspect_ratio=decrease:flags={PLAYER_FLAGS[case['player']['kernel']]},"
                              f"pad={pw}:{ph}:(ow-iw)/2:(oh-ih)/2:color=black")
                    else:
                        cw, ch = case["enc"]
                        vf = f"{sel},pad={cw}:{ch}:(ow-iw)/2:(oh-ih)/2:color=black"
                    run_or_print(a, [ff, "-y", "-v", "error", "-i", enc, "-vf", vf, "-frames:v", "1", "-pix_fmt", "gray10le", "-f", "rawvideo", out_d], out_d)


def prepare_ramp(a, plan):
    """The synthetic WTW still: a horizontal ramp from code 64 to 1019 (above white 940 on the right),
    1-code noise, plus a few 1-px white-to-WTW lines (where clipping or halos show)."""
    w, h = WTW_RAMP["size"]
    d = raw_path(plan.work, "native2160", WTW_RAMP["id"], 0, 0, "delivered")
    r = raw_path(plan.work, None, WTW_RAMP["id"], 0, 0, "ref").replace(".y16", "_master.y16")
    if a.dry_run:
        print(f"    (synthetic) {d} and {r}")
        return
    import numpy as np
    rng = np.random.default_rng(940)
    x = np.linspace(64.0, 1019.0, w)[None, :].repeat(h, 0) + rng.normal(0.0, 1.0, (h, w))
    x[::97, :] = 1019.0   # thin lines at the WTW ceiling
    codes = np.clip(np.round(x), 4, 1019).astype("<u2")
    os.makedirs(os.path.dirname(d), exist_ok=True)
    os.makedirs(os.path.dirname(r), exist_ok=True)
    codes.tofile(d)
    (codes.astype(np.float64) * 65535.0 / 1023.0).round().astype("<u2").tofile(r)


def run_or_print(a, cmd, out):
    if cmd and os.path.basename(cmd[0]).lower().startswith("ffmpeg") and a.threads > 0:
        cmd = cmd[:1] + ["-threads", str(a.threads)] + cmd[1:]
        if "-c:v" in cmd:   # the encoder's own thread pools
            i = cmd.index("-c:v") + 2
            extra = ["-x265-params", f"pools={a.threads}:frame-threads=1"] if "libx265" in cmd else ["-threads", str(a.threads)]
            cmd = cmd[:i] + extra + cmd[i:]
    if a.dry_run:
        print("   ", " ".join(f'"{c}"' if " " in c else c for c in cmd))
        return
    if os.path.exists(out) and os.path.getsize(out) > 0:
        return
    os.makedirs(os.path.dirname(out), exist_ok=True)
    subprocess.run(cmd, check=True)


# ------------------------------------------------------------------------------------------ render
def busy_processes():
    """Other agents' test runs or a running VideoProcessor (the laptop's GPU courtesy rule)."""
    try:
        out = subprocess.run(["tasklist", "/FO", "CSV"], capture_output=True, text=True).stdout
    except OSError:
        return []
    names = re.findall(r'^"([^"]+)"', out, re.M)
    return sorted({n for n in names if n.lower().startswith(("vstest", "testhost", "videoprocessor.exe", "videoprocessor-test"))})


def wait_quiet(a):
    while not a.dry_run:
        b = busy_processes()
        if not b:
            return
        print("waiting: busy:", ", ".join(b))
        time.sleep(60)


def render(a, plan):
    env = dict(os.environ, VP_TEST_SHADER_DIR=os.path.dirname(a.dll), VP_TEST_ADAPTER=a.adapter)
    for stage in (1, 2):
        for j in plan.jobs:
            if j["stage"] != stage:
                continue
            res = os.path.join(j["dir"], "result.txt")
            if os.path.exists(res) and not a.dry_run:
                continue
            if a.dry_run:
                print(f"    {a.render_tool} {os.path.join(j['dir'], 'job.txt')}")
                continue
            wait_quiet(a)
            r = subprocess.run([a.render_tool, os.path.join(j["dir"], "job.txt")], env=env, capture_output=True, text=True, timeout=7200)
            print(f"{j['id']}: rc {r.returncode}")
            if stage == 1 and r.returncode == 0:
                export_stage1(a, plan, j)
    # The kernel sweep: one PlScaleSweep process per configuration (--batch: one device, one
    # shader compile, every frame of every master), so process start-up does not dominate.
    for s in plan.sweeps:
        folder = os.path.join(plan.work, "sweep", s["case"], f"{s['filter']}_ar{s['antiring']:.2f}_{s['target']}")
        lines = []
        for (m, si, f) in plan.frames:
            if m == WTW_RAMP["id"]:
                continue
            inp = raw_path(plan.work, s["case"], m, si, f, "encoded")
            case = next(c for c in CASES if c["id"] == s["case"])
            mm = next(x for x in MASTERS if x["id"] == m)
            ew, eh = fit(mm["size"][0], mm["size"][1], *case["enc"])
            ow, oh = fit(ew, eh, *(OUTPUT_BOX if s["target"] == "output" else ROUTE_4K))
            out = os.path.join(folder, f"{m}_s{si}_f{f:03d}.y16")
            if os.path.exists(out) and not a.dry_run:
                continue
            lines.append(f"--in {inp} {ew} {eh} --in-format gray10 --size {ow} {oh} --filter {s['filter']} --antiring {s['antiring']} --repeat 8 --out {out}")
        if a.dry_run or not lines:
            continue
        os.makedirs(folder, exist_ok=True)
        batch = os.path.join(folder, "batch.txt")
        with open(batch, "w", newline="\n") as f:
            f.write("\n".join(lines) + "\n")
        wait_quiet(a)
        subprocess.run([a.sweep_tool, "--batch", batch, "--adapter", a.adapter], check=True)
    if a.dry_run:
        print(f"    + {len(plan.sweeps)} PlScaleSweep batch runs ({len(plan.sweeps) * len(plan.frames)} upscales), e.g.:")
        print("     ", a.sweep_tool, "--batch <work>/sweep/<case>/<filter>_ar<A>_<target>/batch.txt --adapter", a.adapter)


def cost(a, plan):
    """GPU cost per output frame of each downscale variant (and the upscale fbo_float32 pair), the
    kit's vpbench method: utilisation sampled every 200 ms over a held still (nocapture), the first
    4 s dropped; ms per output frame = utilisation x refresh period."""
    import threading
    results = {}
    for j in plan.jobs:
        if not (j["case"] == "native2160" or j["variant"].startswith("direct_vp_hybrid")):
            continue
        d = os.path.join(plan.work, "cost", j["id"])
        frame = raw_path(plan.work, j["case"], MASTERS[0]["id"], 0, EVAL_FRAMES[0], "delivered")
        text = job_text(a, dict(j, dir=d), [], plan.work).replace(f"hold {a.hold}", "hold 12") + f"nocapture 1\npicture cost {frame} 1\n"
        if a.dry_run:
            print(f"    cost {j['id']}: {a.render_tool} {os.path.join(d, 'job.txt')} (12 s held still, nvidia-smi)")
            continue
        os.makedirs(d, exist_ok=True)
        with open(os.path.join(d, "render.cfg"), "w", newline="\n") as f:
            f.write(j["cfg"])
        with open(os.path.join(d, "job.txt"), "w", newline="\n") as f:
            f.write(text)
        wait_quiet(a)
        samples, stop = [], threading.Event()

        def sample():
            p = subprocess.Popen(["nvidia-smi", "--query-gpu=utilization.gpu", "--format=csv,noheader,nounits", "-lms", "200"],
                                 stdout=subprocess.PIPE, text=True)
            t0 = time.time()
            for line in p.stdout:
                if stop.is_set():
                    break
                try:
                    samples.append((time.time() - t0, float(line.strip())))
                except ValueError:
                    pass
            p.kill()
        th = threading.Thread(target=sample, daemon=True)
        th.start()
        t0 = time.time()
        subprocess.run([a.render_tool, os.path.join(d, "job.txt")], capture_output=True, text=True, timeout=300,
                       env=dict(os.environ, VP_TEST_SHADER_DIR=os.path.dirname(a.dll), VP_TEST_ADAPTER=a.adapter))
        dur = time.time() - t0
        stop.set()
        th.join(5)
        steady = [u for t, u in samples if 4.0 <= t <= dur - 1.0]
        util = sum(steady) / len(steady) if steady else None
        results[j["variant"] + "@" + j["case"]] = dict(util_pct=util, ms_per_output_frame_60hz=None if util is None else round(util / 100 * 1000 / 60, 3))
        print(j["id"], results[j["variant"] + "@" + j["case"]])
    if not a.dry_run:
        with open(os.path.join(plan.work, "cost.json"), "w", newline="\n") as f:
            json.dump(results, f, indent=1)


def export_stage1(a, plan, j):
    """Stage-1 captures (VP at 3840x2160, R10 PNG) -> 10-bit luma raws for stage 2.
    The R10 code is mapped back to the source code domain with the kit's fixed limited->full inverse."""
    import numpy as np
    import cv2
    rows = [l.split() for l in open(os.path.join(j["dir"], "result.txt")) if len(l.split()) >= 7 and l.split()[2] != "-"]
    for r in rows:
        tag = r[0]
        if tag == "warmup":
            continue
        img = cv2.imread(r[2], cv2.IMREAD_UNCHANGED)
        r10 = (img[..., 2] >> 6).astype(np.float64)
        code = np.clip(np.round(r10 * 876.0 / 1023.0 + 64.0), 0, 1023).astype("<u2")
        out = os.path.join(plan.work, "vp_stage1", j["case"], j["variant"], tag + ".y16")
        os.makedirs(os.path.dirname(out), exist_ok=True)
        code.tofile(out)


# ------------------------------------------------------------------------------------------ main
def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("stage", choices=["plan", "prepare", "render", "cost", "score", "report"])
    ap.add_argument("--work", required=True)
    ap.add_argument("--dry-run", action="store_true")
    ap.add_argument("--ffmpeg", default=r"D:\nn-corpus\VP-NN-TrainKit-r2\ffmpeg\ffmpeg.exe")
    ap.add_argument("--render-tool", default=r"D:\nn-corpus\vp-scale-render\bin\VpScaleRender.exe")
    ap.add_argument("--dll", default=r"D:\nn-corpus\VP-NN-TrainKit\vpbench\vprenderer377\VideoProcessorVPRendererBeta.dll")
    ap.add_argument("--sweep-tool", default=os.path.join(HERE, "bin", "PlScaleSweep.exe"))
    ap.add_argument("--kit", default=r"D:\nn-corpus\VP-NN-TrainKit-r2")
    ap.add_argument("--kit-python", default=r"D:\nn-corpus\VP-NN-TrainKit-r2\runtime\python\python.exe")
    ap.add_argument("--d-staging", default="", help="export staging holding weights/D_hybrid_refine.pt (round 1 on the kit drive)")
    ap.add_argument("--adapter", default="NVIDIA")
    ap.add_argument("--hold", type=float, default=1.0, help="seconds VP shows each still before its capture")
    ap.add_argument("--threads", type=int, default=1, help="ffmpeg threads for prepare (1: the machine-wide single-thread rule; 0 = ffmpeg's default)")
    a = ap.parse_args(argv)
    for p in (a.work,):
        low = os.path.abspath(p).lower()
        if low.startswith(("e:\\", "f:\\")) or low.startswith(r"d:\nn-corpus"):
            print("refusing: --work must not be on E:, F: or under D:\\nn-corpus (read-only sources)")
            return 2
    plan = Plan(a).build()
    est = plan.estimate()
    if a.stage == "plan":
        print(f"{len(MASTERS)} masters x {len(SEGMENT_STARTS)} segments x {len(EVAL_FRAMES)} frames = {est['scored_frames_per_case']} scored frames per case; "
              f"{len(CASES)} cases; {len(variants())} VP variants; {len(SWEEP_FILTERS)} x {len(SWEEP_ANTIRING)} x 2 sweep configs")
        print(json.dumps(est, indent=1))
        for p in plan.problems:
            print("PROBLEM:", p)
        if not a.dry_run:
            plan.write()
            print("wrote", os.path.join(plan.work, "plan.json"))
        else:
            print(f"dry run: {len(plan.jobs)} VpScaleRender jobs validated (not written); first job:")
            if plan.jobs:
                print(job_text(a, plan.jobs[0], plan.frames[:2], plan.work))
        return 1 if plan.problems else 0
    if plan.problems and not a.dry_run:
        for p in plan.problems:
            print("PROBLEM:", p)
        return 1
    if a.stage == "prepare":
        prepare(a, plan)
        prepare_ramp(a, plan)
    elif a.stage == "render":
        if not a.dry_run:
            plan.write()
        render(a, plan)
    elif a.stage == "cost":
        cost(a, plan)
    elif a.stage == "score":
        cmd = [a.kit_python, "-I", "-B", os.path.join(HERE, "metrics.py"), "--work", plan.work, "--kit", a.kit, "--plan", os.path.join(plan.work, "plan.json")]
        if a.d_staging:
            cmd += ["--d-staging", a.d_staging]
        if a.dry_run:
            print("   ", " ".join(cmd))
        else:
            wait_quiet(a)
            subprocess.run(cmd, check=True, env=dict(os.environ, PYTHONDONTWRITEBYTECODE="1"))
    elif a.stage == "report":
        cmd = [sys.executable, os.path.join(HERE, "report.py"), "--work", plan.work]
        print("   ", " ".join(cmd)) if a.dry_run else subprocess.run(cmd, check=True)
    return 0


if __name__ == "__main__":
    sys.exit(main())
