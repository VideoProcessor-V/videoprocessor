#pragma once

#include <VP-V/core/FallbackCatalog.h>
#include <VP-V/core/SkeletonAcquire.h>
#include <VP-V/modules/repair/temporal_detail/products/TdProductReads.h>
#include <VP-V/modules/scaling/output/AdaptiveDownscale.h>
#include <VP-V/modules/scaling/output/PicturePrecision.h>

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
	// CF-2 switch-over: motion stays on this read (the ExternalView of P3) until P3 has a GPU Acquire publication: at this tip
	// kP3AnalysisField has no dispatcher texture set and no owner fill (SkeletonModule::gpuHandleOf answers NoGpuHandle), and
	// frameTrust / grainError are in no product. The switch is an Acquire in Register when that lands.
	struct MotionSource
	{
		bool live = false;
		pl_tex field = nullptr;        // the P3 field view (EstimateState::LiveFor): field
		pl_tex frameTrust = nullptr;   // ... frameTrust
		pl_tex grainError = nullptr;   // ... grainError
		float minTrustedShare = 0.25f; // EstimateState::MinTrustedShare()
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
		// Motion vectors can run (motion.estimation and motion.vector_delivery on; feat/m2-motion's
		// MotionVectorsRun()): false = the grain map never asks for an estimate (zero-cost rule).
		bool vectorsRun = true;
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
		// X5-GRAIN-P10: this source frame's shared grain products (P10 N blocks, P11 meanY / coverage); null or an invalid
		// source = none, and the scaler keeps its own Laplacian estimate (a status line says so).
		std::function<AdaptiveDownscale::GrainSource()> grainSource;
		std::function<void()> publishStatus;                     // PublishCustomShaderStatus()
		// Stage 8b one-axis: true for a LUMA prescaler hook (customHooks.IsPrescalerHook); null =
		// none known (a doubler then stays in the list; the blend is still faithful-only).
		std::function<bool(const pl_hook*)> isPrescaler;
		// CF-2 B1 ruling (a): the skeleton Acquire of a published product for this frame (id, sequence, variant; GPU form). Null =
		// the production path (the current frame's skeleton memo, Vpv::Skeleton::CurrentFrameMemo); tests count the asks.
		std::function<VpSkeleton::AcquireResult(VpSkeleton::ProductId, uint64_t, const VpSkeleton::VariantKey&)> acquire;
	};

	// CF-2 switch-over: the grain term's seam (kP10gGrainLevel, one Acquire per upscale frame). Published = the module reads it;
	// anything else = grain_floor (the alias-safe lean) with the cause, on the status and the catalog. The edge term is the
	// module's own kEdgeMap reader (CB-18 S18: the measure acquires the stage variant; EdgeMapStatus). Pure data (tests).
	struct TermSource
	{
		bool published = false;
		std::string cause;   // why the product was not read (empty when published)
	};

	// CF-2 B1 (N1.4): why the vectors of this source frame are not usable by the upscale form's motion term (nullptr = usable):
	// motion off, NLS, no live estimate, a drop (a sequence gap) or a resync (the sequence went back). A repeat of the last
	// sequence keeps the first presentation's verdict (the caller). Pure (tests).
	const char* MotionDiscontinuity(bool vectorsRun, bool nls, bool live, bool haveLast, uint64_t lastSequence, uint64_t sequence);

	// CF-2 FIX1 (gate M1, S2): the inputs of the upscale form's "output scaling" entry for one frame.
	struct UpscaleFallbackInputs
	{
		bool requested = false;
		bool failureHeld = false;
		std::string scalerError;
		bool motionAdaptiveOn = false;
		bool oneAxis = false;
		std::string stageMotionCause;    // the stage's discontinuity verdict (empty: the vectors are usable)
		bool moduleFloorUsed = false;    // the module leaned on the motion floor on its own (no usable factor map)
		std::string moduleFloorCause;
		bool cutAvailable = true;
		std::string cutCause;
		std::string grainCause;          // kP10gGrainLevel not read / domain-refused this frame (grain_floor); empty when read or one-axis
		std::string edgeMapStatus;       // S4: module.EdgeMapStatus() when kEdgeMap was not served by the skeleton memo
		bool motionExternalView = false; // M4: P3 motion read via host.motion ExternalView (P3 has no GPU publication)
	};

	// The upscale form's one "output scaling" decision (the host engages one per frame, CoreSegments): a GPU failure first,
	// then the motion floor (the stage's verdict, or the module's own floor with its cause: gate M1), then no cut verdict,
	// then the grain floor (kP10gGrainLevel not read: Config status and the notice, not only Ctrl+I). Pure (Fallback(), tests).
	FallbackCatalog::Decision UpscaleScalingFallback(const UpscaleFallbackInputs& in);

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
		double rebaseFactor = 1.0;     // Stage 12: the vertical re-base factor (libplacebo samples crop x factor)
		double rebaseFactorX = 1.0;    // ... and the horizontal one
	};

	// fbo_float32 no longer lives here: PicturePrecision.h decides it from the setting alone (S-12BIT).

	class Stage
	{
	public:
		explicit Stage(AdaptiveDownscale::Module& module) : module(module) {}

		// Stage 8-2 / 8b: sets the module's frame and places its measure (first) and blend hooks.
		void Register(const Frame& frame, const Host& host, SharedState state, std::vector<const pl_hook*>& hooks);

		// Stage 8-1 / 8b: the light domain on the frame's parameters (the de-judder extras reuse them).
		void ApplyLight(pl_render_params& params, const Light& light);

		bool UpscaleThisFrame() const { return upscaleThisFrame; }
		// Batch B Q1 (plan-motion-engine-switch.md): a hybrid scaler (the adaptive downscale or the upscale form) registered on this frame
		// and motion can run: it is a vector user, so it registers a vector want (the host sets the ResidualStabilise want, DejudderPolicyRunner.inl).
		// Separate from grainMapWanted, which also asks the estimate for the grain error map (only with the grain map or motion-adaptive on).
		bool UsesVectors() const { return vectorUser; }
		// Stage 8b on a one-axis upscale this frame (faithful only; the prescaler dropped).
		bool OneAxisThisFrame() const { return oneAxisThisFrame; }
		// Prescaler hooks dropped from this frame's list (one-axis upscale).
		int DroppedPrescalers() const { return droppedPrescalers; }

		// The stage's own status lines (no silent fallbacks): the 8b form on the last registered
		// frame and the motion-adapted blend's state; empty when neither applies. The host adds
		// them to the custom-shader status.
		std::vector<std::string> StatusLines() const;

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
		FallbackCatalog::Decision Fallback() const;

		// R1-W1-RES: the read path of the temporal-detail engine's products (confidence, real detail, grain, stability, motion
		// model). Reset 1: the placeholder producer publishes nothing and the engine is not requested, so this is "not requested"
		// (classical weights, byte-identical, no status line). Once the engine is requested and a product is absent, the classical
		// bitrate-quality weights stand in VISIBLY: the "product.adaptive_downscale" decision is engaged and StatusLines() carries its line.
		const TdProducts::ReadResult& TemporalDetailRead() const { return tdRead; }
		// CF-2 B1: this frame's grain seam and the upscale motion verdict (status, tests).
		const TermSource& GrainTerm() const { return grainTerm; }
		const std::string& UpscaleMotionCause() const { return upscaleMotionCause; }
		FallbackCatalog::Decision TemporalDetailFallback() const { return tdRead.fallback; }

		// X5-GRAIN-P10 (gate cb-u1 r1 SHOULD 6): the host offers the shared grain products (Host::grainSource) but the scaler
		// did not use them on the last read-back frame. That engages "product.adaptive_downscale" with the reason; not offered
		// (no callback) is no downgrade (the private estimate is then the design).
		// Two entries by design (gate cb-u1 r3 MUST 1): the grain decision is a CLASSICAL cause and owns the bare id (host:
		// FallbackCatalog::Sync(registry, GrainSourceFallback())); the temporal-detail read is keyed by cause,
		// "product.adaptive_downscale [td: ...]" (TdProducts::FallbackKey; host: TdProducts::SyncRead on TemporalDetailRead()),
		// never Sync() under the bare id. Neither stands for, overwrites or releases the other (TdProductReads.h).
		FallbackCatalog::Decision GrainSourceFallback() const;

	private:
		AdaptiveDownscale::Module& module;
		bool upscaleThisFrame = false;   // Stage 8b on the frame being built
		bool oneAxisThisFrame = false;   // ... upscaled on one axis only (faithful only)
		int droppedPrescalers = 0;
		bool motionAdaptiveOn = false;   // adaptive_motion strength > 0 on this frame (the blend registered)
		float motionStrengthActive = 0.0f;   // active clamped continuum strength (0..1) on this frame
		bool oneAxisLogged = false;
		std::string statusPublished;     // the StatusLines last published
		int upscaleRatioLogged = -1;
		bool mapNlsLogged = false;       // 8-3
		unsigned long long statsTick = 0;
		int lightLogged = -1;            // Stage 8-1: the last logged light domain
		bool requested = false;          // the blend asked for on this frame (downscale on, or the upscale form)
		bool ssimMissing = false;        // downscale on without SSimDownscaler in the chain
		bool vectorUser = false;         // a hybrid form registered on this frame and motion can run (UsesVectors)
		unsigned long long failuresSeen = 0;
		unsigned long long lastFailureTick = 0;   // GetTickCount64() of the last frame with new failures
		bool failureHeld = false;          // a failure within kFailureHoldMs
		TdProducts::ReadResult tdRead;     // the temporal-detail read path of the frame just registered (nothing used in reset 1)
		bool grainOffered = false;         // X5-GRAIN-P10: the host offered the shared grain products on the frame just registered
		TermSource grainTerm;              // CF-2 B1: this frame's grain seam (kP10gGrainLevel)
		std::string upscaleMotionCause;    // ... why the upscale motion term is floored (empty: vectors usable)
		bool upscaleMotionExternalView = false;   // M4: P3 read via host.motion ExternalView (no GPU publication yet)
		bool haveMotionSequence = false;   // the last upscale frame's sequence (drop / resync / repeat)
		uint64_t motionSequenceSeen = 0;
	};
}
