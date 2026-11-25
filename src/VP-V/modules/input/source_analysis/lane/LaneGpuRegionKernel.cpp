// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
// Built without the precompiled header (libplacebo + VP-V headers only).
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <VP-V/modules/input/source_analysis/lane/LaneGpuRegionKernel.h>

#include <VP-V/modules/input/source_analysis/regions/RegionEvidenceGlsl.h>
#include <VP-V/modules/repair/grain_split/GpuPassUtil.h>
#include <VP-V/core/ComputeOnce.h>
#include <VP-V/core/SkeletonAcquire.h>

#include <algorithm>
#include <mutex>
#include <unordered_map>

namespace VpLaneProducts
{
	namespace
	{
		constexpr size_t kTerms = 6;             // S0..S4 and aux
		constexpr size_t kBandsTermCompact = 2;  // P10 Compact: split A, split B, blocks (N, e3, e1, e2)
		constexpr size_t kBandsTermFull = 1;     // P10 Full: split, blocks (N, e3, e1, e2)

		// Per-slot bands-validity registry keyed by P11's re_s2 pl_tex handle so ExtractP11BandTextures
		// knows whether RegionStatsKernel::Step ran with P10 bands bound (re_have_split == 1) or without
		// (m_noBands == true -> re_s2.xyz == 0, re_s4.w == -1). Entries are cleared on Release / destruction.
		std::mutex& SlotBandsMutex()
		{
			static std::mutex m;
			return m;
		}
		std::unordered_map<const void*, bool>& SlotBandsRegistry()
		{
			static std::unordered_map<const void*, bool> reg;
			return reg;
		}
		void RecordSlotBands(const void* s2Tex, bool hasBands, std::vector<const void*>& owned)
		{
			if (s2Tex == nullptr)
				return;
			{
				std::lock_guard<std::mutex> lock(SlotBandsMutex());
				SlotBandsRegistry()[s2Tex] = hasBands;
			}
			if (std::find(owned.begin(), owned.end(), s2Tex) == owned.end())
				owned.push_back(s2Tex);
		}
		bool LookupSlotHasBands(const void* s2Tex)
		{
			if (s2Tex == nullptr)
				return false;
			std::lock_guard<std::mutex> lock(SlotBandsMutex());
			const auto it = SlotBandsRegistry().find(s2Tex);
			return it == SlotBandsRegistry().end() ? true : it->second;
		}
	}

	P11BandTextures ExtractP11BandTextures(const VpSkeleton::AcquireResult& res)
	{
		P11BandTextures out;
		if (!res.Ok() || !res.gpu.Valid())
			return out;
		if (res.gpu.textureSet != nullptr && res.gpu.index >= 0)
		{
			const auto* set11 = static_cast<const GpuProductTextures*>(res.gpu.textureSet);
			const size_t idx = static_cast<size_t>(res.gpu.index);
			out.s2 = res.gpu.ringSlot ? set11->Ring(idx, kP11BandEnergyTerm) : set11->Live(idx, kP11BandEnergyTerm);
			out.s4 = res.gpu.ringSlot ? set11->Ring(idx, kP11CoverageTerm) : set11->Live(idx, kP11CoverageTerm);
		}
		else if (res.gpu.resource != nullptr || res.gpu.view != nullptr)
		{
			out.s2 = static_cast<pl_tex>(res.gpu.resource != nullptr ? res.gpu.resource : res.gpu.view);
			out.s4 = (res.gpu.resource != nullptr && res.gpu.view != nullptr)
				? static_cast<pl_tex>(res.gpu.view) : nullptr;
		}
		if (out.s2 != nullptr && (!out.s2->params.sampleable || out.s2->params.w <= 0 || out.s2->params.h <= 0
			|| out.s2->params.format == nullptr || out.s2->params.format->num_components < 3))
			out.s2 = nullptr;
		if (out.s4 != nullptr && (!out.s4->params.sampleable || out.s4->params.w <= 0 || out.s4->params.h <= 0
			|| out.s4->params.format == nullptr || out.s4->params.format->num_components < 4))
			out.s4 = nullptr;
		out.hasBands = out.s2 != nullptr && LookupSlotHasBands(out.s2);
		return out;
	}

	RegionStatsKernel::~RegionStatsKernel()
	{
		ClearRecordedSlots();
	}

	void RegionStatsKernel::ClearRecordedSlots()
	{
		if (m_recordedS2.empty())
			return;
		{
			std::lock_guard<std::mutex> lock(SlotBandsMutex());
			auto& reg = SlotBandsRegistry();
			for (const void* k : m_recordedS2)
				reg.erase(k);
		}
		m_recordedS2.clear();
	}

	VpSkeleton::ProductTexLayout RegionStatsKernel::Layout(VpSkeleton::Dims dims) const
	{
		const int B = RegionEvidence::BlockSizeFor(int(dims.height));
		const RegionEvidence::Grid g = RegionEvidence::MakeGrid(int(dims.width), int(dims.height), B);
		// Shift derived from BlockSizeFor(h) in {8, 16, 32, 64}: 3 for 8 px (<=720p), 4 for 16..64 px (>=768p).
		const uint8_t blockShift = B <= 8 ? uint8_t(3) : uint8_t(4);
		VpSkeleton::ProductTexLayout l;
		for (size_t t = 0; t < kTerms; ++t)
		{
			VpSkeleton::TexTerm term;
			term.shift = blockShift;
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
		if (m_store == nullptr || gpu == nullptr)
		{
			m_error = "no texture store for the bands stand-in";
			return false;
		}
		const float zero[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
		m_dummy = m_store->Rgba32f(this, 0, 1, 1, zero, false);
		if (!m_dummy)
			m_error = "no rgba32f stand-in texture: " + m_store->LastError();
		return m_dummy != nullptr;
	}

	bool RegionStatsKernel::Prepare(pl_gpu gpu, pl_dispatch, VpSkeleton::Dims)
	{
		m_header = HeaderGlsl();
		m_body = BodyGlsl();
		if (!EnsureDummy(gpu))
			return false;   // m_error says why
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
		VPV_COMPUTE_ONCE_AT("U4", c.seq, ::ComputeOnce::Variant(c.dims.width, c.dims.height));
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

		// P10's blocks: usable when present in either Compact (term 2) or Full (term 1) storage on the same block grid.
		pl_tex bands = nullptr;
		std::string why;
		const GpuInput* p10 = c.Input(VpSkeleton::kP10GrainSplit);
		if (p10 == nullptr || p10->missing)
			why = "the grain split (P10) has no content for this frame";
		else
		{
			pl_tex b = p10->Term(kBandsTermCompact);
			if (b == nullptr)
				b = p10->Term(kBandsTermFull);
			if (b == nullptr)
				why = "the grain split (P10) has no block energy texture";
			else if (b->params.w == g.gw && b->params.h == g.gh
				&& b->params.format != nullptr && b->params.format->num_components >= 4)
				bands = b;
			else
				why = "the grain split (P10) is on another block grid";
		}
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
		{
			RecordSlotBands(c.out[kP11BandEnergyTerm], false, m_recordedS2);
			return Fail(c, "P11: the stats pass failed to build or dispatch");
		}
		RecordSlotBands(c.out[kP11BandEnergyTerm], !m_noBands, m_recordedS2);
		return true;
	}

	void RegionStatsKernel::Release(pl_gpu gpu)
	{
		(void)gpu;
		ClearRecordedSlots();
		if (m_store != nullptr)
			m_store->Forget(this);   // the store destroys the stand-in
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
