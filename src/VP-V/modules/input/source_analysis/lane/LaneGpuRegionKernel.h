// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
#pragma once

// 70b G3: the GPU kernel of P11 (region block statistics) for the skeleton's render-thread dispatcher
// (modules/control/skeleton/gpu/SkeletonGpuDispatcher.h). One compute pass, one thread per block, the tap form of the
// stats shader (RegionEvidenceGlsl.h: StatsPassTapHeaderGlsl / StatsPassTapGlsl): the source is the dispatcher's EXTENDED
// tap plane (exact 10-bit codes), the band energies come from P10's Compact block texel, the aux texel is zeros. The
// product is 6 rgba32f grids (S0..S4 and aux) at the block edge RegionEvidence::BlockSizeFor(height); 96 B per cell, the
// declaration of P11 (core/Products.h).
//
// Visible downgrades (Fallbacks(), never log-only): "P11 without bands" (P10 missing, stored Full, or another grid: the
// pass runs with re_have_split = 0, so grain, texture, ringing and banding read as unknown) and "display facts:
// defaults" (transfer, SDR white and display peak are the shown defaults until the light level unit supplies them).

#include <VP-V/modules/input/source_analysis/lane/LaneGpuProducts.h>
#include <VP-V/modules/input/source_analysis/regions/RegionEvidence.h>

#include <cstdint>
#include <string>
#include <vector>

namespace VpLaneProducts
{
	class RegionStatsKernel : public IProductKernel
	{
	public:
		VpSkeleton::ProductId Product() const override { return VpSkeleton::kP11RegionBlockStats; }
		// 6 absolute rgba32f terms at shift 4 (bw x bh): the declaration's 96 B per cell (ProductTexLayout::MatchesDecl).
		VpSkeleton::ProductTexLayout Layout(VpSkeleton::Dims dims) const override;
		uint32_t Steps(VpSkeleton::Dims dims) const override { return StepsFor(Product(), dims); }
		bool NeedsTap() const override { return true; }
		bool Prepare(pl_gpu gpu, pl_dispatch dispatch, VpSkeleton::Dims dims) override;
		bool Step(KernelContext& c, uint32_t step) override;
		void Release(pl_gpu gpu) override;
		std::string LastError() const override { return m_error; }
		std::vector<FallbackCatalog::Decision> Fallbacks() const override;

		// The display facts the stats pass reads (transfer 0 = SDR gamma, 1 = PQ, 2 = HLG; the shader's re_transfer).
		// Until the light level unit supplies them through the hub they are the shown defaults.
		void SetDisplayFacts(int transfer, double sdrWhiteNits, const RegionEvidence::VisibilityParams& vis);

		// The last Step: whether P10's bands were bound (false = re_have_split 0) and why not.
		bool BandsUsed() const { return !m_noBands; }
		const std::string& BandsWhy() const { return m_bandsWhy; }
		bool DisplayFactsAreDefaults() const { return m_defaultFacts; }

		// The pass GLSL (header and body) and its FNV-1a 64 fingerprint (pinned by the tests).
		static std::string HeaderGlsl();
		static std::string BodyGlsl();
		static uint64_t PassFingerprint();

	private:
		bool Fail(KernelContext& c, const std::string& why);
		bool EnsureDummy(pl_gpu gpu);

		pl_tex m_dummy = nullptr;           // the bands stand-in when P10 has none (1 x 1, never read)
		std::string m_header, m_body;
		std::string m_error;
		std::string m_bandsWhy;
		bool m_noBands = false;
		bool m_defaultFacts = true;
		int m_transfer = 0;
		double m_sdrWhite = 100.0;
		RegionEvidence::VisibilityParams m_vis;
	};
}
