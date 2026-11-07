// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
#pragma once

// 70b G1: the skeleton's render-thread GPU dispatcher (VpSkeleton::IGpuDispatcher) for the VP-owned GPU products
// P10 / P11 / P12. It owns: its own pl_dispatch on the renderer's pl_gpu (captured at GpuCreated), one
// ProductTextures per GPU node (ring + live, allocated at Configure only, never per frame, layout = declaration), the
// tap ring (R16 tap + extended plane per in-flight frame, uploaded in row bands), the product kernels
// (LaneGpuProducts.h) and the deferred releases (old textures of a size change are destroyed at the next GPU-legal
// phase, FlushReleases). No readback: every consumer is a GPU reader. Render thread only.
//
// Lane form (Dispatch, one chunk per call): P10 = [tap band 0..n-1] [extend] [kernel step 0..4] (StepsFor); P11 / P12 =
// their one kernel step (the frame's tap and extend are made inline first when P11 needs them and P10 did not make
// them). Live form (DispatchLive): the same steps in the same order, at once. Off / Original: never constructed.

#include <VP-V/core/AnalysisLane.h>
#include <VP-V/modules/input/source_analysis/lane/LaneGpuProducts.h>

#include <libplacebo/dispatch.h>
#include <libplacebo/gpu.h>

#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace Vpv
{
	namespace Skeleton
	{
		class SkeletonGpuDispatcher : public VpSkeleton::IGpuDispatcher
		{
		public:
			static constexpr size_t kTapRing = 3;   // live + up to two lane frames in flight

			struct Counters
			{
				uint64_t laneChunks = 0;
				uint64_t liveFills = 0;
				uint64_t failures = 0;
				uint64_t tapBands = 0;
				uint64_t extends = 0;
				uint64_t allocations = 0;        // product texture sets allocated
				uint64_t releasesQueued = 0;     // texture sets handed to the deferred release
				uint64_t releasesDone = 0;       // texture sets destroyed by FlushReleases / Release
			};

			// `gpu` is the renderer's (not owned). `hostReadable`: product and tap textures readable (tests only).
			explicit SkeletonGpuDispatcher(pl_gpu gpu, bool hostReadable = false);
			~SkeletonGpuDispatcher() override;
			SkeletonGpuDispatcher(const SkeletonGpuDispatcher&) = delete;
			SkeletonGpuDispatcher& operator=(const SkeletonGpuDispatcher&) = delete;

			bool Valid() const { return m_gpu != nullptr && m_dispatch != nullptr; }
			pl_gpu Gpu() const { return m_gpu; }
			pl_dispatch Dispatcher() const { return m_dispatch; }

			// Setup: the lane whose ReadOf / SourceOf the live form reads (not owned), and the product kernels.
			void SetLane(const VpSkeleton::AnalysisLane* lane) { m_lane = lane; }
			// Tests without a lane: where the live form finds the source view of a frame.
			void SetLiveSourceForTesting(std::function<bool(uint64_t, VpSkeleton::SourceFrameView*)> f) { m_liveSource = std::move(f); }
			void AddKernel(std::unique_ptr<VpLaneProducts::IProductKernel> kernel);
			VpLaneProducts::IProductKernel* KernelOf(VpSkeleton::ProductId id) const;

			// Render thread, GPU-legal phase, after the lane's Configure: (re)allocate the textures of every GPU node of
			// `graph` that has a kernel, at `dims`. Unchanged graph and size: nothing happens. A changed one moves the old
			// textures to the deferred release. False when a product could not be allocated (it is then unavailable,
			// shown through Fallbacks()); the others are kept.
			bool Configure(const VpSkeleton::Graph& graph, const VpSkeleton::Registry& reg, const VpSkeleton::TierPlan& plan,
				VpSkeleton::Dims dims, uint32_t settingsGen = 0);
			// Render thread, GPU-legal phase: destroy the texture sets queued by a size or graph change.
			void FlushReleases();
			// Everything now (GpuReleased / teardown).
			void Release();

			// IGpuDispatcher (render thread).
			bool Dispatch(const VpSkeleton::LaneJobContext& job, uint32_t chunk, uint32_t chunkCount) override;
			bool DispatchLive(const VpSkeleton::Node& node, uint64_t seq, int liveIndex, const VpSkeleton::Header& hdr,
				uint32_t missingInputs) override;

			// Diagnostics and tests.
			const VpLaneProducts::GpuProductTextures* TexturesOf(VpSkeleton::ProductId id) const;
			pl_tex TapOf(uint64_t seq, bool extended) const;
			uint64_t AllocatedBytes() const;
			// The product has textures (false while refused or not yet configured: the lane job declines, the readers fall back).
			bool Available(VpSkeleton::ProductId id) const;
			// Any product or tap texture is allocated now (the video-memory query then already counts it).
			bool HoldsVideoMemory() const;
			// The renderer's device died without GpuReleased: forget every device object WITHOUT calling libplacebo.
			void Abandon();
			// Video memory the dispatcher holds OUTSIDE the product textures (the governor does not see it): the tap ring
			// (tap + extended plane, R16, per in-flight frame) and the kernels' working textures. Analytic (no allocation).
			uint64_t PrivateBytes(VpSkeleton::Dims dims, bool laneForm) const;
			// G5: the last measured device time of a product's pass(es) in ms (pl_timer), 0 = not measured yet. The lane job's
			// predictedMs reads it (a conservative derived default is used until then).
			double MeasuredMs(VpSkeleton::ProductId id) const;
			const Counters& Stats() const { return m_counters; }
			size_t PendingReleases() const { return m_graveyard.size(); }
			const std::string& LastError() const { return m_error; }
			std::vector<FallbackCatalog::Decision> Fallbacks() const;

		private:
			struct Tap
			{
				pl_tex tap = nullptr;
				pl_tex extended = nullptr;
				uint64_t seq = 0;
				bool used = false;
				uint32_t bands = 0;        // bands uploaded (rows [0, bands * rowsPerBand))
				bool extendedDone = false;
				RegionEvidence::Rect picture;
			};
			struct ProductState
			{
				std::unique_ptr<VpLaneProducts::GpuProductTextures> tex;
				uint32_t history = 0;
				std::string refusal;
				pl_timer timer = nullptr;
				uint64_t lastNs = 0;
			};

			Tap* TapFor(uint64_t seq, bool create);
			bool EnsureTap(Tap& t, const VpSkeleton::SourceFrameView& view, const RegionEvidence::Rect& picture);
			bool BuildContext(VpLaneProducts::KernelContext& c, const VpSkeleton::Node& node, uint64_t seq, bool live,
				const VpSkeleton::LaneJobContext* job, uint32_t missing);
			bool Fail(const std::string& why);
			void PollTimer(ProductState& ps);

			pl_gpu m_gpu = nullptr;
			pl_dispatch m_dispatch = nullptr;
			VpLaneProducts::GpuTexBackend m_backend;
			const VpSkeleton::AnalysisLane* m_lane = nullptr;
			std::function<bool(uint64_t, VpSkeleton::SourceFrameView*)> m_liveSource;
			std::vector<std::unique_ptr<VpLaneProducts::IProductKernel>> m_kernels;
			std::map<VpSkeleton::ProductId, ProductState> m_products;
			std::vector<std::unique_ptr<VpLaneProducts::GpuProductTextures>> m_graveyard;
			std::array<Tap, kTapRing> m_taps;
			VpSkeleton::Dims m_dims;
			uint64_t m_graphKey = 0;
			uint32_t m_settingsGen = 0;
			std::vector<uint16_t> m_scratch;
			std::string m_error;
			Counters m_counters;
		};
	}
}
