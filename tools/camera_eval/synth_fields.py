# Copyright(C) 2026 VP-V contributors
# SPDX-License-Identifier: GPL-3.0-only
"""Synthetic block-motion fields with exact camera ground truth for the camera model evaluation.

Usage (run with python -I):  python -I synth_fields.py OUT_ROOT [PAIRS]

Writes one folder per scenario under OUT_ROOT in the flow-file layout camera_eval reads (flow_%06d_%06d.npy,
float32, 4 px grid, 1920x1080), plus truthlab_%06d.npy (camera-block mask on the 16 px grid) and truth.json
({"H": [3x3 per pair, row-major, picture-centred source px], "dominant": bool}). Never write into the repo.
"""
import json
import os
import sys

import numpy as np

W, H, G = 1920, 1080, 4
gw4, gh4 = W // G, H // G
xs = (np.arange(gw4) * G + 1.5 - W / 2)[None, :].repeat(gh4, 0)
ys = (np.arange(gh4) * G + 1.5 - H / 2)[:, None].repeat(gw4, 1)

SCENARIOS = ['pan_clean', 'crowd_pan', 'crowd_shake', 'crowd_static', 'zoom_rot', 'two_layers', 'big_object', 'random', 'water']
NON_DOMINANT = ('two_layers', 'random', 'water')


def hom(s=0.0, a=0.0, tx=0.0, ty=0.0, px=0.0, py=0.0, shx=0.0):
    c, sn = np.cos(a), np.sin(a)
    k = np.exp(s)
    return np.array([[k * c, -k * sn + shx, tx], [k * sn, k * c, ty], [px, py, 1.0]])


def apply(Hm, x, y):
    P = Hm[0, 0] * x + Hm[0, 1] * y + Hm[0, 2]
    Q = Hm[1, 0] * x + Hm[1, 1] * y + Hm[1, 2]
    R = Hm[2, 0] * x + Hm[2, 1] * y + Hm[2, 2]
    return P / R - x, Q / R - y


def blobs(rng, n, rw, rh, region):
    m = np.zeros((gh4, gw4), np.int32)
    for i in range(n):
        cx = rng.uniform(region[0], region[2]) * gw4
        cy = rng.uniform(region[1], region[3]) * gh4
        w = rng.uniform(*rw) * gw4
        h = rng.uniform(*rh) * gh4
        m[int(max(0, cy - h / 2)):int(cy + h / 2), int(max(0, cx - w / 2)):int(cx + w / 2)] = i + 1
    return m


def scenario(name, N, rng):
    """Per pair: (forward field, backward field, truth 3x3, camera mask on the 4 px grid)."""
    out = []
    mov = np.zeros((gh4, gw4), np.int32)
    flat = np.zeros((gh4, gw4), bool)
    if name in ('crowd_pan', 'crowd_static', 'crowd_shake'):
        mov = blobs(rng, 260, (0.02, 0.05), (0.06, 0.14), (0, 0.35, 1, 1.05))
        flat[:int(0.15 * gh4), :] = True
    elif name in ('pan_clean', 'zoom_rot'):
        mov = blobs(rng, 6, (0.04, 0.08), (0.1, 0.25), (0.1, 0.3, 0.9, 0.9))
    elif name == 'two_layers':
        mov[:, : gw4 // 2] = 1
    elif name == 'big_object':
        mov[int(0.2 * gh4):, int(0.1 * gw4):int(0.85 * gw4)] = 1
    ids = np.unique(mov[mov > 0])
    vel = {int(i): rng.normal(0, 2.5, 2) + (np.array([0, 2.0]) if name.startswith('crowd') else 0) for i in ids}
    for t in range(N):
        ph = 2 * np.pi * t / N
        if name == 'crowd_pan':
            Hm = hom(tx=-1.0 + 1.6 * np.sin(ph), ty=0.3 * np.cos(2 * ph), a=1e-4 * np.sin(ph))
        elif name == 'crowd_shake':
            Hm = hom(tx=rng.normal(0, 1.5), ty=rng.normal(0, 1.0))
        elif name == 'crowd_static':
            Hm = hom()
        elif name == 'pan_clean':
            Hm = hom(tx=4 + 2 * np.sin(ph), ty=-1 + 0.5 * np.sin(2 * ph))
        elif name == 'zoom_rot':
            Hm = hom(s=0.003 + 0.001 * np.sin(ph), a=0.0008, tx=0.5, ty=-0.3, px=1e-7)
        elif name == 'two_layers':
            Hm = hom(tx=3.0, ty=0.0)
        elif name == 'big_object':
            Hm = hom(tx=-2.5, ty=0.5)
        else:
            Hm = hom()
        dx, dy = apply(Hm, xs, ys)
        cam = np.ones((gh4, gw4), bool)
        if name == 'two_layers':
            dx = np.where(mov > 0, -3.0, dx)
            dy = np.where(mov > 0, 1.0, dy)
            cam[:] = False
        elif name == 'big_object':
            dx = np.where(mov > 0, 1.5, dx)
            dy = np.where(mov > 0, -0.5, dy)
            cam = mov == 0
        elif name in ('random', 'water'):
            if name == 'random':
                dx = rng.uniform(-6, 6, (gh4, gw4))
                dy = rng.uniform(-6, 6, (gh4, gw4))
            else:
                from numpy.fft import fft2, ifft2
                k = np.exp(-((np.fft.fftfreq(gh4)[:, None] ** 2 + np.fft.fftfreq(gw4)[None, :] ** 2)) / (2 * 0.01 ** 2))
                dx = np.real(ifft2(fft2(rng.normal(size=(gh4, gw4))) * k))
                dy = np.real(ifft2(fft2(rng.normal(size=(gh4, gw4))) * k))
                dx *= 2.5 / dx.std()
                dy *= 2.5 / dy.std()
            cam[:] = False
        else:
            for i, v in vel.items():
                m = mov == i
                jit = rng.normal(0, 0.4, 2)
                dx = np.where(m, dx + v[0] + jit[0], dx)
                dy = np.where(m, dy + v[1] + jit[1], dy)
            cam = mov == 0
        noise = 0.12
        dx = dx + rng.normal(0, noise, dx.shape)
        dy = dy + rng.normal(0, noise, dy.shape)
        if flat.any():
            dx = np.where(flat, dx + rng.normal(0, 3, dx.shape), dx)
            dy = np.where(flat, dy + rng.normal(0, 3, dy.shape), dy)
            cam = cam & ~flat
        fwd = np.stack([dx, dy], -1).astype(np.float32)
        Hi = np.linalg.inv(Hm)
        bdx, bdy = apply(Hi, xs, ys)
        bwd = np.where(np.stack([cam, cam], -1), np.stack([bdx, bdy], -1), -fwd).astype(np.float32)
        if flat.any():
            bwd = np.where(np.stack([flat, flat], -1), rng.normal(0, 3, bwd.shape), bwd).astype(np.float32)
        out.append((fwd, bwd, Hm, cam))
    return out


def generate(root, N=40):
    os.makedirs(root, exist_ok=True)
    for k, n in enumerate(SCENARIOS):
        rng = np.random.default_rng(1000 + k)
        d = os.path.join(root, n)
        os.makedirs(d, exist_ok=True)
        pairs = scenario(n, N, rng)
        truth = []
        for t, (f, b, Hm, cam) in enumerate(pairs, start=1):
            np.save(os.path.join(d, 'flow_%06d_%06d.npy' % (t - 1, t)), f)
            np.save(os.path.join(d, 'flow_%06d_%06d.npy' % (t, t - 1)), b)
            truth.append(Hm.ravel().tolist())
            cb = cam[:cam.shape[0] // 4 * 4, :].reshape(-1, 4, cam.shape[1] // 4, 4).mean((1, 3)) > 0.5
            np.save(os.path.join(d, 'truthlab_%06d.npy' % t), cb)
        json.dump({'H': truth, 'dominant': n not in NON_DOMINANT}, open(os.path.join(d, 'truth.json'), 'w'))
        print(n, 'camera share %.2f' % np.mean([p[3].mean() for p in pairs]))


if __name__ == '__main__':
    generate(sys.argv[1], int(sys.argv[2]) if len(sys.argv) > 2 else 40)
