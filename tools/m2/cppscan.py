# Copyright(C) 2026 VP-V contributors
# SPDX-License-Identifier: GPL-3.0-only
"""Small C++ block scanner for tools/m2/move_tests.py (stdlib only).

mask_cpp() blanks comments, string/char literals (including raw strings R"delim(...)delim") and
preprocessor lines while keeping every offset and newline. split_members() then cuts a scope into
members at depth 0 using brace matching on the masked text.
"""
import re

IDENT = re.compile(r"[A-Za-z_]\w*")
ATTR_BEGIN = re.compile(r"BEGIN_TEST_METHOD_ATTRIBUTE\s*\(\s*(\w+)\s*\)")
ATTR_END = re.compile(r"END_TEST_METHOD_ATTRIBUTE\s*\(\s*\)")
LABEL = re.compile(r"(public|private|protected)\s*:(?!:)")
TEST_CLASS = re.compile(r"\bTEST_CLASS\s*\(\s*(\w+)\s*\)")
TEST_METHOD = re.compile(r"\bTEST_METHOD\s*\(\s*(\w+)\s*\)")
NAMESPACE = re.compile(r"namespace\b\s*([\w:]*)")


def mask_cpp(text):
    n = len(text)
    out = list(text)

    def blank(a, b):
        for k in range(a, min(b, n)):
            if out[k] not in "\n\r":
                out[k] = " "

    i = 0
    line_start = True
    while i < n:
        c = text[i]
        if c == "\n":
            line_start = True
            i += 1
            continue
        if c in " \t\r":
            i += 1
            continue
        if line_start and c == "#":
            # preprocessor logical line (with backslash continuations)
            j = i
            while True:
                nl = text.find("\n", j)
                if nl < 0:
                    j = n
                    break
                if text[nl - 1] == "\\" or (nl >= 2 and text[nl - 1] == "\r" and text[nl - 2] == "\\"):
                    j = nl + 1
                    continue
                j = nl
                break
            blank(i, j)
            i = j
            line_start = False
            continue
        line_start = False
        if c == "/" and i + 1 < n and text[i + 1] == "/":
            j = text.find("\n", i)
            j = n if j < 0 else j
            blank(i, j)
            i = j
            continue
        if c == "/" and i + 1 < n and text[i + 1] == "*":
            j = text.find("*/", i + 2)
            j = n if j < 0 else j + 2
            blank(i, j)
            # a block comment keeps "line_start" semantic unchanged
            i = j
            continue
        if c == '"':
            k = i - 1
            while k >= 0 and (text[k].isalnum() or text[k] == "_"):
                k -= 1
            tok = text[k + 1:i]
            if tok in ("R", "LR", "uR", "UR", "u8R"):
                m = re.compile(r'"([^()\\ \t\r\n]{0,16})\(').match(text, i)
                if m:
                    end = ")" + m.group(1) + '"'
                    j = text.find(end, m.end())
                    j = n if j < 0 else j + len(end)
                    blank(i, j)
                    i = j
                    continue
            j = i + 1
            while j < n and text[j] != '"':
                if text[j] == "\\":
                    j += 1
                elif text[j] == "\n":
                    break
                j += 1
            j = min(j + 1, n)
            blank(i, j)
            i = j
            continue
        if c == "'":
            k = i - 1
            while k >= 0 and (text[k].isalnum() or text[k] in "_."):
                k -= 1
            tok = text[k + 1:i]
            if tok and tok[0].isdigit():
                i += 1  # digit separator
                continue
            j = i + 1
            while j < n and text[j] != "'":
                if text[j] == "\\":
                    j += 1
                elif text[j] == "\n":
                    break
                j += 1
            j = min(j + 1, n)
            blank(i, j)
            i = j
            continue
        i += 1
    return "".join(out)


def match_braces(mask):
    stack, match = [], {}
    for i, ch in enumerate(mask):
        if ch == "{":
            stack.append(i)
        elif ch == "}":
            if not stack:
                raise ValueError("unbalanced '}' at offset %d" % i)
            match[stack.pop()] = i
    if stack:
        raise ValueError("unbalanced '{' at offset %d" % stack[-1])
    return match


class Member:
    __slots__ = ("start", "end", "kind", "name", "head", "body", "lead", "children", "parent", "unnamed")

    def __init__(self):
        self.children = None
        self.parent = None
        self.body = None
        self.name = None
        self.unnamed = False

    def __repr__(self):
        return "<%s %s %d-%d>" % (self.kind, self.name, self.start, self.end)


def _skip_ws(mask, i, hi):
    while i < hi and mask[i] in " \t\r\n":
        i += 1
    return i


def split_members(mask, match, lo, hi):
    """Members of the scope mask[lo:hi] at depth 0."""
    out = []
    i = lo
    while True:
        i = _skip_ws(mask, i, hi)
        if i >= hi:
            break
        start = i
        m = ATTR_BEGIN.match(mask, i)
        if m:
            e = ATTR_END.search(mask, m.end(), hi)
            end = e.end() if e else m.end()
            mem = Member()
            mem.start, mem.end, mem.kind, mem.name, mem.head = start, end, "attr", m.group(1), ""
            out.append(mem)
            i = end
            continue
        m = LABEL.match(mask, i)
        if m:
            mem = Member()
            mem.start, mem.end, mem.kind, mem.name, mem.head = start, m.end(), "label", m.group(1), ""
            out.append(mem)
            i = m.end()
            continue
        depth_p = 0
        j = i
        end = None
        body = None
        while j < hi:
            ch = mask[j]
            if ch == "(":
                depth_p += 1
            elif ch == ")":
                depth_p = max(0, depth_p - 1)
            elif ch == ";" and depth_p == 0:
                end = j + 1
                break
            elif ch == "{":
                k = match[j]
                if depth_p > 0:
                    j = k + 1
                    continue
                head = mask[start:j]
                nxt = _skip_ws(mask, k + 1, hi)
                hs = head.lstrip()
                if nxt < hi and mask[nxt] == ";":
                    end = nxt + 1
                    body = (j, k)
                    break
                if (re.match(r"(template\s*<[^>]*>\s*)?(struct|class|union|enum)\b", hs)
                        and not hs.startswith("TEST_")) or _has_initializer(head):
                    j = k + 1
                    continue
                end = k + 1
                body = (j, k)
                break
            j += 1
        if end is None:
            end = hi
        head = mask[start:(body[0] if body else end)]
        mem = Member()
        mem.start, mem.end, mem.head, mem.body = start, end, head, body
        hs = head.strip()
        mc = TEST_CLASS.match(hs)
        mm = TEST_METHOD.match(hs)
        mn = NAMESPACE.match(hs) if hs.startswith("namespace") else None
        if mc and body:
            mem.kind, mem.name = "class", mc.group(1)
        elif mm and body:
            mem.kind, mem.name = "method", mm.group(1)
        elif mn and body:
            mem.kind, mem.name = "namespace", mn.group(1)
            mem.unnamed = (mn.group(1) == "")
        else:
            mem.kind = "other"
        out.append(mem)
        i = end
    return out


def _has_initializer(head):
    """'=' at paren depth 0 that is an initializer (not part of operator==, <=, etc.)."""
    if "operator" in head:
        return False
    depth = 0
    for i, ch in enumerate(head):
        if ch == "(":
            depth += 1
        elif ch == ")":
            depth -= 1
        elif ch == "=" and depth == 0:
            prev = head[i - 1] if i else ""
            nxt = head[i + 1] if i + 1 < len(head) else ""
            if prev in "=!<>" or nxt == "=":
                continue
            return True
    return False


def build_tree(text, mask=None, match=None):
    """Top-level members with children filled for namespaces and TEST_CLASS bodies."""
    mask = mask or mask_cpp(text)
    match = match or match_braces(mask)

    def fill(members, parent):
        for m in members:
            m.parent = parent
            if m.kind in ("namespace", "class") and m.body:
                m.children = split_members(mask, match, m.body[0] + 1, m.body[1])
                fill(m.children, m)
            m.lead = lead_start(text, mask, m.start)
        return members

    top = split_members(mask, match, 0, len(mask))
    return fill(top, None), mask, match


def walk(members):
    for m in members:
        yield m
        if m.children:
            yield from walk(m.children)


def lead_start(text, mask, start):
    """Start offset including contiguous comment-only lines directly above `start`."""
    ls = text.rfind("\n", 0, start) + 1
    if text[ls:start].strip() != "":
        return start  # shares its line with earlier code
    pos = ls
    while pos > 0:
        pls = text.rfind("\n", 0, pos - 1) + 1
        raw = text[pls:pos]
        if raw.strip() != "" and mask[pls:pos].strip() == "":
            pos = pls
        else:
            break
    return pos


def line_span(text, mask, member):
    """(start, end) covering the member's whole lines, including leading comment lines and a
    trailing same-line comment, when the member owns its lines; else the exact span."""
    s = member.lead
    owns_start = text[text.rfind("\n", 0, s) + 1:s].strip() == ""
    e = member.end
    nl = text.find("\n", e)
    tail_end = len(text) if nl < 0 else nl
    owns_end = mask[e:tail_end].strip() == ""
    if owns_start and owns_end:
        return text.rfind("\n", 0, s) + 1, (len(text) if nl < 0 else nl + 1)
    return s, e


def delete_spans(text, spans):
    """Delete spans (sorted/merged); also drop one blank line before a deleted block when the
    following line is blank or a closing brace line."""
    spans = sorted(spans)
    merged = []
    for s, e in spans:
        if merged and s <= merged[-1][1]:
            merged[-1] = (merged[-1][0], max(e, merged[-1][1]))
        else:
            merged.append((s, e))
    out = []
    pos = 0
    for s, e in merged:
        out.append(text[pos:s])
        # blank line handling
        prev_nl = text.rfind("\n", 0, s - 1) + 1 if s > 0 else 0
        prev_line = text[prev_nl:s] if s > 0 else None
        if prev_line is not None and prev_line.strip() == "" and s > 0 and text[s - 1] == "\n":
            nxt_end = text.find("\n", e)
            nxt_line = text[e:(len(text) if nxt_end < 0 else nxt_end)]
            if nxt_line.strip() == "" or nxt_line.lstrip().startswith("}") or e >= len(text):
                # remove the blank line we just emitted
                joined = "".join(out)
                if joined.endswith(prev_line):
                    joined = joined[: len(joined) - len(prev_line)]
                    out = [joined]
        pos = e
    out.append(text[pos:])
    return "".join(out)


def idents(mask):
    return set(IDENT.findall(mask))


def count_ident(mask, name, excl=None):
    """Occurrences of identifier `name` in mask, outside the excluded (start,end) span."""
    n = 0
    for m in re.finditer(r"(?<![\w])" + re.escape(name) + r"(?![\w])", mask):
        if excl and excl[0] <= m.start() < excl[1]:
            continue
        n += 1
    return n


_QUALS = ("static", "inline", "constexpr", "consteval", "template", "extern", "typedef", "using")


def defined_ident(member):
    """Identifier a helper member defines, or None when unsure / not a helper."""
    if member.kind != "other":
        return None
    h = member.head.strip()
    if not h or h.startswith("using namespace") or h.startswith("static_assert") or h.startswith("TEST_"):
        return None
    h = re.sub(r"^template\s*<[^>]*>\s*", "", h)
    m = re.match(r"(?:struct|class|union)\s+(?:alignas\([^)]*\)\s*)?(\w+)", h)
    if m:
        return m.group(1)
    m = re.match(r"enum(?:\s+class|\s+struct)?\s+(\w+)", h)
    if m:
        return m.group(1)
    m = re.match(r"using\s+(\w+)\s*=", h)
    if m:
        return m.group(1)
    if h.startswith("typedef"):
        t = IDENT.findall(h)
        return t[-1] if t else None
    if h.startswith("#"):
        return None
    p = h.find("(")
    e = h.find("=")
    if p >= 0 and (e < 0 or p < e):
        pre = h[:p].rstrip()
        m = re.search(r"(\w+)\s*$", pre)
        if not m or "operator" in pre:
            return None
        name = m.group(1)
        if name in ("if", "for", "while", "switch", "return", "decltype", "sizeof"):
            return None
        return name
    cut = len(h)
    for ch in "={[":
        k = h.find(ch)
        if 0 <= k < cut:
            cut = k
    pre = h[:cut].rstrip(" ;\t\r\n")
    m = re.search(r"(\w+)\s*$", pre)
    return m.group(1) if m else None


def external_linkage_definition(member, in_unnamed):
    """True for a namespace-scope function/variable definition that has external linkage."""
    if in_unnamed or member.kind != "other":
        return False
    h = member.head.strip()
    if re.match(r"template", h) or h.startswith("#"):
        return False
    is_func = "(" in h and "=" not in h.split("(")[0]
    first = h.split("(")[0] if is_func else h
    toks = set(IDENT.findall(first))
    if toks & set(_QUALS) or toks & {"struct", "class", "enum", "union", "namespace"}:
        return False
    if defined_ident(member) is None:
        return False
    if is_func:
        return member.body is not None
    if "const" in toks:
        return False
    return True
