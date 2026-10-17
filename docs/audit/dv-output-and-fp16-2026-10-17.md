# VP sending Dolby Vision, and the FP16 PC-levels idea (research, 2026-10-17)

> **Owner decision 2026-10-17 (FINAL):**
> - **VPV is the chosen route:** VP sends a TV-led DV tunnel itself through the NVIDIA hidden
>   mode (the "NVIDIA route" below). Built only when a DV-capable display exists; pixels proven
>   bit-exact by loopback first; needs a safe exit/recovery path (stuck-black report). The output
>   is never labelled Dolby Vision.
> - **DeckLink output card: rejected** (buying one does not make sense).
> - **FP16 half-float output (Q2): PARKED** (rounding/dither unknowns). Non-DV displays stay on the
>   10-bit 4:4:4 surface + sigma-delta. The Q2 measurement plan below is not scheduled.
>
> The research below is kept as written.

## Q1: Can VP send DV itself?

Yes, as an unofficial experiment, by one of two routes.

**NVIDIA route (experimental).**
- The bundled NVAPI header has `NV_HDR_MODE_DOLBY_VISION` (= 7, in 3rdparty/nvapi/nvapi.h ~8145-8220).
- The app supplies "RGB8 Dolby Vision encoded" frames: 12 bpc YCbCr 4:2:2 packed into RGB8 with the metadata embedded. That is exactly the tunnel VP decodes. The driver then signals DV to the TV.
- NVIDIA labels the mode "Experimental mode only, not for production!".
- A public report says the TV stayed in DV mode, showing black, after the mode was turned off, until a reboot.

**DeckLink route (clean).** The DeckLink SDK manual (March 2026) documents a Dolby Vision playback API (section 2.4.12.3):
- Each frame is 12-bit RGB with `bmdFrameContainsDolbyVisionMetadata` set, and its DM bytes are attached with `SetBytes(bmdDeckLinkFrameMetadataDolbyVision)`.
- The colourspace is `bmdColorspaceDolbyVisionNative`, with CM version and mastering configuration.
- `bmdDeckLinkStatusSinkSupportsDolbyVision` reports whether the sink supports DV.
- Desktop Video does the HDMI packing and signalling.

The DeckLink 8K Pro G2 lists DV up to 4K60. The same output module also gives bit-exact 12-bit output with BTB/WTW.

**Other routes.** None found:
- AMD AGS: HDR10 only.
- Intel: no Windows API (on Linux, libreelec-x86-DV does it with patched drivers).
- HDFury: no documented feature turns a PC tunnel into TV-led DV. Unverified; would need confirmation from HDFury.

**Pixels.** An 8-bit surface into an 8-bit link, full range, identity LUTs, fullscreen independent flip, with dithering off and no Windows HDR, ACM, Night light or ICC, is normally pass-through. Proof: loop the output back through capture and confirm a 100% CRC pass rate in VP's formatter.

**Metadata.** VP would build ETSI GS CCM 001 packets (DolbyVisionTunnelSender.h already makes valid ones).
- DV source: pass through the received DM, provided VP keeps the picture in the source domain.
- HDR10/SDR source: generate L1 from VP's own scene measurement.
- There is no Dolby SDK, licence or certification, so TV behaviour is not guaranteed, and the output must not be called DV.

**What the TV gains.** It runs its own display management tuned to its panel, often better than its HDR10 curve. A middle path: VP tone-maps to the panel and declares the panel as the source, so the TV's mapping is near identity and DV acts as a 12-bit 4:2:2 transport over an 8-bit link.

**Verdict.**
- NVAPI proof of concept: about 1-2 weeks, risky.
- DeckLink output module plus DV: weeks, plus the card.
- Defer both until a DV display is chosen.

## Q2: FP16 surface with the PC-levels trick

The idea: pre-scale the video-level signal, including BTB/WTW, into full range, run the GPU at full range, and set the display to video levels.

**Precision.** Every FP16 value was enumerated against a 12-bit full-range output.

| 10-bit code | sRGB step (12-bit codes) | PQ step |
|---|---|---|
| 64 (black) | 0.15 | 0.08 |
| 512 | 0.54 | 0.35 |
| 800 | 1.22 (about 11.7 bits) | 0.43 |
| 900 | 1.04 | 0.34 |
| 940 (white) | 0.98 | 0.24 |
| 1019 (WTW) | 0.88 | 0.22 |

- SDR (sRGB): 4010 of 4096 codes are reachable. The 86 missing codes all lie in 12-bit 3012-3660.
- Windows HDR (PQ): all 4096 codes are reachable, at least 13.2 bits.

**Sigma-delta.** If the measured mapping is exact and undithered, the sigma-delta keeps code control wherever the step is under one code. In the band it can target only reachable codes from a measured table. If the driver dithers FP16 down to the link depth, exact control is lost.

**The trick also works on today's 10-bit surface.** FP16 only adds the extra bits.

**Unknowns that need measuring.** The driver's FP16-to-link conversion and rounding are undocumented, and so is whether it dithers.

**Measurement (laptop + UltraStudio 4K Mini, loopback).**
1. Use the HDMI port wired to the NVIDIA GPU, at 1080p24/60.
2. NVIDIA settings: RGB, Full, 12 bpc. Windows HDR, ACM, Night light and ICC off. Record the driver version.
3. Test app: fullscreen FP16 G10_P709, a 64×64 grid with one tile per 12-bit code, plus ±1 ulp rows. Confirm "Independent Flip" in PresentMon.
4. Capture about 300 frames as raw R12B.
5. Analyse:
   - fit the code→code map;
   - check for temporal or spatial variance (meaning the driver dithers);
   - list the missing codes;
   - check that BTB/WTW come through intact.
6. Repeat with a 10-bit UNORM surface, an 8-bit link, Windows HDR (PQ) on, and dithering forced off.
7. DV pixel test: send an RGB8 tunnel and capture it. A 100% CRC pass rate means the path is bit-exact.
8. Repeat on the 3090.

**Effort.** Measurement about 1-2 days; implementation about 1-2 weeks. Re-measure after driver updates.

## Sources

- DeckLink SDK manual (March 2026): https://documents.blackmagicdesign.com/UserManuals/DeckLinkSDKManual.pdf
- NVIDIA forum, DV mode stuck: https://forums.developer.nvidia.com/t/cannot-turn-off-dolby-vision-hdr-mode-after-calling-nvapi-disp-hdrcolorcontrol-with-nv-hdr-mode-dolby-vision/199530
- NVAPI HDR capabilities: https://docs.nvidia.com/nvapi/struct__NV__HDR__CAPABILITIES__V3.html
- Microsoft, high dynamic range: https://learn.microsoft.com/en-us/windows/win32/direct3darticles/high-dynamic-range
- libreelec-x86-DV: https://github.com/CroqueMr/libreelec-x86-DV
- ETSI GS CCM 001: https://www.etsi.org/deliver/etsi_gs/CCM/001_099/001/01.01.01_60/gs_ccm001v010101p.pdf
