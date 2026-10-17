# Planner answers: Dolby Vision next steps (2026-10-17)

Owner relayed the planner's answers to docs/audit/planner-brief-dv-next-2026-10-17.md. Summary:

1. **Order:** (a) live verification first (cinema session: levels per title, tunnel survives
   Vertex2 + DeckLink, R10 gate, CRC pass rate, black fallbacks) - the only DV item before the deploy;
   then (f) the format-neutral metadata layer; then (d) L1 into the LED/CNTR look-ahead; (b) a VP-owned
   curve only if the projector shows a shortfall (shadows / L1 min likely); (c) trims after (b), on
   content that carries them, owner's eye as judge; (e) more devices when one needs it.
2. **Certification:** a non-certified reimplementation is acceptable for a private system; never call
   it Dolby Vision compliance (wording: "VP's interpretation of DV metadata"); every DM-derived behaviour
   behind its own switch, off until judged on the projector; lean on L1 (facts about the picture), treat
   L2/L8 trims as an experiment (intent for other displays).
3. **Tone curve:** libplacebo's curve informed by L1 stays default; a VP-owned curve only as an option
   behind the same "metadata -> curve parameters" interface (a profile key, not a rewrite).
4. **DV + LED:** L1 is the plan for the look-ahead (dump, floor, early ramps, mapped through the actual
   curve); the PRE_OUTPUT measurement stays the feedback; without L1 the queued-frame statistics as
   today; control law unchanged.
5. **Structure:** a format-neutral **scene descriptor** (max/avg/min, optional trims, mastering
   primaries/peak, source-of-truth tag; pluggable parsers: DV tunnel, DV2, HDR10+ VSIF, static HDR10,
   LLDV static, measured) consumed by the tone curve, LED look-ahead, Stage 12 and stats; a **display
   profile** (peak, black, EOTF, gamut, does-its-own-DM); a **mapping policy** (curve implementation +
   descriptor fields). DV2 = parser + descriptor extension; a new display = a profile.
6. **Capture card must guarantee:** bit-exact 8-bit RGB 4:4:4 at 4K 23.976-60 (untouched LSBs; no
   scaling / range / colour conversion / dither / enhancement); 10/12-bit YCbCr 4:2:2 at 4K 23.976-60
   (HDMI 2.0b); infoframe access through the SDK (static HDR, DV VSIF, HDR10+ VSIF); PCIe Gen3 x4+; low
   constant latency; documented SDK with timestamps. Bench-verify a synthetic tunnel through a candidate
   with the stream recorder before buying.
7. **Live qualification:** titles - profile 7 disc, profile 8.1 disc, Apple TV DV (5 / 8.4), HDR10,
   HDR10+, SDR-in-DV; TV-led and LLDV where offered; 30 s recordings each. Gates:
   `dv_tunnel_gpu_unpack` on after 0 VP CRC failures, 0 black fallbacks, GPU bit-identical to CPU on
   every recording, same levels log on both paths (30 min live per title); `dv_trims` on only after a
   side-by-side on a title with 100-nit L2 trims across highlights / skin / shadows, default off unless
   the owner prefers it on all three; a chain-matrix axis "DV tunnel on" with the recordings as fixtures.
