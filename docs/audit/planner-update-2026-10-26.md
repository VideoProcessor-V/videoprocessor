Planner update and read-only review request (2026-10-26)

It has been about a week since the last planner exchange (planner-answers-dv-next-2026-10-17.md).
This is a status update plus a READ-ONLY review request. Please do not build, run tests, use the GPU,
or write to any worktree. Write your answers to a new file next to this one:
docs/audit/planner-review-2026-10-26.md (on the docs/todo branch, or hand them to the owner).

Where to read (newest first)
- Short plan and rules: docs/TODO.md (branch docs/todo on github.com/VideoProcessor-V/videoprocessor).
- Master schedule (Opus-reviewed): docs/plans/2026-10-26/schedule-2026-10-31.md.
- Plan status, new items N1-N9, owner answers: docs/plans/2026-10-26/plan-review-consolidated.md (section 0).
- Bill-divergence programme: docs/plans/2026-10-26/plan-n3-bill-divergence-programme.md,
  divergence-sweep-firstpass.md, upstream-divergence-register-DRAFT.md.
- Chroma: docs/plans/2026-10-26/plan-n8-chroma.md. Nets and runner: plan-n9-nets-and-runner.md.
- Code: branch integ/batch1-wip (d85519e2) = everything merged this week. Master integration plan:
  docs/design/integration-batch-plan.md (status board section 1).

What happened since 2026-10-17 (summary)
1. Integration batch: groups A-E merged (silent-fallback catalogue, perf/efficiency, segment runner,
   repeat-frame reuse, LED/Dynamic Drive glue, calibration LUT host, generic status API 44,
   diagnostics adapter, frame analysis store + single luma decode, M2 module moves for output, DV,
   motion, entrance (S1), still-zero estimator, entrance_gpu_drain switch), hdr_inference either/or,
   Config usability (Dynamic Drive main page, four tiers with Upper "planned", per-field Set by,
   Approve/Keep), tests moved out of Bill's test file, test-category manifest + generated filters,
   copyright headers (VP-V contributors, GPL-3.0-only).
2. Every package had at least one independent audit before its build; every merge was audited before
   its build; cheaper models drafted, Opus audited.
3. Baseline 2026-10-26 on d85519e2: CPU 4283/4284, Config passed, GPU hardware 19/19, WARP graphics
   tests on the RTX 219/219. Chain gate 263/268: stale log-string test (DV neutral trims), stale
   early-upload tests (owner decided early upload in every mode), a general_rate_fixes flip-test failure
   (suspected stale DLL pair; re-run pending), stale dimensions.json (regenerated).
4. Bug hunt: in-between frames differ by +-1/16-bit from plain draws depending on in-flight GPU work
   (libplacebo scaling passes); cured by a GPU finish after the entrance stages (switch
   entrance_gpu_drain, default off); root cause open pending the D3D11 debug layer.
5. Divergence sweep: 178 Bill files changed (+44k); most added lines are VP feature code inside Bill's
   files. Owner rule: every behaviour change to Bill's code must be a PR-worthy bug fix (registered),
   else an either/or with the Original pathway = Bill's, else revert; a final Bill-code gate before the
   big test.
6. Net design changes (owner): the upscaling net becomes B-refine on top of the Stage 8b hybrid upscale
   (net off = exactly 8b); route via a higher resolution then the hybrid downscale, direct-to-output
   only as an optional performance fallback to the actual output size; aspect/A-lens/NLS applied only in
   the final per-axis scale; 1440p doubled per axis; half-resolution-trunk D requested for round 3;
   colour repair (net A colour branch) a priority; full precision default (fp16 only a manual switch).
   Earlier 3090 net timings were optimistic (D ~61 GMAC at 2560x1600, 3.4 ms floor); realistic D 6-9 ms,
   B-refine ~2.9 ms after runner tuning (estimates; 3090 measurement confirms, nothing waits on it).
7. New audit types before the big test: intent audits per module (fallback directives, precision
   throughput, grain, BTB/WTW, rates, modularity) and efficiency sweeps of every separated module.

Questions for the planner (read-only; answer each briefly, with a recommendation)
Q1. Order and risk: does the schedule's ordering (WP-9 chain on the critical path; N3 extraction in a
    second lane; Bill merge after the big test) look right? What would you move?
Q2. Bill merge timing: extract VP code from Bill's files first, then the big test, then the Bill merge
    (124 conflict hunks in 39 files in a dry run) — or merge Bill earlier? The plan says merge first only
    if extraction slips past about two weeks.
Q3. B-refine on Stage 8b: any concerns about training a residual net on 8b's output (8b is grain-aware and
    motion-informed; per-axis variants), versus the plain x2 net?
Q4. Colour repair: a colour branch of A versus a separate small chroma net — which, and what labels?
Q5. The timing hazard (+-1/16-bit, cured by a GPU drain): acceptable to ship with the switch off and keep
    hunting, or should the drain default on after costing?
Q6. Missing items: anything from your earlier reviews (stage 7/12, DV next, detection stress tests) that
    the current plan has dropped or contradicts?
Q7. Test strategy: the plan adds the chain gate per merge group, a CPU defaults fingerprint, frozen r6 /
    B3 reference sets, a GPU slot on/off matrix, and GPU tests on both WARP and hardware. Gaps?
