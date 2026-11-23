// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
#include <pch.h>
#include "PresentationHostHelpers.h"

#include <DebugLog.h>
#include <DisplayRefreshRatePolicy.h>
#include <VP-V/modules/control/diagnostics/DiagnosticHistory.h>
#include <VP-V/modules/motion/consumers/DejudderPolicy.h>
#include <VP-V/modules/output/presentation/RepeatReuseRegistryKey.h>

#include <atlbase.h>
#include <libplacebo/d3d11.h>

#include <algorithm>
#include <chrono>
#include <cmath>

namespace Vpv
{
	namespace PresentationHost
	{
		namespace
		{
			using SteadyClock = std::chrono::steady_clock;

			int64_t PerformanceCounterNow()
			{
				LARGE_INTEGER counter{};
				QueryPerformanceCounter(&counter);
				return counter.QuadPart;
			}
		}

		double RenderAheadNow()
		{
			return std::chrono::duration<double>(SteadyClock::now().time_since_epoch()).count();
		}

		void RenderAheadBreak(RenderAhead::Presenter& presenter)
		{
			presenter.Break(RenderAheadNow());
		}

		void ResetRenderAheadGate(RenderAhead::Presenter& presenter, const char* why)
		{
			const int level = presenter.ResetGate();
			DebugLog::Log("Render-ahead gate: reset (%s; was %s)", why,
				level == 0 ? "full" : "degraded");
			DiagnosticHistory::Global().RecordDecision("ra-gate-reset", why, level);
		}

		double OpenLoopGridVsync(double displayHz, double now, double qpcFrequency,
			bool (*dwmCompositionTiming)(double&, int64_t&))
		{
			double dwmHz = 0.0;
			int64_t vblank = 0;
			if (!dwmCompositionTiming || !dwmCompositionTiming(dwmHz, vblank) || vblank <= 0 || !(displayHz > 0.0) ||
				std::abs(dwmHz - displayHz) > OpenLoopDisplayRate::kDwmAgreesWithMode * displayHz)
				return 0.0;
			const double vblankSeconds = static_cast<double>(vblank) / qpcFrequency;
			return std::abs(now - vblankSeconds) < 1.0 ? vblankSeconds : 0.0;
		}

		void ResolveRenderAheadTimers(RenderAhead::Presenter& presenter, bool timersReady,
			ID3D11DeviceContext* renderTimingContext,
			AlphaD3D11FrameGpuTimer& extraTimer, AlphaD3D11FrameGpuTimer& estimateTimer,
			bool ditherLastExternal)
		{
			if (timersReady && renderTimingContext)
				presenter.ResolveTimers(extraTimer, estimateTimer, renderTimingContext, RenderAheadNow(), ditherLastExternal);
		}

		int UpdateRenderAheadGate(RenderAhead::Presenter& presenter, double now, double sourcePeriod,
			double displayPeriod, bool late, int maxExtras, double utilisation, bool vpOwnedChain,
			bool l1Available, double ditherMsOwn, double ditherMsExternal)
		{
			RenderAhead::PresenterGateFrame frame;
			frame.sourcePeriod = sourcePeriod;
			frame.displayPeriod = displayPeriod;
			frame.utilisation = utilisation;
			frame.vpOwnedChain = vpOwnedChain;
			frame.l1Available = l1Available;
			frame.ditherMsOwn = ditherMsOwn;
			frame.ditherMsExternal = ditherMsExternal;
			frame.late = late;
			frame.maxExtras = maxExtras;
			return RenderAhead::HostSteps::UpdateGateLogged(presenter, now, frame);
		}

		UINT DejudderExtrasBetween(const Dejudder::ExtrasState& extras, UINT from, UINT to)
		{
			return DejudderPolicy::ExtrasBetween(extras.dejudderExtraIds, from, to);
		}

		void AdjustSampleForDejudderExtras(const Dejudder::ExtrasState& extras,
			AlphaDxgiPresentationSample& sample, IDXGISwapChain* native)
		{
			if (native != extras.dejudderIdsSwapchain)
				return;
			uint32_t count = sample.presentCount, refresh = sample.presentRefreshCount;
			if (DejudderPolicy::MainPresentFor(extras.dejudderExtraIds, extras.dejudderMainRefresh, count, refresh))
			{
				sample.presentCount = count;
				sample.presentRefreshCount = refresh;
			}
		}

		OpenLoopDisplayRate::Choice SelectDejudderDisplayRate(DisplayRateHost& h)
		{
			OpenLoopDisplayRate::Candidates c;
			int64_t dwmVblank = 0;
			c.statisticsHz = h.presentationTelemetry.Snapshot().measuredDisplayHz;
			if (h.renderAheadActive && !DisplayRefreshBounds::Accepts(c.statisticsHz))
			{
				const ULONGLONG now = GetTickCount64();
				if (h.openLoopCandidatesTick == 0 || now - h.openLoopCandidatesTick >= 2000)
				{
					h.openLoopCandidatesTick = now;
					h.openLoopDisplayConfigHz = h.monitorDisplayConfigHz ? h.monitorDisplayConfigHz(h.videoHwnd) : 0.0;
					h.openLoopModeIntegerHz = h.monitorRefreshHz ? h.monitorRefreshHz(h.videoHwnd) : 0.0;
				}
				if (h.dwmCompositionTiming)
					h.dwmCompositionTiming(c.dwmHz, dwmVblank);
				c.displayConfigHz = h.openLoopDisplayConfigHz;
				c.modeIntegerHz = h.openLoopModeIntegerHz;
				c.hostHz = h.renderLoadDisplayRefreshHz.load(std::memory_order_acquire);
				// The last resort, when nothing else is known: a short vblank sample on the
				// swapchain's output (blocks about ten refreshes, at most once per 10 s).
				// Hunt 2 05-renderahead M5 (queue_timing_fixes): a whole-hertz mode alone is
				// not enough to skip it (the cadence now ranks above that mode).
				const bool nothing = !DisplayRefreshBounds::Accepts(c.displayConfigHz) &&
					!DisplayRefreshBounds::Accepts(c.dwmHz) && !DisplayRefreshBounds::Accepts(c.hostHz) &&
					(h.queueTimingFixes || !DisplayRefreshBounds::Accepts(c.modeIntegerHz));
				if (nothing && !(h.openLoopCadence.Hz() > 0.0) && h.swapchain &&
					(h.openLoopCadenceTick == 0 || now - h.openLoopCadenceTick >= 10000))
				{
					h.openLoopCadenceTick = now;
					h.openLoopCadence.Reset();
					CComPtr<IDXGISwapChain> native;
					native.Attach(pl_d3d11_swapchain_unwrap(h.swapchain));
					CComPtr<IDXGIOutput> output;
					LARGE_INTEGER frequency{};
					if (native && SUCCEEDED(native->GetContainingOutput(&output)) && output &&
						QueryPerformanceFrequency(&frequency) && frequency.QuadPart > 0)
					{
						for (size_t i = 0; i <= OpenLoopDisplayRate::VBlankCadence::kMinIntervals; ++i)
						{
							if (FAILED(output->WaitForVBlank()))
								break;
							h.openLoopCadence.Add(static_cast<double>(PerformanceCounterNow()) / static_cast<double>(frequency.QuadPart));
						}
					}
				}
				c.cadenceHz = h.openLoopCadence.Hz();
			}
			OpenLoopDisplayRate::Choice choice = OpenLoopDisplayRate::Select(c, h.renderAheadActive, h.queueTimingFixes);
			// Hunt 2 05-renderahead M5 (queue_timing_fixes): in open loop the planned rate is
			// refined to the display's real refresh, measured from DWM's vblank times, so the
			// chain does not drift into a lateness correction (a dropped extra) every few
			// minutes. Only DWM vblanks at the planned rate (the same display) are used.
			if (h.queueTimingFixes && choice.openLoop)
			{
				LARGE_INTEGER frequency{};
				if (dwmVblank > 0 && QueryPerformanceFrequency(&frequency) && frequency.QuadPart > 0 &&
					DisplayRefreshBounds::Accepts(c.dwmHz) &&
					std::abs(c.dwmHz - choice.hz) <= OpenLoopDisplayRate::kDwmAgreesWithMode * choice.hz)
					h.openLoopGridRate.Add(static_cast<double>(dwmVblank) / static_cast<double>(frequency.QuadPart), choice.hz);
				const double refined = h.openLoopGridRate.Refine(choice.hz);
				if (refined != choice.hz && !h.openLoopGridRateLogged)
				{
					h.openLoopGridRateLogged = true;
					DebugLog::Log("Render-ahead: open loop, display rate refined from DWM vblanks: %.6f Hz (planned %.6f Hz from %s)",
						refined, choice.hz, OpenLoopDisplayRate::Name(choice.source));
				}
				choice.hz = refined;
			}
			else
			{
				h.openLoopGridRate.Reset();
				h.openLoopGridRateLogged = false;
			}
			if (h.renderAheadActive && OpenLoopDisplayRate::Changed(h.dejudderDisplayRateLogged, choice))
			{
				if (choice.openLoop)
					DebugLog::Log("Render-ahead: open loop, display rate %.3f Hz from %s (no DXGI frame statistics; candidates: "
						"display config %.3f, DWM %.3f, application %.3f, whole-Hz mode %.0f, vblank cadence %.3f Hz)",
						choice.hz, OpenLoopDisplayRate::Name(choice.source), c.displayConfigHz, c.dwmHz, c.hostHz,
						c.modeIntegerHz, c.cadenceHz);
				else if (choice.source == OpenLoopDisplayRate::Source::Statistics)
					DebugLog::Log("Render-ahead: closed loop, display rate %.3f Hz from %s", choice.hz,
						OpenLoopDisplayRate::Name(choice.source));
				else
					DebugLog::Log("Render-ahead: no display rate from any source (statistics, display config, DWM, "
						"application, mode, vblank cadence)");
				// a = 1 open loop / 0 closed (or none), x = display Hz.
				DiagnosticHistory::Global().RecordDecision(choice.openLoop ? "ra-open-loop" : "ra-closed-loop",
					OpenLoopDisplayRate::Name(choice.source), choice.openLoop ? 1 : 0, 0, choice.hz);
				h.dejudderDisplayRateLogged = choice;
			}
			// Silent-fallbacks batch: open-loop presentation and a de-judder held for want of a display rate.
			if (h.syncFallback)
				h.syncFallback(FallbackCatalog::Presentation(h.renderAheadActive && choice.openLoop, h.heldNoDisplayRate,
					choice.openLoop ? OpenLoopDisplayRate::Name(choice.source) : ""));
			return choice;
		}

		void UpdateRenderAheadBudget(BudgetHost& h, double captureRateHz)
		{
			if (!h.renderAheadEnabled)
			{
				h.renderAheadBudgetVeto = false;
				h.renderAheadBudgetCredit = 0;
				if (h.releaseFallback)
					h.releaseFallback("render-ahead");
				return;
			}
			double displayHz = h.renderLoadDisplayRefreshHz.load(std::memory_order_acquire);
			if (!(displayHz > 0.0))
				displayHz = h.presentationTelemetry.Snapshot().measuredDisplayHz;
			const RenderAhead::HostSteps::BudgetStep step = RenderAhead::HostSteps::StepDelayBudget(h.renderAheadBudgetVeto, captureRateHz, displayHz,
				h.earlyUploadQueueTarget.load(std::memory_order_acquire), h.renderAheadQueueCredit);
			if (!step.decided)
				return;   // rates not known yet: the last decision stands
			h.renderAheadBudgetCredit = step.credit;
			if (step.changed)
			{
				if (step.veto)
				{
					RenderAheadBreak(h.renderAheadPresenter);
					h.renderAheadPresenter.credit.Reset();
					h.renderAheadPresenter.creditFrames.store(0, std::memory_order_release);
				}
				else
					h.renderAheadPresenter.epoch.Invalidate();   // as render_ahead switched on: the gate starts again
				h.renderAheadBudgetVeto = step.veto;
			}
			if (step.veto)
			{
				if (h.engageFallback)
					h.engageFallback("render-ahead", step.why);
			}
			else if (h.releaseFallback)
				h.releaseFallback("render-ahead");
		}

		double RenderAheadChainRatio(double dejudderSourceHz, double displayHz)
		{
			return dejudderSourceHz > 0.0 && displayHz > 0.0 ? displayHz / dejudderSourceHz : 0.0;
		}

		void CheckRenderAheadChainDepthLocked(ChainDepthHost& h)
		{
			if (!h.renderAheadEnabled || !h.hasVpOwnedSwapchain || h.swapchainBlit || !h.hasSwapchain)
			{
				h.renderAheadChainWantedSince = -1.0;
				return;
			}
			const RenderAhead::ChainDepth wanted = RenderAhead::ChainDepthFor(h.renderAheadActive, h.lowLatency, h.ratio);
			if (!RenderAhead::HostSteps::ChainDepthSettled(h.renderAheadChainWantedSince, h.renderAheadPresenter.chainDepth, wanted, RenderAheadNow(), h.ratio, h.lowLatency))
				return;
			RenderAheadBreak(h.renderAheadPresenter);
			h.renderAheadPresenter.epoch.Invalidate();   // another queue depth: the gate starts again
			if (!h.recreateSwapchain || !h.recreateSwapchain())
			{
				// Keep the attempt from repeating every frame; ConfigureAndFallback recovers
				// a missing chain as for any other recreation failure.
				h.renderAheadPresenter.chainDepth = wanted;
				return;
			}
			if (h.ensureAutoLimitedFallback)
				h.ensureAutoLimitedFallback();
		}

		uint64_t DisplayLutSignature(uint64_t observedBytes, uint64_t observedWriteTime,
			bool displayLutParsed, bool hasDisplayLut)
		{
			return RepeatRenderReuse::Signature().Add(observedBytes)
				.Add(observedWriteTime)
				.Add(displayLutParsed).Add(hasDisplayLut).Value();
		}

		VpRepeatReuse::VpContext BuildRepeatReuseContext(const RepeatReuseHost& h,
			const pl_frame& image, const pl_frame& target)
		{
			VpRepeatReuse::VpContext c;
			c.temporal.phasePicture = h.phasePicture;
			c.temporal.dejudderExtras = h.dejudderExtras;
			c.temporal.crossfade = h.crossfade;
			c.temporal.startupHoldBlack = h.startupHoldBlack;
			c.temporal.deinterlacing = h.deinterlacing;
			c.temporal.frameMixing = h.frameMixing;
			c.temporal.ledMeasurePerRefresh = std::find(h.frameHooks.begin(), h.frameHooks.end(), h.ledMeasurementHook) != h.frameHooks.end();
			c.cache = h.cache;
			c.gpu = h.gpu;
			c.encodingRepair = h.encodingRepair;
			c.image = &image;
			c.target = &target;
			c.extension = { h.rebaseFactor, h.rebaseFactorX, h.rebaseKernel, h.displayLutSignature, 0,
				RepeatRenderReuse::Signature().Add(h.fallbacksHeading).Value(), VpRepeatReuse::RegistryFingerprint(h.pipelineSlots) };
			using K = VpRepeatReuse::HookKind;
			const VpRepeatReuse::KnownHook known[] = {
				{ h.ledMeasurementHook, K::PictureNeutral, 0, "LED measurement" },
				{ h.cache ? h.cache->Hook() : nullptr, K::PictureNeutral, 0, "rendered-picture cache" },
				{ h.calibrationStageHook, K::Generation, h.displayLutSignature, "calibration LUT stage" },
				{ h.encodingRepair, K::Static, 0, "output encoding repair" },
				// Extended Range: a function of the source frame, the colours, the LUT and the settings
				// (all in the key) - static per source frame (audit H1).
				{ h.extendedRangeHooks.Carry(), K::Static, 0, "Extended Range carry" },
				{ h.extendedRangeHooks.LateRestore(), K::Static, 0, "Extended Range late restore" },
				{ h.extendedRangeHooks.Restore(false), K::Static, 0, "Extended Range restore" },
				{ h.extendedRangeHooks.Restore(true), K::Static, 0, "Extended Range restore" },
				// AdaptiveDownscale smooths its grain map over time: opaque until it exposes a state generation.
				{ h.adaptiveDownscale.Measure(), K::Opaque, 0, "AdaptiveDownscale" },
				{ h.adaptiveDownscale.Blend(), K::Opaque, 0, "AdaptiveDownscale" },
			};
			for (const auto& k : known)
				if (c.knownCount < static_cast<int>(c.known.size()))
					c.known[c.knownCount++] = k;
			// Left out of the reuse draw: picture-neutral only. The occlusion mask hook stays (audit M3): on a
			// repeat it carries the sigma-delta's grain-tolerance map.
			c.exclude[c.excludeCount++] = h.ledMeasurementHook;
			c.exclude[c.excludeCount++] = h.cache ? h.cache->Hook() : nullptr;
			return c;
		}
	}
}
