# Copyright(C) 2026 VP-V contributors
# SPDX-License-Identifier: GPL-3.0-only
"""Hook-gating checker (owner directive 2026-11-11: a VP-V call site in a Bill file does NOTHING in the Original pathway).

Usage: python -I tools/check_hook_gating.py [--base-ref a591ad3b5] [--root PATH] [--rev TIP] [--since MERGE-BASE]
                                            [--files F...] [--json] [--fail-untagged] [--no-git]

--base-ref is BILL's tip (the file-ownership baseline; a ref whose tree contains src/VP-V/ is refused). To see only
what a branch ADDED, use --since <merge-base> (hits, untagged lines and new sites on added lines only; unverified
VPV-GATED markers and their gated-marker-unverified call sites are always reported, wherever they are).

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
            above) and whose body the checker verified: `if (<positive predicate, || - joined>) return ...;`
            (predicate from the list above) at the body's top level, preceded only by inert statements
            (declarations without calls, and the guard's own predicate in one of its two house forms,
            `const bool originalPathway = OriginalPathwayOf(<x>.host);` or `... =
            Vpv::Hub::ExtensionHub::Instance().OriginalPathway(<x>.host);`, no other receiver chain); any earlier
            block, call or assignment leaves the marker unverified.
            A marker without that early return is a hit ("gated-marker-unverified"). Every definition is
            verified and reported on its own (also two of one name in different files); a call of a name with
            several definitions is gated only when all of them are verified. When the early return tests
            a PARAMETER, every call must pass a predicate form for it ("gated-arg-unverified" otherwise, S1).
            A local `bool originalPathway = <not the pathway query>;` or a plain assignment to a predicate name
            shadows it for the whole function: no guard, early return or gated body may rely on it (S5).
  vp-function  the innermost NAMED enclosing function is absent at --base-ref (header line added, name unused
            by Bill's base file); an edited header of a Bill function no longer exempts its body (S2).
  allowed   its callee is in ALLOW_CALLEES or the site is in ALLOW_SITES (reason required; reported).
  inert     a declaration without a call, a guard line itself, a brace / comment / preprocessor line.
Anything else is a hit ("unguarded"): file:line, exit 1.

Untagged VP insertions: a line added since --base-ref (git diff -w -U0, the working tree) that is not a hook
line and calls a VP-V namespace (Vp*::, Vpv::, RendererExtension::, ...). Reported as "untagged" (counted in the
summary line); they fail only with --fail-untagged (the footprint ratchet governs stray lines).

UNTAGGED VP-V REFERENCES (T-HOOKGATE-UNTAGGED, only with --since): a line added since <merge-base> in a Bill-owned
PRODUCTION file (the bill_files set above) that is not a hook line and names a VP-V SYMBOL fails the gate, tag or no
tag forgotten. VP-V symbol = a name DECLARED in a src/VP-V/**/*.h|.hpp header (namespace / class / struct / enum /
using alias / #define / function / method / data-member or variable name, >= 3 chars) MINUS every identifier that
Bill's baseline (--base-ref, files outside src/VP-V) already contains. Comments and string literals are ignored.
Output: `  untagged VP-V reference <file>:<line> <text>   [<symbols>]` per line, then
`<N> new untagged VP-V references in Bill files (lines added since <ref>)`; exit 1 when N > 0. Without --since
nothing changes. False positives: a new Bill-side name that a VP-V header also declares (e.g. a Qt slot or a local
called `tip`), declarations picked up from inline bodies in headers; list-and-justify, or rename. False negatives:
a VP-V name Bill's baseline also uses (it is then Bill's own), enumerators, a new #include of a VP-V header,
names declared only in .inl / .cpp files of src/VP-V, and symbols reached only through `auto` / a pointer member.

HOOK LINES = THE HONEST H2 (CC-audit-1, WAIVER-CC-H2-METRIC; rule 19): per Bill-owned file under src/ (documentation
files reported, not gated), H1 = non-blank added or changed lines vs --base-ref (tagged or not) and H2 = H1 + the
non-blank lines, recursively, of every VP-authored file (src/VP-V/**, or absent from Bill's tree) that an added #include
pulls into a class or function scope (vp_footprint.scope_map). Gated against docs/audit/hook-lines-baseline.json (v2:
per-file ceilings + production / tests / project totals; a v1 baseline is refused, exit 2) and the `h2_ceiling` of
hook-budget.json. Such fragments must be registered in docs/audit/host-fragments.json and included once (exit 1), are
checked EXPANDED (their statements pass the Original guard like Bill lines), and a VP-V function-like macro invoked at
class or function scope is a `macro-in-bill-scope` hit. An unresolved class / function-scope include: exit 2. The old
narrow tagged-line count is reported as `tagged`, information only. Selftests: --selftest.

Summary line: `hook gating: <hooks> hook statements, <unguarded> unguarded, <unverified> unverified markers,
<allowed> allowed, <untagged> untagged; exit <n>` (machine-readable like the other cheap checks).

SITES (rule 32, refs/107-host-seams-plan.md s5): a SITE is one maximal run of consecutive hook lines in a Bill-owned
file (a block, a cluster, or one line). docs/audit/hook-sites.json is the MANIFEST (one record per site: id, file,
anchor = enclosing symbol + 1-2 Bill lines before/after, the hook lines, the gate class; seeded with --write-manifest,
the re-graft script's input); docs/audit/hook-budget.json holds the sites-per-file ceilings (only DOWN; seeded with
--write-budget); docs/audit/hook-approvals.json lists the coordinator-approved seam ids (HOOK-APPROVAL-<n>: file +
a text fragment + seam + expiry; one row approves at most `max_sites` new sites, default 1, S4). A site whose (file, whitespace-insensitive text) is not in the manifest is NEW and
fails unless an approval matches it; a production file over its ceiling fails; a count below the ceiling is reported
as "ratchet down". Second summary line: `hook sites: <n> sites, <new> new (<approved> approved), <removed> removed,
<over> over budget; manifest <state>`.

EXPIRY (refs/107 s5.2): an approval's `expiry` is null (a canonical seam), a trigger id, or a list of trigger ids
(any of them). A trigger LANDED at a revision when an ancestor commit carries the trailer `Hook-Trigger: <id>`, or
docs/audit/hook-triggers.json lists it as {"<id>": {"landed": "<sha>"}} (the fallback for exports without history).
An approval whose trigger has landed while its text_contains is still in its file fails: `EXPIRED <id> (trigger <t>
landed at <sha>) still present`. An expiry or a temporary-budget trigger that hook-triggers.json does not list fails
too (UNKNOWN TRIGGER). hook-budget.json may carry `temporary`: {"<file>": {"limit": n, "reverts_to": m, "until":
[ids]}}: the ceiling is `limit` until ALL `until` triggers have landed, `reverts_to` from then on; the summary line
names every temporary ceiling in force.
"""
import argparse
import datetime
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
               r"CurrentQueueHoldFraction|RegisterDejudderCacheFrame|UpdatePipelineSlots|OrderFrameSlotHooks|CollectFrameHooks|DrawFrame)",
     "reason": "M2-relocated host members (input\\queue_timing IngressPolicy.inl / QueueTarget.inl, motion\\consumers "
               "PhaseTimeline.inl, segments\\CoreSegments.inl, segments\\EntranceHostMembers.inl): Bill's own pulldown / film clock / queue-target / slot-order / frame hook list / draw "
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
    {"file": "src/VideoProcessor-Lib/vprenderer/LibplaceboVideoRenderer.cpp", "pattern": r"\bPlanEntrance\(renderImage\b",
     "reason": "owner REVERT 2026-11-12 (r2/p4-original-srcres-feed): the P4 e-i move stopped feeding the source-resolution monitor in "
               "Original; pre-P4 (77fe0397e) that feed ran in Original. In Original the body feeds and configures the monitor and "
               "returns before any VP-V stage (EntrancePlanner.inl). Proof: EntranceHookTests "
               "TheEntranceSiteSitsBeforeTheEntranceDecisionsAndOutsideTheDrawCallback. CONFIRMED by the coordinator 2026-11-12 18:34 (rule 9; full pre-P4 Original behaviour: feed, sourceAutoActive, AutoFactors, kernel ID)."},
    {"file": "src/VideoProcessor-Config/ConfigEditorWindow.cpp", "pattern": r"\bdetailPulse\b", "symbol": "detailPulse",
     "reason": "name collision, not a VP-V reference: a ConfigEditorWindow.cpp LOCAL named detailPulse (lines 10230-10231) shares its "
               "spelling with the EntranceSegment.h:114 member (DPTR-OFF, r2/dptr-off). Granted by the coordinator, "
               "refs/owner-decisions-2026-11-07.md (2026-11-12 11:30 ruling 5: CEW:10230-10231 detailPulse ALLOW_SITES name "
               "collision GRANTED). Honoured by the --since VP-V symbol report only; proof: UntaggedVpReferenceTest."},
]

# Waived added lines for the --since VP-V symbol report. Keyed by the FULL TEXT of the added line (exact match after
# whitespace normalisation: leading indentation kept, inner runs collapsed, trailing blanks dropped; comments included),
# NOT a line number, so the entry survives a rebase but a changed or extended line is flagged again. An entry is honoured
# only when it matches EXACTLY ONE added line of its file in the run (zero = not reached, more than one = ambiguous:
# nothing honoured, the lines are flagged). Every entry names its waiver and expiry.
_P3_WAIVER = ("WAIVER-P3-ALIAS-REPOINT (coordinator 2026-11-12 11:30 ruling 2, conditional; condition met: the Phase 3 opus "
              "gate r2-phase3-opus-gate.md confirmed 8/8 lines are re-points of an existing alias read, no new site). "
              "Expires at the re-graft. Proof: UntaggedVpReferenceTest, r2-phase3-opus-gate.md.")
_P11_WAIVER = ("WAIVER-P11-REQUALIFY (coordinator-delegated; the P11 opus gate r2-p11-opus-gate.md confirmed 11/11 lines). "
               "Expires at the re-graft. Proof: UntaggedVpReferenceTest, r2-p11-opus-gate.md.")
_A1C_WAIVER = ("WAIVER-CC-RELOC2-BASELINE (CC series A1 73fc28716 / series C ccbeb5618 re-qualified 15 pre-existing Impl "
               "field accesses onto dejudderExtras.* / renderAheadPresenter.*; no new hook site, verified by lead20/CC-gemini-verify-2.md). "
               "Expires at the re-graft. Proof: test_check_hook_gating.py, lead20/CC-gemini-verify-2.md.")
_LVR = "src/VideoProcessor-Lib/vprenderer/LibplaceboVideoRenderer.cpp"
ALLOW_UNTAGGED_REFS = [
    {"file": _LVR, "text": '\t\t\trenderAheadPresenter.creditFrames.load(std::memory_order_relaxed), ratio,', "reason": _P3_WAIVER},
    {"file": _LVR, "text": '\t\t\t\trenderAheadPresenter.creditFrames.store(0, std::memory_order_release);', "reason": _P3_WAIVER},
    {"file": _LVR, "text": '\t\t\t\trenderAheadPresenter.mainInterval = plan.nextInterval;', "reason": _P3_WAIVER},
    {"file": _LVR, "text": '\t\tconst bool ditherOwnOnExtras = activeSettings.ditherVectorsOwn && !renderAheadPresenter.l1;', "reason": _P3_WAIVER},
    {"file": _LVR, "text": '\t\t\trenderAheadPresenter.mainInterval = plan.nextInterval;', "reason": _P3_WAIVER},
    {"file": _LVR, "text": '\t\t\trenderAheadPresenter.creditFrames.store(activeSettings.renderAheadQueueCredit ?', "reason": _P3_WAIVER},
    {"file": _LVR, "text": "\t\t\t\tdiag.mainInterval = static_cast<int>(presentPlan.interval); // the main present's SyncInterval", "reason": _P3_WAIVER},
    {"file": _LVR, "text": '\tconst double estimatedLeadMs = m_impl->renderAheadPresenter.estimatedLeadMs.load(std::memory_order_relaxed);', "reason": _P3_WAIVER},
    {"file": _LVR, "text": '\t\twant(C::MotionIntent, vpv.motion.intent.intentInterpreter && vpv.motion.intent.intentInterpreter->Active());', "reason": _P11_WAIVER},
    {"file": _LVR, "text": '\t\t\t\tvpv.sourceAnalysis.sourceMonitor.Release(d3d11->gpu); // and the tile readback', "reason": _P11_WAIVER},
    {"file": _LVR, "text": '\t\tdescriptions.push_back(SourceResolutionMonitor::Describe(vpv.sourceAnalysis.sourceMonitor.Read(), vpv.sourceAnalysis.sourceMonitor.AutoRatio(),', "reason": _P11_WAIVER},
    {"file": _LVR, "text": '\t\t\tactiveSettings.ambientAdapt || vpv.led.intent.ledIntentLimitsSent.shadowMinimumShare != 0.0 ||', "reason": _P11_WAIVER},
    {"file": _LVR, "text": '\t\t\tvpv.led.intent.ledIntentLimitsSent.ambientBlackNits != 0.0)', "reason": _P11_WAIVER},
    {"file": _LVR, "text": '        vpv.calibration.calibrationLutAttached = calibrationHost.LutAttached(rendered, targetLutApplied);', "reason": _P11_WAIVER},
    {"file": _LVR, "text": '\t\tm_impl->vpv.sourceAnalysis.sourceMonitor.Discontinuity("resync or source change");', "reason": _P11_WAIVER},
    {"file": _LVR, "text": '\t\tm_impl->vpv.sourceAnalysis.sourceMonitor.Discontinuity("live queue reset (seek or skip)");', "reason": _P11_WAIVER},
    {"file": _LVR, "text": '        m_impl ? m_impl->vpv.motion.intent.intentInterpreter : std::shared_ptr<IntentInterpreter::Observer>();', "reason": _P11_WAIVER},
    {"file": _LVR, "text": '\t\tconst bool intentWalkActive = m_impl && m_impl->vpv.motion.intent.intentInterpreter->Active();', "reason": _P11_WAIVER},
    {"file": _LVR, "text": '\t\t\t\tm_impl->vpv.sourceAnalysis.sourceMonitor.Discontinuity("file discontinuity (seek, step or rate change)");', "reason": _P11_WAIVER},
    {"file": _LVR, "text": '\tDejudderCacheStage::Stage dejudderCacheStage{ dejudderExtras.dejudderCache, dejudderExtras.dejudderGeometry, dejudderExtras.dejudderGeometryValid };', "reason": _A1C_WAIVER},
    {"file": _LVR, "text": '\t\t\t\tdejudderExtras.dejudderOutput.Release();', "reason": _A1C_WAIVER},
    {"file": _LVR, "text": '\t\t\t\tdejudderExtras.dejudderOutputSwapchain = nullptr;', "reason": _A1C_WAIVER},
    {"file": _LVR, "text": '\t\t\t\tdejudderExtras.dejudderExtraIds.clear();', "reason": _A1C_WAIVER},
    {"file": _LVR, "text": '\t\t\t\tdejudderExtras.dejudderMainRefresh.clear();', "reason": _A1C_WAIVER},
    {"file": _LVR, "text": '\t\t\t\tdejudderExtras.dejudderIdsSwapchain = nullptr;', "reason": _A1C_WAIVER},
    {"file": _LVR, "text": '\t\t\tif (RenderAheadActive() || dejudderExtras.renderAheadSwapchainLogged)', "reason": _A1C_WAIVER},
    {"file": _LVR, "text": '\t\t\t\tdejudderExtras.renderAheadSwapchainLogged = true;', "reason": _A1C_WAIVER},
    {"file": _LVR, "text": '                dejudderExtras.dejudderCache.Invalidate();', "reason": _A1C_WAIVER},
    {"file": _LVR, "text": '                dejudderExtras.dejudderGeometryValid = false;', "reason": _A1C_WAIVER},
    {"file": _LVR, "text": '                dejudderExtras.dejudderGeometry.numHolds = 0;', "reason": _A1C_WAIVER},
    {"file": _LVR, "text": '                dejudderExtras.dejudderNlsRetrySequence = sourceSequence;', "reason": _A1C_WAIVER},
    {"file": _LVR, "text": '\t\t\t\tdiag.openLoop = renderAheadPresenter.displayRate.openLoop;', "reason": _A1C_WAIVER},
    {"file": _LVR, "text": '\t\t\t\tdiag.holds = dejudderExtras.dejudderExtraStats.holds;', "reason": _A1C_WAIVER},
    {"file": _LVR, "text": '\t\t\t\t\t\t\t\t\t\tm_impl->dejudderExtras.renderAheadFlushReanchor.store(true, std::memory_order_release);', "reason": _A1C_WAIVER},
]


HOOK_MARK_RE = re.compile(r"//\s*VPV-HOOK(?![-\w])")
MANIFEST_PATH = "docs/audit/hook-sites.json"
BUDGET_PATH = "docs/audit/hook-budget.json"
HOOK_LINES_PATH = "docs/audit/hook-lines-baseline.json"
APPROVALS_PATH = "docs/audit/hook-approvals.json"
TRIGGERS_PATH = "docs/audit/hook-triggers.json"
TRAILER_KEY = "Hook-Trigger"
SITE_NOTE = ("a site = one maximal run of consecutive VPV-HOOK lines (vp_footprint hook_line_set widened to any line carrying "
             "`// VPV-HOOK`) in a Bill-owned file (present at the base ref, outside src/VP-V/); matched by file + whitespace-"
             "insensitive text; the anchor (enclosing symbol, Bill lines before / after) is for the re-graft script")


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


def _pred_term(term, bad=frozenset()):
    """`term` is a predicate form, and not a bare local name shadowed by a non-pathway value (HOOKGATE-CHECKER-HOLES S5)."""
    t = _unparen(term)
    return bool(PRED_RE.fullmatch(t)) and t not in bad


def negative_guard(cond, vp_slots, bad=frozenset()):
    """True when the condition is false in the Original pathway: a top-level && term is `!<pred>` or Runs(VpSlot)."""
    for term in _top_split(cond, "&&"):
        t = _unparen(term)
        if t.startswith("!") and _pred_term(t[1:], bad):
            return True
        m = RUNS_RE.fullmatch(t)
        if m and m.group(1) in vp_slots:
            return True
    return False


def positive_guard(cond, bad=frozenset()):
    """True when the condition is true in the Original pathway: a top-level || term is `<pred>`."""
    for term in _top_split(cond, "||"):
        if _pred_term(term, bad):
            return True
    return False


def early_return_before(func, stmt_start, parse_only=False, bad=frozenset()):
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
        if positive_guard(cond, bad) and re.match(r"^\s*(?:\{\s*)?return\b", body):
            return True
    return False


def _positive_early_return(ch, bad=frozenset()):
    c = _cond_of(ch.text)
    if not c:
        return False
    body = " ".join(x.text for x in ch.children) if ch.kind == "open" else c[1]
    return positive_guard(c[0], bad) and bool(re.match(r"^\s*(?:\{\s*)?return\b", body))


# HOOKGATE-CHECKER-HOLES S5: PRED_RE matches the predicate by NAME, so `bool originalPathway = false;` used to satisfy
# a guard. A bare predicate name declared in the enclosing function counts only when its value is the pathway query
# itself (a PRED_RE form, OriginalPathwayOf(<x>) or the hub query); a declaration without a value, any other value, or
# a plain assignment to it anywhere in the function shadows the predicate, and guards naming it no longer count.
PRED_DECL_RE = re.compile(r"^(?:(?:const|static|constexpr)\s+)*bool\s+(originalPathway|pipelineOriginalPathway)\s*"
                          r"(?:=\s*(?P<eq>.+?)|\{(?P<br>.*)\}|\((?P<pa>.*)\))?\s*$", re.S)
PRED_ASSIGN_RE = re.compile(r"^(originalPathway|pipelineOriginalPathway)\s*(?:=(?!=)|\|=|&=|\^=)")
PATHWAY_INIT_RE = re.compile(
    r"(?:[A-Za-z_][\w:]*(?:\.|->))*(?:OriginalPathway\(\s*\)|pipelineOriginalPathway|originalPathwayOn\(\s*\))"
    r"|(?:OriginalPathwayOf|Vpv::Hub::ExtensionHub::Instance\(\s*\)\.OriginalPathway)\(\s*[A-Za-z_]\w*(?:(?:\.|->)\w+)*\s*\)")
_SHADOW_CACHE = {}


def _walk(node):
    stack = [node]
    while stack:
        n = stack.pop()
        for ch in n.children:
            yield ch
            if ch.kind == "open":
                stack.append(ch)


def _shadowing_piece(piece):
    """The predicate name a statement piece shadows (a non-pathway declaration or a plain assignment), or None."""
    m = PRED_DECL_RE.match(piece)
    if m:
        init = m.group("eq") or m.group("br") or m.group("pa")
        if init is None or not PATHWAY_INIT_RE.fullmatch(_unparen(init.strip())):
            return m.group(1)
        return None
    m = PRED_ASSIGN_RE.match(piece)
    return m.group(1) if m else None


def shadowed_predicates(func):
    """The bare predicate names `func` declares or assigns with a value that is not the pathway query (S5)."""
    if func is None:
        return frozenset()
    hit = _SHADOW_CACHE.get(id(func))
    if hit is not None and hit[0] is func:
        return hit[1]
    bad = set()
    for ch in _walk(func):
        for piece in ch.text.split(";"):
            name = _shadowing_piece(piece.strip())
            if name:
                bad.add(name)
    bad = frozenset(bad)
    _SHADOW_CACHE[id(func)] = (func, bad)
    return bad


# The one call allowed before a VPV-GATED guard: the declaration of the guard's own predicate from the pathway query,
# in exactly one of the two house forms: the plain (unqualified) free call `const bool originalPathway =
# OriginalPathwayOf(ic.host);`, or the hub singleton `... = Vpv::Hub::ExtensionHub::Instance().OriginalPathway(f.host);`.
# No other receiver, call or qualifier may precede the query: a receiver chain such as `ResetAll().OriginalPathway(c.host)`
# or `Flush()->Drain().OriginalPathway(c.host)` would hide an effect inside the admitted declaration (HG2-RECEIVER-CHAIN,
# gate C-T3). Every accepted text is also accepted by the previous pattern (a tightening). Nothing else may share the
# statement.
PATHWAY_QUERY_DECL_RE = re.compile(
    r"^(?:const\s+)?bool\s+(?:originalPathway|pipelineOriginalPathway)\s*=\s*"
    r"(?:OriginalPathwayOf|Vpv::Hub::ExtensionHub::Instance\(\s*\)\.OriginalPathway)"
    r"\(\s*[A-Za-z_][\w]*(?:(?:\.|->)\w+)*\s*\)\s*;?\s*$")


def gated_guard_first(func):
    """A VPV-GATED body is verified only when its positive early return comes before every effect: the top-level
    children before it are inert (declarations without calls, comments). Any block (if / for / lambda body) or a
    statement with a call or an assignment ends the prefix (HOOKGATE-CHECKER-HOLES (2))."""
    for ch in func.children:
        if _positive_early_return(ch):
            return True
        if ch.kind == "open":
            return False
        if PATHWAY_QUERY_DECL_RE.match(ch.text.strip()):
            continue   # `const bool originalPathway = OriginalPathwayOf(<x>.host);` or the hub form: the guard's own predicate
        pieces = [p.strip() for p in ch.text.split(";") if p.strip()]
        if any(_shadowing_piece(p) for p in pieces):
            return False   # S5: `bool originalPathway = false;` before the guard is a declaration without a call, not the query
        if not all(_inert_piece(p, set()) for p in pieces):
            return False
    return False


def _top_args(text, open_at, angles=False, partial=False):
    """The top-level comma-separated arguments of the parenthesis that opens at text[open_at], or None if unbalanced.
    angles: `<` / `>` nest too (a parameter list's template types; never used for call arguments, where `<` compares).
    partial: when the text ends first (a statement cut at a lambda body's `{`), the arguments completed so far."""
    opens, closes = ("([{<", ")]}>") if angles else ("([{", ")]}")
    depth, cur, args = 0, [], []
    for ch in text[open_at:]:
        if ch in opens:
            depth += 1
            if depth == 1:
                continue
        elif ch in closes:
            depth -= 1
            if depth == 0:
                last = "".join(cur).strip()
                return args + [last] if (args or last) else []
        elif ch == "," and depth == 1:
            args.append("".join(cur).strip())
            cur = []
            continue
        cur.append(ch)
    return args if partial else None


def gated_predicate_param(func):
    """HOOKGATE-CHECKER-HOLES S1: the index of the parameter a VPV-GATED body's early return tests, or None (the
    predicate is a member, or the body declares it from the pathway query)."""
    for ch in func.children:
        if not _positive_early_return(ch):
            continue
        c = _cond_of(ch.text)
        names = [_unparen(t) for t in _top_split(c[0], "||") if PRED_RE.fullmatch(_unparen(t))]
        bare = [n for n in names if re.fullmatch(r"[A-Za-z_]\w*", n)]
        if not bare:
            return None
        header = func.text
        k = header.rfind(")")
        depth, open_at = 0, None
        for i in range(k, -1, -1):
            if header[i] == ")":
                depth += 1
            elif header[i] == "(":
                depth -= 1
                if depth == 0:
                    open_at = i
                    break
        if open_at is None:
            return None
        params = _top_args(header.replace("->", "\x00\x01"), open_at, angles=True) or []
        for idx, p in enumerate(params):
            decl = p.split("=", 1)[0].strip()
            m = re.search(r"([A-Za-z_]\w*)\s*$", decl)
            if m and m.group(1) in bare:
                return idx
        return None
    return None


def allowed_callee(text):
    for k, e in enumerate(ALLOW_CALLEES):
        if re.search(r"\b(?:" + e["callee"] + r")\s*\(", text):
            return k
    return None


def _gated_args_ok(text, name, idx, bad=frozenset()):
    """S1: every call of `name` in `text` passes a predicate form (PRED_RE, not a shadowed local) at parameter `idx`."""
    calls = list(re.finditer(r"\b%s\s*\(" % re.escape(name), text))
    if not calls:
        return False
    for m in calls:
        args = _top_args(text, m.end() - 1, partial=True)
        if args is None or idx >= len(args) or not _pred_term(args[idx], bad):
            return False
    return True


def gated_callee(text, gated, bad=frozenset()):
    """(name, file, line, verified, args_ok) of the first VPV-GATED callee called in `text`, or None.
    A name with several VPV-GATED definitions counts as verified only when every one of them is; the definition
    reported is then the first unverified one (HG-GATED-NAME-COLLISION). args_ok is False when any verified
    definition's early return tests a PARAMETER and a call passes anything but a predicate form for it (S1)."""
    params = getattr(gated, "param", {})
    for name, defs in gated.items():
        if re.search(r"\b%s\s*\(" % re.escape(name), text):
            if isinstance(defs, tuple):
                rel, line, verified = defs
            else:
                rel, line, verified = next((d for d in defs if not d[2]), defs[0])
            req_idxs = params.get(name)
            if req_idxs is None:
                ok = True
            elif isinstance(req_idxs, int):
                ok = _gated_args_ok(text, name, req_idxs, bad)
            else:
                ok = all(_gated_args_ok(text, name, idx, bad) for idx in req_idxs)
            return name, rel, line, verified, ok
    return None


def guarded(stmt, vp_slots, gated=None):
    func = enclosing_function(stmt)
    bad = shadowed_predicates(func)
    # 1. enclosing blocks (the function's own body counts up to the function)
    s = stmt
    while s is not None and s.kind != "root":
        if s.kind == "open":
            if is_function_header(s.text):
                break
            c = _cond_of(s.text)
            if c and negative_guard(c[0], vp_slots, bad):
                return "guarded"
            if allowed_callee(s.text) is not None:
                return "allowed-enclosing"
            g = gated_callee(s.text, gated or {}, bad)
            if g and g[3] and g[4]:
                return "gated-enclosing"
            if s.text.strip() == "else" or s.text.strip().startswith("else "):
                # the else of a positive check: the previous sibling header
                sib = s.parent.children
                k = sib.index(s)
                if k > 0:
                    pc = _cond_of(sib[k - 1].text)
                    if pc and positive_guard(pc[0], bad):
                        return "guarded"
        s = s.parent
    # 2. brace-less if: the statement itself or its previous sibling carries the condition
    c = _cond_of(stmt.text)
    if c and negative_guard(c[0], vp_slots, bad):
        return "guarded"
    sib = stmt.parent.children
    k = sib.index(stmt)
    if k > 0:
        pc = _cond_of(sib[k - 1].text)
        if pc and sib[k - 1].kind == "stmt" and pc[1] == "" and negative_guard(pc[0], vp_slots, bad):
            return "guarded"
        if sib[k - 1].kind == "open":
            pass
    # 3. early return in the enclosing function
    if func is not None and early_return_before(func, stmt.start, bad=bad):
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
        if first.startswith("!") and _pred_term(first[1:], bad):
            return "guarded"
    # 5. an inline `if (<allowed callee>(...)) stmt;` runs its body only when the callee says so
    if c and c[1] and allowed_callee(c[0]) is not None:
        return "allowed-enclosing"
    return None


# ---------------------------------------------------------------- gated callees

class GatedMap(dict):
    """{name: (file, line, verified)}; .param[name] = the index of the parameter the early return tests (S1), when it
    tests a parameter."""

    def __init__(self, *a, **k):
        dict.__init__(self, *a, **k)
        self.param = {}


def gated_markers(tree, files):
    """GatedMap {name: [(file, line, verified), ...]} for every `// VPV-GATED` function definition in `files`,
    sorted by file and line, with .param[name] = set of parameter indices tested by verified early returns (S1).
    EVERY definition of a name is kept and verified on its own (HG-GATED-NAME-COLLISION)."""
    out = GatedMap()
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
                        verified = gated_guard_first(ch)
                        out.setdefault(name, []).append((rel, ch.start, verified))
                        idx = gated_predicate_param(ch) if verified else None
                        if idx is not None:
                            out.param.setdefault(name, set()).add(idx)
    res = GatedMap({name: sorted(defs) for name, defs in out.items()})
    res.param = out.param
    return res


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


NO_BASE = object()   # check_file / classify_statement called without the base-ref identifiers (legacy callers)
FUNC_NAME_RE = re.compile(r"([A-Za-z_][\w:]*)\s*\([^()]*(?:\([^()]*\)[^()]*)*\)\s*(?:const|noexcept|override|final|volatile|&|&&)?\s*$")


class BaseIdents(frozenset):
    """The call/definition names of the base file, plus `.words`: every identifier it uses (for declarator names)."""
    words = frozenset()


DECL_SCOPE_RE = re.compile(r"^\s*(?:template\s*<.*>\s*)?(?:class|struct|union|enum|namespace|extern)\b")
DECLARATOR_RE = re.compile(r"([A-Za-z_]\w*)\s*(?:=\s*)?$")


def base_identifiers(text):
    """The names Bill's base file calls or defines (an identifier followed by `(`); None = the file is absent at base."""
    if text is None:
        return None
    code = strip_code(text)
    out = BaseIdents(re.findall(r"\b([A-Za-z_]\w*)\s*\(", code))
    out.words = frozenset(re.findall(r"\b[A-Za-z_]\w*\b", code))
    return out


def _new_declaration(lam, added, base_idents):
    """A lambda with no named function around it (a member / namespace-scope initializer): it is VP-V's only when
    every enclosing node up to the class / namespace scope starts on an added line and the declaration it initialises
    names something Bill's base file never uses."""
    n, top = lam, None
    while n is not None and n.kind != "root" and not DECL_SCOPE_RE.match(n.text):
        if n.kind == "open" and n.start not in added:
            return False
        top = n
        n = n.parent
    if top is None or top is lam:
        return False
    d = DECLARATOR_RE.search(top.text.strip())
    if not d:
        return False
    return base_idents is None or d.group(1) not in getattr(base_idents, "words", base_idents)


def vp_function(func, added, base_idents=NO_BASE):
    """HOOKGATE-CHECKER-HOLES S2: `func` is a function ABSENT at the base ref (its body is VP-V's; its call sites are
    checked instead). An added header line alone (a new parameter, a reformat) no longer exempts a Bill function: the
    innermost NAMED enclosing function (a lambda defers to the function around it) must have an added header and a
    name Bill's base file never uses."""
    if func is None or func.start not in added:
        return False
    if base_idents is NO_BASE:
        return True
    f, lam = func, func
    while f is not None:
        m = None if LAMBDA_HEADER_RE.search(f.text.strip()) else FUNC_NAME_RE.search(f.text)
        if m:
            break
        lam = f
        f = enclosing_function(f)
    if f is None:
        return _new_declaration(lam, added, base_idents)
    if f.start not in added:
        return False
    return base_idents is None or m.group(1).split("::")[-1] not in base_idents


def classify_statement(stmt, hook_lines, added, vp_slots, gated, allow_used, base_idents=NO_BASE):
    text = stmt.text
    if INERT_RE.match(text) or not text:
        return "inert", None
    c = _cond_of(text)
    if c and stmt.kind == "open" and c[1] == "" and (negative_guard(c[0], vp_slots) or positive_guard(c[0])):
        return "inert", None   # the guard line itself
    g = guarded(stmt, vp_slots, gated)
    if g:
        return g, None
    func = enclosing_function(stmt)
    gc = gated_callee(text, gated, shadowed_predicates(func))
    if gc:
        name, rel, line, verified, args_ok = gc
        if not verified:
            return "gated-marker-unverified", "%s (%s:%d)" % (name, rel, line)
        if not args_ok:
            p = gated.param[name]
            idx = p if isinstance(p, int) else min(p)
            return "gated-arg-unverified", "%s (%s:%d) parameter %d" % (name, rel, line, idx)
        return "gated", "%s (%s:%d)" % (name, rel, line)
    k = allowed_callee(text)
    if k is not None:
        allow_used[("callee", k)] = allow_used.get(("callee", k), 0) + 1
        return "allowed", "callee: " + ALLOW_CALLEES[k]["callee"]
    if vp_function(func, added, base_idents):
        return "vp-function", None   # a function absent at the base ref: its CALL SITES are checked, not its body
    if c and stmt.kind == "open" and c[1] == "":
        return "inert", None   # an `if (<vp condition>)` header: its body statements are judged on their own
    locals_, inputs = vp_locals(func, hook_lines | added) if func is not None else (set(), set())
    pieces = [p.strip() for p in text.split(";") if p.strip()]
    if pieces and all(_inert_piece(p, locals_, inputs) for p in pieces):
        return "inert", None
    return "unguarded", None


def _norm(text):
    return re.sub(r"\s+", " ", text).strip()


def _key(text):
    """The match key: every whitespace removed (indentation, spacing inside parentheses and line breaks do not matter)."""
    return re.sub(r"\s+", "", text)


def _short_name(rel):
    base = rel.rsplit("/", 1)[-1]
    stem, _, ext = base.rpartition(".")
    initials = "".join(ch for ch in stem if ch.isupper()) or stem[:3].upper()
    return "%s.%s" % (initials, ext)


# Fields --write-manifest recomputes from the tree; every other field of a matched entry (id, seam, expiry, any
# hand-added key) is kept as it was.
_FRESH_FIELDS = ("file", "start", "end", "anchor", "lines", "text", "gate")


def merge_manifest_sites(old_sites, new_sites):
    """Minimal-diff regeneration of the manifest site list.

    A tree site matches an old entry with the same (file, _key(text)).  Identical-text duplicates pair up in order
    when their count is unchanged; when it changed, the pairs with the same anchor are taken first, then the nearest
    old start line, then order (a heuristic: two duplicates with equal anchors that both moved can still swap ids).
    A matched site keeps the old entry's id, hand-filled fields and position; only the recomputed fields are
    refreshed.  Old entries with no tree site are dropped.  Unmatched tree sites are appended in tree order with
    their generated id, renumbered past the highest number in use for that prefix (old dropped ids included, so an
    id is never reused) when the id is already taken."""
    olds, news = {}, {}
    for i, m in enumerate(old_sites):
        olds.setdefault((m.get("file"), _key(m.get("text", ""))), []).append(i)
    for j, s_ in enumerate(new_sites):
        news.setdefault((s_["file"], _key(s_["text"])), []).append(j)
    matched = {}
    taken = set()
    for k, js in news.items():
        is_ = olds.get(k, [])
        if len(is_) == len(js):
            pairs = list(zip(is_, js))
        else:
            cand = sorted((old_sites[i].get("anchor") != new_sites[j]["anchor"],
                           abs((old_sites[i].get("start") or 0) - new_sites[j]["start"]), i, j) for i in is_ for j in js)
            pairs, ui, uj = [], set(), set()
            for _a, _d, i, j in cand:
                if i not in ui and j not in uj:
                    ui.add(i)
                    uj.add(j)
                    pairs.append((i, j))
        for i, j in pairs:
            matched[i] = new_sites[j]
            taken.add(j)
    added = [s_ for j, s_ in enumerate(new_sites) if j not in taken]
    out = []
    for i, m in enumerate(old_sites):
        s_ = matched.get(i)
        if s_ is None:
            continue
        e = dict(m)
        for f in _FRESH_FIELDS:
            e[f] = s_[f]
        for f, v in s_.items():
            if f not in e:
                e[f] = v
        out.append(e)
    used = set(m.get("id") for m in old_sites if m.get("id"))
    for s_ in added:
        sid = s_["id"]
        if sid in used:
            prefix = sid.rsplit("-", 1)[0]
            top = 0
            for u in used:
                p, _, n = u.rpartition("-")
                if p == prefix and n.isdigit():
                    top = max(top, int(n))
            sid = "%s-%04d" % (prefix, top + 1)
            s_ = dict(s_, id=sid)
        used.add(sid)
        out.append(s_)
    return out


def extract_sites(rel, text, hook_lines, tree_parse, classes_by_line):
    """One record per maximal run of consecutive hook lines."""
    lines = text.splitlines()
    runs = []
    for n in sorted(hook_lines):
        if runs and runs[-1][1] == n - 1:
            runs[-1][1] = n
        else:
            runs.append([n, n])
    # enclosing symbol: the nearest function header whose block spans the site
    headers = []
    stack = [tree_parse]
    while stack:
        node = stack.pop()
        for ch in node.children:
            if ch.kind == "open":
                stack.append(ch)
                if is_function_header(ch.text):
                    headers.append(ch)
    out = []
    for k, (a, b) in enumerate(runs, 1):
        sym = ""
        best = None
        for h in headers:
            if h.start <= a and b <= h.end and (best is None or h.start >= best.start):
                best = h
        if best is not None:
            m = re.search(r"([A-Za-z_][\w:<>~]*)\s*\([^()]*(?:\([^()]*\)[^()]*)*\)\s*(?:const|noexcept|override|final)?\s*$", best.text)
            sym = m.group(1) if m else best.text[:60]
        before, after = [], []
        i = a - 2
        while i >= 0 and len(before) < 2:
            if lines[i].strip() and (i + 1) not in hook_lines:
                before.insert(0, _norm(lines[i]))
            i -= 1
        i = b
        while i < len(lines) and len(after) < 1:
            if lines[i].strip() and (i + 1) not in hook_lines:
                after.append(_norm(lines[i]))
            i += 1
        site_lines = [lines[n - 1].rstrip() for n in range(a, b + 1)]
        gate = "declaration"
        order = ["unguarded", "gated-marker-unverified", "gated-arg-unverified", "non-pathway", "vp-function", "allowed", "allowed-enclosing",
                 "gated", "gated-enclosing", "guarded", "inert", "declaration"]
        found = [classes_by_line[n] for n in range(a, b + 1) if n in classes_by_line]
        for cls in order:
            if cls in found:
                gate = cls
                break
        out.append({"id": "%s-%04d" % (_short_name(rel), k), "file": rel, "start": a, "end": b,
                    "anchor": {"symbol": sym, "before": before, "after": after}, "lines": site_lines,
                    "text": _norm(" ".join(re.sub(r"//.*$", "", l) for l in site_lines)) or _norm(" ".join(site_lines)),
                    "gate": gate, "seam": None, "expiry": None})
    return out


def load_json(tree, rel):
    try:
        return json.loads(tree.read(rel))
    except (OSError, RuntimeError, ValueError):
        return None


TRAILER_LINE_RE = re.compile(r"^[A-Za-z][\w-]*[ \t]*:[ \t]*\S")


def trailer_values(message):
    """Values of the `Hook-Trigger:` lines in a commit message's TRAILER REGION (key matches in any case). The region is
    the last paragraph(s) made only of `Key: value` lines (a continuation line starts with whitespace): a standard git
    trailer block, and the blank-line variant where `Hook-Trigger:` sits in its own paragraph before `Co-Authored-By:`
    (commit 867594f19). The subject (first paragraph) never counts; a `Hook-Trigger:` line in ordinary body text does not."""
    paras, cur = [], []
    for ln in message.replace("\r\n", "\n").split("\n"):
        if ln.strip():
            cur.append(ln)
        elif cur:
            paras.append(cur)
            cur = []
    if cur:
        paras.append(cur)
    vals = []
    for para in reversed(paras[1:][-2:]):   # at most the last two Key: value-only paragraphs
        if not all(TRAILER_LINE_RE.match(l) or (k > 0 and l[:1] in " \t") for k, l in enumerate(para)):
            break
        for l in para:
            m = re.match(r"^%s[ \t]*:[ \t]*(.*)$" % re.escape(TRAILER_KEY), l, re.I)
            if m:
                vals.append(m.group(1))
    return vals


def trigger_scan(tree, root=None, rev=None, no_git=True):
    """Where the landed triggers come from, and every way that source can be wrong. Returns a dict:
    known (ids in docs/audit/hook-triggers.json), landed ({id: sha}), errors (each one fails the run), source (summary text).
    Trailers: `Hook-Trigger: <id>[, <id>]` on the ancestors of `rev` (HEAD for the working tree); the key matches in any case
    (git's trailer parser plus `-i` on the pre-filter grep), a value matches a known id stripped and in any case, and a value
    that names no known id is an UNKNOWN LANDED TRIGGER. Only real trailers count (git's own trailer parser): `Hook-Trigger:`
    in the body text is not a landing. A git failure in git mode is an error, never an empty history."""
    doc = load_json(tree, TRIGGERS_PATH) or {}
    known, landed, errors = set(), {}, []
    for k, v in doc.items():
        if k.startswith("_"):
            continue
        known.add(k)
        if isinstance(v, dict) and v.get("landed"):
            landed[k] = str(v["landed"])
    by_norm = {k.strip().lower(): k for k in known}
    if no_git or not root:
        return {"known": known, "landed": landed, "errors": errors, "source": "json only"}
    try:
        log = git(["log", rev or "HEAD", "-i", "--grep=^%s:" % TRAILER_KEY, "--format=%H%x00%B%x1e"], root)
    except RuntimeError as e:
        errors.append("TRIGGER SOURCE FAILED: git history of %s is unavailable, so landed triggers cannot be read (%s); rerun in a "
                      "git checkout, or use --no-git to check docs/audit/hook-triggers.json alone" % (rev or "HEAD", " ".join(str(e).split())[:160]))
        return {"known": known, "landed": landed, "errors": errors, "source": "json + trailers FAILED"}
    for rec in log.split("\x1e"):
        if "\x00" not in rec:
            continue
        sha, body = rec.strip("\r\n").split("\x00", 1)
        for v in trailer_values(body):
            for raw in v.split(","):
                tid = raw.strip()
                if not tid:
                    continue
                kid = by_norm.get(tid.lower())
                if kid is None:
                    errors.append("UNKNOWN LANDED TRIGGER %s at %s" % (tid, sha[:12]))
                    continue
                landed[kid] = sha[:12]   # git log runs newest first: the oldest landing commit wins
    return {"known": known, "landed": landed, "errors": errors, "source": "json + trailers of %s" % (rev or "HEAD")}


def landed_triggers(tree, root=None, rev=None, no_git=True):
    """(known trigger ids, {id: sha} landed): docs/audit/hook-triggers.json, plus `Hook-Trigger: <id>` commit trailers on
    the ancestors of `rev` (HEAD for the working tree) unless no_git. The errors are in trigger_scan()."""
    t = trigger_scan(tree, root, rev, no_git)
    return t["known"], t["landed"]


def _expiry_ids(expiry):
    if expiry is None:
        return []
    return [expiry] if isinstance(expiry, str) else list(expiry)


# CC-HOOKGATING-BETATESTS: Bill's BetaTests binary hosts gated // VPV-HOOK sites (BetaParametersTests.cpp);
# include it alongside production files in site_checks and untagged_vp_references (Original-pathway guarding stays production-only).
BETA_TESTS_PREFIX = "src/VideoProcessor-BetaTests/"


def _site_gated(row):
    return row.get("bucket") == "production" or (row.get("file") or "").startswith(BETA_TESTS_PREFIX)


def site_checks(tree, per_file, root=None, rev=None, no_git=True):
    """Manifest / budget / approvals / expiry: returns a dict with the counts and the failures."""
    sites = [s for r in per_file for s in r.get("sites", [])]
    prod = {r["file"] for r in per_file if _site_gated(r)}
    manifest = load_json(tree, MANIFEST_PATH)
    budget = load_json(tree, BUDGET_PATH)
    approvals = load_json(tree, APPROVALS_PATH) or {"approvals": []}
    res = {"sites": len(sites), "new": [], "approved": [], "removed": [], "over": [], "ratchet": [],
           "manifest": "none" if manifest is None else MANIFEST_PATH, "budget": "none" if budget is None else BUDGET_PATH,
           "approvals_used": [], "expired": [], "unknown": [], "temporary": [], "trigger_errors": [],
           "approval_missing": [], "budget_errors": [], "headroom": [], "approval_over": []}
    ts = trigger_scan(tree, root, rev, no_git)
    known_t, landed_t = ts["known"], ts["landed"]
    res["trigger_errors"] = ts["errors"]
    res["trigger_source"] = ts["source"]
    texts = {}
    def _file_key(rel):
        if rel not in texts:
            try:
                texts[rel] = _key(tree.read(rel))
            except (OSError, RuntimeError):
                texts[rel] = None   # unreadable: reported as APPROVAL FILE MISSING, never matched as empty text
        return texts[rel]
    for ap in approvals.get("approvals", []):
        if _file_key(ap.get("file") or "") is None:
            res["approval_missing"].append("APPROVAL FILE MISSING %s %s" % (ap.get("id"), ap.get("file")))
        for tid in _expiry_ids(ap.get("expiry")):
            if tid not in known_t and tid not in landed_t:
                res["unknown"].append("approval %s names trigger %s (not in %s)" % (ap.get("id"), tid, TRIGGERS_PATH))
        for tid in _expiry_ids(ap.get("expiry")):
            if tid in landed_t and ap.get("text_contains") and _key(ap["text_contains"]) in (_file_key(ap.get("file") or "") or ""):
                res["expired"].append("EXPIRED %s (trigger %s landed at %s) still present in %s" % (ap.get("id"), tid, landed_t[tid], ap.get("file")))
                break
    if manifest is not None:
        known = {}
        for m in manifest.get("sites", []):
            known.setdefault((m["file"], _key(m["text"])), []).append(m["id"])
        seen = set()
        for s in sites:
            key = (s["file"], _key(s["text"]))
            if key in known:
                seen.add(key)
                continue
            if s["file"] not in prod:
                continue
            ok = None
            for ap in approvals.get("approvals", []):
                if ap.get("file") == s["file"] and ap.get("text_contains") and _key(ap["text_contains"]) in _key(s["text"]):
                    ok = ap["id"]
                    break
            if ok:
                res["approved"].append(dict(s, approval=ok))
                res["approvals_used"].append(ok)
            else:
                res["new"].append(s)
        for key, ids in known.items():
            if key not in seen:
                res["removed"].extend(ids)
        # HOOKGATE-CHECKER-HOLES (4): one approval row approves at most max_sites new sites (default 1), so a single
        # `text_contains` cannot silently admit every later copy of the same statement.
        used = {}
        for aid in res["approvals_used"]:
            used[aid] = used.get(aid, 0) + 1
        rows = {ap.get("id"): ap for ap in approvals.get("approvals", [])}
        for aid, n in sorted(used.items()):
            cap = rows.get(aid, {}).get("max_sites", 1)
            if not isinstance(cap, int) or isinstance(cap, bool) or cap < 1:
                res["approval_over"].append("APPROVAL INVALID %s max_sites %r (a positive integer)" % (aid, cap))
            elif n > cap:
                res["approval_over"].append("APPROVAL OVER %s approves %d sites > max_sites %d" % (aid, n, cap))
    if budget is not None:
        limits = dict(budget.get("files", {}))
        for f, tmp in sorted((budget.get("temporary") or {}).items()):
            if int(tmp["reverts_to"]) > int(limits.get(f, 0)):
                res["budget_errors"].append("BUDGET INCONSISTENT temporary %s reverts_to %d > files %d (the files ceiling is the "
                                            "one source of truth: reverts_to may not exceed it)" % (f, int(tmp["reverts_to"]), int(limits.get(f, 0))))
            until = list(tmp.get("until") or [])
            for tid in until:
                if tid not in known_t and tid not in landed_t:
                    res["unknown"].append("temporary budget %s names trigger %s (not in %s)" % (f, tid, TRIGGERS_PATH))
            pending = [u for u in until if u not in landed_t]
            if pending:
                limits[f] = int(tmp["limit"])
                res["temporary"].append({"file": f, "limit": int(tmp["limit"]), "reverts_to": int(tmp["reverts_to"]), "pending": pending})
            else:
                limits[f] = int(tmp["reverts_to"])
        temp_files = {x["file"]: x for x in res["temporary"]}
        counts = {}
        for s in sites:
            if s["file"] in prod:
                counts[s["file"]] = counts.get(s["file"], 0) + 1
        for f, n in sorted(counts.items()):
            lim = limits.get(f, 0)
            if n > lim:
                res["over"].append("%s %d sites > %sbudget %d" % (f, n, "temporary " if f in temp_files else "", lim))
            elif n < lim:
                tx = temp_files.get(f)
                if tx is None:
                    res["ratchet"].append("%s %d sites < budget %d (ratchet the ceiling down)" % (f, n, lim))
                elif n < tx["reverts_to"]:
                    # a pending temporary ceiling: the ratchet reference is the base budget it reverts to
                    res["ratchet"].append("%s %d sites < budget %d (ratchet the ceiling down)" % (f, n, tx["reverts_to"]))
                else:
                    # base <= count < temporary limit: the allowance is not yet used here (informational, exit unchanged)
                    res["headroom"].append("temporary headroom %s %d < %d (pending %s)" % (f, n, lim, ", ".join(tx["pending"])))
    for x in res["temporary"]:
        x["count"] = counts.get(x["file"], 0) if budget is not None else 0
    return res


def non_pathway(rel):
    return rel.startswith(NON_PATHWAY_PREFIXES) or rel in NON_PATHWAY_FILES


def check_file(tree, rel, vp_slots, gated, allow_used, base_ref=None, no_git=False, added=None, base_idents=NO_BASE, fragments=None):
    text = tree.read(rel)
    if text.splitlines() and text.splitlines()[0].strip().startswith("// VPV-HOOK-FILE"):
        return {"file": rel, "hook_file": True, "statements": [], "untagged": [], "sites": []}
    if fragments:
        # CC-audit-1 spec 2: check the includer with its class / function-scope VP fragments expanded in place. Every
        # expanded line is a hook line (a VP line in a Bill scope), so a statement inside a function-scope fragment must
        # pass the Original guard like any Bill line; results map back to the includer line (`via` = fragment:line).
        text, linemap, frag_lines = expand_fragments(rel, text, fragments, tree_reader(tree))
        was_added = set(added or ())
        res = _check_text(rel, text, vp_slots, gated, allow_used,
                          {k for k, (f, ln, top) in enumerate(linemap, 1) if top in was_added}, frag_lines, base_idents)

        def back(k):
            f, ln, top = linemap[k - 1] if 0 < k <= len(linemap) else (rel, k, k)
            return top, (None if f == rel else "%s:%d" % (f, ln))
        for key in ("statements", "untagged"):
            for s in res[key]:
                s["line"], via = back(s["line"])
                if via:
                    s["via"] = via
        for s in res["sites"]:
            s["start"], via = back(s["start"])
            s["end"] = back(s["end"])[0]
            if via:
                s["via"] = via
        return res
    return _check_text(rel, text, vp_slots, gated, allow_used, set(added or ()), set(), base_idents)


def _check_text(rel, text, vp_slots, gated, allow_used, added, extra_hook_lines, base_idents=NO_BASE):
    hook_lines = hook_line_set(text) | extra_hook_lines
    tree = parse_statements(strip_code(text))
    tree_parse_cache = [tree]
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
            if e["file"] == rel and not e.get("symbol") and re.search(e["pattern"], st.text):
                allow_used[("site", k)] = allow_used.get(("site", k), 0) + 1
                results.append({"file": rel, "line": st.start, "class": "allowed", "why": "site: " + e["reason"][:60], "text": st.text[:120]})
                break
        else:
            cls, why = classify_statement(st, hook_lines, added, vp_slots, gated, allow_used, base_idents)
            if cls == "unguarded" and non_pathway(rel):
                cls = "non-pathway"
            results.append({"file": rel, "line": st.start, "class": cls, "why": why, "text": st.text[:120]})
    classes_by_line = {}
    for r in results:
        classes_by_line.setdefault(r["line"], r["class"])
    sites = extract_sites(rel, text, hook_lines, tree_parse_cache[0], classes_by_line)
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
    return {"file": rel, "hook_file": False, "statements": results, "untagged": untagged, "sites": sites}


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


def bill_tip_check(root, base_ref):
    """--base-ref must be BILL's tip (no src/VP-V/ in its tree); a merge base of ours is the wrong ref (use --since)."""
    try:
        names = git(["ls-tree", "-d", "--name-only", base_ref, "--", "src/VP-V"], root).strip()
    except RuntimeError:
        return None
    return "src/VP-V" in names


# ---------------------------------------------------------------- untagged VP-V references (--since)
# T-HOOKGATE-UNTAGGED (coordinator 2026-11-12): the VP_CALL_RE "untagged" report above only knows a fixed list of
# VP-V namespaces, so an added `sourceMonitor.SetGeometryDetect(..); SyncFallback(FallbackCatalog::X())` slipped
# through as "0 new". Under --since this check is derived from the VP-V sources instead.
IDENT_RE = re.compile(r"[A-Za-z_]\w*")
VPSYM_TYPE_RE = re.compile(r"\b(?:namespace|class|struct|union|enum(?:\s+class|\s+struct)?)\s+([A-Za-z_]\w*)")
VPSYM_USING_RE = re.compile(r"\busing\s+([A-Za-z_]\w*)\s*=")
VPSYM_DEFINE_RE = re.compile(r"^\s*#\s*define\s+([A-Za-z_]\w*)", re.M)
VPSYM_FUNC_RE = re.compile(r"^\s*(?:(?:template\s*<[^>]*>|virtual|static|inline|constexpr|explicit|friend|extern)\s+)*"
                           r"([\w:<>,*&\s~]*?[\s*&])?(~?[A-Za-z_]\w*)\s*\(", re.M)
VPSYM_VAR_RE = re.compile(r"^\s*(?:(?:static|const|constexpr|inline|mutable|volatile)\s+)*"
                          r"[\w:<>,*&\s]*?[\s*&]+([A-Za-z_]\w*)\s*(?:\[[^\]]*\])?\s*(?:=|\{|;)", re.M)
VPSYM_NOT_DECL = frozenset(("return", "if", "else", "for", "while", "switch", "case", "throw", "delete", "new", "goto",
                            "using", "namespace", "typedef", "do", "try", "catch", "co_return", "co_await", "sizeof",
                            "static_assert", "operator", "typename", "template", "public", "private", "protected", "default"))
VPSYM_MIN_LEN = 3
VPSYM_HEADER_EXT = (".h", ".hpp")


def vp_symbols(tree):
    """Names DECLARED in the VP-V headers of `tree` (src/VP-V/**/*.h|.hpp): namespace / class / struct / union / enum
    names, `using X =` aliases, #define macros, function / method names and data-member / variable names. Names only
    (no qualification), at least VPSYM_MIN_LEN characters, keywords dropped. The caller subtracts every name Bill's
    baseline already uses (bill_identifiers), so generic names (Get, size, x ...) never count."""
    names = set()
    heads = [f for f in tree.code_files() if f.startswith(VPV_PREFIX) and f.lower().endswith(VPSYM_HEADER_EXT)]
    tree.prefetch(heads)
    for rel in heads:
        text = tree.read(rel)
        names.update(VPSYM_DEFINE_RE.findall(text))
        code = strip_code(text)
        names.update(VPSYM_TYPE_RE.findall(code))
        names.update(VPSYM_USING_RE.findall(code))
        for m in VPSYM_FUNC_RE.finditer(code):
            before, name = m.group(1) or "", m.group(2)
            if re.search(r"[=.+\-(]|->", before):
                continue
            first = IDENT_RE.match(before.strip() or name)
            if first and first.group(0) in VPSYM_NOT_DECL:
                continue
            names.add(name.lstrip("~"))
        for m in VPSYM_VAR_RE.finditer(code):
            line = m.group(0)
            first = IDENT_RE.match(line.strip())
            if first and first.group(0) in VPSYM_NOT_DECL:
                continue
            if "(" in line[:m.start(1) - m.start(0)]:
                continue
            names.add(m.group(1))
    return set(n for n in names if len(n) >= VPSYM_MIN_LEN and n not in VPSYM_NOT_DECL)


def bill_identifiers(root, base_ref, candidates):
    """The subset of `candidates` that appears (as an identifier, comments included) anywhere in Bill's baseline
    code (every src code file at base_ref outside src/VP-V): those names are Bill's own, not VP-V symbols."""
    base = Tree(root, base_ref)
    rels = [f for f in git(["ls-tree", "-r", "--name-only", base_ref, "--", "src"], root).splitlines()
            if f.lower().endswith(CODE_EXT) and not f.startswith(VPV_PREFIX) and "/3rdparty/" not in f and "/x64/" not in f]
    base.prefetch(rels)
    seen = set()
    for rel in rels:
        text = base.cache.get(rel)
        if text:
            seen.update(candidates.intersection(IDENT_RE.findall(text)))
    return seen


def norm_line(line):
    """Whitespace-normalised line text for the waiver entries: indentation kept, inner whitespace runs collapsed."""
    return line[:len(line) - len(line.lstrip(" 	"))] + " ".join(line.split())


def untagged_vp_references(tree, per_file, since_added, root, base_ref, allow_used=None):
    """Added lines (since_added: file -> line numbers) of Bill-owned PRODUCTION (and BetaTests) files that are not hook lines and
    name a VP-V symbol (vp_symbols minus bill_identifiers). Returns [{file, line, text, symbols}]."""
    wanted = [(r["file"], since_added.get(r["file"])) for r in per_file
              if _site_gated(r) and not r.get("hook_file") and since_added.get(r["file"])]
    if not wanted:
        return []
    syms = vp_symbols(tree)
    syms -= bill_identifiers(root, base_ref, syms)
    out = []
    for rel, added in wanted:
        text = tree.read(rel)
        hooks = hook_line_set(text)
        lines = text.splitlines()
        code = strip_code(text).splitlines()
        cand = [n for n in sorted(added) if n not in hooks and n <= len(code)]
        waived = {}   # line -> ALLOW_UNTAGGED_REFS index; an entry counts only when it matches exactly one added line
        for k, e in enumerate(ALLOW_UNTAGGED_REFS):
            if e["file"] == rel:
                m = [n for n in cand if norm_line(lines[n - 1]) == e["text"]]
                if len(m) == 1:
                    waived[m[0]] = k
        for n in cand:
            if n in waived:
                if allow_used is not None:
                    allow_used[("untag", waived[n])] = allow_used.get(("untag", waived[n]), 0) + 1
                continue
            hit = sorted(syms.intersection(IDENT_RE.findall(code[n - 1])))
            for k, e in enumerate(ALLOW_SITES):   # granted name collisions: drop the named symbol (entry key "symbol")
                if e.get("symbol") in hit and e["file"] == rel and re.search(e["pattern"], code[n - 1]):
                    hit.remove(e["symbol"])
                    if allow_used is not None:
                        allow_used[("site", k)] = allow_used.get(("site", k), 0) + 1
            if hit:
                out.append({"file": rel, "line": n, "text": lines[n - 1].strip()[:120], "symbols": hit[:6]})
    return out


# ---------------------------------------------------------------- the honest H2 metric (CC-audit-1, WAIVER-CC-H2-METRIC)
# H2 per Bill-owned file (present at --base-ref, under src/, outside src/VP-V/, not a documentation file) = H1 (non-blank
# added or changed lines, tagged or not) + the non-blank lines, recursively, of every VP-authored file an ADDED #include
# pulls into a class or function scope (vp_footprint.h2_row). Production AND tests are gated (tests hold Bill text too,
# rule 29); project files are counted. The old narrow count (vp_footprint.hook_line_set on added production lines) is
# kept as `tagged`, for information only. Rule 19: no move of lines between a Bill scope and a fragment lowers H2.
H2_METRIC_VERSION = 2
# The tu measure's own version (G-CC2 MUST-A): 2 = shared headers only through LIVE includes (not under #if / #ifdef)
# from VP-V .cpp files a build file lists as ClCompile, and never a file defining members of a Bill-owned class. A
# baseline seeded with an older tu measure is re-seeded by a named migration (--migrate WAIVER-<ID>) or a union-base
# rebase (--rebase-to). 3 (G-CC4 MUST-B) = the same on LEXED text (no include, class or member in a comment or a
# string), a guard only in a header on a macro the compiler does not predefine, sources only from items a .vcxproj
# really compiles in x64 Release (imports resolved; false / undecidable conditions and ExcludedFromBuild drop an
# item), no decoy-class exemption, macro qualifiers expanded; and (V-GEM, Gemini 2be3cebf4) a shared header is exempt
# only for its declaration-only lines (vp_footprint.header_body_lines): its bodies count in every Bill includer's tu,
# and the walk continues through it. 4 (CC-gate-5 MUST-C / SHOULD-E) = strict _brace_kind (class/ns heads, fail-closed
# unknown -> function), lone `{` opening function bodies, all #define continuation lines, .sln Release|x64 filtering,
# ExcludedFromBuild attribute / Remove / ItemDefinitionGroup, and header-guard pre-define dead-include detection.
TU_MEASURE_VERSION = 4
# The most a named RELOCATION may let one includer's combined h2 + tu grow (G-CC2 SHOULD-B; bookkeeping lines:
# a declaration and an include per moved definition): --allowance N, 0 <= N <= this. G-CC4 SHOULD-D: 8, and a
# relocation needs a real h2 drop in every includer whose tu rises.
RELOCATION_MAX_ALLOWANCE = 8
# --rebase-to: a landed union tip is an ancestor of (or equal to) a branch matching one of these (--union-ref adds one)
UNION_REF_PATTERNS = ("r2/composite*",)
DOC_EXT = (".md", ".txt", ".html", ".htm", ".rst")
H2_BUCKETS = ("production", "tests", "project")
RENDERER_FILES = ("src/VideoProcessor-Lib/vprenderer/LibplaceboVideoRenderer.cpp",
                  "src/VideoProcessor-Lib/vprenderer/LibplaceboVideoRenderer.h",
                  "src/VideoProcessor-Lib/vprenderer/LibplaceboPluginVideoRenderer.cpp",
                  "src/VideoProcessor-Lib/vprenderer/LibplaceboPluginVideoRenderer.h",
                  "src/VideoProcessor-VPRenderer/LibplaceboPluginExports.cpp")
FRAGMENTS_PATH = "docs/audit/host-fragments.json"
VP_MACRO_RE = re.compile(r"^\s*#\s*define\s+([A-Za-z_]\w*)\(", re.M)


def tree_reader(tree):
    """read(rel) -> text or None over a Tree (for include resolution)."""
    def read(rel):
        try:
            return tree.read(rel)
        except (OSError, RuntimeError):
            return None
    return read


def h2_rows(tree, root, base_ref, diff, no_git, only):
    """{file: {bucket, h1, inc, h2, fragments, errors}} for every Bill-owned file with H2 > 0 (doc files: bucket "docs",
    information only). Under --no-git there is no Bill baseline: every non-blank line of each --files file counts as
    added (fail closed) and only src/VP-V/ files are VP-authored."""
    read = tree_reader(tree)
    rows = {}
    if no_git:
        shared = None
        for rel in only or []:
            text = read(rel)
            if text is None:
                continue
            nb = [i for i, ln in enumerate(text.splitlines(), 1) if ln.strip()]
            inc = [i for i in nb if vp_footprint.INCLUDE_ANY_RE.match(text.splitlines()[i - 1])]
            rows[rel] = vp_footprint.h2_row(rel, nb, inc, read, None, text)
            rows[rel]["bucket"] = vp_footprint.bucket_of(rel)
            if inc and shared is None:
                shared = _shared_headers(tree, read, None, root, no_git)
            _add_tu(rows[rel], rel, inc, read, None, shared or set())
        return rows
    bill_tree = set(git(["ls-tree", "-r", "--name-only", base_ref], root).splitlines())
    want = [p for p, d in diff.items() if p in bill_tree and p.startswith("src/") and not p.startswith(VPV_PREFIX)
            and d["nonblank_added_lines"]]
    if only:
        pats = {p.replace("\\", "/") for p in only}
        want = [p for p in want if p in pats]
    tree.prefetch([p for p in want if diff[p]["include_lines"]])
    shared = _shared_headers(tree, read, bill_tree, root, no_git) if any(diff[p]["include_lines"] for p in want) else set()
    macros = _build_macros(tree, root, no_git) if shared else {}
    for p in want:
        d = diff[p]
        if not tree.rev and not tree.exists(p):
            continue
        r = vp_footprint.h2_row(p, d["nonblank_added_lines"], d["include_lines"], read, bill_tree)
        r["bucket"] = "docs" if p.lower().endswith(DOC_EXT) else vp_footprint.bucket_of(p)
        _add_tu(r, p, d["include_lines"], read, bill_tree, shared, macros)
        if r["h2"] or r["tu"]:
            rows[p] = r
    return rows


def _shared_headers(tree, read, bill_tree, root=None, no_git=False):
    """The VP-authored files a VP-authored production .cpp reaches through live includes (vp_footprint.shared_vp_headers):
    not part of tu. G-CC2 MUST-A: only a .cpp that a build file lists as ClCompile counts as a source, and a file
    defining members of a Bill-owned class is never shared. G-CC4 MUST-B 3 & SHOULD-E: only items a .vcxproj listed in
    VideoProcessor.sln (Release|x64) really compiles, with PreprocessorDefinitions checked against header guards."""
    vp = [f for f in tree.code_files() if vp_footprint.is_vp_authored(f, bill_tree)]
    tree.prefetch(vp)
    compiled = [f for f in vp if vp_footprint.is_compiled_vp_source(f, bill_tree)]
    btexts = _build_texts(tree, root, no_git, (".vcxproj", ".props", ".targets", ".sln"))
    listed = vp_footprint.build_listed_sources(btexts, compiled)
    preproc_defs = vp_footprint.build_preprocessor_defines(btexts)
    tree.prefetch([p for p in (bill_tree or ()) if p.startswith("src/") and p.lower().endswith((".h", ".hpp"))])
    classes = vp_footprint.bill_class_names(read, bill_tree)
    return vp_footprint.shared_vp_headers(sorted(listed), read, bill_tree, classes, preproc_defs=preproc_defs)


def _build_texts(tree, root, no_git, exts):
    """{path: text} of the tree's build files with these extensions (outside x64/ and 3rdparty/). Git mode: the tracked
    files of the checked tree; --no-git: a walk of <root>/src plus root-level .sln files."""
    names = []
    if no_git:
        root_dir = root or "."
        base = os.path.join(root_dir, "src")
        for d, _, fs in os.walk(base):
            for f in fs:
                rel = os.path.relpath(os.path.join(d, f), root_dir).replace("\\", "/")
                names.append(rel)
        if ".sln" in exts and os.path.isdir(root_dir):
            for f in os.listdir(root_dir):
                if f.lower().endswith(".sln") and os.path.isfile(os.path.join(root_dir, f)):
                    names.append(f)
    else:
        try:
            names = git(["ls-tree", "-r", "--name-only", tree.rev] if tree.rev else ["ls-files"], root).splitlines()
        except RuntimeError:
            return {}
    out = {}
    if not no_git:
        tree.prefetch([n for n in names if n.lower().endswith(exts) and "/x64/" not in n and "/3rdparty/" not in n])
    for n in names:
        if n.lower().endswith(exts) and "/x64/" not in n and "/3rdparty/" not in n:
            try:
                if not no_git:
                    out[n] = tree.read(n)
                else:
                    with open(os.path.join(root or ".", *n.split("/")), encoding="utf-8", errors="replace") as fh:
                        out[n] = fh.read()
            except (OSError, RuntimeError):
                continue
    return out


def _build_macros(tree, root, no_git):
    """vp_footprint.build_include_macros over the tree's .props / .targets files (git mode; none under --no-git)."""
    if no_git:
        return {}
    try:
        names = git(["ls-tree", "-r", "--name-only", tree.rev] if tree.rev else ["ls-files"], root).splitlines()
    except RuntimeError:
        return {}
    texts = []
    for n in names:
        if n.lower().endswith((".props", ".targets")) and "/x64/" not in n and "/3rdparty/" not in n:
            try:
                texts.append(tree.read(n))
            except (OSError, RuntimeError):
                continue
    return vp_footprint.build_include_macros(texts)


def _add_tu(r, rel, include_lines, read, bill_tree, shared, macros=None):
    """G-CC1 (7): r["tu"] = VP lines compiled in this Bill file's TU through added FILE-scope includes; r["combined"] =
    h2 + tu (the relocation-proof count)."""
    tu, tu_files, errs = vp_footprint.tu_row(rel, include_lines, read, bill_tree, shared, macros=macros)
    r["tu"], r["tu_files"], r["combined"] = tu, tu_files, r["h2"] + tu
    r["errors"] = r["errors"] + errs


def expand_fragments(rel, text, fragments, read):
    """`text` with every class / function-scope VP fragment (vp_footprint.vp_included_lines records) expanded in place,
    recursively (nested VP-authored includes too). Returns (expanded text, linemap, fragment line set): linemap[k - 1] =
    (file, line, includer line) for expanded line k."""
    at = {f["line"]: f["fragment"] for f in fragments}
    out, linemap, frag_lines = [], [], set()

    def emit_file(path, body, top, seen):
        for j, ln in enumerate(body.splitlines(), 1):
            m = vp_footprint.INCLUDE_RE.match(ln)
            if m:
                p, t = vp_footprint.resolve_include(m.group(1), path, read)
                if p is not None and p.startswith(VPV_PREFIX) and p not in seen:
                    emit_file(p, t, top, seen | {p})
                    continue
            out.append(ln)
            linemap.append((path, j, top))
            frag_lines.add(len(out))

    for i, ln in enumerate(text.splitlines(), 1):
        if i in at:
            body = read(at[i])
            if body is not None:
                emit_file(at[i], body, i, {at[i]})
                continue
        out.append(ln)
        linemap.append((rel, i, i))
    return "\n".join(out) + "\n", linemap, frag_lines


def fragment_check(tree, h2):
    """Every VP file included at class or function scope must be registered in docs/audit/host-fragments.json
    ({fragment, includer, anchor_text}) and included exactly once; a registry entry nothing includes is stale.
    Returns the hits (exit 1)."""
    reg = load_json(tree, FRAGMENTS_PATH) or {"fragments": []}
    entries = reg.get("fragments", [])
    uses = {}
    for rel, r in sorted(h2.items()):
        # G-CC1 MUST-2: EVERY added #include at class or function scope, whatever it resolves to (a Bill header there
        # can itself include VP lines at its own file scope), must be registered
        for f in r.get("scoped", [{"target": x["fragment"], "line": x["line"], "scope": x["scope"]} for x in r["fragments"]]):
            uses.setdefault(f["target"], []).append((rel, f["line"], f["scope"]))
    hits = []
    for frag, us in sorted(uses.items()):
        where = ", ".join("%s:%d (%s scope)" % u for u in us)
        if len(us) > 1:
            hits.append("fragment %s included %d times at class/function scope (%s): a fragment is included exactly once" % (frag, len(us), where))
        mine = [e for e in entries if e.get("fragment") == frag]
        if not mine:
            hits.append("UNREGISTERED fragment %s included at %s: list it in %s ({fragment, includer, anchor_text}); its lines "
                        "count toward the includer's H2 either way" % (frag, where, FRAGMENTS_PATH))
            continue
        for e in mine:
            if e.get("includer") not in [u[0] for u in us]:
                hits.append("fragment %s is registered for includer %s but included by %s" % (frag, e.get("includer"), where))
                continue
            try:
                itext = tree.read(e["includer"])
            except (OSError, RuntimeError):
                itext = ""
            if not e.get("anchor_text") or _key(e["anchor_text"]) not in _key(itext):
                hits.append("fragment %s: anchor_text of its registry entry is not in %s" % (frag, e.get("includer")))
    for e in entries:
        if e.get("fragment") not in uses:
            hits.append("stale %s entry: %s is not included at class/function scope by any Bill file (remove the entry)" % (
                FRAGMENTS_PATH, e.get("fragment")))
    return hits


DEFINE_RE = re.compile(r"^\s*#\s*define\s+([A-Za-z_]\w*)(\()?(.*)$")


def vp_macro_defs(text):
    """{name: function_like} of the macros `text` #defines that can hide lines: every function-like macro, and an
    object-like macro whose replacement holds a `;`, `{` or `}` (declarations or statements; a value-only constant
    such as `#define VPV_MAX 4` expands to one expression and hides nothing). Continuation lines are joined."""
    out = {}
    lines = text.splitlines()
    i = 0
    while i < len(lines):
        ln = lines[i]
        while ln.rstrip().endswith("\\") and i + 1 < len(lines):
            i += 1
            ln = ln.rstrip()[:-1] + " " + lines[i]
        i += 1
        m = DEFINE_RE.match(ln)
        if not m:
            continue
        body = re.sub(r"//.*$|/\*.*?\*/", "", m.group(3))
        if m.group(2) or re.search(r"[;{}]", body):
            out[m.group(1)] = bool(m.group(2))
    return out


def vp_macros(tree, bill_tree=None):
    """The line-hiding macros (vp_macro_defs) #defined in every VP-authored code file: src/VP-V/, and with a Bill tree
    to compare, every file Bill's tree does not have (G-CC1 SHOULD-6). {name: function_like}."""
    rels = [f for f in tree.code_files() if vp_footprint.is_vp_authored(f, bill_tree)]
    tree.prefetch(rels)
    names = {}
    for rel in rels:
        try:
            names.update(vp_macro_defs(tree.read(rel)))
        except (OSError, RuntimeError):
            continue
    return names


def macro_check(tree, diff, h2, no_git, only, root=None, base_ref=None):
    """`macro-in-bill-scope`: an added line of a Bill-owned file that invokes, at class or function scope, a VP-authored
    macro that can hide lines (vp_macros: function-like, or object-like with a declaration / statement body), whose body
    would be VP lines the H2 count cannot see. A name the same file also uses on a line that is not added is Bill's own
    spelling and is skipped. Returns the hits (exit 1)."""
    cands = {}
    if no_git:
        for rel in only or []:
            try:
                text = tree.read(rel)
            except (OSError, RuntimeError):
                continue
            cands[rel] = {i: ln for i, ln in enumerate(text.splitlines(), 1)}
    else:
        for p, r in h2.items():
            if r["bucket"] != "docs" and p in diff:
                cands[p] = diff[p]["added_text"]
    if not cands:
        return []
    bill_tree = None
    if not no_git and root and base_ref:
        try:
            bill_tree = set(git(["ls-tree", "-r", "--name-only", base_ref], root).splitlines())
        except RuntimeError:
            bill_tree = None
    names = vp_macros(tree, bill_tree)
    if not names:
        return []
    fn = sorted((re.escape(n) for n, f in names.items() if f), key=len, reverse=True)
    obj = sorted((re.escape(n) for n, f in names.items() if not f), key=len, reverse=True)
    alts = ([r"\b(" + "|".join(fn) + r")\s*\("] if fn else []) + ([r"\b(" + "|".join(obj) + r")\b"] if obj else [])
    mre = re.compile("|".join("(?:%s)" % a for a in alts))
    hits = []
    for rel, added in sorted(cands.items()):
        if not any(mre.search(t) for t in added.values()):
            continue
        try:
            text = tree.read(rel)
        except (OSError, RuntimeError):
            continue
        scopes = vp_footprint.scope_map(text)
        code = strip_code(text).splitlines()
        # a macro this file #defines itself has its body in the file's own lines (counted by H1 when added)
        own = {n for n in vp_macro_defs(text) if n in names}
        for i, ln in enumerate(code, 1):
            if i not in added:
                own.update(m.group(m.lastindex) for m in mre.finditer(ln))
        for i in sorted(added):
            if i > len(code) or scopes[i - 1] == "file":
                continue
            used = [m.group(m.lastindex) for m in mre.finditer(code[i - 1]) if m.group(m.lastindex) not in own]
            if used:
                hits.append("macro-in-bill-scope %s:%d %s scope invokes VP-V macro %s: %s" % (
                    rel, i, scopes[i - 1], ", ".join(sorted(set(used))), text.splitlines()[i - 1].strip()[:100]))
    return hits


def load_h2_baseline(tree, no_git=False):
    """(baseline dict or None, error or None), read from the checked tree (the --rev tree with --rev). A baseline that
    is not version 2 (the retired narrow hook-line count) is refused: exit 2, re-baseline under a WAIVER commit. In git
    mode a MISSING baseline is refused too (G-CC1 SHOULD-1: deleting the file must never switch the gate off), and so is
    a v2 baseline without the `tu` / `combined` ceilings (G-CC1 (7))."""
    try:
        text = tree.read(HOOK_LINES_PATH)
    except (OSError, RuntimeError):
        if no_git:
            return None, None
        return None, ("%s is missing: no H2 verdict in git mode (G-CC1 SHOULD-1); restore it, or seed it with "
                      "--write-hook-lines-baseline --migrate WAIVER-<ID>" % HOOK_LINES_PATH)
    try:
        base = json.loads(text)
    except ValueError as e:
        return None, "%s is not valid JSON (%s): no verdict" % (HOOK_LINES_PATH, e)
    if base.get("version") != H2_METRIC_VERSION or base.get("metric") != "H2":
        return base, ("%s is a v1 (narrow tagged-line) baseline: the metric is now the honest H2 (CC-audit-1, rule 19); "
                      "re-baseline under a WAIVER commit (--write-hook-lines-baseline --migrate WAIVER-<ID>)" % HOOK_LINES_PATH)
    if not isinstance(base.get("tu"), dict) or not isinstance(base.get("combined"), dict):
        return base, ("%s has no tu / combined ceilings (G-CC1 (7)): seed them with --write-hook-lines-baseline --migrate "
                      "WAIVER-<ID>" % HOOK_LINES_PATH)
    if base.get("tu_measure") != TU_MEASURE_VERSION:
        return base, ("%s was seeded with tu measure %s, the checker measures tu %d (G-CC2 MUST-A / G-CC4 MUST-B: live "
                      "includes of the lexed text from VP-V .cpp files a .vcxproj really compiles, no Bill-member files): "
                      "re-seed tu / combined with --write-hook-lines-baseline --migrate WAIVER-<ID>, or on a union merge "
                      "--rebase-to <union tip> --waiver WAIVER-<ID>" % (HOOK_LINES_PATH, base.get("tu_measure", 1),
                                                                        TU_MEASURE_VERSION))
    return base, None


def _ceiling_check(name, measured, ceil, over, ratchet, extra=None):
    """Per-file `measured` against `ceil` (a file not listed has a ceiling of 0; `extra` = a second ceiling, the lower
    one holds). Appends to `over` / `ratchet`."""
    for f in sorted(set(measured) | set(ceil or {}) | set(extra or {})):
        n = measured.get(f, 0)
        lim = ceil.get(f, 0) if ceil is not None else None
        if extra and f in extra:
            lim = int(extra[f]) if lim is None else min(lim, int(extra[f]))
        if lim is None:
            continue
        if n > lim:
            over.append("%s %s %d > ceiling %d" % (f, name, n, lim))
        elif n < lim:
            ratchet.append("%s %s %d < ceiling %d (ratchet down)" % (f, name, n, lim))


def hook_lines_metric(tree, per_file, h2, added_by_file, no_git, root):
    """The gated Bill-line metric = honest H2 per Bill-owned file (h2_rows), production + tests + project, with the
    per-file ceilings and bucket totals of docs/audit/hook-lines-baseline.json (v2) and the optional `h2_ceiling` of
    docs/audit/hook-budget.json. `tagged` = the retired narrow count (information)."""
    tagged = {}
    for r in per_file:
        if r.get("bucket") != "production":
            continue
        try:
            hs = vp_footprint.hook_line_set(tree.read(r["file"]))
        except (OSError, RuntimeError, KeyError):
            continue
        n = len(hs) if no_git else len(hs & added_by_file.get(r["file"], set()))
        if n:
            tagged[r["file"]] = n
    gated = {f: r for f, r in h2.items() if r["bucket"] in H2_BUCKETS}
    files = {f: r["h2"] for f, r in sorted(gated.items()) if r["h2"]}
    tu = {f: r["tu"] for f, r in sorted(gated.items()) if r.get("tu")}
    combined = {f: r["h2"] + r["tu"] for f, r in sorted(gated.items()) if r.get("tu")}
    totals = {b: sum(r["h2"] for r in gated.values() if r["bucket"] == b) for b in H2_BUCKETS}
    total = sum(totals.values())
    renderer = sum(files.get(f, 0) for f in RENDERER_FILES)
    docs = {f: r["h2"] for f, r in sorted(h2.items()) if r["bucket"] == "docs" and r["h2"]}
    errors = [e for r in h2.values() for e in r["errors"]]
    base, base_error = load_h2_baseline(tree, no_git)
    budget = load_json(tree, BUDGET_PATH) or {}
    over, ratchet, tu_over = [], [], []
    extra = budget.get("h2_ceiling") or {}
    if base is None or base_error is not None:
        # G-CC1 SHOULD-1 / G-CC2 SHOULD-A: when the baseline is absent OR unusable (v1, no tu, an old tu measure: a
        # migration), the h2 ceilings still hold: the hook-budget h2_ceiling, and the per-file h2 ceilings of a v2 H2
        # baseline (the lower one), so a swapped-in v1 file plus --migrate never raises h2
        v2_files = base.get("files") if (isinstance(base, dict) and base.get("version") == H2_METRIC_VERSION and
                                         base.get("metric") == "H2" and isinstance(base.get("files"), dict)) else None
        if extra or v2_files is not None:
            _ceiling_check("h2", files, v2_files, over, ratchet if base is None else [], extra)
    if base is not None and base_error is None:
        _ceiling_check("h2", files, base.get("files", {}), over, ratchet, extra)
        # G-CC1 (7): tu per includer, and the combined h2 + tu per includer, are non-increasing too; growth passes
        # only by a RELOCATION waiver recorded in the baseline's _updates (--write-hook-lines-baseline --relocation)
        _ceiling_check("tu", tu, base["tu"], tu_over, ratchet)
        _ceiling_check("combined h2+tu", combined, base["combined"], tu_over, ratchet)
        for b in H2_BUCKETS:
            lim = (base.get("totals") or {}).get(b, 0)
            if totals[b] > lim:
                over.append("%s TOTAL h2 %d > baseline %d" % (b, totals[b], lim))
        if total > base.get("total", 0):
            over.append("TOTAL h2 %d > baseline %d" % (total, base.get("total", 0)))
    return {"metric": "H2", "files": files, "total": total, "totals": dict(totals, renderer=renderer),
            "h1": {f: r["h1"] for f, r in sorted(gated.items())},
            "included": {f: r["inc"] for f, r in sorted(gated.items()) if r["inc"]},
            "fragments": [dict(f, includer=rel) for rel, r in sorted(gated.items()) for f in r["fragments"]],
            "docs": docs, "tagged": tagged, "tagged_total": sum(tagged.values()),
            "tu": tu, "tu_total": sum(tu.values()), "combined": combined,
            "tu_files": {f: r["tu_files"] for f, r in sorted(gated.items()) if r.get("tu")},
            "baseline": None if base is None or base_error else base.get("total", 0),
            "baseline_totals": None if base is None or base_error else base.get("totals"),
            "baseline_error": base_error, "errors": errors, "over": over + tu_over, "tu_over": tu_over, "ratchet": ratchet}


def run(root, base_ref, only=None, no_git=False, fail_untagged=False, rev=None, since=None):
    tree = Tree(root, rev)
    if not no_git and bill_tip_check(root, base_ref):
        raise SystemExit("--base-ref %s has src/VP-V/ in its tree: it is one of OUR tips, not Bill's baseline (a591ad3b5). "
                         "To report only what a branch ADDED since a merge base, use --since <merge-base> (the Bill baseline stays "
                         "--base-ref)." % base_ref)
    vp_slots = vp_slots_from(tree)
    gated = gated_markers(tree, tree.code_files())
    allow_used = {}
    files = bill_files(tree, base_ref, only, no_git)
    added_by_file = {}
    diff = {}
    diff_error = None
    if not no_git:
        try:
            diff = vp_footprint.parse_diff(git(["diff"] + vp_footprint.DIFF_ARGS + ["-U0", base_ref] + ([rev] if rev else []) +
                                               ["--", "src", ":(exclude)src/VP-V"], root))
            added_by_file = {k: set(v["added_lines"]) for k, v in diff.items()}
        except RuntimeError as e:
            # fail CLOSED: without the diff the H2 metric would read 0
            diff_error = "git diff %s cannot be read (%s): no H2 verdict" % (base_ref, " ".join(str(e).split())[:160])
    h2 = h2_rows(tree, root, base_ref, diff, no_git, only) if diff_error is None else {}
    per_file = []
    tree.prefetch([rel for rel, _ in files])
    # S2: the names each changed file uses at Bill's base, so `vp-function` means a function ABSENT at the base ref
    base_tree = Tree(root, base_ref) if (not no_git and added_by_file) else None
    if base_tree is not None:
        base_tree.prefetch([rel for rel, _ in files if added_by_file.get(rel)])
    for rel, bucket in files:
        idents = NO_BASE
        if base_tree is not None:
            try:
                idents = base_identifiers(base_tree.read(rel))
            except (OSError, RuntimeError):
                idents = None   # absent at the base ref: every function in it is VP-V's
        r = check_file(tree, rel, vp_slots, gated, allow_used, base_ref, no_git, added_by_file.get(rel),
                       idents, h2[rel]["fragments"] if rel in h2 else None)
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
            elif s["class"] in ("gated-marker-unverified", "gated-arg-unverified"):
                hits.append(s)   # the call site of an unverified marker, or one passing a non-predicate argument (S1)
            elif s["class"] == "allowed":
                allowed.append(s)
        if r["bucket"] == "production":
            untagged.extend(r["untagged"])
    for name, defs in gated.items():
        for rel, line, verified in defs:   # every definition, also of a name defined in several files
            if not verified:
                unverified.append({"file": rel, "line": line, "class": "gated-marker-unverified", "text": name})
    texts = [s["text"] for r in per_file for s in r["statements"] if r["bucket"] == "production"]
    for k, e in enumerate(ALLOW_CALLEES):
        if ("callee", k) not in allow_used and any(re.search(r"\b(?:" + e["callee"] + r")\s*\(", t) for t in texts):
            allow_used[("callee", k)] = 0   # reached through an enclosing header
    stale = [("callee", k) for k in range(len(ALLOW_CALLEES)) if ("callee", k) not in allow_used] + \
            [("site", k) for k in range(len(ALLOW_SITES)) if ("site", k) not in allow_used]
    if not (since and not no_git):   # symbol-keyed entries are used only by the --since symbol report
        stale = [x for x in stale if not (x[0] == "site" and ALLOW_SITES[x[1]].get("symbol"))]
    sc = site_checks(tree, per_file, root, rev, no_git)
    since_added = None
    untagged_vpv = []
    if since and not no_git:
        try:
            d = vp_footprint.parse_diff(git(["diff"] + vp_footprint.DIFF_ARGS + ["-U0", since] + ([rev] if rev else []) +
                                            ["--", "src", ":(exclude)src/VP-V"], root))
            since_added = {k: set(v["added_lines"]) for k, v in d.items()}
        except RuntimeError as e:
            # fail CLOSED: an unreadable --since (typo, unfetched sha) must never report "0 new" and exit 0
            print("check_hook_gating: --since %s cannot be read in %s (%s); no verdict" % (
                since, root, " ".join(str(e).split())[:160]), file=sys.stderr)
            raise SystemExit(2)
        def _new(s):
            return s["line"] in since_added.get(s["file"], set())
        def _new_site(s):
            return any(n in since_added.get(s["file"], set()) for n in range(s["start"], s["end"] + 1))
        # A VPV-GATED marker lives in src/VP-V (outside since_added) and its body can lose the early return
        # without any Bill line changing: unverified markers are a whole-tree invariant (0 at a green base), so
        # they and their call sites are kept under --since (HOOKGATE-CHECKER-HOLES (1)). A marker that starts testing
        # a parameter re-judges its old call sites without a Bill line changing, so S1 argument hits are kept too.
        hits = [h for h in hits if _new(h) or h["class"] in ("gated-marker-unverified", "gated-arg-unverified")]
        untagged = [u for u in untagged if _new(u)]
        untagged_vpv = untagged_vp_references(tree, per_file, since_added, root, base_ref, allow_used)
        stale = [x for x in stale if x not in allow_used]
        sc["new"] = [x for x in sc["new"] if _new_site(x)]
        sc["approved"] = [x for x in sc["approved"] if _new_site(x)]
    hl = hook_lines_metric(tree, per_file, h2, added_by_file, no_git, root)
    if diff_error:
        hl["errors"].append(diff_error)
    hl["fragment_hits"] = fragment_check(tree, h2)
    hl["macro_hits"] = macro_check(tree, diff, h2, no_git, only, root, base_ref)
    sc["hook_lines"] = hl
    # G-CC1 SHOULD-7: measure = ceiling; a ceiling above the measured value (ratchet down) is slack a later commit
    # could refill unnoticed, so it fails until the WAIVER ratchet commit lowers it
    rc = 1 if (hl["over"] or hl["ratchet"] or hl["fragment_hits"] or hl["macro_hits"] or hits or unverified or
               (fail_untagged and untagged) or sc["new"] or sc["over"] or sc["expired"] or sc["unknown"] or
               sc["trigger_errors"] or sc["approval_missing"] or sc["budget_errors"] or sc["approval_over"] or
               untagged_vpv) else 0
    if hl["errors"] or hl["baseline_error"]:
        rc = 2   # fail closed: an unresolved class / function-scope include or a retired (v1) baseline gives no verdict
    return {"base_ref": base_ref, "rev": rev, "since": since, "vp_slots": sorted(vp_slots), "site_checks": sc,
            "gated": {k: [list(d) for d in v] for k, v in gated.items()},
            "files": per_file, "counts": counts, "hits": hits, "unverified": unverified, "untagged": untagged, "untagged_vpv": untagged_vpv,
            "allowed": allowed, "allow_used": {"%s:%d" % k: v for k, v in allow_used.items()}, "stale_allow": stale, "rc": rc}


def render(res):
    out = ["hook gating: base %s, tree %s%s" % (res["base_ref"], res.get("rev") or "working tree",
                                                 (", only lines added since %s" % res["since"]) if res.get("since") else "")]
    c = res["counts"]
    hook_stmts = sum(v for k, v in c.items() if k != "declaration")
    for s in res["hits"]:
        tail = ("   [gated-arg-unverified: %s is not passed a pathway predicate]" % s.get("why")
                if s["class"] == "gated-arg-unverified" else "")
        out.append("  UNGUARDED %s:%d %s%s" % (s["file"], s["line"], s["text"], tail))
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
    if res.get("since"):
        for k, e in enumerate(ALLOW_UNTAGGED_REFS):
            out.append("  allow-list untagged ref [%d] %s <%s>: %s" % (res["allow_used"].get("untag:%d" % k, 0), e["file"], e["text"].strip()[:70], e["reason"]))
    for kind, k in res["stale_allow"]:
        out.append("  stale allow-list entry (no site uses it, may be deleted): %s %d" % (kind, k))
    for name, defs in sorted(res["gated"].items()):
        for rel, line, verified in defs:
            out.append("  VPV-GATED %s (%s:%d) %s" % (name, rel, line, "verified" if verified else "NOT VERIFIED"))
    out.append("  classes (every bucket incl. tests, whole files, before the --since filter; not the gate): " +
               ", ".join("%s %d" % (k, v) for k, v in sorted(c.items())))
    out.append("  gate: %d unguarded (production files%s, incl. unverified-marker call sites), %d unverified markers" % (
        len(res["hits"]), (", lines added since %s" % res["since"]) if res.get("since") else "", len(res["unverified"])))
    sc = res["site_checks"]
    for s in sc["new"]:
        out.append("  NEW SITE (not in %s, no HOOK-APPROVAL) %s:%d-%d [%s] %s" % (MANIFEST_PATH, s["file"], s["start"], s["end"], s["gate"], s["text"][:100]))
    for s in sc["approved"]:
        out.append("  new site approved by %s %s:%d-%d %s" % (s["approval"], s["file"], s["start"], s["end"], s["text"][:80]))
    for line in sc["over"]:
        out.append("  OVER BUDGET " + line)
    for line in sc["ratchet"]:
        out.append("  " + line)
    for line in sc.get("headroom", []):
        out.append("  " + line)
    for line in sc["expired"]:
        out.append("  " + line)
    for line in sc["unknown"]:
        out.append("  UNKNOWN TRIGGER " + line)
    for line in sc["trigger_errors"] + sc["approval_missing"] + sc["budget_errors"] + sc.get("approval_over", []):
        out.append("  " + line)
    for x in sc["temporary"]:
        out.append("  temporary ceiling %s %d (reverts to %d when %s land)" % (x["file"], x["limit"], x["reverts_to"], ", ".join(x["pending"])))
    if sc["removed"]:
        out.append("  removed sites (ids no longer present; regenerate the manifest with --write-manifest): " + ", ".join(sc["removed"][:20]) + (" ..." if len(sc["removed"]) > 20 else ""))
    temp = "".join("; temporary %s %d <= %d (reverts to %d)" % (x["file"].rsplit("/", 1)[-1], x["count"], x["limit"], x["reverts_to"])
                   if x["count"] <= x["limit"] else "; temporary %s %d > %d" % (x["file"].rsplit("/", 1)[-1], x["count"], x["limit"])
                   for x in sc["temporary"])
    out.append("hook sites: %d sites, %d new (%d approved), %d removed, %d over budget; manifest %s, budget %s; %d expired, %d unknown triggers, "
               "%d trigger errors, %d approval files missing, %d budget errors%s; trigger source: %s" % (
        sc["sites"], len(sc["new"]) + len(sc["approved"]), len(sc["approved"]), len(sc["removed"]), len(sc["over"]), sc["manifest"], sc["budget"],
        len(sc["expired"]), len(sc["unknown"]), len(sc["trigger_errors"]), len(sc["approval_missing"]), len(sc["budget_errors"]), temp,
        sc.get("trigger_source", "json only")))
    hl = sc.get("hook_lines", {"files": {}, "total": 0, "totals": {}, "baseline": None, "over": [], "ratchet": [], "errors": [],
                                "baseline_error": None, "fragment_hits": [], "macro_hits": [], "fragments": [], "docs": {},
                                "tagged_total": 0, "h1": {}, "included": {}})
    t = hl.get("totals", {})
    split = "production %d, tests %d, project %d; renderer %d; tagged (old narrow count, information) %d" % (
        t.get("production", 0), t.get("tests", 0), t.get("project", 0), t.get("renderer", 0), hl.get("tagged_total", 0))
    if hl.get("baseline_error"):
        out.append("hook LINES (honest H2): %d (%s); %s" % (hl["total"], split, hl["baseline_error"]))
    elif hl["baseline"] is None:
        out.append("hook LINES (honest H2): %d (no baseline at %s; run --write-hook-lines-baseline); %s" % (hl["total"], HOOK_LINES_PATH, split))
    else:
        out.append("hook LINES (honest H2): %d (baseline %d, trend %+d); %s" % (hl["total"], hl["baseline"], hl["total"] - hl["baseline"], split))
    for f, n in sorted(hl["files"].items(), key=lambda x: -x[1])[:12]:
        inc = hl.get("included", {}).get(f, 0)
        out.append("  h2 %s %d%s" % (f, n, (" (h1 %d + %d included at class/function scope)" % (hl["h1"].get(f, 0), inc)) if inc else ""))
    for x in hl.get("fragments", []):
        out.append("  VP fragment at %s scope %s:%d <- %s (%d lines counted)" % (x["scope"], x["includer"], x["line"], x["fragment"], x["lines"]))
    out.append("tu (VP lines compiled in Bill TUs through added file-scope includes, shared VP-V headers excluded): %d; "
               "combined h2+tu per includer: %s" % (hl.get("tu_total", 0), ", ".join(
                   "%s %d (h2 %d + tu %d)" % (f.rsplit("/", 1)[-1], c, hl["files"].get(f, 0), hl["tu"][f])
                   for f, c in sorted(hl.get("combined", {}).items())) or "none"))
    if hl.get("docs"):
        out.append("  Bill documentation lines (information, not gated): " + ", ".join("%s %d" % kv for kv in sorted(hl["docs"].items())))
    for line in hl.get("ratchet", [])[:12]:
        out.append("  " + line)
    for line in hl["over"]:
        out.append("  OVER HOOK-LINES BASELINE " + line)
    for line in hl.get("fragment_hits", []):
        out.append("  FRAGMENT " + line)
    for line in hl.get("macro_hits", []):
        out.append("  " + line)
    for line in hl.get("errors", []):
        out.append("  UNRESOLVED (fail closed) " + line)
    if hl["over"]:
        out.append("FAIL (hook lines): the honest H2 of a Bill file or bucket grew past the baseline. Every VP line in a Bill file "
                   "counts, tagged or not, and so does every VP line included at class or function scope: move logic into VP-V "
                   "compiled files behind a one-line call. The baseline only shrinks.")
    if hl.get("tu_over"):
        out.append("FAIL (tu): VP lines compiled in a Bill TU through a file-scope include grew, or the combined h2 + tu did. "
                   "Moving a body out of a Bill class into a file-scope VP file is a RELOCATION, not a reduction: it passes only "
                   "under a named RELOCATION waiver (--write-hook-lines-baseline --relocation WAIVER-<ID>, recorded in _updates).")
    if hl.get("ratchet"):
        out.append("FAIL (ratchet): a measured value is below its ceiling (measure = ceiling, G-CC1 SHOULD-7): lower the "
                   "ceilings with --write-hook-lines-baseline in the WAIVER ratchet commit that follows.")
    if hl.get("fragment_hits") or hl.get("macro_hits"):
        out.append("FAIL (fragments): a VP file included at class or function scope must be registered in %s and included "
                   "exactly once; a VP-V function-like macro must not be invoked in a Bill class or function scope." % FRAGMENTS_PATH)
    if hl.get("errors") or hl.get("baseline_error"):
        out.append("NO VERDICT (exit 2): an unresolved include at class or function scope, an unreadable diff, or a retired v1 "
                   "hook-lines baseline.")
    out.append("hook gating: %d hook statements, %d unguarded, %d unverified markers, %d allowed, %d untagged; exit %d" % (
        hook_stmts, len(res["hits"]), len(res["unverified"]), len(res["allowed"]), len(res["untagged"]), res["rc"]))
    if sc["expired"] or sc["unknown"]:
        out.append("FAIL (rule 32 expiry): an approved transitional site outlived its trigger, or an expiry names a trigger "
                   "%s does not list: collapse the site into its seam (refs/107-host-seams-plan.md s4) or fix the id." % TRIGGERS_PATH)
    if sc["trigger_errors"] or sc["approval_missing"] or sc["budget_errors"]:
        out.append("FAIL (rule 32 expiry inputs): the trigger source, an approval's file or the budget cannot be trusted; a missed "
                   "trigger never passes silently. Fix the trailer/id, the approval's file (renamed or moved?), or the budget.")
    if sc.get("approval_over"):
        out.append("FAIL (rule 32 approvals): one HOOK-APPROVAL row admits more new sites than its max_sites (default 1); each "
                   "further site needs its own approval id from the coordinator, or an explicit max_sites on the row.")
    if sc["new"] or sc["over"]:
        out.append("FAIL (rule 32): a NEW hook site in a Bill file, or a file over its site ceiling: route the feature through an "
                   "existing seam (refs/107-host-seams-plan.md s1), or get a HOOK-APPROVAL id from the coordinator "
                   "(docs/audit/hook-approvals.json); ceilings only go down.")
    if res["rc"] and not (sc["new"] or sc["over"] or sc["expired"] or sc["unknown"] or sc["trigger_errors"] or sc["approval_missing"] or
                          sc["budget_errors"] or sc.get("approval_over") or res.get("untagged_vpv") or hl["over"] or
                          hl.get("ratchet") or hl.get("fragment_hits") or hl.get("macro_hits") or hl.get("errors") or
                          hl.get("baseline_error")) or res["hits"] or res["unverified"]:
        out.append("FAIL: a VP-V hook in a Bill file runs in the Original pathway. Wrap the call site in the canonical guard "
                   "(`if (!pipelineSlots.OriginalPathway())`), or move the logic into its VP-V module behind a `// VPV-GATED` "
                   "function with `if (originalPathway) return;`, or ask the lead for an allow-list entry with a reason.")
    if res.get("since"):
        uv = res.get("untagged_vpv", [])
        for u in uv:
            out.append("  untagged VP-V reference %s:%d %s   [%s]" % (u["file"], u["line"], u["text"], ", ".join(u["symbols"])))
        out.append("%d new untagged VP-V references in Bill files (lines added since %s)" % (len(uv), res["since"]))
        if uv:
            out.append("FAIL (rule 32): an added line in a Bill-owned production file names a symbol declared under src/VP-V/ and "
                       "carries no VPV-HOOK tag: a per-feature call site must not be added (route it through a seam, "
                       "refs/107-host-seams-plan.md), and an untagged one is never a way around the gate.")
    return "\n".join(out)


def selftest():
    """The checker's H2 selftests (CC-audit-1 T3, T9, T11, T12; vp_footprint --selftest runs T1, T2, T4-T8, T10, T13)."""
    import contextlib
    import io
    import tempfile
    vp_footprint.h2_selftest()
    bill = "src/VideoProcessor-Lib/vprenderer/Renderer.cpp"
    slots = "{ Id::MotionEstimation, \"m\", Side::Input, 50, HookPoint::Prepass, Status::Built, Origin::Vp },\n"

    def w(root, rel, text):
        p = os.path.join(root, *rel.split("/"))
        os.makedirs(os.path.dirname(p), exist_ok=True)
        with open(p, "w", encoding="utf-8") as fh:
            fh.write(text)

    def cli(root, *extra):
        buf = io.StringIO()
        with contextlib.redirect_stdout(buf):
            rc = main(["--root", root, "--no-git", "--files", bill] + list(extra))
        return rc, buf.getvalue()

    def bill_fn(body):
        return "struct Impl\n{\n\tPipelineSlots::Registry pipelineSlots;\n\tvoid Render()\n\t{\n" + body + "\t}\n};\n"

    reg = {"fragments": [{"fragment": "src/VP-V/host/Frag.inl", "includer": bill, "anchor_text": "void Render()"}]}
    with tempfile.TemporaryDirectory() as root:
        w(root, SLOTS_HEADER, slots)
        # T3: a fragment included inside a function body: counted, and its unguarded VP call is a hit at the include line
        w(root, "src/VP-V/host/Frag.inl", "\tVpMotion::Run(pipelineSlots);\n")
        w(root, bill, bill_fn("\t\tdoBill();\n#include <VP-V/host/Frag.inl>   // VPV-HOOK\n"))
        w(root, FRAGMENTS_PATH, json.dumps(reg))
        res = run(root, DEFAULT_BASE, [bill], no_git=True)
        hl = res["site_checks"]["hook_lines"]
        assert hl["files"][bill] == 9 + 1, hl["files"]
        assert hl["fragments"] and hl["fragments"][0]["scope"] == "function", hl["fragments"]
        assert [(h["line"], h.get("via")) for h in res["hits"]] == [(7, "src/VP-V/host/Frag.inl:1")], res["hits"]
        assert not hl["fragment_hits"], hl["fragment_hits"]
        # ... and the same call guarded inside the fragment passes
        w(root, "src/VP-V/host/Frag.inl", "\tif (!pipelineSlots.OriginalPathway())\n\t\tVpMotion::Run(pipelineSlots);\n")
        assert run(root, DEFAULT_BASE, [bill], no_git=True)["hits"] == []
        # T12: an unregistered fragment, a second include, a stale entry: hits (exit 1)
        os.remove(os.path.join(root, *FRAGMENTS_PATH.split("/")))
        rc, out = cli(root)
        assert rc == 1 and "UNREGISTERED fragment src/VP-V/host/Frag.inl" in out, out
        w(root, FRAGMENTS_PATH, json.dumps(reg))
        w(root, bill, bill_fn("#include <VP-V/host/Frag.inl>   // VPV-HOOK\n#include <VP-V/host/Frag.inl>   // VPV-HOOK\n"))
        rc, out = cli(root)
        assert rc == 1 and "included 2 times" in out, out
        w(root, bill, bill_fn("\t\tdoBill();\n"))
        rc, out = cli(root)
        assert rc == 1 and "stale" in out, out
        # T12 (G-CC1 SHOULD-4): an entry registered for another includer, and one without anchor_text, are hits
        w(root, bill, bill_fn("\t\tdoBill();\n#include <VP-V/host/Frag.inl>   // VPV-HOOK\n"))
        w(root, FRAGMENTS_PATH, json.dumps({"fragments": [dict(reg["fragments"][0], includer="src/Other.cpp")]}))
        rc, out = cli(root)
        assert rc == 1 and "registered for includer src/Other.cpp" in out, out
        w(root, FRAGMENTS_PATH, json.dumps({"fragments": [dict(reg["fragments"][0], anchor_text="")]}))
        rc, out = cli(root)
        assert rc == 1 and "anchor_text" in out, out
        w(root, FRAGMENTS_PATH, json.dumps(reg))
        assert not run(root, DEFAULT_BASE, [bill], no_git=True)["site_checks"]["hook_lines"]["fragment_hits"]
        os.remove(os.path.join(root, *FRAGMENTS_PATH.split("/")))
        # G-CC1 MUST-2: a BILL header included at class or function scope is a hit unless registered (it can carry VP
        # lines through its own file-scope includes)
        w(root, "src/VideoProcessor-Lib/vprenderer/BillHdr.h", "#include <VP-V/host/Frag.inl>\n")
        w(root, bill, bill_fn("#include \"BillHdr.h\"\n"))
        rc, out = cli(root)
        assert rc == 1 and "UNREGISTERED fragment src/VideoProcessor-Lib/vprenderer/BillHdr.h" in out, out
        # G-CC1 MUST-1: a computed include at class or function scope gives no verdict (exit 2)
        w(root, bill, bill_fn("#include VPV_FRAG\n"))
        rc, out = cli(root)
        assert rc == 2 and "computed or unparsable" in out, out
        # T9: a VP-V function-like macro invoked at class or function scope is a hit; at file scope it is not
        w(root, "src/VP-V/core/Macros.h", "#define VPV_STAMP(x) do { Vpv::Stamp(x); } while (0)\n"
                                          "#define VPV_MEMBERS \\\n\tint a; \\\n\tint b;\n#define VPV_LIMIT 4\n")
        w(root, bill, bill_fn("\t\tVPV_STAMP(1);   // VPV-HOOK\n"))
        rc, out = cli(root)
        assert rc == 1 and "macro-in-bill-scope" in out and "VPV_STAMP" in out, out
        w(root, bill, "VPV_STAMP(0)\n" + bill_fn("\t\tdoBill();\n"))
        assert "macro-in-bill-scope" not in cli(root)[1]
        # G-CC1 SHOULD-6: an object-like macro with a declaration body is a hit; a value-only constant is not
        w(root, bill, "struct Impl\n{\n\tVPV_MEMBERS\n\tint v = VPV_LIMIT;\n};\n")
        rc, out = cli(root)
        assert rc == 1 and "VPV_MEMBERS" in out and "VPV_LIMIT" not in out, out
        os.remove(os.path.join(root, "src", "VP-V", "core", "Macros.h"))
        # an unresolved include at class / function scope: no verdict (exit 2)
        w(root, bill, bill_fn("#include <VP-V/host/Missing.inl>   // VPV-HOOK\n"))
        rc, out = cli(root)
        assert rc == 2 and "UNRESOLVED" in out, out
        # T11: a v1 baseline -> exit 2; --write-hook-lines-baseline migrates it to v2; growth then fails (exit 1)
        w(root, bill, bill_fn("\t\tdoBill();\n"))
        w(root, HOOK_LINES_PATH, json.dumps({"base_ref": DEFAULT_BASE, "total": 1, "files": {bill: 1}}))
        rc, out = cli(root)
        assert rc == 2 and "v1" in out, out
        # G-CC1 SHOULD-2: the migration needs an explicit WAIVER id, recorded in _updates
        rc, out = cli(root, "--write-hook-lines-baseline")
        assert rc == 1 and "--migrate" in out, out
        rc, out = cli(root, "--write-hook-lines-baseline", "--migrate", "WAIVER-T11")
        assert rc == 0 and "migrating" in out, out
        base = vp_footprint.read_json(os.path.join(root, *HOOK_LINES_PATH.split("/")))
        assert (base["version"], base["metric"], base["files"][bill]) == (2, "H2", 8), base
        assert base["tu"] == {} and base["combined"] == {} and "WAIVER-T11" in base["_updates"][0], base
        assert cli(root)[0] == 0
        w(root, bill, bill_fn("\t\tdoBill();\n\t\tmore();\n"))
        rc, out = cli(root)
        assert rc == 1 and "OVER HOOK-LINES BASELINE" in out, out
        assert cli(root, "--write-hook-lines-baseline")[0] == 1, "the baseline only shrinks"
        assert cli(root, "--write-hook-lines-baseline", "--relocation", "WAIVER-T")[0] == 1, "h2 growth is never a relocation"
        # G-CC1 SHOULD-7: a value below its ceiling fails (measure = ceiling) until the ratchet commit lowers it
        w(root, bill, "struct Impl\n{\n\tPipelineSlots::Registry pipelineSlots;\n};\n")
        rc, out = cli(root)
        assert rc == 1 and "ratchet down" in out and "FAIL (ratchet)" in out, out
        assert cli(root, "--write-hook-lines-baseline")[0] == 0 and cli(root)[0] == 0
        # G-CC1 (7), the T8 analogue end to end: a body moved to a FILE-scope VP file is a relocation (h2 -n, tu +n):
        # it fails until a named RELOCATION waiver raises tu / combined, recorded in _updates
        os.remove(os.path.join(root, *HOOK_LINES_PATH.split("/")))
        w(root, bill, "struct Impl\n{\n\tPipelineSlots::Registry pipelineSlots;\n" +
          "".join("\tvoid f%d() {}\n" % i for i in range(10)) + "};\n")
        assert cli(root, "--write-hook-lines-baseline")[0] == 1, "a missing baseline is a migration too"
        assert cli(root, "--write-hook-lines-baseline", "--migrate", "WAIVER-T8")[0] == 0
        w(root, "src/VP-V/host/Body.h", "".join("void Impl::f%d() {}\n" % i for i in range(10)))
        w(root, bill, "struct Impl\n{\n\tPipelineSlots::Registry pipelineSlots;\n};\n#include <VP-V/host/Body.h>\n")
        rc, out = cli(root)
        assert rc == 1 and "FAIL (tu)" in out and "combined h2+tu 15 > ceiling 0" in out, out
        assert cli(root, "--write-hook-lines-baseline")[0] == 1, "a tu raise needs --relocation"
        # G-CC2 SHOULD-B: a relocation needs a stated allowance and stays inside it: h2 14 -> 5, tu 0 -> 10, combined
        # 14 -> 15 (+1, the include line): allowance 0 is refused, 1 passes
        rc, out = cli(root, "--write-hook-lines-baseline", "--relocation", "WAIVER-T-RELOC")
        assert rc == 1 and "--allowance" in out, out
        rc, out = cli(root, "--write-hook-lines-baseline", "--relocation", "WAIVER-T-RELOC", "--allowance", "0")
        assert rc == 1 and "beyond its allowance 0" in out and "combined +1" in out, out
        rc, out = cli(root, "--write-hook-lines-baseline", "--relocation", "WAIVER-T-RELOC", "--allowance", "1")
        assert rc == 0, out
        base = vp_footprint.read_json(os.path.join(root, *HOOK_LINES_PATH.split("/")))
        assert base["tu"] == {bill: 10} and base["combined"] == {bill: 15} and "RELOCATION allowance 1" in base["_updates"][0], base
        assert base["tu_measure"] == TU_MEASURE_VERSION, base
        assert cli(root)[0] == 0
        # MUTATION (G-CC2 SHOULD-B): new VP code written straight into the file-scope header (no h2 taken away) is
        # not a relocation, whatever the allowance
        w(root, "src/VP-V/host/Body.h", "".join("void Impl::f%d() {}\n" % i for i in range(30)))
        rc, out = cli(root, "--write-hook-lines-baseline", "--relocation", "WAIVER-T-GROW", "--allowance", "2")
        assert rc == 1 and "beyond its allowance 2" in out and "tu +20" in out, out
        rc, out = cli(root, "--write-hook-lines-baseline", "--relocation", "WAIVER-T-GROW", "--allowance", str(RELOCATION_MAX_ALLOWANCE + 1))
        assert rc == 1 and "--allowance" in out, out
        assert RELOCATION_MAX_ALLOWANCE <= 8, "G-CC4 SHOULD-D: a small cap"
        # MUTATION (G-CC4 SHOULD-D): one new body (tu +1, combined +1, h2 +0) inside allowance 1 passed as a
        # "relocation" before 1d; with no h2 drop it is refused
        w(root, "src/VP-V/host/Body.h", "".join("void Impl::f%d() {}\n" % i for i in range(11)))
        rc, out = cli(root, "--write-hook-lines-baseline", "--relocation", "WAIVER-T-GROW", "--allowance", "1")
        assert rc == 1 and "real h2 drop" in out and "tu +1, h2 +0" in out, out
        w(root, "src/VP-V/host/Body.h", "".join("void Impl::f%d() {}\n" % i for i in range(10)))
        assert cli(root)[0] == 0
        # MUTATION (G-CC2 MUST-A): a dummy VP-V .cpp with the include switched off (`#if 0`) does not make the moved
        # body "shared": tu stays 10 (before the fix: tu 0 < ceiling 10)
        w(root, "src/VP-V/host/Body.cpp", "#if 0\n#include \"Body.h\"\n#endif\n")
        rc, out = cli(root)
        assert rc == 0 and "< ceiling 10" not in out, out
        # ... nor `#ifdef` a macro nobody defines, nor a live include in a .cpp no build file lists
        w(root, "src/VP-V/host/Body.cpp", "#ifdef VPV_NEVER\n#include \"Body.h\"\n#endif\n")
        assert "< ceiling 10" not in cli(root)[1]
        w(root, "src/VP-V/host/Body.cpp", "#include \"Body.h\"\n")
        rc, out = cli(root)
        assert rc == 0 and "< ceiling 10" not in out, out
        # ... nor a live include from a build-listed .cpp: Body.h defines members of Bill's Impl, never shared
        w(root, "src/VP-V/build/T.items.props", '<Project><ItemGroup><ClCompile Include="$(MSBuildThisFileDirectory)..\\host\\Body.cpp" />'
                                                 '</ItemGroup></Project>\n')
        # G-CC4 MUST-B 3: the .props counts only through a project that imports it
        w(root, "src/T/T.vcxproj", '<Project><Import Project="$(MSBuildThisFileDirectory)..\\VP-V\\build\\T.items.props" /></Project>\n')
        rc, out = cli(root)
        assert rc == 0 and "< ceiling 10" not in out, out
        # a genuine interface header that a build-listed VP-V .cpp includes live is shared (not tu)
        w(root, "src/VP-V/host/Api.h", "struct Api { int a; };\n")
        w(root, "src/VP-V/host/Body.cpp", "#include \"Api.h\"\n")
        w(root, bill, "struct Impl\n{\n\tPipelineSlots::Registry pipelineSlots;\n};\n#include <VP-V/host/Body.h>\n"
                      "#include <VP-V/host/Api.h>\n")
        res = run(root, DEFAULT_BASE, [bill], no_git=True)["site_checks"]["hook_lines"]
        assert res["tu"] == {bill: 10}, res["tu"]
        # MUTATIONS (G-CC4 MUST-B, the gate-4 gamed rows end to end): each made a moved body "shared" (tu 0) before 1d
        tu_now = lambda: run(root, DEFAULT_BASE, [bill], no_git=True)["site_checks"]["hook_lines"]["tu"].get(bill)
        w(root, "src/VP-V/host/Free.h", "".join("void free%d();\n" % i for i in range(10)))
        moved_bill = ("struct Impl\n{\n\tPipelineSlots::Registry pipelineSlots;\n};\n#include <VP-V/host/Body.h>\n"
                      "#include <VP-V/host/Api.h>\n#include <VP-V/host/Free.h>\n")
        w(root, bill, moved_bill)
        assert tu_now() == 20, "control: Free.h is not shared"
        w(root, "src/VP-V/host/Body.cpp", "#include \"Api.h\"\n#include \"Free.h\"\n")
        assert tu_now() == 10, "control: a live include of a free-function header from a compiled .cpp is shared"
        for cpp in ("/*\n#include \"Free.h\"\n*/\n", "// x \\\n#include \"Free.h\"\n",
                    "const char* s = R\"(\n#include \"Free.h\"\n)\";\n",
                    "#ifndef _WIN32\n#define _WIN32\n#include \"Free.h\"\n#endif\n",
                    "#ifndef BODY_CPP\n#define BODY_CPP\n#include \"Free.h\"\n#endif\n"):   # a guard in a .cpp is no guard
            w(root, "src/VP-V/host/Body.cpp", "#include \"Api.h\"\n" + cpp)
            assert tu_now() == 20, cpp
        w(root, "src/VP-V/host/Body.cpp", "#include \"Api.h\"\n#include \"Free.h\"\n")
        props = lambda body: w(root, "src/VP-V/build/T.items.props", "<Project>" + body + "</Project>\n")
        item = '<ClCompile Include="$(MSBuildThisFileDirectory)..\\host\\Body.cpp"%s'
        for body in ('<ItemGroup Condition="false">' + item % " />" + "</ItemGroup>",
                     '<ItemGroup>' + item % ' Condition="false" />' + "</ItemGroup>",
                     '<ItemGroup>' + item % "><ExcludedFromBuild>true</ExcludedFromBuild></ClCompile>" + "</ItemGroup>"):
            props(body)
            assert tu_now() == 21, body   # Api.h is no longer shared either
        props('<ItemGroup>' + item % " />" + "</ItemGroup>")
        os.remove(os.path.join(root, "src", "T", "T.vcxproj"))
        assert tu_now() == 21, "a stray .props that no project imports compiles nothing"
        w(root, "src/T/T.vcxproj", '<Project><Import Project="$(MSBuildThisFileDirectory)..\\VP-V\\build\\T.items.props" /></Project>\n')
        assert tu_now() == 10
        # decoys of the Bill class name and macro qualifiers never make Bill-member bodies shared
        w(root, "src/VP-V/host/Body.cpp", "#include \"Api.h\"\n#include \"Free.h\"\n#include \"Body.h\"\n")
        impl10 = "".join("void Impl::f%d() {}\n" % i for i in range(10))
        for decoy, body in (("namespace Decoy {\nstruct Impl {};\n}\n", impl10), ("#if 0\nstruct Impl {};\n#endif\n", impl10),
                            ("/*\nstruct Impl {};\n*/\n", impl10), ("struct Impl\n;\n", impl10),
                            ("#define VPV_I Impl\n", impl10.replace("Impl::", "VPV_I::"))):
            w(root, "src/VP-V/host/Body.h", decoy + body)
            n = len([ln for ln in (decoy + body).splitlines() if ln.strip()])
            assert tu_now() == n, (decoy, tu_now(), n)
        w(root, "src/VP-V/host/Body.h", impl10)
        w(root, "src/VP-V/host/Body.cpp", "#include \"Api.h\"\n")
        os.remove(os.path.join(root, "src", "VP-V", "host", "Free.h"))
        w(root, bill, "struct Impl\n{\n\tPipelineSlots::Registry pipelineSlots;\n};\n#include <VP-V/host/Body.h>\n"
                      "#include <VP-V/host/Api.h>\n")
        assert tu_now() == 10
        # MUTATION (G-CC2 SHOULD-A): grow h2, swap in a v1 baseline and --migrate: refused while the hook-budget
        # h2_ceiling holds (before the fix: exit 0, the baseline written at the grown value)
        w(root, bill, "struct Impl\n{\n\tPipelineSlots::Registry pipelineSlots;\n};\n#include <VP-V/host/Body.h>\n")
        assert cli(root)[0] == 0
        w(root, BUDGET_PATH, json.dumps({"h2_ceiling": {bill: 5}}))
        w(root, bill, "struct Impl\n{\n\tPipelineSlots::Registry pipelineSlots;\n\tint more;\n\tint evenMore;\n};\n"
                      "#include <VP-V/host/Body.h>\n")
        w(root, HOOK_LINES_PATH, json.dumps({"base_ref": DEFAULT_BASE, "total": 99, "files": {bill: 99}}))
        rc, out = cli(root, "--write-hook-lines-baseline", "--migrate", "WAIVER-GAME")
        assert rc == 1 and "grew" in out and "h2 7 > ceiling 5" in out, out
        # ... and a v2 baseline with an old tu measure (a re-seed of tu) still holds its own h2 ceilings
        w(root, BUDGET_PATH, json.dumps({}))
        w(root, HOOK_LINES_PATH, json.dumps({"version": 2, "metric": "H2", "base_ref": DEFAULT_BASE, "total": 5,
                                             "totals": {"production": 5}, "files": {bill: 5}, "tu": {}, "combined": {}}))
        rc, out = cli(root)
        assert rc == 2 and "tu measure" in out, out
        rc, out = cli(root, "--write-hook-lines-baseline", "--migrate", "WAIVER-GAME")
        assert rc == 1 and "h2 7 > ceiling 5" in out, out
    print("selftest OK")
    return 0


def _union_refs(root, extra=None):
    """MUST-D (CC-gate-5): remote-tracking branches only (`refs/remotes/*/r2/composite*`, never local `refs/heads/`
    which anyone can create locally to launder growth). `extra` allows an explicit pattern (`refs/...` or remote suffix)."""
    pats = list(UNION_REF_PATTERNS) + ([extra] if extra else [])
    args = ["for-each-ref", "--format=%(refname:short)"]
    for p in pats:
        if p.startswith("refs/"):
            args.append(p)
        else:
            args.append("refs/remotes/*/" + p)
    try:
        return [r for r in git(args, root).splitlines() if r.strip()]
    except RuntimeError:
        return []


def _is_ancestor(root, a, b):
    try:
        git(["merge-base", "--is-ancestor", a, b], root)
        return True
    except RuntimeError:
        return False


def _is_first_parent_ancestor(root, a, b):
    """MUST-D: true only when commit `a` lies on the `--first-parent` chain of ref `b` (a merged side-branch commit is
    not a first-parent union tip)."""
    try:
        target = git(["rev-parse", "--verify", a + "^{commit}"], root).strip()
        chain = git(["rev-list", "--first-parent", b], root).splitlines()
        return target in chain
    except RuntimeError:
        return False


def rebase_hook_lines_baseline(root, base_ref, files, head, union_tip, waiver, union_ref=None, measure=None):
    """--write-hook-lines-baseline --rebase-to <union tip> --waiver WAIVER-<ID> (A20-CC ruling + CC-gate-5 MUST-D).

    A train merges a landed union tip U (a remote-tracking composite first-parent commit) into the line whose ceilings
    the baseline holds (measured at O = its `measured_at`). Rebased ceilings are capped downward at M(HEAD) so a merge
    that shrank never creates free slack:

        ceiling = min(M(O) + M(U) - M(B), M(HEAD))      B = merge-base(O, U), per file / includer / bucket, floored at 0

    Refused unless: U is an ancestor of `head` and a first-parent ancestor of a remote union branch
    (`refs/remotes/*/r2/composite*` / `union_ref`); O is a committed ancestor of `head` (never "(working tree)");
    U is not in O's history; merge-base(O, U) != O; the baseline is not itself an unratcheted rebase."""
    measure = measure or (lambda rev: run(root, base_ref, files, False, False, rev, None)["site_checks"]["hook_lines"])
    if not re.match(r"^WAIVER-[A-Za-z0-9_.\-]+$", waiver or ""):
        return 1, "refusing --rebase-to without --waiver WAIVER-<ID>"
    try:
        u = git(["rev-parse", "--verify", "--short=9", union_tip + "^{commit}"], root).strip()
        h = git(["rev-parse", "--verify", "--short=9", head + "^{commit}"], root).strip()
    except RuntimeError as e:
        return 2, "refusing --rebase-to: %s" % e
    if not _is_ancestor(root, u, h):
        return 1, "refusing --rebase-to %s: not an ancestor of %s" % (u, h)
    landed = [r for r in _union_refs(root, union_ref) if _is_first_parent_ancestor(root, u, r)]
    if not landed:
        return 1, ("refusing --rebase-to %s: not a landed union tip (no remote union branch matching %s has it on first-parent history)" %
                   (u, ", ".join(list(UNION_REF_PATTERNS) + ([union_ref] if union_ref else []))))
    bl_path = os.path.join(root, *HOOK_LINES_PATH.split("/"))
    try:
        old = vp_footprint.read_json(bl_path)
    except (OSError, ValueError):
        return 2, "refusing --rebase-to: %s is missing or not JSON (seed it first)" % HOOK_LINES_PATH
    at = str(old.get("measured_at") or "")
    if old.get("rebase"):
        return 1, ("refusing --rebase-to: the baseline is an unratcheted rebase (%s); ratchet it at a commit first "
                   "(--write-hook-lines-baseline)" % at)
    if at.strip().startswith("("):
        return 1, "refusing --rebase-to: the baseline's measured_at (%r) is working tree, not a commit" % at
    try:
        o = git(["rev-parse", "--verify", "--short=9", (at.split() or ["?"])[0] + "^{commit}"], root).strip()
    except RuntimeError:
        return 2, "refusing --rebase-to: the baseline's measured_at (%r) is not a commit" % at
    if not _is_ancestor(root, o, h):
        return 1, "refusing --rebase-to: the baseline's commit %s is not an ancestor of %s" % (o, h)
    if _is_ancestor(root, u, o):
        return 1, "refusing --rebase-to %s: already in the history of the baseline's commit %s; nothing to rebase" % (u, o)
    b = git(["merge-base", o, u], root).strip()[:9]
    if b == o:
        return 1, ("refusing --rebase-to %s: merge-base(%s, %s) == %s (baseline commit is already an ancestor of union tip; "
                   "ratchet or seed instead)" % (u, o, u, o))
    mo, mu, mb, mh = measure(o), measure(u), measure(b), measure(h)
    errs = ["%s: %s" % (r, e) for r, m in ((o, mo), (u, mu), (b, mb), (h, mh)) for e in m["errors"]]
    if errs:
        return 2, "refusing --rebase-to without a verdict at every measured commit: " + "; ".join(errs)

    def comb(key, fallback_files=False):
        out = {}
        for f in sorted(set(mo[key]) | set(mu[key]) | set(mb[key]) | set(mh[key])):
            def val(m):
                return m[key].get(f, m["files"].get(f, 0) if fallback_files else 0)
            n = min(val(mo) + val(mu) - val(mb), val(mh))
            if n > 0:
                out[f] = n
        return out

    files_c, tu_c = comb("files"), comb("tu")
    combined_c = {f: n for f, n in comb("combined", True).items() if f in tu_c}
    totals = {k: max(0, min(mo["totals"].get(k, 0) + mu["totals"].get(k, 0) - mb["totals"].get(k, 0),
                            mh["totals"].get(k, 0)))
              for k in list(H2_BUCKETS) + ["renderer"]}
    changed = []
    for key, new in (("h2", files_c), ("tu", tu_c), ("combined", combined_c)):
        prev = old.get("files" if key == "h2" else key) or {}
        for f in sorted(set(prev) | set(new)):
            if prev.get(f, 0) != new.get(f, 0):
                changed.append("%s %s %d -> %d" % (f, key, prev.get(f, 0), new.get(f, 0)))
    today = datetime.date.today().isoformat()
    updates = list(old.get("_updates") or [])
    updates.insert(0, "%s %s: REBASE onto union tip %s (landed in %s) from %s, merge base %s: ceilings = min(M(%s) + M(%s) - "
                      "M(%s), M(%s)), tu measure %d%s" %
                   (today, waiver, u, landed[0], o, b, o, u, b, h, TU_MEASURE_VERSION,
                    (": " + "; ".join(changed)) if changed else ": no ceiling changed"))
    doc = dict(old)
    doc.update({"version": H2_METRIC_VERSION, "metric": "H2", "tu_measure": TU_MEASURE_VERSION, "base_ref": base_ref,
                "measured_at": "%s + %s - %s (rebase)" % (o, u, b),
                "rebase": {"ours": o, "union": u, "merge_base": b, "landed_in": landed[0], "waiver": waiver, "head": h},
                "total": sum(totals[k] for k in H2_BUCKETS),
                "totals": dict({k: totals[k] for k in H2_BUCKETS}, renderer_information=totals["renderer"]),
                "files": files_c, "tu": tu_c, "combined": combined_c, "_updates": updates})
    with open(bl_path, "w", encoding="utf-8", newline="\n") as fh:
        json.dump(doc, fh, indent=1)
        fh.write("\n")
    bpath = os.path.join(root, *BUDGET_PATH.split("/"))
    try:
        bdoc = vp_footprint.read_json(bpath)
    except (OSError, ValueError):
        bdoc = None
    if bdoc is not None:
        bdoc["h2_ceiling"] = dict(sorted(files_c.items()))
        with open(bpath, "w", encoding="utf-8") as fh:
            json.dump(bdoc, fh, indent=1)
            fh.write("\n")
    return 0, ("wrote %s: REBASE onto %s from %s (merge base %s), H2 %d (%s), tu %d; %d ceilings changed" %
               (HOOK_LINES_PATH, u, o, b, doc["total"], ", ".join("%s %d" % (k, totals[k]) for k in H2_BUCKETS),
                sum(tu_c.values()), len(changed)))


def main(argv=None):
    ap = argparse.ArgumentParser(description="Fail on a VP-V hook in a Bill file that is not gated off in the Original pathway.")
    ap.add_argument("--selftest", action="store_true", help="run the H2 metric selftests (T1-T13) and exit")
    ap.add_argument("--root", default=os.path.dirname(HERE))
    ap.add_argument("--base-ref", default=DEFAULT_BASE)
    ap.add_argument("--files", nargs="*")
    ap.add_argument("--json", action="store_true")
    ap.add_argument("--fail-untagged", action="store_true")
    ap.add_argument("--no-git", action="store_true", help="no git: --files are the Bill-owned files; no untagged report")
    ap.add_argument("--rev", help="check this git revision's tree instead of the working tree (no checkout needed)")
    ap.add_argument("--since", help="report only hits / untagged lines / new sites on lines ADDED since this ref (a merge base); "
                                    "the budget still applies to the whole file; also fails (exit 1) on added lines in Bill-owned production files that name a "
                                    "symbol declared in src/VP-V/ headers and unknown to Bill's baseline, tagged or not-yet-tagged: see the module "
                                    "docstring, UNTAGGED VP-V REFERENCES")
    ap.add_argument("--write-manifest", action="store_true", help="(re)seed docs/audit/hook-sites.json from the tree")
    ap.add_argument("--write-budget", action="store_true", help="(re)seed docs/audit/hook-budget.json from the tree (ceilings = current counts)")
    ap.add_argument("--write-hook-lines-baseline", action="store_true", help="write docs/audit/hook-lines-baseline.json; refused if it would grow")
    ap.add_argument("--migrate", metavar="WAIVER-ID", help="with --write-hook-lines-baseline: allow a migration (baseline missing, v1, "
                                                          "or without tu / combined); recorded in _updates (G-CC1 SHOULD-2)")
    ap.add_argument("--relocation", metavar="WAIVER-ID", help="with --write-hook-lines-baseline: allow a tu / combined ceiling to "
                                                             "rise (a named RELOCATION, G-CC1 (7)); recorded in _updates")
    ap.add_argument("--allowance", type=int, metavar="N", help="with --relocation: the most one includer's combined h2 + tu may "
                                                              "grow, and tu may rise beyond the h2 it took away (0..%d; G-CC2 "
                                                              "SHOULD-B; G-CC4 SHOULD-D: only with a real h2 drop)" %
                                                              RELOCATION_MAX_ALLOWANCE)
    ap.add_argument("--rebase-to", metavar="SHA", help="with --write-hook-lines-baseline and --waiver: re-measure the ceilings "
                                                       "for a train that merged this landed union tip: M(baseline commit) + "
                                                       "M(SHA) - M(merge base), never at HEAD (rebase_hook_lines_baseline)")
    ap.add_argument("--waiver", metavar="WAIVER-ID", help="with --rebase-to: the waiver id recorded in _updates")
    ap.add_argument("--union-ref", metavar="PATTERN", help="with --rebase-to: another branch pattern that names a landed "
                                                           "union line (default %s)" % ", ".join(UNION_REF_PATTERNS))
    args = ap.parse_args(argv)
    if args.selftest:
        return selftest()
    if args.rebase_to:
        if not args.write_hook_lines_baseline or args.no_git or args.migrate or args.relocation:
            print("--rebase-to needs --write-hook-lines-baseline, git mode, and no --migrate / --relocation")
            return 2
        rc, msg = rebase_hook_lines_baseline(args.root, args.base_ref, args.files, args.rev or "HEAD", args.rebase_to,
                                             args.waiver, args.union_ref)
        print(msg)
        return rc
    res = run(args.root, args.base_ref, args.files, args.no_git, args.fail_untagged, args.rev, args.since)
    if args.write_hook_lines_baseline:
        hl = res["site_checks"]["hook_lines"]
        if hl["errors"]:
            print("refusing to write a hook-lines baseline without a verdict: " + "; ".join(hl["errors"]))
            return 2
        h2_over = [x for x in hl["over"] if x not in hl.get("tu_over", [])]
        if h2_over:
            print("refusing to write a hook-lines baseline that grew: " + "; ".join(h2_over))
            return 1
        waiver_rx = re.compile(r"^WAIVER-[A-Za-z0-9_.\-]+$")
        sha = args.rev or "working tree"
        if not args.no_git:
            # G-CC2 SHOULD-C: the sha is recorded on the working tree too (HEAD, the commit the tree sits on)
            try:
                sha = git(["rev-parse", "--short=9", args.rev or "HEAD"], args.root).strip() + ("" if args.rev else " (working tree)")
            except RuntimeError:
                pass
        bl_path = os.path.join(args.root, *HOOK_LINES_PATH.split("/"))
        try:
            updates = list(vp_footprint.read_json(bl_path).get("_updates") or [])
        except (OSError, ValueError, AttributeError):
            updates = []
        today = datetime.date.today().isoformat()
        if hl["baseline"] is None:
            # G-CC1 SHOULD-2: a migration (missing, v1, or no tu / combined ceilings) writes any values, so it needs an
            # explicit WAIVER id, recorded in _updates
            why = hl["baseline_error"] or "no baseline"
            if not (args.migrate and waiver_rx.match(args.migrate)):
                print("refusing to migrate %s (%s) without --migrate WAIVER-<ID> (G-CC1 SHOULD-2)" % (HOOK_LINES_PATH, why))
                return 1
            print("migrating %s under %s (needs its own WAIVER commit, rule 19/28)" % (HOOK_LINES_PATH, args.migrate))
            # every tu / combined ceiling the migration raises is named in the record (G-CC2 SHOULD-B spirit)
            try:
                old = vp_footprint.read_json(bl_path)
            except (OSError, ValueError):
                old = {}
            raised = []
            for key in ("tu", "combined"):
                prev = old.get(key) if isinstance(old.get(key), dict) else None
                if prev is None:
                    continue
                for f, n in sorted((hl.get(key) or {}).items()):
                    # an includer without tu had combined = its h2
                    was = prev.get(f, (old.get("files") or {}).get(f, 0) if key == "combined" else 0)
                    if n > was:
                        raised.append("%s %s %d -> %d" % (f, key, was, n))
            updates.insert(0, "%s %s: migration at %s (%s)%s" % (today, args.migrate, sha, why,
                                                               (": raised " + "; ".join(raised)) if raised else ""))
        if hl.get("tu_over"):
            if not (args.relocation and waiver_rx.match(args.relocation)):
                print("refusing to raise a tu / combined ceiling without --relocation WAIVER-<ID>: " + "; ".join(hl["tu_over"]))
                return 1
            # G-CC2 SHOULD-B: a relocation is bounded. Per includer the combined h2 + tu may grow by at most the
            # stated --allowance N (bookkeeping), and tu may rise by at most the h2 it took away plus N: new VP code
            # written straight into a file-scope header is not a relocation.
            if args.allowance is None or not (0 <= args.allowance <= RELOCATION_MAX_ALLOWANCE):
                print("refusing a RELOCATION without --allowance N (0..%d): the most one includer's combined h2 + tu may "
                      "grow (G-CC2 SHOULD-B)" % RELOCATION_MAX_ALLOWANCE)
                return 1
            old = vp_footprint.read_json(bl_path) if os.path.exists(bl_path) else {}
            old_files, old_tu, old_comb = old.get("files") or {}, old.get("tu") or {}, old.get("combined") or {}
            deltas, beyond = [], []
            for f in sorted(set(hl.get("tu", {})) | set(hl.get("combined", {})) | set(old_tu) | set(old_comb)):
                # an includer without tu has combined = h2 (the combined map lists only includers with tu)
                d_tu = hl.get("tu", {}).get(f, 0) - old_tu.get(f, 0)
                d_comb = (hl.get("combined", {}).get(f, hl["files"].get(f, 0)) -
                          old_comb.get(f, old_files.get(f, 0)))
                h2_drop = old_files.get(f, 0) - hl["files"].get(f, 0)
                if d_tu <= 0 and d_comb <= 0:
                    continue
                deltas.append("%s tu %+d, h2 %+d, combined %+d" % (f, d_tu, -h2_drop, d_comb))
                # G-CC4 SHOULD-D: a tu rise is a relocation only where the includer's h2 really fell
                if d_comb > args.allowance or d_tu > h2_drop + args.allowance or (d_tu > 0 and h2_drop <= 0):
                    beyond.append(deltas[-1])
            if beyond:
                print("refusing a RELOCATION beyond its allowance %d (combined growth <= N, tu rise <= h2 drop + N and a "
                      "real h2 drop per includer whose tu rises, G-CC2 SHOULD-B / G-CC4 SHOULD-D): %s" %
                      (args.allowance, "; ".join(beyond)))
                return 1
            updates.insert(0, "%s %s: RELOCATION allowance %d at %s: %s" % (today, args.relocation, args.allowance, sha,
                                                                         "; ".join(deltas)))
        totals = {b: hl["totals"][b] for b in H2_BUCKETS}
        os.makedirs(os.path.join(args.root, "docs", "audit"), exist_ok=True)
        doc = {"_note": "Honest H2 per Bill-owned file (tools/check_hook_gating.py, CC-audit-1): non-blank added or changed lines vs "
                        "the Bill baseline, tagged or not, plus the non-blank lines of every VP-authored file included at class "
                        "or function scope; production, tests and project files (documentation files are reported, not gated). "
                        "tu (G-CC1 (7)): per Bill includer, the non-blank lines of VP-authored files reached from its added "
                        "FILE-scope includes (shared VP-V headers that a VP-V production .cpp also compiles are excluded); combined = "
                        "h2 + tu per includer. Ratchet: every ceiling only shrinks (measure = ceiling); a tu / combined raise needs "
                        "a RELOCATION waiver recorded in _updates. Written with --write-hook-lines-baseline.",
               "version": H2_METRIC_VERSION, "metric": "H2", "tu_measure": TU_MEASURE_VERSION,
               "base_ref": args.base_ref, "measured_at": sha,
               "total": sum(totals.values()), "totals": dict(totals, renderer_information=hl["totals"]["renderer"]),
               "files": dict(sorted(hl["files"].items())), "tu": dict(sorted(hl.get("tu", {}).items())),
               "combined": dict(sorted(hl.get("combined", {}).items()))}
        if updates:
            doc["_updates"] = updates
        with open(bl_path, "w", encoding="utf-8", newline="\n") as fh:
            json.dump(doc, fh, indent=1)
            fh.write("\n")
        # hook-budget.json carries the same per-file ceilings as `h2_ceiling` next to the site ceilings (the checker
        # enforces the lower of the two)
        bpath = os.path.join(args.root, *BUDGET_PATH.split("/"))
        try:
            bdoc = vp_footprint.read_json(bpath)
        except (OSError, ValueError):
            bdoc = None
        if bdoc is not None:
            bdoc["h2_ceiling"] = dict(sorted(hl["files"].items()))
            with open(bpath, "w", encoding="utf-8") as fh:
                json.dump(bdoc, fh, indent=1)
                fh.write("\n")
        print("wrote %s (%d files, H2 %d at %s: %s)" % (HOOK_LINES_PATH, len(hl["files"]), sum(totals.values()), sha,
                                                       ", ".join("%s %d" % kv for kv in sorted(totals.items()))))
        return 0
    if args.write_manifest or args.write_budget:
        if args.rev:
            print("--write-* needs the working tree", file=sys.stderr)
            return 2
        sites = [s for r in res["files"] for s in r.get("sites", [])]
        if args.write_manifest:
            doc = {"_note": SITE_NOTE + "; seam / expiry are filled by hand from refs/107-host-seams-plan.md s2; regenerate with "
                            "`python -I tools/check_hook_gating.py --write-manifest` (matches sites by file and text, identical-text duplicates by anchor then nearest line then order; keeps their "
                            "ids, hand-filled seam / expiry and order; never reuses a dropped id)",
                   "base_ref": args.base_ref, "generated_at": datetime.date.today().isoformat(), "sites": sites}
            old = load_json(Tree(args.root), MANIFEST_PATH) or {}
            sites = merge_manifest_sites(old.get("sites", []), sites)
            doc["sites"] = sites
            os.makedirs(os.path.join(args.root, "docs", "audit"), exist_ok=True)
            with open(os.path.join(args.root, *MANIFEST_PATH.split("/")), "w", encoding="utf-8") as fh:
                json.dump(doc, fh, indent=1)
                fh.write("\n")
            print("wrote %s (%d sites)" % (MANIFEST_PATH, len(sites)))
        if args.write_budget:
            counts = {}
            for r in res["files"]:
                if _site_gated(r) and r.get("sites"):
                    counts[r["file"]] = len(r["sites"])
            doc = {"_note": "Per-file ceiling on VPV-HOOK SITES in Bill-owned files (tools/check_hook_gating.py; " + SITE_NOTE +
                            "). Ratchet: lower a value when a site is collapsed into a seam; never raise one without a HOOK-APPROVAL "
                            "id (rule 32). A file not listed has a ceiling of 0.",
                   "base_ref": args.base_ref, "generated_at": datetime.date.today().isoformat(), "files": dict(sorted(counts.items()))}
            # Keep the hand-written `temporary` blocks (HA-xx allowances with a trigger): a file whose count is above its
            # reverts_to still needs the block, and `files` carries reverts_to, never the temporarily raised count. A block
            # whose file now counts <= reverts_to is no longer needed and is dropped with its files entry = the count.
            old_budget = load_json(Tree(args.root), BUDGET_PATH) or {}
            kept = {}
            for f, tmp in sorted((old_budget.get("temporary") or {}).items()):
                if counts.get(f, 0) > int(tmp["reverts_to"]):
                    kept[f] = tmp
                    counts[f] = int(tmp["reverts_to"])
            if kept:
                doc["files"] = dict(sorted(counts.items()))
                doc["temporary"] = kept
            # the per-file H2 ceilings are written only by --write-hook-lines-baseline: never drop them here
            if "h2_ceiling" in old_budget:
                doc["h2_ceiling"] = old_budget["h2_ceiling"]
            os.makedirs(os.path.join(args.root, "docs", "audit"), exist_ok=True)
            with open(os.path.join(args.root, *BUDGET_PATH.split("/")), "w", encoding="utf-8") as fh:
                json.dump(doc, fh, indent=1)
                fh.write("\n")
            print("wrote %s (%d files, %d sites)" % (BUDGET_PATH, len(counts), sum(counts.values())))
        return 0
    if args.json:
        print(json.dumps({k: v for k, v in res.items() if k != "files"}, indent=1))
    else:
        print(render(res))
    return res["rc"]


if __name__ == "__main__":
    sys.exit(main())
