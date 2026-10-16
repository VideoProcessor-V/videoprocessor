# Stage 12 detection stress tests — planner review and additions

2026-10-16. The set so far (A–E) is thorough where it matters most: no
false lower reading on 42 native frames, exact on uncoded upscales, the
safe direction on coded ones, and the 16/32-px grid and starved-frame
fixes found by building D. Answers to F12 first, then the list.

## F12 — rendition-switch latency: yes, restart the window on a confident change

120 s is most of a streaming outage. Define a **rendition-change event**:
the detail-above-the-declared-rate reading falls by more than a margin
and stays down for N consecutive analyses (≈6 at 2 Hz = 3 s) with no
cut and no discontinuity. On that event, open a fresh, shorter evidence
window (10 s, time-only — a mid-shot switch has no shots to count) for
the lower reading; the usual 3-shot/20 s rule remains for the normal
case. Up-switches stay immediate (detail returns → native at once).
Test: D3 with the event — declared within ≈15 s of the switch, never on
a mere soft shot (C2 must still pass).

## Additions to the list (F1–F11 are all worth doing; these are the gaps)

### Content that is mirror-symmetric by nature (false-positive hunting)
- **G1. Periodic and symmetric content at native 4K:** zone plates,
  moiré test charts, brick walls, fences and grilles, printed text
  columns, architectural façades, venetian blinds. Coherence measures
  mirror symmetry about r; some content has symmetry of its own. The
  detector must read native on all of these (the tracker's 3-shot rule
  will usually save it, but the per-frame reading should not fire).
- **G2. Letterbox and pillarbox edges, title cards, black-bar
  transitions:** hard horizontal/vertical lines produce strong line
  spectra; confirm tiles that straddle a bar edge are excluded or read
  native. (Overlaps F7; the point here is the *edge*, not the geometry.)

### Upscales that are not one clean step
- **G3. Two-step upscales:** 720→1080 by the broadcaster, 1080→2160 by
  the Apple TV; and 480→1080→2160 for archive material. Coherence about
  two rates: the reading should be the lowest (720/480), or native if
  it cannot tell — never the intermediate.
- **G4. Anisotropic upscales with different kernels per axis** (vertical
  by a deinterlacer, horizontal by the scaler): both-axes rule must
  still find the lower rate; test with 1440×1080 → 3840×2160.
- **G5. Scope sources:** a 1080p scope disc has 1920×800 active lines
  inside 1080; upscaled to 3840×1600 inside 2160. Readings must be
  relative to the active picture (the black-bar detector's rectangle),
  not the frame, or an 800-line picture reads as 1080.
- **G6. Film-grain synthesis after the upscale** (AV1 FGS, Dolby Vision
  titles with grain added at 4K): A5 does this synthetically; the real
  case has structured grain at full 4K over a 2K picture. Must still
  read 2K (grain has no mirror symmetry).
- **G7. Upscale followed by sharpening/"enhance" and ringing suppression**
  (F3, but specifically the Apple TV's and the service's own): the
  sharpened upscale adds energy above r that is *derived from* the
  picture below r; check the detail-above test is not fooled by it.

### Time-domain behaviours
- **G8. Alternating native and upscaled shots** (a documentary: 4K
  interviews, 480p archive clips every other shot): the tracker must not
  thrash — native at once on the interviews, re-engage only after the
  3-shot/20 s rule on the archive; measure how many engage/disengage
  transitions per minute and set a cap (hysteresis) if it is more than
  one.
- **G9. Fades and dissolves between native and upscaled shots** (F5):
  the reading during the mix is neither; the tracker must wait for the
  mix to end (the dissolve detector from the cut logic).
- **G10. Long dark films** (noir, horror): most frames carry no
  evidence; confirm the tracker stays native for the whole film and
  that a single bright upscaled shot does not declare.
- **G11. Still frames and freeze frames** at an upscaled rate: repeated
  frames add nothing (C3) — confirm a 30 s freeze on an upscaled shot
  neither declares nor resets.

### Pipeline interactions
- **G12. Tile capture with the de-judder extras running:** the monitor
  must sample A (the source), never a warped extra; and with the
  rendered-warp cache, never the cache.
- **G13. Luma only:** 4:2:0 chroma is always "upscaled" by the decoder;
  confirm the detector reads luma only and that a 4:4:4 RGB capture
  reads the same as its YUV equivalent.
- **G14. Re-base and the other consumers:** with re-base engaged, the
  LED measurement, the black-bar detector, the subtitle move and the
  de-judder's matcher all see consistent geometry (the re-based size);
  E2 covers the picture, this covers the side consumers.
- **G15. Cost under load:** detection at 2 Hz while the matcher, Stage 8
  and the debander run at 4K — confirm the worker never delays a present
  (the timing gate's per-slot budget with the monitor on).

### Readings
- **G16. Effective bit depth with source dither:** a 10-bit master made
  from 8-bit with noise-shaped dither spreads codes; should read 10 (the
  dither *is* the extra bit), and the debander should stay off. A true
  8-bit source with the Apple TV's own dither to 10/12 bits: the
  occupancy test must still read 8 (the dither is temporal and small).
- **G17. Bitrate quality on clean low-detail content** (flat animation,
  a white studio): few blocks, no detail — must not read "poor" and turn
  the debander up on a picture that has nothing wrong with it.

### Real material (the F1/F2 items, made concrete)
- **G18.** Once ffmpeg is available: x264 CRF 18/23/28 and x265 CRF
  20/26/32, each at 1080→2160 and native 2160, with and without
  film-grain synthesis; AV1 with FGS. Same cases as D on real codecs.
- **G19.** Apple TV captures of three known titles: a 1080 master
  streamed at "4K", a true 4K master, and a 2K-DI film at 4K; at two
  renditions each (good and starved network). This is the test that
  decides the tooltip wording for `auto`.

## Priority

G1, G3, G5, G8, G12 and the F12 event first (they are the ones that
could produce a visibly wrong picture); G18/G19 when the tooling and
captures exist; the rest as the fixture set grows. Keep the real-footage
gate's "0 native frames read lower" as the invariant every addition must
preserve.
