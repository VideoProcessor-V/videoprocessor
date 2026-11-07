# Copyright(C) 2026 VP-V contributors
# SPDX-License-Identifier: GPL-3.0-only
"""Source-identity evaluation reels R1-R9 (doc 78 section 9): rung encodes on disk, the PLAYER CHAIN as ffmpeg filter strings.

From native masters (>= 3840 wide, D:\\testclips) this builds
  * RUNG files: a master window re-encoded ONCE at its coded size (N x ratio per axis: 1/2, 2/3, 1/3, 5/12, 1/4, letterbox /
    pillarbox variants, native-4K stream encodes, studio 1080->2160 encodes), GOP ~2 s derived from the master rate, fixed GOP
    (scenecut off), frame types read back with ffprobe, per-frame QP from the encoder's own log where it exists (x265 csv);
  * REELS: lists of segments. Nothing of the player chain is rendered to disk. Per reel the manifest stores the ffmpeg inputs and
    ONE -filter_complex string (upscale kernel + params, unsharp amount/radius, dither, 8/10-bit, chroma pix_fmt, crop/pad,
    drawtext/drawbox overlays, synthetic UI menus, PiP, 3:2 / blended FRC, splices) that the harness
    (SourceIdChainEvalTests, VP_SOURCE_ID_REELS) runs through its ffmpeg pipe at the delivered size;
  * TRUTH: per segment (coded n per axis, kernel, sharpening, double-scale n1, active area, native-coded flag, chroma chain, bit
    depth, dither, regime/hold) inside reels.json, plus one per-frame CSV per reel (output frame -> source frame, pict_type, QP,
    repeat flag) for the TD-2 frame-pattern scorer.
Deterministic: fixed seeds / parameters / thread counts, ffmpeg version recorded. No clip content is committed anywhere.

  python -I tools/eval/source_id/make_identity_reels.py --out D:/testclips/sourceid            (everything)
  python -I tools/eval/source_id/make_identity_reels.py --out D:/testclips/sourceid --plan      (list + disk estimate only)
  python -I tools/eval/source_id/make_identity_reels.py --selftest                              (pure logic, no ffmpeg)
Options: --families R1,R2  --masters id,id  --gops 15 (reel length = gops x ~2 s)  --limit N  --manifest-only  --threads 6
         --min-free-gb 40  --rung-budget-gb 15  --preset-override ultrafast (smoke only)  --ffmpeg-dir D:/tools/ffmpeg/bin
"""
import argparse
import csv
import hashlib
import json
import math
import os
import random
import re
import shutil
import subprocess
import sys
from fractions import Fraction

SEED = 20261107
SCHEMA = 1
FONT = "C:/Windows/Fonts/arial.ttf"
MAJOR_RATE = 1.6            # rate step regarded as a MAJOR quality event (same as make_abr_reels.py)
CLIPS_ROOT = "D:/testclips"
EVAL_ROOT = CLIPS_ROOT + "/eval-2026-10-31"

# ---------------------------------------------------------------- masters
# (id, class, parts[(relative path, ss seconds)], licence override or None). Parts are concatenated and looped (flagged) when
# the sources are shorter than the reel. 8-bit masters (pix_fmt yuv420p) may be played AS-IS; the others only via rung encodes.
MASTERS = [
    ("film_dragon", "film24_grain", [(EVAL_ROOT + "/film_grain_24p/The_Dragon_Dies_Hard_Trailer_1975_35mm_Flat_Mono/The_Dragon_Dies_Hard_Trailer_1975_35mm_Flat_Mono.webm", 30.0)], None),
    ("film_married", "film24_grain", [(EVAL_ROOT + "/film_grain_24p/A_Married_Woman_Trailer_1964_35mm_Flat_Mono_UHD/A_Married_Woman_Trailer_1964_35mm_Flat_Mono_UHD.webm", 40.0)], None),
    ("nat30_nikon", "nature30", [(EVAL_ROOT + "/nature_30p_4k/4K_Nature_Cinematography_recorded_with_Nikon_150b/4K_Nature_Cinematography_recorded_with_Nikon_150b.webm", 20.0)], None),
    ("nat30_sequim", "nature30", [(EVAL_ROOT + "/nature_30p_4k/Drone_4k_Video_Sequim_WA/Drone_4k_Video_Sequim_WA.webm", 60.0)], None),
    ("nat24_steeple", "nature24", [(EVAL_ROOT + "/nature_other/Steeple_Jason_Island_Falkland_Islands_Malvin_17c7/Steeple_Jason_Island_Falkland_Islands_Malvin_17c7.webm", 10.0)], None),
    ("nat25_funchal", "static_long_take25", [(EVAL_ROOT + "/nature_other/Funchal_Madeira_amazing_view_from_the_window_1abb/Funchal_Madeira_amazing_view_from_the_window_1abb.webm", 10.0)], None),
    ("sport60_k808", "hfr60_pans", [(EVAL_ROOT + "/hfr_4k60/20230513_K808_4K_60FPS_HDR/20230513_K808_4K_60FPS_HDR.webm", 8.0)], None),
    ("sport30_torschuss", "sports30", [(EVAL_ROOT + "/sports_ball/Amateurfu_ball_Torschuss_von_oben/Amateurfu_ball_Torschuss_von_oben.webm", 0.5)], None),
    ("dark60_meridian", "dark_lowkey60", [(CLIPS_ROOT + "/meridian_src/Meridian_UHD4k5994_HDR_P3PQ.mp4", 100.0)],
     {"lic": "CC BY 4.0 Netflix Open Content", "url": "http://download.opencontent.netflix.com/Meridian/Meridian_UHD4k5994_HDR_P3PQ.mp4"}),
    ("flat_sol", "flat_cg_anime24", [(EVAL_ROOT + "/hdr/sol_levante_hdr10_24p_t40/sol_levante_hdr10_24p_t40.mov", 0.0),
                                      (EVAL_ROOT + "/hdr/sol_levante_hdr10_24p_t150/sol_levante_hdr10_24p_t150.mov", 0.0)], None),
    ("grain60_nocturne", "hdr_grain60", [(EVAL_ROOT + "/hdr_film_grain/nocturne_filmgrain_hdr_60p_t120/nocturne_filmgrain_hdr_60p_t120.mov", 0.0),
                                          (EVAL_ROOT + "/hdr_film_grain/nocturne_filmgrain_hdr_60p_t300/nocturne_filmgrain_hdr_60p_t300.mov", 0.0),
                                          (EVAL_ROOT + "/hdr_film_grain/nocturne_filmgrain_hdr_60p_t480/nocturne_filmgrain_hdr_60p_t480.mov", 0.0)], None),
    ("synth_credits", "credits_scroll24", [], {"lic": "synthetic (generated, no third-party content)", "url": ""}),
]
SYNTHETIC = {"synth_credits": dict(w=3840, h=2160, fps=(24000, 1001))}

# swscale kernels (always present) and zimg ones (only with zscale in the ffmpeg build; --no-zscale drops them).
KERNELS = {
    "bilinear": dict(backend="sws", flags="bilinear", family="bilinear", params={}),
    "bicubic": dict(backend="sws", flags="bicubic", family="bicubic", params={"B": 0.0, "C": 0.6}),        # swscale default
    "catmull": dict(backend="sws", flags="bicubic", family="bicubic", params={"B": 0.0, "C": 0.5}),
    "mitchell": dict(backend="sws", flags="bicubic", family="bicubic", params={"B": 0.3333, "C": 0.3333}),
    "lanczos2": dict(backend="sws", flags="lanczos", family="lanczos2", params={"taps": 2}),
    "lanczos3": dict(backend="sws", flags="lanczos", family="lanczos3", params={"taps": 3}),
    "lanczos4": dict(backend="sws", flags="lanczos", family="lanczos4", params={"taps": 4}),
    "spline": dict(backend="sws", flags="spline", family="spline", params={}),
    "spline16": dict(backend="zimg", flags="spline16", family="spline16", params={}),
    "spline36": dict(backend="zimg", flags="spline36", family="spline36", params={}),
}
KERNEL_ROTATION = ["bicubic", "lanczos3", "bilinear", "catmull", "lanczos2", "spline", "mitchell", "lanczos4", "spline36", "spline16"]
RATIOS = [(1, 2), (2, 3), (1, 3), (5, 12), (1, 4)]
LETTERBOX_H = 1608     # 2.39:1 active height of a 3840x2160 raster (even)
PILLARBOX_W = 2880     # 4:3 active width of a 3840x2160 raster


def even(v):
    return int(round(v / 2.0)) * 2


def fps_rational(f):
    for num, den in ((24000, 1001), (30000, 1001), (60000, 1001)):
        if abs(f - num / float(den)) < 0.005:
            return (num, den)
    return (int(round(f)), 1)


def fstr(r):
    return "%d/%d" % r if r[1] != 1 else "%d" % r[0]


def P(path):
    return path.replace("\\", "/")


def fpath(path):
    return "'" + P(path).replace(":", "\\:") + "'"


# ---------------------------------------------------------------- context
class Ctx:
    def __init__(self, args):
        self.args = args
        d = args.ffmpeg_dir
        self.ffmpeg = os.path.join(d, "ffmpeg.exe") if os.path.exists(os.path.join(d, "ffmpeg.exe")) else "ffmpeg"
        self.ffprobe = os.path.join(d, "ffprobe.exe") if os.path.exists(os.path.join(d, "ffprobe.exe")) else "ffprobe"
        self.version = ""
        self.zscale = False
        self.encoders = set()
        self.masters = {}
        self.rungs = {}
        self.reels = []
        self.out = P(args.out)
        self.rung_dir = self.out + "/rungs"
        self.work_dir = self.out + "/work"
        self.truth_dir = self.out + "/truth"
        self.gops = args.gops


def run(cmd, **kw):
    return subprocess.run(cmd, stdout=subprocess.PIPE, stderr=subprocess.PIPE, **kw)


def probe_tools(ctx):
    r = run([ctx.ffmpeg, "-hide_banner", "-version"])
    ctx.version = r.stdout.decode("utf-8", "replace").splitlines()[0] if r.returncode == 0 else "unknown"
    enc = run([ctx.ffmpeg, "-hide_banner", "-encoders"]).stdout.decode("utf-8", "replace")
    for name in ("libx264", "libx265", "libsvtav1", "libaom-av1"):
        if re.search(r"\b%s\b" % re.escape(name), enc):
            ctx.encoders.add(name)
    flt = run([ctx.ffmpeg, "-hide_banner", "-filters"]).stdout.decode("utf-8", "replace")
    ctx.zscale = bool(re.search(r"\bzscale\b", flt)) and not ctx.args.no_zscale


def ffprobe_json(ctx, path, entries):
    r = run([ctx.ffprobe, "-v", "error", "-select_streams", "v:0", "-show_entries", entries, "-of", "json", path])
    return json.loads(r.stdout.decode("utf-8", "replace") or "{}")


def load_masters(ctx):
    sel = set(ctx.args.masters.split(",")) if ctx.args.masters else None
    for mid, cls, parts, lic_override in MASTERS:
        if sel and mid not in sel:
            continue
        m = dict(id=mid, cls=cls, parts=[], synthetic=mid in SYNTHETIC, lic="", url="", skip="")
        if m["synthetic"]:
            s = SYNTHETIC[mid]
            m.update(w=s["w"], h=s["h"], fps=s["fps"], pix_fmt="yuv420p10le", eight_bit=False, size_mb=0)
            m["lic"], m["url"] = lic_override["lic"], lic_override["url"]
            ctx.masters[mid] = m
            continue
        ok = True
        for path, ss in parts:
            if not os.path.exists(path):
                m["skip"] = "missing " + path
                ok = False
                break
            info = ffprobe_json(ctx, path, "stream=width,height,avg_frame_rate,r_frame_rate,pix_fmt,duration:format=duration")
            st = (info.get("streams") or [{}])[0]
            fr = Fraction(0)
            for key in ("avg_frame_rate", "r_frame_rate"):
                try:
                    fr = Fraction(st.get(key, "0/1"))
                except (ValueError, ZeroDivisionError):
                    fr = Fraction(0)
                if fr > 0:
                    break
            try:
                dur = float(st.get("duration") or info.get("format", {}).get("duration") or 0)
            except ValueError:
                dur = 0.0
            w, h = int(st.get("width", 0)), int(st.get("height", 0))
            if w < 3840 or float(fr) <= 0:
                m["skip"] = "not a >= 3840 wide native master: %s (%dx%d)" % (path, w, h)
                ok = False
                break
            src = {}
            sj = os.path.join(os.path.dirname(path), "source.json")
            if os.path.exists(sj):
                try:
                    with open(sj, "r", encoding="utf-8") as f:
                        src = json.load(f)
                except (OSError, ValueError):
                    src = {}
            m["parts"].append(dict(path=path, ss=ss, w=w, h=h, fps=float(fr), pix_fmt=st.get("pix_fmt", ""), duration=dur,
                                   url=src.get("url", ""), lic=src.get("lic", ""), author=src.get("author", ""),
                                   size_mb=round(os.path.getsize(path) / 1048576.0, 1)))
        if not ok:
            ctx.masters[mid] = m
            continue
        p0 = m["parts"][0]
        m.update(w=p0["w"], h=p0["h"], fps=fps_rational(p0["fps"]), pix_fmt=p0["pix_fmt"], eight_bit=all(p["pix_fmt"] == "yuv420p" for p in m["parts"]))
        m["lic"] = (lic_override or {}).get("lic") or "; ".join(sorted(set(p["lic"] for p in m["parts"] if p["lic"]))) or "see D:\\testclips\\eval-2026-10-31\\CATALOG.md"
        m["url"] = (lic_override or {}).get("url") or "; ".join(p["url"] for p in m["parts"] if p["url"])
        m["size_mb"] = round(sum(p["size_mb"] for p in m["parts"]), 1)
        ctx.masters[mid] = m
    return ctx


def master_fps(m):
    return m["fps"][0] / float(m["fps"][1])


def gop_of(m):
    return int(round(2.0 * master_fps(m)))


def nframes_of(ctx, m):
    return ctx.gops * gop_of(m)


# ---------------------------------------------------------------- master timeline graph
def master_graph(ctx, m, nframes, pix, label="m"):
    """Inputs ([(path, ss_seconds|None)]) and graph text producing [label]: the master window of `nframes` native frames,
    CFR at the master rate, format `pix`. Multi-part masters are concatenated and cycled (looped) when too short."""
    fps = m["fps"]
    f = master_fps(m)
    if m["synthetic"]:
        txt = ctx.work_dir + "/credits.txt"
        dur = nframes / f + 1.0
        g = ("color=c=black:s=%dx%d:r=%s:d=%.3f,drawtext=fontfile=%s:textfile=%s:reload=0:fontsize=72:line_spacing=28:fontcolor=white:"
             "x=(w-text_w)/2:y=h-t*300,format=%s,trim=end_frame=%d,setpts=PTS-STARTPTS[%s]" % (m["w"], m["h"], fstr(fps), dur, fpath(FONT), fpath(txt), pix, nframes, label))
        return [], g, False
    uses = []
    remaining = nframes
    looped = False
    i = 0
    while remaining > 0:
        part = m["parts"][i % len(m["parts"])]
        avail = int(math.floor((part["duration"] - part["ss"]) * part["fps"])) - 1
        take = min(remaining, max(1, avail))
        uses.append((part, take))
        remaining -= take
        i += 1
        if i > 64:
            raise RuntimeError("master %s cannot fill %d frames" % (m["id"], nframes))
    looped = len(uses) > len(m["parts"]) or any(uses[k][0] is uses[j][0] for k in range(len(uses)) for j in range(k))
    inputs, lines, labels = [], [], []
    for k, (part, take) in enumerate(uses):
        ss = None
        if part["ss"] > 0:
            ss = max(0.0, part["ss"] - 0.25 / part["fps"])
        inputs.append((part["path"], ss))
        lab = label if len(uses) == 1 else "p%d" % k
        lines.append("[%d:v]fps=fps=%s:round=near,trim=start_frame=0:end_frame=%d,setpts=PTS-STARTPTS,format=%s[%s]" % (k, fstr(fps), take, pix, lab))
        labels.append(lab)
    if len(uses) > 1:
        lines.append("%sconcat=n=%d:v=1:a=0[%s]" % ("".join("[%s]" % x for x in labels), len(uses), label))
    return inputs, ";".join(lines), looped


# ---------------------------------------------------------------- rungs
class Rung:
    def __init__(self, ctx, master, kind, cw, ch, bits, codec, rate, tag, pre=None, active=None, latent=None, studio=None, preset=None):
        self.master, self.kind, self.cw, self.ch, self.bits, self.codec = master, kind, cw, ch, bits, codec
        self.rate, self.pre, self.active, self.latent, self.studio = rate, pre, active, latent, studio
        m = ctx.masters[master]
        self.fps = m["fps"]
        self.gop = gop_of(m)
        self.nframes = nframes_of(ctx, m)
        self.id = "%s.%s" % (master, tag)
        self.preset = preset or {"libx265": "fast", "libx264": "medium", "libaom-av1": "cpu8"}.get(codec, "fast")
        self.path = "%s/%s.mkv" % (ctx.rung_dir, self.id)
        self.frames_csv = "%s/%s.frames.csv" % (ctx.rung_dir, self.id)
        self.meta_path = "%s/%s.json" % (ctx.rung_dir, self.id)
        self.meta = None

    def params_hash(self, ctx):
        s = json.dumps([self.master, self.kind, self.cw, self.ch, self.bits, self.codec, self.rate, self.pre, self.active, self.latent,
                        self.studio, self.nframes, self.gop, self.preset, ctx.args.preset_override, ctx.args.threads, ctx.masters[self.master]["parts"] and [p["ss"] for p in ctx.masters[self.master]["parts"]]], sort_keys=True)
        return hashlib.sha256(s.encode()).hexdigest()[:16]

    def est_bytes(self, ctx):
        dur = self.nframes / (self.fps[0] / float(self.fps[1]))
        if self.rate[0] == "kbps":
            kbps = self.rate[1]
        else:
            bpp = 0.10 * (2.0 if "grain" in ctx.masters[self.master]["cls"] else 1.0) * (0.5 if self.rate[1] >= 28 else 1.0)
            kbps = self.cw * self.ch * (self.fps[0] / float(self.fps[1])) * bpp / 1000.0
            if self.cw * self.ch >= 3840 * 2160:
                kbps = min(kbps, 45000)
        return int(kbps * 1000 / 8 * dur)

    def truth(self, ctx):
        m = ctx.masters[self.master]
        act = self.active or dict(x=0, y=0, w=self.cw, h=self.ch)
        return dict(coded=dict(w=self.cw, h=self.ch), active_coded=act, native_coded=(self.cw == m["w"] and self.ch == m["h"]),
                    latent=self.latent, kind=self.kind, bits=self.bits, codec=self.codec, rate=list(self.rate), pre=self.pre,
                    studio=self.studio)


def tag_rate(rate):
    return "crf%d" % rate[1] if rate[0] == "crf" else "%dk" % rate[1]


def need_rung(ctx, master, kind, ratio=None, codec="libx265", bits=10, rate=("crf", 20), pre=None, extra=None):
    """Returns the Rung (created once per unique id). kind: down | lb239 | pb43 | native | studio."""
    m = ctx.masters[master]
    W, H = m["w"], m["h"]
    codec_tag = {"libx265": "x265", "libx264": "x264", "libaom-av1": "aom"}[codec]
    base = "%s_%db_%s" % (codec_tag, bits, tag_rate(rate))
    pre_tag = ("%s." % pre) if pre else ""
    active = latent = studio = None
    if kind == "native":
        cw, ch, tag = W, H, "%sn4k_%s" % (pre_tag, base)
    elif kind in ("down", "lb239", "pb43"):
        p, q = ratio
        cw, ch = even(W * p / float(q)), even(H * p / float(q))
        tag = "%s%sr%dof%d_%s" % (pre_tag, "" if kind == "down" else kind + ".", p, q, base)
        if kind == "lb239":
            ah = even(LETTERBOX_H * p / float(q))
            active = dict(x=0, y=(ch - ah) // 2, w=cw, h=ah)
        elif kind == "pb43":
            aw = even(PILLARBOX_W * p / float(q))
            active = dict(x=(cw - aw) // 2, y=0, w=aw, h=ch)
    elif kind == "studio":
        cw, ch = W, H
        kern = (extra or {}).get("kernel", "lanczos3")
        tag = "%sstudio_%s_%s" % (pre_tag, kern, base)
        latent = dict(w=even(W / 2.0), h=even(H / 2.0))
        studio = dict(kernel=kern, usm=(extra or {}).get("usm"))
    else:
        raise ValueError(kind)
    r = Rung(ctx, master, kind, cw, ch, bits, codec, rate, tag, pre=pre, active=active, latent=latent, studio=studio,
             preset=(ctx.args.preset_override or None))
    if r.id in ctx.rungs:
        return ctx.rungs[r.id]
    ctx.rungs[r.id] = r
    return r


def pre_filter(ctx, m, pre, nframes, fps):
    """Master-level content filters applied before the rung encode (they are content, not player chain)."""
    if not pre:
        return None
    if pre == "dark2":
        # two stops down in a ~gamma 2.4 domain (0.25^(1/2.4) = 0.56): luma gain about black
        return "lutyuv=y='clip((val-minval)*0.56+minval,minval,maxval)'"
    if pre == "fades":
        return "FADES"
    raise ValueError(pre)


def rung_graph(ctx, r):
    m = ctx.masters[r.master]
    pix = "yuv420p10le" if r.bits == 10 else "yuv420p"
    # 10-bit working format even for 8-bit rungs so a gain / scale has headroom; the final format= quantises
    inputs, g, looped = master_graph(ctx, m, r.nframes, "yuv420p10le", "m")
    f = master_fps(m)
    parts = [g]
    cur = "m"
    pf = pre_filter(ctx, m, r.pre, r.nframes, f)
    if pf == "FADES":
        # three fade-through-black cuts at 27%, 53%, 80% of the reel (1.5 s fades): split + trim + fade + concat
        cuts = [int(r.nframes * x) for x in (0.27, 0.53, 0.80)]
        edges = [0] + cuts + [r.nframes]
        fd = int(round(1.5 * f))
        n = len(edges) - 1
        parts.append("[m]split=%d%s" % (n, "".join("[f%d]" % i for i in range(n))))
        outs = []
        for i in range(n):
            a, b = edges[i], edges[i + 1]
            chain = "[f%d]trim=start_frame=%d:end_frame=%d,setpts=PTS-STARTPTS" % (i, a, b)
            if i > 0:
                chain += ",fade=t=in:s=0:n=%d" % fd
            if i < n - 1:
                chain += ",fade=t=out:s=%d:n=%d" % (b - a - fd, fd)
            chain += "[g%d]" % i
            parts.append(chain)
            outs.append("[g%d]" % i)
        parts.append("%sconcat=n=%d:v=1:a=0[mf]" % ("".join(outs), n))
        cur = "mf"
    elif pf:
        parts.append("[%s]%s[mp]" % (cur, pf))
        cur = "mp"
    W, H = m["w"], m["h"]
    flt = []
    if r.kind == "lb239":
        flt.append("crop=%d:%d:0:%d,pad=%d:%d:0:%d:color=black" % (W, LETTERBOX_H, (H - LETTERBOX_H) // 2, W, H, (H - LETTERBOX_H) // 2))
    elif r.kind == "pb43":
        flt.append("crop=%d:%d:%d:0,pad=%d:%d:%d:0:color=black" % (PILLARBOX_W, H, (W - PILLARBOX_W) // 2, W, H, (W - PILLARBOX_W) // 2))
    if r.kind == "studio":
        # master -> latent (Lanczos-3 down, like a mastering house) -> studio kernel up -> optional USM; ENCODED at the coded size
        flt.append(scale_filter(ctx, "lanczos3", r.latent["w"], r.latent["h"]))
        flt.append(scale_filter(ctx, r.studio["kernel"], W, H))
        if r.studio["usm"]:
            flt.append(usm_filter(r.studio["usm"]["amount"], r.studio["usm"]["radius"]))
    elif r.cw != W or r.ch != H:
        flt.append(scale_filter(ctx, "lanczos3", r.cw, r.ch))     # the studio's downscale: Lanczos-3
    flt.append("format=%s" % pix)
    parts.append("[%s]%s[r]" % (cur, ",".join(flt)))
    return inputs, ";".join(parts), looped


def scale_filter(ctx, kernel, w, h):
    k = KERNELS[kernel]
    if k["backend"] == "zimg":
        if not ctx.zscale:
            raise RuntimeError("kernel %s needs zscale" % kernel)
        return "zscale=w=%d:h=%d:filter=%s" % (w, h, k["flags"])
    s = "scale=%d:%d:flags=%s+accurate_rnd+full_chroma_int+bitexact" % (w, h, k["flags"])
    if kernel in ("lanczos2", "lanczos3", "lanczos4"):
        s += ":param0=%d" % k["params"]["taps"]
    elif k["flags"] == "bicubic":
        s += ":param0=%s:param1=%s" % (k["params"]["B"], k["params"]["C"])
    return s


def usm_filter(amount, radius):
    ms = max(3, int(round(radius * 2 + 1)) | 1)
    return "unsharp=lx=%d:ly=%d:la=%s:cx=3:cy=3:ca=0" % (ms, ms, amount)


def codec_args(ctx, r, log_path):
    T = ctx.args.threads
    gop = r.gop
    pixf = "yuv420p10le" if r.bits == 10 else "yuv420p"
    if r.codec == "libx265":
        xp = ("keyint=%d:min-keyint=%d:scenecut=0:open-gop=0:bframes=4:frame-threads=3:pools=%d:csv=%s:csv-log-level=2:log-level=error" %
              (gop, gop, T, os.path.basename(log_path)))   # relative: ffmpeg runs with cwd = the rung dir
        a = ["-c:v", "libx265", "-preset", r.preset, "-pix_fmt", pixf, "-x265-params", xp, "-tag:v", "hvc1"]
    elif r.codec == "libx264":
        a = ["-c:v", "libx264", "-preset", r.preset, "-pix_fmt", pixf, "-x264-params",
             "keyint=%d:min-keyint=%d:scenecut=0:bframes=3:threads=%d" % (gop, gop, T)]
    elif r.codec == "libaom-av1":
        a = ["-c:v", "libaom-av1", "-cpu-used", "8", "-row-mt", "1", "-tiles", "2x2", "-pix_fmt", pixf, "-g", str(gop),
             "-keyint_min", str(gop), "-lag-in-frames", "16"]
    else:
        raise ValueError(r.codec)
    if r.rate[0] == "crf":
        a += ["-crf", str(r.rate[1])]
        if r.codec == "libx265" and r.cw * r.ch >= 3840 * 2160:
            a[a.index("-x265-params") + 1] += ":vbv-maxrate=45000:vbv-bufsize=60000"
        if r.codec == "libaom-av1":
            a += ["-b:v", "0"]
    else:
        a += ["-b:v", "%dk" % r.rate[1]]
    return a


def disk_free_gb(path):
    p = path
    while p and not os.path.exists(p):
        p = os.path.dirname(p)
    return shutil.disk_usage(p or ".").free / 1073741824.0


class StopForDisk(Exception):
    pass


def check_disk(ctx, what):
    free = disk_free_gb(ctx.out)
    if free < ctx.args.min_free_gb:
        raise StopForDisk("STOP before %s: %.1f GB free on the output drive < %.1f GB floor" % (what, free, ctx.args.min_free_gb))


def rungs_total_bytes(ctx):
    n = 0
    for r in ctx.rungs.values():
        if os.path.exists(r.path):
            n += os.path.getsize(r.path)
    return n


def parse_x265_csv(path, nframes, gop):
    """x265 --csv per-frame rows (csv-log-level 2): (Encode Order, Type, POC, QP, ...). Returns {display index: qp} or None."""
    if not os.path.exists(path):
        return None
    rows = []
    hdr = None
    with open(path, "r", encoding="utf-8", errors="replace") as f:
        for line in f:
            cells = [c.strip() for c in line.rstrip(chr(10)).split(",")]
            if hdr is None:
                low = [c.lower() for c in cells]
                if "encode order" in low and "poc" in low and "qp" in low:
                    hdr = low
                continue
            if len(cells) < len(hdr):
                continue
            try:
                eo = hdr.index("encode order")
                rows.append((int(cells[eo]), cells[hdr.index("type")], int(cells[hdr.index("poc")]), float(cells[hdr.index("qp")])))
            except (ValueError, KeyError):
                continue
    if len(rows) != nframes:
        return None
    rows.sort(key=lambda r: r[0])
    pocs = sorted(r[2] for r in rows)
    if pocs == list(range(nframes)):
        return {r[2]: r[3] for r in rows}
    out, gi = {}, -1
    for r in rows:        # POC restarts at each IDR: absolute display index = GOP index x keyint + POC (fixed GOP, scenecut off)
        if r[2] == 0:
            gi += 1
        out[gi * gop + r[2]] = r[3]
    return out if sorted(out) == list(range(nframes)) else None


def ffprobe_frame_types(ctx, path, nframes):
    r = run([ctx.ffprobe, "-v", "error", "-select_streams", "v:0", "-show_entries", "frame=pict_type", "-of", "csv=p=0", path])
    types = [ln.strip().strip(",") for ln in r.stdout.decode("utf-8", "replace").splitlines() if ln.strip()]
    return types


def write_frames_csv(path, types, qps):
    with open(path, "w", encoding="utf-8", newline="\n") as f:
        f.write("frame,pict_type,qp\n")
        for i, t in enumerate(types):
            f.write("%d,%s,%s\n" % (i, t, ("%.2f" % qps[i]) if qps and i in qps else "-1"))


def encode_rung(ctx, r):
    meta = None
    if os.path.exists(r.path) and os.path.exists(r.meta_path):
        try:
            with open(r.meta_path, "r", encoding="utf-8") as f:
                meta = json.load(f)
        except (OSError, ValueError):
            meta = None
    if meta and meta.get("params_hash") == r.params_hash(ctx) and os.path.exists(r.frames_csv):
        r.meta = meta
        return "kept"
    check_disk(ctx, "rung " + r.id)
    est = r.est_bytes(ctx)
    if rungs_total_bytes(ctx) + est > ctx.args.rung_budget_gb * 1073741824:
        raise StopForDisk("STOP before rung %s: rung files would exceed the %.1f GB budget" % (r.id, ctx.args.rung_budget_gb))
    os.makedirs(ctx.rung_dir, exist_ok=True)
    inputs, graph, looped = rung_graph(ctx, r)
    tmp = r.path + ".tmp.mkv"
    csv_log = r.path + ".x265.csv"      # basename goes to x265 (cwd = rung dir)
    if os.path.exists(csv_log):
        os.remove(csv_log)
    cmd = [ctx.ffmpeg, "-y", "-hide_banner", "-v", "error", "-filter_threads", str(ctx.args.threads)]
    for path, ss in inputs:
        cmd += ["-threads", str(ctx.args.threads)]
        if ss:
            cmd += ["-ss", "%.6f" % ss]
        cmd += ["-i", path]
    cmd += ["-filter_complex", graph, "-map", "[r]", "-an", "-sn", "-frames:v", str(r.nframes)] + codec_args(ctx, r, csv_log) + [tmp]
    print("  encode %s (%s, est %.0f MB)" % (r.id, r.codec, est / 1048576.0), flush=True)
    res = run(cmd, cwd=ctx.rung_dir)
    if res.returncode != 0 or not os.path.exists(tmp):
        raise RuntimeError("ffmpeg failed for %s:\n%s\n%s" % (r.id, " ".join(cmd[:12]) + " ...", res.stderr.decode("utf-8", "replace")[-1500:]))
    os.replace(tmp, r.path)
    types = ffprobe_frame_types(ctx, r.path, r.nframes)
    qps = parse_x265_csv(csv_log, r.nframes, r.gop) if r.codec == "libx265" else None
    if len(types) != r.nframes:
        print("  WARNING %s: ffprobe saw %d frames, planned %d" % (r.id, len(types), r.nframes))
    write_frames_csv(r.frames_csv, types, qps)
    if os.path.exists(csv_log):
        os.remove(csv_log)        # the per-frame QP is kept in frames.csv; the raw log is not a published asset
    size = os.path.getsize(r.path)
    dur = r.nframes / (r.fps[0] / float(r.fps[1]))
    meta = dict(params_hash=r.params_hash(ctx), size_bytes=size, kbps=round(size * 8 / dur / 1000.0, 1), frames=len(types), planned_frames=r.nframes,
                qp_source=("x265_csv" if qps else "none"), looped=looped, idr_frames=[i for i, t in enumerate(types) if t == "I"],
                encoder=r.codec, preset=r.preset)
    with open(r.meta_path, "w", encoding="utf-8", newline="\n") as f:
        json.dump(meta, f, indent=1)
    r.meta = meta
    return "encoded"


def load_rung_meta(r):
    if r.meta is None and os.path.exists(r.meta_path):
        with open(r.meta_path, "r", encoding="utf-8") as f:
            r.meta = json.load(f)
    return r.meta


def read_frames_csv(path):
    out = []
    if not os.path.exists(path):
        return out
    with open(path, "r", encoding="utf-8") as f:
        rd = csv.reader(f)
        next(rd, None)
        for row in rd:
            out.append((row[1], float(row[2])))
    return out


def master_frames(ctx, m, a, n):
    """pict_type per frame of an as-is master window (ffprobe on the interval); QP unknown (-1)."""
    part = m["parts"][0]
    cache = "%s/_master_%s.frames.csv" % (ctx.rung_dir, m["id"])
    if os.path.exists(cache):
        return read_frames_csv(cache)
    f = master_fps(m)
    ss = max(0.0, part["ss"] - 0.25 / part["fps"])
    nf = nframes_of(ctx, m)
    r = run([ctx.ffprobe, "-v", "error", "-select_streams", "v:0", "-read_intervals", "%.6f%%+%.3f" % (ss, nf / f + 1.0),
             "-show_entries", "frame=pict_type,best_effort_timestamp_time", "-of", "csv=p=0", part["path"]])
    rows = []
    for ln in r.stdout.decode("utf-8", "replace").splitlines():
        c = ln.strip().split(",")
        try:
            rows.append((float(c[0]), c[1] if len(c) > 1 else "?"))
        except ValueError:
            continue
    rows.sort()
    rows = [x for x in rows if x[0] >= ss - 1e-6][:nf]
    os.makedirs(ctx.rung_dir, exist_ok=True)
    with open(cache, "w", encoding="utf-8", newline="\n") as fh:
        fh.write("frame,pict_type,qp\n")
        for i, (_, t) in enumerate(rows):
            fh.write("%d,%s,-1\n" % (i, t))
    return [(t, -1.0) for _, t in rows]


# ---------------------------------------------------------------- reel model
def seg_stream(src, a, b, chain=None, frc=None, overlays=None, fades=None, note=""):
    return dict(kind="stream", src=src, a=a, b=b, chain=chain or {}, frc=frc, overlays=overlays or [], fades=fades or {}, note=note)


def seg_gen(kind, frames, bits=8, note=""):
    return dict(kind=kind, src=None, a=0, b=frames, chain={"bits": bits}, frc=None, overlays=[], fades={}, note=note)


def stage(w, h, kernel):
    return dict(w=w, h=h, kernel=kernel)


def full_chain(ctx, m, rung, kernel, bits=10, dither="none", usm=None, window=None, stages=None):
    """The ordinary player chain: rung -> one scale to the canvas (or to a window) -> optional USM -> quantise."""
    W, H = m["w"], m["h"]
    if stages is None:
        fw, fh = (window["w"], window["h"]) if window else (W, H)
        stages = [stage(fw, fh, kernel)]
    c = dict(stages=stages, bits=bits, dither=dither)
    if usm:
        c["usm"] = dict(amount=usm[0], radius=usm[1])
    if window:
        c["window"] = window
    return c


class Reel:
    def __init__(self, rid, family, title, master, segs, fps_out=None, note="", flags=None):
        self.id, self.family, self.title, self.master, self.segs = rid, family, title, master, segs
        self.fps_out, self.note, self.flags = fps_out, note, flags or []


def add_reel(ctx, reel):
    if any(r.id == reel.id for r in ctx.reels):
        return
    ctx.reels.append(reel)


# ---------------------------------------------------------------- family builders
def avail(ctx, mid, need_8bit=False, rungs_only=False):
    m = ctx.masters.get(mid)
    if not m or m.get("skip"):
        return None
    if need_8bit and not m["eight_bit"]:
        return None
    return m


def gp(ctx, m, frac):
    """A GOP-aligned frame position at `frac` of the reel (at least one GOP in)."""
    return max(1, int(round(ctx.gops * frac))) * gop_of(m)


def build_r1(ctx):
    n = lambda m: nframes_of(ctx, m)
    for mid in ("film_dragon", "film_married", "nat30_nikon", "nat30_sequim", "sport60_k808", "sport30_torschuss", "nat24_steeple", "dark60_meridian", "nat25_funchal"):
        m = avail(ctx, mid, need_8bit=True)
        if not m:
            continue
        add_reel(ctx, Reel("R1.%s.asis" % mid, "R1", "native 4K master as-is (%s)" % m["cls"], mid,
                           [seg_stream(("master", mid), 0, n(m), {"bits": 8, "native": True}, note="master window as-is")]))
    variants = [("libx265", 8, ("kbps", 3000)), ("libx265", 10, ("kbps", 8000)), ("libx265", 10, ("kbps", 20000)), ("libx264", 8, ("kbps", 8000))]
    for mid in ("film_dragon", "nat30_nikon", "sport60_k808", "nat24_steeple", "dark60_meridian"):
        m = avail(ctx, mid)
        if not m:
            continue
        for codec, bits, rate in variants:
            if codec not in ctx.encoders:
                continue
            r = need_rung(ctx, mid, "native", codec=codec, bits=bits, rate=rate)
            add_reel(ctx, Reel("R1.%s.%s" % (mid, r.id.split(".", 1)[1]), "R1", "native 4K stream encode %s %d-bit %s" % (codec, bits, tag_rate(rate)), mid,
                               [seg_stream(("rung", r.id), 0, n(m), {"bits": bits, "native": True})]))
    m = avail(ctx, "nat30_nikon")
    av1 = "libsvtav1" if "libsvtav1" in ctx.encoders else ("libaom-av1" if "libaom-av1" in ctx.encoders else None)
    if m and av1 == "libaom-av1" and ctx.args.av1:      # opt-in: libaom at 4K is far too slow for the default run (no SVT-AV1 here)
        r = need_rung(ctx, "nat30_nikon", "native", codec="libaom-av1", bits=10, rate=("kbps", 6000))
        add_reel(ctx, Reel("R1.nat30_nikon.%s" % r.id.split(".", 1)[1], "R1", "native 4K stream encode AV1 (libaom) 10-bit 6000k", "nat30_nikon",
                           [seg_stream(("rung", r.id), 0, n(m), {"bits": 10, "native": True})]))


def clean_chain(ctx, m, rung, kernel, **kw):
    return full_chain(ctx, m, rung, kernel, **kw)


def build_r2(ctx):
    masters = ["film_dragon", "nat30_nikon", "sport60_k808", "nat24_steeple", "dark60_meridian", "nat25_funchal", "sport30_torschuss"]
    for mi, mid in enumerate(masters):
        m = avail(ctx, mid)
        if not m:
            continue
        nf = nframes_of(ctx, m)
        for ri, ratio in enumerate(RATIOS):
            kernel = KERNEL_ROTATION[(mi * 5 + ri) % len(KERNEL_ROTATION)]
            if KERNELS[kernel]["backend"] == "zimg" and not ctx.zscale:
                kernel = "lanczos3"
            r = need_rung(ctx, mid, "down", ratio)
            add_reel(ctx, Reel("R2.clean.%s.r%dof%d.%s" % (mid, ratio[0], ratio[1], kernel), "R2", "clean 10-bit player upscale %d/%d, %s" % (ratio[0], ratio[1], kernel), mid,
                               [seg_stream(("rung", r.id), 0, nf, clean_chain(ctx, m, r, kernel))]))
    m = avail(ctx, "nat30_nikon")
    if m:                      # kernel sweep at 1/2
        r = need_rung(ctx, "nat30_nikon", "down", (1, 2))
        for kernel in KERNEL_ROTATION:
            if KERNELS[kernel]["backend"] == "zimg" and not ctx.zscale:
                continue
            add_reel(ctx, Reel("R2.clean.nat30_nikon.r1of2.%s" % kernel, "R2", "kernel sweep 1/2 %s" % kernel, "nat30_nikon",
                               [seg_stream(("rung", r.id), 0, nframes_of(ctx, m), clean_chain(ctx, m, r, kernel))]))
    # dirty chains: grain / USM / 8-bit + dither / low-rate rung
    for mid in ("film_dragon", "film_married", "nat30_nikon", "dark60_meridian"):
        m = avail(ctx, mid)
        if not m:
            continue
        nf = nframes_of(ctx, m)
        for ratio in ((1, 2), (2, 3), (1, 3)):
            r = need_rung(ctx, mid, "down", ratio)
            tag = "%s.r%dof%d" % (mid, ratio[0], ratio[1])
            add_reel(ctx, Reel("R2.dirty.%s.lanczos3_usm0.6_8b_bayer" % tag, "R2", "dirty: lanczos3 + USM 0.6 r1.5 + 8-bit ordered dither", mid,
                               [seg_stream(("rung", r.id), 0, nf, full_chain(ctx, m, r, "lanczos3", bits=8, dither="bayer", usm=(0.6, 1.5)))]))
            add_reel(ctx, Reel("R2.dirty.%s.bicubic_usm1.0_10b" % tag, "R2", "dirty: bicubic + USM 1.0 r1.0 10-bit", mid,
                               [seg_stream(("rung", r.id), 0, nf, full_chain(ctx, m, r, "bicubic", bits=10, usm=(1.0, 1.0)))]))
        r = need_rung(ctx, mid, "down", (1, 2), rate=("crf", 28))
        add_reel(ctx, Reel("R2.dirty.%s.r1of2_crf28.bilinear_8b_noise" % mid, "R2", "dirty: crf 28 rung + bilinear + 8-bit noise dither", mid,
                           [seg_stream(("rung", r.id), 0, nf, full_chain(ctx, m, r, "bilinear", bits=8, dither="noise"))]))
    m = avail(ctx, "nat30_nikon")
    if m:                      # USM sweep
        r = need_rung(ctx, "nat30_nikon", "down", (1, 2))
        for amt in (0.3, 0.6, 1.0):
            add_reel(ctx, Reel("R2.usm.nat30_nikon.r1of2.lanczos3_usm%s" % amt, "R2", "USM sweep %s" % amt, "nat30_nikon",
                               [seg_stream(("rung", r.id), 0, nframes_of(ctx, m), full_chain(ctx, m, r, "lanczos3", usm=(amt, 1.5)))]))


def build_r3(ctx):
    for mid in ("nat30_nikon", "nat24_steeple", "film_dragon"):
        m = avail(ctx, mid)
        if not m:
            continue
        W, H = m["w"], m["h"]
        nf = nframes_of(ctx, m)
        r3 = need_rung(ctx, mid, "down", (1, 3))
        add_reel(ctx, Reel("R3.double.%s.720_1080_2160" % mid, "R3", "double scale 720 -> 1080 (bilinear) -> 2160 (lanczos3)", mid,
                           [seg_stream(("rung", r3.id), 0, nf, full_chain(ctx, m, r3, None, stages=[stage(even(W / 2.0), even(H / 2.0), "bilinear"), stage(W, H, "lanczos3")]))]))
        r4 = need_rung(ctx, mid, "down", (1, 4))
        add_reel(ctx, Reel("R3.double.%s.540_1080_2160" % mid, "R3", "double scale 540 -> 1080 (bicubic) -> 2160 (bilinear)", mid,
                           [seg_stream(("rung", r4.id), 0, nf, full_chain(ctx, m, r4, None, stages=[stage(even(W / 2.0), even(H / 2.0), "bicubic"), stage(W, H, "bilinear")]))]))


def build_r4(ctx):
    def chain(m, r, kernel, window=None):
        return full_chain(ctx, m, r, kernel, window=window)
    for mid in ("nat30_nikon", "nat24_steeple", "dark60_meridian"):
        m = avail(ctx, mid)
        if not m:
            continue
        nf = nframes_of(ctx, m)
        r = need_rung(ctx, mid, "lb239", (1, 2))
        add_reel(ctx, Reel("R4.letterbox239.%s.r1of2.lanczos3" % mid, "R4", "letterbox 2.39 rung (bars coded) 1/2 -> 2160", mid,
                           [seg_stream(("rung", r.id), 0, nf, chain(m, r, "lanczos3"))]))
    for mid in ("film_dragon", "nat24_steeple"):
        m = avail(ctx, mid)
        if not m:
            continue
        nf = nframes_of(ctx, m)
        r = need_rung(ctx, mid, "pb43", (1, 2))
        add_reel(ctx, Reel("R4.pillarbox43.%s.r1of2.bicubic" % mid, "R4", "pillarbox 4:3 rung (bars coded) 1/2 -> 2160", mid,
                           [seg_stream(("rung", r.id), 0, nf, chain(m, r, "bicubic"))]))
    for mid in ("nat30_nikon", "nat24_steeple"):
        m = avail(ctx, mid)
        if not m:
            continue
        W, H = m["w"], m["h"]
        nf = nframes_of(ctx, m)
        r = need_rung(ctx, mid, "down", (1, 2))
        w1 = dict(x=(W - 1920) // 2, y=(H - 1080) // 2, w=1920, h=1080, color="gray")
        add_reel(ctx, Reel("R4.window1080_1to1.%s" % mid, "R4", "windowed video 1920x1080 1:1 on a gray 4K desktop", mid,
                           [seg_stream(("rung", r.id), 0, nf, dict(stages=[], window=w1, bits=10, native_window=True))]))
        w2 = dict(x=(W - 2560) // 2, y=(H - 1440) // 2, w=2560, h=1440, color="gray")
        add_reel(ctx, Reel("R4.window1440_from1080.%s.lanczos3" % mid, "R4", "windowed video 1080p -> 2560x1440 window", mid,
                           [seg_stream(("rung", r.id), 0, nf, full_chain(ctx, m, r, "lanczos3", window=w2))]))
    for mid, order in (("nat30_nikon", ("full", "lb", "full")), ("nat24_steeple", ("full", "pb", "lb"))):
        m = avail(ctx, mid)
        if not m:
            continue
        nf = nframes_of(ctx, m)
        g = gop_of(m)
        edges = [0, gp(ctx, m, 5 / 15.0), gp(ctx, m, 10 / 15.0), nf]
        segs = []
        for i, kind in enumerate(order):
            rr = need_rung(ctx, mid, {"full": "down", "lb": "lb239", "pb": "pb43"}[kind], (1, 2))
            segs.append(seg_stream(("rung", rr.id), edges[i], edges[i + 1], chain(m, rr, "lanczos3"), note="toggle: %s" % kind))
        add_reel(ctx, Reel("R4.toggle.%s.%s" % (mid, "_".join(order)), "R4", "mid-reel aspect/zoom toggles %s" % "/".join(order), mid, segs))


def build_r5(ctx):
    for mid in ("nat30_nikon", "nat24_steeple", "film_dragon"):
        m = avail(ctx, mid)
        if not m:
            continue
        nf = nframes_of(ctx, m)
        for rate in (("crf", 20), ("kbps", 6000)):
            r = need_rung(ctx, mid, "studio", codec="libx265", bits=10, rate=rate, extra=dict(kernel="lanczos3", usm=dict(amount=0.4, radius=1.0)))
            add_reel(ctx, Reel("R5.studio.%s.%s" % (mid, tag_rate(rate)), "R5", "studio 1080 -> 2160 encoded at 2160 (%s)" % tag_rate(rate), mid,
                               [seg_stream(("rung", r.id), 0, nf, {"bits": 10, "native": True})]))


def build_r6(ctx):
    for mid in ("nat30_nikon", "dark60_meridian", "sport60_k808"):
        m = avail(ctx, mid)
        if not m:
            continue
        nf = nframes_of(ctx, m)
        g = gop_of(m)
        r12 = need_rung(ctx, mid, "down", (1, 2))
        r14 = need_rung(ctx, mid, "down", (1, 4))
        r23 = need_rung(ctx, mid, "down", (2, 3))
        r12lo = need_rung(ctx, mid, "down", (1, 2), rate=("crf", 36))

        def S(r, a, b):
            return seg_stream(("rung", r.id), a, b, full_chain(ctx, m, r, "bicubic"))
        b = gp(ctx, m, 7 / 15.0)
        d1, d2 = gp(ctx, m, 5 / 15.0), gp(ctx, m, 9 / 15.0)
        a1, a2 = gp(ctx, m, 6 / 15.0), gp(ctx, m, 12 / 15.0)
        add_reel(ctx, Reel("R6.%s.down_res_1of2_to_1of4" % mid, "R6", "ABR down: 1/2 -> 1/4 at a GOP boundary", mid, [S(r12, 0, b), S(r14, b, nf)]))
        add_reel(ctx, Reel("R6.%s.up_res_1of4_to_1of2" % mid, "R6", "ABR up: 1/4 -> 1/2", mid, [S(r14, 0, b), S(r12, b, nf)]))
        add_reel(ctx, Reel("R6.%s.down_q_1of2_to_lo" % mid, "R6", "ABR quality-only down: crf 20 -> crf 36 at 1/2", mid, [S(r12, 0, b), S(r12lo, b, nf)]))
        add_reel(ctx, Reel("R6.%s.up_q_lo_to_1of2" % mid, "R6", "ABR quality-only up", mid, [S(r12lo, 0, b), S(r12, b, nf)]))
        add_reel(ctx, Reel("R6.%s.dip_1of2_1of4_1of2" % mid, "R6", "ABR dip: 1/2 -> 1/4 (4 GOPs) -> 1/2", mid, [S(r12, 0, d1), S(r14, d1, d2), S(r12, d2, nf)]))
        add_reel(ctx, Reel("R6.%s.alt_2of3_1of4_6gop" % mid, "R6", "ABR alternation 2/3 <-> 1/4 every 6 GOPs (~12 s)", mid,
                           [S(r23, 0, a1), S(r14, a1, a2), S(r23, a2, nf)]))


SUBS = ["THE QUICK BROWN FOX JUMPS OVER THE LAZY DOG", "SIGNAL IS NOT THE SAME AS NOISE", "PACK MY BOX WITH FIVE DOZEN LIQUOR JUGS"]


def sub_overlays(fps):
    out = []
    for i, text in enumerate(SUBS):
        out.append(dict(type="drawtext", text=text, size=84, x="(w-text_w)/2", y="h-260", start=4 + i * 8.0, end=8 + i * 8.0))
    return out


def build_r7(ctx):
    def S(m, r, kernel="lanczos3", **kw):
        return full_chain(ctx, m, r, kernel, **kw)
    for mid in ("nat30_nikon", "nat24_steeple", "dark60_meridian"):
        m = avail(ctx, mid)
        if not m:
            continue
        nf = nframes_of(ctx, m)
        W, H = m["w"], m["h"]
        r = need_rung(ctx, mid, "down", (1, 2))
        add_reel(ctx, Reel("R7.subtitles.%s.r1of2" % mid, "R7", "burned subtitles at output resolution over a 1/2 upscale", mid,
                           [seg_stream(("rung", r.id), 0, nf, S(m, r), overlays=sub_overlays(master_fps(m)))]))
        for label, pw, ph in (("small", 640, 360), ("large", 1280, 720)):
            if label == "large" and mid == "dark60_meridian":
                continue
            ov = [dict(type="pip", master=mid, a=int(round(10 * master_fps(m))), w=pw, h=ph, x=W - pw - 160, y=140, start=6.0, end=24.0)]
            add_reel(ctx, Reel("R7.pip_%s.%s.r1of2" % (label, mid), "R7", "native-grid PiP %dx%d over a 1/2 upscale" % (pw, ph), mid,
                               [seg_stream(("rung", r.id), 0, nf, S(m, r), overlays=ov)], flags=["hold_" + ("expected" if label == "large" else "masked")]))
    for mid in ("nat30_nikon", "nat24_steeple"):
        m = avail(ctx, mid)
        if not m:
            continue
        nf = nframes_of(ctx, m)
        f = master_fps(m)
        r = need_rung(ctx, mid, "down", (1, 2))
        chain = S(m, r)
        g = gop_of(m)
        for label, gap_ms, fade in (("cut", 0, 0), ("black250", 250, 0), ("black500", 500, 0), ("fade", 0, 1)):
            if mid != "nat30_nikon" and label != "black250":
                continue
            gap = int(round(gap_ms / 1000.0 * f))
            fd = int(round(f)) if fade else 0
            n1, nm = gp(ctx, m, 4 / 15.0), gp(ctx, m, 3 / 15.0)
            segs = [seg_stream(("rung", r.id), 0, n1, chain, fades=dict(out=fd) if fade else {})]
            if gap:
                segs.append(seg_gen("black", gap))
            segs.append(seg_gen("menu", nm, bits=8, note="synthetic 4K UI"))
            if gap:
                segs.append(seg_gen("black", gap))
            tail = nf - n1 - nm - 2 * gap
            segs.append(seg_stream(("rung", r.id), n1, n1 + tail, chain, fades=dict(in_=fd) if fade else {}))
            add_reel(ctx, Reel("R7.menu_%s.%s" % (label, mid), "R7", "stream -> synthetic 4K menu -> stream, %s transition" % label, mid, segs))


def build_r8(ctx):
    def S(m, r, kernel="lanczos3"):
        return full_chain(ctx, m, r, kernel)
    for mid in ("nat30_nikon", "nat24_steeple"):
        m = avail(ctx, mid)
        if not m:
            continue
        nf = nframes_of(ctx, m)
        rn = need_rung(ctx, mid, "native", pre="dark2", rate=("kbps", 12000))
        r2 = need_rung(ctx, mid, "down", (1, 2), pre="dark2")
        add_reel(ctx, Reel("R8.dark2.%s.native" % mid, "R8", "two stops darker, native 4K encode", mid, [seg_stream(("rung", rn.id), 0, nf, {"bits": 10, "native": True})]))
        add_reel(ctx, Reel("R8.dark2.%s.r1of2" % mid, "R8", "two stops darker, 1/2 upscale", mid, [seg_stream(("rung", r2.id), 0, nf, S(m, r2))]))
    for mid, tagp, label, pre in (("flat_sol", "flat", "flat CG / anime", None), ("synth_credits", "credits", "credits scroll", None), ("grain60_nocturne", "grain60", "HDR-origin film grain 60p", None),
                                  ("nat30_nikon", "fades", "fades through black", "fades")):
        m = avail(ctx, mid)
        if not m:
            continue
        nf = nframes_of(ctx, m)
        rn = need_rung(ctx, mid, "native", pre=pre, rate=("kbps", 12000))
        r2 = need_rung(ctx, mid, "down", (1, 2), pre=pre)
        add_reel(ctx, Reel("R8.%s.%s.native" % (tagp, mid), "R8", label + ", native 4K encode", mid, [seg_stream(("rung", rn.id), 0, nf, {"bits": 10, "native": True})]))
        add_reel(ctx, Reel("R8.%s.%s.r1of2" % (tagp, mid), "R8", label + ", 1/2 upscale", mid, [seg_stream(("rung", r2.id), 0, nf, S(m, r2))]))


def build_r9(ctx):
    cases = [("nat24_steeple", (60000, 1001), "32", "native"), ("nat24_steeple", (60000, 1001), "32", "r1of2"), ("nat24_steeple", (60000, 1001), "blend", "r1of2"),
             ("film_dragon", (60, 1), "32", "r1of2"), ("sport30_torschuss", (60000, 1001), "32", "r1of2"), ("nat25_funchal", (50, 1), "32", "r1of2")]
    for mid, out_fps, mode, kind in cases:
        m = avail(ctx, mid, need_8bit=(kind == "native"))
        if not m:
            continue
        nf = nframes_of(ctx, m)
        frc = dict(mode=mode, out=list(out_fps))
        if kind == "native":
            seg = seg_stream(("master", mid), 0, nf, {"bits": 8, "native": True}, frc=frc)
        else:
            r = need_rung(ctx, mid, "down", (1, 2))
            seg = seg_stream(("rung", r.id), 0, nf, full_chain(ctx, m, r, "lanczos3"), frc=frc)
        add_reel(ctx, Reel("R9.%s.%s.%s_%s" % (mid, kind, "pulldown" if mode == "32" else "blended", fstr(out_fps).replace("/", "_")), "R9",
                           "%s %s -> %s" % ("3:2 / repeat FRC" if mode == "32" else "blended FRC", kind, fstr(out_fps)), mid, [seg], fps_out=out_fps))


FAMILIES = {"R1": build_r1, "R2": build_r2, "R3": build_r3, "R4": build_r4, "R5": build_r5, "R6": build_r6, "R7": build_r7, "R8": build_r8, "R9": build_r9}


# ---------------------------------------------------------------- reel compilation
def frc_map(in_fps, out_fps, mode, n_in):
    """(src_frame, repeat, blended) per output frame. mode "32": ffmpeg fps (round=near): input i is shown from output frame
    round-half-up(i * out/in) (verified against the real filter output: 3,2,3,2 from frame 0 at 24 -> 60); mode "blend": the
    framerate filter, source = the earlier input frame, flagged blended when the output instant is off the input grid."""
    r = Fraction(out_fps[0], out_fps[1]) / Fraction(in_fps[0], in_fps[1])
    n_out = int(round(n_in * r))
    rows = []
    prev = None
    cur = 0
    for j in range(n_out):
        if mode == "32":
            while cur + 1 < n_in and int(math.floor((cur + 1) * r + Fraction(1, 2))) <= j:
                cur += 1
            src, blended = cur, 0
        else:
            x = Fraction(j) / r
            src = min(int(math.floor(x)), n_in - 1)
            frac = float(x - src)
            blended = 1 if 0.06 < frac < 0.94 else 0
        rows.append((src, 1 if prev == src else 0, blended))
        prev = src
    return rows


def seg_out_frames(seg, in_fps):
    n = seg["b"] - seg["a"]
    if seg.get("frc"):
        return len(frc_map(in_fps, tuple(seg["frc"]["out"]), seg["frc"]["mode"], n))
    return n


def quantise(bits, dither):
    """Player output stage: round to 8 or 10 bits with the chosen dither."""
    if bits == 8:
        out = []
        if dither == "noise":
            out.append("noise=c0s=2:c0f=u+t:c0_seed=%d" % (SEED % 100000))
        sws = {"none": "none", "bayer": "bayer", "noise": "none"}[dither]
        out.append("scale=iw:ih:flags=accurate_rnd+bitexact:sws_dither=%s" % sws)
        out.append("format=yuv420p")
        return out
    return ["format=yuv420p10le"]


def overlay_filters(ctx, m, overlays, fps, add_input):
    """Returns (pre-graph lines, per-frame filter pieces, pip list). drawtext/drawbox pieces are applied inline;
    PiP needs a second input and is returned as (label, spec)."""
    pieces, pips = [], []
    for ov in overlays:
        if ov["type"] == "drawtext":
            pieces.append("drawtext=fontfile=%s:text='%s':fontsize=%d:fontcolor=white:borderw=4:bordercolor=black:x=%s:y=%s:enable='between(t,%s,%s)'" %
                          (fpath(FONT), ov["text"], ov["size"], ov["x"], ov["y"], ov["start"], ov["end"]))
        elif ov["type"] == "drawbox":
            pieces.append("drawbox=x=%d:y=%d:w=%d:h=%d:color=%s:t=fill" % (ov["x"], ov["y"], ov["w"], ov["h"], ov["color"]))
        elif ov["type"] == "pip":
            pips.append(ov)
    return pieces, pips


def menu_graph(m, frames, fps, label):
    W, H = m["w"], m["h"]
    boxes = []
    for i in range(4):
        for j in range(3):
            x, y = 240 + i * 880, 600 + j * 480
            boxes.append("drawbox=x=%d:y=%d:w=760:h=380:color=0x2f4a6b:t=fill" % (x, y))
            boxes.append("drawbox=x=%d:y=%d:w=760:h=380:color=white:t=6" % (x, y))
            boxes.append("drawtext=fontfile=%s:text='TITLE %d':fontsize=64:fontcolor=white:x=%d:y=%d" % (fpath(FONT), i * 3 + j + 1, x + 40, y + 300))
    hdr = "drawtext=fontfile=%s:text='HOME  MOVIES  SHOWS  SETTINGS':fontsize=96:fontcolor=white:x=240:y=240" % fpath(FONT)
    moving = "drawbox=x='240+mod(t*320,3000)':y=520:w=200:h=12:color=0xd0a020:t=fill"
    return ("color=c=0x16202c:s=%dx%d:r=%s:d=%.4f,%s,%s,%s,format=yuv420p,trim=end_frame=%d,setpts=PTS-STARTPTS[%s]" %
            (W, H, fstr(fps), frames / (fps[0] / float(fps[1])) + 1.0, hdr, ",".join(boxes), moving, frames, label))


def compile_reel(ctx, reel):
    m = ctx.masters[reel.master]
    in_fps = m["fps"]
    out_fps = tuple(reel.fps_out) if reel.fps_out else in_fps
    W, H = m["w"], m["h"]
    inputs = []          # [{path, ss}]
    lines = []
    labels = []
    segs_out = []
    out_start = 0

    def add_input(path, ss=None):
        inputs.append(dict(path=P(path), ss=("%.6f" % ss) if ss else None))
        return len(inputs) - 1

    for si, seg in enumerate(reel.segs):
        lab = "s%d" % si
        n_in = seg["b"] - seg["a"]
        kind = seg["kind"]
        chain = seg["chain"]
        bits = chain.get("bits", 10)
        if kind in ("menu", "black"):
            if kind == "menu":
                lines.append(menu_graph(m, n_in, out_fps, "x%d" % si))
            else:
                lines.append("color=c=black:s=%dx%d:r=%s:d=%.4f,format=yuv420p,trim=end_frame=%d,setpts=PTS-STARTPTS[x%d]" %
                             (W, H, fstr(out_fps), n_in / (out_fps[0] / float(out_fps[1])) + 1.0, n_in, si))
            cur = "x%d" % si
            n_out = n_in
        else:
            kind_src, sid = seg["src"]
            if kind_src == "rung":
                r = ctx.rungs[sid]
                idx = add_input(r.path)
                head = "[%d:v]fps=fps=%s:round=near,trim=start_frame=%d:end_frame=%d,setpts=PTS-STARTPTS" % (idx, fstr(in_fps), seg["a"], seg["b"])
            else:
                part = m["parts"][0]
                ss = max(0.0, part["ss"] + seg["a"] / master_fps(m) - 0.25 / master_fps(m))
                idx = add_input(part["path"], ss)
                head = "[%d:v]fps=fps=%s:round=near,trim=start_frame=0:end_frame=%d,setpts=PTS-STARTPTS" % (idx, fstr(in_fps), n_in)
            flt = [head]
            for st in chain.get("stages", []):
                flt.append(scale_filter(ctx, st["kernel"], st["w"], st["h"]))
            if chain.get("usm"):
                flt.append(usm_filter(chain["usm"]["amount"], chain["usm"]["radius"]))
            win = chain.get("window")
            if win:
                flt.append("pad=%d:%d:%d:%d:color=%s" % (W, H, win["x"], win["y"], win.get("color", "black")))
            pieces, pips = overlay_filters(ctx, m, seg["overlays"], out_fps, add_input)
            cur = "x%d" % si
            fades_f = []
            fd = seg["fades"]
            if fd.get("out"):
                fades_f.append("fade=t=out:s=%d:n=%d" % (n_in - fd["out"], fd["out"]))
            if fd.get("in_"):
                fades_f.append("fade=t=in:s=0:n=%d" % fd["in_"])
            quant = [] if chain.get("native") else quantise(bits, chain.get("dither", "none"))
            if pips:
                lines.append(",".join(flt) + "[b%d]" % si)
                base = "b%d" % si
                for pi, pip in enumerate(pips):
                    part = m["parts"][0]
                    ss = max(0.0, part["ss"] + pip["a"] / master_fps(m) - 0.25 / master_fps(m))
                    pidx = add_input(part["path"], ss)
                    lines.append("[%d:v]fps=fps=%s:round=near,trim=start_frame=0:end_frame=%d,setpts=PTS-STARTPTS,scale=%d:%d:flags=lanczos+accurate_rnd+bitexact,format=yuv420p10le[pip%d_%d]" %
                                 (pidx, fstr(in_fps), n_in, pip["w"], pip["h"], si, pi))
                    nxt = "o%d_%d" % (si, pi)
                    lines.append("[%s][pip%d_%d]overlay=x=%d:y=%d:enable='between(t,%s,%s)':format=yuv420p10[%s]" % (base, si, pi, pip["x"], pip["y"], pip["start"], pip["end"], nxt))
                    base = nxt
                tail = pieces + fades_f + quant
                lines.append("[%s]%s[%s]" % (base, ",".join(tail) if tail else "null", cur))
            else:
                lines.append(",".join(flt + pieces + fades_f + quant) + "[%s]" % cur)
            n_out = n_in
            if seg.get("frc"):
                fo = tuple(seg["frc"]["out"])
                n_out = len(frc_map(in_fps, fo, seg["frc"]["mode"], n_in))
                if seg["frc"]["mode"] == "32":
                    fr = "fps=fps=%s:round=near" % fstr(fo)
                else:
                    # framerate drops the last frame: clone-pad then cut to the exact planned count
                    fr = "framerate=fps=%s,tpad=stop=4:stop_mode=clone,trim=end_frame=%d" % (fstr(fo), n_out)
                lines.append("[%s]%s[%sf]" % (cur, fr, cur))
                cur = cur + "f"
        # luma only, exact codes (no range scaling), 16-bit container; the harness divides back by 2^(16-bits)
        if seg.get("frc"):      # fps / framerate already made a CFR stream; a second fps filter would re-round blended timestamps
            lines.append("[%s]extractplanes=y,format=gray16le,setsar=1,setpts=N/FRAME_RATE/TB[%s]" % (cur, lab))
        else:
            lines.append("[%s]extractplanes=y,format=gray16le,fps=fps=%s,setsar=1,setpts=PTS-STARTPTS[%s]" % (cur, fstr(out_fps), lab))
        labels.append(lab)
        segs_out.append(dict(idx=si, kind=kind, out_start=out_start, out_frames=n_out, src_a=seg["a"], src_b=seg["b"], seg=seg))
        out_start += n_out
    if len(labels) == 1:
        lines.append("[%s]null[vout]" % labels[0])
    else:
        lines.append("%sconcat=n=%d:v=1:a=0[vout]" % ("".join("[%s]" % x for x in labels), len(labels)))
    fc = ";\n".join(lines)
    return dict(inputs=inputs, filter_complex=fc, segs=segs_out, frames=out_start, fps=out_fps)


def check_graph(fc, n_inputs):
    """Static sanity: every internal [label] produced exactly once and consumed exactly once; [N:v] inputs exist; [vout] produced."""
    produced, consumed = {}, {}
    for stmt in fc.split(";"):
        stmt = stmt.strip()
        if not stmt:
            continue
        # strip quoted option values (they may hold brackets / commas)
        s = re.sub(r"'[^']*'", "''", stmt)
        m = re.match(r"^((?:\[[^\]]+\])*)(.*?)((?:\[[^\]]+\])*)$", s, re.S)
        ins = re.findall(r"\[([^\]]+)\]", m.group(1))
        outs = re.findall(r"\[([^\]]+)\]", m.group(3))
        for i in ins:
            consumed[i] = consumed.get(i, 0) + 1
        for o in outs:
            produced[o] = produced.get(o, 0) + 1
    problems = []
    for k, v in produced.items():
        if v != 1:
            problems.append("label %s produced %d times" % (k, v))
        if k != "vout" and consumed.get(k, 0) != 1:
            problems.append("label %s consumed %d times" % (k, consumed.get(k, 0)))
    for k, v in consumed.items():
        mm = re.match(r"^(\d+):v$", k)
        if mm:
            if int(mm.group(1)) >= n_inputs:
                problems.append("input %s does not exist" % k)
            continue
        if k not in produced:
            problems.append("label %s consumed but never produced" % k)
    if "vout" not in produced:
        problems.append("no [vout]")
    return problems


# ---------------------------------------------------------------- truth
def seg_truth(ctx, reel, cs):
    m = ctx.masters[reel.master]
    W, H = m["w"], m["h"]
    seg = cs["seg"]
    kind = seg["kind"]
    t = dict(idx=cs["idx"], kind=kind, regime=kind if kind != "stream" else "stream", out_start_frame=cs["out_start"], out_frames=cs["out_frames"],
             src_a=cs["src_a"], src_b=cs["src_b"], native_coded=True, truth_n=dict(x=W, y=H), hold_expected=False, kernel=None, sharpening=None,
             double=None, active_area_out=dict(x=0, y=0, w=W, h=H), n_active_coded=dict(x=W, y=H), bits=seg["chain"].get("bits", 10),
             dither=seg["chain"].get("dither", "none"), chroma_chain=dict(origin="4:2:0 as coded", player_upsampled=False, kernel=None, siting=None),
             picture_n_unsure_allowed=False, latent_picture=None, coded_raster=dict(x=W, y=H), frc=seg.get("frc"))
    if kind in ("menu", "black"):
        t["bits"] = 8
        t["note"] = seg["note"]
        return t
    ks, sid = seg["src"]
    chain = seg["chain"]
    if ks == "rung":
        r = ctx.rungs[sid]
        t["src_id"] = r.id
        cw, ch = r.cw, r.ch
        act = r.active or dict(x=0, y=0, w=cw, h=ch)
        t["bits"] = chain.get("bits", r.bits)
        t["rung"] = dict(codec=r.codec, rate=list(r.rate), kind=r.kind, pre=r.pre, kbps=(load_rung_meta(r) or {}).get("kbps"))
        if r.latent:
            t["picture_n_unsure_allowed"] = True
            t["latent_picture"] = dict(x=r.latent["w"], y=r.latent["h"], kernel=r.studio["kernel"], usm=r.studio["usm"])
    else:
        r = None
        t["src_id"] = "master:" + sid
        cw, ch = W, H
        act = dict(x=0, y=0, w=W, h=H)
        t["bits"] = 8
    stages = chain.get("stages") or []
    win = chain.get("window")
    if stages:
        fw, fh = stages[-1]["w"], stages[-1]["h"]
    elif win:
        fw, fh = cw, ch                      # 1:1 window
    else:
        fw, fh = cw, ch
    ox, oy = (win["x"], win["y"]) if win else (0, 0)
    sx, sy = fw / float(cw), fh / float(ch)
    t["coded_raster"] = dict(x=cw, y=ch)
    t["active_area_out"] = dict(x=int(round(ox + act["x"] * sx)), y=int(round(oy + act["y"] * sy)), w=int(round(act["w"] * sx)), h=int(round(act["h"] * sy)))
    t["n_active_coded"] = dict(x=act["w"], y=act["h"])
    t["truth_n"] = dict(x=int(round(W * cw / float(fw))), y=int(round(H * ch / float(fh))))
    t["native_coded"] = (abs(sx - 1.0) < 1e-9 and abs(sy - 1.0) < 1e-9)
    if stages:
        k = KERNELS[stages[-1]["kernel"]]
        t["kernel"] = dict(name=stages[-1]["kernel"], family=k["family"], backend=k["backend"], params=k["params"])
        if len(stages) == 2:
            k1 = KERNELS[stages[0]["kernel"]]
            t["double"] = dict(n1=dict(x=stages[0]["w"], y=stages[0]["h"]), kernel1=dict(name=stages[0]["kernel"], family=k1["family"], params=k1["params"]))
    if chain.get("usm"):
        t["sharpening"] = dict(amount=chain["usm"]["amount"], radius=chain["usm"]["radius"], msize=max(3, int(round(chain["usm"]["radius"] * 2 + 1)) | 1))
    if r is not None and r.studio and r.studio["usm"]:
        t["studio_sharpening"] = r.studio["usm"]
    if stages or win:
        # chroma: the rung's 4:2:0 chroma is upsampled by the same scaler (SID-F evaluates it; luma-only reels do not)
        t["chroma_chain"] = dict(origin="4:2:0 coded, left-sited", player_upsampled=bool(stages),
                                 kernel=(stages[-1]["kernel"] if stages else None), siting="left")
    for ov in seg["overlays"]:
        if ov["type"] == "pip":
            frac = (ov["h"] * 1.0 / H)
            t["hold_expected"] = frac >= 0.2
            t["overlay_row_fraction"] = frac
    t["overlays"] = [dict(type=o["type"], start=o.get("start"), end=o.get("end")) for o in seg["overlays"]]
    t["fades"] = seg["fades"]
    return t


def truth_events(ctx, reel, seg_truths):
    ev = []
    for i in range(1, len(seg_truths)):
        a, b = seg_truths[i - 1], seg_truths[i]
        geo = (a["truth_n"] != b["truth_n"] or a["active_area_out"] != b["active_area_out"] or a["regime"] != b["regime"] or a["native_coded"] != b["native_coded"])
        qual = False
        cls = "none"
        ra, rb = a.get("rung") or {}, b.get("rung") or {}
        if geo:
            cls = "major"
        elif ra.get("kbps") and rb.get("kbps"):
            hi, lo = max(ra["kbps"], rb["kbps"]), max(1.0, min(ra["kbps"], rb["kbps"]))
            qual = abs(hi - lo) > 0.01
            cls = "major" if hi / lo >= MAJOR_RATE else "minor"
        if a["regime"] != "stream" or b["regime"] != "stream":
            kind = "regime"
        elif geo:
            kind = "geometry"
        elif qual:
            kind = "quality"
        else:
            kind = "content"
        ev.append(dict(frame=b["out_start_frame"], from_seg=i - 1, to_seg=i, kind=kind, cls=cls,
                       direction=("drop" if (kind == "quality" and ra.get("kbps", 0) > rb.get("kbps", 0)) or (kind == "geometry" and b["truth_n"]["y"] < a["truth_n"]["y"]) else "rise")))
    return ev


def write_truth_csv(ctx, reel, comp, seg_truths):
    m = ctx.masters[reel.master]
    in_fps = m["fps"]
    os.makedirs(ctx.truth_dir, exist_ok=True)
    path = "%s/%s.frames.csv" % (ctx.truth_dir, reel.id)
    out_f = comp["fps"][0] / float(comp["fps"][1])
    with open(path, "w", encoding="utf-8", newline="\n") as f:
        f.write("reel,out_frame,t_s,seg,regime,src_id,src_frame,pict_type,qp,repeat,blended,gop_idx,idr,flags\n")
        for cs in comp["segs"]:
            seg = cs["seg"]
            n_in = seg["b"] - seg["a"]
            types = []
            if seg["kind"] == "stream":
                ks, sid = seg["src"]
                if ks == "rung":
                    types = read_frames_csv(ctx.rungs[sid].frames_csv)
                else:
                    types = master_frames(ctx, m, seg["a"], n_in)
            rows = frc_map(in_fps, tuple(seg["frc"]["out"]), seg["frc"]["mode"], n_in) if seg.get("frc") else [(j, 0, 0) for j in range(n_in)]
            gop = gop_of(m)
            for j, (src, rep, bl) in enumerate(rows):
                of = cs["out_start"] + j
                if seg["kind"] != "stream":
                    f.write("%s,%d,%.6f,%d,%s,,-1,-,-1,0,0,-1,0,\n" % (reel.id, of, of / out_f, cs["idx"], seg["kind"]))
                    continue
                sf = seg["a"] + src
                pt, qp = types[sf] if sf < len(types) else ("?", -1.0)
                flags = []
                fd = seg["fades"]
                if fd.get("out") and src >= n_in - fd["out"]:
                    flags.append("fade_out")
                if fd.get("in_") and src < fd["in_"]:
                    flags.append("fade_in")
                f.write("%s,%d,%.6f,%d,stream,%s,%d,%s,%s,%d,%d,%d,%d,%s\n" % (reel.id, of, of / out_f, cs["idx"], ctx_src_id(seg), sf, pt, ("%.2f" % qp),
                                                                                  rep, bl, sf // gop, 1 if pt == "I" else 0, "|".join(flags)))
    return path


def ctx_src_id(seg):
    ks, sid = seg["src"]
    return sid if ks == "rung" else "master:" + sid


# ---------------------------------------------------------------- driver
def plan_rungs_for(ctx):
    return sorted(ctx.rungs.values(), key=lambda r: r.id)


def build_all(ctx, families):
    for fam in families:
        FAMILIES[fam](ctx)
    if ctx.args.only_reel:
        ctx.reels = [r for r in ctx.reels if any(x in r.id for x in ctx.args.only_reel.split(","))]
    if ctx.args.limit:
        ctx.reels = ctx.reels[:ctx.args.limit]
    if ctx.args.limit or ctx.args.only_reel:
        used = set()
        for r in ctx.reels:
            for s in r.segs:
                if s["src"] and s["src"][0] == "rung":
                    used.add(s["src"][1])
        ctx.rungs = {k: v for k, v in ctx.rungs.items() if k in used}


def validate_reels(ctx):
    bad = 0
    for reel in ctx.reels:
        comp = compile_reel(ctx, reel)
        probs = check_graph(comp["fc"] if "fc" in comp else comp["filter_complex"], len(comp["inputs"]))
        fpsv = comp["fps"][0] / float(comp["fps"][1])
        if comp["frames"] / fpsv < 30.0 - 1e-6 and ctx.gops >= 15:
            probs.append("reel shorter than 30 s (%.2f s)" % (comp["frames"] / fpsv))
        for p in probs:
            print("GRAPH PROBLEM %s: %s" % (reel.id, p))
            bad += 1
    return bad


def print_plan(ctx):
    est = 0
    for r in plan_rungs_for(ctx):
        est += r.est_bytes(ctx)
    fam = {}
    for r in ctx.reels:
        fam[r.family] = fam.get(r.family, 0) + 1
    print("masters: %s" % ", ".join("%s%s" % (k, "(SKIP: %s)" % v["skip"] if v.get("skip") else "") for k, v in ctx.masters.items()))
    print("reels: %d  %s" % (len(ctx.reels), "  ".join("%s=%d" % (k, fam[k]) for k in sorted(fam))))
    print("rungs: %d  estimated %.2f GB (budget %.1f GB)   D: free %.1f GB (floor %.1f GB)" % (len(ctx.rungs), est / 1073741824.0, ctx.args.rung_budget_gb,
                                                                                               disk_free_gb(ctx.out), ctx.args.min_free_gb))


def write_manifest(ctx):
    rungs_out, reels_out = {}, []
    for r in plan_rungs_for(ctx):
        meta = load_rung_meta(r)
        if not meta or not os.path.exists(r.path):
            continue
        d = r.truth(ctx)
        d.update(id=r.id, master=r.master, path=P(r.path), frames_csv=P(r.frames_csv), fps=list(r.fps), frames=meta["frames"], gop=r.gop,
                 size_bytes=meta["size_bytes"], kbps=meta["kbps"], qp_source=meta["qp_source"], looped=meta["looped"], preset=meta["preset"])
        rungs_out[r.id] = d
    for reel in ctx.reels:
        need = [s["src"][1] for s in reel.segs if s["src"] and s["src"][0] == "rung"]
        if any(n not in rungs_out for n in need):
            continue
        comp = compile_reel(ctx, reel)
        seg_truths = [seg_truth(ctx, reel, cs) for cs in comp["segs"]]
        tpath = write_truth_csv(ctx, reel, comp, seg_truths)
        m = ctx.masters[reel.master]
        looped = any(rungs_out[n]["looped"] for n in need)
        reels_out.append(dict(id=reel.id, family=reel.family, title=reel.title, master=reel.master, fps=list(comp["fps"]), width=m["w"], height=m["h"],
                              pix_fmt="gray16le", frames=comp["frames"], duration_s=round(comp["frames"] / (comp["fps"][0] / float(comp["fps"][1])), 4),
                              looped=looped, flags=reel.flags, note=reel.note, inputs=comp["inputs"], filter_complex=comp["filter_complex"],
                              segments=seg_truths, events=truth_events(ctx, reel, seg_truths), truth_csv=P(tpath)))
    masters_out = {}
    for k, m in ctx.masters.items():
        masters_out[k] = dict(id=k, cls=m["cls"], skip=m.get("skip", ""), width=m.get("w"), height=m.get("h"), fps=list(m["fps"]) if m.get("fps") else None,
                              eight_bit=m.get("eight_bit"), licence=m["lic"], url=m["url"], size_mb=m.get("size_mb"),
                              parts=[dict(path=P(p["path"]), ss_s=p["ss"], pix_fmt=p["pix_fmt"], licence=p["lic"], author=p["author"], url=p["url"]) for p in m["parts"]])
    doc = dict(schema=SCHEMA, generator="tools/eval/source_id/make_identity_reels.py", seed=SEED, ffmpeg=ctx.version, zscale=ctx.zscale,
               encoders=sorted(ctx.encoders), gops=ctx.gops, threads=ctx.args.threads, output_pix_fmt="gray16le",
               note="luma codes: the harness takes y16 >> (16 - bits) per segment, bits from segments[].bits; no film frames are stored with this file",
               masters=masters_out, rungs=rungs_out, reels=reels_out)
    os.makedirs(ctx.out, exist_ok=True)
    with open(ctx.out + "/reels.json", "w", encoding="utf-8", newline="\n") as f:
        json.dump(doc, f, indent=1, sort_keys=False)
    return len(reels_out), len(rungs_out)


def write_credits(ctx):
    if "synth_credits" not in ctx.masters:
        return
    rnd = random.Random(SEED)
    first = ["ANNA", "BRUNO", "CLARA", "DAVID", "ELENA", "FELIX", "GRETA", "HUGO", "IRIS", "JONAS", "KARIN", "LUKAS", "MAYA", "NOAH", "OLGA", "PAUL"]
    last = ["ARNOLD", "BERG", "CHEN", "DUBOIS", "EKLUND", "FISCHER", "GARCIA", "HOLM", "ITO", "JENSEN", "KOVAC", "LARSEN", "MOREAU", "NOVAK", "OKAFOR", "PETROV"]
    role = ["GAFFER", "BEST BOY", "FOCUS PULLER", "COLORIST", "SOUND MIXER", "EDITOR", "RUNNER", "SCRIPT SUPERVISOR"]
    os.makedirs(ctx.work_dir, exist_ok=True)
    lines = []
    for i in range(120):
        lines.append("%s    %s %s" % (rnd.choice(role), rnd.choice(first), rnd.choice(last)))
    with open(ctx.work_dir + "/credits.txt", "w", encoding="utf-8", newline="\n") as f:
        f.write("\n".join(lines) + "\n")


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--out", default="D:/testclips/sourceid")
    ap.add_argument("--families", default="R1,R2,R3,R4,R5,R6,R7,R8,R9")
    ap.add_argument("--masters", default="")
    ap.add_argument("--gops", type=int, default=15, help="GOPs per reel (~2 s each; 15 = >= 30 s)")
    ap.add_argument("--limit", type=int, default=0)
    ap.add_argument("--only-reel", default="", help="comma list of reel id substrings")
    ap.add_argument("--plan", action="store_true")
    ap.add_argument("--manifest-only", action="store_true")
    ap.add_argument("--threads", type=int, default=6)
    ap.add_argument("--min-free-gb", type=float, default=40.0)
    ap.add_argument("--rung-budget-gb", type=float, default=15.0)
    ap.add_argument("--preset-override", default="")
    ap.add_argument("--ffmpeg-dir", default="D:/tools/ffmpeg/bin")
    ap.add_argument("--no-zscale", action="store_true")
    ap.add_argument("--av1", action="store_true", help="also build the AV1 (libaom, slow) 4K stream reel")
    ap.add_argument("--selftest", action="store_true")
    args = ap.parse_args(argv)
    if args.selftest:
        return selftest()
    ctx = Ctx(args)
    probe_tools(ctx)
    load_masters(ctx)
    os.makedirs(ctx.work_dir, exist_ok=True)
    write_credits(ctx)
    build_all(ctx, [f for f in args.families.split(",") if f in FAMILIES])
    bad = validate_reels(ctx)
    print_plan(ctx)
    if bad:
        print("graph validation failed; nothing generated")
        return 2
    if args.plan:
        return 0
    try:
        if not args.manifest_only:
            check_disk(ctx, "the batch")
            for r in plan_rungs_for(ctx):
                state = encode_rung(ctx, r)
                if state == "kept":
                    print("  keep   %s" % r.id)
    except StopForDisk as e:
        print(str(e))
        return 3
    nr, nrung = write_manifest(ctx)
    print("manifest: %d reels, %d rungs -> %s/reels.json   ffmpeg: %s" % (nr, nrung, ctx.out, ctx.version))
    return 0


# ---------------------------------------------------------------- selftest (no ffmpeg)
def selftest():
    class A:
        out = "D:/testclips/sourceid"
        gops = 15
        threads = 6
        preset_override = ""
        ffmpeg_dir = "."
        no_zscale = False
        masters = ""
        limit = 0
        only_reel = ""
        av1 = True
    ctx = Ctx(A)
    ctx.zscale = True
    ctx.encoders = {"libx264", "libx265", "libaom-av1"}
    for mid, cls, parts, lic in MASTERS:
        m = dict(id=mid, cls=cls, parts=[], synthetic=mid in SYNTHETIC, lic="x", url="", skip="", w=3840, h=2160, fps=(24000, 1001), pix_fmt="yuv420p", eight_bit=True, size_mb=0)
        for path, ss in parts:
            m["parts"].append(dict(path=path, ss=ss, w=3840, h=2160, fps=23.976, pix_fmt="yuv420p", duration=40.0, url="", lic="", author="", size_mb=1))
        ctx.masters[mid] = m
    ctx.masters["sport60_k808"]["fps"] = (60000, 1001)
    ctx.masters["nat30_nikon"]["fps"] = (30000, 1001)
    ctx.masters["nat25_funchal"]["fps"] = (25, 1)
    ctx.masters["sport30_torschuss"]["fps"] = (30000, 1001)
    ctx.masters["nat24_steeple"]["fps"] = (24000, 1001)
    ctx.masters["film_dragon"]["fps"] = (24, 1)
    assert fps_rational(59.94) == (60000, 1001) and fps_rational(25.0) == (25, 1) and fps_rational(23.976) == (24000, 1001)
    assert even(3840 * 5 / 12.0) == 1600 and even(2160 * 5 / 12.0) == 900 and even(2160 * 2 / 3.0) == 1440
    assert gop_of(ctx.masters["nat24_steeple"]) == 48 and gop_of(ctx.masters["sport60_k808"]) == 120
    # every reel is >= 30 s at the master rate
    for mid, m in ctx.masters.items():
        assert nframes_of(ctx, m) / master_fps(m) >= 30.0, mid
    # frame-rate conversion maps
    rows = frc_map((24000, 1001), (60000, 1001), "32", 24)
    assert len(rows) == 60 and rows[0][0] == 0 and rows[-1][0] == 23
    counts = [sum(1 for r in rows if r[0] == k) for k in range(24)]
    assert set(counts) <= {2, 3} and sum(counts) == 60 and counts[:4] == [3, 2, 3, 2], counts
    assert sum(r[1] for r in rows) == 60 - 24            # repeats = outputs - distinct inputs
    rows2 = frc_map((25, 1), (50, 1), "32", 10)
    assert [r[0] for r in rows2] == [i // 2 for i in range(20)]
    assert len(rows2) == 20
    # kernels and filter strings
    assert scale_filter(ctx, "lanczos3", 3840, 2160).startswith("scale=3840:2160:flags=lanczos+")
    assert "param0=0.0:param1=0.6" in scale_filter(ctx, "bicubic", 3840, 2160)
    assert scale_filter(ctx, "spline36", 3840, 2160) == "zscale=w=3840:h=2160:filter=spline36"
    assert usm_filter(0.6, 1.5) == "unsharp=lx=5:ly=5:la=0.6:cx=3:cy=3:ca=0"
    # build every family, compile every reel, check the graphs and the truth
    for fam in FAMILIES.values():
        fam(ctx)
    ids = [r.id for r in ctx.reels]
    assert len(ids) == len(set(ids))
    bad = 0
    for reel in ctx.reels:
        comp = compile_reel(ctx, reel)
        probs = check_graph(comp["filter_complex"], len(comp["inputs"]))
        if probs:
            bad += 1
            print("selftest graph problem", reel.id, probs[:3])
        fpsv = comp["fps"][0] / float(comp["fps"][1])
        assert comp["frames"] / fpsv >= 29.99, (reel.id, comp["frames"] / fpsv)
        assert sum(cs["out_frames"] for cs in comp["segs"]) == comp["frames"]
    assert bad == 0
    # truth sanity
    by = {r.id: r for r in ctx.reels}
    t = seg_truth(ctx, by["R2.clean.nat30_nikon.r1of2.lanczos3"], compile_reel(ctx, by["R2.clean.nat30_nikon.r1of2.lanczos3"])["segs"][0])
    assert t["truth_n"] == dict(x=1920, y=1080) and not t["native_coded"] and t["kernel"]["family"] == "lanczos3"
    reel = by["R3.double.nat30_nikon.720_1080_2160"]
    t = seg_truth(ctx, reel, compile_reel(ctx, reel)["segs"][0])
    assert t["truth_n"] == dict(x=1280, y=720) and t["double"]["n1"] == dict(x=1920, y=1080)
    reel = by["R4.letterbox239.nat30_nikon.r1of2.lanczos3"]
    t = seg_truth(ctx, reel, compile_reel(ctx, reel)["segs"][0])
    assert t["active_area_out"] == dict(x=0, y=276, w=3840, h=1608) and t["n_active_coded"] == dict(x=1920, y=804), t
    reel = by["R4.window1440_from1080.nat30_nikon"] if "R4.window1440_from1080.nat30_nikon" in by else by["R4.window1440_from1080.nat30_nikon.lanczos3"]
    t = seg_truth(ctx, reel, compile_reel(ctx, reel)["segs"][0])
    assert t["truth_n"] == dict(x=2880, y=1620) and t["active_area_out"]["w"] == 2560, t
    reel = by["R4.window1080_1to1.nat30_nikon"]
    t = seg_truth(ctx, reel, compile_reel(ctx, reel)["segs"][0])
    assert t["native_coded"] and t["truth_n"] == dict(x=3840, y=2160) and t["active_area_out"] == dict(x=960, y=540, w=1920, h=1080), t
    reel = by["R5.studio.nat30_nikon.crf20"]
    t = seg_truth(ctx, reel, compile_reel(ctx, reel)["segs"][0])
    assert t["native_coded"] and t["picture_n_unsure_allowed"] and t["latent_picture"]["x"] == 1920
    reel = by["R7.pip_large.nat30_nikon.r1of2"]
    assert seg_truth(ctx, reel, compile_reel(ctx, reel)["segs"][0])["hold_expected"]
    reel = by["R7.pip_small.nat30_nikon.r1of2"]
    assert not seg_truth(ctx, reel, compile_reel(ctx, reel)["segs"][0])["hold_expected"]
    reel = by["R7.menu_black250.nat30_nikon"]
    kinds = [s["kind"] for s in compile_reel(ctx, reel)["segs"]]
    assert kinds == ["stream", "black", "menu", "black", "stream"], kinds
    # rung budget estimate stays far below the 15 GB cap
    est = sum(r.est_bytes(ctx) for r in ctx.rungs.values())
    print("selftest ok: %d reels, %d rungs, estimated rung bytes %.2f GB" % (len(ctx.reels), len(ctx.rungs), est / 1073741824.0))
    return 0


if __name__ == "__main__":
    sys.exit(main())
