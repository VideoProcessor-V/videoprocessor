# Settings: what changed and why (2026-10-24)

For the owner, one page, to read before clicking through the preview. Nothing was removed: every
setting is still there, once. Details: `organisation.md` (the map) and `VP-settings-reference.md`.

## Where things are now

- **VP-V page** (sidebar): everything VP adds, one tab per unit (Input, Motion, Repair, Scaling,
  Colour, Dynamic Drive, Output). Each tab has **Main** (the tier slider and the switches you might
  use) and **Advanced** (what Main sets, fine tuning, measured internals). Why: the old Rendering page
  had 177 settings in one long list.
- **VP Renderer > Rendering** now holds only Bill's settings (quality, tone mapping, processing). Why:
  his users find his settings where they expect them; ours have their own home.
- **Dynamic Drive page**: the same Main / Advanced layout. Main: on/off, what drives the LED (your
  script, compensate, none), the LED profile, the speed preset, the projector link. Advanced: what
  those set, the contrast and cut tuning, the projector's limits. The old 56-control "Advanced LED"
  card is now five small cards.
- **Proposed retirement** tab: the 39 switches proposed for retirement, still working until you
  decide. Two were taken off the list because they are still in use (render-ahead queue credit,
  motion look-ahead frames).

## How the controls work

- **One master per group.** A tier, a slider or a feature switch is the master; what it sets is on
  Advanced, greyed "Set by ...". Tick **Custom** on that card to change those by hand; untick it to
  get the master's values back. Tuning of a feature that is off says "Used only when ... is on".
- **One control per setting.** No setting has two live controls any more.
- **Default** (on every tier) = the values VP ships with. **Reset** = back to the tier you last chose.
  No tier is ever written that you did not choose; values that match no tier show **Custom**.
- **Original pathway** (Bill's pipeline): every VP addition is off, so the VP-V page and the Dynamic
  Drive page are disabled; Bill's pages work exactly as upstream.

## Tier meanings that changed

| Unit | Before | Now | Why |
|---|---|---|---|
| Motion, Classic | "no motion processing": set your de-judder amount to zero, and leaving Classic set it back to 0.25 | VP's basic motion search only (whole pixels, nothing ahead); **your de-judder amount is never touched** by any tier | Your taste was lost when you passed through Classic. For no motion processing, set the amount to *As shot*. |
| Motion, Low | matcher only | matcher at quarter-pixel precision (the only difference from Classic) | Classic and Low need a real difference now that neither sets the amount. |
| Scaling, Low | the hybrid downscale without re-base | **re-base, plain scaling** (what VP ships) | The shipped values matched no tier, so a fresh setup showed Custom and some buttons wrote Classic. |
| Scaling, Mid | re-base + hybrid downscale | unchanged | |
| Output, Low | always VP's sigma-delta | **as shipped**: the shader list decides (the cinema list loads the sigma-delta) | Same reason; a button could otherwise switch the 8-bit link's sigma-delta off. |
| Output, Mid | (none) | always VP's sigma-delta | The explicit choice moved up one step. |
| The tiers VP ships with | matched no tier on Scaling and Output | Motion Mid, Repair Low, Scaling Low, Output Low | A fresh configuration now reads as tiers, never Custom. |

## Smaller changes

- Dolby Vision trims moved to Colour; their default (and the DV tunnel GPU unpack's) is now on, as in
  the renderer.
- Expert plumbing moved to Advanced: the capture upload timing, and the SSIM / CNN steadiness switches.
- The de-judder amount's eight fields and the de-judder's other shaping (fade matching, pulldown)
  are separate cards.
- The VP-V and Dynamic Drive pages fit the default window without sideways scrolling.

## Still to decide

- Approve (or not) the 39 retirements, by group.
- The Original pathway also needs a few more VP keys switched off in the renderer (startup hold,
  compression-aware deband, the residual steadiers, motion estimate-ahead, hold-or-blend); listed for
  the renderer lead.
