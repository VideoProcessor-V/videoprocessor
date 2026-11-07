// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
#pragma once

// NullTables (SID-C1): what the descale statistic T of a candidate size looks like on a NATIVE picture (null) and
// on a picture really upscaled from that size (alt), per size class. The evidence increment of one evaluation is
// the log-likelihood ratio ln p(T | alt) - ln p(T | null) of two Gaussians.
//
// Classes (checked in this order; the first that fits wins):
//   K8        n within +-2 of N k / 8   (k = 1 .. 7)      8-px coding grids and their harmonics dip here on natives
//   K16       n within +-2 of N k / 16  (k odd)           the same at 16-px
//   Rational  n within +-2 of N a / b, b <= 6, gcd 1      the usual upscale ratios (1/3, 2/3, 1/5 ...)
//   Generic   everything else
//
// PLACEHOLDER VALUES. The calibration run (native reels, per class) will write NullTables.generated.h; until
// then these numbers are CONSERVATIVE: the null is wide and deep for the classes where natives are known to
// dip (F0 on real frames: natives sit at ln D between -0.12 and 0, the 7/8 and 3/4 grids deepest), the alt is
// wide and shallow, and every increment is clamped, so the placeholders can only make the estimator abstain
// MORE than calibrated tables would (a weak 8-bit grainy dip never reaches the thresholds), never claim more.
#include "ChainCommon.h"

namespace SourceIdentity
{
	namespace Chain
	{
		constexpr int kClassK8 = 0;
		constexpr int kClassK16 = 1;
		constexpr int kClassRational = 2;
		constexpr int kClassGeneric = 3;
		constexpr int kNullClassCount = 4;

		struct GaussDist
		{
			double mean = 0.0;
			double sd = 1.0;
		};

		inline const char* NullClassName(int c)
		{
			switch (c)
			{
			case kClassK8: return "k/8";
			case kClassK16: return "k/16";
			case kClassRational: return "rational<=6";
			default: break;
			}
			return "generic";
		}

		// Class of latent size n on an N-pixel axis.
		inline int ClassifyLatent(int n, int N, int rationalQMax = 6, double tol = 2.0)
		{
			const double dn = static_cast<double>(n);
			const double dN = static_cast<double>(N);
			for (int k = 1; k < 8; ++k)
				if (std::abs(dn - dN * k / 8.0) <= tol)
					return kClassK8;
			for (int k = 1; k < 16; k += 2)
				if (std::abs(dn - dN * k / 16.0) <= tol)
					return kClassK16;
			for (int b = 2; b <= rationalQMax; ++b)
				for (int a = 1; a < b; ++a)
					if (GcdInt(a, b) == 1 && std::abs(dn - dN * static_cast<double>(a) / static_cast<double>(b)) <= tol)
						return kClassRational;
			return kClassGeneric;
		}

		struct NullTables
		{
			bool placeholder = true;
			GaussDist nul[kNullClassCount];
			GaussDist alt[kNullClassCount];

			static NullTables Placeholder()
			{
				NullTables t;
				t.placeholder = true;
				t.nul[kClassK8].mean = -0.12;       t.nul[kClassK8].sd = 0.10;
				t.nul[kClassK16].mean = -0.10;      t.nul[kClassK16].sd = 0.08;
				t.nul[kClassRational].mean = -0.04; t.nul[kClassRational].sd = 0.05;
				t.nul[kClassGeneric].mean = -0.05;  t.nul[kClassGeneric].sd = 0.06;
				for (int c = 0; c < kNullClassCount; ++c)
				{
					t.alt[c].mean = -0.30;
					t.alt[c].sd = 0.20;
				}
				return t;
			}

			// ln p(T | alt) - ln p(T | null) for class c (positive favours "upscaled from this size").
			double LogLikelihoodRatio(int c, double stat) const
			{
				if (c < 0 || c >= kNullClassCount)
					c = kClassGeneric;
				const double z0 = (stat - nul[c].mean) / nul[c].sd;
				const double z1 = (stat - alt[c].mean) / alt[c].sd;
				return 0.5 * z0 * z0 - 0.5 * z1 * z1 + std::log(nul[c].sd / alt[c].sd);
			}
		};
	}
}
