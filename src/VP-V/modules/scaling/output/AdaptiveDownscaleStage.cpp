#include <pch.h>
#include "AdaptiveDownscaleStage.h"

#include <DebugLog.h>
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
		const bool upscaleBlend = frame.slotRuns && frame.adaptiveUpscale && sourceCropH + 0.5f < targetCropH;
		const bool downscaleOn = frame.slotRuns && frame.adaptiveDownscale;   // slot off: as adaptive_downscale off
		upscaleThisFrame = upscaleBlend;
		// Findings 2026-10-25 #1: what Fallback() reports (no effect on the picture).
		newFailures = module.Failures() - failuresSeen;
		failuresSeen = module.Failures();
		requested = upscaleBlend || downscaleOn;
		ssimMissing = false;
		// SSimSuperRes takes the grain term through alpha only when the blend runs (its
		// vp_grain_strength; 0 otherwise).
		const pl_hook* superresHook = nullptr;
		for (const pl_hook* hook : hooks)
			if (CustomHookChain::SetHookParameter(hook, "vp_grain_strength",
				upscaleBlend && frame.upscaleSuperresScale ? 1.0 : 0.0))
				superresHook = hook;
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
		if (!frame.lowBitrateOff && (upscaleBlend || frame.ssimBitrateAware))
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
		if (upscaleBlend)
		{
			module.SetFrame(adaptiveNow, frame.sourceSequence, FramePeriod(frame, host));
			AdaptiveDownscale::Upscale up;
			up.on = true;
			up.wBase = static_cast<float>(frame.upscaleWBase * state.bitrateWeightScale);
			up.superresAlpha = frame.upscaleSuperresScale && superresFound;
			up.nativeHeight = sourceCropH;
			module.SetUpscale(up);
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
			}
			state.grainMapWanted = adaptive.grainMap && module.Active();
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
				state.grainMapWanted = adaptive.grainMap && module.Active();
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
