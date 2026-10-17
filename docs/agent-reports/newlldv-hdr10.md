# newlldv repurposed: BT.2020 without an HDR flag is HDR (LLDV and HDR10)

Branch `feat/newlldv-hdr10` (fork), based on `merge-v1.4.02` 70d668d7 plus the
CI workflow branch `ci/hardware-skip`. Owner decision 2026-10-17: the HDFury
Integral 2 should never need its manual HDR toggle. With the toggle off it drops
only the HDR (DRM) InfoFrame; the AVI InfoFrame still says BT.2020.

## Behaviour

With `[general] newlldv: true`:

- A BT.2020, 10/12-bit signal with no HDR InfoFrame is treated as PQ / BT.2020
  once it has been stable for 1.5 s. LLDV and ordinary HDR10 look identical on
  the wire here (no VSIF is visible), so both take the same path.
- Static metadata comes from the active `[lldv.*]` profile. Precedence:
  explicit settings (command-line `/lldv_*`, `hdr_luminance` User) > profile >
  VP defaults (MaxCLL 1000, MaxFALL 401, mastering 0.001-4000). libplacebo
  ignores MaxCLL/MaxFALL; only the mastering range bounds tone mapping, so a
  1000- or 4000-nit HDR10 grade is not clipped and peak detection sets the
  scene level as usual. The defaults therefore suit HDR10 as well as LLDV.
- Both follow modes work now: "Follow input" and "Follow input (LLDV)". The old
  requirement for both LLDV follow modes is gone.
- Never inferred for: the DV tunnel (or a profile that forces it), BT.709 or
  unknown colorimetry, 8-bit or unknown depth, a source that sends a real HDR
  InfoFrame (or a PQ/HLG EOTF), or an explicit `hdr_colorspace: REC709`.

### SDR veto (`[general] newlldv_sdr_veto`, default true)

While PQ is inferred, the capture thread samples the picture (160 x 90 grid, at
most every 50 ms, after the frame was handed to the renderer, so no added frame
latency). A picture is SDR evidence when, over the letterbox-cropped active
area, at least 0.2% of samples are at 10-bit code 880 or above, or the 99.9th
percentile is at least 870.

- A shot with 0.5 s of evidence counts. After two such shots the veto arms; the
  signal returns to SDR at the next cut (cut detector: grid and histogram
  distance between successive samples; entering or leaving black is a cut).
- At most one flip per signal epoch. A mode/rate/format change, or the signal
  leaving and returning, starts a new epoch (PQ again).
- Armed with no cut for 20 s: switches without a cut. A single uncut shot with
  continued evidence (a static SDR menu) counts as a second shot after 20 s.
- Gap: the renderer has no crossfade between PQ and SDR interpretations, so
  the no-cut fallback switches in place (libplacebo applies it as a dynamic
  state change; non-dynamic renderers restart).

## Visibility

- Log: `HDR10 inferred (no HDR flag from source; newlldv): raw=BT2020/SDR ...
  metadata MaxCLL=... mastering=...` and `HDR10 inference ended (newlldv) ...`.
- Veto log lines start `newlldv SDR veto:` (evidence per shot, armed, returning
  to SDR at a cut / without a cut). Eligibility refusals: `newlldv: not
  inferring HDR: <reason>`.
- Status: the main window's EOTF field and the published source EOTF read
  `PQ (inferred)`; a transient overlay item "HDR: PQ inferred (no HDR flag)" or
  "HDR: SDR (picture veto; no HDR flag)" appears on change.
- Rule variable `hdr_inferred` (Boolean) for `when:` profiles.
- Config: "Treat BT.2020 without HDR flag as HDR (LLDV/HDR10)" (key `newlldv`,
  unchanged) and "Return real SDR to SDR from the picture (SDR veto)" (key
  `newlldv_sdr_veto`), both with tooltips, on the LLDV profile page.

## Capture fixes

- Magewell: RGB input carries its colorimetry only in the AVI InfoFrame; VP
  read RGB BT.2020 as BT.709. `ReadAviBt2020` reads the raw AVI InfoFrame
  (PB2 C1C0 = 3, PB3 EC = 5/6) with a cross-check of the AVI pixel encoding.
  It is not compiled on CI (no SDK there) or on this laptop; the AVI byte decode
  is unit tested (`AviInfoFrameColorimetry.h`).
- DeckLink: the EOTF only updated when `GetInt` succeeded, so a PQ value could
  latch after the DRM InfoFrame disappeared. A frame without HDR metadata whose
  EOTF is unreadable now clears the latch to SDR (logged). The latch is
  confirmed in code; whether the SDK actually fails `GetInt` in that case was
  not observable without hardware.

## Tests

`src/VideoProcessor-Test/HdrInferenceTests.cpp`, 19 tests: eligibility (LLDV,
HDR10 flag dropped; tunnel, BT.709, unknown, P3, real InfoFrame, PQ/HLG, 8-bit,
unknown depth never), precedence, flag-missing sequences, early candidate loss,
veto only at a cut after two shots, one shot / short bursts never veto, PQ
programme never vetoes, no-cut fallback, static SDR shot, one flip per epoch and
new epochs, mode change during PQ, veto switch off, statistics on synthetic SDR
vs PQ re-encodings (203/100/400-nit white, 1000/4000-nit peak), thresholds and
letterbox crop, cut detector, P010 sampling, AVI decode.

`HdrInferenceRealClipVeto` (env `VP_TEST_REAL_FOOTAGE=D:\hfr`): a programme of
3 s shots cut between the seven D:\hfr SDR clips, then the same frames as
synthetic PQ. Local run: SDR vetoed at 7.5 s at the cut into shot 3 (121 of 121
frames SDR evidence); synthetic PQ 14 shots, 12 cuts found, 0 evidence frames,
stays PQ; no false cuts inside shots.

## Owner real-chain check

1. Integral 2: HDR toggle off. VP config: `newlldv: true` (veto left on).
   Restart VP.
2. Play an HDR10 title. Within about 2 s read in `C:\Videoprocessor\vp\logs\vp.log`:
   `newlldv: BT.2020 / 10-bit / no HDR InfoFrame candidate seen`, then
   `HDR10 inferred (no HDR flag from source; newlldv)` with the profile
   metadata, and `newlldv SDR veto: picture statistics started`. The EOTF field
   shows `PQ (inferred)`. No `newlldv SDR veto: armed` line should follow
   during the film.
3. Play an SDR title (output as BT.2020 by the player). It starts as inferred PQ
   (washed out for a few seconds), then expect `newlldv SDR veto: SDR evidence
   in shot 1 of 2`, `... shot 2 of 2`, `armed`, and `returning epoch N to SDR at
   a cut`; the picture becomes correct SDR at a cut.
4. If a source is 8-bit or BT.709 the log says `newlldv: not inferring HDR:
   <reason>`. On Magewell RGB the line `Magewell AVI InfoFrame (RGB input):
   ... bt2020=1 consistent=1` confirms the colorimetry read.
