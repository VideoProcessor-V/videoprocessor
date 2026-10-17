# Planner brief: Dolby Vision next steps (2026-10-17)

Context: VideoProcessor (VP) fork for a Barco FL35 RGB-laser projector (8-bit input link, ~low-hundreds
nits), RTX 3090 cinema PC, capture today DeckLink r210 via an HDFury Vertex2 (DV EDID, no scaling);
a new capture card is coming (model not chosen; must be cost-effective). Owner goal: highest fidelity,
durable for 5 years (DV2, a new display). Branch merge-v1.4.02 (fork vmcpherson2012-dev/videoprocessor).

## What VP does with Dolby Vision today (all built and tested on synthetic streams; NOT yet on a live source)

1. **TV-led DV tunnel decode (Bill's base, extended tonight):** the player hides a 12-bit IPT 4:2:2
   picture + a display-management (DM) packet in 8-bit RGB HDMI. VP finds the CRC-checked packet and
   rebuilds the 12-bit picture bit-exactly.
   - Containers: DeckLink r210, R10b, R10l (R10 GUI gate still to widen).
   - Any timing (23.976-60, 1080p/4K/odd rasters); interlaced never locks.
   - Robustness: time-based loss limits; black (never garbage) when unsure; automatic SDR exit when
     the tunnel stops without EOS (conservative "still a tunnel?" check); metadata-ID wrap guard;
     2-of-3 majority vote on damaged packets (CRC still the authority); multi-packet DM.
   - **GPU unpack** (switch `dv_tunnel_gpu_unpack`, off): CPU only reads the packet (2.6 ms -> 0.007 ms
     at 4K); GPU pass 0.41 ms; bit-identical to the CPU path on hardware; fallback to CPU, then black.
2. **Metadata use:**
   - L1 (scene max/avg) drives libplacebo's tone mapping per frame; L1 **min** now carried and shown
     (libplacebo has no input for it).
   - L2 / L8 trims parsed; an **L2 trim stage** exists (switch `dv_trims`, off; non-certified
     reimplementation: per-display trim chosen/interpolated in PQ by the display peak; CDL
     slope/offset/power on normalised PQ luminance, chroma weight, saturation gain; guards near black,
     gamut cap, soft highlight shoulder, corrupt-code rejection).
   - L5/L6/L9/L11 read, not applied. One log line per title will list which levels actually arrive.
3. **VP pipeline on DV:** source-resolution detection + re-base now run on the decoded intensity plane
   (before libplacebo's DV decode); de-judder / interpolation / sigma-delta dither run after the decode
   (not yet verified live).
4. **Not possible over HDMI:** FEL/MEL enhancement layers (combined in the player); LLDV (player-led)
   is plain PQ with no metadata - VP only substitutes static values from `[lldv.*]`.

## Decisions / questions for the planner

1. **Order of the next DV work** - candidates (effort S/M/L):
   - (a) Live verification on a real Apple TV / UHD player through the current and the new capture card:
     which DM levels arrive; does the tunnel survive the card's processing; R10 gate (S, needs hardware).
   - (b) A VP-owned, L1-driven tone curve tuned to the measured FL35 peak/black (uses the L1 minimum for
     shadows), alternative to libplacebo's curve (M).
   - (c) L2/L8 trim stage: validate look on content that carries trims; L8 needs target-display indices
     (L10 / predefined targets, unparsed) (M-L).
   - (d) DV L1 feeding the LED/CNTR dynamic-contrast look-ahead (scene max/avg known before render,
     mapped through the actual dynamic curve; measurement stays the check) (M).
   - (e) Tunnel on more devices (encoding-agnostic tunnel; Magewell only if it passes untouched RGB8) (M-L).
   - (f) A format-neutral metadata layer (DV / DV2 / HDR10+ parsers feeding one tone-mapping path) for
     future-proofing (M).
2. **Fidelity vs certification:** is a non-certified reimplementation of DV display-management ideas
   (trims, L1 curves) acceptable as the long-term path, given VP cannot license Dolby's DM?
3. **Tone curve ownership:** keep libplacebo's tone mapping informed by L1, or move to a VP-owned curve
   built for the FL35 (and future displays via a display profile)?
4. **DV + LED:** should DV metadata drive the laser/CNTR look-ahead, or only refine it?
5. **DV2 / future displays:** what structure now makes DV2 (and a DV-capable display that VP feeds
   rather than tone-maps for) a parser + profile change rather than a rewrite?
6. **Capture card criteria for DV:** what must the new card guarantee (bit-exact 8-bit RGB LSBs at
   4K 24-60, no scaling/range conversion, metadata passthrough) - and is HDR10+ capture worth requiring?
7. **Test strategy:** synthetic tunnels are bit-exact; what live qualification protocol (titles, modes,
   recordings via the stream recorder) should gate turning `dv_tunnel_gpu_unpack` / `dv_trims` on?

## References in the repo

- `docs/VP_DOLBY_VISION_TUNNEL.md` (Bill's description + "VP fork additions" sections)
- `docs/audit/dv-extension-ranking-2026-10-16.md` (12 ranked projects)
- `docs/VP-fidelity-roadmap-2026.md` (five-year roadmap, owner questions)
- `docs/audit/capture-card-readiness-2026-10-16.md` (card integration checklist)
- Code: `video_frame_formatter/CR210DolbyVisionTunnelFormatter*.cpp`, `vprenderer/GpuDolbyVisionTunnelUnpack.*`,
  `vprenderer/DolbyVisionTrims.h`, `shaders/DV trims.glsl`, `vprenderer/DolbyVisionLuminancePolicy.h`
