# FL35 projector test tally

Everything built for the FL35 that still has to be checked on the projector.
Every feature is modular and off by default (unless noted), so each can be
switched on and judged on its own. Update the Result column as each one is
tried: **OK**, **Problem: …** (and tell Claude), or **Skip**.

Last updated: 2026-10-11. **Test folder: `C:\dev\deploy\VideoProcessor-FL35-rangefix2` (or rangefix
for everything except the dither) or newer.** Earlier folders are not to be used on the projector: from Stage 7
step 4 up to that build, the picture was output full range whenever the gamma
boost or Dynamic Drive was on (a libplacebo bug, repaired in e7b3cceb). See the
folder's DEPLOY.txt for the exact steps.

## Basics
| # | Check | How | Result |
|---|---|---|---|
| 1 | Edge blanking 10 / 15 px | F3 blanked (default), F4 full raster | not tested |
| 2 | Brightness same as VP2 / madVR | gamma boost on; log says "gamma boost on (exponent 0.9167)" | not tested |
| 3 | Saturation +5 | compare with madVR | not tested |
| 4 | Queue switches by frame rate | a 59.94 / 60p source goes to Low_Latency, film back to Normal | not tested |
| 5 | Stage 5 queue delay | overlay (Ctrl+I) "Queue delay: 5 fr = 209 ms" with film; raise AVR audio by about 84 ms; lip sync | not tested |
| 6 | No extra dropped frames or render time from the longer queue | overlay, 4K film, compare with the 5k build | not tested |

## Zoom and subtitles
| # | Check | How | Result |
|---|---|---|---|
| 7 | Black bars removed | Config > Zoom > Black bars removed > Use in VP | not tested |
| 8 | Fill (zoom169-like) on a 1.78:1 title | Zoom > fill | not tested |
| 9 | Subtitle fit (on) | a film with subtitles in the bars, zoom on: crop opens to keep them | not tested |
| 10 | Bar changes land on the right frame | aspect changes in a film | not tested |

## Shaders (Config > Shaders > Standard, one at a time)
| # | Check | How | Result |
|---|---|---|---|
| 11 | CfL chroma | switch on, compare | not tested |
| 12 | SSim downscaler (4K sources) | switch on, compare | not tested |
| 13 | FSRCNNX / ArtCNN / SSimSuperRes (1080p sources) | one at a time | not tested |
| 14 | Sigma-delta dither: ramp test (stage 3) | Shader settings: test_ramp 1; smooth ramp, no banding; tune noise and motion threshold | not tested |
| 15 | Sigma-delta on real film, still scenes | compare banding with it off; also watch very slow gradients for faint flat strips (values within 0.13 code snap to the whole code, to stop fp16 flicker) | not tested |
| 16 | Stage 4 motion-compensated dither | Shader settings: motion_compensation 1; slow pans and fades, compare with 0 | not tested |
| 17 | GPU time with sigma-delta + motion compensation | overlay render time (budget about 1 ms) | not tested |

## Dynamic Drive (Config > Dynamic Drive)
| # | Check | How | Result |
|---|---|---|---|
| 18 | Log only | watch the log, nothing sent | not tested |
| 19 | Script mode behaves like beta 22 | Mode Script | not tested |
| 20 | Compensate: slow grey fade looks continuous | Mode Compensate | not tested |
| 21 | Compensate: dark-to-bright cut, no flash or dip | tune LED response time by eye (default 80 ms) | not tested |
| 22 | Compensate: still mid-brightness scene, no breathing | 30 s | not tested |
| 23 | Compensate: 5-10% grey field steady | | not tested |
| 24 | Lowest LED level without look-ahead (100) in Low_Latency | a 60p source | not tested |
| 25 | Highlight protection levels | Medium / Strong / Light | not tested |
| 26 | Projector gamma 2.2 vs 2.4 | default 2.2; 2.4 only if a light meter says so (future maybe) | not tested |

## Experimental fade to black (off by default)
| # | Check | How | Result |
|---|---|---|---|
| 27 | LDMM 0 brings normal light back at once | Tera Term, DEPLOY.txt §6 a | not tested |
| 28 | Colours stepped in turns: does the black change colour? | Tera Term, DEPLOY.txt §6 b | not tested |
| 29 | First VP run with "Read the projector's replies" on | log shows no refused LDC* commands | not tested |
| 30 | Fade to black in a film | Experimental on, taking turns; log "custom dim on/off" | not tested |

## De-judder and occlusion mask (Stage 6; off by default; needs a build after 2026-10-11)
| # | Check | How | Result |
|---|---|---|---|
| 32 | 24p pan judder, Gentle | Config: Rendering, Cinema, De-judder Gentle (0.25). A slow pan: steadier than off? Compare with Off. | not tested |
| 33 | Edges and objects during pans | Same pan: no tearing, ghosting or wobbling edges around moving objects; subtitles steady | not tested |
| 34 | No 4:1 stepping, frame timing | Ctrl+I "De-judder:" line: late 0, full 0. vp.log: "Alpha frame pacing trace … submit_gap" no more often than with it off | not tested |
| 35 | De-judder log lines | vp.log: "De-judder phase: cadence 2.500", "De-judder extra: … phi0 0.000/0.200, delta 0.4/0.8/0.6" | not tested |
| 36 | Medium (0.35) | Same pan; more smoothing, any artefacts? | not tested |
| 37 | Occlusion mask with the sigma-delta | Occlusion mask on (de-judder off or on): dark gradients behind moving objects show no smearing or crawling dither; log "De-judder occlusion mask: used on …" | not tested |
| 38 | 4K source | Repeat 32 and 34 with a 4K film source (render load) | not tested |

## Light meter (future maybe, not a blocker)
| # | Check | How | Result |
|---|---|---|---|
| 31 | Projector gamma from readings at 16, 40, 80, 126, 180, 235 | DEPLOY.txt "Light meter"; settles Projector gamma and gamma boost | not measured |
