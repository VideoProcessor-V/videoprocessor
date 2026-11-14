#!/usr/bin/env python3
# Copyright(C) 2026 VP-V contributors
# SPDX-License-Identifier: GPL-3.0-only
"""Producer dependency table (refs/129 L-PROD, N1 rule b for producers).

For every skeleton product declared in src/VP-V/core/Products.h this tool lists the DECLARED inputs
(AddInput lag / AddLead lead) and records, from the code of the product's owner fill, whether the input is
  R  read through Acquire (the lane / owner-fill read shape),
  X  recomputed privately or read outside Acquire (a bypass signature matched),
  N  a declared input the owner fill does not read at all,
  NP the product has no production producer (nothing to read from).
The signatures live in tools/producer_chain_map.json (files, read shapes, bypass shapes); the evidence
file:line of every verdict is printed. Outputs: docs/design/producer-chain.md and .json (the JSON also carries
the scheduler block: a topological order of the ordering edges, levels, and the cycle check).

Usage: python -I tools/producer_chain.py [--root R] [--write] [--check] [--json]
  --write  regenerate docs/design/producer-chain.{md,json}
  --check  exit 1 when the committed files differ from a regeneration (freshness)
"""
import argparse
import json
import os
import re
import sys

MAP_REL = "tools/producer_chain_map.json"
PRODUCTS_H = "src/VP-V/core/Products.h"
CATALOG_H = "src/VP-V/core/ProductCatalog.h"
MODULES = "src/VP-V/modules"
OUT_MD = "docs/design/producer-chain.md"
OUT_JSON = "docs/design/producer-chain.json"

MAKE_RE = re.compile(r"ProductDecl\s+(\w+)\s*=\s*(?:detail::)?Make\(\s*(k\w+)\s*,\s*([^,]+?)\s*,\s*\"([^\"]*)\"\s*,\s*Backend::(\w+)")
INPUT_RE = re.compile(r"(?:detail::)?AddInput\(\s*(\w+)\s*,\s*(k\w+)\s*,\s*(\w+)\s*\)")
LEAD_RE = re.compile(r"(?:detail::)?AddLead\(\s*(\w+)\s*,\s*(k\w+)\s*,\s*(\w+)\s*\)")
VARIANT_RE = re.compile(r"(\w+)\.variants\s*=\s*Variants::NonIdentical")
SUBST_RE = re.compile(r"(\w+)\.(noOfaSubstitute|shortQueueSubstitute)\s*=\s*(k\w+)")
TOKEN_RE = re.compile(r"constexpr\s+(?:const\s+char\s*\*|char)\s*(k\w+)(?:\[\])?\s*=\s*\"([^\"]*)\"")
FLAG_RE = re.compile(r"\{\s*(k\w+)\s*,\s*(k\w+)\s*,\s*FlagKind::(\w+)\s*,\s*\"([^\"]*)\"")
ROW_RE = re.compile(r"\{\s*(k\w+)\s*,\s*\"([^\"]*)\"\s*,\s*(k?\w+)\s*,\s*\"([^\"]*)\"\s*\}")
LAG_CONST_RE = re.compile(r"constexpr\s+(?:uint8_t|int|unsigned)\s+(k\w+)\s*=\s*(\d+)\s*;")
REG_FILL_RE = re.compile(r"\bRegisterOwnerFill\(\s*(?:VpSkeleton::)?(k\w+)")


def strip_comments(text):
    """Remove // and /* */ comments (string contents kept), keeping line numbers."""
    out = []
    i, n, state = 0, len(text), 0
    while i < n:
        c = text[i]
        nxt = text[i + 1] if i + 1 < n else ""
        if state == 0:
            if c == "/" and nxt == "/":
                state = 1
                i += 2
            elif c == "/" and nxt == "*":
                state = 2
                i += 2
            elif c == '"':
                state = 3
                out.append(c)
                i += 1
            else:
                out.append(c)
                i += 1
        elif state == 1:
            if c == "\n":
                out.append(c)
                state = 0
            i += 1
        elif state == 2:
            if c == "*" and nxt == "/":
                state = 0
                i += 2
            else:
                if c == "\n":
                    out.append(c)
                i += 1
        else:  # string literal
            out.append(c)
            if c == "\\" and nxt:
                out.append(nxt)
                i += 2
                continue
            if c == '"' or c == "\n":
                state = 0
            i += 1
    return "".join(out)


def read(path):
    with open(path, "r", encoding="utf-8", errors="replace") as f:
        return f.read()


def parse_products(root):
    text = strip_comments(read(os.path.join(root, PRODUCTS_H)))
    lag_consts = {m.group(1): int(m.group(2)) for m in LAG_CONST_RE.finditer(text)}
    tokens = {m.group(1): m.group(2) for m in TOKEN_RE.finditer(text)}
    prods, order, events = {}, [], []
    for m in MAKE_RE.finditer(text):
        var, kid, tok, name, backend = m.groups()
        tok = tok.strip()
        token = tok.strip('"') if tok.startswith('"') else tokens.get(tok, tok)
        prods[kid] = {"id": kid, "token": token, "name": name, "backend": backend, "inputs": [],
                      "variants": "Identical", "noOfaSubstitute": None, "shortQueueSubstitute": None}
        order.append(kid)
        events.append((m.start(), "make", var, kid))
    for rx, kind in ((INPUT_RE, "in"), (LEAD_RE, "lead"), (VARIANT_RE, "var"), (SUBST_RE, "sub")):
        for m in rx.finditer(text):
            events.append((m.start(), kind, m.group(1), m))
    events.sort(key=lambda e: e[0])

    def num(s):
        return int(s) if s.isdigit() else lag_consts.get(s)

    current = {}   # a local variable name is reused by several declarations: the latest Make above wins
    for _, kind, var, payload in events:
        if kind == "make":
            current[var] = prods[payload]
            continue
        p = current.get(var)
        if p is None:
            continue
        if kind == "in":
            lag = num(payload.group(3))
            p["inputs"].append({"product": payload.group(2), "lag": 0 if lag is None else lag, "lead": 0})
        elif kind == "lead":
            lead = num(payload.group(3))
            p["inputs"].append({"product": payload.group(2), "lag": 0, "lead": 1 if lead is None else lead})
        elif kind == "var":
            p["variants"] = "NonIdentical"
        else:
            p[payload.group(2)] = payload.group(3)
    return prods, order


def parse_catalog(root):
    text = strip_comments(read(os.path.join(root, CATALOG_H)))
    rows = {}
    seg = text.split("ProductRows()", 1)[-1].split("RowOf(", 1)[0]
    for m in ROW_RE.finditer(seg):
        rows[m.group(1)] = {"unit_folder": m.group(2), "producer_text": m.group(4)}
    flags = []
    seg = text.split("ProductFlags()", 1)[-1].split("ConsumerFlags()", 1)[0]
    for m in FLAG_RE.finditer(seg):
        flags.append({"product": m.group(1), "input": m.group(2), "kind": m.group(3), "item": m.group(4)})
    return rows, flags


def label_of(pid, tok, labels):
    if pid in labels:
        return labels[pid]
    m = re.match(r"kP(\d+[a-z]*)", pid)
    return "P" + m.group(1) if m else tok


def discover_fill_files(root):
    """Owner-fill registrations (RegisterOwnerFill(kPx ...)) outside tests: product -> [relative files]."""
    found = {}
    base = os.path.join(root, MODULES)
    for dp, dn, fn in os.walk(base):
        dn[:] = [d for d in dn if d != "tests"]
        for f in fn:
            if not f.endswith((".cpp", ".h", ".inl")):
                continue
            full = os.path.join(dp, f)
            rel = os.path.relpath(full, base).replace("\\", "/")
            for m in REG_FILL_RE.finditer(strip_comments(read(full))):
                found.setdefault(m.group(1), set()).add(rel)
    return {k: sorted(v) for k, v in found.items()}


def scan_files(root, files, patterns):
    """Return [(file, line, text)] of lines matching any pattern."""
    hits = []
    for rel in files:
        full = os.path.join(root, MODULES, rel)
        if not os.path.isfile(full):
            continue
        for i, line in enumerate(strip_comments(read(full)).splitlines(), 1):
            for pat in patterns:
                if re.search(pat, line):
                    hits.append((rel, i, line.strip()[:110]))
                    break
    return hits


def classify(root, prods, order, rows, flags, cmap):
    fills = discover_fill_files(root)
    labels = cmap.get("labels", {})
    table = []
    for pid in order:
        p = prods[pid]
        files = list(cmap.get("products", {}).get(pid, {}).get("files", []))
        for f in fills.get(pid, []):
            if f not in files:
                files.append(f)
        prow = rows.get(pid, {})
        reserved = prow.get("producer_text", "").startswith("reserved")             and not cmap.get("products", {}).get(pid, {}).get("ignore_reserved", False)
        edges, seen = [], set()
        for inp in p["inputs"]:
            ip, lag, lead = inp["product"], inp["lag"], inp["lead"]
            if ip in seen:
                for e in edges:
                    if e["input"] == ip:
                        if lead:
                            e["leads"].append(lead)
                        else:
                            e["lags"].append(lag)
                continue
            seen.add(ip)
            edges.append(_edge(root, cmap, pid, ip, lag, lead, files, reserved, flags, True))
        for u in cmap.get("undeclared_external", []):
            if u["product"] == pid:
                e = _edge(root, cmap, pid, u["input"], 0, 0, files, reserved, flags, False)
                e["note"] = u["note"]
                edges.append(e)
        table.append({"id": pid, "label": label_of(pid, p["token"], labels), "token": p["token"], "name": p["name"],
                      "backend": p["backend"], "variants": p["variants"],
                      "noOfaSubstitute": p["noOfaSubstitute"], "shortQueueSubstitute": p["shortQueueSubstitute"],
                      "lane": cmap.get("lanes", {}).get(pid, "-"), "producer_files": files,
                      "producer_text": prow.get("producer_text", ""), "reserved": reserved,
                      "no_producer": (not files) or reserved,
                      "no_producer_owner": cmap.get("no_producer_owners", {}).get(pid, ""), "edges": edges})
    return table


def _edge(root, cmap, pid, ip, lag, lead, files, reserved, flags, declared):
    key = pid + "<-" + ip
    ov = cmap.get("edges", {}).get(key, {})
    efiles = ov.get("files", files)
    e = {"input": ip, "lags": [] if lead else [lag], "leads": [lead] if lead else [], "declared": declared,
         "status": "", "evidence": [], "kind": "", "note": ""}
    if not files or reserved:
        e["status"] = "NP"
        e["note"] = "no production producer"
    else:
        reads = ov.get("read") or [r.replace("{k}", ip) for r in cmap.get("default_read", [])]
        bypass = [b for b in cmap.get("bypass", []) if b["input"] == ip and (not b.get("products") or pid in b["products"])]
        bhits, kinds = [], set()
        for b in bypass:
            h = scan_files(root, b.get("files", efiles), [b["regex"]])
            if h:
                bhits += h
                kinds.add(b["kind"])
        rhits = scan_files(root, efiles, reads)
        if bhits:
            e["status"], e["evidence"], e["kind"] = "X", bhits[:3], "+".join(sorted(kinds))
        elif rhits:
            e["status"], e["evidence"] = "R", rhits[:2]
        else:
            e["status"] = "N"
    if ov.get("note"):
        e["note"] = (e["note"] + "; " if e["note"] else "") + ov["note"]
    e["interim"] = ov.get("interim", "")
    if e["interim"]:
        e["note"] = (e["note"] + "; " if e["note"] else "") + "INTERIM (" + e["interim"] + ")"
    for f in flags:
        if f["product"] == pid and f["input"] == ip:
            e["note"] = (e["note"] + "; " if e["note"] else "") + "flag " + f["kind"] + ": " + f["item"][:90]
    return e


def schedule(table):
    """Ordering edges: lag==0 and lead==0, non-self (mirrors Registry::Validate's Kahn). Lead, lag>0 and
    own-previous-frame edges are temporal and listed separately. Deterministic: ready set sorted by id."""
    ids = [t["id"] for t in table]
    idset = set(ids)
    ordering, temporal = [], []
    for t in table:
        for e in t["edges"]:
            if not e["declared"] or e["input"] not in idset:
                continue
            if 0 in e["lags"] and e["input"] != t["id"]:
                ordering.append([e["input"], t["id"]])
            for lg in sorted(set(x for x in e["lags"] if x > 0)):
                temporal.append({"from": e["input"], "to": t["id"], "kind": "lag", "n": lg})
            for ld in sorted(set(e["leads"])):
                temporal.append({"from": e["input"], "to": t["id"], "kind": "lead", "n": ld})
    ordering = sorted(set(map(tuple, ordering)))
    indeg = {i: 0 for i in ids}
    succ = {i: [] for i in ids}
    for a, b in ordering:
        succ[a].append(b)
        indeg[b] += 1
    level = {i: 0 for i in ids}
    ready = sorted(i for i in ids if indeg[i] == 0)
    topo = []
    while ready:
        n = ready.pop(0)
        topo.append(n)
        for m in sorted(succ[n]):
            level[m] = max(level[m], level[n] + 1)
            indeg[m] -= 1
            if indeg[m] == 0:
                ready.append(m)
                ready.sort()
    cycle = sorted(i for i in ids if indeg[i] > 0)
    return {"ordering_edges": [list(x) for x in ordering], "temporal_edges": temporal, "topological_order": topo,
            "levels": {i: level[i] for i in topo}, "cycle_members": cycle, "acyclic": not cycle}


def counts(table):
    c = {"R": 0, "X": 0, "N": 0, "NP": 0}
    for t in table:
        for e in t["edges"]:
            if e["declared"]:
                c[e["status"]] += 1
    return c


def build(root):
    cmap = json.loads(read(os.path.join(root, MAP_REL)))
    prods, order = parse_products(root)
    rows, flags = parse_catalog(root)
    table = classify(root, prods, order, rows, flags, cmap)
    return {"schema": 1, "counts": counts(table), "products": table, "scheduler": schedule(table),
            "unowned_no_producer": [t["id"] for t in table if t["no_producer"] and not t["no_producer_owner"]],
            "test_only_references": cmap.get("test_only_references", [])}


def render_md(doc):
    lab = {t["id"]: t["label"] for t in doc["products"]}
    L = ["<!-- GENERATED by tools/producer_chain.py --write; do not edit by hand. -->", "",
         "# Producer dependency table", "",
         "Machine-checked (refs/129 L-PROD). R = read shape found (a regex hit on the owner fill, NOT proven Acquire); "
         "X = it recomputes it privately or reads it outside Acquire; N = declared but never read; NP = the product has "
         "no production producer. Fix owners are the lanes of refs/129. Counts count declared (product, input) pairs.", "",
         "Counts: R %(R)d, X %(X)d, N %(N)d, NP %(NP)d." % doc["counts"], "",
         "| Product | Lane | Producer files | Input (lag/lead) | St | Evidence / note |", "|---|---|---|---|---|---|"]
    for t in doc["products"]:
        files = "<br>".join(f.split("/")[-1] for f in t["producer_files"]) or "-"
        var = ""
        if t["variants"] == "NonIdentical":
            var = " (variants" + (", no-OFA " + lab.get(t["noOfaSubstitute"], "?") if t["noOfaSubstitute"] else "") \
                  + (", short-queue " + lab.get(t["shortQueueSubstitute"], "?") if t["shortQueueSubstitute"] else "") + ")"
        head = "**%s** %s%s" % (t["label"], t["token"], var)
        if not t["edges"]:
            L.append("| %s | %s | %s | (no inputs) | - | |" % (head, t["lane"], files))
            continue
        for k, e in enumerate(t["edges"]):
            lag = ",".join(str(x) for x in sorted(set(e["lags"]))) if e["lags"] else ""
            ld = ",".join("lead" + str(x) for x in sorted(set(e["leads"])))
            lg = "/".join(x for x in [("lag" + lag) if lag and lag != "0" else "", ld] if x)
            ev = "; ".join("%s:%d" % (os.path.basename(f), n) for f, n, _ in e["evidence"])
            note = ((e["kind"] + ": ") if e["kind"] else "") + ev + ((" | " if ev else "") + e["note"] if e["note"] else "")
            inp = lab.get(e["input"], e["input"]) + (" " + lg if lg else "") + ("" if e["declared"] else " (undeclared)")
            L.append("| %s | %s | %s | %s | %s | %s |" % (head if k == 0 else "", t["lane"] if k == 0 else "",
                     files if k == 0 else "", inp, e["status"], note.replace("|", "/")))
    np_rows = [t for t in doc["products"] if t["no_producer"]]
    L += ["", "## Products with no production producer", "",
          "Each carries an owner lane or a 102b trigger (A1c: NP is allowed with a trigger, never close it by adding products).", "",
          "| Product | Lane | Owner / 102b trigger |", "|---|---|---|"]
    for t in np_rows:
        L.append("| **%s** %s | %s | %s |" % (t["label"], t["token"], t["lane"], t["no_producer_owner"] or "UNOWNED"))
    ti = [t for t in doc["products"] for e in t["edges"] if e.get("interim")]
    if ti:
        L += ["", "## Interim edges (declared, not real)", "",
              "Declared in Products.h but the producer does not read them yet. They still order in Registry::Validate; the tag records why and who removes the interim.", ""]
        for t in doc["products"]:
            for e in t["edges"]:
                if e.get("interim"):
                    L.append("- %s <- %s: %s" % (t["label"], lab.get(e["input"], e["input"]), e["interim"]))
    tests_only = doc.get("test_only_references", [])
    if tests_only:
        L += ["", "## Test-only references (no production caller)", ""]
        L += ["- %s: %s" % (x["where"], x["note"]) for x in tests_only]
    s = doc["scheduler"]
    L += ["", "## Scheduler order", "",
          "Ordering edges are lag 0, lead 0, non-self (Registry::Validate's Kahn); lag > 0, lead and own-previous-frame edges are "
          "temporal. Acyclic: **%s**." % ("yes" if s["acyclic"] else "NO, cycle: " + ", ".join(s["cycle_members"])), "",
          "Order: " + " , ".join("%s(L%d)" % (lab.get(i, i), s["levels"][i]) for i in s["topological_order"]), ""]
    return "\n".join(L) + "\n"


def render_json(doc):
    return json.dumps(doc, indent=1) + "\n"


def main(argv=None):
    ap = argparse.ArgumentParser()
    ap.add_argument("--root", default=os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
    ap.add_argument("--write", action="store_true")
    ap.add_argument("--check", action="store_true")
    ap.add_argument("--json", action="store_true")
    a = ap.parse_args(argv)
    doc = build(a.root)
    md, js = render_md(doc), render_json(doc)
    if a.write:
        for rel, body in ((OUT_MD, md), (OUT_JSON, js)):
            with open(os.path.join(a.root, rel), "w", encoding="utf-8", newline="\n") as f:
                f.write(body)
    if a.check:
        bad = [rel for rel, body in ((OUT_MD, md), (OUT_JSON, js))
               if not os.path.isfile(os.path.join(a.root, rel)) or read(os.path.join(a.root, rel)) != body]
        if bad:
            print("producer_chain: stale generated file(s): " + ", ".join(bad) + " (run --write)")
            return 1
    if a.check and doc["unowned_no_producer"]:
        print("producer_chain: no-producer product(s) without an owner lane or 102b trigger: " + ", ".join(doc["unowned_no_producer"]))
        return 1
    if a.json:
        sys.stdout.write(js)
    elif not a.write and not a.check:
        sys.stdout.write(md)
    else:
        print("producer_chain: counts %s, acyclic %s" % (doc["counts"], doc["scheduler"]["acyclic"]))
    return 0 if doc["scheduler"]["acyclic"] else 1


if __name__ == "__main__":
    sys.exit(main())
