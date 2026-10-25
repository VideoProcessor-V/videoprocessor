# F-8: either/or between VP's HDR inference and Bill's newlldv heuristic

Branch `feat/hdr-inference-choice` from `feat/m2-dv` 27f56b0c. Finding F-8 in
[module-findings-2026-10-25-m2-dv.md](module-findings-2026-10-25-m2-dv.md); owner rule
vp-either-or-overlaps (where VP overlaps Bill's function, both stay selectable, Original = Bill's).

## The overlap

Bill's `[general] newlldv` switch enables a heuristic in his `CVideoProcessorDlg::UpdateNewLldvCandidate`
(origin/v1.4.02-beta): a valid capture state that is **BT.2020 + SDR EOTF + no HDR InfoFrame** is
treated as LLDV/PQ after 1500 ms; nothing else. Since 2026-10-17 VP replaced it in his dialog with
`HdrInferenceTracker` (input.signal): 10/12-bit (8-bit only with an LLDV indication), no DV tunnel,
explicit hdr_colorspace REC709 forbids it, signal epochs (a mode change restarts the
stabilisation), and the picture-statistics SDR veto (`newlldv_sdr_veto`). There was no way to get
Bill's behaviour back.

## The setting

| | |
|---|---|
| Key | `[vpv] hdr_inference` = `vp` \| `original` (also read from `[vprenderer]`, as the other VP-V globals) |
| Default | `vp` - the behaviour since 2026-10-17 (unchanged) |
| Original pathway | `original` (OriginalPathway.h entry, slots.json `original_pathway`) |
| Gate | `HdrInferenceChoice::Effective(setting, registry)`: Bill's heuristic whenever the slot registry has `input.signal` in its upstream form (Original pathway) or not running. `input.signal` is a host slot that `pipeline_slots_off` cannot switch off today, so in practice the gate is the Original pathway |
| Master switch | `[general] newlldv` (Bill's key) for both; `newlldv_sdr_veto` only acts in `vp` (the original heuristic has no veto) |
| When it applies | GUI start-up (`VideoProcessorApp` passes `/hdr_inference <effective>`); Config tooltip says so |
| Config control | VP-V page, Pipeline (expert) tab: "HDR inference (newlldv)" (`vpv.expert.hdr_inference`) |
| Status / dump | start-up log line (`HdrInferenceChoice::StartupLine`: what runs, the Original-pathway override, a command-line override, an unreadable value); the LLDV decision line and the raw-capture note carry `hdr_inference=` (and `hdr_inference_problem=`); the main window EOTF reads "PQ (inferred, original)" and the HDR overlay item "PQ inferred (no HDR flag; original heuristic)" (", hdr_inference invalid" when the key was unreadable); the renderer's settings dump has `hdr_inference=` and the key is in the live-apply fingerprint; `/help` lists `/hdr_inference` |

## How it is built

- **VP-V (committed, 6d122c78):**
  - `HdrInferenceOptions::originalHeuristic` in `HdrInference.h/.cpp`. The tracker then uses
    `EvaluateOriginalNewLldvEligibility` (Bill's rule), the same 1500 ms confirmation, no epochs and
    no veto. The default path is untouched.
  - `HdrInferenceChoice.h`: Parse, Effective (registry gate), FromConfig (for the GUI), Describe.
  - `HdrInferenceChoiceTests`: Bill's rule, original-mode tracker, VP default unchanged, key +
    registry gate + FromConfig.
- **Wiring as a patch** ([hdr-inference-choice-callsites.patch](hdr-inference-choice-callsites.patch)):
  Bill's files get call sites only.
  - `RendererProfileConfig.h` (Bill's): the validator choice and `[vpv]` membership (2 lines).
  - `LibplaceboVideoRenderer.cpp` (host): the setting, its reader, the `key=hdr_inference` line in
    `ApplyOriginalPathwaySettings`, fingerprint, change log, dump line (about 15 lines).
  - `VideoProcessorApp.cpp` (Bill's): `/hdr_inference` from `HdrInferenceChoice::FromConfig`, its
    validation and its dispatch (one call).
  - `VideoProcessorDlg.cpp/.h` (Bill's): `SelectOriginalNewLldvHeuristic(bool)`, which sets the
    tracker option (one setter).
  - `ConfigEditorWindowTests.cpp` (Bill's test file): the allow-list / inventory line that every
    `[vpv]` key has (stored by the VP-V page).
  - VP files that depend on the above: `OriginalPathway.h` (57 entries), slots.json
    `original_pathway`, `VpvSection.cpp` (the control), `VpvConfigTests.cpp`,
    `VP-settings-reference.md`, `HdrInferenceChoiceWiringTests.cpp` (new) and its project entry,
    the regenerated components-master / tier table.
  - They are in the patch because the drift check and `PipelineSkeletonTests` require every Original
    entry to be a key the validator accepts and the renderer sets.
  - `git apply --check --cached` is clean on this branch. The patch touches none of the DV glue,
    so it is independent of the m2-dv patches.

## Wiring tests (both)

1. Renderer: `HdrInferenceChoiceWiringTests` checks:
   - the validator accepts vp/original and refuses others;
   - through the renderer DLL's settings dump: default `vp`, both values, invalid kept, the Original
     pathway forcing `original`, and the fingerprint changing.
2. Config: `VpvConfigTests::testExpertPipelineKeys`: the combo shows vp by default, and choosing
   Original stores `[vpv] hdr_inference: original`. `ConfigEditorWindowTests` checks the key's
   coverage entry.

## Not covered / limits

- No live switch: the choice is read at start-up, like `newlldv` itself (both are command-line
  switches built from the configuration).
- "Original" is **Bill's candidate decision**: BT.2020 + SDR + no InfoFrame, confirmed after 1.5 s,
  and (audit 2) both HDR modes "Follow input (LLDV)" - his `IsNewLldvModeSelected`, restored as
  `LldvModesSelected()` and passed in the signal, so an explicit BT.2020 hdr_colorspace rejects as
  upstream. It gates eligibility (a mode change restarts the stabilisation; upstream gated the use).
  The dialog flow around it stays VP's (restart scheduling, the veto capture gate - inert with no
  veto). Restoring his full dialog flow would mean reverting VP's changes inside his file.
- GUI start-up has no automated test (Bill's App/dialog). `HdrInferenceChoice::FromConfig` is
  tested instead.

## Verification (2026-10-25, after the audit fixes)

- Patch applied, Magewell SDK, whole solution: rc=0 (D:\codexideoprocessor\hdr-choice-build5.log).
- CPU classes HdrInferenceChoiceTests, HdrInferenceChoiceWiringTests, HdrInferenceTests,
  DvIncomingHdrModeTests, PipelineSkeletonTests, ConfigFileTests: 231/231
  (D:\codexideoprocessor\hdr-choice-trx8).
- Config tests, off screen, `-Match "VP-V|every schema key|inventory"`: 13/13, gate passed
  (D:\codexideoprocessor\hdr-choice-trx\config).
- Not run: GPU / on-screen; the GUI start-up path (Bill's App / dialog) has no automated test.
