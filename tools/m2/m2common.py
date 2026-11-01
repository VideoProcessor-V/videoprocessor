# Copyright(C) 2026 VP-V contributors
# SPDX-License-Identifier: GPL-3.0-only
"""Shared helpers for the tools/m2 scripts: git access, MSBuild project text tokenising, props IO.

Stdlib only. Nothing here re-serialises XML: every edit is a text-span edit so that bytes
outside the edited spans stay identical.
"""
import re
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
DEFAULT_BILL = "a591ad3b"

# Bill projects whose items are moved by P1 (only those that exist at the bill ref).
BILL_PROJECTS = [
    "src/VideoProcessor-Lib/VideoProcessor-Lib.vcxproj",
    "src/VideoProcessor-Test/VideoProcessor-Test.vcxproj.vcxproj",
    "src/VideoProcessor-Config/VideoProcessor-Config.vcxproj",
    "src/VideoProcessor-ConfigTests/VideoProcessor-ConfigTests.vcxproj",
    "src/VideoProcessor-GUI/VideoProcessor-GUI.vcxproj",
    "src/VideoProcessor-VPRenderer/VideoProcessor-VPRenderer.vcxproj",
    "src/VideoProcessor-VPRenderer/VideoProcessor-VPRendererBeta.vcxproj",
    "src/VideoProcessor-BetaTests/VideoProcessor-BetaTests.vcxproj",
]
PROPS_DIR = "src/VP-V/build"


def props_name(project_path):
    """Project file basename with every trailing .vcxproj removed."""
    name = Path(project_path).name
    while name.endswith(".vcxproj"):
        name = name[: -len(".vcxproj")]
    return name


def props_path(project_path):
    return "%s/%s.items.props" % (PROPS_DIR, props_name(project_path))


# ---------------------------------------------------------------- git

def git(*args, check=True, text=False):
    p = subprocess.run(["git", "-C", str(ROOT)] + list(args), stdout=subprocess.PIPE,
                       stderr=subprocess.PIPE)
    if check and p.returncode != 0:
        raise RuntimeError("git %s failed: %s" % (" ".join(args), p.stderr.decode("utf-8", "replace")))
    return p.stdout.decode("utf-8", "replace") if text else p.stdout


def git_ok(*args):
    return subprocess.run(["git", "-C", str(ROOT)] + list(args), stdout=subprocess.DEVNULL,
                          stderr=subprocess.DEVNULL).returncode == 0


def bill_exists(bill, path):
    return git_ok("cat-file", "-e", "%s:%s" % (bill, path))


def bill_bytes(bill, path):
    if not bill_exists(bill, path):
        return None
    return git("show", "%s:%s" % (bill, path))


def ls_files(*pathspecs):
    out = git("ls-files", "-z", "--", *pathspecs)
    return sorted(p for p in out.decode("utf-8").split("\0") if p)


def bill_ls(bill, *pathspecs):
    out = git("ls-tree", "-r", "-z", "--name-only", bill, "--", *pathspecs)
    return sorted(p for p in out.decode("utf-8").split("\0") if p)


# ---------------------------------------------------------------- text IO (preserve BOM / newlines)

def decode(raw):
    """bytes -> (text, bom). Newlines are kept as found."""
    bom = raw.startswith(b"\xef\xbb\xbf")
    if bom:
        raw = raw[3:]
    return raw.decode("utf-8"), bom


def encode(text, bom):
    return (b"\xef\xbb\xbf" if bom else b"") + text.encode("utf-8")


def read_working(path):
    p = ROOT / path
    if not p.exists():
        return None
    return p.read_bytes()


def write_if_changed(path, data):
    p = ROOT / path
    if p.exists() and p.read_bytes() == data:
        return False
    p.parent.mkdir(parents=True, exist_ok=True)
    p.write_bytes(data)
    return True


# ---------------------------------------------------------------- MSBuild text tokenising

_TOKEN = re.compile(
    r"<!--.*?-->|<\?.*?\?>|<!\[CDATA\[.*?\]\]>|"
    r"<(/?)([A-Za-z_][\w:.\-]*)((?:[^>\"']|\"[^\"]*\"|'[^']*')*?)(/?)>",
    re.S)
_ATTR = re.compile(r"([\w:.\-]+)\s*=\s*(\"[^\"]*\"|'[^']*')")


def attrs_of(attr_text):
    return {m.group(1): m.group(2)[1:-1] for m in _ATTR.finditer(attr_text)}


class Item:
    __slots__ = ("tag", "include", "start", "end", "text", "lead", "group", "attrs", "key", "children")

    def __init__(self):
        self.lead = []      # [(start, end, text)] comment spans directly preceding the item
        self.children = []  # child element texts (for filters: <Filter> value)


class Group:
    __slots__ = ("start", "end", "open_end", "close_start", "attrs", "items", "comments", "index")

    def __init__(self):
        self.items = []
        self.comments = []


def norm_include(inc):
    s = inc.replace("/", "\\")
    while s.startswith(".\\"):
        s = s[2:]
    return s.lower()


def parse_project(text):
    """Return list of top-level ItemGroups (depth 1 directly under the root) with their items."""
    groups = []
    stack = []
    cur_group = None
    cur_item = None
    pending_comments = []   # comments at group-child level awaiting an item
    last_child_end = None
    for m in _TOKEN.finditer(text):
        s = m.group(0)
        if s.startswith("<!--"):
            if cur_group is not None and cur_item is None and len(stack) == 2:
                pending_comments.append((m.start(), m.end(), s))
                cur_group.comments.append((m.start(), m.end(), s))
            continue
        if s.startswith("<?") or s.startswith("<![CDATA["):
            continue
        closing, name, attr_text, selfclose = m.group(1), m.group(2), m.group(3), m.group(4)
        if closing:
            if not stack:
                continue
            depth = len(stack)
            opened = stack.pop()
            if cur_item is not None and depth == 3 and opened == cur_item.tag and cur_group is not None:
                cur_item.end = m.end()
                cur_item.text = text[cur_item.start:cur_item.end]
                cur_group.items.append(cur_item)
                cur_item = None
            elif cur_group is not None and depth == 2 and opened == "ItemGroup":
                cur_group.end = m.end()
                cur_group.close_start = m.start()
                groups.append(cur_group)
                cur_group = None
                pending_comments = []
            continue
        depth_after = len(stack) + 1
        if name == "ItemGroup" and len(stack) == 1:
            cur_group = Group()
            cur_group.start = m.start()
            cur_group.open_end = m.end()
            cur_group.attrs = attrs_of(attr_text)
            pending_comments = []
            if selfclose:
                cur_group.end = m.end()
                cur_group.close_start = m.end()
                groups.append(cur_group)
                cur_group = None
            else:
                stack.append(name)
            continue
        if selfclose:
            if cur_group is not None and len(stack) == 2 and cur_item is None:
                a = attrs_of(attr_text)
                if "Include" in a:
                    it = Item()
                    it.tag, it.include, it.attrs = name, a["Include"], a
                    it.start, it.end = m.start(), m.end()
                    it.text = text[it.start:it.end]
                    it.key = (name, norm_include(a["Include"]))
                    it.lead = _take_lead(pending_comments, text, m.start())
                    pending_comments = []
                    cur_group.items.append(it)
                else:
                    pending_comments = []
            elif cur_group is not None and cur_item is not None and len(stack) == 3:
                pass
            continue
        # open tag
        if cur_group is not None and len(stack) == 2 and cur_item is None:
            a = attrs_of(attr_text)
            if "Include" in a:
                cur_item = Item()
                cur_item.tag, cur_item.include, cur_item.attrs = name, a["Include"], a
                cur_item.start = m.start()
                cur_item.key = (name, norm_include(a["Include"]))
                cur_item.lead = _take_lead(pending_comments, text, m.start())
                pending_comments = []
            else:
                pending_comments = []
        stack.append(name)
    return groups


def _take_lead(pending, text, item_start):
    """Comments directly preceding the item (only whitespace between)."""
    lead = []
    pos = item_start
    for (cs, ce, ct) in reversed(pending):
        if text[ce:pos].strip() == "":
            lead.insert(0, (cs, ce, ct))
            pos = cs
        else:
            break
    return lead


def all_items(groups):
    return [it for g in groups for it in g.items]


def item_filter_name(item):
    m = re.search(r"<Filter>([^<]*)</Filter>", item.text)
    return m.group(1) if m else None


# ---------------------------------------------------------------- span removal with line handling

def remove_spans(text, spans):
    """Remove [start,end) spans. Lines left whitespace-only by the removal are removed whole
    (with their newline); otherwise just the spans (plus one adjacent separating space)."""
    if not spans:
        return text
    spans = sorted(spans)
    mask = bytearray(len(text))
    for s, e in spans:
        for i in range(s, e):
            mask[i] = 1
    # line boundaries
    out_removed = mask
    pos = 0
    n = len(text)
    while pos < n:
        nl = text.find("\n", pos)
        end = n if nl < 0 else nl + 1
        if any(mask[pos:end]):
            rest = "".join(text[i] for i in range(pos, end) if not mask[i])
            if rest.strip() == "":
                for i in range(pos, end):
                    out_removed[i] = 1
            else:
                # partial removal: drop one separating space before a removed run
                i = pos
                while i < end:
                    if mask[i] and (i == pos or not mask[i - 1]):
                        j = i
                        while j < end and mask[j]:
                            j += 1
                        before_space = i > pos and text[i - 1] == " " and not mask[i - 1]
                        after_ok = j >= end or text[j] in " \r\n"
                        if before_space and after_ok:
                            out_removed[i - 1] = 1
                        i = j
                    else:
                        i += 1
        pos = end
    return "".join(text[i] for i in range(n) if not out_removed[i])


def item_group_span_with_lead(it):
    s = it.start
    if it.lead:
        s = it.lead[0][0]
    return s, it.end


def norm_item_text(text, full_text, start):
    """Item text with the indentation of its first line removed from continuation lines;
    first line unindented. Returns '\\n'-joined text."""
    ls = full_text.rfind("\n", 0, start) + 1
    prefix = full_text[ls:start]
    base = prefix if prefix.strip() == "" else ""
    lines = text.replace("\r\n", "\n").split("\n")
    out = [lines[0]]
    for ln in lines[1:]:
        if base and ln.startswith(base):
            ln = ln[len(base):]
        out.append(ln.rstrip())
    return "\n".join(out)


def detect_newline(text, default="\r\n"):
    i = text.find("\n")
    if i < 0:
        return default
    return "\r\n" if i > 0 and text[i - 1] == "\r" else "\n"


# ---------------------------------------------------------------- props files

PROPS_HEADER = (
    "<?xml version=\"1.0\" encoding=\"utf-8\"?>\n"
    "<!--\n"
    "  Copyright(C) 2026 VP-V contributors\n"
    "  SPDX-License-Identifier: GPL-3.0-only\n"
    "  VP-V items for {name}; maintained by tools/m2/move_project_items.py;\n"
    "  Include paths are relative to the importing project's directory (MSBuild resolves item paths\n"
    "  against the project).\n"
    "-->\n"
    "<Project xmlns=\"http://schemas.microsoft.com/developer/msbuild/2003\">\n"
)


class PropsItem:
    def __init__(self, tag, include, norm_text, key):
        self.tag, self.include, self.norm_text, self.key = tag, include, norm_text, key


def parse_props(raw):
    """-> (items, newline, bom) from an existing props file, or ([], None, False)."""
    if raw is None:
        return [], None, False
    text, bom = decode(raw)
    nl = detect_newline(text)
    groups = parse_project(text)
    items = []
    for g in groups:
        for it in g.items:
            s = it.lead[0][0] if it.lead else it.start
            full = text[s:it.end]
            norm = norm_item_text(full, text, s)
            items.append(PropsItem(it.tag, it.include, norm, it.key))
    return items, nl, bom


def render_props(name, items, newline):
    tags = []
    for it in items:
        if it.tag not in tags:
            tags.append(it.tag)
    out = [PROPS_HEADER.format(name=name)]
    for tag in tags:
        out.append("  <ItemGroup>\n")
        for it in items:
            if it.tag != tag:
                continue
            lines = it.norm_text.split("\n")
            out.append("    " + lines[0] + "\n")
            for ln in lines[1:]:
                out.append(("    " + ln if ln.strip() else "") + "\n")
        out.append("  </ItemGroup>\n")
    out.append("</Project>\n")
    text = "".join(out)
    if newline == "\r\n":
        text = text.replace("\n", "\r\n")
    return text.encode("utf-8")


def eprint(*a):
    print(*a, file=sys.stderr)


# ---------------------------------------------------------------- test_moves.json

CONFIG = "tools/m2/test_moves.json"


def load_config():
    import json
    from collections import OrderedDict
    raw = (ROOT / CONFIG).read_text(encoding="utf-8")
    return json.loads(raw, object_pairs_hook=OrderedDict)


def save_config(cfg):
    import json
    text = json.dumps(cfg, indent=1, ensure_ascii=False) + "\n"
    write_if_changed(CONFIG, text.encode("utf-8"))
