# Copyright(C) 2026 VP-V contributors
# SPDX-License-Identifier: GPL-3.0-only
"""Bug-ticket intake: StateReport bundle, soak triage seed or loop-rig failure -> BUG-NNNN.md.

python -I tools/triage/intake.py INPUT --bugs-dir DIR [--tag Picture|Judder|Colour|Sync|Other|Auto] [--title TEXT]

Exit codes: 0 written, 2 usage / bugs-dir refused, 3 unreadable input, 4 input already ingested.
"""
import argparse
import datetime
import hashlib
import json
import os
import re
import sys
import zipfile

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(os.path.dirname(HERE), "vplog"))
import parse  # noqa: E402

TAGS = ("Picture", "Judder", "Colour", "Sync", "Other")
WINDOW_S = 30.0
MAX_LINES = 80
DUP_THRESHOLD = 0.6
UNKNOWN = "unknown"
_ID_RE = re.compile(r"^BUG-(\d{4,})\.md$")
_KEY_RE = re.compile(r"^([a-z_0-9]+): ?(.*)$")


def one_line(value, limit=300):
    text = " ".join(str(value).split()) if value is not None else ""
    return text[:limit] if text else UNKNOWN


def build_family(build_id):
    """Text before the last '+' or '-' segment; unchanged when there is none."""
    if not build_id or build_id == UNKNOWN:
        return UNKNOWN
    cut = max(build_id.rfind("+"), build_id.rfind("-"))
    return build_id[:cut] if cut > 0 else build_id


def _repo_root():
    cur = HERE
    while True:
        if os.path.exists(os.path.join(cur, ".git")):
            return cur
        up = os.path.dirname(cur)
        if up == cur:
            return None
        cur = up


def bugs_dir_ok(path):
    root = _repo_root()
    if root is None:
        return True
    a = os.path.normcase(os.path.realpath(path))
    b = os.path.normcase(os.path.realpath(root))
    return not (a == b or a.startswith(b + os.sep))


def _parse_time(text):
    if not text or text == UNKNOWN:
        return None
    try:
        return datetime.datetime.fromisoformat(str(text).strip()).replace(tzinfo=None)
    except ValueError:
        return None


# ---- readers -------------------------------------------------------------------------------

def read_bundle(path):
    try:
        zf = zipfile.ZipFile(path)
    except (zipfile.BadZipFile, OSError) as exc:
        raise ValueError("cannot read zip: %s" % exc)
    with zf:
        names = zf.namelist()

        def text(name):
            if name not in names:
                return None
            return zf.read(name).decode("utf-8", "replace")

        def jtext(name):
            try:
                doc = json.loads(text(name) or "{}")
            except ValueError:
                return {}
            return doc if isinstance(doc, dict) else {}

        manifest = jtext("manifest.json")
        live = jtext("live-settings.json")
        log_text = text("log-excerpt.txt") or ""
        crash = text("crash-state.txt") or ""
        h = hashlib.sha256()
        cfg = sorted(n for n in names if n.startswith("config/") and not n.endswith("/"))
        for n in cfg:
            h.update(n.encode("utf-8"))
            h.update(zf.read(n))
    lines = log_text.splitlines()
    trigger = str(manifest.get("trigger") or UNKNOWN)
    return {
        "input_kind": "bundle", "bundle": os.path.abspath(path),
        "build_id": str(manifest.get("build_id") or UNKNOWN), "trigger": trigger,
        "config_fingerprint": h.hexdigest()[:16] if cfg else UNKNOWN,
        "source_format": _source_format(crash, live),
        "trigger_time": _parse_time(manifest.get("created_local")),
        "lines": lines, "events": parse.parse_lines(lines),
        "detail": "commit: %s; version: %s; renderer: %s" % (
            manifest.get("commit") or UNKNOWN, manifest.get("version") or UNKNOWN,
            manifest.get("renderer") or UNKNOWN),
        "text_hint": trigger,
    }


def _source_format(crash, live):
    for line in crash.splitlines():
        m = re.match(r"^\s*(source[\w ]*?)\s*[:=]\s*(\S.*)$", line, re.I)
        if m:
            return one_line(m.group(2), 120)
    for key, value in live.items():
        if "source" in str(key).lower() and value not in (None, ""):
            return one_line(value, 120)
    return UNKNOWN


def _events_for(log_path, raw_lines):
    if log_path and os.path.isfile(log_path):
        try:
            with open(log_path, "r", encoding="utf-8", errors="replace") as fh:
                lines = fh.read().splitlines()
            return lines, parse.parse_lines(lines)
        except OSError:
            pass
    lines = [str(x) for x in raw_lines]
    return lines, parse.parse_lines(lines)


def read_soak(doc):
    build = str(doc.get("build_id") or UNKNOWN)
    check = str(doc.get("check") or UNKNOWN)
    raw = [e.get("raw", "") for e in doc.get("first_events") or [] if isinstance(e, dict)]
    lines, events = _events_for(doc.get("log_path"), raw)
    return {"input_kind": "soak", "bundle": str(doc.get("out_dir") or UNKNOWN),
            "build_id": build, "trigger": check,
            "config_fingerprint": hashlib.sha256((build + check).encode()).hexdigest()[:16],
            "source_format": UNKNOWN, "trigger_time": _parse_time(doc.get("trigger_wall")),
            "lines": lines, "events": events,
            "detail": "verdict: %s; %s" % (doc.get("verdict") or UNKNOWN, doc.get("detail") or UNKNOWN),
            "text_hint": "%s %s" % (check, doc.get("detail") or "")}


def read_looprig(doc):
    build = str(doc.get("build_id") or UNKNOWN)
    scen = str(doc.get("scenario") or UNKNOWN)
    lines, events = _events_for(doc.get("log_path"), [str(x) for x in doc.get("first_frames") or []])
    trig = "%s/%s/%s" % (scen, doc.get("row") or UNKNOWN, doc.get("measure") or UNKNOWN)
    return {"input_kind": "looprig", "bundle": str(doc.get("log_path") or UNKNOWN),
            "build_id": build, "trigger": trig,
            "config_fingerprint": hashlib.sha256((build + scen).encode()).hexdigest()[:16],
            "source_format": UNKNOWN, "trigger_time": None, "lines": lines, "events": events,
            "detail": "measure %s: value %s vs threshold %s" % (
                doc.get("measure") or UNKNOWN, doc.get("value", UNKNOWN), doc.get("threshold", UNKNOWN)),
            "text_hint": trig}


def read_input(path):
    with open(path, "rb") as fh:
        data = fh.read()
    sha = hashlib.sha256(data).hexdigest()
    if path.lower().endswith(".zip") or data[:2] == b"PK":
        info = read_bundle(path)
    else:
        try:
            doc = json.loads(data.decode("utf-8"))
        except ValueError as exc:
            raise ValueError("not JSON: %s" % exc)
        if not isinstance(doc, dict):
            raise ValueError("JSON input is not an object")
        if doc.get("source") == "soak":
            info = read_soak(doc)
        elif doc.get("source") == "looprig":
            info = read_looprig(doc)
        else:
            raise ValueError("unknown JSON source: %r" % doc.get("source"))
    info["input_sha256"] = sha
    return info


# ---- analysis ------------------------------------------------------------------------------

def window_events(info):
    """Return (events, excerpt lines, note)."""
    events = info["events"]
    trig = info.get("trigger_time")
    walled = [e for e in events if e.wall is not None]
    if trig is not None and walled:
        sel = [e for e in walled if abs((e.wall - trig).total_seconds()) <= WINDOW_S]
        sel.sort(key=lambda e: abs((e.wall - trig).total_seconds()))
        sel = sorted(sel[:MAX_LINES], key=lambda e: e.line_no)
        return sel, [e.raw for e in sel], "events within +/-30 s of the trigger, max 80"
    lines = [ln for ln in info.get("lines", []) if ln.strip()] or [e.raw for e in events]
    return list(events[-MAX_LINES:]), lines[-MAX_LINES:], "no trigger wall time: last 80 lines"


def _ident(e):
    f = e.fields
    return f.get("cause") or f.get("id") or f.get("reason") or f.get("api") or ""


def derive_tag(events, hint):
    text = (hint or "").lower()
    kinds = {e.kind for e in events}
    cats = {parse.category(e) for e in events}
    if "gpu_error" in kinds or re.search(r"gpu|device", text):
        return "Picture"
    if cats & {"drop", "cadence"} or kinds & {"repeat", "cadence"} or re.search(r"drop|repeat|cadence|judder", text):
        return "Judder"
    if "source_change" in kinds or re.search(r"colou?r|hdr|tonemap|black level|gamut|transfer", text):
        return "Colour"
    if "gate" in kinds or "resync" in cats or "resync" in kinds or re.search(r"resync|gate|sync", text):
        return "Sync"
    return "Other"


def signature_set(events, tag, build_id):
    sig = {"%s:%s" % (e.kind, _ident(e)) for e in events}
    sig.add("tag:" + tag)
    sig.add("family:" + build_family(build_id))
    return sig


def jaccard(a, b):
    if not a and not b:
        return 1.0
    return len(a & b) / float(len(a | b))


# ---- bugs dir ------------------------------------------------------------------------------

def read_ticket(path):
    meta = {}
    try:
        with open(path, "r", encoding="utf-8", errors="replace") as fh:
            for line in fh:
                if line.startswith("## "):
                    break
                m = _KEY_RE.match(line.rstrip("\n"))
                if m and m.group(1) not in meta:
                    meta[m.group(1)] = m.group(2).strip()
    except OSError:
        pass
    return meta


def tickets(bugs_dir):
    out = []
    for name in sorted(os.listdir(bugs_dir)):
        m = _ID_RE.match(name)
        if m:
            meta = read_ticket(os.path.join(bugs_dir, name))
            meta.setdefault("id", name[:-3])
            out.append((int(m.group(1)), meta))
    return out


def write_index(bugs_dir):
    rows = ["# Bug index", "",
            "| id | status | tag | trigger | build | date | title | dup-of |",
            "|---|---|---|---|---|---|---|---|"]
    for _, meta in tickets(bugs_dir):
        cells = [(meta.get(k) or UNKNOWN).replace("|", "/") for k in
                 ("id", "status", "tag", "trigger", "build_id", "date", "title", "dup_of")]
        rows.append("| %s |" % " | ".join(cells))
    with open(os.path.join(bugs_dir, "INDEX.md"), "w", encoding="utf-8", newline="\n") as fh:
        fh.write("\n".join(rows) + "\n")


def fill_template(template, values):
    return re.sub(r"\{\{(\w+)\}\}", lambda m: values.get(m.group(1), UNKNOWN), template)


def summary_text(events):
    s = parse.summarize(events)
    lines = []
    for key in ("total", "first_wall", "last_wall", "malformed"):
        lines.append("- %s: %s" % (key, s.get(key) if s.get(key) is not None else UNKNOWN))
    for key in ("counts_by_kind", "counts_by_category", "counts_by_cause"):
        body = ", ".join("%s=%d" % kv for kv in sorted(s[key].items())) or "none"
        lines.append("- %s: %s" % (key, body))
    return "\n".join(lines)


def main(argv=None):
    ap = argparse.ArgumentParser(prog="intake.py")
    ap.add_argument("input")
    ap.add_argument("--bugs-dir")
    ap.add_argument("--tag", default="Auto", choices=TAGS + ("Auto",))
    ap.add_argument("--title")
    try:
        args = ap.parse_args(argv)
    except SystemExit:
        return 2
    if not args.bugs_dir:
        print("error: --bugs-dir is required", file=sys.stderr)
        return 2
    if not bugs_dir_ok(args.bugs_dir):
        print("error: --bugs-dir must not be inside the source repository", file=sys.stderr)
        return 2
    try:
        info = read_input(args.input)
    except (ValueError, OSError) as exc:
        print("error: %s" % exc, file=sys.stderr)
        return 3
    os.makedirs(args.bugs_dir, exist_ok=True)
    existing = tickets(args.bugs_dir)
    for _, meta in existing:
        if meta.get("input_sha256") == info["input_sha256"]:
            print("already ingested as %s" % meta["id"])
            return 4

    events, excerpt, note = window_events(info)
    tag = args.tag if args.tag != "Auto" else derive_tag(events, info.get("text_hint"))
    sig = signature_set(events, tag, info["build_id"])
    dup, best = "none", 0.0
    for _, meta in existing:
        if meta.get("status") != "open" or not meta.get("signature"):
            continue
        score = jaccard(sig, set(filter(None, meta["signature"].split("|"))))
        if score >= DUP_THRESHOLD and score > best:
            best, dup = score, meta["id"]
    bug_id = "BUG-%04d" % (max([n for n, _ in existing], default=0) + 1)
    title = one_line(args.title) if args.title else "%s: %s" % (tag, one_line(info["trigger"], 80))
    values = {
        "id": bug_id, "title": title, "date": datetime.date.today().isoformat(), "tag": tag,
        "trigger": one_line(info["trigger"]), "build_id": one_line(info["build_id"]),
        "build_family": build_family(info["build_id"]),
        "config_fingerprint": info["config_fingerprint"], "source_format": one_line(info["source_format"]),
        "input_kind": info["input_kind"], "bundle": one_line(info["bundle"], 500),
        "input_sha256": info["input_sha256"], "dup_of": dup,
        "signature": "|".join(sorted(sig)),
        "duplicate_note": ("Possible duplicate of %s" % dup) if dup != "none" else "No duplicate found.",
        "detail": info.get("detail") or UNKNOWN, "summary": summary_text(events),
        "excerpt_note": note, "excerpt": "\n".join(excerpt) if excerpt else UNKNOWN,
    }
    with open(os.path.join(HERE, "TICKET-TEMPLATE.md"), "r", encoding="utf-8") as fh:
        body = fill_template(fh.read(), values)
    with open(os.path.join(args.bugs_dir, bug_id + ".md"), "w", encoding="utf-8", newline="\n") as fh:
        fh.write(body)
    write_index(args.bugs_dir)
    print(bug_id)
    return 0


if __name__ == "__main__":
    sys.exit(main())
