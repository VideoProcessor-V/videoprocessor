# Audit — delta6 fixes, Stage 8-1 / 8-2, Stage 10 restore fix, RS-232 connection test

Range 0caca534..0439818f: 7dc2086d, 64c6cedc, dad7adf3, 0439818f.
The other Claude session was idle throughout; the checkout stayed clean.
Build: OK — full x64 Release /t:Rebuild at 0439818f, 0 errors. VideoProcessor-BetaTests built: OK.
Tests:
- native 2536 total, 2534 passed, 2 failed (ShellStylePlacementWithoutSizingHintsIsNotReverted,
  SameSizeEditWithRestoredWriteTimeInvalidatesCache — both known). New tests pass: AdaptiveDownscaleBlendsEdgesTextureAndGrain,
  ExtendedRangeFirstFrameAfterOtherFrames and the other ExtendedRange tests, DownscaleLightIsAScalingChoiceForDownscalingFrames,
  KeyProfileChoiceLastsUntilTheRulesChange, ConnectionTestPassesFailsAndExplains, plugin API test (27).
- VideoProcessorConfigTests 103 passed, 2 failed (the two known RPC tests).
- VideoProcessorBetaTests (377): 4 of 4 PASS lines.

## Blockers
- None.

## Should fix
1. **RS-232: a renderer created during the test runs its LED drive as a dry run.**
   - `SuspendLedDriveForTest` marks only the renderer object it was called
     on.
   - If VP creates a new renderer during the ~8 s test (renderer restart,
     capture restart, a signal arriving, an Apply that restarts it), the new
     renderer starts its own LED drive. That drive cannot open the COM port
     the test holds, so it logs "continuing as a dry run".
   - `OnProjectorTestDone` then calls `ResumeLedDriveAfterTest` on the new
     renderer, where `m_ledDriveSuspended` is false, so it does nothing. The
     LED drive stays a dry run, silently, until VP restarts.
   - Fix: hold the test state at the dialog level (a new renderer starts its
     LED drive only after the test), or restart the LED drive in
     `OnProjectorTestDone` whenever a test ran.
2. **RS-232: Config falls back to opening the port itself on any RPC failure,
   not only when no VP is running.**
   - The fallback also runs on an RPC timeout, a VP that does not answer,
     and VP's own error "A projector test is already running".
   - Its result always says "(tested by Config; VideoProcessor is not
     running)".
   - If VP's LED drive holds the port, it fails cleanly but with a
     misleading message.
   - If VP is running but not holding the port (LED drive off or
     log_only), Config drives the LED while video may be playing. That
     bypasses VP's playback refusal.
   - Fix: fall back only when the local connection is refused (nothing
     listening on the RPC port); otherwise show the RPC error.
3. **Stage 8-2: the gate is not the SSIM shader's own condition.**
   - The measure hook switches on when the picture at PRE_KERNEL is taller
     than the destination. SSimDownscaler's `//!WHEN` is
     `NATIVE_CROPPED.h > POSTKERNEL.h`, the source's own height.
   - These differ after a LUMA prescaler. A 1080p source on the 1600-line
     output: FSRCNNX/ArtCNN (output > 1.3 × source) doubles it to 2160, and
     libplacebo then downscales 2160 → 1600. The blend runs, but SSIM does
     not.
   - The blend then mixes the faithful ewa_lanczossharp branch into the
     plain Mitchell result, which changes 1080p upscaling, not just 4K
     downscaling.
   - Fix: in the renderer, gate on the source crop height against the
     destination height (as SSIM does), or have the measure hook use the
     native height.

## Notes
- **Stage 10 restore form (Prepare).**
  - `Prepare` runs after the user shaders, the Stage 8 hooks, NLS and the
    occlusion mask are in the list. So it sees every PRE_KERNEL /
    POST_KERNEL hook. The 6d cache and the encoding repair, added later,
    are not kernel-stage hooks.
  - The trigger is list membership: whenever SSimDownscaler / SSimSuperRes
    or the Stage 8 hooks are configured, the texture restore is used, even
    on frames where their `//!WHEN` is false. On those frames it costs the
    half-float storage (up to ¼ code) that the inline form avoids.
  - Hooks outside the trigger have not been tested with the inline restore:
    texture-output user shaders at LINEAR / SIGMOID / SCALED, and NLS at
    MAIN. Owner's case: NLS scope profile + Stage 10 + SSIM.
  - If libplacebo rejects the restore, the existing guard turns Stage 10
    off until restart (logged), possibly after one frame with unconverted
    primaries. A GPU probe of NLS + carry + SSIM would settle it.
- **Restore texture:** recreated on a size change and freed in
  `RetireOutput` before the device. `Hooks` has no destructor, but
  `RetireOutput` is the only teardown.
- **Dead band 0.0023.** BTB/WTW within 2 codes of the legal range (codes
  62–63, 941–942) still clip as today. Out-of-range pixels reach user
  shaders premultiplied by 1 + o; measured within 2 codes after
  measure + SSIM + blend.
- **AdaptiveDownscale:**
  - Resources are freed in `RetireOutput` before the device, recreated on
    a size change, and released on a GPU change.
  - The read-back is non-blocking (`pl_buf_poll` with timeout 0), with one
    outstanding read; stats are taken on downscaling frames only.
  - The grain state advances once per source sequence. Source-path extras
    re-run without advancing; the 6d extras do not run the hooks at all.
  - The keys are validated (input group), fingerprinted and live-applied
    as one `changed` entry, with a Config card. `edge_hi ≤ edge_lo` is
    corrected silently (+1).
- **Sigma-delta kept last.**
  - Sigma hooks are found by their `levels_full` parameter (as D5-1) and
    moved to the end of the list only if another OUTPUT hook follows them
    (logged once).
  - The occlusion mask is inserted before the sigma-delta afterwards, and
    the repair at the front.
  - The LED compensation and the 6d cache are PRE_OUTPUT, so they are
    unaffected.
- **Delta 6 fixes:**
  - "shader rule manual" is still never cleared, except at restart (not
    addressed).
  - `ToggleKeyOverride` assumes only the key changes fullscreen / video
    only. Changing it with the main window's control in between makes the
    entry wrong.
- **RS-232 timing:**
  - The worker has no timeout of its own. A hung COM driver would leave
    the LED drive suspended, and further tests refused ("already running"),
    until it returns. The RPC side times out at 50 s.
  - Video that starts during the test plays at LED 60 without
    compensation for up to ~8 s (the refusal is checked only at the start).
  - `Stop()` runs on the UI thread, about a second at most.

## Verified OK
- **Delta 6 fixes:**
  - Per-group release: a key override ends only when its own group's rules
    result changes; unresolved rules release nothing. Persistence true
    keeps live overrides, as before (tested).
  - `use_frame_queue` live apply restarts the renderer when the box
    changes.
  - The display-rule auto key clears its entry. Fullscreen and video only
    toggle their entry.
- **Stage 8-1:** `downscale_light` (linear | gamma) is validated,
  fingerprinted, live-applied and has a Config control.
  `disable_linear_scaling` is set only on frames that downscale on either
  axis, logged on change. The extras reuse A's parameters.
- **Stage 8-2:**
  - Measure at PRE_KERNEL (returns SIG_NONE), blend at POST_KERNEL after
    the SSIM passes.
  - `w = w_base × (1 − edge) × grain`, luma-only by default, alpha from the
    faithful branch (the carry). The GPU test covers edges, texture, grain
    ramp and cut snap in both light domains, with a clean log.
- **Stage 10:**
  - The carry rides as alpha = 1 + o, so premultiplied input to user
    shaders is unchanged for in-range pixels.
  - The texture restore after kernel-stage texture hooks avoids
    libplacebo's "resizing non-resizable stage" rejection (root cause
    measured). The inline form is unchanged elsewhere (edges max 1, mean
    ≤ 0.01).
  - The 6d cache returns SIG_NONE.
- **RS-232:**
  - The test always tries to put the light back up (the high step runs even
    after a failed low step). The LED drive is stopped first, which
    restores full light and closes the port.
  - It restarts from the latest saved configuration: an Apply during the
    test is held and used at resume.
  - Refused while a frame was rendered within 2 s. A second request while
    one runs is refused.
  - The worker hands its result through a shared object, so a VP closing
    during the test cannot write to a destroyed window.
  - Plugin API 27 (header and test). Remote targets never fall back.
