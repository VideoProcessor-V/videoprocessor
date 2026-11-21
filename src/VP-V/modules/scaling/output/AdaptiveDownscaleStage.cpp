#include <pch.h>
#include "AdaptiveDownscaleStage.h"

#include <DebugLog.h>
#include <VP-V/core/PipelineSlots.h>
#include <VP-V/core/Products.h>
#include <VP-V/modules/control/skeleton/SkeletonAccess.h>
#include <VP-V/modules/output/dither/CustomHookChain.h>
#include <VP-V/modules/scaling/output/DownscaleLight.h>
#include <vprenderer/GrainPreserve.h>

#include <algorithm>
#include <cmath>

// WP-13: moved from LibplaceboVideoRenderer.cpp (RenderLocked, Stage 8-2 / 8b and 8-1).
// Statement order, the host reads and the log text are unchanged.
namespace AdaptiveDownscaleStage
{
	namespace
	{
		float FramePeriod(const Frame& frame, const Host& host)
		{
			return frame.nominalSourceRateHz > 0.0 ? static_cast<float>(1.0 / frame.nominalSourceRateHz) :
				host.fallbackFramePeriod();
		}

		// Stage 12 §2c / S8-1: the motion for holding the CNN / SSIM residual (vectors of this source
		// frame only; never with NLS - its map is not linear; never starts the matcher).
		void SetResidualMotion(AdaptiveDownscale::Module& module, const Frame& frame, const MotionSource& m)
		{
			module.ClearMotion();
			if (!frame.nlsSubmitted && m.live && m.field && m.frameTrust)
			{
				AdaptiveDownscale::Motion motion;
				motion.field = m.field;
				motion.frameTrust = m.frameTrust;
				motion.minTrustedShare = m.minTrustedShare;
				motion.sourceCrop = frame.sourceCrop;
				module.SetMotion(motion);
			}
		}

		// CF-2 B1 ruling (a): the edge term's single seam. kEdgeMap's stage variant (the planes leaving scaling.geometry, the
		// source crop) through the skeleton: published = the module reads it; anything else = the private Sobel (the interim),
		// with the cause. One ask per frame.
		TermSource ReadEdge(const Frame& frame, const Host& host, AdaptiveDownscale::Module& module)
		{
			module.ClearEdgeSource();
			TermSource t;
			VpSkeleton::VariantKey key;
			key.stage = VpSkeleton::VariantKey::StageAfter(static_cast<uint8_t>(PipelineSlots::Id::ScalingGeometry));
			const pl_rect2df& c = frame.sourceCrop;
			const float cx0 = (std::max)(0.0f, std::floor((std::min)(c.x0, c.x1)));
			const float cy0 = (std::max)(0.0f, std::floor((std::min)(c.y0, c.y1)));
			const float cx1 = std::ceil((std::max)(c.x0, c.x1)), cy1 = std::ceil((std::max)(c.y0, c.y1));
			if (cx1 > cx0 && cy1 > cy0)
			{
				key.cropX = static_cast<uint32_t>(cx0);
				key.cropY = static_cast<uint32_t>(cy0);
				key.cropW = static_cast<uint32_t>(cx1 - cx0);
				key.cropH = static_cast<uint32_t>(cy1 - cy0);
			}
			VpSkeleton::AcquireResult r;
			if (host.acquire)
				r = host.acquire(VpSkeleton::kEdgeMap, frame.sourceSequence, key);
			else
			{
				const Vpv::Skeleton::FrameMemo fm = Vpv::Skeleton::CurrentFrameMemo();
				if (!fm.Valid())
				{
					t.cause = "edge: no skeleton memo for this renderer";
					return t;
				}
				r = fm.memo->Acquire(VpSkeleton::kEdgeMap, fm.seq, VpSkeleton::AcquireForm::Gpu, key);
			}
			if (!r.Ok() || !r.gpu.Valid() || r.gpu.resource == nullptr)
			{
				t.cause = std::string("edge: kEdgeMap stage variant ") + (r.Ok() ? "has no GPU texture" : VpSkeleton::Token(r.cause));
				return t;
			}
			AdaptiveDownscale::EdgeSource es;
			es.map = static_cast<pl_tex>(r.gpu.resource);
			es.pin = r.pin;
			es.sourceCrop = frame.sourceCrop;
			module.SetEdgeSource(es);
			t.published = true;
			return t;
		}

		// The grain term's seam: kP10gGrainLevel is a reader of the composed vector plan, not a product, at this tip (no id to
		// Acquire); the private grain scale / map stands in, visibly. The switch-over is an Acquire here when P-b publishes it.
		TermSource ReadGrain()
		{
			TermSource t;
			t.cause = "grain: the P10g grain product is not published (no product id at this tip)";
			return t;
		}
	}

	std::string InterimLine(const TermSource& edge, const TermSource& grain)
	{
		if (edge.published && grain.published)
			return std::string();
		std::string causes;
		const TermSource* terms[2] = { &edge, &grain };
		for (const TermSource* t : terms)
			if (!t->published)
				causes += (causes.empty() ? std::string() : std::string("; ")) + t->cause;
		return "edge/grain: private interim (" + causes + ")";
	}

	FallbackCatalog::Decision UpscaleScalingFallback(const UpscaleFallbackInputs& in)
	{
		const bool stageFloor = in.motionAdaptiveOn && !in.oneAxis && !in.stageMotionCause.empty();
		const bool floor = stageFloor || in.moduleFloorUsed;
		const FallbackCatalog::Decision base = FallbackCatalog::OutputScalingUpscale(in.requested, in.failureHeld, in.scalerError,
			floor, stageFloor ? in.stageMotionCause : in.moduleFloorCause);
		if (base.engaged || !in.requested)
			return base;
		if (!in.cutAvailable)
			return FallbackCatalog::Engaged("output scaling", "adaptive downscale: no cut verdict this frame (" + in.cutCause + ")");
		if (!in.interim.empty())
			return FallbackCatalog::Engaged("output scaling", "adaptive upscale: " + in.interim);
		return base;
	}

	const char* MotionDiscontinuity(bool vectorsRun, bool nls, bool live, bool haveLast, uint64_t lastSequence, uint64_t sequence)
	{
		if (!vectorsRun) return "motion vectors off";
		if (nls) return "NLS active";
		if (haveLast && sequence < lastSequence) return "resync";
		if (haveLast && sequence > lastSequence + 1) return "dropped source frames";
		if (!live) return "no live estimate";
		return nullptr;
	}

	void Stage::Register(const Frame& frame, const Host& host, SharedState state, std::vector<const pl_hook*>& hooks)
	{
		// Stage 8-2: the blend measures at PREKERNEL and blends after the user hooks at POSTKERNEL
		// (after the SSIM shader's passes); it needs the SSIM downscaler in the chain.
		// Stage 8b: a source shorter than the destination with the upscale blend on takes the module
		// in its upscale form (no SSIM needed: the perceptual branch is whatever upscales, CNN +
		// SuperRes when enabled).
		// Stage 12: the height libplacebo will sample (re-based when the factor is below 1).
		const float sourceCropH = std::fabs(frame.sourceCrop.y1 - frame.sourceCrop.y0) * static_cast<float>(frame.rebaseFactor);
		const float targetCropH = std::fabs(frame.targetCrop.y1 - frame.targetCrop.y0);
		// N9-P12: either axis upscaled (a width-only upscale, e.g. an anamorphic squeeze, was missed
		// by the height-only gate); exactly one axis upscaled = the one-axis form (faithful only).
		const float sourceCropW = std::fabs(frame.sourceCrop.x1 - frame.sourceCrop.x0) * static_cast<float>(frame.rebaseFactorX);
		const float targetCropW = std::fabs(frame.targetCrop.x1 - frame.targetCrop.x0);
		const AdaptiveDownscale::UpscaleAxes axes = AdaptiveDownscale::UpscaleAxesOf(sourceCropW, sourceCropH, targetCropW, targetCropH);
		const bool upscaleBlend = frame.slotRuns && frame.adaptiveUpscale && axes.any;
		const bool oneAxis = upscaleBlend && axes.oneAxis;
		oneAxisThisFrame = oneAxis;
		droppedPrescalers = 0;
		motionAdaptiveOn = frame.slotRuns && frame.adaptive && frame.adaptive->motionAdaptive &&
			(upscaleBlend || frame.adaptiveDownscale);
		const bool downscaleOn = frame.slotRuns && frame.adaptiveDownscale;   // slot off: as adaptive_downscale off
		upscaleThisFrame = upscaleBlend;
		if (!upscaleBlend)
		{
			module.ClearEdgeSource();   // the pin is not held past the upscale form
			edgeTerm = TermSource();
			grainTerm = TermSource();
			upscaleMotionCause.clear();
			haveMotionSequence = false;   // the next upscale run starts a new motion history
		}
		// Findings 2026-10-25 #1: what Fallback() reports (no effect on the picture).
		NoteFailures(module.Failures() - failuresSeen, GetTickCount64());
		failuresSeen = module.Failures();
		requested = upscaleBlend || downscaleOn;
		ssimMissing = false;
		vectorUser = false;   // set below by the hybrid forms that really register on this frame
		// SSimSuperRes takes the grain term through alpha only when the blend runs (its
		// vp_grain_strength; 0 otherwise).
		// One-axis: SuperRes takes the (zero) grain term through alpha whatever the setting, so it
		// adds nothing on the faithful-only picture.
		const pl_hook* superresHook = nullptr;
		for (const pl_hook* hook : hooks)
			if (CustomHookChain::SetHookParameter(hook, "vp_grain_strength",
				upscaleBlend && (frame.upscaleSuperresScale || oneAxis) ? 1.0 : 0.0))
				superresHook = hook;
		// One-axis: the CNN doubler would invent detail on the axis that is not upscaled (and
		// costs a full doubling): dropped from this frame's hooks, so PRE_KERNEL is the source.
		if (oneAxis && host.isPrescaler)
		{
			const auto end = std::remove_if(hooks.begin(), hooks.end(), [&](const pl_hook* hook) { return host.isPrescaler(hook); });
			droppedPrescalers = static_cast<int>(hooks.end() - end);
			hooks.erase(end, hooks.end());
		}
		const bool superresFound = superresHook != nullptr;
		const AdaptiveDownscale::Settings& adaptive = *frame.adaptive;
		// Stage 12 §2c / delta 15 S8-2: at a low bitrate quality the CNN's and SSIM's weights fall
		// towards the floor and the faithful branch's anti-ringing rises (low_bitrate_handling off:
		// every source clean).
		AdaptiveDownscale::Settings adaptiveNow = adaptive;
#if defined(PL_RENDER_PARAMS_FBO_FLOAT32)
		adaptiveNow.float32Picture = frame.fboFloat32 != 0;
#endif
		state.bitrateWeightScale = 1.0;
		// R1-W1-RES: read the temporal-detail products (absent in reset 1: nothing changes below; a requested engine with a missing
		// product shows in StatusLines and keeps the classical bitrate-quality weights that follow).
		tdRead = TdProducts::ReadToday(TdProducts::Consumer::AdaptiveScaler);
		if (frame.slotRuns && !frame.lowBitrateOff && (upscaleBlend || frame.ssimBitrateAware))
		{
			double reading = 1.0;
			double quality = host.bitrate(reading) ? reading : 1.0;
			if (frame.lowBitrateOn) quality = (std::min)(quality, 0.4);
			state.bitrateWeightScale = AdaptiveDownscale::BitrateScale(quality, frame.ssimBitrateFloor);
			adaptiveNow.antiring = AdaptiveDownscale::BitrateAntiring(adaptive.antiring, state.bitrateWeightScale,
				frame.ssimBitrateFloor);
			if (!upscaleBlend)
				adaptiveNow.wBase = static_cast<float>(adaptive.wBase * state.bitrateWeightScale);
		}
		// residual_stabilise_grain_guard: the held residual change limited to the coding noise the
		// bitrate reading implies (no reading: no limit).
		adaptiveNow.residualCodingNoiseCodes = 0.0f;
		if (adaptiveNow.residualGrainGuard)
		{
			double quality = 0.0;
			if (host.bitrate(quality))
				adaptiveNow.residualCodingNoiseCodes = static_cast<float>(GrainPreserve::CodingNoiseCodes(quality));
		}
		// FIX-X5-CUT-P4 / C5: the grain term's cut event is the shared one, the skeleton scene product's delivered cut (asked through
		// the skeleton's SceneOf, also with frame_analysis_store off; never re-classified here); the module asks it once per new source frame.
		if (!module.HasCutSource())
			module.SetCutSource(AdaptiveDownscale::SceneCutSource());
		if (upscaleBlend)
		{
			module.SetFrame(adaptiveNow, frame.sourceSequence, FramePeriod(frame, host));
			AdaptiveDownscale::Upscale up;
			up.on = true;
			up.wBase = static_cast<float>(frame.upscaleWBase * state.bitrateWeightScale);
			up.superresAlpha = (frame.upscaleSuperresScale || oneAxis) && superresFound;
			up.nativeHeight = sourceCropH;
			up.nativeWidth = sourceCropW;
			up.oneAxis = oneAxis;
			// CF-2 B1: the motion read (the ExternalView of P3, the live estimate: once per frame) and its verdict (N1.4). A
			// repeat of the last sequence keeps the first presentation's verdict (no flicker on a repeated frame).
			const MotionSource m = host.motion();
			if (!(haveMotionSequence && frame.sourceSequence == motionSequenceSeen))
			{
				const char* why = MotionDiscontinuity(frame.vectorsRun, frame.nlsSubmitted, m.live && m.field && m.frameTrust,
					haveMotionSequence, motionSequenceSeen, frame.sourceSequence);
				upscaleMotionCause = why ? why : "";
				haveMotionSequence = true;
				motionSequenceSeen = frame.sourceSequence;
			}
			up.vectorsInvalid = !upscaleMotionCause.empty();
			edgeTerm = ReadEdge(frame, host, module);
			grainTerm = ReadGrain();
			if (oneAxis != oneAxisLogged)
			{
				oneAxisLogged = oneAxis;
				DebugLog::Log("Adaptive upscale: %s", oneAxis ?
					"faithful only (one-axis upscale; CNN doubler skipped)" : "both axes (hybrid: sharp + alias-safe blend)");
			}
			module.SetUpscale(up);
			vectorUser = frame.vectorsRun;   // batch B Q1: the hybrid upscaler reads the motion (residual hold, grain map, motion-adapted blend)
			module.ClearGrainField();
			SetResidualMotion(module, frame, m);
			if (adaptive.grainMap && m.live && m.grainError)
			{
				AdaptiveDownscale::GrainField field;
				field.error = m.grainError;
				field.field = m.field;
				field.frameTrust = m.frameTrust;
				field.minTrustedShare = m.minTrustedShare;
				field.sourceCrop = frame.sourceCrop;
				bool known = true;
				if (frame.nlsSubmitted)
				{
					field.nls = host.nlsMapping();
					known = field.nls.valid;
				}
				if (known)
					module.SetGrainField(field);
			}
			state.grainMapWanted = (adaptive.grainMap || adaptive.motionAdaptive) && module.Active() && frame.vectorsRun;
			hooks.insert(hooks.begin(), module.Measure());
			// The blend before SSimSuperRes (both POST_KERNEL): SuperRes refines the blended picture
			// with its strength scaled by the grain term.
			const auto superres = std::find(hooks.begin(), hooks.end(), superresHook);
			if (superresHook && superres != hooks.end())
				hooks.insert(superres, module.Blend());
			else
				hooks.push_back(module.Blend());
			const int ratio = module.LastUpscaleRatio();
			if (ratio != upscaleRatioLogged)
			{
				upscaleRatioLogged = ratio;
				DebugLog::Log("Adaptive upscale: %s", ratio == 2 ? "faithful branch from the doubler's output at source resolution" :
					ratio == 1 ? "faithful branch from the source picture" : "not applied on the last frame (prescale ratio not 1 or 2)");
			}
		}
		else if (downscaleOn)
		{
			module.SetUpscale(AdaptiveDownscale::Upscale());
			if (host.ssimInChain())
			{
				module.SetFrame(frame.ssimBitrateAware ? adaptiveNow : adaptive, frame.sourceSequence,
					FramePeriod(frame, host));
				// SSIM's `//!WHEN NATIVE_CROPPED.h POSTKERNEL.h >`: the source crop against the
				// destination, not the (prescaled) PRE_KERNEL picture (audit stage8-rs232 item 3).
				// Audit 2026-10-16: the shader now runs when either axis shrinks (a width-only
				// downscale ran pass 2 without L2); the blend follows.
				{
					const float sourceCropW = std::fabs(frame.sourceCrop.x1 - frame.sourceCrop.x0) * static_cast<float>(frame.rebaseFactorX);
					module.SetSourceDownscales(sourceCropH > std::fabs(frame.targetCrop.y1 - frame.targetCrop.y0) + 0.5f ||
						sourceCropW > std::fabs(frame.targetCrop.x1 - frame.targetCrop.x0) + 0.5f);
				}
				// 8-3: the matcher's field for this source frame, when it ran for it (A->B, estimated
				// at upload); the map follows NLS's own map (bundled NLS / NLS+ only; otherwise the
				// scalar).
				vectorUser = frame.vectorsRun;   // batch B Q1: the hybrid downscaler reads the motion (residual hold, grain map, motion-adapted blend)
				module.ClearGrainField();
				const MotionSource m = host.motion();
				SetResidualMotion(module, frame, m);
				if (adaptive.grainMap && m.live && m.grainError)
				{
					AdaptiveDownscale::GrainField field;
					field.error = m.grainError;
					field.field = m.field;
					field.frameTrust = m.frameTrust;
					field.minTrustedShare = m.minTrustedShare;
					field.sourceCrop = frame.sourceCrop;
					bool known = true;
					if (frame.nlsSubmitted)
					{
						field.nls = host.nlsMapping();
						known = field.nls.valid;
					}
					if (known)
						module.SetGrainField(field);
					else if (!mapNlsLogged)
					{
						mapNlsLogged = true;
						DebugLog::Log("Adaptive downscale: grain map not used with NLS shader '%s' (its map is not known); per-frame grain",
							frame.nlsFile);
					}
				}
				// The next source frame's estimate runs for the map when the blend acts on this
				// picture (a downscale; Active() is the last measured frame's).
				// The motion-adapted blend needs this frame's vectors as much as the map does.
				state.grainMapWanted = (adaptive.grainMap || adaptive.motionAdaptive) && module.Active() && frame.vectorsRun;
				hooks.insert(hooks.begin(), module.Measure());
				hooks.push_back(module.Blend());
				state.noSsimLogged = false;
			}
			else
			{
				ssimMissing = true;
				if (!state.noSsimLogged)
				{
					state.grainMapWanted = false;
					state.noSsimLogged = true;
					DebugLog::Log("Adaptive downscale: on, but the SSimDownscaler shader is not in the chain; not used");
				}
			}
			if (statsTick == 0 || GetTickCount64() - statsTick >= 1000)
			{
				statsTick = GetTickCount64();
				host.publishStatus();
			}
		}
		if (!downscaleOn && !upscaleBlend)
			state.grainMapWanted = false;
		// The stage's own status lines are republished when they change (the 8b form, the
		// motion-adapted state), not only on the downscale's once-a-second tick.
		std::string status;
		for (const std::string& line : StatusLines())
			status += line + "\n";
		if (status != statusPublished)
		{
			statusPublished = status;
			if (host.publishStatus)
				host.publishStatus();
		}
	}

	std::vector<std::string> Stage::StatusLines() const
	{
		std::vector<std::string> lines;
		// The module's readouts are the last measured frame's (audit: never claim a blend that did not run).
		if (upscaleThisFrame)
		{
			if (!module.Active() || module.LastUpscaleRatio() == 0)
				lines.push_back(oneAxisThisFrame && droppedPrescalers > 0 ?
					"Adaptive upscale: not applied on the last frame (no picture yet or prescale ratio not 1 or 2; CNN doubler skipped: one-axis)" :
					"Adaptive upscale: not applied on the last frame (no picture yet or prescale ratio not 1 or 2)");
			else
				lines.push_back(oneAxisThisFrame ?
					(droppedPrescalers > 0 ? "Adaptive upscale: faithful only (one-axis upscale; CNN doubler skipped)" :
						"Adaptive upscale: faithful only (one-axis upscale)") :
					module.LastUpscaleRatio() == 2 ?
					"Adaptive upscale: hybrid (sharp = CNN prescaler + main scaler: the CNN evaluation arm; alias-safe = ScalerPass)" :
					"Adaptive upscale: hybrid (sharp = main scaler, alias-safe = ScalerPass)");
			// CF-2 B1 ruling (a): the edge / grain terms are the private in-shader ones until their products are published.
			const std::string interim = InterimLine(edgeTerm, grainTerm);
			if (!interim.empty())
				lines.push_back(interim);
			// N1.4: no / invalid vectors with adaptive_motion on: the motion floor, with the cause (one-axis: faithful only already).
			if (motionAdaptiveOn && !oneAxisThisFrame && !upscaleMotionCause.empty())
				lines.push_back("Adaptive upscale motion: no vectors (alias-safe weight; " + upscaleMotionCause + ")");
			// FIX1 (gate M1): the module's own floor (no usable factor map) while the stage found the vectors usable.
			else if (module.MotionFloorUsed())
				lines.push_back("Adaptive upscale motion: no vectors (alias-safe weight; " + module.MotionFloorCauseText() + ")");
		}
		if (motionAdaptiveOn)
		{
			const AdaptiveDownscale::MotionState m = module.LastMotionState();
			lines.push_back(upscaleThisFrame && module.MotionFloorUsed() ?
				"Motion-adapted blend: no vectors on this frame (upscale: the motion floor, alias-safe lean)" :
				m == AdaptiveDownscale::MotionState::Used ?
				"Motion-adapted blend: on (vectors; untracked or fast motion uses the faithful scaler)" :
				m == AdaptiveDownscale::MotionState::NoVectors ? "Motion-adapted blend: no vectors on this frame (plain blend)" :
				"Motion-adapted blend: idle (the blend did not run on the last frame)");
		}
		// FIX-X5-CUT-P4 / C5: never a silent downgrade. No scene group for this frame: no cut verdict this frame.
		if (!module.CutAvailable())
			lines.push_back("Adaptive downscale: no cut verdict this frame (" + module.CutCause() + ")");
		// The APL (diagnostic mean luma) was not read from the P4 record: shown, not a cut fault.
		if (!module.AplCause().empty())
			lines.push_back("Adaptive downscale: APL not read (" + module.AplCause() + ")");
		// CB-18 S3 (plan line 69): the measure's edge map not served by the memo (unavailable: the measure did not run; or
		// computed in the module without a skeleton memo). The last measured frame's, while the blend is asked for.
		if (requested && !module.EdgeMapStatus().empty())
			lines.push_back(module.EdgeMapStatus());
		const std::string td = TdProducts::StatusLine(TdProducts::Consumer::AdaptiveScaler, tdRead);
		if (!td.empty())
			lines.push_back(td);
		return lines;
	}

	void Stage::ApplyLight(pl_render_params& params, const Light& light)
	{
		// Stage 8-1: a downscaling frame in gamma light when chosen (the de-judder extras reuse
		// these parameters).
		const bool downscaleGamma = light.slotRuns && light.downscaleGamma;   // slot off: linear (libplacebo's)
		const bool gamma = downscaleGamma && DownscaleLight::Downscales(light.sourceCrop, light.targetCrop);
		if (gamma)
			params.disable_linear_scaling = true;
		// 8b: both branches in one light domain: linear (no sigmoid) or gamma.
		if (upscaleThisFrame && light.slotRuns)
		{
			if (light.upscaleGamma)
				params.disable_linear_scaling = true;
			else
				params.sigmoid_params = nullptr;
		}
		const int logged = downscaleGamma ? (gamma ? 2 : 1) : 0;
		if (logged != lightLogged)
		{
			lightLogged = logged;
			DebugLog::Log("Downscale light: %s", logged == 2 ? "gamma (downscaling frame)" :
				logged == 1 ? "gamma chosen; linear on this frame (not downscaling)" : "linear");
		}
	}
}
