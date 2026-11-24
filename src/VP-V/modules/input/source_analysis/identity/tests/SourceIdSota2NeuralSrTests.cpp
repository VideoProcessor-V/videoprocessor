/*
 * Copyright(C) 2026 VP-V contributors
 * SPDX-License-Identifier: GPL-3.0-only
 */
// Red-Team independent adversarial tests for SID-SOTA-2:
// Nonlinear AI / Neural Super-Resolution witness (NeuralSrWitness & ChainEstimator kReasonNeuralSrDetected)
// (Rule 3 mandatory same-input negative controls and adversarial false-positive traps, pure CPU C++14 /W4).
//
// Covers:
//   1. Sota2ShockFilteredSuperResolutionFiresNeuralSrWitnessAndChainReasonBit
//   2. Sota2AdversarialFalsePositiveTrapsAllRejectNeuralSr
//   3. Sota2WorkspaceReuseNonFiniteInputsAndOriginalPathwayForcesOff
#include "pch.h"
#include "CppUnitTest.h"

#include "ChainEstimatorFixtures.h"
#include "GeometryFixtures.h"
#include "W1DescaleRef.h"

#include <VP-V/modules/input/source_analysis/identity/SourceGeometryView.h>
#include <VP-V/modules/input/source_analysis/identity/chain/ChainEstimator.h>
#include <VP-V/modules/input/source_analysis/identity/chain/DescaleVerifier.h>
#include <VP-V/modules/input/source_analysis/identity/chain/KernelBank.h>
#include <VP-V/modules/input/source_analysis/identity/chain/NeuralSrWitness.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <string>
#include <vector>

using namespace Microsoft::VisualStudio::CppUnitTestFramework;

namespace Tests
{
	namespace Sota2NeuralFx
	{
		namespace C = SourceIdentity::Chain;
		namespace G = SourceIdentity::Geometry;

		constexpr int kUhdW = 3840;
		constexpr int kUhdH = 2160;
		constexpr int kBicubic = 1;
		constexpr int kMitchell = 3;
		constexpr int kLanczos3 = 5;

		struct SplitMix64
		{
			uint64_t state;
			explicit SplitMix64(uint64_t seed) : state(seed) {}

			uint64_t NextU64()
			{
				uint64_t z = (state += 0x9e3779b97f4a7c15ull);
				z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ull;
				z = (z ^ (z >> 27)) * 0x94d049bb133111ebull;
				return z ^ (z >> 31);
			}

			double Uniform01()
			{
				return static_cast<double>(NextU64() >> 11) * (1.0 / 9007199254740992.0);
			}

			double Normal01()
			{
				double u1 = Uniform01();
				if (u1 < 1e-12)
					u1 = 1e-12;
				const double u2 = Uniform01();
				return std::sqrt(-2.0 * std::log(u1)) * std::cos(2.0 * C::kPi * u2);
			}
		};

		inline uint16_t Quantize10(double v)
		{
			double q = std::floor(v + 0.5);
			if (q < 64.0)
				q = 64.0;
			if (q > 940.0)
				q = 940.0;
			return static_cast<uint16_t>(q);
		}

		inline void Upscale1D(const std::vector<double>& src, int nOut, int kernel, std::vector<double>& dst)
		{
			const int nIn = static_cast<int>(src.size());
			dst.assign(static_cast<size_t>(nOut), 512.0);
			if (nIn == nOut)
			{
				dst = src;
				return;
			}
			const int r = C::Kernel(kernel).radius;
			const int taps = 2 * r;
			std::vector<double> wt(static_cast<size_t>(taps));
			for (int i = 0; i < nOut; ++i)
			{
				const double p = (static_cast<double>(i) + 0.5) * static_cast<double>(nIn) / static_cast<double>(nOut) - 0.5;
				const double fl = std::floor(p);
				C::TapWeights(kernel, p - fl, wt.data());
				double acc = 0.0;
				for (int s = 0; s < taps; ++s)
				{
					int c = static_cast<int>(fl) - r + 1 + s;
					if (c < 0)
						c = 0;
					if (c > nIn - 1)
						c = nIn - 1;
					acc += wt[static_cast<size_t>(s)] * src[static_cast<size_t>(c)];
				}
				dst[static_cast<size_t>(i)] = acc;
			}
		}

		// Synthesizes a 1080p latent line with piecewise-textured regions separated by strong step edges,
		// upscales 2x to 4K, and (when applyShockSr == true) applies nonlinear Osher-Rudin / Alvarez-Mazorra
		// monotonic shock-filter edge steepening strictly on strong 2x-grid edges (sharp 1-px edges without
		// LTI oscillatory zero crossings, while interiors remain 1080p-bandlimited).
		inline void SynthSteppedTextured2xLine(
			int nIn,
			int nOut,
			int kernel,
			bool applyShockSr,
			SplitMix64& rng,
			std::vector<double>& outLine)
		{
			std::vector<double> src(static_cast<size_t>(nIn), 512.0);
			const int blockLatent = 84;
			const int offset = static_cast<int>(rng.NextU64() % 41ull);
			double ar = 0.0;
			for (int i = 0; i < nIn; ++i)
			{
				const int blk = (i + offset) / blockLatent;
				const double plateau = ((blk & 1) == 0) ? 290.0 : 630.0;
				ar = 0.55 * ar + 16.0 * rng.Normal01();
				const double tex = C::ClampD(ar, -48.0, 48.0);
				src[static_cast<size_t>(i)] = plateau + tex;
			}

			Upscale1D(src, nOut, kernel, outLine);

			if (applyShockSr)
			{
				std::vector<double> prev;
				for (int iter = 0; iter < 6; ++iter)
				{
					prev = outLine;
					for (int x = 2; x < nOut - 2; ++x)
					{
						const double gxWide = 0.5 * (prev[static_cast<size_t>(x + 2)] - prev[static_cast<size_t>(x - 2)]);
						if (std::fabs(gxWide) <= 45.0)
							continue; // keep 1080p interior texture untouched (non-hallucinated)
						const double gx = 0.5 * (prev[static_cast<size_t>(x + 1)] - prev[static_cast<size_t>(x - 1)]);
						const double lap = prev[static_cast<size_t>(x - 1)] - 2.0 * prev[static_cast<size_t>(x)] + prev[static_cast<size_t>(x + 1)];
						const double shock = -0.45 * std::tanh(lap / 10.0) * std::fabs(gx);
						const double lo = (std::min)(prev[static_cast<size_t>(x - 2)], prev[static_cast<size_t>(x + 2)]);
						const double hi = (std::max)(prev[static_cast<size_t>(x - 2)], prev[static_cast<size_t>(x + 2)]);
						outLine[static_cast<size_t>(x)] = C::ClampD(prev[static_cast<size_t>(x)] + shock, lo, hi);
					}
				}
			}

			for (int i = 0; i < nOut; ++i)
				outLine[static_cast<size_t>(i)] = static_cast<double>(Quantize10(outLine[static_cast<size_t>(i)]));
		}

		// Adversarial Trap (a): Genuine native 4K natural 1/f^2 broadband texture + sharp native 4K step edges.
		// Synthesizes exact 1/f amplitude (1/f^2 PSD) random-phase Fourier series across the full 4K Nyquist
		// band [1 .. N/2] via circularly-free multi-harmonic synthesis, plus crisp 1-px native edges so both
		// edge and interior sample gates are strongly populated.
		inline void SynthNative1OverF2Line(int nOut, SplitMix64& rng, std::vector<double>& outLine)
		{
			outLine.assign(static_cast<size_t>(nOut), 0.0);
			// Exact 1/f amplitude spectrum over dense log/linear harmonics up to Nyquist f = 0.495:
			constexpr int kModes = 192;
			double freqs[kModes];
			double phases[kModes];
			double amps[kModes];
			for (int m = 0; m < kModes; ++m)
			{
				const double u = (static_cast<double>(m) + 0.5) / static_cast<double>(kModes);
				const double f = 0.008 * std::pow(0.495 / 0.008, u); // spans [0.008, 0.495] cycles/px smoothly
				// Compensate log-spacing density df ~ f so power per unit linear frequency df follows exact 1/f^2!
				const double dfWeight = std::sqrt(f);
				freqs[m] = f;
				phases[m] = 2.0 * C::kPi * rng.Uniform01();
				amps[m] = (1.6 * dfWeight / f) * (0.7 + 0.6 * rng.Uniform01());
			}

			const int stepPeriod = 220;
			const int stepOff = static_cast<int>(rng.NextU64() % 97ull);
			for (int x = 0; x < nOut; ++x)
			{
				double acc = 0.0;
				const double xd = static_cast<double>(x);
				for (int m = 0; m < kModes; ++m)
					acc += amps[m] * std::cos(2.0 * C::kPi * freqs[m] * xd + phases[m]);
				const double edge = (((x + stepOff) / stepPeriod) % 2 == 0) ? 360.0 : 620.0;
				outLine[static_cast<size_t>(x)] = static_cast<double>(Quantize10(edge + acc));
			}
		}

		// Adversarial Trap (b): Crisp vector graphics / sharp 1-px step edges on completely flat background (FFT Gibbs trap).
		inline void SynthCrispVectorGraphicsLine(int nOut, SplitMix64& rng, std::vector<double>& outLine)
		{
			outLine.assign(static_cast<size_t>(nOut), 160.0);
			const int period = 180;
			const int off = static_cast<int>(rng.NextU64() % 73ull);
			for (int x = 0; x < nOut; ++x)
			{
				const int p = (x + off) % period;
				outLine[static_cast<size_t>(x)] = (p < 70) ? 840.0 : ((p < 120) ? 480.0 : 160.0);
			}
		}

		// Adversarial Trap (d): Heavy native 4K film grain over sharp native 4K edges.
		inline void SynthHeavyNativeFilmGrainLine(int nOut, SplitMix64& rng, std::vector<double>& outLine)
		{
			outLine.resize(static_cast<size_t>(nOut));
			const int period = 200;
			const int off = static_cast<int>(rng.NextU64() % 61ull);
			for (int x = 0; x < nOut; ++x)
			{
				const double base = (((x + off) / period) % 2 == 0) ? 330.0 : 650.0;
				const double grain = 20.0 * rng.Normal01();
				outLine[static_cast<size_t>(x)] = static_cast<double>(Quantize10(base + grain));
			}
		}

		enum class SceneKind : int
		{
			ShockNeuralSr2x = 0,
			ClassicalLanczos2x = 1,
			ClassicalBicubic2x = 2,
			Native1OverF2 = 3,
			VectorGraphicsFlat = 4,
			HeavyNativeGrain = 5
		};

		inline C::LineSnapshot BuildSceneSnapshot(SceneKind kind, uint64_t seq, int64_t timeUs, uint64_t seed)
		{
			C::LineRect active{ 0, 0, kUhdW, kUhdH };
			const C::LinePlan plan = C::LineSampler::Plan(active, seq);
			Assert::IsTrue(plan.valid);

			C::LineSnapshot snap;
			snap.seq = seq;
			snap.timeUs = timeUs;
			snap.rasterW = kUhdW;
			snap.rasterH = kUhdH;
			snap.active = plan.active;
			snap.inner = plan.inner;
			snap.rowCount = plan.rowCount;
			snap.colCount = plan.colCount;
			snap.rowY.assign(plan.rowY, plan.rowY + plan.rowCount);
			snap.colX.assign(plan.colX, plan.colX + plan.colCount);
			snap.rows.assign(static_cast<size_t>(plan.rowCount) * static_cast<size_t>(plan.inner.w), 512);
			snap.cols.assign(static_cast<size_t>(plan.colCount) * static_cast<size_t>(plan.inner.h), 512);
			snap.flags = C::kEvCut;
			snap.cuts = 1;
			snap.eventTimeUs = timeUs;

			SplitMix64 rng(seed);
			std::vector<double> line;
			auto synthOne = [&](int nOut) {
				switch (kind)
				{
				case SceneKind::ShockNeuralSr2x:
					SynthSteppedTextured2xLine(nOut / 2, nOut, kMitchell, true, rng, line);
					break;
				case SceneKind::ClassicalLanczos2x:
					SynthSteppedTextured2xLine(nOut / 2, nOut, kLanczos3, false, rng, line);
					break;
				case SceneKind::ClassicalBicubic2x:
					SynthSteppedTextured2xLine(nOut / 2, nOut, kBicubic, false, rng, line);
					break;
				case SceneKind::Native1OverF2:
					SynthNative1OverF2Line(nOut, rng, line);
					break;
				case SceneKind::VectorGraphicsFlat:
					SynthCrispVectorGraphicsLine(nOut, rng, line);
					break;
				case SceneKind::HeavyNativeGrain:
					SynthHeavyNativeFilmGrainLine(nOut, rng, line);
					break;
				}
			};

			for (int j = 0; j < plan.rowCount; ++j)
			{
				synthOne(kUhdW);
				uint16_t* dst = &snap.rows[static_cast<size_t>(j) * static_cast<size_t>(plan.inner.w)];
				for (int i = 0; i < plan.inner.w; ++i)
					dst[i] = static_cast<uint16_t>(line[static_cast<size_t>(plan.inner.x + i)]);
			}
			for (int j = 0; j < plan.colCount; ++j)
			{
				synthOne(kUhdH);
				uint16_t* dst = &snap.cols[static_cast<size_t>(j) * static_cast<size_t>(plan.inner.h)];
				for (int i = 0; i < plan.inner.h; ++i)
					dst[i] = static_cast<uint16_t>(line[static_cast<size_t>(plan.inner.y + i)]);
			}
			return snap;
		}
	}

	TEST_CLASS(SourceIdSota2NeuralSrTests)
	{
	public:
		// 1. Genuine nonlinear shock-filtered super-resolved 1080p->4K image (sharp 1-px edges at 2x grid,
		//    missing LTI zero crossings, collapsed interior PSD above 0.5 Nyquist):
		//      - Fires NeuralSrVerdict::NeuralSrDetected and NeuralSrReason::NeuralEdgeLockAndInteriorDrop;
		//      - Fires kReasonNeuralSrDetected ("AI-upscaled (skip LTI inverse)") and increments neuralSrDetections
		//        in ChainEstimator;
		//      - Mandatory same-input negative control (neuralSr.enabled == false on identical snapshot) leaves
		//        kReasonNeuralSrDetected unset and neuralSrDetections == 0.
		TEST_METHOD(Sota2ShockFilteredSuperResolutionFiresNeuralSrWitnessAndChainReasonBit)
		{
			using namespace Sota2NeuralFx;

			const C::NeuralSrSettings defCfg;
			Assert::IsFalse(defCfg.enabled, L"NeuralSrSettings must default to OFF");
			Assert::AreEqual(std::string("AI-upscaled (skip LTI inverse)"),
				SourceGeometry::ReasonWords(C::kReasonNeuralSrDetected),
				L"ReasonWords(kReasonNeuralSrDetected) must render exact contract string");

			const C::LineSnapshot srSnap = BuildSceneSnapshot(SceneKind::ShockNeuralSr2x, 10, 1000000, 0x2201ull);
			const double floorCodes = 1.0 / std::sqrt(12.0);

			C::LineSet lsH, lsV;
			Assert::IsTrue(C::ToLineSet(srSnap, 0, lsH));
			Assert::IsTrue(C::ToLineSet(srSnap, 1, lsV));

			// Verify classical LTI DescaleVerifier has NO deep dip at 1920/1080 because shock-steepened 1-px edges
			// violate LTI kernel subspaces (bestLtiStat > -0.09):
			C::DescaleVerifier ver;
			C::VerifyOptions vOpt;
			const int candsH[2] = { 1920, 2560 };
			const int candsV[2] = { 1080, 1440 };
			C::VerifyResult vrH, vrV;
			Assert::IsTrue(ver.Evaluate(lsH, candsH, 2, vOpt, vrH));
			Assert::IsTrue(ver.Evaluate(lsV, candsV, 2, vOpt, vrV));
			Assert::IsTrue(vrH.candidates[0].valid && vrH.candidates[0].exactDone);
			Assert::IsTrue(vrV.candidates[0].valid && vrV.candidates[0].exactDone);
			Assert::IsTrue(vrH.candidates[0].stat > defCfg.ltiDipVeto,
				L"fixture: shock-filtered SR edges break LTI subspace so classical LTI stat sits above ltiDipVeto (-0.09)");
			Assert::IsTrue(vrV.candidates[0].stat > defCfg.ltiDipVeto);

			// Direct NeuralSrWitness evaluation on H and V:
			C::NeuralSrSettings onCfg;
			onCfg.enabled = true;
			C::NeuralSrWorkspace ws;
			const C::NeuralSrResult resH = C::NeuralSrWitness::Evaluate(lsH, 1920, vrH.candidates[0].stat, floorCodes, onCfg, ws);
			const C::NeuralSrResult resV = C::NeuralSrWitness::Evaluate(lsV, 1080, vrV.candidates[0].stat, floorCodes, onCfg, ws);

			Assert::IsTrue(resH.neuralUpscaled, L"NeuralSrWitness must detect shock-filtered 2x SR on H");
			Assert::IsTrue(resH.verdict == C::NeuralSrVerdict::NeuralSrDetected);
			Assert::IsTrue(resH.reason == C::NeuralSrReason::NeuralEdgeLockAndInteriorDrop);
			Assert::IsTrue(resH.edgePhaseBicoherence >= onCfg.minBicoherence);
			Assert::IsTrue(resH.edgeToInteriorHfRatio >= onCfg.minEdgeToInteriorHfRatio);
			Assert::IsTrue(resH.interiorSpectralDropExcessDecades >= onCfg.interiorDropDecadesMargin);
			Assert::IsTrue(resH.validLines >= onCfg.minValidLines);

			Assert::IsTrue(resV.neuralUpscaled, L"NeuralSrWitness must detect shock-filtered 2x SR on V");
			Assert::IsTrue(resV.verdict == C::NeuralSrVerdict::NeuralSrDetected);
			Assert::IsTrue(resV.reason == C::NeuralSrReason::NeuralEdgeLockAndInteriorDrop);

			// Direct disabled negative control on identical LineSet:
			const C::NeuralSrResult resOff = C::NeuralSrWitness::Evaluate(lsH, 1920, vrH.candidates[0].stat, floorCodes, defCfg, ws);
			Assert::IsFalse(resOff.neuralUpscaled);
			Assert::IsTrue(resOff.verdict == C::NeuralSrVerdict::Abstain);
			Assert::IsTrue(resOff.reason == C::NeuralSrReason::Disabled);

			// End-to-end ChainEstimator with neuralSr.enabled = true vs enabled = false negative control on identical srSnap:
			{
				ChainFx::ManualClock clk;
				clk.Set(1000.0);
				C::EstimatorSettings cfg;
				cfg.outputStepOverride = 1;
				cfg.neuralSr.enabled = true;
				C::ChainEstimator est(cfg, clk.Fn());
				est.SetSynchronous(true);
				est.SetMode(C::EstimatorMode::On);
				Assert::IsTrue(est.SubmitLines(srSnap));

				G::Record rec;
				Assert::IsTrue(est.Held().ReadLatest(0, rec));
				const C::EstimatorCounters cnt = est.GetCounters();
				Assert::IsTrue(cnt.neuralSrDetections >= 1u, L"ChainEstimator must increment neuralSrDetections");
				Assert::IsTrue(((rec.measured[0].reasonBits | rec.measured[1].reasonBits) & C::kReasonNeuralSrDetected) != 0u,
					L"ChainEstimator must set kReasonNeuralSrDetected on measured geometry");
				Assert::IsTrue(((rec.applied[0].reasonBits | rec.applied[1].reasonBits) & C::kReasonNeuralSrDetected) != 0u,
					L"ChainEstimator must set kReasonNeuralSrDetected on applied geometry");

				const std::string stLine = est.StatusLine();
				Assert::IsTrue(stLine.find("AI-upscaled (skip LTI inverse)") != std::string::npos,
					L"ChainEstimator StatusLine() must surface 'AI-upscaled (skip LTI inverse)'");

				const SourceGeometry::Delivered deliv{ kUhdW, kUhdH };
				const SourceGeometry::ConfidentGeometry cg = SourceGeometry::ConfidentOf(rec, deliv);
				Assert::IsFalse(cg.AnyConfident(), L"AI-upscaled stream must skip confident LTI inverse claim");
				est.Stop();
			}

			// Mandatory in-test negative control: default-OFF ChainEstimator on the EXACT SAME srSnap:
			{
				ChainFx::ManualClock clk;
				clk.Set(1000.0);
				C::EstimatorSettings defEstCfg;
				defEstCfg.outputStepOverride = 1;
				Assert::IsFalse(defEstCfg.neuralSr.enabled);
				C::ChainEstimator estDef(defEstCfg, clk.Fn());
				estDef.SetSynchronous(true);
				estDef.SetMode(C::EstimatorMode::On);
				Assert::IsTrue(estDef.SubmitLines(srSnap));

				G::Record recDef;
				Assert::IsTrue(estDef.Held().ReadLatest(0, recDef));
				Assert::AreEqual(uint64_t(0), estDef.GetCounters().neuralSrDetections,
					L"negative control: default-OFF ChainEstimator must report 0 neuralSrDetections");
				Assert::AreEqual(uint32_t(0), recDef.measured[0].reasonBits & C::kReasonNeuralSrDetected);
				Assert::AreEqual(uint32_t(0), recDef.applied[0].reasonBits & C::kReasonNeuralSrDetected);
				Assert::IsTrue(estDef.StatusLine().find("AI-upscaled") == std::string::npos);
				estDef.Stop();
			}
		}

		// 2. Mandatory adversarial false-positive negative controls (MUST NEVER fire NeuralSrDetected!):
		//      (a) Native sharp natural 1/f^2 4K texture (with sharp native 4K geometric edges),
		//      (b) Crisp vector graphics / sharp step edges on flat background (FFT Gibbs trap),
		//      (c) Classical Lanczos3 & Bicubic 1080p->4K upscales on the identical underlying scene,
		//      (d) Heavy native 4K film grain.
		TEST_METHOD(Sota2AdversarialFalsePositiveTrapsAllRejectNeuralSr)
		{
			using namespace Sota2NeuralFx;

			const double floorCodes = 1.0 / std::sqrt(12.0);
			C::NeuralSrSettings onCfg;
			onCfg.enabled = true;
			C::NeuralSrWorkspace ws;
			C::DescaleVerifier ver;
			C::VerifyOptions vOpt;
			const int candsH[2] = { 1920, 2560 };

			auto assertChainNeverFiresNeuralSr = [&](const C::LineSnapshot& snap, const wchar_t* label, bool expectAbstain) {
				ChainFx::ManualClock clk;
				clk.Set(1000.0);
				C::EstimatorSettings cfg;
				cfg.outputStepOverride = 1;
				cfg.neuralSr.enabled = true;
				C::ChainEstimator est(cfg, clk.Fn());
				est.SetSynchronous(true);
				est.SetMode(C::EstimatorMode::On);
				Assert::IsTrue(est.SubmitLines(snap), label);

				G::Record rec;
				Assert::IsTrue(est.Held().ReadLatest(0, rec), label);
				const C::EstimatorCounters cnt = est.GetCounters();
				Assert::AreEqual(uint64_t(0), cnt.neuralSrDetections, label);
				if (expectAbstain)
					Assert::IsTrue(cnt.neuralSrAbstains >= 1u, label);
				Assert::AreEqual(uint32_t(0), rec.measured[0].reasonBits & C::kReasonNeuralSrDetected, label);
				Assert::AreEqual(uint32_t(0), rec.measured[1].reasonBits & C::kReasonNeuralSrDetected, label);
				Assert::AreEqual(uint32_t(0), rec.applied[0].reasonBits & C::kReasonNeuralSrDetected, label);
				Assert::AreEqual(uint32_t(0), rec.applied[1].reasonBits & C::kReasonNeuralSrDetected, label);
				Assert::IsTrue(est.StatusLine().find("AI-upscaled") == std::string::npos, label);
				est.Stop();
			};

			// (a) Native sharp natural 1/f^2 4K texture + sharp native edges:
			{
				const C::LineSnapshot natSnap = BuildSceneSnapshot(SceneKind::Native1OverF2, 20, 2000000, 0x3301ull);
				C::LineSet lsH;
				Assert::IsTrue(C::ToLineSet(natSnap, 0, lsH));
				C::VerifyResult vr;
				Assert::IsTrue(ver.Evaluate(lsH, candsH, 2, vOpt, vr));
				const C::NeuralSrResult r = C::NeuralSrWitness::Evaluate(lsH, 1920, vr.candidates[0].stat, floorCodes, onCfg, ws);
				Assert::IsFalse(r.neuralUpscaled, L"Trap (a): native sharp 1/f^2 4K texture MUST NOT fire NeuralSrWitness");
				Assert::IsTrue(r.verdict == C::NeuralSrVerdict::NotNeural);
				Assert::IsTrue(r.reason == C::NeuralSrReason::NaturalBroadbandSpectrum);
				Assert::IsTrue(r.edgeSamples >= onCfg.minEdgeSamples, L"fixture: Trap (a) genuinely exercises edge path");
				Assert::IsTrue(r.interiorSamples >= onCfg.minInteriorSamples, L"fixture: Trap (a) genuinely exercises interior path");
				assertChainNeverFiresNeuralSr(natSnap, L"Trap (a): native 1/f^2 4K", false);
			}

			// (b) Crisp vector graphics / sharp step edges on flat background (FFT Gibbs trap):
			{
				const C::LineSnapshot vecSnap = BuildSceneSnapshot(SceneKind::VectorGraphicsFlat, 30, 3000000, 0x3302ull);
				C::LineSet lsH;
				Assert::IsTrue(C::ToLineSet(vecSnap, 0, lsH));
				const C::NeuralSrResult r = C::NeuralSrWitness::Evaluate(lsH, 1920, 0.0, floorCodes, onCfg, ws);
				Assert::IsFalse(r.neuralUpscaled, L"Trap (b): crisp vector graphics on flat background MUST NOT fire NeuralSrWitness");
				Assert::IsTrue(r.verdict == C::NeuralSrVerdict::Abstain);
				Assert::IsTrue(r.reason == C::NeuralSrReason::InsufficientInteriorTexture,
					L"Trap (b): raw-signal interior activity gate must reject FFT Gibbs ringing on flat backgrounds");
				Assert::IsTrue(r.edgeSamples >= onCfg.minEdgeSamples, L"fixture: Trap (b) has strong step edges");
				assertChainNeverFiresNeuralSr(vecSnap, L"Trap (b): crisp vector graphics", true);
			}

			// (c) Classical Lanczos3 & Bicubic 1080p->4K upscales (same underlying scene WITHOUT shock steepening):
			{
				const SceneKind ltiKinds[2] = { SceneKind::ClassicalLanczos2x, SceneKind::ClassicalBicubic2x };
				for (int k = 0; k < 2; ++k)
				{
					const C::LineSnapshot ltiSnap = BuildSceneSnapshot(ltiKinds[k], static_cast<uint64_t>(40 + k), 4000000, 0x2201ull);
					C::LineSet lsH;
					Assert::IsTrue(C::ToLineSet(ltiSnap, 0, lsH));
					C::VerifyResult vr;
					Assert::IsTrue(ver.Evaluate(lsH, candsH, 2, vOpt, vr));
					Assert::IsTrue(vr.candidates[0].stat <= onCfg.ltiDipVeto,
						L"fixture: classical Lanczos3/Bicubic 2x upscale produces deep LTI descale dip <= -0.09");

					// Pillar 1 LTI veto path:
					const C::NeuralSrResult rVeto = C::NeuralSrWitness::Evaluate(lsH, 1920, vr.candidates[0].stat, floorCodes, onCfg, ws);
					Assert::IsFalse(rVeto.neuralUpscaled, L"Trap (c): classical LTI upscale MUST NOT fire NeuralSrWitness");
					Assert::IsTrue(rVeto.verdict == C::NeuralSrVerdict::NotNeural);
					Assert::IsTrue(rVeto.reason == C::NeuralSrReason::ClassicalLtiConfirmed);

					// Adversarial Pillar-2/3 check even if Pillar 1 is bypassed (bestLtiStat = 0.0):
					const C::NeuralSrResult rNoVeto = C::NeuralSrWitness::Evaluate(lsH, 1920, 0.0, floorCodes, onCfg, ws);
					Assert::IsFalse(rNoVeto.neuralUpscaled,
						L"Trap (c): classical LTI upscale has bandlimited edges and must not fire even without Pillar 1 veto");

					assertChainNeverFiresNeuralSr(ltiSnap, L"Trap (c): classical LTI 1080p->4K", false);
				}
			}

			// (d) Heavy native 4K film grain:
			{
				const C::LineSnapshot grainSnap = BuildSceneSnapshot(SceneKind::HeavyNativeGrain, 50, 5000000, 0x3304ull);
				C::LineSet lsH;
				Assert::IsTrue(C::ToLineSet(grainSnap, 0, lsH));
				C::VerifyResult vr;
				Assert::IsTrue(ver.Evaluate(lsH, candsH, 2, vOpt, vr));
				const C::NeuralSrResult r = C::NeuralSrWitness::Evaluate(lsH, 1920, vr.candidates[0].stat, floorCodes, onCfg, ws);
				Assert::IsFalse(r.neuralUpscaled, L"Trap (d): heavy native 4K film grain MUST NOT fire NeuralSrWitness");
				Assert::IsTrue(r.verdict == C::NeuralSrVerdict::NotNeural);
				Assert::IsTrue(r.reason == C::NeuralSrReason::NaturalBroadbandSpectrum);
				assertChainNeverFiresNeuralSr(grainSnap, L"Trap (d): heavy native 4K film grain", false);
			}
		}

		// 3. Workspace reuse bit-identity, non-finite / invalid input guards, and Original pathway forces OFF.
		TEST_METHOD(Sota2WorkspaceReuseNonFiniteInputsAndOriginalPathwayForcesOff)
		{
			using namespace Sota2NeuralFx;

			const C::LineSnapshot srSnap = BuildSceneSnapshot(SceneKind::ShockNeuralSr2x, 60, 6000000, 0x4401ull);
			C::LineSet lsH;
			Assert::IsTrue(C::ToLineSet(srSnap, 0, lsH));
			const double floorCodes = 1.0 / std::sqrt(12.0);

			C::NeuralSrSettings onCfg;
			onCfg.enabled = true;
			C::NeuralSrWorkspace wsDirty, wsFresh;

			// Pollute wsDirty with invalid inputs, NaN samples, non-power-of-two FFT size, and different candidateN:
			{
				C::LineSet badLs = lsH;
				badLs.Line(2)[100] = std::numeric_limits<double>::quiet_NaN();
				const C::NeuralSrResult rNan = C::NeuralSrWitness::Evaluate(badLs, 1920, 0.0, floorCodes, onCfg, wsDirty);
				Assert::IsFalse(rNan.neuralUpscaled);
				Assert::IsTrue(rNan.verdict == C::NeuralSrVerdict::Abstain);
				Assert::IsTrue(rNan.reason == C::NeuralSrReason::InvalidInput);

				C::NeuralSrSettings badFft = onCfg;
				badFft.fftSize = 200; // not a power of two
				const C::NeuralSrResult rBadFft = C::NeuralSrWitness::Evaluate(lsH, 1920, 0.0, floorCodes, badFft, wsDirty);
				Assert::IsFalse(rBadFft.neuralUpscaled);
				Assert::IsTrue(rBadFft.reason == C::NeuralSrReason::InvalidInput);

				const C::NeuralSrResult rBadFloor = C::NeuralSrWitness::Evaluate(lsH, 1920, 0.0, -1.0, onCfg, wsDirty);
				Assert::IsFalse(rBadFloor.neuralUpscaled);
				Assert::IsTrue(rBadFloor.reason == C::NeuralSrReason::InvalidInput);

				C::NeuralSrSettings altFft = onCfg;
				altFft.fftSize = 128;
				(void)C::NeuralSrWitness::Evaluate(lsH, 1280, 0.0, floorCodes, altFft, wsDirty);
			}

			// Reused dirty workspace vs brand-new fresh workspace must produce bit-identical telemetry:
			const C::NeuralSrResult rReused = C::Evaluate(lsH, 1920, 0.0, floorCodes, onCfg, wsDirty);
			const C::NeuralSrResult rFresh = C::Evaluate(lsH, 1920, 0.0, floorCodes, onCfg, wsFresh);
			Assert::IsTrue(rReused.neuralUpscaled && rFresh.neuralUpscaled);
			Assert::IsTrue(W1DescaleRef::MemEq(rReused.edgePhaseBicoherence, rFresh.edgePhaseBicoherence));
			Assert::IsTrue(W1DescaleRef::MemEq(rReused.edgeToInteriorHfRatio, rFresh.edgeToInteriorHfRatio));
			Assert::IsTrue(W1DescaleRef::MemEq(rReused.hfKurtosis, rFresh.hfKurtosis));
			Assert::IsTrue(W1DescaleRef::MemEq(rReused.interiorSpectralDropExcessDecades, rFresh.interiorSpectralDropExcessDecades));

			// Original pathway forces ChainEstimator OFF even when neuralSr is enabled:
			ChainFx::ManualClock clk;
			clk.Set(1000.0);
			C::EstimatorSettings cfg;
			cfg.outputStepOverride = 1;
			cfg.neuralSr.enabled = true;
			C::ChainEstimator est(cfg, clk.Fn());
			est.SetSynchronous(true);
			est.SetMode(C::EstimatorMode::On);
			est.SetOriginalPathway(true);
			Assert::IsFalse(est.Running());
			Assert::IsFalse(est.SubmitLines(srSnap));
			Assert::AreEqual(uint64_t(0), est.GetCounters().neuralSrDetections);

			est.SetOriginalPathway(false);
			Assert::IsTrue(est.Running());
			Assert::IsTrue(est.SubmitLines(srSnap));
			Assert::IsTrue(est.GetCounters().neuralSrDetections >= 1u);
			est.Stop();
		}
	};
}
