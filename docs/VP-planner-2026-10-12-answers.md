# Planner answers — 2026-10-12 status, Stage 8 questions, delta 6 findings

On `C:\dev\handoff\planner-2026-10-12-status.txt` and
`docs/VP-stage8-survey-proposal.md`.

## 1. Applied — acknowledged, with the corrected status

Thank you for correcting the status line to the repo's facts (7c-3 built
and audited; Stage 10 built; RC4's clamp fixed by the FL35 patches before
Stage 10). My draft was behind. Stage 6d built ahead of measurement at
the owner's request is fine: the stats line still reports the number,
and `dejudder_warp_stage: source` remains the A/B reference.

## 2. Stage 10 design change — approved

Re-encoding source → output gamma (sign-preserving) at RGB so libplacebo
linearises with its usual transfer is the right fix: the investigation
prototype's "declare the source in the output space" silently changed the
upscale look, and your numbers (edges max 1, mean ≤ 0.01 versus 5.7/54)
are the proof. Storing the offset directly in alpha rather than a base
value, because scaler weights do not sum to exactly 1, is likewise
correct. Record both in the Stage 10 notes.

## 3. Stage 8

### a) Faithful branch — A

Keep Mitchell as SSIM's base (the shader is designed to correct it) and
have VP compute the faithful `ewa_lanczossharp` branch itself from
PREKERNEL with libplacebo's sampler (~1 ms). B would make "faithful" mean
Mitchell, which is softer than the pipeline's best kernel; C changes the
assumptions SSIM was built on. A costs a millisecond and keeps both
branches what they claim to be.

### b) Config home — native rendering-profile keys

Yes. The blend is native code, the keys are per profile, and constraint
12 wants them in settings: `adaptive_*` and `downscale_light` as
rendering-profile keys on the Rendering page.

### c) Grain map on even cadence — run the matcher for it

Run the matcher for the grain map alone when `grain_map` is on and the
source is film-rate (≤ 30 fps), the same pattern as mask-only mode. 1–2 ms
per *source* frame at 4K is cheap (it is not per refresh), and the owner's
25p and 30p material would otherwise lose the per-pixel map. On the 60 Hz
display 24p is the main case and already has the matcher; this closes the
other two. Not for 50/60p sources.

And yes: use the field's existing motion-compensated match error per
trusted block as the grain measure's base, adding only the mean-removed
(high-pass) variant. No second difference pass.

## 4. Delta 6 — proposed commit approved, with two additions

- `persist_profile_selection` default **false**: yes. The rule says a key
  overrides until restart or profile change; the owner's cinema/game
  selection is by `when:` rules, so persistence of a key choice is not
  wanted. Say in the Config tooltip that today's behaviour (selections
  surviving restart) is the `true` setting.
- `stats_overlay` setting; key choices released on a profile/source
  change; the overlay "key override" line naming what a key changed;
  config keys for the two queue checks; the F2 comment fix: all yes.
- **Addition 1 — the global keyboard hook.** Single-letter keys acting
  while another application has focus is a real hazard (typing elsewhere
  toggles VP). Add `global_hotkeys: off` (default off): unmodified keys
  act only when VP has focus; modified keys (Ctrl/Shift/F-keys) may stay
  global for the remote-keyboard case. Show the setting in Config.
- **Addition 2 — DirectShow-only keys.** The PQ/Auto transfer and the
  legacy shader/display rules are madVR-path keys; mark them as such in
  the Config list rather than giving them VP-renderer config keys that do
  nothing.

Commit as "Delta 6: settings-first audit", then the audit as usual.
