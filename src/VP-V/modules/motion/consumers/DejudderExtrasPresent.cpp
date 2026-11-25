// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
//
// CC series A (A2+A3): compiled home of Vpv::Dejudder::PresentExtras, moved verbatim from
// LibplaceboVideoRenderer.cpp::Impl::PresentDejudderExtras. Bindings at the top of PresentExtras
// keep the body text and every log line byte-identical.
#include <pch.h>
#include "DejudderExtrasPresent.h"

#include <DebugLog.h>
#include <DisplayRefreshRatePolicy.h>
#include <GeneralRateLog.h>
#include <VP-V/modules/control/diagnostics/DiagnosticHistory.h>
#include <VP-V/modules/control/skeleton/SkeletonAccess.h>
#include <VP-V/modules/input/queue_timing/AlphaQueuePolicyVpCompat.h>
#include <VP-V/modules/input/signal/ResyncCallSites.h>
#include <VP-V/modules/output/dither/CustomHookChain.h>
#include <VP-V/modules/output/dither/SigmaDeltaVectors.h>
#include <VP-V/modules/output/presentation/PresentationHostHelpers.h>
#include <vprenderer/RendererExtension.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <type_traits>
#include <vector>

namespace Vpv
{
	namespace Dejudder
	{
		static_assert(std::is_default_constructible<ExtrasState>::value, "the renderer's Impl default-constructs its member");

		namespace
		{
			using SteadyClock = std::chrono::steady_clock;
			using Vpv::PresentationHost::PerformanceCounterNow;
			using Vpv::PresentationHost::RenderAheadNow;

			// Delta 8 R1 timing (planner 2026-10-13): the GPU time of an extra's
			// sigma-delta passes (libplacebo's per-pass timers; `last` is the most recent
			// completed measurement of that pass).
			struct DitherExtraPassTotals
			{
				double sigmaNs = 0.0;
			};

			void DitherExtraPassCallback(void* privateData, const struct pl_render_info* info)
			{
				if (!privateData || !info || !info->pass || !info->pass->shader || !info->pass->shader->description)
					return;
				if (std::strstr(info->pass->shader->description, "sigma-delta"))
					static_cast<DitherExtraPassTotals*>(privateData)->sigmaNs += static_cast<double>(info->pass->last);
			}

			// Comparison frames must never issue libplacebo's per-pass disjoint queries
			// inside VP's v2 disjoint interval. The paired libplacebo build retains its
			// timers but suppresses their issue and readback for exactly this render call.
			class LibplaceboTimerSuspensionScope
			{
			public:
				LibplaceboTimerSuspensionScope(pl_d3d11 d3d11, bool requested)
					: m_d3d11(d3d11), m_requested(requested),
					m_active(requested && d3d11 &&
						pl_d3d11_set_timer_queries_suspended(d3d11, true))
				{
				}

				~LibplaceboTimerSuspensionScope()
				{
					Resume();
				}

				LibplaceboTimerSuspensionScope(const LibplaceboTimerSuspensionScope&) = delete;
				LibplaceboTimerSuspensionScope& operator=(const LibplaceboTimerSuspensionScope&) = delete;

				bool Active() const { return m_active; }
				bool Failed() const { return m_requested && !m_active; }

				void Resume()
				{
					if (m_active)
					{
						pl_d3d11_set_timer_queries_suspended(m_d3d11, false);
						m_active = false;
					}
				}

			private:
				pl_d3d11 m_d3d11 = nullptr;
				bool m_requested = false;
				bool m_active = false;
			};
		}

		void LogRenderAhead(ExtrasState& dejudderExtras, const VpMotion::DejudderPolicyState& dejudderPolicy,
			const RenderAhead::Presenter& renderAheadPresenter, const std::string& dejudderGate,
			bool renderAheadActive, bool renderAheadTimersReady, double ratio, int gateLevel,
			const char* holding)
		{
			const ULONGLONG now = GetTickCount64();
			if (dejudderExtras.renderAheadLogTick != 0 && now - dejudderExtras.renderAheadLogTick < 5000)
				return;
			dejudderExtras.renderAheadLogTick = now;
			DebugLog::Log("Render-ahead: %s%s%s%llu extras presented, %llu frames planned, %llu restarts, %llu late, %llu extras dropped, %llu refreshes held "
				"(%llu presents after a hold), lead %.2f ms, target lead %.2f ms, gate %s (%s), latency floor %.2f ms, "
				"p95 new frame %.2f ms, extra %.2f ms (own vectors) / %.2f ms (de-judder vectors), estimate %.2f ms, queue credit %zu, cadence %.3f, %s, "
				"display %.3f Hz from %s (%s loop, %llu open-loop frames, %llu presents late by QPC, %llu early-shown chain corrections, %llu re-anchors, %llu anchors lowered, %llu after a backlog flush; shown - planned: interval 1 %+.2f refreshes "
				"(%llu, %llu on time), after holds %+.2f (%llu, %llu on time))",
				holding ? "holding (" : "", holding ? holding : "", holding ? "), " : "", dejudderExtras.dejudderExtraStats.extras,
				renderAheadPresenter.frames, renderAheadPresenter.restarts, renderAheadPresenter.late, renderAheadPresenter.dropped, renderAheadPresenter.held,
				renderAheadPresenter.intervalPresents, renderAheadPresenter.lead * 1000.0, renderAheadPresenter.targetLeadMs,
				RenderAhead::LevelName(gateLevel, DejudderPolicy::MaxExtras(ratio)).c_str(),
				RenderAhead::ResolveGate(dejudderGate, renderAheadActive) == RenderAhead::GateMode::Period ?
					"period" : "refresh",
				dejudderPolicy.dejudderLatencyFloor.Floor() * 1000.0, renderAheadPresenter.newFrameCost.P95(), renderAheadPresenter.extraCostOwn.P95(),
				renderAheadPresenter.extraCostExternal.P95(), renderAheadPresenter.estimateCost.P95(),
				renderAheadPresenter.creditFrames.load(std::memory_order_relaxed), ratio,
				renderAheadTimersReady ? "GPU timers" : "CPU timings", renderAheadPresenter.displayRate.hz,
				OpenLoopDisplayRate::Name(renderAheadPresenter.displayRate.source), renderAheadPresenter.displayRate.openLoop ? "open" : "closed",
				renderAheadPresenter.openLoopFrames, renderAheadPresenter.lateByPresent, renderAheadPresenter.schedule.EarlyCorrections(),
				renderAheadPresenter.schedule.Reanchors(), renderAheadPresenter.schedule.AnchorsLowered(), renderAheadPresenter.flushReanchors,
				renderAheadPresenter.shownCount[0] ? renderAheadPresenter.shownError[0] / renderAheadPresenter.shownCount[0] : 0.0, renderAheadPresenter.shownCount[0],
				renderAheadPresenter.shownOnTime[0], renderAheadPresenter.shownCount[1] ? renderAheadPresenter.shownError[1] / renderAheadPresenter.shownCount[1] : 0.0,
				renderAheadPresenter.shownCount[1], renderAheadPresenter.shownOnTime[1]);
		}

		bool PresentExtras(ExtrasHost& host, ExtrasState& dejudderExtras, const ExtrasFrame& f)
		{
			if (!host.calls)
				return false;

			// Bindings to Impl names so the body below stays verbatim and log lines stay byte-identical.
			auto& dejudderPolicy = host.dejudderPolicy;
			auto& dejudderReason = dejudderPolicy.dejudderReason;
			auto& diagLastHoldCode = dejudderPolicy.diagLastHoldCode;
			auto& dejudderEffectiveStrength = dejudderPolicy.dejudderEffectiveStrength;
			auto& dejudderEvenCycle = dejudderPolicy.dejudderEvenCycle;
			auto& dejudderLatencyFloor = dejudderPolicy.dejudderLatencyFloor;
			auto& dejudderCallLatency = dejudderPolicy.dejudderCallLatency;
			auto& dejudderLoggedFloor = dejudderPolicy.dejudderLoggedFloor;
			auto& dejudderPhaseGeneration = dejudderPolicy.dejudderPhaseGeneration;
			auto& dejudderPhaseLogPending = dejudderPolicy.dejudderPhaseLogPending;
			auto& dejudderPredictedVsync = dejudderPolicy.dejudderPredictedVsync;
			auto& dejudderPredictedGeneration = dejudderPolicy.dejudderPredictedGeneration;
			auto& dejudderPredictedSequence = dejudderPolicy.dejudderPredictedSequence;
			auto& interpolating = dejudderPolicy.interpolating;

			auto& renderAheadPresenter = host.renderAheadPresenter;
			auto& renderAheadSchedule = renderAheadPresenter.schedule;
			auto& renderAheadNewFrameCost = renderAheadPresenter.newFrameCost;
			auto& renderAheadExtraCostOwn = renderAheadPresenter.extraCostOwn;
			auto& renderAheadExtraCostExternal = renderAheadPresenter.extraCostExternal;
			auto& renderAheadEstimateCost = renderAheadPresenter.estimateCost;
			auto& renderAheadEpoch = renderAheadPresenter.epoch;
			auto& renderAheadDriftLateness = renderAheadPresenter.driftLateness;
			auto& renderAheadCredit = renderAheadPresenter.credit;
			auto& renderAheadChainDepth = renderAheadPresenter.chainDepth;
			auto& renderAheadLastMainInterval = renderAheadPresenter.lastMainInterval;

			auto& motionValidity = host.motionValidity;
			auto& dejudder = host.dejudder;
			const auto& dejudderEstimatedSequence = host.dejudderEstimatedSequence;
			const auto& dejudderEstimatedEpoch = host.dejudderEstimatedEpoch;
			const auto& dejudderLayoutValid = host.dejudderLayoutValid;
			const auto& dejudderLayoutRgb = host.dejudderLayoutRgb;
			auto& dejudderNextTextures = host.dejudderNextTextures;
			auto& dejudderAhead = host.dejudderAhead;
			const auto& renderAheadBudgetCredit = host.renderAheadBudgetCredit;

			const pl_log log = host.log;
			const pl_d3d11 d3d11 = host.d3d11;
			const pl_renderer renderer = host.renderer;
			const pl_swapchain swapchain = host.swapchain;
			IDXGISwapChain1* const vpOwnedSwapchain = host.vpOwnedSwapchain;
			const auto& activeSettings = host.activeSettings;
			auto& textures = host.textures;
			auto& presentationTelemetry = host.presentationTelemetry;
			auto& gpuFrameTimer = host.gpuFrameTimer;
			auto& passTimerHook = host.passTimerHook;
			ID3D11DeviceContext* const renderTimingContext = host.renderTimingContext;
			auto& outputEncodingRepair = host.outputEncodingRepair;
			auto& occlusionMaskHook = host.occlusionMaskHook;
			auto& grainToleranceParamSet = host.grainToleranceParamSet;
			auto& dejudderMaskParamSet = host.dejudderMaskParamSet;
			const bool temporalActive = host.temporalActive;
			auto* const outputMerger = host.outputMerger;
			auto& entrance = host.entrance;
			auto& entranceOnceAheadNow = host.entranceOnceAheadNow;
			const bool startupHoldBlack = host.startupHoldBlack;
			const double rebaseFade = host.rebaseFade;
			auto& resync = host.resync;
			auto& resyncDrive = host.resyncDrive;
			auto& frameInterpolator = host.frameInterpolator;
			auto& renderAheadExtraTimer = host.renderAheadExtraTimer;
			auto& renderAheadEstimateTimer = host.renderAheadEstimateTimer;
			const bool renderAheadTimersReady = host.renderAheadTimersReady;

			ExtrasHostCalls& calls = *host.calls;
			auto DejudderHold = [&calls](const char* why) { calls.DejudderHold(why); };
			auto UpdateRenderAheadBudget = [&calls](double captureRateHz) { calls.UpdateRenderAheadBudget(captureRateHz); };
			auto RenderAheadActive = [&calls]() { return calls.RenderAheadActive(); };
			auto RenderedWarpSettings = [&calls](int outputHeight) { return calls.RenderedWarpSettings(outputHeight); };
			auto SelectDejudderDisplayRate = [&calls]() { return calls.SelectDejudderDisplayRate(); };
			auto ResolveRenderAheadTimers = [&calls]() { calls.ResolveRenderAheadTimers(); };
			auto ResetRenderAheadGate = [&calls](const char* why) { calls.ResetRenderAheadGate(why); };
			auto OpenLoopGridVsync = [&calls](double displayHz, double now, double qpcFrequency) { return calls.OpenLoopGridVsync(displayHz, now, qpcFrequency); };
			auto UpdateRenderAheadGate = [&calls](double now, double sourcePeriod, double displayPeriod, bool late, int maxExtras) { return calls.UpdateRenderAheadGate(now, sourcePeriod, displayPeriod, late, maxExtras); };
			auto LogRenderAhead = [&](double ratio, int gateLevel, const char* holding = nullptr)
			{
				Vpv::Dejudder::LogRenderAhead(dejudderExtras, dejudderPolicy, renderAheadPresenter,
					activeSettings.dejudderGate, RenderAheadActive(), renderAheadTimersReady, ratio, gateLevel, holding);
			};
			auto EntranceOnce = [&calls]() { return calls.EntranceOnce(); };
			auto EntranceOnceExtra = [&calls](const pl_frame& image, uint64_t sourceSequence, uint64_t nextSourceSequence,
				bool interp, const ::Dejudder::Planes& a, ::Dejudder::Planes& processedA,
				::Dejudder::Planes& processedB, const pl_frame*& processedFrame)
			{
				return calls.EntranceOnceExtra(image, sourceSequence, nextSourceSequence, interp, a, processedA, processedB, processedFrame);
			};
			auto DrainEntranceBeforeRender = [&calls]() { calls.DrainEntranceBeforeRender(); };
			auto DitherVectorsSupported = [&calls]() { return calls.DitherVectorsSupported(); };
			auto RenderCrossfadeMix = [&calls](const pl_frame& from, const pl_frame& to, pl_frame& target,
				const pl_render_params& params, const pl_color_space* historyColor, double factor)
			{
				return calls.RenderCrossfadeMix(from, to, target, params, historyColor, factor);
			};
			auto EstimateAhead = [&calls](uint64_t sourceSequence, uint64_t nextSourceSequence, bool renderedPath,
				FrameGpuTimingScope* timing)
			{
				return calls.EstimateAhead(sourceSequence, nextSourceSequence, renderedPath, timing);
			};
			auto SyncFallback = [&calls](const FallbackCatalog::Decision& entry) { calls.SyncFallback(entry); };

			// ---- Verbatim body of PresentDejudderExtras ----
			// The prediction is only for the frame straight after (same generation);
			// every call consumes it, whatever happens below.
			// Only for the frame it was made for: a frame dropped by drift
			// correction in between must not be counted late (audit 6b-2b).
			const double predictedVsync = dejudderPredictedGeneration == f.frameGeneration &&
				dejudderPredictedSequence == f.sourceSequence ? dejudderPredictedVsync : 0.0;
			dejudderPredictedVsync = 0.0;
			if (dejudderReason != DejudderPolicy::Reason::Active || !dejudder)
				return false;
			// Render-ahead (Bug hunt 05 M3): a frame that holds for a reason that keeps the
			// timing (no estimate, MotionValidity, a cadence repeat, a composed picture) is
			// still planned, hold-only, and committed, so the chain, B's interval and the
			// queue credit continue; returning uncommitted broke the chain, which showed this
			// frame short by the lead and the next one long. Only real discontinuities (a
			// generation change, missing rates or timing, no swapchain) break it.
			// render_ahead off: every hold returns here at once, as before.
			// r6 fix 4: render-ahead only inside the delay budget (decided per frame from the rates).
			UpdateRenderAheadBudget(f.captureRateHz);
			const bool renderAheadOn = RenderAheadActive();
			const char* timedHold = nullptr;
			auto holdKeepsTiming = [&](const char* why)
			{
				if (!renderAheadOn)
				{
					DejudderHold(why);
					return true;   // return now (render_ahead off: unchanged)
				}
				if (!timedHold)
					timedHold = why;
				return false;
			};
			if (dejudderEstimatedSequence != f.sourceSequence && holdKeepsTiming("no motion estimate for this frame")) return false;
			if (dejudderExtras.dejudderNlsRetrySequence != 0 && dejudderExtras.dejudderNlsRetrySequence == f.sourceSequence &&
				holdKeepsTiming("NLS hook recovery redrew the frame")) return false;
			// MotionValidity: the estimate's epoch is over (a discontinuity since).
			if (!motionValidity.Current(dejudderEstimatedEpoch) && holdKeepsTiming(MotionValidity::Describe(motionValidity.LastCause()))) return false;
			// The rendered warp's settings for this output (general scaling).
			const int outputHeight = f.target && f.target->num_planes > 0 && f.target->planes[0].texture ?
				f.target->planes[0].texture->params.h : 0;
			const ::Dejudder::Settings renderedWarpSettings = RenderedWarpSettings(outputHeight);
			if (f.cadenceRepeat && holdKeepsTiming("cadence repeat")) return false;
			// A moved subtitle's composed picture: the rendered warp holds its
			// rectangles still (owner 2026-10-13); the source warp cannot.
			const bool sourcePlanes = f.image && f.image->num_planes == 2 && f.image->planes[0].texture == textures[0] &&
				f.image->planes[1].texture == textures[1];
			// An RGB source (no YCbCr planes): the rendered warp only, checked below.
			const bool rgbSource = dejudderLayoutValid && dejudderLayoutRgb;
			if (!f.image)
			{
				DejudderHold("composed source (subtitle moving)");
				return false;
			}
			if (!sourcePlanes && !rgbSource && !(dejudderExtras.dejudderSubtitleComposed && activeSettings.dejudderWarpRendered &&
				dejudderExtras.dejudderGeometryValid && dejudderExtras.dejudderGeometry.numHolds > 0) && holdKeepsTiming("composed source (subtitle moving)"))
				return false;
			// The composed picture exists only as the cache (checked again below, where
			// render_ahead off holds as before): render-ahead holds it with the timing kept.
			if (renderAheadOn && !sourcePlanes && !(interpolating ? nullptr : dejudderExtras.dejudderCache.Texture(f.sourceSequence)))
				holdKeepsTiming(rgbSource ? "RGB source without the rendered picture (the rendered warp only)" :
					"composed source without the cached picture");
			if (!host.flipPresentation)
			{
				DejudderHold("no flip presentation");
				return false;
			}
			LARGE_INTEGER frequency{};
			QueryPerformanceFrequency(&frequency);
			const double qpc = static_cast<double>(frequency.QuadPart);
			// The statistics' measured rate; with render_ahead on and no statistics, the
			// open-loop fallback (OpenLoopDisplayRate). render_ahead off: unchanged.
			renderAheadPresenter.displayRate = SelectDejudderDisplayRate();
			const double displayHz = renderAheadPresenter.displayRate.hz > 0.0 ? renderAheadPresenter.displayRate.hz :
				presentationTelemetry.Snapshot().measuredDisplayHz;
			// Which timing is missing, for the log (the same checks as before).
			const char* timingMissing = !CaptureRateBounds::Accepts(f.captureRateHz) ? "timing not known: source rate" :
				!DisplayRefreshBounds::Accepts(displayHz) ? "timing not known: display rate" :
				qpc <= 0.0 || f.callbackQpc <= 0 ? "timing not known: frame arrival" :
				f.mainCallQpc <= f.callbackQpc ? "timing not known: present before arrival" : nullptr;
			dejudderExtras.dejudderHeldNoDisplayRate = CaptureRateBounds::Accepts(f.captureRateHz) && !DisplayRefreshBounds::Accepts(displayHz);
			if (timingMissing)
			{
				DejudderHold(timingMissing);
				return false;
			}
			const double sourcePeriod = 1.0 / f.captureRateHz;
			const double displayPeriod = 1.0 / displayHz;
			// Render-ahead (render_ahead): A's refreshes planned from the film clock and its
			// extras queued back to back (no vblank waits); the gate per source period
			// (dejudder_gate period, the default with render_ahead) or per refresh.
			const bool queuedMode = RenderAheadActive();
			const RenderAhead::GateMode gateMode = RenderAhead::ResolveGate(activeSettings.dejudderGate, queuedMode);
			ResolveRenderAheadTimers();
			if (queuedMode && !gpuFrameTimer.Snapshot().supported)
				renderAheadNewFrameCost.Add(RenderAheadNow(), f.workMs);   // no GPU timer: the CPU work
			if (gateMode == RenderAhead::GateMode::Refresh)
			{
				renderAheadPresenter.l1 = false;
				// The extras end at the last extra's vblank, and only then does B's
				// whole pipeline start: it must fit in the refresh that follows.
				if (RenderAhead::RefreshGateHolds(f.workMs, displayPeriod))
				{
					DejudderHold("frame work too long for extras (the next frame could be late)");
					return false;
				}
			}
			CComPtr<IDXGISwapChain> native;
			native.Attach(pl_d3d11_swapchain_unwrap(swapchain));
			if (!native)
			{
				DejudderHold("no DXGI swapchain");
				return false;
			}
			// The output is for the per-refresh path's vblank waits only. Render-ahead queues
			// its extras back to back (FIFO back-pressure, no vblank waits), so it needs none:
			// on hybrid graphics (the panel on the iGPU, rendering on the discrete GPU)
			// GetContainingOutput fails with DXGI_ERROR_UNSUPPORTED, which used to hold every
			// frame "no output for vsync" (render-ahead follow-up 2026-10-17).
			// (render_ahead off retries every frame, as before; render-ahead asks once per swapchain.)
			if (dejudderExtras.dejudderOutputSwapchain != native.p && !(queuedMode && dejudderExtras.dejudderOutputFailedSwapchain == native.p))
			{
				dejudderExtras.dejudderOutput.Release();
				dejudderExtras.dejudderOutputSwapchain = nullptr;
				if (SUCCEEDED(native->GetContainingOutput(&dejudderExtras.dejudderOutput)) && dejudderExtras.dejudderOutput)
					dejudderExtras.dejudderOutputSwapchain = native.p;
				else
					dejudderExtras.dejudderOutputFailedSwapchain = native.p;
			}
			if (!dejudderExtras.dejudderOutput && !queuedMode)
			{
				DejudderHold("no output for vsync");
				return false;
			}
			UINT presentId = 0;
			if (FAILED(native->GetLastPresentCount(&presentId)) || presentId == 0)
			{
				DejudderHold("no present id");
				return false;
			}
			const UINT mainPresentId = presentId;   // A's own present
			if (dejudderExtras.dejudderIdsSwapchain != native.p)
			{
				// Present ids belong to one swapchain.
				dejudderExtras.dejudderExtraIds.clear();
				dejudderExtras.dejudderMainRefresh.clear();
				dejudderExtras.dejudderIdsSwapchain = native.p;
			}

			// Phase state: a new queue generation or source rate starts it again.
			const double ratio = DejudderPolicy::CadenceRatio(f.captureRateHz, displayHz);
			// R3 (resync_event on): the Indicator's FULL key in place of the queue generation (also a seek,
			// an in-place source change, a format change); off/shadow: the queue generation, as before.
			const uint64_t phaseGeneration = VpResync::ReaderGeneration(resync, f.frameGeneration, f.sourceSequence);
			// R3: a continuity slip or a timing event re-anchors the render-ahead schedule (the gate's
			// ladder is kept); a FULL is the generation change above.
			const VpIndicator::CounterMask renderAheadResync = VpResync::TakeIfDriving(resync, resyncDrive, VpResync::Reader::RenderAhead);
			if (dejudderPhaseGeneration != phaseGeneration ||
				std::abs(ratio - dejudderExtras.dejudderExtraStats.cadenceRatio) > 0.02)
			{
				dejudderPhaseGeneration = phaseGeneration;
				dejudderLatencyFloor.Reset();
				dejudderLatencyFloor.SetSourceRate(f.captureRateHz);
				dejudderLoggedFloor = 0.0;
				dejudderCallLatency = 0.0;
				dejudderPhaseLogPending = true;
				renderAheadSchedule.Break();   // render-ahead: a new anchor
			}
			dejudderExtras.dejudderExtraStats.cadenceRatio = ratio;
			const double callLatency = static_cast<double>(f.mainCallQpc - f.callbackQpc) / qpc;
			dejudderCallLatency = dejudderCallLatency <= 0.0 || std::abs(callLatency - dejudderCallLatency) > 0.05 ?
				callLatency : dejudderCallLatency + 0.05 * (callLatency - dejudderCallLatency);
			const double callbackA = static_cast<double>(f.callbackQpc) / qpc;
			// B needs its whole work plus a margin to make a vsync after it starts.
			// Re-audit 6b-2a: B's predicted present call already contains its whole
			// work (A's callback-to-call time), so PlanExtra only needs what follows
			// the call. The whole work is in the hold above.
			const double renderAllowance = f.renderMs / 1000.0 + 0.004;

			// Render-ahead (design Phase 1, steps 1-4): every refresh A owns, planned now.
			RenderAhead::FramePlan plan;
			bool openLoopFrame = false;        // render-ahead without statistics this frame
			int openLoopLateRefreshes = 0;     // the most any extra was late by its QPC return
			double plannedFirstVsync = 0.0;
			size_t planSlot = 0;
			std::vector<std::pair<uint32_t, double>> plannedIds;
			int gateLevel = 0;
			if (queuedMode)
			{
				// A new queue generation, cadence or render_ahead switched on: the gate and its
				// measurements start again (bug hunt 05 H3).
				if (renderAheadEpoch.Changed(phaseGeneration, ratio))
					ResetRenderAheadGate("new generation, cadence or render_ahead on");
				else if ((renderAheadResync & (VpIndicator::Bit(VpIndicator::kContinuity) | VpIndicator::Bit(VpIndicator::kTiming))) != 0)
				{
					renderAheadSchedule.Break();
					DiagnosticHistory::Global().RecordDecision("ra-reanchor",
						(renderAheadResync & VpIndicator::Bit(VpIndicator::kContinuity)) != 0 ? "resync: continuity slip" : "resync: timing",
						static_cast<int64_t>(f.sourceSequence));
				}
				// Presents outside the plan since the last one (a per-refresh timeline frame,
				// a drift-correction repeat): the chain starts again.
				if (renderAheadPresenter.expectedPresent != 0 && mainPresentId != renderAheadPresenter.expectedPresent)
					renderAheadSchedule.Break();
				// A backlog flush shed queued latency: a new anchor follows it, without the
				// lead's holds (Schedule::AnchorLatency withoutLead).
				const bool flushReanchor = dejudderExtras.renderAheadFlushReanchor.exchange(false, std::memory_order_acq_rel);
				if (flushReanchor)
				{
					renderAheadSchedule.Break();
					++renderAheadPresenter.flushReanchors;
					DiagnosticHistory::Global().RecordDecision("ra-reanchor", "after a backlog flush",
						static_cast<int64_t>(f.sourceSequence));
				}
				// Closed loop on DXGI's statistics, without waiting: the latest present on
				// screen and the vsync it showed at (none on some internal panels: open loop).
				double gridVsync = 0.0;
				bool late = false;
				bool closedLoop = false;
				bool openLoopDrift = false;   // late only by an open-loop drift correction (M5)
				// Only statistics the presentation telemetry accepts (a measured display rate):
				// composed presentation on hybrid graphics returns statistics the telemetry
				// rejects (no measured rate), and matched per present they disagree with any
				// consistent plan (shown - planned +1.2..+1.5 refreshes for interval-1
				// presents, -0.2..-0.3 after holds, 25-30% on time), so the closed loop
				// moved the chain wrongly every few frames and logged ~30% of frames late
				// (render-ahead follow-up 2026-10-17). Those displays run the open loop.
				DXGI_FRAME_STATISTICS statistics{};
				if (!renderAheadPresenter.displayRate.openLoop && SUCCEEDED(native->GetFrameStatistics(&statistics)) &&
					statistics.SyncQPCTime.QuadPart > 0)
				{
					closedLoop = true;
					gridVsync = static_cast<double>(statistics.SyncQPCTime.QuadPart) / qpc;
					const double shownAt = gridVsync - static_cast<double>(static_cast<int32_t>(
						statistics.SyncRefreshCount - statistics.PresentRefreshCount)) * displayPeriod;
					late = renderAheadSchedule.Observe(statistics.PresentCount, shownAt, displayPeriod);
					if (renderAheadSchedule.ObservedCount() != renderAheadPresenter.observedSeen)
					{
						renderAheadPresenter.observedSeen = renderAheadSchedule.ObservedCount();
						const uint32_t id = renderAheadSchedule.LastObservedId();
						const auto known = std::find_if(renderAheadPresenter.presentIntervals.begin(), renderAheadPresenter.presentIntervals.end(),
							[id](const std::pair<uint32_t, int>& p) { return p.first == id; });
						if (known != renderAheadPresenter.presentIntervals.end())
						{
							const int kind = known->second > 1 ? 1 : 0;
							const double e = renderAheadSchedule.LastObservedError();
							renderAheadPresenter.shownError[kind] += e;
							++renderAheadPresenter.shownCount[kind];
							if (std::abs(e) <= 0.5) ++renderAheadPresenter.shownOnTime[kind];
						}
					}
					if (std::find(dejudderExtras.dejudderExtraIds.begin(), dejudderExtras.dejudderExtraIds.end(), statistics.PresentCount) == dejudderExtras.dejudderExtraIds.end() &&
						(dejudderExtras.dejudderMainRefresh.empty() || dejudderExtras.dejudderMainRefresh.back().first != statistics.PresentCount))
					{
						dejudderExtras.dejudderMainRefresh.emplace_back(statistics.PresentCount, statistics.PresentRefreshCount);
						if (dejudderExtras.dejudderMainRefresh.size() > 64)
							dejudderExtras.dejudderMainRefresh.pop_front();
					}
				}
				const double now = static_cast<double>(PerformanceCounterNow()) / qpc;
				// Open loop (no statistics): no closed-loop correction. DWM's vblank, when it
				// composes at this rate, puts an anchor on the vsync grid; lateness comes from
				// the presents' QPC return times (the last frame's, below).
				openLoopFrame = !closedLoop;
				if (openLoopFrame)
				{
					++renderAheadPresenter.openLoopFrames;
					gridVsync = OpenLoopGridVsync(displayHz, now, qpc);
					late = renderAheadPresenter.openLoopLate;
					openLoopDrift = renderAheadPresenter.openLoopLate;
				}
				renderAheadPresenter.openLoopLate = false;
				bool restart = false, behind = false;
				// The furthest A's present can land: the presents in flight (frame latency),
				// each at most a frame's refreshes, its own frame's and the lead.
				const int queueDepth = vpOwnedSwapchain ? static_cast<int>(renderAheadChainDepth.latency) : 2;
				const double maxAhead = RenderAhead::PlannedLead(sourcePeriod, displayPeriod) +
					(queueDepth + 1) * std::ceil(ratio - RenderAhead::kTolerance) * displayPeriod;
				plannedFirstVsync = renderAheadSchedule.FirstVsync(now, displayPeriod, gridVsync, restart, behind, maxAhead);
				// A new anchor: the old chain's pending presents no longer apply (else a
				// correction already made by the restart is made again when they show).
				if (restart)
					renderAheadSchedule.Break();
				// An anchor taken in a backlog (start-up) places every chained frame long before
				// its time: replaced, sustained only, on this frame's own vsync (a re-anchor, not
				// late; Schedule::AnchorTooLate).
				if (!restart && renderAheadSchedule.AnchorTooLate(plannedFirstVsync, callbackA,
					RenderAhead::PlannedLead(sourcePeriod, displayPeriod), sourcePeriod))
				{
					DebugLog::Log("Render-ahead: anchor latency %.2f ms too late (this frame's %.2f ms): re-anchored on its own refresh",
						renderAheadSchedule.AnchorLatency(false, plannedFirstVsync, callbackA) * 1000.0,
						(plannedFirstVsync - callbackA) * 1000.0);
					DiagnosticHistory::Global().RecordDecision("ra-reanchor", "anchor latency too late",
						static_cast<int64_t>(f.sourceSequence), 0,
						(plannedFirstVsync - callbackA) * 1000.0);
					renderAheadSchedule.Break();
					restart = true;
				}
				late = late || behind;
				if (late)
				{
					++renderAheadPresenter.late;
					++dejudderExtras.dejudderExtraStats.nextFrameLate;
				}
				if (restart)
					++renderAheadPresenter.restarts;
				renderAheadPresenter.lead = RenderAhead::PlannedLead(sourcePeriod, displayPeriod);
				RendererExtension::NotifyLifecycle(RendererExtension::Lifecycle::QueueDepth, host.host, nullptr, nullptr, nullptr, nullptr, 0, nullptr, sourcePeriod > 0.0 ? renderAheadPresenter.lead / sourcePeriod : -1.0);
				// L, the constant that fixes which refresh is phase 0, without the lead: fixed
				// at the anchor (a restart's first vsync - film time) and kept by chained
				// frames (Schedule::AnchorLatency, Bug hunt 05 H1: a floor over chained samples
				// ratcheted upward on every inexact cadence). The floor window only reports it.
				const double anchorLatency = renderAheadSchedule.AnchorLatency(restart, plannedFirstVsync, callbackA,
					flushReanchor && restart ? renderAheadPresenter.lead : 0.0);
				dejudderLatencyFloor.Add(anchorLatency);
				const int maxExtras = DejudderPolicy::MaxExtras(ratio);
				// A hold-only frame leaves lateness's drop for the next frame with extras.
				int drops = timedHold ? 0 : renderAheadSchedule.TakeLateDrops() + (behind ? 1 : 0);
				if (gateMode == RenderAhead::GateMode::Period)
				{
					// Lateness for the ladder: a late observed present or a chain that fell
					// behind at once; an open-loop drift correction only when sustained
					// (SustainedLateness, bug hunt 05 M5). A re-anchor is never late (M2).
					const double gateNow = RenderAheadNow();
					// Open loop, a chain found behind is inferred the same way (the plan's grid
					// is not the display's): sustained only too. On the laptop panel single
					// composition hiccups (costs at 60% of the budget) stepped the ladder down
					// every 5-10 s, faster than it may climb, to hold (render-ahead on-screen
					// 2026-10-17). The frame still drops its extra; closed loop is unchanged.
					const bool openLoopBehind = behind && openLoopFrame;
					const bool gateLate = (behind && !openLoopBehind) || (late && !openLoopDrift && !openLoopBehind) ||
						((openLoopDrift || openLoopBehind) && renderAheadDriftLateness.Note(gateNow));
					gateLevel = UpdateRenderAheadGate(gateNow, sourcePeriod, displayPeriod, gateLate, maxExtras);
					drops += gateLevel >= RenderAhead::HoldLevel(maxExtras) ? 16 : RenderAhead::DropsAt(gateLevel);
				}
				RenderAhead::FrameInput in;
				in.filmTime = callbackA;
				in.sourcePeriod = sourcePeriod;
				in.displayPeriod = displayPeriod;
				in.firstVsync = plannedFirstVsync;
				in.latencyFloor = anchorLatency;
				in.lead = renderAheadPresenter.lead;
				in.strength = timedHold ? 0.0 : dejudderEffectiveStrength;   // a hold-only plan
				in.evenCycle = dejudderEvenCycle;
				in.maxExtras = maxExtras;
				in.dropExtras = drops;
				in.arrivalJitter = renderAheadSchedule.NoteArrival(callbackA, sourcePeriod);   // H2's tolerance
				plan = RenderAhead::PlanFrame(in);
				if (!plan.valid)
				{
					DejudderHold("render-ahead: no plan (timing)");
					return false;
				}
				plannedIds.emplace_back(mainPresentId, plannedFirstVsync);
				renderAheadPresenter.presentIntervals.emplace_back(mainPresentId, renderAheadLastMainInterval);
				renderAheadLastMainInterval = 1;
				renderAheadPresenter.targetLeadMs = f.swapEndQpc > 0 ?
					(plannedFirstVsync - static_cast<double>(f.swapEndQpc) / qpc) * 1000.0 : 0.0;
				++renderAheadPresenter.frames;
				renderAheadPresenter.dropped += static_cast<unsigned long long>(plan.dropped);
				renderAheadPresenter.held += static_cast<unsigned long long>(plan.slots.size()) -
					static_cast<unsigned long long>(plan.rendered);
			}
			if (timedHold)
			{
				// Render-ahead hold-only frame (M3): A shown for all its refreshes; the plan is
				// committed as an extras frame's would be (B's interval and refresh), the
				// credit kept. A plan that would need A presented again (more holds in a row
				// than DXGI's interval allows) cannot be made without rendering: the chain ends.
				DejudderHold(timedHold);
				if (queuedMode && plan.valid && plan.rendered == 0 && RenderAhead::IntervalsFit(plan))
				{
					renderAheadSchedule.Commit(plan.nextFirstVsync, plannedIds);
					renderAheadPresenter.mainInterval = plan.nextInterval;
					UINT lastPresent = 0;
					renderAheadPresenter.expectedPresent = SUCCEEDED(native->GetLastPresentCount(&lastPresent)) ? lastPresent + 1 : 0;
					renderAheadPresenter.committed = true;
					LogRenderAhead(ratio, gateLevel);
				}
				return false;
			}

			::Dejudder::Planes a;
			a.luma = textures[0];
			a.chroma = textures[1];
			a.chromaShiftX = 1;
			a.chromaShiftY = f.lossless422 ? 0 : 1;
			std::vector<const pl_hook*> hooks;
			// S1 covers every present: with the grain in alpha the hook stays for
			// the extras, without the mask (first refresh only).
			if (grainToleranceParamSet)
				occlusionMaskHook.DropMask();
			for (int i = 0; i < f.params->num_hooks; ++i)
				if (f.params->hooks[i] != f.ledMeasurementHook &&
					(f.params->hooks[i] != occlusionMaskHook.Get() || grainToleranceParamSet) &&
					f.params->hooks[i] != dejudderExtras.dejudderCache.Hook())
					hooks.push_back(f.params->hooks[i]);
			// The mask is for a source frame's first refresh only (6c).
			if (dejudderMaskParamSet)
			{
				for (const pl_hook* hook : hooks)
					CustomHookChain::SetHookParameter(hook, "occlusion_mask", 0.0);
				dejudderMaskParamSet = false;
			}
			// Without the LED measurement the extras' list may no longer contain a
			// hook that triggers the PRE_OUTPUT encoding bug; then the repair would
			// encode a second time (audit 6b-2a).
			const auto repair = std::find(hooks.begin(), hooks.end(), outputEncodingRepair.Get());
			if (repair != hooks.end())
			{
				std::vector<const pl_hook*> withoutRepair(hooks);
				withoutRepair.erase(withoutRepair.begin() + (repair - hooks.begin()));
				const pl_render_errors errors = pl_renderer_get_errors(renderer);
				if (!OutputEncodingRepair::Needed(withoutRepair, &errors,
					f.target->lut ? f.target->lut_type : PL_LUT_UNKNOWN))
					hooks = std::move(withoutRepair);
			}
			pl_render_params params = *f.params;
			params.hooks = hooks.empty() ? nullptr : hooks.data();
			params.num_hooks = static_cast<int>(hooks.size());
			// 6d: with A's picture cached, the extras render it warped, with
			// only the OUTPUT-stage hooks (no repair unless still needed).
			// Interpolation renders the source path only (the interpolated planes go through
			// the whole chain); a composed or RGB picture holds (owner).
			const pl_tex cached = interpolating ? nullptr : dejudderExtras.dejudderCache.Texture(f.sourceSequence);
			// The composed picture exists only as the cache: without it the source
			// warp would show the subtitle where it was.
			if (!sourcePlanes && !cached)
			{
				DejudderHold(rgbSource ? "RGB source without the rendered picture (the rendered warp only)" :
					"composed source without the cached picture");
				return false;
			}
			std::vector<const pl_hook*> outputHooks;
			pl_render_params renderedParams = DejudderRendered::OutputOnly(params, hooks, outputHooks);
			{
				const auto repairHook = std::find(outputHooks.begin(), outputHooks.end(), outputEncodingRepair.Get());
				if (repairHook != outputHooks.end())
				{
					std::vector<const pl_hook*> withoutRepair(outputHooks);
					withoutRepair.erase(withoutRepair.begin() + (repairHook - outputHooks.begin()));
					const pl_render_errors errors = pl_renderer_get_errors(renderer);
					// The cached picture renders without the target LUT (Bug hunt 2026-10-17).
					if (!OutputEncodingRepair::Needed(withoutRepair, &errors, PL_LUT_UNKNOWN))
						outputHooks = std::move(withoutRepair);
					renderedParams.hooks = outputHooks.empty() ? nullptr : outputHooks.data();
					renderedParams.num_hooks = static_cast<int>(outputHooks.size());
				}
			}
			if (cached && !dejudderExtras.dejudderRenderedWarp)
				dejudderExtras.dejudderRenderedWarp = std::make_unique<DejudderRendered::Warp>(log, d3d11->gpu);
			const float strength = dejudderEffectiveStrength;
			const float black[] = { 0.0f, 0.0f, 0.0f };

			bool spent = false;
			bool haveFirstVsync = queuedMode;   // render-ahead: A's refresh is planned, not waited for
			double phi0 = queuedMode ? plan.phi0 : 0.0;
			int extras = 0;
			// What the warps read: the matcher's own until (B, C) is estimated
			// ahead, then the copy of (A, B)'s.
			const VpMotion::P3FieldSet liveSet = calls.LiveFieldSet();
			pl_tex warpField = liveSet.field, warpTrust = liveSet.frameTrust, warpSub = liveSet.subField;
			pl_tex warpCamera = liveSet.camera;
			pl_tex warpMask = liveSet.pixelMask;
			pl_tex warpVectorOk = liveSet.vectorOk;
			bool aheadTried = false;
			float previousShift = 0.0f;   // A, shown at shift 0 (delta 8 R1)
			DejudderPolicy::Stop stop = DejudderPolicy::Stop::None;
			const char* failure = nullptr;
			// The ladder's L1 (render-ahead gate): the sigma-delta takes the de-judder's vectors
			// on the extras instead of its own search (dither_vectors own otherwise kept).
			const bool ditherOwnOnExtras = activeSettings.ditherVectorsOwn && !renderAheadPresenter.l1;
			for (;;)
			{
				DejudderPolicy::ExtraDecision decision;
				double loggedFloor = dejudderLatencyFloor.Floor();
				UINT syncInterval = 1;
				float plannedShift = 0.0f;
				if (queuedMode)
				{
					// Render-ahead: the next planned extra; the holds before it are its
					// SyncInterval. Back to back: FIFO back-pressure, no vblank waits, and B is
					// never waited for (its place is fixed by the film clock).
					while (planSlot < plan.slots.size() && !plan.slots[planSlot].render)
						++planSlot;
					if (planSlot >= plan.slots.size())
					{
						stop = DejudderPolicy::Stop::PastNextFrame;
						break;
					}
					const RenderAhead::Slot& slot = plan.slots[planSlot++];
					decision.present = true;
					decision.targetVsync = slot.vsync;
					decision.delta = slot.delta;
					syncInterval = static_cast<UINT>(std::max(1, std::min(slot.interval, RenderAhead::kMaxSyncInterval)));
					plannedShift = slot.shift;
				}
				else
				{
					if ((*f.nextFrameArrived)()) { stop = DejudderPolicy::Stop::NextFrameArrived; break; }
					// Wait until the previous picture (A or the last extra) is on screen.
					double previousVsync = 0.0;
					bool shown = false;
					DXGI_FRAME_STATISTICS statistics{};
					for (int wait = 0; wait < 3 && !shown; ++wait)
					{
						const double vblankWaitStart = PassTimerHook::Hook::NowMs();
						const bool vblankFailed = FAILED(dejudderExtras.dejudderOutput->WaitForVBlank());
						passTimerHook.AddCpu("de-judder extra: vblank wait", PassTimers::Kind::CpuWait,
							PassTimerHook::Hook::NowMs() - vblankWaitStart);
						if (vblankFailed)
							break;
						spent = true;
						if (FAILED(native->GetFrameStatistics(&statistics)))
							break;
						if (statistics.PresentCount >= presentId)
						{
							previousVsync = static_cast<double>(statistics.SyncQPCTime.QuadPart) / qpc -
								static_cast<double>(static_cast<int32_t>(statistics.SyncRefreshCount -
									statistics.PresentRefreshCount)) * displayPeriod;
							shown = true;
						}
					}
					if (!shown) { failure = "picture not seen on screen"; break; }
					if (!haveFirstVsync)
					{
						haveFirstVsync = true;
						// This frame was predicted (by the last frame's extras) to show
						// at dejudderPredictedVsync; later means the extras cost it a
						// refresh.
						if (predictedVsync > 0.0 && previousVsync > predictedVsync + 0.5 * displayPeriod)
						{
							++dejudderExtras.dejudderExtraStats.nextFrameLate;
							DejudderHold("the frame after extras showed a refresh late");
						}
						if (statistics.PresentCount == mainPresentId)
						{
							dejudderExtras.dejudderMainRefresh.emplace_back(mainPresentId, statistics.PresentRefreshCount);
							if (dejudderExtras.dejudderMainRefresh.size() > 64)
								dejudderExtras.dejudderMainRefresh.pop_front();
						}
						dejudderLatencyFloor.Add(previousVsync - callbackA);
						// A pipeline hiccup shows as the floor moving by more than a
						// refresh (planner 6b-2a).
						const double floor = dejudderLatencyFloor.Floor();
						if (dejudderLoggedFloor > 0.0 && std::abs(floor - dejudderLoggedFloor) > displayPeriod)
							DebugLog::Log("De-judder: latency floor moved %.2f -> %.2f ms (more than one refresh)",
								dejudderLoggedFloor * 1000.0, floor * 1000.0);
						if (dejudderLoggedFloor <= 0.0 || std::abs(floor - dejudderLoggedFloor) > displayPeriod)
							dejudderLoggedFloor = floor;
					}
					DejudderPolicy::ExtraInput in;
					in.previousVsync = previousVsync;
					in.displayPeriod = displayPeriod;
					in.sourcePeriod = sourcePeriod;
					in.callbackA = callbackA;
					in.latencyFloor = dejudderLatencyFloor.Floor();
					in.callToCallback = dejudderCallLatency;
					in.renderAllowance = renderAllowance;
					in.nextFrameArrived = (*f.nextFrameArrived)();
					in.extrasDone = extras;
					in.maxExtras = DejudderPolicy::MaxExtras(ratio);
					if (extras == 0)
						phi0 = (previousVsync - callbackA - in.latencyFloor) / sourcePeriod;
					decision = DejudderPolicy::PlanExtra(in);
					loggedFloor = in.latencyFloor;
					if (!decision.present)
					{
						stop = decision.stop;
						if (stop == DejudderPolicy::Stop::NextFrameNear && extras > 0)
						{
							dejudderPredictedVsync = decision.targetVsync;
							dejudderPredictedGeneration = f.frameGeneration;
							dejudderPredictedSequence = f.nextSourceSequence;
						}
						break;
					}
					// Too late to render for that vsync: B's refresh must stay free.
					if (static_cast<double>(PerformanceCounterNow()) / qpc +
						std::max(f.renderMs, dejudderExtras.dejudderExtraStats.lastMs) / 1000.0 + 0.004 > decision.targetVsync)
					{
						failure = "too late for the refresh";
						break;
					}

				}

				const auto extraStart = SteadyClock::now();
				// Delta 10 (general): on an even cadence of N refreshes the k-th
				// extra is at exactly k/N.
				// Interpolation: the picture at the refresh's true phase (strength 1).
				// Render-ahead: the planner's shift (the same rules, from the planned phase).
				const float shift = queuedMode ? plannedShift : strength * static_cast<float>(dejudderEvenCycle > 0 ?
					static_cast<double>(extras + 1) / dejudderEvenCycle : decision.delta);
				// Render-ahead gate: X, an extra's GPU time (warp, vectors and render), on every
				// other extra, with libplacebo's pass timers suspended so that disjoint
				// intervals never nest (as the frame timer does).
				const bool timeExtra = queuedMode && renderAheadTimersReady && (++dejudderExtras.renderAheadTimerOrdinal % 2) == 0;
				LibplaceboTimerSuspensionScope extraTimerSuspension(d3d11, timeExtra);
				FrameGpuTimingScope extraTiming(renderAheadExtraTimer,
					timeExtra && extraTimerSuspension.Active() ? renderTimingContext : nullptr, f.frameGeneration, f.sourceSequence);
				extraTiming.BeginStage(AlphaD3D11FrameGpuTimer::Stage::CoreRender);
				pl_frame image = *f.image;
				const pl_render_params* extraParams = &params;
				bool renderedCrop = false;
				pl_frame extraFrom{};       // hunt 3 F3: the crossfade's path being left for this extra
				bool extraFade = false;
				pl_tex warpedPicture = nullptr;
				pl_tex warpedDisplacement = nullptr;   // planner D2: per pixel, for the sigma-delta
				// With M6 the picture moves per pixel: the sigma-delta follows the
				// warp's own per-pixel displacement (the step since the previous
				// present) instead of block vectors.
				const bool pixelVectors = warpMask != nullptr && !ditherOwnOnExtras && DitherVectorsSupported() &&
					shift > 0.0f;
				if (cached && dejudderExtras.dejudderRenderedWarp)
				{
					if (!dejudderExtras.dejudderRenderedWarp->Run(cached, warpField, warpTrust,
						dejudder->GetSettings().minTrustedShare, shift, dejudderExtras.dejudderGeometry, warpedPicture, warpSub,
						&renderedWarpSettings, warpCamera, warpMask, warpVectorOk,
						pixelVectors ? &warpedDisplacement : nullptr, pixelVectors ? (shift - previousShift) / shift : 1.0f))
					{
						failure = "rendered warp failed";
						break;
					}
					image = DejudderRendered::Frame(warpedPicture, dejudderExtras.dejudderCache.Repr(), dejudderExtras.dejudderCache.Color());
					extraParams = &renderedParams;
					renderedCrop = true;
				}
				else
				{
					::Dejudder::Planes warped;
					// B3 (pipeline_entrance_once): the warp / interpolation reads the processed A (and B);
					// the picture then needs no entrance stages. Else today's path below.
					::Dejudder::Planes processedA, processedB;
					const pl_frame* processedFrame = nullptr;
					if (EntranceOnce())
					{
						entranceOnceAheadNow = false;
						(void)EntranceOnceExtra(image, f.sourceSequence, f.nextSourceSequence, interpolating, a, processedA,
							processedB, processedFrame);
						if (entranceOnceAheadNow)
						{
							// B3 (i) estimated (B, C) ahead: this and the remaining extras read (A, B)'s snapshot,
							// as after the rendered warp's own estimate ahead.
							aheadTried = true;
							calls.RefreshSnapshotFields(warpField, warpTrust, warpSub, warpCamera, warpMask, warpVectorOk);
						}
					}
					if (interpolating)
					{
						// Stage A: A and B (the next frame's planes, uploaded for the estimate)
						// at the phase, with the field the warps read; B's occlusion mask when
						// it is current.
						::Dejudder::Planes b = a;
						b.luma = dejudderNextTextures[0];
						b.chroma = dejudderNextTextures[1];
						Interpolator::Motion motion;
						motion.field = warpField;
						motion.frameTrust = warpTrust;
						motion.subField = warpSub;
						motion.minTrustedShare = dejudder->GetSettings().minTrustedShare;
						for (int slot = 0; slot < 2; ++slot)
							if (dejudderExtras.dejudderMaskSequence[slot] == f.nextSourceSequence && f.nextSourceSequence != 0 &&
								motionValidity.Current(dejudderExtras.dejudderMaskEpoch[slot]))
								motion.occlusionB = calls.Occlusion(slot);
						if (!frameInterpolator)
							frameInterpolator = std::make_unique<Interpolator::FrameInterpolator>(log, d3d11->gpu);
						frameInterpolator->SetGrain(activeSettings.interpolationGrain);
						bool interpOk = false;
						if (processedFrame)
						{
							interpOk = frameInterpolator->Run(processedA, processedB, motion, shift, activeSettings.interpolationFallback, warped,
								a.luma->params.w, a.luma->params.h);
						}
						else
						{
							// POD1-H1: pass live SkeletonMemo + (sourceSequence, nextSourceSequence) for shared kBandMemo acquisition.
							const Vpv::Skeleton::FrameMemo fm = Vpv::Skeleton::MemoForHost(host.host, f.sourceSequence);
							interpOk = frameInterpolator->Run(a, b, motion, shift, activeSettings.interpolationFallback, warped,
								0, 0, fm.memo.get(), f.sourceSequence, f.nextSourceSequence);
							const std::string& bmWhy = frameInterpolator->LastBandMemoFallback();
							calls.SyncFallback(bmWhy.empty()
								? FallbackCatalog::Released("product.interpolator")
								: FallbackCatalog::ProductFallback("product.interpolator", bmWhy));
						}
						if (!interpOk)
						{
							if (++dejudderExtras.interpolationFailures == 1 || dejudderExtras.interpolationFailures % 2400 == 0)
								DebugLog::Log("Interpolation: failed (%s; %llu failures)", frameInterpolator->LastError().c_str(),
									dejudderExtras.interpolationFailures);
							failure = "interpolation failed";
							break;
						}
						++dejudderExtras.interpolatedPresents;
					}
					else if (!(processedFrame ? dejudder->WarpProcessed(processedA, shift, warped, true) :
						dejudder->Warp(a, shift, warped, true)))
					{
						failure = "warp failed";
						break;
					}
					if (processedFrame)
					{
						image = *processedFrame;   // the processed geometry (re-based crop), warped planes
						entrance.OnceWorking();
					}
					image.planes[0].texture = warped.luma;
					image.planes[1].texture = warped.chroma;
					// Hunt 3 F3: during a crossfade the extra is mixed like the main present (its own
					// path being left from the native warped planes).
					const pl_frame warpedNative = image;
					// Pipeline skeleton (B3/R2): the extra runs the same entrance stage list as the main
					// frame (re-base, VP deband per frame, the detail stages' decision of its source
					// frame), with the extra's own module slots.
					if (!processedFrame)
					{
						EntranceSegment::Request rq;
						rq.path = EntranceSegment::Path::Extra;
						rq.sequence = f.sourceSequence;
						(void)entrance.Run(rq, image);
					}
					// Hunt 3 F3: the extra's path being left (the crossfade runs on every present of the
					// fade, not only on A's), with the same stages (S1 CrossfadeFrom).
					if (!processedFrame && rebaseFade < 1.0 && !temporalActive)
					{
						extraFrom = warpedNative;
						EntranceSegment::Request rq;
						rq.path = EntranceSegment::Path::CrossfadeFrom;
						rq.sequence = f.sourceSequence;
						extraFade = entrance.Run(rq, extraFrom);
					}
				}
				DrainEntranceBeforeRender();   // entrance_gpu_drain: after the extra's last entrance run
				// Delta 8 R1: the sigma-delta takes the motion since the previous
				// present (A or the last extra: the shift step) from the de-judder's
				// field instead of searching. The guard restores the hooks (own
				// search, VPVEC unbound) on every way out of this extra.
				struct VectorGuard
				{
					std::vector<const pl_hook*> hooks;
					// Back to the own search (also before the fallback render of A
					// after a failed extra: audit 2026-10-14).
					void Release()
					{
						for (const pl_hook* hook : hooks)
						{
							CustomHookChain::SetHookParameter(hook, "vector_source", 0.0);
							CustomHookChain::SetHookParameter(hook, "vector_pixel", 0.0);
							SetSigmaDeltaVectors(hook, nullptr);
							SetSigmaDeltaPixelVectors(hook, nullptr);
						}
						hooks.clear();
					}
					~VectorGuard() { Release(); }
				} vectorGuard;
				{
					const bool patch = DitherVectorsSupported();
					const bool use = DejudderRendered::UseExternalVectors(ditherOwnOnExtras, patch,
						warpField != nullptr, dejudderExtras.dejudderGeometryValid);
					pl_tex vectors = nullptr;
					if (use)
					{
						const int w = cached ? cached->params.w :
							static_cast<int>(std::lround(std::fabs(dejudderExtras.dejudderGeometry.targetCrop.x1 - dejudderExtras.dejudderGeometry.targetCrop.x0)));
						const int h = cached ? cached->params.h :
							static_cast<int>(std::lround(std::fabs(dejudderExtras.dejudderGeometry.targetCrop.y1 - dejudderExtras.dejudderGeometry.targetCrop.y0)));
						if (!dejudderExtras.dejudderVectorField)
							dejudderExtras.dejudderVectorField = std::make_unique<DejudderRendered::VectorField>(log, d3d11->gpu);
						if (!dejudderExtras.dejudderVectorField->Run(warpField, warpTrust,
							dejudder->GetSettings().minTrustedShare, shift - previousShift, dejudderExtras.dejudderGeometry, w, h, vectors,
							warpSub, &renderedWarpSettings, warpCamera))
							vectors = nullptr;
					}
					const pl_render_params& listParams = *extraParams;
					for (int i = 0; vectors && i < listParams.num_hooks; ++i)
					{
						const pl_hook* hook = listParams.hooks[i];
						if (CustomHookChain::SetHookParameter(hook, "vector_source", 1.0))
						{
							// Counted (and restored by the guard) only when VPVEC was
							// accepted (audit 2026-10-14).
							if (SetSigmaDeltaVectors(hook, vectors))
							{
								vectorGuard.hooks.push_back(hook);
								// D2: the per-pixel displacement the rendered warp applied.
								if (warpedDisplacement && SetSigmaDeltaPixelVectors(hook, warpedDisplacement))
									CustomHookChain::SetHookParameter(hook, "vector_pixel", 1.0);
							}
							else
								CustomHookChain::SetHookParameter(hook, "vector_source", 0.0);
						}
					}
					const int state = !use ? (ditherOwnOnExtras ? 0 : patch ? 1 : 2) :
						(vectors && !vectorGuard.hooks.empty() ? 3 : 4);
					if (state == 3)
						++dejudderExtras.ditherVectorsExternal;
					if (state != dejudderExtras.ditherVectorsLogged && (state == 3 || state == 2 || state == 4))
					{
						dejudderExtras.ditherVectorsLogged = state;
						static const char* const names[] = { "own (dither_vectors own)", "own", "own (this libplacebo has no fork patch 0004)",
							"de-judder vectors on the extras", "own (vector field or the sigma-delta hook not available)" };
						DebugLog::Log("Sigma-delta vectors on de-judder extras: %s (%llu extras so far)", names[state],
							dejudderExtras.ditherVectorsExternal);
					}
				}
				previousShift = shift;
				const bool externalThisExtra = !vectorGuard.hooks.empty();
				// R1 timing: this extra's sigma-delta GPU time.
				DitherExtraPassTotals ditherTotals;
				pl_render_params timedParams = *extraParams;
				timedParams.info_callback = DitherExtraPassCallback;
				timedParams.info_priv = &ditherTotals;
				passTimerHook.Install(timedParams);   // efficiency timers T1/T5 (chains the R1 callback)
				extraParams = &timedParams;
				// output_segments_merged: the extras draw through the same S2/S3 segment objects (merged
				// for these draws only; the fallback draw of A keeps the separate list).
				const pl_render_errors extraErrors = pl_renderer_get_errors(renderer);
				OutputSegments::Merger::Scope mergedExtra(outputMerger, timedParams, &extraErrors);
				pl_frame target = *f.target;
				if (renderedCrop)
				{
					target.crop = dejudderExtras.dejudderGeometry.targetCrop;   // the cache's rounded rectangle: 1:1
					// Bug hunt 2026-10-17: the cached picture was taken at PRE_OUTPUT,
					// after libplacebo applied the target's (normalized) calibration LUT
					// in colour management; with the LUT on this target it was applied a
					// second time. OutputOnly clears only the parameters' LUT.
					target.lut = nullptr;
					target.lut_type = PL_LUT_UNKNOWN;
				}
				bool rendered = false;
				bool submitted = false;
				double blockMs = 0.0;
				if (vpOwnedSwapchain)
				{
					CComPtr<ID3D11Texture2D> backbuffer;
					if (FAILED(vpOwnedSwapchain->GetBuffer(0, IID_PPV_ARGS(&backbuffer))) || !backbuffer)
					{
						failure = "no backbuffer";
						break;
					}
					struct pl_d3d11_wrap_params wrapParams{};
					wrapParams.tex = static_cast<ID3D11Resource*>(backbuffer.p);
					pl_tex fbo = pl_d3d11_wrap(d3d11->gpu, &wrapParams);
					if (!fbo || !fbo->params.renderable)
					{
						pl_tex_destroy(d3d11->gpu, &fbo);
						failure = "backbuffer not renderable";
						break;
					}
					target.planes[0].texture = fbo;
					pl_frame_clear(d3d11->gpu, &target, black);
					rendered = (extraFade && RenderCrossfadeMix(extraFrom, image, target, *extraParams, nullptr, rebaseFade)) ||
						pl_render_image(renderer, &image, &target, extraParams);
					if (rendered && startupHoldBlack) pl_frame_clear(d3d11->gpu, &target, black);   // h3-latch K3
					pl_gpu_flush(d3d11->gpu);
					pl_tex_destroy(d3d11->gpu, &fbo);
					backbuffer.Release();
					if (rendered)
					{
						// X ends before the present: a Present blocked by FIFO back-pressure
						// would put the wait into the extra's GPU interval.
						extraTiming.EndRender();
						const int64_t presentStart = PerformanceCounterNow();
						// Render-ahead: the holds before this extra are its interval.
						submitted = SUCCEEDED(vpOwnedSwapchain->Present(syncInterval, 0));
						blockMs = static_cast<double>(PerformanceCounterNow() - presentStart) * 1000.0 / qpc;
					}
				}
				else
				{
					struct pl_swapchain_frame frame{};
					if (!pl_swapchain_start_frame(swapchain, &frame))
					{
						failure = "no swapchain frame";
						break;
					}
					target.planes[0].texture = frame.fbo;
					pl_frame_clear(d3d11->gpu, &target, black);
					rendered = (extraFade && RenderCrossfadeMix(extraFrom, image, target, *extraParams, nullptr, rebaseFade)) ||
						pl_render_image(renderer, &image, &target, extraParams);
					if (rendered && startupHoldBlack) pl_frame_clear(d3d11->gpu, &target, black);   // h3-latch K3
					// An acquired frame must be submitted: on a failed extra show A
					// itself again, never a black or partial picture - with the
					// sigma-delta's own search (A did not move: no extra's vectors).
					if (!rendered)
					{
						vectorGuard.Release();
						// Bug hunt 2026-10-17: A itself with A's own target (its crop and
						// calibration LUT), not the cached picture's.
						pl_frame fallbackTarget = *f.target;
						fallbackTarget.planes[0].texture = frame.fbo;
						pl_frame_clear(d3d11->gpu, &fallbackTarget, black);
						pl_render_image(renderer, f.image, &fallbackTarget, &params);
					}
					// X ends before the present (FIFO back-pressure blocks in it, not GPU work).
					extraTiming.EndRender();
					const int64_t presentStart = PerformanceCounterNow();
					submitted = pl_swapchain_submit_frame(swapchain);
					if (submitted)
					{
						// libplacebo's swap is Present(1, 0); render-ahead holds before this
						// extra present with their interval instead.
						if (syncInterval > 1)
						{
							// Hunt 2 05-renderahead L5: libplacebo's D3D11 swap_buffers
							// (7.360.1 and 7.377) is only Present(1, 0) plus, on failure, an
							// error log and the device-lost mark; no frame-latency wait and
							// no present counter. The native present must therefore report
							// its own failure.
							const HRESULT intervalResult = native->Present(syncInterval, 0);
							if (FAILED(intervalResult))
							{
								submitted = false;
								DebugLog::Log("Render-ahead: extra present with interval %u failed: 0x%08lX",
									static_cast<unsigned>(syncInterval), static_cast<unsigned long>(intervalResult));
							}
						}
						else
							pl_swapchain_swap_buffers(swapchain);
					}
					blockMs = static_cast<double>(PerformanceCounterNow() - presentStart) * 1000.0 / qpc;
				}
				if (!rendered) ++dejudderExtras.dejudderExtraStats.renderFailures;
				if (!rendered || !submitted)
				{
					failure = rendered ? "extra present failed" : "extra render failed";
					break;
				}
				++extras;
				++dejudderExtras.dejudderExtraStats.extras;
				extraTiming.Accept(++dejudderExtras.renderAheadTimerSerial);
				if (timeExtra)
					renderAheadPresenter.NoteExtraTimerMode(dejudderExtras.renderAheadTimerSerial, externalThisExtra);   // the 64 newest kept
				extraTimerSuspension.Resume();
				if (ditherTotals.sigmaNs > 0.0)
				{
					double& average = externalThisExtra ? dejudderExtras.dejudderExtraStats.ditherMsExternal : dejudderExtras.dejudderExtraStats.ditherMsOwn;
					const double ms = ditherTotals.sigmaNs / 1e6;
					average = average > 0.0 ? average + 0.1 * (ms - average) : ms;
					dejudderExtras.dejudderExtraStats.ditherLastExternal = externalThisExtra;
				}
				dejudderExtras.dejudderExtraStats.lastMs = std::chrono::duration<double, std::milli>(
					SteadyClock::now() - extraStart).count();
				if (queuedMode && !renderAheadTimersReady)   // no GPU timer: the CPU time without the FIFO wait
					(externalThisExtra ? renderAheadExtraCostExternal : renderAheadExtraCostOwn).Add(RenderAheadNow(),
						std::max(0.0, dejudderExtras.dejudderExtraStats.lastMs - blockMs));
				// Render-ahead: a full FIFO is the back-pressure that paces it, not a stall.
				if (!queuedMode && blockMs > 500.0 * displayPeriod)
					++dejudderExtras.dejudderExtraStats.swapchainFull;
				native->GetLastPresentCount(&presentId);
				if (queuedMode)
				{
					plannedIds.emplace_back(presentId, decision.targetVsync);
					renderAheadPresenter.presentIntervals.emplace_back(presentId, static_cast<int>(syncInterval));
					while (renderAheadPresenter.presentIntervals.size() > 128)
						renderAheadPresenter.presentIntervals.pop_front();
				}
				// Open loop: a present that returned more than a refresh after its planned
				// vsync was not shown on time (OpenLoopDisplayRate::LateRefreshes).
				if (queuedMode && openLoopFrame)
				{
					const int lateBy = OpenLoopDisplayRate::LateRefreshes(static_cast<double>(PerformanceCounterNow()) / qpc,
						decision.targetVsync, displayPeriod);
					if (lateBy > 0)
					{
						++renderAheadPresenter.lateByPresent;
						openLoopLateRefreshes = std::max(openLoopLateRefreshes, lateBy);
					}
				}
				dejudderExtras.dejudderExtraIds.push_back(presentId);
				if (dejudderExtras.dejudderExtraIds.size() > 256)
					dejudderExtras.dejudderExtraIds.pop_front();
				static GeneralRate::LogInterval extrasLog = GeneralRate::LogInterval::ForFrames(240);   // fixed-constant batch B7 2026-10-18: seconds, not frames
				if (dejudderExtras.dejudderExtraStats.extras <= 200 || extrasLog.Due())
					DebugLog::Log("De-judder extra: source=%llu k=%d phi0=%.3f delta=%.3f shift=%.3f ms=%.2f block_ms=%.2f latency_floor_ms=%.2f extras=%llu%s%s",
						static_cast<unsigned long long>(f.sourceSequence), extras, phi0, decision.delta, shift,
						dejudderExtras.dejudderExtraStats.lastMs, blockMs, loggedFloor * 1000.0, dejudderExtras.dejudderExtraStats.extras,
						interpolating ? " interpolated=1" : "", queuedMode ? (syncInterval > 1 ? " queued=1 after_hold=1" : " queued=1") : "");
				// Render-ahead renders ahead of its refresh by design (lateness comes from
				// the statistics instead).
				if (!queuedMode && static_cast<double>(PerformanceCounterNow()) / qpc > decision.targetVsync)
				{
					++dejudderExtras.dejudderExtraStats.late;
					failure = "extra finished after its refresh";
					break;
				}
				// Estimate ahead in this extra's spare refresh (the next loop
				// waits for its vblank first).
				if (!aheadTried)
				{
					aheadTried = true;
					// Render-ahead gate: E, the estimate ahead's GPU time.
					const bool timeEstimate = queuedMode && renderAheadTimersReady;
					LibplaceboTimerSuspensionScope estimateTimerSuspension(d3d11, timeEstimate);
					FrameGpuTimingScope estimateTiming(renderAheadEstimateTimer,
						timeEstimate && estimateTimerSuspension.Active() ? renderTimingContext : nullptr, f.frameGeneration, f.sourceSequence);
					const auto estimateStart = SteadyClock::now();
					const bool estimatedAhead = EstimateAhead(f.sourceSequence, f.nextSourceSequence,
						cached != nullptr && dejudderExtras.dejudderRenderedWarp != nullptr, &estimateTiming);
					if (estimatedAhead && dejudderAhead.LastReason() == ::Dejudder::AheadSchedule::Reason::Ahead)
						estimateTiming.Accept(++dejudderExtras.renderAheadTimerSerial);
					else
						estimateTiming.Reject();
					estimateTimerSuspension.Resume();
					if (queuedMode && estimatedAhead && !renderAheadTimersReady)
						renderAheadEstimateCost.Add(RenderAheadNow(),
							std::chrono::duration<double, std::milli>(SteadyClock::now() - estimateStart).count());
					if (estimatedAhead)
					{
						// Planner Q7: with R-A, the roll-off starts on a fast pan's
						// first frame (the next pair's strength, rate-limited).
						calls.RefreshSnapshotFields(warpField, warpTrust, warpSub, warpCamera, warpMask, warpVectorOk);
					}
				}
			}
			// B's own first present: the sigma-delta's vectors from A's field
			// (from A's last picture, at previousShift, to B at the full vector).
			dejudderExtras.dejudderNextVectorsSequence = 0;
			dejudderExtras.dejudderNextVectorsFrom = 0;   // R3
			dejudderExtras.ditherNextGate.Clear();
			// Hunt 2 09-L4: not after a failed extra. The fallback showed A at shift 0 (or
			// nothing was presented), so vectors from previousShift would carry from the
			// wrong place; the sigma-delta's own search covers B (vector validity rule).
			if (!failure && !activeSettings.ditherVectorsOwn && DitherVectorsSupported() &&
				warpField && warpTrust &&
				dejudderExtras.dejudderGeometryValid && f.nextSourceSequence != 0)
			{
				const int w = cached ? cached->params.w :
					static_cast<int>(std::lround(std::fabs(dejudderExtras.dejudderGeometry.targetCrop.x1 - dejudderExtras.dejudderGeometry.targetCrop.x0)));
				const int h = cached ? cached->params.h :
					static_cast<int>(std::lround(std::fabs(dejudderExtras.dejudderGeometry.targetCrop.y1 - dejudderExtras.dejudderGeometry.targetCrop.y0)));
				if (!dejudderExtras.dejudderNextVectorField)
					dejudderExtras.dejudderNextVectorField = std::make_unique<DejudderRendered::VectorField>(log, d3d11->gpu);
				pl_tex next = nullptr;
				if (dejudderExtras.dejudderNextVectorField->Run(warpField, warpTrust, dejudder->GetSettings().minTrustedShare, previousShift,
						dejudderExtras.dejudderGeometry, w, h, next, warpSub, &renderedWarpSettings, warpCamera, true) && next)
				{
					dejudderExtras.dejudderNextVectors = next;
					dejudderExtras.dejudderNextVectorsSequence = f.nextSourceSequence;
					dejudderExtras.dejudderNextVectorsFrom = f.sourceSequence;   // R3: A, for the stamp check at B
					dejudderExtras.ditherNextGate.Bind(f.sourceSequence, f.nextSourceSequence, dejudderEstimatedEpoch);
					dejudderExtras.dejudderNextVectorsGeometry = dejudderExtras.dejudderGeometry;
					dejudderExtras.dejudderNextVectorsW = w;
					dejudderExtras.dejudderNextVectorsH = h;
				}
			}
			dejudderExtras.dejudderExtraStats.lastExtrasPerFrame = extras;
			if (extras > 0)
				++dejudderExtras.dejudderExtraStats.sourceFrames;
			SyncFallback(dejudderExtras.motionConsumerFailures.Note(GetTickCount64(), failure));   // D3: warp / interpolation failures listed
			if (failure)
				DejudderHold(failure);
			else if (!queuedMode && extras == 0 && stop != DejudderPolicy::Stop::NextFrameArrived)
				DejudderHold(DejudderPolicy::Name(stop));
			else if (queuedMode && extras == 0)
				// Render-ahead planned no extra for this frame: say why (every hold has a reason).
				DejudderHold(plan.dropped > 0 ? (gateLevel >= RenderAhead::HoldLevel(DejudderPolicy::MaxExtras(ratio)) ?
					"render-ahead gate: hold (extras dropped)" : "render-ahead: extras dropped (gate or late)") :
					plan.slots.empty() ? "render-ahead: no refresh after the frame's own (cadence)" :
					"render-ahead: every refresh after the frame's own is a hold (phase)");
			// Render-ahead: the plan is queued; B lands at the refresh after A's last one
			// (with the holds after the last extra as its interval). A failed extra ends the
			// chain instead (the caller breaks it): B then shows at its own time.
			if (queuedMode && !failure)
			{
				renderAheadSchedule.Commit(plan.nextFirstVsync, plannedIds);
				if (openLoopLateRefreshes > 0)
				{
					// As a statistics-observed late present: the chain moves, the next frame
					// drops one extra, and the next frame counts it late and feeds the gate.
					// The first after a new anchor re-anchors only (bug hunt 05 M2).
					renderAheadPresenter.openLoopLate = renderAheadSchedule.NoteLate(openLoopLateRefreshes, displayPeriod);
				}
				renderAheadPresenter.mainInterval = plan.nextInterval;
				UINT lastPresent = 0;
				renderAheadPresenter.expectedPresent = SUCCEEDED(native->GetLastPresentCount(&lastPresent)) ? lastPresent + 1 : 0;
				renderAheadPresenter.committed = true;
				// render_ahead_queue_credit: floor(lead / P_s), latched while the cadence holds
				// (bug hunt 05 M4).
				renderAheadPresenter.creditFrames.store(activeSettings.renderAheadQueueCredit ?
					renderAheadCredit.CommitFrames(renderAheadBudgetCredit, ratio) : 0, std::memory_order_release);   // r6 fix 4
				LogRenderAhead(ratio, gateLevel);
			}
			// Report-a-problem: a frame with extras ends the hold; the next hold is a new decision.
			if (extras > 0 && !failure)
				diagLastHoldCode = 0;
			// After a second of frames, so the latency floor (and phi0) is settled.
			// Hunt 2 13-timing L3: a second of frames at the source rate (was 24 frames).
			if (dejudderPhaseLogPending && haveFirstVsync && dejudderLatencyFloor.Count() >=
				AlphaQueuePolicy::FramesForSeconds(1.0, f.captureRateHz, 24, activeSettings.queueTimingFixes)) // GENERAL-REF: AlphaQueuePolicy::FramesForSeconds fallback when captureRateHz unmeasured
			{
				dejudderPhaseLogPending = false;
				DebugLog::Log("De-judder phase: cadence %.3f refreshes per source frame (source %.3f Hz, display %.3f Hz), phi0 %.3f, latency floor %.2f ms, this frame %d extras, stop %s",
					ratio, f.captureRateHz, displayHz, phi0, dejudderLatencyFloor.Floor() * 1000.0, extras,
					failure ? failure : DejudderPolicy::Name(stop));
			}
			return spent || extras > 0;
		}
	}
}
