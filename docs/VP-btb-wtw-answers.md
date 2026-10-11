# BTB/WTW preservation — planner answers

2026-10-11. On `VP-btb-wtw-investigation.md`. The investigation is sound
and the measurements settle what was guesswork: the owner's SDR chain
(BT.709 → P3, 1080p upscaled) loses BTB to linearisation and WTW to the
sigmoid, and the carry prototype restores both at ≤1 code of change on
in-range content. Approved with the scope cuts below, which remove the
only piece of real colour design from the plan.

## Decisions

1. **Build the carry + VP-owned SDR conversion as proposed**, behind
   `preserve_extended_range` (`on | off`); `off` is today's exact path and
   is the regression reference. Default `on` once the cinema check (§
   Verification) has passed; `off` until then.

2. **No gamut compression. Scope the VP conversion to BT.709 → P3 only.**
   BT.709 lies inside P3, so a pure matrix produces nothing out of gamut
   and the measured code tables are the whole story. SDR BT.2020 sources
   are rare and, when one appears, the switch turns itself off for that
   source (log once) and libplacebo's perceptual mapping is used as today
   — losing BTB/WTW there is accepted. This removes §4.1 entirely.

3. **HDR/LLDV: out of scope, permanently.** Below-black in PQ is a few
   thousandths of a nit and above-white is what the tone map compresses by
   design; there is nothing to preserve. The libplacebo path stays for
   HDR, and the switch is SDR-only. §4.2 closes.

4. **Transfers:** the VP conversion supports pure-power source and target
   gammas (the owner's 2.2 in / 2.2+boost out) with sign-preserving
   `sign(v)|v|^g`. For any other `sdr_input_transfer` the switch turns
   itself off for that source (log once). Add BT.1886 only if the owner
   ever selects it.

5. **LED compensation shader: sign-preserving fix now.** One line; the
   module guide §6 updated; a GPU test with code 40 and code 1019 greys
   through the hook with gain ≠ 1.

6. **Alpha-channel ownership must be documented.** The carry owns alpha
   from `PL_HOOK_RGB` to `PL_HOOK_SCALED` and drops it there; the
   occlusion mask (M1′) owns alpha from the native OUTPUT hook to the
   sigma-delta. They never overlap; write that in both shaders' headers
   and in the hook-chain notes.

7. **Hook order (§4.5) is a Code-auditor check:** carry before NLS at RGB;
   restore at SCALED before LED measurement and the PRE_OUTPUT texture
   hooks; `OutputEncodingRepair` unchanged.

8. **No libplacebo fork.** Keep the §5 note as the fallback.

## Order

After Stage 6 as proposed, as its own stage ("Stage 10 — extended range"):
10a hooks + switch + GPU tests from the probe tables (both switch states);
10b LED shader sign fix (can land with 10a). Nothing for 10c.

## Verification on the projector (one short session, SDR source)

- **WTW:** a 235→255 ramp (8-bit codes) at the output: with the switch
  on, the ramp must continue above reference white; with it off, it must
  flatten at 235 on upscaled content. WTW is the valuable half: the
  projector has large headroom above code 235 at CNTR 50, so these codes
  are genuinely brighter on screen.
- **BTB:** a PLUGE (bars at 8-bit codes 4, 12, 16, 20). Whether the bars
  below 16 are *visible* depends on the projector's own handling of
  sub-black at `input level: video` and brightness 50; if the FL35 clips
  them to black, BTB preservation is correct but invisible, and that
  should be recorded so nobody chases it later. If they are visible, the
  owner has his black-jacket detail back.

## Tests

- The probe tables in §1 and §3 as regression fixtures, both switch
  states.
- In-range content: max 1 code, mean ≤0.03 versus `off`, on the bar
  patterns at up 1.5 and down 0.5.
- Switch auto-off on SDR BT.2020 and on non-power transfers, with the log
  line.
- LED shader: code 40 and 1019 greys preserved through gain 1.3 and
  gamma boost.
