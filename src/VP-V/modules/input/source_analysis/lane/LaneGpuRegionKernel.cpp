// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
// Built without the precompiled header (libplacebo + VP-V headers only).
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <VP-V/modules/input/source_analysis/lane/LaneGpuRegionKernel.h>

#include <VP-V/modules/input/source_analysis/regions/RegionEvidenceGlsl.h>
#include <VP-V/modules/repair/grain_split/GpuPassUtil.h>

#include <algorithm>

namespace VpLaneProducts
{
	namespace
	{
		constexpr size_t kTerms = 6;      // S0..S4 and aux
		constexpr size_t kBandsTerm = 2;  // P10 Compact: split A, split B, blocks (N, e3, e1, e2)
	}

	VpSkeleton::ProductTexLayout RegionStatsKernel::Layout(VpSkeleton::Dims dims) const
	{
		const int B = RegionEvidence::BlockSizeFor(int(dims.height));
		const RegionEvidence::Grid g = RegionEvidence::MakeGrid(int(dims.width), int(dims.height), B);
		VpSkeleton::ProductTexLayout l;
		for (size_t t = 0; t < kTerms; ++t)
		{
			VpSkeleton::TexTerm term;
			term.shift = 4;
			term.fmt = VpSkeleton::TexFormat::RGBA32F;
			term.absW = uint32_t(g.gw);
			term.absH = uint32_t(g.gh);
			l.terms.push_back(term);
		}
		return l;
	}

	std::string RegionStatsKernel::HeaderGlsl() { return RegionEvidence::StatsPassTapHeaderGlsl(); }
	std::string RegionStatsKernel::BodyGlsl() { return RegionEvidence::StatsPassTapGlsl(); }

	uint64_t RegionStatsKernel::PassFingerprint()
	{
		uint64_t h = 0xCBF29CE484222325ull;
		const std::string s = HeaderGlsl() + "\n" + BodyGlsl();
		for (const char ch : s)
		{
			h ^= uint8_t(ch);
			h *= 0x100000001B3ull;
		}
		return h;
	}

	void RegionStatsKernel::SetDisplayFacts(int transfer, double sdrWhiteNits, const RegionEvidence::VisibilityParams& vis)
	{
		m_transfer = transfer;
		m_sdrWhite = sdrWhiteNits;
		m_vis = vis;
		m_defaultFacts = false;
	}

	bool RegionStatsKernel::EnsureDummy(pl_gpu gpu)
	{
		if (m_dummy)
			return true;
		const pl_fmt fmt = gpu ? VpvGpu::FloatFormat(gpu, 4) : nullptr;
		if (!fmt)
			return false;
		const float zero[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
		pl_tex_params tp{};
		tp.w = 1;
		tp.h = 1;
		tp.format = fmt;
		tp.sampleable = true;
		tp.initial_data = zero;
		m_dummy = pl_tex_create(gpu, &tp);
		return m_dummy != nullptr;
	}

	bool RegionStatsKernel::Prepare(pl_gpu gpu, pl_dispatch, VpSkeleton::Dims)
	{
		m_header = HeaderGlsl();
		m_body = BodyGlsl();
		if (!EnsureDummy(gpu))
		{
			m_error = "no rgba32f format for the bands stand-in";
			return false;
		}
		m_error.clear();
		return true;
	}

	bool RegionStatsKernel::Fail(KernelContext& c, const std::string& why)
	{
		m_error = why;
		c.error = why;
		return false;
	}

	bool RegionStatsKernel::Step(KernelContext& c, uint32_t step)
	{
		if (step != 0)
			return Fail(c, "P11 has one pass");
		if (m_header.empty())
		{
			m_header = HeaderGlsl();
			m_body = BodyGlsl();
		}
		const int w = int(c.dims.width), h = int(c.dims.height);
		if (w <= 0 || h <= 0)
			return Fail(c, "P11: empty frame");
		if (!c.extended || int(c.extended->params.w) != w || int(c.extended->params.h) != h)
			return Fail(c, "P11: the extended tap plane is missing or has another size");
		if (c.outCount < kTerms)
			return Fail(c, "P11: the slot has fewer than 6 textures");
		const int B = RegionEvidence::BlockSizeFor(h);
		const RegionEvidence::Grid g = RegionEvidence::MakeGrid(w, h, B);
		for (size_t t = 0; t < kTerms; ++t)
			if (!c.out[t] || c.out[t]->params.w != g.gw || c.out[t]->params.h != g.gh)
				return Fail(c, "P11: an output grid does not match the block grid");

		// P10's blocks: usable only when present, Compact (e1, e2 stored) and on the same grid.
		pl_tex bands = nullptr;
		std::string why;
		const GpuInput* p10 = c.Input(VpSkeleton::kP10GrainSplit);
		if (p10 == nullptr || p10->missing)
			why = "the grain split (P10) has no content for this frame";
		else if (pl_tex b = p10->Term(kBandsTerm))
		{
			if (b->params.w == g.gw && b->params.h == g.gh)
				bands = b;
			else
				why = "the grain split (P10) is on another block grid";
		}
		else
			why = "the grain split (P10) is stored Full (no band energies)";
		m_noBands = bands == nullptr;
		m_bandsWhy = why;
		if (bands == nullptr)
		{
			if (!EnsureDummy(c.gpu))
				return Fail(c, "P11: no bands stand-in texture");
			bands = m_dummy;
		}

		const RegionEvidence::Rect pic = RegionEvidence::PictureOrFrame(c.picture, w, h);
		const int frame[2] = { w, h }, lo[2] = { pic.left, pic.top }, hi[2] = { pic.right, pic.bottom };
		const int block = B, transfer = m_transfer, haveSplit = m_noBands ? 0 : 1;
		const float vis[4] = { float(m_sdrWhite), float(m_vis.displayPeakNits), float(m_vis.kneeShare), float(m_vis.weber) };
		const float dark = float(m_vis.darkNits);
		const pl_shader_var vars[8] = { { pl_var_ivec2("re_frame"), frame, true }, { pl_var_ivec2("re_rect_lo"), lo, true },
			{ pl_var_ivec2("re_rect_hi"), hi, true }, { pl_var_int("re_block"), &block, true },
			{ pl_var_int("re_transfer"), &transfer, true }, { pl_var_int("re_have_split"), &haveSplit, true },
			{ pl_var_vec4("re_vis"), vis, true }, { pl_var_float("re_dark"), &dark, true } };
		const pl_shader_desc desc[8] = { VpvGpu::Sampled("re_src", c.extended), VpvGpu::Sampled("re_bands", bands),
			VpvGpu::Storage("re_s0", c.out[0]), VpvGpu::Storage("re_s1", c.out[1]), VpvGpu::Storage("re_s2", c.out[2]),
			VpvGpu::Storage("re_s3", c.out[3]), VpvGpu::Storage("re_s4", c.out[4]), VpvGpu::Storage("re_s5", c.out[5]) };
		if (!VpvGpu::Pass(c.dispatch, "VideoProcessor P11 region block statistics", m_header, m_body.c_str(), desc, 8, vars, 8,
				(g.gw + 7) / 8, (g.gh + 7) / 8, 0, 8, 8, c.timer))
			return Fail(c, "P11: the stats pass failed to build or dispatch");
		return true;
	}

	void RegionStatsKernel::Release(pl_gpu gpu)
	{
		if (m_dummy && gpu)
			pl_tex_destroy(gpu, &m_dummy);
		m_dummy = nullptr;
	}

	std::vector<FallbackCatalog::Decision> RegionStatsKernel::Fallbacks() const
	{
		std::vector<FallbackCatalog::Decision> out;
		if (m_noBands)
			out.push_back(FallbackCatalog::Engaged("region_stats_bands",
				"P11 without bands: " + m_bandsWhy + "; grain, texture, ringing and banding read as unknown"));
		else
			out.push_back(FallbackCatalog::Released("region_stats_bands"));
		if (m_defaultFacts)
			out.push_back(FallbackCatalog::Engaged("region_stats_display",
				"display facts: defaults (SDR gamma, SDR white " + std::to_string(int(m_sdrWhite)) + " nits, display peak " +
				std::to_string(int(m_vis.displayPeakNits)) + " nits) until the light level unit supplies them"));
		else
			out.push_back(FallbackCatalog::Released("region_stats_display"));
		return out;
	}
}
