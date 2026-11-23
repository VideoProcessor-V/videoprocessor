// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
//
// Temporal Detail v2 — Stage TD-S0 (2026-11-23_TD-consolidated-plan.md s2.1 & s3.2):
// Explicitly labelled, non-silent stand-in registry, status tokens, counters, and 1-line swap
// interfaces for Wave 2-6 and missing dependencies (Rule 4: ZERO silent fallbacks).
//
// Header-only, C++14/17, no Windows headers.
#pragma once

#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <string>

namespace TdV2
{
	// Every missing or Wave 2-6 dependency has an explicit, visible stand-in token (plan s2.1 D1-D12 + runtime fallbacks).
	enum class StandInKind : uint8_t
	{
		P10VariantRegistration = 0,
		P10gGrainSeparator,
		GrainGuardW4c,
		BandMemoW2,
		ComposedVectorsGpu,
		P3bBackwardMotion,
		P3rRefinedMotion,
		LongPairDrift,
		P13SourceRing,
		P15FramePattern,
		P16BlockGrid,
		FocusMapK32,
		TdCgChromaGrid,
		TransformUnknownBound,
		LowConfidenceL3,
		NonFiniteClosure,
		Count
	};

	struct StandInMeta
	{
		StandInKind kind;
		const char* catalogKey;   // Matches FallbackCatalog key convention
		const char* statusText;   // Plain-English Ctrl+I / Config status line
		const char* swapTarget;   // Exact Wave 2-6 / core product for 1-line swap
	};

	inline const StandInMeta& MetaOf(StandInKind kind)
	{
		static const StandInMeta kTable[static_cast<size_t>(StandInKind::Count)] = {
			{ StandInKind::P10VariantRegistration, "td.standin.p10_reg",
			  "P10 variant: td-standin-reg", "P10 variant owner registration (W2-6)" },
			{ StandInKind::P10gGrainSeparator, "td.standin.p10g",
			  "P10g: single-frame stand-in (B 8+4 / H2 GPU pending)", "P10g hybrid separator (B 8+4 + H2)" },
			{ StandInKind::GrainGuardW4c, "td.standin.grain_guard",
			  "grain_guard: W4c port stand-in", "repair/grain_split/GrainGuard.h (kUnitVersion=1)" },
			{ StandInKind::BandMemoW2, "td.standin.band_memo",
			  "band_memo: BandFromBase stand-in", "kBandMemo (id 30)" },
			{ StandInKind::ComposedVectorsGpu, "td.standin.composed_vectors",
			  "composed_vectors: TD U14 stand-in", "kComposedVectors (id 34)" },
			{ StandInKind::P3bBackwardMotion, "td.standin.p3b",
			  "motion: P3b stand-in from P2/P3", "P3b backward motion producer" },
			{ StandInKind::P3rRefinedMotion, "td.standin.p3r",
			  "motion: refinement not bound", "P3r FieldRefineProducer" },
			{ StandInKind::LongPairDrift, "td.standin.long_pair",
			  "drift: reduced (seeded search, 1-session long pair pending)", "1-session hinted NVOF long pair" },
			{ StandInKind::P13SourceRing, "td.standin.p13_ring",
			  "scheduler: render-scope stand-in (lane/P13 off)", "P13 SourceRingFeed + SpareHost" },
			{ StandInKind::P15FramePattern, "td.standin.p15",
			  "frame pattern: stand-in (CPU tile P16)", "P15 QualityPattern live GPU producer" },
			{ StandInKind::P16BlockGrid, "td.standin.p16_grid",
			  "P16 grid: TD stand-in (exact-copy + step map)", "kCodingEvidence GPU 4-ch x 2-tex grid" },
			{ StandInKind::FocusMapK32, "td.standin.focus_map",
			  "focus_map: TD E2/E3 stand-in", "kFocusMap (id 32)" },
			{ StandInKind::TdCgChromaGrid, "td.standin.td_cg",
			  "chroma: coarse band only (TD-CG pending)", "TD-CG P14g coded chroma grid" },
			{ StandInKind::TransformUnknownBound, "td.fallback.transform_unknown",
			  "transform unknown (pixel-domain band bound)", "AVC/HEVC identified transform cell" },
			{ StandInKind::LowConfidenceL3, "td.fallback.low_conf_l3",
			  "L3 low-confidence coarse carry", "Full L2/L1 high-confidence path" },
			{ StandInKind::NonFiniteClosure, "td.fallback.non_finite",
			  "non-finite input closed to zero", "Finite upstream evidence" }
		};
		const size_t idx = static_cast<size_t>(kind);
		if (idx < static_cast<size_t>(StandInKind::Count))
			return kTable[idx];
		return kTable[static_cast<size_t>(StandInKind::NonFiniteClosure)];
	}

	struct StandInSlot
	{
		bool active = false;
		uint64_t hitCount = 0;
		std::string detail;
	};

	// Visible stand-in & fallback telemetry ledger. Every stand-in path MUST call Engage() here.
	class StandInRegistry
	{
	public:
		void Reset()
		{
			for (auto& s : slots_)
			{
				s.active = false;
				s.hitCount = 0;
				s.detail.clear();
			}
		}

		void Engage(StandInKind kind, const char* detail = nullptr)
		{
			const size_t idx = static_cast<size_t>(kind);
			if (idx >= slots_.size())
				return;
			slots_[idx].active = true;
			++slots_[idx].hitCount;
			if (detail != nullptr && detail[0] != '\0')
				slots_[idx].detail = detail;
		}

		void Clear(StandInKind kind)
		{
			const size_t idx = static_cast<size_t>(kind);
			if (idx >= slots_.size())
				return;
			slots_[idx].active = false;
		}

		bool IsActive(StandInKind kind) const
		{
			const size_t idx = static_cast<size_t>(kind);
			return idx < slots_.size() ? slots_[idx].active : false;
		}

		uint64_t HitCount(StandInKind kind) const
		{
			const size_t idx = static_cast<size_t>(kind);
			return idx < slots_.size() ? slots_[idx].hitCount : 0;
		}

		const std::string& Detail(StandInKind kind) const
		{
			static const std::string kEmpty;
			const size_t idx = static_cast<size_t>(kind);
			return idx < slots_.size() ? slots_[idx].detail : kEmpty;
		}

		size_t ActiveCount() const
		{
			size_t n = 0;
			for (const auto& s : slots_)
				if (s.active) ++n;
			return n;
		}

		bool AnyActive() const { return ActiveCount() > 0; }

		// Human-readable single-line summary for Ctrl+I / Config / test logs.
		std::string FormatStatusSummary() const
		{
			std::string out;
			for (size_t i = 0; i < slots_.size(); ++i)
			{
				if (!slots_[i].active)
					continue;
				if (!out.empty())
					out += " | ";
				const auto kind = static_cast<StandInKind>(i);
				out += MetaOf(kind).statusText;
				if (!slots_[i].detail.empty())
				{
					out += " [";
					out += slots_[i].detail;
					out += "]";
				}
			}
			return out.empty() ? std::string("all native products bound") : out;
		}

	private:
		std::array<StandInSlot, static_cast<size_t>(StandInKind::Count)> slots_{};
	};

	// ---- 1-line swap stand-in port for Wave 4c GrainSplit::Guard (STANDIN-W4C-GRAINGUARD, plan s2.1 D3) ----
	// Mirrors GrainSplit::Guard::kUnitVersion = 1 law: if output fine-band variance drops below the allowed
	// retention floor relative to input fine-band variance, compute the exact add-back fraction so own grain
	// is never attenuated. Replacing this port with Wave 4c's GrainGuard.h on merge is a 1-line swap.
	struct TdGuardPort
	{
		static constexpr uint32_t kUnitVersion = 1;
		// Tolerance derived from authoritative grain bar (refs/111 s7.3: RMS within +/-2% -> variance within 0.98^2 = 0.9604).
		static constexpr double kMinVarianceRetention = 0.98 * 0.98;
		static constexpr double kVarianceFloor = 1e-9; // strictly numerical singularity guard

		struct Decision
		{
			bool violated = false;
			double addBackFraction = 0.0; // fraction of (inFine - outFine) to add back in [0, 1]
		};

		static Decision Evaluate(double inFineVar, double outFineVar, StandInRegistry* standIns = nullptr)
		{
			if (standIns != nullptr)
				standIns->Engage(StandInKind::GrainGuardW4c);
			Decision d;
			if (!std::isfinite(inFineVar) || !std::isfinite(outFineVar))
			{
				if (standIns != nullptr)
					standIns->Engage(StandInKind::NonFiniteClosure, "TdGuardPort non-finite variance");
				d.violated = true;
				d.addBackFraction = 1.0;
				return d;
			}
			if (inFineVar <= kVarianceFloor)
				return d;
			const double ratio = outFineVar / inFineVar;
			if (ratio < kMinVarianceRetention)
			{
				d.violated = true;
				// Solve linearly in std-dev space: bring sqrt(outFineVar) back up to 0.98 * sqrt(inFineVar).
				const double targetStd = 0.98 * std::sqrt(inFineVar);
				const double curStd = outFineVar > 0.0 ? std::sqrt(outFineVar) : 0.0;
				const double denom = std::sqrt(inFineVar) - curStd;
				if (denom > kVarianceFloor)
				{
					const double frac = (targetStd - curStd) / denom;
					d.addBackFraction = frac < 0.0 ? 0.0 : (frac > 1.0 ? 1.0 : frac);
				}
				else
				{
					d.addBackFraction = 1.0;
				}
			}
			return d;
		}
	};
}
