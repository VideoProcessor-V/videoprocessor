#pragma once

#include <VP-V/core/FallbackCatalog.h>
#include <VP-V/modules/scaling/output/AdaptiveDownscale.h>

#include <libplacebo/renderer.h>

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

// WP-13 (integration batch 1): the scaling.output glue that lived in the renderer host
// (LibplaceboVideoRenderer.cpp RenderLocked: the Stage 8-2 / 8b measure and blend registration,
// the bitrate weighting, the residual motion and grain field, and the Stage 8-1 light domain) as
// one VP-V stage object. The host keeps one call per hook point:
//
//   adaptiveStage.Register(frame, host, state, frameHooks)   before the sigma-delta domain step
//   adaptiveStage.ApplyLight(frameParams, light)            on the frame's render parameters
//
// The statements, their order and the log lines are exactly today's (moved, not changed): the
// move commit is byte-identical. The maths stays in AdaptiveDownscale.cpp and the shaders.
// Threading: render thread only (as the host code it replaces). No locks.
namespace AdaptiveDownscaleStage
{
	// The de-judder outputs for this source frame (host: DejudderLiveFieldFor(sequence) &&
	// !VpPhaseDrawn(renderImage); the textures only when live).
	struct MotionSource
	{
		bool live = false;
		pl_tex field = nullptr;        // dejudder->Field()
		pl_tex frameTrust = nullptr;   // dejudder->FrameTrust()
		pl_tex grainError = nullptr;   // dejudder->GrainError()
		float minTrustedShare = 0.25f; // dejudder->GetSettings().minTrustedShare
	};

	// The settings the stage reads (from the renderer's active settings) and this frame's facts.
	struct Frame
	{
		bool adaptiveDownscale = false;          // adaptive_downscale
		bool adaptiveUpscale = false;            // adaptive_upscale
		double upscaleWBase = 1.0;               // adaptive_upscale_w_base
		bool upscaleSuperresScale = true;        // adaptive_upscale_superres_scale
		const AdaptiveDownscale::Settings* adaptive = nullptr;   // the module's settings (never null)
		bool lowBitrateOff = false;              // low_bitrate_handling == off
		bool lowBitrateOn = false;               // low_bitrate_handling == on
		bool ssimBitrateAware = true;            // ssim_bitrate_aware
		double ssimBitrateFloor = 0.4;           // ssim_bitrate_floor
		int fboFloat32 = -1;                     // fbo_float32 (Beta builds)
		uint64_t sourceSequence = 0;
		double nominalSourceRateHz = 0.0;
		pl_rect2df sourceCrop{};                 // renderImage.crop
		pl_rect2df targetCrop{};                 // target.crop
		double rebaseFactor = 1.0;               // Stage 12: the vertical re-base factor
		double rebaseFactorX = 1.0;
		bool nlsSubmitted = false;               // submittedNls != nullptr
		const char* nlsFile = "";                // nlsRule.filename (log only)
		// The slot switch (pipeline_slots_off, Original pathway): off = adaptive_downscale and
		// adaptive_upscale off (no hooks): byte-identical to those switches off.
		bool slotRuns = true;
	};

	// What the stage asks the host for, only where today's code asked (each one is a pure read
	// except publishStatus and the fallback frame period's one-time notice).
	struct Host
	{
		std::function<float()> fallbackFramePeriod;              // AdaptiveFallbackFramePeriod()
		std::function<bool(double& bitrateQuality)> bitrate;     // sourceMonitor.Read(): reading, quality
		std::function<NlsMaskMapping::Mapping()> nlsMapping;     // KnownNlsMapping()
		std::function<bool()> ssimInChain;                       // customHooks.SubmitsFile("SSimDownscaler.glsl", lowLatency)
		std::function<MotionSource()> motion;
		std::function<void()> publishStatus;                     // PublishCustomShaderStatus()
	};

	// Host members other host code still reads (status line, the de-judder's grain-map request).
	struct SharedState
	{
		double& bitrateWeightScale;
		bool& grainMapWanted;
		bool& noSsimLogged;
	};

	struct Light
	{
		bool downscaleGamma = false;   // downscale_light == gamma
		bool upscaleGamma = false;     // upscale_light == gamma
		pl_rect2df sourceCrop{};       // renderImage.crop
		pl_rect2df targetCrop{};       // target.crop
		bool slotRuns = true;          // scaling.output's slot switch: off = libplacebo's linear light
	};

	class Stage
	{
	public:
		explicit Stage(AdaptiveDownscale::Module& module) : module(module) {}

		// Stage 8-2 / 8b: sets the module's frame and places its measure (first) and blend hooks.
		void Register(const Frame& frame, const Host& host, SharedState state, std::vector<const pl_hook*>& hooks);

		// Stage 8-1 / 8b: the light domain on the frame's parameters (the de-judder extras reuse them).
		void ApplyLight(pl_render_params& params, const Light& light);

		bool UpscaleThisFrame() const { return upscaleThisFrame; }

		// New module failures seen at `nowMs` (Register calls it each frame; tests drive it directly).
		void NoteFailures(unsigned long long newFailures, unsigned long long nowMs)
		{
			if (newFailures > 0)
				lastFailureTick = nowMs;
			failureHeld = lastFailureTick != 0 && nowMs - lastFailureTick < kFailureHoldMs;
		}

		// ADR-0001 (findings 2026-10-25 #1): the "output scaling" entry for the frame just registered:
		// the blend asked for but SSimDownscaler not in the shader list (libplacebo's downscaler), or
		// the blend failed on the GPU (plain SSIM / the unblended picture). A GPU failure holds the
		// entry for kFailureHoldMs after the last failing frame, so a one-off failure does not flicker
		// in the status and is still listed when a problem report is taken (audit 2026-10-25 M-2).
		static constexpr unsigned long long kFailureHoldMs = 5000;
		FallbackCatalog::Decision Fallback() const
		{
			return FallbackCatalog::OutputScaling(requested, failureHeld ? 1u : 0u, ssimMissing);
		}

	private:
		AdaptiveDownscale::Module& module;
		bool upscaleThisFrame = false;   // Stage 8b on the frame being built
		int upscaleRatioLogged = -1;
		bool mapNlsLogged = false;       // 8-3
		unsigned long long statsTick = 0;
		int lightLogged = -1;            // Stage 8-1: the last logged light domain
		bool requested = false;          // the blend asked for on this frame (downscale on, or the upscale form)
		bool ssimMissing = false;        // downscale on without SSimDownscaler in the chain
		unsigned long long failuresSeen = 0;
		unsigned long long lastFailureTick = 0;   // GetTickCount64() of the last frame with new failures
		bool failureHeld = false;          // a failure within kFailureHoldMs
	};
}
