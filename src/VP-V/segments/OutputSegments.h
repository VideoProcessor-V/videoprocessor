#pragma once

#include <VP-V/core/FallbackStatus.h>
#include <VP-V/core/PipelineSlots.h>
#include <VP-V/core/SegmentRunner.h>
#include <VP-V/core/SlotRegistry.h>
#include <VP-V/modules/output/dither/DitherNeedSeam.h>

#include <libplacebo/renderer.h>
#include <libplacebo/shaders/custom.h>

#include <cstdint>
#include <functional>
#include <memory>
#include <utility>
#include <vector>

namespace CustomHookChain { class Chain; }
namespace LedDrive { class MeasurementHook; }
namespace CalibrationLutStage { class Hook; }
namespace OutputEncodingRepair { class Hook; }
namespace OcclusionMask { class Hook; }
namespace SigmaDeltaDomain { struct Domain; }
namespace HookSegment { class Runner; }

// DRAFT (2026-10-24, not integrated): the OUTPUT-CHAIN segments S2 and S3 as stage adapters
// around the existing modules (central-migration-needs.md §6; core/SegmentRunner.h).
//
//   S2 OutputColourSegment (PRE_OUTPUT): SDR tone shape -> DV trims -> [Stage 13: placeholder]
//      -> LED measurement -> LED compensation (gamma boost / adaptive gamma) -> [black level:
//      placeholder] -> | user PRE_OUTPUT shaders (an edge) | -> calibration LUT (VP stage)
//   S3 OutputFinishSegment (OUTPUT): encoding repair -> occlusion mask -> dither (sigma-delta) LAST
//
// The calibration LUT stays the LAST stage of S2 (not the first of S3): it must act on target
// light before libplacebo's output encoding (Beta 377 encodes between PRE_OUTPUT and OUTPUT),
// and the de-judder rendered-picture cache, a PRE_OUTPUT hook after it, keeps the LUT-mapped
// picture for the extras. S3 can take it only once VP owns the encoding (§6.2).
//
// Separate form (commit 1, the default): the adapters append EXACTLY today's hooks with today's
// parameters in today's order; nothing about a pass changes, so the output is byte-identical.
// The maths stays in the modules (the GLSL files, LedMeasurementHook, CalibrationLutStage,
// OutputEncodingRepair, OcclusionMaskHook); the adapters only own which hooks run, in which
// order, with which parameters.
//
// Merged form (commit 2, output_segments_merged = on, default off): Fuse() replaces each run of
// adjacent segment hooks at one stage with one libplacebo hook (vprenderer/HookSegment.h) that
// hands each stage's output texture straight to the next. Gated by a tolerance test.
namespace OutputSegments
{
	// ---- S2 -------------------------------------------------------------------------------
	struct ToneShapeParams { double headroom = 0, kneeStart = 0, strength = 0, eotf = 2.2, recovery = 0; };
	// The DV trims' shader parameters in the order the host sets them (feat/dv-l8: the selection's
	// list, the ICtCp/output-primaries list, then signal PQ, gamma, peak, kr, kb). Names are string
	// literals (DvTrimSelection / DvDolbyStyle ShaderParameters).
	using ParameterList = std::vector<std::pair<const char*, double>>;
	struct CompensationParams
	{
		double gain = 1, knee = 0.08, gainDomain = 0, eotf = 2.2, boost = 1, boostKnee = 0, boostKneeWidth = 0.05;
		double shadowLift = 0, veil = 0, wtwPassthrough = 1, btbPassthrough = 1;
		// GEM-LED-EXTRANGE-HOST: exact 10-bit limited SDI/HDMI extended code bounds [4, 1019] over [64, 940]
		// -> [(4-64)/876, (1019-64)/876] = [-60/876, 955/876]; full-range targets set [0.0, 1.0].
		double extendedFloor = -60.0 / 876.0, extendedCeiling = 955.0 / 876.0;
	};

	// What the host decided for this frame (the decisions, logging and fallbacks stay in the host
	// until the b3-b7 extractions move them into the stages).
	struct ColourFrame
	{
		bool toneShape = false;
		ToneShapeParams tone;
		bool dvTrims = false;
		ParameterList dv;
		// CF-1 CAM-3: colour.appearance. The chain is DvHostStage's (it owns the loader); null or appearance = false adds nothing.
		bool appearance = false;
		ParameterList appearanceParams;
		CustomHookChain::Chain* appearanceChain = nullptr;
		bool ledMeasure = false;        // led && pipelineSlots.Runs(DriveLedMeasure)
		bool ledBlock = false;          // led || boost != 1 || shadow lift > 0 (the compensation block)
		bool ledCompensation = false;   // pipelineSlots.Runs(DriveLedCompensation)
		CompensationParams compensation;
		const std::vector<const pl_hook*>* userPreOutput = nullptr;   // the edge (customHooks.Collect)
		bool calibrationLut = false;    // calibrationLutStage.Lut() != nullptr
	};

	// How many hooks each stage contributed (the host's "NOT applied" states read these).
	// unhandled = slots that run but have no S2 stage yet (a component registered in a placeholder
	// slot before its stage is written): the host reports them, never silently skipped.
	struct ColourCounts { size_t toneShape = 0, dvTrims = 0, appearance = 0, ledMeasure = 0, ledCompensation = 0, ledCompensationLoaded = 0, user = 0, lut = 0, unhandled = 0; };

	// What PrepareDither did. A domain drop (the sigma-delta cannot quantise this target) is a
	// fallback; the slot switched off is a choice and never engages one (merge audit 2026-10-25).
	struct DitherPrep
	{
		bool found = false;          // a sigma-delta hook was in the list
		bool domainDropped = false;  // removed: unsupported code domain (slot on)
		bool slotOff = false;        // removed: output.dither switched off
	};
	// The "requested" argument for FallbackCatalog::Dither: never with the slot off.
	inline bool DitherFallbackRequested(const DitherPrep& prep, bool quantiserProblem)
	{
		return !prep.slotOff && (prep.found || quantiserProblem);
	}

	using TagFn = std::function<void(size_t start, PipelineSlots::Id slot)>;

	class OutputColourSegment
	{
	public:
		OutputColourSegment(CustomHookChain::Chain& toneShape, CustomHookChain::Chain& dvTrims,
			LedDrive::MeasurementHook& measure, CustomHookChain::Chain& compensation, CalibrationLutStage::Hook& lut);

		// Separate form: appends today's S2 hooks to `hooks` (tagging slot hooks through `tag`, the
		// host's TagSlotHooks) and records them as this frame's S2 members.
		// The stages follow the registry's effective order and run only when their slot runs.
		ColourCounts Collect(const ColourFrame& frame, const PipelineSlots::Registry& registry,
			std::vector<const pl_hook*>& hooks, const TagFn& tag);
		bool Member(const pl_hook* hook) const;
		// The slot a member hook belongs to (Id::Count = not a member), for per-slot timing.
		PipelineSlots::Id SlotOf(const pl_hook* hook) const;
		SegmentRunner::Segment& Segment() { return segment; }

	private:
		CustomHookChain::Chain& toneShape;
		CustomHookChain::Chain& dvTrims;
		LedDrive::MeasurementHook& measure;
		CustomHookChain::Chain& compensation;
		CalibrationLutStage::Hook& lut;
		std::vector<const pl_hook*> members;
		std::vector<PipelineSlots::Id> memberSlots;   // parallel to members
		SegmentRunner::Segment segment;
	};

	// ---- S3 -------------------------------------------------------------------------------
	// W23-4p (G-T1): per-frame GPU dither_need request (compiled implementation in OutputSegments.cpp).
	struct DitherNeedGpuRequest
	{
		bool requested = false, originalPathway = false, sigmaDeltaActive = false, slotOff = false;
		bool domainSupported = true, nlsActive = false, discontinuity = false, overrideInputs = false, requireBind = true;
		int colorBits = 8, regionTransfer = 0;
		pl_gpu gpu = nullptr;
		const void* host = nullptr;
		uint64_t sourceSequence = 0;
		double dtSeconds = 0.0, aBase = 0.15, overrideGrainAgeSeconds = 0.0, overrideEvidenceAgeSeconds = 0.0;
		pl_rect2df sourceCrop{}, targetCrop{};
		LightLevel::Settings lightSettings;
		LightLevel::Context lightContext;
		const SourceIdentity::QualityView* quality = nullptr;
		pl_tex overrideGsBlocks = nullptr;
		pl_tex overridePrecursors[RegionEvidence::kPrecursorTextures] = {};
		RegionEvidence::Grid overrideGrid;
	};

	// The OUTPUT end. In the separate form it is four steps at today's four places, because
	// host hooks (NLS, the ExtendedRange restores, the de-judder cache) are placed between them;
	// the merged form is the one exit.
	class OutputFinishSegment
	{
	public:
		OutputFinishSegment(OutputEncodingRepair::Hook& repair, OcclusionMask::Hook& mask);
		~OutputFinishSegment();

		void BeginFrame() { members.clear(); }
		// 1. The dither's code domain (today 24539-24567); an unsupported domain removes it.
		//    Returns true when a dither hook was found.
		//    output.dither off in the registry removes it the same way.
		DitherPrep PrepareDither(std::vector<const pl_hook*>& hooks, const SigmaDeltaDomain::Domain& domain, bool outputFloat32,
			const PipelineSlots::Registry& registry);
		static std::string DescribeDitherDomainChange(const SigmaDeltaDomain::Domain& domain, bool active, int& lastSignature);
		static FallbackCatalog::Decision DitherFallback(const DitherPrep& prep, const SigmaDeltaDomain::Domain& domain,
			const std::string& quantiserProblem);
		// 1b. dither_need (default off): CPU host seam decision and live GPU block-pass/HookGuard execution.
		DitherNeedSeam::Outcome EvaluateDitherNeed(bool requested, bool originalPathway,
			const DitherNeedSeam::Feed& feed = DitherNeedSeam::Feed(),
			const RegionEvidence::Grid& grid = RegionEvidence::Grid(),
			const DitherNeed::Params& base = DitherNeed::Params());
		static void PopulateDitherNeedRequest(pl_gpu gpu, const void* host, uint64_t sourceSequence,
			bool sigmaDeltaActive, bool slotOff, bool domainSupported, int colorBits, bool nlsActive,
			const SourceIdentity::QualityView* quality, double sdrTargetNits, double sdrBlackNits, const char* hdrTargetGamma,
			const char* toneMapping, const char* qualitySetting, const ToneMappingTuning::Overrides& toneTuning,
			const SdrToneShape::Settings& sdrTone, const char* sdrInputTransfer, const char* outputGamma,
			bool hasContrastRecovery, double contrastRecovery, const pl_frame& sourceFrame, const pl_frame& targetFrame,
			double captureRateHz, double nominalSourceRateHz, const CustomHookChain::Chain& customHooks, DitherNeedGpuRequest& req);
		std::string FormatDitherNeedLog(const std::string& status) const;
		DitherNeedSeam::Outcome RunDitherNeedGpu(const DitherNeedGpuRequest& req, const pl_hook* const* hooks, int numHooks,
			const std::function<void(std::function<void()>)>& attachRelease = nullptr);
		void ReleaseDitherNeedBind();
		void ReleaseGpu(pl_gpu gpu);
		pl_tex DitherNeedCurrent() const;
		size_t DitherNeedVramBytes() const;
		std::string DitherNeedLastError() const;
		bool DitherNeedDemandActive() const;
		const DitherNeedSeam::Counters& DitherNeedCounters() const;
		bool DitherNeedStatusChanged(const std::string& status);
		// 2. The dither runs last at OUTPUT (24572-24593). Returns true when it was moved.
		bool KeepDitherLast(std::vector<const pl_hook*>& hooks, bool levelsFull);
		// 3. The mask parameters (24637-24644): returns the dither's index (hooks.size() = none).
		size_t MarkMask(std::vector<const pl_hook*>& hooks, bool install, bool grainInstall, double grainScale);
		//    and the mask hook just before the dither (24671).
		void InsertMask(std::vector<const pl_hook*>& hooks, size_t ditherIndex);
		// 4. The libplacebo 7.360 encoding repair, first at OUTPUT (24786-24799). True = inserted.
		bool Repair(std::vector<const pl_hook*>& hooks, const pl_render_errors* errors, enum pl_lut_type targetLut,
			const pl_color_repr& targetRepr);
		// 5. GEM-025(a) (sigma-delta MUST-8): libplacebo disabled the occlusion mask hook (its signature is in the render's
		//    disabled_hooks). Latches for the session (the hook stays disabled in libplacebo); true only on the call that latched.
		//    While latched the host installs neither the mask nor the grain map (install = grainInstall = false).
		// 6. GEM-025(b) (sigma-delta MUST-8): the NLS retry redraw must not read the failed draw's vectors or mask.
		//    NlsHookRecovery::RenderOwned calls the frame's draw exactly twice only on an NLS retry, so the 2nd OnDraw of a frame is
		//    the retry. The callback is type-erased: SigmaDeltaVectors::NextVectorGuard is internal to each translation unit.
		//    BeginFrameDraws: per frame, before the render; resets the draw count (an empty callback = Original pathway, OnDraw no-op).
		void BeginFrameDraws(std::function<void()> releaseVectors);
		//    OnDraw: ++draws; on the 2nd: releaseVectors(), occlusion_mask 0 on every parameters.hooks entry, maskParamSet = false,
		//    ++NlsRetryReleases. Returns true then.
		bool OnDraw(pl_render_params& parameters, bool& maskParamSet);
		//    EndFrameDraws: after the render; drops the callback (it references the frame's local guard).
		void EndFrameDraws();
		unsigned long long NlsRetryReleases() const { return nlsRetryReleases; }
		bool NoteDisabledHooks(const uint64_t* signatures, int count);
		bool OcclusionMaskDisabled() const { return maskDisabled; }
		bool Member(const pl_hook* hook) const;
		// The stage a member hook is ("encoding_repair", "occlusion_mask", "sigma_delta").
		const char* StageName(const pl_hook* hook) const;
		SegmentRunner::Segment& Segment() { return segment; }

	private:
		struct DitherNeedGpuState;
		void Note(const pl_hook* hook);
		OutputEncodingRepair::Hook& repair;
		OcclusionMask::Hook& mask;
		std::vector<const pl_hook*> members;
		SegmentRunner::Segment segment;
		DitherNeedSeam::Stage ditherNeed;
		std::unique_ptr<DitherNeedGpuState> ditherNeedGpu;
		std::string ditherNeedLogged;
		bool maskDisabled = false;
		std::function<void()> releaseVectors;
		int draws = 0;
		unsigned long long nlsRetryReleases = 0;
	};
}
