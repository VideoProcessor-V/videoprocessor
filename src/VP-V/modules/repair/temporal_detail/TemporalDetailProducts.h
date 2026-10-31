// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
//
// TemporalDetail's declared products. The descriptor mirrors design doc 25 section B1 (id, inputs with lag,
// backend, history, size(dims), reads, tier, residency, variants) and maps 1:1 onto core/Products.h when
// package S0 lands; until then it is plain data nothing consumes. Header-only, C++14-compatible, no Windows headers.
#pragma once

#include <cstdint>

#include <VP-V/modules/repair/temporal_detail/TemporalDetail.h>

namespace TemporalDetail
{
	enum class Backend { Ofa, GpuCompute, Cpu, RenderOnly };
	enum class Residency { Transient, Ring, Persistent };
	enum class Variants { Identical, NonIdentical };

	struct InputDecl
	{
		const char* product;   // a product id, "source.luma", or "settings[repair]"
		int lag;               // frames: 0 = same frame, -1 = previous, +d = d frames ahead (queue)
		bool optional;
	};

	// Bit set of the Indicator counters a product honours (reader policies: TemporalDetail.h).
	constexpr uint32_t Bit(Counter c) { return 1u << static_cast<unsigned>(c); }

	struct ProductDecl
	{
		const char* id;
		InputDecl inputs[6];
		int inputCount;
		Backend backend;
		int history;                                  // frames of history the ring keeps
		uint64_t (*sizeBytes)(int w, int h, int K);   // at plane w x h, K past slots
		uint32_t reads;                               // Bit(Counter) mask
		int tier;                                     // memory tier (0 = never dropped)
		Residency residency;
		Variants variants;
	};

	namespace Detail
	{
		// Block maps only: ~ (w/16)(h/16) blocks x 12 floats.
		constexpr uint64_t AnalysisBytes(int w, int h, int) { return static_cast<uint64_t>((w + 15) / 16) * static_cast<uint64_t>((h + 15) / 16) * 12u * 4u; }
		// K slots of R16F structure (2 B/px) plus a half-resolution R16F base (0.5 B/px), plus the future slot.
		constexpr uint64_t RefstoreBytes(int w, int h, int K) { return static_cast<uint64_t>(K + 1) * static_cast<uint64_t>(w) * static_cast<uint64_t>(h) * 5u / 2u; }
		// The output luma plane (R16F).
		constexpr uint64_t OutBytes(int w, int h, int) { return static_cast<uint64_t>(w) * static_cast<uint64_t>(h) * 2u; }
	}

	constexpr uint32_t kReadsAnalysis = Bit(Counter::Full) | Bit(Counter::Continuity) | Bit(Counter::RebaseH) | Bit(Counter::RebaseV);
	constexpr uint32_t kReadsRefstore = kReadsAnalysis | Bit(Counter::SettingsRepair);

	// td.analysis: S/G split, E1/E2/F/q block maps, rebound counters, softening run. Block maps only.
	constexpr ProductDecl kAnalysis = {
		"td.analysis",
		{ { "rebased.luma", 0, false }, { "p4.cut_fade", 0, false }, { "bitrate.quality", 0, true } }, 3,
		Backend::GpuCompute, 1, &Detail::AnalysisBytes, kReadsAnalysis, 1, Residency::Ring, Variants::Identical };

	// td.refstore: K past slots + 1 future slot (S, half-resolution base) and per-block state.
	constexpr ProductDecl kRefstore = {
		"td.refstore",
		{ { "td.analysis", 0, false }, { "motion.p3_p2", 0, false }, { "camera.model", 0, true }, { "queue.frames", 1, true } }, 4,
		Backend::GpuCompute, kSlots, &Detail::RefstoreBytes, kReadsRefstore, 1, Residency::Ring, Variants::Identical };

	// td.out: the output luma plane (structure transferred, grain untouched).
	constexpr ProductDecl kOut = {
		"td.out",
		{ { "td.analysis", 0, false }, { "td.refstore", 0, false } }, 2,
		Backend::RenderOnly, 0, &Detail::OutBytes, kReadsRefstore, 1, Residency::Transient, Variants::Identical };

	constexpr const ProductDecl* kProducts[] = { &kAnalysis, &kRefstore, &kOut };
}
