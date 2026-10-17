# Dolby Vision tunnelling - what exists and how to extend it (2026-10-16)

Read-only research for the owner ("the DV tunnelling Bill did interests me; would be interesting if
it's extendable").

## Today

- **TV-led DV tunnel:** the player hides a 12-bit IPT 4:2:2 picture plus a display-management (DM)
  packet inside 8-bit RGB HDMI. Chain: Apple TV / UHD player -> HDFury Vertex2 (DV EDID, no scaling)
  -> DeckLink r210 capture. VP finds the DM packet (CRC-checked), rebuilds the 12-bit picture on the
  CPU (AVX2, threaded), hands the DM matrices to libplacebo's Dolby decode, then VP's tone mapping.
  Once locked, a frame without a valid packet is black (never garbage); 24 bad frames drop the cache;
  EOS or a reset falls back to SDR (an SDR switch without EOS can stay black until Restart capture).
- **LLDV (player-led):** ordinary PQ HDR from the player; VP only detects it and replaces the
  Vertex2's fixed HDR block with the `[lldv.*]` profile's values - effectively static HDR10.
- **Metadata used:** only Level 1 (scene max/avg PQ) per frame into libplacebo's tone mapper; the L1
  minimum is read and dropped; L2/L8 trims, L5, L6, L9, L11 logged as "not applied". Not Dolby display
  management - libplacebo tone mapping informed by L1.
- **Limits:** DeckLink r210 only; 4K23.976 qualified; Magewell unsupported; FEL/MEL impossible over
  HDMI (combined in the player); source-resolution detection / re-base do not run on DV ("packed
  signal"); de-judder / sigma-delta / shaders run after the decode but are unverified live.

## Ranked extension projects (S/M/L)

1. **Check which DV levels arrive (S)** - read the "extension L%zu" log lines on the Apple TV and a UHD
   player; decides whether trims are possible.
2. **L1 + FL35-peak tone curve with the L1 minimum (S-M)** - scene-driven curve tuned to the measured
   projector peak/black; choice between L1 and peak detection.
3. **L2/L8 trims for the FL35's brightness (M-L, only if 1 finds them)** - parse trims, a VP GPU stage
   after tone mapping, nearest/interpolated target trim (a reimplementation, not Dolby-certified).
4. **L1 driving the LED/CNTR look-ahead (M)** - scene max/avg known before render, mapped through the
   actual dynamic curve; the measurement stays as the check.
5. **Re-base / source resolution on DV (M)** - measure on the decoded intensity plane.
6. **Live chain check on DV (S, testing)** - de-judder, sigma-delta, interpolation, shaders on a tunnel.
7. **GPU tunnel unpack (M)** - free the CPU (like the native RGB GPU unpack).
8. **More timings and packings (M)** - 50/60/24.000, R10b/R10l.
9. **Robustness (M)** - the 16-value metadata ID wrap; SDR switch without EOS.
10. **Magewell RGB8 tunnel (L, uncertain)** - only if the card passes untouched RGB8.
11. **HDR10+ (L, probably blocked)** - capture cards rarely pass the InfoFrames.
12. **FEL/MEL - not feasible** over HDMI.

Future-proofing (owner: "earn its keep through the next 5 years despite DV2, a new TV"): keep the
tunnel decoder and metadata handling data-driven (levels parsed generically, curves parameterised by
the measured display), so a new display or a DV revision changes parameters, not code paths.

Key files: `video_frame_formatter/CR210DolbyVisionTunnelFormatter*.cpp`, `IVideoFrameFormatter.h`
(`DolbyVisionTunnelFrameMetadata`), `vprenderer/DolbyVisionLuminancePolicy.h`, the renderer's dovi/L1
block (~15014-15128), `docs/VP_DOLBY_VISION_TUNNEL.md`, `docs/VP_DV_UHD_QUALIFICATION.md`.
