# Copyright(C) 2026 VP-V contributors
# SPDX-License-Identifier: GPL-3.0-only
"""VP-V footprint in Bill's files.

Measures how much VP-V code lives in Bill's (upstream) files, by comparing a
Bill ref with a head ref.  Pure git + Python stdlib.

  python -I tools/vp_footprint.py [--bill a591ad3b] [--head HEAD]
         [--files PATH...] [--json] [--append CSV] [--budget JSON]
         [--top N] [--selftest]

Hook lines: an added line that ends with `// VPV-HOOK`, or lies between a line
that is exactly `// VPV-HOOK` and a following `// VPV-HOOK-END`, or any line of
a file whose first line is `// VPV-HOOK-FILE`.  Stray = adds - hook adds.
Hook / stray are INFORMATION only.

THE METRIC is the honest H2 (CC-audit-1, rule 19; WAIVER-CC-H2-METRIC):
  H1 = non-blank added or changed lines vs the Bill ref (git diff -w --histogram
       --no-renames -U0), tagged or not, in every Bill-owned file (present at the
       Bill ref, outside src/VP-V/);
  H2 = H1 + the non-blank lines, recursively, of every VP-authored file
       (src/VP-V/**, or a file absent from Bill's tree) that an ADDED #include
       pulls into a class or function scope of that Bill file (scope_map).
An unresolved #include at class or function scope fails closed.  Moving lines
of a Bill class body into a fragment included at the same place can never lower
H2 (it rises by the include line).  --budget ceilings compare h2.
"""
import argparse
import csv
import datetime
import fnmatch
import json
import os
import posixpath
import re
import subprocess
import sys
import tempfile

VPV_PREFIX = "src/VP-V/"
PROJECT_EXT = (".vcxproj", ".filters", ".sln", ".props", ".targets")
HUNK_RE = re.compile(r"^@@ -(\d+)(?:,(\d+))? \+(\d+)(?:,(\d+))? @@")
DIFF_ARGS = ["-w", "--diff-algorithm=histogram", "--no-renames", "--no-color", "--no-ext-diff"]
INCLUDE_RE = re.compile(r'^\s*#\s*include\s*[<"]([^>"]+)[>"]')
# Any #include directive, parsable or not (a computed `#include MACRO` form): G-CC1 MUST-1.
INCLUDE_ANY_RE = re.compile(r'^\s*#\s*include\b')
# Include roots tried after the includer's own folder (the projects' AdditionalIncludeDirectories that VP-V uses).
INCLUDE_ROOTS = ("src", "src/VideoProcessor-Lib", "src/VideoProcessor-ConfigCore")
BUDGET_METRIC = "H2"


def read_json(path):
    with open(path, encoding="utf-8") as fh:
        return json.load(fh)


def git(args, repo=None, text=True):
    cmd = ["git"] + (["-C", repo] if repo else []) + args
    out = subprocess.run(cmd, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    if out.returncode != 0:
        raise RuntimeError("git %s failed: %s" % (" ".join(args), out.stderr.decode("utf-8", "replace")))
    return out.stdout.decode("utf-8", "replace") if text else out.stdout


def bucket_of(path):
    low = path.lower()
    if low.endswith(PROJECT_EXT):
        return "project"
    parts = low.split("/")
    base = parts[-1]
    if any(("test" in p) for p in parts[:-1]) or "tests" in base or base.startswith("test"):
        return "tests"
    return "production"


def parse_diff(text):
    """Return {path: {'adds': n, 'dels': n, 'hunks': n, 'added_lines': [lineno,...], 'nonblank_added_lines': [lineno,...],
    'include_lines': [lineno,...] (added #include lines), 'added_text': {lineno: text}}}."""
    files = {}
    cur = None
    new_line = 0
    in_header = False   # between `diff --git` and the first hunk: only there are `---` / `+++` file headers
    for raw in text.splitlines():
        if raw.startswith("diff --git "):
            cur = None
            in_header = True
            continue
        if in_header and raw.startswith("+++ "):
            p = raw[4:]
            if p == "/dev/null":
                cur = None
            else:
                p = p[2:] if p.startswith("b/") else p
                cur = files.setdefault(p.strip('"'), {"adds": 0, "dels": 0, "hunks": 0, "added_lines": [],
                                                      "nonblank_added_lines": [], "include_lines": [], "added_text": {}})
            continue
        if in_header and raw.startswith("--- "):
            continue
        if cur is None:
            continue
        m = HUNK_RE.match(raw)
        if m:
            in_header = False
            cur["hunks"] += 1
            new_line = int(m.group(3))
            continue
        if raw.startswith("+"):
            cur["adds"] += 1
            cur["added_lines"].append(new_line)
            body = raw[1:]
            if body.strip():
                cur["nonblank_added_lines"].append(new_line)
                cur["added_text"][new_line] = body
                if INCLUDE_ANY_RE.match(body):
                    cur["include_lines"].append(new_line)
            new_line += 1
        elif raw.startswith("-"):
            cur["dels"] += 1
    return files


def hook_line_set(content):
    """Set of 1-based line numbers in `content` that are hook lines."""
    lines = content.splitlines()
    out = set()
    if lines and lines[0].strip().startswith("// VPV-HOOK-FILE"):
        return set(range(1, len(lines) + 1))
    inblock = False
    for i, ln in enumerate(lines, 1):
        s = ln.strip()
        if s.startswith("// VPV-HOOK-END"):
            out.add(i)
            inblock = False
        elif s == "// VPV-HOOK":
            out.add(i)
            inblock = True
        elif inblock or s.endswith("// VPV-HOOK"):
            out.add(i)
    return out


# ---------------------------------------------------------------- scopes and VP includes (H2)

_FUNC_END_RE = re.compile(r"\)\s*(?:const\b|noexcept\b(?:\s*\([^()]*\))?|override\b|final\b|volatile\b|mutable\b|&&|&|"
                          r"throw\s*\(\s*\)|->\s*[^{};]+?)*\s*$")
_LAMBDA_END_RE = re.compile(r"\]\s*(?:mutable\s*)?$")
_CTOR_INIT_RE = re.compile(r"\)\s*(?:noexcept\b\s*)?:(?!:)")
_NAMESPACE_RE = re.compile(r"(?:^|\s)(?:inline\s+)?namespace\b[^=]*$")
_EXTERN_C_RE = re.compile(r"(?:^|\s)extern\s*\"\"\s*$")
_CLASS_RE = re.compile(r"\b(?:class|struct|union|enum)\b")
_TRY_RE = re.compile(r"(?:^|\s)try\s*$")


def _classify_brace(header, parent):
    """The kind of the block a `{` opens: 'namespace' (file scope), 'class' (class / struct / union / enum body),
    'function' (a function, lambda or function-try body; every block inside a function), 'init' (a brace
    initializer or another data brace outside a function)."""
    if parent == "function":
        return "function"
    h = " ".join(header.split())
    if _NAMESPACE_RE.search(h) or _EXTERN_C_RE.search(h):
        return "namespace"
    if _FUNC_END_RE.search(h) or _LAMBDA_END_RE.search(h) or _TRY_RE.search(h):
        return "function"
    if _CTOR_INIT_RE.search(h) and (h.endswith(")") or h.endswith("}")):
        return "function"
    if re.search(r"=\s*$", h) or h.endswith(","):
        return "init"
    if _CLASS_RE.search(h):
        return "class"
    return "init"


def _scope_name(stack):
    top = stack[-1]
    if top == "function":
        return "function"
    if top in ("class", "init"):
        return "class"
    return "file"


def scope_map(text):
    """The scope at the START of every line (1-based line n -> result[n - 1]): 'file' (namespace or global scope),
    'class' (inside a class / struct / union / enum body or a data initializer outside functions) or 'function'
    (inside a function or lambda body). A brace-stack lexer that ignores comments, string / char literals (raw
    strings included), digit separators and preprocessor lines (with continuations). Line numbering = splitlines()."""
    out = []
    stack = ["namespace"]
    headers = []
    header = []
    in_block = False
    raw_end = None
    pp_cont = False
    for line in text.splitlines():
        out.append(_scope_name(stack))
        if pp_cont:
            pp_cont = line.rstrip().endswith("\\")
            continue
        if not in_block and raw_end is None and line.lstrip().startswith("#"):
            pp_cont = line.rstrip().endswith("\\")
            continue
        i, n = 0, len(line)
        while i < n:
            if in_block:
                j = line.find("*/", i)
                if j < 0:
                    i = n
                    break
                in_block = False
                i = j + 2
                continue
            if raw_end is not None:
                j = line.find(raw_end, i)
                if j < 0:
                    i = n
                    break
                i = j + len(raw_end)
                raw_end = None
                header.append('""')
                continue
            c = line[i]
            if c == "/" and line.startswith("//", i):
                break
            if c == "/" and line.startswith("/*", i):
                in_block = True
                i += 2
                continue
            if c == '"':
                t = i
                while t > 0 and (line[t - 1].isalnum() or line[t - 1] == "_"):
                    t -= 1
                prefix = line[t:i]
                if prefix in ("R", "u8R", "uR", "UR", "LR"):
                    p = line.find("(", i + 1)
                    if p >= 0:
                        raw_end = ")" + line[i + 1:p] + '"'
                        i = p + 1
                        continue
                j = i + 1
                while j < n and line[j] != '"':
                    j += 2 if line[j] == "\\" else 1
                header.append('""')
                i = j + 1
                continue
            if c == "'":
                t = i
                while t > 0 and (line[t - 1].isalnum() or line[t - 1] in "_."):
                    t -= 1
                if t < i and line[t].isdigit():   # a digit separator (1'000'000)
                    i += 1
                    continue
                j = i + 1
                while j < n and line[j] != "'":
                    j += 2 if line[j] == "\\" else 1
                header.append("''")
                i = j + 1
                continue
            if c == "{":
                kind = _classify_brace("".join(header), stack[-1])
                headers.append(header)
                header = []
                stack.append(kind)
            elif c == "}":
                if len(stack) > 1:
                    kind = stack.pop()
                    header = headers.pop()
                    if kind == "init":
                        header.append("{}")
                    else:
                        header = []
                else:
                    header = []
            elif c == ";":
                header = []
            else:
                header.append(c)
            i += 1
        header.append(" ")
    return out


def _norm_include(name):
    return name.replace("\\", "/")


def resolve_include(name, from_rel, read):
    """(path, text) of the repo file an #include names, tried against the includer's folder and INCLUDE_ROOTS;
    (None, None) when none exists. `read(rel)` returns the text or None."""
    name = _norm_include(name)
    bases = []
    d = posixpath.dirname(from_rel)
    if d:
        bases.append(d)
    bases.extend(INCLUDE_ROOTS)
    for b in bases:
        cand = posixpath.normpath(posixpath.join(b, name))
        if cand.startswith("../"):
            continue
        text = read(cand)
        if text is not None:
            return cand, text
    return None, None


def is_vp_authored(path, bill_tree):
    """VP-authored = under src/VP-V/, or (with a Bill tree to compare) a file Bill's tree does not have."""
    return path.startswith(VPV_PREFIX) or (bill_tree is not None and path not in bill_tree)


def _nonblank(text):
    return sum(1 for ln in text.splitlines() if ln.strip())


def _fragment_lines(path, text, read, bill_tree, seen, errors):
    """Non-blank lines of a VP file pulled into a Bill scope plus, recursively, those of every VP-authored file
    it includes (each file once per expansion: `seen` is the cycle / repeat guard). An include in it that
    resolves to nothing is an error (fail closed)."""
    total = _nonblank(text)
    nested = []
    for i, ln in enumerate(text.splitlines(), 1):
        if not INCLUDE_ANY_RE.match(ln):
            continue
        m = INCLUDE_RE.match(ln)
        if not m:
            errors.append("%s:%d computed or unparsable #include inside a VP file pulled into a class or function scope "
                          "(fail closed): %s" % (path, i, ln.strip()[:100]))
            continue
        p, t = resolve_include(m.group(1), path, read)
        if p is None:
            errors.append("%s:%d unresolved #include <%s> inside a VP file pulled into a class or function scope" % (path, i, m.group(1)))
            continue
        if not is_vp_authored(p, bill_tree) or p in seen:
            continue
        seen.add(p)
        n, sub = _fragment_lines(p, t, read, bill_tree, seen, errors)
        total += n
        nested.append({"fragment": p, "lines": n, "nested": sub})
    return total, nested


def vp_included_lines(includer, text, read, bill_tree, consider=None, scopes=None):
    """The VP-authored files that #include lines of `text` (the Bill file `includer`) pull into a CLASS or
    FUNCTION scope. `consider`: the line numbers to look at (the added lines; None = every line). Returns
    (found, errors): found = [{line, scope, fragment, lines (non-blank, recursive), nested}], errors = unresolved
    includes at class / function scope (fail closed). A file-scope include is never counted (refs/112 S8).
    A computed / unparsable #include at class or function scope is an error too (G-CC1 MUST-1). `scoped` (third
    value) = EVERY considered #include at class or function scope, whatever it resolves to ({line, scope, target}):
    the checker's fragment registry gates them all (G-CC1 MUST-2: a Bill header included there can carry VP lines)."""
    lines = text.splitlines()
    cand = [i for i, ln in enumerate(lines, 1) if (consider is None or i in consider) and INCLUDE_ANY_RE.match(ln)]
    if not cand:
        return [], [], []
    scopes = scopes or scope_map(text)
    found, errors, scoped = [], [], []
    for i in cand:
        if scopes[i - 1] == "file":
            continue
        m = INCLUDE_RE.match(lines[i - 1])
        if not m:
            errors.append("%s:%d computed or unparsable #include at %s scope (fail closed: cannot tell whose lines it pulls "
                          "in): %s" % (includer, i, scopes[i - 1], lines[i - 1].strip()[:100]))
            continue
        name = m.group(1)
        p, t = resolve_include(name, includer, read)
        if p is None:
            errors.append("%s:%d unresolved #include <%s> at %s scope (fail closed: cannot tell whose lines it pulls in)" % (
                includer, i, name, scopes[i - 1]))
            continue
        scoped.append({"line": i, "scope": scopes[i - 1], "target": p, "vp": is_vp_authored(p, bill_tree)})
        if not is_vp_authored(p, bill_tree):
            continue
        n, nested = _fragment_lines(p, t, read, bill_tree, {p}, errors)
        found.append({"line": i, "scope": scopes[i - 1], "fragment": p, "lines": n, "nested": nested})
    return found, errors, scoped


def h2_row(path, nonblank_added, include_lines, read, bill_tree, text=None):
    """{h1, inc, h2, fragments, errors} for one Bill file. `include_lines` = the added #include line numbers (only
    these can pull VP lines in; Bill's own includes are not added lines). `text` = the head text (read when needed)."""
    h1 = len(nonblank_added)
    found, errors, scoped = [], [], []
    if include_lines:
        if text is None:
            text = read(path)
        if text is not None:
            found, errors, scoped = vp_included_lines(path, text, read, bill_tree, consider=set(include_lines))
        else:
            # G-CC1 SHOULD-5: an includer that cannot be read gives no verdict (fail closed)
            errors = ["%s cannot be read: its %d added #include line(s) give no verdict (fail closed)" % (path, len(include_lines))]
    inc = sum(f["lines"] for f in found)
    return {"h1": h1, "inc": inc, "h2": h1 + inc, "fragments": found, "errors": errors, "scoped": scoped}


def is_compiled_vp_source(path, bill_tree):
    """A VP-authored production translation unit (.cpp / .cc / .cxx outside test folders)."""
    return path.lower().endswith((".cpp", ".cc", ".cxx")) and is_vp_authored(path, bill_tree) and bucket_of(path) == "production"


COND_OPEN_RE = re.compile(r'^\s*#\s*(?:if|ifdef|ifndef)\b')
COND_END_RE = re.compile(r'^\s*#\s*endif\b')
GUARD_IFNDEF_RE = re.compile(r'^\s*#\s*ifndef\s+(\w+)\s*$')
DEFINE_NAME_RE = re.compile(r'^\s*#\s*define\s+(\w+)')
OBJECT_MACRO_RE = re.compile(r'^\s*#\s*define\s+([A-Za-z_]\w*)(?![\w(])(?:\s+(.*?))?\s*$')
INCLUDE_LINE_RE = re.compile(r'^\s*#\s*include\b')
HEADER_EXT = (".h", ".hpp", ".hh", ".hxx", ".inl")
# G-CC4 MUST-B 2: macros the compiler (MSVC x64 Release, C++) or the build always predefines can never be a header's
# own include guard: `#ifndef _WIN32` opens a dead block, not a guard.
PREDEFINED_MACROS = frozenset((
    "_WIN32", "_WIN64", "_MSC_VER", "_MSC_FULL_VER", "_MSC_BUILD", "_MSC_EXTENSIONS", "_MSVC_LANG", "__cplusplus",
    "_M_X64", "_M_AMD64", "_M_IX86", "_M_ARM64", "_M_ARM", "_CPPRTTI", "_CPPUNWIND", "_NATIVE_WCHAR_T_DEFINED",
    "_WCHAR_T_DEFINED", "_MT", "_DLL", "_DEBUG", "NDEBUG", "UNICODE", "_UNICODE", "WIN32", "_WINDOWS", "_CONSOLE",
    "_USRDLL", "_LIB", "__FILE__", "__LINE__", "__DATE__", "__TIME__", "__COUNTER__", "__STDC__", "__STDC_HOSTED__",
    "_INTEGRAL_MAX_BITS", "_ISO_VOLATILE", "_KERNEL_MODE", "__clang__", "__GNUC__", "_CHAR_UNSIGNED", "__AVX__",
    "__AVX2__", "__SSE2__", "_M_FP_PRECISE", "_OPENMP", "_CONTROL_FLOW_GUARD", "__SANITIZE_ADDRESS__"))
_RAW_PREFIXES = ("R", "u8R", "uR", "UR", "LR")


def lex(text):
    """`text` with every comment and every string / char literal (raw strings included) blanked to spaces, line for
    line (G-CC4 MUST-B 1): nothing in dead text can be read as an #include, a class or a member definition. The
    operand of an `#include` directive is kept (the directive's file name, not a literal). Newlines are kept, so line
    numbers do not move; a `//` comment ending in a backslash continues on the next line, as the compiler reads it."""
    text = text or ""
    out = []
    i, n = 0, len(text)
    state = None        # None (code), "line", "block", '"', "'", "raw"
    raw_close = None
    keep_line = False   # inside an #include directive line
    at_line_start = True
    while i < n:
        c = text[i]
        if at_line_start:
            at_line_start = False
            if state is None:
                eol = text.find("\n", i)
                keep_line = bool(INCLUDE_LINE_RE.match(text[i:eol if eol >= 0 else n]))
            else:
                keep_line = False
        if c == "\n":
            prev = text[i - 1] if i > 0 else ""
            if prev == "\r" and i > 1:
                prev = text[i - 2]
            if state == "line" and prev != "\\":
                state = None
            elif state in ('"', "'"):
                state = None              # an unterminated literal ends at the line end (ill-formed anyway)
            out.append("\n")
            i += 1
            at_line_start = True
            continue
        if c == "\r":
            out.append(c)
            i += 1
            continue
        if state in ("line", "block"):
            if state == "block" and text.startswith("*/", i):
                state = None
                out.append("  ")
                i += 2
                continue
            out.append("\t" if c == "\t" else " ")
            i += 1
            continue
        if state == "raw":
            if text.startswith(raw_close, i):
                out.append(" " * len(raw_close))
                i += len(raw_close)
                state = None
                continue
            out.append(" ")
            i += 1
            continue
        if state in ('"', "'"):
            if c == "\\" and i + 1 < n and text[i + 1] not in "\r\n":
                out.append("  ")
                i += 2
                continue
            if c == state:
                state = None
                out.append(c)
            else:
                out.append(" ")
            i += 1
            continue
        if text.startswith("//", i):
            state = "line"
            out.append("  ")
            i += 2
            continue
        if text.startswith("/*", i):
            state = "block"
            out.append("  ")
            i += 2
            continue
        if keep_line:
            out.append(c)
            i += 1
            continue
        if c == '"':
            j = i - 1
            while j >= 0 and (text[j].isalnum() or text[j] == "_"):
                j -= 1
            if text[j + 1:i] in _RAW_PREFIXES:
                k = text.find("(", i + 1)
                delim = text[i + 1:k] if k >= 0 else None
                if delim is not None and len(delim) <= 16 and not re.search(r'[\s()\\"]', delim):
                    raw_close = ")" + delim + '"'
                    state = "raw"
                    out.append('"' + " " * (k - i))
                    i = k + 1
                    continue
            state = '"'
            out.append(c)
            i += 1
            continue
        if c == "'":
            # a digit separator (1'000'000, 0x1'F) is not a char literal
            j = i - 1
            while j >= 0 and (text[j].isalnum() or text[j] in "_'"):
                j -= 1
            if i > 0 and i + 1 < n and text[i + 1].isalnum() and text[j + 1:i][:1].isdigit():
                out.append(c)
                i += 1
                continue
            state = "'"
            out.append(c)
            i += 1
            continue
        out.append(c)
        i += 1
    return "".join(out)


def header_guard_macro(text, path=None):
    """If `path` is a header with a classic `#ifndef GUARD` / `#define GUARD` ... `#endif` guard (not a compiler-predefined
    macro), return (guard_line_index, GUARD); else (None, None)."""
    if path is None or not path.lower().endswith(HEADER_EXT):
        return None, None
    lines = lex(text).splitlines()
    directives = [i for i, ln in enumerate(lines) if ln.lstrip().startswith("#")]
    if len(directives) < 3:
        return None, None
    m1 = GUARD_IFNDEF_RE.match(lines[directives[0]])
    m2 = DEFINE_NAME_RE.match(lines[directives[1]])
    if m1 and m2 and m1.group(1) == m2.group(1) and m1.group(1) not in PREDEFINED_MACROS:
        depth, match = 0, None
        for i in directives:
            if COND_OPEN_RE.match(lines[i]):
                depth += 1
            elif COND_END_RE.match(lines[i]):
                depth -= 1
                if depth == 0:
                    match = i
                    break
        if match == directives[-1]:
            return directives[0], m1.group(1)
    return None, None


def live_includes_with_defines(text, path=None):
    """Pairs `(include_name, frozenset_of_macros_defined_before_it)` for every live #include of `text` (SHOULD-E: allows
    detecting an include killed by pre-defining its header guard macro)."""
    lines = lex(text).splitlines()
    guard, _ = header_guard_macro(text, path)
    out = []
    defined = set()
    depth = 0
    for i, ln in enumerate(lines):
        if COND_OPEN_RE.match(ln):
            if i != guard:
                depth += 1
            continue
        if COND_END_RE.match(ln):
            if depth > 0:
                depth -= 1
            continue
        if depth == 0:
            dm = DEFINE_NAME_RE.match(ln)
            if dm:
                defined.add(dm.group(1))
                continue
            um = re.match(r"^\s*#\s*undef\s+([A-Za-z_]\w*)\b", ln)
            if um:
                defined.discard(um.group(1))
                continue
            m = INCLUDE_RE.match(ln)
            if m:
                out.append((m.group(1), frozenset(defined)))
    return out


def live_includes(text, path=None):
    """The #include names of `text` OUTSIDE every preprocessor conditional (G-CC2 MUST-A: an include under `#if 0`,
    `#ifdef NEVER` or any other condition is not proof that a VP-V translation unit compiles the file), read from the
    lexed text (G-CC4 MUST-B 1: an include in a comment or a raw string is not an include). The one exception is a
    classic include guard (G-CC4 MUST-B 2), and only in a header (`path` a .h / .hpp / .inl; no path = no guard):
    `#ifndef X` / `#define X` as the first two directives, the matching `#endif` as the last directive, and X not a
    macro the compiler predefines. In a .cpp every conditional is dead."""
    return [name for name, _ in live_includes_with_defines(text, path)]


_COND_CMP_RE = re.compile(r"^'([^']*)'\s*(==|!=)\s*'([^']*)'$")
_COND_EXISTS_RE = re.compile(r"^(!?)\s*exists\s*\(\s*'([^']*)'\s*\)$", re.IGNORECASE)


def _ms_local(tag):
    return tag.rsplit("}", 1)[-1] if isinstance(tag, str) else ""


def _ms_subst(value, this_dir, project_name, project_dir):
    """MSBuild property substitution for the few properties these build paths use, for the x64 Release build; None
    when another property, item or metadata reference stays (undecidable here)."""
    v = (value or "").replace("\\", "/")
    v = re.sub(r"\$\(MSBuildThisFileDirectory\)", lambda m: (this_dir + "/") if this_dir else "", v, flags=re.I)
    v = re.sub(r"\$\(MSBuildProjectDirectory\)", lambda m: (project_dir or "."), v, flags=re.I)
    v = re.sub(r"\$\(MSBuildProjectName\)", lambda m: project_name, v, flags=re.I)
    v = re.sub(r"\$\(Configuration\)", "Release", v, flags=re.I)
    v = re.sub(r"\$\(Platform\)", "x64", v, flags=re.I)
    if "$(" in v or "%(" in v or "@(" in v:
        return None
    return v


def eval_condition(cond, this_dir="", project_name="", project_dir="", exists=None):
    """An MSBuild Condition for the x64 Release build (G-CC4 MUST-B 3): True / False, or None when it cannot be decided
    here (the caller fails closed). Decided forms: empty, true / false, `'<lhs>' == / != '<rhs>'` over
    $(Configuration) / $(Platform) and the path properties, [!]Exists('path') with `exists(path)`, and `A and B` /
    `A or B` of those."""
    c = (cond or "").strip()
    if not c:
        return True
    low = c.lower()
    if low == "true":
        return True
    if low == "false":
        return False
    for op in (r"\s+or\s+", r"\s+and\s+"):
        parts = re.split(op, c, flags=re.I)
        if len(parts) > 1:
            vals = [eval_condition(x, this_dir, project_name, project_dir, exists) for x in parts]
            if "or" in op:
                return True if True in vals else (None if None in vals else False)
            return False if False in vals else (None if None in vals else True)
    m = _COND_CMP_RE.match(c)
    if m:
        lhs = _ms_subst(m.group(1), this_dir, project_name, project_dir)
        rhs = _ms_subst(m.group(3), this_dir, project_name, project_dir)
        if lhs is None or rhs is None:
            return None
        eq = lhs.strip().lower() == rhs.strip().lower()
        return eq if m.group(2) == "==" else not eq
    m = _COND_EXISTS_RE.match(c)
    if m and exists is not None:
        path = _ms_subst(m.group(2), this_dir, project_name, project_dir)
        if path is None:
            return None
        hit = bool(exists(posixpath.normpath(path)))
        return (not hit) if m.group(1) else hit
    return None


def _parse_build_xml(text):
    import xml.etree.ElementTree as ET
    try:
        return ET.fromstring(text)
    except ET.ParseError:
        return None


_SLN_PROJ_RE = re.compile(r'^Project\("[^"]+"\)\s*=\s*"[^"]+"\s*,\s*"([^"]+\.vcxproj)"\s*,\s*"(\{[^}]+\})"', re.I)
_SLN_BUILD_X64_RE = re.compile(r'^\s*(\{[^}]+\})\.Release\|x64\.Build\.0\s*=', re.I)


def _sln_release_x64_projects(sln_text, sln_dir):
    """Projects in `VideoProcessor.sln` built under Release|x64 (SHOULD-E)."""
    guid_to_path = {}
    built = set()
    for ln in sln_text.splitlines():
        m = _SLN_PROJ_RE.match(ln.strip())
        if m:
            p = m.group(1).replace("\\", "/")
            if sln_dir:
                p = posixpath.join(sln_dir, p)
            guid_to_path[m.group(2).upper()] = posixpath.normpath(p)
            continue
        mb = _SLN_BUILD_X64_RE.match(ln)
        if mb:
            built.add(mb.group(1).upper())
    if not built:
        return set(guid_to_path.values())
    return {p for g, p in guid_to_path.items() if g in built}


def build_preprocessor_defines(build_texts):
    """Macro names defined in `<PreprocessorDefinitions>` of x64 Release build files (SHOULD-E)."""
    defs = set()
    for bf, text in (build_texts or {}).items():
        if not bf.lower().endswith((".vcxproj", ".props", ".targets")):
            continue
        root = _parse_build_xml(text or "")
        if root is None:
            continue
        this_dir = posixpath.dirname(bf)
        proj_name = posixpath.basename(bf).rsplit(".", 1)[0]
        for el in root.iter():
            if _ms_local(el.tag) != "PreprocessorDefinitions":
                continue
            if eval_condition(el.get("Condition"), this_dir, proj_name, this_dir, lambda p: p in build_texts) is False:
                continue
            for tok in (el.text or "").split(";"):
                tok = tok.strip()
                if not tok or tok.startswith("%(") or tok.startswith("$("):
                    continue
                name = tok.split("=", 1)[0].strip()
                if re.match(r"^[A-Za-z_]\w*$", name):
                    defs.add(name)
    return defs


def build_listed_sources(build_texts, files):
    """The repo files of `files` that a real build compiles as `<ClCompile Include=...>` (G-CC2 MUST-A: a .cpp no project
    compiles proves nothing). `build_texts` = {path: text} of the tree's .vcxproj / .props / .targets / .sln files.
    G-CC4 MUST-B 3 & SHOULD-E: only items a .vcxproj compiles count: items of the .vcxproj itself or of a file it imports
    (`<Import Project=...>`, resolved, recursively), in the x64 Release build. When VideoProcessor.sln is present, only
    .vcxproj projects built in Release|x64 are scanned. Honors ExcludedFromBuild child AND attribute, `<ClCompile Remove=...>`,
    and `<ItemDefinitionGroup><ClCompile><ExcludedFromBuild>true`."""
    files = set(files)
    known = dict(build_texts or {})
    out = set()

    def exists(path):
        return path in known or path in files

    def item_targets(item, this_dir, project_dir):
        v = item.replace("\\", "/").strip()
        if v.lower().startswith("$(msbuildthisfiledirectory)"):
            s = _ms_subst(v, this_dir, "", project_dir)
            return [posixpath.normpath(s)] if s is not None else []
        if "$(" in v or "*" in v or "?" in v or "%(" in v or "@(" in v:
            return []
        cands = [posixpath.normpath(posixpath.join(d, v)) if d else posixpath.normpath(v) for d in (project_dir, this_dir)]
        rest = v
        while rest.startswith("../") or rest.startswith("./"):
            rest = rest[3:] if rest.startswith("../") else rest[2:]
        rest = rest.lower()
        hits = [f for f in files if f.lower() == rest or f.lower().endswith("/" + rest)]
        if len(hits) == 1:
            cands.append(hits[0])
        return cands

    sln_projects = None
    for p, txt in known.items():
        if posixpath.basename(p).lower() == "videoprocessor.sln":
            sln_projects = {x.lower() for x in _sln_release_x64_projects(txt or "", posixpath.dirname(p))}
            break

    for proj in sorted(p for p in known if p.lower().endswith(".vcxproj")):
        if sln_projects is not None and proj.lower() not in sln_projects:
            continue
        project_dir = posixpath.dirname(proj)
        project_name = posixpath.basename(proj)[:-len(".vcxproj")]
        seen = set()
        stack = [proj]
        proj_included = set()
        proj_removed = set()
        default_excluded = False
        explicit_unexcluded = set()
        while stack:
            bf = stack.pop()
            if bf in seen:
                continue
            seen.add(bf)
            root = _parse_build_xml(known.get(bf) or "")
            if root is None:
                continue          # unparsable: proves nothing
            this_dir = posixpath.dirname(bf)

            def ev(cond):
                return eval_condition(cond, this_dir, project_name, project_dir, exists)

            for el in root.iter():
                tag = _ms_local(el.tag)
                if tag == "Import":
                    if ev(el.get("Condition")) is not True:
                        continue
                    raw = (el.get("Project") or "").replace("\\", "/")
                    target = _ms_subst(raw, this_dir, project_name, project_dir)
                    if target is None:
                        continue
                    if not raw.lower().startswith(("$(msbuildthisfiledirectory)", "$(msbuildprojectdirectory)")) and this_dir:
                        target = posixpath.join(this_dir, target)
                    target = posixpath.normpath(target)
                    if target in known:
                        stack.append(target)
                elif tag == "ItemDefinitionGroup":
                    if ev(el.get("Condition")) is False:
                        continue
                    for child in el:
                        if _ms_local(child.tag) != "ClCompile":
                            continue
                        if ev(child.get("Condition")) is False:
                            continue
                        for meta in child:
                            if _ms_local(meta.tag) == "ExcludedFromBuild":
                                if (meta.text or "").strip().lower() != "false" and ev(meta.get("Condition")) is not False:
                                    default_excluded = True
                elif tag == "ItemGroup":
                    if ev(el.get("Condition")) is not True:
                        continue
                    for item in el:
                        if _ms_local(item.tag) != "ClCompile":
                            continue
                        if item.get("Remove"):
                            if ev(item.get("Condition")) is not False:
                                for rem in item.get("Remove").split(";"):
                                    for cand in item_targets(rem, this_dir, project_dir):
                                        if cand in files:
                                            proj_removed.add(cand)
                                            break
                            continue
                        if not item.get("Include"):
                            continue
                        if ev(item.get("Condition")) is not True:
                            continue
                        excluded = False
                        explicit_false = False
                        attr_ex = item.get("ExcludedFromBuild")
                        if attr_ex is not None:
                            if attr_ex.strip().lower() == "false":
                                explicit_false = True
                            else:
                                excluded = True
                        for meta in item:
                            if _ms_local(meta.tag) != "ExcludedFromBuild":
                                continue
                            if (meta.text or "").strip().lower() == "false":
                                if ev(meta.get("Condition")) is True:
                                    explicit_false = True
                                continue
                            if ev(meta.get("Condition")) is not False:
                                excluded = True
                        if excluded:
                            continue
                        for inc in item.get("Include").split(";"):
                            for cand in item_targets(inc, this_dir, project_dir):
                                if cand in files:
                                    proj_included.add(cand)
                                    if explicit_false:
                                        explicit_unexcluded.add(cand)
                                    break
        if default_excluded:
            proj_included &= explicit_unexcluded
        out |= (proj_included - proj_removed)
    return out


CLASS_NAME_RE = re.compile(r'^\s*(?:class|struct)\s+(?:__declspec\([^)]*\)\s*)?([A-Za-z_]\w*)\s*(?:final\s*)?(?::|\{|$)')
MEMBER_DEF_RE = re.compile(r'^(?:[\w:<>,*&\s]+?\s+)?(?:[*&]\s*)?((?:[A-Za-z_]\w*::)+)~?[A-Za-z_]\w*\s*\(')


def bill_class_names(read, bill_tree):
    """Class / struct names declared in Bill-owned headers, plus `Impl` (the renderer's private Impl, declared in Bill's
    LibplaceboVideoRenderer.cpp)."""
    names = {"Impl"}
    for p in sorted(bill_tree or ()):
        if not p.startswith("src/") or not p.lower().endswith((".h", ".hpp")):
            continue
        t = read(p)
        if not t:
            continue
        for ln in t.splitlines():
            m = CLASS_NAME_RE.match(ln)
            if m:
                names.add(m.group(1))
    return names


def defines_bill_members(text, bill_classes):
    """True when `text` defines a member of a Bill-owned class out of line (`Ret Impl::f(...)`, `Bill::Impl::f(`): such
    a file is a body moved out of Bill's TU, never a shared interface header (G-CC2 MUST-A 2). Only lines at file
    (namespace / global) scope of the LEXED text are definition heads (scope_map), whatever their indentation.
    G-CC4 MUST-B 4: a class the file defines itself never exempts a Bill class of the same name (no decoy `struct Impl`,
    live, dead, commented or forward-declared): a file defining `Name::` members for a Bill class name is Bill-member
    (fail closed). MUST-B 5: an object-like macro qualifier (`#define X Impl`, then `X::f(`) is expanded; one that does
    not expand to plain identifiers fails closed."""
    text = lex(text)
    bill_classes = set(bill_classes)
    macros = {}
    for ln in text.splitlines():
        m = OBJECT_MACRO_RE.match(ln)
        if m:
            macros[m.group(1)] = (m.group(2) or "").strip()

    def expand(q, depth=0):
        if q not in macros:
            return [q]
        parts = [x for x in re.split(r"\s*::\s*", macros[q]) if x]
        if depth > 8 or not parts or not all(re.match(r"^[A-Za-z_]\w*$", x) for x in parts):
            return None
        res = []
        for x in parts:
            e = expand(x, depth + 1)
            if e is None:
                return None
            res.extend(e)
        return res

    scopes = scope_map(text)
    for n, ln in enumerate(text.splitlines()):
        if n < len(scopes) and scopes[n] != "file":
            continue
        if ln.lstrip().startswith("#") or ln.rstrip().endswith(";"):
            continue   # a directive, a declaration or a call statement: not a definition head
        m = MEMBER_DEF_RE.match(ln)
        if not m:
            continue
        quals = []
        for q in (q for q in m.group(1).split("::") if q):
            e = expand(q)
            if e is None:
                return True
            quals.extend(e)
        if any(q in bill_classes for q in quals):
            return True
    return False


def shared_vp_headers(sources, read, bill_tree, bill_classes=None, preproc_defs=None):
    """Every VP-authored file a VP-authored production .cpp reaches through LIVE #includes (recursively; see
    live_includes): these compile in VP-V's own translation units too (shared interface headers), so the `tu` measure
    does not count them. The caller passes only sources a build file compiles (build_listed_sources). A file that
    defines members of a Bill-owned class (defines_bill_members) is never shared, whatever includes it.
    SHOULD-E: an include whose target header guard macro is already #defined before the include or in
    PreprocessorDefinitions is treated as dead."""
    classes = bill_classes if bill_classes is not None else {"Impl"}
    gdefs = set(preproc_defs or ())
    seen = set()
    visited = set()
    stack = []
    for s in sources:
        t = read(s)
        if t is not None:
            stack.append((s, t))
    while stack:
        q, t = stack.pop()
        for name, prior_defs in live_includes_with_defines(t, q):
            r, u = resolve_include(name, q, read)
            if r is None or r in visited or not is_vp_authored(r, bill_tree):
                continue
            _, gmacro = header_guard_macro(u, r)
            if gmacro and (gmacro in prior_defs or gmacro in gdefs):
                continue
            visited.add(r)
            if not defines_bill_members(u, classes):
                seen.add(r)
            stack.append((r, u))
    return seen


BUILD_DEFINE_RE = re.compile(r'\b([A-Za-z_]\w*)=(?:&quot;|")([^&"<>;]+)(?:&quot;|")')
COMPUTED_INCLUDE_RE = re.compile(r'^\s*#\s*include\s+([A-Za-z_]\w*)\s*(?://.*)?$')


def build_include_macros(texts):
    """{MACRO: {header name}} from the build files' PreprocessorDefinitions (`NAME=&quot;path&quot;` in .props /
    .targets): the computed `#include MACRO` extension points VP-V's builds define. Every value a build gives is kept
    (each is compiled in some configuration)."""
    out = {}
    for t in texts:
        for m in BUILD_DEFINE_RE.finditer(t):
            if m.group(2).lower().endswith((".h", ".hpp", ".inl", ".ipp")):
                out.setdefault(m.group(1), set()).add(m.group(2))
    return out


_DECL_END_RE = re.compile(r";\s*$")
_ASSIGN_RE = re.compile(r"(?<![=!<>+\-*/%&|^])=(?!=)")
_FUNC_HEAD_END_RE = re.compile(r"(?:\)\s*(?:const|noexcept(?:\s*\([^()]*\))?|override|final|volatile|mutable|&&|&|\s)*"
                               r"(?:->\s*[\w:<>,*&\s]+)?|\)\s*:.*|\btry|\belse|\bdo)\s*$")
_CLASS_HEAD_RE = re.compile(r"^(?:template\s*<[^()]*>\s*)?(?:class|struct|union|enum(?:\s+class|\s+struct)?)\b[^()=]*$")
_BARE_CLASS_HEAD_RE = re.compile(r"^(?:class|struct|union)\b[^()=]*$")
_BARE_ENUM_HEAD_RE = re.compile(r"^enum(?:\s+(?:class|struct))?\b[^()=]*$")
_NS_HEAD_RE = re.compile(r"^(?:(?:inline\s+)?namespace(?:\s+[A-Za-z_][\w:]*)?|extern\s*\"[^\"]*\")$")
_STRUCTURE_RE = re.compile(r"^(?:[}]+;?|public\s*:|private\s*:|protected\s*:|(?:inline\s+)?namespace\b[^;()=]*|"
                           r"extern\s*\"[^\"]*\"\s*\{?|template\s*<[^()]*>)$")
_body_cache = {}


def _strip_leading_templates(h):
    """Strip any leading `template<...>` prefix(es) with balanced angle brackets."""
    s = h.strip()
    while True:
        m = re.match(r"^template\s*<", s)
        if not m:
            return s
        depth, i = 0, m.end() - 1
        while i < len(s):
            if s[i] == "<":
                depth += 1
            elif s[i] == ">":
                depth -= 1
                if depth == 0:
                    s = s[i + 1:].strip()
                    break
            i += 1
        else:
            return s


def _brace_kind(header, parent):
    """MUST-C (CC-gate-5): the kind of a `{` from the text since the last `;` / `{` / `}`:
    - `ns` ONLY for `namespace` or `extern "<lit>"` directly followed by `{`
    - `enum` ONLY when head after last `template<...>` matches enum head
    - `class` ONLY when head after last `template<...>` matches `_BARE_CLASS_HEAD_RE` (no `=`)
    - fail closed: every unknown head is `function`."""
    if parent in ("function", "init"):
        return parent
    h = header.strip()
    if _NS_HEAD_RE.match(h):
        return "ns"
    tail = _strip_leading_templates(h)
    if _BARE_ENUM_HEAD_RE.match(tail) and not _ASSIGN_RE.search(tail):
        return "enum"
    if _BARE_CLASS_HEAD_RE.match(tail) and not _ASSIGN_RE.search(tail):
        return "class"
    return "function"


def header_body_lines(text):
    """G-CC4 / V-GEM (2be3cebf4) + MUST-C (CC-gate-5): the lines of a VP-V header that are NOT declaration-only: every
    line inside a function or lambda body or an initializer, a line (including a lone `{`) that opens a function body,
    every line of a function-like `#define` or multi-line `#define` continuation (object-like or function-like), and a
    namespace-scope variable definition (`= ...;` / brace initializer). Declarations (prototypes, fields, forward
    declarations, type aliases, class and namespace heads, access labels, enum bodies, `= default` / `= delete`) are
    exempt. Unknown shapes count (fail closed)."""
    if text in _body_cache:
        return _body_cache[text]
    lines = lex(text).splitlines()
    stack, header, n = [], [], 0
    pp_cont = False
    define_cont = False
    for ln in lines:
        kind = stack[-1] if stack else "ns"
        s = ln.strip()
        was_pp_cont = pp_cont
        was_define_cont = define_cont
        directive = was_pp_cont or s.startswith("#")
        continues = directive and ln.rstrip().endswith("\\")
        pp_cont = continues
        if directive:
            if was_define_cont:
                n += 1   # MUST-C: every continuation line of #define (object-like or function-like) counts
                define_cont = continues
            elif not was_pp_cont and re.match(r"^#\s*define\b", s):
                is_fn_macro = bool(re.match(r"^#\s*define\s+\w+\(", s))
                define_cont = continues
                if is_fn_macro or continues:
                    n += 1   # function-like macro or multi-line #define head counts
            else:
                define_cont = False
                if was_pp_cont and kind in ("function", "init"):
                    n += 1
            continue
        define_cont = False
        if s:
            if kind in ("function", "init"):
                n += 1
            elif kind == "enum":
                pass
            else:
                if s == "{":
                    # MUST-C: a lone `{` counts when it opens a function/init body; structural only for ns/class/enum.
                    decl = _brace_kind("".join(header), kind) in ("ns", "class", "enum")
                else:
                    decl = (_STRUCTURE_RE.match(s) or _CLASS_HEAD_RE.match(s) or s.startswith(("using ", "typedef ", "friend ",
                            "static_assert", "template <", "template<")) and _DECL_END_RE.search(s))
                    if not decl and _DECL_END_RE.search(s) and "{" not in s:
                        # a declaration statement; at namespace scope an initializer makes it a variable definition
                        decl = not (kind == "ns" and _ASSIGN_RE.search(s)) or re.search(r"=\s*(?:default|delete|0)\s*;\s*$", s)
                    if not decl and kind == "class" and _DECL_END_RE.search(s) and "{" not in s and "(" not in s:
                        decl = True   # a field with a default member initializer
                    if not decl and _CLASS_HEAD_RE.match(s.rstrip(";").split("{")[0].strip() or "x") and "(" not in s and \
                            not _ASSIGN_RE.search(s):
                        decl = True   # `struct Api { int a; };` on one line
                if not decl:
                    n += 1
        for c in ln:
            if c == "{":
                stack.append(_brace_kind("".join(header), stack[-1] if stack else "ns"))
                header = []
            elif c == "}":
                if stack:
                    stack.pop()
                header = []
            elif c == ";":
                header = []
            else:
                header.append(c)
        header.append(" ")
    _body_cache[text] = n
    return n


def tu_row(path, include_lines, read, bill_tree, shared, text=None, macros=None):
    """G-CC1 (7) `tu`: the non-blank lines of every VP-authored file (any extension) reached from an ADDED FILE-scope
    #include of the Bill file `path`, recursively, each file once per includer. A shared header (a VP-V production
    .cpp compiles it too) counts only its non-declaration lines (header_body_lines, V-GEM 2be3cebf4: its inline /
    template bodies and variable definitions still compile in Bill's TU), and the walk goes on through it. These lines compile in Bill's translation unit while H2
    does not see them: a body moved from a Bill class into a file-scope VP file is a RELOCATION (h2 -n, tu +n), never a
    reduction. Returns (tu, {vp file: lines}, errors): a computed / unparsable added include at file scope is an error
    (fail closed); an unresolved one (a system or SDK header) is not VP code and is skipped. Inside a VP file, a
    computed `#include MACRO` is followed through `macros` (build_include_macros: the build's own definitions); one
    no build defines is an error."""
    if not include_lines:
        return 0, {}, []
    if text is None:
        text = read(path)
    if text is None:
        return 0, {}, ["%s cannot be read: no tu verdict (fail closed)" % path]
    lines = text.splitlines()
    scopes = scope_map(text)
    seen, errors = {}, []
    macros = macros or {}

    def walk(q, t):
        seen[q] = header_body_lines(t) if q in shared else _nonblank(t)
        for j, ln in enumerate(t.splitlines(), 1):
            if not INCLUDE_ANY_RE.match(ln):
                continue
            m = INCLUDE_RE.match(ln)
            names = [m.group(1)] if m else None
            if not m:
                c = COMPUTED_INCLUDE_RE.match(ln)
                names = sorted(macros.get(c.group(1), ())) if c else []
                if not names:
                    errors.append("%s:%d computed or unparsable #include in a VP file compiled in %s that no build defines "
                                  "(fail closed): %s" % (q, j, path, ln.strip()[:100]))
                    continue
            for name in names:
                r, u = resolve_include(name, q, read)
                if r is not None and r not in seen and is_vp_authored(r, bill_tree):
                    walk(r, u)

    for i in include_lines:
        if i > len(lines) or scopes[i - 1] != "file":
            continue
        m = INCLUDE_RE.match(lines[i - 1])
        if not m:
            errors.append("%s:%d computed or unparsable #include at file scope (fail closed): %s" % (path, i, lines[i - 1].strip()[:100]))
            continue
        q, t = resolve_include(m.group(1), path, read)
        if q is None or q in seen or not is_vp_authored(q, bill_tree):
            continue
        walk(q, t)
    return sum(seen.values()), {f: n for f, n in seen.items() if n}, errors


def file_scope_inl_lines(bill, head, repo=None):
    """INFORMATION number reported in every CC commit (A20-CC order 3), not part of H2: the non-blank lines of every
    VP-authored .inl reached from an ADDED file-scope #include in a Bill-owned src/ file (refs/112 S8 route), following
    nested .inl includes only, each .inl counted once. Returns (total, {includer: lines}, {inl: lines})."""
    bill_files = set(git(["ls-tree", "-r", "--name-only", bill], repo).splitlines())
    diff = parse_diff(git(["diff"] + DIFF_ARGS + ["-U0", bill, head, "--", "src", ":(exclude)src/VP-V"], repo))
    read = GitReader(head, repo)
    seen = {}

    def walk(q, t):
        if q in seen:
            return
        seen[q] = _nonblank(t)
        for ln in t.splitlines():
            m = INCLUDE_RE.match(ln)
            if m and m.group(1).endswith(".inl"):
                r, u = resolve_include(m.group(1), q, read)
                if r is not None and is_vp_authored(r, bill_files):
                    walk(r, u)

    per = {}
    for p, d in sorted(diff.items()):
        if p not in bill_files or not d["include_lines"]:
            continue
        text = read(p)
        if text is None:
            continue
        scopes = scope_map(text)
        lines = text.splitlines()
        for i in d["include_lines"]:
            m = INCLUDE_RE.match(lines[i - 1])
            if not m:
                continue   # a computed include: the H2 / tu measures fail closed on it; this report is information
            name = m.group(1)
            if not name.endswith(".inl") or scopes[i - 1] != "file":
                continue
            q, t = resolve_include(name, p, read)
            if q is None or not is_vp_authored(q, bill_files):
                continue
            before = set(seen)
            walk(q, t)
            per[p] = per.get(p, 0) + sum(n for k, n in seen.items() if k not in before)
    return sum(seen.values()), per, seen


class GitReader(object):
    """read(rel) -> text or None for one revision (cached; existence from one ls-tree)."""

    def __init__(self, rev, repo=None, files=None):
        self.rev, self.repo = rev, repo
        self.files = files if files is not None else set(git(["ls-tree", "-r", "--name-only", rev], repo).splitlines())
        self.cache = {}

    def __call__(self, rel):
        if rel not in self.files:
            return None
        if rel not in self.cache:
            try:
                self.cache[rel] = git(["show", "%s:%s" % (self.rev, rel)], self.repo)
            except RuntimeError:
                self.cache[rel] = None
        return self.cache[rel]


def measure(bill, head, only_files=None, repo=None):
    bill_files = set(git(["ls-tree", "-r", "--name-only", bill], repo).splitlines())
    head_files = set(git(["ls-tree", "-r", "--name-only", head], repo).splitlines())
    bill_prod = {f for f in bill_files if not f.startswith(VPV_PREFIX)}
    diff = parse_diff(git(["diff"] + DIFF_ARGS + ["-U0", bill, head], repo))
    marked = set()
    try:
        marked = {l.split(":", 1)[1] for l in git(["grep", "-l", "VPV-HOOK", head], repo).splitlines() if ":" in l}
    except RuntimeError:
        pass
    rows = []
    read = GitReader(head, repo, head_files)
    errors = []
    for path, d in diff.items():
        if path not in bill_prod or path not in head_files:
            continue
        hook = 0
        if path in marked and d["added_lines"]:
            hs = hook_line_set(read(path) or "")
            hook = sum(1 for n in d["added_lines"] if n in hs)
        h = h2_row(path, d["nonblank_added_lines"], d["include_lines"], read, bill_files)
        errors.extend(h["errors"])
        rows.append({"path": path, "bucket": bucket_of(path), "adds": d["adds"], "dels": d["dels"],
                     "hunks": d["hunks"], "hook": hook, "stray": d["adds"] - hook,
                     "h1": h["h1"], "inc": h["inc"], "h2": h["h2"],
                     "fragments": [{"line": f["line"], "scope": f["scope"], "fragment": f["fragment"], "lines": f["lines"]}
                                   for f in h["fragments"]]})
    # misplaced files
    bill_dirs = {"/".join(f.split("/")[:2]) for f in bill_files if f.startswith("src/") and f.count("/") >= 2}
    misplaced, hookfiles = [], []
    cand = [f for f in sorted(head_files - bill_files)
            if not f.startswith(VPV_PREFIX) and "/".join(f.split("/")[:2]) in bill_dirs]
    counts, hookset = {}, set()
    if cand:
        for ln in git(["grep", "-c", "", head, "--"] + cand, repo).splitlines():
            name, _, n = ln.rpartition(":")
            counts[name.split(":", 1)[1]] = int(n)
        try:
            hookset = {l.split(":", 1)[1] for l in git(["grep", "-l", "-e", "^// VPV-HOOK-FILE", head, "--"] + cand, repo).splitlines()}
        except RuntimeError:
            pass
    for f in cand:
        n = counts.get(f, 0)
        (hookfiles if f in hookset else misplaced).append({"path": f, "lines": n})
    rows.sort(key=lambda r: (-r["h2"], r["path"]))
    if only_files:
        pats = [p.replace("\\", "/") for p in only_files]
        shown = [r for r in rows if any(r["path"] == p or fnmatch.fnmatch(r["path"], p) for p in pats)]
        have = {r["path"] for r in shown}
        for p in pats:
            if p not in have and p in bill_prod:
                shown.append({"path": p, "bucket": bucket_of(p), "adds": 0, "dels": 0, "hunks": 0, "hook": 0, "stray": 0,
                              "h1": 0, "inc": 0, "h2": 0, "fragments": []})
    else:
        shown = rows
    totals = {}
    for b in ("production", "tests", "project"):
        sel = [r for r in rows if r["bucket"] == b]
        totals[b] = {k: sum(r[k] for r in sel) for k in ("adds", "dels", "hunks", "hook", "stray", "h1", "inc", "h2")}
        totals[b]["files"] = len(sel)
    return {"bill": bill, "head": head, "files": shown, "totals": totals, "errors": errors,
            "misplaced": {"count": len(misplaced), "lines": sum(m["lines"] for m in misplaced), "files": misplaced},
            "hook_files": {"count": len(hookfiles), "lines": sum(m["lines"] for m in hookfiles), "files": hookfiles}}


def check_budget(rows, budget):
    """Return (bad, notes). The ceilings compare the honest H2 (`metric` must be "H2": a budget without it holds the
    retired stray ceilings and is refused, re-baseline with --write-budget under a WAIVER commit). `upstream_candidates`
    (path -> {lines, ids, waiver}) annotates H2 lines that are owner-approved updates of Bill's own code/tests (PR
    candidates). They still count (the ceiling holds them, by waiver); the annotation is checked so it cannot go
    stale: the file must still carry at least that many H2 lines and its ceiling must cover them."""
    bad, notes = [], []
    if budget.get("metric") != BUDGET_METRIC:
        return (["budget metric %r is not %r: the stray ceilings are retired (CC-audit-1, rule 19); re-baseline with "
                 "--write-budget under a WAIVER commit" % (budget.get("metric"), BUDGET_METRIC)], notes)
    default = budget.get("default")
    limits = budget.get("files", {})
    by_path = {r["path"]: r for r in rows}
    for r in rows:
        lim = limits.get(r["path"], default)
        if lim is not None and r["h2"] > lim:
            bad.append("%s h2 %d > budget %d" % (r["path"], r["h2"], lim))
    # G-CC1 SHOULD-3: stray (added lines without a hook tag) is a second, non-increasing ceiling, so N tagged lines
    # turned into N untagged ones never pass silently. Required once the budget is on H2.
    stray = budget.get("stray_ceiling")
    if not isinstance(stray, dict):
        bad.append("budget has no stray_ceiling: seed it with --write-budget --migrate WAIVER-<ID> (G-CC1 SHOULD-3)")
    else:
        for r in rows:
            lim = stray.get(r["path"], 0)
            if r.get("stray", 0) > lim:
                bad.append("%s stray %d > stray ceiling %d" % (r["path"], r["stray"], lim))
    for path, up in sorted(budget.get("upstream_candidates", {}).items()):
        n = int(up.get("lines", 0))
        lim = limits.get(path, default)
        h2 = by_path[path]["h2"] if path in by_path else 0
        notes.append("Upstream candidates: %s %d of %d H2 lines (%s, %s)" % (path, n, h2, up.get("ids", "?"),
                                                                              up.get("waiver", "no waiver id")))
        if lim is None or n > lim:
            bad.append("%s upstream candidates %d exceed budget %s" % (path, n, lim))
        if n > h2:
            bad.append("%s upstream candidates %d > h2 %d: lines upstreamed or removed, ratchet the annotation "
                       "and the ceiling down" % (path, n, h2))
    return bad, notes


def write_budget(path, res, migrate_note=None, migrate=None):
    """Rewrite the budget's `files` (h2) and `stray_ceiling` ceilings to the measured values (keeps every other key).
    Refused (returns the reasons) when a ceiling would grow. A migration (a budget missing, not on H2, or without
    stray_ceiling) is refused unless `migrate` names the WAIVER id, which is recorded in `_updates` (G-CC1 SHOULD-2)."""
    try:
        budget = read_json(path)
    except (OSError, ValueError):
        budget = None
    rows = res["files"]
    migrating = budget is None or budget.get("metric") != BUDGET_METRIC or not isinstance(budget.get("stray_ceiling"), dict)
    if migrating and not (migrate and re.match(r"^WAIVER-[A-Za-z0-9_.\-]+$", migrate)):
        return ["the budget needs a migration (missing, not on metric H2, or no stray_ceiling): refused without "
                "--migrate WAIVER-<ID> (G-CC1 SHOULD-2)"]
    if migrating:
        # G-CC2 SHOULD-A: a migration (a budget swapped for a non-H2 one, or deleted) never raises an h2 ceiling the
        # sibling docs/audit/hook-budget.json h2_ceiling already holds
        try:
            hb = read_json(os.path.join(os.path.dirname(path), "hook-budget.json"))
        except (OSError, ValueError):
            hb = None
        ceil = (hb or {}).get("h2_ceiling") if isinstance(hb, dict) else None
        if isinstance(ceil, dict):
            bad = ["%s h2 %d > hook-budget h2_ceiling %d (a migration never raises h2)" % (r["path"], r["h2"], int(ceil[r["path"]]))
                   for r in rows if r["path"] in ceil and r["h2"] > int(ceil[r["path"]])]
            if bad:
                return bad
    if budget is None:
        budget = {"default": 0}
    if budget.get("metric") == BUDGET_METRIC:
        # h2 ceilings never grow, also when the migration only seeds stray_ceiling
        bad = [b for b in check_budget(rows, budget)[0] if " h2 " in b or (" stray " in b and not migrating)]
        if bad:
            return bad
    budget["metric"] = BUDGET_METRIC
    budget["base_ref"] = res["bill"]
    budget["measured_at"] = res["head"]
    budget["files"] = dict(sorted((r["path"], r["h2"]) for r in rows if r["h2"] > 0))
    budget["stray_ceiling"] = dict(sorted((r["path"], r["stray"]) for r in rows if r.get("stray", 0) > 0))
    if migrating:
        budget.setdefault("_updates", []).insert(0, "%s %s: migration at %s%s" % (
            datetime.date.today().isoformat(), migrate, res["head"], (" (" + migrate_note + ")") if migrate_note else ""))
    with open(path, "w", encoding="utf-8", newline="\n") as fh:
        json.dump(budget, fh, indent=1, ensure_ascii=False)
        fh.write("\n")
    return []


def render(res, top):
    out = ["Footprint: bill=%s head=%s (the metric is h2 = h1 + inc; stray / hook are information)" % (res["bill"], res["head"]),
           "%-6s %-6s %-5s %-6s %-5s %-5s %-6s %-5s  %s" % ("h2", "h1", "inc", "stray", "adds", "dels", "hunks", "hook", "file (bucket)")]
    for r in res["files"][:top]:
        out.append("%-6d %-6d %-5d %-6d %-5d %-5d %-6d %-5d  %s (%s)" % (r["h2"], r["h1"], r["inc"], r["stray"], r["adds"], r["dels"],
                                                                       r["hunks"], r["hook"], r["path"], r["bucket"]))
        for f in r.get("fragments", []):
            out.append("       + %s scope include at line %d: %s (%d lines)" % (f["scope"], f["line"], f["fragment"], f["lines"]))
    out.append("Totals per bucket:")
    for b, t in res["totals"].items():
        out.append("  %-10s files=%d h2=%d h1=%d inc=%d adds=%d dels=%d hunks=%d hook=%d stray=%d" % (
            b, t["files"], t["h2"], t["h1"], t["inc"], t["adds"], t["dels"], t["hunks"], t["hook"], t["stray"]))
    for e in res.get("errors", []):
        out.append("UNRESOLVED (fail closed): " + e)
    m, h = res["misplaced"], res["hook_files"]
    out.append("Misplaced VP files in Bill folders: %d files, %d lines" % (m["count"], m["lines"]))
    for f in m["files"][:top]:
        out.append("  %s (%d)" % (f["path"], f["lines"]))
    out.append("Hook files (VPV-HOOK-FILE): %d files, %d lines" % (h["count"], h["lines"]))
    return "\n".join(out)


def selftest():
    diff = ("diff --git a/src/A.cpp b/src/A.cpp\n--- a/src/A.cpp\n+++ b/src/A.cpp\n"
            "@@ -3,0 +4,2 @@ x\n+one\n+two // VPV-HOOK\n@@ -10 +12 @@\n-old\n+new\n"
            "diff --git a/src/B.h b/src/B.h\n--- /dev/null\n+++ b/src/B.h\n@@ -0,0 +1 @@\n+z\n"
            "diff --git a/src/Del.cpp b/src/Del.cpp\n--- a/src/Del.cpp\n+++ /dev/null\n@@ -1,2 +0,0 @@\n-gone1\n-gone2\n")
    p = parse_diff(diff)
    assert p["src/A.cpp"]["adds"] == 3 and p["src/A.cpp"]["dels"] == 1 and p["src/A.cpp"]["hunks"] == 2, p
    assert p["src/A.cpp"]["added_lines"] == [4, 5, 12], p
    assert "/dev/null" not in p and "src/Del.cpp" not in p, p
    hs = hook_line_set("a\n// VPV-HOOK\nb\nc\n// VPV-HOOK-END\nd // VPV-HOOK\ne\n")
    assert hs == {2, 3, 4, 5, 6}, hs
    assert hook_line_set("// VPV-HOOK-FILE\nx\n") == {1, 2}
    assert bucket_of("src/X/Foo.vcxproj") == "project" and bucket_of("src/VideoProcessor-Test/a.cpp") == "tests"
    assert bucket_of("src/VideoProcessor-Lib/Foo.cpp") == "production"
    rows = [{"path": "t.cpp", "stray": 0, "h2": 10}]
    b = {"metric": "H2", "default": 0, "files": {"t.cpp": 10}, "stray_ceiling": {}, "upstream_candidates": {"t.cpp": {"lines": 4}}}
    assert check_budget(rows, b)[0] == [], check_budget(rows, b)
    b["upstream_candidates"]["t.cpp"]["lines"] = 11
    assert len(check_budget(rows, b)[0]) == 2, check_budget(rows, b)
    assert check_budget([{"path": "t.cpp", "stray": 0, "h2": 11}], {"metric": "H2", "default": 0, "files": {"t.cpp": 10},
                                                                    "stray_ceiling": {}})[0]
    assert check_budget(rows, {"default": 0, "files": {"t.cpp": 99}})[0], "a stray-metric budget is refused"
    # G-CC1 SHOULD-3: stray is a second non-increasing ceiling; a budget without it is refused
    sb = {"metric": "H2", "default": 0, "files": {"t.cpp": 10}, "stray_ceiling": {"t.cpp": 3}}
    assert check_budget([{"path": "t.cpp", "stray": 3, "h2": 10}], sb)[0] == []
    assert check_budget([{"path": "t.cpp", "stray": 4, "h2": 10}], sb)[0], "tagged lines turned untagged: stray grows"
    assert check_budget(rows, {"metric": "H2", "default": 0, "files": {"t.cpp": 10}})[0], "no stray_ceiling is refused"
    # G-CC1 SHOULD-2: a migration needs --migrate WAIVER-<ID>, recorded in _updates
    import os as _os
    import tempfile as _tf
    with _tf.TemporaryDirectory() as td:
        bp = _os.path.join(td, "b.json")
        res = {"bill": "a", "head": "h", "files": [{"path": "t.cpp", "stray": 2, "h2": 5}]}
        assert write_budget(bp, res), "a missing budget is a migration: refused without --migrate"
        assert write_budget(bp, res, migrate="not-a-waiver"), "the id must be a WAIVER-<ID>"
        assert write_budget(bp, res, migrate="WAIVER-T-MIG") == []
        wb = read_json(bp)
        assert wb["files"] == {"t.cpp": 5} and wb["stray_ceiling"] == {"t.cpp": 2} and "WAIVER-T-MIG" in wb["_updates"][0], wb
        assert write_budget(bp, {"bill": "a", "head": "h", "files": [{"path": "t.cpp", "stray": 3, "h2": 5}]}), "stray grew"
        assert write_budget(bp, {"bill": "a", "head": "h", "files": [{"path": "t.cpp", "stray": 1, "h2": 4}]}) == []
    with _tf.TemporaryDirectory() as d:
        git(["init", "-q"], d)
        git(["config", "user.email", "t@example.invalid"], d)
        git(["config", "user.name", "t"], d)
        _os.makedirs(_os.path.join(d, "src", "Bill"), exist_ok=True)
        with open(_os.path.join(d, "src", "Bill", "A.cpp"), "w", newline="\n") as fh:
            fh.write("int a = 1;\n")
        git(["add", "."], d)
        git(["commit", "-q", "-m", "bill"], d)
        bill_sha = git(["rev-parse", "HEAD"], d).strip()
        with open(_os.path.join(d, "src", "Bill", "A.cpp"), "w", newline="\n") as fh:
            fh.write("int a = 1;\nint h = 2; // VPV-HOOK\nint s = 3;\n")
        with open(_os.path.join(d, "src", "Bill", "Extra.cpp"), "w", newline="\n") as fh:
            fh.write("int e = 1;\n")
        with open(_os.path.join(d, "src", "Bill", "HookFile.cpp"), "w", newline="\n") as fh:
            fh.write("// VPV-HOOK-FILE\nint hf = 1;\n")
        git(["add", "."], d)
        git(["commit", "-q", "-m", "head"], d)
        m = measure(bill_sha, "HEAD", repo=d)
        assert len(m["files"]) == 1 and m["files"][0]["adds"] == 2 and m["files"][0]["hook"] == 1 and m["files"][0]["stray"] == 1, m
        assert m["misplaced"]["count"] == 1 and m["hook_files"]["count"] == 1, m
    h2_selftest()
    print("selftest OK")


def _reader(files):
    return lambda rel: files.get(rel)


def h2_selftest():
    """The H2 selftests of CC-audit-1 that need no git (T1, T2, T4-T8, T10, T13; the checker runs T3, T9, T11, T12)."""
    frag20 = "".join("\tint m%d;\n" % i for i in range(20))
    files = {"src/VP-V/host/F.h": frag20}
    bill_tree = {"src/A.cpp"}
    read = _reader(files)

    def h2_of(text, added=None):
        lines = text.splitlines()
        nb = [i for i, ln in enumerate(lines, 1) if ln.strip() and (added is None or i in added)]
        inc = [i for i in nb if INCLUDE_ANY_RE.match(lines[i - 1])]
        return h2_row("src/A.cpp", nb, inc, read, bill_tree, text)

    # T1: a 20-line VP fragment inside `struct Impl { };` -> 1 + 20
    t1 = "struct Impl\n{\n#include <VP-V/host/F.h>\n};\n"
    r = h2_of(t1, {3})
    assert (r["h1"], r["inc"], r["h2"]) == (1, 20, 21), r
    assert r["fragments"][0]["scope"] == "class", r
    # T2: the same include after `};` (file scope) -> 1
    r = h2_of("struct Impl\n{\n};\n#include <VP-V/host/F.h>\n", {4})
    assert (r["h2"], r["fragments"]) == (1, []), r
    # function scope is counted too (the guard part of T3 is the checker's)
    r = h2_of("void f()\n{\n\tif (x)\n\t{\n#include <VP-V/host/F.h>\n\t}\n}\n", {5})
    assert r["h2"] == 21 and r["fragments"][0]["scope"] == "function", r
    # T4: a fragment that includes another VP .inl -> recursive count
    files["src/VP-V/host/G.h"] = "// outer\n#include <VP-V/host/F.h>\n\n"
    r = h2_of("struct Impl\n{\n#include <VP-V/host/G.h>\n};\n", {3})
    assert r["inc"] == 2 + 20 and r["h2"] == 23, r
    files["src/VP-V/host/C1.h"] = "#include \"C2.h\"\n"
    files["src/VP-V/host/C2.h"] = "#include \"C1.h\"\nx\n"
    r = h2_of("struct S {\n#include <VP-V/host/C1.h>\n};\n", {2})
    assert r["inc"] == 1 + 2, r   # cycle guard
    # T5 / T6 / T7 on the diff parser: a `// VPV-HOOK note` line and an untagged line count; blank lines do not
    d = parse_diff("diff --git a/src/A.cpp b/src/A.cpp\n--- a/src/A.cpp\n+++ b/src/A.cpp\n@@ -1,0 +2,4 @@\n"
                   "+\tx(); // VPV-HOOK note\n+\ty();\n+\n+   \t\n")["src/A.cpp"]
    assert d["nonblank_added_lines"] == [2, 3] and d["added_lines"] == [2, 3, 4, 5], d
    # a `+++ `-looking content line inside a hunk is an added line, not a file header
    d = parse_diff("diff --git a/src/A.cpp b/src/A.cpp\n--- a/src/A.cpp\n+++ b/src/A.cpp\n@@ -1,0 +1,2 @@\n+++ x;\n+-- y;\n")
    assert list(d) == ["src/A.cpp"] and d["src/A.cpp"]["nonblank_added_lines"] == [1, 2], d
    # T8 (anti-gaming): 10 added lines of a class body moved into a fragment at the same place -> h2 never decreases
    body10 = "".join("\tint v%d = 0;   // VPV-HOOK\n" % i for i in range(10))
    before = "struct Impl\n{\n\tint bill;\n" + body10 + "};\n"
    r0 = h2_of(before, set(range(4, 14)))
    files["src/VP-V/host/Moved.h"] = body10
    after = "struct Impl\n{\n\tint bill;\n#include <VP-V/host/Moved.h>   // VPV-HOOK\n};\n"
    r1 = h2_of(after, {4})
    assert (r0["h2"], r1["h2"]) == (10, 11), (r0, r1)
    # T10: a Bill-baseline include at class scope (not an added line, the pch <vld.h> form) -> 0
    r = h2_of("struct S\n{\n\t#include <vld.h>\n};\n", set())
    assert r["h2"] == 0 and r["errors"] == [], r
    # ... and the same system-header form as an ADDED line at class scope fails closed (G-CC1 SHOULD-4)
    r = h2_of("struct S\n{\n\t#include <vld.h>\n};\n", {3})
    assert r["errors"] and "unresolved #include <vld.h>" in r["errors"][0], r
    # G-CC1 MUST-1: a computed include at class or function scope fails closed (it can name any fragment)
    files["src/VP-V/host/Defs.h"] = "#define VPV_FRAG <VP-V/host/F.h>\n"
    r = h2_of("#include <VP-V/host/Defs.h>\nstruct Impl\n{\n#include VPV_FRAG\n};\n", {1, 4})
    assert r["errors"] and "computed or unparsable" in r["errors"][0] and r["h2"] == 2, r
    r = h2_of("void f()\n{\n  #  include   VPV_FRAG // x\n}\n", {3})
    assert r["errors"] and "function scope" in r["errors"][0], r
    # ... also nested inside a fragment
    files["src/VP-V/host/H.h"] = "#include VPV_FRAG\n"
    r = h2_of("struct Impl\n{\n#include <VP-V/host/H.h>\n};\n", {3})
    assert r["errors"] and "computed or unparsable" in r["errors"][0], r
    # G-CC1 MUST-2: every class / function-scope include is reported in `scoped`, a Bill header too (the checker's
    # registry gates them all)
    r = h2_of("struct Impl\n{\n#include <VP-V/host/F.h>\n};\n", {3})
    assert [(s["target"], s["vp"]) for s in r["scoped"]] == [("src/VP-V/host/F.h", True)], r
    # G-CC1 SHOULD-5: an includer that cannot be read fails closed
    r = h2_row("src/Gone.cpp", [1], [1], read, bill_tree)
    assert r["errors"] and "cannot be read" in r["errors"][0], r
    # an ADDED unresolved include at class scope fails closed
    r = h2_of("struct S\n{\n#include <nowhere.h>\n};\n", {3})
    assert r["errors"] and "fail closed" in r["errors"][0], r
    # a Bill (non-VP) header at class scope is Bill's text, not counted
    files["src/A.h"] = "a\nb\n"
    bill_tree.add("src/A.h")
    r = h2_of("struct S\n{\n#include \"A.h\"\n};\n", {3})
    assert r["h2"] == 1 and r["errors"] == [], r
    assert [(s["target"], s["vp"]) for s in r["scoped"]] == [("src/A.h", False)], r   # MUST-2: still reported
    # T13: a `// VPV-HOOK M2 ...` opener with `// VPV-HOOK-END`: every non-blank added line counts regardless
    t13 = "void f()\n{\n\t// VPV-HOOK M2 P5b-2: note\n\ta();\n\tb();\n\t// VPV-HOOK-END\n}\n"
    r = h2_of(t13, {3, 4, 5, 6})
    assert r["h2"] == 4, r
    # ... while the retired narrow count saw only the -END line (the hole H2 closes; G-CC1 SHOULD-4)
    assert len(hook_line_set(t13) & {3, 4, 5, 6}) == 1, hook_line_set(t13)
    # G-CC1 (7) tu, the T8 analogue: 10 class-body lines moved to a FILE-scope VP header still compile in Bill's
    # TU: h2 falls by 10, tu rises by 10, the combined h2 + tu never decreases
    files["src/VP-V/host/Body.h"] = "".join("void Impl::f%d() {}\n" % i for i in range(10))
    r0 = h2_of(before, set(range(4, 14)))
    t0 = tu_row("src/A.cpp", [], read, bill_tree, set(), before)[0]
    moved = "struct Impl\n{\n\tint bill;\n};\n#include <VP-V/host/Body.h>   // VPV-HOOK\n"
    r1 = h2_of(moved, {5})
    t1 = tu_row("src/A.cpp", [5], read, bill_tree, set(), moved)
    assert (r0["h2"], t0, r1["h2"], t1[0]) == (10, 0, 1, 10) and r1["h2"] + t1[0] >= r0["h2"] + t0, (r0, r1, t1)
    # a shared header (a VP-V .cpp compiles it too) is not counted, nor anything only it reaches
    files["src/VP-V/host/Api.h"] = "struct Api;\n#include <VP-V/host/F.h>\n"
    files["src/VP-V/host/Api.cpp"] = "#include \"Api.h\"\n"
    shared = shared_vp_headers(["src/VP-V/host/Api.cpp"], read, bill_tree)
    assert shared == {"src/VP-V/host/Api.h", "src/VP-V/host/F.h"}, shared
    assert tu_row("src/A.cpp", [1], read, bill_tree, shared, "#include <VP-V/host/Api.h>\n")[0] == 0
    # G-CC2 MUST-A: only LIVE includes make a file shared (an include guard is not a condition) ...
    assert live_includes("#if 0\n#include \"Body.h\"\n#endif\n#include \"Api.h\"\n") == ["Api.h"]
    assert live_includes("#ifdef VPV_NEVER\n#include \"Body.h\"\n#else\n#include \"X.h\"\n#endif\n") == []
    guarded = "#ifndef G_H\n#define G_H\n#include \"Api.h\"\n#if 1\n#include \"Body.h\"\n#endif\n#endif\n"
    assert live_includes(guarded, "src/VP-V/host/G.h") == ["Api.h"]
    # G-CC4 MUST-B 2 (mutations): a guard counts only in a header, only on a macro the compiler does not predefine,
    # and only when its #endif is the last directive
    assert live_includes(guarded, "src/VP-V/host/G.cpp") == [], "no guard in a .cpp"
    assert live_includes(guarded) == [], "no path, no guard"
    assert live_includes("#ifndef _WIN32\n#define _WIN32\n#include \"Body.h\"\n#endif\n", "src/VP-V/host/G.h") == []
    assert live_includes("#ifndef G_H\n#define G_H\n#endif\n#include \"Api.h\"\n#if 0\n#endif\n", "src/VP-V/host/G.h") == \
        ["Api.h"], "an early #endif closes the guard; what follows is still read"
    assert live_includes("#ifndef G_H\n#define G_H\n#endif\n#if 0\n#include \"Body.h\"\n#endif\n", "src/VP-V/host/G.h") == []
    # G-CC4 MUST-B 1 (mutations): an include in a block comment, after a continued // comment or in a raw string is
    # not an include; one after a closed comment and the include operand itself are
    assert live_includes("/*\n#include \"Body.h\"\n*/\n") == []
    assert live_includes("// note \\\n#include \"Body.h\"\n") == []
    assert live_includes("const char* s = R\"(\n#include \"Body.h\"\n)\";\n") == []
    assert live_includes("const char* s = u8R\"x(\n#include \"Body.h\"\n)x\";\n") == []
    assert live_includes("/* a */\n#include \"Api.h\"   // why\n") == ["Api.h"]
    assert live_includes("int a = 1'000; char c = '\"';\n#include \"Api.h\"\n") == ["Api.h"], "digit separator, char"
    lexed = lex("a /* b */ c // d\n\"e\" R\"(f\ng)\" 'h'\n#include <x.h> // y\n")
    assert lexed.splitlines() == ["a         c     ", "\" \" R\"  ", "    ' '", "#include <x.h>     "], lexed
    files["src/VP-V/host/Dummy.cpp"] = "#if 0\n#include \"Body.h\"\n#endif\n"
    assert shared_vp_headers(["src/VP-V/host/Dummy.cpp"], read, bill_tree) == set()
    # ... a file defining members of a Bill-owned class is never shared, even through a live include ...
    files["src/VP-V/host/Live.cpp"] = "#include \"Body.h\"\n#include \"Api.h\"\n"
    assert shared_vp_headers(["src/VP-V/host/Live.cpp"], read, bill_tree) == {"src/VP-V/host/Api.h", "src/VP-V/host/F.h"}
    assert defines_bill_members("void Impl::f0() {}\n", {"Impl"})
    assert defines_bill_members("namespace X {\n\tinline bool AnalysisLumaSource::Full() const\n\t{ return true; }\n}\n",
                                {"Impl", "AnalysisLumaSource"})
    assert not defines_bill_members("class Model\n{\n};\ninline void Model::Run()\n{\n}\n", {"Impl"})   # a VP-V class
    assert not defines_bill_members("void f()\n{\n\tImpl::g(1,\n\t\t2);\n}\n", {"Impl"})   # a call inside a function
    assert not defines_bill_members("// void Impl::f0() {}\n/* void Impl::f1() {} */\n", {"Impl"})   # comments only
    # G-CC4 MUST-B 4 (mutations): no decoy class of the Bill name exempts it, live, dead, commented or forward-declared
    body = "void Impl::f0() {}\n"
    for decoy in ("namespace Decoy {\nstruct Impl {};\n}\n", "#if 0\nstruct Impl {};\n#endif\n", "/*\nstruct Impl {};\n*/\n",
                  "struct Impl\n;\n", "template <class T> struct Impl;\n", "class Model\n{\n};\ninline void Model::Run()\n{\n}\n"):
        assert defines_bill_members(decoy + body, {"Impl", "Model"}), decoy
    assert defines_bill_members("class Model\n{\n};\ninline void Model::Run()\n{\n}\n", {"Impl", "Model"}), \
        "a class named like a Bill class with Name:: members fails closed"
    # G-CC4 MUST-B 5 (mutations): a macro qualifier is expanded, or fails closed
    assert defines_bill_members("#define VPV_I Impl\nvoid VPV_I::h() {}\n", {"Impl"})
    assert defines_bill_members("#define VPV_A VPV_B\n#define VPV_B Impl\nvoid VPV_A::h() {}\n", {"Impl"})
    assert defines_bill_members("#define VPV_Q Bill::Impl\nvoid VPV_Q::h() {}\n", {"Impl"})
    assert defines_bill_members("#define VPV_W(x) x\n#define VPV_E VPV_W(Impl)\nvoid VPV_E::h() {}\n", {"Impl"}), "fails closed"
    assert not defines_bill_members("#define VPV_M Model\nvoid VPV_M::h() {}\n", {"Impl"})
    # ... and only a .cpp that a real build compiles counts as a source (G-CC4 MUST-B 3)
    srcs = ["src/VP-V/host/Live.cpp", "src/VP-V/host/Api.cpp", "src/VP-V/host/Dummy.cpp"]
    items = ('<Project><ItemGroup>'
             '<ClCompile Include="$(MSBuildThisFileDirectory)..\\host\\Live.cpp" />'
             '<ClCompile Include="..\\VP-V\\host\\Api.cpp" />'
             '<ClCompile Include="$(Other)\\Dummy.cpp" />'
             '</ItemGroup></Project>\n')
    proj = ('<Project xmlns="http://schemas.microsoft.com/developer/msbuild/2003">'
            '<Import Project="$(MSBuildThisFileDirectory)..\\VP-V\\build\\$(MSBuildProjectName).items.props" '
            'Condition="Exists(\'$(MSBuildThisFileDirectory)..\\VP-V\\build\\$(MSBuildProjectName).items.props\')" /></Project>\n')
    listed = build_listed_sources({"src/VP-V/build/R.items.props": items, "src/R/R.vcxproj": proj}, srcs)
    assert listed == {"src/VP-V/host/Live.cpp", "src/VP-V/host/Api.cpp"}, listed
    # mutations: a stray .props nothing imports, a false / undecidable condition, ExcludedFromBuild
    assert build_listed_sources({"src/VP-V/build/R.items.props": items}, srcs) == set(), "a stray .props"
    for bad in ('<ItemGroup Condition="false"><ClCompile Include="$(MSBuildThisFileDirectory)..\\host\\Live.cpp" /></ItemGroup>',
                '<ItemGroup Condition="\'$(Configuration)\'==\'Debug\'"><ClCompile Include="$(MSBuildThisFileDirectory)..\\host\\Live.cpp" /></ItemGroup>',
                '<ItemGroup><ClCompile Include="$(MSBuildThisFileDirectory)..\\host\\Live.cpp" Condition="$(VpvNever)" /></ItemGroup>',
                '<ItemGroup><ClCompile Include="$(MSBuildThisFileDirectory)..\\host\\Live.cpp"><ExcludedFromBuild>true'
                '</ExcludedFromBuild></ClCompile></ItemGroup>',
                '<ItemGroup><ClCompile Include="$(MSBuildThisFileDirectory)..\\host\\Live.cpp"><ExcludedFromBuild '
                'Condition="\'$(Platform)\'==\'x64\'">true</ExcludedFromBuild></ClCompile></ItemGroup>'):
        got = build_listed_sources({"src/VP-V/build/R.items.props": "<Project>" + bad + "</Project>", "src/R/R.vcxproj": proj}, srcs)
        assert got == set(), (bad, got)
    ok = ('<Project><ItemGroup Condition="\'$(Configuration)|$(Platform)\'==\'Release|x64\'"><ClCompile Include="$(MSBuildThisFileDirectory)'
          '..\\host\\Live.cpp"><ExcludedFromBuild Condition="\'$(Platform)\'==\'Win32\'">true</ExcludedFromBuild></ClCompile>'
          '</ItemGroup></Project>')
    assert build_listed_sources({"src/VP-V/build/R.items.props": ok, "src/R/R.vcxproj": proj}, srcs) == {"src/VP-V/host/Live.cpp"}
    gated = proj.replace("Condition=\"Exists(", "Condition=\"!Exists(")
    assert build_listed_sources({"src/VP-V/build/R.items.props": items, "src/R/R.vcxproj": gated}, srcs) == set(), "Import off"
    # V-GEM (2be3cebf4): a shared header is exempt only for its declaration-only lines
    decl = ("#pragma once\n#include <vector>\nnamespace Vpv\n{\n\tstruct Api;\n\tstruct Model\n\t{\n\tpublic:\n\t\tint a = 0;\n"
            "\t\tvoid Run();\n\t\tModel() = default;\n\t};\n\tenum class E\n\t{\n\t\tA,\n\t\tB = 2,\n\t};\n"
            "\tusing Alias = int;\n\tvoid Free(int a,\n\t\tint b);\n\tstruct Api { int a; };\n}\n")
    assert header_body_lines(decl) == 1, header_body_lines(decl)   # `void Free(int a,`: a split prototype counts (fail closed)
    bodies = ("namespace Vpv\n{\n\tinline int Twice(int x)\n\t{\n\t\treturn 2 * x;\n\t}\n\tinline void One() {}\n"
              "\tstruct S\n\t{\n\t\tint Get() const { return v; }\n\t\tint v = 0;\n\t};\n\tinline const int kTable[] = {\n\t\t1,\n\t\t2,\n\t};\n"
              "\tinline int g = 5;\n\ttemplate <class T> T Id(T t)\n\t{\n\t\treturn t;\n\t}\n}\n")
    # MUST-C: lone `{` opening a function body counts: Twice 4 (head, {, return, }); One 1; Get 1; kTable 4; g 1; Id 4 = 15
    assert header_body_lines(bodies) == 15, header_body_lines(bodies)
    # MUTATION: a new VP-V .cpp that includes a body-heavy header a Bill TU uses must NOT lower tu
    files["src/VP-V/host/Heavy.h"] = bodies
    before = tu_row("src/A.cpp", [1], read, bill_tree, set(), "#include <VP-V/host/Heavy.h>\n")[0]
    files["src/VP-V/host/HeavyUser.cpp"] = "#include \"Heavy.h\"\n"
    shared_heavy = shared_vp_headers(["src/VP-V/host/HeavyUser.cpp"], read, bill_tree)
    assert "src/VP-V/host/Heavy.h" in shared_heavy
    after = tu_row("src/A.cpp", [1], read, bill_tree, shared_heavy, "#include <VP-V/host/Heavy.h>\n")[0]
    assert (before, after) == (_nonblank(bodies), 15), (before, after)
    # ... and the walk goes on through a shared header: what it includes still compiles in Bill's TU
    files["src/VP-V/host/Outer.h"] = "#include \"Heavy.h\"\nstruct Outer;\n"
    files["src/VP-V/host/OuterUser.cpp"] = "#include \"Outer.h\"\n"
    sh = shared_vp_headers(["src/VP-V/host/OuterUser.cpp"], read, bill_tree)
    assert tu_row("src/A.cpp", [1], read, bill_tree, sh, "#include <VP-V/host/Outer.h>\n")[0] == 15
    # a nested VP include is counted once; a system header is skipped; a computed one fails closed
    files["src/VP-V/host/Outer.inl"] = "a\n#include <VP-V/host/Body.h>\n#include <windows.h>\n"
    t = tu_row("src/A.cpp", [1, 2], read, bill_tree, set(), "#include <VP-V/host/Outer.inl>\n#include <VP-V/host/Body.h>\n")
    assert t[0] == 3 + 10 and t[2] == [], t
    assert tu_row("src/A.cpp", [1], read, bill_tree, set(), "#include VPV_FRAG\n")[2], "computed file-scope include"
    # a computed include INSIDE a VP file follows the build's own definition; one no build defines fails closed
    files["src/VP-V/host/Ext.h"] = "#ifdef EXT_HEADER\n#include EXT_HEADER\n#endif\n"
    mac = build_include_macros(['<PreprocessorDefinitions>EXT_HEADER=&quot;VP-V/host/Body.h&quot;;X=1</PreprocessorDefinitions>'])
    assert mac == {"EXT_HEADER": {"VP-V/host/Body.h"}}, mac
    t = tu_row("src/A.cpp", [1], read, bill_tree, set(), "#include <VP-V/host/Ext.h>\n", mac)
    assert t[0] == 3 + 10 and t[2] == [], t
    assert tu_row("src/A.cpp", [1], read, bill_tree, set(), "#include <VP-V/host/Ext.h>\n")[2], "undefined computed include"
    # the lexer: comments, strings, raw strings, char literals and digit separators never move a brace
    tricky = ('namespace n {\nconst char* s = "{";\nconst char* r = R"x(}{)x";\nchar c = \'{\';\nint big = 1\'000;\n'
              '/* { */ // {\nstruct T {\n#define M() {\n#include <VP-V/host/F.h>\n};\n}\n#include <VP-V/host/F.h>\n')
    sc = scope_map(tricky)
    assert sc[8] == "class" and sc[11] == "file", sc
    assert scope_map("extern \"C\" {\nint x;\n}\n")[1] == "file"
    assert scope_map("Foo::Foo() : a(1), b{2}\n{\n\tx();\n}\n")[2] == "function"
    assert scope_map("auto l = [](int v)\n{\n\treturn v;\n};\n")[2] == "function"
    assert scope_map("static const int t[] = {\n1,\n};\n")[1] == "class"
    assert scope_map("enum class E : int\n{\nA,\n};\n")[2] == "class"


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--bill", default="a591ad3b")
    ap.add_argument("--head", default="HEAD")
    ap.add_argument("--files", nargs="*")
    ap.add_argument("--json", action="store_true")
    ap.add_argument("--append")
    ap.add_argument("--budget")
    ap.add_argument("--write-budget", metavar="JSON",
                    help="rewrite the budget's ceilings to the measured h2 (refused if one would grow; the one-time stray -> H2 "
                         "migration needs a WAIVER commit)")
    ap.add_argument("--inl", action="store_true", help="print the file-scope .inl lines compiled in Bill TUs (information) and exit")
    ap.add_argument("--note", help="with --write-budget: extra text for the _updates entry recorded on a migration")
    ap.add_argument("--migrate", metavar="WAIVER-ID", help="with --write-budget: allow a migration (budget missing, not on H2, or "
                                                          "without stray_ceiling); the id is recorded in _updates (G-CC1 SHOULD-2)")
    ap.add_argument("--top", type=int, default=25)
    ap.add_argument("--selftest", action="store_true")
    ap.add_argument("--repo")
    a = ap.parse_args()
    if a.selftest:
        selftest()
        return 0
    if a.inl:
        total, per, files = file_scope_inl_lines(a.bill, a.head, a.repo)
        for p, n in sorted(per.items()):
            print("  %s %d" % (p, n))
        print("file-scope .inl lines compiled in Bill TU at %s: %d (%d .inl files; information, not part of H2)" % (
            a.head, total, len(files)))
        return 0
    res = measure(a.bill, a.head, a.files, a.repo)
    if a.write_budget:
        if a.files:
            print("--write-budget needs every file (no --files)", file=sys.stderr)
            return 2
        if res["errors"]:
            print("refusing to write a budget: unresolved includes:\n" + "\n".join(res["errors"]), file=sys.stderr)
            return 2
        bad = write_budget(a.write_budget, res, a.note, a.migrate)
        if bad:
            print("refusing to write a budget that grew:\n" + "\n".join(bad), file=sys.stderr)
            return 1
        print("wrote %s (h2 ceilings at %s)" % (a.write_budget, a.head))
        return 0
    if a.json:
        print(json.dumps(res, indent=1))
    else:
        print(render(res, a.top))
    if a.append:
        sha = git(["rev-parse", "--short", a.head], a.repo).strip()
        new = True
        try:
            new = open(a.append).read() == ""
        except OSError:
            pass
        with open(a.append, "a", newline="") as fh:
            w = csv.writer(fh)
            if new:
                w.writerow(["date", "bill", "head", "prod_adds", "prod_hook", "prod_stray", "prod_hunks",
                            "tests_adds", "tests_stray", "project_adds", "project_hook", "project_stray",
                            "misplaced_files", "misplaced_lines", "hookfile_lines"])
            t = res["totals"]
            w.writerow([datetime.date.today().isoformat(), a.bill, sha, t["production"]["adds"], t["production"]["hook"],
                        t["production"]["stray"], t["production"]["hunks"], t["tests"]["adds"], t["tests"]["stray"],
                        t["project"]["adds"], t["project"]["hook"], t["project"]["stray"],
                        res["misplaced"]["count"], res["misplaced"]["lines"], res["hook_files"]["lines"]])
    if res["errors"]:
        print("UNRESOLVED include at class or function scope (fail closed, no verdict):\n" + "\n".join(res["errors"]), file=sys.stderr)
        return 2
    if a.budget:
        budget = read_json(a.budget)
        bad, notes = check_budget(res["files"], budget)
        for n in notes:
            print(n)
        if bad:
            print("BUDGET EXCEEDED:\n" + "\n".join(bad), file=sys.stderr)
            return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
