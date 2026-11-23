// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
#pragma once

// Renderer-host presentation and repeat-reuse helpers moved out of LibplaceboVideoRenderer.cpp::Impl
// into a compiled VP-V unit (output.presentation). Keep RenderAheadPolicy.h / RenderAheadHostSteps.h
// free of Win32/DXGI/libplacebo headers so pure CPU tests compile unchanged.

#include <VP-V/core/FallbackCatalog.h>
#include <VP-V/core/SlotRegistry.h>
#include <VP-V/modules/colour/extended_range/ExtendedRange.h>
#include <VP-V/modules/motion/consumers/DejudderExtrasPresent.h>
#include <VP-V/modules/output/presentation/OpenLoopDisplayRate.h>
#include <VP-V/modules/output/presentation/RenderAheadHostSteps.h>
#include <VP-V/modules/output/presentation/RenderAheadPolicy.h>
#include <VP-V/modules/output/presentation/RepeatReuseGlue.h>
#include <VP-V/modules/scaling/output/AdaptiveDownscale.h>
#include <vprenderer/AlphaD3D11FrameGpuTimer.h>
#include <vprenderer/AlphaPresentationTelemetry.h>

#include <windows.h>
#include <dxgi.h>
#include <libplacebo/renderer.h>
#include <libplacebo/swapchain.h>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace Vpv
{
	namespace PresentationHost
	{
		// One VP-V definition matching Bill's PerformanceCounterNow in LibplaceboVideoRenderer.cpp
		// (returns 0 on QueryPerformanceCounter failure; pinned by test_reloc2_timing_helper_drift.py).
		int64_t PerformanceCounterNow();

		double RenderAheadNow();

		void RenderAheadBreak(RenderAhead::Presenter& presenter);

		void ResetRenderAheadGate(RenderAhead::Presenter& presenter, const char* why);

		double OpenLoopGridVsync(double displayHz, double now, double qpcFrequency,
			bool (*dwmCompositionTiming)(double&, int64_t&));

		void ResolveRenderAheadTimers(RenderAhead::Presenter& presenter, bool timersReady,
			ID3D11DeviceContext* renderTimingContext,
			AlphaD3D11FrameGpuTimer& extraTimer, AlphaD3D11FrameGpuTimer& estimateTimer,
			bool ditherLastExternal);

		int UpdateRenderAheadGate(RenderAhead::Presenter& presenter, double now, double sourcePeriod,
			double displayPeriod, bool late, int maxExtras, double utilisation, bool vpOwnedChain,
			bool l1Available, double ditherMsOwn, double ditherMsExternal);

		UINT DejudderExtrasBetween(const Dejudder::ExtrasState& extras, UINT from, UINT to);

		void AdjustSampleForDejudderExtras(const Dejudder::ExtrasState& extras,
			AlphaDxgiPresentationSample& sample, IDXGISwapChain* native);

		struct DisplayRateHost
		{
			bool renderAheadActive = false;
			bool queueTimingFixes = false;
			bool heldNoDisplayRate = false;
			HWND videoHwnd = nullptr;
			pl_swapchain swapchain = nullptr;
			AlphaPresentationTelemetry& presentationTelemetry;
			const std::atomic<double>& renderLoadDisplayRefreshHz;
			ULONGLONG& openLoopCandidatesTick;
			ULONGLONG& openLoopCadenceTick;
			double& openLoopDisplayConfigHz;
			double& openLoopModeIntegerHz;
			OpenLoopDisplayRate::VBlankCadence& openLoopCadence;
			OpenLoopDisplayRate::VBlankGridRate& openLoopGridRate;
			bool& openLoopGridRateLogged;
			OpenLoopDisplayRate::Choice& dejudderDisplayRateLogged;
			double (*monitorDisplayConfigHz)(HWND) = nullptr;
			double (*monitorRefreshHz)(HWND) = nullptr;
			bool (*dwmCompositionTiming)(double&, int64_t&) = nullptr;
			std::function<void(const FallbackCatalog::Decision&)> syncFallback;
		};

		OpenLoopDisplayRate::Choice SelectDejudderDisplayRate(DisplayRateHost& h);

		struct BudgetHost
		{
			bool renderAheadEnabled = false;
			bool renderAheadQueueCredit = false;
			bool& renderAheadBudgetVeto;
			size_t& renderAheadBudgetCredit;
			const std::atomic<double>& renderLoadDisplayRefreshHz;
			const std::atomic<size_t>& earlyUploadQueueTarget;
			AlphaPresentationTelemetry& presentationTelemetry;
			RenderAhead::Presenter& renderAheadPresenter;
			std::function<void(const char*, const std::string&)> engageFallback;
			std::function<void(const char*)> releaseFallback;
		};

		void UpdateRenderAheadBudget(BudgetHost& h, double captureRateHz);

		double RenderAheadChainRatio(double dejudderSourceHz, double displayHz);

		struct ChainDepthHost
		{
			bool renderAheadEnabled = false;
			bool renderAheadActive = false;
			bool hasVpOwnedSwapchain = false;
			bool swapchainBlit = false;
			bool hasSwapchain = false;
			bool lowLatency = false;
			double ratio = 0.0;
			double& renderAheadChainWantedSince;
			RenderAhead::Presenter& renderAheadPresenter;
			std::function<bool()> recreateSwapchain;
			std::function<void()> ensureAutoLimitedFallback;
		};

		void CheckRenderAheadChainDepthLocked(ChainDepthHost& h);

		uint64_t DisplayLutSignature(uint64_t observedBytes, uint64_t observedWriteTime,
			bool displayLutParsed, bool hasDisplayLut);

		struct RepeatReuseHost
		{
			bool phasePicture = false;
			bool dejudderExtras = false;
			bool crossfade = false;
			bool startupHoldBlack = false;
			bool deinterlacing = false;
			bool frameMixing = false;
			const std::vector<const pl_hook*>& frameHooks;
			const pl_hook* ledMeasurementHook = nullptr;
			DejudderRendered::Cache* cache = nullptr;
			pl_gpu gpu = nullptr;
			const pl_hook* encodingRepair = nullptr;
			const pl_hook* calibrationStageHook = nullptr;
			const ExtendedRange::Hooks& extendedRangeHooks;
			const AdaptiveDownscale::Module& adaptiveDownscale;
			double rebaseFactor = 1.0;
			double rebaseFactorX = 1.0;
			int rebaseKernel = -1;
			uint64_t displayLutSignature = 0;
			std::string fallbacksHeading;
			const PipelineSlots::Registry& pipelineSlots;
		};

		VpRepeatReuse::VpContext BuildRepeatReuseContext(const RepeatReuseHost& h,
			const pl_frame& image, const pl_frame& target);
	}
}
