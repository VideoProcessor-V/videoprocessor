#!/usr/bin/env python3
# Copyright(C) 2026 VP-V contributors
# SPDX-License-Identifier: GPL-3.0-only
"""Scaling sweeps through VpScaleRender, scored by the common eval harness (tools/eval on test/eval-harness).

Sweeps: 1 downscale (hybrid w_base x faithful), 2 upscale (8b grid, direct and 2-stage route),
3 one-axis / anamorphic, 4 motion-adapted vs optimal (sequences), 5 camera model on/off (--camera-model-key,
doubles sweeps 1, 2, 4).  Stages: plan | prepare | render | score | report; each takes --dry-run.
Read-only on D:\\nn-corpus; writes only under --work.  See README.md.
  python -I tools/scaling_sweep/sweep.py plan --dry-run
"""
import argparse
import csv
import json
import os
import re
import shutil
import struct
import subprocess
import sys
import time
import zlib

import numpy as np

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(os.path.dirname(HERE))
DEFAULT_WORK = r"D:\codex\scaling-sweep"
DEFAULT_HARNESS = r"D:\codex\videoprocessor\test-eval-harness\tools\eval"
VP_ROOT = r"D:\codex\videoprocessor\feat-scaling-8b-blend"
CANVAS = (3840, 2160)
SEQ_FRAMES = 24
STILL_RATE, SEQ_RATE = (24000, 1001), (24, 1)

MASTERS = [  # 4K masters (read-only corpus), same list as tools/upscale_study
    ("boat", r"D:\nn-corpus\trimmed\boat4k\reference_ffv1.mkv"),
    ("chimera", r"D:\nn-corpus\trimmed\chimera4k\reference_ffv1.mkv"),
    ("foodmarket", r"D:\nn-corpus\trimmed\foodmarket4k\reference_ffv1.mkv"),
    ("meridian", r"D:\nn-corpus\trimmed\meridian4k_existing\reference_ffv1.mkv"),
    ("sintel", r"D:\nn-corpus\trimmed\sintel4k\reference_ffv1.mkv"),
    ("sollevante", r"D:\nn-corpus\trimmed\sollevante4k\reference_ffv1.mkv"),
    ("tos", r"D:\nn-corpus\trimmed\tos4k\reference_ffv1.mkv"),
]
W_BASES = ["0", "0.1", "0.2", "0.3", "0.5", "0.7", "1.0"]
UP_W_BASES = ["0", "0.25", "0.5", "0.75", "1.0"]
AXIS_W_BASES = ["0", "1.0"]
MOTION_LO, MOTION_HI, MOTION_FLOOR, MOTION_SMOOTH = ["0.05", "0.1", "0.2"], ["0.3", "0.6"], ["0", "0.5"], ["250"]
GUARDS = ["grain_rms_ratio_Y", "grain_keep_vs_input_Y", "grain_lag1_delta_Y", "grain_lag1_vs_input_Y",
          "halo_overshoot_Y", "flicker_excess_Y", "ghost_coeff_Y", "smear_Y"]
GATING = ["psnr_Y", "ssim_Y", "lpips", "halo_overshoot_Y", "grain_rms_ratio_Y", "grain_keep_vs_input_Y",
          "flicker_excess_Y", "ghost_coeff_Y", "smear_Y"]
SWEEP_NAMES = {1: "downscale", 2: "upscale", 3: "one-axis", 4: "motion"}


def load_settings():
    with open(os.path.join(HERE, "sweep_settings.json"), encoding="utf8") as f:
        return json.load(f)


S = load_settings()


def split_shaders(d):
    d = dict(d)
    return d, d.pop("shaders", [])


def refuse_work(path):
    """None when --work is acceptable, else the refusal text (E:, F: and D:\\nn-corpus are read-only sources)."""
    p = os.path.abspath(path).replace("/", "\\").lower().rstrip("\\")
    if p.startswith(("e:\\", "f:\\")) or p in ("e:", "f:") or p == r"d:\nn-corpus" or p.startswith("d:\\nn-corpus\\"):
        return "refusing: --work must not be on E:, F: or under D:\\nn-corpus (read-only sources)"
    return None


def vtag(x):
    return str(x).replace(".", "p")


def even(x):
    return int(round(x / 2)) * 2


def route_size(src):
    """Stage 1 of the route: 2x the source, capped at 3840x2160 (aspect kept)."""
    s = min(2.0, CANVAS[0] / src[0], CANVAS[1] / src[1])
    return even(src[0] * s), even(src[1] * s)


# ------------------------------------------------------------------------------------ variants
def _V(vid, sweep, stages, base, kind="direct"):
    return dict(id=vid, sweep=sweep, kind=kind, stages=stages, base=base)   # stages: [(settings, shaders), ...]


def down_stage(w, faithful="ortho", adaptive="on", shaders=True):
    d, sh = split_shaders(S["down"])
    d.update(adaptive_downscale=adaptive, adaptive_w_base=w, adaptive_faithful=faithful)
    return d, (sh if shaders else [])


def up_stage(w, sr="on", light="linear", adaptive="on", upscaler=None, shaders=True):
    u, sh = split_shaders(S["up"])
    u.update(adaptive_upscale=adaptive, adaptive_upscale_w_base=w, adaptive_upscale_superres_scale=sr, upscale_light=light)
    if upscaler:
        u["upscaler"] = upscaler
    return u, (sh if shaders else [])


def up_control():
    return up_stage("1.0", adaptive="off", upscaler="ewa_lanczossharp", shaders=False)


def variants_down():
    v = [_V("ctrl_lanczos", 1, [down_stage("0.2", adaptive="off", shaders=False)], "ctrl_lanczos"),
         _V("ssim_only", 1, [down_stage("0.2", adaptive="off")], "ctrl_lanczos")]
    for faithful in ("auto", "ortho"):
        for w in W_BASES:
            v.append(_V(f"w{vtag(w)}_{faithful}", 1, [down_stage(w, faithful)], "ctrl_lanczos"))
    return v


def variants_up(sweep, down_w):
    """Direct 8b grid + the 2-stage route (stage 1 = the same 8b settings, stage 2 = hybrid downscale at down_w)."""
    ws = UP_W_BASES if sweep == 2 else AXIS_W_BASES
    lights = ("linear", "gamma") if sweep == 2 else ("linear",)
    v = [_V("ctrl_ewa", sweep, [up_control()], "ctrl_ewa")]
    if sweep == 2:
        v.append(_V("r_ctrl_ewa", sweep, [up_control(), down_stage(down_w)], "ctrl_ewa", "route"))
    for w in ws:
        for sr in ("on", "off"):
            for light in lights:
                n = f"w{vtag(w)}_sr{sr}_{light}"
                st = up_stage(w, sr, light)
                v.append(_V("d_" + n, sweep, [st], "ctrl_ewa"))
                if sweep == 2:
                    v.append(_V("r_" + n, sweep, [st, down_stage(down_w)], "ctrl_ewa", "route"))
    return v


def variants_motion(form, best_w):
    stage = (lambda **k: down_stage(best_w, **k)) if form == "down" else (lambda **k: up_stage(best_w, **k))
    base, sh = stage()
    v = [_V("opt", 4, [(dict(base, adaptive_motion="off"), sh)], "opt")]
    for lo in MOTION_LO:
        for hi in MOTION_HI:
            if float(lo) >= float(hi):
                continue
            for fl in MOTION_FLOOR:
                for sm in MOTION_SMOOTH:
                    s = dict(base, adaptive_motion="on", adaptive_motion_speed_lo=lo, adaptive_motion_speed_hi=hi,
                             adaptive_motion_floor=fl, adaptive_motion_smooth_ms=sm)
                    v.append(_V(f"am_lo{vtag(lo)}_hi{vtag(hi)}_fl{vtag(fl)}_sm{sm}", 4, [(s, sh)], "opt"))
    return v


def camera_expand(variants, key):
    out = []
    for v in variants:
        for state in ("off", "on"):
            c = dict(v, id=f"{v['id']}__cm_{state}", base=f"{v['base']}__cm_{state}",
                     stages=[(dict(s, **{key: state}), sh) for s, sh in v["stages"]])
            out.append(c)
    return out


# ------------------------------------------------------------------------------------ geometries
def make_pics(a, geom_id, masters, kind):
    pics, off = [], 0
    for mid in masters:
        frames = [(a.seq_start.get(mid, 24), SEQ_FRAMES)] if kind == "seq" else [(f, 1) for f in a.stills]
        for f, n in frames:
            pics.append(dict(tag=f"{mid}_f{f:03d}", master=mid, frame=f, n=n, offset=off))
            off += n
    return pics


def geometries(a):
    n_still = [m[0] for m in MASTERS[:a.n_masters]]
    n_seq = [m[0] for m in MASTERS[:a.n_seq_masters]]
    g = []

    def add(gid, sweep, form, kind, src, out, window=None, masters=None):
        g.append(dict(id=gid, sweep=sweep, form=form, kind=kind, src=src, out=out, window=window or out,
                      pics=make_pics(a, gid, masters or n_still, kind)))
    for o in ((2560, 1440), (1920, 1080)):
        add(f"s1_down_{o[0]}x{o[1]}", 1, "down", "still", CANVAS, o)
    for s in ((1920, 1080), (2560, 1440), (1280, 720)):
        add(f"s2_up_{s[0]}x{s[1]}_2560x1440", 2, "up", "still", s, (2560, 1440))
    add("s2_up_1920x1080_win2560x1600", 2, "up", "still", (1920, 1080), (2560, 1440), (2560, 1600))
    for tag, s in (("v_only", (2560, 1080)), ("h_only", (1920, 1440)), ("anamorphic", (2048, 1080))):
        add(f"s3_{tag}_{s[0]}x{s[1]}_2560x1440", 3, "up", "still", s, (2560, 1440))
    for m in n_seq:
        add(f"s4_down_{m}", 4, "down", "seq", CANVAS, (2560, 1440), masters=[m])
        add(f"s4_up_{m}", 4, "up", "seq", (1920, 1080), (2560, 1440), masters=[m])
    return [x for x in g if x["sweep"] in a.sweeps]


def geom_variants(a, g):
    if g["sweep"] == 1:
        v = variants_down()
    elif g["sweep"] in (2, 3):
        v = variants_up(g["sweep"], a.down_w_base)
    else:
        v = variants_motion(g["form"], a.best_w_base)
    if a.camera_model_key and g["sweep"] in (1, 2, 4):
        v = camera_expand(v, a.camera_model_key)
    return v


# ------------------------------------------------------------------------------------ job / cfg text
def cfg_text(settings, viewport=None):
    kv = dict(S["common"])
    kv.update(settings)
    kv["source_resolution"] = "native"
    body = [f"{k}: {v}" for k, v in kv.items()]
    lines = ["[general]", "persist_profile_selection: false", "[vprenderer]"] + body + ["[vprenderer.Default]"] + body
    if viewport:   # the geometry's screen profile (the first named viewport profile is the default)
        lines += ["[vprenderer.viewport.sweep]", "label: sweep"] + [f"{k}: {v}" for k, v in viewport.items()]
        lines += ["[vprenderer.zoom.sweep]", "label: sweep", "automatic_crop: false"]
    return "\n".join(lines) + "\n" + S["shader_sections"] + "\n"


def src_raw(work, g, pic):
    return os.path.join(work, "src", g["id"], pic["tag"] + ".y16")


def stage1_raw(work, g, vid, pic):
    return os.path.join(work, "stage1", g["id"], vid, pic["tag"] + ".y16")


def job_text(a, j, g, work):
    d = j["dir"]
    seq = g["kind"] == "seq"
    rate = SEQ_RATE if seq else STILL_RATE
    L = [f"dll {a.renderer_dll}", f"config {os.path.join(d, 'render.cfg')}", f"shots {os.path.join(d, 'shots')}",
         f"result {os.path.join(d, 'result.txt')}", f"log {os.path.join(d, 'vp.log')}",
         f"source {j['source'][0]} {j['source'][1]} {rate[0]} {rate[1]}", f"window {j['window'][0]} {j['window'][1]}", f"hold {a.hold}"]
    if j["shaders"]:
        L.append("shaders " + " ".join(j["shaders"]))
    L.append(f"require_adapter {S['adapter']}")
    for h in j["hooks"]:
        L.append(f"require_hook {h}")
    if seq:
        L += ["motion vectors", "capture per_frame", f"settle {a.settle}", f"require_motion {a.require_motion}"]
    for i, pic in enumerate(g["pics"]):
        p = stage1_raw(work, g, j["variant"], pic) if j["feed"] == "stage1" else src_raw(work, g, pic)
        if seq:
            L.append(f"picture_seq {pic['tag']} {p} {pic['n']}")
        else:
            if i == 0:   # warm-up so VP's kernel identification and latch see evidence first
                L.append(f"picture warmup {p} 1")
            L.append(f"picture {pic['tag']} {p} 1")
    return "\n".join(L) + "\n"


def hooks_for(shaders):
    names = {"ssim_downscaler": "SSimDownscaler.glsl", "fsrcnnx": "FSRCNNX_x2_16-0-4-1.glsl", "ssim_superres": "SSimSuperRes.glsl"}
    return sorted({h for sh in shaders for k, h in names.items() if sh.endswith(k)})


class Plan:
    def __init__(self, a):
        self.a = a
        self.work = os.path.abspath(a.work)
        self.geoms = geometries(a)
        self.jobs, self.problems = [], []
        for g in self.geoms:
            for v in geom_variants(a, g):
                for si, (settings, shaders) in enumerate(v["stages"]):
                    last = si == len(v["stages"]) - 1
                    jid = f"{g['id']}__{v['id']}__s{si + 1}"
                    first_route = v["kind"] == "route" and si == 0
                    window = route_size(g["src"]) if first_route else g["window"]
                    self.jobs.append(dict(id=jid, dir=os.path.join(self.work, "vp", jid), geom=g["id"], variant=v["id"], stage=si + 1,
                                          final=last, base=v["base"], feed="stage1" if si == 1 else "delivered",
                                          source=list(route_size(g["src"]) if si == 1 else g["src"]), window=list(window),
                                          shaders=shaders, hooks=hooks_for(shaders), cfg=cfg_text(settings, g.get("viewport"))))
        self.check()

    def geom(self, gid):
        return next(g for g in self.geoms if g["id"] == gid)

    def check(self):
        a = self.a
        for t, p in (("ffmpeg", a.ffmpeg), ("VpScaleRender", a.render_tool), ("renderer DLL", a.renderer_dll), ("eval harness", os.path.join(a.harness, "eval_harness.py"))):
            if not (p and os.path.exists(p)):
                self.problems.append(f"{t} not found: {p}")
        for _, p in MASTERS[:max(a.n_masters, a.n_seq_masters)]:
            if not os.path.exists(p):
                self.problems.append(f"master missing: {p}")
        for gid in {j["geom"] for j in self.jobs}:
            ids = [j["id"] for j in self.jobs if j["geom"] == gid]
            if len(ids) != len(set(ids)):
                self.problems.append(f"duplicate job ids in {gid}")

    def estimate(self):
        a = self.a
        per = {}
        for sw in sorted({g["sweep"] for g in self.geoms}):
            gs = [g for g in self.geoms if g["sweep"] == sw]
            jobs = [j for j in self.jobs if self.geom(j["geom"])["sweep"] == sw]
            gpu = sum(15 + (3 + 0.6 * SEQ_FRAMES if self.geom(j["geom"])["kind"] == "seq" else (len(self.geom(j["geom"])["pics"]) + 1) * (a.hold + 0.3)) for j in jobs)
            runs = sum(1 for j in jobs if j["final"])
            frames = sum(sum(p["n"] for p in self.geom(j["geom"])["pics"]) for j in jobs if j["final"])
            per[sw] = dict(name=SWEEP_NAMES[sw], geometries=len(gs), variants=len({j["variant"] for j in jobs}), jobs=len(jobs),
                           scored_variant_geometries=runs, scored_frames=frames, gpu_h=round(gpu / 3600, 2),
                           score_cpu_h=round((runs * 8 + frames * 3) / 3600, 2))
        return per


# ------------------------------------------------------------------------------------ PNG / capture conversion
def _png_chunks(data):
    pos = 8
    while pos < len(data):
        n, typ = struct.unpack(">I4s", data[pos:pos + 8])
        yield typ, data[pos + 8:pos + 8 + n]
        pos += 12 + n


def write_png16(path, rgb):
    """Minimal 16-bit RGB PNG writer (filter 0); rgb is uint16 (h, w, 3)."""
    h, w, _ = rgb.shape
    raw = b"".join(b"\x00" + rgb[y].astype(">u2").tobytes() for y in range(h))

    def chunk(t, d):
        return struct.pack(">I", len(d)) + t + d + struct.pack(">I", zlib.crc32(t + d) & 0xFFFFFFFF)
    with open(path, "wb") as f:
        f.write(b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 16, 2, 0, 0, 0)) + chunk(b"IDAT", zlib.compress(raw)) + chunk(b"IEND", b""))


def _read_png_py(path):
    """Fallback 16-bit RGB/RGBA PNG decoder (numpy; filters 3/4 are per-byte and slow: use cv2 for real captures). Returns the first channel (R) as uint16."""
    with open(path, "rb") as fh:
        data = fh.read()
    ihdr, idat = None, b""
    for typ, d in _png_chunks(data):
        if typ == b"IHDR":
            ihdr = struct.unpack(">IIBBBBB", d)
        elif typ == b"IDAT":
            idat += d
    w, h, depth, ctype = ihdr[:4]
    if depth != 16 or ctype not in (2, 6):
        raise ValueError("fallback PNG reader handles 16-bit RGB/RGBA only")
    bpp = 6 if ctype == 2 else 8
    raw = np.frombuffer(zlib.decompress(idat), np.uint8).reshape(h, 1 + w * bpp)
    prev = np.zeros(w * bpp, np.int32)
    img = np.zeros((h, w * bpp), np.uint8)
    for y in range(h):
        f, line = raw[y, 0], raw[y, 1:].astype(np.int32)
        if f == 0:
            cur = line
        elif f == 2:
            cur = (line + prev) & 255
        else:   # 1 sub, 3 average, 4 paeth: sequential in the previous bytes of the same row
            cur = np.zeros(w * bpp, np.int32)
            for i in range(w * bpp):
                a_ = cur[i - bpp] if i >= bpp else 0
                b_ = prev[i]
                c_ = prev[i - bpp] if i >= bpp else 0
                if f == 1:
                    p = a_
                elif f == 3:
                    p = (a_ + b_) >> 1
                else:
                    pa, pb, pc = abs(b_ - c_), abs(a_ - c_), abs(a_ + b_ - 2 * c_)
                    p = a_ if pa <= pb and pa <= pc else (b_ if pb <= pc else c_)
                cur[i] = (line[i] + p) & 255
        img[y] = cur
        prev = cur
    px = img.reshape(h, w, bpp // 2, 2)
    return (px[..., 0, 0].astype(np.uint16) << 8) | px[..., 0, 1]


def read_png_r(path):
    """The R channel of a 16-bit capture PNG as uint16 (cv2 when present, else the fallback decoder)."""
    try:
        import cv2
        img = cv2.imread(path, cv2.IMREAD_UNCHANGED)
        if img is not None and img.dtype == np.uint16:
            return img[..., 2]
    except ImportError:
        pass
    return _read_png_py(path)


def png_to_luma_codes(path):
    """VP's rendered-output capture (R10 code << 6 in 16 bits, grey picture) -> limited-range 10-bit luma codes
    (same mapping as tools/upscale_study export_stage1)."""
    r10 = (read_png_r(path) >> 6).astype(np.float64)
    return np.clip(np.round(r10 * 876.0 / 1023.0 + 64.0), 0, 1023).astype("<u2")


def yuv420p10_bytes(Y):
    h, w = Y.shape
    c = np.full(((h + 1) // 2, (w + 1) // 2), 512, "<u2")
    return Y.astype("<u2").tobytes() + c.tobytes() * 2


def pad_codes(Y, window):
    """Centre a picture on a window-sized canvas of limited black (code 64)."""
    h, w = Y.shape
    W, H = window
    if (w, h) == (W, H):
        return Y
    out = np.full((H, W), 64, "<u2")
    y0, x0 = (H - h) // 2, (W - w) // 2
    out[y0:y0 + h, x0:x0 + w] = Y
    return out


def result_rows(result_path):
    """-> [(tag, frame_index, png)] of ok captures (warm-up and unmatched captures dropped)."""
    rows = []
    if not os.path.exists(result_path):
        return rows
    for line in open(result_path, encoding="utf8", errors="replace"):
        f = line.split()
        if len(f) >= 7 and f[6] == "ok" and f[0] not in ("warmup", "-") and os.path.exists(f[2]):
            rows.append((f[0], int(f[1]), f[2]))
    return rows


def write_capture_dir(out_dir, g, rows, vp_commit="", config_hash=""):
    """rows from result_rows -> NNNNNN.yuv (Y from the capture, chroma 512) + capture.json (schema 1) keyed by the geometry's global frame index."""
    os.makedirs(out_dir, exist_ok=True)
    pics = {p["tag"]: p for p in g["pics"]}
    frames = []
    for tag, idx, png in sorted(rows, key=lambda r: (pics[r[0]]["offset"] + r[1]) if r[0] in pics else 1 << 30):
        if tag not in pics or idx >= pics[tag]["n"]:
            continue
        gi = pics[tag]["offset"] + idx
        name = f"{gi:06d}.yuv"
        with open(os.path.join(out_dir, name), "wb") as f:
            f.write(yuv420p10_bytes(png_to_luma_codes(png)))
        frames.append(dict(file=name, source_frame_index=gi))
    meta = dict(schema=1, producer="VpScaleRender", vp_commit=vp_commit, config_hash=config_hash, width=g["window"][0], height=g["window"][1],
                pix_fmt="yuv420p10le", bitdepth=10, range="limited", matrix="709", transfer="sdr", fps=24.0, frames=frames)
    with open(os.path.join(out_dir, "capture.json"), "w", newline="\n") as f:
        json.dump(meta, f, indent=1)
    return meta


def suite_json(g, work):
    """Harness suite for one geometry, protocol `same`; master = ideal-resampled reference, coded = plain Catmull-Rom (the 'input' of the *_vs_input gates)."""
    def src(kind):
        return dict(kind="raw", path=os.path.join(work, kind, g["id"] + ".yuv").replace("\\", "/"), width=g["window"][0], height=g["window"][1],
                    pix_fmt="yuv420p10le", fps=24.0)
    cases = []
    for p in g["pics"]:
        cases.append(dict(id=f"{g['id']}__{p['tag']}", content_type="film24", clip=p["master"], case=f"{g['id']}__{p['tag']}", protocol="same",
                          frames=p["n"], master=dict(src("ref"), start=p["offset"]), coded=dict(src("coded"), start=p["offset"])))
    return dict(name=g["id"], description=f"scaling sweep {SWEEP_NAMES[g['sweep']]}: {g['src']} -> {g['out']} in window {g['window']}", cases=cases)


# ------------------------------------------------------------------------------------ prepare
def run_or_print(a, cmd, out):
    if a.dry_run:
        print("   ", " ".join(f'"{c}"' if " " in c else c for c in cmd))
        return
    if os.path.exists(out) and os.path.getsize(out) > 0:
        return
    os.makedirs(os.path.dirname(out), exist_ok=True)
    subprocess.run(cmd, check=True)


def prepare_cmd(a, g, pic, master_path, outs):
    """One ffmpeg per picture: master -> fitted+padded 3840x2160 canvas -> (source raw, reference raw, coded raw), luma only."""
    sel = f"between(n\\,{pic['frame']}\\,{pic['frame'] + pic['n'] - 1})"
    sw, sh = g["src"]
    ow, oh = g["out"]
    fit = (f"scale={CANVAS[0]}:{CANVAS[1]}:force_original_aspect_ratio=increase:flags=lanczos,crop={CANVAS[0]}:{CANVAS[1]}" if g.get("fill") else
           f"scale={CANVAS[0]}:{CANVAS[1]}:force_original_aspect_ratio=decrease:flags=lanczos,pad={CANVAS[0]}:{CANVAS[1]}:(ow-iw)/2:(oh-ih)/2")
    chain = (f"[0:v]select={sel},{fit},format=yuv420p10le,extractplanes=y,split=3[a][b][c];"
             f"[a]scale={sw}:{sh}:flags=lanczos[s];[b]scale={ow}:{oh}:flags=lanczos[r];[c]scale={ow}:{oh}:flags=bicubic:param0=0:param1=0.5[k]")
    # ffmpeg 9: -fps_mode is an output option (before each output); extractplanes needs a fixed input format.
    cmd = [a.ffmpeg, "-y", "-v", "error", "-threads", str(a.threads), "-i", master_path, "-filter_complex", chain]
    for label, path in zip("srk", outs):
        cmd += ["-map", f"[{label}]", "-fps_mode", "passthrough", "-frames:v", str(pic["n"]), "-f", "rawvideo", "-pix_fmt", "gray10le", path]
    return cmd


def encode_cmds(a, g, pic, raw):
    """Optional HEVC round trip of the (already ideally downscaled) source, GOP-bearing like a stream."""
    w, h = g["src"]
    mp4 = raw + ".mp4"
    enc = [a.ffmpeg, "-y", "-v", "error", "-f", "rawvideo", "-pix_fmt", "gray10le", "-s", f"{w}x{h}", "-r", "24", "-i", raw, "-c:v", "libx265",
           "-pix_fmt", "gray10", "-b:v", f"{a.encode_kbps}k", "-g", "48", "-bf", "3", "-x265-params", "pools=1:frame-threads=1", mp4]
    dec = [a.ffmpeg, "-y", "-v", "error", "-i", mp4, "-f", "rawvideo", "-pix_fmt", "gray10le", raw + ".dec"]
    return enc, dec


def assemble(work, g, kind):
    """Per-picture luma raws -> one planar yuv420p10le file per geometry (picture centred in the window, chroma 512)."""
    out = os.path.join(work, kind, g["id"] + ".yuv")
    if os.path.exists(out):
        return
    os.makedirs(os.path.dirname(out), exist_ok=True)
    ow, oh = g["out"]
    with open(out, "wb") as f:
        for p in g["pics"]:
            raw = np.fromfile(os.path.join(work, "pre", kind, g["id"], p["tag"] + ".y16"), "<u2").reshape(p["n"], oh, ow)
            for fr in raw:
                f.write(yuv420p10_bytes(pad_codes(fr, g["window"])))


def prepared(plan):
    """The geometries whose prepare output (suite + ref + coded) is missing."""
    return [g["id"] for g in plan.geoms if not all(os.path.exists(p) for p in (
        os.path.join(plan.work, "suites", g["id"] + ".json"), os.path.join(plan.work, "ref", g["id"] + ".yuv"),
        os.path.join(plan.work, "coded", g["id"] + ".yuv")))]


def prepare(a, plan):
    """Returns 0, or 1 after printing every failed picture (partial outputs removed; nothing assembled for it)."""
    masters = dict(MASTERS)
    failed = []
    for g in plan.geoms:
        ok = True
        for p in g["pics"]:
            outs = [src_raw(plan.work, g, p)] + [os.path.join(plan.work, "pre", k, g["id"], p["tag"] + ".y16") for k in ("ref", "coded")]
            try:
                if not a.dry_run:
                    for o in outs:   # every output's folder (ffmpeg opens all three)
                        os.makedirs(os.path.dirname(o), exist_ok=True)
                run_or_print(a, prepare_cmd(a, g, p, masters[p["master"]], outs), outs[0])
            except (subprocess.CalledProcessError, OSError) as e:
                print(f"PREPARE FAILED {g['id']} {p['tag']}: {e}", flush=True)
                for o in outs:
                    if os.path.exists(o):
                        os.remove(o)
                failed.append(f"{g['id']}/{p['tag']}")
                ok = False
                continue
            if a.encode_kbps and g["sweep"] in (2, 4) and g["form"] == "up":
                enc, dec = encode_cmds(a, g, p, outs[0])
                if a.dry_run:
                    print("   ", " ".join(enc), "\n   ", " ".join(dec))
                elif not os.path.exists(outs[0] + ".dec"):
                    subprocess.run(enc, check=True)
                    subprocess.run(dec, check=True)
                    os.replace(outs[0] + ".dec", outs[0])
        if a.dry_run:
            print(f"    assemble {g['id']}: ref/ coded/ yuv + suites/{g['id']}.json")
            continue
        if not ok:
            continue
        for kind in ("ref", "coded"):
            assemble(plan.work, g, kind)
        os.makedirs(os.path.join(plan.work, "suites"), exist_ok=True)
        with open(os.path.join(plan.work, "suites", g["id"] + ".json"), "w", newline="\n") as f:
            json.dump(suite_json(g, plan.work), f, indent=1)
    if failed:
        print("PREPARE FAILED for", len(failed), "picture(s); render/score refuse until prepare succeeds:", *failed, sep="\n  ")
        return 1
    return 0


# ------------------------------------------------------------------------------------ render
def busy_processes():
    try:
        out = subprocess.run(["tasklist", "/FO", "CSV"], capture_output=True, text=True).stdout
    except OSError:
        return []
    names = re.findall(r'^"([^"]+)"', out, re.M)
    return sorted({n for n in names if n.lower().startswith(("vstest", "testhost", "videoprocessor.exe", "videoprocessor-test", "vpscalerender"))})


def wait_quiet(a):
    """Never kills anything: waits while another test / VP / render process holds the GPU."""
    while not a.dry_run:
        b = busy_processes()
        if not b:
            return
        print("waiting: busy:", ", ".join(b), flush=True)
        time.sleep(60)


def write_jobs(plan):
    for j in plan.jobs:
        os.makedirs(j["dir"], exist_ok=True)
        with open(os.path.join(j["dir"], "render.cfg"), "w", newline="\n") as f:
            f.write(j["cfg"])
        with open(os.path.join(j["dir"], "job.txt"), "w", newline="\n") as f:
            f.write(job_text(plan.a, j, plan.geom(j["geom"]), plan.work))
    with open(os.path.join(plan.work, "plan.json"), "w", newline="\n") as f:
        json.dump(dict(geoms=plan.geoms, jobs=[{k: v for k, v in j.items() if k != "cfg"} for j in plan.jobs], estimate=plan.estimate()), f, indent=1)


def export_stage1(plan, j):
    """Stage-1 captures -> 10-bit luma raws (frames in order) that the route's stage-2 job reads."""
    g = plan.geom(j["geom"])
    by = {}
    for tag, idx, png in result_rows(os.path.join(j["dir"], "result.txt")):
        by.setdefault(tag, {})[idx] = png
    for p in g["pics"]:
        out = stage1_raw(plan.work, g, j["variant"], p)
        os.makedirs(os.path.dirname(out), exist_ok=True)
        with open(out, "wb") as f:
            for i in range(p["n"]):
                f.write(png_to_luma_codes(by[p["tag"]][i]).tobytes())


def render(a, plan):
    env = dict(os.environ, VP_TEST_SHADER_DIR=os.path.dirname(a.renderer_dll), VP_TEST_ADAPTER=S["adapter"])
    bad = []
    if not a.dry_run:
        write_jobs(plan)
    for stage in (1, 2):
        for j in [x for x in plan.jobs if x["stage"] == stage]:
            done = os.path.join(j["dir"], "done.txt")
            if a.dry_run:
                print(f"    {a.render_tool} {os.path.join(j['dir'], 'job.txt')}")
                continue
            if os.path.exists(done):
                continue
            wait_quiet(a)
            r = subprocess.run([a.render_tool, os.path.join(j["dir"], "job.txt")], env=env, capture_output=True, text=True, timeout=7200)
            print(f"{j['id']}: rc {r.returncode}", flush=True)
            if r.returncode != 0:
                bad.append(j["id"])
                continue
            if not j["final"]:
                try:
                    export_stage1(plan, j)
                except (KeyError, OSError) as e:
                    print(f"  stage-1 export failed: {e!r}")
                    bad.append(j["id"])
                    continue
            open(done, "w").write("ok\n")
    if bad:
        print("FAILED jobs:", *bad, sep="\n  ")
    return 1 if bad else 0


# ------------------------------------------------------------------------------------ score / report
def score(a, plan):
    bad = 0
    harness = os.path.join(a.harness, "eval_harness.py")
    for j in [x for x in plan.jobs if x["final"]]:
        g = plan.geom(j["geom"])
        cap = os.path.join(plan.work, "cap", g["id"], j["variant"])
        base = os.path.join(plan.work, "cap", g["id"], j["base"])
        out = os.path.join(plan.work, "score", g["id"], j["variant"])
        cmd = [a.score_python, "-I", harness, "run", "--suite", os.path.join(plan.work, "suites", g["id"] + ".json"), "--candidate", "capture:" + cap,
               "--baseline", "capture:" + base, "--out", out, "--no-cost"] + (["--lpips"] if a.lpips else [])
        if a.dry_run:
            print("   ", " ".join(f'"{c}"' if " " in c else c for c in cmd))
            continue
        if os.path.exists(os.path.join(out, "results.csv")):
            continue
        for jj in (j, next(x for x in plan.jobs if x["geom"] == g["id"] and x["variant"] == j["base"] and x["final"])):
            d = os.path.join(plan.work, "cap", g["id"], jj["variant"])
            if not os.path.exists(os.path.join(d, "capture.json")):
                write_capture_dir(d, g, result_rows(os.path.join(jj["dir"], "result.txt")))
        if subprocess.run(cmd).returncode != 0:
            bad += 1
    return 1 if bad else 0


def num(v):
    try:
        return float(v)
    except (TypeError, ValueError):
        return None


def summarise(csv_path):
    """results.csv -> dict(metric -> mean candidate), fails (guard metrics with a FAIL flag), flags count."""
    vals, fails, flags = {}, set(), {}
    with open(csv_path, encoding="utf8", newline="") as f:
        for r in csv.DictReader(f):
            flags[r["flag"]] = flags.get(r["flag"], 0) + 1
            x = num(r["candidate"])
            if x is not None and x == x and abs(x) != float("inf"):
                vals.setdefault(r["metric"], []).append(x)
            if r["flag"] == "FAIL" and r["metric"] in GUARDS:
                fails.add(r["metric"])
    return {k: sum(v) / len(v) for k, v in vals.items()}, sorted(fails), flags


def best_variant(rows):
    """Highest mean ssim_Y among variants that break no hard guard (no FAIL flag on a GUARDS metric)."""
    ok = [r for r in rows if not r["fails"] and r["m"].get("ssim_Y") is not None]
    return max(ok, key=lambda r: (r["m"]["ssim_Y"], r["m"].get("psnr_Y", 0))) if ok else None


def report(a, plan):
    out_dir = os.path.join(plan.work, "report")
    for sw in sorted({g["sweep"] for g in plan.geoms}):
        lines = [f"# Sweep {sw}: {SWEEP_NAMES[sw]}", "",
                 "Mean over the geometry's pictures, candidate vs the sweep's control/baseline (harness protocol `same`, capture luma R10 -> limited codes).",
                 "BTB/WTW rows are not gated here: the R10 capture is full-range RGB, so beyond-limit codes cannot appear (see README).", ""]
        for g in [x for x in plan.geoms if x["sweep"] == sw]:
            rows = []
            for j in [x for x in plan.jobs if x["geom"] == g["id"] and x["final"]]:
                p = os.path.join(plan.work, "score", g["id"], j["variant"], "results.csv")
                if os.path.exists(p):
                    m, fails, flags = summarise(p)
                    rows.append(dict(v=j["variant"], base=j["base"], m=m, fails=fails, flags=flags))
            lines += [f"## {g['id']}", "", f"{g['src'][0]}x{g['src'][1]} -> {g['out'][0]}x{g['out'][1]}, window {g['window'][0]}x{g['window'][1]}, "
                      f"{len(g['pics'])} picture(s); {len(rows)} variants scored.", ""]
            if not rows:
                lines += ["No scored variants yet.", ""]
                continue
            lines += ["| variant | guard FAILs | " + " | ".join(GATING) + " |", "|---|---|" + "---|" * len(GATING)]
            for r in rows:
                lines.append(f"| {r['v']} | {', '.join(r['fails']) or '-'} | " + " | ".join("NA" if r["m"].get(k) is None else f"{r['m'][k]:.4g}" for k in GATING) + " |")
            b = best_variant(rows)
            lines += ["", f"Best variant that breaks no hard guard (max ssim_Y): **{b['v'] if b else 'none'}**", ""]
        os.makedirs(out_dir, exist_ok=True) if not a.dry_run else None
        path = os.path.join(out_dir, f"sweep{sw}_{SWEEP_NAMES[sw]}.md")
        if a.dry_run:
            print("   would write", path)
        else:
            with open(path, "w", encoding="utf8", newline="\n") as f:
                f.write("\n".join(lines) + "\n")
            print("wrote", path)
    return 0


# ------------------------------------------------------------------------------------ main
def parse_args(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("stage", choices=["plan", "prepare", "render", "score", "report"])
    ap.add_argument("--work", default=DEFAULT_WORK)
    ap.add_argument("--dry-run", action="store_true")
    ap.add_argument("--sweeps", default="1,2,3,4", help="comma list of sweeps to include")
    ap.add_argument("--camera-model-key", default="", help="renderer cfg key; doubles sweeps 1, 2, 4 with KEY: on / off")
    ap.add_argument("--n-masters", type=int, default=3, help="stills: the first N masters")
    ap.add_argument("--n-seq-masters", type=int, default=3, help="sequences (sweep 4): the first N masters")
    ap.add_argument("--stills", default="12,35", help="master frame numbers used as stills")
    ap.add_argument("--seq-start", default="", help="sweep 4 start frames, e.g. boat=60,chimera=24 (default 24; choose pans)")
    ap.add_argument("--down-w-base", default="0.2", help="route stage 2 (hybrid downscale) adaptive_w_base")
    ap.add_argument("--best-w-base", default="0.2", help="sweep 4 optimal baseline w_base (down and up form)")
    ap.add_argument("--hold", type=float, default=1.0)
    ap.add_argument("--settle", type=float, default=3.0)
    ap.add_argument("--require-motion", type=float, default=0.5)
    ap.add_argument("--encode-kbps", type=int, default=0, help="HEVC round trip of the upscale sources (0 = ideal downscale only)")
    ap.add_argument("--threads", type=int, default=1)
    ap.add_argument("--lpips", action="store_true")
    ap.add_argument("--ffmpeg", default=r"D:\nn-corpus\VP-NN-TrainKit-r2\ffmpeg\ffmpeg.exe")
    ap.add_argument("--render-tool", default=VP_ROOT + r"\x64\Release\VpScaleRender\VpScaleRender.exe")
    ap.add_argument("--renderer-dll", default=VP_ROOT + r"\x64\Release\vprenderer\VideoProcessorVPRenderer.dll")
    ap.add_argument("--harness", default=DEFAULT_HARNESS)
    ap.add_argument("--score-python", default=sys.executable, help="python for the harness (LPIPS needs the torch venv, e.g. D:\\hfr\\venv\\Scripts\\python.exe)")
    a = ap.parse_args(argv)
    a.sweeps = [int(x) for x in a.sweeps.split(",") if x]
    a.stills = [int(x) for x in a.stills.split(",") if x]
    a.seq_start = {k: int(v) for k, v in (kv.split("=") for kv in a.seq_start.split(",") if kv)}
    return a


def main(argv=None):
    a = parse_args(argv)
    msg = refuse_work(a.work)
    if msg:
        print(msg)
        return 2
    plan = Plan(a)
    if a.stage == "plan":
        est = plan.estimate()
        print(f"work {plan.work}; camera-model key: {a.camera_model_key or '(none)'}; {len(plan.geoms)} geometries, {len(plan.jobs)} VpScaleRender jobs")
        for sw, e in est.items():
            print(f"  sweep {sw} {e['name']:<10} geometries {e['geometries']:>2}  variants/geom {e['variants']:>3}  jobs {e['jobs']:>4}  "
                  f"scored (variant x geom) {e['scored_variant_geometries']:>4}  frames {e['scored_frames']:>5}  GPU ~{e['gpu_h']} h  score CPU ~{e['score_cpu_h']} h")
        print(f"  total GPU ~{round(sum(e['gpu_h'] for e in est.values()), 2)} h (laptop RTX 3050 Ti estimate), score CPU ~{round(sum(e['score_cpu_h'] for e in est.values()), 2)} h")
        for p in plan.problems:
            print("PROBLEM:", p)
        if a.dry_run:
            print("dry run: nothing written; first job:\n" + job_text(a, plan.jobs[0], plan.geom(plan.jobs[0]["geom"]), plan.work))
        else:
            os.makedirs(plan.work, exist_ok=True)
            write_jobs(plan)
            print("wrote", os.path.join(plan.work, "plan.json"), "and", len(plan.jobs), "job files")
        return 1 if plan.problems and not a.dry_run else 0
    if plan.problems and not a.dry_run and a.stage != "report":
        print(*["PROBLEM: " + p for p in plan.problems], sep="\n")
        return 1
    if a.stage == "prepare":
        return prepare(a, plan)
    if a.stage in ("render", "score") and not a.dry_run:
        missing = prepared(plan)
        if missing:
            print("PROBLEM: prepare output missing for", ", ".join(missing), "- run prepare first (it must exit 0)")
            return 1
    return {"render": render, "score": score, "report": report}[a.stage](a, plan)


if __name__ == "__main__":
    sys.exit(main())
