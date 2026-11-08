// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
#pragma once

// The VP-V members of the renderer settings (M2 P3-2): moved verbatim (initialisers and comments included)
// out of struct RendererSettings in vprenderer/LibplaceboVideoRenderer.cpp. The host struct inherits them
// through RendererSettingsExtension (vprenderer/RendererSettingsExtension.h, included by the H7 props
// define RENDERER_SETTINGS_EXTENSION_HEADER), so every `settings.<field>` site compiles unchanged and the
// copy, assignment and value semantics are those of the old members. Plain data: no construction or
// destruction order. The loader, fingerprint, live-copy and delta code that reads and writes these fields
// lives in VpvRendererSettings.{h,cpp} next to this header.

#include <string>
#include <vector>

#include <IntentDescriptor.h>
#include <MotionAhead.h>
#include <vprenderer/InterpolationPolicy.h>
#include <vprenderer/SdrToneShape.h>
#include <vprenderer/AdaptiveDownscale.h>
#include <vprenderer/DetailPulse.h>
#include <vprenderer/EdgeBlanking.h>
#include <vprenderer/TextureRestore.h>

namespace VpvRenderer
{
	struct Fields
	{
        bool presentEveryVsyncWithheld = false;   // VP-V PresentationOwner: requested, render-ahead owns presentation
		int saturation = 0;
		// Stage 6 de-judder: 0 = off (today's path); its own match tolerance.
		double dejudderStrength = 0.0;
		// Delta 10: even (2:2) cadences, 0 = off; 29.97 included or not.
		double dejudderStrengthEven = 0.0;
		bool dejudderEvenIncludes1001 = false;
		double dejudderMatchTolerance = 4.5;   // owner 2026-10-14: with the fade match (planner Q8's condition met)
		double dejudderFrameTrust = 0.25;   // 6b-2: whole-frame hold below this share
		bool occlusionMask = false;         // 6c: the de-judder's occlusion mask for the sigma-delta
		bool repeatRenderReuse = false;     // WP-24: captured repeats drawn from the rendered-picture cache (draft: off)
		// Refinements (docs/VP-build-plan-refinements.md); inert while the
		// de-judder and the grain map are off.
		bool dejudderGlobalPrior = true;          // D1
		double dejudderMotionScaleLo = 0.25;      // D2, source px per source frame
		double dejudderMotionScaleHi = 1.5;       // D2 (0 = off)
		bool dejudderVectorSmooth = true;         // D3
		double dejudderTrustScale = 0.6;          // D4: share at full strength (0 = off)
		bool dejudderOcclusionFill = false;       // D5 (experiment)
		// Stage 11a estimator-lite (docs/VP-stage11a-estimator-lite.md).
		bool dejudderEstCandidates = true;        // E1
		double dejudderEstLambda = 0.5;           // E1 smoothness penalty, codes per source px
		bool dejudderEstQpel = true;              // E3
		bool dejudderEstStillZero = true;         // identical blocks: exactly the zero vector (bug hunt 2026-10-25)
		bool dejudderEstGate = true;              // E4
		bool dejudderEstSplit = false;            // E2 (off from 2026-10-14: with the 4.5 tolerance and M6 it costs benefit)
		double dejudderEstSplitSad = 0.0;         // E2 threshold (0 = the match tolerance)
		// Delta 14 (docs/VP-delta-14-soe-rules.md): R-A the strength's roll-off
		// above a fast pan, R-B the camera/object split (1 = off).
		bool dejudderMotionRolloff = false;
		double dejudderRollLo = 8.0;
		double dejudderRollHi = 24.0;
		double dejudderRollFloor = 0.25;
		double dejudderObjectStrength = 1.0;
		// Motion fixes (owner 2026-10-13 night; DejudderWarp.h M1-M3): M1 the
		// texture-scaled tolerance (0 = off), M2 two global candidates, M3
		// regional hold with its cut test, the coarse search range (1/32
		// texels) and the forward/backward agreement (half-resolution px).
		// Defaults (owner 2026-10-14, the night's measurements): with the 4.5
		// tolerance, texture 0.25, the fade match, the split off and M6's
		// per-pixel trust, the 7 real HFR clips go from 22.8% to 42.1% of the
		// judder removed (film-like motion 14.4 -> 31.8%), damaged 32x32 tiles
		// 0.4 -> 0.9% (film-like 0.19 -> 0.36%), the synthetic corpus 833 ->
		// 942, wrongly trusted at most 1.9%. "More": dejudder_match_tolerance 8
		// with dejudder_pixel_trust_lo 1.7 / hi 4.5 (45.1%).
		double dejudderTextureTolerance = 0.25;
		bool dejudderGlobalTwo = true;
		bool dejudderRegionalHold = true;
		double dejudderCutHistogram = 0.3;
		int dejudderSearchRange = 8;
		double dejudderConsistencyPx = 2.0;
		// M4 (planner Q11): smooth fades matched after a global gain, at most
		// at the floor strength (off by default until measured on the projector).
		bool dejudderFadeMatch = true;
		double dejudderFadeFloor = 0.25;
		// M5: the pixel verification of trusted blocks (share of off pixels).
		bool dejudderVerify = false;
		double dejudderVerifyShare = 0.2;
		// M6: per-pixel trust on trusted blocks' vectors (multiples of the
		// match tolerance where a pixel's weight starts to fall and reaches 0).
		bool dejudderPixelTrust = true;
		// The landing check (owner 2026-10-14, on: damage to a third).
		bool dejudderLandingCheck = true;
		// Pruning 2026-10-17 (RetiredSettings.h): dejudder_picture_area, fix_scene_cooldown and
		// dither_vectors_new_frame are retired - black bars are always left out of the matcher's
		// statistics, the scene-cut cooldown is always 2 s at the content rate, and a new frame's
		// dither always takes the de-judder's motion when it is valid.
		// Roadmap item 2 (owner 2026-10-15, on): DeckLink R12B/R12L/r210 as
		// native RGB (GpuRgbUnpack) instead of the CPU's 10-bit 4:2:0 P010.
		bool nativeRgbIngest = true;
		// DV extension item 7 (owner 2026-10-16, off until validated): the
		// Dolby Vision tunnel's picture unpacked on the GPU; the CPU keeps the
		// DM packet (its pixels only). A failed unpack falls back to the CPU.
		bool dvTunnelGpuUnpack = true;   // default on (test/dv-synthetic verdict, 2026-10-24; failures listed under Fallbacks engaged)
		// DV extension item 3 (owner 2026-10-17, off): Dolby Vision L2 trims as a
		// VP pass after libplacebo's tone mapping (shaders\DV trims.glsl; a
		// non-certified reimplementation, see DolbyVisionTrims.h).
		bool dvTrims = true;             // default on (test/dv-synthetic verdict, 2026-10-24; not certified)
		std::string dvToneMapping = "libplacebo";   // feat/dv-l8: libplacebo | dolby_style (DvDolbyStyle.h; approx.)
		std::string dvCompareCaptureFolder;          // feat/dv-l8 WIP: DV comparison capture folder ("" = VP_DV_COMPARE_PATH / off)
		// Restore switch (DV tunnel bug hunt 2026-10-17, default on): the whole-raster
		// SDR alignment probe, the grey SDR exit, the loss limit in source pictures
		// and L2 trims before L8 (CRGBDolbyVisionTunnelFormatter). Off = before them.
		bool dvTunnelFixes = true;
		// Pulldown detection (owner 2026-10-14, off): film carried in a faster
		// signal by repeated frames is found and shown at its film rate.
		bool dejudderPulldown = false;
		double dejudderPixelTrustLo = 3.0;
		double dejudderPixelTrustHi = 8.0;
		// Owner 2026-10-13 efficiency: the matcher one level coarser above 1920
		// wide (off | search = the searches only | all = every pass).
		std::string dejudderLowRes = "all";
		// Owner 2026-10-13: the next pair estimated in an extra's spare refresh
		// when the frame after next is queued (else before the frame, as before).
		bool dejudderEstimateAhead = true;
		// Owner 2026-10-13 (docs/VP-gpu-capture-low-latency-plan.md): gpu =
		// V210 / UYVY / HDYC unpacked on the GPU from the raw frame (the same
		// planes, byte for byte); standard = the CPU formatters. Default gpu on
		// both pathways (owner 2026-10-15: about half the time at 4K, no CPU
		// conversion; every CPU consumer reads the raw frame - audited).
		std::string capturePath = "gpu";
		// Experimental (owner 2026-10-15, option A of the pinned-memory work):
		// early = each raw frame uploaded on the conversion thread as it arrives
		// (EarlyUpload, a second device and shared textures); render = the
		// render thread uploads it (today).
		std::string captureUpload = "auto";   // auto: early upload in every mode (VRAM-guarded); efficiency_fixes off: low-latency path only
		// Planner Q4 (2026-10-16): every key and value of the profile sections applied, so a
		// profile edit always changes the effective fingerprint (and is applied live) - also
		// for keys added later, without anyone having to list them in the fingerprint.
		std::string profileValues;
		// Owner 2026-10-15: the captured signal's range. auto = from the
		// capture format (YCbCr and 10-bit RGB limited; 8- and 12-bit RGB
		// full), as before; full / limited override it for a source that
		// sends otherwise (the DeckLink SDK reports no range). The picture and
		// the LED drive (its exact measurement follows the picture; its
		// look-ahead reads the captured frame) both follow it.
		std::string sourceRange = "auto";
		// Stage 12 step A (owner 2026-10-15): source_resolution - native |
		// auto (the default since 2026-10-16: the Picture slider's Standard) | any
		// height in lines. A height below the frame's re-bases the source to it
		// (SourceRebase.h).
		std::string sourceResolution = "auto";
		// Stage 12: the confidence auto needs before it re-bases (spec 3).
		double sourceResolutionAutoMinConfidence = 0.8;
		// The Picture slider (owner 2026-10-16): the shots and seconds of agreeing evidence
		// before auto declares a lower source (standard 3 and 10 s).
		int sourceResolutionEvidenceShots = 3;
		double sourceResolutionEvidenceSeconds = 10.0;
		// Stage 12 §2b/§2d: VP's own debander (off | auto | on) and whether the
		// bitrate quality reading raises it (auto | off | on).
		std::string debandVp = "off";
		std::string lowBitrateHandling = "auto";
		// Delta 15 S8-2: the SSIM weight (and the CNN's, 2c) lowered at a low
		// bitrate quality, down to the floor; the faithful anti-ringing raised.
		bool ssimBitrateAware = true;
		double ssimBitrateFloor = 0.4;
		// docs/VP-nvof-ahead.md (owner 2026-10-16): how many source frames ahead the
		// optical-flow engine works (0 = off); off in low latency; never more than queued.
		int motionAheadFrames = MotionAhead::kDefaultDepth;
		// The engine is the established default (owner 2026-10-16); off = the matcher only.
		bool opticalFlow = true;
		// Planner Q1 (2026-10-16): the whole-frame motion from the engine's field when its pair
		// came ahead (the global search the fallback). On the corpus identical without the
		// taste; on known pans the engine reads the speed exactly where the search overshot
		// medium pans by a step, so the taste's roll-off and camera follow the true speed.
		bool opticalFlowGlobal = true;
		// 2026-10-17 (docs/VP-nvof-ahead.md, the engine's extras): the global search when the
		// engine's whole-frame motion is not clear; the engine's cost and its own forward/
		// backward consistency as trust inputs (Dejudder::Settings).
		bool opticalFlowGlobalFallback = true;
		// corpus 2026-10-17: no measured benefit on real clips (cost trust identical; fb check trades removal for damage) - off by default, owner gate
		bool opticalFlowCostTrust = false;
		bool opticalFlowFbCheck = false;
		// VP's 50/60p frame interpolation (docs/VP-interpolator-50-60-plan.md; owner
		// defaults 2026-10-16, open for confirmation): off | auto (a non-integer
		// source/display ratio only) | always (smooth-motion parity for video-class
		// sources); where the vectors are untrusted hold (the nearest frame) or blend;
		// gap fill off; the film / video classification legacy (IsFilmLike) or measured.
		InterpolationPolicy::Mode interpolation = InterpolationPolicy::Mode::Off;
		InterpolationPolicy::Fallback interpolationFallback = InterpolationPolicy::Fallback::Hold;
		bool interpolationFillGaps = true;   // hiccup repair: on by default (owner 2026-10-18)
		InterpolationPolicy::SourceClass sourceClass = InterpolationPolicy::SourceClass::Legacy;
		// Content recognition (intent interpreter item 1, 2026-10-17; default off):
		// the rule-based content class for the rules' `content` variable, and the
		// seconds a new class must hold before it is taken (at a cut).
		bool intentContent = false;
		double intentContentHoldSeconds = 5.0;
		// Intent interpreter items 2-5 (2026-10-17; every function default off;
		// docs/VP-intent-interpreter-design.md): motion intent (a multiplier on the
		// de-judder strength in [intent_motion_min, 1], decided per shot, applied at
		// cuts), ambient adaptation (render to the effective black B_eff), black
		// protection (report / LED shadow cap) and the display mapping readout.
		bool intentMotion = false;
		MotionIntentPolicy intentMotionPolicy = MotionIntentPolicy::Display;
		double intentMotionMin = 0.5;
		double intentMotionMinShotSeconds = 2.0;
		double intentEaseSeconds = 1.5;   // intent_ease_s: the fade model (0 = instant at the cut)
		bool ambientAdapt = false;
		double ambientAdaptStrength = 1.0;
		double ambientScreenReflect = 0.01;
		double ambientMaxBlackNits = 1.0;
		double ambientTauUpSeconds = 15.0;
		double ambientTauDownSeconds = 90.0;
		BlackIntentMode intentBlack = BlackIntentMode::Off;
		bool intentDisplay = false;
		// Film grain (owner 2026-10-17: never averaged or interpolated away; each switch's
		// default is the faithful path, the other value the established one byte for byte;
		// docs/audit/grain-audit-2026-10-17.md): interpolation_grain, deband_vp_grain_guard,
		// dejudder_source_warp_filter (residual_stabilise_grain_guard is in `adaptive`,
		// deband_grain beside deband_strength).
		InterpolationPolicy::Grain interpolationGrain = InterpolationPolicy::Grain::Preserve;
		bool debandVpGrainGuard = true;
		// deband_vp_grain_guard on (hunt 2 07-scaling H1; the default) = the grain split: the
		// guard keeps grain by debanding only the low band; veto = the 2026-10-17 spatial-share
		// veto (debandVpGrainSplit false); off = no guard.
		bool debandVpGrainSplit = true;
		// deband_vp_exclusive (hunt 2 15-intent C14: libplacebo's deband on SDR thins fine
		// grain, 0.86x at 0.5 code): on (default) = while VP's debander runs (deband_vp auto
		// or on) with its grain guard, libplacebo's automatic deband (deband_strength auto)
		// is off; an explicit deband_strength (light / default) is kept. off = both run.
		bool debandVpExclusive = true;
		// detail_pulse_stabilise / texture_restore (docs/VP-detail-restore.md, 2026-10-17):
		// run after the VP debander on the plane libplacebo scales from, before every scaling
		// step; both read the source-resolution monitor's original frame never. detail_pulse_stabilise
		// on (default) scales each frame's own detail towards its best-coded recent neighbour
		// (cap detail_pulse_cap); texture_restore statistical (off by default, experimental)
		// repairs grain and in-focus texture towards the reference (cap texture_restore_cap).
		// Off = the stage is not called at all.
		bool detailPulseStabilise = true;
		double detailPulseCap = DetailPulse::kDefaultCap;
		// detail_pulse_fast (2026-10-18, default on): the same pulse in fewer GPU passes' work
		// (docs/agent-reports/detail-pulse-perf.md); off = the original passes, for A/B.
		bool detailPulseFast = DetailPulse::kDefaultFast;
		bool textureRestore = false;
		double textureRestoreCap = TextureRestore::kDefaultCap;
		bool dejudderSourceWarpCatmull = true;
		// Hunt 2 M1: nls_sampling. catmull (default): the NLS / NLS+ warp samples the
		// source with a separable Catmull-Rom widened by the map's local minification
		// (a low-pass at the centre); bilinear: the hooked texture's bilinear sampler,
		// the earlier output.
		bool nlsSamplingCatmull = true;
		// Owner 2026-10-16: the dither as one choice per profile - off, libplacebo's (the dithering
		// method below), or VP's temporal sigma-delta (loaded by the renderer itself, whatever the
		// shader list says). auto (unset): as before - the shader list and the dithering setting.
		std::string temporalDither = "auto";
		// Pipeline skeleton (design freeze 2026-10-24; docs/design/VP-pipeline-final.md):
		// pipeline_original_pathway on = every VP addition off, mixed slots in their upstream
		// form (ADR-0019); pipeline_order = slots moved within their zone (ADR-0021, validated;
		// empty = the designed order); pipeline_slots_off = slots switched off at slot level;
		// calibration_lut_stage = where the projector calibration LUT runs (libplacebo = its
		// target LUT, before VP's output hooks, as before; vp = VP's last colour hook, R1).
		bool pipelineOriginalPathway = false;
		std::string pipelineOrder;
		std::string pipelineSlotsOff;
		std::string calibrationLutStage = "libplacebo";
		std::string hdrInference = "vp";   // [vpv] hdr_inference (F-8): the GUI's choice; dumped and fingerprinted here
		// pipeline_entrance_once (B3, ADR-0024): the motion consumers (interpolated pictures, hiccup
		// gap pairs, source-warp extras) warp or blend the PROCESSED A and B - the entrance stages
		// run once per source frame - instead of re-running the stages on every picture. The
		// default (owner accepted 2026-10-24); off = the per-picture stages (a diagnostic). The
		// Original pathway never uses it.
		bool pipelineEntranceOnce = true;
		// frame_analysis_store (WP-23): VP's per-source-frame analysis record + ONE shared luma decode
		// read by every analyser (VpFrameAnalysisHost.h). Off until the big test; never in Original.
		bool frameAnalysisStore = false;
		// resync_event (R1/R3): off | shadow | on. Shadow detects resync/discontinuity events for the status
		// line and problem report only; the picture is unchanged. On (expert until the R5 gate): the central
		// Indicator drives the timing-family readers, the libplacebo flush and the re-base decision (R3, L1,
		// R4a). Never in Original.
		std::string resyncEvent = "off";
		// entrance_gpu_drain (expert; bug hunt 2026-10-25, root cause open): after the entrance stages
		// ran on a draw, "flush" submits the GPU queue and "finish" waits for it before libplacebo's
		// render. Pictures between frames then match plain draws byte for byte (without it libplacebo's
		// scaling passes vary by +-1 of 16 bits on scattered textured pixels with the GPU's timing).
		// off = today's path (the default until it is costed on the 3090); Original pathway: off.
		std::string entranceGpuDrain = "off";
		std::string pipelineSettingsProblems;   // refused values, shown under "Fallbacks engaged" (audit item 4)
		// Kernel-matched re-base (docs/VP-stage12-build-plan.md; owner 2026-10-16: optimise
		// 1080p): the identified player kernel's inverse for an exact 2:1 luma re-base.
		bool rebaseKernelMatch = true;
		// Owner 2026-10-16: tiles holding overlay text (the player's own subtitles, drawn at
		// the output resolution) are no evidence for source_resolution auto.
		bool sourceResolutionTextGuard = true;
		// Audit 2026-10-16: a mixed screen (a native UI with a lower-resolution video in it, a 4K
		// overlay, picture-in-picture) is read as native - no whole-frame re-base.
		bool sourceResolutionMixedGuard = true;
		// Hunt 2 M2/M3: the coding readings (effective bits, blocking, banding) robust to
		// upscales, dither and noise; off = the fixed 8/16-px grid and one-pixel occupancy.
		bool sourceCodingFixes = true;
		// Latch review 2026-10-17 (owner): source_resolution auto's latch fixes - re-engage on the
		// standard after native evidence, level-aware native readings, per-tile floor test, shape
		// cuts, the switch at a cut / real black / fast pan / low-detail moment or a crossfade, a
		// short release fade; off = the behaviour before (and no fades).
		bool sourceResolutionLatchFixes = true;
		// h3-latch 2026-10-18: [vprenderer] source_resolution_cut_fixes (default on, with the latch fixes):
		// camera moves, scrolls, dark passes and flashes are not cuts, same-level shot / reverse-shot cuts
		// are, and shots also need part of the evidence time (SourceResolution::Tracker::Settings::cutFixes).
		bool sourceResolutionCutFixes = true;
		// h3-latch 2026-10-18 (K3): [vprenderer] active_picture_startup_hold (default on): until the first crop
		// decision of a queue generation the output is black, or the remembered crop of the same source
		// (StartupCropHold); presentation only.
		bool activePictureStartupHold = true;
		double sourceResolutionEngageMaxWait = 20.0;   // s: the longest wait for a switching moment
		double sourceResolutionCrossfade = 1.5;        // s: the crossfade after that wait
		double sourceResolutionReleaseFade = 0.25;     // s: the fade back to native (at most 0.25)
		// Audit 2026-10-16 (pulldown review D1/D3/D4): keep the lock across counted misses, re-phase
		// in place, a cadence-aware film clock; off = the 2026-10-14 behaviour.
		bool pulldownFixes = true;
		// Bug hunt 2026-10-17: a capture timestamp break is a gap (and the dequeue checks a
		// pair's times), the film clock re-bases on a signal-rate change, and the timeline
		// holds the last measured display rate; off = the behaviour before.
		bool timingContinuityFixes = true;
		// Hunt 2 2026-10-17 (06-interp, 02-dv classifier): the exclusive 15 fps pulldown floor,
		// the film clock re-anchored on a re-phase (robust anchor), the queue target scaled while
		// locked, the source-only film / video class, the content classifier's carrier / animation /
		// static UI rules and content intent without look-ahead; off = the behaviour before.
		bool cadenceClassFixes = true;
		// Hunt 2 fix group 5 (motion): the matcher's history restarts on a pair not chained
		// to the last estimate and forgets the strength state with the rest; A's repeat
		// draws never read the (B, C) field estimated ahead; the ahead frame C is chosen by
		// B's adjacency and DV rules; DV pairs need byte-equal DM matrices; a matcher setting
		// or motion source change invalidates the vectors; a repeat-preserving queue drop
		// marks a gap; off = the behaviour before.
		bool motionHistoryFixes = true;
		bool vectorDemand = true;
		// Hunt 2 group 5 (13-timing, 05-renderahead M5): the backlog flush measures
		// staleness beyond the queue target, the timeline steering is in seconds, the cadence
		// correction measures the capture rate on QPC with constants in seconds and
		// a bounded phase, an overflow drop keeps the dropped frame's gap, the
		// credit-reduced target keeps the look-ahead's frames, and open loop ranks the
		// whole-hertz mode below the vblank cadence; off = the behaviour before.
		bool queueTimingFixes = true;
		// Fixed-constant batch 2026-10-18 (owner [[vp-general-not-fixed]]): the film / video class
		// derived from the measured rate (DejudderPolicy::IsFilmLike(hz, true), no fixed bands) and
		// the gap-fill span as a time (InterpolationTimeline::MaxSpanFrames); off = the behaviour before.
		bool generalRateFixes = true;
		// Efficiency round 2026-10-18 (docs/agent-reports/efficiency.md): work the renderer did
		// twice is done once - the frame shown takes the planes the de-judder uploaded for it
		// as the next frame (UploadReuse.h), and the look-ahead walk measures a frame's active
		// picture only for an observer that still wants the frame. Bit-identical output; off =
		// the earlier behaviour (both computations again).
		bool efficiencyFixes = true;
		// Render-ahead (docs/VP-render-ahead-design.md, owner's top priority 2026-10-17): the
		// de-judder's extras queued straight after their frame (no vblank waits), so the next
		// frame renders while they are shown; off until the owner's on-screen runs pass.
		// dejudder_gate: auto (period with render_ahead, else refresh) | period | refresh;
		// render_ahead_utilisation: the GPU share of a source period the gate budgets;
		// render_ahead_queue_credit: the queue target gives back the planned lead.
		bool renderAhead = false;
		std::string dejudderGate = "auto";
		double renderAheadUtilisation = 0.85;
		bool renderAheadQueueCredit = false;
		// Delta 9 synthesis-lite (docs/VP-delta-8-9-10.md).
		double dejudderHoldFeatherPx = 4.0;       // H1 (0 = off)
		int dejudderHoldHysteresis = 2;           // H2 (1 = off)
		bool dejudderBlockTaper = false;          // H3 (experiment)
		double dejudderTaperSad = 0.0;            // H3 (0 = 2 x match tolerance)
		double ditherGrainToleranceScale = 3.0;   // S1: sigma-delta tolerance x this on grain (1 = off)
		bool ditherVectorsOwn = false;   // delta 8 R1: dither_vectors own (auto = the de-judder's on extras)
		// S2 (planner 2026-10-12): every libplacebo FBO in rgba32f. -1 auto
		// (on, on everywhere (owner 2026-10-16)), 0 off, 1 on. The output pass is
		// rgba32f regardless (LibplaceboRenderParameters).
		int fboFloat32 = -1;
		bool preserveExtendedRange = true;  // Stage 10: BTB/WTW kept through scaling (SDR); on by default (owner 2026-10-16: keep BTB and WTW)
		// Audit 2026-10-16: the carry also for BT.1886 / sRGB, SDR in other primaries, a calibration
		// LUT and HDR passed through unmapped (off = the Stage 10 eligibility).
		bool extendedRangeMoreCases = true;   // GPU tests pass (2026-10-17 night); owner: BTB/WTW in all cases
		SdrToneShape::Settings sdrTone;     // delta 13: SDR tone shape and headroom (off)
		bool dejudderWarpRendered = true;   // 6d: extras warp A's rendered picture (false: a warped source)
		bool outputSegmentsMerged = false;   // S2/S3 as one PRE_OUTPUT + one OUTPUT hook (draft; tolerance gate)
		std::string downscaleLight = "linear";   // Stage 8-1: linear | gamma
		bool adaptiveDownscale = false;          // Stage 8-2: the SSIM / faithful blend
		// Stage 8b: the CNN / faithful upscale blend (grain only), its weight,
		// SSimSuperRes's strength by the grain term, and the light domain.
		bool adaptiveUpscale = false;
		double adaptiveUpscaleWBase = 1.0;
		bool adaptiveUpscaleSuperresScale = true;
		std::string upscaleLight = "linear";     // linear | gamma
		AdaptiveDownscale::Settings adaptive;
		bool debandGrain = false;   // deband_grain: libplacebo's deband adds grain (off: VP's dither does that job)
		EdgeBlanking::Insets edgeBlanking;
	};
}
