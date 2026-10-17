# The low-latency path: audit and assignment proposal (2026-10-16, on battery - code reading)

## How it is decided today

"Low latency" is the **name** of the selected queue profile: `NormalizeName(queue profile) == "low_latency"`
(case-insensitive; "Low Latency" with a space or "low-latency" do not count). The renderer
(`LibplaceboVideoRenderer.cpp`, `ApplyApplicationState`) and the GUI (`VideoProcessorDlg.cpp` ~13012)
each work it out from the profile snapshot.

## Effects (audit)

| Effect | Status | Where |
|---|---|---|
| De-judder off | yes | `DejudderPolicy.h` (Reason::LowLatency), renderer releases it |
| Optical-flow producer not started / depth 0 | yes | renderer `wantProducer ... && !lowLatency`; `MotionAhead::Decide` |
| Queue frame minimum (5 for the look-ahead) exempt | yes | `VideoProcessorDlg.cpp` ~13011, `QueueLatency::ApplyFrameMinimum` |
| `fbo_float32: auto` -> on (32-bit float; owner 2026-10-16: half-float was never agreed, higher precision is the default) | no (changed) | renderer frame params |
| Grain map per frame (no matcher) | yes, **indirectly** - no explicit check; follows from the de-judder being off | `AdaptiveDownscale` uses the map only for the current frame |
| `cinema_only` custom shaders skipped | yes | `CustomHookChain::Submitted`; side effect: a cinema-only SSimDownscaler also turns the adaptive downscale off |
| Early upload (`capture_upload: early`, second D3D11 device) | **independent** - not tied to low latency; per renderer/input profile | renderer settings |

## Found

1. **Bug, fixed (this commit):** the flag was lost on every renderer Build. `Build()` makes a new
   internal state (flag false) and the GUI applies the profile snapshot *before* `Build()`, so after a
   start or the restart that switching queue can trigger, the renderer ran the full film path
   (de-judder, producer, 32-bit buffers, cinema-only shaders) until something re-applied the snapshot.
   Now kept on the renderer (`m_lowLatencyQueue`, under the state lock) and restored in `Build()`.
   **GPU confirmation pending (on mains):** a harness run starting directly in Low_Latency must show
   the de-judder off and no producer from the first frame.
2. **Name matching differs:** action rules (`DisplayRuleExpression.h`) treat "Low Latency",
   "low-latency" and "low_latency" alike; the renderer and GUI accept only "low_latency" (any case).
3. **Docs / tooltips out of step:** Config's "Low latency" queue preset writes 2 / 0 / 1 but the shipped
   `[queue.Low_Latency]` is 1 / 1 / 1 (it shows as Custom); the motion-ahead tooltip names "the Reliable
   queue (target 5)" while the shipped file calls it `[queue.Normal]`; BUILD-STATUS says capture_path
   default "standard", the code uses "gpu".
4. **Grain map in low latency has no test of its own** (it relies on the de-judder being off).

## Assigning the low-latency vs regular path by profile / rate - today

- Queue profiles, renderer profiles and input profiles are separate groups, each chosen by its own
  `when:` rule (first match in file order; the default profile's rule is never evaluated) or a shortcut.
  Nothing links them: the same rule has to be copied into each group by hand.
- Rule variables include `source_rate` (the signal rate, truncated: 59.94 -> 59), `cadence` (exact),
  `width`, `height`, `actual_refresh`, `eotf` ...; ranges work (`${source_rate} == 48-61`).
- The shipped file has **no rules** on the queue profiles: Low_Latency is reached only by Shift+L.
- Config's rate switch writes only `${cadence} > N` (default 59 - misses 50 Hz).
- The rate is the **signal's**: film carried in a 60 Hz signal (3:2) matches a 50/60 rule.

## Proposal (for the owner's decision)

1. **A property, not a name:** a queue key `low_latency_path: on|off` (default: on for a profile named
   Low_Latency, so existing files behave the same); the renderer and GUI read the property; the
   name test kept only as that default. One matching function everywhere.
2. **One visible choice on the Queue page:** per queue profile, "Use VP's low-latency path" with what it
   does (de-judder off, no frames ahead, cinema-only shaders off, early upload on; 32-bit float buffers kept)
   and a rate rule that accepts ranges (e.g. 48-61, both 50 and 60 Hz families) - prefilled for 50/60.
3. **No duplicated renderer rules:** `capture_upload: auto` (new default) = early upload in the
   low-latency path, render-thread upload otherwise; likewise any other renderer setting that should
   follow the path reads the path instead of a copied rule.
4. **Later:** a content-rate rule variable (from the pulldown detector) so film in a 60 Hz signal can
   stay on the regular path.

Decisions needed: (1) property vs keep the name, (2) early upload automatic in low latency once it
passes the four-way test (early upload x producer), (3) the default rate rule (50/60 Hz -> low latency?).
