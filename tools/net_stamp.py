#!/usr/bin/env python3
"""VP net stamp: a sidecar fingerprint for a TrainKit export, derived ONLY from what the kit recorded.

Why (coordinator 2026-10-24): VP's fingerprint check (src/VP-V/nets/NetFingerprint.h) needs fields
the round-1/2 exporter does not write: upstream_key, a typed pass order and intermediate format,
the label schema of label-less nets, and A's delivered_bound. The installed kit must not change
while round 2 runs, so this tool derives them from the kit's own records and writes a sidecar
(<export stem>.vpstamp.json) that NetPackage reads when the export lacks a field. It never
guesses: a field it cannot derive from a record makes it refuse, with the reason (exit 2).

Records it reads (all read only):
  - export_<net>.json: the fingerprint fields; its id must recompute from them (kit h()).
  - the shaders it lists: their header fingerprint must be that id; sha256 goes in the sidecar.
  - the kit config the export was made with (--config, e.g. <staging>/config.json): its bound /
    scale_bound and grain split must hash to the recorded `bounds` / `grain_split`.
  - for nets after VP's deband/texture-restore slots (B, D): the render settings
    (--scaling-manifest, the kit's data/scaling/scaling_manifest.json) must hash to the recorded
    `scaler_blend_settings` and say deband, VP deband, texture restore and detail pulse were off.
  - optional --kit <kit folder>: sha256 of trainkit/main.py and export_glsl.py (the kit identity).

The kit's h(obj) = sha256(json.dumps(obj, sort_keys=True))[:16] (trainkit/main.py fingerprint()).

  python tools/net_stamp.py --export <staging>/export_C_rebase_refine.json --config <staging>/config.json \
      [--scaling-manifest <kit>/data/scaling/scaling_manifest.json] [--kit <kit>] --out <dir>

Stdlib only. Writes only to --out (never into the export folder unless that is --out).
"""
import argparse
import hashlib
import json
import os
import re
import struct
import sys

SCHEMA = "vp-net-stamp/1"

# VP slot ids (docs/design/slots.json) for the kit's stage-order tokens. Tokens before the pixel
# path's front end (or outside it) carry no slot; any other token is unknown -> refuse.
TOKEN_SLOT = [
    (r"^A_on_original", "repair.encoding"),
    (r"^A$", "repair.encoding"),
    (r"^1D_rebase_pass", "scaling.rebase"),
    (r"^C$", "scaling.rebase"),
    (r"^x2_pass", "scaling.route_4k"),
    (r"^B(\(|$)", "scaling.route_4k"),
    (r"^hybrid_ssim_downscale", "scaling.output"),
    (r"^D$", "scaling.output"),
]
NO_SLOT = [r"^dv_reconstruction", r"^detect_on_original", r"^crop$", r"^rest_of_VP$"]

# The slot each net kind serves (src/VP-V/nets/NetPackage.cpp SlotOf).
KIND_SLOT = {"A": "repair.encoding", "C": "scaling.rebase", "B": "scaling.route_4k", "D": "scaling.output"}

# Slots that hold a NET (their output depends on which net, and whether it ran).
NET_SLOTS = {"repair.encoding", "scaling.rebase", "scaling.route_4k", "scaling.refine"}

# Shader profiles of the kit's render settings that are classical (no VP net). Anything else, or
# any name mentioning a VPNN shader, means the renders may have contained a net: not provable.
CLASSICAL_SHADERS = {"standard.ssim_downscaler", "standard.fsrcnnx", "standard.ssim_superres"}
# Slots that exist (today) only to run a net: absent from renders made without nets.
NET_ONLY_SLOTS = {"repair.encoding", "scaling.route_4k", "scaling.refine"}
# Net kinds whose training inputs are the kit's VP scaling renders (input_stage "D: inside the
# planned hybrid SSIM downscale ..."): their render settings describe what ran upstream.
RENDER_INPUT_KINDS = {"D"}
# Render-settings keys that start VP's motion estimate (the hybrid then runs with vectors).
MOTION_KEYS = {"motion_engine", "occlusion_mask", "motion_vectors"}

# VP front-end slots that modify the picture and sit between scaling.rebase and scaling.output in
# VP's designed order; the render settings must show them off for a net after them.
FRONT_END_SETTINGS_OFF = ["deband", "deband_vp", "texture_restore", "detail_pulse_stabilise"]
SLOT_ORDER = {"repair.encoding": 70, "scaling.rebase": 60, "repair.deband": 80, "scaling.route_4k": 100, "scaling.output": 160}


class Refuse(Exception):
    pass


def h(obj):
    return hashlib.sha256(json.dumps(obj, sort_keys=True).encode()).hexdigest()[:16]


def sha256_file(path):
    with open(path, "rb") as f:
        return hashlib.sha256(f.read()).hexdigest()


def f32(v):
    return struct.unpack("f", struct.pack("f", float(v)))[0]


def split_order(text):
    """Top-level '>' split that respects (...) and [...]; per_axis[...] is flattened in place."""
    out, depth, cur = [], 0, ""
    for ch in text:
        if ch in "([":
            depth += 1
        elif ch in ")]":
            depth -= 1
        if ch == ">" and depth == 0:
            out.append(cur)
            cur = ""
        else:
            cur += ch
    out.append(cur)
    flat = []
    for tok in out:
        m = re.match(r"^per_axis\[(.*)\]$", tok)
        flat.extend(split_order(m.group(1)) if m else [tok])
    return flat


def token_slot(tok):
    for pat, slot in TOKEN_SLOT:
        if re.search(pat, tok):
            return slot
    for pat in NO_SLOT:
        if re.search(pat, tok):
            return None
    raise Refuse(f"stage order token '{tok}' has no known VP slot mapping")


def net_kind(fp, shaders_text):
    stage = fp.get("input_stage", "")
    m = re.match(r"^([A-D]):", stage)
    if m:
        return m.group(1)
    if "b_structure" in fp:
        return "B"
    raise Refuse("cannot tell the net kind (input_stage has no 'A:'..'D:' prefix and no b_structure)")


def upstream_key(order_text, slot):
    tokens = split_order(order_text)
    slots = [token_slot(t) for t in tokens]
    if slot not in slots:
        raise Refuse(f"the recorded stage order has no stage for slot {slot}")
    first = slots.index(slot)
    key = []
    for s in slots[:first]:
        if s and s != slot and s not in key:
            key.append(s)
    return ">".join(key)


def a_output_bound(shader_text):
    """(c0, c1, cmax) literals of A's bounded output pass."""
    m = re.search(r"//!DESC VPNN A output.*?float bound = clamp\(([-0-9.e]+) \+ ([-0-9.e]+) \* \(mx - mn\) \* 1023\.0, 0\.0, ([-0-9.e]+)\)",
                  shader_text, re.S)
    if not m:
        raise Refuse("A's bounded output pass (its bound constants) was not found in the shader")
    return tuple(float(x) for x in m.groups())


def stamp(export_path, config_path, scaling_manifest=None, kit_dir=None):
    with open(export_path, encoding="utf-8") as f:
        ex = json.load(f)
    fp = ex.get("fingerprint")
    if not isinstance(fp, dict) or "fingerprint" not in fp:
        raise Refuse("the export json has no fingerprint")
    recompute = h({k: v for k, v in fp.items() if k not in ("fingerprint", "stage_order_text")})
    if recompute != fp["fingerprint"]:
        raise Refuse(f"the fingerprint fields hash to {recompute}, not the recorded id {fp['fingerprint']} (edited or a different kit)")
    derived = {}

    shader_dir = os.path.join(os.path.dirname(os.path.abspath(export_path)), "shaders")
    shas, texts = {}, {}
    for name in ex.get("shaders", []):
        p = os.path.join(shader_dir, name)
        if not os.path.exists(p):
            raise Refuse(f"shader {name} listed by the export is missing")
        with open(p, encoding="utf-8") as f:
            texts[name] = f.read()
        if f"VPNN pipeline fingerprint: {fp['fingerprint']} " not in texts[name]:
            raise Refuse(f"shader {name} does not carry the export's fingerprint {fp['fingerprint']}")
        shas[name] = sha256_file(p)
    if not shas:
        raise Refuse("the export lists no shaders")

    with open(config_path, encoding="utf-8") as f:
        cfg = json.load(f)
    for key in ("bound", "scale_bound", "split_eps_codes", "split_r2160"):
        if key not in cfg:
            raise Refuse(f"the config has no {key}")
    if h({"bound": cfg["bound"], "scale_bound": cfg["scale_bound"]}) != fp.get("bounds"):
        raise Refuse("the config's bound/scale_bound do not hash to the recorded `bounds`: not the config this export was made with")
    grain = dict(operator="self-guided filter", eps_codes=cfg["split_eps_codes"], r2160=cfg["split_r2160"], pad="reflect")
    if h(grain) != fp.get("grain_split"):
        raise Refuse("the config's grain split does not hash to the recorded `grain_split`")
    derived["config"] = "bounds and grain_split hashes match the recorded fingerprint"

    kind = net_kind(fp, texts)
    slot = KIND_SLOT[kind]

    # The kit's render settings (the scaling renders' VP settings): --scaling-manifest, else the
    # export's own render_settings.json (round 3). Used only when they hash to the recorded
    # scaler_blend_settings (the settings the training renders actually used).
    if not scaling_manifest:
        side = os.path.join(os.path.dirname(os.path.abspath(export_path)), "render_settings.json")
        scaling_manifest = side if os.path.exists(side) else None
    rs = None
    if scaling_manifest:
        with open(scaling_manifest, encoding="utf-8") as f:
            sm = json.load(f)
        settings = sm.get("settings", sm)
        if h({k: v for k, v in settings.items() if k in ("common", "down", "up", "adapter")}) != fp.get("scaler_blend_settings"):
            raise Refuse("the render settings do not hash to the recorded scaler_blend_settings")
        rs = settings

    # upstream_key: the recorded stage order (which must hash to the recorded stage_order).
    order = fp.get("stage_order_text")
    if not order:
        raise Refuse("the export records no stage_order_text")
    if h(order) != fp.get("stage_order"):
        raise Refuse("stage_order_text does not hash to the recorded stage_order")
    key = upstream_key(order, slot)
    derived["upstream_key"] = f"stage_order_text tokens before {slot}"
    # upstream_nets: the NETS whose output the training data went through (audit 2026-10-24). The
    # planned stage order cannot prove it: the kit trains a downstream net "with the fallback" when
    # an upstream net is missing, under the same fingerprint. Only an explicit kit record can.
    upstream_net_slots = [k for k in key.split(">") if k in NET_SLOTS] if key else []
    if not upstream_net_slots:
        upstream_nets = ""
        derived["upstream_nets"] = "no net slot upstream in the recorded order"
    elif fp.get("upstream_nets") is not None:
        upstream_nets = fp["upstream_nets"]
        derived["upstream_nets"] = "the export's own record"
    elif kind in RENDER_INPUT_KINDS and rs is not None and net_free(rs):
        # The training inputs were VP renders whose recorded (hash-verified) settings load no net
        # shader: no VP net ran upstream. Such a net MATCHES only while no upstream net is active.
        # Only for nets whose recorded inputs ARE those renders (D: inside VP's hybrid scaler); C's
        # and B's inputs are the kit's Python ports of A's output, which these settings say nothing about.
        upstream_nets = ""
        derived["upstream_nets"] = ("none: the hash-verified render settings load only classical shaders (" +
                                    ", ".join(sorted(shader_profiles(rs))) + ")")
        # Its module order then holds only the classical slots of the planned order: the net slots
        # (A's repair.encoding, the route_4k / refine nets) did not run in those renders.
        key = ">".join(k for k in key.split(">") if k and k not in NET_ONLY_SLOTS)
        derived["upstream_key"] += "; net-only slots removed (no net ran in the renders)"
    else:
        raise Refuse("the training data passed through net slot(s) " + ", ".join(upstream_net_slots) +
                     ", but the kit recorded no upstream_nets and no net-free render settings: cannot prove the upstream nets")
    if SLOT_ORDER[slot] > SLOT_ORDER["repair.deband"]:
        if rs is None:
            raise Refuse(f"{slot} comes after VP's deband/texture-restore slots: the render settings (--scaling-manifest) are needed to know they were off")
        common = rs.get("common", {})
        on = [k for k in FRONT_END_SETTINGS_OFF if common.get(k) != "off"]
        if on:
            raise Refuse("the render settings do not show these front-end stages off: " + ", ".join(on))
        derived["upstream_key"] += "; render settings (hash-verified) show " + ", ".join(FRONT_END_SETTINGS_OFF) + " off"

    # pass order: only the separable nets' own structure records define it (b_structure /
    # c_structure); the shared texture_formats wording ("x2 H->V hand-off") is B's, not A's or D's.
    if kind in ("B", "C"):
        prose = " | ".join(str(fp.get(k, "")) for k in ("b_structure", "c_structure"))
        if "H->V" in prose or "H then V" in prose:
            pass_order = "H>V"
            derived["pass_order"] = "'H then V' in b_structure / c_structure"
        else:
            raise Refuse("no recorded pass order in b_structure / c_structure")
    else:
        pass_order = "n/a"
    m = re.search(r"intermediates (rgba32f|rgba16f)", fp.get("texture_formats", ""))
    if not m:
        raise Refuse("texture_formats does not name the net intermediates' format")
    intermediate = m.group(1)
    handoff = "n/a"
    if kind in ("B", "C"):
        if "pass outputs r16" in fp.get("texture_formats", "") or "hand-off r16" in fp.get("texture_formats", ""):
            handoff = "r16"
        else:
            raise Refuse("texture_formats does not name the r16 hand-off this net kind needs")
    # D: the hybrid's motion mode in its training renders. Recorded (round 2: "NO VECTORS: ...",
    # D_motion: "vectors"), else derived from hash-verified render settings that never start VP's
    # motion estimate (dejudder 0, no interpolation, no motion keys: the estimate does not run,
    # docs/VP-scale-render-motion.md root cause 1).
    hybrid_motion = "n/a"
    if kind == "D":
        rec = fp.get("hybrid_motion_mode")
        if rec:
            hybrid_motion = "no_vectors" if "NO VECTORS" in str(rec) else str(rec).lower()
            derived["hybrid_motion_mode"] = "the export's own record"
        elif rs is not None:
            common = rs.get("common", {})
            keys = {k for part in ("common", "down", "up") for k in rs.get(part, {})}
            if str(common.get("dejudder_strength", "")) == "0" and common.get("interpolation") == "off" and not (keys & MOTION_KEYS):
                hybrid_motion = "no_vectors"
                derived["hybrid_motion_mode"] = "render settings: dejudder 0, interpolation off, no motion keys (the estimate never starts)"
            else:
                raise Refuse("the render settings may have run VP's motion estimate: the hybrid motion mode cannot be derived")
        else:
            raise Refuse("D needs its hybrid motion mode: no record and no render settings")

    stage_contract = fp.get("stage_contract")
    if not stage_contract:
        raise Refuse("the export records no stage_contract")

    has_params = any("//!PARAM " in t for t in texts.values())
    if has_params:
        if not fp.get("label_schema"):
            raise Refuse("the shaders take PARAM labels but the export records no label_schema")
        label_schema = fp["label_schema"]
    else:
        label_schema = "none"

    delivered_bound = "n/a"
    if kind == "A":
        db = cfg.get("delivered_bound")
        if not isinstance(db, dict) or not all(k in db for k in ("c0", "c1", "cmax")):
            raise Refuse("the config has no delivered_bound, which A's output pass uses")
        for name, text in texts.items():
            lit = a_output_bound(text)
            want = (f32(db["c0"]), f32(db["c1"]), f32(db["cmax"]))
            if lit != want:
                raise Refuse(f"{name}: the shader's bound constants {lit} are not the config's delivered_bound {want}")
        delivered_bound = {k: db[k] for k in ("c0", "c1", "cmax")}
        derived["delivered_bound"] = "config delivered_bound, equal to the constants in A's output pass"

    kit = {"config_sha256": sha256_file(config_path)}
    if kit_dir:
        for rel in ("trainkit/main.py", "trainkit/export_glsl.py"):
            p = os.path.join(kit_dir, rel)
            if not os.path.exists(p):
                raise Refuse(f"--kit given but {rel} is missing")
            kit[rel] = sha256_file(p)
    if scaling_manifest:
        kit["scaling_manifest_sha256"] = sha256_file(scaling_manifest)

    return {
        "schema": SCHEMA,
        "export": os.path.basename(export_path),
        "export_fingerprint": fp["fingerprint"],
        "kind": kind,
        "slot": slot,
        "shaders": shas,
        "fields": {
            "upstream_key": key,
            "upstream_nets": upstream_nets,
            "hybrid_motion_mode": hybrid_motion,
            "pass_order": pass_order,
            "intermediate_format": intermediate,
            "handoff_format": handoff,
            "stage_contract": stage_contract,
            "label_schema": label_schema,
            "delivered_bound": delivered_bound,
        },
        "derived_from": derived,
        "kit": kit,
        "stamped_by": "tools/net_stamp.py",
    }


def shader_profiles(rs):
    return {sh for part in ("down", "up") for sh in rs.get(part, {}).get("shaders", [])}


def net_free(rs):
    """True when the render settings load only known classical shader profiles (no VP net)."""
    profiles = shader_profiles(rs)
    text = json.dumps(rs).lower()
    return profiles <= CLASSICAL_SHADERS and "vpnn" not in text


def sidecar_name(export_path):
    return os.path.splitext(os.path.basename(export_path))[0] + ".vpstamp.json"


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--export", required=True)
    ap.add_argument("--config", required=True)
    ap.add_argument("--scaling-manifest")
    ap.add_argument("--kit")
    ap.add_argument("--out", required=True, help="folder for <export stem>.vpstamp.json")
    a = ap.parse_args(argv)
    try:
        s = stamp(a.export, a.config, a.scaling_manifest, a.kit)
    except Refuse as e:
        print(f"REFUSED {os.path.basename(a.export)}: {e}", file=sys.stderr)
        return 2
    os.makedirs(a.out, exist_ok=True)
    path = os.path.join(a.out, sidecar_name(a.export))
    with open(path, "w", encoding="utf-8", newline="\n") as f:
        json.dump(s, f, indent=1, sort_keys=True)
        f.write("\n")
    print(f"stamped {path}: upstream_key='{s['fields']['upstream_key']}'")
    return 0


if __name__ == "__main__":
    sys.exit(main())
