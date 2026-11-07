// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
#pragma once

// 70b G2: the GPU kernel of lane product P10 (grain split) for the skeleton's render-thread dispatcher
// (SkeletonGpuDispatcher). The dispatcher puts the tap bands and the extend pass in front (StepsFor); this kernel's five
// steps are GrainSplit::Gpu::Splitter::Step(0..4) (rows, columns, grain level, neighbourhood minimum, Wiener share) on the
// EXTENDED tap plane: component 0, codeUnit 64 / 65535 (a texel is code * 64 / 65535), B = RegionEvidence::BlockSizeFor(h),
// scale B / 16 (the law of RegionEvidence.h). The same steps run for the lane form (one per chunk) and the live form (all
// at once), so lane == live by construction. Two Splitters (lane, live): their private intermediates survive across the
// lane job's gaps and a live fill never clobbers a lane job in flight.

#include <VP-V/modules/input/source_analysis/lane/LaneGpuProducts.h>
#include <VP-V/modules/repair/grain_split/GrainSplitGpu.h>

namespace VpLaneProducts
{
	class GrainSplitKernel : public IProductKernel
	{
	public:
		// `requested`: the either/or setting grain_split_storage. Full until the grain gate has passed for Compact.
		explicit GrainSplitKernel(GrainSplit::Storage requested = GrainSplit::Storage::Full) : m_requested(requested) {}

		VpSkeleton::ProductId Product() const override { return VpSkeleton::kP10GrainSplit; }
		VpSkeleton::ProductTexLayout Layout(VpSkeleton::Dims dims) const override;
		uint32_t Steps(VpSkeleton::Dims) const override { return GrainSplit::Gpu::Splitter::StepCount(); }
		bool NeedsTap() const override { return true; }
		bool Prepare(pl_gpu gpu, pl_dispatch dispatch, VpSkeleton::Dims dims) override;
		bool Step(KernelContext& c, uint32_t step) override;
		void Release(pl_gpu gpu) override;
		std::string LastError() const override { return m_error; }
		// Working set of one Splitter (rgba32f temporaries, the arena-free layout of GrainSplitGpu): Compact = rows + bands
		// (two full-size planes), Full = bands (rows is the product), plus the block grid; two Splitters (lane + live).
		uint64_t PrivateBytes(VpSkeleton::Dims dims, bool laneForm) const override;
		std::vector<FallbackCatalog::Decision> Fallbacks() const override;

		// The storage the layout really uses (a refused Compact is Full, shown through Fallbacks()).
		GrainSplit::Storage Granted() const { return Choice().storage; }
		// The last Frame the lane / live form produced (valid after its step 4; reset when a run fails).
		const GrainSplit::Gpu::Frame& LastFrame(bool live) const { return live ? m_live.Out() : m_lane.Out(); }

		// The plane the kernel reads: R16 unorm, codeUnit 64 / 65535 (the tap law).
		static double CodeUnit() { return 64.0 / 65535.0; }

	private:
		GrainSplit::Gpu::StorageChoice Choice() const;
		GrainSplit::Storage m_requested;
		GrainSplit::Gpu::Splitter m_lane, m_live;
		std::string m_error;
	};
}
