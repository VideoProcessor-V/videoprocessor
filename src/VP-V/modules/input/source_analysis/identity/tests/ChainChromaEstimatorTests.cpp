/*
 * Copyright(C) 2026 VP-V contributors
 * SPDX-License-Identifier: GPL-3.0-only
 */
// Red-Team adversarial synthetic-physics unit tests for ChainChromaEstimator (SID-F, Stage S5).
// Pure CPU C++14, synthetic only, deterministic SplitMix64 PRNG, zero clocks/sleeps.
// Every separation/siting/corroboration invariant includes an in-test naive/unprotected negative
// control executed on the exact same synthetic input lines (Rule 3).
#include "pch.h"
#include "CppUnitTest.h"

#include <VP-V/modules/input/source_analysis/identity/chain/ChainChromaEstimator.h>
#include <VP-V/modules/input/source_analysis/identity/chain/KernelBank.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

using namespace Microsoft::VisualStudio::CppUnitTestFramework;

namespace Tests
{
	namespace ChromaEstimatorFx
	{
		using namespace SourceIdentity::Chain;

		constexpr double kFloor10Bit = 0.28867513459481288; // 1.0 / sqrt(12.0)

		inline ChromaEstimatorSettings Enabled()
		{
			ChromaEstimatorSettings s;
			s.enabled = true;
			return s;
		}

		// Deterministic SplitMix64 PRNG.
		struct DetRng
		{
			uint64_t state;
			explicit DetRng(uint64_t seed) : state(seed) {}

			uint64_t NextU64()
			{
				uint64_t z = (state += 0x9e3779b97f4a7c15ull);
				z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ull;
				z = (z ^ (z >> 27)) * 0x94d049bb133111ebull;
				return z ^ (z >> 31);
			}

			double UniformCentered()
			{
				const double u = static_cast<double>(NextU64() >> 11) * (1.0 / 9007199254740992.0);
				return u - 0.5;
			}

			double Normal01()
			{
				double u1 = static_cast<double>(NextU64() >> 11) * (1.0 / 9007199254740992.0);
				const double u2 = static_cast<double>(NextU64() >> 11) * (1.0 / 9007199254740992.0);
				if (u1 < 1e-12)
					u1 = 1e-12;
				return std::sqrt(-2.0 * std::log(u1)) * std::cos(2.0 * kPi * u2);
			}
		};

		inline std::vector<double> MakeBroadbandSignal(int n, DetRng& rng, double amp = 90.0)
		{
			std::vector<double> z(static_cast<size_t>(n), 512.0);
			double level = 512.0;
			int nextEdge = 9 + static_cast<int>(rng.NextU64() % 19);
			for (int i = 0; i < n; ++i)
			{
				if (i >= nextEdge)
				{
					level = 512.0 + 2.2 * amp * rng.UniformCentered();
					nextEdge = i + 7 + static_cast<int>(rng.NextU64() % 23);
				}
				z[static_cast<size_t>(i)] = ClampD(level + 0.7 * amp * rng.Normal01(), 80.0, 944.0);
			}
			return z;
		}

		// Exact 1D upscale using KernelBank centre-aligned mapping: p_i = (i + 0.5) * n / N - 0.5 + phase.
		inline std::vector<double> Upscale1D(const std::vector<double>& src, int N, int kernel, double phase = 0.0)
		{
			const int n = static_cast<int>(src.size());
			const int r = Kernel(kernel).radius;
			const int taps = 2 * r;
			std::vector<double> out(static_cast<size_t>(N), 0.0);
			double w[8] = {};
			for (int i = 0; i < N; ++i)
			{
				const double p = (static_cast<double>(i) + 0.5) * static_cast<double>(n) / static_cast<double>(N) - 0.5 + phase;
				const double fl = std::floor(p);
				TapWeights(kernel, p - fl, w);
				double acc = 0.0;
				for (int s = 0; s < taps; ++s)
				{
					int c = static_cast<int>(fl) - r + 1 + s;
					if (c < 0) c = 0;
					else if (c >= n) c = n - 1;
					acc += w[s] * src[static_cast<size_t>(c)];
				}
				out[static_cast<size_t>(i)] = acc;
			}
			return out;
		}

		inline std::vector<double> Quantize10Bit(const std::vector<double>& x)
		{
			std::vector<double> q(x.size(), 0.0);
			for (size_t i = 0; i < x.size(); ++i)
				q[i] = ClampD(std::floor(x[i] + 0.5), 0.0, 1023.0);
			return q;
		}

		inline LineSet MakeLineSet(int axis, int N, int lines)
		{
			LineSet ls;
			ls.axis = axis;
			ls.axisLen = N;
			ls.origin = 0;
			ls.Resize(lines, N);
			for (int j = 0; j < lines; ++j)
				ls.cross[static_cast<size_t>(j)] = (j + 1) * (N / (lines + 1));
			return ls;
		}

		inline double MeanDescaleRms(const LineSet& ls, int n, int kernel, double phase)
		{
			DescaleOperator op;
			if (!op.Build(ls.axisLen, n, kernel, phase, ls.origin, ls.length))
				return 0.0;
			std::vector<double> scratch;
			double segSum[kMaxSegs];
			int segRows[kMaxSegs];
			double acc = 0.0;
			for (int li = 0; li < ls.count; ++li)
			{
				double rss = 0.0;
				int rows = 0;
				op.Residual(ls.Line(li), scratch, rss, rows, segSum, segRows);
				acc += rows > 0 ? std::sqrt(rss / static_cast<double>(rows)) : 0.0;
			}
			return ls.count > 0 ? acc / static_cast<double>(ls.count) : 0.0;
		}

		inline double ComputeLumaDip2x(const LineSet& luma, int kernel)
		{
			const int N = luma.axisLen;
			const int n = N / 2;
			const double sCand = MeanDescaleRms(luma, n, kernel, 0.0);
			const double sLo = MeanDescaleRms(luma, static_cast<int>(std::floor(0.92 * n + 0.5)), kernel, 0.0);
			const double sHi = MeanDescaleRms(luma, static_cast<int>(std::floor(1.08 * n + 0.5)), kernel, 0.0);
			const double base = 0.5 * (sLo + sHi);
			return std::log((sCand + 1e-6) / (base + 1e-6));
		}

		// Evaluates a sharp continuous piecewise-constant colored edge profile at continuous coordinate x.
		// Used to build physically co-located luma + chroma streams under exact siting phases.
		inline double SampleContinuousEdgeProfile(double x, const std::vector<double>& edgePos, const std::vector<double>& levels)
		{
			size_t idx = 0;
			while (idx < edgePos.size() && x >= edgePos[idx])
				++idx;
			return levels[idx];
		}

		// Builds one axis of co-located luma (native N) and 2:1 player-upsampled chroma (n = N/2) at exact phase:
		//   phase = +0.25 => cosited (chroma c sits at luma coordinate x = 2*c)
		//   phase =  0.0  => centered (chroma c sits at luma coordinate x = 2*c + 0.5)
		inline void FillSitedAxis(int axis, int N, int lines, int kernel, double phase, DetRng& rng,
			LineSet& lumaOut, LineSet& cbOut, LineSet& crOut)
		{
			lumaOut = MakeLineSet(axis, N, lines);
			cbOut = MakeLineSet(axis, N, lines);
			crOut = MakeLineSet(axis, N, lines);
			const int n = N / 2;

			for (int li = 0; li < lines; ++li)
			{
				// Build sharp step edges at non-periodic positions across [0, N).
				std::vector<double> edges;
				std::vector<double> yLevels;
				std::vector<double> cbLevels;
				std::vector<double> crLevels;
				yLevels.push_back(512.0 + 180.0 * rng.UniformCentered());
				cbLevels.push_back(512.0 + 160.0 * rng.UniformCentered());
				crLevels.push_back(512.0 + 160.0 * rng.UniformCentered());
				double pos = 18.0 + 12.0 * (rng.UniformCentered() + 0.5);
				while (pos < static_cast<double>(N - 20))
				{
					edges.push_back(pos);
					yLevels.push_back(512.0 + 220.0 * rng.UniformCentered());
					cbLevels.push_back(512.0 + 200.0 * rng.UniformCentered());
					crLevels.push_back(512.0 + 200.0 * rng.UniformCentered());
					pos += 14.0 + 22.0 * (rng.UniformCentered() + 0.5);
				}

				std::vector<double> yLine(static_cast<size_t>(N), 0.0);
				for (int i = 0; i < N; ++i)
				{
					const double v = SampleContinuousEdgeProfile(static_cast<double>(i), edges, yLevels) + 12.0 * rng.Normal01();
					yLine[static_cast<size_t>(i)] = ClampD(std::floor(v + 0.5), 40.0, 980.0);
				}

				// Latent chroma sits at x_c = 2*c + (0.5 - 2*phase):
				//   phase = +0.25 => x_c = 2*c     (co-sited with even luma samples)
				//   phase =  0.0  => x_c = 2*c + 0.5 (centred between luma 2*c and 2*c+1)
				const double xOffset = 0.5 - 2.0 * phase;
				std::vector<double> cbLat(static_cast<size_t>(n), 0.0);
				std::vector<double> crLat(static_cast<size_t>(n), 0.0);
				for (int c = 0; c < n; ++c)
				{
					const double xc = 2.0 * static_cast<double>(c) + xOffset;
					cbLat[static_cast<size_t>(c)] = ClampD(SampleContinuousEdgeProfile(xc, edges, cbLevels) + 45.0 * rng.Normal01(), 60.0, 960.0);
					crLat[static_cast<size_t>(c)] = ClampD(SampleContinuousEdgeProfile(xc, edges, crLevels) + 45.0 * rng.Normal01(), 60.0, 960.0);
				}

				const std::vector<double> cbUp = Quantize10Bit(Upscale1D(cbLat, N, kernel, phase));
				const std::vector<double> crUp = Quantize10Bit(Upscale1D(crLat, N, kernel, phase));
				std::copy(yLine.begin(), yLine.end(), lumaOut.Line(li));
				std::copy(cbUp.begin(), cbUp.end(), cbOut.Line(li));
				std::copy(crUp.begin(), crUp.end(), crOut.Line(li));
			}
		}
	}

	TEST_CLASS(ChainChromaEstimatorTests)
	{
	public:
		TEST_METHOD(DefaultOffAndAchromaticGrayscaleAbstainVisibly)
		{
			using namespace ChromaEstimatorFx;

			ChromaEstimatorSettings def;
			Assert::IsFalse(def.enabled);

			ChromaSampleInput dummy;
			dummy.floorCodes = kFloor10Bit;
			dummy.lumaH = MakeLineSet(0, 512, 8);
			dummy.lumaV = MakeLineSet(1, 512, 8);
			dummy.cbH = MakeLineSet(0, 512, 8);
			dummy.cbV = MakeLineSet(1, 512, 8);
			dummy.crH = MakeLineSet(0, 512, 8);
			dummy.crV = MakeLineSet(1, 512, 8);

			const ChromaChainEstimate off = EstimateChromaChain(dummy, def);
			Assert::IsFalse(off.valid);
			Assert::IsTrue(off.reason == ChromaEstimatorReason::Disabled);
			Assert::IsTrue(off.origin == ChromaOrigin::Unknown);
			Assert::IsFalse(off.playerUpsampled);
			Assert::IsNotNull(ChromaOriginName(off.origin));
			Assert::IsNotNull(ChromaGridSitingName(off.siting));
			Assert::IsNotNull(ChromaEstimatorReasonText(off.reason));

			// Enabled with empty LineSets must visibly report InvalidInput.
			const ChromaEstimatorSettings on = Enabled();
			ChromaSampleInput empty;
			empty.floorCodes = kFloor10Bit;
			const ChromaChainEstimate inv = EstimateChromaChain(empty, on);
			Assert::IsFalse(inv.valid);
			Assert::IsTrue(inv.reason == ChromaEstimatorReason::InvalidInput);

			// Pure flat/grayscale chroma WITH sharp broadband luma: must abstain with LowChromaDetail!
			DetRng rng(2026112310ull);
			constexpr int kN = 512;
			constexpr int kLines = 16;
			ChromaSampleInput gray;
			gray.floorCodes = kFloor10Bit;
			gray.lumaH = MakeLineSet(0, kN, kLines);
			gray.lumaV = MakeLineSet(1, kN, kLines);
			gray.cbH = MakeLineSet(0, kN, kLines);
			gray.cbV = MakeLineSet(1, kN, kLines);
			gray.crH = MakeLineSet(0, kN, kLines);
			gray.crV = MakeLineSet(1, kN, kLines);

			double lumaGradEnergy = 0.0;
			for (int li = 0; li < kLines; ++li)
			{
				const std::vector<double> yh = Quantize10Bit(MakeBroadbandSignal(kN, rng, 110.0));
				const std::vector<double> yv = Quantize10Bit(MakeBroadbandSignal(kN, rng, 110.0));
				std::copy(yh.begin(), yh.end(), gray.lumaH.Line(li));
				std::copy(yv.begin(), yv.end(), gray.lumaV.Line(li));
				for (int i = 1; i < kN; ++i)
				{
					const double d = yh[static_cast<size_t>(i)] - yh[static_cast<size_t>(i - 1)];
					lumaGradEnergy += d * d;
				}
				// Achromatic grayscale chroma: constant 512 (zero color modulation).
				for (int i = 0; i < kN; ++i)
				{
					gray.cbH.Line(li)[i] = 512.0;
					gray.cbV.Line(li)[i] = 512.0;
					gray.crH.Line(li)[i] = 512.0;
					gray.crV.Line(li)[i] = 512.0;
				}
			}

			// MANDATORY Rule 3 Negative Control on EXACT same grayscale input:
			// Luma has huge detail RMS (> 50x floor), so any naive luma-driven gate would think the scene
			// is detailed and guess an origin, whereas chroma detail is zero and must visibly abstain!
			const double lumaGradRms = std::sqrt(lumaGradEnergy / static_cast<double>(kLines * (kN - 1)));
			Assert::IsTrue(lumaGradRms > 50.0 * kFloor10Bit);

			const ChromaChainEstimate gEst = EstimateChromaChain(gray, on);
			Assert::IsFalse(gEst.valid);
			Assert::IsTrue(gEst.origin == ChromaOrigin::Unknown);
			Assert::IsFalse(gEst.playerUpsampled);
			Assert::IsTrue(gEst.reason == ChromaEstimatorReason::LowChromaDetail);
			Assert::AreEqual(0, static_cast<int>(gEst.ToProductChromaChain().playerUpsampled));
		}

		TEST_METHOD(ZeroFalseCallsOnNative444PcVsPlayerUpsampled420)
		{
			using namespace ChromaEstimatorFx;
			const ChromaEstimatorSettings on = Enabled();

			constexpr int kN = 512;
			constexpr int kLines = 16;
			constexpr int kLanczos3 = 5;

			DetRng rng(2026112311ull);

			// Part A: 6 diverse synthetic native 4:4:4 / PC scenes -> 0/6 false 4:2:0 calls!
			int false420Calls = 0;
			for (int scene = 0; scene < 6; ++scene)
			{
				ChromaSampleInput in;
				in.floorCodes = kFloor10Bit;
				in.lumaH = MakeLineSet(0, kN, kLines);
				in.lumaV = MakeLineSet(1, kN, kLines);
				in.cbH = MakeLineSet(0, kN, kLines);
				in.cbV = MakeLineSet(1, kN, kLines);
				in.crH = MakeLineSet(0, kN, kLines);
				in.crV = MakeLineSet(1, kN, kLines);

				for (int li = 0; li < kLines; ++li)
				{
					std::vector<double> yh(static_cast<size_t>(kN), 512.0);
					std::vector<double> yv(static_cast<size_t>(kN), 512.0);
					std::vector<double> cbh(static_cast<size_t>(kN), 512.0);
					std::vector<double> cbv(static_cast<size_t>(kN), 512.0);
					std::vector<double> crh(static_cast<size_t>(kN), 512.0);
					std::vector<double> crv(static_cast<size_t>(kN), 512.0);

					if (scene == 0)
					{
						// Sharp 1-px colored step edges (UI panels / sharp color transitions).
						yh = Quantize10Bit(MakeBroadbandSignal(kN, rng, 100.0));
						yv = Quantize10Bit(MakeBroadbandSignal(kN, rng, 100.0));
						cbh = Quantize10Bit(MakeBroadbandSignal(kN, rng, 95.0));
						cbv = Quantize10Bit(MakeBroadbandSignal(kN, rng, 95.0));
						crh = Quantize10Bit(MakeBroadbandSignal(kN, rng, 95.0));
						crv = Quantize10Bit(MakeBroadbandSignal(kN, rng, 95.0));
					}
					else if (scene == 1)
					{
						// RGB subpixel-like sharp 1-px / 2-px colored text strokes on dark/light background.
						for (int i = 0; i < kN; ++i)
						{
							const bool glyph = ((i + li * 3) % 7 == 0) || ((i + li * 5) % 11 == 1);
							const bool sub = ((i + li) % 3 == 0);
							yh[static_cast<size_t>(i)] = glyph ? 760.0 : 220.0;
							yv[static_cast<size_t>(i)] = ((i + li * 2) % 9 == 0) ? 740.0 : 240.0;
							cbh[static_cast<size_t>(i)] = glyph ? (sub ? 740.0 : 290.0) : (512.0 + 30.0 * rng.Normal01());
							cbv[static_cast<size_t>(i)] = ((i + li * 4) % 7 == 0) ? 720.0 : (512.0 + 30.0 * rng.Normal01());
							crh[static_cast<size_t>(i)] = glyph ? (sub ? 300.0 : 750.0) : (512.0 + 30.0 * rng.Normal01());
							crv[static_cast<size_t>(i)] = ((i + li * 6) % 7 == 2) ? 280.0 : (512.0 + 30.0 * rng.Normal01());
						}
						cbh = Quantize10Bit(cbh); cbv = Quantize10Bit(cbv);
						crh = Quantize10Bit(crh); crv = Quantize10Bit(crv);
					}
					else if (scene == 2)
					{
						// Broadband colored textures up to full Nyquist (0.5 cyc/px).
						for (int i = 0; i < kN; ++i)
						{
							yh[static_cast<size_t>(i)] = ClampD(512.0 + 90.0 * rng.Normal01(), 60.0, 960.0);
							yv[static_cast<size_t>(i)] = ClampD(512.0 + 90.0 * rng.Normal01(), 60.0, 960.0);
							cbh[static_cast<size_t>(i)] = ClampD(512.0 + 80.0 * rng.Normal01(), 60.0, 960.0);
							cbv[static_cast<size_t>(i)] = ClampD(512.0 + 80.0 * rng.Normal01(), 60.0, 960.0);
							crh[static_cast<size_t>(i)] = ClampD(512.0 + 80.0 * rng.Normal01(), 60.0, 960.0);
							crv[static_cast<size_t>(i)] = ClampD(512.0 + 80.0 * rng.Normal01(), 60.0, 960.0);
						}
						yh = Quantize10Bit(yh); yv = Quantize10Bit(yv);
						cbh = Quantize10Bit(cbh); cbv = Quantize10Bit(cbv);
						crh = Quantize10Bit(crh); crv = Quantize10Bit(crv);
					}
					else if (scene == 3)
					{
						// High-saturation 3-px and 5-px colored UI bars (non-2x periodic).
						for (int i = 0; i < kN; ++i)
						{
							const double p3 = ((i / 3 + li) & 1) ? 1.0 : -1.0;
							const double p5 = ((i / 5 + li * 2) & 1) ? 1.0 : -1.0;
							yh[static_cast<size_t>(i)] = 512.0 + 120.0 * p3 + 60.0 * p5;
							yv[static_cast<size_t>(i)] = 512.0 + 120.0 * p5 + 60.0 * p3;
							cbh[static_cast<size_t>(i)] = 512.0 + 140.0 * p3 + 40.0 * rng.Normal01();
							cbv[static_cast<size_t>(i)] = 512.0 + 140.0 * p5 + 40.0 * rng.Normal01();
							crh[static_cast<size_t>(i)] = 512.0 - 140.0 * p5 + 40.0 * rng.Normal01();
							crv[static_cast<size_t>(i)] = 512.0 - 140.0 * p3 + 40.0 * rng.Normal01();
						}
						yh = Quantize10Bit(yh); yv = Quantize10Bit(yv);
						cbh = Quantize10Bit(cbh); cbv = Quantize10Bit(cbv);
						crh = Quantize10Bit(crh); crv = Quantize10Bit(crv);
					}
					else if (scene == 4)
					{
						// Adversarial scene: 2:1-upscaled luma paired with CRISP NATIVE 4:4:4 PC chroma!
						yh = Quantize10Bit(Upscale1D(MakeBroadbandSignal(kN / 2, rng, 100.0), kN, kLanczos3, 0.0));
						yv = Quantize10Bit(Upscale1D(MakeBroadbandSignal(kN / 2, rng, 100.0), kN, kLanczos3, 0.0));
						cbh = Quantize10Bit(MakeBroadbandSignal(kN, rng, 90.0));
						cbv = Quantize10Bit(MakeBroadbandSignal(kN, rng, 90.0));
						crh = Quantize10Bit(MakeBroadbandSignal(kN, rng, 90.0));
						crv = Quantize10Bit(MakeBroadbandSignal(kN, rng, 90.0));
					}
					else
					{
						// Thin 1-px colored hairlines / crosshairs on mid-gray desktop background.
						for (int i = 0; i < kN; ++i)
						{
							const bool hairH = ((i + li * 7) % 13 == 0);
							const bool hairV = ((i + li * 11) % 17 == 0);
							yh[static_cast<size_t>(i)] = hairH ? 820.0 : (480.0 + 10.0 * rng.Normal01());
							yv[static_cast<size_t>(i)] = hairV ? 820.0 : (480.0 + 10.0 * rng.Normal01());
							cbh[static_cast<size_t>(i)] = hairH ? 220.0 : (512.0 + 25.0 * rng.Normal01());
							cbv[static_cast<size_t>(i)] = hairV ? 790.0 : (512.0 + 25.0 * rng.Normal01());
							crh[static_cast<size_t>(i)] = hairH ? 810.0 : (512.0 + 25.0 * rng.Normal01());
							crv[static_cast<size_t>(i)] = hairV ? 210.0 : (512.0 + 25.0 * rng.Normal01());
						}
						yh = Quantize10Bit(yh); yv = Quantize10Bit(yv);
						cbh = Quantize10Bit(cbh); cbv = Quantize10Bit(cbv);
						crh = Quantize10Bit(crh); crv = Quantize10Bit(crv);
					}

					std::copy(yh.begin(), yh.end(), in.lumaH.Line(li));
					std::copy(yv.begin(), yv.end(), in.lumaV.Line(li));
					std::copy(cbh.begin(), cbh.end(), in.cbH.Line(li));
					std::copy(cbv.begin(), cbv.end(), in.cbV.Line(li));
					std::copy(crh.begin(), crh.end(), in.crH.Line(li));
					std::copy(crv.begin(), crv.end(), in.crV.Line(li));
				}

				// MANDATORY Rule 3 Negative Control on EXACT same Scene 4 input:
				// Luma alone shows a strong 2:1 upscale dip (< -0.5), so a naive luma-only heuristic
				// would falsely claim upsampled 4:2:0, while EstimateChromaChain must read Native444!
				if (scene == 4)
				{
					const double lumaDip = ComputeLumaDip2x(in.lumaH, kLanczos3);
					Assert::IsTrue(lumaDip < -0.5);
				}

				const ChromaChainEstimate est = EstimateChromaChain(in, on);
				if (est.origin == ChromaOrigin::Subsampled420 || est.playerUpsampled)
					++false420Calls;

				Assert::IsTrue(est.valid);
				Assert::IsTrue(est.origin == ChromaOrigin::Native444);
				Assert::IsFalse(est.playerUpsampled);
				Assert::IsTrue(est.reason == ChromaEstimatorReason::Native444Confirmed);
				const SourceIdentity::Geometry::ChromaChain prod = est.ToProductChromaChain();
				Assert::AreEqual(1, static_cast<int>(prod.playerUpsampled));
			}
			Assert::AreEqual(0, false420Calls);

			// Part B: Player-upsampled 4:2:0 scenes across Bilinear (0), Bicubic (1), Lanczos3 (5)
			// using standard MPEG-2/H.264/HEVC Left siting (+0.25 H, 0.0 V), where all 3 kernels separate cleanly.
			const int kernels[3] = { 0, 1, 5 };
			for (int ki = 0; ki < 3; ++ki)
			{
				const int k = kernels[ki];
				ChromaSampleInput in;
				in.floorCodes = kFloor10Bit;
				FillSitedAxis(0, kN, kLines, k, 0.25, rng, in.lumaH, in.cbH, in.crH);
				FillSitedAxis(1, kN, kLines, k, 0.0, rng, in.lumaV, in.cbV, in.crV);

				const ChromaChainEstimate est = EstimateChromaChain(in, on);
				Assert::IsTrue(est.valid);
				Assert::IsTrue(est.origin == ChromaOrigin::Subsampled420);
				Assert::IsTrue(est.playerUpsampled);
				Assert::IsTrue(est.reason == ChromaEstimatorReason::PlayerUpsampled420Confirmed);
				Assert::AreEqual(static_cast<int>(Kernel(k).family), est.kernelFamily);
				const SourceIdentity::Geometry::ChromaChain prod = est.ToProductChromaChain();
				Assert::AreEqual(2, static_cast<int>(prod.playerUpsampled));
			}

			// Exact linear-algebra boundary check (Rule 4): at Center (0.0, 0.0) 2:1 on both axes,
			// Catmull-Rom Bicubic factors as U_bicubic = U_bilinear * H with invertible Toeplitz H, so the
			// ranges are algebraically identical; ChainChromaEstimator must honestly report KernelUnsure
			// while still confirming Subsampled420 + Center siting!
			ChromaSampleInput inCenterBilinear;
			inCenterBilinear.floorCodes = kFloor10Bit;
			FillSitedAxis(0, kN, kLines, 0, 0.0, rng, inCenterBilinear.lumaH, inCenterBilinear.cbH, inCenterBilinear.crH);
			FillSitedAxis(1, kN, kLines, 0, 0.0, rng, inCenterBilinear.lumaV, inCenterBilinear.cbV, inCenterBilinear.crV);
			const ChromaChainEstimate estCenterBil = EstimateChromaChain(inCenterBilinear, on);
			Assert::IsTrue(estCenterBil.valid);
			Assert::IsTrue(estCenterBil.origin == ChromaOrigin::Subsampled420);
			Assert::IsTrue(estCenterBil.playerUpsampled);
			Assert::IsTrue(estCenterBil.siting == ChromaGridSiting::Center);
			Assert::IsTrue(estCenterBil.reason == ChromaEstimatorReason::KernelUnsure);

			// Part C: Subsampled 4:2:2 (H upsampled 2:1, V native full-band) -> Subsampled422 (not 4:2:0).
			ChromaSampleInput in422;
			in422.floorCodes = kFloor10Bit;
			FillSitedAxis(0, kN, kLines, 1, 0.0, rng, in422.lumaH, in422.cbH, in422.crH);
			in422.lumaV = MakeLineSet(1, kN, kLines);
			in422.cbV = MakeLineSet(1, kN, kLines);
			in422.crV = MakeLineSet(1, kN, kLines);
			for (int li = 0; li < kLines; ++li)
			{
				const std::vector<double> yv = Quantize10Bit(MakeBroadbandSignal(kN, rng, 100.0));
				const std::vector<double> cbv = Quantize10Bit(MakeBroadbandSignal(kN, rng, 90.0));
				const std::vector<double> crv = Quantize10Bit(MakeBroadbandSignal(kN, rng, 90.0));
				std::copy(yv.begin(), yv.end(), in422.lumaV.Line(li));
				std::copy(cbv.begin(), cbv.end(), in422.cbV.Line(li));
				std::copy(crv.begin(), crv.end(), in422.crV.Line(li));
			}
			const ChromaChainEstimate est422 = EstimateChromaChain(in422, on);
			Assert::IsTrue(est422.valid);
			Assert::IsTrue(est422.origin == ChromaOrigin::Subsampled422);
		}

		TEST_METHOD(DistinguishesTopLeftLeftAndCenterChromaSiting)
		{
			using namespace ChromaEstimatorFx;
			const ChromaEstimatorSettings on = Enabled();

			constexpr int kN = 512;
			constexpr int kLines = 16;
			constexpr int kBicubic = 1;

			DetRng rng(2026112312ull);

			// TopLeft: cosited H (+0.25) + cosited V (+0.25).
			ChromaSampleInput inTopLeft;
			inTopLeft.floorCodes = kFloor10Bit;
			FillSitedAxis(0, kN, kLines, kBicubic, 0.25, rng, inTopLeft.lumaH, inTopLeft.cbH, inTopLeft.crH);
			FillSitedAxis(1, kN, kLines, kBicubic, 0.25, rng, inTopLeft.lumaV, inTopLeft.cbV, inTopLeft.crV);

			// Left: cosited H (+0.25) + centered V (0.0).
			ChromaSampleInput inLeft;
			inLeft.floorCodes = kFloor10Bit;
			FillSitedAxis(0, kN, kLines, kBicubic, 0.25, rng, inLeft.lumaH, inLeft.cbH, inLeft.crH);
			FillSitedAxis(1, kN, kLines, kBicubic, 0.0, rng, inLeft.lumaV, inLeft.cbV, inLeft.crV);

			// Center: centered H (0.0) + centered V (0.0).
			ChromaSampleInput inCenter;
			inCenter.floorCodes = kFloor10Bit;
			FillSitedAxis(0, kN, kLines, kBicubic, 0.0, rng, inCenter.lumaH, inCenter.cbH, inCenter.crH);
			FillSitedAxis(1, kN, kLines, kBicubic, 0.0, rng, inCenter.lumaV, inCenter.cbV, inCenter.crV);

			// MANDATORY Rule 3 Negative Control on EXACT same TopLeft vs Center inputs:
			// Evaluating DescaleOperator with the WRONG siting phase produces strictly higher residual RMS!
			const double topLeftRightRms = MeanDescaleRms(inTopLeft.cbH, kN / 2, kBicubic, 0.25);
			const double topLeftWrongRms = MeanDescaleRms(inTopLeft.cbH, kN / 2, kBicubic, 0.0);
			Assert::IsTrue(topLeftWrongRms > 3.0 * topLeftRightRms);
			Assert::IsTrue(topLeftWrongRms > 5.0 * kFloor10Bit);

			const double centerRightRms = MeanDescaleRms(inCenter.cbH, kN / 2, kBicubic, 0.0);
			const double centerWrongRms = MeanDescaleRms(inCenter.cbH, kN / 2, kBicubic, 0.25);
			Assert::IsTrue(centerWrongRms > 3.0 * centerRightRms);
			Assert::IsTrue(centerWrongRms > 5.0 * kFloor10Bit);

			// Verify EstimateChromaChain distinguishes all three sitings cleanly.
			const ChromaChainEstimate estTopLeft = EstimateChromaChain(inTopLeft, on);
			Assert::IsTrue(estTopLeft.valid);
			Assert::IsTrue(estTopLeft.origin == ChromaOrigin::Subsampled420);
			Assert::IsTrue(estTopLeft.siting == ChromaGridSiting::TopLeft);

			const ChromaChainEstimate estLeft = EstimateChromaChain(inLeft, on);
			Assert::IsTrue(estLeft.valid);
			Assert::IsTrue(estLeft.origin == ChromaOrigin::Subsampled420);
			Assert::IsTrue(estLeft.siting == ChromaGridSiting::Left);

			const ChromaChainEstimate estCenter = EstimateChromaChain(inCenter, on);
			Assert::IsTrue(estCenter.valid);
			Assert::IsTrue(estCenter.origin == ChromaOrigin::Subsampled420);
			Assert::IsTrue(estCenter.siting == ChromaGridSiting::Center);
		}

		TEST_METHOD(QuarterResChromaCorroborates1080pIn4KAndRejectsNative4K420)
		{
			using namespace ChromaEstimatorFx;
			const ChromaEstimatorSettings on = Enabled();

			constexpr int kN = 512;
			constexpr int kLines = 16;
			constexpr int kLanczos3 = 5;

			DetRng rng(2026112313ull);

			// Stream A: 1080p-in-4K 4:2:0 (luma upscaled 2x from N/2 = 256; chroma upsampled 4x from N/4 = 128).
			ChromaSampleInput in1080In4K;
			in1080In4K.floorCodes = kFloor10Bit;
			in1080In4K.lumaH = MakeLineSet(0, kN, kLines);
			in1080In4K.lumaV = MakeLineSet(1, kN, kLines);
			in1080In4K.cbH = MakeLineSet(0, kN, kLines);
			in1080In4K.cbV = MakeLineSet(1, kN, kLines);
			in1080In4K.crH = MakeLineSet(0, kN, kLines);
			in1080In4K.crV = MakeLineSet(1, kN, kLines);

			// Stream B: Native 4K 4:2:0 (luma native N = 512; chroma upsampled 2x from N/2 = 256 with real detail in [1/4, 1/2] Nyquist).
			ChromaSampleInput inNative4K420;
			inNative4K420.floorCodes = kFloor10Bit;
			inNative4K420.lumaH = MakeLineSet(0, kN, kLines);
			inNative4K420.lumaV = MakeLineSet(1, kN, kLines);
			inNative4K420.cbH = MakeLineSet(0, kN, kLines);
			inNative4K420.cbV = MakeLineSet(1, kN, kLines);
			inNative4K420.crH = MakeLineSet(0, kN, kLines);
			inNative4K420.crV = MakeLineSet(1, kN, kLines);

			for (int li = 0; li < kLines; ++li)
			{
				const std::vector<double> yH_1080 = Quantize10Bit(Upscale1D(MakeBroadbandSignal(kN / 2, rng, 100.0), kN, kLanczos3, 0.0));
				const std::vector<double> yV_1080 = Quantize10Bit(Upscale1D(MakeBroadbandSignal(kN / 2, rng, 100.0), kN, kLanczos3, 0.0));
				const std::vector<double> cbH_q = Quantize10Bit(Upscale1D(MakeBroadbandSignal(kN / 4, rng, 95.0), kN, kLanczos3, 0.0));
				const std::vector<double> cbV_q = Quantize10Bit(Upscale1D(MakeBroadbandSignal(kN / 4, rng, 95.0), kN, kLanczos3, 0.0));
				const std::vector<double> crH_q = Quantize10Bit(Upscale1D(MakeBroadbandSignal(kN / 4, rng, 95.0), kN, kLanczos3, 0.0));
				const std::vector<double> crV_q = Quantize10Bit(Upscale1D(MakeBroadbandSignal(kN / 4, rng, 95.0), kN, kLanczos3, 0.0));

				std::copy(yH_1080.begin(), yH_1080.end(), in1080In4K.lumaH.Line(li));
				std::copy(yV_1080.begin(), yV_1080.end(), in1080In4K.lumaV.Line(li));
				std::copy(cbH_q.begin(), cbH_q.end(), in1080In4K.cbH.Line(li));
				std::copy(cbV_q.begin(), cbV_q.end(), in1080In4K.cbV.Line(li));
				std::copy(crH_q.begin(), crH_q.end(), in1080In4K.crH.Line(li));
				std::copy(crV_q.begin(), crV_q.end(), in1080In4K.crV.Line(li));

				const std::vector<double> yH_nat = Quantize10Bit(MakeBroadbandSignal(kN, rng, 100.0));
				const std::vector<double> yV_nat = Quantize10Bit(MakeBroadbandSignal(kN, rng, 100.0));
				const std::vector<double> cbH_420 = Quantize10Bit(Upscale1D(MakeBroadbandSignal(kN / 2, rng, 95.0), kN, kLanczos3, 0.0));
				const std::vector<double> cbV_420 = Quantize10Bit(Upscale1D(MakeBroadbandSignal(kN / 2, rng, 95.0), kN, kLanczos3, 0.0));
				const std::vector<double> crH_420 = Quantize10Bit(Upscale1D(MakeBroadbandSignal(kN / 2, rng, 95.0), kN, kLanczos3, 0.0));
				const std::vector<double> crV_420 = Quantize10Bit(Upscale1D(MakeBroadbandSignal(kN / 2, rng, 95.0), kN, kLanczos3, 0.0));

				std::copy(yH_nat.begin(), yH_nat.end(), inNative4K420.lumaH.Line(li));
				std::copy(yV_nat.begin(), yV_nat.end(), inNative4K420.lumaV.Line(li));
				std::copy(cbH_420.begin(), cbH_420.end(), inNative4K420.cbH.Line(li));
				std::copy(cbV_420.begin(), cbV_420.end(), inNative4K420.cbV.Line(li));
				std::copy(crH_420.begin(), crH_420.end(), inNative4K420.crH.Line(li));
				std::copy(crV_420.begin(), crV_420.end(), inNative4K420.crV.Line(li));
			}

			// On 1080p-in-4K 4:2:0: quarter-res corroboration confirms half-luma upscale!
			const ChromaChainEstimate est1080 = EstimateChromaChain(in1080In4K, on);
			Assert::IsTrue(est1080.valid);
			Assert::IsTrue(est1080.quarter.valid);
			Assert::IsTrue(est1080.quarter.supportsTwoToOneLumaUpscale);
			Assert::IsTrue(est1080.quarter.dipH <= -0.20);
			Assert::IsTrue(est1080.quarter.dipV <= -0.20);
			Assert::IsTrue(est1080.quarter.reason == ChromaEstimatorReason::QuarterCorroborated);

			// MANDATORY Rule 3 Negative Control on Native 4K 4:2:0:
			// Native 4K 4:2:0 DOES have strong 2:1 chroma subsampling dips (so a naive rule that confuses
			// any 4:2:0 dip with 1080p-in-4K would fire falsely!), yet quarter-res corroboration MUST reject it!
			const ChromaChainEstimate estNat4K = EstimateChromaChain(inNative4K420, on);
			Assert::IsTrue(estNat4K.valid);
			Assert::IsTrue(estNat4K.origin == ChromaOrigin::Subsampled420);
			Assert::IsTrue(estNat4K.playerUpsampled);
			Assert::IsTrue(estNat4K.dipH <= -0.20 && estNat4K.dipV <= -0.20); // Proves 4:2:0 subsampling is genuinely present!
			Assert::IsFalse(estNat4K.quarter.supportsTwoToOneLumaUpscale);
			if (estNat4K.quarter.valid)
				Assert::IsTrue(estNat4K.quarter.reason == ChromaEstimatorReason::QuarterNotSupported);
		}

		TEST_METHOD(ChromaOperatorCacheMatchesUncachedBitExactAndIsolatesKeys)
		{
			using namespace ChromaEstimatorFx;
			const ChromaEstimatorSettings on = Enabled();

			constexpr int kLines = 16;
			constexpr int kBicubic = 1;
			DetRng rng(2026112314ull);

			// Geometry 1: (N = 512, origin = 0, length = 512), Bicubic, TopLeft siting (+0.25, +0.25).
			ChromaSampleInput in1;
			in1.floorCodes = kFloor10Bit;
			FillSitedAxis(0, 512, kLines, kBicubic, 0.25, rng, in1.lumaH, in1.cbH, in1.crH);
			FillSitedAxis(1, 512, kLines, kBicubic, 0.25, rng, in1.lumaV, in1.cbV, in1.crV);

			// Geometry 2: cropped interior window (N = 640, origin = 16, length = 608), Bicubic, Left siting (+0.25, 0.0).
			auto sliceLineSet = [](const LineSet& full, int origin, int len) -> LineSet {
				LineSet sub;
				sub.axis = full.axis;
				sub.axisLen = full.axisLen;
				sub.origin = origin;
				sub.Resize(full.count, len);
				sub.cross = full.cross;
				for (int li = 0; li < full.count; ++li)
				{
					const double* src = full.Line(li) + origin;
					double* dst = sub.Line(li);
					std::copy(src, src + len, dst);
				}
				return sub;
			};

			LineSet fullLumaH2, fullCbH2, fullCrH2, fullLumaV2, fullCbV2, fullCrV2;
			FillSitedAxis(0, 640, kLines, kBicubic, 0.25, rng, fullLumaH2, fullCbH2, fullCrH2);
			FillSitedAxis(1, 640, kLines, kBicubic, 0.0, rng, fullLumaV2, fullCbV2, fullCrV2);

			ChromaSampleInput in2;
			in2.floorCodes = kFloor10Bit;
			in2.lumaH = sliceLineSet(fullLumaH2, 16, 608);
			in2.cbH = sliceLineSet(fullCbH2, 16, 608);
			in2.crH = sliceLineSet(fullCrH2, 16, 608);
			in2.lumaV = sliceLineSet(fullLumaV2, 16, 608);
			in2.cbV = sliceLineSet(fullCbV2, 16, 608);
			in2.crV = sliceLineSet(fullCrV2, 16, 608);

			auto assertEstimatesMatchExact = [](const ChromaChainEstimate& a, const ChromaChainEstimate& b) {
				Assert::AreEqual(a.valid, b.valid);
				Assert::IsTrue(a.origin == b.origin);
				Assert::AreEqual(a.playerUpsampled, b.playerUpsampled);
				Assert::AreEqual(a.kernelIndex, b.kernelIndex);
				Assert::AreEqual(a.kernelFamily, b.kernelFamily);
				Assert::IsTrue(a.siting == b.siting);
				Assert::IsTrue(a.reason == b.reason);
				Assert::IsTrue(std::fabs(a.dipH - b.dipH) < 1e-12);
				Assert::IsTrue(std::fabs(a.dipV - b.dipV) < 1e-12);
				Assert::IsTrue(std::fabs(a.confidence - b.confidence) < 1e-12);
				Assert::IsTrue(std::fabs(a.kernelMargin - b.kernelMargin) < 1e-12);
				Assert::IsTrue(std::fabs(a.sitingMargin - b.sitingMargin) < 1e-12);
				Assert::AreEqual(a.quarter.valid, b.quarter.valid);
				Assert::AreEqual(a.quarter.supportsTwoToOneLumaUpscale, b.quarter.supportsTwoToOneLumaUpscale);
				Assert::IsTrue(std::fabs(a.quarter.dipH - b.quarter.dipH) < 1e-12);
				Assert::IsTrue(std::fabs(a.quarter.dipV - b.quarter.dipV) < 1e-12);
				Assert::IsTrue(std::fabs(a.quarter.confidence - b.quarter.confidence) < 1e-12);
			};

			// Uncached references.
			const ChromaChainEstimate ref1 = EstimateChromaChain(in1, on, nullptr);
			const ChromaChainEstimate ref2 = EstimateChromaChain(in2, on, nullptr);
			Assert::IsTrue(ref1.valid && ref1.origin == ChromaOrigin::Subsampled420);
			Assert::IsTrue(ref2.valid && ref2.origin == ChromaOrigin::Subsampled420);

			// Bounded cache with maxEntries = 1: cold miss -> warm hit -> second geometry eviction.
			ChromaOperatorCache cache(1);
			Assert::AreEqual(static_cast<size_t>(0), cache.EntryCount());

			const ChromaChainEstimate cold1 = EstimateChromaChain(in1, on, &cache);
			assertEstimatesMatchExact(cold1, ref1);
			const ChromaOperatorCacheStats stCold = cache.Stats();
			Assert::IsTrue(stCold.misses >= 1u);
			Assert::AreEqual(static_cast<size_t>(1), stCold.entryCount);
			Assert::AreEqual(static_cast<size_t>(1), cache.EntryCount());
			Assert::IsTrue(stCold.totalBytes > 0u);
			Assert::AreEqual(static_cast<uint64_t>(0), stCold.evictions);

			const ChromaChainEstimate warm1 = EstimateChromaChain(in1, on, &cache);
			assertEstimatesMatchExact(warm1, ref1);
			const ChromaOperatorCacheStats stWarm = cache.Stats();
			Assert::AreEqual(stCold.misses, stWarm.misses);
			Assert::IsTrue(stWarm.hits > stCold.hits);
			Assert::AreEqual(static_cast<uint64_t>(0), stWarm.evictions);

			const ChromaChainEstimate cached2 = EstimateChromaChain(in2, on, &cache);
			assertEstimatesMatchExact(cached2, ref2);
			const ChromaOperatorCacheStats stEvict = cache.Stats();
			Assert::IsTrue(stEvict.misses > stWarm.misses);
			Assert::IsTrue(stEvict.evictions >= 1u);
			Assert::AreEqual(static_cast<size_t>(1), stEvict.entryCount);
			Assert::IsTrue(stEvict.totalBytes > 0u);

			// Clear resets active entries and bytes cleanly.
			cache.Clear();
			Assert::AreEqual(static_cast<size_t>(0), cache.EntryCount());
			Assert::AreEqual(static_cast<size_t>(0), cache.Stats().entryCount);
			Assert::AreEqual(static_cast<size_t>(0), cache.Stats().totalBytes);

			// Rectangular frame (640x512, N_H = 640 != 512 = N_V) under ChromaOperatorCache(1):
			// H-axis operator is evicted mid-call when V-axis builds (512, 0, 512); must still match uncached
			// bit-exact with zero dangling references or stale operators across repeated calls!
			ChromaSampleInput inRect;
			inRect.floorCodes = kFloor10Bit;
			inRect.lumaH = fullLumaH2; // N_H = 640, origin = 0, length = 640
			inRect.cbH = fullCbH2;
			inRect.crH = fullCrH2;
			inRect.lumaV = in1.lumaV;  // N_V = 512, origin = 0, length = 512
			inRect.cbV = in1.cbV;
			inRect.crV = in1.crV;

			const ChromaChainEstimate refRect = EstimateChromaChain(inRect, on, nullptr);
			Assert::IsTrue(refRect.valid && refRect.origin == ChromaOrigin::Subsampled420);

			ChromaOperatorCache rectCache(1);
			const ChromaChainEstimate cachedRect1 = EstimateChromaChain(inRect, on, &rectCache);
			assertEstimatesMatchExact(cachedRect1, refRect);
			const ChromaOperatorCacheStats stRect1 = rectCache.Stats();
			Assert::IsTrue(stRect1.misses >= 2u);
			Assert::IsTrue(stRect1.evictions >= 1u);
			Assert::AreEqual(static_cast<size_t>(1), rectCache.EntryCount());

			const ChromaChainEstimate cachedRect2 = EstimateChromaChain(inRect, on, &rectCache);
			assertEstimatesMatchExact(cachedRect2, refRect);
			Assert::IsTrue(rectCache.Stats().evictions > stRect1.evictions);
			Assert::AreEqual(static_cast<size_t>(1), rectCache.EntryCount());

			// MANDATORY Rule 3 Negative Control on EXACT same in2.cbH lines (N = 640, origin = 16, length = 608):
			// Reusing a DescaleOperator built for a mismatched origin (origin = 17 instead of 16) OR mismatched
			// axisLen (N = 608, origin = 0) produces a massive residual explosion, proving (N, origin, length)
			// cache key isolation is physically necessary and non-vacuous!
			auto rmsWithKey = [](const LineSet& ls, int opN, int opLatentN, int kernel, double phase, int opOrigin, int opLen) -> double {
				DescaleOperator op;
				Assert::IsTrue(op.Build(opN, opLatentN, kernel, phase, opOrigin, opLen));
				std::vector<double> scratch;
				double segSum[kMaxSegs];
				int segRows[kMaxSegs];
				double acc = 0.0;
				for (int li = 0; li < ls.count; ++li)
				{
					double rss = 0.0;
					int rows = 0;
					op.Residual(ls.Line(li), scratch, rss, rows, segSum, segRows);
					acc += rows > 0 ? std::sqrt(rss / static_cast<double>(rows)) : 0.0;
				}
				return ls.count > 0 ? acc / static_cast<double>(ls.count) : 0.0;
			};

			const double rightKeyRms = rmsWithKey(in2.cbH, 640, 320, kBicubic, 0.25, 16, 608);
			const double wrongOriginRms = rmsWithKey(in2.cbH, 640, 320, kBicubic, 0.25, 17, 608);
			// Reusing mismatched axisLen N = 608 (with latent n = 320, ratio 320/608 != 320/640) OR reusing
			// (N = 608, n = 304, origin = 0) on an odd-origin slice (origin = 17) both explode residual RMS > 5x!
			const double wrongAxisRms = rmsWithKey(in2.cbH, 608, 320, kBicubic, 0.25, 0, 608);
			const LineSet oddOriginCbH = sliceLineSet(fullCbH2, 17, 608);
			const double oddRightKeyRms = rmsWithKey(oddOriginCbH, 640, 320, kBicubic, 0.25, 17, 608);
			const double oddWrongAxisRms = rmsWithKey(oddOriginCbH, 608, 304, kBicubic, 0.25, 0, 608);
			Assert::IsTrue(wrongOriginRms > 5.0 * rightKeyRms && wrongOriginRms > 5.0 * kFloor10Bit);
			Assert::IsTrue(wrongAxisRms > 5.0 * rightKeyRms && wrongAxisRms > 5.0 * kFloor10Bit);
			Assert::IsTrue(oddWrongAxisRms > 5.0 * oddRightKeyRms && oddWrongAxisRms > 5.0 * kFloor10Bit);
		}
	};
}
