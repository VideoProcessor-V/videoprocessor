# Tone + gamut map (`colour.tone_gamut`)

Unit: **colour** | side: processing | order: 170 | status: **built** | hook point: libplacebo

## Purpose / why

libplacebo tone/gamut mapping (keeps all of its colour science) OR, for low-end cards, the combined static HDR->SDR 3D LUT in the same slot (planned, not built). The migrated file is the output-side policy: which target colour space, transfer and range the picture is mapped to, and how that target is declared to Windows (DXGI).

## Contract

- Input: output-size picture in the source colour space
- Output: picture in the target colour space (linear target light needed by LED compensation)
- Geometry: output size
- Delay share: 0 frames
- Look-ahead: peak detection (off keeps DTM off)

## How the code works

### Output policy (migrated: `LibplaceboOutputPolicy.h/.cpp`)

Pure, renderer-independent C++. Despite the name it has no libplacebo or D3D dependency, so it is unit-testable. Namespace `LibplaceboOutput`:

- Parsing: `ParsePresentation` (`auto` / `composed` / `direct`), `ParseRange` (`auto` / `full` / `limited`), `ParseGamma` (`auto`, `srgb`, `bt1886`, `1.8` ... `2.8`; anything else = `UNSUPPORTED`), `ParseSdrAdjustGamma` (`auto`, `passthrough` or empty = preserve codes, `off` / `no`, anything else = `on`).
- `MakeSdrOutputContract(request, target, reportBt2020)`: separates the pixel target (`REC709` / `P3_D65` / `BT2020`) from the transport. The DXGI transport is always forced to P709; BT.2020 is signalled to the display only by the NVIDIA AVI InfoFrame when `report_bt2020_to_display` is on.
- `MakePlan(request)` gives a `Plan`:
  - presentation model: blit (composed) only for an explicit `composed`; otherwise flip;
  - for `limited`: a studio DXGI colour space, G24 for `auto` / `2.4`, G22 only with the Limited/G22 experiment enabled; any other gamma makes the plan invalid;
  - for full range: the standard Full G22/P709 swapchain, with the calibrated display transfer (BT.1886 or pure power) applied in the renderer as `targetTransfer`.
- `ExecuteVerifiedTransition(check, set)`: the Check / Set / Check sequence for `SetColorSpace1`.
- `Finalize(plan, evidence)` gives `Actual`: whether the requested encoding is active and whether it is safe to render (for example an unverified Full/sRGB restore, a missing `IDXGISwapChain3`, or BT.2020 without flip is unsafe).
- `ShouldFallbackToComposed(plan, actual)`: AUTO + Limited whose encoding did not become active.
- `ResolveSdrGamma`, `ResolveCalibrationTargetTransfer`, `ResolveRenderTargetTransfer`, `ResolveCalibrationTransfers`: per frame, the transfer the renderer targets (with a calibration LUT: `hdr_tone_map_target_gamma`, or `sdr_lut_input_gamma` for SDR input) and whether the SDR gamma is adjusted, suppressed, not applicable or blocked, with a reason.
- Diagnostics: `AnalyzePackedR10` (R10G10B10A2 readback statistics, near-black buckets), `ExpandR10ToR16`, `ClassifyOneShotSignal` (NvAPI InfoFrame set / read-back), `DescribeLimitedTransfer` (status text for the Limited G22/G24 cases), `ToString` / `ToRangeString` / `ToGammaString`.

Data flow (all in LibplaceboVideoRenderer.cpp, render thread / swapchain negotiation):

1. On initialise, the output settings are parsed into a `Request`, passed through `MakeSdrOutputContract`, then `MakePlan`. The embedded preview window is forced to COMPOSED / FULL / SRGB / REC709.
2. The swapchain is created per `Plan::useBlit`. `ExecuteVerifiedTransition` applies the DXGI colour space; `Finalize` records `actualOutput`.
3. If `ShouldFallbackToComposed`, the swapchain is recreated as composed Full/sRGB (`EnsureAutoLimitedFallback`).
4. While `actualOutput.safeToRender` is false, frames are not presented; the negotiation is retried every second and on a monitor change.
5. Each frame, `ResolveCalibrationTransfers` decides the target transfer and the SDR gamma action.
6. `GetOutputContractStatus` exposes the plan and the actual state to the GUI and Config.

### Tone and gamut mapping itself (legacy, not migrated)

libplacebo does it. `LibplaceboRenderParameters.cpp` maps the settings to `pl_render_params`:

- `tone_mapping` to `pl_tone_map_*` (spline when `auto`);
- `gamut_mapping` to `pl_gamut_map_*`;
- `peak_detection` to the peak-detect parameters (`off` = none, `high_quality` = percentile below 100);
- the tone tuning overrides (`ToneMappingTuning.h`).

There is no combined HDR->SDR LUT code.

### Config keys actually read

Read per renderer profile (`[vprenderer.Default]` and the `[vprenderer.<name>]` / `[vprenderer.color.<name>]` profiles that apply) by LibplaceboVideoRenderer.cpp.

Tone and gamut keys:

- `tone_mapping`: `auto` / `spline` / `bt2390` / `st2094-40` / `reinhard`.
- `gamut_mapping`: `auto` / `perceptual` / `softclip` / `relative` / `desaturate`.
- `peak_detection`: `auto` / `default` (`on`) / `high_quality` / `off`.
- Tone tuning keys (ToneMappingTuning.h): `knee_adaptation`, `knee_minimum`, `knee_maximum`, `knee_default`, `slope_tuning`, `slope_offset`, `spline_contrast`, `knee_offset`, `reinhard_contrast`, `percentile`, `smoothing_period`, `scene_threshold_low`, `scene_threshold_high`, `black_cutoff`, `film_strength`, `mastering_clip`.
- `tone_mapping_tuning` is not a key: it is the name logged for this group.

Output-policy keys (the migrated file's inputs):

- `output_presentation`, `output_range`.
- `output_gamma` (full range) or `output_transport_gamma` (limited range).
- `target_primaries` / `sdr_target_primaries`, `report_bt2020_to_display`.
- `sdr_adjust_gamma`, `sdr_input_transfer`, `hdr_tone_map_target_gamma`, `sdr_lut_input_gamma`.
- `diagnostic_allow_limited_g22`, `diagnostic_allow_full_g22`, `diagnostic_vp_owned_dxgi_presenter`.

`combined_hdr_lut` is not read anywhere (planned).

### User-visible fallbacks / notices

- The planned entry `Tone map: libplacebo (combined LUT missing)` is not implemented (there is no combined LUT to miss).
- AUTO Limited that cannot be declared switches to the composed Full/sRGB swapchain. This is log-only ("AUTO Limited unavailable; recreating the stable composed Full/sRGB swapchain"), with no "Fallbacks engaged" entry.
- An unsupported or rejected output request (for example Limited with gamma 2.2 while the experiment is off) renders on the standard Full/sRGB contract. Also log-only ("fallback_policy=Full/sRGB").
- An unsafe output contract stops presentation until a retry succeeds. The reason is shown only through the output-contract status (`GetOutputContractStatus`) and the log.
- `calibration LUT` (a separate slot) does register under "Fallbacks engaged".

### Dependencies to lift it

`LibplaceboOutputPolicy.*`: C++ standard library and `pch.h` only. It is compiled into VideoProcessor-Lib, x64 only. The forwarding header src/VideoProcessor-Lib/vprenderer/LibplaceboOutputPolicy.h keeps old includes building until M2. The DXGI / NvAPI side stays in the renderer.

### Test it standalone

- `LibplaceboOutputPolicyTests` (namespace `Tests`, LibplaceboOutputPolicyTests.cpp): CPU only. Set `VP_DISPLAY_SNAPSHOT_TEST_LOG` only to capture an optional display snapshot.
- `LibplaceboLutParserTests` also uses the policy, but it runs D3D11 (WARP unless `VP_TEST_GPU=hardware`); run it only when GPU tests are allowed.

Run single-threaded:

`vstest.console.exe x64\Release\VideoProcessor-Test.dll /Platform:x64 /TestCaseFilter:"FullyQualifiedName~LibplaceboOutputPolicyTests" /Logger:"trx;LogFileName=tone_gamut.trx"`

## Cost

This slot's code is **Bill's** (upstream `v1.4.02-beta`). It stays at its path and VP does not change it
(owner principle 2026-10-24). The notes below describe how VP drives it and what that costs. Changes
inside Bill's files are suggestions for Bill, or for a VP-V adapter around them (M2 plan, option (d)).

Efficiency review 2026-10-24 (code reading only; estimates from pixel counts x operations, not measured). Ranked by saving. The policy itself adds no GPU pass; the tone/gamut mapping cost is libplacebo's (peak detection off = no peak-detect compute) and is not costed here.

1. **Rendered-output capture** (screenshot, or the problem report's thumbnail with `report_thumbnail` on; per event, render thread). `CaptureRenderedOutput` creates a full-size staging texture, then `CopyResource` + `Map`, which waits for the GPU to finish the frame. On the same thread it then runs `AnalyzePackedR10` at step 1 (2560 x 1600 = 4.1 M pixels x 3 channels: min, max, sum, buckets), an FNV-1a over all 16 MB byte by byte (16.4 M iterations) and SHA-256 of the same 16 MB. Only the PNG and JSON writes go to the capture worker. Estimated 30-80 ms render-thread hitch per capture at 2560 x 1600 (stats ~10 ms, FNV ~15-20 ms, SHA ~10-40 ms, plus the GPU wait), about 2x at 4K. Fix: move the stats, FNV, SHA and the JSON into the existing capture worker (the pixels are already moved there). Byte-identical files: yes. Risk: low. Removing the GPU wait as well (map the staging copy a frame later) is a further option, risk medium.
2. **Per frame** (render thread): `ResolveCalibrationTransfers` with three `ParseGamma` string comparisons on settings strings, `reason` strings assigned from literals (heap allocations), the copy into `lastSdrGammaDecision`, and an `std::ostringstream` signature (11 fields, `.str()` twice) that only detects a change for the `SDR_GAMMA` log line. ~3-8 µs per frame, ~0.2-0.5 ms CPU per second at 60 Hz. Fix: parse the three gammas when settings apply (initialise already parses `output_gamma`), recompute the decision only when its inputs change, and compare a small POD key instead of the string. Byte-identical: yes. Risk: low.
3. **Output diagnostic readback** (only with output diagnostics on): once per output initialise, after 30 frames, up to 10 attempts. Each creates a full-size staging texture, maps it synchronously (GPU wait plus a ~16 MB copy) and runs `AnalyzePackedR10` at step 8. Diagnostics only, per event; no change.
4. **`MakeSdrOutputContract` / `MakePlan` / `Finalize` / `ShouldFallbackToComposed`**: per initialise and per negotiation retry (once a second while unsafe); a few branches. No cost. `GetOutputContractStatus` (`ParseGamma`, `DescribeLimitedTransfer`) runs only when the GUI asks.

Cost when off: there is no off. Per frame the policy costs a few µs of CPU (item 2) and nothing on the GPU.

## Settings

- Existing keys: `tone_mapping`, `gamut_mapping`, `peak_detection`, the tone tuning keys (logged as `tone_mapping_tuning`; see above), and the output-policy keys above
- Planned keys: `combined_hdr_lut`
- Tiers: Classic/Low may use the combined LUT; Mid/Upper libplacebo

## Fallbacks

- "Fallbacks engaged" entry: Tone map: libplacebo (combined LUT missing) (planned; not implemented)
- Behaviour: Missing/invalid combined LUT -> libplacebo tone mapping (planned). Today: AUTO Limited -> composed Full/sRGB swapchain, and a rejected output request -> Full/sRGB; both log only.

## Net fingerprint / upstream

- Fingerprint: (none)
- Upstream modules: `scaling.output`

## How to build / test

Build the solution via the shared build-slot runner (see AGENTS.md); tests single-threaded; GPU tests only on mains with `VP_TEST_GPU=hardware` (`VP_TEST_ADAPTER=NVIDIA` picks the GeForce).

Placeholder tests:
- (none yet)

Isolation tests:
- `LibplaceboOutputPolicyTests`
- libplacebo gamut probe (the wide-gamut probe in `LibplaceboLutParserTests`; GPU)

Contract tests:
- combined LUT must output linear light or teach compensation the display response (ADR-0013)

Chain tests:
- LED + Stage 13 + LUT + black level combo

## Known issues / open items

- combined LUT vs LED compensation catch (decide when building the LUT option)

## Where the code lives (live file list)

Code location: **legacy**. The authoritative list is `live_files` in [MANIFEST.json](MANIFEST.json).

- `src/VideoProcessor-Lib/vprenderer/LibplaceboRenderParameters.cpp`
- `src/VideoProcessor-Lib/vprenderer/LibplaceboOutputPolicy.cpp`
- `src/VideoProcessor-Lib/ToneMappingTuning.h`

## Migration directive

Code moves into this folder only when this slot is touched (opportunistic until M1, a dedicated phase after M1, finished by M2). A move = `git mv` + project/include updates + this module's `live_files` + `docs/design/components-master.json` (regenerate with `python tools/check_module_manifests.py --write`) in the SAME commit, with the build, this module's tests and the chain-gate subset passing. Never leave a copy behind; one module per change; never during a release/deploy window. Update this README in the same commit as any slot change.

## Links

- Design: [VP-pipeline-final.md](../../../../../docs/design/VP-pipeline-final.md)
- Slot data: [slots.json](../../../../../docs/design/slots.json)
- Decisions: ADR-0013, ADR-0002, ADR-0016 in [docs/decisions](../../../../../docs/decisions/README.md)

<!-- vpv-settings-tests:begin (generated by tools/vpv_tests/seed_module_readmes.py) -->

## Settings

- Keys: `tone_mapping`, `tone_mapping_tuning`, `gamut_mapping`, `peak_detection`.
- Planned keys (not yet implemented): `combined_hdr_lut`.
- Tiers: Classic/Low may use the combined LUT; Mid/Upper libplacebo.
- VP-V page, Colour tab: not tiered (libplacebo does the colour in every tier; the cheaper combined HDR->SDR LUT for Classic/Low is not yet implemented.).
- Plain-language guide: [docs/settings/VP-settings-reference.md#6-colour](../../../../../docs/settings/VP-settings-reference.md#6-colour).
- Fallback entry ("Fallbacks engaged"): `Tone map: libplacebo (combined LUT missing)`.

## Tests

- Placeholder (byte-identity on/off, zero cost): n/a.
- Isolation (fixed inputs, expected outputs, cost): libplacebo gamut probe.
- Contract (formats, geometry, fallback entry): combined LUT must output linear light or teach compensation the display response (ADR-0013).
- Chain (total delay at the 4/3/2-frame budgets): LED + Stage 13 + LUT + black level combo.
- Combinations: pairwise / high-risk / random lines in `tests/vpv/` that set this slot's unit (`tools/run_combinations.ps1`); matrix: `docs/design/slot-test-matrix.md`.

<!-- vpv-settings-tests:end -->
