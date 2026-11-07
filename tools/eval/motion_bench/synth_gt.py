# Copyright(C) 2026 VP-V contributors
# SPDX-License-Identifier: GPL-3.0-only
"""Synthetic motion benchmark sequences with exact ground-truth flow (numpy + cv2; use D:\\hfr\\venv python -I).

  python -I synth_gt.py OUT_ROOT [--only NAME ...] [--frames 12] [--seed N]

Per scene, under OUT_ROOT/<scene>/ (never inside the repo):
  seq.yuv            yuv420p, 16-bit LE samples holding 10-bit limited-range values (chroma 512), W x H x N
  gt/pair_%06d.flo   exact flow of pair t (frame t-1 -> t), Middlebury .flo, source px; p in t-1 lands on p + flow in t
  occ/pair_%06d.npy  uint8 occlusion-boundary band: pixels of frame t-1 that are occluded/out of frame in t, dilated 5x5
  lab/pair_%06d.npy  uint8 labels: 1 camera (background, visible), 2 object (visible), 0 other (occluded)
  small/pair_%06d.npy uint8 mask of small fast objects (<= 32 px, >= 20 px/frame), only for scenes that have them
  meta.json          W,H,N,fps,bitdepth, pairs, scene parameters, gt_warp_mae (self-check of the ground truth)
Background: frame t(p) = Tex(F^-t(p)) with the similarity F(p) = c + s Rot(theta) (p - c) + v, so the flow is analytic
(S - I)(p - c) + v. Objects are textured translating shapes; their flow is the constant translation per frame.
"""
import argparse
import json
import os
import sys

import cv2
import numpy as np

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import score_flow  # noqa: E402

W, H, FPS = 1280, 720, 24.0


def make_texture(rng, tw=2800, th=1800):
    t = np.zeros((th, tw), np.float32)
    for sigma, wgt in ((1.0, 1.0), (2.0, 1.0), (4.0, 1.0), (8.0, 0.9), (16.0, 0.8), (32.0, 0.7), (64.0, 0.6)):
        n = rng.standard_normal((th, tw)).astype(np.float32)
        n = cv2.GaussianBlur(n, (0, 0), sigma)
        n /= max(float(n.std()), 1e-6)
        t += wgt * n
    for _ in range(900):  # hard-edged structure
        x, y = int(rng.integers(0, tw)), int(rng.integers(0, th))
        w, h = int(rng.integers(8, 160)), int(rng.integers(8, 160))
        t[y:y + h, x:x + w] += float(rng.normal(0, 1.2))
    t = cv2.GaussianBlur(t, (0, 0), 0.8)
    t = (t - t.mean()) / t.std()
    return np.clip(0.45 + 0.17 * t, 0.02, 0.98).astype(np.float32)


def make_patch(rng, w, h):
    p = make_texture(rng, w + 64, h + 64)[32:32 + h, 32:32 + w]
    return np.clip(p + float(rng.uniform(-0.2, 0.2)), 0.02, 0.98).astype(np.float32)


class Obj:
    def __init__(self, rng, kind, w, h, c0, v, small=False):
        self.kind, self.w, self.h, self.c0, self.v, self.small = kind, w, h, np.array(c0, float), np.array(v, float), small
        self.patch = make_patch(rng, w, h)

    def centre(self, t):
        return self.c0 + t * self.v


def scenes(rng, N):
    def small_objs(n):
        out = []
        for i in range(n):
            ang = np.deg2rad(rng.uniform(-30, 30)) + (0 if i % 2 == 0 else np.pi)
            sp = rng.uniform(20, 28)
            size = int(rng.integers(16, 33))
            c0 = (640 + rng.uniform(-220, 220) - np.cos(ang) * sp * N / 2, 360 + rng.uniform(-110, 110) - np.sin(ang) * sp * N / 2)
            out.append(Obj(rng, 'rect' if i % 2 else 'disc', size, size, c0, (sp * np.cos(ang), sp * np.sin(ang)), True))
        return out
    big = lambda: [Obj(rng, 'rect', 220, 160, (330, 280), (14.0, 3.0)), Obj(rng, 'disc', 200, 200, (950, 430), (-12.0, -4.0))]
    # name: (s, theta_deg, (vx, vy), objects, grain sigma)
    return {
        'pan_int': (1.0, 0.0, (-8.0, 3.0), [], 0.0),
        'pan_subpix': (1.0, 0.0, (3.37, -1.62), [], 0.0),
        'pan_slow': (1.0, 0.0, (0.62, 0.31), [], 0.0),
        'zoom': (1.012, 0.0, (1.5, 0.0), [], 0.0),
        'rot': (1.0, 0.6, (0.0, 0.0), [], 0.0),
        'pan_small_fast': (1.0, 0.0, (4.5, 0.0), small_objs(6), 0.0),
        'pan_occlusion': (1.0, 0.0, (-5.0, 1.0), big(), 0.0),
        'pan_grain': (1.0, 0.0, (1.83, 0.77), [Obj(rng, 'rect', 120, 90, (500, 300), (3.0, 1.0))], 0.02),
        'zoomrot_objects': (1.008, 0.4, (2.0, -1.0), big() + small_objs(2), 0.0),
    }


def inverse_map(p, c, S, v, k):
    """Apply F^-1 k times to points p (H,W,2): F(q) = c + S (q - c) + v."""
    Si = np.linalg.inv(S)
    q = p.copy()
    for _ in range(k):
        d = q - c - v
        q = c + d @ Si.T
    return q


def render_frame(tex, objs, t, S, v, grain, rng):
    c = np.array([W / 2.0, H / 2.0])
    gy, gx = np.mgrid[0:H, 0:W].astype(np.float64)
    p = np.stack([gx, gy], -1)
    q = inverse_map(p, c, S, np.asarray(v, float), t)
    mx = (q[..., 0] - c[0] + tex.shape[1] / 2.0).astype(np.float32)
    my = (q[..., 1] - c[1] + tex.shape[0] / 2.0).astype(np.float32)
    img = cv2.remap(tex, mx, my, cv2.INTER_CUBIC, borderMode=cv2.BORDER_REFLECT_101)
    layer = np.zeros((H, W), np.uint8)
    for i, o in enumerate(objs, 1):
        cx, cy = o.centre(t)
        x0, x1 = int(max(0, np.floor(cx - o.w / 2 - 1))), int(min(W, np.ceil(cx + o.w / 2 + 2)))
        y0, y1 = int(max(0, np.floor(cy - o.h / 2 - 1))), int(min(H, np.ceil(cy + o.h / 2 + 2)))
        if x1 <= x0 or y1 <= y0:
            continue
        ys, xs = np.mgrid[y0:y1, x0:x1].astype(np.float64)
        ux, uy = xs - cx, ys - cy
        if o.kind == 'rect':
            m = (np.abs(ux) <= o.w / 2 - 0.5) & (np.abs(uy) <= o.h / 2 - 0.5)
        else:
            m = (ux / (o.w / 2 - 0.5)) ** 2 + (uy / (o.h / 2 - 0.5)) ** 2 <= 1.0
        pm = (ux + o.w / 2 - 0.5).astype(np.float32)
        pn = (uy + o.h / 2 - 0.5).astype(np.float32)
        val = cv2.remap(o.patch, pm, pn, cv2.INTER_CUBIC, borderMode=cv2.BORDER_REFLECT_101)
        sub, lsub = img[y0:y1, x0:x1], layer[y0:y1, x0:x1]
        sub[m] = val[m]
        lsub[m] = i
    if grain > 0:
        n = cv2.GaussianBlur(rng.standard_normal((H, W)).astype(np.float32), (0, 0), 0.7)
        img = img + grain * n / max(float(n.std()), 1e-6)
    return np.clip(img, 0.0, 1.0), layer


def to_yuv10(img):
    y = np.rint(64.0 + 876.0 * img).astype(np.uint16)
    u = np.full((H // 2, W // 2), 512, np.uint16)
    return y.tobytes() + u.tobytes() + u.tobytes()


def gen_scene(root, name, spec, objs_unused, N, tex, rng):
    s, th, v, objs, grain = spec
    th_r = np.deg2rad(th)
    S = s * np.array([[np.cos(th_r), -np.sin(th_r)], [np.sin(th_r), np.cos(th_r)]])
    c = np.array([W / 2.0, H / 2.0])
    d = os.path.join(root, name)
    for sub in ('gt', 'occ', 'lab', 'small'):
        os.makedirs(os.path.join(d, sub), exist_ok=True)
    frames, layers = [], []
    with open(os.path.join(d, 'seq.yuv'), 'wb') as f:
        for t in range(N):
            img, lay = render_frame(tex, objs, t, S, v, grain, rng)
            frames.append(img)
            layers.append(lay)
            f.write(to_yuv10(img))
    cv2.imwrite(os.path.join(d, 'frame0.png'), np.rint(frames[0] * 255).astype(np.uint8))
    gy, gx = np.mgrid[0:H, 0:W].astype(np.float64)
    bg = np.stack([(S[0, 0] - 1) * (gx - c[0]) + S[0, 1] * (gy - c[1]) + v[0], S[1, 0] * (gx - c[0]) + (S[1, 1] - 1) * (gy - c[1]) + v[1]], -1)
    k5 = np.ones((5, 5), np.uint8)
    maes, has_small = [], any(o.small for o in objs)
    for t in range(1, N):
        l0, l1 = layers[t - 1], layers[t]
        flow = bg.copy()
        for i, o in enumerate(objs, 1):
            flow[l0 == i] = o.centre(t) - o.centre(t - 1)
        tx, ty = gx + flow[..., 0], gy + flow[..., 1]
        ix, iy = np.rint(tx).astype(int), np.rint(ty).astype(int)
        inside = (ix >= 0) & (ix < W) & (iy >= 0) & (iy < H)
        same = np.zeros((H, W), bool)
        same[inside] = l1[iy[inside], ix[inside]] == l0[inside]
        occ = ~same
        lab = np.where(occ, 0, np.where(l0 == 0, 1, 2)).astype(np.uint8)
        band = cv2.dilate(occ.astype(np.uint8), k5)
        score_flow.write_flo(os.path.join(d, 'gt', 'pair_%06d.flo' % t), flow)
        np.save(os.path.join(d, 'occ', 'pair_%06d.npy' % t), (band * 255).astype(np.uint8))
        np.save(os.path.join(d, 'lab', 'pair_%06d.npy' % t), lab)
        if has_small:
            sm = np.zeros((H, W), np.uint8)
            for i, o in enumerate(objs, 1):
                if o.small:
                    sm[(l0 == i) & ~occ] = 255
            np.save(os.path.join(d, 'small', 'pair_%06d.npy' % t), sm)
        # GT self-check: frame t sampled at p + flow against frame t-1 over visible background (grain-free scenes ~ interpolation error)
        warped = cv2.remap(frames[t], tx.astype(np.float32), ty.astype(np.float32), cv2.INTER_CUBIC, borderMode=cv2.BORDER_REFLECT_101)
        sel = (lab == 1) & (cv2.erode((lab == 1).astype(np.uint8), k5) > 0)
        maes.append(float(np.mean(np.abs(warped[sel] - frames[t - 1][sel]))) if sel.any() else float('nan'))
    meta = dict(scene=name, W=W, H=H, N=N, fps=FPS, bitdepth=10, pairs=list(range(1, N)), s=s, theta_deg=th, v=list(v),
                grain=grain, objects=[dict(kind=o.kind, w=o.w, h=o.h, c0=list(o.c0), v=list(o.v), small=o.small) for o in objs],
                gt_warp_mae=float(np.nanmean(maes)))
    with open(os.path.join(d, 'meta.json'), 'w') as f:
        json.dump(meta, f, indent=1)
    return meta


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('out')
    ap.add_argument('--only', nargs='*')
    ap.add_argument('--frames', type=int, default=12)
    ap.add_argument('--seed', type=int, default=20261107)
    a = ap.parse_args(argv)
    if os.path.abspath(a.out).startswith(os.path.abspath(os.path.join(HERE, '..', '..', '..')) + os.sep):
        print('refusing to write inside the repository', file=sys.stderr)
        return 2
    rng = np.random.default_rng(a.seed)
    tex = make_texture(rng)
    sc = scenes(rng, a.frames)
    for name, spec in sc.items():
        if a.only and name not in a.only:
            continue
        m = gen_scene(a.out, name, spec, None, a.frames, tex, np.random.default_rng(a.seed + 1))
        print('%-18s gt_warp_mae=%.5f (gray 0..1)' % (name, m['gt_warp_mae']))
    return 0


if __name__ == '__main__':
    sys.exit(main())
