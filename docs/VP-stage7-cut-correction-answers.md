# Owner correction — bright cut and exit from black: planner confirmation

2026-10-11. On C:\dev\handoff\planner-bright-cut-correction.txt (the
owner's plan). Summary of the planner's reply (the full text was relayed in
the session):

- **Confirmed:** bright cut → CNTR to `cut_cntr_level` (default 59), never
  to 50 because of the cut, kept if already below; exit from black → CNTR
  raised to `cut_cntr_level` during the black, LED started early capped at
  `black_prelift_max`, VP gain prepared (7c-3); at the cut VP gain to its
  maximum with the knee rolling off what would clip; after the cut LED and
  CNTR paired, CNTR descending from `cut_cntr_level`, VP gain interpolating;
  the shaped fade-up only for the remaining shortfall.
  `DumpTarget = max(cut_cntr_level, the max_cut_dip value)`, never above the
  current value; `cut_cntr_dump: off` still holds.
- **Tightening 1:** `dmd_clip_cntr` defaults to **59**. The light-meter table
  was taken at equal white and does not establish the clip point; the
  script's crush above ≈ 60 does. `cut_cntr_level` defaults to
  `dmd_clip_cntr`; both keys kept. Assist above 59 (to `cntr_max_usable`)
  stays allowed where the picture has no whites. A fixed-LED white sweep
  sets `dmd_clip_cntr`.
- **Tightening 2:** the paired LED / CNTR move at the script-matched rates;
  the displayed-light ramp for the shortfall runs at a fast
  `recovery_rate_*`, bounded by the modelled light; the LED command on a cut
  is one command to the scene's level.
- **Policy:** §3.3, §3.5, §6 and §7 deltas applied to the repo's file (the
  planner sent no copy).

## Implementer note

- **The clip is not in the pairing's arithmetic.** For a highlight h the
  displayed light is h × F whatever the split between VP's gain and CNTR,
  so above `dmd_clip_cntr` more CNTR adds no highlight light. Feeding the
  clip into the headroom used for the CNTR need (as 7c-2 first did) makes
  each CNTR step ask for the next. Instead:
  - while the scene has highlights (VP's headroom limited), CNTR stops at
    `dmd_clip_cntr`;
  - the highlight floor uses CNTR's headroom only up to `dmd_clip_cntr`, so
    the LED stays where that suffices;
  - the fade-up's gain ceiling shrinks in proportion when CNTR is above the
    clip;
  - without highlights the assist may still go to `cntr_max_usable`.
- **`recovery_rate_*` defaults:** gain 4.0, min 20, max 400 levels/s,
  confirmed in docs/VP-recovery-rate-answers.md (which also restores the
  missing reply, docs/VP-stage7-7c2-built-answers.md). `rate_preset` sets
  both families: either preset puts `recovery_rate_*` back to these fast
  defaults; explicit keys override. Dark cuts: the paired move after the cut
  stays at the scene rates (the earlier remark that it could use the
  recovery law is withdrawn) - nothing in the code used it.
