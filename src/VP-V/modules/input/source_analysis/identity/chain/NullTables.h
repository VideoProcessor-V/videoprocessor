// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
#pragma once

// NullTables (SID Stage S3, PKG-SRCID-FLOOR / G-PhaseA): bit-depth-stratified null/alt calibration schema.
//
// Strict depth isolation contract:
//   - N8 (8-bit content, stepCodes >= 4 in 10-bit code space since 2^(10-8) = 4),
//     N10 (10-bit content, stepCodes == 1 or outputBits == 10), and
//     N12 (12-bit content, outputBits >= 12) each have an independent DepthNullTable slot.
//   - Select(depth) NEVER cross-substitutes one bit depth's table for another (e.g. N8 is never used for N10/N12).
//   - Any unknown depth, uncalibrated depth, or depth with invalid parameters visibly falls back to
//     NullTables::Placeholder() with explicit usedPlaceholderFallback = true and fallbackReason (Rule 4).
//
// Rule 1 derivations:
//   - stepCodes >= 4 => N8: in the estimator's 10-bit code domain, 8-bit codes are spaced by 2^(10 - 8) = 4.
//   - RuleOfThreeBoundPerHour() = 3.0 / H: Hanley & Lippman-Hand (1983) 95% confidence upper Poisson rate bound
//     when 0 false positives are observed over H hours (-ln(0.05) = 2.9957 -> 3.0 / H).
//
// Pure C++14, header-only, deterministic, no Windows headers, /W4 /WX clean.
#include "ChainNullTables.h"

#include <cmath>
#include <limits>

namespace SourceIdentity
{
	namespace Chain
	{
		enum class NullTableSourceKind : int
		{
			Placeholder = 0,
			Calibrated = 1
		};

		enum class BitDepthClass : int
		{
			Unknown = 0,
			N8 = 8,
			N10 = 10,
			N12 = 12
		};

		inline const char* NullTableSourceKindName(NullTableSourceKind k)
		{
			switch (k)
			{
			case NullTableSourceKind::Calibrated: return "calibrated";
			case NullTableSourceKind::Placeholder:
			default: return "placeholder";
			}
		}

		inline const char* BitDepthClassName(BitDepthClass d)
		{
			switch (d)
			{
			case BitDepthClass::N8: return "N8";
			case BitDepthClass::N10: return "N10";
			case BitDepthClass::N12: return "N12";
			case BitDepthClass::Unknown:
			default: return "unknown";
			}
		}

		// Classifies effective bit depth from measured 10-bit code step (2^(10 - 8) = 4 for 8-bit) and optional output bit depth.
		inline BitDepthClass ClassifyBitDepthFromStep(int stepCodes, int outputBits = 0)
		{
			if (stepCodes >= 4)
				return BitDepthClass::N8;
			if (outputBits >= 12)
				return BitDepthClass::N12;
			if (stepCodes == 1 || outputBits == 10)
				return BitDepthClass::N10;
			if (outputBits == 8)
				return BitDepthClass::N8;
			return BitDepthClass::Unknown;
		}

		struct DepthNullTable
		{
			BitDepthClass depthClass = BitDepthClass::Unknown;
			NullTableSourceKind sourceKind = NullTableSourceKind::Placeholder;
			bool calibrated = false;
			double sampleHours = 0.0;
			int nativeShots = 0;
			NullTables tables = NullTables::Placeholder();

			// Rule of Three 95% confidence upper FP-rate bound per hour when 0 FPs are observed over sampleHours:
			// -ln(0.05) = 2.99573... -> 3.0 / sampleHours.
			double RuleOfThreeBoundPerHour() const
			{
				return (std::isfinite(sampleHours) && sampleHours > 0.0)
					? (3.0 / sampleHours)
					: std::numeric_limits<double>::infinity();
			}

			bool HasValidParameters() const
			{
				for (int c = 0; c < kNullClassCount; ++c)
				{
					if (!std::isfinite(tables.nul[c].mean) || !std::isfinite(tables.nul[c].sd) || !(tables.nul[c].sd > 0.0))
						return false;
					if (!std::isfinite(tables.alt[c].mean) || !std::isfinite(tables.alt[c].sd) || !(tables.alt[c].sd > 0.0))
						return false;
				}
				return true;
			}

			bool IsCalibrated() const
			{
				return depthClass != BitDepthClass::Unknown &&
					calibrated &&
					sourceKind == NullTableSourceKind::Calibrated &&
					!tables.placeholder &&
					std::isfinite(sampleHours) && sampleHours > 0.0 &&
					nativeShots > 0 &&
					HasValidParameters();
			}

			static DepthNullTable PlaceholderFor(BitDepthClass d)
			{
				DepthNullTable t;
				t.depthClass = d;
				t.sourceKind = NullTableSourceKind::Placeholder;
				t.calibrated = false;
				t.sampleHours = 0.0;
				t.nativeShots = 0;
				t.tables = NullTables::Placeholder();
				return t;
			}
		};

		struct DepthNullTableLookupResult
		{
			NullTables tables = NullTables::Placeholder();
			BitDepthClass requestedDepth = BitDepthClass::Unknown;
			BitDepthClass resolvedDepth = BitDepthClass::Unknown;
			NullTableSourceKind sourceKind = NullTableSourceKind::Placeholder;
			bool calibrated = false;
			bool usedPlaceholderFallback = true;
			const char* fallbackReason = "placeholder_only";
		};

		struct DepthNullTableSet
		{
			DepthNullTable n8 = DepthNullTable::PlaceholderFor(BitDepthClass::N8);
			DepthNullTable n10 = DepthNullTable::PlaceholderFor(BitDepthClass::N10);
			DepthNullTable n12 = DepthNullTable::PlaceholderFor(BitDepthClass::N12);

			// Zero invented calibrated numbers: every depth defaults to conservative Placeholder tables.
			static DepthNullTableSet PlaceholderOnly()
			{
				return DepthNullTableSet();
			}

			// Strict depth isolation: NEVER cross-substitutes N8 for N10/N12 or any depth for another.
			DepthNullTableLookupResult Select(BitDepthClass depth) const
			{
				DepthNullTableLookupResult r;
				r.requestedDepth = depth;
				const DepthNullTable* slot = nullptr;
				switch (depth)
				{
				case BitDepthClass::N8:  slot = &n8;  break;
				case BitDepthClass::N10: slot = &n10; break;
				case BitDepthClass::N12: slot = &n12; break;
				case BitDepthClass::Unknown:
				default:
					r.tables = NullTables::Placeholder();
					r.resolvedDepth = BitDepthClass::Unknown;
					r.sourceKind = NullTableSourceKind::Placeholder;
					r.calibrated = false;
					r.usedPlaceholderFallback = true;
					r.fallbackReason = "unknown_depth_fallback_placeholder";
					return r;
				}

				if (!slot->IsCalibrated() || slot->depthClass != depth)
				{
					r.tables = NullTables::Placeholder();
					r.resolvedDepth = BitDepthClass::Unknown;
					r.sourceKind = NullTableSourceKind::Placeholder;
					r.calibrated = false;
					r.usedPlaceholderFallback = true;
					r.fallbackReason = (slot->calibrated || slot->sourceKind == NullTableSourceKind::Calibrated || slot->depthClass != depth)
						? "invalid_table_params_fallback_placeholder"
						: "depth_uncalibrated_fallback_placeholder";
					return r;
				}

				r.tables = slot->tables;
				r.resolvedDepth = depth;
				r.sourceKind = NullTableSourceKind::Calibrated;
				r.calibrated = true;
				r.usedPlaceholderFallback = false;
				r.fallbackReason = "ok";
				return r;
			}

			bool AnyCalibrated() const
			{
				return (n8.IsCalibrated() && n8.depthClass == BitDepthClass::N8) ||
					(n10.IsCalibrated() && n10.depthClass == BitDepthClass::N10) ||
					(n12.IsCalibrated() && n12.depthClass == BitDepthClass::N12);
			}

			bool AllCalibrated() const
			{
				return (n8.IsCalibrated() && n8.depthClass == BitDepthClass::N8) &&
					(n10.IsCalibrated() && n10.depthClass == BitDepthClass::N10) &&
					(n12.IsCalibrated() && n12.depthClass == BitDepthClass::N12);
			}
		};
	}
}
