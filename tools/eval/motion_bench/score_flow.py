# Copyright(C) 2026 VP-V contributors
# SPDX-License-Identifier: GPL-3.0-only
"""Score a predicted optical flow against a ground truth (Middlebury .flo). numpy + stdlib only.

  python -I score_flow.py --pred A.flo --gt B.flo [--camera-labels labels.npy|.csv] [--object-mask mask.png|.npy]
                          [--small-mask small.npy|.png] [--prev-pred P.flo [--prev-gt G.flo]] --out scores.csv
  python -I score_flow.py --self-test

Camera labels: integer array (H,W) or block grid (divides H,W; upsampled by repeat): 1 = camera, 2 = object, else ignored.
Object mask: occlusion-boundary pixels, nonzero = on a boundary (.npy, .csv or 8-bit non-interlaced .png).
Small mask: pixels of small fast objects (<= 32 px, >= 20 px/frame); metric prefix "smallfast".
"""
import argparse
import hashlib
import os
import struct
import sys
import tempfile
import zlib

import numpy as np

FLO_MAGIC = 202021.25
OUTLIER_PX = 3.0
SUBPIXEL_PX = 2.0


def read_flo(path):
    with open(path, "rb") as f:
        magic = struct.unpack("<f", f.read(4))[0]
        if magic != np.float32(FLO_MAGIC):
            raise ValueError("%s: bad .flo magic %r" % (path, magic))
        w, h = struct.unpack("<ii", f.read(8))
        if w <= 0 or h <= 0 or w > 65536 or h > 65536:
            raise ValueError("%s: bad size %dx%d" % (path, w, h))
        data = np.frombuffer(f.read(w * h * 8), dtype="<f4")
    if data.size != w * h * 2:
        raise ValueError("%s: truncated" % path)
    return data.reshape(h, w, 2).astype(np.float64)


def write_flo(path, flow):
    h, w = flow.shape[:2]
    with open(path, "wb") as f:
        f.write(struct.pack("<f", FLO_MAGIC))
        f.write(struct.pack("<ii", w, h))
        f.write(np.ascontiguousarray(flow, dtype="<f4").tobytes())


def _read_png_gray(path):
    with open(path, "rb") as f:
        raw = f.read()
    if raw[:8] != b"\x89PNG\r\n\x1a\n":
        raise ValueError("%s: not a PNG" % path)
    pos, idat, hdr = 8, b"", None
    while pos < len(raw):
        ln, typ = struct.unpack(">I4s", raw[pos:pos + 8])
        body = raw[pos + 8:pos + 8 + ln]
        pos += 12 + ln
        if typ == b"IHDR":
            hdr = struct.unpack(">IIBBBBB", body)
        elif typ == b"IDAT":
            idat += body
    w, h, depth, ctype, _, _, interlace = hdr
    if depth != 8 or interlace != 0 or ctype not in (0, 2, 4, 6):
        raise ValueError("%s: only 8-bit non-interlaced gray/RGB(A) PNG supported" % path)
    ch = {0: 1, 2: 3, 4: 2, 6: 4}[ctype]
    stride = w * ch
    d = zlib.decompress(idat)
    out = np.zeros((h, stride), dtype=np.int32)
    prev = np.zeros(stride, dtype=np.int32)
    p = 0
    for y in range(h):
        ft = d[p]
        line = np.frombuffer(d, dtype=np.uint8, count=stride, offset=p + 1).astype(np.int32)
        p += 1 + stride
        if ft == 0:
            cur = line
        elif ft == 2:
            cur = (line + prev) & 255
        else:  # sub/average/paeth are sequential in x
            cur = np.zeros(stride, dtype=np.int32)
            for x in range(stride):
                a = cur[x - ch] if x >= ch else 0
                b = prev[x]
                c = prev[x - ch] if x >= ch else 0
                if ft == 1:
                    pr = a
                elif ft == 3:
                    pr = (a + b) >> 1
                elif ft == 4:
                    pa, pb, pc = abs(b - c), abs(a - c), abs(a + b - 2 * c)
                    pr = a if (pa <= pb and pa <= pc) else (b if pb <= pc else c)
                else:
                    raise ValueError("bad PNG filter")
                cur[x] = (line[x] + pr) & 255
        out[y] = cur
        prev = cur
    return out.reshape(h, w, ch)[:, :, 0]


def read_array(path):
    ext = os.path.splitext(path)[1].lower()
    if ext == ".npy":
        return np.load(path, allow_pickle=False)
    if ext == ".csv":
        return np.loadtxt(path, delimiter=",")
    if ext == ".png":
        return _read_png_gray(path)
    raise ValueError("unsupported array file %s" % path)


def fit_to(arr, h, w, name):
    arr = np.asarray(arr)
    if arr.shape == (h, w):
        return arr
    if arr.ndim == 2 and h % arr.shape[0] == 0 and w % arr.shape[1] == 0:
        return np.repeat(np.repeat(arr, h // arr.shape[0], axis=0), w // arr.shape[1], axis=1)
    raise ValueError("%s shape %s does not match %dx%d" % (name, arr.shape, h, w))


def _sample(field, xs, ys):
    """Bilinear sample of field (H,W,2) at float coords, clamped to the border."""
    h, w = field.shape[:2]
    xs = np.clip(xs, 0, w - 1)
    ys = np.clip(ys, 0, h - 1)
    x0 = np.floor(xs).astype(int)
    y0 = np.floor(ys).astype(int)
    x1 = np.minimum(x0 + 1, w - 1)
    y1 = np.minimum(y0 + 1, h - 1)
    fx = (xs - x0)[..., None]
    fy = (ys - y0)[..., None]
    return ((field[y0, x0] * (1 - fx) + field[y0, x1] * fx) * (1 - fy)
            + (field[y1, x0] * (1 - fx) + field[y1, x1] * fx) * fy)


def temporal_inconsistency(prev, cur):
    """Mean |cur(x) - prev(x - prev(x))|: previous flow warped by itself, against the current flow."""
    h, w = cur.shape[:2]
    gy, gx = np.mgrid[0:h, 0:w].astype(np.float64)
    warped = _sample(prev, gx - prev[..., 0], gy - prev[..., 1])
    d = cur - warped
    return float(np.mean(np.hypot(d[..., 0], d[..., 1])))


def _stats(epe, sel, prefix, out):
    n = int(np.count_nonzero(sel))
    out.append((prefix + "_n", n))
    if n:
        e = epe[sel]
        out.append((prefix + "_epe", float(np.mean(e))))
        out.append((prefix + "_outlier_pct", float(100.0 * np.mean(e > OUTLIER_PX))))
    else:
        out.append((prefix + "_epe", "nan"))
        out.append((prefix + "_outlier_pct", "nan"))


def score(pred, gt, labels=None, objmask=None, prev_pred=None, prev_gt=None, smallmask=None):
    if pred.shape != gt.shape:
        raise ValueError("pred/gt size mismatch %s vs %s" % (pred.shape, gt.shape))
    h, w = gt.shape[:2]
    d = pred - gt
    epe = np.hypot(d[..., 0], d[..., 1])
    valid = np.isfinite(epe) & (np.abs(gt[..., 0]) < 1e9) & (np.abs(gt[..., 1]) < 1e9)  # Middlebury unknown = 1e9
    rows = []
    _stats(epe, valid, "all", rows)
    if labels is not None:
        lab = fit_to(labels, h, w, "camera-labels")
        _stats(epe, valid & (lab == 1), "camera", rows)
        _stats(epe, valid & (lab == 2), "object", rows)
    mag = np.hypot(gt[..., 0], gt[..., 1])
    _stats(epe, valid & (mag < SUBPIXEL_PX), "subpixel", rows)
    if objmask is not None:
        m = fit_to(objmask, h, w, "object-mask") != 0
        _stats(epe, valid & m, "occlusion_boundary", rows)
    if smallmask is not None:
        _stats(epe, valid & (fit_to(smallmask, h, w, "small-mask") != 0), "smallfast", rows)
    if prev_pred is not None:
        tcp = temporal_inconsistency(prev_pred, pred)
        rows.append(("tc_pred", tcp))
        if prev_gt is not None:
            tcg = temporal_inconsistency(prev_gt, gt)
            rows.append(("tc_gt", tcg))
            rows.append(("tc_excess", tcp - tcg))
    return rows


def render(rows):
    lines = ["metric,value"]
    for k, v in rows:
        lines.append("%s,%s" % (k, v if isinstance(v, (str, int)) else "%.9g" % v))
    return ("\n".join(lines) + "\n").encode("ascii")


def run(args):
    def once():
        pred, gt = read_flo(args.pred), read_flo(args.gt)
        labels = read_array(args.camera_labels) if args.camera_labels else None
        mask = read_array(args.object_mask) if args.object_mask else None
        small = read_array(args.small_mask) if getattr(args, "small_mask", None) else None
        pp = read_flo(args.prev_pred) if args.prev_pred else None
        pg = read_flo(args.prev_gt) if args.prev_gt else None
        if pg is not None and pp is None:
            raise ValueError("--prev-gt needs --prev-pred")
        return render(score(pred, gt, labels, mask, pp, pg, small))
    a = once()
    b = once()
    ha, hb = hashlib.sha256(a).hexdigest(), hashlib.sha256(b).hexdigest()
    if ha != hb:
        print("NONDETERMINISTIC: %s != %s" % (ha, hb), file=sys.stderr)
        return 1
    with open(args.out, "wb") as f:
        f.write(a)
    print("sha256=%s (run twice, identical) -> %s" % (ha, args.out))
    return 0


def self_test():
    h, w = 48, 64
    ys, xs = np.mgrid[0:h, 0:w]
    ok = True

    def check(name, got, want, tol=1e-9):
        nonlocal ok
        if not abs(got - want) <= tol:
            ok = False
            print("FAIL %s: got %r want %r" % (name, got, want), file=sys.stderr)

    with tempfile.TemporaryDirectory(prefix="score_flow_selftest_") as td:
        p = lambda n: os.path.join(td, n)
        # case 1: constant 1 px offset -> EPE exactly 1.0 (values are multiples of 0.25: exact in float32)
        gt = np.stack([(xs % 8) * 0.25 + 1.0, (ys % 4) * 0.5], axis=-1)
        write_flo(p("gt1.flo"), gt)
        write_flo(p("pr1.flo"), gt + np.array([1.0, 0.0]))
        ns = argparse.Namespace(pred=p("pr1.flo"), gt=p("gt1.flo"), camera_labels=None, object_mask=None,
                                prev_pred=None, prev_gt=None, out=p("s1.csv"))
        if run(ns) != 0:
            return 1
        r = dict(score(read_flo(ns.pred), read_flo(ns.gt)))
        check("case1 epe_all", r["all_epe"], 1.0)
        check("case1 outlier", r["all_outlier_pct"], 0.0)

        # case 2: left half camera (gt u=4; error (3,4) -> EPE 5), right half object (gt (.5,.25); error (0,.5) -> .5)
        gt2 = np.zeros((h, w, 2))
        gt2[:, : w // 2, 0] = 4.0
        gt2[:, w // 2:, 0] = 0.5
        gt2[:, w // 2:, 1] = 0.25
        pr2 = gt2.copy()
        pr2[:, : w // 2] += np.array([3.0, 4.0])
        pr2[:, w // 2:, 1] += 0.5
        write_flo(p("gt2.flo"), gt2)
        write_flo(p("pr2.flo"), pr2)
        np.save(p("lab.npy"), np.where(xs < w // 2, 1, 2))
        mask = np.zeros((h, w), dtype=np.uint8)
        mask[:, w // 2 + 4: w // 2 + 8] = 255
        np.save(p("mask.npy"), mask)
        r = dict(score(read_flo(p("pr2.flo")), read_flo(p("gt2.flo")), read_array(p("lab.npy")), read_array(p("mask.npy"))))
        check("case2 all", r["all_epe"], 2.75)
        check("case2 camera", r["camera_epe"], 5.0)
        check("case2 object", r["object_epe"], 0.5)
        check("case2 camera outlier", r["camera_outlier_pct"], 100.0)
        check("case2 object outlier", r["object_outlier_pct"], 0.0)
        check("case2 all outlier", r["all_outlier_pct"], 50.0)
        check("case2 subpixel", r["subpixel_epe"], 0.5)
        check("case2 subpixel n", r["subpixel_n"], h * (w // 2), 0)
        check("case2 occlusion", r["occlusion_boundary_epe"], 0.5)
        check("case2 occlusion n", r["occlusion_boundary_n"], h * 4, 0)

        sm = np.zeros((h, w), dtype=np.uint8)
        sm[:8, :16] = 255  # inside the camera half, EPE 5
        r = dict(score(read_flo(p("pr2.flo")), read_flo(p("gt2.flo")), smallmask=sm))
        check("case2b smallfast", r["smallfast_epe"], 5.0)
        check("case2b smallfast n", r["smallfast_n"], 128, 0)

        # case 3: previous flow constant (1,0), current (1.5,0): warped previous = (1,0) -> 0.5; gt consistent -> excess 0.5
        prev = np.zeros((h, w, 2)); prev[..., 0] = 1.0
        cur = np.zeros((h, w, 2)); cur[..., 0] = 1.5
        r = dict(score(cur, prev, prev_pred=prev, prev_gt=prev))
        check("case3 tc_pred", r["tc_pred"], 0.5)
        check("case3 tc_gt", r["tc_gt"], 0.0)
        check("case3 tc_excess", r["tc_excess"], 0.5)

        # case 4: .flo round trip
        check("case4 roundtrip", float(np.max(np.abs(read_flo(p("gt1.flo")) - gt))), 0.0)
    print("self-test %s" % ("PASS" if ok else "FAIL"))
    return 0 if ok else 1


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--pred")
    ap.add_argument("--gt")
    ap.add_argument("--camera-labels")
    ap.add_argument("--object-mask")
    ap.add_argument("--small-mask")
    ap.add_argument("--prev-pred")
    ap.add_argument("--prev-gt")
    ap.add_argument("--out")
    ap.add_argument("--self-test", action="store_true")
    a = ap.parse_args(argv)
    if a.self_test:
        return self_test()
    if not (a.pred and a.gt and a.out):
        ap.error("--pred, --gt and --out are required")
    return run(a)


if __name__ == "__main__":
    sys.exit(main())
