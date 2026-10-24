#!/usr/bin/env python3
# Copyright(C) 2026 Bill Slack
# SPDX-License-Identifier: GPL-3.0-only
"""Writes the synthetic DV tunnel corpus (test/dv-synthetic, 2026-10-24).

Usage: python -I make_corpus.py <out-root> [--only name1,name2] [--list]

Each segment is a VP raw-capture corpus folder (RawCaptureCorpus.h: capture.txt,
frames.csv, frame_NNNNNN.bin; repeated pictures share one file) plus:
  truth.json   per frame: picture id, stamp, scene, what VP must show
               (picture / black / sdr / any) and the DM VP must decode;
               per picture: the patch list with codes and the reference RGB;
  planes_<p>.bin  (accuracy segments) the exact planes VP's formatter must write:
               the intensity plane (12-bit code << 4) then the alternating chroma
               plane (code << 4), little-endian uint16, width x height each.
Deterministic: the same command writes the same bytes.
"""
import hashlib
import json
import os
import sys

import numpy as np

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import dvsynth as dv  # noqa: E402

PQ = dv.nits_to_pq_code


def base_dm(rep, l1=None, trims=(), extra=(), source=(62, 3696), refresh=1):
    ext = []
    if l1 is not None:
        ext.append(dv.ext_l1(*l1))
    ext += list(trims) + list(extra)
    return {"rep": rep, "scene_refresh": refresh, "source_min_pq": source[0], "source_max_pq": source[1], "ext": ext}


def expected_dm(dm):
    """What VP must decode from an authored (well-formed) DM, as plain JSON."""
    out = {"valid": True, "source_min_pq": dm.get("source_min_pq", 62), "source_max_pq": dm.get("source_max_pq", 3696),
           "l1": None, "l2": [], "l8": [], "l5": None, "l6": None, "l11": None,
           "ycc": dm.get("ycc", dm["rep"].ycc), "off": dm.get("off", dm["rep"].off), "lms": dm.get("lms", dm["rep"].lms)}
    for level, payload in dm["ext"]:
        vals = [int.from_bytes(payload[k:k + 2], "big") for k in range(0, len(payload) - 1, 2)]
        if level == 1 and out["l1"] is None:
            out["l1"] = vals[:3]
        elif level == 2:
            out["l2"].append(vals[:7])
        elif level == 8:
            out["l8"].append([payload[0]] + [int.from_bytes(payload[1 + k:3 + k], "big") for k in range(0, 12, 2)])
        elif level == 5 and out["l5"] is None:
            out["l5"] = vals[:4]
        elif level == 6 and out["l6"] is None:
            out["l6"] = vals[:4]
        elif level == 11 and out["l11"] is None:
            out["l11"] = list(payload[:4])
    return out


def truth_line(fr):
    j = lambda v: ",".join(str(int(x)) for x in v)
    words = ["frame", "index=%d" % fr["index"], "file=%s" % fr["file"], "picture=%s" % fr["picture"],
             "stamp=%d" % fr["stamp"], "scene=%d" % fr["scene"], "expect=%s" % fr["expect"], "note=%s" % (fr.get("note") or "-").replace(" ", "_")]
    dm = fr["dm"]
    if dm is None:
        words.append("dm=none")
    elif dm.get("keep"):
        words.append("dm=keep")
    elif dm.get("tolerant"):
        words.append("dm=tolerant")
    else:
        words.append("dm=valid")
        words.append("src=%d,%d" % (dm["source_min_pq"], dm["source_max_pq"]))
        words.append("l1=%s" % (j(dm["l1"]) if dm["l1"] else "-"))
        words.append("l2n=%d" % len(dm["l2"]))
        for k, t in enumerate(dm["l2"]):
            words.append("l2_%d=%s" % (k, j(t)))
        words.append("l8n=%d" % len(dm["l8"]))
        for k, t in enumerate(dm["l8"]):
            words.append("l8_%d=%s" % (k, j(t)))
        words.append("l5=%s" % (j(dm["l5"]) if dm["l5"] else "-"))
        words.append("l6=%s" % (j(dm["l6"]) if dm["l6"] else "-"))
        words.append("l11=%s" % (j(dm["l11"]) if dm["l11"] else "-"))
        words.append("ycc=%s" % j(dm["ycc"]))
        words.append("off=%s" % j(dm["off"]))
        words.append("lms=%s" % j(dm["lms"]))
    return " ".join(words)


class Segment:
    def __init__(self, root, name, w, h, rate, encoding="r210", placement="low8", rep=dv.REP_P5, kind="tunnel",
                 keep_planes=False, note=""):
        self.root, self.name, self.w, self.h = root, name, w, h
        self.rate = rate          # (numerator, denominator)
        self.encoding, self.placement, self.rep = encoding, placement, rep
        self.kind, self.keep_planes, self.note = kind, keep_planes, note
        self.folder = os.path.join(root, name)
        os.makedirs(self.folder, exist_ok=True)
        self.frames, self.pictures, self.files = [], {}, {}
        self.picture_cache = {}

    def picture(self, pid, stamp, scene, variant="patches"):
        key = (stamp, scene, variant)
        if key not in self.picture_cache:
            I, C, patches = dv.build_picture(self.w, self.h, self.rep, stamp, scene, variant)
            self.picture_cache[key] = (I, C, patches)
            if pid not in self.pictures:
                for p in patches:
                    if "codes" in p:
                        p["reference_rgb2020_nits"] = [float(v) for v in self.rep.decode(np.array(p["codes"], dtype=np.float64))]
                self.pictures[pid] = {"variant": variant, "patches": patches, "planes": None}
        return self.picture_cache[key]

    def add(self, pid, stamp, scene, dm, header=None, expect="picture", expect_dm=None, variant="patches",
            mutate=None, corrupt=None, raw_bytes=None, note=""):
        """One frame. dm=None: no DM packet at all. header: dict for dv.packets. mutate: f(I, C)
        applied after embedding (e.g. a truncated transfer). raw_bytes: f(bytes) on the container."""
        I, C, _ = self.picture(pid, stamp, scene, variant)
        I, C = I.copy(), C.copy()
        if dm is not None:
            body = dv.dm_bytes(dm)
            hdr = dict(header or {})
            override = hdr.pop("body_override", None)
            pk = dv.packets(override if override is not None else body, **hdr)
            drop = hdr.get("_drop", None)
            ok = dv.embed(I, C, pk, corrupt=corrupt)
            assert ok, "raster too small for the DM"
        if mutate:
            mutate(I, C)
        if self.keep_planes and self.pictures[pid]["planes"] is None and dm is not None and mutate is None and not corrupt:
            planes_name = "planes_%s.bin" % pid
            with open(os.path.join(self.folder, planes_name), "wb") as f:
                f.write((I.astype("<u2") << 4).tobytes())
                f.write((C.astype("<u2") << 4).tobytes())
            self.pictures[pid]["planes"] = planes_name
            self.pictures[pid]["planes_frame"] = len(self.frames)
        data = dv.container(dv.rgb8(I, C), self.encoding, self.placement, pad_bits=len(self.frames) & 3,
                            row_pad_seed=7)
        if raw_bytes:
            data = raw_bytes(data)
        digest = hashlib.sha256(data).hexdigest()
        if digest not in self.files:
            fname = "frame_%06d.bin" % (len(self.files) + 1)
            with open(os.path.join(self.folder, fname), "wb") as f:
                f.write(data)
            self.files[digest] = fname
        self.frames.append({"index": len(self.frames), "file": self.files[digest], "picture": pid, "stamp": stamp,
                            "scene": scene, "expect": expect,
                            "dm": expect_dm if expect_dm is not None else (expected_dm(dm) if dm is not None else None),
                            "note": note})

    def finish(self, extra=None):
        num, den = self.rate
        enc_id = dv.ENCODING_IDS[self.encoding]
        rb = dv.row_bytes(self.encoding, self.w)
        with open(os.path.join(self.folder, "capture.txt"), "w", newline="\n") as f:
            f.write("format=vp-raw-capture-1\nsource=dv-synthetic (tools/dv_synth/make_corpus.py)\n")
            f.write("width=%d\nheight=%d\ninterlaced=0\nrate_numerator=%d\nrate_denominator=%d\n" % (self.w, self.h, num, den))
            f.write("rate_hz=%.6f\nencoding=%s\nencoding_id=%d\nbytes_per_row=%d\nbytes_per_frame=%d\n" %
                    (num / den, self.encoding, enc_id, rb, rb * self.h))
            if self.kind == "lldv":
                f.write("eotf=PQ\neotf_id=3\ncolorspace=BT.2020\ncolorspace_id=8\nhdr_infoframe=0\nlldv=1\n")
            else:
                f.write("eotf=SDR\neotf_id=1\ncolorspace=Rec.709\ncolorspace_id=4\nhdr_infoframe=0\n")
                f.write("capture_dolby_vision_tunnel_flag=1\n")
            f.write("dv_placement=%s\ndv_representation=%s\nnote=%s\n" % (self.placement, self.rep.name, self.note))
            f.write("never_commit=generated synthetic pixels; regenerate with make_corpus.py\n")
        period = 10_000_000 * den // num
        with open(os.path.join(self.folder, "frames.csv"), "w", newline="\n") as f:
            f.write("sequence,file,counter,timing_timestamp,capture_timestamp,arrival_qpc\n")
            for fr in self.frames:
                t = fr["index"] * period
                f.write("%d,%s,%d,%d,%d,%d\n" % (fr["index"], fr["file"], fr["index"] + 1, t, t, t + 1))
        truth = {"schema": "vp-dv-synthetic-truth-1", "name": self.name, "width": self.w, "height": self.h,
                 "rate": [num, den], "encoding": self.encoding, "placement": self.placement,
                 "representation": self.rep.name, "kind": self.kind, "note": self.note,
                 "frames": self.frames, "pictures": self.pictures}
        if extra:
            truth.update(extra)
        with open(os.path.join(self.folder, "truth.json"), "w", newline="\n") as f:
            json.dump(truth, f, indent=1)
        # The same truth as one line per frame / picture for the C++ tests (no JSON parser there).
        with open(os.path.join(self.folder, "truth.txt"), "w", newline="\n") as f:
            f.write("segment name=%s width=%d height=%d encoding=%s placement=%s representation=%s kind=%s\n" %
                    (self.name, self.w, self.h, self.encoding, self.placement, self.rep.name, self.kind))
            for pid, pic in self.pictures.items():
                f.write("picture id=%s planes=%s\n" % (pid, pic["planes"] or "-"))
            for fr in self.frames:
                f.write(truth_line(fr) + "\n")


# ------------------------------------------------------------------ segment recipes
ACC_DM = lambda rep: base_dm(rep, l1=(0, PQ(1000), PQ(48)), source=(0, PQ(1000)))


def seg_accuracy(root, name, w, h, rate, encoding, placement, rep, frames=6):
    s = Segment(root, name, w, h, rate, encoding, placement, rep, keep_planes=True,
                note="decode accuracy: patches, ramps, extremes, detail; source and L1 within 1000 nits")
    dm = ACC_DM(rep)
    for k in range(frames):
        s.add("patches%d" % k, k, 0, dm, header={"current_id": k & 15})
    s.finish()


def seg_cuts(root, name, rate, mode, w=1920, h=1080, rep=dv.REP_P5):
    """Scene cuts at known frames. mode: same (each frame carries its scene's DM), advance (the
    last frame of a scene announces the next scene's DM, the cut frame says no_md), picture (the
    picture changes with the scene too)."""
    cuts = [0, 17, 29, 30, 53, 77]           # scene starts; scene 3 is a single frame
    total = 96
    # Per scene L1 (min, max, avg) and gain: alternate bright/dim masters so tone mapping moves.
    scenes = [((0, PQ(4000), PQ(60)), 4.0), ((0, PQ(600), PQ(20)), 1.0), ((0, PQ(4000), PQ(60)), 4.0),
              ((0, PQ(200), PQ(10)), 0.5), ((0, PQ(1000), PQ(30)), 2.0), ((0, PQ(4000), PQ(60)), 4.0)]
    s = Segment(root, name, w, h, rate, "r210", "low8", rep,
                note="scene cuts at frames %s (%s metadata)" % (cuts, mode))
    scene_of = lambda f: max(i for i, c in enumerate(cuts) if c <= f)
    for f in range(total):
        sc = scene_of(f)
        l1, gain = scenes[sc]
        variant = ("scene%g" % gain) if mode == "picture" else "flat"
        dm = base_dm(rep, l1=l1, source=(62, PQ(4000)))
        sid = sc & 15
        pid = "s%d" % sc if mode == "picture" else "flat"
        if mode == "advance" and f + 1 < total and scene_of(f + 1) != sc:
            nl1, _ = scenes[sc + 1]
            ndm = base_dm(rep, l1=nl1, source=(62, PQ(4000)))
            s.add(pid, f, sc, ndm, header={"current_id": sid, "affected_id": (sid + 1) & 15},
                  expect_dm=expected_dm(dm), variant=variant, note="announces scene %d's DM" % (sc + 1))
        elif mode == "advance" and f in cuts and f > 0:
            s.add(pid, f, sc, dm, header={"current_id": sid, "no_md": True}, expect_dm=expected_dm(dm),
                  variant=variant, note="cut frame: no_md, the announced DM applies")
        else:
            s.add(pid, f, sc, dm, header={"current_id": sid}, variant=variant)
    s.finish({"cuts": cuts, "scene_l1": [x[0] for x in scenes], "scene_gain": [x[1] for x in scenes]})


def seg_metadata(root, name, rep=dv.REP_P5):
    """Metadata coverage: one DM per frame sweeping L1/L2/L5/L6/L8/L11 and the profiles."""
    s = Segment(root, name, 1920, 1080, (24000, 1001), "r210", "low8", rep,
                note="metadata coverage sweep (one authored DM per frame)")
    dms = []
    for mn, mx, av in [(0, 0, 0), (0, 4095, 2048), (62, 3696, 1800), (4095, 4095, 4095), (7, 2081, 819), (100, 3000, 3500)]:
        dms.append(("L1 %d/%d/%d" % (mn, mx, av), base_dm(rep, l1=(mn, mx, av))))
    real_l2 = dv.ext_l2(2081, 2048, 2048, 2048, 1462, 2132, 512)
    dms.append(("L2 real profile-5 trim", base_dm(rep, l1=(2, 3383, 819), trims=[real_l2], extra=[dv.ext_l6(4000, 50, 0, 0)],
                                                   source=(62, 3696))))
    trims = [dv.ext_l2(PQ(n), 2048 + 100 * k, 2048 - 50 * k, 2048 + 30 * k, 2048, 2048 + 20 * k, 2048)
             for k, n in enumerate([100, 300, 600, 1000, 2000, 4000, 48, 200])]
    for n in (1, 2, 4, 8):
        dms.append(("%d L2 trims" % n, base_dm(rep, l1=(0, PQ(4000), PQ(50)), trims=trims[:n], source=(62, PQ(4000)))))
    dms.append(("10 L2 trims (overflow)", base_dm(rep, l1=(0, PQ(4000), PQ(50)), trims=trims + trims[:2], source=(62, PQ(4000)))))
    l8s = [dv.ext_l8(k, 2048 + 10 * k, 2048, 2048, 2048, 2048, 2048, mid_clip=(2048 + k, 2048 - k) if k & 1 else None) for k in range(6)]
    dms.append(("L8 before L2", base_dm(rep, l1=(0, PQ(1000), PQ(30)), trims=l8s + trims[:2], source=(62, PQ(1000)))))
    dms.append(("L5 active area", base_dm(rep, l1=(0, PQ(1000), PQ(30)), extra=[dv.ext_l5(0, 0, 140, 140)])))
    dms.append(("L5 max offsets", base_dm(rep, l1=(0, PQ(1000), PQ(30)), extra=[dv.ext_l5(8191, 8191, 8191, 8191)])))
    dms.append(("L6 mastering", base_dm(rep, l1=(0, PQ(1000), PQ(30)), extra=[dv.ext_l6(1000, 1, 1000, 400)])))
    dms.append(("L11 movies reference", base_dm(rep, l1=(0, PQ(1000), PQ(30)), extra=[dv.ext_l11(1, 0, True)])))
    dms.append(("source range 7..3079", base_dm(rep, l1=(7, 3079, 1000), source=(7, 3079))))
    dms.append(("every level", base_dm(rep, l1=(0, PQ(1000), PQ(30)), trims=[real_l2, l8s[1]],
                                        extra=[dv.ext_l5(0, 0, 280, 280), dv.ext_l6(1000, 1, 1000, 400), dv.ext_l11(1, 0, True),
                                               (3, bytes([8, 0, 8, 0, 8, 0])), (9, bytes([2])), (10, bytes([255, 11, 179, 0, 0])),
                                               (254, bytes([0, 2]))])))
    # A long DM: 40 L8 blocks (multi-packet, ~13 packets).
    many = [dv.ext_l8(k % 30, 2048, 2048, 2048, 2048, 2048, 2048, mid_clip=(2048, 2048)) for k in range(40)]
    dms.append(("40 L8 blocks (long DM)", base_dm(rep, l1=(0, PQ(1000), PQ(30)), trims=[real_l2] + many)))
    # Profiles alternating (the matrices change): p8 YCbCr matrices on a p5 picture are still a valid DM.
    dms.append(("p8 matrices", dict(base_dm(dv.REP_P8, l1=(0, PQ(1000), PQ(30))), rep=dv.REP_P8)))
    for k, (label, dm) in enumerate(dms):
        s.add("pic", 0, 0, dm, header={"current_id": k & 15}, note=label)
    s.finish({"labels": [d[0] for d in dms]})


def seg_robustness(root, name):
    """Missing / malformed / truncated / extreme metadata; VP must show black (never packed
    pixels or a crash), keep or drop metadata as documented, and recover."""
    rep = dv.REP_P5
    s = Segment(root, name, 1920, 1080, (24000, 1001), "r210", "low8", rep,
                note="robustness: each block of 8 frames is one fault, then 4 good frames (recovery)")
    good = base_dm(rep, l1=(0, PQ(1000), PQ(30)), source=(62, PQ(1000)))
    labels = []
    f = [0]
    cid = [0]

    def add(dm, header=None, expect="picture", expect_dm=None, label="", **kw):
        h = dict(header or {})
        h.setdefault("current_id", cid[0] & 15)
        s.add("pic", 0, 0, dm, header=h, expect=expect, expect_dm=expect_dm, note=label, **kw)
        labels.append(label)
        f[0] += 1

    def good_frames(n, label="good"):
        for _ in range(n):
            add(good, label=label)

    def bad_l1(**over):
        d = base_dm(rep, l1=(0, PQ(1000), PQ(30)), source=(62, PQ(1000)))
        d.update(over)
        return d

    good_frames(8, "lock")
    # 1. DM missing for 8 frames (no packet): black, then the picture back.
    for _ in range(8):
        add(None, expect="black", label="no DM packet")
    good_frames(4, "recovery after missing DM")
    # 2. One damaged copy of the packet: still decoded (two good copies).
    for _ in range(4):
        add(good, label="one copy damaged", corrupt={(0, 0)})
    # 3. All copies damaged: black.
    for _ in range(4):
        add(good, expect="black", label="all copies damaged", corrupt={(0, 0), (0, 1), (0, 2)})
    good_frames(4, "recovery after CRC loss")
    # 4. no_md frames after lock: the cached DM stays.
    for _ in range(6):
        add(good, header={"no_md": True}, expect="picture", expect_dm=expected_dm(good), label="no_md keeps DM")
    # 5. Truncated multi-packet DM (the last continuation packet never arrives).
    long_dm = base_dm(rep, l1=(0, PQ(1000), PQ(30)), trims=[dv.ext_l8(k, mid_clip=(2048, 2048)) for k in range(12)])
    pk = dv.packets(dv.dm_bytes(long_dm), current_id=cid[0])

    def truncate_dm(I, C, n=len(pk)):
        # Overwrite the last packet's pixels with a plain picture (no valid packet there).
        start = (n - 1) * 3072
        flatI, flatC = I.reshape(-1), C.reshape(-1)
        flatC[start:start + 3072] ^= 1
    for _ in range(4):
        add(long_dm, expect="any", expect_dm={"keep": True}, label="truncated DM (last packet lost)", mutate=truncate_dm)
    good_frames(4, "recovery after truncated DM")
    # 6. Malformed bodies (CRC-valid packets with bad content).
    mal = [
        ("ext block length beyond the body", bad_l1(ext_length_override={1: 4000})),
        ("L1 out of range (5000)", base_dm(rep, l1=(0, 5000, 30), source=(62, PQ(1000)))),
        ("L1 wrong length (4 bytes)", dict(good, ext=[(1, dv.be16(0) + dv.be16(PQ(1000)))])),
        ("num_ext_blocks 255, none present", bad_l1(num_ext_override=255)),
        ("signal_bit_depth 14", bad_l1(bit_depth=14)),
        ("signal_color_space 1", bad_l1(color_space=1)),
        ("unknown level 200 + L1", dict(good, ext=[(200, bytes(40))] + good["ext"])),
        ("zero-length block + L1", dict(good, ext=[(4, b"")] + good["ext"])),
        ("L2 field out of range", dict(good, ext=good["ext"] + [dv.ext_l2(2081, 5000, 2048, 2048, 2048, 2048, 2048)])),
        ("L6 above 10000", dict(good, ext=good["ext"] + [dv.ext_l6(20000, 1, 20000, 400)])),
    ]
    for label, dm in mal:
        expect = "any"
        for _ in range(3):
            add(dm, expect=expect, expect_dm={"tolerant": True}, label=label)
        good_frames(2, "recovery after " + label)
    # 7. Extreme but well-formed values.
    extremes = [
        ("matrices int16 extremes", dict(good, ycc=[32767, -32768, 32767, -32768, 32767, -32768, 32767, -32768, 32767],
                                        lms=[32767, -32768, 0, 0, 32767, -32768, -32768, 0, 32767])),
        ("offsets huge", dict(good, off=[0xFFFFFFFF, 0xFFFFFFFF, 0])),
        ("source min > max", dict(good, source_min_pq=4000, source_max_pq=100)),
        ("source max 0", dict(good, source_min_pq=0, source_max_pq=0)),
        ("L1 min > max > avg inverted", base_dm(rep, l1=(4095, 0, 4095), source=(62, PQ(1000)))),
    ]
    for label, dm in extremes:
        for _ in range(3):
            add(dm, expect="any", expect_dm={"tolerant": True}, label=label)
        good_frames(2, "recovery after " + label)
    # 8. Declared length larger than the raster can carry (0x2F00 bound).
    add(good, header={"length_override": 0x3000}, expect="any", expect_dm={"tolerant": True}, label="declared length 0x3000")
    good_frames(3, "recovery after declared length")
    # 9. Truncated frame: the tail never written (last rows zero).
    def zero_tail(I, C):
        I[-1, :] = 0
        C[-1, :] = 0
    for _ in range(3):
        add(good, expect="hold", label="truncated transfer (last row blank)", mutate=zero_tail)
    good_frames(3, "recovery after truncated transfer")
    # 10. Every other frame without DM (flicker source): black on those, picture on the others.
    for k in range(8):
        if k & 1:
            add(None, expect="black", label="alternate frames without DM")
        else:
            add(good, label="alternate frames with DM")
    good_frames(3, "recovery after alternating loss")
    # 11. Metadata id wrap-around 13..2.
    for k in range(6):
        cid[0] = (13 + k) & 15
        add(good, label="id wrap %d" % cid[0])
    cid[0] = 0
    # 12. Long loss (30 frames, beyond the 24-picture metadata limit), then fresh DM.
    for _ in range(30):
        add(None, expect="black", label="long DM loss (30)")
    good_frames(6, "recovery after long loss")
    # 13. EOS, SDR pictures (no tunnel content: plain RGB), back to DV.
    add(good, header={"eos": True}, expect="any", label="EOS")

    def plain_rgb(data, ww=1920, hh=1080):
        return data
    sdr_pic = np.full((1080, 1920, 3), 61, dtype=np.uint8)
    sdr_pic[:, ::2, 1] = 181
    sdr_bytes = dv.container(sdr_pic, "r210", "low8", row_pad_seed=7)
    for _ in range(6):
        add(None, expect="sdr", label="SDR after EOS", raw_bytes=lambda d: sdr_bytes)
    good_frames(6, "DV again after SDR")
    s.finish({"labels": labels})


def seg_policy(root, name, rep=dv.REP_P5, trims="real", w=1920, h=1080):
    """Policy at the projector's level: a 4000-nit master with L1 per scene and (optionally) L2/L8
    trims at 100/600/1000 nits. Scenes: bright (L1 4000), mid (1000), dim (200)."""
    s = Segment(root, name, w, h, (24000, 1001), "r210", "low8", rep,
                note="policy: 4000-nit master, L1 per scene, trims=%s" % trims)
    # Trims a colourist might set: the 100-nit pass lifts mids and desaturates a little, the
    # 600 / 1000-nit passes are milder. (slope, offset, power, chroma_weight, saturation)
    sets = {
        "real": [dv.ext_l2(PQ(100), 2048 + 160, 2048 + 40, 2048 - 200, 2048 - 120, 2048 - 160, 2048),
                 dv.ext_l2(PQ(600), 2048 + 60, 2048 + 10, 2048 - 80, 2048 - 40, 2048 - 60, 2048),
                 dv.ext_l2(PQ(1000), 2048 + 30, 2048, 2048 - 40, 2048 - 20, 2048 - 30, 2048)],
        "none": [],
        "100only": [dv.ext_l2(PQ(100), 2048 + 160, 2048 + 40, 2048 - 200, 2048 - 120, 2048 - 160, 2048)],
        "neutral": [dv.ext_l2(PQ(100)), dv.ext_l2(PQ(600)), dv.ext_l2(PQ(1000))],
    }
    tr = sets[trims]
    scenes = [("bright", 4.0, (0, PQ(4000), PQ(80))), ("mid", 1.0, (0, PQ(1000), PQ(25))), ("dim", 0.2, (0, PQ(200), PQ(5)))]
    for k, (label, gain, l1) in enumerate(scenes):
        dm = base_dm(rep, l1=l1, trims=tr, extra=[dv.ext_l6(4000, 50, 0, 0)], source=(62, PQ(4000)))
        s.add("scene_%s" % label, k, k, dm, header={"current_id": k}, variant="scene%g" % gain, note=label)
    s.finish({"scenes": [{"label": a, "gain": g, "l1": list(l)} for a, g, l in scenes], "trims": trims})


def seg_btb(root, name, rep=dv.REP_P5):
    """BTB/WTW-like: the extremes row against a DM whose source range is narrower than the
    codes used (source 62..3696; codes 0..4095), L1 max below the brightest codes."""
    s = Segment(root, name, 1920, 1080, (24000, 1001), "r210", "low8", rep, keep_planes=True,
                note="extremes below the source minimum and above the source maximum / L1 max")
    dm = base_dm(rep, l1=(62, 3696, PQ(30)), source=(62, 3696))
    s.add("extremes", 0, 0, dm, header={"current_id": 1})
    s.finish()


def seg_lldv(root, name, src_seg_maker, target_nits, w=1920, h=1080, rep=dv.REP_P5, variant="patches", l1=None):
    """An LLDV-like companion: the same content as the source as a PQ BT.2020 YCbCr 4:2:2 10-bit
    limited V210 picture, tone mapped by the reference DM model to target_nits (None = not
    mapped: an HDR10-like carriage of the same light, to isolate the precision)."""
    import dm_model
    s = Segment(root, name, w, h, (24000, 1001), "V210", "n/a", rep, kind="lldv",
                note="LLDV-like companion (V210 PQ BT.2020 limited), target %s" % target_nits)
    I, C, patches = dv.build_picture(w, h, rep, 0, 0, variant)
    P = C[:, 0::2].astype(np.float64).repeat(2, axis=1)
    T = C[:, 1::2].astype(np.float64).repeat(2, axis=1)
    rgb = rep.decode(np.stack([I.astype(np.float64), P, T], axis=-1))
    if target_nits is not None:
        rgb = dm_model.reference_dm(rgb, l1=l1, target_nits=target_nits)
    rgb = np.clip(rgb, 0.0, 10000.0)
    rp = dv.nits_to_pq(rgb)
    kr, kb = 0.2627, 0.0593
    y = kr * rp[..., 0] + (1 - kr - kb) * rp[..., 1] + kb * rp[..., 2]
    cb = (rp[..., 2] - y) / (2 * (1 - kb))
    cr = (rp[..., 0] - y) / (2 * (1 - kr))
    Y = np.clip(np.rint(64 + 876 * y), 4, 1019).astype(np.uint32)
    cbm = 0.5 * (cb[:, 0::2] + cb[:, 1::2])
    crm = 0.5 * (cr[:, 0::2] + cr[:, 1::2])
    Cb = np.clip(np.rint(512 + 896 * cbm), 4, 1019).astype(np.uint32)
    Cr = np.clip(np.rint(512 + 896 * crm), 4, 1019).astype(np.uint32)
    data = dv.v210(Y, Cb, Cr)
    with open(os.path.join(s.folder, "frame_000001.bin"), "wb") as f:
        f.write(data)
    s.files["x"] = "frame_000001.bin"
    for k in range(4):
        s.frames.append({"index": k, "file": "frame_000001.bin", "picture": "lldv", "stamp": 0, "scene": 0,
                         "expect": "picture", "dm": None, "note": "LLDV-like"})
    for p in patches:
        if "codes" in p:
            p["reference_rgb2020_nits"] = [float(v) for v in rep.decode(np.array(p["codes"], dtype=np.float64))]
    s.pictures["lldv"] = {"variant": variant, "patches": patches, "planes": None}
    s.finish({"lldv_target_nits": target_nits, "l1": l1})


def recipes(root):
    r = {}
    r["acc-1080p24-p5-r210-low8"] = lambda: seg_accuracy(root, "acc-1080p24-p5-r210-low8", 1920, 1080, (24000, 1001), "r210", "low8", dv.REP_P5)
    r["acc-1080p24-p5-r210-high8"] = lambda: seg_accuracy(root, "acc-1080p24-p5-r210-high8", 1920, 1080, (24000, 1001), "r210", "high8", dv.REP_P5)
    r["acc-1080p24-p5-r210-high8rep"] = lambda: seg_accuracy(root, "acc-1080p24-p5-r210-high8rep", 1920, 1080, (24000, 1001), "r210", "high8rep", dv.REP_P5)
    r["acc-1080p30-p5-R10b-low8"] = lambda: seg_accuracy(root, "acc-1080p30-p5-R10b-low8", 1920, 1080, (30000, 1001), "R10b", "low8", dv.REP_P5)
    r["acc-1080p60-p5-R10l-high8"] = lambda: seg_accuracy(root, "acc-1080p60-p5-R10l-high8", 1920, 1080, (60000, 1001), "R10l", "high8", dv.REP_P5)
    r["acc-1080p24-p8-r210-low8"] = lambda: seg_accuracy(root, "acc-1080p24-p8-r210-low8", 1920, 1080, (24000, 1001), "r210", "low8", dv.REP_P8)
    r["acc-2160p24-p5-r210-low8"] = lambda: seg_accuracy(root, "acc-2160p24-p5-r210-low8", 3840, 2160, (24000, 1001), "r210", "low8", dv.REP_P5, frames=3)
    r["acc-2160p30-p5-R10b-high8"] = lambda: seg_accuracy(root, "acc-2160p30-p5-R10b-high8", 3840, 2160, (30000, 1001), "R10b", "high8", dv.REP_P5, frames=2)
    r["acc-2160p60-p5-r210-high8"] = lambda: seg_accuracy(root, "acc-2160p60-p5-r210-high8", 3840, 2160, (60000, 1001), "r210", "high8", dv.REP_P5, frames=4)
    r["cuts-1080p24-same"] = lambda: seg_cuts(root, "cuts-1080p24-same", (24000, 1001), "same")
    r["cuts-1080p24-advance"] = lambda: seg_cuts(root, "cuts-1080p24-advance", (24000, 1001), "advance")
    r["cuts-1080p24-picture"] = lambda: seg_cuts(root, "cuts-1080p24-picture", (24000, 1001), "picture")
    r["cuts-1080p60-same"] = lambda: seg_cuts(root, "cuts-1080p60-same", (60000, 1001), "same")
    r["meta-1080p24-p5"] = lambda: seg_metadata(root, "meta-1080p24-p5")
    r["robust-1080p24"] = lambda: seg_robustness(root, "robust-1080p24")
    r["policy-1080p24-real"] = lambda: seg_policy(root, "policy-1080p24-real", trims="real")
    r["policy-1080p24-none"] = lambda: seg_policy(root, "policy-1080p24-none", trims="none")
    r["policy-1080p24-100only"] = lambda: seg_policy(root, "policy-1080p24-100only", trims="100only")
    r["policy-1080p24-neutral"] = lambda: seg_policy(root, "policy-1080p24-neutral", trims="neutral")
    r["btb-1080p24"] = lambda: seg_btb(root, "btb-1080p24")
    r["lldv-1080p24-unmapped"] = lambda: seg_lldv(root, "lldv-1080p24-unmapped", None, None)
    r["lldv-1080p24-bright-1000"] = lambda: seg_lldv(root, "lldv-1080p24-bright-1000", None, 1000.0, variant="scene4",
                                                     l1=(0, PQ(4000), PQ(80)))
    r["lldv-1080p24-bright-100"] = lambda: seg_lldv(root, "lldv-1080p24-bright-100", None, 100.0, variant="scene4",
                                                    l1=(0, PQ(4000), PQ(80)))
    return r


def main():
    if len(sys.argv) < 2:
        print(__doc__)
        return 2
    root = sys.argv[1]
    r = recipes(root)
    if "--list" in sys.argv:
        print("\n".join(r))
        return 0
    only = None
    if "--only" in sys.argv:
        only = set(sys.argv[sys.argv.index("--only") + 1].split(","))
    os.makedirs(root, exist_ok=True)
    for name, make in r.items():
        if only and name not in only:
            continue
        print("writing", name, flush=True)
        make()
    return 0


if __name__ == "__main__":
    sys.exit(main())
