# Copyright(C) 2026 VP-V contributors
# SPDX-License-Identifier: GPL-3.0-only
"""Hook-gating checker (owner directive 2026-11-11: a VP-V call site in a Bill file does NOTHING in the Original pathway).

Usage: python -I tools/check_hook_gating.py [--base-ref a591ad3b5] [--root PATH] [--rev TIP] [--files F...] [--json]
                                            [--fail-untagged] [--no-git]

Bill-owned files = the files present at --base-ref (Bill's tip, the vp_footprint.py convention: every file
outside src/VP-V/) that are production code (vp_footprint.bucket_of == "production"; tests and project files
are counted but never fail: they have no Original pathway). Hook lines = vp_footprint.hook_line_set (a line
ending with `// VPV-HOOK`, a `// VPV-HOOK` .. `// VPV-HOOK-END` block, or a `// VPV-HOOK-FILE`; hook FILES are
VP-V files in Bill folders and are checked as callees, not as call sites).

Every hook STATEMENT inside a function body of a Bill-owned production file must be one of:
  guarded   it sits inside the canonical Original guard: an enclosing `if`/`while` whose condition is a
            conjunction containing `!<x>.OriginalPathway()`, `!<x>.pipelineOriginalPathway`,
            `!originalPathwayOn()`, `!originalPathway` (src/VP-V/core/OriginalPathway.h; the renderer's
            PipelineSlots::Registry::OriginalPathway(), LibplaceboVideoRenderer.cpp line ~23250 is the most
            common tagged form), or `<x>.Runs(PipelineSlots::Id::<slot>)` for a slot of Origin::Vp (read from
            src/VP-V/core/PipelineSlots.h: ApplyOriginalPathway disables every Vp-origin slot), or the `else`
            of a positive check; or the enclosing function returns early on the positive predicate
            (`if (<orig>) return ...;` before the statement, at the function's top level).
  gated     it calls a function whose definition carries `// VPV-GATED` on its signature line (or the line
            above) and whose body the checker verified: the first statements contain
            `if (<positive predicate, || - joined>) return ...;` with the predicate from the list above.
            A marker without that early return is a hit ("gated-marker-unverified").
  allowed   its callee is in ALLOW_CALLEES or the site is in ALLOW_SITES (reason required; reported).
  inert     a declaration without a call, a guard line itself, a brace / comment / preprocessor line.
Anything else is a hit ("unguarded"): file:line, exit 1.

Untagged VP insertions: a line added since --base-ref (git diff -w -U0, the working tree) that is not a hook
line and calls a VP-V namespace (Vp*::, Vpv::, RendererExtension::, ...). Reported as "untagged" (counted in the
summary line); they fail only with --fail-untagged (the footprint ratchet governs stray lines).

Summary line: `hook gating: <hooks> hook statements, <unguarded> unguarded, <unverified> unverified markers,
<allowed> allowed, <untagged> untagged; exit <n>` (machine-readable like the other cheap checks).
"""
import argparse
import importlib.util
import json
import os
import re
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
_spec = importlib.util.spec_from_file_location("vp_footprint", os.path.join(HERE, "vp_footprint.py"))
vp_footprint = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(vp_footprint)

VPV_PREFIX = "src/VP-V/"
# Files whose hooks are pathway-INDEPENDENT plumbing: the settings schema and Config pages (the [vpv] keys incl.
# pipeline_original_pathway itself are validated and shown in BOTH pathways: doc 91 H, owner 2026-11-10 - Bill's
# pages untouched in both, VP-V settings on VP-V cards), the GUI shell, the log observer / crash ring. The Original
# pathway is a renderer-pipeline property (OriginalPathway.h: "in renderer settings"), so these files' hooks are
# reported as class "non-pathway" (listed in the sweep), never failed. Everything else (the renderer, the capture /
# analysis / timing code of VideoProcessor-Lib) is in the failing scope.
NON_PATHWAY_PREFIXES = ("src/VideoProcessor-Config/", "src/VideoProcessor-GUI/", "src/VideoProcessor-ConfigTests/")
NON_PATHWAY_FILES = {"src/VideoProcessor-Lib/ConfigSchema.h", "src/VideoProcessor-Lib/MainConfigSchema.h",
                     "src/VideoProcessor-Lib/RendererProfileConfig.h", "src/VideoProcessor-Lib/DebugLog.h",
                     "src/VideoProcessor-Lib/UnifiedProfileRuntime.cpp"}
CODE_EXT = (".h", ".hpp", ".cpp", ".cc", ".cxx", ".inl", ".c")
SLOTS_HEADER = "src/VP-V/core/PipelineSlots.h"
DEFAULT_BASE = "a591ad3b5"

# The predicate forms (positive). Negated forms are `!` + one of these.
PRED_RE = re.compile(r"(?:[\w:]+(?:\.|->))*(?:OriginalPathway\(\)|pipelineOriginalPathway\b|originalPathwayOn\(\)|originalPathway\b)")
RUNS_RE = re.compile(r"(?:[\w:]+(?:\.|->))?Runs\(\s*PipelineSlots::Id::(\w+)\s*\)")
SLOT_LINE_RE = re.compile(r"\{\s*Id::(\w+)\s*,.*Origin::(\w+)\s*\}")
VP_CALL_RE = re.compile(r"\b(?:Vp[A-Z]\w*|Vpv|RendererExtension|RendererSettingsHooks|MotionAhead|RenderAhead|"
                        r"PipelineSlots|OriginalPathway|ProfileSchemaHooks|SchemaHooks|ConfigPageHost|DialogExtension|"
                        r"CaptureDeliveryHook|CrashState)::")
CONTROL_RE = re.compile(r"^\s*(if|else|for|while|switch|catch|return|case|do|try)\b")
FUNC_HEADER_RE = re.compile(r"\)\s*(?:const|noexcept|override|final|volatile|&&?|throw\s*\(\s*\)|->\s*[\w:<>\s*&]+)*\s*$")
LAMBDA_HEADER_RE = re.compile(r"\]\s*$|\]\s*\([^()]*\)\s*(?:mutable\s*)?(?:->\s*[\w:<>\s*&]+)?\s*$|\]\s*\([^()]*\)\s*noexcept\s*$")
SCOPE_RE = re.compile(r"^\s*(class|struct|union|enum|namespace|extern)\b")
CALL_RE = re.compile(r"\b([A-Za-z_]\w*)\s*\(")
DECL_RE = re.compile(r"^\s*(?:static\s+|const\s+|constexpr\s+|mutable\s+)*[\w:<>,\s*&]+?[\s*&]+[A-Za-z_]\w*(?:\s*\[[^\]]*\])?(?:\s*=\s*[^(;]+)?\s*;?\s*$")
INERT_RE = re.compile(r"^[\s{}();,]*$|^\s*(else|break|continue|default\s*:|case\s+[^:]+:)\s*;?\s*$")

# Allow-list by CALLEE (qualified name as it appears at the call site; a regex). Each entry: the reason why
# every call site is inert in the Original pathway BY CONSTRUCTION, and the proof. Reported in the output.
ALLOW_CALLEES = [
    {"callee": r"Vpv::Capture::(IsRaw|DvTunnelRawOf|DvTunnelHighAlignedOf|RawEncodingOf|RawFrameOf|HasEarlyUpload|EarlyUploadDue)",
     "reason": "payload readers (input\\capture CapturePayload.h): a raw / DV-tunnel / early-upload payload exists only when a "
               "VP capture route produced it; the Original table forces capture_path standard, capture_upload render, "
               "native_rgb_ingest off, dv_tunnel_gpu_unpack off (OriginalPathway.h rows 1-4), so on Bill's payload every "
               "reader returns false / none and Bill's branch runs. Proof: CaptureExtensionTests + OriginalPathwayByteIdenticalToBill."},
    {"callee": r"RendererExtension::(NotifyLifecycle|NotifyQueue|NotifyFrame|NotifyEntrance|NotifyAfterRender|DecideIngress|Ingress|"
               r"OnConvertedFrame|AfterPublish|PlanOutputs|ResolveFor|ResolveQueueTarget|CommandFor|WithStatusLines|CollectStatusLines)",
     "reason": "the extension bus (vprenderer\\RendererExtension.h, a VPV-HOOK-FILE): pure dispatch to the registered extensions; "
               "each extension's handler reads its own VP-V setting, which the Original table forces off (OriginalPathway.h), or "
               "carries Bill's relocated code in its upstream form (P10-5/6 ingress, P10 capture routes: UpstreamForm()). "
               "Proof: OriginalPathwayByteIdenticalToBill, IngressPolicyTests, SettingsDefaultsGoldenTests (Original case)."},
    {"callee": r"RendererSettingsHooks::(Fingerprint|ReadProfile|ReadBase|ApplyGlobal|CopyLive|AppendDeltaText)",
     "reason": "the settings overlay that IMPLEMENTS the Original pathway (control\\config VpvRendererSettings.cpp: reads "
               "pipeline_original_pathway and applies OriginalPathway::Overrides()); it must run in Original. Proof: "
               "SettingsDefaultsGoldenTests (Original case), RendererSettingsIdentityTests."},
    {"callee": r"(?:m_impl->)?(IngressHardwareArrival|IngressCountersBefore|IngressAfterRate|QueueTargetVp|ResolveMotionEngine|NoteTimelinePlanned|"
               r"CurrentQueueHoldFraction|RegisterDejudderCacheFrame|UpdatePipelineSlots|OrderFrameSlotHooks)",
     "reason": "M2-relocated host members (input\\queue_timing IngressPolicy.inl / QueueTarget.inl, motion\\consumers "
               "PhaseTimeline.inl, segments\\CoreSegments.inl): Bill's own pulldown / film clock / queue-target / slot-order "
               "logic moved out of the host (host = call sites only); VP options inside are either/or on settings the Original "
               "table forces off; UpdatePipelineSlots is what applies ApplyOriginalPathway. Proof: IngressPolicyTests, "
               "QueueTimingPolicy, PipelineSkeletonTests (Original pathway), OriginalPathwayByteIdenticalToBill."},
    {"callee": r"Vpv::Ingress::(CallbackGate::Use|Take|ToRecord)|m_ingressGate\.(CloseAndDrain|Open)",
     "reason": "P10-8 callback lifetime gate (queue_timing CallbackGate.h) and the C7b cadence facts hand-over: object lifetime "
               "/ record plumbing with no picture or timing effect; the cadence record is only built while "
               "frame_analysis_store is on (Original row: off). Proof: CallbackGateCoverageTests, FrameAnalysisStoreTests."},
    {"callee": r"Vpv::(MakeRendererAbiBridge|PluginGuard::(Check|Attach)|AttachedForCreate|RegisterConfigProviders)|m_abiBridge\.(reset)|m_abiBridge->Query",
     "reason": "the ABI attach / extension query (abi\\RendererAbiBridge.h, VpvPluginGuard.h): the plumbing through which the "
               "VP-V settings module (and the Original overlay) reach the renderer at all; no pathway can run without it. "
               "Proof: LibplaceboPluginProxyTests, BillIdenticalSchemaTests."},
    {"callee": r"VpStatusText::SetExtensionLines",
     "reason": "status text only (control\\diagnostics): appends the extensions' status lines to the status dialog; writes "
               "nothing the pipeline reads. Proof: StatusExtensionTests (Original = no extension lines)."},
    {"callee": r"PassTimers::Inbox::Motion\(\)\.DrainInto",
     "reason": "pass-timer telemetry drain (control\\diagnostics PassTimers): a pure observer of timing records; writes nothing "
               "Bill reads. Proof: PassTimersTests."},
    {"callee": r"vectorEngine\.PlanProducer",
     "reason": "the optical-flow producer LIFECYCLE (motion\\estimation VectorEngine.cpp PlanProducer -> ProducerPlan::Lifecycle): "
               "in Original (optical_flow off, vector_demand off, motion_ahead_frames 0, no matcher: OriginalPathway.h rows) the plan "
               "can only STOP a producer left over from a live pathway switch, never start one; gating the call would leak that "
               "producer. Proof: ProducerPlanTests (lifecycle table), VectorEngineHookTests."},
    {"callee": r"rebaseWarmup\.Stop",
     "reason": "teardown of the FX-RB warm-up worker (scaling\\rebase RebaseWarmupJob.h): the worker starts only on the VP-V "
               "pathway (its Frame() call carries !pipelineSlots.OriginalPathway()), but a live switch to Original must still "
               "stop a running worker at GPU release; no worker = no effect. Proof: RebaseWarmupGateTests."},
    {"callee": r"captures\.OnFrameRendered",
     "reason": "rendered-output capture requests (control\\diagnostics CaptureRequests): a pure observer of the finished frame "
               "for screenshots / tests; writes nothing the pipeline reads. OriginalPathwayByteIdenticalToBill itself reads the "
               "Original picture through it. Proof: RendererCaptureHarnessTests."},
    {"callee": r"VpDiagnostics::(TestAdapterFromEnvironment|D3D11DebugFromEnvironment)",
     "reason": "TEST / DEBUG ENVIRONMENT switches (control\\diagnostics DiagnosticsAdapter.h): VP_TEST_ADAPTER / VP_D3D11_DEBUG are "
               "unset in production, so the call does nothing; the capture harness decides the adapter for BOTH renders (refs/106, "
               "the P5c lane). Gating them off in Original would put the Original render on a different GPU than the VP-V render "
               "in every same-adapter comparison test. Proof: DiagnosticsAdapterTests."},
]

# Allow-list by SITE: file + a text fragment unique to the statement + reason. Keep it short.
ALLOW_SITES = [
    {"file": "src/VideoProcessor-Lib/vprenderer/LibplaceboVideoRenderer.cpp", "pattern": r"\bbuiltImpl\b",
     "reason": "P10-8: Bill's own `m_impl` construction split in three statements so the callback lifetime gate closes between "
               "the old and the new Impl (the same object, built and installed in the same order). Proof: CallbackGateCoverageTests."},
    {"file": "src/VideoProcessor-Lib/vprenderer/LibplaceboVideoRenderer.cpp", "pattern": r"\bingressUse\b",
     "reason": "P10-8: the callback lifetime gate refuses a capture / file callback only while the Impl is being replaced "
               "(Bill's code would touch a destroyed Impl: a crash fix, DIV register P10-8); never during steady rendering. "
               "Proof: CallbackGateCoverageTests."},
    {"file": "src/VideoProcessor-Lib/vprenderer/LibplaceboVideoRenderer.cpp", "pattern": r"deviceParams\.adapter = testAdapter",
     "reason": "the VP_TEST_ADAPTER result applied to Bill's device parameters: set only when the test environment names an "
               "adapter (unset in production and by the capture harness for both renders); see the VpDiagnostics callee entry."},
]


HOOK_MARK_RE = re.compile(r"//\s*VPV-HOOK(?![-\w])")


def hook_line_set(content):
    """vp_footprint.hook_line_set, widened: a line carrying `// VPV-HOOK` anywhere (with or without a trailing
    note, e.g. `// VPV-HOOK: resync`, `// VPV-HOOK M2 P5b-7`) is a hook line; blocks and hook files as there."""
    out = vp_footprint.hook_line_set(content)
    for i, ln in enumerate(content.splitlines(), 1):
        if HOOK_MARK_RE.search(ln):
            out.add(i)
    return out


def git(args, repo=None):
    cmd = ["git"] + (["-C", repo] if repo else []) + args
    out = subprocess.run(cmd, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    if out.returncode != 0:
        raise RuntimeError("git %s failed: %s" % (" ".join(args), out.stderr.decode("utf-8", "replace")))
    return out.stdout.decode("utf-8", "replace")


class Tree(object):
    """Where the files come from: the working tree under root, or a git revision (--rev) of the repo at root."""

    def __init__(self, root, rev=None):
        self.root, self.rev = root, rev
        self.cache = {}

    def prefetch(self, rels):
        """One `git cat-file --batch` for many files of the revision (a `git show` per file is 100x slower)."""
        if not self.rev:
            return
        want = [r for r in rels if r not in self.cache]
        if not want:
            return
        proc = subprocess.run(["git", "-C", self.root, "cat-file", "--batch"],
                              input="".join("%s:%s\n" % (self.rev, r) for r in want).encode("utf-8"),
                              stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        data = proc.stdout
        pos = 0
        for r in want:
            nl = data.find(b"\n", pos)
            header = data[pos:nl].decode("utf-8", "replace")
            pos = nl + 1
            parts = header.split()
            if len(parts) < 3 or parts[-1] == "missing":
                self.cache[r] = None
                continue
            size = int(parts[2])
            self.cache[r] = data[pos:pos + size].decode("utf-8", "replace")
            pos += size + 1

    def read(self, rel):
        if self.rev:
            if rel not in self.cache:
                self.prefetch([rel])
            text = self.cache.get(rel)
            if text is None:
                raise RuntimeError("%s not in %s" % (rel, self.rev))
            return text
        with open(os.path.join(self.root, *rel.split("/")), encoding="utf-8", errors="replace") as fh:
            return fh.read()

    def exists(self, rel):
        if self.rev:
            try:
                git(["cat-file", "-e", "%s:%s" % (self.rev, rel)], self.root)
                return True
            except RuntimeError:
                return False
        return os.path.isfile(os.path.join(self.root, *rel.split("/")))

    def code_files(self):
        if self.rev:
            return [f for f in git(["ls-tree", "-r", "--name-only", self.rev, "--", "src"], self.root).splitlines()
                    if f.lower().endswith(CODE_EXT) and "/3rdparty/" not in f and "/x64/" not in f]
        return all_code_files(self.root)

    def grep_l(self, needle):
        """Files under src containing `needle` (a cheap pre-filter)."""
        try:
            if self.rev:
                return [l.split(":", 1)[1] for l in git(["grep", "-l", needle, self.rev, "--", "src"], self.root).splitlines() if ":" in l]
            return [l for l in git(["grep", "-l", "--untracked", needle, "--", "src"], self.root).splitlines()]
        except RuntimeError:
            return None


# ---------------------------------------------------------------- source model

def strip_code(text):
    """Comments and string / char literals removed; line structure kept; preprocessor lines blanked."""
    out = []
    i, n = 0, len(text)
    block = False
    line_start = True
    while i < n:
        c = text[i]
        two = text[i:i + 2]
        if block:
            if two == "*/":
                block = False
                i += 2
            else:
                if c == "\n":
                    out.append("\n")
                i += 1
            continue
        if two == "/*":
            block = True
            i += 2
            continue
        if two == "//":
            while i < n and text[i] != "\n":
                i += 1
            continue
        if line_start and c == "#":
            while i < n and text[i] != "\n":
                if text[i] == "\\" and i + 1 < n and text[i + 1] == "\n":
                    out.append("\n")
                    i += 2
                    continue
                i += 1
            continue
        if c in "\"'":
            q = c
            out.append(q)
            i += 1
            while i < n and text[i] != q:
                if text[i] == "\\":
                    i += 1
                if i < n and text[i] == "\n":
                    out.append("\n")
                i += 1
            out.append(q)
            i += 1
            line_start = False
            continue
        out.append(c)
        if c == "\n":
            line_start = True
        elif not c.isspace():
            line_start = False
        i += 1
    return "".join(out)


class Stmt(object):
    __slots__ = ("text", "start", "end", "kind", "parent", "children", "depth", "open_line")

    def __init__(self, text, start, end, kind, parent, depth):
        self.text, self.start, self.end, self.kind, self.parent, self.depth = text, start, end, kind, parent, depth
        self.children = []
        self.open_line = end   # for kind "open": the line of the `{`


def parse_statements(stripped):
    """A tree of statements. kind: 'open' (a `{` header, children = the block), 'stmt' (ends at `;`), 'close'."""
    root = Stmt("", 0, 0, "root", None, 0)
    stack = [root]
    buf = []
    start = None
    line = 1
    paren = 0

    def flush(kind, end_line):
        text = re.sub(r"\s+", " ", "".join(buf)).strip()
        s = Stmt(text, start if start is not None else end_line, end_line, kind, stack[-1], len(stack) - 1)
        stack[-1].children.append(s)
        del buf[:]
        return s

    i, n = 0, len(stripped)
    while i < n:
        c = stripped[i]
        if c == "\n":
            line += 1
            buf.append(" ")
            i += 1
            continue
        if start is None and not c.isspace():
            start = line
        if c == "(":
            paren += 1
        elif c == ")":
            paren = max(0, paren - 1)
        if c == "{":
            s = flush("open", line)
            stack.append(s)
            start = None
            paren = 0
            i += 1
            continue
        if c == "}":
            if buf and "".join(buf).strip():
                flush("stmt", line)
            start = None
            if len(stack) > 1:
                opener = stack.pop()
                opener.end = line
            i += 1
            continue
        if c == ";" and paren == 0:
            buf.append(c)
            flush("stmt", line)
            start = None
            i += 1
            continue
        buf.append(c)
        i += 1
    if buf and "".join(buf).strip():
        flush("stmt", line)
    return root


def is_function_header(text):
    t = text.strip()
    if not t or CONTROL_RE.match(t) or SCOPE_RE.match(t):
        return False
    if LAMBDA_HEADER_RE.search(t):
        return True
    if "(" not in t:
        return False
    if re.search(r"=\s*$", t):
        return False
    return bool(FUNC_HEADER_RE.search(t))


def enclosing_function(stmt):
    s = stmt.parent
    while s is not None and s.kind != "root":
        if s.kind == "open" and is_function_header(s.text):
            return s
        s = s.parent
    return None


def _cond_of(text):
    """The condition of an if / else if / while header, or None."""
    m = re.match(r"^\s*(?:else\s+)?(?:if|while)\s*\((.*)$", text.strip())
    if not m:
        return None
    rest = m.group(1)
    depth = 1
    for k, ch in enumerate(rest):
        if ch == "(":
            depth += 1
        elif ch == ")":
            depth -= 1
            if depth == 0:
                return rest[:k], rest[k + 1:].strip()
    return rest, ""


def _top_split(cond, op):
    parts, depth, cur = [], 0, []
    i = 0
    while i < len(cond):
        ch = cond[i]
        if ch == "(":
            depth += 1
        elif ch == ")":
            depth -= 1
        if depth == 0 and cond.startswith(op, i):
            parts.append("".join(cur))
            cur = []
            i += len(op)
            continue
        cur.append(ch)
        i += 1
    parts.append("".join(cur))
    return [p.strip() for p in parts]


def _unparen(t):
    t = t.strip()
    while t.startswith("(") and t.endswith(")"):
        depth = 0
        ok = True
        for k, ch in enumerate(t):
            if ch == "(":
                depth += 1
            elif ch == ")":
                depth -= 1
                if depth == 0 and k != len(t) - 1:
                    ok = False
                    break
        if not ok:
            break
        t = t[1:-1].strip()
    return t


def negative_guard(cond, vp_slots):
    """True when the condition is false in the Original pathway: a top-level && term is `!<pred>` or Runs(VpSlot)."""
    for term in _top_split(cond, "&&"):
        t = _unparen(term)
        if t.startswith("!") and PRED_RE.fullmatch(_unparen(t[1:])):
            return True
        m = RUNS_RE.fullmatch(t)
        if m and m.group(1) in vp_slots:
            return True
    return False


def positive_guard(cond):
    """True when the condition is true in the Original pathway: a top-level || term is `<pred>`."""
    for term in _top_split(cond, "||"):
        if PRED_RE.fullmatch(_unparen(term)):
            return True
    return False


def early_return_before(func, stmt_start, parse_only=False):
    """An `if (<positive>) return ...;` at the function's top level before line stmt_start."""
    for ch in func.children:
        if ch.start >= stmt_start and not parse_only:
            break
        c = _cond_of(ch.text)
        if not c:
            continue
        cond, rest = c
        body = rest
        if ch.kind == "open":
            body = " ".join(x.text for x in ch.children)
        if positive_guard(cond) and re.match(r"^\s*(?:\{\s*)?return\b", body):
            return True
    return False


def allowed_callee(text):
    for k, e in enumerate(ALLOW_CALLEES):
        if re.search(r"\b(?:" + e["callee"] + r")\s*\(", text):
            return k
    return None


def gated_callee(text, gated):
    for name, (rel, line, verified) in gated.items():
        if re.search(r"\b%s\s*\(" % re.escape(name), text):
            return name, rel, line, verified
    return None


def guarded(stmt, vp_slots, gated=None):
    # 1. enclosing blocks (the function's own body counts up to the function)
    s = stmt
    while s is not None and s.kind != "root":
        if s.kind == "open":
            if is_function_header(s.text):
                break
            c = _cond_of(s.text)
            if c and negative_guard(c[0], vp_slots):
                return "guarded"
            if allowed_callee(s.text) is not None:
                return "allowed-enclosing"
            g = gated_callee(s.text, gated or {})
            if g and g[3]:
                return "gated-enclosing"
            if s.text.strip() == "else" or s.text.strip().startswith("else "):
                # the else of a positive check: the previous sibling header
                sib = s.parent.children
                k = sib.index(s)
                if k > 0:
                    pc = _cond_of(sib[k - 1].text)
                    if pc and positive_guard(pc[0]):
                        return "guarded"
        s = s.parent
    # 2. brace-less if: the statement itself or its previous sibling carries the condition
    c = _cond_of(stmt.text)
    if c and negative_guard(c[0], vp_slots):
        return "guarded"
    sib = stmt.parent.children
    k = sib.index(stmt)
    if k > 0:
        pc = _cond_of(sib[k - 1].text)
        if pc and sib[k - 1].kind == "stmt" and pc[1] == "" and negative_guard(pc[0], vp_slots):
            return "guarded"
        if sib[k - 1].kind == "open":
            pass
    # 3. early return in the enclosing function
    func = enclosing_function(stmt)
    if func is not None and early_return_before(func, stmt.start):
        return "guarded"
    # 4. a short-circuit: `x = !<pred> && call(...)` or `!<pred> && call(...)` (the call never runs in Original)
    expr = stmt.text
    if c and c[1]:
        expr = c[0]
    elif "=" in expr and not expr.lstrip().startswith("if"):
        expr = expr.split("=", 1)[1]
    terms = _top_split(expr.rstrip(";"), "&&")
    if len(terms) > 1:
        first = _unparen(terms[0])
        if first.startswith("!") and PRED_RE.fullmatch(_unparen(first[1:])):
            return "guarded"
    # 5. an inline `if (<allowed callee>(...)) stmt;` runs its body only when the callee says so
    if c and c[1] and allowed_callee(c[0]) is not None:
        return "allowed-enclosing"
    return None


# ---------------------------------------------------------------- gated callees

def gated_markers(tree, files):
    """{name: (file, line, verified)} for every `// VPV-GATED` function definition in `files`."""
    out = {}
    pre = tree.grep_l("VPV-GATED")
    if pre is not None:
        files = [f for f in files if f in set(pre)]
    tree.prefetch(files)
    for rel in files:
        try:
            text = tree.read(rel)
        except (OSError, RuntimeError):
            continue
        if "VPV-GATED" not in text:
            continue
        lines = text.splitlines()
        marked = set()
        for i, ln in enumerate(lines, 1):
            if "// VPV-GATED" in ln:
                code = ln.split("//", 1)[0].strip()
                marked.add(i if code else i + 1)
        parsed = parse_statements(strip_code(text))
        stack = [parsed]
        while stack:
            node = stack.pop()
            for ch in node.children:
                if ch.kind == "open":
                    stack.append(ch)
                    if any(ch.start <= m <= ch.children[0].start if ch.children else ch.start <= m <= ch.end for m in marked) \
                            and is_function_header(ch.text):
                        m = re.search(r"([A-Za-z_][\w:]*)\s*\([^()]*(?:\([^()]*\)[^()]*)*\)\s*(?:const|noexcept|override|final|volatile|&|&&)?\s*$", ch.text)
                        if not m:
                            continue
                        name = m.group(1).split("::")[-1]
                        verified = early_return_before(ch, 10 ** 9, parse_only=True)
                        out[name] = (rel, ch.start, verified)
    return out


# ---------------------------------------------------------------- the check

def vp_slots_from(tree):
    slots = set()
    try:
        for ln in tree.read(SLOTS_HEADER).splitlines():
            m = SLOT_LINE_RE.search(ln)
            if m and m.group(2) == "Vp":
                slots.add(m.group(1))
    except (OSError, RuntimeError):
        pass
    return slots


DECL_NAME_RE = re.compile(r"^(?:const\s+|static\s+|constexpr\s+)*[\w:<>,\s*&]+?[\s*&]+([A-Za-z_]\w*)\s*(?:[({=;]|\[|$)")
CAST_RE = re.compile(r"\b(?:static_cast|reinterpret_cast|const_cast|dynamic_cast)<[^>]*>\s*\(|\bstd::move\s*\(")


def vp_locals(func, vp_lines):
    """Names declared by statements of `func` that sit on VP lines (hook or added lines), and the subset handed to an
    allowed callee as an argument (their fills are that call's inputs)."""
    names = set()
    inputs = set()
    stack = [func]
    while stack:
        node = stack.pop()
        for ch in node.children:
            if ch.kind == "open":
                stack.append(ch)
            if ch.start in vp_lines or ch.end in vp_lines:
                for e in ALLOW_CALLEES:
                    for m in re.finditer(r"\b(?:" + e["callee"] + r")\s*\((?P<args>[^;]*)\)", ch.text):
                        inputs.update(re.findall(r"[A-Za-z_]\w*", m.group("args")))
                for piece in ch.text.split(";"):
                    piece = piece.strip()
                    if not piece or CONTROL_RE.match(piece):
                        continue
                    m = DECL_NAME_RE.match(piece)
                    if m and not re.search(r"\breturn\b", piece):
                        names.add(m.group(1))
    return names, names & inputs


def _has_call(piece):
    """A call other than a cast (static_cast<T>(x) is not an effect)."""
    stripped = CAST_RE.sub("(", piece)
    return bool(CALL_RE.search(stripped))


def _inert_piece(piece, locals_, inputs=frozenset()):
    if INERT_RE.match(piece):
        return True
    has_call = _has_call(piece)
    m = re.match(r"^([A-Za-z_]\w*)\s*(?:(?:\.|->)\w+|\[[^\]]*\])*\s*=[^=]", piece)
    if m and m.group(1) in locals_ and not has_call:
        return True   # a fill of a VP local (a context struct handed to an allowed / gated callee)
    if m and m.group(1) in inputs:
        return True   # a fill of an allowed callee's input struct: part of that (allowed) call
    m = re.match(r"^([A-Za-z_]\w*)\s*(?:\.|->)(reset|clear|swap)\s*\([^()]*\)\s*$", piece)
    if m and m.group(1) in locals_:
        return True   # releasing a VP local
    if not has_call and DECL_NAME_RE.match(piece) and not re.search(r"\breturn\b|\bthrow\b", piece):
        return True   # a declaration with no call
    if not has_call and not re.search(r"[=+\-*/%|&^]=?|\+\+|--|\breturn\b|\bthrow\b", piece):
        return True
    return False


def classify_statement(stmt, hook_lines, added, vp_slots, gated, allow_used):
    text = stmt.text
    if INERT_RE.match(text) or not text:
        return "inert", None
    c = _cond_of(text)
    if c and stmt.kind == "open" and c[1] == "" and (negative_guard(c[0], vp_slots) or positive_guard(c[0])):
        return "inert", None   # the guard line itself
    g = guarded(stmt, vp_slots, gated)
    if g:
        return g, None
    gc = gated_callee(text, gated)
    if gc:
        name, rel, line, verified = gc
        return ("gated" if verified else "gated-marker-unverified"), "%s (%s:%d)" % (name, rel, line)
    k = allowed_callee(text)
    if k is not None:
        allow_used[("callee", k)] = allow_used.get(("callee", k), 0) + 1
        return "allowed", "callee: " + ALLOW_CALLEES[k]["callee"]
    func = enclosing_function(stmt)
    if func is not None and func.start in added:
        return "vp-function", None   # a function absent at the base ref: its CALL SITES are checked, not its body
    if c and stmt.kind == "open" and c[1] == "":
        return "inert", None   # an `if (<vp condition>)` header: its body statements are judged on their own
    locals_, inputs = vp_locals(func, hook_lines | added) if func is not None else (set(), set())
    pieces = [p.strip() for p in text.split(";") if p.strip()]
    if pieces and all(_inert_piece(p, locals_, inputs) for p in pieces):
        return "inert", None
    return "unguarded", None


def non_pathway(rel):
    return rel.startswith(NON_PATHWAY_PREFIXES) or rel in NON_PATHWAY_FILES


def check_file(tree, rel, vp_slots, gated, allow_used, base_ref=None, no_git=False, added=None):
    text = tree.read(rel)
    hook_lines = hook_line_set(text)
    if text.splitlines() and text.splitlines()[0].strip().startswith("// VPV-HOOK-FILE"):
        return {"file": rel, "hook_file": True, "statements": [], "untagged": []}
    added = set(added or ())
    tree = parse_statements(strip_code(text))
    results = []
    stack = [tree]
    all_stmts = []
    while stack:
        node = stack.pop()
        for ch in node.children:
            all_stmts.append(ch)
            if ch.kind == "open":
                stack.append(ch)
    all_stmts.sort(key=lambda s: (s.start, s.depth))
    for st in all_stmts:
        span = range(st.start, (st.start if st.kind == "open" else st.end) + 1)
        if not any(l in hook_lines for l in span):
            continue
        if enclosing_function(st) is None:
            results.append({"file": rel, "line": st.start, "class": "declaration", "text": st.text[:120]})
            continue
        for k, e in enumerate(ALLOW_SITES):
            if e["file"] == rel and re.search(e["pattern"], st.text):
                allow_used[("site", k)] = allow_used.get(("site", k), 0) + 1
                results.append({"file": rel, "line": st.start, "class": "allowed", "why": "site: " + e["reason"][:60], "text": st.text[:120]})
                break
        else:
            cls, why = classify_statement(st, hook_lines, added, vp_slots, gated, allow_used)
            if cls == "unguarded" and non_pathway(rel):
                cls = "non-pathway"
            results.append({"file": rel, "line": st.start, "class": cls, "why": why, "text": st.text[:120]})
    untagged = []
    if added:
        lines = text.splitlines()
        stripped_lines = strip_code(text).splitlines()
        for n in sorted(added):
            if n in hook_lines or n > len(lines):
                continue
            code = stripped_lines[n - 1] if n - 1 < len(stripped_lines) else ""
            if VP_CALL_RE.search(code):
                untagged.append({"file": rel, "line": n, "class": "untagged", "text": lines[n - 1].strip()[:120]})
    return {"file": rel, "hook_file": False, "statements": results, "untagged": untagged}


def bill_files(tree, base_ref, only=None, no_git=False):
    if no_git:
        files = list(only or [])
    else:
        files = [f for f in git(["ls-tree", "-r", "--name-only", base_ref], tree.root).splitlines()
                 if f.startswith("src/") and not f.startswith(VPV_PREFIX) and f.lower().endswith(CODE_EXT)]
        if only:
            pats = [p.replace("\\", "/") for p in only]
            files = [f for f in files if f in pats]
        if tree.rev:
            head = set(git(["ls-tree", "-r", "--name-only", tree.rev, "--", "src"], tree.root).splitlines())
            files = [f for f in files if f in head]
    out = []
    for f in files:
        if not tree.rev and not tree.exists(f):
            continue
        out.append((f, vp_footprint.bucket_of(f)))
    return out


def all_code_files(root):
    out = []
    for dp, dn, fn in os.walk(os.path.join(root, "src")):
        dn[:] = [d for d in dn if d not in ("x64", ".git", "3rdparty", "Debug", "Release")]
        for f in fn:
            if f.lower().endswith(CODE_EXT):
                out.append(os.path.relpath(os.path.join(dp, f), root).replace(os.sep, "/"))
    return out


def run(root, base_ref, only=None, no_git=False, fail_untagged=False, rev=None):
    tree = Tree(root, rev)
    vp_slots = vp_slots_from(tree)
    gated = gated_markers(tree, tree.code_files())
    allow_used = {}
    files = bill_files(tree, base_ref, only, no_git)
    added_by_file = {}
    if not no_git and files:
        try:
            diff = vp_footprint.parse_diff(git(["diff"] + vp_footprint.DIFF_ARGS + ["-U0", base_ref] + ([rev] if rev else []) +
                                               ["--", "src", ":(exclude)src/VP-V"], root))
            added_by_file = {k: set(v["added_lines"]) for k, v in diff.items()}
        except RuntimeError:
            added_by_file = {}
    per_file = []
    tree.prefetch([rel for rel, _ in files])
    for rel, bucket in files:
        r = check_file(tree, rel, vp_slots, gated, allow_used, base_ref, no_git, added_by_file.get(rel))
        r["bucket"] = bucket
        per_file.append(r)
    hits, unverified, untagged, allowed, counts = [], [], [], [], {}
    for r in per_file:
        for s in r["statements"]:
            counts[s["class"]] = counts.get(s["class"], 0) + 1
            if r["bucket"] != "production":
                continue
            if s["class"] == "unguarded":
                hits.append(s)
            elif s["class"] == "gated-marker-unverified":
                hits.append(s)   # the call site of an unverified marker is an unguarded hook
            elif s["class"] == "allowed":
                allowed.append(s)
        if r["bucket"] == "production":
            untagged.extend(r["untagged"])
    for name, (rel, line, verified) in gated.items():
        if not verified:
            unverified.append({"file": rel, "line": line, "class": "gated-marker-unverified", "text": name})
    texts = [s["text"] for r in per_file for s in r["statements"] if r["bucket"] == "production"]
    for k, e in enumerate(ALLOW_CALLEES):
        if ("callee", k) not in allow_used and any(re.search(r"\b(?:" + e["callee"] + r")\s*\(", t) for t in texts):
            allow_used[("callee", k)] = 0   # reached through an enclosing header
    stale = [("callee", k) for k in range(len(ALLOW_CALLEES)) if ("callee", k) not in allow_used] + \
            [("site", k) for k in range(len(ALLOW_SITES)) if ("site", k) not in allow_used]
    rc = 1 if (hits or unverified or (fail_untagged and untagged)) else 0
    return {"base_ref": base_ref, "rev": rev, "vp_slots": sorted(vp_slots), "gated": {k: list(v) for k, v in gated.items()},
            "files": per_file, "counts": counts, "hits": hits, "unverified": unverified, "untagged": untagged,
            "allowed": allowed, "allow_used": {"%s:%d" % k: v for k, v in allow_used.items()}, "stale_allow": stale, "rc": rc}


def render(res):
    out = ["hook gating: base %s, tree %s" % (res["base_ref"], res.get("rev") or "working tree")]
    c = res["counts"]
    hook_stmts = sum(v for k, v in c.items() if k != "declaration")
    for s in res["hits"]:
        out.append("  UNGUARDED %s:%d %s" % (s["file"], s["line"], s["text"]))
    for s in res["unverified"]:
        out.append("  UNVERIFIED VPV-GATED marker (no `if (<original>) return` found) %s:%d %s" % (s["file"], s["line"], s["text"]))
    for s in res["untagged"]:
        out.append("  untagged %s:%d %s" % (s["file"], s["line"], s["text"]))
    by = {}
    for s in res["allowed"]:
        by[s["why"]] = by.get(s["why"], 0) + 1
    for k, e in enumerate(ALLOW_CALLEES):
        n = res["allow_used"].get("callee:%d" % k, 0)
        out.append("  allow-list callee [%d sites] %s: %s" % (n, e["callee"], e["reason"]))
    for k, e in enumerate(ALLOW_SITES):
        n = res["allow_used"].get("site:%d" % k, 0)
        out.append("  allow-list site [%d] %s /%s/: %s" % (n, e["file"], e["pattern"], e["reason"]))
    for kind, k in res["stale_allow"]:
        out.append("  stale allow-list entry (no site uses it, may be deleted): %s %d" % (kind, k))
    for name, (rel, line, verified) in sorted(res["gated"].items()):
        out.append("  VPV-GATED %s (%s:%d) %s" % (name, rel, line, "verified" if verified else "NOT VERIFIED"))
    out.append("  classes: " + ", ".join("%s %d" % (k, v) for k, v in sorted(c.items())))
    out.append("hook gating: %d hook statements, %d unguarded, %d unverified markers, %d allowed, %d untagged; exit %d" % (
        hook_stmts, len(res["hits"]), len(res["unverified"]), len(res["allowed"]), len(res["untagged"]), res["rc"]))
    if res["rc"]:
        out.append("FAIL: a VP-V hook in a Bill file runs in the Original pathway. Wrap the call site in the canonical guard "
                   "(`if (!pipelineSlots.OriginalPathway())`), or move the logic into its VP-V module behind a `// VPV-GATED` "
                   "function with `if (originalPathway) return;`, or ask the lead for an allow-list entry with a reason.")
    return "\n".join(out)


def main(argv=None):
    ap = argparse.ArgumentParser(description="Fail on a VP-V hook in a Bill file that is not gated off in the Original pathway.")
    ap.add_argument("--root", default=os.path.dirname(HERE))
    ap.add_argument("--base-ref", default=DEFAULT_BASE)
    ap.add_argument("--files", nargs="*")
    ap.add_argument("--json", action="store_true")
    ap.add_argument("--fail-untagged", action="store_true")
    ap.add_argument("--no-git", action="store_true", help="no git: --files are the Bill-owned files; no untagged report")
    ap.add_argument("--rev", help="check this git revision's tree instead of the working tree (no checkout needed)")
    args = ap.parse_args(argv)
    res = run(args.root, args.base_ref, args.files, args.no_git, args.fail_untagged, args.rev)
    if args.json:
        print(json.dumps({k: v for k, v in res.items() if k != "files"}, indent=1))
    else:
        print(render(res))
    return res["rc"]


if __name__ == "__main__":
    sys.exit(main())
