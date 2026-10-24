# Original pathway overrides: verification (settings agent, 2026-10-24)

ADR-0019 asks the settings agent to check the `slots.json` `original_pathway.settings` values
marked "verify", and the key names, against the upstream build. The reference is the
`bill-v1.4.02` worktree (9ff095fa), `src/VideoProcessor-Lib` and `src/VideoProcessor-Config`.
These are proposed corrections for the skeleton lead, who owns `slots.json`. They are not applied
here, to avoid a concurrent edit.

| Slot | slots.json says | Upstream fact | Proposed override |
|---|---|---|---|
| input.capture | `capture_upload: verify` | Upstream has no `capture_upload`; it uploads on the render thread. | `capture_upload: render` |
| input.queue_timing | `target_latency_ms: upstream default (verify)` | Upstream has no `target_latency_ms`; the queue is in frames (`target_frames`, MainConfigSchema / RendererProfileConfig). | remove the key (absent); the queue follows `target_frames` |
| motion.estimation | `motion_engine: off` | `motion_engine` takes only auto / vp / smooth_motion. VP's estimation is switched off by its own keys. | `optical_flow: off`, `motion_ahead_frames: 0` (with `dejudder_strength: 0`, which stops the matcher) |
| motion.consumers | `dejudder_strength: 0 (verify key)` | The key exists and 0 = off. `dither_vectors` takes auto / own, not off. | `dejudder_strength: 0`, `dejudder_strength_even: 0`, `interpolation: off`, `interpolation_fill_gaps: off`; drop `dither_vectors` (unused once the sigma-delta is off) |
| scaling.output | `downscale_light: upstream default (verify)` | Upstream has no key; libplacebo's default light is linear. | `downscale_light: linear` (the same as absent) |
| output.dither | `sigma_delta: off`, `dithering: upstream default (verify)` | `sigma_delta` is not a key; the key is `temporal_dither`. `dithering` exists upstream with default auto. | `temporal_dither: libplacebo`; `dithering`: leave as configured (an upstream key) |
| drive.* / projector_control | `led_compensation: off` | `led_compensation` is not a key; it is a hook rule name. | `[led_drive.*] enabled: false` (or `command_source: none`) |
| repair.texture_restore | `detail_pulse_stabilise: off`, `texture_restore: off` | correct | as is |
| colour.extended_range | both off | correct (upstream clips) | as is |
| output.calibration_lut | `calibration_lut_stage: libplacebo` | a planned key | as is (planned) |

The other entries name real keys with values the parsers accept, and need no change.

The VP-V Config "Original pathway" switch (`[vpv] pathway: original`) is the Config side. When
the renderer reads it, the overrides above are applied in the renderer, never written over the
user's values in the file.
