// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
// Built without the precompiled header (libplacebo + VP-V headers only).
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <VP-V/modules/input/source_analysis/lane/LaneGpuLightKernel.h>

#include <VP-V/modules/input/source_analysis/light_level/LightLevelBlocks.h>
#include <VP-V/modules/input/source_analysis/light_level/LightLevelBlocksGlsl.h>
#include <VP-V/modules/repair/grain_split/GpuPassUtil.h>

#include <algorithm>
#include <memory>

namespace VpLaneProducts
{
	namespace
	{
		// P11's terms the shader reads: S0 (mean code), S3 (mean PQ), S4 (coverage, grain N).
		constexpr size_t kS0Term = 0, kS3Term = 3, kS4Term = 4;
		constexpr size_t kHistoryKept = 16;
	}

	VpSkeleton::ProductTexLayout LightLevelKernel::Layout(VpSkeleton::Dims dims) const
	{
		const int B = RegionEvidence::BlockSizeFor(int(dims.height));
		const RegionEvidence::Grid g = RegionEvidence::MakeGrid(int(dims.width), int(dims.height), B);
		VpSkeleton::ProductTexLayout l;
		VpSkeleton::TexTerm term;
		term.shift = 4;
		term.fmt = VpSkeleton::TexFormat::RGBA32F;
		term.absW = uint32_t(g.gw);
		term.absH = uint32_t(g.gh);
		l.terms.push_back(term);
		return l;
	}

	std::string LightLevelKernel::HeaderGlsl() { return LightLevel::Blocks::LightLevelBlocksHeaderGlsl(); }
	std::string LightLevelKernel::BodyGlsl() { return LightLevel::Blocks::LightLevelBlocksPassGlsl(); }

	uint64_t LightLevelKernel::PassFingerprint()
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

	void LightLevelKernel::SetSettings(const LightLevel::Settings& settings, const LightLevel::Context& context)
	{
		m_settings = settings;
		m_context = context;
		m_defaultFacts = false;
		++m_generation;
	}

	bool LightLevelKernel::EnsureDummy(pl_gpu gpu)
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

	bool LightLevelKernel::EnsureLut(pl_gpu gpu)
	{
		if (m_lutTex && m_builtGeneration == m_generation)
			return true;
		const pl_fmt fmt = gpu ? VpvGpu::FloatFormat(gpu, 4) : nullptr;
		if (!fmt)
			return false;
		std::unique_ptr<LightLevel::Lut> lut(new LightLevel::Lut(LightLevel::Build(m_settings, m_context)));
		m_texels = LightLevel::PackTexels(*lut);
		m_fingerprint = lut->fingerprint;
		if (m_lutTex)
			pl_tex_destroy(gpu, &m_lutTex);
		m_lutTex = nullptr;
		pl_tex_params tp{};
		tp.w = LightLevel::kLutSize;
		tp.h = 1;
		tp.format = fmt;
		tp.sampleable = true;
		tp.initial_data = m_texels.data();
		m_lutTex = pl_tex_create(gpu, &tp);
		if (!m_lutTex)
			return false;
		m_builtGeneration = m_generation;
		m_history.clear();   // a new context: the one-pole starts again (the CPU twin's Context reset)
		return true;
	}

	bool LightLevelKernel::Prepare(pl_gpu gpu, pl_dispatch, VpSkeleton::Dims)
	{
		m_header = HeaderGlsl();
		m_body = BodyGlsl();
		if (!EnsureDummy(gpu))
		{
			m_error = "no rgba32f format for the light level stand-in";
			return false;
		}
		m_builtGeneration = 0;   // rebuild and upload the LUT
		if (!EnsureLut(gpu))
		{
			m_error = "the light level LUT could not be uploaded";
			return false;
		}
		m_history.clear();   // also on a size change
		m_error.clear();
		return true;
	}

	bool LightLevelKernel::Fail(KernelContext& c, const std::string& why)
	{
		m_error = why;
		c.error = why;
		return false;
	}

	bool LightLevelKernel::FullRecorded(uint64_t seq) const
	{
		for (const auto& e : m_history)
			if (e.first == seq)
				return e.second;
		return false;
	}

	void LightLevelKernel::Record(uint64_t seq, bool full)
	{
		for (auto& e : m_history)
			if (e.first == seq)
			{
				e.second = full;
				return;
			}
		m_history.push_back(std::make_pair(seq, full));
		if (m_history.size() > kHistoryKept)
			m_history.erase(m_history.begin());
	}

	bool LightLevelKernel::Step(KernelContext& c, uint32_t step)
	{
		namespace LB = LightLevel::Blocks;
		if (step != 0)
			return Fail(c, "P12 has one pass");
		if (m_header.empty())
		{
			m_header = HeaderGlsl();
			m_body = BodyGlsl();
		}
		const int w = int(c.dims.width), h = int(c.dims.height);
		if (w <= 0 || h <= 0)
			return Fail(c, "P12: empty frame");
		if (c.outCount < 1 || !c.out[0])
			return Fail(c, "P12: the slot has no texture");
		const int B = RegionEvidence::BlockSizeFor(h);
		const RegionEvidence::Grid g = RegionEvidence::MakeGrid(w, h, B);
		if (c.out[0]->params.w != g.gw || c.out[0]->params.h != g.gh)
			return Fail(c, "P12: the output grid does not match the block grid");
		if (!EnsureDummy(c.gpu) || !EnsureLut(c.gpu))
			return Fail(c, "P12: no stand-in or LUT texture");

		// P11's grids: the full mode needs all three on this grid.
		pl_tex s0 = nullptr, s3 = nullptr, s4 = nullptr;
		std::string why;
		const GpuInput* p11 = c.Input(VpSkeleton::kP11RegionBlockStats);
		if (p11 == nullptr || p11->missing)
			why = "the region statistics (P11) have no content for this frame";
		else
		{
			s0 = p11->Term(kS0Term);
			s3 = p11->Term(kS3Term);
			s4 = p11->Term(kS4Term);
			if (!s0 || !s3 || !s4)
				why = "the region statistics (P11) have no textures";
			else if (s0->params.w != g.gw || s0->params.h != g.gh || s3->params.w != g.gw || s3->params.h != g.gh ||
				s4->params.w != g.gw || s4->params.h != g.gh)
				why = "the region statistics (P11) are on another block grid";
		}
		const bool full = why.empty();

		// P4's mean picture code (16-bit scale -> the LUT's 10-bit code): the frame level.
		double aplCode = -1.0;
		const GpuInput* p4 = c.Input(VpSkeleton::kP4SourceStats);
		SourceStatsRecord rec;
		if (p4 != nullptr && !p4->missing && ReadSourceStats(p4->cpu, p4->cpuBytes, &rec) && rec.pixelCount > 0)
			aplCode = double(rec.mean) / 64.0;
		float frameJ = 0.0f, frameV = 0.0f;
		if (aplCode >= 0.0)
		{
			const LightLevel::PackedEntry<float> e = LightLevel::FetchPacked<float>(m_texels.data(), float(aplCode));
			frameJ = e.J;
			frameV = (std::min)(e.V, float(LB::kVCap));
		}

		LB::Params p = LB::ParamsFor(w, h, c.picture, B);
		p.frameRateHz = m_frameRateHz;
		const LB::Kernel kern = LB::KernelFor(p);
		const float alpha = float(LB::TemporalAlpha(p));

		// The one-pole state: the product's own frame seq - 1, only when it was made in the full mode and is another texture.
		pl_tex prev = c.OwnPrev(0);
		const bool usePrev = full && c.seq > 0 && prev != nullptr && prev != c.out[0] && prev->params.w == g.gw &&
			prev->params.h == g.gh && FullRecorded(c.seq - 1);
		m_resetUnexpected = full && !usePrev && !m_history.empty();
		m_lastFull = full;
		m_lastUsedPrev = usePrev;
		if (!full)
			m_frameWhy = why + (aplCode >= 0.0 ? "; one level for the whole frame from the source APL, confidence " +
				std::to_string(p.frameLevelConf) : std::string("; no source APL either: code only (zeros, confidence 0)"));
		else
			m_frameWhy.clear();

		const int grid[2] = { g.gw, g.gh }, radius = kern.radius, reset = usePrev ? 0 : 1, mode = full ? 0 : 1;
		const float sigma = float(kern.sigmaBlocks), gate = m_gate, frameConf = float(p.frameLevelConf);
		const float frameVOut = full ? 0.0f : frameV;
		const pl_shader_var vars[10] = { { pl_var_ivec2("ll_grid"), grid, true }, { pl_var_float("ll_sigma"), &sigma, true },
			{ pl_var_int("ll_radius"), &radius, true }, { pl_var_float("ll_frame_j"), &frameJ, true },
			{ pl_var_float("ll_gate"), &gate, true }, { pl_var_int("ll_reset"), &reset, true },
			{ pl_var_float("ll_alpha"), &alpha, true }, { pl_var_int("ll_mode"), &mode, true },
			{ pl_var_float("ll_frame_v"), &frameVOut, true }, { pl_var_float("ll_frame_conf"), &frameConf, true } };
		const pl_shader_desc desc[6] = { VpvGpu::Sampled("re_s0", full ? s0 : m_dummy), VpvGpu::Sampled("re_s3", full ? s3 : m_dummy),
			VpvGpu::Sampled("re_s4", full ? s4 : m_dummy), VpvGpu::Sampled("ll_prev", usePrev ? prev : m_dummy),
			VpvGpu::Sampled("light_level_lut", m_lutTex), VpvGpu::Storage("ll_out", c.out[0]) };
		if (!VpvGpu::Pass(c.dispatch, "VideoProcessor P12 light level blocks", m_header, m_body.c_str(), desc, 6, vars, 10,
				(g.gw + 7) / 8, (g.gh + 7) / 8, 0, 8, 8, c.timer))
			return Fail(c, "P12: the block pass failed to build or dispatch");
		Record(c.seq, full);
		return true;
	}

	void LightLevelKernel::Release(pl_gpu gpu)
	{
		if (m_dummy && gpu)
			pl_tex_destroy(gpu, &m_dummy);
		if (m_lutTex && gpu)
			pl_tex_destroy(gpu, &m_lutTex);
		m_dummy = nullptr;
		m_lutTex = nullptr;
		m_builtGeneration = 0;
		m_history.clear();
	}

	std::vector<FallbackCatalog::Decision> LightLevelKernel::Fallbacks() const
	{
		std::vector<FallbackCatalog::Decision> out;
		if (!m_lastFull)
			out.push_back(FallbackCatalog::Engaged("light_level_frame", "P12 frame-level: " + m_frameWhy));
		else
			out.push_back(FallbackCatalog::Released("light_level_frame"));
		if (m_defaultFacts)
			out.push_back(FallbackCatalog::Engaged("light_level_display",
				"display facts: defaults (SDR context, default light level settings) until the light level unit supplies them"));
		else
			out.push_back(FallbackCatalog::Released("light_level_display"));
		if (m_resetUnexpected)
			out.push_back(FallbackCatalog::Engaged("light_level_smoothing",
				"P12 smoothing reset: the previous frame's light level was not available; the surround restarts unsmoothed"));
		else
			out.push_back(FallbackCatalog::Released("light_level_smoothing"));
		return out;
	}
}
