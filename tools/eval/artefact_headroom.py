# Copyright(C) 2026 VP-V contributors
# SPDX-License-Identifier: GPL-3.0-only
"""Round-2 HEADROOM for targeted repairs (same method as mosquito_headroom.py): how much a PERFECT
repair could gain, per artefact x codec x rate, 1080p / 2160p, judged in the DELIVERED output space.
CPU only; scratch pixels outside the worktree.

  python -I tools/eval/artefact_headroom.py --scratch D:/codex/videoprocessor/eval-scratch/<dir> \
         --out tools/eval/prototypes/artefact_headroom

Windows: 1024x768 x 12 frames (2160p: native crop of a 4K master; 1080p: Lanczos downscale of the
whole master to 1920 wide, then the crop; masters narrower than 3840 run at 1080p only).
Encodes (ffmpeg, encoder defaults otherwise, GOP 48):
  SDR masters (8-bit delivery): libx264 8-bit CRF 23/28 (preset medium, deblock on), libx265 8-bit
      CRF 24/30 (SAO on), libaom-av1 8-bit cpu-used 6 CRF 30/40; reference = the 8-bit master window.
  HDR masters (PQ, 10-bit delivery): libx265 10-bit CRF 24/30, libaom-av1 10-bit CRF 30/40;
      reference = the 10-bit master window.
  plus, for HDR masters, a calibration row per context: the reference quantised to 8 bits in the
      delivered space with no dither (the 8-bit quantisation error, for scale; SDR masters are 8-bit already).
Contexts (owner requirement): SDR = native SDR source in gamma codes; HDR-PQ = HDR10 passed
through, measured on PQ codes; HDR->SDR = reference and decode both tone-mapped by the static
spline (tonemap_ref, VP FL35 setting: peak detection off, 100 nit) and measured on SDR gamma codes
(float, no output quantisation: VP dithers the 8-bit link).  All luma, 10-bit code units.
Masks come from the reference in the same space:
  sigma_n   robust noise level (MAD of the 1-px residual on the 30 % least-structured pixels)
  smooth    5x5 local energy of the 1-px residual <= 2 sigma_n^2 (no texture / grain above noise)
  gradient  smooth & 0.05 <= |grad gauss(ref, 2)| <= 3 codes/px (smooth ramps, no edges)
  dark      gauss(ref, 2) below ~2.1 nit (SDR: V <= 0.2; PQ: e <= PQ(2.1 nit))
  texture   not within r of a strong edge (near_edge_zone rule) & local residual RMS (7x7) in
            [1.5, 8] codes (low-contrast texture INCLUDING grain; the 1.5 floor sits above 8-bit
            quantisation noise, 1.15 codes RMS)
Frequency split (so grain loss is never counted as banding): coarse error = gauss(err, 3 px) ->
banding / dark blocking; fine error = err - gauss(err, 1 px) -> texture / grain loss.
Metrics (err = decode - reference; all shares are of the whole-window error energy):
  (1) banding: band_share = energy of gauss(err, 3) in the gradient mask -> headroom_band_db =
      -10 log10(1 - share); false_flat = share of ramp pixels (gradient mask with reference slope
      >= max(0.1, 0.06 sigma_n) code/px at gauss 4, i.e. above the grain / noise gradient) whose
      decoded slope fell below 25 % of the reference's (ramps turned into plateaus / contours);
      step_p99 = 99th percentile |gauss(err, 3)| in the mask (codes; 4 = one 8-bit step).
  (2) dark blocking: dark_share = energy of gauss(err, 3) in dark & smooth -> headroom_dark_db;
      blockiness = BI(dec) - BI(ref), BI = mean |first difference| across the 8-px grid lines /
      mean |first difference| inside blocks, over dark pixels (h and v); block_share = removable
      boundary-step energy as share (approximate: excess squared step across grid lines / 4, times
      the 8 pixels of block row / column each step offsets).
  (3) texture loss: tex_ratio = residual energy of dec / ref in the texture mask (1 - ratio =
      deficit); tex_share = energy of the 1-px residual of err in the mask -> headroom_tex_db
      (an upper bound: only a master-knowing repair could remove it; no invented detail).
"""
import argparse
import csv
import json
import os
import subprocess
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

import numpy as np  # noqa: E402
from scipy import ndimage  # noqa: E402

import eval_io as eio  # noqa: E402
import eval_metrics as em  # noqa: E402
import tonemap_ref as tr  # noqa: E402

FF = eio.DEFAULT_FFMPEG
T = r"D:\testclips\eval-2026-10-31"
W, H, N = 1024, 768, 12
NT = r"D:\nn-corpus\trimmed"
# name, path, seek s, fps, source WxH, transfer, mastering peak (PQ), content
MASTERS = [
    ("sintel_sky", NT + r"\sintel4k\reference_ffv1.mkv", 13.3, 24.0, (3840, 1744), "sdr", None,
     "CG animation, bright sky gradient"),
    ("cosmos_dark", NT + r"\cosmos2k_existing\reference_ffv1.mkv", 100.0, 23.976, (2048, 858), "sdr", None,
     "CG, dark smooth gradients (2K master: 1080p only)"),
    ("meridian_dark", NT + r"\meridian4k_existing\reference_ffv1.mkv", 40.0, 59.94, (3840, 2160), "sdr", None,
     "live action noir, dark + fog gradients"),
    ("tos4k_dark", NT + r"\tos4k\reference_ffv1.mkv", 26.7, 24.0, (3840, 1714), "sdr", None,
     "film/CG, dark scene"),
    ("tos_dark_2stops", r"D:\testclips\variants\tos_dark_2stops_23976p_3min.mp4", 120.0, 23.976, (1920, 800), "sdr",
     None, "tos -2 stops (H.264 ~9.7 Mb/s variant, pre-compressed; 1080p only)"),
    ("sol_sky", T + r"\hdr\sol_levante_hdr10_24p_t150\sol_levante_hdr10_24p_t150.mov", 3.3, 24.0, (3840, 2160), "pq",
     1000.0, "HDR10 animation, sky / smooth gradients (12-bit 4:4:4 master)"),
    ("sol_dark", T + r"\hdr\sol_levante_hdr10_24p_t40\sol_levante_hdr10_24p_t40.mov", 7.8, 24.0, (3840, 2160), "pq",
     1000.0, "HDR10 animation, dark scene"),
    ("nocturne_dark", T + r"\hdr_film_grain\nocturne_filmgrain_hdr_60p_t300\nocturne_filmgrain_hdr_60p_t300.mov", 5.6,
     60.0, (3840, 2160), "pq", 4000.0, "HDR live action, very dark, synthetic film grain"),
    ("nocturne_mid", T + r"\hdr_film_grain\nocturne_filmgrain_hdr_60p_t120\nocturne_filmgrain_hdr_60p_t120.mov", 4.4,
     60.0, (3840, 2160), "pq", 4000.0, "HDR live action, low-key, synthetic film grain"),
]
X264 = ["-c:v", "libx264", "-preset", "medium", "-g", "48"]
X265 = ["-c:v", "libx265", "-preset", "medium", "-g", "48", "-x265-params", "log-level=error"]
AOM = ["-c:v", "libaom-av1", "-cpu-used", "6", "-b:v", "0", "-g", "48", "-row-mt", "1"]
ENC_SDR = [("H.264", "x264 crf23", X264 + ["-crf", "23"]), ("H.264", "x264 crf28", X264 + ["-crf", "28"]),
           ("HEVC", "x265-8 crf24", X265 + ["-crf", "24"]), ("HEVC", "x265-8 crf30", X265 + ["-crf", "30"]),
           ("AV1", "aom-8 crf30", AOM + ["-crf", "30"]), ("AV1", "aom-8 crf40", AOM + ["-crf", "40"])]
ENC_HDR = [("HEVC", "x265-10 crf24", X265 + ["-crf", "24"]), ("HEVC", "x265-10 crf30", X265 + ["-crf", "30"]),
           ("AV1", "aom-10 crf30", AOM + ["-crf", "30"]), ("AV1", "aom-10 crf40", AOM + ["-crf", "40"])]
CAL = "8-bit quantised (calibration)"
RASTERS = {"2160p": (3840, 2160), "1080p": (1920, 1080)}
DARK_NITS = 100.0 * 0.2 ** 2.4  # 2.1 nit: V = 0.2 on a 100-nit SDR display


def run(cmd):
    r = subprocess.run(cmd, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    if r.returncode != 0:
        raise RuntimeError(" ".join(cmd) + "\n" + r.stderr.decode("utf8", "replace")[-800:])


def prepare(scratch, name, path, ss, src, raster, pix):
    out = os.path.join(scratch, "%s_%s_%s.yuv" % (name, raster, pix))
    if os.path.exists(out):
        return out
    sw, sh = src
    if raster == "2160p":
        vf = "crop=%d:%d:%d:%d" % (W, H, (sw - W) // 2 // 2 * 2, (sh - H) // 2 // 2 * 2)
    else:
        if sw == 1920:
            dw, dh, sc = sw, sh, ""
        else:
            dw, dh = 1920, int(round(sh * 1920 / sw / 2)) * 2
            sc = "scale=%d:%d:flags=lanczos," % (dw, dh)
        vf = sc + "crop=%d:%d:%d:%d" % (W, H, (dw - W) // 2 // 2 * 2, (dh - H) // 2 // 2 * 2)
    run([FF, "-hide_banner", "-loglevel", "error", "-y", "-ss", "%.4f" % ss, "-i", path, "-frames:v", str(N), "-vf", vf,
         "-pix_fmt", pix, "-f", "rawvideo", out + ".tmp"])
    os.replace(out + ".tmp", out)
    return out


def luma_seq(path, pix, n=N):
    fr = eio.read_raw_frames(path, W, H, pix, 0, n)
    return np.stack([eio.to_bitdepth(f, 10).Y for f in fr])


def encode(scratch, name, raster, label, args, src_raw, src_pix, fps):
    tag = label.replace(" ", "_")
    enc = os.path.join(scratch, "%s_%s_%s.mkv" % (name, raster, tag))
    dec = os.path.join(scratch, "%s_%s_%s.yuv" % (name, raster, tag))
    if not os.path.exists(dec):
        run([FF, "-hide_banner", "-loglevel", "error", "-y", "-f", "rawvideo", "-pix_fmt", src_pix, "-s", "%dx%d" % (W, H),
             "-r", "%g" % fps, "-i", src_raw] + args + ["-pix_fmt", src_pix, enc])
        run([FF, "-hide_banner", "-loglevel", "error", "-y", "-i", enc, "-pix_fmt", "yuv420p10le", "-f", "rawvideo",
             dec + ".tmp"])
        os.replace(dec + ".tmp", dec)
    return enc, dec


# ------------------------------------------------------------------ masks and metrics (shared with ltm_eval)
def _hp(a, s=1.0):
    return a - ndimage.gaussian_filter(a, s, mode="nearest")


def noise_sigma(r):
    g = np.hypot(*np.gradient(ndimage.gaussian_filter(r, 2.0, mode="nearest")))
    sel = g <= np.quantile(g, 0.3)
    v = _hp(r)[sel]
    return max(1.4826 * float(np.median(np.abs(v - np.median(v)))), 0.25)


def masks(r, dark_code):
    """Reference-only masks for one frame (10-bit codes in the delivered space)."""
    hr = _hp(r)
    sig = noise_sigma(r)
    loc = ndimage.uniform_filter(hr * hr, size=5, mode="nearest")
    smooth = loc <= 2.0 * sig * sig
    lp2 = ndimage.gaussian_filter(r, 2.0, mode="nearest")
    g = np.hypot(*np.gradient(lp2))
    grad = smooth & (g >= 0.05) & (g <= 3.0)
    dark = lp2 <= dark_code
    lpe = ndimage.gaussian_filter(r, 1.0, mode="nearest")
    ge = np.hypot(ndimage.sobel(lpe, 0, mode="nearest"), ndimage.sobel(lpe, 1, mode="nearest")) / 8.0
    edges = ge > em.EDGE_GRAD * em.peak(10)
    rr = max(4, int(round(min(r.shape) / 128.0)))
    near = ndimage.binary_dilation(edges, iterations=rr) if edges.any() else np.zeros(r.shape, bool)
    lrms = np.sqrt(np.maximum(ndimage.uniform_filter(hr * hr, size=7, mode="nearest"), 0.0))
    tex = ~near & (lrms >= 1.5) & (lrms <= 8.0)
    return dict(smooth=smooth, grad=grad, dark=dark, dark_smooth=dark & smooth, tex=tex, sigma=sig)


def _block_stats(a, m, grid=8):
    """mean |d1| on grid lines and inside blocks over mask m (both directions)."""
    dx = np.abs(np.diff(a, axis=1))
    dy = np.abs(np.diff(a, axis=0))
    mx = m[:, :-1] & m[:, 1:]
    my = m[:-1, :] & m[1:, :]
    bx = np.zeros(dx.shape, bool)
    bx[:, grid - 1::grid] = True
    by = np.zeros(dy.shape, bool)
    by[grid - 1::grid, :] = True
    out = {}
    for k, (d, mm, b) in enumerate(((dx, mx, bx), (dy, my, by))):
        out[k] = (d[mm & b], d[mm & ~b])
    return out


def frame_metrics(r, c, dark_code):
    """Accumulable sums for one frame pair (reference r, decode / candidate c)."""
    e = c - r
    m = masks(r, dark_code)
    le = ndimage.gaussian_filter(e, 3.0, mode="nearest")  # coarse band: banding / blocking (grain-free)
    he = _hp(e)  # fine band: texture / grain
    s = dict(tot=float((e * e).sum()), n=e.size)
    s["band"] = float((le[m["grad"]] ** 2).sum())
    s["grad_n"] = int(m["grad"].sum())
    gr = np.hypot(*np.gradient(ndimage.gaussian_filter(r, 4.0, mode="nearest")))
    gc = np.hypot(*np.gradient(ndimage.gaussian_filter(c, 4.0, mode="nearest")))
    ramp = m["grad"] & (gr >= max(0.1, 0.06 * m["sigma"]))  # ramps clearly above the grain / noise gradient
    s["ramp_n"] = int(ramp.sum())
    s["false_flat"] = int((ramp & (gc < 0.25 * gr)).sum())
    s["step"] = np.abs(le[m["grad"]])
    s["dark_e"] = float((le[m["dark_smooth"]] ** 2).sum())
    s["dark_n"] = int(m["dark"].sum())
    bi = []
    blk = 0.0
    for img in (c, r):
        st = _block_stats(img, m["dark"])
        num = sum(float(v[0].sum()) for v in st.values())
        nb = sum(v[0].size for v in st.values())
        den = sum(float(v[1].sum()) for v in st.values())
        ni = sum(v[1].size for v in st.values())
        bi.append((num, nb, den, ni))
    st = _block_stats(e, m["dark"])
    for b, i in st.values():
        if b.size and i.size:
            ex = max(float((b * b).mean()) - float((i * i).mean()), 0.0)
            blk += ex / 4.0 * b.size * 8  # each boundary step offsets ~8 px of its block row/column
    s["bi_c"], s["bi_r"] = bi
    s["blk"] = blk
    hc = _hp(c)
    hr = _hp(r)
    s["tex_c"] = float((hc[m["tex"]] ** 2).sum())
    s["tex_r"] = float((hr[m["tex"]] ** 2).sum())
    s["tex_e"] = float((he[m["tex"]] ** 2).sum())
    s["tex_n"] = int(m["tex"].sum())
    return s


def summarize(frames):
    tot = sum(f["tot"] for f in frames)
    n = sum(f["n"] for f in frames)
    hd = lambda share: float(-10 * np.log10(max(1.0 - share, 1e-12)))
    gsum = lambda k: sum(f[k] for f in frames)
    res = dict(psnr=10 * np.log10(1023.0 ** 2 / (tot / n)) if tot else float("inf"))
    sh = lambda k: gsum(k) / tot if tot else 0.0
    res["grad_frac"] = gsum("grad_n") / n
    res["band_share"] = sh("band")
    res["headroom_band_db"] = hd(res["band_share"])
    res["false_flat"] = gsum("false_flat") / gsum("ramp_n") if gsum("ramp_n") > 100 else None
    steps = np.concatenate([f["step"] for f in frames]) if frames else np.zeros(0)
    res["step_p99"] = float(np.quantile(steps, 0.99)) if steps.size > 100 else None
    res["dark_frac"] = gsum("dark_n") / n
    res["dark_share"] = sh("dark_e")
    res["headroom_dark_db"] = hd(res["dark_share"])

    def bi(key):
        num = sum(f[key][0] for f in frames)
        nb = sum(f[key][1] for f in frames)
        den = sum(f[key][2] for f in frames)
        ni = sum(f[key][3] for f in frames)
        return (num / nb) / (den / ni) if nb and ni and den > 0 else None

    bc, br = bi("bi_c"), bi("bi_r")
    res["blockiness"] = (bc - br) if bc is not None and br is not None else None
    res["block_share"] = sh("blk") if res["dark_frac"] > 0.01 else None
    res["tex_frac"] = gsum("tex_n") / n
    res["tex_ratio"] = gsum("tex_c") / gsum("tex_r") if gsum("tex_r") > 0 else None
    res["tex_share"] = sh("tex_e")
    res["headroom_tex_db"] = hd(res["tex_share"])
    return res


def seq_metrics(ref, cod, dark_code):
    return summarize([frame_metrics(r, c, dark_code) for r, c in zip(ref, cod)])


def dark_code_for(space):
    if space == "pq":
        return float(tr.norm_to_code(tr.pq_oetf(DARK_NITS)))
    return float(tr.norm_to_code(0.2))


def to_sdr(seq, peak):
    m = tr.ToneMapper("global", peak, 2160)
    return np.stack([m.process(f) for f in seq])


def quantise8(seq):
    return np.rint(seq / 4.0) * 4.0


# ------------------------------------------------------------------ main
COLS = ["context", "master", "raster", "codec", "encode", "kbps_full", "psnr", "grad_frac", "band_share",
        "headroom_band_db", "false_flat", "step_p99", "dark_frac", "dark_share", "headroom_dark_db", "blockiness",
        "block_share", "tex_frac", "tex_ratio", "tex_share", "headroom_tex_db", "content"]


def fmt(v):
    if v is None:
        return "-"
    if isinstance(v, float):
        return "%.4g" % v
    return str(v)


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--scratch", required=True)
    ap.add_argument("--out", required=True)
    ap.add_argument("--only", default="", help="comma list of master names")
    a = ap.parse_args(argv)
    os.makedirs(a.scratch, exist_ok=True)
    os.makedirs(a.out, exist_ok=True)
    only = set(x for x in a.only.split(",") if x)
    rows = []
    for name, path, ss, fps, src, transfer, mpeak, content in MASTERS:
        if only and name not in only:
            continue
        for raster, full in RASTERS.items():
            if raster == "2160p" and src[0] < 3840:
                continue
            hdr = transfer == "pq"
            pix = "yuv420p10le" if hdr else "yuv420p"
            m = prepare(a.scratch, name, path, ss, src, raster, pix)
            ref = luma_seq(m, pix)
            contexts = [("HDR-PQ", "pq"), ("HDR->SDR", "sdr")] if hdr else [("SDR", "sdr")]
            refs = {}
            for ctx, space in contexts:
                refs[ctx] = to_sdr(ref, mpeak) if ctx == "HDR->SDR" else ref
            encs = ENC_HDR if hdr else ENC_SDR
            decoded = []
            for codec, label, args in encs:
                enc, dec = encode(a.scratch, name, raster, label, args, m, pix, fps)
                kbps = os.path.getsize(enc) * 8 / (N / fps) / 1000.0 * (full[0] * full[1]) / (W * H)
                decoded.append((codec, label, kbps, luma_seq(dec, "yuv420p10le")))
            for ctx, space in contexts:
                rr = refs[ctx]
                dk = dark_code_for(space)
                items = [("calibration", CAL, None, quantise8(rr))] if hdr else []  # SDR masters are 8-bit
                for codec, label, kbps, d in decoded:
                    items.append((codec, label, kbps, to_sdr(d, mpeak) if ctx == "HDR->SDR" else d))
                for codec, label, kbps, d in items:
                    met = seq_metrics(rr, d, dk)
                    row = dict(context=ctx, master=name, raster=raster, codec=codec, encode=label, kbps_full=kbps,
                               content=content, **met)
                    rows.append(row)
                    print(json.dumps({k: (round(v, 4) if isinstance(v, float) else v) for k, v in row.items()}), flush=True)
    write_tables(rows, a.out)
    return 0


def write_tables(rows, out):
    with open(os.path.join(out, "headroom.csv"), "w", encoding="utf8", newline="") as f:
        w = csv.writer(f, lineterminator="\n")
        w.writerow(COLS)
        for r in rows:
            w.writerow([fmt(r.get(c)) for c in COLS])
    md = ["<!-- Copyright(C) 2026 VP-V contributors -->", "<!-- SPDX-License-Identifier: GPL-3.0-only -->", "",
          "# Round-2 artefact headroom (banding, dark blocking, texture loss) by context x codec x rate", "",
          "Means over the masters of each context; luma, 10-bit codes in the delivered space. See artefact_headroom.py.",
          "Headroom = PSNR gain if a PERFECT repair removed all of that artefact's error. 'cal' = the reference",
          "quantised to 8 bits without dither (8-bit quantisation error), for scale.", "",
          "| context | raster | encode | Mbps | PSNR | band hd dB | false-flat | step p99 | dark hd dB | blockiness | "
          "block share | tex ratio | tex hd dB |",
          "|---|---|---|---|---|---|---|---|---|---|---|---|---|"]
    keys = []
    for r in rows:
        k = (r["context"], r["raster"], r["encode"])
        if k not in keys:
            keys.append(k)
    order = {"SDR": 0, "HDR-PQ": 1, "HDR->SDR": 2}
    keys.sort(key=lambda k: (order[k[0]], k[1] != "2160p", k[2] != CAL, k[2]))

    def mean(rs, k):
        v = [r[k] for r in rs if r.get(k) is not None]
        return float(np.mean(v)) if v else None

    def mx(rs, k):
        v = [r[k] for r in rs if r.get(k) is not None]
        return float(np.max(v)) if v else None

    f2 = lambda v, p="%.2f": "-" if v is None else p % v
    for k in keys:
        rs = [r for r in rows if (r["context"], r["raster"], r["encode"]) == k]
        kb = mean(rs, "kbps_full")
        md.append("| %s | %s | %s | %s | %s | %s / %s | %s | %s | %s / %s | %s | %s | %s | %s / %s |" % (
            k[0], k[1], "cal" if k[2] == CAL else k[2], f2(kb / 1000.0 if kb else None, "%.1f"), f2(mean(rs, "psnr")),
            f2(mean(rs, "headroom_band_db")), f2(mx(rs, "headroom_band_db")), f2(mean(rs, "false_flat"), "%.3f"),
            f2(mean(rs, "step_p99"), "%.1f"), f2(mean(rs, "headroom_dark_db")), f2(mx(rs, "headroom_dark_db")),
            f2(mean(rs, "blockiness"), "%.3f"), f2(mean(rs, "block_share"), "%.4f"), f2(mean(rs, "tex_ratio"), "%.3f"),
            f2(mean(rs, "headroom_tex_db")), f2(mx(rs, "headroom_tex_db"))))
    md += ["", "## Per master", "", "| " + " | ".join(COLS) + " |", "|" + "---|" * len(COLS)]
    for r in rows:
        md.append("| " + " | ".join(fmt(r.get(c)) for c in COLS) + " |")
    md.append("")
    with open(os.path.join(out, "headroom.md"), "w", encoding="utf8", newline="\n") as f:
        f.write("\n".join(md))


if __name__ == "__main__":
    sys.exit(main())
