// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
#pragma once

// 70b G3 / 15c C3: GPU kernel of P11 (region block statistics) for SkeletonGpuDispatcher: one compute pass over the extended
// tap plane + P10 block energies (both Compact term 2 and Full term 1) -> 6 rgba32f grids (S0..S4, aux; 96 B/cell).
// Visible downgrades (Fallbacks()): "P11 without bands" (re_have_split = 0) and "display facts: defaults".

#include <VP-V/modules/input/source_analysis/lane/LaneGpuProducts.h>
#include <VP-V/modules/input/source_analysis/regions/RegionEvidence.h>

#include <cstdint>
#include <string>
#include <vector>

namespace VpSkeleton { struct AcquireResult; }

namespace VpLaneProducts
{
	constexpr size_t kP11TensorTerm = 1;       // re_s1 = (jxx, jyy, jxy, lap_e)
	constexpr size_t kP11BandEnergyTerm = 2;   // re_s2 = (e1, e2, e3, off_lap)
	constexpr size_t kP11CoverageTerm = 4;     // re_s4 = (in_pic, off_share, vis, N)
	struct P11BandTextures { pl_tex s2 = nullptr; pl_tex s4 = nullptr; };
	P11BandTextures ExtractP11BandTextures(const VpSkeleton::AcquireResult& res);

	class RegionStatsKernel : public IProductKernel
	{
	public:
		VpSkeleton::ProductId Product() const override { return VpSkeleton::kP11RegionBlockStats; }
		// 6 absolute rgba32f terms at shift 4 (bw x bh): the declaration's 96 B per cell (ProductTexLayout::MatchesDecl).
		VpSkeleton::ProductTexLayout Layout(VpSkeleton::Dims dims) const override;
		uint32_t Steps(VpSkeleton::Dims dims) const override { return StepsFor(Product(), dims); }
		bool NeedsTap() const override { return true; }
		void SetStaticTextures(IStaticTextures* store) override { m_store = store; }
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

		pl_tex m_dummy = nullptr;           // the bands stand-in when P10 has none (1 x 1, never read); owned by the texture store
		IStaticTextures* m_store = nullptr;
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
