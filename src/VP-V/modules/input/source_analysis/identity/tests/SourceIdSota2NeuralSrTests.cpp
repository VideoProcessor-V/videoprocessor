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
//   4. Sota2MultiEvalSequenceDemotesConfidentNativeCodedBumpsAppliedGenAndPreservesStatus
//   5. Sota2StateTransitionProgressiveToNeuralSrAndBackTracksExactAppliedGen
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

		// Synthesizes either:
		//   - (applyShockSr == false): a classical 1080p->4K LTI upscaled line (bandlimited edges + bandlimited interiors), or
		//   - (applyShockSr == true): a genuine nonlinear SR / shock-contour 2x super-resolved line where textured interiors
		//     remain 1080p-bandlimited while strong contours are reconstructed at crisp 1-px 4K positions without LTI ringing
		//     (breaking classical LTI kernel subspaces while exhibiting cross-scale edge phase-lock and interior PSD collapse).
		inline void SynthSteppedTextured2xLine(
			int nIn,
			int nOut,
			int kernel,
			bool applyShockSr,
			SplitMix64& rng,
			std::vector<double>& outLine)
		{
			if (!applyShockSr)
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
				for (int i = 0; i < nOut; ++i)
					outLine[static_cast<size_t>(i)] = static_cast<double>(Quantize10(outLine[static_cast<size_t>(i)]));
				return;
			}

			// Nonlinear SR path: 1080p-bandlimited interior texture + crisp 1-px 4K-grid contour reconstruction.
			std::vector<double> texLat(static_cast<size_t>(nIn), 0.0);
			for (int i = 0; i < nIn; ++i)
				texLat[static_cast<size_t>(i)] = C::ClampD(20.0 * rng.Normal01(), -52.0, 52.0);

			std::vector<double> texUp;
			Upscale1D(texLat, nOut, kLanczos3, texUp);

			std::vector<int> edges;
			std::vector<double> levels;
			double curLevel = 305.0;
			levels.push_back(curLevel);
			int pos = 34 + static_cast<int>(rng.NextU64() % 17ull);
			while (pos < nOut - 36)
			{
				edges.push_back(pos);
				curLevel = (curLevel < 500.0) ? 635.0 : 305.0;
				levels.push_back(curLevel);
				pos += 70 + static_cast<int>(rng.NextU64() % 23ull);
			}

			outLine.resize(static_cast<size_t>(nOut));
			size_t edgeIdx = 0;
			for (int x = 0; x < nOut; ++x)
			{
				while (edgeIdx < edges.size() && x >= edges[edgeIdx])
					++edgeIdx;
				const double val = levels[edgeIdx] + texUp[static_cast<size_t>(x)];
				outLine[static_cast<size_t>(x)] = static_cast<double>(Quantize10(val));
			}
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

		// 4. SOTA2-HIGH-3 multi-evaluation sequence regression:
		//    Across a multi-shot / multi-eval sequence of AI-upscaled frames with PowerProbe active:
		//      - Mandatory same-input negative control (neuralSr.enabled == false on the exact same sequence)
		//        accumulates positive native LLR via PowerProbe and falsely latches Confident CodedRaster::NativeCoded;
		//      - Protected ChainEstimator (neuralSr.enabled == true on the exact same sequence) NEVER latches
		//        Confident (!AnyConfident()) on any evaluation, bumps appliedGen when demoting confident reader
		//        output via UnsureLike, keeps latchedPower == false, and preserves "AI-upscaled (skip LTI inverse)"
		//        in StatusLine() throughout.
		TEST_METHOD(Sota2MultiEvalSequenceDemotesConfidentNativeCodedBumpsAppliedGenAndPreservesStatus)
		{
			using namespace Sota2NeuralFx;

			// Build 3-shot x 6-eval (18 evaluations at 0.8 s cadence -> 4.0 s dwell/shot, all 3 shots live in window)
			// nonlinear AI-upscaled 1080p->4K stream:
			std::vector<C::LineSnapshot> srStream;
			srStream.reserve(18u);
			for (int s = 0; s < 18; ++s)
			{
				const int64_t timeUs = static_cast<int64_t>(s) * 800000;
				C::LineSnapshot snap = BuildSceneSnapshot(
					SceneKind::ShockNeuralSr2x,
					static_cast<uint64_t>(100 + s),
					timeUs,
					0x2201ull + static_cast<uint64_t>(s * 31));
				if ((s % 6) != 0)
				{
					snap.flags = 0u;
					snap.cuts = 0;
					snap.eventTimeUs = -1;
				}
				srStream.push_back(snap);
			}

			const SourceGeometry::Delivered deliv{ kUhdW, kUhdH };

			// (A) Mandatory Same-Input Negative Control (neuralSr.enabled == false on srStream):
			//     PowerProbe sees its synthetic injection on sharp 1-px AI edges while LTI candidates do not dip,
			//     causing unprotected ChainEstimator to falsely reach Confident NativeCoded!
			{
				ChainFx::ManualClock clk;
				C::EstimatorSettings cfgOff;
				cfgOff.outputStepOverride = 1;
				Assert::IsFalse(cfgOff.neuralSr.enabled);
				C::ChainEstimator estOff(cfgOff, clk.Fn());
				estOff.SetSynchronous(true);
				int probeSeenH = 0;
				int probeSeenV = 0;
				estOff.SetEvaluationObserver([&](const C::EvaluationDetail& d) {
					if (d.axis[0].probeRan && d.axis[0].probeSeen)
						++probeSeenH;
					if (d.axis[1].probeRan && d.axis[1].probeSeen)
						++probeSeenV;
				});
				estOff.SetMode(C::EstimatorMode::On);
				for (size_t i = 0; i < srStream.size(); ++i)
				{
					clk.Set(static_cast<double>(i) * 800.0);
					Assert::IsTrue(estOff.SubmitLines(srStream[i]));
				}

				Assert::AreEqual(18, probeSeenH, L"fixture: PowerProbe is active and seen on H across srStream");
				Assert::AreEqual(18, probeSeenV, L"fixture: PowerProbe is active and seen on V across srStream");

				G::Record recOff;
				Assert::IsTrue(estOff.Held().ReadLatest(0, recOff));
				const SourceGeometry::ConfidentGeometry cgOff = SourceGeometry::ConfidentOf(recOff, deliv);
				Assert::IsTrue(cgOff.AnyConfident() && cgOff.axis[0].confident && cgOff.axis[1].confident,
					L"hazard proof (negative control): with neuralSr=false, multi-eval AI-upscaled stream falsely latches Confident");
				Assert::AreEqual(kUhdW, cgOff.axis[0].n);
				Assert::AreEqual(kUhdH, cgOff.axis[1].n);
				Assert::IsTrue(recOff.codedRaster == G::CodedRaster::NativeCoded,
					L"hazard proof (negative control): with neuralSr=false, multi-eval AI-upscaled stream falsely publishes NativeCoded");
				Assert::IsTrue(recOff.appliedGen >= 1u && recOff.appliedGen <= 2u,
					L"hazard proof (negative control): neuralSr=false latches NativeCoded on H and V and bumps appliedGen");
				Assert::IsTrue(estOff.LatchedPowerForTest());
				Assert::IsTrue(estOff.StatusLine().find("native, confident") != std::string::npos);
				Assert::IsTrue(estOff.StatusLine().find("AI-upscaled") == std::string::npos);
				estOff.Stop();
			}

			// (B) Protected ChainEstimator (neuralSr.enabled == true on the EXACT SAME srStream):
			//     NEVER latches Confident on any evaluation, keeps appliedGen == 0u on steady Unknown -> Unknown
			//     evaluations (zero spurious per-frame appliedGen inflation!), and preserves StatusLine() reason!
			{
				ChainFx::ManualClock clk;
				C::EstimatorSettings cfgOn;
				cfgOn.outputStepOverride = 1;
				cfgOn.neuralSr.enabled = true;
				C::ChainEstimator estOn(cfgOn, clk.Fn());
				estOn.SetSynchronous(true);
				int probeSeenH = 0;
				int probeSeenV = 0;
				estOn.SetEvaluationObserver([&](const C::EvaluationDetail& d) {
					if (d.axis[0].probeRan && d.axis[0].probeSeen)
						++probeSeenH;
					if (d.axis[1].probeRan && d.axis[1].probeSeen)
						++probeSeenV;
				});
				estOn.SetMode(C::EstimatorMode::On);
				for (size_t i = 0; i < srStream.size(); ++i)
				{
					clk.Set(static_cast<double>(i) * 800.0);
					Assert::IsTrue(estOn.SubmitLines(srStream[i]));

					G::Record stepRec;
					Assert::IsTrue(estOn.Held().ReadLatest(0, stepRec));
					const SourceGeometry::ConfidentGeometry stepCg = SourceGeometry::ConfidentOf(stepRec, deliv);
					Assert::IsFalse(stepCg.AnyConfident(),
						L"SOTA2-HIGH-3: neuralSr=true must NEVER latch Confident on any evaluation in a multi-eval AI-upscaled stream");
					Assert::IsTrue(stepRec.codedRaster != G::CodedRaster::NativeCoded,
						L"SOTA2-HIGH-3: neuralSr=true must NEVER publish CodedRaster::NativeCoded");
					Assert::AreEqual(uint64_t(0), stepRec.appliedGen,
						L"SOTA23-HIGH-2: steady Unknown -> Unknown AI-upscaled evaluations must NEVER inflate appliedGen");

					const std::string stLine = estOn.StatusLine();
					Assert::IsTrue(stLine.find("AI-upscaled (skip LTI inverse)") != std::string::npos,
						L"SOTA2-HIGH-3: StatusLine() must preserve 'AI-upscaled (skip LTI inverse)' across every evaluation");
					Assert::IsTrue(stLine.find("native, confident") == std::string::npos);
				}

				Assert::AreEqual(18, probeSeenH);
				Assert::AreEqual(18, probeSeenV);

				G::Record recOn;
				Assert::IsTrue(estOn.Held().ReadLatest(0, recOn));
				const C::EstimatorCounters cntOn = estOn.GetCounters();
				Assert::IsTrue(cntOn.neuralSrDetections >= 18u);
				Assert::AreEqual(uint64_t(0), recOn.appliedGen,
					L"SOTA23-HIGH-2: appliedGen must stay 0u across steady Unknown -> Unknown AI-SR stream");
				Assert::AreEqual(0, estOn.StatusInput().latchNx);
				Assert::AreEqual(0, estOn.StatusInput().latchNy);
				Assert::IsFalse(estOn.LatchedPowerForTest(),
					L"SOTA2-HIGH-3: SOTA-demoted AI-upscaled stream must not enter confident latched cadence");
				Assert::IsTrue((recOn.applied[0].reasonBits & C::kReasonNeuralSrDetected) != 0u);
				Assert::IsTrue((recOn.applied[1].reasonBits & C::kReasonNeuralSrDetected) != 0u);
				estOn.Stop();
			}
		}

		// 5. SOTA23-HIGH-2 real state-transition regression (Progressive -> AI-SR -> Progressive):
		//    Proves appliedGen tracks true published Record::applied state transitions with zero spurious inflation
		//    and zero latch pollution:
		//      - Phase 1 (Progressive 1080p->4K Lanczos3, 3 shots x 4 evals): latches Confident 1920x1080 (appliedGen: 0 -> 1,
		//        then +0 across steady Latched frames);
		//      - Phase 2 (AI-SR ShockNeuralSr2x, 1 shot x 4 evals): demotes applied to Unknown (appliedGen: 1 -> 2,
		//        then +0 across repeated AI-SR frames even while AxisReader window stays above c1; latchNx/Ny == 0);
		//      - Phase 3 (Progressive 1080p->4K Lanczos3, 1 shot x 4 evals): recovers Unknown -> Latched (appliedGen: 2 -> 3
		//        even though AxisReader remained Latched across the boundary, then +0 while steady Latched);
		//      - Same-input negative control (neuralSr.enabled == false on the exact same 20-frame sequence) fails to
		//        demote during Phase 2, keeping stale Confident 1920x1080 across all Phase 2 AI-SR evaluations.
		TEST_METHOD(Sota2StateTransitionProgressiveToNeuralSrAndBackTracksExactAppliedGen)
		{
			using namespace Sota2NeuralFx;

			// 20-evaluation schedule at constant 1.05 s cadence (window = 16.732 s; 4 evals/shot -> 3.15 s >= 3.0 s dwell):
			//   f =  0..11 : 3 shots x 4 evals ClassicalLanczos2x (1920x1080 -> 3840x2160)
			//   f = 12..15 : 1 shot  x 4 evals ShockNeuralSr2x    (AI-SR 1080p -> 4K)
			//   f = 16..19 : 1 shot  x 4 evals ClassicalLanczos2x (1920x1080 -> 3840x2160)
			std::vector<C::LineSnapshot> seqStream;
			seqStream.reserve(20u);
			for (int f = 0; f < 20; ++f)
			{
				const bool isAiSr = (f >= 12 && f < 16);
				const SceneKind kind = isAiSr ? SceneKind::ShockNeuralSr2x : SceneKind::ClassicalLanczos2x;
				const int64_t timeUs = static_cast<int64_t>(f) * 1050000;
				const uint64_t seed = (isAiSr ? 0x6200ull : 0x5100ull) + static_cast<uint64_t>(f * 37);
				C::LineSnapshot snap = BuildSceneSnapshot(kind, static_cast<uint64_t>(200 + f), timeUs, seed);
				if ((f % 4) != 0)
				{
					snap.flags = 0u;
					snap.cuts = 0;
					snap.eventTimeUs = -1;
				}
				seqStream.push_back(snap);
			}

			const SourceGeometry::Delivered deliv{ kUhdW, kUhdH };

			// (A) Mandatory Same-Input Negative Control (neuralSr.enabled == false on seqStream):
			//     Phase 2 AI-SR frames are not detected, so applied stays falsely Latched 1920x1080 across f=12..15!
			{
				ChainFx::ManualClock clk;
				C::EstimatorSettings cfgOff;
				cfgOff.outputStepOverride = 1;
				Assert::IsFalse(cfgOff.neuralSr.enabled);
				C::ChainEstimator estOff(cfgOff, clk.Fn());
				estOff.SetSynchronous(true);
				estOff.SetMode(C::EstimatorMode::On);
				for (size_t f = 0; f < seqStream.size(); ++f)
				{
					clk.Set(static_cast<double>(f) * 1050.0);
					Assert::IsTrue(estOff.SubmitLines(seqStream[f]));
					G::Record r;
					Assert::IsTrue(estOff.Held().ReadLatest(0, r));
					if (f >= 12 && f < 16)
					{
						const SourceGeometry::ConfidentGeometry cgMid = SourceGeometry::ConfidentOf(r, deliv);
						Assert::IsTrue(cgMid.axis[0].confident && cgMid.axis[0].n == 1920 &&
							cgMid.axis[1].confident && cgMid.axis[1].n == 1080,
							L"hazard proof (negative control): with neuralSr=false, Phase 2 AI-SR keeps stale progressive 1920x1080 latch");
					}
				}
				G::Record recOffEnd;
				Assert::IsTrue(estOff.Held().ReadLatest(0, recOffEnd));
				const SourceGeometry::ConfidentGeometry cgEnd = SourceGeometry::ConfidentOf(recOffEnd, deliv);
				Assert::IsTrue(cgEnd.axis[0].confident && cgEnd.axis[0].n == 1920 &&
					cgEnd.axis[1].confident && cgEnd.axis[1].n == 1080);
				Assert::IsTrue(recOffEnd.appliedGen >= 1u,
					L"negative control: appliedGen reflects latched axis/kernel transitions when neuralSr=false");
				estOff.Stop();
			}

			// (B) Protected ChainEstimator (neuralSr.enabled == true on the EXACT SAME seqStream):
			//     Exact appliedGen transitions: 0 -> 1 (Phase 1 latch) -> 2 (Phase 2 AI-SR demote) -> 3 (Phase 3 re-latch)!
			{
				ChainFx::ManualClock clk;
				C::EstimatorSettings cfgOn;
				cfgOn.outputStepOverride = 1;
				cfgOn.neuralSr.enabled = true;
				C::ChainEstimator estOn(cfgOn, clk.Fn());
				estOn.SetSynchronous(true);
				estOn.SetMode(C::EstimatorMode::On);

				for (size_t f = 0; f < seqStream.size(); ++f)
				{
					clk.Set(static_cast<double>(f) * 1050.0);
					Assert::IsTrue(estOn.SubmitLines(seqStream[f]));

					G::Record stepRec;
					Assert::IsTrue(estOn.Held().ReadLatest(0, stepRec));
					const SourceGeometry::ConfidentGeometry stepCg = SourceGeometry::ConfidentOf(stepRec, deliv);
					const C::ChainStatusInput stIn = estOn.StatusInput();
					Assert::AreEqual(0, stIn.latchNx, L"SOTA23-HIGH-2: zero latchNx pollution");
					Assert::AreEqual(0, stIn.latchNy, L"SOTA23-HIGH-2: zero latchNy pollution");

					if (f < 7)
					{
						Assert::IsFalse(stepCg.AnyConfident());
						Assert::AreEqual(uint64_t(0), stepRec.appliedGen,
							L"Phase 1 acquiring (f < 7): applied stays Unknown -> appliedGen == 0u");
					}
					else if (f < 12)
					{
						Assert::IsTrue(stepCg.axis[0].confident && stepCg.axis[0].n == 1920);
						Assert::IsTrue(stepCg.axis[1].confident && stepCg.axis[1].n == 1080);
						Assert::AreEqual(uint64_t(1), stepRec.appliedGen,
							L"Phase 1 latched (f = 7..11): Unknown -> Latched bumps appliedGen to exact 1u and stays steady");
						Assert::AreEqual(uint64_t(0), estOn.GetCounters().neuralSrDetections);
					}
					else if (f < 16)
					{
						Assert::IsFalse(stepCg.AnyConfident(),
							L"Phase 2 AI-SR (f = 12..15): applied must demote to Unknown on every AI-SR evaluation");
						Assert::AreEqual(uint64_t(2), stepRec.appliedGen,
							L"Phase 2 AI-SR (f = 12..15): Latched -> Unknown bumps appliedGen to exact 2u with zero per-frame inflation");
						Assert::IsFalse(estOn.LatchedPowerForTest());
						Assert::IsTrue(estOn.StatusLine().find("AI-upscaled (skip LTI inverse)") != std::string::npos);
					}
					else
					{
						Assert::IsTrue(stepCg.axis[0].confident && stepCg.axis[0].n == 1920,
							L"Phase 3 recovery (f = 16..19): H must re-latch Confident 1920");
						Assert::IsTrue(stepCg.axis[1].confident && stepCg.axis[1].n == 1080,
							L"Phase 3 recovery (f = 16..19): V must re-latch Confident 1080");
						Assert::AreEqual(uint64_t(3), stepRec.appliedGen,
							L"Phase 3 recovery (f = 16..19): Unknown -> Latched must bump appliedGen to exact 3u and stay steady");
						Assert::IsTrue(estOn.StatusLine().find("1080 -> 2160 lanczos-like, confident") != std::string::npos);
					}
				}

				Assert::IsTrue(estOn.GetCounters().neuralSrDetections >= 8u);
				estOn.Stop();
			}
		}
	};
}
