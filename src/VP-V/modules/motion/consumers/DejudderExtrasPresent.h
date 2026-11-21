// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
#pragma once

// CC series A (A1-A3): the de-judder extras' state, frame description, host interface and compiled body
// (Vpv::Dejudder::PresentExtras, moved verbatim from LibplaceboVideoRenderer.cpp::Impl::PresentDejudderExtras).
// Impl holds ONE member `Vpv::Dejudder::ExtrasState dejudderExtras;` and forwards through
// `Vpv::HostAccess::PresentDejudderExtras(*this, extrasFrame)` in src/VP-V/host/LvrHostAccess.inl.
//
// Threading: the render thread only, as before, apart from renderAheadFlushReanchor (atomic: set at the render loop's
// backlog flush, taken by the next frame's extras).

#include <VP-V/core/FallbackCatalog.h>
#include <VP-V/modules/control/config/VpvRendererFields.h>
#include <VP-V/modules/input/signal/ResyncExtension.h>
#include <VP-V/modules/motion/consumers/DejudderAhead.h>
#include <VP-V/modules/motion/consumers/DejudderState.h>
#include <VP-V/modules/motion/consumers/FrameInterpolator.h>
#include <VP-V/modules/motion/consumers/OcclusionMaskHook.h>
#include <VP-V/modules/motion/consumers/RenderedWarp.h>
#include <VP-V/modules/motion/estimation/EstimateState.h>
#include <VP-V/modules/motion/estimation/MotionValidity.h>
#include <VP-V/modules/output/dither/OutputEncodingRepair.h>
#include <VP-V/modules/output/presentation/OpenLoopDisplayRate.h>
#include <VP-V/modules/output/presentation/RenderAheadPolicy.h>
#include <VP-V/segments/EntranceSegment.h>
#include <VP-V/segments/OutputSegmentsMerge.h>
#include <vprenderer/AlphaD3D11FrameGpuTimer.h>
#include <vprenderer/AlphaPresentationTelemetry.h>
#include <vprenderer/PassTimerHook.h>

#include <atlbase.h>
#include <dxgi.h>
#include <dxgi1_2.h>
#include <libplacebo/d3d11.h>
#include <libplacebo/gpu.h>
#include <libplacebo/renderer.h>
#include <libplacebo/swapchain.h>

#include <atomic>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <string>
#include <utility>

namespace Vpv
{
	namespace Dejudder
	{
		// 6b-2 extra presents: the counters.
		struct DejudderExtraStats
		{
			unsigned long long sourceFrames = 0;    // source frames that got at least one extra
			unsigned long long extras = 0;
			unsigned long long holds = 0;           // source frames held (no extras) while active
			unsigned long long swapchainFull = 0;   // an extra's Present blocked > half a refresh
			unsigned long long late = 0;            // an extra finished after its vsync
			unsigned long long nextFrameLate = 0;   // the frame after extras showed a refresh late
			unsigned long long renderFailures = 0;
			double lastMs = 0.0;                    // warp + render + present of the last extra
			// Delta 8 R1: the sigma-delta's GPU time per extra (running averages),
			// with the de-judder's vectors and with its own search.
			double ditherMsExternal = 0.0;
			double ditherMsOwn = 0.0;
			bool ditherLastExternal = false;
			int lastExtrasPerFrame = 0;
			double cadenceRatio = 0.0;
			std::string lastHold;
		};

		struct ExtrasState
		{
			// ---- 6b-2 extra presents: phase state, the vsync output and the counters ----
			DejudderExtraStats dejudderExtraStats;
			CComPtr<IDXGIOutput> dejudderOutput;
			IDXGISwapChain* dejudderOutputSwapchain = nullptr;   // identity only, not owned
			IDXGISwapChain* dejudderOutputFailedSwapchain = nullptr;   // GetContainingOutput failed (identity only)
			// Present ids of recent extras and the refresh each main present (A)
			// showed on, so presentation telemetry sees what it would without extras.
			std::deque<uint32_t> dejudderExtraIds;
			std::deque<std::pair<uint32_t, uint32_t>> dejudderMainRefresh;
			IDXGISwapChain* dejudderIdsSwapchain = nullptr;   // identity only, not owned

			// ---- render-ahead (docs/VP-render-ahead-design.md): the extras' own counters, not the Presenter's ----
			unsigned long long renderAheadTimerOrdinal = 0, renderAheadTimerSerial = 0;
			unsigned long long renderAheadLogTick = 0;
			bool renderAheadSwapchainLogged = false;
			// Render-ahead: a backlog flush dropped queued frames to shed latency; the chain
			// re-anchors on the next frame (else its fixed L holds the shed latency back in
			// and the queue overflows again: a flush every few frames on the laptop panel).
			std::atomic<bool> renderAheadFlushReanchor{ false };

			// ---- 6c: the occlusion mask. Estimate(A, B) writes B's mask into the slot
			// not holding the mask for the frame now showing; dejudderMaskSequence
			// says which source frame each slot is for (0 = none/used). ----
			uint64_t dejudderMaskSequence[2] = { 0, 0 };
			uint64_t dejudderMaskEpoch[2] = { 0, 0 };   // the motion epoch each slot's mask was made in
			// VP's 50/60p interpolation (Stage A): the extras' machinery with the interpolator in place of the warp.
			unsigned long long interpolatedPresents = 0, interpolationFailures = 0;

			// ---- Stage 6d: A's PRE_OUTPUT picture for the extras, and its warp ----
			DejudderRendered::Cache dejudderCache;
			std::unique_ptr<DejudderRendered::Warp> dejudderRenderedWarp;
			DejudderRendered::Geometry dejudderGeometry;
			bool dejudderGeometryValid = false;   // this frame's geometry (both warp paths; delta 8 R1)
			// Hunt 2 10-layout M1: the source frame whose draw the NLS hook recovery redrew; its
			// extras hold (their geometry, cache and NLS map were the failed draw's).
			uint64_t dejudderNlsRetrySequence = 0;
			// Owner 2026-10-13: this frame's picture has a moved subtitle (cut/paste);
			// the rendered warp's extras run, holding its rectangles still.
			bool dejudderSubtitleComposed = false;
			// Delta 8 R1: the sigma-delta's vectors on the extras (VPVEC).
			std::unique_ptr<DejudderRendered::VectorField> dejudderVectorField;
			unsigned long long ditherVectorsExternal = 0;
			int ditherVectorsLogged = -1;

			// ---- motion.consumers (group D audit D3): failed warp / interpolation passes, visible while recent ----
			FallbackCatalog::MotionConsumerFailures motionConsumerFailures;
			// The de-judder held this frame because the display rate is not known (presentation entry).
			bool dejudderHeldNoDisplayRate = false;

			// ---- Efficiency (owner 2026-10-14): the sigma-delta's vectors for B's own
			// first present, made from A's field as A's extras end (the motion from
			// A's last picture to B), so B's refresh skips the sigma-delta's own
			// search (~6 ms on the laptop). For this sequence and geometry only. ----
			std::unique_ptr<DejudderRendered::VectorField> dejudderNextVectorField;
			pl_tex dejudderNextVectors = nullptr;
			uint64_t dejudderNextVectorsSequence = 0;
			uint64_t dejudderNextVectorsFrom = 0;   // R3: A's sequence (resync_event on: A and B must share their stamps)
			MotionValidity::ConsumerGate ditherNextGate;   // the next-frame VPVEC's pair and epoch (step 1 E)
			DejudderRendered::Geometry dejudderNextVectorsGeometry;
			int dejudderNextVectorsW = 0, dejudderNextVectorsH = 0;
			uint64_t ditherVectorsNextUsed = 0;
			bool ditherVectorsNextLogged = false;
		};

		// Per-frame inputs to PresentExtras (formerly Impl::DejudderExtrasFrame).
		struct ExtrasFrame
		{
			const pl_frame* image = nullptr;           // A as rendered
			const pl_frame* target = nullptr;          // A's target; the FBO is replaced per extra
			const pl_render_params* params = nullptr;  // A's parameters and hooks
			const pl_hook* ledMeasurementHook = nullptr;
			bool lossless422 = false;
			bool cadenceRepeat = false;
			uint64_t frameGeneration = 0;
			uint64_t sourceSequence = 0;
			uint64_t nextSourceSequence = 0;   // B, if known
			int64_t callbackQpc = 0;   // A's capture callback
			int64_t mainCallQpc = 0;   // A's present call
			double captureRateHz = 0.0;
			// A's whole processing time, without waits: RenderLoop's pre-render
			// work (look-ahead, conversion wait excluded) plus RenderLocked from
			// entry to A's Present returning. B needs about as long from the
			// moment it starts, so this is what must fit in one refresh.
			double workMs = 0.0;
			double renderMs = 0.0;   // A's render call: the after-call allowance for B
			const std::function<bool()>* nextFrameArrived = nullptr;
			int64_t swapEndQpc = 0;   // render-ahead: when A's Present returned (the target lead's base)
		};

		// Caller-bounded D3D11 timestamp-disjoint scope shared between PresentExtras and EstimateAhead
		// (matches AlphaFrameGpuTimingScope in LibplaceboVideoRenderer.cpp).
		class FrameGpuTimingScope
		{
		public:
			FrameGpuTimingScope(AlphaD3D11FrameGpuTimer& timer,
				ID3D11DeviceContext* context, uint64_t generation,
				uint64_t sourceSequence)
				: m_timer(timer), m_context(context),
				m_issued(timer.BeginRender(context, generation, sourceSequence))
			{
			}

			~FrameGpuTimingScope()
			{
				Reject();
			}

			FrameGpuTimingScope(const FrameGpuTimingScope&) = delete;
			FrameGpuTimingScope& operator=(const FrameGpuTimingScope&) = delete;

			void EndRender()
			{
				if (m_issued && !m_ended)
				{
					m_timer.EndRender(m_context);
					m_ended = true;
				}
			}

			bool BeginStage(AlphaD3D11FrameGpuTimer::Stage stage)
			{
				return m_issued && m_timer.BeginStage(m_context, stage);
			}

			void EndStage()
			{
				if (m_issued)
				{
					m_timer.EndStage(m_context);
					m_context->Flush();
				}
			}

			void Accept(uint64_t submissionSerial)
			{
				if (!m_issued || m_finalized)
					return;
				EndRender();
				m_timer.SetFrameOutcome(true, submissionSerial);
				m_finalized = true;
			}

			void Reject()
			{
				if (!m_issued || m_finalized)
					return;
				EndRender();
				m_timer.SetFrameOutcome(false);
				m_finalized = true;
			}

		private:
			AlphaD3D11FrameGpuTimer& m_timer;
			ID3D11DeviceContext* m_context = nullptr;
			bool m_issued = false;
			bool m_ended = false;
			bool m_finalized = false;
		};

		// Forwarding callback interface implemented by Vpv::HostAccess::PresentDejudderExtras.
		struct ExtrasHostCalls
		{
			virtual ~ExtrasHostCalls() = default;
			virtual void DejudderHold(const char* why) = 0;
			virtual void UpdateRenderAheadBudget(double captureRateHz) = 0;
			virtual bool RenderAheadActive() const = 0;
			virtual ::Dejudder::Settings RenderedWarpSettings(int outputHeight) const = 0;
			virtual OpenLoopDisplayRate::Choice SelectDejudderDisplayRate() = 0;
			virtual void ResolveRenderAheadTimers() = 0;
			virtual void ResetRenderAheadGate(const char* why) = 0;
			virtual double OpenLoopGridVsync(double displayHz, double now, double qpcFrequency) const = 0;
			virtual int UpdateRenderAheadGate(double now, double sourcePeriod, double displayPeriod, bool late, int maxExtras) = 0;
			virtual bool EntranceOnce() const = 0;
			virtual bool EntranceOnceExtra(const pl_frame& image, uint64_t sourceSequence, uint64_t nextSourceSequence,
				bool interpolating, const ::Dejudder::Planes& a, ::Dejudder::Planes& processedA,
				::Dejudder::Planes& processedB, const pl_frame*& processedFrame) = 0;
			virtual void DrainEntranceBeforeRender() = 0;
			virtual bool DitherVectorsSupported() const = 0;
			virtual bool RenderCrossfadeMix(const pl_frame& from, const pl_frame& to, pl_frame& target,
				const pl_render_params& params, const pl_color_space* historyColor, double factor) = 0;
			virtual bool EstimateAhead(uint64_t sourceSequence, uint64_t nextSourceSequence, bool renderedPath,
				FrameGpuTimingScope* timing) = 0;
			virtual void SyncFallback(const FallbackCatalog::Decision& entry) = 0;
			virtual VpMotion::P3FieldSet LiveFieldSet() const = 0;
			virtual void RefreshSnapshotFields(pl_tex& warpField, pl_tex& warpTrust, pl_tex& warpSub,
				pl_tex& warpCamera, pl_tex& warpMask, pl_tex& warpVectorOk) const = 0;
			virtual pl_tex Occlusion(int slot) const = 0;
		};

		// Host view built once per call by the forwarding adapter in LvrHostAccess.inl.
		struct ExtrasHost
		{
			const void* host = nullptr;
			pl_log log = nullptr;
			pl_d3d11 d3d11 = nullptr;
			pl_renderer renderer = nullptr;
			pl_swapchain swapchain = nullptr;
			IDXGISwapChain1* vpOwnedSwapchain = nullptr;
			const VpvRenderer::Fields& activeSettings;
			bool flipPresentation = false;
			pl_tex (&textures)[2];
			AlphaPresentationTelemetry& presentationTelemetry;
			AlphaD3D11FrameGpuTimer& gpuFrameTimer;
			PassTimerHook::Hook& passTimerHook;
			ID3D11DeviceContext* renderTimingContext = nullptr;
			OutputEncodingRepair::Hook& outputEncodingRepair;
			OcclusionMask::Hook& occlusionMaskHook;
			bool& grainToleranceParamSet;
			bool& dejudderMaskParamSet;
			bool temporalActive = false;
			OutputSegments::Merger* outputMerger = nullptr;
			EntranceSegment::Segment& entrance;
			bool& entranceOnceAheadNow;
			bool startupHoldBlack = false;
			double rebaseFade = 1.0;
			VpResync::Shadow& resync;
			VpResync::Drive& resyncDrive;
			std::unique_ptr<Interpolator::FrameInterpolator>& frameInterpolator;
			AlphaD3D11FrameGpuTimer& renderAheadExtraTimer;
			AlphaD3D11FrameGpuTimer& renderAheadEstimateTimer;
			bool renderAheadTimersReady = false;

			// Whole VP-V state objects (not per-field aliases).
			VpMotion::DejudderPolicyState& dejudderPolicy;
			RenderAhead::Presenter& renderAheadPresenter;
			MotionValidity::State& motionValidity;
			std::unique_ptr<::Dejudder::MotionWarp>& dejudder;
			const uint64_t& dejudderEstimatedSequence;
			const uint64_t& dejudderEstimatedEpoch;
			const bool& dejudderLayoutValid;
			const bool& dejudderLayoutRgb;
			pl_tex (&dejudderNextTextures)[2];
			::Dejudder::AheadSchedule& dejudderAhead;
			const size_t& renderAheadBudgetCredit;

			ExtrasHostCalls* calls = nullptr;
		};

		// One line per 5 s while render-ahead queues extras (formerly Impl::LogRenderAhead).
		void LogRenderAhead(ExtrasState& dejudderExtras, const VpMotion::DejudderPolicyState& dejudderPolicy,
			const RenderAhead::Presenter& renderAheadPresenter, const std::string& dejudderGate,
			bool renderAheadActive, bool renderAheadTimersReady, double ratio, int gateLevel,
			const char* holding = nullptr);

		bool PresentExtras(ExtrasHost& h, ExtrasState& s, const ExtrasFrame& f);
	}

	namespace HostAccess
	{
		template<class HostImpl>
		bool PresentDejudderExtras(HostImpl& i, const Dejudder::ExtrasFrame& f);
	}
}
