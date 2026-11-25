#include <pch.h>
#include "AdaptiveDownscaleStage.h"

#include <DebugLog.h>
#include <VP-V/core/Products.h>
#include <VP-V/modules/control/skeleton/SkeletonAccess.h>
#include <VP-V/modules/motion/estimation/P3FieldFill.h>
#include <VP-V/modules/output/dither/CustomHookChain.h>
#include <VP-V/modules/scaling/output/DownscaleLight.h>
#include <libplacebo/filters.h>
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

		// X5-GRAIN-P10: this frame's shared grain products (P10 N, P11 meanY and coverage) from the host, when it
		// supplies them; none (no callback, or an invalid source) leaves the private Laplacian estimate, shown in StatusLines.
		// The status text of each grain estimate (one line; the same reasons as GrainSourceFallback).
		const char* GrainStatusLine(AdaptiveDownscale::GrainSourceUse use)
		{
			using U = AdaptiveDownscale::GrainSourceUse;
			switch (use)
			{
			case U::Shared: return "Adaptive grain: from the shared grain split (P10) and region statistics (P11)";
			case U::NoUsableBlock: return "Adaptive grain: own estimate (no usable shared grain block on the last read-back frame)";
			case U::MeasureDomain: return "Adaptive grain: own estimate (measure domain is linear / HDR; the shared products are in source codes)";
			case U::Pending: return "Adaptive grain: shared grain split (P10) bound; waiting for the first read-back of a bound frame";
			case U::NotSupplied: break;
			}
			return "Adaptive grain: own estimate (the shared grain products are not supplied to the scaler)";
		}

		// Returns whether the host offers the shared products at all (a callback): an offered source that is not used is a
		// FallbackCatalog downgrade (GrainSourceFallback), not only a status line.
		bool SetSharedGrain(AdaptiveDownscale::Module& module, const Host& host)
		{
			module.ClearGrainSource();
			if (!host.grainSource)
				return false;
			const AdaptiveDownscale::GrainSource source = host.grainSource();
			if (source.Valid())
				module.SetGrainSource(source);
			return true;
		}

		// Stage 12 §2c / S8-1: the motion for holding the CNN / SSIM residual and the motion-adapted blend
		// (vectors of this source frame only; carries known NLS mapping when NLS is active so the motion-adapted
		// blend warps through nls_coordinate while residual hold stays off; never starts the matcher).
		void SetResidualMotion(AdaptiveDownscale::Module& module, const Frame& frame, const MotionSource& m,
			const std::shared_ptr<const void>& pin, const NlsMaskMapping::Mapping& nlsMap, bool nlsUnknown)
		{
			module.ClearMotion();
			if (!nlsUnknown && m.live && m.field && m.frameTrust)
			{
				AdaptiveDownscale::Motion motion;
				motion.field = m.field;
				motion.frameTrust = m.frameTrust;
				motion.pin = pin;
				motion.minTrustedShare = m.minTrustedShare;
				motion.sourceCrop = frame.sourceCrop;
				motion.nls = frame.nlsSubmitted ? nlsMap : NlsMaskMapping::Mapping{};
				module.SetMotion(motion);
			}
		}

		// M1#1: upscale motion term via Acquire(kP3AnalysisField, AcquireForm::Gpu), symmetric with ReadGrain.
		// Never silently falls back to host.motion().field; host.motion is consulted ONLY for auxiliary
		// frameTrust (when P3FieldFactsOf(r)->trust is absent) and minTrustedShare.
		struct UpscaleMotionRead
		{
			MotionSource motion;
			std::shared_ptr<const void> pin;
			std::string cause;   // empty when kP3AnalysisField + frameTrust are live
		};

		UpscaleMotionRead ReadUpscaleMotion(const Frame& frame, const Host& host)
		{
			UpscaleMotionRead out;
			VpSkeleton::AcquireResult r;
			if (host.acquire)
				r = host.acquire(VpSkeleton::kP3AnalysisField, frame.sourceSequence, VpSkeleton::VariantKey());
			else
			{
				const Vpv::Skeleton::FrameMemo fm = Vpv::Skeleton::CurrentFrameMemo();
				if (!fm.Valid())
				{
					out.cause = "no skeleton memo for this renderer";
					return out;
				}
				r = fm.memo->Acquire(VpSkeleton::kP3AnalysisField, fm.seq, VpSkeleton::AcquireForm::Gpu);
			}
			if (!r.Ok() || !r.gpu.Valid() || r.gpu.resource == nullptr)
			{
				out.cause = std::string("kP3AnalysisField ") + (r.Ok() ? "has no GPU texture" : VpSkeleton::Token(r.cause));
				return out;
			}
			pl_tex p3Field = static_cast<pl_tex>(r.gpu.resource);
			const VpMotion::VerifiedField* facts = VpMotion::P3FieldFactsOf(r);
			const bool haveAux = static_cast<bool>(host.motion);
			const MotionSource aux = haveAux ? host.motion() : MotionSource();
			pl_tex frameTrust = (facts && facts->trust) ? facts->trust : aux.frameTrust;
			const float minTrustedShare = haveAux ? aux.minTrustedShare : 0.25f;
			if (frameTrust == nullptr)
			{
				out.cause = "kP3AnalysisField has no frame trust";
				return out;
			}
			out.motion.live = true;
			out.motion.field = p3Field;
			out.motion.frameTrust = frameTrust;
			out.motion.minTrustedShare = minTrustedShare;
			out.pin = r.pin;
			return out;
		}

		// CF-2 switch-over: the grain term's single seam. kP10gGrainLevel (id 35, "grain.level", the repair.grain_split owner
		// fill; RGBA32F, one texel per 16 x 16 source-luma block, x = N_b) through the skeleton: published = the module reads it;
		// anything else = grain_floor (no private grain term), with the cause. One ask per upscale frame.
		TermSource ReadGrain(const Frame& frame, const Host& host, AdaptiveDownscale::Module& module,
			const NlsMaskMapping::Mapping& nlsMap)
		{
			module.ClearGrainLevel();
			TermSource t;
			if (frame.nlsSubmitted && !nlsMap.valid)
			{
				t.cause = "NLS map unknown";
				return t;
			}
			VpSkeleton::AcquireResult r;
			if (host.acquire)
				r = host.acquire(VpSkeleton::kP10gGrainLevel, frame.sourceSequence, VpSkeleton::VariantKey());
			else
			{
				const Vpv::Skeleton::FrameMemo fm = Vpv::Skeleton::CurrentFrameMemo();
				if (!fm.Valid())
				{
					t.cause = "no skeleton memo for this renderer";
					return t;
				}
				r = fm.memo->Acquire(VpSkeleton::kP10gGrainLevel, fm.seq, VpSkeleton::AcquireForm::Gpu);
			}
			if (!r.Ok() || !r.gpu.Valid() || r.gpu.resource == nullptr)
			{
				t.cause = std::string("kP10gGrainLevel ") + (r.Ok() ? "has no GPU texture" : VpSkeleton::Token(r.cause));
				return t;
			}
			AdaptiveDownscale::GrainLevelSource gs;
			gs.level = static_cast<pl_tex>(r.gpu.resource);
			gs.pin = r.pin;
			gs.sourceCrop = frame.sourceCrop;
			gs.nls = (frame.nlsSubmitted && nlsMap.valid) ? nlsMap : NlsMaskMapping::Mapping{};
			module.SetGrainLevel(gs);
			t.published = true;
			return t;
		}
	}

	FallbackCatalog::Decision UpscaleScalingFallback(const UpscaleFallbackInputs& in)
	{
		const bool stageFloor = in.motionAdaptiveOn && !in.oneAxis && !in.stageMotionCause.empty();
		const bool floor = stageFloor || (in.motionAdaptiveOn && !in.oneAxis && in.moduleFloorUsed);
		const FallbackCatalog::Decision base = FallbackCatalog::OutputScalingUpscale(in.requested, in.failureHeld, in.scalerError,
			floor, stageFloor ? in.stageMotionCause : in.moduleFloorCause);
		if (base.engaged || !in.requested)
			return base;
		if (!in.cutAvailable)
			return FallbackCatalog::Engaged("output scaling", "adaptive downscale: no cut verdict this frame (" + in.cutCause + ")");
		if (!in.oneAxis && !in.grainCause.empty())
			return FallbackCatalog::Engaged("output scaling", "adaptive upscale: no grain level, alias-safe grain weight (" + in.grainCause + ")");
		if (!in.edgeMapStatus.empty())
			return FallbackCatalog::Engaged("output scaling", "adaptive upscale: " + in.edgeMapStatus);
		if (!in.oneAxis && in.motionExternalView)
			return FallbackCatalog::Engaged("output scaling", "motion: P3 read through the ExternalView (P3 has no GPU publication)");
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
		motionStrengthActive = (frame.slotRuns && frame.adaptive && (upscaleBlend || frame.adaptiveDownscale))
			? AdaptiveDownscale::MotionSanitizeUnit(frame.adaptive->motionStrength) : 0.0f;
		motionAdaptiveOn = motionStrengthActive > 0.0f;
		const NlsMaskMapping::Mapping nlsMap = (frame.nlsSubmitted && host.nlsMapping)
			? host.nlsMapping() : NlsMaskMapping::Mapping{};
		const bool nlsUnknown = frame.nlsSubmitted && !nlsMap.valid;
		const bool downscaleOn = frame.slotRuns && frame.adaptiveDownscale;   // slot off: as adaptive_downscale off
		upscaleThisFrame = upscaleBlend;
		if (!upscaleBlend)
		{
			module.ClearGrainLevel();   // the pin is not held past the upscale form
			module.ClearMotion();       // M1#1: do not hold an upscale P3 motion pin past the upscale form
			grainTerm = TermSource();
			upscaleMotionCause.clear();
			upscaleMotionExternalView = false;
			haveMotionSequence = false;   // the next upscale run starts a new motion history
		}
		// Findings 2026-10-25 #1: what Fallback() reports (no effect on the picture).
		NoteFailures(module.Failures() - failuresSeen, GetTickCount64());
		failuresSeen = module.Failures();
		requested = upscaleBlend || downscaleOn;
		ssimMissing = false;
		vectorUser = false;   // set below by the hybrid forms that really register on this frame
		grainOffered = false;   // set below when the scalar pass is registered and the host offers the shared products
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
			// M1#1: upscale motion comes through Acquire(kP3AnalysisField, AcquireForm::Gpu) once per upscale frame
			// when !oneAxis && vectorsRun && !nlsUnknown (never falling back to host.motion().field). Discontinuity
			// verdicts (motion vectors off, NLS active, resync, dropped source frames) and repeated-frame persistence
			// stay intact; unavailability surfaces the Acquire cause visibly in status & Fallback().
			UpscaleMotionRead um;
			if (!oneAxis && frame.vectorsRun && !nlsUnknown)
				um = ReadUpscaleMotion(frame, host);
			upscaleMotionExternalView = false;
			const bool motionLive = um.motion.live && um.motion.field && um.motion.frameTrust;
			if (!(haveMotionSequence && frame.sourceSequence == motionSequenceSeen))
			{
				const char* why = MotionDiscontinuity(frame.vectorsRun, nlsUnknown, motionLive,
					haveMotionSequence, motionSequenceSeen, frame.sourceSequence);
				if (why != nullptr)
					upscaleMotionCause = (!motionLive && !um.cause.empty() && std::string(why) == "no live estimate")
						? um.cause : std::string(why);
				else
					upscaleMotionCause.clear();
				haveMotionSequence = true;
				motionSequenceSeen = frame.sourceSequence;
			}
			up.vectorsInvalid = !upscaleMotionCause.empty();
			grainTerm = ReadGrain(frame, host, module, nlsMap);
			if (oneAxis != oneAxisLogged)
			{
				oneAxisLogged = oneAxis;
				DebugLog::Log("Adaptive upscale: %s", oneAxis ?
					"faithful only (one-axis upscale; CNN doubler skipped)" : "both axes (hybrid: sharp + alias-safe blend)");
			}
			module.SetUpscale(up);
			vectorUser = frame.vectorsRun;   // batch B Q1: the hybrid upscaler reads the motion (residual hold, grain map, motion-adapted blend)
			// Switch-over: the upscale form's grain term is kP10gGrainLevel (ReadGrain), so no private grain map here; the
			// estimate is still wanted for the residual hold and the motion-adapted blend.
			module.ClearGrainField();
			// S4-H2: upscale uses kP10gGrainLevel via ReadGrain(), never the downscale scalar P10/P11 grainSource.
			module.ClearGrainSource();
			SetResidualMotion(module, frame, um.motion, um.pin, nlsMap, nlsUnknown);
			state.grainMapWanted = (AdaptiveDownscale::MotionSanitizeUnit(adaptive.motionStrength) > 0.0f) && module.Active() && frame.vectorsRun;
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
				module.SetFrame(adaptiveNow, frame.sourceSequence,   // GEM-W1B V3 MUST-3: float32Picture + coding-noise guard survive ssim_bitrate_aware off
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
				grainOffered = SetSharedGrain(module, host);
				const MotionSource m = host.motion ? host.motion() : MotionSource();
				SetResidualMotion(module, frame, m, std::shared_ptr<const void>(), nlsMap, nlsUnknown);
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
						field.nls = nlsMap;
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
				state.grainMapWanted = (adaptive.grainMap || AdaptiveDownscale::MotionSanitizeUnit(adaptive.motionStrength) > 0.0f) && module.Active() && frame.vectorsRun;
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
			// CF-2 switch-over: kP10gGrainLevel not read or refused on linear/HDR domain: the grain floor (alias-safe lean), with cause.
			if (!oneAxisThisFrame && !grainTerm.published)
				lines.push_back("Adaptive upscale grain: no grain level (alias-safe weight; " + grainTerm.cause + ")");
			else if (!oneAxisThisFrame && module.PublishedGrainDomainRefused())
				lines.push_back("Adaptive upscale grain: kP10gGrainLevel refused (alias-safe weight; measure domain is linear / HDR)");
			// M4 (CF-2): P3 motion read via host.motion ExternalView (no GPU Acquire publication yet) — visible, never silent.
			if (!oneAxisThisFrame && upscaleMotionExternalView)
				lines.push_back("Adaptive upscale motion: P3 read through the ExternalView (P3 has no GPU publication)");
			// N1.4: no / invalid vectors with adaptive_motion > 0: the motion floor, with the cause (one-axis: faithful only already).
			if (motionAdaptiveOn && !oneAxisThisFrame && !upscaleMotionCause.empty())
				lines.push_back("Adaptive upscale motion: no vectors (alias-safe weight) (" + upscaleMotionCause + ")");
			// FIX1 (gate M1): the module's own floor (no usable factor map) while the stage found the vectors usable.
			else if (motionAdaptiveOn && !oneAxisThisFrame && module.MotionFloorUsed())
				lines.push_back("Adaptive upscale motion: no vectors (alias-safe weight) (" + module.MotionFloorCauseText() + ")");
		}
		if (motionAdaptiveOn)
		{
			const int pct = static_cast<int>(std::lround(AdaptiveDownscale::MotionSanitizeUnit(motionStrengthActive) * 100.0f));
			const std::string strengthText = std::to_string(pct / 100) + "." +
				(pct % 100 < 10 ? "0" : "") + std::to_string(pct % 100);
			const AdaptiveDownscale::MotionState m = module.LastMotionState();
			lines.push_back(upscaleThisFrame && module.MotionFloorUsed() ?
				"Motion-adapted blend: strength " + strengthText + " (no vectors on this frame; upscale: alias-safe lean)" :
				m == AdaptiveDownscale::MotionState::Used ?
				"Motion-adapted blend: strength " + strengthText + " (vectors; untracked or fast motion uses the faithful scaler)" :
				m == AdaptiveDownscale::MotionState::NoVectors ?
				"Motion-adapted blend: strength " + strengthText + " (no vectors on this frame; plain blend)" :
				"Motion-adapted blend: idle (strength " + strengthText + "; the blend did not run on the last frame)");
		}
		// FIX-X5-CUT-P4 / C5: never a silent downgrade. No scene group for this frame: no cut verdict this frame.
		if (!module.CutAvailable())
			lines.push_back("Adaptive downscale: no cut verdict this frame (" + module.CutCause() + ")");
		// GEM-W1B V3 MUST-6: fbo_float32 asked, the GPU gave 16-bit float for the faithful picture: shown, never silent.
		// Gated on `requested`: the flag is reset only when the module measures again, so a switch to off must not leave it on screen.
		if (requested && module.HalfFloatPictureFallback())
			lines.push_back("Adaptive scaler: 16-bit float picture (32-bit float not available)");
		// The APL (diagnostic mean luma) was not read from the P4 record: shown, not a cut fault.
		if (!module.AplCause().empty())
			lines.push_back("Adaptive downscale: APL not read (" + module.AplCause() + ")");
		// CB-18 S3 (plan line 69): the measure's edge map not served by the memo (unavailable: the measure did not run; or
		// computed in the module without a skeleton memo). The last measured frame's, while the blend is asked for.
		if (requested && !module.EdgeMapStatus().empty())
			lines.push_back(module.EdgeMapStatus());
		// X5-GRAIN-P10 (no silent fallbacks): which grain estimate the last scalar pass used.
		// Gate cb-u1 r1 MUST 5: "shared" only when the read-back stats tag says the frame's energy came from P10 / P11.
		if (!upscaleThisFrame && module.Active())
			lines.push_back(GrainStatusLine(module.LastGrainSourceUse()));
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
		// GEM-W1B V3 MUST-1: the size libplacebo will sample is the crop x the re-base factor
		// (as Register measures it), so a re-based frame that upscales is not taken as a downscale.
		pl_rect2df sampled = light.sourceCrop;
		{
			const float fx = static_cast<float>(light.rebaseFactorX), fy = static_cast<float>(light.rebaseFactor);
			sampled.x1 = sampled.x0 + (light.sourceCrop.x1 - light.sourceCrop.x0) * fx;
			sampled.y1 = sampled.y0 + (light.sourceCrop.y1 - light.sourceCrop.y0) * fy;
		}
		const bool gamma = downscaleGamma && DownscaleLight::Downscales(sampled, light.targetCrop);
		if (gamma)
			params.disable_linear_scaling = true;
		// G12: VP-V pathway default downscaler is Lanczos when downscaler=auto and quality!=fast
		// (Original pathway leaves upstream hermite untouched).
		if (light.vpvDefaultDownscaler)
			params.downscaler = &pl_filter_lanczos;
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

	FallbackCatalog::Decision Stage::Fallback() const
	{
		// CF-2 B1 / FIX1: the upscale form's own entry (the alias-safe ScalerPass failed: the main scaler alone; no vectors,
		// by the stage's verdict or the module's own floor: the floor; no cut verdict; no grain level: the grain floor).
		if (upscaleThisFrame)
		{
			UpscaleFallbackInputs in;
			in.requested = requested;
			in.failureHeld = failureHeld;
			in.scalerError = module.ScalerError();
			in.motionAdaptiveOn = motionAdaptiveOn;
			in.oneAxis = oneAxisThisFrame;
			in.stageMotionCause = upscaleMotionCause;
			in.moduleFloorUsed = module.MotionFloorUsed();
			in.moduleFloorCause = module.MotionFloorCauseText();
			in.cutAvailable = module.CutAvailable();
			in.cutCause = in.cutAvailable ? std::string() : module.CutCause();
			in.grainCause = oneAxisThisFrame ? std::string() :
				(!grainTerm.published ? grainTerm.cause :
				(module.PublishedGrainDomainRefused() ? std::string("measure domain is linear / HDR") : std::string()));
			in.edgeMapStatus = module.EdgeMapStatus();
			in.motionExternalView = upscaleMotionExternalView;
			const FallbackCatalog::Decision up = UpscaleScalingFallback(in);
			if (!up.engaged && requested && module.HalfFloatPictureFallback())
				return FallbackCatalog::Engaged("output scaling", "adaptive scaler: 16-bit float picture (32-bit float not available)");
			return up;
		}
		const FallbackCatalog::Decision base = FallbackCatalog::OutputScaling(requested, failureHeld ? 1u : 0u, ssimMissing);
		// FIX-X5-CUT-P4 / C5: no scene group (the skeleton's scene product) for this frame: no cut verdict this frame (also on StatusLines()).
		if (!base.engaged && requested && !module.CutAvailable())
			return FallbackCatalog::Engaged("output scaling", "adaptive downscale: no cut verdict this frame (" + module.CutCause() + ")");
		// GEM-W1B V3 MUST-6 (audit-W1B SHOULD-1): the 32 -> 16-bit float picture fallback reaches Config status and the
		// problem report too, not only StatusLines().
		if (!base.engaged && requested && module.HalfFloatPictureFallback())
			return FallbackCatalog::Engaged("output scaling", "adaptive scaler: 16-bit float picture (32-bit float not available)");
		return base;
	}

	FallbackCatalog::Decision Stage::GrainSourceFallback() const
	{
		const char* id = "product.adaptive_downscale";
		if (upscaleThisFrame || !grainOffered || !module.Active())
			return FallbackCatalog::Released(id);
		switch (module.LastGrainSourceUse())
		{
		case AdaptiveDownscale::GrainSourceUse::Shared: return FallbackCatalog::Released(id);
		case AdaptiveDownscale::GrainSourceUse::NoUsableBlock: return FallbackCatalog::Engaged(id, "grain: no usable shared block");
		case AdaptiveDownscale::GrainSourceUse::MeasureDomain: return FallbackCatalog::Engaged(id, "grain: measure domain is linear / HDR");
		// r2 NIT: bound, outcome not read back yet; nothing has been dropped, so no downgrade is claimed (the status line says pending).
		case AdaptiveDownscale::GrainSourceUse::Pending: return FallbackCatalog::Released(id);
		case AdaptiveDownscale::GrainSourceUse::NotSupplied: break;
		}
		return FallbackCatalog::Engaged(id, "grain: the offered shared products are invalid (missing or grid mismatch)");
	}
}
