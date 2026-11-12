// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
#include <pch.h>
#include <VP-V/modules/input/source_analysis/lane/LaneGpuGrainKernel.h>
#include <VP-V/modules/repair/grain_split/GrainNodeGpu.h>

namespace VpLaneProducts
{
	GrainSplit::Gpu::StorageChoice GrainSplitKernel::Choice() const
	{
		// The plane is an R16 unorm texture: only its type matters to ChooseStorage.
		pl_fmt_t fmt{};
		fmt.type = PL_FMT_UNORM;
		return GrainSplit::Gpu::ChooseStorage(m_requested, &fmt, CodeUnit());
	}

	VpSkeleton::ProductTexLayout GrainSplitKernel::Layout(VpSkeleton::Dims dims) const
	{
		GrainSplit::GrainDesc desc;
		desc.w = int(dims.width);
		desc.h = int(dims.height);
		desc.B = RegionEvidence::BlockSizeFor(desc.h);
		desc.scale = double(desc.B) / 16.0;
		desc.codeUnit = CodeUnit();
		desc.component = 0;
		pl_fmt_t fmt{};
		fmt.type = PL_FMT_UNORM;
		return GrainSplit::Gpu::ChooseNodeLayout(m_requested, &fmt, CodeUnit(), desc).layout;
	}

	bool GrainSplitKernel::Prepare(pl_gpu gpu, pl_dispatch dispatch, VpSkeleton::Dims dims)
	{
		(void)dispatch;
		(void)dims;
		m_error.clear();
		return gpu != nullptr;
	}

	bool GrainSplitKernel::Step(KernelContext& c, uint32_t step)
	{
		GrainSplit::Gpu::Splitter& sp = c.live ? m_live : m_lane;
		const GrainSplit::Gpu::StorageChoice choice = Choice();
		const bool compact = choice.storage == GrainSplit::Storage::Compact;
		const size_t need = compact ? 3 : 2;
		if (!c.extended || c.outCount < need)
		{
			m_error = c.error = "grain split kernel: no extended plane or output textures";
			return false;
		}
		GrainSplit::Gpu::Options opt;
		opt.storage = choice.storage;
		opt.arena = nullptr;
		opt.seq = c.seq;
		opt.timer = c.timer;
		opt.targets.split = c.out[0];
		if (compact)
		{
			opt.targets.splitB = c.out[1];
			opt.targets.blocks = c.out[2];
		}
		else
			opt.targets.blocks = c.out[1];
		const int h = int(c.dims.height);
		const int B = RegionEvidence::BlockSizeFor(h);
		if (!sp.Step(c.gpu, c.dispatch, c.extended, 0, CodeUnit(), double(B) / 16.0, B, GrainSplit::Params(), opt, step))
		{
			m_error = c.error = sp.LastError();
			return false;
		}
		return true;
	}

	uint64_t GrainSplitKernel::PrivateBytes(VpSkeleton::Dims dims, bool laneForm) const
	{
		const uint64_t B = uint64_t(RegionEvidence::BlockSizeFor(int(dims.height)));
		const uint64_t px = uint64_t(dims.width) * dims.height;
		const uint64_t blocks = ((uint64_t(dims.width) + B - 1) / B) * ((uint64_t(dims.height) + B - 1) / B);
		// v3 (refs/116): two half-size rg32f planes (tmp, low) = 4 B per plane px, plus the rgba32f block grid (quiet); the
		// product textures are the slot's, not private.
		const uint64_t halfPx = ((uint64_t(dims.width) + 1u) / 2u) * ((uint64_t(dims.height) + 1u) / 2u);
		const uint64_t one = 2u * halfPx * 8u + blocks * 16u;
		(void)px;
		return laneForm ? 2u * one : one;
	}

	void GrainSplitKernel::Release(pl_gpu gpu)
	{
		m_lane.Release(gpu);
		m_live.Release(gpu);
	}

	std::vector<FallbackCatalog::Decision> GrainSplitKernel::Fallbacks() const
	{
		std::vector<FallbackCatalog::Decision> d;
		d.push_back(GrainSplit::Gpu::StorageFallback(Choice()));
		return d;
	}
}
