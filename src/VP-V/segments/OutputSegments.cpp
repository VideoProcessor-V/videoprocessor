#include <pch.h>
#include "OutputSegments.h"

#include <VP-V/modules/output/calibration_lut/CalibrationLutStage.h>
#include <VP-V/modules/drive/led_measure/LedMeasurementHook.h>
#include <VP-V/modules/output/dither/CustomHookChain.h>
#include <VP-V/modules/motion/consumers/OcclusionMaskHook.h>
#include <VP-V/modules/output/dither/OutputEncodingRepair.h>
#include <VP-V/modules/output/dither/SigmaDeltaDomain.h>
#include <VP-V/core/GpuAttributionPl.h>
#include <VP-V/core/VramUsers.h>
#include <VP-V/modules/control/skeleton/SkeletonFacts.h>
#include <VP-V/modules/output/dither/DitherNeedGpu.h>
#include <VP-V/modules/control/config/VpvRendererFields.h>
#include <VP-V/core/OutputHookOrder.h>
#include <VP-V/core/SlotSwitchSettings.h>
#include <VP-V/modules/output/presentation/PresentationOwner.h>
#include <VP-V/modules/scaling/rebase/EntranceGpuDrain.h>
#include <VP-V/segments/CoreSegmentCaches.h>
#include <DebugLog.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <iterator>

namespace OutputSegments
{
	using PipelineSlots::Id;
	using CustomHookChain::SetHookParameter;

	OutputColourSegment::OutputColourSegment(CustomHookChain::Chain& toneShapeIn, CustomHookChain::Chain& dvTrimsIn,
		LedDrive::MeasurementHook& measureIn, CustomHookChain::Chain& compensationIn, CalibrationLutStage::Hook& lutIn)
		: toneShape(toneShapeIn), dvTrims(dvTrimsIn), measure(measureIn), compensation(compensationIn), lut(lutIn),
		segment(SegmentRunner::kS2FallbackId, SegmentRunner::OutputColourStages(), SegmentRunner::OutputColourEntry())
	{
	}

	ColourCounts OutputColourSegment::Collect(const ColourFrame& f, const PipelineSlots::Registry& registry,
		std::vector<const pl_hook*>& hooks, const TagFn& tag)
	{
		ColourCounts counts;
		members.clear();
		memberSlots.clear();
		auto note = [this, &hooks](size_t start, Id slot)
		{
			for (size_t i = start; i < hooks.size(); ++i)
			{
				members.push_back(hooks[i]);
				memberSlots.push_back(slot);
			}
		};

		// colour.sdr_tone_shape (delta 13).
		auto toneStage = [&]()
		{
			if (f.toneShape)
			{
				const size_t start = hooks.size();
				toneShape.Collect(false, hooks);
				tag(start, Id::ColourSdrToneShape);
				for (size_t i = start; i < hooks.size(); ++i)
				{
					SetHookParameter(hooks[i], "headroom", f.tone.headroom);
					SetHookParameter(hooks[i], "knee_start", f.tone.kneeStart);
					SetHookParameter(hooks[i], "strength", f.tone.strength);
					SetHookParameter(hooks[i], "eotf", f.tone.eotf);
					SetHookParameter(hooks[i], "recovery", f.tone.recovery);
				}
				counts.toneShape = hooks.size() - start;
				note(start, Id::ColourSdrToneShape);
			}
		};
		// colour.dm_trims (CF-1 P3): the DV L8 / L2 trims, their own slot and switch (before: the colour slot's).
		auto dmTrimsStage = [&]()
		{
			if (f.dvTrims)
			{
				const size_t start = hooks.size();
				dvTrims.Collect(false, hooks);
				tag(start, Id::ColourDmTrims);   // CF-1 P3: the trims are the colour.dm_trims slot (own switch)
				for (size_t i = start; i < hooks.size(); ++i)
					for (const auto& parameter : f.dv)
						SetHookParameter(hooks[i], parameter.first, parameter.second);
				counts.dvTrims = hooks.size() - start;
				note(start, Id::ColourDmTrims);
			}
		};
		// colour.appearance (CF-1 CAM-3): the CAM16 room and screen adaptation, after the DV trims and before the LED measurement.
		auto appearanceStage = [&]()
		{
			if (!f.appearance || !f.appearanceChain)
				return;
			const size_t start = hooks.size();
			f.appearanceChain->Collect(false, hooks);
			tag(start, Id::ColourAppearance);
			for (size_t i = start; i < hooks.size(); ++i)
				for (const auto& parameter : f.appearanceParams)
					SetHookParameter(hooks[i], parameter.first, parameter.second);
			counts.appearance = hooks.size() - start;
			note(start, Id::ColourAppearance);
		};
		// drive.led_measure: sees the picture before the gain.
		auto measureStage = [&]()
		{
			if (!f.ledBlock || !f.ledMeasure)
				return;
			hooks.push_back(measure.Hook());
			tag(hooks.size() - 1, Id::DriveLedMeasure);
			counts.ledMeasure = 1;
			note(hooks.size() - 1, Id::DriveLedMeasure);
		};
		// drive.led_compensation (gain, gamma boost, shadow lift, veil; BTB/WTW passthrough).
		auto compensationStage = [&]()
		{
			if (!f.ledBlock)
				return;
			const size_t start = hooks.size();
			const CompensationParams& c = f.compensation;
			// GEM-200 (PERF-3, matching shaders/LED compensation.glsl:181-183 & W1B GEM-186):
			// skip pushing the full-screen pass when the shader is a pure identity, while still
			// reporting ledCompensationLoaded so genuinely missing shaders still hold full drive.
			const bool identity = c.gain == 1.0 && c.boost == 1.0 && !(c.shadowLift * c.veil > 0.0);
			if (f.ledCompensation)
			{
				if (identity)
					counts.ledCompensationLoaded = compensation.SubmittedCount(false);
				else
				{
					compensation.Collect(false, hooks);
					counts.ledCompensationLoaded = hooks.size() - start;
				}
			}
			tag(start, Id::DriveLedCompensation);
			for (size_t i = start; i < hooks.size(); ++i)
			{
				SetHookParameter(hooks[i], "gain", c.gain);
				SetHookParameter(hooks[i], "knee", c.knee);
				SetHookParameter(hooks[i], "gain_domain", c.gainDomain);
				SetHookParameter(hooks[i], "compensation_eotf", c.eotf);
				SetHookParameter(hooks[i], "boost", c.boost);
				SetHookParameter(hooks[i], "boost_knee", c.boostKnee);
				SetHookParameter(hooks[i], "boost_knee_width", c.boostKneeWidth);
				SetHookParameter(hooks[i], "shadow_lift", c.shadowLift);
				SetHookParameter(hooks[i], "veil", c.veil);
				SetHookParameter(hooks[i], "wtw_passthrough", c.wtwPassthrough);
				SetHookParameter(hooks[i], "btb_passthrough", c.btbPassthrough);
				SetHookParameter(hooks[i], "extended_floor", c.extendedFloor);
				SetHookParameter(hooks[i], "extended_ceiling", c.extendedCeiling);
			}
			counts.ledCompensation = hooks.size() - start;
			note(start, Id::DriveLedCompensation);
		};
		// The edge: the user's shaders. They run before VP's calibration LUT (R1), wherever the
		// module order puts the LUT, exactly as today.
		bool edgeDone = false;
		auto edge = [&]()
		{
			if (edgeDone)
				return;
			edgeDone = true;
			if (f.userPreOutput)
			{
				hooks.insert(hooks.end(), f.userPreOutput->begin(), f.userPreOutput->end());
				counts.user = f.userPreOutput->size();
			}
		};
		// output.calibration_lut (VP stage, R1): VP's last colour hook in the designed order.
		auto lutStage = [&]()
		{
			edge();
			if (!f.calibrationLut)
				return;
			hooks.push_back(lut.Get());
			tag(hooks.size() - 1, Id::OutputCalibrationLut);
			counts.lut = 1;
			note(hooks.size() - 1, Id::OutputCalibrationLut);
		};

		// The stages in the REGISTRY's effective order (validated by OrderValidator against
		// SlotContracts), each only when its slot runs (pipeline_slots_off, Original pathway): a slot
		// that is off adds no hook. drive.black_level is a
		// placeholder: no component, so it never runs and costs nothing.
		struct Stage { Id slot; int order; int kind; };
		Stage stages[] = {
			{ Id::ColourSdrToneShape, registry.Order(Id::ColourSdrToneShape), 0 },
			{ Id::ColourDmTrims, registry.Order(Id::ColourDmTrims), 5 },
			{ Id::ColourAppearance, registry.Order(Id::ColourAppearance), 6 },   // CAM-3: the appearance pass
			{ Id::DriveLedMeasure, registry.Order(Id::DriveLedMeasure), 1 },
			{ Id::DriveLedCompensation, registry.Order(Id::DriveLedCompensation), 2 },
			{ Id::DriveBlackLevel, registry.Order(Id::DriveBlackLevel), 4 },     // placeholder
			{ Id::OutputCalibrationLut, registry.Order(Id::OutputCalibrationLut), 3 },
		};
		std::stable_sort(std::begin(stages), std::end(stages), [](const Stage& x, const Stage& y) { return x.order < y.order; });
		for (const Stage& st : stages)
		{
			if (!registry.Runs(st.slot))
			{
				if (st.kind == 3)
					edge();   // the user's shaders still run where the LUT would be
				continue;
			}
			switch (st.kind)
			{
			case 0: toneStage(); break;
			case 1: measureStage(); break;
			case 2: compensationStage(); break;
			case 3: lutStage(); break;
			case 5: dmTrimsStage(); break;
			case 6: appearanceStage(); break;
			default:
				// A placeholder slot with a component but no S2 stage yet: counted for the host to report
				// (it never runs silently, and never runs half-wired).
				++counts.unhandled;
				break;
			}
		}
		edge();
		return counts;
	}

	bool OutputColourSegment::Member(const pl_hook* hook) const
	{
		return std::find(members.begin(), members.end(), hook) != members.end();
	}

	Id OutputColourSegment::SlotOf(const pl_hook* hook) const
	{
		for (size_t i = 0; i < members.size(); ++i)
			if (members[i] == hook)
				return memberSlots[i];
		return Id::Count;
	}

	struct OutputFinishSegment::DitherNeedGpuState
	{
		static constexpr uint16_t kConsumerId = 0x444Eu;   // "DN" — unique consumer id for dither_need P10/P11/P12 demand

		DitherNeed::Gpu::BlockPass pass;
		DitherNeed::Gpu::HookGuard guard;
		LightLevel::LutCache lutCache;
		pl_gpu gpu = nullptr;
		pl_dispatch dispatch = nullptr;
		pl_tex lutTex = nullptr;
		pl_tex lutOutTex = nullptr;
		bool demandActive = false;
		const void* demandHost = nullptr;
		uint64_t demandErasures = 0;
		// Per-source-frame reuse state so secondary motionDraws / cadenceRepeat draws of the same
		// sourceSequence reuse st.pass.Current() without re-stepping temporal EMA or calling Invalidate().
		bool hasSteppedSequence = false;
		uint64_t lastSteppedSourceSequence = 0;
		std::string lastSteppedLutFingerprint;
		int lastSteppedGridW = 0;
		int lastSteppedGridH = 0;
		int lastSteppedBlockPx = 0;
		int lastSteppedColorBits = 0;
		int lastSteppedSourceBits = 0;
		double lastSteppedABase = 0.0;
		DitherNeedSeam::Outcome lastSteppedOutcome;

		void ResetSteppedSequence()
		{
			hasSteppedSequence = false;
			lastSteppedSourceSequence = 0;
			lastSteppedLutFingerprint.clear();
			lastSteppedGridW = 0;
			lastSteppedGridH = 0;
			lastSteppedBlockPx = 0;
			lastSteppedColorBits = 0;
			lastSteppedSourceBits = 0;
			lastSteppedABase = 0.0;
			lastSteppedOutcome = DitherNeedSeam::Outcome();
		}

		void ReleaseGpuResources(pl_gpu releaseGpu)
		{
			guard.Release();
			ResetSteppedSequence();
			pl_gpu activeGpu = gpu;
			if (activeGpu != nullptr && releaseGpu == activeGpu)
			{
				pass.Release(activeGpu);
				if (lutTex) pl_tex_destroy(activeGpu, &lutTex);
				if (lutOutTex) pl_tex_destroy(activeGpu, &lutOutTex);
				if (dispatch) pl_dispatch_destroy(&dispatch);
			}
			else if (activeGpu == nullptr && releaseGpu != nullptr)
			{
				pass.Release(releaseGpu);
			}
			else
			{
				// Device changed or already gone (releaseGpu == nullptr): forget handles without calling libplacebo on a dead device.
				pass.Release(nullptr);
				lutTex = nullptr;
				lutOutTex = nullptr;
				dispatch = nullptr;
			}
			gpu = nullptr;
			lutCache.Reset();
		}

		static bool UploadLut1024(pl_gpu g, pl_tex& tex, const std::vector<float>& rgba)
		{
			if (!g || rgba.size() != static_cast<size_t>(LightLevel::kLutSize) * 4u)
				return false;
			const pl_fmt f4 = VpvGpu::FloatFormat(g, 4);
			if (!f4)
				return false;
			if (tex)
				pl_tex_destroy(g, &tex);
			pl_tex_params tp{};
			tp.w = LightLevel::kLutSize;
			tp.h = 1;
			tp.format = f4;
			tp.sampleable = true;
			tp.initial_data = rgba.data();
			tex = pl_tex_create(g, &tp);
			return tex != nullptr;
		}

		size_t TotalVramBytes() const
		{
			return pass.VramBytes() +
				static_cast<size_t>(VpSkeleton::TexBytesOf(lutTex)) +
				static_cast<size_t>(VpSkeleton::TexBytesOf(lutOutTex));
		}

		void SyncVram(const void* owner) const
		{
			const size_t bytes = TotalVramBytes();
			if (bytes > 0)
				VpSkeleton::VramUsers::Instance().DeclareFixed(owner, VpSkeleton::kUserDitherNeed, static_cast<uint64_t>(bytes));
			else
				VpSkeleton::VramUsers::Instance().Release(owner, VpSkeleton::kUserDitherNeed);
		}

		void EnsureDemand(const void* host, const void* owner)
		{
			if (!host)
				return;
			Vpv::Skeleton::IExternalDemand* demand = Vpv::Skeleton::Demand();
			if (!demand)
				return;
			const uint64_t erasures = demand->ExternalErasures(host);
			if (demandActive && demandHost == host && demandErasures == erasures)
				return;
			if (demandActive && demandHost != nullptr && demandHost != host)
				demand->ClearExternalDemand(demandHost, owner);
			VpSkeleton::ConsumerDecl c;
			c.id = kConsumerId;
			c.name = "output.dither (dither need)";
			VpSkeleton::AddConsume(c, VpSkeleton::kP10GrainSplit, 0);
			VpSkeleton::AddConsume(c, VpSkeleton::kP11RegionBlockStats, 0);
			VpSkeleton::AddConsume(c, VpSkeleton::kP12LightLevel, 0);
			const uint64_t before = demand->ExternalErasures(host);
			demand->SetRaisingExternalDemand(host, owner, std::vector<VpSkeleton::ConsumerDecl>(1, c),
				std::vector<VpSkeleton::ProductId>(), std::string("dither need"));
			demandActive = true;
			demandHost = host;
			demandErasures = before;
		}

		void ClearDemand(const void* owner)
		{
			if (!demandActive)
				return;
			if (Vpv::Skeleton::IExternalDemand* demand = Vpv::Skeleton::Demand())
				if (demandHost != nullptr)
					demand->ClearExternalDemand(demandHost, owner);
			demandActive = false;
			demandHost = nullptr;
			demandErasures = 0;
		}

		static Vpv::Skeleton::DitherNeedDisplayFacts BuildDisplayFacts(const DitherNeedGpuRequest& req)
		{
			Vpv::Skeleton::DitherNeedDisplayFacts df;
			df.regionTransfer = req.regionTransfer;
			df.sdrWhiteNits = req.lightSettings.displayWhiteNits;
			df.displayPeakNits = req.lightContext.displayPeakNits > 0.0
				? req.lightContext.displayPeakNits : req.lightSettings.displayWhiteNits;
			df.displayBlackNits = req.lightSettings.displayBlackNits;
			df.ambientNits = req.lightSettings.ambientNits;
			df.weber = req.lightSettings.weber;
			df.sdrGamma = LightLevel::EffectivePower(req.lightSettings);
			df.llContextKind = req.lightContext.kind == LightLevel::Context::HdrPassthrough ? 1
				: req.lightContext.kind == LightLevel::Context::HdrToSdr ? 2 : 0;
			df.llFullRange = req.lightContext.fullRange;
			df.llSourcePeakNits = req.lightContext.sourcePeakNits;
			df.llSourceMinNits = req.lightContext.sourceMinNits;
			df.llDynamicPeakNits = req.lightContext.dynamicPeakNits;
			df.llSdrTargetNits = req.lightSettings.sdrTargetNits;
			df.llSdrBlackNits = req.lightSettings.sdrBlackNits;
			df.llTransfer = req.lightSettings.transfer == LightLevel::Transfer::Srgb ? 1
				: req.lightSettings.transfer == LightLevel::Transfer::Power ? 2 : 0;
			df.llGammaPower = req.lightSettings.gammaPower;
			df.llToneMapping = req.lightSettings.toneMapping;
			df.llQuality = req.lightSettings.quality;
			df.llSdrToneKnee = req.lightSettings.shape.knee;
			df.llSdrToneHeadroom = req.lightSettings.shape.headroom;
			df.llSdrToneKneeStart = req.lightSettings.shape.kneeStart;
			df.llSdrToneKneeStrength = req.lightSettings.shape.kneeStrength;
			return df;
		}
	};

	OutputFinishSegment::OutputFinishSegment(OutputEncodingRepair::Hook& repairIn, OcclusionMask::Hook& maskIn)
		: repair(repairIn), mask(maskIn),
		segment(SegmentRunner::kS3FallbackId, SegmentRunner::OutputFinishStages(), SegmentRunner::OutputFinishEntry())
	{
	}

	OutputFinishSegment::~OutputFinishSegment()
	{
		ReleaseGpu(nullptr);
	}

	void OutputFinishSegment::ReleaseDitherNeedBind()
	{
		if (ditherNeedGpu)
			ditherNeedGpu->guard.Release();
	}

	void OutputFinishSegment::ReleaseGpu(pl_gpu releaseGpu)
	{
		if (!ditherNeedGpu)
			return;
		DitherNeedGpuState& st = *ditherNeedGpu;
		st.ClearDemand(this);
		st.ReleaseGpuResources(releaseGpu);
		VpSkeleton::VramUsers::Instance().Release(this, VpSkeleton::kUserDitherNeed);
	}

	pl_tex OutputFinishSegment::DitherNeedCurrent() const
	{
		return ditherNeedGpu ? ditherNeedGpu->pass.Current() : nullptr;
	}

	size_t OutputFinishSegment::DitherNeedVramBytes() const
	{
		return ditherNeedGpu ? ditherNeedGpu->TotalVramBytes() : 0;
	}

	std::string OutputFinishSegment::DitherNeedLastError() const
	{
		return ditherNeedGpu ? ditherNeedGpu->pass.LastError() : std::string();
	}

	bool OutputFinishSegment::DitherNeedDemandActive() const
	{
		return ditherNeedGpu && ditherNeedGpu->demandActive;
	}

	DitherNeedSeam::Outcome OutputFinishSegment::EvaluateDitherNeed(bool requested, bool originalPathway,
		const DitherNeedSeam::Feed& feed, const RegionEvidence::Grid& grid, const DitherNeed::Params& base)
	{
		return ditherNeed.Evaluate(requested, originalPathway, feed, grid, base);
	}

	const DitherNeedSeam::Counters& OutputFinishSegment::DitherNeedCounters() const { return ditherNeed.Count(); }

	bool OutputFinishSegment::DitherNeedStatusChanged(const std::string& status)
	{
		if (status == ditherNeedLogged) return false;
		ditherNeedLogged = status;
		return true;
	}

	void OutputFinishSegment::PopulateDitherNeedRequest(pl_gpu gpu, const void* host, uint64_t sourceSequence,
		bool sigmaDeltaActive, bool slotOff, bool domainSupported, int colorBits, bool nlsActive,
		const SourceIdentity::QualityView* qualityView, double sdrTargetNits, double sdrBlackNits, const char* hdrTargetGamma,
		const char* toneMapping, const char* qualitySetting, const ToneMappingTuning::Overrides& toneTuning,
		const SdrToneShape::Settings& sdrTone, const char* sdrInputTransfer, const char* outputGamma,
		bool hasContrastRecovery, double contrastRecovery, const pl_frame& sourceFrame, const pl_frame& targetFrame,
		double captureRateHz, double nominalSourceRateHz, const CustomHookChain::Chain& customHooks, DitherNeedGpuRequest& req)
	{
		req.requested = true;
		req.originalPathway = false;
		req.gpu = gpu;
		req.host = host;
		req.sourceSequence = sourceSequence;
		req.sigmaDeltaActive = sigmaDeltaActive;
		req.slotOff = slotOff;
		req.domainSupported = domainSupported;
		req.colorBits = colorBits;
		req.sourceBits = sourceFrame.repr.bits.color_depth > 0 ? sourceFrame.repr.bits.color_depth : sourceFrame.repr.bits.sample_depth;
		req.nlsActive = nlsActive;
		req.quality = qualityView;
		req.dtSeconds = captureRateHz > 0.0 ? (1.0 / captureRateHz)
			: (nominalSourceRateHz > 0.0 ? (1.0 / nominalSourceRateHz) : 0.0);
		if (req.host != nullptr)
			if (Vpv::Skeleton::IExternalDemand* demand = Vpv::Skeleton::Demand())
			{
				const Vpv::Skeleton::SceneAnswer scene = demand->SceneOf(req.host, req.sourceSequence);
				if (scene.known && scene.cut)
					req.discontinuity = true;
			}
		double qBase = 0.15;
		if (customHooks.QuantizerConfigured("noise_amplitude", qBase))
			req.aBase = qBase;
		req.lightSettings.sdrTargetNits = sdrTargetNits;
		req.lightSettings.sdrBlackNits = sdrBlackNits;
		req.lightSettings.displayWhiteNits = sdrTargetNits;
		req.lightSettings.displayBlackNits = sdrBlackNits;
		if (toneMapping) req.lightSettings.toneMapping = toneMapping;
		if (qualitySetting) req.lightSettings.quality = qualitySetting;
		req.lightSettings.tuning = toneTuning;
		req.lightSettings.shape = sdrTone;
		if (sdrInputTransfer) req.lightSettings.sdrInputTransfer = sdrInputTransfer;
		if (outputGamma) req.lightSettings.outputGamma = outputGamma;
		if (hasContrastRecovery) req.lightSettings.contrastRecovery = contrastRecovery;
		{
			const std::string t = LightLevel::detail::Normalize(hdrTargetGamma ? hdrTargetGamma : "");
			double gp = 2.4;
			if (t == "srgb") req.lightSettings.transfer = LightLevel::Transfer::Srgb;
			else if (LightLevel::detail::ParseNumber(t, gp) && gp >= 1.8 && gp <= 2.8)
			{
				req.lightSettings.transfer = LightLevel::Transfer::Power;
				req.lightSettings.gammaPower = gp;
			}
			else req.lightSettings.transfer = LightLevel::Transfer::Bt1886;
		}
		req.sourceCrop = sourceFrame.crop;
		req.targetCrop = targetFrame.crop;
		req.lightContext.fullRange = pl_color_levels_guess(&sourceFrame.repr) == PL_COLOR_LEVELS_FULL;
		req.lightContext.sourcePeakNits = sourceFrame.color.hdr.max_luma > 0.0f ? static_cast<double>(sourceFrame.color.hdr.max_luma) : 0.0;
		req.lightContext.sourceMinNits = sourceFrame.color.hdr.min_luma > 0.0f ? static_cast<double>(sourceFrame.color.hdr.min_luma) : 0.0;
		req.lightContext.displayPeakNits = targetFrame.color.hdr.max_luma > 0.0f ? static_cast<double>(targetFrame.color.hdr.max_luma) : 0.0;
		req.regionTransfer = sourceFrame.color.transfer == PL_COLOR_TRC_PQ ? 1
			: (sourceFrame.color.transfer == PL_COLOR_TRC_HLG ? 2 : 0);
		const bool srcHdr = pl_color_transfer_is_hdr(sourceFrame.color.transfer);
		const bool dstHdr = pl_color_transfer_is_hdr(targetFrame.color.transfer);
		req.lightContext.kind = !srcHdr ? LightLevel::Context::Sdr
			: (dstHdr ? LightLevel::Context::HdrPassthrough : LightLevel::Context::HdrToSdr);
	}

	std::string OutputFinishSegment::FormatDitherNeedLog(const std::string& status) const
	{
		const DitherNeedSeam::Counters& n = ditherNeed.Count();
		const std::string err = DitherNeedLastError();
		char buf[384];
		std::snprintf(buf, sizeof(buf),
			"%s%s (frames so far: on %llu, not needed %llu, unavailable %llu, stale %llu, nls declined %llu, classical quality %llu)",
			status.c_str(), err.empty() ? "" : (" [" + err + "]").c_str(),
			static_cast<unsigned long long>(n.framesOn), static_cast<unsigned long long>(n.framesNotNeeded),
			static_cast<unsigned long long>(n.framesUnavailable), static_cast<unsigned long long>(n.framesStale),
			static_cast<unsigned long long>(n.framesNlsDeclined), static_cast<unsigned long long>(n.framesClassicalQuality));
		return std::string(buf);
	}

	DitherNeedSeam::Outcome OutputFinishSegment::RunDitherNeedGpu(const DitherNeedGpuRequest& req,
		const pl_hook* const* hooks, int numHooks,
		const std::function<void(std::function<void()>)>& attachRelease)
	{
		// Off or Original pathway: immediate no-op (zero GPU calls, zero demand, zero counters, zero retained VRAM).
		if (!req.requested || req.originalPathway)
		{
			if (ditherNeedGpu)
			{
				ditherNeedGpu->ReleaseGpuResources(req.gpu);
				ditherNeedGpu->ClearDemand(this);
				ditherNeedGpu->SyncVram(this);
			}
			return ditherNeed.Evaluate(false, req.originalPathway, DitherNeedSeam::Feed(), RegionEvidence::Grid());
		}

		if (!ditherNeedGpu)
			ditherNeedGpu = std::make_unique<DitherNeedGpuState>();
		DitherNeedGpuState& st = *ditherNeedGpu;

		// auto asked for, but sigma-delta is not running by configuration: release GPU resources + visible inactive fallback.
		if (!req.sigmaDeltaActive)
		{
			st.ReleaseGpuResources(req.gpu);
			st.ClearDemand(this);
			st.SyncVram(this);
			ditherNeed.Evaluate(false, false, DitherNeedSeam::Feed(), RegionEvidence::Grid());
			DitherNeedSeam::Outcome inactive;
			inactive.active = true;
			const std::string why = req.slotOff ? "output.dither is switched off in pipeline_slots_off"
				: "the sigma-delta dither is not selected (temporal_dither)";
			inactive.fallback = FallbackCatalog::DitherNeedInactive(true, why);
			inactive.status = "dither need: inactive (" + why + ")";
			return inactive;
		}

		// GEM-224 + W23-NEON-H3: non-linear stretch invalidates affine VPDN placement — release GPU VRAM, visibly decline and count.
		if (req.nlsActive)
		{
			st.ReleaseGpuResources(req.gpu);
			st.ClearDemand(this);
			st.SyncVram(this);
			DitherNeedSeam::Feed nlsFeed;
			nlsFeed.colorBits = req.colorBits;
			nlsFeed.sourceBits = req.sourceBits;
			nlsFeed.domainSupported = req.domainSupported;
			nlsFeed.dtSeconds = req.dtSeconds;
			nlsFeed.discontinuity = req.discontinuity;
			nlsFeed.quality = req.quality;
			nlsFeed.nlsActive = true;
			nlsFeed.gpuGrainValid = true;
			nlsFeed.gpuEvidenceValid = true;
			const LightLevel::Lut& lut = st.lutCache.Get(req.lightSettings, req.lightContext);
			nlsFeed.lut = &lut;
			DitherNeed::Params baseParams;
			baseParams.aBase = req.aBase;
			return ditherNeed.Evaluate(true, false, nlsFeed, RegionEvidence::Grid(), baseParams);
		}

		// Consumer-driven GPU raising demand for P10/P11/P12 (unless test overrides GPU inputs directly or domain is unsupported).
		if (!req.overrideInputs && req.host != nullptr)
		{
			if (req.domainSupported)
			{
				st.EnsureDemand(req.host, this);
				if (Vpv::Skeleton::IExternalDemand* demand = Vpv::Skeleton::Demand())
					demand->SyncDitherNeedDisplayFacts(req.host, DitherNeedGpuState::BuildDisplayFacts(req));
			}
			else
				st.ClearDemand(this);
		}

		// If pl_gpu changed without a prior ReleaseGpu, abandon old GPU handles safely before adopting the new device.
		if (st.gpu != nullptr && req.gpu != st.gpu)
		{
			st.guard.Release();
			st.ResetSteppedSequence();
			st.pass.Release(nullptr);
			st.lutTex = nullptr;
			st.lutOutTex = nullptr;
			st.dispatch = nullptr;
			st.lutCache.Reset();
			st.gpu = nullptr;
			VpSkeleton::VramUsers::Instance().Release(this, VpSkeleton::kUserDitherNeed);
		}

		// W23-REG-M2: branch on !req.domainSupported BEFORE st.lutCache.Get() so ReleaseGpuResources()
		// cannot invalidate a bound Lut reference or waste CPU rebuilding the LUT only to destroy it.
		const LightLevel::Lut* lutPtr = nullptr;
		if (!req.domainSupported)
		{
			st.ReleaseGpuResources(req.gpu);
			st.SyncVram(this);
		}
		else
		{
			lutPtr = &st.lutCache.Get(req.lightSettings, req.lightContext);
			if (req.gpu != nullptr)
			{
				st.gpu = req.gpu;
				if (!st.dispatch)
				{
					st.dispatch = pl_dispatch_create(req.gpu->log, req.gpu);
					GpuAttribution::Watch(st.dispatch, "dither_need_gpu");
				}
				if (st.lutCache.LastRebuilt() || !st.lutTex || !st.lutOutTex)
				{
					DitherNeedGpuState::UploadLut1024(req.gpu, st.lutTex, LightLevel::PackTexels(*lutPtr));
					DitherNeedGpuState::UploadLut1024(req.gpu, st.lutOutTex, LightLevel::PackOutputTexels(*lutPtr));
				}
			}
		}

		// Secondary motionDraw / cadenceRepeat draws of the same sourceSequence reuse the already-stepped
		// dither-need map without re-stepping temporal EMA or invalidating state.
		{
			const bool gridMatchesOverride = !req.overrideInputs ||
				(req.overrideGrid.gw == st.lastSteppedGridW && req.overrideGrid.gh == st.lastSteppedGridH &&
					(req.overrideGrid.B > 0 ? req.overrideGrid.B : 16) == st.lastSteppedBlockPx);
			if (req.domainSupported && lutPtr != nullptr && req.sourceSequence > 0 && st.hasSteppedSequence &&
				st.lastSteppedSourceSequence == req.sourceSequence &&
				!req.discontinuity &&
				st.lastSteppedLutFingerprint == lutPtr->fingerprint &&
				st.lastSteppedColorBits == req.colorBits &&
				st.lastSteppedSourceBits == req.sourceBits &&
				st.lastSteppedABase == req.aBase &&
				gridMatchesOverride &&
				st.pass.Current() != nullptr)
			{
				st.guard.Release();
				DitherNeed::Gpu::Placement reusePlacement;
				const bool reusePlacementOk = DitherNeed::Gpu::PlacementFromCrops(
					req.sourceCrop.x0, req.sourceCrop.y0, req.sourceCrop.x1, req.sourceCrop.y1,
					req.targetCrop.x0, req.targetCrop.y0, req.targetCrop.x1, req.targetCrop.y1,
					st.lastSteppedBlockPx > 0 ? st.lastSteppedBlockPx : 16, reusePlacement);
				DitherNeedSeam::Outcome reuseOutcome = st.lastSteppedOutcome;
				if (!reusePlacementOk)
				{
					st.pass.Invalidate();
					st.ResetSteppedSequence();
					st.SyncVram(this);
					ditherNeed.RecordGpuFailure(reuseOutcome, DitherNeed::kPassFailed, false);
					return reuseOutcome;
				}
				if (req.requireBind)
				{
					if (!st.guard.Apply(hooks, numHooks, st.pass.Current(), reusePlacement))
					{
						st.pass.Invalidate();
						st.ResetSteppedSequence();
						st.SyncVram(this);
						ditherNeed.RecordGpuFailure(reuseOutcome, DitherNeed::Gpu::HookGuard::MissingAfterApply(), false);
						return reuseOutcome;
					}
					if (attachRelease)
						attachRelease([this]() { ReleaseDitherNeedBind(); });
				}
				return reuseOutcome;
			}
		}

		// Acquire shared GPU products P10 (grain blocks) and P11 (region precursors) — zero private recomputation.
		DitherNeed::Gpu::Args args;
		args.lut = st.lutTex;
		args.lutOut = st.lutOutTex;
		args.derive = RegionEvidence::Uniforms(RegionEvidence::DeriveParams());
		RegionEvidence::Grid grid;
		double grainAge = 0.0;
		double evidenceAge = 0.0;
		if (req.overrideInputs)
		{
			args.gsBlocks = req.overrideGsBlocks;
			for (size_t i = 0; i < RegionEvidence::kPrecursorTextures; ++i)
				args.precursors[i] = req.overridePrecursors[i];
			grid = req.overrideGrid;
			grainAge = req.overrideGrainAgeSeconds;
			evidenceAge = req.overrideEvidenceAgeSeconds;
		}
		else if (req.host != nullptr)
		{
			if (Vpv::Skeleton::IExternalDemand* demand = Vpv::Skeleton::Demand())
			{
				const Vpv::Skeleton::DitherNeedGpuInputs gi = demand->ReadDitherNeedGpuInputs(req.host, req.sourceSequence);
				args.gsBlocks = static_cast<pl_tex>(const_cast<void*>(gi.gsBlocks));
				for (size_t i = 0; i < RegionEvidence::kPrecursorTextures && i < Vpv::Skeleton::DitherNeedGpuInputs::kPrecursors; ++i)
					args.precursors[i] = static_cast<pl_tex>(const_cast<void*>(gi.precursors[i]));
				if (gi.srcW > 0 && gi.srcH > 0 && gi.blockPx > 0)
					grid = RegionEvidence::MakeGrid(gi.srcW, gi.srcH, gi.blockPx);
				else
				{
					grid.gw = gi.gridW;
					grid.gh = gi.gridH;
					grid.B = gi.blockPx > 0 ? gi.blockPx : 16;
				}
				if (gi.haveGrain && req.sourceSequence > gi.grainSeq && req.dtSeconds > 0.0)
					grainAge = static_cast<double>(req.sourceSequence - gi.grainSeq) * req.dtSeconds;
				if (gi.haveEvidence && req.sourceSequence > gi.evidenceSeq && req.dtSeconds > 0.0)
					evidenceAge = static_cast<double>(req.sourceSequence - gi.evidenceSeq) * req.dtSeconds;
			}
		}
		args.gridW = grid.gw;
		args.gridH = grid.gh;
		args.blockPx = grid.B > 0 ? grid.B : 16;

		bool allPrecursors = true;
		for (pl_tex t : args.precursors)
			if (!t) allPrecursors = false;

		DitherNeed::Gpu::Placement placement;
		const bool placementOk = DitherNeed::Gpu::PlacementFromCrops(
			req.sourceCrop.x0, req.sourceCrop.y0, req.sourceCrop.x1, req.sourceCrop.y1,
			req.targetCrop.x0, req.targetCrop.y0, req.targetCrop.x1, req.targetCrop.y1,
			args.blockPx, placement);

		unsigned gpuMissing = DitherNeed::Gpu::BlockPass::Validate(args);
		if (!placementOk)
			gpuMissing |= DitherNeed::kPassFailed;
		if (req.requireBind && !DitherNeed::Gpu::Supported())
			gpuMissing |= DitherNeed::kNoSetTexture;

		DitherNeedSeam::Feed feed;
		feed.lut = (lutPtr != nullptr && st.lutTex && st.lutOutTex) ? lutPtr : nullptr;
		feed.quality = req.quality;
		feed.grainAgeSeconds = grainAge;
		feed.evidenceAgeSeconds = evidenceAge;
		feed.lutAgeSeconds = 0.0;
		feed.colorBits = req.colorBits;
		feed.sourceBits = req.sourceBits;
		feed.domainSupported = req.domainSupported;
		feed.dtSeconds = req.dtSeconds;
		feed.discontinuity = req.discontinuity;
		feed.gpuGrainValid = args.gsBlocks != nullptr;
		feed.gpuEvidenceValid = allPrecursors;
		feed.extraMissing = gpuMissing;

		DitherNeed::Params baseParams;
		baseParams.aBase = req.aBase;
		DitherNeedSeam::Outcome outcome = ditherNeed.Evaluate(true, false, feed, grid, baseParams);
		if (!outcome.extraAllowed)
		{
			st.guard.Release();
			st.pass.Invalidate();
			st.ResetSteppedSequence();
			st.SyncVram(this);
			return outcome;
		}

		// Execute GPU block pass and bind VPDN across the frame's draw via HookGuard.
		st.guard.Release();
		DitherNeed::FrameArgs fa;
		fa.params = outcome.params;
		fa.grid = grid;
		fa.dtSeconds = req.dtSeconds;
		fa.discontinuity = req.discontinuity || outcome.decision.reset;
		fa.enabled = true;
		fa.missing = 0;
		fa.needed = true;
		fa.colorBits = req.colorBits;
		fa.lutFingerprint = lutPtr != nullptr ? lutPtr->fingerprint : std::string();

		const DitherNeed::Gpu::BlockPass::StepResult step = st.pass.Step(req.gpu, st.dispatch, args, fa, req.requireBind);
		st.SyncVram(this);
		if (!step.ran || st.pass.Current() == nullptr)
		{
			st.ResetSteppedSequence();
			ditherNeed.RecordGpuFailure(outcome, step.missing ? step.missing : DitherNeed::kPassFailed);
			return outcome;
		}

		if (req.requireBind)
		{
			if (!st.guard.Apply(hooks, numHooks, st.pass.Current(), placement))
			{
				st.pass.Invalidate();
				st.ResetSteppedSequence();
				st.SyncVram(this);
				ditherNeed.RecordGpuFailure(outcome, DitherNeed::Gpu::HookGuard::MissingAfterApply());
				return outcome;
			}
			if (attachRelease)
				attachRelease([this]() { ReleaseDitherNeedBind(); });
		}
		if (req.sourceSequence > 0 && lutPtr != nullptr)
		{
			st.hasSteppedSequence = true;
			st.lastSteppedSourceSequence = req.sourceSequence;
			st.lastSteppedLutFingerprint = lutPtr->fingerprint;
			st.lastSteppedGridW = args.gridW;
			st.lastSteppedGridH = args.gridH;
			st.lastSteppedBlockPx = args.blockPx;
			st.lastSteppedColorBits = req.colorBits;
			st.lastSteppedSourceBits = req.sourceBits;
			st.lastSteppedABase = req.aBase;
			st.lastSteppedOutcome = outcome;
		}
		else
			st.ResetSteppedSequence();
		return outcome;
	}

	void OutputFinishSegment::Note(const pl_hook* hook)
	{
		if (!Member(hook))
			members.push_back(hook);
	}

	DitherPrep OutputFinishSegment::PrepareDither(std::vector<const pl_hook*>& hooks, const SigmaDeltaDomain::Domain& domain,
		bool outputFloat32, const PipelineSlots::Registry& registry)
	{
		// output.dither off (pipeline_slots_off): the sigma-delta is not submitted, as for a code
		// domain it does not support; libplacebo's own dither then runs as configured. Only the
		// domain case is reported as a fallback (DitherPrep).
		const bool ditherRuns = registry.Runs(Id::OutputDither);
		DitherPrep prep;
		prep.slotOff = !ditherRuns;
		for (size_t i = 0; i < hooks.size();)
		{
			const pl_hook* hook = hooks[i];
			const bool sigma = SetHookParameter(hook, "levels_full", domain.full ? 1.0 : 0.0);
			prep.found = prep.found || sigma;
			if (sigma && (!domain.supported || !ditherRuns))
			{
				if (ditherRuns)
					prep.domainDropped = true;
				hooks.erase(hooks.begin() + static_cast<std::ptrdiff_t>(i));
				continue;
			}
			if (sigma)
				Note(hook);
			SetHookParameter(hook, "swapchain_bits", static_cast<double>(domain.sample));
			SetHookParameter(hook, "color_bits", static_cast<double>(domain.color));
			SetHookParameter(hook, "output_float32", outputFloat32 ? 1.0 : 0.0);
			++i;
		}
		return prep;
	}

	std::string OutputFinishSegment::DescribeDitherDomainChange(const SigmaDeltaDomain::Domain& domain, bool active, int& lastSignature)
	{
		const int signature = (domain.full ? 1 : 0) | (domain.sample << 1) | (domain.color << 8);
		if (!active || signature == lastSignature)
			return std::string();
		lastSignature = signature;
		char buf[256];
		if (domain.supported)
			std::snprintf(buf, sizeof(buf), "Sigma-delta code domain from the target: %s range, %d-bit swapchain, %d-bit codes",
				domain.full ? "full" : "limited", domain.sample, domain.color);
		else
			std::snprintf(buf, sizeof(buf),
				"Sigma-delta: off for this output (%s range, %d-bit swapchain, colour depth %d: not an 8- or 10-bit code domain)",
				domain.full ? "full" : "limited", domain.sample, domain.color);
		return std::string(buf);
	}

	FallbackCatalog::Decision OutputFinishSegment::DitherFallback(const DitherPrep& prep, const SigmaDeltaDomain::Domain& domain,
		const std::string& quantiserProblem)
	{
		return FallbackCatalog::Dither(DitherFallbackRequested(prep, !quantiserProblem.empty()),
			prep.domainDropped ? FallbackCatalog::DitherDrop::UnsupportedDomain :
			quantiserProblem == CustomHookChain::Chain::kQuantizerNotLoaded ? FallbackCatalog::DitherDrop::ShaderNotLoaded :
			!quantiserProblem.empty() ? FallbackCatalog::DitherDrop::DisabledByLibplacebo : FallbackCatalog::DitherDrop::None,
			prep.domainDropped ? std::string(domain.full ? "full" : "limited") + " range, " +
				std::to_string(domain.sample) + "-bit swapchain, colour depth " + std::to_string(domain.color) : std::string());
	}

	bool OutputFinishSegment::KeepDitherLast(std::vector<const pl_hook*>& hooks, bool levelsFull)
	{
		std::vector<const pl_hook*> sigmaHooks;
		bool otherOutputAfterSigma = false;
		for (const pl_hook* hook : hooks)
		{
			const bool sigma = SetHookParameter(hook, "levels_full", levelsFull ? 1.0 : 0.0);
			if (sigma)
				sigmaHooks.push_back(hook);
			else if (!sigmaHooks.empty() && hook && (hook->stages & PL_HOOK_OUTPUT))
				otherOutputAfterSigma = true;
		}
		if (!otherOutputAfterSigma)
			return false;
		for (const pl_hook* hook : sigmaHooks)
			hooks.erase(std::find(hooks.begin(), hooks.end(), hook));
		hooks.insert(hooks.end(), sigmaHooks.begin(), sigmaHooks.end());
		return true;
	}

	size_t OutputFinishSegment::MarkMask(std::vector<const pl_hook*>& hooks, bool install, bool grainInstall, double grainScale)
	{
		size_t sigmaIndex = hooks.size();
		for (size_t i = 0; i < hooks.size(); ++i)
			if (SetHookParameter(hooks[i], "occlusion_mask", install ? 1.0 : 0.0) && sigmaIndex == hooks.size())
				sigmaIndex = i;
		for (size_t i = 0; i < hooks.size(); ++i)
			SetHookParameter(hooks[i], "grain_tolerance_scale", grainInstall && sigmaIndex < hooks.size() ? grainScale : 1.0);
		return sigmaIndex;
	}

	void OutputFinishSegment::InsertMask(std::vector<const pl_hook*>& hooks, size_t ditherIndex)
	{
		hooks.insert(hooks.begin() + static_cast<std::ptrdiff_t>(ditherIndex), mask.Get());
		Note(mask.Get());
	}

	bool OutputFinishSegment::Repair(std::vector<const pl_hook*>& hooks, const pl_render_errors* errors,
		enum pl_lut_type targetLut, const pl_color_repr& targetRepr)
	{
		if (!OutputEncodingRepair::Needed(hooks, errors, targetLut))
			return false;
		repair.SetTarget(targetRepr);
		hooks.insert(hooks.begin(), repair.Get());
		Note(repair.Get());
		return true;
	}

	bool OutputFinishSegment::NoteDisabledHooks(const uint64_t* signatures, int count)
	{
		if (maskDisabled || !signatures || count <= 0)
			return false;
		const uint64_t maskSignature = mask.Get()->signature;
		for (int i = 0; i < count; ++i)
			if (signatures[i] == maskSignature)
			{
				maskDisabled = true;
				return true;
			}
		return false;
	}

	void OutputFinishSegment::BeginFrameDraws(std::function<void()> releaseVectorsIn)
	{
		releaseVectors = std::move(releaseVectorsIn);
		draws = 0;
	}

	bool OutputFinishSegment::OnDraw(pl_render_params& parameters, bool& maskParamSet)
	{
		if (!releaseVectors)
			return false;
		if (++draws != 2)
			return false;
		releaseVectors();
		for (int i = 0; i < parameters.num_hooks; ++i)
			SetHookParameter(parameters.hooks[i], "occlusion_mask", 0.0);
		maskParamSet = false;
		++nlsRetryReleases;
		return true;
	}

	void OutputFinishSegment::EndFrameDraws()
	{
		releaseVectors = nullptr;
		draws = 0;
	}

	const char* OutputFinishSegment::StageName(const pl_hook* hook) const
	{
		return hook == repair.Get() ? "encoding_repair" : hook == mask.Get() ? "occlusion_mask" : "sigma_delta";
	}

	bool OutputFinishSegment::Member(const pl_hook* hook) const
	{
		return std::find(members.begin(), members.end(), hook) != members.end();
	}

	PipelineSlotsPlan BuildPipelineSlotsPlan(const ::VpvRenderer::Fields& settings)
	{
		PipelineSlotsPlan out;
		if (settings.pipelineOriginalPathway)
			out.next.ApplyOriginalPathway();
		std::string problems = settings.pipelineSettingsProblems;
		std::string token;
		auto flush = [&]()
		{
			if (token.empty())
				return;
			const PipelineSlots::Id id = PipelineSlots::FromName(token.c_str());
			if (id == PipelineSlots::Id::Count)
				problems += "pipeline_slots_off: unknown slot '" + token + "'; ";
			else if (!PipelineSlots::SwitchHonoured(id))
				problems += "pipeline_slots_off: " + token + " has no slot switch yet; ";
			else
				out.next.SetEnabled(id, false);
			token.clear();
		};
		for (char ch : settings.pipelineSlotsOff)
		{
			if (ch == ',' || ch == ' ' || ch == ';' || ch == '\t')
				flush();
			else
				token += ch;
		}
		flush();
		if (!problems.empty())
		{
			problems.resize(problems.size() - 2);
			out.fallbacks.push_back(FallbackCatalog::Engaged("pipeline settings", "ignored: " + problems));
		}
		else
			out.fallbacks.push_back(FallbackCatalog::Released("pipeline settings"));
		std::string reason;
		if (!settings.pipelineOriginalPathway && !out.next.SetOrderByNames(settings.pipelineOrder, &reason))
			out.fallbacks.push_back(FallbackCatalog::Engaged("pipeline order", "designed order kept: " + reason));
		else
			out.fallbacks.push_back(FallbackCatalog::Released("pipeline order"));
		if (settings.presentEveryVsyncWithheld)
			out.fallbacks.push_back(FallbackCatalog::Engaged(PresentationOwner::FallbackId(), PresentationOwner::WithheldText()));
		else
			out.fallbacks.push_back(FallbackCatalog::Released(PresentationOwner::FallbackId()));
		out.fallbacks.push_back(FallbackCatalog::AmbientAdaptReplaced(settings.ambientAdaptReplaced));
		out.fallbacks.push_back(FallbackCatalog::SaturationReplaced(settings.saturationReplaced, settings.saturationConfigured));
		out.next.SetEntranceOnce(settings.pipelineEntranceOnce && !settings.pipelineOriginalPathway);
		(void)out.next.ActiveEntrance();
		out.logState = settings.pipelineOriginalPathway ? "Original pathway (every VP addition off)" :
			out.next.IsDefaultOrder() ? "designed order" : "custom order";
		std::string entrance;
		for (PipelineSlots::Id id : out.next.ActiveEntrance())
			entrance += (entrance.empty() ? "" : " > ") + std::string(PipelineSlots::Get(id).name);
		out.logState += "; entrance: " + (entrance.empty() ? std::string("none") : entrance);
		const std::string slotSwitches = SlotSwitchSettings::Describe(SlotSwitchSettings::FromText(settings.pipelineSlotsOff),
			settings.pipelineOriginalPathway);
		if (!slotSwitches.empty())
			out.logState += "; " + slotSwitches;
		out.logState += settings.pipelineEntranceOnce && !settings.pipelineOriginalPathway ?
			"; entrance once (B3) on: motion pictures from the processed A and B" : "; entrance once (B3) off";
		EntranceGpuDrain::Mode drainMode = EntranceGpuDrain::Mode::Off;
		EntranceGpuDrain::Parse(settings.entranceGpuDrain, drainMode);
		if (drainMode != EntranceGpuDrain::Mode::Off && !settings.pipelineOriginalPathway &&
			out.next.Runs(PipelineSlots::Id::ScalingRebase))
		{
			out.logState += std::string("; entrance GPU drain: ") + EntranceGpuDrain::Name(drainMode);
			out.fallbacks.push_back(FallbackCatalog::Engaged("entrance GPU drain", EntranceGpuDrain::StatusReason(drainMode)));
		}
		else
			out.fallbacks.push_back(FallbackCatalog::Released("entrance GPU drain"));
		return out;
	}

	void OrderSlotHooksInPlace(std::vector<const pl_hook*>& frameHooks,
		const std::vector<std::pair<const pl_hook*, PipelineSlots::Id>>& frameSlotHooks,
		const PipelineSlots::Registry& pipelineSlots)
	{
		if (frameSlotHooks.size() < 2)
			return;
		std::vector<OutputHookOrder::Tagged<const pl_hook*>> tagged;
		tagged.reserve(frameHooks.size());
		for (const pl_hook* hook : frameHooks)
		{
			OutputHookOrder::Tagged<const pl_hook*> t;
			t.hook = hook;
			for (const auto& entry : frameSlotHooks)
				if (entry.first == hook) { t.kind = OutputHookOrder::Kind::Slot; t.slot = entry.second; break; }
			tagged.push_back(t);
		}
		frameHooks = OutputHookOrder::OrderSlotsInPlace(tagged,
			[&pipelineSlots](PipelineSlots::Id id) { return pipelineSlots.Order(id); });
	}

	void TraceFrameHookListIfEnabled(const void* renderer, const pl_render_params& frameParams,
		const std::vector<std::pair<const pl_hook*, PipelineSlots::Id>>& frameSlotHooks,
		const CustomHookChain::Chain& customHooks, const pl_hook* submittedNls, bool (*readEnv)())
	{
		if (!Vpv::Segments::CoreSegmentCaches::Instance().TraceHookListEnabled(renderer, readEnv))
			return;
		std::string list;
		for (int i = 0; i < frameParams.num_hooks; ++i)
		{
			const pl_hook* hook = frameParams.hooks[i];
			std::string tag = "other";
			for (const auto& entry : frameSlotHooks)
				if (entry.first == hook) { tag = PipelineSlots::Get(entry.second).name; break; }
			if (tag == "other")
			{
				if (hook != nullptr && hook == submittedNls) tag = "nls";
				else if (customHooks.IsQuantizerHook(hook)) tag = "quantiser";
				else if (customHooks.IsPrescalerHook(hook)) tag = "prescaler";
			}
			list += (i ? ", " : "") + tag;
		}
		if (!Vpv::Segments::CoreSegmentCaches::Instance().ShouldLogHookList(renderer, list))
			return;
		DebugLog::Log("VP hook list: n=%d [%s]", frameParams.num_hooks, list.c_str());
	}
}
