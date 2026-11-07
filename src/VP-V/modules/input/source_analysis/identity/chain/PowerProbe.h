// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
#pragma once

// PowerProbe (SID-C1 unit 5): "power by injection". A "native" verdict (no dip anywhere) is only worth something
// if the estimator could have SEEN a dip in this very picture. The probe takes the sampled lines, upscales them
// itself at an off-grid ratio (default 0.61, bicubic, then rounded to the delivered output step like a player
// would), and runs the verifier on that with the known latent size. If the verifier finds its own injected
// upscale the picture has the fine detail and the noise margin the test needs: "seen", and a native verdict
// from the real candidates counts as positive evidence. If it does not, the evaluation is UNSURE with the
// reason it can name: dark (low mean level), flat (almost no line structure) or soft (structure but no fine
// detail above the rounding/dither floor).
//
// The synthetic axis has N' = round(N / ratio) pixels; the real picture plays the latent signal (size N). The
// ratio is off every simple rational and every k/8, k/16 size on purpose, so the probe never lands on a class
// where natives are known to dip.
//
// Use a verifier instance of its own for the probe: its operators are 1.64x longer than the candidates' and would
// otherwise push them out of a shared operator cache.
#include "DescaleVerifier.h"

namespace SourceIdentity
{
	namespace Chain
	{
		struct ProbeSettings
		{
			double ratio = 0.61;            // latent size / synthetic size
			int kernel = 1;                 // bicubic B = 0, C = 0.5
			double seenThreshold = -0.15;   // T at or below this = the injected upscale was seen
			double darkMeanCodes = 100.0;   // mean line level (10-bit codes) below this = dark
			double flatStdCodes = 3.0;      // median line standard deviation below this = flat
			double lineGateFactor = 1.5;        // as the evidence stage
			double minGateFraction = 0.25;
			int minGateLinesFloor = 3;
			double trimFraction = 0.2;
		};

		struct ProbeResult
		{
			bool ran = false;
			bool seen = false;
			double stat = 0.0;
			int gatedLines = 0;
			uint32_t reason = 0;            // kReasonNoPower plus Dark / Flat / Soft when unseen
			double meanCode = 0.0;
			double medianLineStd = 0.0;
			int syntheticAxisLen = 0;
			int syntheticLength = 0;
		};

		class PowerProbe
		{
		public:
			// floorCodes: rounding/dither floor of the delivered picture (10-bit codes); roundStepCodes: the output
			// quantisation step in 10-bit codes (4 for an 8-bit output, 1 for 10-bit, 0 = do not round).
			static bool Run(DescaleVerifier& verifier, const LineSet& ls, double floorCodes, double roundStepCodes,
				const ProbeSettings& ps, ProbeResult& out)
			{
				out = ProbeResult();
				if (ls.count < 1 || ls.length < 128 || ls.axisLen < 64 || !(ps.ratio > 0.2) || !(ps.ratio < 0.95))
				{
					out.reason = kReasonBadInput | kReasonNoPower;
					return false;
				}
				// What the real lines look like (for the reason when the probe is unseen).
				double sumMean = 0.0;
				std::vector<double> stds(static_cast<size_t>(ls.count), 0.0);
				for (int j = 0; j < ls.count; ++j)
				{
					const double* y = ls.Line(j);
					double m = 0.0;
					for (int i = 0; i < ls.length; ++i)
						m += y[i];
					m /= static_cast<double>(ls.length);
					double v = 0.0;
					for (int i = 0; i < ls.length; ++i)
						v += (y[i] - m) * (y[i] - m);
					sumMean += m;
					stds[static_cast<size_t>(j)] = std::sqrt(v / static_cast<double>(ls.length));
				}
				out.meanCode = sumMean / static_cast<double>(ls.count);
				out.medianLineStd = MedianOf(stds.data(), ls.count);

				// Synthetic upscale of every line: N' output pixels from the N real ones.
				const KernelInfo& ki = Kernel(ps.kernel);
				const int r = ki.radius;
				const int taps = 2 * r;
				const int Nn = RoundToInt(static_cast<double>(ls.axisLen) / ps.ratio);
				const double step = static_cast<double>(ls.axisLen) / static_cast<double>(Nn);
				int jlo = -1;
				int jhi = -1;
				for (int j = 0; j < Nn; ++j)
				{
					const double p = (static_cast<double>(j) + 0.5) * step - 0.5;
					const int c = static_cast<int>(std::floor(p)) - r + 1;
					if (c < ls.origin)
						continue;
					if (c + taps - 1 > ls.origin + ls.length - 1)
						break;
					if (jlo < 0)
						jlo = j;
					jhi = j;
				}
				if (jlo < 0 || jhi - jlo + 1 < 128)
				{
					out.reason = kReasonShortLines | kReasonNoPower;
					return false;
				}
				const int L2 = jhi - jlo + 1;
				std::vector<double> w(static_cast<size_t>(L2) * static_cast<size_t>(taps), 0.0);
				std::vector<int> c0(static_cast<size_t>(L2), 0);
				for (int j = 0; j < L2; ++j)
				{
					const double p = (static_cast<double>(jlo + j) + 0.5) * step - 0.5;
					const double fl = std::floor(p);
					TapWeights(ps.kernel, p - fl, &w[static_cast<size_t>(j) * static_cast<size_t>(taps)]);
					c0[static_cast<size_t>(j)] = static_cast<int>(fl) - r + 1 - ls.origin;
				}
				LineSet syn;
				syn.axis = ls.axis;
				syn.axisLen = Nn;
				syn.origin = jlo;
				syn.Resize(ls.count, L2);
				syn.cross = ls.cross;
				for (int l = 0; l < ls.count; ++l)
				{
					const double* y = ls.Line(l);
					double* o = syn.Line(l);
					for (int j = 0; j < L2; ++j)
					{
						const double* ww = &w[static_cast<size_t>(j) * static_cast<size_t>(taps)];
						const double* yy = y + c0[static_cast<size_t>(j)];
						double acc = 0.0;
						for (int s = 0; s < taps; ++s)
							acc += ww[s] * yy[s];
						if (roundStepCodes > 0.0)
							acc = std::floor(acc / roundStepCodes + 0.5) * roundStepCodes;
						o[j] = acc;
					}
				}
				out.syntheticAxisLen = Nn;
				out.syntheticLength = L2;

				VerifyOptions opt;
				opt.exactOnly = true;
				opt.onlyKernel = ps.kernel;
				opt.exactHalfWidth = 0;
				VerifyResult vr;
				const int cand[1] = { ls.axisLen };
				if (!verifier.Evaluate(syn, cand, 1, opt, vr) || vr.candidates.empty() || !vr.candidates[0].valid || !vr.candidates[0].exactDone)
				{
					out.reason = (vr.reason | kReasonNoPower);
					return false;
				}
				out.ran = true;
				const CandidateResult& cr = vr.candidates[0];
				std::vector<double> gated;
				const double floorC = floorCodes > 1e-3 ? floorCodes : 1e-3;
				for (int j = 0; j < cr.lineCount; ++j)
					if (cr.lineBase[static_cast<size_t>(j)] >= ps.lineGateFactor * floorC)
						gated.push_back(cr.lineStat[static_cast<size_t>(j)]);
				out.gatedLines = static_cast<int>(gated.size());
				const int minGate = std::max(ps.minGateLinesFloor, static_cast<int>(std::ceil(ps.minGateFraction * static_cast<double>(cr.lineCount))));
				if (out.gatedLines >= minGate)
				{
					out.stat = TrimmedMeanOf(gated.data(), out.gatedLines, ps.trimFraction);
					out.seen = out.stat <= ps.seenThreshold;
				}
				if (!out.seen)
				{
					out.reason |= kReasonNoPower;
					if (out.meanCode < ps.darkMeanCodes)
						out.reason |= kReasonDark;
					else if (out.medianLineStd < ps.flatStdCodes)
						out.reason |= kReasonFlat;
					else
						out.reason |= kReasonSoft;
				}
				return true;
			}
		};
	}
}
