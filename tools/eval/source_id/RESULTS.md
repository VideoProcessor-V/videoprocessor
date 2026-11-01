# Source identity: streaming-suite evaluation, result of the first run

Produced on branch feat/source-identity with `make_abr_reels.py` (122 reels: 52 no-switch controls, 5 suite congestion cases, 65 spliced
switches) and `SourceIdStreamingEvalTests` + `replay.py`, CPU only. Settings: monitor cadence 2 analyses/s, detector windows 1.0 s,
confirm 0.5 s, minStep 0.20 (PipelineSettings defaults).

## Summary (honest reading)

* **Recall is low (major 9 of 77 splices, 0.12; drops 0.21, rises 0.03).** Precision 0.55 (11 of 20 events match a splice). When an
  event fires it is on time: latency median 0.58 s, p95 1.08 s, boundary error median 0.08 s.
* **Why:** the frame-level signal only has two parts that can move at a rung switch, and neither exists on most of this material.
  (1) The delivered-resolution term needs the classical monitor to read the rung's upscale per frame; on these 960x544 crops of
  bicubic-upscaled 720p rungs of 4-8 s clips the classical tracker never declares a lower ratio (no engage in any lane on any reel,
  including constant 720p), so the term stays 1. (2) The relative noise term needs noise to lose: it moves on noisy content
  (crowd_run, tos at 2160p, one tos AVC) and abstains on clean masters, animation and film (flat-area sigma < 0.25 codes).
  A direct check of a third candidate (fine-band energy) shows the 720p rung has MORE fine energy than the 1080p rungs (upscale
  ringing; ratio 1.05-1.7) and 1080p 6000 -> 3000 changes it by < 2 % on film: no usable relative signal there.
  NR metrics are weak (findings-streaming.md); this run confirms it for rate-only and resolution switches on clean sources.
* **False alarms on no-switch controls:** 5 ABR events in 0.095 h of control stream (all in the first 3 s of crowd_run reels, a pan
  that changes the noise level), 0 consensus changes after the first 10 s, **0 one-time notices**.
* **False engages on clean high-bitrate sources: 0 of 19 top-rung controls** engaged with consensus off and 0 with consensus on
  (0 of 52 controls in either lane); consensus suppressed or added no engage.
* **Notices and latch:** none fired; the consensus latched no lower ratio or tier on any switch reel, because the classical
  resolution voter never declared (above) and a detector event alone only lowers the event-anchored level.
* **Starved shots / rung ordering:** not measurable (the signal saturates at 1.0 on most streams; tos shows an INVERTED ordering
  because the 2160p rungs read lower than 720p). Do not use the frame-level quality as a rung or starvation score.

## What this means for the product

The plumbing (P14 held state, view, fallbacks, deferred rebase, gate) is exercised and safe: off is unchanged, a fresh record can
only move the engage towards native, and nothing false engaged. The ABR DETECTION value is NOT demonstrated on this suite. Next
steps that need new evidence rather than tuning: full-raster rung files (not 960x544 crops) and clips of 30 s or more so the classical
tracker can declare the rung; real service captures (the StreamRecorder recordings have timing only); a signal for rate-only switches
on clean masters (none found: noise and fine-band energy both fail).
## Per reel

| reel | kind | events | result |
|---|---|---|---|
| crowd_hevc_gop2_cbr_const_1080p_6000 | control | 0 | - |
| crowd_hevc_gop2_cbr_const_1080p_3000 | control | 0 | - |
| crowd_hevc_gop2_cbr_down_q_1080p_6000_to_1080p_3000 | down_q | 0 | major drop MISS |
| crowd_hevc_gop2_cbr_up_q_1080p_3000_to_1080p_6000 | up_q | 0 | major rise MISS |
| crowd_hevc_gop2_const_1080p_6000 | control | 0 | - |
| crowd_hevc_gop2_const_1080p_4500 | control | 0 | - |
| crowd_hevc_gop2_const_1080p_3000 | control | 0 | - |
| crowd_hevc_gop2_const_720p_1500 | control | 0 | - |
| crowd_hevc_gop2_down_res_1080p_6000_to_720p_1500 | down_res | 0 | major drop MISS |
| crowd_hevc_gop2_up_res_720p_1500_to_1080p_6000 | up_res | 0 | major rise MISS |
| crowd_hevc_gop2_down_q_1080p_6000_to_1080p_3000 | down_q | 0 | major drop MISS |
| crowd_hevc_gop2_up_q_1080p_3000_to_1080p_6000 | up_q | 0 | major rise MISS |
| crowd_hevc_gop2_down_minor_1080p_6000_to_1080p_4500 | down_minor | 0 | minor drop MISS |
| crowd_hevc_gop2_dip_720p_1500 | dip | 0 | major drop MISS; major rise MISS |
| crowd_hevc_gop2_const_2160p_16000 | control | 0 | - |
| crowd_hevc_gop2_const_2160p_8000 | control | 0 | - |
| crowd_hevc_gop2_down_q_2160p_16000_to_2160p_8000 | down_q | 0 | major drop MISS |
| crowd_hevc_gop2_up_q_2160p_8000_to_2160p_16000 | up_q | 0 | major rise MISS |
| grain35_av1_const_1080p_4500 | control | 2 | - |
| grain35_av1_const_720p_1500 | control | 0 | - |
| grain35_av1_down_res_1080p_4500_to_720p_1500 | down_res | 0 | major drop MISS |
| grain35_av1_up_res_720p_1500_to_1080p_4500 | up_res | 2 | major rise MISS |
| grain35_av1_dip_720p_1500 | dip | 0 | major drop MISS; major rise MISS |
| grain35_av1_fgs_const_1080p_4500 | control | 0 | - |
| grain35_av1_fgs_const_720p_1500 | control | 0 | - |
| grain35_av1_fgs_down_res_1080p_4500_to_720p_1500 | down_res | 0 | major drop MISS |
| grain35_av1_fgs_up_res_720p_1500_to_1080p_4500 | up_res | 0 | major rise MISS |
| grain35_av1_fgs_dip_720p_1500 | dip | 0 | major drop MISS; major rise MISS |
| grain35_hevc_gop2_cbr_const_1080p_6000 | control | 0 | - |
| grain35_hevc_gop2_cbr_const_1080p_3000 | control | 0 | - |
| grain35_hevc_gop2_cbr_down_q_1080p_6000_to_1080p_3000 | down_q | 0 | major drop MISS |
| grain35_hevc_gop2_cbr_up_q_1080p_3000_to_1080p_6000 | up_q | 0 | major rise MISS |
| grain35_hevc_gop2_const_1080p_6000 | control | 0 | - |
| grain35_hevc_gop2_const_1080p_4500 | control | 0 | - |
| grain35_hevc_gop2_const_1080p_3000 | control | 0 | - |
| grain35_hevc_gop2_const_720p_1500 | control | 0 | - |
| grain35_hevc_gop2_down_res_1080p_6000_to_720p_1500 | down_res | 0 | major drop MISS |
| grain35_hevc_gop2_up_res_720p_1500_to_1080p_6000 | up_res | 0 | major rise MISS |
| grain35_hevc_gop2_down_q_1080p_6000_to_1080p_3000 | down_q | 0 | major drop MISS |
| grain35_hevc_gop2_up_q_1080p_3000_to_1080p_6000 | up_q | 0 | major rise MISS |
| grain35_hevc_gop2_down_minor_1080p_6000_to_1080p_4500 | down_minor | 0 | minor drop MISS |
| grain35_hevc_gop2_dip_720p_1500 | dip | 0 | major drop MISS; major rise MISS |
| meridian_hevc_gop2_cbr_const_1080p_6000 | control | 0 | - |
| meridian_hevc_gop2_cbr_const_1080p_3000 | control | 0 | - |
| meridian_hevc_gop2_cbr_down_q_1080p_6000_to_1080p_3000 | down_q | 0 | major drop MISS |
| meridian_hevc_gop2_cbr_up_q_1080p_3000_to_1080p_6000 | up_q | 0 | major rise MISS |
| meridian_hevc_gop2_const_1080p_6000 | control | 0 | - |
| meridian_hevc_gop2_const_1080p_4500 | control | 0 | - |
| meridian_hevc_gop2_const_1080p_3000 | control | 0 | - |
| meridian_hevc_gop2_const_720p_1500 | control | 0 | - |
| meridian_hevc_gop2_down_res_1080p_6000_to_720p_1500 | down_res | 0 | major drop MISS |
| meridian_hevc_gop2_up_res_720p_1500_to_1080p_6000 | up_res | 0 | major rise MISS |
| meridian_hevc_gop2_down_q_1080p_6000_to_1080p_3000 | down_q | 0 | major drop MISS |
| meridian_hevc_gop2_up_q_1080p_3000_to_1080p_6000 | up_q | 0 | major rise MISS |
| meridian_hevc_gop2_down_minor_1080p_6000_to_1080p_4500 | down_minor | 0 | minor drop MISS |
| meridian_hevc_gop2_dip_720p_1500 | dip | 0 | major drop MISS; major rise MISS |
| sintel_av1_const_1080p_4500 | control | 0 | - |
| sintel_av1_const_720p_1500 | control | 0 | - |
| sintel_av1_down_res_1080p_4500_to_720p_1500 | down_res | 0 | major drop MISS |
| sintel_av1_up_res_720p_1500_to_1080p_4500 | up_res | 0 | major rise MISS |
| sintel_av1_dip_720p_1500 | dip | 0 | major drop MISS; major rise MISS |
| sintel_av1_fgs_const_1080p_4500 | control | 0 | - |
| sintel_av1_fgs_const_720p_1500 | control | 0 | - |
| sintel_av1_fgs_down_res_1080p_4500_to_720p_1500 | down_res | 0 | major drop MISS |
| sintel_av1_fgs_up_res_720p_1500_to_1080p_4500 | up_res | 0 | major rise MISS |
| sintel_av1_fgs_dip_720p_1500 | dip | 0 | major drop MISS; major rise MISS |
| sintel_avc_gop2_const_1080p_4500 | control | 0 | - |
| sintel_avc_gop2_const_1080p_3000 | control | 0 | - |
| sintel_avc_gop2_down_minor_1080p_4500_to_1080p_3000 | down_minor | 0 | minor drop MISS |
| sintel_hevc_gop2_cbr_const_1080p_6000 | control | 0 | - |
| sintel_hevc_gop2_cbr_const_1080p_3000 | control | 0 | - |
| sintel_hevc_gop2_cbr_down_q_1080p_6000_to_1080p_3000 | down_q | 0 | major drop MISS |
| sintel_hevc_gop2_cbr_up_q_1080p_3000_to_1080p_6000 | up_q | 0 | major rise MISS |
| sintel_hevc_gop2_const_1080p_6000 | control | 0 | - |
| sintel_hevc_gop2_const_1080p_4500 | control | 0 | - |
| sintel_hevc_gop2_const_1080p_3000 | control | 0 | - |
| sintel_hevc_gop2_const_720p_1500 | control | 0 | - |
| sintel_hevc_gop2_down_res_1080p_6000_to_720p_1500 | down_res | 0 | major drop MISS |
| sintel_hevc_gop2_up_res_720p_1500_to_1080p_6000 | up_res | 0 | major rise MISS |
| sintel_hevc_gop2_down_q_1080p_6000_to_1080p_3000 | down_q | 0 | major drop MISS |
| sintel_hevc_gop2_up_q_1080p_3000_to_1080p_6000 | up_q | 0 | major rise MISS |
| sintel_hevc_gop2_down_minor_1080p_6000_to_1080p_4500 | down_minor | 0 | minor drop MISS |
| sintel_hevc_gop2_dip_720p_1500 | dip | 0 | major drop MISS; major rise MISS |
| tos_av1_const_1080p_4500 | control | 0 | - |
| tos_av1_const_720p_1500 | control | 0 | - |
| tos_av1_down_res_1080p_4500_to_720p_1500 | down_res | 0 | major drop MISS |
| tos_av1_up_res_720p_1500_to_1080p_4500 | up_res | 0 | major rise MISS |
| tos_av1_dip_720p_1500 | dip | 0 | major drop MISS; major rise MISS |
| tos_av1_fgs_const_1080p_4500 | control | 0 | - |
| tos_av1_fgs_const_720p_1500 | control | 0 | - |
| tos_av1_fgs_down_res_1080p_4500_to_720p_1500 | down_res | 0 | major drop MISS |
| tos_av1_fgs_up_res_720p_1500_to_1080p_4500 | up_res | 0 | major rise MISS |
| tos_av1_fgs_dip_720p_1500 | dip | 0 | major drop MISS; major rise MISS |
| tos_avc_gop2_const_1080p_4500 | control | 0 | - |
| tos_avc_gop2_const_1080p_3000 | control | 1 | - |
| tos_avc_gop2_down_minor_1080p_4500_to_1080p_3000 | down_minor | 1 | minor drop hit +0.6s |
| tos_hevc_gop2_cbr_const_1080p_6000 | control | 2 | - |
| tos_hevc_gop2_cbr_const_1080p_3000 | control | 0 | - |
| tos_hevc_gop2_cbr_down_q_1080p_6000_to_1080p_3000 | down_q | 1 | major drop hit +0.6s |
| tos_hevc_gop2_cbr_up_q_1080p_3000_to_1080p_6000 | up_q | 0 | major rise MISS |
| tos_hevc_gop2_const_1080p_6000 | control | 0 | - |
| tos_hevc_gop2_const_1080p_4500 | control | 0 | - |
| tos_hevc_gop2_const_1080p_3000 | control | 0 | - |
| tos_hevc_gop2_const_720p_1500 | control | 0 | - |
| tos_hevc_gop2_down_res_1080p_6000_to_720p_1500 | down_res | 1 | major drop MISS |
| tos_hevc_gop2_up_res_720p_1500_to_1080p_6000 | up_res | 0 | major rise MISS |
| tos_hevc_gop2_down_q_1080p_6000_to_1080p_3000 | down_q | 1 | major drop hit +0.6s |
| tos_hevc_gop2_up_q_1080p_3000_to_1080p_6000 | up_q | 0 | major rise MISS |
| tos_hevc_gop2_down_minor_1080p_6000_to_1080p_4500 | down_minor | 2 | minor drop hit +0.6s |
| tos_hevc_gop2_dip_720p_1500 | dip | 0 | major drop MISS; major rise MISS |
| tos_hevc_gop2_const_2160p_16000 | control | 0 | - |
| tos_hevc_gop2_const_2160p_8000 | control | 0 | - |
| tos_hevc_gop2_down_q_2160p_16000_to_2160p_8000 | down_q | 1 | major drop hit +1.1s |
| tos_hevc_gop2_up_q_2160p_8000_to_2160p_16000 | up_q | 0 | major rise MISS |
| tos_hevc_gop4_const_1080p_4500 | control | 0 | - |
| tos_hevc_gop4_const_1080p_3000 | control | 0 | - |
| tos_hevc_gop4_down_minor_1080p_4500_to_1080p_3000 | down_minor | 0 | minor drop MISS |
| crowd_hevc_gop2_switch_congestion | congestion | 1 | major drop hit +0.5s |
| grain35_hevc_gop2_switch_congestion | congestion | 2 | major drop hit +0.5s; major rise hit +0.9s; minor rise MISS |
| meridian_hevc_gop2_switch_congestion | congestion | 1 | major drop hit +0.7s; major rise MISS |
| sintel_hevc_gop2_switch_congestion | congestion | 1 | major drop hit +0.9s; major rise MISS; minor rise MISS |
| tos_hevc_gop2_switch_congestion | congestion | 1 | major drop hit +0.6s; major rise MISS; minor rise MISS |
