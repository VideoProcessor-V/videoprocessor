#include <pch.h>
#include "DynamicDriveFrame.h"

#include <VP-V/modules/drive/led_compensation/LedDriveModule.h>
#include <VP-V/modules/drive/led_compensation/LedRawTransfer.h>
#include <VP-V/modules/drive/led_measure/LedLookahead.h>
#include <DebugLog.h>
#include <DisplayRefreshRatePolicy.h>

#include <Windows.h>

#include <algorithm>

namespace LedDrive
{
	DynamicDriveFrame::DynamicDriveFrame(std::shared_ptr<QueuedFrameStatistics::IObserver>& walkObserverIn,
		FallbackStatus::Registry& fallbacksIn, CustomHookChain::Loader loader) :
		DynamicDriveShared(walkObserverIn), fallbacks(fallbacksIn), compensation(std::move(loader))
	{
	}

	bool DynamicDriveFrame::SendIntentLimits(double shadowMinimumShare, double ambientBlackNits, double displayBlackNits)
	{
		const std::shared_ptr<LedDriveModule> led = std::atomic_load(&module);
		if (!led)
			return false;
		// The display black at full drive lets the LED drive derive the ambient share floor
		// (LedDriveModule::SetIntentLimits).
		led->SetIntentLimits(shadowMinimumShare, ambientBlackNits, displayBlackNits);
		DebugLog::Log("Intent interpreter: LED intent limits shadow minimum share %.3f, ambient black %.5f nits, display black %.5f nits",
			shadowMinimumShare, ambientBlackNits, displayBlackNits);
		return true;
	}

	int DynamicDriveFrame::PictureOnlyMasks(const DynamicDriveFrameInputs& in, MeasurementRect* masks,
		PixelRect* sourceMasks)
	{
		// Bug hunt 2 (owner 2026-10-17, led_picture_only): measure the PICTURE ONLY. The detected
		// subtitle (padded box and card), and a moved subtitle's place, cover and cleanup fill,
		// are left out of every LED statistic. Detection runs only with a subtitle style that
		// moves cues (Black / Generated gray) or the box test; with no detection nothing is
		// masked. VP's own overlays (stats, OSD, profile panel, the subtitle test box) are target
		// overlays libplacebo draws after the output pass, so this PRE_OUTPUT measurement never
		// sees them.
		const pl_rect2df& crop = in.image->crop;
		std::vector<SubtitleBoxRect> boxes;
		if (in.subtitle && in.subtitle->detected && in.subtitle->bounds.Valid())
		{
			boxes.push_back(ExpandSubtitleBox(in.subtitle->bounds, in.sourceWidth, in.sourceHeight, in.subtitlePadding));
			boxes.push_back(in.subtitle->sourcePanel);
		}
		if (in.subtitleComposed && in.relocatedSubtitle)
			for (const SubtitleBoxRect& r : { in.relocatedSubtitle->destination, in.relocatedSubtitle->layoutCoverage,
				in.relocatedSubtitle->generatedCleanup, in.relocatedSubtitle->source })
				boxes.push_back(r);
		// NLS warps the picture before the scaler: a box follows the NLS map (the shader's
		// output-to-source curve, inverted per edge). An NLS shader whose map is not known masks
		// the box's rows across the whole width, a little taller.
		const NlsMaskMapping::Mapping nls = in.nlsSubmitted && in.nlsMapping ? in.nlsMapping() : NlsMaskMapping::Mapping();
		auto outputOf = [&](double source, bool vertical)
		{
			const double scale = vertical ? nls.yScale : nls.xScale;
			const double protect = vertical ? nls.yProtect : nls.xProtect;
			double lo = 0.0, hi = 1.0;
			for (int i = 0; i < 40; ++i)
			{
				const double mid = 0.5 * (lo + hi);
				if (NlsMaskMapping::MapCoordinate(mid, scale, nls.curve, nls.geometry, protect) < source)
					lo = mid;
				else
					hi = mid;
			}
			return 0.5 * (lo + hi);
		};
		int count = 0;
		for (const SubtitleBoxRect& box : boxes)
		{
			if (!box.Valid() || count >= MAX_MASK_RECTS) continue;
			MeasurementRect m = SourceRectInCrop(box.left, box.top, box.right, box.bottom,
				crop.x0, crop.y0, crop.x1, crop.y1);
			if (Empty(m)) continue;
			sourceMasks[count] = { static_cast<double>(box.left), static_cast<double>(box.top),
				static_cast<double>(box.right), static_cast<double>(box.bottom) };
			if (in.nlsSubmitted && nls.valid)
				m = { outputOf(m.x0, false), outputOf(m.y0, true), outputOf(m.x1, false), outputOf(m.y1, true) };
			else if (in.nlsSubmitted)
				m = { 0.0, (std::max)(0.0, m.y0 - 0.02), 1.0, (std::min)(1.0, m.y1 + 0.02) };
			masks[count++] = m;
		}
		return count;
	}

	std::string DynamicDriveFrame::CompensationUnavailable(bool slotRuns, size_t collected, bool runtimeDisabled)
	{
		if (collected > 0)
			return std::string();
		return LedDriveModule::CompensationUnavailableReason(slotRuns, !slotRuns || runtimeDisabled, !runtimeDisabled);
	}

	void DynamicDriveFrame::PrepareFrame(const DynamicDriveFrameInputs& in, std::vector<const pl_hook*>& hooks,
		const std::function<void(size_t, PipelineSlots::Id)>& tagSlot)
	{
		const pl_frame& image = *in.image;
		const pl_frame& target = *in.target;
		const pl_frame& screen = *in.screen;
		// LED drive, gain in: measurement first (it must see the picture before the gain), then
		// the compensation hook with this frame's parameters.
		const std::shared_ptr<LedDriveModule> led = std::atomic_load(&module);
		// The gamma boost's base is the gamma this frame's output is encoded for (delta 4), not
		// the projector's compensation_eotf.
		const double displayGamma = in.displayGamma;
		if (led)
			led->SetDisplayGamma(displayGamma);
		const double boostExponent = BoostExponent(gammaBoost.load(std::memory_order_acquire), displayGamma);
		if (boostExponent != lastLoggedBoostExponent)
		{
			lastLoggedBoostExponent = boostExponent;
			DebugLog::Log("Dynamic Drive: gamma boost %s (exponent %.4f on output gamma %.2f)",
				boostExponent != 1.0 ? "on" : "off", boostExponent, displayGamma);
		}
		if (!(led || boostExponent != 1.0 || shadowLift.load(std::memory_order_acquire) > 0.0))
			return;
		if (!compensationConfigured)
		{
			ConfiguredShaderRule compensationRule;
			compensationRule.name = "led_compensation";
			compensationRule.filename = "LED compensation.glsl";
			compensation.Configure({ compensationRule });
			compensationConfigured = true;
		}
		compensation.Ensure(*in.hookTemplates);
		CompensationGain gain;
		gain.knee = knee.load(std::memory_order_acquire);
		gain.eotf = compensationEotf.load(std::memory_order_acquire);
		if (led)
		{
			// The look-ahead reads the captured frame: its range as the picture's.
			led->SetSourceRange(*in.sourceRange == "full" ? 1 : *in.sourceRange == "limited" ? 0 : -1);
			// Integration 2026-10-17 (owner: measured, not nominal): the source rate the LED
			// look-ahead converts led_lookahead_ms with is the measured one (the film rate when
			// pulldown is locked, else the measured capture rate); nominal only when nothing is
			// measured yet.
			const double sourceRateHz = CaptureRateBounds::Accepts(in.captureRateHz) ?
				in.captureRateHz : in.nominalSourceRateHz;
			// Render-ahead (design step 6): this frame is rendered ahead of its display, so the
			// gain and the projector drive use the planned refresh it lands on (the chain's next
			// refresh) instead of the wall clock.
			double aheadMs = 0.0;
			if (in.renderAheadActive)
			{
				LARGE_INTEGER qpcFrequency{};
				LARGE_INTEGER qpcNow{};
				if (QueryPerformanceFrequency(&qpcFrequency) && qpcFrequency.QuadPart > 0)
				{
					const int64_t now = QueryPerformanceCounter(&qpcNow) ? qpcNow.QuadPart : 0;
					aheadMs = (in.renderAheadNextVsyncSeconds - static_cast<double>(now) /
						static_cast<double>(qpcFrequency.QuadPart)) * 1000.0;
				}
			}
			gain = aheadMs > 0.0 && aheadMs < 1000.0 ?
				led->BeginFrameAhead(in.sourceSequence, aheadMs, sourceRateHz) :
				led->BeginFrame(in.sourceSequence, sourceRateHz);
			if (led->WantsPredictions())
			{
				// The look-ahead's raw-to-output transfer follows this frame's colour mapping;
				// rebuilt only when that changes.
				const pl_color_map_params* colorMap = in.params->color_map_params ?
					in.params->color_map_params : &pl_color_map_default_params;
				const uint64_t transferSignature = RawTransferSignature(image.color, target.color, colorMap);
				if (transferSignature != rawTransferSignature)
				{
					rawTransferSignature = transferSignature;
					led->SetRawTransfer(std::make_shared<const RawTransfer>(
						BuildRawTransfer(image.color, target.color, colorMap)));
				}
			}
			const HdrPeakAnalysisCrop::TrustedPicture& trusted = *in.trustedPicture;
			const LedDriveConfig config = led->Config();
			measurement.SetFrame(ActivePictureRect(trusted.available, trusted.left, trusted.top, trusted.right,
				trusted.bottom, image.crop.x0, image.crop.y0, image.crop.x1, image.crop.y1), config.aplWeights,
				boostExponent, in.sourceSequence);
			// Bug hunt 2 (owner 2026-10-17, led_peak_full_coverage): the peak over every pixel, in
			// boxes the share of the screen of the script's full-screen downscale. The screen is
			// the whole output (the script's screenshot).
			const double screenW = screen.crop.x1 - screen.crop.x0;
			const double screenH = screen.crop.y1 - screen.crop.y0;
			measurement.SetPeak(config.peakFullCoverage, screenW, screenH);
			MeasurementRect masks[MAX_MASK_RECTS];
			PixelRect sourceMasks[MAX_MASK_RECTS];
			const int maskCount = config.pictureOnly ? PictureOnlyMasks(in, masks, sourceMasks) : 0;
			measurement.SetMask(masks, maskCount);
			// The look-ahead measures queued raw frames (source raster): the screen in source
			// pixels for the box size, and the current subtitle for their masks.
			{
				const double cropW = image.crop.x1 - image.crop.x0;
				const double cropH = image.crop.y1 - image.crop.y0;
				const double shownW = target.crop.x1 - target.crop.x0;
				const double shownH = target.crop.y1 - target.crop.y0;
				static_cast<LookaheadObserver&>(led->Lookahead()).SetPictureGeometry(
					shownW > 0.0 && cropW > 0.0 ? screenW * cropW / shownW : 0.0,
					shownH > 0.0 && cropH > 0.0 ? screenH * cropH / shownH : 0.0,
					sourceMasks, maskCount);
			}
			// Delta 8 R2: the grain map (the previous source frame's, as for S1) for
			// grain-suppressed highlight statistics.
			measurement.SetGrain(config.highlightGrainSuppress ? in.grainMap : nullptr, in.grainFloor, image.crop);
			if (in.measureSlotRuns)
			{
				hooks.push_back(measurement.Hook());
				tagSlot(hooks.size() - 1, PipelineSlots::Id::DriveLedMeasure);
			}
		}
		const size_t compensationStart = hooks.size();
		if (in.compensationSlotRuns)
			compensation.Collect(false, hooks);
		tagSlot(compensationStart, PipelineSlots::Id::DriveLedCompensation);
		if (boostExponent != 1.0 && hooks.size() == compensationStart)
		{
			if (!boostMissingLogged)
				DebugLog::Log("Dynamic Drive: WARNING gamma boost NOT applied - shaders\\LED compensation.glsl did not load or was disabled; the picture is plain output_gamma (exponent %.4f missing)",
					boostExponent);
			boostMissingLogged = true;
		}
		else if (hooks.size() > compensationStart)
		{
			if (boostMissingLogged)
				DebugLog::Log("Dynamic Drive: gamma boost applied again (compensation hook running)");
			boostMissingLogged = false;
		}
		for (size_t i = compensationStart; i < hooks.size(); ++i)
		{
			CustomHookChain::SetHookParameter(hooks[i], "gain", gain.active ? gain.gain : 1.0);
			CustomHookChain::SetHookParameter(hooks[i], "knee", gain.knee);
			CustomHookChain::SetHookParameter(hooks[i], "gain_domain", gain.domain == GainDomain::Linear ? 1.0 : 0.0);
			CustomHookChain::SetHookParameter(hooks[i], "compensation_eotf", gain.eotf);
			CustomHookChain::SetHookParameter(hooks[i], "boost", boostExponent);
			CustomHookChain::SetHookParameter(hooks[i], "boost_knee", boostKnee.load(std::memory_order_acquire));
			CustomHookChain::SetHookParameter(hooks[i], "boost_knee_width", boostKneeWidth.load(std::memory_order_acquire));
			CustomHookChain::SetHookParameter(hooks[i], "shadow_lift", shadowLift.load(std::memory_order_acquire));
			CustomHookChain::SetHookParameter(hooks[i], "veil", gain.veil);
			CustomHookChain::SetHookParameter(hooks[i], "wtw_passthrough",
				wtwPassthrough.load(std::memory_order_acquire) ? 1.0 : 0.0);
			CustomHookChain::SetHookParameter(hooks[i], "btb_passthrough",
				btbPassthrough.load(std::memory_order_acquire) ? 1.0 : 0.0);
		}
	}

	void DynamicDriveFrame::OnHookErrors(const uint64_t* disabledSignatures, int count)
	{
		compensation.MarkRuntimeFailures(disabledSignatures, count);
		const std::shared_ptr<LedDriveModule> led = std::atomic_load(&module);
		for (int i = 0; led && !measurementDisabledLogged && i < count; ++i)
			if (disabledSignatures[i] == measurement.Hook()->signature)
			{
				DebugLog::Log("Dynamic Drive: WARNING libplacebo disabled the measurement pass; the LED keeps its last statistics");
				measurementDisabledLogged = true;
			}
	}

	void DynamicDriveFrame::CollectMeasurements()
	{
		const std::shared_ptr<LedDriveModule> led = std::atomic_load(&module);
		if (!led)
			return;
		std::vector<RawMeasurement> measured;
		std::vector<uint64_t> measuredSequences;
		measurement.Collect(measured, 0, &measuredSequences);
		for (size_t i = 0; i < measured.size(); ++i)
			led->SubmitMeasurement(measured[i], measuredSequences[i]);
	}

	void DynamicDriveFrame::Release()
	{
		measurement.Release();
		compensation.Clear();
		compensationConfigured = false;
	}

	void DynamicDriveFrame::Engage(const char* id, const std::string& reason)
	{
		if (fallbacks.Engage(id, reason))
			DebugLog::Log("%s", fallbacks.Heading().c_str());
	}

	void DynamicDriveFrame::ReleaseFallback(const char* id)
	{
		if (fallbacks.Release(id))
			DebugLog::Log("%s", fallbacks.Heading().c_str());
	}

	void DynamicDriveFrame::MergeModuleFallbacks(const std::shared_ptr<LedDriveModule>&)
	{
	}
}
