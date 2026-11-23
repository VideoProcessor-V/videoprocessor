/*
 * Copyright(C) 2026 VP-V contributors
 * SPDX-License-Identifier: GPL-3.0-only
 */
// Red-Team independent adversarial integration & proving tests for Source Identity Stage S2
// (2026-11-23_SourceID-plan.md s5.2, Owner-approved OD-1 & OD-2, Rules 1-4 & 8).
//
// Exercises REAL production wiring across ChainEstimator, ChainEvidence, BuildConventionCandidates,
// SourceIdConsensus, SourceIdPipeline, and SourceResolutionMonitor:
//   1. Default-OFF invariance & W1 byte-identity when Stage S2 inputs are posted.
//   2. P3 convention-driven exact proposals across 4K / 1440p / 1080p rasters (with conventionOnly negative control).
//   3. R1 fundamental-harmonic selection (1/3 vs 2/3 AND detailAbove sub-harmonic vs clean super-harmonic)
//      through real ChainEstimator (with naive max-credit negative controls).
//   4. End-to-end OD-1 Consensus + Pipeline + Monitor classical-alone gate (with blockClassicalAloneEntry == false negative control).
//   5. End-to-end ChainEstimator ChainJointLatch: classical-alone blocked, Joint 1-shot (vs 1-shot silent negative control),
//      Chain-alone 2-shot, OD-2 G1 <= 180 s hard bound under persistent classical misreading, and G2 once-per-bound
//      fresh joint renewal + stale credit rejection.
//   6. End-to-end ABR NoteAbrStepEvent + ChainEvidence::VoidBefore(t0Us) through real ChainEstimator
//      (pending blocks entry/move, confirmed step-up releases & voids with unvoided negative control, unconfirmed native timeout).
//   7. Wired ChainEdgeStats + useEmpiricalFloor inside real ChainEvidence & ChainEstimator on 8-bit dithered /
//      8-in-10 HDMI-loop signals (with un-gated / hardcoded-floor negative controls + visible Rule 4 fallback proofs).
//   8. Adversarial edge cases: NaN / Inf / zero / negative inputs, empty/short buffers, rapid ON->OFF->ON reset safety.
//
// CPU only, synthetic physics only, deterministic (fixed SplitMix64 seeds, manual clocks, zero real-time sleeps).
#include "pch.h"
#include "CppUnitTest.h"

#include "ChainEstimatorFixtures.h"
#include "GeometryFixtures.h"

#include <VP-V/modules/input/source_analysis/SourceResolutionMonitor.h>
#include <VP-V/modules/input/source_analysis/identity/SourceIdConsensus.h>
#include <VP-V/modules/input/source_analysis/identity/SourceIdPipeline.h>
#include <VP-V/modules/input/source_analysis/identity/SourceIdVote.h>
#include <VP-V/modules/input/source_analysis/identity/chain/ChainEdgeStats.h>
#include <VP-V/modules/input/source_analysis/identity/chain/ChainEstimator.h>
#include <VP-V/modules/input/source_analysis/identity/chain/ChainEvidence.h>
#include <VP-V/modules/input/source_analysis/identity/chain/ChainJointLatch.h>
#include <VP-V/modules/input/source_analysis/identity/chain/ChainStepMark.h>
#include <VP-V/modules/input/source_analysis/identity/chain/KernelBank.h>
#include <VP-V/modules/scaling/geometry/SourceGeometrySet.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <memory>
#include <string>
#include <vector>

using namespace Microsoft::VisualStudio::CppUnitTestFramework;

namespace Tests
{
	namespace S2Fx
	{
		namespace C = SourceIdentity::Chain;
		namespace G = SourceIdentity::Geometry;
		namespace Geo = Scaling::GeometrySet;

		constexpr int kUhdW = 3840;
		constexpr int kUhdH = 2160;
		constexpr int kBilinear = 0;
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

			double UniformCentered() { return Uniform01() - 0.5; }
			double TpdfUnit() { return UniformCentered() + UniformCentered(); }

			double Normal01()
			{
				double u1 = Uniform01();
				if (u1 < 1e-12) u1 = 1e-12;
				const double u2 = Uniform01();
				return std::sqrt(-2.0 * std::log(u1)) * std::cos(2.0 * C::kPi * u2);
			}
		};

		inline uint16_t QuantizeCode(double v, int step)
		{
			const double s = static_cast<double>(step > 0 ? step : 1);
			double q = std::floor(v / s + 0.5) * s;
			if (q < 64.0) q = 64.0;
			if (q > 940.0) q = 940.0;
			return static_cast<uint16_t>(q);
		}

		// Synthesizes a 1D signal of length `nOut` by upscaling an independent broadband latent signal of length `nIn`
		// with `kernel` (using KernelBank::TapWeights). When `nIn == nOut`, produces a genuine native broadband line.
		inline void SynthLine1D(int nIn, int nOut, int kernel, int step, SplitMix64& rng,
			std::vector<double>& outBuf, double extraNoiseSigma = 0.0, bool tpdfDither = false, double flatFraction = 0.0)
		{
			outBuf.assign(static_cast<size_t>(nOut), 512.0);
			const int activeEnd = static_cast<int>(std::floor(static_cast<double>(nIn) * (1.0 - flatFraction)));

			std::vector<double> src(static_cast<size_t>(nIn), 512.0);
			for (int i = 0; i < nIn; ++i)
			{
				if (i < activeEnd)
					src[static_cast<size_t>(i)] = 512.0 + 45.0 * rng.Normal01();
				else
					src[static_cast<size_t>(i)] = 512.0 + 0.02 * static_cast<double>(i); // quiet ramp
			}

			if (nIn == nOut)
			{
				outBuf = src;
			}
			else
			{
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
						if (c < 0) c = 0;
						if (c > nIn - 1) c = nIn - 1;
						acc += wt[static_cast<size_t>(s)] * src[static_cast<size_t>(c)];
					}
					outBuf[static_cast<size_t>(i)] = acc;
				}
			}

			for (int i = 0; i < nOut; ++i)
			{
				double v = outBuf[static_cast<size_t>(i)];
				if (tpdfDither)
					v += static_cast<double>(step) * rng.TpdfUnit();
				if (extraNoiseSigma > 0.0)
					v += extraNoiseSigma * rng.Normal01();
				outBuf[static_cast<size_t>(i)] = static_cast<double>(QuantizeCode(v, step));
			}
		}

		// Builds a real LineSnapshot matching LineSampler::Plan for `(rasterW, rasterH)` with independent synthetic
		// 1D upscaled (or native when srcW == rasterW && srcH == rasterH) rows & columns.
		inline C::LineSnapshot MakeSynthSnapshot(
			int rasterW, int rasterH, int srcW, int srcH, int kernel, int step,
			uint64_t seq, int64_t timeUs, uint64_t seed,
			bool cut = false, double extraNoiseSigma = 0.0, bool tpdfDither = false, double flatFraction = 0.0)
		{
			C::LineRect active;
			active.x = 0; active.y = 0; active.w = rasterW; active.h = rasterH;
			const C::LinePlan plan = C::LineSampler::Plan(active, seq);
			Assert::IsTrue(plan.valid, L"fixture: valid LinePlan");

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

			SplitMix64 rng(seed ^ (seq * 0x9e3779b97f4a7c15ull));
			std::vector<double> line;
			for (int j = 0; j < plan.rowCount; ++j)
			{
				SynthLine1D(srcW, rasterW, kernel, step, rng, line, extraNoiseSigma, tpdfDither, flatFraction);
				uint16_t* dst = &snap.rows[static_cast<size_t>(j) * static_cast<size_t>(plan.inner.w)];
				for (int i = 0; i < plan.inner.w; ++i)
					dst[i] = static_cast<uint16_t>(line[static_cast<size_t>(plan.inner.x + i)]);
			}
			for (int j = 0; j < plan.colCount; ++j)
			{
				SynthLine1D(srcH, rasterH, kernel, step, rng, line, extraNoiseSigma, tpdfDither, flatFraction);
				uint16_t* dst = &snap.cols[static_cast<size_t>(j) * static_cast<size_t>(plan.inner.h)];
				for (int i = 0; i < plan.inner.h; ++i)
					dst[i] = static_cast<uint16_t>(line[static_cast<size_t>(plan.inner.y + i)]);
			}
			return snap;
		}

		inline C::LineSnapshot MakeFlatSnapshot(int rasterW, int rasterH, uint16_t code, uint64_t seq, int64_t timeUs, bool cut = false)
		{
			C::LineRect active;
			active.x = 0; active.y = 0; active.w = rasterW; active.h = rasterH;
			const C::LinePlan plan = C::LineSampler::Plan(active, seq);
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
			snap.rows.assign(static_cast<size_t>(plan.rowCount) * static_cast<size_t>(plan.inner.w), code);
			snap.cols.assign(static_cast<size_t>(plan.colCount) * static_cast<size_t>(plan.inner.h), code);
			if (cut)
			{
				snap.flags = C::kEvCut;
				snap.cuts = 1;
				snap.eventTimeUs = timeUs;
			}
			return snap;
		}

		inline SourceIdentity::Vote MakeAxisVote(SourceIdentity::VoterId voter, double rH, double rV, double conf,
			int64_t timeUs, uint64_t evSeq, bool veto = false)
		{
			SourceIdentity::Vote v;
			v.voter = voter;
			v.h.ratioH = rH;
			v.h.ratioV = rV;
			SourceIdentity::SetField(v, SourceIdentity::Field::AxisH, conf);
			SourceIdentity::SetField(v, SourceIdentity::Field::AxisV, conf);
			v.evidenceTimeUs = timeUs;
			v.evidenceSeq = evSeq;
			v.safetyVeto = veto;
			return v;
		}

		inline G::Record MakeConfidentGeomRecord(uint64_t seq, int64_t timeUs, int nx, int Nx, int ny, int Ny, uint64_t streamKey = 1)
		{
			G::Record rec;
			rec.streamKey = streamKey;
			rec.evalSeq = seq;
			rec.evalTimeUs = timeUs;
			rec.maxAgeUs = 20000000;
			rec.codedRaster = (nx < Nx || ny < Ny) ? G::CodedRaster::PlayerUpscaled : G::CodedRaster::NativeCoded;
			for (int a = 0; a < 2; ++a)
			{
				const int n = a == 0 ? nx : ny;
				const int N = a == 0 ? Nx : Ny;
				G::AxisGeom g;
				g.state = G::LatchState::Latched;
				g.n = n;
				g.N = N;
				g.ratio = static_cast<float>(static_cast<double>(n) / static_cast<double>(N));
				g.llr = 25.0f;
				g.conf = 1.0f;
				rec.measured[a] = g;
				rec.applied[a] = g;
			}
			return rec;
		}

		inline C::CandidateResult MakeSyntheticCandidate(int n, double lineBaseVal, double lineStatVal,
			int lineCount, int segCount, double edgeSegBase, double edgeSegRms, double flatSegBase, double flatSegRms,
			double dofRatio, int edgeSegsPerLine)
		{
			C::CandidateResult c;
			c.nProposed = n;
			c.n = n;
			c.valid = true;
			c.exactDone = true;
			c.kernel = kLanczos3;
			c.family = static_cast<int>(C::Family::Lanczos);
			c.stat = lineStatVal;
			c.lineCount = lineCount;
			c.segCount = segCount;
			c.lineStat.assign(static_cast<size_t>(lineCount), lineStatVal);
			c.lineBase.assign(static_cast<size_t>(lineCount), lineBaseVal);
			c.lineRms.assign(static_cast<size_t>(lineCount), flatSegRms);
			const size_t total = static_cast<size_t>(lineCount) * static_cast<size_t>(segCount);
			c.segSum.resize(total);
			c.segRows.assign(total, C::kSegPx);
			c.segStat.resize(total);
			c.segBase.resize(total);
			for (int li = 0; li < lineCount; ++li)
			{
				for (int sg = 0; sg < segCount; ++sg)
				{
					const size_t idx = static_cast<size_t>(li * segCount + sg);
					const bool isEdge = sg < edgeSegsPerLine;
					const double rms = isEdge ? edgeSegRms : flatSegRms;
					const double base = isEdge ? edgeSegBase : flatSegBase;
					c.segSum[idx] = rms * rms * static_cast<double>(C::kSegPx) * (1.0 - dofRatio);
					c.segBase[idx] = base;
					c.segStat[idx] = std::log(rms / (base > 1e-6 ? base : 1e-6));
				}
			}
			return c;
		}
	}

	TEST_CLASS(SourceIdStageS2Tests)
	{
	public:
		// 1. Default-OFF invariance: every Stage S2 switch defaults to false, and posting Stage S2 events
		//    leaves W1 ChainEstimator output bit-exact identical with all Stage S2 counters strictly zero.
		TEST_METHOD(DefaultSettingsAreAllOffAndLeaveW1BehaviorByteIdentical)
		{
			using namespace S2Fx;
			const C::EstimatorSettings defEst;
			Assert::IsFalse(defEst.safetyRules);
			Assert::IsFalse(defEst.jointLatch.enabled);
			Assert::IsFalse(defEst.stepMark.enabled);
			Assert::IsFalse(defEst.conventionDrivenProposals);
			Assert::IsFalse(defEst.useEmpiricalFloor);
			Assert::IsFalse(defEst.evidence.edgeStats.enabled);

			const SourceIdentity::Settings defCons;
			Assert::IsFalse(defCons.blockClassicalAloneEntry);
			const SourceIdentity::PipelineSettings defPipe;
			Assert::IsFalse(defPipe.consensus.blockClassicalAloneEntry);

			// Drive two default-OFF ChainEstimators on the exact same synthetic snapshots:
			// `plain` gets W1 calls only; `withS2Calls` also receives Stage S2 SetClassicalFresh & NoteAbrStepEvent.
			ChainFx::ManualClock clkA, clkB;
			C::ChainEstimator plain(defEst, clkA.Fn());
			C::ChainEstimator withS2Calls(defEst, clkB.Fn());
			plain.SetSynchronous(true);
			withS2Calls.SetSynchronous(true);
			plain.SetMode(C::EstimatorMode::On);
			withS2Calls.SetMode(C::EstimatorMode::On);

			for (int i = 0; i < 6; ++i)
			{
				const uint64_t seq = static_cast<uint64_t>(10 + i * 10);
				const int64_t tUs = static_cast<int64_t>(i + 1) * 1000000;
				clkA.Set(static_cast<double>(i + 1) * 1500.0);
				clkB.Set(static_cast<double>(i + 1) * 1500.0);

				const C::LineSnapshot snap = MakeSynthSnapshot(1920, 1080, 960, 540, kBilinear, 1, seq, tUs, 1001u + i, i == 0 || i == 4);
				withS2Calls.SetClassicalFresh(true, 0.5, 0.5, false, false, false, false);
				withS2Calls.NoteAbrStepEvent(tUs, 0.25, seq);

				Assert::IsTrue(plain.SubmitLines(snap));
				Assert::IsTrue(withS2Calls.SubmitLines(snap));

				G::Record rA, rB;
				Assert::IsTrue(plain.Held().ReadLatest(0, rA));
				Assert::IsTrue(withS2Calls.Held().ReadLatest(0, rB));
				Assert::IsTrue(ChainFx::SameBytes(G::Pack(rA), G::Pack(rB)),
					L"Stage S2 OFF must stay byte-identical to W1 even when S2 events are posted");
			}

			const C::EstimatorCounters cnt = withS2Calls.GetCounters();
			Assert::AreEqual(uint64_t(6), cnt.evaluations);
			Assert::AreEqual(uint64_t(0), cnt.r1HarmonicSelections);
			Assert::AreEqual(uint64_t(0), cnt.p3ConventionProposals);
			Assert::AreEqual(uint64_t(0), cnt.empiricalFloorEvals);
			Assert::AreEqual(uint64_t(0), cnt.empiricalFloorFallbacks);
			Assert::AreEqual(uint64_t(0), cnt.jointLatch.entriesJoint);
			Assert::AreEqual(uint64_t(0), cnt.jointLatch.entriesChainAlone);
			Assert::AreEqual(uint64_t(0), cnt.jointLatch.wouldBeClassicalOnly);
			Assert::AreEqual(uint64_t(0), cnt.stepMark.marksRaised);
		}

		// 2. P3 convention-driven proposals across 4K / 1440p / 1080p rasters + real ChainEstimator integration,
		//    with in-test negative control comparing conventionOnly == true vs false.
		TEST_METHOD(P3ConventionDrivenProposalsExcludeNonConventionalAndParkedAcrossRasters)
		{
			using namespace S2Fx;

			auto containsNear = [](const int* arr, int n, int target) {
				for (int i = 0; i < n; ++i)
					if (std::abs(arr[i] - target) <= 1)
						return true;
				return false;
			};

			// (a) 4K raster (3840x2160): priority classicalN -> latchedN -> conventional geometries.
			// Non-conventional (1600, 2500) and parked anamorphic (1440) MUST be excluded when conventionOnly = true.
			{
				const int extras[3] = { 1600, 2500, 1440 }; // 1440/3840 = 0.375 is parked HDV 1440x1080
				int candsStrict[16] = {};
				const int nStrict = C::BuildConventionCandidates(0, 3840, 3840, 2160, 1280, 1920, extras, 3, true, candsStrict, 16);
				Assert::IsTrue(nStrict >= 3);
				Assert::AreEqual(1280, candsStrict[0], L"priority 1: classical A conventional size");
				Assert::AreEqual(1920, candsStrict[1], L"priority 2: currently latched conventional size");
				Assert::IsTrue(containsNear(candsStrict, nStrict, 2560), L"2560 (1440p) included on 4K");
				Assert::IsFalse(containsNear(candsStrict, nStrict, 1600), L"unlisted 1600 excluded under conventionOnly=true");
				Assert::IsFalse(containsNear(candsStrict, nStrict, 2500), L"unlisted 2500 excluded under conventionOnly=true");
				Assert::IsFalse(containsNear(candsStrict, nStrict, 1440), L"parked HDV 1440 excluded under conventionOnly=true");
				for (int i = 0; i < nStrict; ++i)
					Assert::IsTrue(Geo::ConventionalAxis(candsStrict[i], 3840, 0, 3840, 2160),
						L"every candidate under conventionOnly=true is a non-parked conventional axis");

				// In-test negative control (Rule 3): with conventionOnly = false on the EXACT SAME extraProposed,
				// unlisted / parked ratios ARE admitted (proving the filter actively excluded them above).
				int candsLoose[16] = {};
				const int nLoose = C::BuildConventionCandidates(0, 3840, 3840, 2160, 1280, 1920, extras, 3, false, candsLoose, 16);
				Assert::IsTrue(nLoose > nStrict);
				Assert::IsTrue(containsNear(candsLoose, nLoose, 1600), L"negative control: conventionOnly=false admits 1600");
				Assert::IsTrue(containsNear(candsLoose, nLoose, 1440), L"negative control: conventionOnly=false admits parked 1440");
			}

			// (b) 1440p raster (2560x1440): 1920x1080 (3/4) and 1280x720 (1/2) are conventional; 2560x1440 is native (excluded).
			{
				const int extras[2] = { 1600, 1440 };
				int cands1440[16] = {};
				const int n1440 = C::BuildConventionCandidates(0, 2560, 2560, 1440, 1920, 0, extras, 2, true, cands1440, 16);
				Assert::IsTrue(n1440 >= 2);
				Assert::AreEqual(1920, cands1440[0]);
				Assert::IsTrue(containsNear(cands1440, n1440, 1280));
				Assert::IsFalse(containsNear(cands1440, n1440, 1600));
				for (int i = 0; i < n1440; ++i)
					Assert::IsTrue(Geo::ConventionalAxis(cands1440[i], 2560, 0, 2560, 1440));
			}

			// (c) 1080p raster (1920x1080): 1280x720 (2/3) & 960x540 (1/2) included; non-conventional 1500 excluded;
			//     non-conventional classicalN (1500) must also be rejected from slot 0!
			{
				const int extras[1] = { 1500 };
				int cands1080[16] = {};
				const int n1080 = C::BuildConventionCandidates(0, 1920, 1920, 1080, 1500, 960, extras, 1, true, cands1080, 16);
				Assert::IsTrue(n1080 >= 2);
				Assert::AreEqual(960, cands1080[0], L"non-conventional classicalN=1500 skipped; latched conventional 960 takes slot 0");
				Assert::IsTrue(containsNear(cands1080, n1080, 1280));
				Assert::IsFalse(containsNear(cands1080, n1080, 1500));
			}

			// (d) End-to-end through real ChainEstimator: p3ConventionProposals increments, exact-verifies conventional sizes.
			{
				ChainFx::ManualClock clk;
				C::EstimatorSettings cfg;
				cfg.safetyRules = true;
				cfg.conventionDrivenProposals = true;
				C::ChainEstimator est(cfg, clk.Fn());
				est.SetSynchronous(true);
				est.SetMode(C::EstimatorMode::On);

				bool saw1280Exact = false;
				bool saw720Exact = false;
				bool sawNonConventional = false;
				est.SetEvaluationObserver([&](const C::EvaluationDetail& d) {
					for (const auto& c : d.axis[0].cands)
					{
						if (std::abs(c.nProposed - 1280) <= 1 && c.exact) saw1280Exact = true;
						if (!Geo::ConventionalAxis(c.nProposed, d.axis[0].N, 0, kUhdW, kUhdH)) sawNonConventional = true;
					}
					for (const auto& c : d.axis[1].cands)
					{
						if (std::abs(c.nProposed - 720) <= 1 && c.exact) saw720Exact = true;
						if (!Geo::ConventionalAxis(c.nProposed, d.axis[1].N, 1, kUhdW, kUhdH)) sawNonConventional = true;
					}
				});

				est.SetClassicalFresh(true, 1.0 / 3.0, 1.0 / 3.0, false);
				const C::LineSnapshot snap = MakeSynthSnapshot(kUhdW, kUhdH, 1280, 720, kBilinear, 1, 10, 1000000, 2001u, true);
				Assert::IsTrue(est.SubmitLines(snap));

				Assert::IsTrue(saw1280Exact && saw720Exact, L"P3 exact-verified true 1280x720 candidate on both axes");
				Assert::IsFalse(sawNonConventional, L"P3 + safetyRules never proposed a non-conventional or parked size");
				Assert::AreEqual(uint64_t(2), est.GetCounters().p3ConventionProposals, L"1 P3 proposal call per axis");
			}
		}

		// 3. R1 fundamental-harmonic selection through real ChainEstimator:
		//    (a) 1/3 (1280x720 -> 4K) vs 2/3 (2560x1440) harmonic competition with naive max-credit negative control;
		//    (b) detailAbove false sub-harmonic (1/4 = 960x540) vs clean super-harmonic (1/2 = 1920x1080) with negative control.
		TEST_METHOD(R1FundamentalHarmonicSelectionThroughRealChainEstimatorWithNegativeControl)
		{
			using namespace S2Fx;

			// Part A: Real ChainEstimator on 10-bit 720p -> 4K (1280x720 = 1/3) where both 1/3 and its 2x harmonic 2/3 (2560x1440) dip!
			{
				ChainFx::ManualClock clk;
				C::EstimatorSettings cfg;
				cfg.jointLatch.enabled = true;
				cfg.conventionDrivenProposals = true;
				C::ChainEstimator est(cfg, clk.Fn());
				est.SetSynchronous(true);
				est.SetMode(C::EstimatorMode::On);

				bool sawBoth13And23Dip = false;
				est.SetEvaluationObserver([&](const C::EvaluationDetail& d) {
					bool dip13 = false, dip23 = false;
					for (const auto& c : d.axis[0].cands)
					{
						if (std::abs(c.n - 1280) <= 1 && c.evJudged && c.evStat < -0.1) dip13 = true;
						if (std::abs(c.n - 2560) <= 1 && c.evJudged && c.evStat < -0.1) dip23 = true;
					}
					if (dip13 && dip23) sawBoth13And23Dip = true;
				});

				// Classical fresh confirms roll-off at 1/3 (1280x720) with no detailAbove.
				est.SetClassicalFresh(true, 1.0 / 3.0, 1.0 / 3.0, false);

				// Feed 1 shot lasting 6 s (past 3 s dwell) using bandlimited Lanczos3 so both 1/3 and 2/3
				// genuinely dip and accumulate current-shot credit (FM-3 harmonic competition).
				for (int s = 0; s <= 6; ++s)
				{
					clk.Set(static_cast<double>(s) * 1500.0);
					const C::LineSnapshot snap = MakeSynthSnapshot(kUhdW, kUhdH, 1280, 720, kLanczos3, 1,
						static_cast<uint64_t>(10 + s), static_cast<int64_t>(s) * 1000000, 3001u + s, s == 0);
					Assert::IsTrue(est.SubmitLines(snap));
				}

				Assert::IsTrue(sawBoth13And23Dip, L"fixture: 720p->4K genuinely creates harmonic dips at BOTH 1/3 (1280) and 2/3 (2560)");
				const C::ChainStatusInput si = est.StatusInput();
				Assert::AreEqual(1280, si.latchNx, L"R1 selects true 1/3 fundamental (1280), never 2/3 alias (2560)");
				Assert::AreEqual(720, si.latchNy);
				Assert::IsTrue(est.GetCounters().r1HarmonicSelections >= 1, L"R1 fundamental-harmonic selection visibly counted");

				// In-test negative control (Rule 3): when 2/3 alias has slightly higher credit than 1/3 fundamental,
				// naive max-credit picks the WRONG 2560x1440 alias, whereas SelectFundamentalHarmonic picks 1280x720!
				std::vector<C::HarmonicCandidate> comp(2);
				comp[0].nx = 2560; comp[0].Nx = kUhdW; comp[0].ny = 1440; comp[0].Ny = kUhdH;
				comp[0].creditX = 14.0; comp[0].creditY = 14.0; comp[0].classicalRollOffConfirmed = false; comp[0].detailAbove = false;
				comp[1].nx = 1280; comp[1].Nx = kUhdW; comp[1].ny = 720; comp[1].Ny = kUhdH;
				comp[1].creditX = 9.0; comp[1].creditY = 9.0; comp[1].classicalRollOffConfirmed = true; comp[1].detailAbove = false;

				int naiveIdx = (std::min)(comp[0].creditX, comp[0].creditY) >= (std::min)(comp[1].creditX, comp[1].creditY) ? 0 : 1;
				Assert::AreEqual(2560, comp[static_cast<size_t>(naiveIdx)].nx, L"negative control: naive max-credit picks wrong 2/3 alias");
				const auto r1Res = C::SelectFundamentalHarmonic(comp, kUhdW, kUhdH, 7.5);
				Assert::IsTrue(r1Res.found && r1Res.nx == 1280 && r1Res.ny == 720);
				Assert::IsTrue(r1Res.reason == C::HarmonicSelectionReason::SelectedFundamentalConfirmed);
			}

			// Part B: Genuine 1080p -> 4K (1920x1080 = 1/2) where classical sees 1/2 (implying detailAbove on 1/4 = 960x540).
			// Real ChainEstimator MUST latch 1920x1080 (never 960x540), AND naive max-credit negative control proves
			// a higher-credit detailAbove false sub-harmonic would fool naive selection!
			{
				ChainFx::ManualClock clk;
				C::EstimatorSettings cfg;
				cfg.jointLatch.enabled = true;
				cfg.conventionDrivenProposals = true;
				C::ChainEstimator est(cfg, clk.Fn());
				est.SetSynchronous(true);
				est.SetMode(C::EstimatorMode::On);

				est.SetClassicalFresh(true, 0.5, 0.5, false); // sees 1/2 -> marks 1/4 (960x540) as detailAbove!
				for (int s = 0; s <= 6; ++s)
				{
					clk.Set(static_cast<double>(s) * 1500.0);
					const C::LineSnapshot snap = MakeSynthSnapshot(kUhdW, kUhdH, 1920, 1080, kBilinear, 1,
						static_cast<uint64_t>(10 + s), static_cast<int64_t>(s) * 1000000, 3101u + s, s == 0);
					Assert::IsTrue(est.SubmitLines(snap));
				}

				const C::ChainStatusInput si = est.StatusInput();
				Assert::AreEqual(1920, si.latchNx, L"real ChainEstimator latches clean 1920x1080 super-harmonic, never 960x540");
				Assert::AreEqual(1080, si.latchNy);

				// Bonus R1 detailAbove rejection check through real ChainEstimator: when ALL candidates have detailAbove == true
				// (classical sees genuine high-frequency detail above), ChainEstimator MUST NOT enter via joint or R1 override!
				{
					ChainFx::ManualClock clkDirty;
					C::ChainEstimator estDirty(cfg, clkDirty.Fn());
					estDirty.SetSynchronous(true);
					estDirty.SetMode(C::EstimatorMode::On);
					estDirty.SetClassicalFresh(true, 0.5, 0.5, true); // detailAbove == true on all candidates!
					for (int s = 0; s <= 6; ++s)
					{
						clkDirty.Set(static_cast<double>(s) * 1500.0);
						const C::LineSnapshot snap = MakeSynthSnapshot(kUhdW, kUhdH, 1920, 1080, kLanczos3, 1,
							static_cast<uint64_t>(10 + s), static_cast<int64_t>(s) * 1000000, 3151u + s, s == 0);
						Assert::IsTrue(estDirty.SubmitLines(snap));
					}
					Assert::AreEqual(0, estDirty.StatusInput().latchNx, L"real ChainEstimator refuses to enter when detailAbove invalidates all candidates");
					Assert::AreEqual(uint64_t(0), estDirty.GetCounters().jointLatch.entriesJoint);
				}

				// In-test negative control (Rule 3): detailAbove false sub-harmonic with higher credit fools naive max-credit,
				// while SelectFundamentalHarmonic rejects it and chooses clean 1920x1080!
				std::vector<C::HarmonicCandidate> comp(2);
				comp[0].nx = 960; comp[0].Nx = kUhdW; comp[0].ny = 540; comp[0].Ny = kUhdH;
				comp[0].creditX = 18.0; comp[0].creditY = 18.0; comp[0].classicalRollOffConfirmed = true; comp[0].detailAbove = true;
				comp[1].nx = 1920; comp[1].Nx = kUhdW; comp[1].ny = 1080; comp[1].Ny = kUhdH;
				comp[1].creditX = 10.0; comp[1].creditY = 10.0; comp[1].classicalRollOffConfirmed = true; comp[1].detailAbove = false;

				int naiveIdx = (std::min)(comp[0].creditX, comp[0].creditY) >= (std::min)(comp[1].creditX, comp[1].creditY) ? 0 : 1;
				Assert::AreEqual(960, comp[static_cast<size_t>(naiveIdx)].nx, L"negative control: naive max-credit picks dirty 960x540");
				const auto r1Res = C::SelectFundamentalHarmonic(comp, kUhdW, kUhdH, 7.5);
				Assert::IsTrue(r1Res.found && r1Res.nx == 1920 && r1Res.ny == 1080);
				Assert::IsTrue(r1Res.reason == C::HarmonicSelectionReason::RejectedSubHarmonicDetailAbove);
			}
		}

		// 4. End-to-end OD-1 Consensus & Pipeline classical-alone gate (blockClassicalAloneEntry):
		//    Negative control (gate OFF) enters; gate ON blocks classical-alone with visible "would-be (classical only)"
		//    and classicalOnlyBlocked counter; Chain-alone and Joint still enter; releases to native are never blocked.
		TEST_METHOD(ConsensusAndPipelineOd1ClassicalAloneGateWithNegativeControl)
		{
			using namespace S2Fx;
			using namespace SourceIdentity;

			// (a) Direct Consensus: negative control (OFF) vs OD-1 gate (ON) on the exact same classical-alone stream.
			{
				Consensus naiveOff; // blockClassicalAloneEntry == false
				Consensus gatedOn;
				gatedOn.SetBlockClassicalAloneEntry(true);

				Result lastOff, lastOn;
				uint64_t seq = 0;
				for (int64_t t = 0; t <= 6000000; t += 500000)
				{
					const Vote v = MakeAxisVote(VoterId::ClassicalResolution, 0.5, 0.5, 0.9, t, ++seq);
					naiveOff.Submit(v);
					gatedOn.Submit(v);
					lastOff = naiveOff.Evaluate(seq, t);
					lastOn = gatedOn.Evaluate(seq, t);
				}

				// Negative control proves the exact same classical votes DO latch 0.5x0.5 without the OD-1 gate!
				Assert::AreEqual(0.5, naiveOff.Current().ratioH, 1e-9, L"negative control: gate OFF lets classical-alone enter");
				Assert::AreEqual(0.5, naiveOff.Current().ratioV, 1e-9);
				Assert::AreEqual(uint64_t(0), naiveOff.GetStats().classicalOnlyBlocked);

				// Gate ON: blocked visibly on both axes!
				Assert::AreEqual(1.0, gatedOn.Current().ratioH, 1e-9, L"OD-1 gate ON blocks classical-alone entry on H");
				Assert::AreEqual(1.0, gatedOn.Current().ratioV, 1e-9, L"OD-1 gate ON blocks classical-alone entry on V");
				Assert::AreEqual(std::string("would-be (classical only)"), lastOn.field[0].reason);
				Assert::AreEqual(std::string("would-be (classical only)"), lastOn.field[1].reason);
				Assert::IsTrue(gatedOn.GetStats().classicalOnlyBlocked >= 26);

				// Non-chain voter (IdNet alone) is also blocked (distinct visible reason, never counted as classicalOnlyBlocked).
				Consensus idNetOnly;
				idNetOnly.SetBlockClassicalAloneEntry(true);
				const Vote nv = MakeAxisVote(VoterId::IdNet, 0.5, 0.5, 0.9, 1000000, 1);
				idNetOnly.Submit(nv);
				const Result rn = idNetOnly.Evaluate(1, 1000000);
				Assert::AreEqual(1.0, idNetOnly.Current().ratioH, 1e-9);
				Assert::AreEqual(std::string("held: no chain witness"), rn.field[0].reason);
				Assert::AreEqual(uint64_t(0), idNetOnly.GetStats().classicalOnlyBlocked);
			}

			// (b) End-to-end SourceIdPipeline: classical-alone blocked -> Chain-alone enters -> Joint enters -> native release unblocked.
			{
				Pipeline pOff;
				pOff.SetConsensusOn(true);
				pOff.SetBlockClassicalAloneEntry(false);

				Pipeline pOn;
				pOn.SetConsensusOn(true);
				pOn.SetBlockClassicalAloneEntry(true);

				auto makeFrame = [](uint64_t seq, int64_t tUs, bool classicalUp, double ratio) {
					FrameInput in;
					in.seq = seq;
					in.timeUs = tUs;
					in.streamKey = 1;
					in.deliveredRatio = classicalUp ? ratio : 1.0;
					in.classical.reading = true;
					in.classical.declaredRatio = classicalUp ? ratio : 1.0;
					in.classical.confidence = classicalUp ? 0.9 : 0.0;
					in.classical.bitrateQuality = 0.9;
					return in;
				};

				for (int i = 1; i <= 14; ++i)
				{
					const FrameInput f = makeFrame(static_cast<uint64_t>(i), static_cast<int64_t>(i) * 500000, true, 0.5);
					pOff.Push(f);
					pOn.Push(f);
				}

				// Negative control: pOff entered 0.5x0.5; pOn stayed native 1.0x1.0 with visible "would-be (classical only)"!
				Assert::AreEqual(0.5, pOff.LastResult().value.ratioH, 1e-6);
				Assert::AreEqual(1.0, pOn.LastResult().value.ratioH, 1e-6);
				Assert::AreEqual(std::string("would-be (classical only)"), pOn.LastResult().field[0].reason);
				Assert::IsTrue(pOn.ConsensusStats().classicalOnlyBlocked >= 20);

				// Now test Chain-alone full path with gate ON (classical silent, P14g Geometry::HeldState provides 2 distinct evals).
				Pipeline pChain;
				pChain.SetConsensusOn(true);
				pChain.SetBlockClassicalAloneEntry(true);
				G::HeldState geom;
				pChain.SetGeometrySource(&geom);

				for (int i = 1; i <= 14; ++i)
				{
					const uint64_t seq = static_cast<uint64_t>(i);
					const int64_t tUs = static_cast<int64_t>(i) * 500000;
					// Publish distinct chain evaluations so N4 (chainMinDistinctEvaluations == 2) is satisfied.
					if (i == 1 || i == 2)
						geom.Publish(MakeConfidentGeomRecord(seq, tUs, 1920, kUhdW, 1080, kUhdH));
					pChain.Push(makeFrame(seq, tUs, false, 1.0)); // classical silent (confidence 0)
				}
				Assert::AreEqual(0.5, pChain.LastResult().value.ratioH, 1e-3, L"Chain-alone enters cleanly with OD-1 gate ON");
				Assert::AreEqual(0.5, pChain.LastResult().value.ratioV, 1e-3);

				// Release back to native by classical alone (safety veto or native vote) is NEVER blocked by OD-1 gate!
				FrameInput vetoFrame = makeFrame(15, 15 * 500000, false, 1.0);
				vetoFrame.classical.detailAbove = true;
				vetoFrame.classical.confidence = 0.9;
				pChain.Push(vetoFrame);
				Assert::AreEqual(1.0, pChain.LastResult().value.ratioH, 1e-6, L"release to native is never blocked");
				Assert::AreEqual(1.0, pChain.LastResult().value.ratioV, 1e-6);

				// Joint (Classical + Chain both voting 0.5) enters cleanly with OD-1 gate ON.
				Pipeline pJoint;
				pJoint.SetConsensusOn(true);
				pJoint.SetBlockClassicalAloneEntry(true);
				G::HeldState geomJ;
				pJoint.SetGeometrySource(&geomJ);
				for (int i = 1; i <= 14; ++i)
				{
					const uint64_t seq = static_cast<uint64_t>(i);
					const int64_t tUs = static_cast<int64_t>(i) * 500000;
					if (i == 1 || i == 2)
						geomJ.Publish(MakeConfidentGeomRecord(seq, tUs, 1920, kUhdW, 1080, kUhdH));
					pJoint.Push(makeFrame(seq, tUs, true, 0.5));
				}
				Assert::AreEqual(0.5, pJoint.LastResult().value.ratioH, 1e-3, L"Joint (A+B) enters cleanly with OD-1 gate ON");
				Assert::AreEqual(0.5, pJoint.LastResult().value.ratioV, 1e-3);
			}
		}

		// 5. End-to-end ChainEstimator ChainJointLatch:
		//    - Classical-alone blocked visibly on native 4K;
		//    - Joint enters in 1 shot (with 1-shot classical-silent negative control proving 1 shot cannot enter Chain-alone);
		//    - Chain-alone enters in 2 shots with classical silent;
		//    - OD-2 G1 enforces <= 180 s hard bound under persistent classical misreading;
		//    - OD-2 G2 renews ONCE on fresh joint re-proof at 120 s and rejects stale credit.
		TEST_METHOD(EndToEndChainEstimatorJointLatchEntriesAndOd2HardBoundG1VsG2)
		{
			using namespace S2Fx;

			// (a) Classical-alone on native 4K NEVER enters in ChainEstimator and visibly reports "would-be (classical only)".
			{
				ChainFx::ManualClock clk;
				C::EstimatorSettings cfg;
				cfg.jointLatch.enabled = true;
				cfg.conventionDrivenProposals = true;
				C::ChainEstimator est(cfg, clk.Fn());
				est.SetSynchronous(true);
				est.SetMode(C::EstimatorMode::On);

				est.SetClassicalFresh(true, 0.5, 0.5, false); // false classical 1080p reading on native 4K!
				for (int s = 0; s <= 6; ++s)
				{
					clk.Set(static_cast<double>(s) * 1500.0);
					const C::LineSnapshot nat = MakeSynthSnapshot(kUhdW, kUhdH, kUhdW, kUhdH, kBilinear, 1,
						static_cast<uint64_t>(10 + s), static_cast<int64_t>(s) * 1000000, 4001u + s, s == 0);
					Assert::IsTrue(est.SubmitLines(nat));
				}
				Assert::AreEqual(0, est.StatusInput().latchNx, L"classical-alone never latches on native 4K");
				Assert::IsTrue(est.GetCounters().jointLatch.wouldBeClassicalOnly >= 1);
				Assert::AreEqual(uint64_t(0), est.GetCounters().jointLatch.entriesJoint);
				Assert::AreEqual(uint64_t(0), est.GetCounters().jointLatch.entriesChainAlone);
				bool sawWouldBeNotice = false;
				std::string notice;
				while (est.TakeNotice(notice))
					if (notice.find("would-be (classical only)") != std::string::npos)
						sawWouldBeNotice = true;
				Assert::IsTrue(sawWouldBeNotice, L"would-be (classical only) surfaced visibly via TakeNotice");
			}

			// (b) Joint latches in 1 shot; negative control proves the EXACT SAME 1 shot does NOT latch when classical is silent!
			{
				ChainFx::ManualClock clkSilent, clkJoint;
				C::EstimatorSettings cfg;
				cfg.jointLatch.enabled = true;
				cfg.conventionDrivenProposals = true;
				C::ChainEstimator estSilent(cfg, clkSilent.Fn());
				C::ChainEstimator estJoint(cfg, clkJoint.Fn());
				estSilent.SetSynchronous(true);
				estJoint.SetSynchronous(true);
				estSilent.SetMode(C::EstimatorMode::On);
				estJoint.SetMode(C::EstimatorMode::On);

				estSilent.SetClassicalFresh(false, 1.0, 1.0, false);
				estJoint.SetClassicalFresh(true, 0.5, 0.5, false);

				// Single cut-free shot lasting 5 s (past 3 s dwell).
				for (int s = 0; s <= 5; ++s)
				{
					clkSilent.Set(static_cast<double>(s) * 1500.0);
					clkJoint.Set(static_cast<double>(s) * 1500.0);
					const C::LineSnapshot up = MakeSynthSnapshot(kUhdW, kUhdH, 1920, 1080, kBilinear, 1,
						static_cast<uint64_t>(10 + s), static_cast<int64_t>(s) * 1000000, 4101u + s, s == 0);
					Assert::IsTrue(estSilent.SubmitLines(up));
					Assert::IsTrue(estJoint.SubmitLines(up));
				}

				// Negative control: 1 shot is capped at 0.5 * c1 < c1, so Chain-alone cannot latch in 1 shot!
				Assert::AreEqual(0, estSilent.StatusInput().latchNx, L"negative control: 1 shot with classical silent does not latch");
				Assert::AreEqual(uint64_t(0), estSilent.GetCounters().jointLatch.entriesChainAlone);

				// Joint (A+B) latches in 1 shot!
				Assert::AreEqual(1920, estJoint.StatusInput().latchNx);
				Assert::AreEqual(1080, estJoint.StatusInput().latchNy);
				Assert::AreEqual(uint64_t(1), estJoint.GetCounters().jointLatch.entriesJoint);
				Assert::AreEqual(uint64_t(0), estJoint.GetCounters().jointLatch.entriesChainAlone);

				// (c) Continue `estSilent` into a 2nd shot (cut at s = 6..11) with classical STILL completely silent -> Chain-alone latches!
				for (int s = 6; s <= 11; ++s)
				{
					clkSilent.Set(static_cast<double>(s) * 1500.0);
					const C::LineSnapshot up = MakeSynthSnapshot(kUhdW, kUhdH, 1920, 1080, kBilinear, 1,
						static_cast<uint64_t>(10 + s), static_cast<int64_t>(s) * 1000000, 4101u + s, s == 6);
					Assert::IsTrue(estSilent.SubmitLines(up));
				}
				Assert::AreEqual(1920, estSilent.StatusInput().latchNx, L"Chain-alone latches in 2 shots with classical silent");
				Assert::AreEqual(1080, estSilent.StatusInput().latchNy);
				Assert::AreEqual(uint64_t(1), estSilent.GetCounters().jointLatch.entriesChainAlone);
				Assert::AreEqual(uint64_t(0), estSilent.GetCounters().jointLatch.entriesJoint);
			}

			// (d) OD-2 G1 (<= 180 s hard bound under persistent classical misreading) vs G2 stale rejection vs G2 once-per-bound renewal.
			{
				auto runPersistentClassicalStale = [](C::JointHoldPolicy policy, int64_t& releasedOffsetSec, C::JointLatchCounters& outCnt) {
					ChainFx::ManualClock clk;
					C::EstimatorSettings cfg;
					cfg.jointLatch.enabled = true;
					cfg.jointLatch.holdPolicy = policy;
					cfg.conventionDrivenProposals = true;
					C::ChainEstimator est(cfg, clk.Fn());
					est.SetSynchronous(true);
					est.SetMode(C::EstimatorMode::On);
					est.SetClassicalFresh(true, 0.5, 0.5, false);

					int64_t tLatchSec = -1;
					uint64_t seq = 10;
					for (int s = 0; s <= 10 && tLatchSec < 0; ++s, ++seq)
					{
						clk.Set(static_cast<double>(s) * 1500.0);
						const C::LineSnapshot up = MakeSynthSnapshot(kUhdW, kUhdH, 1920, 1080, kBilinear, 1, seq,
							static_cast<int64_t>(s) * 1000000, 4201u + s, s == 0);
						Assert::IsTrue(est.SubmitLines(up));
						if (est.StatusInput().latchNx == 1920)
							tLatchSec = s;
					}
					Assert::IsTrue(tLatchSec >= 0, L"fixture: latched 1920x1080");

					// Switch to uninformative flat/dark frames while classical A persistently misreads 1920x1080!
					// (Uninformative B lets A pause the 60 s starvation clock — worst case for hard bound.)
					releasedOffsetSec = -1;
					for (int dt = 1; dt <= 200 && releasedOffsetSec < 0; ++dt, ++seq)
					{
						const int64_t tSec = tLatchSec + dt;
						clk.Set(static_cast<double>(tSec) * 1500.0);
						const C::LineSnapshot flat = MakeFlatSnapshot(kUhdW, kUhdH, 64, seq, tSec * 1000000, false);
						Assert::IsTrue(est.SubmitLines(flat));
						if (est.StatusInput().latchNx == 0)
							releasedOffsetSec = dt;
					}
					outCnt = est.GetCounters().jointLatch;
				};

				// G1 enforces <= 180 s hard bound despite persistent classical misreading!
				int64_t g1Rel = -1;
				C::JointLatchCounters g1Cnt;
				runPersistentClassicalStale(C::JointHoldPolicy::G1_ChainOnlyHardBound, g1Rel, g1Cnt);
				Assert::IsTrue(g1Rel > 0 && g1Rel <= 180, L"OD-2 G1 enforces <= 180 s hard bound under persistent classical misreading");
				Assert::IsTrue(g1Cnt.releasesCeiling >= 1);
				Assert::IsTrue(g1Cnt.classicalHoldPauses > 0);
				Assert::AreEqual(uint64_t(0), g1Cnt.g2JointRenewals);

				// G2 with stale credit (no new B credit since tLatch) MUST reject stale credit and release <= 180 s!
				int64_t g2StaleRel = -1;
				C::JointLatchCounters g2StaleCnt;
				runPersistentClassicalStale(C::JointHoldPolicy::G2_JointOncePerBound, g2StaleRel, g2StaleCnt);
				Assert::IsTrue(g2StaleRel > 0 && g2StaleRel <= 180, L"OD-2 G2 rejects stale credit and releases <= 180 s");
				Assert::AreEqual(uint64_t(0), g2StaleCnt.g2JointRenewals);
			}

			// (e) OD-2 G2 once-per-bound fresh joint renewal in a long cut-free single-shot true 1080p->4K take:
			//     Shot stays capped at 0.5*c1 (so ChainReconfirmed cannot fire), while uncapped current-shot credit
			//     is earned fresh at t = tLatch + 120 s -> G2 renews ONCE, then exhausts at tLatch + 240 s!
			{
				ChainFx::ManualClock clk;
				C::EstimatorSettings cfg;
				cfg.jointLatch.enabled = true;
				cfg.jointLatch.holdPolicy = C::JointHoldPolicy::G2_JointOncePerBound;
				cfg.conventionDrivenProposals = true;
				C::ChainEstimator est(cfg, clk.Fn());
				est.SetSynchronous(true);
				est.SetMode(C::EstimatorMode::On);
				est.SetClassicalFresh(true, 0.5, 0.5, false);

				// Reuse one cached synthetic 1080p->4K snapshot template (updating seq & timeUs) for fast 250-s simulation.
				const C::LineSnapshot upTemplate = MakeSynthSnapshot(kUhdW, kUhdH, 1920, 1080, kBilinear, 1, 10, 0, 4301u, false);
				auto feedUpAt = [&](uint64_t seq, int64_t tSec, bool cut) {
					clk.Set(static_cast<double>(tSec) * 1500.0);
					C::LineSnapshot snap = upTemplate;
					snap.seq = seq;
					snap.timeUs = tSec * 1000000;
					snap.flags = cut ? C::kEvCut : 0u;
					snap.cuts = cut ? 1 : 0;
					snap.eventTimeUs = cut ? snap.timeUs : -1;
					Assert::IsTrue(est.SubmitLines(snap));
				};

				uint64_t seq = 10;
				int64_t tLatch = -1;
				for (int s = 0; s <= 10 && tLatch < 0; ++s, ++seq)
				{
					feedUpAt(seq, s, s == 0);
					if (est.StatusInput().latchNx == 1920)
						tLatch = s;
				}
				Assert::IsTrue(tLatch >= 0);

				// One long cut-free shot: step every 5 s up to tLatch + 125 s so fresh current-shot credit arrives right at the 120 s bound!
				for (int dt = 5; dt <= 125; dt += 5, ++seq)
					feedUpAt(seq, tLatch + dt, false);

				const C::JointLatchCounters afterFirstBound = est.GetCounters().jointLatch;
				Assert::AreEqual(1920, est.StatusInput().latchNx, L"G2 renewed once at 120 s and remained latched");
				Assert::AreEqual(uint64_t(1), afterFirstBound.g2JointRenewals, L"G2 performed exactly 1 fresh joint renewal");
				Assert::AreEqual(uint64_t(0), afterFirstBound.releases);

				// Continue cut-free through the SECOND 120 s bound (+245 s) + 65 s ceiling (+310 s):
				// G2 MUST NOT renew a second time without Chain-alone confirmation!
				for (int dt = 130; dt <= 315 && est.StatusInput().latchNx != 0; dt += 5, ++seq)
					feedUpAt(seq, tLatch + dt, false);

				const C::JointLatchCounters afterSecondBound = est.GetCounters().jointLatch;
				Assert::AreEqual(uint64_t(1), afterSecondBound.g2JointRenewals, L"G2 never renews more than once per bound");
				Assert::AreEqual(0, est.StatusInput().latchNx, L"exhausted G2 releases by ceiling");
			}
		}

		// 6. End-to-end ABR NoteAbrStepEvent + ChainEvidence::VoidBefore(t0Us) through real ChainEstimator:
		//    - Direct ChainEvidence::VoidBefore negative control (without VoidBefore, stale pre-step credit persists);
		//    - Pending ABR mark blocks entry in real ChainEstimator;
		//    - Confirmed step-up voids pre-step credit & releases immediately (with stepMark OFF negative control);
		//    - Unconfirmed ABR mark on steady native stream times out cleanly with 0 false confirmations/entries.
		TEST_METHOD(EndToEndAbrStepMarkAndEvidenceVoidBeforeThroughRealChainEstimator)
		{
			using namespace S2Fx;

			// (a) Direct ChainEvidence::VoidBefore negative control: without VoidBefore, pre-step credit stays >= c1!
			{
				C::EvidenceSettings es;
				es.conventionOnly = true;
				es.pairPrior = true;
				C::ChainEvidence evNoVoid(es);
				C::ChainEvidence evVoided(es);

				C::VerifyResult vx, vy;
				vx.candidates.push_back(MakeSyntheticCandidate(960, 8.0, -1.2, 32, 16, 8.0, 0.3, 8.0, 0.3, 0.25, 16));
				vy.candidates.push_back(MakeSyntheticCandidate(540, 8.0, -1.2, 32, 16, 8.0, 0.3, 8.0, 0.3, 0.25, 16));

				auto feedPre = [&](C::ChainEvidence& ev) {
					C::EvidenceUpdate u;
					int64_t tSec = 0;
					for (uint32_t shot = 1; shot <= 3; ++shot)
					{
						for (int k = 0; k < 4; ++k, ++tSec)
						{
							C::AxisObservation ox, oy;
							ox.verify = &vx; ox.axisLen = kUhdW; ox.floorCodes = 0.29; ox.probeRan = true; ox.shotId = shot; ox.timeUs = tSec * 1000000; ox.rasterW = kUhdW; ox.rasterH = kUhdH;
							oy.verify = &vy; oy.axisLen = kUhdH; oy.floorCodes = 0.29; oy.probeRan = true; oy.shotId = shot; oy.timeUs = tSec * 1000000; oy.rasterW = kUhdW; oy.rasterH = kUhdH;
							ev.Submit(0, ox, u);
							ev.Submit(1, oy, u);
						}
					}
				};

				feedPre(evNoVoid);
				feedPre(evVoided);
				Assert::IsTrue(evNoVoid.Summary(0).aboveC1, L"fixture: pre-step 960x540 crossed c1");
				Assert::IsTrue(evVoided.Summary(0).aboveC1);

				// Apply VoidBefore(t0 = 12 s) ONLY to evVoided.
				evVoided.VoidBefore(12 * 1000000);

				// Negative control: without VoidBefore, stale pre-step 960 credit persists above c1!
				Assert::IsTrue(evNoVoid.Summary(0).aboveC1 && evNoVoid.LlrAtN(0, 960) >= evNoVoid.GetThresholds().c1,
					L"negative control: without VoidBefore, stale pre-step credit persists");
				// With VoidBefore(12 s), 100% of pre-step credit is gone!
				Assert::IsFalse(evVoided.Summary(0).aboveC1);
				Assert::IsFalse(evVoided.Summary(0).hasBest);
				Assert::IsTrue(evVoided.LlrAtN(0, 960) < -1e8);
			}

			// (b) Through real ChainEstimator: pending ABR mark blocks entry.
			{
				ChainFx::ManualClock clk;
				C::EstimatorSettings cfg;
				cfg.jointLatch.enabled = true;
				cfg.stepMark.enabled = true;
				cfg.conventionDrivenProposals = true;
				C::ChainEstimator est(cfg, clk.Fn());
				est.SetSynchronous(true);
				est.SetMode(C::EstimatorMode::On);
				est.SetClassicalFresh(true, 0.5, 0.5, false);

				// Accumulate 2 s of 1080p->4K (s = 0..2, dwell < 3 s), then raise pending ABR mark at s == 3
				// right before the first >= 3.0 s dwell-qualifying eval!
				for (int s = 0; s <= 3; ++s)
				{
					clk.Set(static_cast<double>(s) * 1500.0);
					const uint64_t seq = static_cast<uint64_t>(10 + s);
					if (s == 3)
						est.NoteAbrStepEvent(static_cast<int64_t>(s) * 1000000, 0.25, seq);
					const C::LineSnapshot up = MakeSynthSnapshot(kUhdW, kUhdH, 1920, 1080, kBilinear, 1, seq,
						static_cast<int64_t>(s) * 1000000, 5001u + s, s == 0);
					Assert::IsTrue(est.SubmitLines(up));
				}
				Assert::AreEqual(0, est.StatusInput().latchNx, L"pending ABR step mark blocks entry");
				Assert::AreEqual(uint64_t(1), est.GetCounters().stepMark.marksRaised);
				Assert::IsTrue(est.GetCounters().jointLatch.blockedByStepMark >= 1);
			}

			// (c) Confirmed ABR step-up (540p -> 1080p) through real ChainEstimator with stepMark OFF negative control!
			{
				auto runStepUp = [](bool enableStepMark, int& latchAfter, C::EstimatorCounters& outCnt) {
					ChainFx::ManualClock clk;
					C::EstimatorSettings cfg;
					cfg.jointLatch.enabled = true;
					cfg.stepMark.enabled = enableStepMark;
					cfg.conventionDrivenProposals = true;
					C::ChainEstimator est(cfg, clk.Fn());
					est.SetSynchronous(true);
					est.SetMode(C::EstimatorMode::On);
					est.SetClassicalFresh(true, 0.25, 0.25, false); // 960x540

					uint64_t seq = 10;
					int s = 0;
					for (; s <= 8 && est.StatusInput().latchNx == 0; ++s, ++seq)
					{
						clk.Set(static_cast<double>(s) * 1500.0);
						const C::LineSnapshot up540 = MakeSynthSnapshot(kUhdW, kUhdH, 960, 540, kBilinear, 1, seq,
							static_cast<int64_t>(s) * 1000000, 5101u + s, s == 0);
						Assert::IsTrue(est.SubmitLines(up540));
					}
					Assert::AreEqual(960, est.StatusInput().latchNx, L"fixture: latched 960x540 before ABR step-up");

					// ABR step-up at t0 = s seconds to larger 1920x1080 (0.5x0.5):
					const int64_t t0Us = static_cast<int64_t>(s) * 1000000;
					est.NoteAbrStepEvent(t0Us, +0.25, seq);
					est.SetClassicalFresh(true, 0.5, 0.5, false);

					// 2 post-t0 evaluations of 1920x1080 frames:
					for (int k = 0; k < 2; ++k, ++s, ++seq)
					{
						clk.Set(static_cast<double>(s) * 1500.0);
						const C::LineSnapshot up1080 = MakeSynthSnapshot(kUhdW, kUhdH, 1920, 1080, kBilinear, 1, seq,
							static_cast<int64_t>(s) * 1000000, 5201u + k, false);
						Assert::IsTrue(est.SubmitLines(up1080));
					}
					latchAfter = est.StatusInput().latchNx;
					outCnt = est.GetCounters();
				};

				// Negative control (stepMark OFF): stale 960x540 latch stays STUCK after ABR step-up!
				int latchOff = -1;
				C::EstimatorCounters cntOff;
				runStepUp(false, latchOff, cntOff);
				Assert::AreEqual(960, latchOff, L"negative control: without stepMark, stale 960x540 latch stays stuck");
				Assert::AreEqual(uint64_t(0), cntOff.stepMark.immediateReleases);

				// With stepMark ON: 2 post-t0 classical 1080p readings confirm step-up, void pre-t0 credit, and release immediately!
				int latchOn = -1;
				C::EstimatorCounters cntOn;
				runStepUp(true, latchOn, cntOn);
				Assert::AreEqual(0, latchOn, L"confirmed step-up releases stale 960x540 latch immediately");
				Assert::AreEqual(uint64_t(1), cntOn.stepMark.confirmedByClassical);
				Assert::AreEqual(uint64_t(1), cntOn.stepMark.immediateReleases);
				Assert::IsTrue(cntOn.jointLatch.releasesAbr >= 1);
			}

			// (d) Unconfirmed ABR mark on steady native 4K stream times out cleanly with 0 false confirmations/entries!
			{
				ChainFx::ManualClock clk;
				C::EstimatorSettings cfg;
				cfg.jointLatch.enabled = true;
				cfg.stepMark.enabled = true;
				cfg.conventionDrivenProposals = true;
				C::ChainEstimator est(cfg, clk.Fn());
				est.SetSynchronous(true);
				est.SetMode(C::EstimatorMode::On);
				est.SetClassicalFresh(false, 1.0, 1.0, false);

				est.NoteAbrStepEvent(1000000, -0.20, 10);
				for (int s = 1; s <= 3; ++s)
				{
					clk.Set(static_cast<double>(s) * 1500.0);
					const C::LineSnapshot nat = MakeSynthSnapshot(kUhdW, kUhdH, kUhdW, kUhdH, kBilinear, 1,
						static_cast<uint64_t>(9 + s), static_cast<int64_t>(s) * 1000000, 5301u + s, false);
					Assert::IsTrue(est.SubmitLines(nat));
				}
				const C::EstimatorCounters cnt = est.GetCounters();
				Assert::AreEqual(uint64_t(1), cnt.stepMark.marksRaised);
				Assert::AreEqual(uint64_t(1), cnt.stepMark.unconfirmedTimeouts);
				Assert::AreEqual(uint64_t(0), cnt.stepMark.confirmedByChain);
				Assert::AreEqual(uint64_t(0), cnt.stepMark.confirmedByClassical);
				Assert::AreEqual(0, est.StatusInput().latchNx);
			}
		}

		// 7. Wired ChainEdgeStats + useEmpiricalFloor inside real ChainEvidence & ChainEstimator:
		//    - 8-bit TPDF-dithered 80%-flat signal: negative control (edgeStats OFF) drops to LowDetail;
		//      edgeStats ON recovers clean evidence, sets edgeStatsUsed, reclassifies isolated 1/2 to kClassRational;
		//    - 8x8 codec-grid comb stays kClassK8;
		//    - Fallback input visibly sets edgeStatsFallback & edgeStatsReason (never silent);
		//    - Re-quantized 8-in-10 HDMI-loop signal: negative control (useEmpiricalFloor OFF) understates floor 4x;
		//      useEmpiricalFloor ON recovers true floor & increments empiricalFloorEvals;
		//    - Flat clipped signal increments empiricalFloorFallbacks visibly.
		TEST_METHOD(WiredChainEdgeStatsAndEmpiricalFloorRecover8BitDitheredSignalWithVisibleFallbacks)
		{
			using namespace S2Fx;

			// Part A: Wired ChainEdgeStats inside real ChainEvidence.
			{
				const double floor8Tpdf = C::TpdfDitherFloorCodes(4.0); // 1.633 codes

				// Build a VerifyResult where whole-line baseline is diluted by 80% flat dither (lineBase = 2.0 < 1.5*1.633 = 2.45),
				// while 20% (3 of 15) segments per line carry sharp upscaled 1080p->4K edges (segBase = 7.0, segRms = 1.85),
				// plus quiet odd k/8 comb probes at 3/8, 5/8, 7/8.
				C::VerifyResult vrIsolated;
				vrIsolated.candidates.push_back(MakeSyntheticCandidate(1920, 2.0, -0.10, 16, 15, 7.0, 1.85, 1.70, 1.65, 0.5, 3));
				vrIsolated.candidates.push_back(MakeSyntheticCandidate(1440, 2.0, 0.00, 16, 15, 7.0, 7.0, 1.70, 1.65, 0.375, 3));
				vrIsolated.candidates.push_back(MakeSyntheticCandidate(2400, 2.0, 0.01, 16, 15, 7.0, 7.0, 1.70, 1.65, 0.625, 3));
				vrIsolated.candidates.push_back(MakeSyntheticCandidate(3360, 2.0, -0.01, 16, 15, 7.0, 7.0, 1.70, 1.65, 0.875, 3));

				C::AxisObservation obs;
				obs.verify = &vrIsolated;
				obs.axisLen = kUhdW;
				obs.floorCodes = floor8Tpdf;
				obs.probeRan = true;
				obs.probeSeen = true;
				obs.shotId = 1;
				obs.timeUs = -1; // immediate credit for unit comparison
				obs.rasterW = kUhdW;
				obs.rasterH = kUhdH;

				// In-test negative control (Rule 3): with edgeStats OFF, whole-line gating drops the 8-bit dithered signal!
				C::EvidenceSettings offCfg;
				offCfg.edgeStats.enabled = false;
				C::ChainEvidence evOff(offCfg);
				C::EvidenceUpdate upOff;
				Assert::IsTrue(evOff.Submit(0, obs, upOff));
				Assert::IsFalse(upOff.cand[0].informative, L"negative control: whole-line gating drops 80%-flat 8-bit signal");
				Assert::IsTrue((upOff.cand[0].reason & C::kReasonLowDetail) != 0u);
				Assert::IsFalse(upOff.cand[0].edgeStatsUsed);

				// With edgeStats ON on the EXACT SAME VerifyResult: recovers clean evidence & isolates 1/2 notch!
				C::EvidenceSettings onCfg;
				onCfg.edgeStats.enabled = true;
				C::ChainEvidence evOn(onCfg);
				C::EvidenceUpdate upOn;
				Assert::IsTrue(evOn.Submit(0, obs, upOn));
				Assert::IsTrue(upOn.cand[0].informative, L"wired ChainEdgeStats recovers 80%-flat 8-bit signal");
				Assert::IsTrue(upOn.cand[0].edgeStatsUsed);
				Assert::IsFalse(upOn.cand[0].edgeStatsFallback);
				Assert::IsTrue(upOn.cand[0].edgeStatsReason == C::EdgeStatsReason::Ok);
				Assert::AreEqual(C::kClassRational, upOn.cand[0].cls, L"isolated 1/2 notch reclassified from K8 to Rational");
				Assert::IsTrue(upOn.cand[0].combReason == C::EdgeStatsReason::IsolatedHalfNotch);
				Assert::IsTrue(upOn.cand[0].increment > 0.0, L"positive LLR earned on recovered 8-bit dip");

				// 8x8 codec-grid comb check: when 3/8, 5/8, 7/8 ALSO dip (-0.12), 1/2 MUST stay in harsh kClassK8!
				C::VerifyResult vrComb = vrIsolated;
				vrComb.candidates[1].stat = -0.12;
				vrComb.candidates[2].stat = -0.10;
				vrComb.candidates[3].stat = -0.15;
				C::AxisObservation obsComb = obs;
				obsComb.verify = &vrComb;
				C::ChainEvidence evComb(onCfg);
				C::EvidenceUpdate upComb;
				Assert::IsTrue(evComb.Submit(0, obsComb, upComb));
				Assert::AreEqual(C::kClassK8, upComb.cand[0].cls, L"8x8 codec-grid comb kept safely in K8");
				Assert::IsTrue(upComb.cand[0].combReason == C::EdgeStatsReason::CombGridDetected);

				// Visible Rule 4 fallback: whole-line gating passes (lineBase = 8.0), edgeStats ON,
				// but segment arrays are empty -> MUST fall back visibly (edgeStatsFallback == true, edgeStatsReason != Ok/Disabled)!
				C::VerifyResult vrFallback;
				C::CandidateResult cf = MakeSyntheticCandidate(1920, 8.0, -0.8, 16, 15, 8.0, 1.5, 8.0, 1.5, 0.5, 15);
				cf.segSum.clear(); // force ComputeSegmentGatedStat failure!
				cf.segCount = 0;
				vrFallback.candidates.push_back(cf);
				C::AxisObservation obsFb = obs;
				obsFb.verify = &vrFallback;
				C::ChainEvidence evFb(onCfg);
				C::EvidenceUpdate upFb;
				Assert::IsTrue(evFb.Submit(0, obsFb, upFb));
				Assert::IsTrue(upFb.cand[0].informative, L"clean fallback to whole-line trimmed mean");
				Assert::IsFalse(upFb.cand[0].edgeStatsUsed);
				Assert::IsTrue(upFb.cand[0].edgeStatsFallback, L"Rule 4: fallback to whole-line stat is visibly flagged");
				Assert::IsTrue(upFb.cand[0].edgeStatsReason == C::EdgeStatsReason::InvalidInput);
			}

			// Part B: Wired useEmpiricalFloor inside real ChainEstimator on simulated 8-in-10 HDMI loop & flat clipped fallback.
			{
				// Simulated re-quantized 8-in-10 HDMI loop: step == 1 (10-bit codes, not multiples of 4),
				// 75% quiet background with sigma = 1.30 codes + 25% sharp upscaled edges.
				const C::LineSnapshot loopSnap = MakeSynthSnapshot(kUhdW, kUhdH, 1920, 1080, kBilinear, 1,
					10, 1000000, 6001u, true, 1.30, false, 0.75);
				Assert::AreEqual(1, C::DetectStepCodes(loopSnap), L"fixture: 8-in-10 link looks like step=1 to bit-mask");

				// In-test negative control (useEmpiricalFloor == false): hardcoded floor is 0.289 (> 4x too small!).
				double floorOff = 0.0;
				{
					ChainFx::ManualClock clk;
					C::EstimatorSettings cfg;
					cfg.useEmpiricalFloor = false;
					C::ChainEstimator est(cfg, clk.Fn());
					est.SetSynchronous(true);
					est.SetMode(C::EstimatorMode::On);
					est.SetEvaluationObserver([&](const C::EvaluationDetail& d) { floorOff = d.axis[0].floorCodes; });
					Assert::IsTrue(est.SubmitLines(loopSnap));
					Assert::IsTrue(floorOff < 0.35, L"negative control: hardcoded step-1 floor is ~0.289 (> 4x below true 1.30)");
					Assert::AreEqual(uint64_t(0), est.GetCounters().empiricalFloorEvals);
				}

				// With useEmpiricalFloor == true on the EXACT SAME snapshot: recovers ~1.30 codes!
				double floorOn = 0.0;
				{
					ChainFx::ManualClock clk;
					C::EstimatorSettings cfg;
					cfg.useEmpiricalFloor = true;
					C::ChainEstimator est(cfg, clk.Fn());
					est.SetSynchronous(true);
					est.SetMode(C::EstimatorMode::On);
					est.SetEvaluationObserver([&](const C::EvaluationDetail& d) { floorOn = d.axis[0].floorCodes; });
					Assert::IsTrue(est.SubmitLines(loopSnap));
					Assert::IsTrue(floorOn > 0.95 && floorOn < 1.75, L"empirical floor measures true ~1.30 code noise on 8-in-10 link");
					Assert::IsTrue(floorOn > 3.0 * floorOff, L"empirical floor beats hardcoded step-1 floor by > 3x");
					Assert::AreEqual(uint64_t(1), est.GetCounters().empiricalFloorEvals);
					Assert::AreEqual(uint64_t(0), est.GetCounters().empiricalFloorFallbacks);

					// Visible fallback on completely constant clipped flat frame: empiricalFloorFallbacks MUST increment!
					clk.Set(2000.0);
					double floorFlat = 0.0;
					est.SetEvaluationObserver([&](const C::EvaluationDetail& d) { floorFlat = d.axis[0].floorCodes; });
					const C::LineSnapshot flatSnap = MakeFlatSnapshot(kUhdW, kUhdH, 512, 20, 2000000, false);
					Assert::IsTrue(est.SubmitLines(flatSnap));
					Assert::AreEqual(uint64_t(2), est.GetCounters().empiricalFloorEvals);
					Assert::AreEqual(uint64_t(1), est.GetCounters().empiricalFloorFallbacks, L"Rule 4: theoretical floor fallback is visibly counted");
					Assert::IsTrue(std::fabs(floorFlat - C::QuantizationFloorCodes(4.0)) < 1e-6,
						L"constant multiple-of-4 frame falls back cleanly to theoretical 8-bit floor");
				}
			}
		}

		// 8. Adversarial edge cases: NaN / Inf / zero / negative inputs through SetClassicalFresh / NoteAbrStepEvent /
		//    VoidBefore / BuildConventionCandidates, empty/short buffers, and rapid ON -> OFF -> ON toggle reset safety.
		TEST_METHOD(AdversarialEdgeCasesNanZeroNegativeShortBuffersAndRapidToggleResetSafety)
		{
			using namespace S2Fx;
			const double nan = std::numeric_limits<double>::quiet_NaN();
			const double inf = std::numeric_limits<double>::infinity();

			// (a) BuildConventionCandidates malformed / short / null inputs return 0 cleanly.
			{
				int out[8] = {};
				Assert::AreEqual(0, C::BuildConventionCandidates(0, 0, 3840, 2160, 1920, 1920, nullptr, 0, true, out, 8));
				Assert::AreEqual(0, C::BuildConventionCandidates(0, 15, 3840, 2160, 8, 8, nullptr, 0, true, out, 8));
				Assert::AreEqual(0, C::BuildConventionCandidates(0, 3840, 3840, 2160, 1920, 0, nullptr, 0, true, nullptr, 8));
				Assert::AreEqual(0, C::BuildConventionCandidates(0, 3840, 3840, 2160, 1920, 0, nullptr, 0, true, out, 0));
				Assert::AreEqual(0, C::BuildConventionCandidates(0, 3840, 0, 0, 1920, 0, nullptr, 0, true, out, 8));
			}

			// (b) ChainEvidence::VoidBefore boundary timestamps (negative, 0, INT64_MAX) never crash.
			{
				C::ChainEvidence ev;
				ev.VoidBefore(-1);
				ev.VoidBefore(0);
				ev.VoidBefore((std::numeric_limits<int64_t>::max)());
				Assert::IsFalse(ev.Summary(0).hasBest);
			}

			// (c) Real ChainEstimator with NaN / Inf / zero / negative SetClassicalFresh & NoteAbrStepEvent + empty/short LineSnapshot.
			{
				ChainFx::ManualClock clk;
				C::EstimatorSettings cfg;
				cfg.jointLatch.enabled = true;
				cfg.stepMark.enabled = true;
				cfg.conventionDrivenProposals = true;
				cfg.useEmpiricalFloor = true;
				cfg.evidence.edgeStats.enabled = true;
				C::ChainEstimator est(cfg, clk.Fn());
				est.SetSynchronous(true);
				est.SetMode(C::EstimatorMode::On);

				// Malformed classical fresh & malformed ABR step events:
				est.SetClassicalFresh(true, nan, 0.5, false);
				est.NoteAbrStepEvent(-100, 0.25, 10);
				est.NoteAbrStepEvent(1000000, nan, 10);
				est.NoteAbrStepEvent(1000000, inf, 10);

				// Empty LineSnapshot (0 rows, 0 cols): must never crash!
				C::LineSnapshot emptySnap;
				emptySnap.seq = 10;
				emptySnap.timeUs = 1000000;
				Assert::IsTrue(est.SubmitLines(emptySnap));
				Assert::AreEqual(uint64_t(3), est.GetCounters().stepMark.rejectedInvalidInput, L"all 3 invalid ABR events rejected visibly");
				Assert::AreEqual(uint64_t(0), est.GetCounters().stepMark.marksRaised);

				// Short 4-pixel snapshot + negative/zero/Inf classical ratios: must never crash or latch!
				clk.Set(2000.0);
				est.SetClassicalFresh(true, -0.5, 0.0, false);
				C::LineSnapshot tinySnap;
				tinySnap.seq = 20;
				tinySnap.timeUs = 2000000;
				tinySnap.rasterW = 4; tinySnap.rasterH = 4;
				tinySnap.active = { 0, 0, 4, 4 };
				tinySnap.inner = { 0, 0, 4, 4 };
				tinySnap.rowCount = 1; tinySnap.colCount = 1;
				tinySnap.rowY = { 2 }; tinySnap.colX = { 2 };
				tinySnap.rows = { 100, 200, 300, 400 };
				tinySnap.cols = { 100, 200, 300, 400 };
				Assert::IsTrue(est.SubmitLines(tinySnap));
				Assert::AreEqual(0, est.StatusInput().latchNx);

				clk.Set(3000.0);
				est.SetClassicalFresh(true, inf, -inf, false);
				const C::LineSnapshot nat = MakeSynthSnapshot(kUhdW, kUhdH, kUhdW, kUhdH, kBilinear, 1, 30, 3000000, 7001u, false);
				Assert::IsTrue(est.SubmitLines(nat));
				G::Record rec;
				Assert::IsTrue(est.Held().ReadLatest(0, rec));
				Assert::IsTrue(std::isfinite(rec.applied[0].ratio) && std::isfinite(rec.applied[1].ratio));
				Assert::AreEqual(0, est.StatusInput().latchNx);
			}

			// (d) Rapid toggle ON -> OFF -> ON reset safety (all Stage S2 switches through ChainEstimator setters).
			{
				ChainFx::ManualClock clk;
				C::EstimatorSettings cfg;
				cfg.jointLatch.enabled = true;
				cfg.stepMark.enabled = true;
				cfg.conventionDrivenProposals = true;
				cfg.useEmpiricalFloor = true;
				C::ChainEstimator est(cfg, clk.Fn());
				est.SetSynchronous(true);
				est.SetMode(C::EstimatorMode::On);
				est.SetClassicalFresh(true, 0.5, 0.5, false);

				// Phase 1 (ON): latch 1920x1080 in 1 shot (flatFraction = 0.50 + 10-bit TPDF dither provides realistic quiet segments for useEmpiricalFloor).
				for (int s = 0; s <= 5; ++s)
				{
					clk.Set(static_cast<double>(s) * 1500.0);
					const C::LineSnapshot up = MakeSynthSnapshot(kUhdW, kUhdH, 1920, 1080, kBilinear, 1,
						static_cast<uint64_t>(10 + s), static_cast<int64_t>(s) * 1000000, 7101u + s, s == 0, 0.0, true, 0.50);
					Assert::IsTrue(est.SubmitLines(up));
				}
				Assert::AreEqual(1920, est.StatusInput().latchNx, L"Phase 1: latched 1920x1080");

				// Phase 2 (toggle ALL Stage S2 switches OFF): must cleanly release & reset without leaking stale latch!
				C::JointLatchSettings jOff;
				jOff.enabled = false;
				C::StepMarkSettings smOff;
				smOff.enabled = false;
				C::EdgeStatsSettings esOff;
				esOff.enabled = false;
				est.SetJointLatchSettings(jOff);
				est.SetStepMarkSettings(smOff);
				est.SetConventionDrivenProposals(false);
				est.SetEdgeStatsSettings(esOff, false);

				Assert::IsFalse(est.GetJointLatchSettings().enabled);
				Assert::IsFalse(est.GetStepMarkSettings().enabled);
				Assert::IsFalse(est.ConventionDrivenProposals());
				Assert::IsFalse(est.GetEdgeStatsSettings().enabled);
				Assert::AreEqual(0, est.StatusInput().latchNx, L"toggling Stage S2 OFF cleanly releases latch");
				Assert::IsTrue(est.GetCounters().jointLatch.releasesReset >= 1, L"reset release counted visibly");

				// Phase 3 (toggle Stage S2 BACK ON): clean state, latches again cleanly in 1 shot!
				C::JointLatchSettings jOn;
				jOn.enabled = true;
				C::StepMarkSettings smOn;
				smOn.enabled = true;
				est.SetJointLatchSettings(jOn);
				est.SetStepMarkSettings(smOn);
				est.SetConventionDrivenProposals(true);
				est.SetClassicalFresh(true, 0.5, 0.5, false);

				for (int s = 10; s <= 15; ++s)
				{
					clk.Set(static_cast<double>(s) * 1500.0);
					const C::LineSnapshot up = MakeSynthSnapshot(kUhdW, kUhdH, 1920, 1080, kBilinear, 1,
						static_cast<uint64_t>(10 + s), static_cast<int64_t>(s) * 1000000, 7201u + s, s == 10, 0.0, true, 0.50);
					Assert::IsTrue(est.SubmitLines(up));
				}
				Assert::AreEqual(1920, est.StatusInput().latchNx, L"Phase 3: cleanly re-latches after OFF->ON toggle");
				Assert::AreEqual(uint64_t(2), est.GetCounters().jointLatch.entriesJoint);
			}
		}
	};
}
