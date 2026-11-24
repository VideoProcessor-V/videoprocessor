/*
 * Copyright(C) 2026 VP-V contributors
 * SPDX-License-Identifier: GPL-3.0-only
 */
// Red-Team independent adversarial tests for SID-SOTA-3:
// Broadcast interlaced / motion-bob & weave-comb forensics (InterlacedForensics & ChainEstimator kReasonInterlacedBob)
// (Rule 3 mandatory same-input negative controls and progressive/grating false-positive traps, pure CPU C++14 /W4).
//
// Covers:
//   1. Sota3MotionBob1080iTo2160pFiresReasonAndSuppressesConfident540pVerticalLatch
//   2. Sota3WeaveComb1080iAnd1080iTo2160pFiresDirectAndThroughChainEstimatorDeliver
//   3. Sota3AdversarialFalsePositiveTrapsProgressive540p720pAndVenetianBlindsNeverFire
#include "pch.h"
#include "CppUnitTest.h"

#include "ChainEstimatorFixtures.h"
#include "GeometryFixtures.h"
#include "W1DescaleRef.h"

#include <VP-V/modules/input/source_analysis/identity/SourceGeometryView.h>
#include <VP-V/modules/input/source_analysis/identity/chain/ChainEstimator.h>
#include <VP-V/modules/input/source_analysis/identity/chain/DescaleVerifier.h>
#include <VP-V/modules/input/source_analysis/identity/chain/InterlacedForensics.h>
#include <VP-V/modules/input/source_analysis/identity/chain/KernelBank.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <string>
#include <vector>

using namespace Microsoft::VisualStudio::CppUnitTestFramework;

namespace Tests
{
	namespace Sota3InterlacedFx
	{
		namespace C = SourceIdentity::Chain;
		namespace G = SourceIdentity::Geometry;

		constexpr int kFhdW = 1920;
		constexpr int kFhdH = 1080;
		constexpr int kUhdW = 3840;
		constexpr int kUhdH = 2160;
		constexpr int kBicubic = 1;
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

		// Upscales a 1-D broadband line from nIn -> nOut using exact centre-aligned mapping with sub-sample `phase`.
		inline void SynthUpscaledLine1D(int nIn, int nOut, int kernel, double phase, SplitMix64& rng, std::vector<double>& outBuf)
		{
			std::vector<double> src(static_cast<size_t>(nIn), 512.0);
			for (int i = 0; i < nIn; ++i)
				src[static_cast<size_t>(i)] = 512.0 + 60.0 * rng.Normal01();

			outBuf.assign(static_cast<size_t>(nOut), 512.0);
			if (nIn == nOut && phase == 0.0)
			{
				for (int i = 0; i < nOut; ++i)
					outBuf[static_cast<size_t>(i)] = static_cast<double>(Quantize10(src[static_cast<size_t>(i)]));
				return;
			}

			const int r = C::Kernel(kernel).radius;
			const int taps = 2 * r;
			std::vector<double> wt(static_cast<size_t>(taps));
			const double ratio = static_cast<double>(nIn) / static_cast<double>(nOut);
			for (int i = 0; i < nOut; ++i)
			{
				const double p = (static_cast<double>(i) + 0.5) * ratio - 0.5 + phase;
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
				outBuf[static_cast<size_t>(i)] = static_cast<double>(Quantize10(acc));
			}
		}

		// Builds a LineSnapshot with independent horizontal (srcW -> rasterW, phaseH) and vertical (srcH -> rasterH, phaseV) upscaling.
		// Motion-bobbed 1080i -> 2160p has srcW = 1920, srcH = 540, and alternating top/bottom field phaseV = +0.25 / -0.25!
		inline C::LineSnapshot MakeAnisotropicSnapshot(
			int rasterW,
			int rasterH,
			int srcW,
			int srcH,
			int kernelH,
			int kernelV,
			double phaseH,
			double phaseV,
			uint64_t seq,
			int64_t timeUs,
			uint64_t seed,
			bool cut)
		{
			C::LineRect active{ 0, 0, rasterW, rasterH };
			const C::LinePlan plan = C::LineSampler::Plan(active, seq);
			Assert::IsTrue(plan.valid);

			C::LineSnapshot snap;
			snap.seq = seq;
			snap.timeUs = timeUs;
			snap.rasterW = rasterW;
			snap.rasterH = rasterH;
			snap.active = plan.active;
			snap.inner = plan.inner;
			snap.rowCount = plan.rowCount;
			snap.colCount = plan.colCount;
			snap.rowY.assign(plan.rowY, plan.rowY + plan.rowCount);
			snap.colX.assign(plan.colX, plan.colX + plan.colCount);
			snap.rows.assign(static_cast<size_t>(plan.rowCount) * static_cast<size_t>(plan.inner.w), 512);
			snap.cols.assign(static_cast<size_t>(plan.colCount) * static_cast<size_t>(plan.inner.h), 512);
			if (cut)
			{
				snap.flags = C::kEvCut;
				snap.cuts = 1;
				snap.eventTimeUs = timeUs;
			}

			SplitMix64 rng(seed);
			std::vector<double> line;
			for (int j = 0; j < plan.rowCount; ++j)
			{
				SynthUpscaledLine1D(srcW, rasterW, kernelH, phaseH, rng, line);
				uint16_t* dst = &snap.rows[static_cast<size_t>(j) * static_cast<size_t>(plan.inner.w)];
				for (int i = 0; i < plan.inner.w; ++i)
					dst[i] = static_cast<uint16_t>(line[static_cast<size_t>(plan.inner.x + i)]);
			}
			for (int j = 0; j < plan.colCount; ++j)
			{
				SynthUpscaledLine1D(srcH, rasterH, kernelV, phaseV, rng, line);
				uint16_t* dst = &snap.cols[static_cast<size_t>(j) * static_cast<size_t>(plan.inner.h)];
				for (int i = 0; i < plan.inner.h; ++i)
					dst[i] = static_cast<uint16_t>(line[static_cast<size_t>(plan.inner.y + i)]);
			}
			return snap;
		}

		// Continuous smooth horizontal profile with crisp moving vertical bars/slanted edges (smooth along y within a field).
		inline double EvalFieldScene(double x, double y)
		{
			double v = 240.0 + 40.0 * std::sin(0.012 * y) * std::cos(0.015 * x);
			// Repeated smooth-edged vertical objects across x so horizontal gradient is strong on edges and calm between:
			const double cell = std::fmod(x + 0.18 * y + 10000.0, 96.0);
			const double edge1 = 0.5 * (1.0 + std::tanh((cell - 28.0) / 1.4));
			const double edge2 = 0.5 * (1.0 - std::tanh((cell - 62.0) / 1.4));
			v += 460.0 * edge1 * edge2;
			return v;
		}

		// Builds a genuine broadcast 1080i weave plane (1920x1080, field stride s = 1):
		// even rows sample Field 0 at x, odd rows sample Field 1 translated horizontally by fieldShiftPx (1/60s motion).
		inline ChainFx::Plane MakeWeave1080iPlane(int w, int h, double fieldShiftPx)
		{
			ChainFx::Plane p;
			p.w = w;
			p.h = h;
			p.luma.resize(static_cast<size_t>(w) * static_cast<size_t>(h));
			for (int y = 0; y < h; ++y)
			{
				const double dx = ((y & 1) != 0) ? fieldShiftPx : 0.0;
				uint16_t* dst = &p.luma[static_cast<size_t>(y) * static_cast<size_t>(w)];
				for (int x = 0; x < w; ++x)
					dst[x] = Quantize10(EvalFieldScene(static_cast<double>(x) - dx, static_cast<double>(y)));
			}
			return p;
		}

		// Separable 2-D upscale of a uint16_t Plane from (inW x inH) -> (outW x outH) with KernelBank kernel.
		inline ChainFx::Plane UpscalePlane2D(const ChainFx::Plane& src, int outW, int outH, int kernel)
		{
			const int r = C::Kernel(kernel).radius;
			const int taps = 2 * r;
			std::vector<double> tmp(static_cast<size_t>(outW) * static_cast<size_t>(src.h), 0.0);

			// Horizontal pass:
			{
				std::vector<double> wt(static_cast<size_t>(taps));
				const double rx = static_cast<double>(src.w) / static_cast<double>(outW);
				std::vector<double> wxTab(static_cast<size_t>(outW) * static_cast<size_t>(taps));
				std::vector<int> cxTab(static_cast<size_t>(outW));
				for (int x = 0; x < outW; ++x)
				{
					const double p = (static_cast<double>(x) + 0.5) * rx - 0.5;
					const double fl = std::floor(p);
					C::TapWeights(kernel, p - fl, &wxTab[static_cast<size_t>(x) * static_cast<size_t>(taps)]);
					cxTab[static_cast<size_t>(x)] = static_cast<int>(fl) - r + 1;
				}
				for (int y = 0; y < src.h; ++y)
				{
					const uint16_t* sRow = &src.luma[static_cast<size_t>(y) * static_cast<size_t>(src.w)];
					double* dRow = &tmp[static_cast<size_t>(y) * static_cast<size_t>(outW)];
					for (int x = 0; x < outW; ++x)
					{
						const double* w = &wxTab[static_cast<size_t>(x) * static_cast<size_t>(taps)];
						const int c0 = cxTab[static_cast<size_t>(x)];
						double acc = 0.0;
						for (int s = 0; s < taps; ++s)
						{
							int cx = c0 + s;
							if (cx < 0) cx = 0;
							if (cx > src.w - 1) cx = src.w - 1;
							acc += w[s] * static_cast<double>(sRow[cx]);
						}
						dRow[x] = acc;
					}
				}
			}

			// Vertical pass:
			ChainFx::Plane dst;
			dst.w = outW;
			dst.h = outH;
			dst.luma.resize(static_cast<size_t>(outW) * static_cast<size_t>(outH));
			{
				const double ry = static_cast<double>(src.h) / static_cast<double>(outH);
				std::vector<double> wyTab(static_cast<size_t>(outH) * static_cast<size_t>(taps));
				std::vector<int> cyTab(static_cast<size_t>(outH));
				for (int y = 0; y < outH; ++y)
				{
					const double p = (static_cast<double>(y) + 0.5) * ry - 0.5;
					const double fl = std::floor(p);
					C::TapWeights(kernel, p - fl, &wyTab[static_cast<size_t>(y) * static_cast<size_t>(taps)]);
					cyTab[static_cast<size_t>(y)] = static_cast<int>(fl) - r + 1;
				}
				for (int y = 0; y < outH; ++y)
				{
					const double* w = &wyTab[static_cast<size_t>(y) * static_cast<size_t>(taps)];
					const int c0 = cyTab[static_cast<size_t>(y)];
					uint16_t* dRow = &dst.luma[static_cast<size_t>(y) * static_cast<size_t>(outW)];
					for (int x = 0; x < outW; ++x)
					{
						double acc = 0.0;
						for (int s = 0; s < taps; ++s)
						{
							int cy = c0 + s;
							if (cy < 0) cy = 0;
							if (cy > src.h - 1) cy = src.h - 1;
							acc += w[s] * tmp[static_cast<size_t>(cy) * static_cast<size_t>(outW) + static_cast<size_t>(x)];
						}
						dRow[x] = Quantize10(acc);
					}
				}
			}
			return dst;
		}
	}

	TEST_CLASS(SourceIdSota3InterlacedTests)
	{
	public:
		// 1. Motion-bobbed 1080i -> 2160p (alternating field parity +0.25/-0.25 + 2:1 vertical/horizontal anisotropy):
		//      - Fires MotionBob, sets kReasonInterlacedBob ("interlaced broadcast"), increments interlacedBobDetections,
		//        and SUPPRESSES confident progressive 540p vertical latch!
		//      - Mandatory same-input negative control (interlaced.enabled == false on the exact same stream) proves
		//        unprotected ChainEstimator falsely latches confident progressive 540 -> 2160 on the vertical axis!
		TEST_METHOD(Sota3MotionBob1080iTo2160pFiresReasonAndSuppressesConfident540pVerticalLatch)
		{
			using namespace Sota3InterlacedFx;

			const C::InterlacedSettings defCfg;
			Assert::IsFalse(defCfg.enabled, L"InterlacedSettings must default to OFF");
			Assert::AreEqual(std::string("interlaced broadcast"),
				SourceGeometry::ReasonWords(C::kReasonInterlacedBob),
				L"ReasonWords(kReasonInterlacedBob) must render 'interlaced broadcast'");

			// Build 4-shot (12 evaluations, 1.6 s cadence -> 3.2 s dwell per shot >= 3.0 s) motion-bobbed 1080i -> 2160p stream:
			//   - H: 1920 -> 3840 Lanczos3 (rH = 0.5)
			//   - V: 540 -> 2160 Bicubic field bob (rV = 0.25) with alternating top/bottom field phase (+0.25 / -0.25)
			std::vector<C::LineSnapshot> bobStream;
			for (int s = 0; s < 12; ++s)
			{
				const bool isCut = (s % 3) == 0;
				const double fieldPhase = ((s & 1) == 0) ? 0.25 : -0.25;
				bobStream.push_back(MakeAnisotropicSnapshot(
					kUhdW, kUhdH, 1920, 540, kLanczos3, kBicubic, 0.0, fieldPhase,
					static_cast<uint64_t>(10 + s), static_cast<int64_t>(s) * 1600000,
					0x3100ull + static_cast<uint64_t>(s * 43), isCut));
			}

			// (A) Direct AnalyzeMotionBob parity-flip & seek-reset verification on real verified lines:
			const double floorCodes = 1.0 / std::sqrt(12.0);
			C::VerifierSettings vsPhase;
			vsPhase.phaseCount = 5;
			vsPhase.phaseStep = 0.25;
			C::DescaleVerifier ver(vsPhase);
			C::VerifyOptions vOpt;
			const int candsH[1] = { 1920 };
			const int candsV[1] = { 540 };

			C::InterlacedSettings onCfg;
			onCfg.enabled = true;
			C::InterlacedWorkspace ws;

			for (int f = 0; f < 2; ++f)
			{
				C::LineSet lsH, lsV;
				Assert::IsTrue(C::ToLineSet(bobStream[static_cast<size_t>(f)], 0, lsH));
				Assert::IsTrue(C::ToLineSet(bobStream[static_cast<size_t>(f)], 1, lsV));
				C::VerifyResult vrH, vrV;
				Assert::IsTrue(ver.Evaluate(lsH, candsH, 1, vOpt, vrH));
				Assert::IsTrue(ver.Evaluate(lsV, candsV, 1, vOpt, vrV));

				C::BobAxisInput bi;
				bi.validH = true;
				bi.nH = vrH.candidates[0].n;
				bi.NH = kUhdW;
				bi.statH = vrH.candidates[0].stat;
				bi.validV = true;
				bi.nV = vrV.candidates[0].n;
				bi.NV = kUhdH;
				bi.statV = vrV.candidates[0].stat;
				bi.phaseV = vrV.candidates[0].phase;

				const C::InterlacedResult ir = C::AnalyzeMotionBob(
					bi, &lsH, floorCodes, bobStream[static_cast<size_t>(f)].seq, onCfg, ws);
				Assert::IsTrue(ir.interlacedDetected, L"AnalyzeMotionBob must detect 1080i->2160p motion-bob");
				Assert::IsTrue(ir.kind == C::InterlacedKind::MotionBob);
				Assert::AreEqual(0.5, ir.axisRatioVOverH, 0.02);
				Assert::IsTrue(ir.horizAboveHalfSnr > 2.25);
				if (f == 0)
				{
					Assert::IsFalse(ir.fieldParityFlipObserved);
					Assert::IsTrue(ir.reason == C::InterlacedReason::MotionBobAnisotropicSplit);
				}
				else
				{
					Assert::IsTrue(ir.fieldParityFlipObserved, L"second consecutive field must observe 0.5-line parity flip");
					Assert::IsTrue(ir.reason == C::InterlacedReason::MotionBobFieldParityFlip);
				}
			}

			// Out-of-order seek seq < prevSeq clears stale parity history:
			{
				C::LineSet lsH;
				Assert::IsTrue(C::ToLineSet(bobStream[0], 0, lsH));
				C::BobAxisInput biSeek;
				biSeek.validH = true; biSeek.nH = 1920; biSeek.NH = kUhdW; biSeek.statH = -0.8;
				biSeek.validV = true; biSeek.nV = 540;  biSeek.NV = kUhdH; biSeek.statV = -0.8;
				biSeek.phaseV = 0.25;
				const C::InterlacedResult irSeek = C::AnalyzeMotionBob(biSeek, &lsH, floorCodes, 2, onCfg, ws);
				Assert::IsTrue(irSeek.interlacedDetected);
				Assert::IsFalse(irSeek.fieldParityFlipObserved, L"out-of-order seek must reset temporal parity history");
			}

			// (B) Mandatory Same-Input Negative Control (interlaced.enabled == false on bobStream):
			//     Unprotected ChainEstimator falsely latches CONFIDENT progressive 540 -> 2160 on axis 1!
			const SourceGeometry::Delivered deliv{ kUhdW, kUhdH };
			{
				ChainFx::ManualClock clk;
				C::EstimatorSettings cfgOff;
				cfgOff.outputStepOverride = 1;
				cfgOff.verifier.phaseCount = 5;
				cfgOff.verifier.phaseStep = 0.25;
				Assert::IsFalse(cfgOff.interlaced.enabled);
				C::ChainEstimator estOff(cfgOff, clk.Fn());
				estOff.SetSynchronous(true);
				estOff.SetMode(C::EstimatorMode::On);
				for (size_t i = 0; i < bobStream.size(); ++i)
				{
					clk.Set(static_cast<double>(i) * 1600.0);
					Assert::IsTrue(estOff.SubmitLines(bobStream[i]));
				}
				G::Record recOff;
				Assert::IsTrue(estOff.Held().ReadLatest(0, recOff));
				const SourceGeometry::ConfidentGeometry cgOff = SourceGeometry::ConfidentOf(recOff, deliv);
				Assert::IsTrue(cgOff.axis[1].confident && std::abs(cgOff.axis[1].n - 540) <= 2,
					L"hazard proof (negative control): with interlaced=false, ChainEstimator falsely latches confident progressive 540p on V");
				Assert::AreEqual(uint64_t(0), estOff.GetCounters().interlacedBobDetections);
				Assert::AreEqual(uint32_t(0), recOff.applied[1].reasonBits & C::kReasonInterlacedBob);
				estOff.Stop();
			}

			// (C) Protected ChainEstimator (interlaced.enabled == true on the EXACT SAME bobStream):
			//     Fires kReasonInterlacedBob AND suppresses confident progressive 540p vertical latch!
			{
				ChainFx::ManualClock clk;
				C::EstimatorSettings cfgOn;
				cfgOn.outputStepOverride = 1;
				cfgOn.verifier.phaseCount = 5;
				cfgOn.verifier.phaseStep = 0.25;
				cfgOn.interlaced.enabled = true;
				C::ChainEstimator estOn(cfgOn, clk.Fn());
				estOn.SetSynchronous(true);
				estOn.SetMode(C::EstimatorMode::On);
				for (size_t i = 0; i < bobStream.size(); ++i)
				{
					clk.Set(static_cast<double>(i) * 1600.0);
					Assert::IsTrue(estOn.SubmitLines(bobStream[i]));
				}
				G::Record recOn;
				Assert::IsTrue(estOn.Held().ReadLatest(0, recOn));
				const C::EstimatorCounters cntOn = estOn.GetCounters();
				Assert::IsTrue(cntOn.interlacedBobDetections >= 12u, L"interlacedBobDetections must increment on motion-bob frames");
				Assert::AreEqual(uint64_t(0), cntOn.interlacedWeaveDetections);
				Assert::IsTrue((recOn.measured[0].reasonBits & C::kReasonInterlacedBob) != 0u);
				Assert::IsTrue((recOn.measured[1].reasonBits & C::kReasonInterlacedBob) != 0u);
				Assert::IsTrue((recOn.applied[1].reasonBits & C::kReasonInterlacedBob) != 0u);

				const SourceGeometry::ConfidentGeometry cgOn = SourceGeometry::ConfidentOf(recOn, deliv);
				Assert::IsFalse(cgOn.axis[1].confident,
					L"interlaced=true MUST suppress confident progressive 540p latch on vertical axis");
				Assert::IsTrue(estOn.StatusLine().find("interlaced broadcast") != std::string::npos,
					L"StatusLine() must visibly surface 'interlaced broadcast' reason on suppressed V axis");
				estOn.Stop();
			}
		}

		// 2. Weave-combed 1080i (s = 1) and 1080i -> 2160p (s = 2):
		//      - Prove AnalyzeWeaveComb fires WeaveComb on both s = 1 (1920x1080) and s = 2 (3840x2160),
		//        while zero-motion static progressive frame on the exact same scene (fieldShiftPx == 0) does NOT fire!
		//      - Prove live ChainEstimator::Deliver (without EvidenceLevel::Frames) fires WeaveComb, sets
		//        kReasonInterlacedBob, and increments interlacedWeaveDetections (including P010 msbShift == 0)!
		TEST_METHOD(Sota3WeaveComb1080iAnd1080iTo2160pFiresDirectAndThroughChainEstimatorDeliver)
		{
			using namespace Sota3InterlacedFx;

			const double floorCodes = 1.0 / std::sqrt(12.0);
			C::InterlacedSettings onCfg;
			onCfg.enabled = true;

			// (A) Native 1080i weave (1920x1080, s = 1) vs progressive zero-shift negative control (fieldShiftPx = 0.0):
			const ChainFx::Plane weave1080i = MakeWeave1080iPlane(kFhdW, kFhdH, 7.0);
			const ChainFx::Plane prog1080p = MakeWeave1080iPlane(kFhdW, kFhdH, 0.0);

			const C::InterlacedResult res1080i = C::AnalyzeWeaveComb(
				weave1080i.luma.data(), weave1080i.w, weave1080i.h, weave1080i.w, floorCodes, onCfg);
			Assert::IsTrue(res1080i.interlacedDetected, L"AnalyzeWeaveComb must detect native 1080i weave (s=1)");
			Assert::IsTrue(res1080i.kind == C::InterlacedKind::WeaveComb);
			Assert::IsTrue(res1080i.reason == C::InterlacedReason::WeaveCombConfirmed);
			Assert::AreEqual(1, res1080i.weaveStride);
			Assert::IsTrue(res1080i.weaveCombRatio >= onCfg.weaveCombToSameFieldRatio);
			Assert::IsTrue(res1080i.flatToEdgeCombRatio <= onCfg.weaveMaxUniformGratingShare);

			// Same-scene progressive negative control (fieldShiftPx == 0.0):
			const C::InterlacedResult resProg1080 = C::AnalyzeWeaveComb(
				prog1080p.luma.data(), prog1080p.w, prog1080p.h, prog1080p.w, floorCodes, onCfg);
			Assert::IsFalse(resProg1080.interlacedDetected,
				L"negative control: zero inter-field shift on identical 1080p scene must NOT fire WeaveComb");
			Assert::IsTrue(resProg1080.kind == C::InterlacedKind::None);
			Assert::IsTrue(resProg1080.reason == C::InterlacedReason::ProgressiveIsotropic);

			// Double-plane overload + NaN guard check:
			std::vector<double> weave1080Dbl(weave1080i.luma.begin(), weave1080i.luma.end());
			const C::InterlacedResult res1080Dbl = C::AnalyzeWeaveComb(
				weave1080Dbl.data(), weave1080i.w, weave1080i.h, weave1080i.w, floorCodes, onCfg);
			Assert::IsTrue(res1080Dbl.interlacedDetected && res1080Dbl.weaveStride == 1);
			weave1080Dbl[static_cast<size_t>(50 * weave1080i.w + 100)] = std::numeric_limits<double>::quiet_NaN();
			const C::InterlacedResult resNan = C::AnalyzeWeaveComb(
				weave1080Dbl.data(), weave1080i.w, weave1080i.h, weave1080i.w, floorCodes, onCfg);
			Assert::IsFalse(resNan.interlacedDetected);
			Assert::IsTrue(resNan.kind == C::InterlacedKind::Abstain);
			Assert::IsTrue(resNan.reason == C::InterlacedReason::InvalidInput);

			// (B) 2x upscaled 1080i -> 2160p weave (3840x2160, s = 2) vs upscaled progressive 1080p -> 2160p control:
			//     Use smaller 960x540 -> 1920x1080 2x weave + full 3840x2160 weave:
			const ChainFx::Plane weave2160p = UpscalePlane2D(weave1080i, kUhdW, kUhdH, kLanczos3);
			const ChainFx::Plane prog2160p = UpscalePlane2D(prog1080p, kUhdW, kUhdH, kLanczos3);

			const C::InterlacedResult res2160Weave = C::AnalyzeWeaveComb(
				weave2160p.luma.data(), weave2160p.w, weave2160p.h, weave2160p.w, floorCodes, onCfg);
			Assert::IsTrue(res2160Weave.interlacedDetected, L"AnalyzeWeaveComb must detect 2x upscaled 1080i->2160p weave (s=2)");
			Assert::IsTrue(res2160Weave.kind == C::InterlacedKind::WeaveComb);
			Assert::IsTrue(res2160Weave.reason == C::InterlacedReason::WeaveCombConfirmed);
			Assert::AreEqual(2, res2160Weave.weaveStride);
			Assert::IsTrue(res2160Weave.weaveCombRatio >= onCfg.weaveCombToSameFieldRatio);

			const C::InterlacedResult res2160Prog = C::AnalyzeWeaveComb(
				prog2160p.luma.data(), prog2160p.w, prog2160p.h, prog2160p.w, floorCodes, onCfg);
			Assert::IsFalse(res2160Prog.interlacedDetected,
				L"negative control: progressive 1080p->2160p Lanczos3 upscale on identical scene must NOT fire WeaveComb");

			// (C) End-to-end ChainEstimator::Deliver (ChainFx::Feed) WITHOUT EvidenceLevel::Frames enabled:
			{
				ChainFx::ManualClock clk;
				clk.Set(1000.0);
				C::EstimatorSettings cfg;
				cfg.outputStepOverride = 1;
				cfg.interlaced.enabled = true;
				C::ChainEstimator est(cfg, clk.Fn());
				est.SetSynchronous(true);
				est.SetMode(C::EstimatorMode::On);
				ChainFx::Feed(est, weave2160p, 1, 1.0);

				G::Record rec;
				Assert::IsTrue(est.Held().ReadLatest(0, rec));
				const C::EstimatorCounters cnt = est.GetCounters();
				Assert::IsTrue(cnt.interlacedWeaveDetections >= 1u,
					L"ChainEstimator::Deliver must run WeaveComb when interlaced.enabled=true even without EvidenceLevel::Frames");
				Assert::IsTrue((rec.measured[0].reasonBits & C::kReasonInterlacedBob) != 0u);
				Assert::IsTrue((rec.applied[0].reasonBits & C::kReasonInterlacedBob) != 0u);
				est.Stop();
			}

			// Also verify P010 MSB-aligned SourceFrameView (msbShift == 0, samples = code10 << 6) scales properly:
			{
				ChainFx::Plane weaveP010 = weave1080i;
				for (size_t i = 0; i < weaveP010.luma.size(); ++i)
					weaveP010.luma[i] = static_cast<uint16_t>(weaveP010.luma[i] << 6);
				VpSkeleton::SourceFrameView vP010 = ChainFx::View(weaveP010, 1);
				vP010.msbShift = 0;

				ChainFx::ManualClock clk;
				clk.Set(1000.0);
				C::EstimatorSettings cfg;
				cfg.outputStepOverride = 1;
				cfg.interlaced.enabled = true;
				C::ChainEstimator est(cfg, clk.Fn());
				est.SetSynchronous(true);
				est.SetMode(C::EstimatorMode::On);
				est.NoteFrame(1, 1.0, 0, 0, 0, 0);
				est.Deliver(vP010);
				Assert::IsTrue(est.GetCounters().interlacedWeaveDetections >= 1u,
					L"ChainEstimator::Deliver WeaveComb must scale P010 msbShift==0 planes consistently with floorCodes");

				// Same-input P010 negative control on progressive 1080p plane (msbShift == 0 -> must NOT false-trigger!):
				ChainFx::Plane progP010 = prog1080p;
				for (size_t i = 0; i < progP010.luma.size(); ++i)
					progP010.luma[i] = static_cast<uint16_t>(progP010.luma[i] << 6);
				VpSkeleton::SourceFrameView vProgP010 = ChainFx::View(progP010, 2);
				vProgP010.msbShift = 0;
				clk.Set(3000.0);
				est.NoteFrame(2, 3.0, 0, 0, 0, 0);
				est.Deliver(vProgP010);
				Assert::AreEqual(uint64_t(1), est.GetCounters().interlacedWeaveDetections,
					L"P010 msbShift==0 progressive frame must NOT false-trigger WeaveComb due to 64x unscaled sample codes");
				est.Stop();
			}
		}

		// 3. Mandatory adversarial false-positive negative controls (MUST NEVER fire MotionBob or WeaveComb!):
		//      (a) Progressive 540p -> 2160p (960x540 -> 3840x2160) Lanczos3 upscale,
		//      (b) Progressive 720p -> 2160p (1280x720 -> 3840x2160) Lanczos3 upscale,
		//      (c) Native 2160p fine horizontal gratings / Venetian blinds (both pure & with vertical window mullions).
		TEST_METHOD(Sota3AdversarialFalsePositiveTrapsProgressive540p720pAndVenetianBlindsNeverFire)
		{
			using namespace Sota3InterlacedFx;

			const double floorCodes = 1.0 / std::sqrt(12.0);
			C::InterlacedSettings onCfg;
			onCfg.enabled = true;
			C::InterlacedWorkspace ws;

			// (a) Progressive 540p -> 2160p (960x540 -> 3840x2160) Lanczos3 upscale:
			{
				const C::LineSnapshot snap540 = MakeAnisotropicSnapshot(
					kUhdW, kUhdH, 960, 540, kLanczos3, kLanczos3, 0.0, 0.0, 10, 1000000, 0x5401ull, true);
				C::LineSet lsH;
				Assert::IsTrue(C::ToLineSet(snap540, 0, lsH));
				C::BobAxisInput bi;
				bi.validH = true; bi.nH = 960; bi.NH = kUhdW; bi.statH = -0.9;
				bi.validV = true; bi.nV = 540; bi.NV = kUhdH; bi.statV = -0.9;
				bi.phaseV = 0.0;
				const C::InterlacedResult ir = C::AnalyzeMotionBob(bi, &lsH, floorCodes, 10, onCfg, ws);
				Assert::IsFalse(ir.interlacedDetected, L"Trap (a): progressive 540p->2160p MUST NOT fire MotionBob");
				Assert::IsTrue(ir.kind == C::InterlacedKind::None);
				Assert::IsTrue(ir.reason == C::InterlacedReason::ProgressiveIsotropic);

				ChainFx::ManualClock clk;
				clk.Set(1000.0);
				C::EstimatorSettings cfg;
				cfg.outputStepOverride = 1;
				cfg.interlaced.enabled = true;
				C::ChainEstimator est(cfg, clk.Fn());
				est.SetSynchronous(true);
				est.SetMode(C::EstimatorMode::On);
				Assert::IsTrue(est.SubmitLines(snap540));
				G::Record rec;
				Assert::IsTrue(est.Held().ReadLatest(0, rec));
				Assert::AreEqual(uint64_t(0), est.GetCounters().interlacedBobDetections);
				Assert::AreEqual(uint64_t(0), est.GetCounters().interlacedWeaveDetections);
				Assert::AreEqual(uint32_t(0), rec.applied[1].reasonBits & C::kReasonInterlacedBob);
				est.Stop();
			}

			// (b) Progressive 720p -> 2160p (1280x720 -> 3840x2160) Lanczos3 upscale:
			{
				const C::LineSnapshot snap720 = MakeAnisotropicSnapshot(
					kUhdW, kUhdH, 1280, 720, kLanczos3, kLanczos3, 0.0, 0.0, 20, 2000000, 0x7201ull, true);
				C::LineSet lsH;
				Assert::IsTrue(C::ToLineSet(snap720, 0, lsH));
				C::BobAxisInput bi;
				bi.validH = true; bi.nH = 1280; bi.NH = kUhdW; bi.statH = -0.9;
				bi.validV = true; bi.nV = 720;  bi.NV = kUhdH; bi.statV = -0.9;
				bi.phaseV = 0.0;
				const C::InterlacedResult ir = C::AnalyzeMotionBob(bi, &lsH, floorCodes, 20, onCfg, ws);
				Assert::IsFalse(ir.interlacedDetected, L"Trap (b): progressive 720p->2160p MUST NOT fire MotionBob");
				Assert::IsTrue(ir.kind == C::InterlacedKind::None);
				Assert::IsTrue(ir.reason == C::InterlacedReason::ProgressiveIsotropic);

				ChainFx::ManualClock clk;
				clk.Set(1000.0);
				C::EstimatorSettings cfg;
				cfg.outputStepOverride = 1;
				cfg.interlaced.enabled = true;
				C::ChainEstimator est(cfg, clk.Fn());
				est.SetSynchronous(true);
				est.SetMode(C::EstimatorMode::On);
				Assert::IsTrue(est.SubmitLines(snap720));
				Assert::AreEqual(uint64_t(0), est.GetCounters().interlacedBobDetections);
				Assert::AreEqual(uint64_t(0), est.GetCounters().interlacedWeaveDetections);
				est.Stop();
			}

			// (c) Native 2160p fine horizontal gratings / Venetian blinds:
			//     Test BOTH pure 1-px & 2-px horizontal gratings AND Venetian blinds crossed by vertical window mullions!
			{
				ChainFx::Plane pureBlinds;
				pureBlinds.w = kUhdW;
				pureBlinds.h = kUhdH;
				pureBlinds.luma.resize(static_cast<size_t>(kUhdW) * static_cast<size_t>(kUhdH));
				for (int y = 0; y < kUhdH; ++y)
				{
					const uint16_t slat = ((y & 1) == 0) ? 140u : 820u;
					uint16_t* row = &pureBlinds.luma[static_cast<size_t>(y) * static_cast<size_t>(kUhdW)];
					for (int x = 0; x < kUhdW; ++x)
						row[x] = slat;
				}
				const C::InterlacedResult rPure = C::AnalyzeWeaveComb(
					pureBlinds.luma.data(), pureBlinds.w, pureBlinds.h, pureBlinds.w, floorCodes, onCfg);
				Assert::IsFalse(rPure.interlacedDetected, L"Trap (c1): pure 1-px horizontal Venetian blinds MUST NOT fire WeaveComb");
				Assert::IsTrue(rPure.kind == C::InterlacedKind::None);
				Assert::IsTrue(rPure.reason == C::InterlacedReason::NativeHorizontalGratingRejected);

				// Adversarial Venetian blinds with sharp vertical window-frame mullions + horizontal texture so edgeCount >> 32:
				ChainFx::Plane texturedBlinds = pureBlinds;
				for (int y = 0; y < kUhdH; ++y)
				{
					const double slat = (((y / 2) & 1) == 0) ? 180.0 : 780.0; // 2-px slats (attacks s=2 too!)
					uint16_t* row = &texturedBlinds.luma[static_cast<size_t>(y) * static_cast<size_t>(kUhdW)];
					for (int x = 0; x < kUhdW; ++x)
					{
						const double mullion = ((x % 320) < 16) ? 160.0 : 0.0;
						const double gentleGrad = 25.0 * std::sin(0.08 * static_cast<double>(x));
						row[x] = Quantize10(slat + mullion + gentleGrad);
					}
				}
				const C::InterlacedResult rTex = C::AnalyzeWeaveComb(
					texturedBlinds.luma.data(), texturedBlinds.w, texturedBlinds.h, texturedBlinds.w, floorCodes, onCfg);
				Assert::IsFalse(rTex.interlacedDetected,
					L"Trap (c2): Venetian blinds with vertical mullions MUST be rejected by flatToEdgeCombRatio guard");
				Assert::IsTrue(rTex.kind == C::InterlacedKind::None);
				Assert::IsTrue(rTex.reason == C::InterlacedReason::NativeHorizontalGratingRejected);

				ChainFx::ManualClock clk;
				clk.Set(1000.0);
				C::EstimatorSettings cfg;
				cfg.outputStepOverride = 1;
				cfg.interlaced.enabled = true;
				C::ChainEstimator est(cfg, clk.Fn());
				est.SetSynchronous(true);
				est.SetMode(C::EstimatorMode::On);
				ChainFx::Feed(est, texturedBlinds, 1, 1.0);
				Assert::AreEqual(uint64_t(0), est.GetCounters().interlacedBobDetections);
				Assert::AreEqual(uint64_t(0), est.GetCounters().interlacedWeaveDetections);

				// Original pathway forces OFF:
				est.SetOriginalPathway(true);
				Assert::IsFalse(est.Running());
				est.Stop();
			}
		}
	};
}
