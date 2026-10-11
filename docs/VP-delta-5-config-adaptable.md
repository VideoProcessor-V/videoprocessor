# Delta 5 — config-adaptable code (applies to every stage except the LED module)

2026-10-11. New constraint 10 in the brief (revision 15). Apply to existing
code as an audit pass and to all future work.

## Rule

Nothing outside the LED module may assume:
- limited range (16–235) or full range — read `output_range`;
- 8-bit output or a 10-bit swapchain — read the target bit depth and
  swapchain format (the sigma-delta already takes `levels_full` and
  `swapchain_bits`; make every other shader and hook read the same);
- a particular gamma (2.2 or 2.4) — read `output_gamma` /
  `sdr_input_transfer`;
- P3, BT.709 or any fixed primaries — read `target_primaries` and the
  source's colourspace;
- 2560×1600 or 60 Hz — use `{{output_width}}`, `{{output_height}}`,
  `{{refresh_hz}}` / `${cadence}`.

Everything in that list comes from the active profile or the hook-chain
templates. The LED module is the one place the owner's projector numbers
live (light-meter fit, CNTR reserve, latencies), by design.

## What to do now

1. Audit pass over existing code (hook chain, blanking, sigma-delta,
   upscalers' `//!WHEN`, carry/restore, de-judder, queue, output encoding
   repair): list every literal that falls under the rule, replace it with
   the profile or template value, and report the list.
2. Add a second run of each stage's GPU tests with `output_range: full`
   (and, where cheap, with an 8-bit swapchain); both must pass.
3. The PR-candidate pieces must pass this pass cleanly — it is what makes
   them general for the fork's other users.

Commit as "Delta 5: config-adaptable audit".
