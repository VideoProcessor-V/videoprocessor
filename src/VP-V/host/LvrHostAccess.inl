// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
//
// CC slice 12 step 12.2 / series A (refs/112 P2: ONE host include in place of the per-module .inl lines): the out-of-line
// bodies and forwarding-only Vpv::HostAccess adapters of LibplaceboVideoRenderer::Impl, right after the Impl struct.
// Included exactly once, by one `// VPV-HOOK` line; no other translation unit may include it.
#include <VP-V/modules/motion/estimation/VectorEngineEstimate.inl>   // P5a-5: the estimate and the engine keeper (Impl members)
#include <VP-V/modules/motion/consumers/DejudderPolicyRunner.inl>   // M2 P5b-3: the de-judder policy (Impl members)
#include <VP-V/modules/motion/consumers/PhaseTimeline.inl>   // M2 P5b-4: phase picture, timeline helpers, status, release (Impl members)
#include <VP-V/modules/input/capture/CaptureImpl.inl>   // P10: the capture code (Impl members)
#include <VP-V/segments/CoreSegments.inl>   // P4-2: UpdatePipelineSlots, OrderFrameSlotHooks (Impl members)
#include <VP-V/segments/EntranceHostMembers.inl>   // P4-5: the entrance host B3 helpers, RenderCrossfadeMix, DrainEntranceBeforeRender (Impl members)
#include <VP-V/segments/EntrancePlanner.inl>   // P4 e: the entrance planner (Impl member)
#include <VP-V/modules/motion/consumers/IntentMotionMethods.h>   // P11a-f: compiled in IntentMotionMethods.cpp
#include <VP-V/modules/drive/led_compensation/LedIntentMethods.h>   // P11a-f: compiled in LedIntentMethods.cpp
#include <VP-V/modules/drive/led_compensation/LedSdrLiftMethods.h>   // P11a-d: compiled in LedSdrLiftMethods.cpp
#include <VP-V/modules/output/calibration_lut/CalibrationPresentMethods.inl>   // P11a-e: the calibration pattern present (Impl member)
#include <VP-V/modules/output/presentation/PresentationHostHelpers.h>   // Package C 4: compiled in PresentationHostHelpers.cpp

void LibplaceboVideoRenderer::Impl::ConfigureIntentInterpreter() { ConfigureIntentMotion(); ConfigureIntentAmbient(); }
void LibplaceboVideoRenderer::Impl::ConfigureIntentAmbient() { Vpv::LedIntent::ConfigureAmbient(vpv.led.intent, activeSettings); }
double LibplaceboVideoRenderer::Impl::IntentTargetBlackNits(uint64_t sourceSequence) { return Vpv::LedIntent::TargetBlackNits(vpv.led.intent, vpv.motion.intent, sdrBlackNits, sourceSequence); }
void LibplaceboVideoRenderer::Impl::EvaluateIntentBlackDisplay(const SceneDescriptor& scene, const DisplayProfile& display) { Vpv::LedIntent::EvaluateBlackDisplay(vpv.led.intent, vpv.motion.intent, activeSettings, sdrBlackNits, dynamicDrive, scene, display); }

double LibplaceboVideoRenderer::Impl::RenderAheadNow() { return Vpv::PresentationHost::RenderAheadNow(); }
void LibplaceboVideoRenderer::Impl::RenderAheadBreak() { Vpv::PresentationHost::RenderAheadBreak(renderAheadPresenter); }
void LibplaceboVideoRenderer::Impl::ResetRenderAheadGate(const char* why) { Vpv::PresentationHost::ResetRenderAheadGate(renderAheadPresenter, why); }
double LibplaceboVideoRenderer::Impl::OpenLoopGridVsync(double displayHz, double now, double qpcFrequency) { return Vpv::PresentationHost::OpenLoopGridVsync(displayHz, now, qpcFrequency, &DwmCompositionTiming); }
void LibplaceboVideoRenderer::Impl::ResolveRenderAheadTimers() { Vpv::PresentationHost::ResolveRenderAheadTimers(renderAheadPresenter, renderAheadTimersReady, renderTimingContext.p, renderAheadExtraTimer, renderAheadEstimateTimer, dejudderExtras.dejudderExtraStats.ditherLastExternal); }
int LibplaceboVideoRenderer::Impl::UpdateRenderAheadGate(double now, double sourcePeriod, double displayPeriod, bool late, int maxExtras) { return Vpv::PresentationHost::UpdateRenderAheadGate(renderAheadPresenter, now, sourcePeriod, displayPeriod, late, maxExtras, activeSettings.renderAheadUtilisation, vpOwnedSwapchain != nullptr, activeSettings.ditherVectorsOwn && DitherVectorsSupported(), dejudderExtras.dejudderExtraStats.ditherMsOwn, dejudderExtras.dejudderExtraStats.ditherMsExternal); }
UINT LibplaceboVideoRenderer::Impl::DejudderExtrasBetween(UINT from, UINT to) const { return Vpv::PresentationHost::DejudderExtrasBetween(dejudderExtras, from, to); }
void LibplaceboVideoRenderer::Impl::AdjustSampleForDejudderExtras(AlphaDxgiPresentationSample& sample, IDXGISwapChain* native) const { Vpv::PresentationHost::AdjustSampleForDejudderExtras(dejudderExtras, sample, native); }
double LibplaceboVideoRenderer::Impl::RenderAheadChainRatio() const { return Vpv::PresentationHost::RenderAheadChainRatio(dejudderSourceHz, renderLoadDisplayRefreshHz.load(std::memory_order_acquire)); }
uint64_t LibplaceboVideoRenderer::Impl::DisplayLutSignature() const { return Vpv::PresentationHost::DisplayLutSignature(calibrationHost.Reload().ObservedBytes(), calibrationHost.Reload().ObservedWriteTime(), displayLutParsed, displayLut != nullptr); }

OpenLoopDisplayRate::Choice LibplaceboVideoRenderer::Impl::SelectDejudderDisplayRate()
{
	Vpv::PresentationHost::DisplayRateHost h{ RenderAheadActive(), activeSettings.queueTimingFixes, dejudderExtras.dejudderHeldNoDisplayRate,
		videoHwnd, swapchain, presentationTelemetry, renderLoadDisplayRefreshHz, openLoopCandidatesTick, openLoopCadenceTick,
		openLoopDisplayConfigHz, openLoopModeIntegerHz, openLoopCadence, openLoopGridRate, openLoopGridRateLogged, dejudderDisplayRateLogged,
		&MonitorDisplayConfigHz, &MonitorRefreshHz, &DwmCompositionTiming, [this](const FallbackCatalog::Decision& d) { SyncFallback(d); } };
	return Vpv::PresentationHost::SelectDejudderDisplayRate(h);
}

void LibplaceboVideoRenderer::Impl::UpdateRenderAheadBudget(double captureRateHz)
{
	Vpv::PresentationHost::BudgetHost h{ activeSettings.renderAhead, activeSettings.renderAheadQueueCredit, renderAheadBudgetVeto, renderAheadBudgetCredit,
		renderLoadDisplayRefreshHz, earlyUploadQueueTarget, presentationTelemetry, renderAheadPresenter,
		[this](const char* id, const std::string& reason) { EngageFallback(id, reason); }, [this](const char* id) { ReleaseFallback(id); } };
	Vpv::PresentationHost::UpdateRenderAheadBudget(h, captureRateHz);
}

void LibplaceboVideoRenderer::Impl::CheckRenderAheadChainDepthLocked()
{
	Vpv::PresentationHost::ChainDepthHost h{ activeSettings.renderAhead, RenderAheadActive(), vpOwnedSwapchain != nullptr, swapchainBlit, swapchain != nullptr,
		lowLatencyQueueActive.load(std::memory_order_acquire), RenderAheadChainRatio(), renderAheadChainWantedSince, renderAheadPresenter,
		[this]() { return RecreateSwapchain(swapchainBlit, suppressLimitedNegotiation, "render-ahead chain depth"); },
		[this]() { EnsureAutoLimitedFallback("render-ahead chain depth"); } };
	Vpv::PresentationHost::CheckRenderAheadChainDepthLocked(h);
}

VpRepeatReuse::VpContext LibplaceboVideoRenderer::Impl::RepeatReuseContext(const pl_frame& image, const pl_frame& target)
{
	Vpv::PresentationHost::RepeatReuseHost h{ interpolating || VpPhaseDrawn(image), dejudderReason == DejudderPolicy::Reason::Active && dejudderExtrasAllowed,
		rebaseFade < 1.0, startupHoldBlack, temporalRenderer.Active(), smoothMotionEngine || temporalRenderer.Mixing(),
		frameHooks, dynamicDrive.MeasurementHookPointer(), &dejudderExtras.dejudderCache, d3d11 ? d3d11->gpu : nullptr,
		outputEncodingRepair.Get(), calibrationHost.Stage().Get(), extendedRangeHooks, adaptiveDownscale,
		rebaseFactor, rebaseFactorX, sourceRebase.Kernel(), DisplayLutSignature(), fallbacks.Heading(), pipelineSlots };
	return Vpv::PresentationHost::BuildRepeatReuseContext(h, image, target);
}

void LibplaceboVideoRenderer::Impl::ConfigureIntentMotion()
{
	Vpv::IntentMotion::Configure(vpv.motion.intent, activeSettings, contentIntent.get());
}

double LibplaceboVideoRenderer::Impl::EaseBlackShare(double measured, bool haveMeasurement)
{
	return Vpv::IntentMotion::EaseBlackShare(vpv.motion.intent, activeSettings.intentEaseSeconds, measured, haveMeasurement);
}

double LibplaceboVideoRenderer::Impl::IntentFrameBlackNits(uint64_t sourceSequence, bool inputIsSdr, const pl_frame& image)
{
	const double renderBlackNits = IntentTargetBlackNits(sourceSequence);
	UpdateAmbientSdrLift(renderBlackNits, inputIsSdr, image);
	return renderBlackNits;
}

void LibplaceboVideoRenderer::Impl::UpdateAmbientSdrLift(double renderBlackNits, bool inputIsSdr, const pl_frame& image)
{
	Vpv::LedSdrLift::UpdateAmbientSdrLift(vpv.led.sdrLift, sdrBlackNits, sdrTargetNits, renderBlackNits, inputIsSdr, image.color.transfer);
}

double LibplaceboVideoRenderer::Impl::LedDisplayGamma(enum pl_color_transfer transfer)
{
	return Vpv::LedSdrLift::DisplayGamma(transfer);
}

template<class HostImpl>
bool Vpv::HostAccess::PresentDejudderExtras(HostImpl& i, const Vpv::Dejudder::ExtrasFrame& f)
{
	struct Calls final : Vpv::Dejudder::ExtrasHostCalls
	{
		HostImpl& i;
		explicit Calls(HostImpl& impl) : i(impl) {}
		void DejudderHold(const char* why) override { i.DejudderHold(why); }
		void UpdateRenderAheadBudget(double captureRateHz) override { i.UpdateRenderAheadBudget(captureRateHz); }
		bool RenderAheadActive() const override { return i.RenderAheadActive(); }
		::Dejudder::Settings RenderedWarpSettings(int outputHeight) const override { return i.RenderedWarpSettings(outputHeight); }
		OpenLoopDisplayRate::Choice SelectDejudderDisplayRate() override { return i.SelectDejudderDisplayRate(); }
		void ResolveRenderAheadTimers() override { i.ResolveRenderAheadTimers(); }
		void ResetRenderAheadGate(const char* why) override { i.ResetRenderAheadGate(why); }
		double OpenLoopGridVsync(double displayHz, double now, double qpcFrequency) const override { return HostImpl::OpenLoopGridVsync(displayHz, now, qpcFrequency); }
		int UpdateRenderAheadGate(double now, double sourcePeriod, double displayPeriod, bool late, int maxExtras) override { return i.UpdateRenderAheadGate(now, sourcePeriod, displayPeriod, late, maxExtras); }
		bool EntranceOnce() const override { return i.EntranceOnce(); }
		bool EntranceOnceExtra(const pl_frame& image, uint64_t sourceSequence, uint64_t nextSourceSequence,
			bool interpolating, const ::Dejudder::Planes& a, ::Dejudder::Planes& processedA,
			::Dejudder::Planes& processedB, const pl_frame*& processedFrame) override
		{
			return i.EntranceOnceExtra(image, sourceSequence, nextSourceSequence, interpolating, a, processedA, processedB, processedFrame);
		}
		void DrainEntranceBeforeRender() override { i.DrainEntranceBeforeRender(); }
		bool DitherVectorsSupported() const override { return i.DitherVectorsSupported(); }
		bool RenderCrossfadeMix(const pl_frame& from, const pl_frame& to, pl_frame& target,
			const pl_render_params& params, const pl_color_space* historyColor, double factor) override
		{
			return i.RenderCrossfadeMix(from, to, target, params, historyColor, factor);
		}
		bool EstimateAhead(uint64_t sourceSequence, uint64_t nextSourceSequence, bool renderedPath,
			Vpv::Dejudder::FrameGpuTimingScope* timing) override
		{
			return i.EstimateAhead(sourceSequence, nextSourceSequence, renderedPath, timing);
		}
		void SyncFallback(const FallbackCatalog::Decision& entry) override { i.SyncFallback(entry); }
		VpMotion::P3FieldSet LiveFieldSet() const override { return i.vectorEstimate.Live(); }
		void RefreshSnapshotFields(pl_tex& warpField, pl_tex& warpTrust, pl_tex& warpSub,
			pl_tex& warpCamera, pl_tex& warpMask, pl_tex& warpVectorOk) const override
		{
			const auto snapSet = i.vectorEstimate.Snapshot();
			warpField = snapSet.Field();
			warpTrust = i.vectorEstimate.LookAheadTrust();
			warpSub = snapSet.SubField();
			if (warpCamera) warpCamera = snapSet.Camera();
			if (warpMask) warpMask = snapSet.PixelMask();
			if (warpVectorOk) warpVectorOk = snapSet.VectorOk();
		}
		pl_tex Occlusion(int slot) const override { return i.vectorEstimate.Occlusion(slot); }
	} calls(i);

	Vpv::Dejudder::ExtrasHost host{
		&i, i.log, i.d3d11, i.renderer, i.swapchain, i.vpOwnedSwapchain.p,
		i.activeSettings, i.actualOutput.presentationModel == LibplaceboOutput::PresentationModel::FLIP,
		i.textures, i.presentationTelemetry, i.gpuFrameTimer, i.passTimerHook,
		i.renderTimingContext.p, i.outputEncodingRepair, i.occlusionMaskHook,
		i.grainToleranceParamSet, i.dejudderMaskParamSet, i.temporalRenderer.Active(),
		i.outputMerger.get(), i.entrance, i.entranceOnceAheadNow, i.startupHoldBlack,
		i.rebaseFade, i.resync, i.resyncDrive, i.frameInterpolator,
		i.renderAheadExtraTimer, i.renderAheadEstimateTimer, i.renderAheadTimersReady,
		i.dejudderPolicy, i.renderAheadPresenter, i.motionValidity, i.dejudder,
		i.dejudderEstimatedSequence, i.dejudderEstimatedEpoch,
		i.dejudderLayout.valid, i.dejudderLayout.rgb, i.dejudderNextTextures,
		i.dejudderAhead, i.renderAheadBudgetCredit, &calls
	};
	return Vpv::Dejudder::PresentExtras(host, i.dejudderExtras, f);
}
