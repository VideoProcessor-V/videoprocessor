# Copyright(C) 2026 VP-V contributors
# SPDX-License-Identifier: GPL-3.0-only
"""Regionally adaptive TemporalDetail: transfer ceiling, per-block oracle, and a fitted per-block strength rule.

The model is run at its transfer ceiling (Model::Tuning::forceWeight = 1: every block with a selected reference
transfers; projection, coherence, grain budget and guard still apply), giving per pixel the most it would add,
D = out_force - in. A per-block strength a_b in [0, 1] then sets out = in + a_b D (block strengths interpolated
bilinearly between block centres, as the model's own weights are).

  oracle   a_b* = clamp(<truth - in, D>_b / <D, D>_b, 0, 1)   (uses the master: an upper bound, not a rule)
  rule     a_b  = clamp(c . f_b, 0, 1) from LOCAL evidence only (no master), fitted by weighted least squares
           on a_b* (weights <D, D>_b) over training cases and scored on held-out cases:
             deficit   1 - E1(in) / max over the last 4 frames of E1(frame warped to now)   (texture lost here)
             signif    log(max E1 warped / grain level)                                     (the reference has real detail)
             trust     per-block vector trust of the backward pair (td_flow; later the camera model's confidence)
             grain     log(1 + grain level) (quietest sub-blocks of the fine band)
             dark      1 - block mean / 255 (8-bit equivalent)
             phase     frame-mean deficit (the I/B quality phase, measured, no GOP assumed)
E1 = mean square of the fine band Y - G(1 px) Y; warps use chained DIS backward flow (the stand-in vectors).

  python -I tools/eval/td_regional.py collect --suite <suite.json> --out <dir> [--max-frames 48]
  python -I tools/eval/td_regional.py fit --data <dir> --train tos,boat --out tools/eval/prototypes/.../regional.md
"""
import argparse
import glob
import json
import math
import os
import sys

import numpy as np
from scipy import ndimage

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
sys.path.insert(0, os.path.join(HERE, "..", "td-cli"))

import eval_harness as eh  # noqa: E402
import candidates_temporal_detail as ctd  # noqa: E402

B = 16
FEATURES = ["bias", "deficit", "signif", "trust", "grain", "dark", "phase"]


def fine(a):
    return a - ndimage.gaussian_filter(a, 1.0, mode="nearest")


def block_mean(a, b=B):
    h, w = a.shape
    gh, gw = -(-h // b), -(-w // b)
    p = np.pad(a, ((0, gh * b - h), (0, gw * b - w)), mode="edge")
    return p.reshape(gh, b, gw, b).mean(axis=(1, 3))


def grain_level(f1, b=B, sub=4):
    """Per block: mean of the 4 smallest sub-block variances of the fine band (texture_restore's estimator shape)."""
    h, w = f1.shape
    gh, gw = -(-h // b), -(-w // b)
    p = np.pad(f1, ((0, gh * b - h), (0, gw * b - w)), mode="edge")
    k = b // sub
    v = p.reshape(gh, k, sub, gw, k, sub).var(axis=(2, 5))          # (gh, k, gw, k)
    v = np.sort(v.transpose(0, 2, 1, 3).reshape(gh, gw, k * k), axis=2)
    return v[:, :, :4].mean(axis=2)


def upsample_blocks(a, shape, b=B):
    """Block values at block centres -> per pixel, bilinear (edges clamped)."""
    h, w = shape
    gy = (np.arange(h) + 0.5) / b - 0.5
    gx = (np.arange(w) + 0.5) / b - 0.5
    return ndimage.map_coordinates(a, np.meshgrid(gy, gx, indexing="ij"), order=1, mode="nearest")


class Features:
    """Per-block local evidence from the input stream only (no master)."""

    def __init__(self):
        import td_flow
        self.flow = td_flow
        self.hist = []          # (Y8 float, E1 map) of the last frames
        self.flows = []         # dense backward flows cur -> prev for consecutive pairs

    def push(self, frame):
        import cv2
        Y = np.asarray(frame.Y, np.float64) / (1 << (frame.bitdepth - 8))
        y8 = np.clip(np.floor(Y + 0.5), 0, 255).astype(np.uint8)
        f1 = fine(Y)
        e1 = block_mean(f1 * f1)
        n = grain_level(f1)
        trust = np.ones_like(e1)
        if self.hist:
            fb = self.flow.dense_flow(y8, self.hist[-1][0])
            self.flows.append(fb)
            trust = self.flow.block_reduce(fb, B)[2].astype(np.float64)
        h, w = Y.shape
        best = np.zeros_like(e1)
        if self.hist:
            yy, xx = np.mgrid[0:h, 0:w].astype(np.float32)
            px, py = xx.copy(), yy.copy()
            for k in range(1, min(4, len(self.hist)) + 1):
                fl = self.flows[-k]
                dx = cv2.remap(fl[:, :, 0].astype(np.float32), px, py, cv2.INTER_LINEAR, borderMode=cv2.BORDER_REPLICATE)
                dy = cv2.remap(fl[:, :, 1].astype(np.float32), px, py, cv2.INTER_LINEAR, borderMode=cv2.BORDER_REPLICATE)
                px, py = px + dx, py + dy
                ref = self.hist[-k][1]
                wr = cv2.remap(ref.astype(np.float32), px, py, cv2.INTER_LINEAR, borderMode=cv2.BORDER_REPLICATE)
                fr = fine(wr.astype(np.float64))
                best = np.maximum(best, block_mean(fr * fr))
        self.hist.append((y8, Y))
        self.hist = self.hist[-5:]
        self.flows = self.flows[-4:]
        eps = 1e-3
        deficit = np.clip(1.0 - e1 / (best + eps), 0.0, 1.0) if best.any() else np.zeros_like(e1)
        signif = np.log((best + eps) / (n + eps)) if best.any() else np.zeros_like(e1)
        dark = 1.0 - block_mean(Y) / 255.0
        phase = np.full_like(e1, float(deficit.mean()))
        return np.stack([np.ones_like(e1), deficit, signif, trust, np.log1p(n), dark, phase], axis=-1)


def collect(a):
    with open(a.suite, encoding="utf8") as f:
        suite = json.load(f)
    os.makedirs(a.out, exist_ok=True)
    for case in suite["cases"]:
        dst = os.path.join(a.out, case["id"] + ".npz")
        if os.path.isfile(dst):
            continue
        inputs, truth = eh.build_case_inputs(case, a.max_frames)
        cand = ctd.TemporalDetailCLI("td_force", budget=0.08, boost_max=0.0, force_weight=1.0)
        cand.reset(inputs[0].meta())
        feats = Features()
        F, num, den, mse = [], [], [], []
        for i, (fr, tr) in enumerate(zip(inputs, truth)):
            out = cand.process(fr)
            D = out.Y - fr.Y
            F.append(feats.push(fr))
            num.append(block_mean((tr.Y - fr.Y) * D))
            den.append(block_mean(D * D))
            mse.append(float(np.mean((tr.Y - fr.Y) ** 2)))
        cand.reset({})
        ft = case.get("frame_types") or ""
        np.savez_compressed(dst, F=np.stack(F), num=np.stack(num), den=np.stack(den), mse=np.array(mse),
                            types=np.array(list(ft[:len(inputs)])), bitdepth=inputs[0].bitdepth)
        print("collected", case["id"], flush=True)
    return 0


def gain_db(num, den, mse_in, alpha):
    """PSNR change from out = in + alpha D per block: MSE drops by 2 alpha num - alpha^2 den (block means)."""
    d = np.mean(2 * alpha * num - alpha * alpha * den)
    return 10 * math.log10(mse_in / max(mse_in - d, 1e-12))


def fit(a):
    data = {}
    for f in sorted(glob.glob(os.path.join(a.data, "*.npz"))):
        data[os.path.splitext(os.path.basename(f))[0]] = dict(np.load(f))
    train = [k for k in data if k.split("_")[0] in a.train.split(",")]
    test = [k for k in data if k not in train]

    def sel(k, only_b=True):
        d = data[k]
        m = (d["types"] == "B") if (only_b and d["types"].size) else np.ones(d["F"].shape[0], bool)
        m[:2] = False   # no history yet
        return d["F"][m], d["num"][m], d["den"][m], float(np.mean(d["mse"][m]))
    X, y, wt = [], [], []
    for k in train:
        F, num, den, _ = sel(k, only_b=False)
        ok = den > 1e-6
        X.append(F[ok]); y.append(np.clip(num[ok] / den[ok], 0, 1)); wt.append(den[ok])
    X, y, wt = np.concatenate(X), np.concatenate(y), np.concatenate(wt)
    sw = np.sqrt(wt)
    coef, *_ = np.linalg.lstsq(X * sw[:, None], y * sw, rcond=None)
    L = ["<!-- Copyright(C) 2026 VP-V contributors -->", "<!-- SPDX-License-Identifier: GPL-3.0-only -->", "",
         "# TemporalDetail: regional adaptation (transfer ceiling, oracle, fitted local rule)", "",
         "Per-block strength a_b on the model's transfer-ceiling output (forceWeight 1, boost off). Train cases: %s; "
         "held-out: %s. PSNR change on B-frames (and all frames) vs the decoded input; MSE in block-mean form over "
         "the 960x544 crops (frames from the 3rd on)." % (", ".join(train), ", ".join(test)), "",
         "Fitted rule a_b = clamp(c . f, 0, 1): " + ", ".join("%s %+.4f" % (n, c) for n, c in zip(FEATURES, coef)), "",
         "| case | set | ceiling a=1 (B) | global best const a (B) | oracle a* (B) | rule (B) | rule (all) | rule mean a |",
         "|---|---|---|---|---|---|---|---|"]
    tot = {"train": [], "test": []}
    for k in train + test:
        F, num, den, mse_b = sel(k, True)
        Fa, numa, dena, mse_a = sel(k, False)
        ceil = gain_db(num, den, mse_b, 1.0)
        consts = [(gain_db(num, den, mse_b, c), c) for c in np.linspace(0, 1, 21)]
        best_c = max(consts)
        orc = gain_db(num, den, mse_b, np.clip(np.where(den > 1e-9, num / np.maximum(den, 1e-12), 0), 0, 1))
        ab = np.clip(F @ coef, 0, 1)
        rule = gain_db(num, den, mse_b, ab)
        rule_all = gain_db(numa, dena, mse_a, np.clip(Fa @ coef, 0, 1))
        s = "train" if k in train else "test"
        tot[s].append((ceil, best_c[0], orc, rule, rule_all, float(ab.mean())))
        L.append("| %s | %s | %+.3f | %+.3f (a=%.2f) | %+.3f | %+.3f | %+.3f | %.3f |" % (
            k, s, ceil, best_c[0], best_c[1], orc, rule, rule_all, ab.mean()))
    for s, v in tot.items():
        if v:
            m = np.mean(np.array(v), axis=0)
            L.append("| **mean %s** | | %+.3f | %+.3f | %+.3f | %+.3f | %+.3f | %.3f |" % (s, *m))
    L.append("")
    with open(a.out, "w", encoding="utf8", newline="\n") as f:
        f.write("\n".join(L) + "\n")
    with open(os.path.splitext(a.out)[0] + "_coef.json", "w", encoding="utf8", newline="\n") as f:
        json.dump(dict(features=FEATURES, coef=[float(c) for c in coef], train=train), f, indent=1)
        f.write("\n")
    print("\n".join(L))
    return 0


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest="cmd", required=True)
    c = sub.add_parser("collect")
    c.add_argument("--suite", required=True)
    c.add_argument("--out", required=True)
    c.add_argument("--max-frames", type=int, default=48)
    c.set_defaults(fn=collect)
    f = sub.add_parser("fit")
    f.add_argument("--data", required=True)
    f.add_argument("--train", required=True)
    f.add_argument("--out", required=True)
    f.set_defaults(fn=fit)
    a = ap.parse_args(argv)
    return a.fn(a)


if __name__ == "__main__":
    sys.exit(main())
