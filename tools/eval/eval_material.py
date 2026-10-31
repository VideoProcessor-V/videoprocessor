# Copyright(C) 2026 VP-V contributors
# SPDX-License-Identifier: GPL-3.0-only
"""Material discovery -> material_catalog.json.

Walks the known corpus roots on the evaluation workstation, records format / frame windows /
availability / GOP where known, and assigns content types with the EXPLICIT rules in CONTENT_RULES
(also documented in README.md).  Missing roots simply yield no entries (availability is visible).

content types: film24, nature30, sports50_60, dark.   `proxy: true` = stand-in material, not the real
thing (see KNOWN_GAPS).
"""
import csv
import glob
import json
import os
import subprocess

import eval_io as eio

HFR = r"D:\hfr"
NN = r"D:\nn-corpus"
TESTCLIPS = r"D:\testclips"
FFPROBE = r"D:\tools\ffmpeg\bin\ffprobe.exe"
FFMPEG = eio.DEFAULT_FFMPEG
DECODE_FROM = 20      # tools/make_codec_corpus.py: decoded frames are source frames 20..23 AFTER a 1 s seek
DECODED = 4

# Content-type rules (every matching rule applies: a dark film clip is both dark and film24).  fps bands are applied to the MEASURED rate of the material.
CONTENT_RULES = [
    ("name contains 'dark' (tos_dark_2stops)", "dark"),
    ("measured fps <= 26 (23.976 / 24 / 25)", "film24"),
    ("26 < measured fps < 48 (29.97 / 30)", "nature30"),
    ("measured fps >= 48 (50 / 59.94 / 60 / 120)", "sports50_60"),
]

KNOWN_GAPS = [
    "no real ball/puck sports footage: Jockey, ReadySetGo, crowd_run, park_joy and meridian 60p are proxies (fast "
    "motion, not ball-tracking content)",
    "no true 30p nature-documentary material: meridian 30p and the decimated boat4k / foodmarket4k 60p are proxies",
    "dark material = tos_dark_2stops_23976p_3min.mp4 plus dark windows of cosmos / meridian / tos4k only",
    "HDR -> SDR tone-mapping material: only chimera4k (PQ, masters are tagged 8-bit 'tv' without HDR flags in "
    "the manifest); meridian / cosmos 'HDR' names carry no HDR tags",
    "no SVT-AV1 encoder in the workstation ffmpeg (D:\\tools\\ffmpeg essentials): AV1 cases exist only as "
    "pre-made decodes (G18 av1fgs, nn-corpus svtav1 pairs)",
    "synthetic sports generator not written yet (eval_synth.generate_sports_clip raises NotImplementedError)",
    "G18 cases hold 4 frames each: temporal metrics are NA there (the breathing clip has 48 frames)",
]


def _ctypes(name, fps, extra=()):
    out = []
    for text, ct in CONTENT_RULES:
        if ct == "dark":
            if "dark" in name.lower() or "dark" in extra:
                out.append("dark")
        elif ct == "film24" and fps and fps <= 26.0 or ct == "nature30" and fps and 26.0 < fps < 48.0 \
                or ct == "sports50_60" and fps and fps >= 48.0:
            out.append(ct)
    return out


def _probe(path):
    try:
        r = subprocess.run([FFPROBE, "-v", "error", "-select_streams", "v:0", "-show_entries",
                            "stream=width,height,pix_fmt,r_frame_rate,nb_frames:stream_tags=DURATION", "-of", "json", path],
                           stdout=subprocess.PIPE, stderr=subprocess.PIPE, timeout=60)
        s = json.loads(r.stdout.decode())["streams"][0]
        a, b = s["r_frame_rate"].split("/")
        return dict(width=int(s["width"]), height=int(s["height"]), pix_fmt=s["pix_fmt"], fps=int(a) / int(b),
                    frames=int(s["nb_frames"]) if str(s.get("nb_frames", "")).isdigit() else None)
    except Exception:
        return None


def _fmt(kind, w, h, pix_fmt, fps, frames=None):
    bd = eio.PIX_FMTS.get(pix_fmt, (None,))[0]
    return dict(kind=kind, width=w, height=h, pix_fmt=pix_fmt, bitdepth=bd, fps=round(fps, 6) if fps else None,
                frames=frames)


def _g18():
    root = os.path.join(HFR, "coded")
    man = os.path.join(root, "manifest.csv")
    if not os.path.exists(man):
        return []
    masters = {}
    for name in sorted(os.listdir(HFR)) if os.path.isdir(HFR) else []:
        p = os.path.join(HFR, name)
        if name.endswith("_120fps_420_10bit_YUV.yuv"):
            masters[name.split("_")[0]] = dict(path=p, **_fmt("raw", 3840, 2160, "yuv420p10le", 120.0))
        elif name.endswith("_2160p50.y4m"):
            info = eio.y4m_info(p)
            full = name.split("_2160p50")[0]
            # make_codec_corpus.py names cases by the first '_' token ("park", "crowd")
            masters[full.split("_")[0]] = dict(clipname=full, path=p, **_fmt("y4m", info["width"], info["height"],
                                                                    info["pix_fmt"], info["fps"], info["frames"]))
    out = []
    with open(man, newline="") as f:
        rows = list(csv.DictReader(f))
    for r in rows:
        m = masters.get(r["clip"])
        if m is None:
            continue
        clip = m.get("clipname", r["clip"])
        m = {k: v for k, v in m.items() if k != "clipname"}
        path = os.path.join(root, r["file"])
        # encoder defaults of the G18 script (ffmpeg -preset fast): x265 keyint 250 bframes 4; x264 keyint 250
        # bframes 3; SVT-AV1 preset 8 default GOP.  Mini-GOP = bframes + 1.
        gop = {"x265": (250, 5), "x264": (250, 4)}.get(r["codec"])
        # the generator seeks 1 s into the master (-ss 1) and decodes coded frames 20..23
        start = int(round(1.0 * m["fps"])) + DECODE_FROM
        out.append(dict(
            id="g18/%s" % os.path.splitext(r["file"])[0], group="g18", path=path,
            content_types=_ctypes(clip, m["fps"]), proxy=True,
            format=_fmt("raw", 3840, 2160, "yuv420p10le", m["fps"], DECODED),
            window=dict(first_coded_frame=0, frames=DECODED),
            master=dict(path=m["path"], kind=m["kind"], first_frame=start, format=m, basis="derived: round(1 s * fps) + 20"),
            available=os.path.exists(path) and os.path.exists(m["path"]),
            gop=dict(gop=gop[0], minigop=gop[1], source="encoder-default") if gop else None,
            attrs=dict(clip=clip, scene=r["scene"], codec=r["codec"], crf=int(r["crf"]), upscaler=r["upscaler"] or None,
                       truth=float(r["truth"])),
        ))
    return out


def _nn():
    mpath = os.path.join(NN, "manifest.json")
    if not os.path.exists(mpath):
        return []
    with open(mpath, "r", encoding="utf8") as f:
        man = json.load(f)
    out, masters = [], {}
    for m in man["masters"]:
        p = os.path.join(NN, "trimmed", m["id"], "reference_ffv1.mkv")
        masters[m["id"]] = m
        pq = m["id"] == "chimera4k"
        cts = _ctypes(m["id"] + " " + m.get("note", ""), m["fps"])
        out.append(dict(
            id="nn/master/%s" % m["id"], group="nn_master", path=p, content_types=cts,
            proxy=m["id"] in ("meridian4k_existing", "foodmarket4k", "boat4k"),
            format=_fmt("ffmpeg", m["width"], m["height"], m["pix_fmt"], m["fps"]),
            window=dict(note="decode a short window with -ss; frames identical to the encodes' timeline"),
            available=os.path.exists(p) and os.path.exists(p + ".done"),
            gop=None, attrs=dict(note=m.get("note"), transfer="pq" if pq else "sdr-or-untagged", lossless="ffv1",
                                 licence=m.get("licence"))))
    for p in man["pairs"]:
        mm = masters.get(p["master"])
        outp = p["output"]
        out.append(dict(
            id="nn/pair/%s" % p["id"], group="nn_pair", path=outp,
            content_types=_ctypes(p["master"] + " " + (mm or {}).get("note", ""), (mm or {}).get("fps")),
            proxy=p["master"] in ("meridian4k_existing", "foodmarket4k", "boat4k"),
            format=_fmt("ffmpeg", p["width"], p["height"], p["pix_fmt"], (mm or {}).get("fps")),
            window=dict(note="same timeline as master (encode starts at master frame 0)"),
            master=dict(path=(mm or {}).get("reference") or p["reference"], kind="ffmpeg", first_frame=0),
            available=os.path.exists(outp) and os.path.exists(outp + ".done"),
            gop=dict(gop=int(round(p["gop_s"] * (mm or {}).get("fps", 0))), minigop=5 if p["codec"] == "x265" else None,
                     source="manifest gop_s x fps") if mm else None,
            attrs=dict(codec=p["codec"], rung=p["rung"], kbps=p["target_kbps"], grain_synthesis=p["grain_synthesis"],
                       rung_scaled=p["rung_scaled"], master=p["master"])))
    return out


def _uvg():
    out = []
    for p in sorted(glob.glob(os.path.join(HFR, "*_3840x2160_120fps_420_10bit_YUV.yuv"))):
        clip = os.path.basename(p).split("_")[0]
        n = os.path.getsize(p) // eio.frame_bytes(3840, 2160, "yuv420p10le")
        out.append(dict(id="uvg/%s" % clip, group="uvg", path=p, content_types=_ctypes(clip, 120.0), proxy=True,
                        format=_fmt("raw", 3840, 2160, "yuv420p10le", 120.0, n), window=dict(note="any window; raw, seekable"),
                        available=True, gop=None, attrs=dict(note="UVG raw 4K 120 fps, 10-bit, BT.709 limited")))
    return out


def _breathing():
    d = os.path.join(HFR, "breathing")
    if not os.path.isdir(d):
        return []
    out = []
    for tag, enc in (("24", "enc24.mkv"), ("32", "enc32.mkv")):
        types = os.path.join(d, "types%s.txt" % tag)
        gop = mini = None
        if os.path.exists(types):
            with open(types) as f:
                seq = [t.strip() for t in f.read().replace("\n", "").split(",") if t.strip()]
            ipos = [i for i, t in enumerate(seq) if t == "I"]
            if len(ipos) >= 2:
                gop = ipos[1] - ipos[0]
            ppos = [i for i, t in enumerate(seq) if t in ("P", "I")]
            gaps = [b - a for a, b in zip(ppos, ppos[1:])]
            mini = max(set(gaps), key=lambda g: (gaps.count(g), g)) if gaps else None
        dec = os.path.join(d, "dec%s.yuv" % tag)
        n = os.path.getsize(dec) // eio.frame_bytes(1536, 768, "yuv420p10le") if os.path.exists(dec) else None
        out.append(dict(id="breathing/qp%s" % tag, group="breathing", path=dec, content_types=["film24"], proxy=True,
                        format=_fmt("raw", 1536, 768, "yuv420p10le", 24.0, n),
                        window=dict(first_coded_frame=0, frames=n),
                        master=dict(path=os.path.join(d, "orig.yuv"), kind="raw", first_frame=0,
                                    format=_fmt("raw", 1536, 768, "yuv420p10le", 24.0, n)),
                        available=os.path.exists(dec) and os.path.exists(os.path.join(d, "orig.yuv")),
                        gop=dict(gop=gop, minigop=mini, source="types%s.txt (parsed frame types)" % tag),
                        attrs=dict(note="GOP-breathing test clip, proxy of UNKNOWN content type (film24 by the fps rule only): %d frames, >= 2 GOPs, temporal metrics apply" % (n or 0),
                                   encode=os.path.join(d, enc))))
    return out


def _testclips():
    out = []
    for p in sorted(glob.glob(os.path.join(TESTCLIPS, "film", "*.y4m"))):
        info = eio.y4m_info(p)
        base = os.path.basename(p)
        out.append(dict(id="testclips/film/%s" % base, group="testclips_film", path=p,
                        content_types=_ctypes(base, info["fps"]), proxy="meridian" in base,
                        format=_fmt("y4m", info["width"], info["height"], info["pix_fmt"], info["fps"], info["frames"]),
                        window=dict(note="y4m, seekable by frame"), available=True, gop=None,
                        attrs=dict(note="1920x800 crop of the source, 8-bit")))
    cadence_marks = ("_32_", "_22_", "_50i_", "5994i")
    for p in sorted(glob.glob(os.path.join(TESTCLIPS, "variants", "*.mp4"))):
        base = os.path.basename(p)
        info = _probe(p)
        cad = any(m in base for m in cadence_marks)
        fps = info["fps"] if info else None
        cts = [] if cad else _ctypes(base, fps)
        out.append(dict(id="testclips/variants/%s" % base, group="testclips_variant", path=p, content_types=cts,
                        proxy=("meridian" in base) or ("speedup" in base),
                        format=_fmt("ffmpeg", info["width"], info["height"], info["pix_fmt"], fps, info["frames"]) if info else None,
                        window=dict(note="H.264 crf14 derived clip; decode a window with -ss"),
                        available=info is not None, gop=None,
                        attrs=dict(note="cadence/pulldown variant: not a fidelity-content clip" if cad else
                                   "derived variant (x264 veryfast crf14: coded, not lossless)")))
    return out


def discover():
    entries = []
    for fn in (_g18, _nn, _uvg, _breathing, _testclips):
        try:
            entries.extend(fn())
        except Exception as e:  # discovery must never hide a failure
            entries.append(dict(id="error/%s" % fn.__name__, group="error", path="", content_types=[], proxy=False,
                                format=None, window=None, available=False, gop=None, attrs=dict(error=str(e))))
    entries.sort(key=lambda e: e["id"])
    counts = {}
    for e in entries:
        for ct in e["content_types"]:
            c = counts.setdefault(ct, dict(total=0, available=0, real=0))
            c["total"] += 1
            c["available"] += 1 if e["available"] else 0
            c["real"] += 1 if (e["available"] and not e["proxy"]) else 0
    return dict(schema=1, generated_by="tools/eval/eval_material.py", content_rules=[list(x) for x in CONTENT_RULES],
                known_gaps=KNOWN_GAPS, counts_by_content_type=counts, entries=entries)
