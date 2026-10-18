# detail_pulse_stabilise and texture_restore (agent report, 2026-10-17)

Branch `feat/detail-restore` (worktree D:\codex\videoprocessor\detail), from `integ/2026-10-17`
at f4722145. Local only (GitHub suspended). Design as built: docs/VP-detail-restore.md.

## What was built
- **detail_pulse_stabilise** (default **on**, cap 1.35, range 1.00-1.50). It lifts each frame's
  own luma detail towards the best-coded recent frame's level.
  - Two bands: fine (Y - G1) and mid (G1 - G2.5).
  - Gain per band = sqrt(reference / own energy), clamped to [1, cap], with a 1 % dead zone.
  - The reference is the best of the frames within the last 0.17 s, decaying 0.9 per second,
    plus the next frame (look-ahead) when it is already uploaded and no re-base is engaged.
  - A tile is vetoed (gain 1) where the vectors are untrusted or, without vectors, where its
    low band changed.
  - Cuts, fades and gaps restart the history.
  - Nothing is copied between frames, so grain stays per-frame.
- **texture_restore: off | statistical** (default **off**, experimental; cap 1.50, range
  1.00-2.00). It splits detail into grain and scene texture per 16 px tile.
  - The grain level comes from the tile's quietest sub-blocks; grain is the incoherent fine
    part.
  - Grain is scaled towards the reference grain level. It only scales grain the frame already
    has: nothing is synthesised.
  - Texture is scaled towards the reference texture only where a focus gate passes. Focus is
    the mid band against the low-mid band, and the gate also blocks a focus pull.
  - References follow the vectors; without vectors only unchanged tiles keep them.
- **Order:** monitor (original frame) -> re-base -> debander -> pulse -> texture restore ->
  libplacebo scaling.
  - The stages run after the re-base, not before it as in the roadmap. The kernel-matched
    inverse needs the player's signal untouched, so on a re-based source both stages work at
    the content's own resolution.
  - De-judder extras and Stage B pictures between frames take their frame's decision as a
    repeat. This avoids refresh-to-refresh flicker; the lead added it in the sweep.

## Files
- `src/VideoProcessor-Lib/vprenderer/DetailPulse.h` (188 lines), `.cpp` (800).
- `src/VideoProcessor-Lib/vprenderer/TextureRestore.h` (177 lines), `.cpp` (782).
- `src/VideoProcessor-Lib/vprenderer/LibplaceboVideoRenderer.cpp`:
  - settings at 1164;
  - members at 5131;
  - Forget/Release at 7049, 11707, 20473;
  - Params setup from about 20520 to 20640;
  - extras at 15258-15275;
  - render hook at 22093-22145.
- `src/VideoProcessor-Lib/RendererProfileConfig.h` 726-729.
- `src/VideoProcessor-Config/ConfigEditorWindow.cpp` 8845-8900 ("Experimental content-adaptive
  settings"), plus 10384 and 10396.
- Tests:
  - `src/VideoProcessor-Test/DetailPulseModel.h`, `DetailPulseTests.cpp`;
  - `TextureRestoreModel.h`, `TextureRestoreTests.cpp`;
  - `DetailRestoreConfigTests.cpp`;
  - `RendererCaptureHarnessTests.cpp` (1550);
  - `src/VideoProcessor-ConfigTests/ConfigEditorWindowTests.cpp`.

## Tests (CPU, single-threaded, TRX)
- **Targeted run: 238/238 passed.**
  - Covers DetailPulse, TextureRestore, DetailRestore, GrainPreservation, ConfigFileTests,
    SourceResolutionDetector, Hunt2PictureKeysReachTheSettings and
    EveryProfileKeyChangesTheLiveSettingsFingerprint.
  - Grain RMS stays within 0.75 % for both stages, and grain correlation is unchanged.
  - Breathing on a coded static sequence: 0.135 -> 0.043, with every frame's detail at least
    the input's.
  - Defocused regions get no added texture energy (0 clean), and a focus pull is blocked.
  - The no-vector fallbacks pass.
  - I-frame gain is 1.000.
- **Real clips (VP_TEST_REAL_FOOTAGE=D:\hfr): 2/2 passed.**

  | Clip | Breathing reduction | PSNR change |
  |---|---|---|
  | Pulse, CRF 24, causal | -46 % | -0.02 dB mean |
  | Pulse, CRF 24, with look-ahead | -60 % | -0.03 dB mean |
  | Pulse, CRF 32, causal | -43 % | |
  | Pulse, CRF 32, with look-ahead | -58 % | |
  | Texture restore | nearly inert (camera pans) | -0.007 dB |

- **Source-resolution input:**
  - The monitor's input is bit-identical with the stages on and off.
  - A source-order test pins the order in the renderer.
  - Both real models at their maximum caps leave a 1080-in-4K sequence reading 0.5001 on every
    frame.
- **Config editor tests:** detail restore controls pass. The full suite (plumbing agent) gave
  124 passed; "scene detection defaults off" crashed once and passes when run alone.

## Costs (estimates; no GPU run was allowed)

| Stage | 3050 Ti 4K24 | 3050 Ti 1080p24 | 3090 4K24 | 3090 1080p24 |
|---|---|---|---|---|
| Pulse | 0.55-0.7 ms | ~0.15-0.2 ms | 0.15-0.2 ms | ~0.05 ms |
| Texture restore | 1.2-1.4 ms | ~0.35 ms | 0.3-0.4 ms | ~0.1 ms |

- These are per source frame.
- Each de-judder extra adds a repeat:
  - pulse: about 0.3 ms at 4K on the 3050 Ti;
  - texture restore: about 1.1 ms.

## GPU tests (for Claude)
`/TestCaseFilter:"Name=DetailPulseGpuMatchesModel|Name=DetailPulseGpuTiming|Name=TextureRestoreGpuMatchesModel|Name=TextureRestoreGpuTiming|Name=DetailRestoreGpuSourceMonitorUnchanged"`

- The timing tests need VP_TEST_GPU=hardware.
- The shaders have never executed. Run MatchesModel first, then the chain gate with pulse on
  and off.

## Open points
- The pulse default (on) follows the 3090-cost rule. The research gates still listed owner
  viewing on the 3090 and G19 captures.
- Grain the encoder deleted cannot return without synthesis. That is an owner decision.
- The 0.17 s window keeps CRF 32 I-frames at 1.00, but frames more than 4 after a good frame are
  not lifted.
