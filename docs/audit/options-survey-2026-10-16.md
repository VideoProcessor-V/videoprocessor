# Options survey (2026-10-16): built features vs Config controls

Every renderer profile key added by the fork has a Config control: all `dejudder_*`, Stage 12 / source
resolution (incl. evidence keys, text guard, rebase_kernel_match, deband_vp, residual steadying,
bitrate-aware), optical flow (`optical_flow`, `optical_flow_global`, `motion_ahead_frames`), dither
(`temporal_dither`, `dither_*`, `occlusion_mask`, `fbo_float32`), adaptive scaling (`adaptive_*`), capture
(`capture_path`, `capture_upload`, `source_range`, `native_rgb_ingest`, `[decklink] ycbcr_12bit_capture`),
SDR tone (`sdr_*`, `preserve_extended_range`). No commit in the last 150 names a key missing from both.

**Cadence / pulldown:** `dejudder_pulldown` ("Find film sent with repeated frames", de-judder section,
experimental, default off). Its tunables are fixed constants in `vprenderer/PulldownDetector.h` (lock
after 3 cycles and 2 s, unlock after 3 contradictions in 1 s, 64-row signature, film >= 15 Hz, history
2048, max cycle 1 s, 0.5% signal-rate tolerance, FilmClock window 2 s) - no keys.

**Gaps (fork):** ~30 `[led_drive.*]` tuning keys without controls, by design (page shows core choices;
the rest from the LED profile): `fade_ms`, `fade_min_slope`, `cut_threshold`, `highlight_area_small/large`,
`highlight_release_ms`, `highlight_percentile`, `highlight_persist_ms`, `apl_persist_ms`, `ftb_black_ms`,
`tick_ms`, `rate_up/down`, `deadband`, `early_ramp`, `cut_jump`, `bright_cut_dump`, `anti_pump_band`,
`script_highlight_code`, `led_ceiling`, the step/curve tables (LedDriveConfig.h ~529-914).

**Bill's keys without controls (leave):** old subtitle-box keys (`subtitle_bbox_test`,
`subtitle_cut_paste_*`, `subtitle_box_padding_*`, `subtitle_move_inset`, `subtitle_hold_ms`,
`subtitle_near_bar_px`, `subtitle_detection_optimization`), aliases (`live_profile_updates`,
`profile_update_mode`, `sdr_target_primaries`, `calibration_lut_input_*`), main-config file-only keys.

**Notes for the Config assessment:** pulldown is off by default (24p in 60 Hz gets no de-judder unless on);
a content-rate rule variable from it would keep 3:2 film out of a 50/60 Hz low-latency rule.
