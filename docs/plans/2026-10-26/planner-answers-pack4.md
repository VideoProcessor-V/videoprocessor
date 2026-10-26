# Planner answers, request 4 (2026-10-26) — filed by the coordinator

Received via the owner. Full text kept in the session transcript; the decisions are applied to
kickoff-2026-10-31.md and test-plan-synthetic-loop-shadow.md (sections "Planner adjustments").

A. Week 1 day plan (Sat 31 → Fri 6): Sat = hour 0, T5 fingerprints, triage the EntranceOnce 1-in-5 flake,
   WP-9.0 adapter, audit+merge the 3 drafted branches, N3-P1 + Config no-rewrite; owner confirms plan +
   installs Graphics Tools. Sun = T4 determinism + Original=Bill (drain on), record r6 refs; 9.0 cases;
   N3-P1 audit, 8b one-axis starts. Mon = T1 transitions; 9a (own audit); 8b; merge group 1 + chain gate.
   Tue = T2 queue continuum; 9a audit → 9b; 8b audit+merge; weekly rehearsal Bill merge. Wed = T3 drift +
   low latency; 9c; WP-10 NetHost; owner optional cinema evening (HTPC pack, HDR10+ EDID check, Diva).
   Thu = fixes from T1-T3, bypass coverage (stub nets), overlay install test in a VM; 9.6 + frozen B3-on refs
   (drain on); runner V2 basics. Fri = merge group 2 + chain gate + nightly; week report; slack/WP-16.
B. Owner checklist (shadow days + M1): shadow with LED in none/log_only; WATCH madVR's dropped/repeated
   counters (Ctrl+J) because VP-V shares the 3090 — if they rise, run VP-V lighter or stop; daily status
   glance + notes; rollback rehearsal once. M1: note projector state + AVR lip sync, madVR exported, overlay
   .bat confirms Bill's version; kit (phone Slo-Mo tripod, i1Display Pro at the seat line 20-30 cm, ES-1000
   with black cover once the patch is validated, notebook); watch ~45 min (stats line, ramp, flat fields,
   reference scenes VP-V then madVR, motion slider on the pan, LED picture_only → script → compensate + four
   short tests + headlights/lightning); measure ~30 min (step response, screen white, gamma fields
   16/126/235 and 40/80/180, white sweep above CNTR 59, ES-1000 spectra for the CCSS, lip-sync marker);
   note setting/verdict/number; rollback one step each (uninstall .bat; madVR Startup batch; projector
   LPW1 255 / CNTR 50 on exit).
C. Test plan cross-check: order 1.3 + 1.1 first, then 1.4-1.7; fix the entrance flake before 1.5.
   Criteria: 1.6 bounded wait ≤10 s else lowest-global-motion frame, virtual clock; 1.7 every frame exactly
   once except scheduled slips, error ≤ ½ refresh, needs DXGI stats (HTPC); 1.8 re-init budget per change
   type; 1.9 simulated vs one real driver reset on the 3090; 1.14 not worse than direct by >0.05 dB, no added
   overshoot, plus a perceptual score; 1.15 regression over hours 1-4 with CI < 1 MB/h; 2.5 ±1 frame;
   3.1 every fallback event has a reason code. Gaps: thermal hot-run variant of 1.16; sleep/resume + display
   off in 1.9; FL35 RS-232 emulator on a USB-serial loopback before phase 3; blind A/B (randomised toggle,
   hidden label logged); nets on real phase-2 captures; Bill's installer/updater over an overlaid VP-V.
D. Overlay .bat (calls PowerShell; -DryRun; log; elevation): version from exe resource + uninstall registry,
   allow-list, refuse on mismatch or existing VP-V marker; backup to %ProgramData%\VP-V\overlay-backup\<ts>\
   with manifest.json (paths, Bill/VP-V SHA-256, sizes, versions, added files), verify before swapping;
   runtime pair staged then swapped atomically, failure restores; settings never overwritten — VP keys in
   VideoProcessor-vpv.cfg or a namespaced section Bill ignores, proven by 1.17; uninstall checks VP-V hashes
   (Bill's updater ran → refuse, offer restore anyway), restores + verifies Bill's hashes; no auto-updater;
   VP-V checks its own hashes at start; madVR Startup batch never touched.
E. HTPC overnight pack: zip (bin, tests, portable vstest, fixtures ≤2 GB, run-overnight.cmd, scripts,
   README, manifest); preflight (hashes, display modes, driver, nvidia-smi, power plan, projector output by
   EDID name never used, spare output required for fullscreen/loop tests); TrainKit/madVR running → wait or
   CPU/WARP only, no timing gates beside another GPU job, never stop VP2/madVR; runs in priority order
   time-boxed (GPU suites, 3090 costs, fullscreen timing gate with DXGI on the spare output, live loop via
   spare output → EZCOO → spare Quad input, soak); results zip with summary.md; guarantees: no installs/
   registry/services, keep-awake by process request, display modes restored (also on Ctrl-C or STOP file),
   never opens COM1, never touches VP2's Quad input/madVR/Startup/VP2 settings; any failed guarantee stops it.
