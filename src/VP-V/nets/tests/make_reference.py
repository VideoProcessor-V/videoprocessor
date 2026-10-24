"""Writes the PyTorch reference cases the CPU net executor is checked against (NetRunnerTests).

Why: the CPU reference executor (CpuNetExecutor) runs the TrainKit exporter's GLSL passes on the
CPU. Comparing its output with the kit's OWN PyTorch forward pass, on the same weights, proves the
package parsing and the pass maths before any GPU wiring (barrier B1).

READ-ONLY on the kit: it imports trainkit.common / trainkit.models and loads the exported weights
(<staging>/weights/<net>.pt). It never writes to the kit drive. Run with the kit's Python:

  set PYTHONDONTWRITEBYTECODE=1
  E:\\VP-NN-TrainKit\\runtime\\python\\python.exe -I -B make_reference.py ^
      --kit E:\\VP-NN-TrainKit --staging E:\\VP-NN-TrainKit\\work\\staging --out data

Each case writes data/<case>/case.json (the shader file + its sha256, the bindings, the PARAM
values, sizes) and little-endian float32 planes (row-major, one value per pixel, or two for the
motion mask). Inputs are seeded synthetic noise/ramps, never video pixels.
"""
import argparse
import hashlib
import json
import os
import sys

import numpy as np


def sha256(path):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        h.update(f.read())
    return h.hexdigest()


def picture(rng, h, w, flat_left=True):
    """A test plane with structure the nets react to: a flat left part (gates stay closed), a
    blocky/noisy right part (codec-like steps), a soft ramp, values in about [0.15, 0.85]."""
    y, x = np.mgrid[0:h, 0:w].astype(np.float64)
    p = 0.35 + 0.25 * (x / max(w - 1, 1)) + 0.1 * np.sin(y / 3.0)
    blocks = rng.uniform(-0.06, 0.06, size=((h + 7) // 8, (w + 7) // 8)).repeat(8, 0).repeat(8, 1)[:h, :w]
    noise = rng.normal(0.0, 0.01, size=(h, w))
    right = x >= (w // 3 if flat_left else 0)
    p = p + np.where(right, blocks + noise, 0.0)
    return np.clip(p, 0.15, 0.85).astype(np.float32)


def save(case_dir, name, arr):
    arr = np.ascontiguousarray(arr.astype("<f4"))
    arr.tofile(os.path.join(case_dir, name))
    return name


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--kit", required=True)
    ap.add_argument("--staging", required=True)
    ap.add_argument("--out", required=True)
    a = ap.parse_args()
    sys.path.insert(0, a.kit)
    import torch
    from trainkit import common as C
    from trainkit import models as Mo

    torch.set_grad_enabled(False)
    rng = np.random.default_rng(20261024)

    def load(name):
        d = torch.load(os.path.join(a.staging, "weights", name + ".pt"), map_location="cpu", weights_only=False)
        net = Mo.build(d["spec"])
        net.load_state_dict(d["model"])
        net.eval()
        return net, d["config"], d["fingerprint"]

    def write_case(case, shader, bindings, params, out, extra=None):
        cd = os.path.join(a.out, case)
        os.makedirs(cd, exist_ok=True)
        sp = os.path.join(a.staging, "shaders", shader)
        desc = dict(case=case, shader=shader, shader_sha256=sha256(sp), params=params, bindings=[], output=None)
        for b in bindings:
            arr = b["data"]
            comps = 1 if arr.ndim == 2 else arr.shape[2]
            desc["bindings"].append(dict(name=b["name"], width=arr.shape[1], height=arr.shape[0], components=comps,
                                         file=save(cd, b["name"] + ".f32", arr)))
        desc["output"] = dict(width=out.shape[1], height=out.shape[0], file=save(cd, "expected.f32", out))
        if extra:
            desc.update(extra)
        with open(os.path.join(cd, "case.json"), "w", newline="\n") as f:
            json.dump(desc, f, indent=1)
        print(case, "written", out.shape)

    t = lambda arr: torch.from_numpy(arr)[None, None].float()

    # --- C: re-base refine, H and V (V = the same weights on the transposed picture).
    net, cfg, fp = load("C_rebase_refine")
    x = picture(rng, 40, 48)
    bf = lambda v: C.scale_bound(v, cfg["scale_bound"])
    for axis in ("h", "v"):
        out, _ = net(t(x), bf, axis)
        write_case(f"C_rebase_refine_{axis}", f"C_rebase_refine_{axis}.glsl", [dict(name="HOOKED", data=x)], {},
                   out[0, 0].numpy(), dict(fingerprint=fp["fingerprint"]))

    # --- D: hybrid refine inside the scaler (source 60x45 -> output 40x30, rect = the whole output).
    net, cfg, fp = load("D_hybrid_refine")
    src = picture(rng, 45, 60)
    sout = picture(rng, 30, 40)
    grid = Mo.output_grid((45, 60), (30, 40), (0, 0, 40, 30))
    corr, _ = net(t(sout), C.scale_bound(t(sout), cfg["scale_bound"]), t(src), grid)
    write_case("D_hybrid_refine", "D_hybrid_refine.glsl",
               [dict(name="SCALER_OUT", data=sout), dict(name="SCALER_SRC", data=src)], {},
               (t(sout) + corr)[0, 0].numpy(), dict(fingerprint=fp["fingerprint"]))

    # --- A: delivered-frame repair, gated, labels schema v1 (non-default values on purpose).
    labels = dict(ry=0.5, rx=0.5, kernel="lanczos", codec="HEVC", fgs=False, tier=0.5, bits10=True)
    params = dict(vpnn_ry=0.5, vpnn_rx=0.5, vpnn_kernel=C.PLAYER_KERNELS.index("lanczos"),
                  vpnn_codec=Mo.CODECS.index("HEVC"), vpnn_fgs=0.0, vpnn_bpp_tier=0.5, vpnn_bits10=1.0)
    # A's bound: the export used the kit config's delivered_bound, which the .pt's stored config
    # omits (it keeps split/bound/scale_bound only), so it is read from <staging>/config.json.
    with open(os.path.join(a.staging, "config.json")) as f:
        kit_cfg = json.load(f)
    a_bound = kit_cfg.get("delivered_bound")
    bA = lambda v: C.scale_bound(v, a_bound or cfg["scale_bound"])
    for name in ("A_single_small", "A_motion_small"):
        net, cfg, fp = load(name)
        u = picture(rng, 48, 64)
        cond = Mo.delivered_cond(1, labels["ry"], labels["rx"], labels["kernel"], "cpu", labels["codec"],
                                 labels["fgs"], labels["tier"], labels["bits10"])
        binds = [dict(name="HOOKED", data=u)]
        nb = None
        if net.motion:
            prev = np.clip(np.roll(u, 1, 1) + rng.normal(0, 0.004, u.shape), 0, 1).astype(np.float32)
            nxt = np.clip(np.roll(u, -1, 0) + rng.normal(0, 0.004, u.shape), 0, 1).astype(np.float32)
            mask = rng.uniform(0, 1, size=u.shape + (2,)).astype(np.float32)
            nb = dict(prev=t(prev), next=t(nxt), mprev=t(mask[..., 0]), mnext=t(mask[..., 1]))
            binds += [dict(name="VPNN_PREV", data=prev), dict(name="VPNN_NEXT", data=nxt), dict(name="VPNN_MASK", data=mask)]
        out, _ = net(t(u), cond, bA, "hard", nb=nb)
        logit = net.last_gate["logit"][0, 0].numpy()
        write_case(name, name + ".glsl", binds, params, out[0, 0].numpy(),
                   dict(fingerprint=fp["fingerprint"], label_schema=Mo.LABEL_SCHEMA, bound=a_bound,
                        gate_tiles_open=int((logit > 0).sum()), gate_tiles=int(logit.size)))
        # Round 1's gate stays closed on nearly everything (a known round-1 result), so the case
        # above only proves the exact-copy path. The "_open" case raises the gate's final bias by
        # +24 on BOTH sides (PyTorch here; the C++ test patches the same GLSL literal), which
        # opens every tile and exercises the whole trunk, the conditioning bias and the bound.
        b3 = np.float32(net.gate.c3.bias[0].item())
        b3_open = np.float32(b3 + np.float32(24.0))
        net.gate.c3.bias[0] = float(b3_open)
        out, _ = net(t(u), cond, bA, "hard", nb=nb)
        logit = net.last_gate["logit"][0, 0].numpy()
        patch = {"from": "float gl = %s;" % repr(float(b3)), "to": "float gl = %s;" % repr(float(b3_open))}
        write_case(name + "_open", name + ".glsl", binds, params, out[0, 0].numpy(),
                   dict(fingerprint=fp["fingerprint"], label_schema=Mo.LABEL_SCHEMA, text_patch=patch,
                        gate_tiles_open=int((logit > 0).sum()), gate_tiles=int(logit.size)))


if __name__ == "__main__":
    main()
