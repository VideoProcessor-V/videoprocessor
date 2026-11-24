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

#include <VP-V/modules/input/source_analysis/SourceResolutionDetector.h>
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

		// Builds a letterboxed/cropped LineSnapshot where `active` is a sub-rectangle of `(rasterW, rasterH)`.
		inline C::LineSnapshot MakeLetterboxedSynthSnapshot(
			int rasterW, int rasterH, C::LineRect active, int srcActiveW, int srcActiveH, int kernel, int step,
			uint64_t seq, int64_t timeUs, uint64_t seed, bool cut = false)
		{
			const C::LinePlan plan = C::LineSampler::Plan(active, seq);
			Assert::IsTrue(plan.valid, L"fixture: valid letterboxed LinePlan");

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
			const int rowOff = plan.inner.x - plan.active.x;
			const int colOff = plan.inner.y - plan.active.y;
			for (int j = 0; j < plan.rowCount; ++j)
			{
				SynthLine1D(srcActiveW, active.w, kernel, step, rng, line);
				uint16_t* dst = &snap.rows[static_cast<size_t>(j) * static_cast<size_t>(plan.inner.w)];
				for (int i = 0; i < plan.inner.w; ++i)
					dst[i] = static_cast<uint16_t>(line[static_cast<size_t>(rowOff + i)]);
			}
			for (int j = 0; j < plan.colCount; ++j)
			{
				SynthLine1D(srcActiveH, active.h, kernel, step, rng, line);
				uint16_t* dst = &snap.cols[static_cast<size_t>(j) * static_cast<size_t>(plan.inner.h)];
				for (int i = 0; i < plan.inner.h; ++i)
					dst[i] = static_cast<uint16_t>(line[static_cast<size_t>(colOff + i)]);
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

		inline C::LineSnapshot MakeLetterboxedFlatSnapshot(
			int rasterW, int rasterH, C::LineRect active, uint16_t code, uint64_t seq, int64_t timeUs, bool cut = false)
		{
			const C::LinePlan plan = C::LineSampler::Plan(active, seq);
			Assert::IsTrue(plan.valid, L"fixture: valid letterboxed flat LinePlan");
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

		inline C::NullTables MakeCalibratedEdgeStatsNulls()
		{
			C::NullTables t = C::NullTables::Placeholder();
			t.placeholder = false;
			t.statKind = C::NullStatKind::SegmentExcessVariance;
			return t;
		}

		inline C::ClassicalFreshInput MakeClassicalFresh(
			bool valid, double ratioH, double ratioV, bool detailAbove,
			uint64_t frameSeq, int64_t timeUs, double analysisIntervalSec = 1.0,
			bool mixed = false, bool subtitle = false, bool inFade = false)
		{
			C::ClassicalFreshInput cf;
			cf.valid = valid;
			cf.ratioH = ratioH;
			cf.ratioV = ratioV;
			cf.detailAbove = detailAbove;
			cf.mixed = mixed;
			cf.subtitle = subtitle;
			cf.inFade = inFade;
			cf.frameSeq = frameSeq;
			cf.timeUs = timeUs;
			cf.analysisIntervalSec = analysisIntervalSec;
			return cf;
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

					// 2 post-t0 evaluations with uninformative flat chain lines and 2 distinct fresh classical frames
					// so Classical A's 2-frame confirmation streak is isolated cleanly (Chain-only 1-eval confirmation
					// is tested separately in High1...):
					for (int k = 0; k < 2; ++k, ++s, ++seq)
					{
						const int64_t evalTimeUs = static_cast<int64_t>(s) * 1000000;
						est.SetClassicalFresh(MakeClassicalFresh(true, 0.5, 0.5, false, seq, evalTimeUs, 1.0));
						clk.Set(static_cast<double>(s) * 1500.0);
						const C::LineSnapshot flatPost = MakeFlatSnapshot(kUhdW, kUhdH, 512, seq,
							evalTimeUs, false);
						Assert::IsTrue(est.SubmitLines(flatPost));
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
				Assert::AreEqual(uint64_t(0), cntOn.stepMark.confirmedByChain);
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

				// With edgeStats ON on the EXACT SAME VerifyResult (with calibrated SegmentExcessVariance null tables):
				// recovers clean evidence & isolates 1/2 notch!
				const C::NullTables calEdgeNulls = MakeCalibratedEdgeStatsNulls();
				C::EvidenceSettings onCfg;
				onCfg.edgeStats.enabled = true;
				C::ChainEvidence evOn(onCfg, calEdgeNulls);
				C::EvidenceUpdate upOn;
				Assert::IsTrue(evOn.Submit(0, obs, upOn));
				Assert::IsTrue(upOn.cand[0].informative, L"wired ChainEdgeStats recovers 80%-flat 8-bit signal");
				Assert::IsTrue(upOn.cand[0].edgeStatsUsed);
				Assert::IsFalse(upOn.cand[0].edgeStatsFallback);
				Assert::IsTrue(upOn.cand[0].edgeStatsReason == C::EdgeStatsReason::Ok);
				Assert::AreEqual(C::kClassRational, upOn.cand[0].cls, L"isolated 1/2 notch reclassified from K8 to Rational");
				Assert::IsTrue(upOn.cand[0].combReason == C::EdgeStatsReason::IsolatedHalfNotch);
				Assert::IsTrue(upOn.cand[0].increment > 0.0, L"positive LLR earned on recovered 8-bit dip");

				// 8x8 codec-grid comb check (HIGH-3a): 80%-flat 8-bit 8x8 grid where whole-line cr.stat on 3/8, 5/8, 7/8
				// is diluted above combDipThreshold (-0.03), while the 20% informative edge segments carry genuine dips!
				// ChainEvidence::Submit MUST evaluate 3/8, 5/8, 7/8 via ComputeSegmentGatedStat and keep 1/2 in harsh kClassK8.
				C::VerifyResult vrComb;
				vrComb.candidates.push_back(MakeSyntheticCandidate(1920, 2.0, -0.10, 16, 15, 7.0, 1.85, 1.70, 1.65, 0.5, 3));
				vrComb.candidates.push_back(MakeSyntheticCandidate(1440, 2.0, -0.015, 16, 15, 7.0, 4.50, 1.70, 1.65, 0.375, 3));
				vrComb.candidates.push_back(MakeSyntheticCandidate(2400, 2.0, -0.012, 16, 15, 7.0, 4.80, 1.70, 1.65, 0.625, 3));
				vrComb.candidates.push_back(MakeSyntheticCandidate(3360, 2.0, -0.018, 16, 15, 7.0, 4.00, 1.70, 1.65, 0.875, 3));
				C::AxisObservation obsComb = obs;
				obsComb.verify = &vrComb;
				C::ChainEvidence evComb(onCfg, calEdgeNulls);
				C::EvidenceUpdate upComb;
				Assert::IsTrue(evComb.Submit(0, obsComb, upComb));
				Assert::AreEqual(C::kClassK8, upComb.cand[0].cls, L"80%-flat 8x8 codec-grid comb kept safely in K8 via segment-gated comb probes");
				Assert::IsTrue(upComb.cand[0].combReason == C::EdgeStatsReason::CombGridDetected);

				// In-test negative control (Rule 3): using diluted whole-line cr.stat on 3/8, 5/8, 7/8 alone (the old
				// naive estimator) falls above -0.03 and misclassifies the EXACT SAME 80%-flat 8x8 comb as IsolatedHalfNotch!
				const C::HalfCombCheckResult naiveWholeLineComb = C::ClassifyHalfNotchWithCombCheck(
					1920, kUhdW, upComb.cand[0].stat, true,
					vrComb.candidates[1].stat, vrComb.candidates[2].stat, vrComb.candidates[3].stat, onCfg.edgeStats);
				Assert::AreEqual(C::kClassRational, naiveWholeLineComb.resolvedNullClass,
					L"negative control: diluted whole-line cr.stat alone misclassifies 80%-flat 8x8 comb as Rational");
				Assert::IsTrue(naiveWholeLineComb.reason == C::EdgeStatsReason::IsolatedHalfNotch,
					L"negative control: diluted whole-line cr.stat alone reports IsolatedHalfNotch");

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

		// 9. HIGH-1: End-to-end ABR step-up confirmed by Chain ALONE when classical A is completely silent (cf.valid == false),
		//    with spurious-mark non-release check and stepMark OFF negative control proving the hazard is real.
		TEST_METHOD(High1AbrStepUpConfirmedByChainAloneWhenClassicalSilentWithNegativeControl)
		{
			using namespace S2Fx;

			// (a) 1080p -> native 4K step-up while classical A is completely silent after latching, preceded by a spurious
			//     ABR mark while the 1080p stream is genuinely static (must NOT falsely release).
			{
				auto runChainOnlyStepUpToNative = [](bool enableStepMark, int& latchAfterSpurious, int& latchAfterStepUp, C::EstimatorCounters& outCnt) {
					ChainFx::ManualClock clk;
					C::EstimatorSettings cfg;
					cfg.safetyRules = true;
					cfg.jointLatch.enabled = true;
					cfg.stepMark.enabled = enableStepMark;
					cfg.conventionDrivenProposals = true;
					C::ChainEstimator est(cfg, clk.Fn());
					est.SetSynchronous(true);
					est.SetMode(C::EstimatorMode::On);
					// Classical A is completely silent throughout (pure Chain-alone / blind-A HDMI-loop regime).
					est.SetClassicalFresh(false, 1.0, 1.0, false);

					uint64_t seq = 10;
					int s = 0;
					// Latch 1920x1080 via 2 shots (Chain-alone).
					for (int shot = 0; shot < 2; ++shot)
					{
						for (int k = 0; k < 5; ++k, ++s, ++seq)
						{
							clk.Set(static_cast<double>(s) * 1500.0);
							const C::LineSnapshot up = MakeSynthSnapshot(kUhdW, kUhdH, 1920, 1080, kBilinear, 1,
								seq, static_cast<int64_t>(s) * 1000000, 8001u + seq, k == 0);
							Assert::IsTrue(est.SubmitLines(up));
						}
					}
					Assert::AreEqual(1920, est.StatusInput().latchNx, L"fixture: latched 1920x1080 via Chain-alone");
					Assert::AreEqual(1080, est.StatusInput().latchNy);

					// Spurious ABR mark while stream stays genuinely static at 1920x1080 (classical still silent):
					// Must time out unconfirmed after 2 evals with ZERO false immediate release!
					const int64_t tSpuriousUs = static_cast<int64_t>(s) * 1000000;
					est.NoteAbrStepEvent(tSpuriousUs, +0.25, seq);
					for (int k = 0; k < 2; ++k, ++s, ++seq)
					{
						clk.Set(static_cast<double>(s) * 1500.0);
						const C::LineSnapshot staticUp = MakeSynthSnapshot(kUhdW, kUhdH, 1920, 1080, kBilinear, 1,
							seq, static_cast<int64_t>(s) * 1000000, 8101u + seq, false);
						Assert::IsTrue(est.SubmitLines(staticUp));
					}
					latchAfterSpurious = est.StatusInput().latchNx;

					// Real ABR step-up from 1920x1080 -> native 4K (3840x2160) while classical A remains COMPLETELY silent!
					const int64_t tStepUs = static_cast<int64_t>(s) * 1000000;
					est.SetClassicalFresh(false, 1.0, 1.0, false);
					est.NoteAbrStepEvent(tStepUs, +0.50, seq);
					for (int k = 0; k < 2; ++k, ++s, ++seq)
					{
						clk.Set(static_cast<double>(s) * 1500.0);
						const C::LineSnapshot nat = MakeSynthSnapshot(kUhdW, kUhdH, kUhdW, kUhdH, kBilinear, 1,
							seq, static_cast<int64_t>(s) * 1000000, 8201u + seq, false);
						Assert::IsTrue(est.SubmitLines(nat));
					}
					latchAfterStepUp = est.StatusInput().latchNx;
					outCnt = est.GetCounters();
				};

				// In-test negative control (Rule 3): with stepMark OFF on the EXACT SAME stream, the estimator
				// stays stuck latched on stale 1920x1080 at the exact same post-step eval point!
				int spuriousOff = -1, stepOff = -1;
				C::EstimatorCounters cntOff;
				runChainOnlyStepUpToNative(false, spuriousOff, stepOff, cntOff);
				Assert::AreEqual(1920, spuriousOff);
				Assert::AreEqual(1920, stepOff, L"negative control: without stepMark, stale 1920x1080 latch stays stuck after step-up to native 4K");
				Assert::AreEqual(uint64_t(0), cntOff.stepMark.confirmedByChain);
				Assert::AreEqual(uint64_t(0), cntOff.stepMark.immediateReleases);

				// With stepMark ON: spurious mark times out safely without releasing; real step-up is confirmed by
				// Chain ALONE (confirmedByChain >= 1, confirmedByClassical == 0) and releases immediately!
				int spuriousOn = -1, stepOn = -1;
				C::EstimatorCounters cntOn;
				runChainOnlyStepUpToNative(true, spuriousOn, stepOn, cntOn);
				Assert::AreEqual(1920, spuriousOn, L"spurious ABR mark on static 1080p must not cause false immediate release");
				Assert::AreEqual(uint64_t(1), cntOn.stepMark.unconfirmedTimeouts);
				Assert::AreEqual(0, stepOn, L"chain-confirmed ABR step-up to native 4K releases immediately while classical is silent");
				Assert::IsTrue(cntOn.stepMark.confirmedByChain >= 1, L"confirmedByChain visibly incremented");
				Assert::AreEqual(uint64_t(0), cntOn.stepMark.confirmedByClassical, L"classical was completely silent");
				Assert::AreEqual(uint64_t(1), cntOn.stepMark.immediateReleases);
				Assert::IsTrue(cntOn.jointLatch.releasesAbr >= 1);
			}

			// (b) 540p -> 1080p step-up while classical A is completely silent (step-up to higher upscaled rung).
			{
				auto runChainOnly540To1080 = [](bool enableStepMark, int& latchAfter, C::EstimatorCounters& outCnt) {
					ChainFx::ManualClock clk;
					C::EstimatorSettings cfg;
					cfg.safetyRules = true;
					cfg.jointLatch.enabled = true;
					cfg.stepMark.enabled = enableStepMark;
					cfg.conventionDrivenProposals = true;
					C::ChainEstimator est(cfg, clk.Fn());
					est.SetSynchronous(true);
					est.SetMode(C::EstimatorMode::On);
					est.SetClassicalFresh(false, 1.0, 1.0, false);

					uint64_t seq = 10;
					int s = 0;
					for (int shot = 0; shot < 2; ++shot)
					{
						for (int k = 0; k < 5; ++k, ++s, ++seq)
						{
							clk.Set(static_cast<double>(s) * 1500.0);
							const C::LineSnapshot up540 = MakeSynthSnapshot(kUhdW, kUhdH, 960, 540, kBilinear, 1,
								seq, static_cast<int64_t>(s) * 1000000, 8301u + seq, k == 0);
							Assert::IsTrue(est.SubmitLines(up540));
						}
					}
					Assert::AreEqual(960, est.StatusInput().latchNx, L"fixture: latched 960x540 via Chain-alone");

					// Step up to 1920x1080 while classical A remains completely silent!
					const int64_t t0Us = static_cast<int64_t>(s) * 1000000;
					est.SetClassicalFresh(false, 1.0, 1.0, false);
					est.NoteAbrStepEvent(t0Us, +0.25, seq);
					for (int k = 0; k < 2; ++k, ++s, ++seq)
					{
						clk.Set(static_cast<double>(s) * 1500.0);
						const C::LineSnapshot up1080 = MakeSynthSnapshot(kUhdW, kUhdH, 1920, 1080, kBilinear, 1,
							seq, static_cast<int64_t>(s) * 1000000, 8401u + seq, false);
						Assert::IsTrue(est.SubmitLines(up1080));
					}
					latchAfter = est.StatusInput().latchNx;
					outCnt = est.GetCounters();
				};

				int latchOff = -1, latchOn = -1;
				C::EstimatorCounters cntOff, cntOn;
				runChainOnly540To1080(false, latchOff, cntOff);
				runChainOnly540To1080(true, latchOn, cntOn);

				Assert::AreEqual(960, latchOff, L"negative control: without stepMark, stale 960x540 stays latched on 540p->1080p step-up");
				Assert::AreEqual(0, latchOn, L"chain-confirmed 540p->1080p step-up releases immediately while classical is silent");
				Assert::IsTrue(cntOn.stepMark.confirmedByChain >= 1);
				Assert::AreEqual(uint64_t(0), cntOn.stepMark.confirmedByClassical);
				Assert::AreEqual(uint64_t(1), cntOn.stepMark.immediateReleases);
				Assert::IsTrue(cntOn.jointLatch.releasesAbr >= 1);
			}
		}

		// 10. HIGH-2: Adversarial R1 harmonic override test exercising ChainEstimator::ApplyJointLatch on BOTH
		//     override paths (RejectedSubHarmonicDetailAbove AND SelectedFundamentalConfirmed), proving an
		//     under-c1 candidate NEVER inherits the rejected/alias candidate's >= c1 LLR to falsely enter via
		//     EnteredChainAlone, with in-test naive/unprotected negative controls on the exact same LLR pairs.
		TEST_METHOD(High2R1HarmonicOverrideRefreshesLlrNeverFalselyEntersChainAlone)
		{
			using namespace S2Fx;

			// (a) RejectedSubHarmonicDetailAbove through real ChainEstimator:
			//     960x540 accumulates >= c1 over 2 shots (so haveChainPair == true at 960x540 with llr >= c1),
			//     while 1920x1080 only earns current-shot credit in Shot 2 (jNeed <= open, LlrAtN < c1).
			//     When classical reports clean 1920x1080 (marking 960x540 detailAbove):
			//       - If classical jointly supports 1920x1080 (minShotDwellSec = 3.0): enters via EnteredJoint ONLY!
			//       - If classical is ineligible for joint (minShotDwellSec = 10.0): MUST NOT enter at all!
			{
				auto runRejectedSubHarmonic = [](double jointDwellSec, int& outLatchNx, int& outLatchNy, C::EstimatorCounters& outCnt) {
					ChainFx::ManualClock clk;
					C::EstimatorSettings cfg;
					cfg.safetyRules = true;
					cfg.jointLatch.enabled = true;
					cfg.jointLatch.minShotDwellSec = jointDwellSec;
					cfg.conventionDrivenProposals = true;
					C::ChainEstimator est(cfg, clk.Fn());
					est.SetSynchronous(true);
					est.SetMode(C::EstimatorMode::On);

					uint64_t seq = 10;
					int s = 0;
					// Shot 1 (s = 0..4): 960x540 bilinear with cf.detailAbove = true (blocks latch while 960x540 earns Shot 1 cap).
					est.SetClassicalFresh(true, 0.5, 0.5, true);
					for (int k = 0; k < 5; ++k, ++s, ++seq)
					{
						clk.Set(static_cast<double>(s) * 1500.0);
						const C::LineSnapshot up540 = MakeSynthSnapshot(kUhdW, kUhdH, 960, 540, kBilinear, 1,
							seq, static_cast<int64_t>(s) * 1000000, 8501u + seq, k == 0);
						Assert::IsTrue(est.SubmitLines(up540));
					}
					Assert::AreEqual(0, est.StatusInput().latchNx, L"fixture: detailAbove blocked latch during Shot 1");

					// Shot 2 part 1 (s = 5..9, 5 evals): 960x540 bilinear with cf.detailAbove = true -> Shot 2 flushes at s = 8,
					// building dx.open(960) = +25.0 (capped to +0.5*c1) so 960x540 reaches LLR >= c1 across 2 shots
					// while keeping headroom above jNeed when 1920x1080 evals follow in the same shot!
					for (int k = 0; k < 5; ++k, ++s, ++seq)
					{
						clk.Set(static_cast<double>(s) * 1500.0);
						const C::LineSnapshot up540 = MakeSynthSnapshot(kUhdW, kUhdH, 960, 540, kBilinear, 1,
							seq, static_cast<int64_t>(s) * 1000000, 8601u + seq, k == 0);
						Assert::IsTrue(est.SubmitLines(up540));
					}
					Assert::AreEqual(0, est.StatusInput().latchNx, L"fixture: detailAbove still blocked latch at start of Shot 2");

					// Shot 2 part 2 (s = 10..14, 5 evals, same open shot, no cut, Shot 1 still inside ~16 s window):
					// Even if 1920x1080 lost -3.0/eval on s = 5..9 (-15.0), 5 evals of 1920x1080 (+25.0) bring
					// dx.open(1920) >= +10.0 >= jNeed while keeping dx.open(960) >= +25.0 - 15.0 = +10.0 >= jNeed
					// and 960x540 LlrAtN >= c1 (while 1920x1080 shot-capped LlrAtN stays <= 0.5*c1 < c1!).
					// At s = 14 (k == 4) switch classical to clean 1920x1080 (detailAbove = false), triggering R1
					// RejectedSubHarmonicDetailAbove override from 960x540 -> 1920x1080!
					for (int k = 0; k < 5; ++k, ++s, ++seq)
					{
						if (k == 4)
							est.SetClassicalFresh(true, 0.5, 0.5, false);
						clk.Set(static_cast<double>(s) * 1500.0);
						const C::LineSnapshot up1080 = MakeSynthSnapshot(kUhdW, kUhdH, 1920, 1080, kBilinear, 1,
							seq, static_cast<int64_t>(s) * 1000000, 8701u + seq, false);
						Assert::IsTrue(est.SubmitLines(up1080));
					}
					outLatchNx = est.StatusInput().latchNx;
					outLatchNy = est.StatusInput().latchNy;
					outCnt = est.GetCounters();
				};

				// (a1) Joint-eligible: enters 1920x1080 via EnteredJoint, NEVER via EnteredChainAlone!
				int jNx = -1, jNy = -1;
				C::EstimatorCounters jCnt;
				runRejectedSubHarmonic(C::kMinShotDwellSec, jNx, jNy, jCnt);
				Assert::AreEqual(1920, jNx, L"R1 RejectedSubHarmonicDetailAbove selects clean 1920x1080");
				Assert::AreEqual(1080, jNy);
				Assert::AreEqual(uint64_t(1), jCnt.jointLatch.entriesJoint, L"enters via EnteredJoint when classical jointly supports 1920x1080");
				Assert::AreEqual(uint64_t(0), jCnt.jointLatch.entriesChainAlone, L"under-c1 1920x1080 MUST NOT inherit 960x540 >= c1 LLR for EnteredChainAlone");
				Assert::IsTrue(jCnt.r1HarmonicSelections >= 1);

				// (a2) Joint-ineligible (Shot 2 dwell = 9.0 s < minShotDwellSec = 15.0 s): MUST NOT enter at all!
				int noNx = -1, noNy = -1;
				C::EstimatorCounters noCnt;
				runRejectedSubHarmonic(15.0, noNx, noNy, noCnt);
				Assert::AreEqual(0, noNx, L"when classical is ineligible for joint, under-c1 1920x1080 MUST NOT enter via EnteredChainAlone");
				Assert::AreEqual(uint64_t(0), noCnt.jointLatch.entriesChainAlone);
				Assert::AreEqual(uint64_t(0), noCnt.jointLatch.entriesJoint);
				Assert::IsTrue(noCnt.r1HarmonicSelections >= 1);
			}

			// (b) SelectedFundamentalConfirmed through real ChainEstimator:
			//     2560x1440 (2/3 alias) accumulates >= c1 over 2 shots, while true 1/3 fundamental 1280x720 has
			//     only Shot 2 credit (jNeed <= open, LlrAtN < c1).
			//       - When joint-eligible (minShotDwellSec = 3.0): enters 1280x720 via EnteredJoint, NEVER EnteredChainAlone!
			//       - When joint-ineligible (minShotDwellSec = 15.0): MUST NOT enter at all!
			{
				auto runSelectedFundamental = [](double jointDwellSec, int& outLatchNx, int& outLatchNy, C::EstimatorCounters& outCnt) {
					ChainFx::ManualClock clk;
					C::EstimatorSettings cfg;
					cfg.safetyRules = true;
					cfg.jointLatch.enabled = true;
					cfg.jointLatch.minShotDwellSec = jointDwellSec;
					cfg.conventionDrivenProposals = true;
					C::ChainEstimator est(cfg, clk.Fn());
					est.SetSynchronous(true);
					est.SetMode(C::EstimatorMode::On);

					uint64_t seq = 10;
					int s = 0;
					// Shot 1 (s = 0..4): 2560x1440 Lanczos3 with cf.detailAbove = true -> 2560x1440 earns Shot 1 cap,
					// while 1280x720 gets negative LLR (2560x1440 has broadband energy above 1280x720 Nyquist!).
					est.SetClassicalFresh(true, 2.0 / 3.0, 2.0 / 3.0, true);
					for (int k = 0; k < 5; ++k, ++s, ++seq)
					{
						clk.Set(static_cast<double>(s) * 1500.0);
						const C::LineSnapshot up1440 = MakeSynthSnapshot(kUhdW, kUhdH, 2560, 1440, kLanczos3, 1,
							seq, static_cast<int64_t>(s) * 1000000, 8801u + seq, k == 0);
						Assert::IsTrue(est.SubmitLines(up1440));
					}
					Assert::AreEqual(0, est.StatusInput().latchNx, L"fixture: detailAbove blocked latch during Shot 1");

					// Shot 2 (s = 5..10, 6 evals): 1280x720 Lanczos3 dips BOTH 1280x720 (fundamental) and 2560x1440 (2x alias)!
					// Keep detailAbove = true until s = 10 (k == 5) so 2560x1440 accumulates 2 shots (>= c1) while 1280x720
					// stays < c1 (Shot 1 negative + Shot 2 capped at 0.5*c1), with open-shot credit >= jNeed!
					est.SetClassicalFresh(true, 1.0 / 3.0, 1.0 / 3.0, true);
					for (int k = 0; k < 6; ++k, ++s, ++seq)
					{
						if (k == 5)
							est.SetClassicalFresh(true, 1.0 / 3.0, 1.0 / 3.0, false);
						clk.Set(static_cast<double>(s) * 1500.0);
						const C::LineSnapshot up720 = MakeSynthSnapshot(kUhdW, kUhdH, 1280, 720, kLanczos3, 1,
							seq, static_cast<int64_t>(s) * 1000000, 8901u + seq, k == 0);
						Assert::IsTrue(est.SubmitLines(up720));
					}
					outLatchNx = est.StatusInput().latchNx;
					outLatchNy = est.StatusInput().latchNy;
					outCnt = est.GetCounters();
				};

				// (b1) Joint-eligible: enters 1280x720 via EnteredJoint, NEVER EnteredChainAlone!
				int fNx = -1, fNy = -1;
				C::EstimatorCounters fCnt;
				runSelectedFundamental(C::kMinShotDwellSec, fNx, fNy, fCnt);
				Assert::AreEqual(1280, fNx, L"R1 SelectedFundamentalConfirmed selects clean 1280x720 fundamental");
				Assert::AreEqual(720, fNy);
				Assert::AreEqual(uint64_t(1), fCnt.jointLatch.entriesJoint, L"enters via EnteredJoint when classical jointly supports 1280x720");
				Assert::AreEqual(uint64_t(0), fCnt.jointLatch.entriesChainAlone, L"under-c1 1280x720 MUST NOT inherit 2560x1440 >= c1 LLR for EnteredChainAlone");
				Assert::IsTrue(fCnt.r1HarmonicSelections >= 1);

				// (b2) Joint-ineligible (Shot 2 dwell = 5.0 s < minShotDwellSec = 15.0 s): MUST NOT enter at all!
				int noNx = -1, noNy = -1;
				C::EstimatorCounters noCnt;
				runSelectedFundamental(15.0, noNx, noNy, noCnt);
				Assert::AreEqual(0, noNx, L"when classical is ineligible for joint, under-c1 1280x720 MUST NOT enter via EnteredChainAlone");
				Assert::AreEqual(uint64_t(0), noCnt.jointLatch.entriesChainAlone);
				Assert::AreEqual(uint64_t(0), noCnt.jointLatch.entriesJoint);
				Assert::IsTrue(noCnt.r1HarmonicSelections >= 1);
			}

			// (c) Mandatory in-test negative control (Rule 3) on the EXACT SAME LLR pairs for BOTH override paths:
			//     Without refreshing chainLlrX/Y to LlrAtN(hRes.nx/ny), passing the stale rejected/alias >= c1 LLR
			//     into ChainJointLatch::Step falsely enters via EnteredChainAlone!
			{
				const double c1 = C::ComputeThresholds(C::EvidenceSettings()).c1;
				const double staleAliasLlr = c1 + 2.0;    // >= c1 from rejected sub-harmonic or 2x alias
				const double trueCandLlr = 0.5 * c1;      // < c1 (only 1 shot for overridden candidate)

				auto checkPair = [&](int nx, int ny) {
					C::JointLatchSettings js;
					js.enabled = true;
					js.minShotDwellSec = 10.0; // joint ineligible (dwell = 5.0 s < 10.0 s)

					C::JointLatchInput naiveIn;
					naiveIn.timeUs = 10 * 1000000;
					naiveIn.c1 = c1;
					naiveIn.rasterW = kUhdW;
					naiveIn.rasterH = kUhdH;
					naiveIn.haveChainPair = true;
					naiveIn.chainNx = nx; naiveIn.chainNxDenom = kUhdW;
					naiveIn.chainNy = ny; naiveIn.chainNyDenom = kUhdH;
					naiveIn.chainLlrX = staleAliasLlr; // BUGGY/NAIVE: inherited stale harmonic >= c1 LLR!
					naiveIn.chainLlrY = staleAliasLlr;
					naiveIn.chainShots = 2;
					naiveIn.chainInformative = true;
					naiveIn.classicalValid = true;
					naiveIn.classicalNx = nx; naiveIn.classicalNxDenom = kUhdW;
					naiveIn.classicalNy = ny; naiveIn.classicalNyDenom = kUhdH;
					naiveIn.currentShotDwellSec = 5.0;
					naiveIn.currentShotCreditX = 0.6 * c1;
					naiveIn.currentShotCreditY = 0.6 * c1;

					C::ChainJointLatch naiveLatch(js);
					const auto oNaive = naiveLatch.Step(naiveIn);
					Assert::IsTrue(oNaive.latched, L"negative control: naive un-refreshed LLR falsely latches");
					Assert::IsTrue(oNaive.event == C::JointLatchEvent::EnteredChainAlone,
						L"negative control: naive un-refreshed LLR falsely enters via EnteredChainAlone");

					// Protected (refreshed LLR < c1) on the exact same input: MUST NOT enter via EnteredChainAlone!
					C::JointLatchInput fixedIn = naiveIn;
					fixedIn.chainLlrX = trueCandLlr;
					fixedIn.chainLlrY = trueCandLlr;
					C::ChainJointLatch fixedLatch(js);
					const auto oFixed = fixedLatch.Step(fixedIn);
					Assert::IsFalse(oFixed.latched, L"refreshed under-c1 LLR does not falsely enter when joint-ineligible");
					Assert::AreEqual(uint64_t(0), fixedLatch.Counters().entriesChainAlone);
				};

				checkPair(1920, 1080); // RejectedSubHarmonicDetailAbove pair
				checkPair(1280, 720);  // SelectedFundamentalConfirmed pair
			}
		}

		// 11. HIGH-3b: 80%-flat 8-bit mixed-segment (PiP/UI overlay + upscaled edges) spatial-agreement HOLD test,
		//     with in-test negative controls (un-checked ComputeSegmentGatedStat vs uniform-upscale segments).
		TEST_METHOD(High3Sparse8BitMixedSegmentPipOverlayTriggersHoldWithNegativeControl)
		{
			using namespace S2Fx;
			const double floor8Tpdf = C::TpdfDitherFloorCodes(4.0); // 1.633 codes

			// Build an 80%-flat 8-bit candidate (lineBase = 2.0 < 1.5*1.633 = 2.45 => gated == 0 < minGate, entering
			// the sparse edgeStats path in ChainEvidence::Judge) with 16 lines x 15 segments, 3 informative edge
			// segments per line (48 informative segments total, 192 flat segments = 80% flat):
			//   - 2 edge segments per line (32 / 48 = 66.7%) are upscaled 1080p->4K edges (segBase = 7.0, segRms = 1.85);
			//   - 1 edge segment per line (16 / 48 = 33.3% > maskLimit = 20%) is a crisp native PiP/UI overlay
			//     (segBase = 7.0, segRms = 7.0 => ~0 dip, contradicting the upscaled majority!).
			C::CandidateResult mixedCand = MakeSyntheticCandidate(1920, 2.0, -0.08, 16, 15, 7.0, 1.85, 1.70, 1.65, 0.5, 3);
			for (int li = 0; li < mixedCand.lineCount; ++li)
			{
				const size_t pipIdx = static_cast<size_t>(li * mixedCand.segCount + 2); // 3rd edge segment = native PiP/UI
				const double pipRms = 7.0;
				mixedCand.segSum[pipIdx] = pipRms * pipRms * static_cast<double>(C::kSegPx) * (1.0 - 0.5);
				mixedCand.segBase[pipIdx] = 7.0;
				mixedCand.segStat[pipIdx] = 0.0;
			}

			C::EvidenceSettings onCfg;
			onCfg.edgeStats.enabled = true;

			// In-test negative control #1 (Rule 3): an un-checked segment-gated mean (calling ComputeSegmentGatedStat
			// directly without spatial-agreement mixed detection) on the EXACT SAME mixedCand produces a valid,
			// non-held, strong false upscale dip!
			const C::SegmentGatedResult naiveSeg = C::ComputeSegmentGatedStat(mixedCand, 0.5, floor8Tpdf, onCfg.edgeStats);
			Assert::IsTrue(naiveSeg.valid, L"negative control: un-checked ComputeSegmentGatedStat accepts mixed PiP segments");
			Assert::IsTrue(naiveSeg.stat < -0.5, L"negative control: un-checked segment-gated mean emits strong false dip on PiP overlay");

			// Real ChainEvidence::Submit on the EXACT SAME mixedCand MUST detect contradiction > maskLimit and HOLD!
			C::VerifyResult vrMixed;
			vrMixed.candidates.push_back(mixedCand);
			C::AxisObservation obsMixed;
			obsMixed.verify = &vrMixed;
			obsMixed.axisLen = kUhdW;
			obsMixed.floorCodes = floor8Tpdf;
			obsMixed.probeRan = true;
			obsMixed.probeSeen = true;
			obsMixed.shotId = 1;
			obsMixed.timeUs = -1;
			obsMixed.rasterW = kUhdW;
			obsMixed.rasterH = kUhdH;

			const C::NullTables calEdgeNulls = MakeCalibratedEdgeStatsNulls();
			C::ChainEvidence evMixed(onCfg, calEdgeNulls);
			C::EvidenceUpdate upMixed;
			Assert::IsTrue(evMixed.Submit(0, obsMixed, upMixed));
			Assert::AreEqual(1, upMixed.candidateCount);
			Assert::IsTrue(upMixed.cand[0].mixed, L"sparse 8-bit PiP/UI contradiction flags ce.mixed == true");
			Assert::IsTrue((upMixed.cand[0].reason & C::kReasonMixed) != 0u);
			Assert::IsTrue(upMixed.hold, L"top mixed sparse 8-bit candidate triggers out.hold == true");
			Assert::IsTrue((upMixed.reason & C::kReasonMixed) != 0u);
			Assert::IsFalse(upMixed.informative, L"HOLD evaluation accumulates zero evidence");
			Assert::IsFalse(evMixed.Summary(0).hasBest);

			// In-test contrast control #2: uniform-upscale segments (identical 80%-flat layout, NO PiP contradiction)
			// MUST NOT trigger mixed/HOLD and MUST recover valid evidence!
			C::VerifyResult vrUniform;
			vrUniform.candidates.push_back(MakeSyntheticCandidate(1920, 2.0, -0.08, 16, 15, 7.0, 1.85, 1.70, 1.65, 0.5, 3));
			C::AxisObservation obsUniform = obsMixed;
			obsUniform.verify = &vrUniform;
			C::ChainEvidence evUniform(onCfg, calEdgeNulls);
			C::EvidenceUpdate upUniform;
			Assert::IsTrue(evUniform.Submit(0, obsUniform, upUniform));
			Assert::IsFalse(upUniform.cand[0].mixed, L"uniform upscaled segments are not mixed");
			Assert::IsFalse(upUniform.hold, L"uniform upscaled segments do not HOLD");
			Assert::IsTrue(upUniform.cand[0].edgeStatsUsed);
			Assert::IsTrue(upUniform.cand[0].informative);
		}

		// 12. MEDIUM-1: Short < 5 px strips where EstimateEmpiricalFloor returns valid == false on all axes MUST
		//     visibly increment empiricalFloorFallbacks (Rule 4), contrasted with wide informative lines (== 0 fallbacks).
		TEST_METHOD(Medium1EmpiricalFloorShortLineAllAxisFailureIncrementsFallbackCounter)
		{
			using namespace S2Fx;

			// (a) Short 4-pixel snapshot (inner.w == 4 < 5, inner.h == 4 < 5): ToLineSet succeeds on both axes,
			//     EstimateEmpiricalFloor returns valid == false on BOTH axes -> empiricalFloorFallbacks MUST increment!
			{
				ChainFx::ManualClock clk;
				C::EstimatorSettings cfg;
				cfg.useEmpiricalFloor = true;
				C::ChainEstimator est(cfg, clk.Fn());
				est.SetSynchronous(true);
				est.SetMode(C::EstimatorMode::On);

				double observedFloor = -1.0;
				est.SetEvaluationObserver([&](const C::EvaluationDetail& d) { observedFloor = d.axis[0].floorCodes; });

				C::LineSnapshot shortSnap;
				shortSnap.seq = 10;
				shortSnap.timeUs = 1000000;
				shortSnap.rasterW = 4; shortSnap.rasterH = 4;
				shortSnap.active = { 0, 0, 4, 4 };
				shortSnap.inner = { 0, 0, 4, 4 };
				shortSnap.rowCount = 1; shortSnap.colCount = 1;
				shortSnap.rowY = { 2 }; shortSnap.colX = { 2 };
				shortSnap.rows = { 101, 203, 305, 407 }; // step == 1, length == 4 < 5
				shortSnap.cols = { 101, 203, 305, 407 };

				// Confirm direct EstimateEmpiricalFloor fails on this LineSet (proving the all-axis failure path is hit).
				C::LineSet lsCheck;
				Assert::IsTrue(C::ToLineSet(shortSnap, 0, lsCheck));
				C::EdgeStatsSettings esOn;
				esOn.enabled = true;
				Assert::IsFalse(C::EstimateEmpiricalFloor(lsCheck, 1.0, esOn).valid,
					L"fixture: 4-px line makes EstimateEmpiricalFloor return valid == false");

				Assert::IsTrue(est.SubmitLines(shortSnap));
				const C::EstimatorCounters cnt = est.GetCounters();
				Assert::AreEqual(uint64_t(0), cnt.empiricalFloorEvals, L"no valid empirical floor eval on < 5 px strips");
				Assert::IsTrue(cnt.empiricalFloorFallbacks >= 1, L"Rule 4: all-axis EstimateEmpiricalFloor failure visibly increments empiricalFloorFallbacks");
				Assert::IsTrue(std::isfinite(observedFloor) && observedFloor > 0.0, L"fell back cleanly to positive finite floorCodes");
			}

			// (b) In-test negative control (Rule 3): normal wide informative lines where EstimateEmpiricalFloor succeeds
			//     increment empiricalFloorEvals and leave empiricalFloorFallbacks == 0!
			{
				ChainFx::ManualClock clk;
				C::EstimatorSettings cfg;
				cfg.useEmpiricalFloor = true;
				C::ChainEstimator est(cfg, clk.Fn());
				est.SetSynchronous(true);
				est.SetMode(C::EstimatorMode::On);

				const C::LineSnapshot wideSnap = MakeSynthSnapshot(kUhdW, kUhdH, 1920, 1080, kBilinear, 1,
					10, 1000000, 9001u, true, 1.30, false, 0.75);
				Assert::IsTrue(est.SubmitLines(wideSnap));
				const C::EstimatorCounters cnt = est.GetCounters();
				Assert::AreEqual(uint64_t(1), cnt.empiricalFloorEvals, L"negative control: wide informative snapshot succeeds empirical floor");
				Assert::AreEqual(uint64_t(0), cnt.empiricalFloorFallbacks, L"negative control: zero fallbacks when EstimateEmpiricalFloor succeeds");
			}
		}

		// 13. MEDIUM-2:
		//     (a) Legacy ApplyLatch (safetyRules = true, jointLatch = false, stepMark = true) consumes
		//         stepMarkImmediateReleasePending on confirmed ABR step-up and clears it cleanly (with stepMark OFF negative control);
		//     (b) Letterboxed/cropped active dimensions (active.h < rasterH) pass active-dimension Nx/Ny to ChainStepMark
		//         rather than full-raster bars (with mismatched rasterH denominator negative control).
		TEST_METHOD(Medium2LegacyApplyLatchStepMarkImmediateReleaseAndLetterboxActiveDims)
		{
			using namespace S2Fx;

			// (a) MEDIUM-2a: Legacy ApplyLatch (jointLatch.enabled == false, safetyRules == true, stepMark == true)
			//     immediately releases on confirmed ABR step-up and clears stepMarkImmediateReleasePending!
			{
				auto runLegacyStepUp = [](bool enableStepMark, int& latchAfterStep, int& latchAfterReacquire, C::EstimatorCounters& outCnt) {
					ChainFx::ManualClock clk;
					C::EstimatorSettings cfg;
					cfg.safetyRules = true;
					cfg.jointLatch.enabled = false; // Explicitly exercise legacy ApplyLatch path!
					cfg.stepMark.enabled = enableStepMark;
					cfg.conventionDrivenProposals = true;
					C::ChainEstimator est(cfg, clk.Fn());
					est.SetSynchronous(true);
					est.SetMode(C::EstimatorMode::On);
					est.SetClassicalFresh(false, 1.0, 1.0, false);

					uint64_t seq = 10;
					int s = 0;
					// Latch 960x540 in 2 shots via legacy UpscaleLatch.
					for (int shot = 0; shot < 2; ++shot)
					{
						for (int k = 0; k < 5; ++k, ++s, ++seq)
						{
							clk.Set(static_cast<double>(s) * 1500.0);
							const C::LineSnapshot up540 = MakeSynthSnapshot(kUhdW, kUhdH, 960, 540, kBilinear, 1,
								seq, static_cast<int64_t>(s) * 1000000, 9101u + seq, k == 0);
							Assert::IsTrue(est.SubmitLines(up540));
						}
					}
					Assert::AreEqual(960, est.StatusInput().latchNx, L"fixture: legacy ApplyLatch latched 960x540");
					Assert::AreEqual(uint64_t(1), est.GetCounters().latch.entries);
					Assert::AreEqual(uint64_t(0), est.GetCounters().jointLatch.entriesChainAlone);
					Assert::AreEqual(uint64_t(0), est.GetCounters().jointLatch.entriesJoint);

					// Trigger ABR step-up to native 4K and confirm via chain contradiction:
					const int64_t tStepUs = static_cast<int64_t>(s) * 1000000;
					est.NoteAbrStepEvent(tStepUs, +0.75, seq);
					for (int k = 0; k < 2; ++k, ++s, ++seq)
					{
						clk.Set(static_cast<double>(s) * 1500.0);
						const C::LineSnapshot nat = MakeSynthSnapshot(kUhdW, kUhdH, kUhdW, kUhdH, kBilinear, 1,
							seq, static_cast<int64_t>(s) * 1000000, 9201u + seq, false);
						Assert::IsTrue(est.SubmitLines(nat));
					}
					latchAfterStep = est.StatusInput().latchNx;

					// Verify stepMarkImmediateReleasePending was cleared (not stuck true): re-acquire 1920x1080 in 2 shots
					// and feed 1 additional eval; legacy ApplyLatch must stay latched at 1920x1080!
					latchAfterReacquire = 0;
					if (enableStepMark)
					{
						for (int shot = 0; shot < 2; ++shot)
						{
							for (int k = 0; k < 5; ++k, ++s, ++seq)
							{
								clk.Set(static_cast<double>(s) * 1500.0);
								const C::LineSnapshot up1080 = MakeSynthSnapshot(kUhdW, kUhdH, 1920, 1080, kBilinear, 1,
									seq, static_cast<int64_t>(s) * 1000000, 9301u + seq, k == 0);
								Assert::IsTrue(est.SubmitLines(up1080));
							}
						}
						// One more eval to confirm no stale release flag drops the new latch:
						clk.Set(static_cast<double>(s) * 1500.0);
						const C::LineSnapshot hold1080 = MakeSynthSnapshot(kUhdW, kUhdH, 1920, 1080, kBilinear, 1,
							seq, static_cast<int64_t>(s) * 1000000, 9399u, false);
						Assert::IsTrue(est.SubmitLines(hold1080));
						latchAfterReacquire = est.StatusInput().latchNx;
					}
					outCnt = est.GetCounters();
				};

				// In-test negative control (Rule 3): with stepMark OFF on the exact same sequence, legacy ApplyLatch
				// stays stuck latched at 960x540!
				int stepOff = -1, reAcqOff = -1;
				C::EstimatorCounters cntOff;
				runLegacyStepUp(false, stepOff, reAcqOff, cntOff);
				Assert::AreEqual(960, stepOff, L"negative control: without stepMark, legacy ApplyLatch stays stuck at 960x540");
				Assert::AreEqual(uint64_t(0), cntOff.latch.releasesAbr);
				Assert::AreEqual(uint64_t(0), cntOff.stepMark.immediateReleases);

				// With stepMark ON: legacy ApplyLatch immediately releases, clears pending flag, and cleanly re-acquires 1920x1080!
				int stepOn = -1, reAcqOn = -1;
				C::EstimatorCounters cntOn;
				runLegacyStepUp(true, stepOn, reAcqOn, cntOn);
				Assert::AreEqual(0, stepOn, L"legacy ApplyLatch immediately releases when stepMark confirms step-up");
				Assert::AreEqual(uint64_t(1), cntOn.latch.releasesAbr);
				Assert::AreEqual(uint64_t(1), cntOn.stepMark.immediateReleases);
				Assert::AreEqual(1920, reAcqOn, L"stepMarkImmediateReleasePending was cleared so subsequent 1920x1080 latch holds");
			}

			// (b) MEDIUM-2b: Letterboxed 2.39:1 active picture (active = 3840x1608 inside 3840x2160 raster) on BOTH
			//     jointLatchOn = true AND jointLatchOn = false (legacy ApplyLatch), proving ChainStepMark receives
			//     active-dimension Ny = 1608 (not full rasterH = 2160).
			{
				const C::LineRect letterboxActive = { 0, 276, kUhdW, 1608 }; // 2.39:1 active area inside 4K raster
				for (int modeIdx = 0; modeIdx < 2; ++modeIdx)
				{
					const bool useJoint = (modeIdx == 0);
					ChainFx::ManualClock clk;
					C::EstimatorSettings cfg;
					cfg.safetyRules = true;
					cfg.jointLatch.enabled = useJoint;
					cfg.stepMark.enabled = true;
					cfg.conventionDrivenProposals = true;
					C::ChainEstimator est(cfg, clk.Fn());
					est.SetSynchronous(true);
					est.SetMode(C::EstimatorMode::On);
					est.SetClassicalFresh(true, 0.25, 0.25, false);

					uint64_t seq = 10;
					int s = 0;
					// Latch 0.25 x 0.25 of active letterbox (960 x 402 inside active 3840 x 1608).
					const int shotsNeeded = useJoint ? 1 : 2;
					for (int shot = 0; shot < shotsNeeded; ++shot)
					{
						for (int k = 0; k < 5; ++k, ++s, ++seq)
						{
							clk.Set(static_cast<double>(s) * 1500.0);
							const C::LineSnapshot lb540 = MakeLetterboxedSynthSnapshot(
								kUhdW, kUhdH, letterboxActive, 960, 402, kBilinear, 1,
								seq, static_cast<int64_t>(s) * 1000000, 9401u + seq, k == 0);
							Assert::IsTrue(est.SubmitLines(lb540));
						}
					}
					Assert::AreEqual(960, est.StatusInput().latchNx, L"fixture: latched letterbox width 960");
					Assert::AreEqual(402, est.StatusInput().latchNy, L"fixture: latched letterbox active height 402 (not 540)");

					// Spurious ABR mark while stream & classical remain at 0.25 x 0.25: active Ny = 1608 matches 402/1608 == 0.25,
					// so classical MUST NOT falsely confirm!
					est.NoteAbrStepEvent(static_cast<int64_t>(s) * 1000000, +0.25, seq);
					for (int k = 0; k < 2; ++k, ++s, ++seq)
					{
						clk.Set(static_cast<double>(s) * 1500.0);
						const C::LineSnapshot lb540 = MakeLetterboxedSynthSnapshot(
							kUhdW, kUhdH, letterboxActive, 960, 402, kBilinear, 1,
							seq, static_cast<int64_t>(s) * 1000000, 9501u + seq, false);
						Assert::IsTrue(est.SubmitLines(lb540));
					}
					Assert::AreEqual(960, est.StatusInput().latchNx, L"letterbox active Ny=1608 prevents false confirmation on static 0.25x0.25");
					Assert::AreEqual(uint64_t(0), est.GetCounters().stepMark.confirmedByClassical);
					Assert::AreEqual(uint64_t(1), est.GetCounters().stepMark.unconfirmedTimeouts);

					// Real ABR step-up to 0.5 x 0.5 (1920 x 804 inside active 3840 x 1608): submit flat/uninformative
					// letterboxed frames preserving letterboxActive and 2 distinct fresh classical frames so Chain stays
					// neutral on Eval #1 and Classical A's 2-frame streak confirms cleanly via active-dimension Ny=1608!
					est.NoteAbrStepEvent(static_cast<int64_t>(s) * 1000000, +0.25, seq);
					for (int k = 0; k < 2; ++k, ++s, ++seq)
					{
						const int64_t evalTimeUs = static_cast<int64_t>(s) * 1000000;
						est.SetClassicalFresh(MakeClassicalFresh(true, 0.5, 0.5, false, seq, evalTimeUs, 1.0));
						clk.Set(static_cast<double>(s) * 1500.0);
						const C::LineSnapshot lbFlat = MakeLetterboxedFlatSnapshot(
							kUhdW, kUhdH, letterboxActive, 512, seq, evalTimeUs, false);
						Assert::IsTrue(est.SubmitLines(lbFlat));
					}
					Assert::AreEqual(0, est.StatusInput().latchNx, L"letterbox step-up confirmed cleanly with active-dimension Ny=1608");
					Assert::AreEqual(uint64_t(1), est.GetCounters().stepMark.confirmedByClassical);
					Assert::AreEqual(uint64_t(0), est.GetCounters().stepMark.confirmedByChain);
					Assert::AreEqual(uint64_t(1), est.GetCounters().stepMark.immediateReleases);
				}

				// In-test negative control (Rule 3): feeding the exact same letterboxed 0.5x0.5 active height (ny = 804)
				// into ChainStepMark::OnClassicalFrame with mismatched full-raster denominator Ny = rasterH (2160) FAILS
				// to confirm (804/2160 = 0.372 is rejected as non-conventional!), proving active Ny = 1608 is required!
				C::StepMarkSettings smCfg;
				smCfg.enabled = true;
				C::ChainStepMark smBadDenom(smCfg);
				C::StepMarkLatchContext lctx;
				lctx.latched = true;
				lctx.nx = 960; lctx.Nx = kUhdW;
				lctx.ny = 402; lctx.Ny = 1608;
				smBadDenom.OnAbrStep(1000000, +0.25);

				for (int k = 0; k < 2; ++k)
				{
					C::StepMarkClassicalFrame badCfr;
					badCfr.timeUs = static_cast<int64_t>(2 + k) * 1000000;
					badCfr.informative = true;
					badCfr.hasPair = true;
					badCfr.nx = 1920; badCfr.Nx = kUhdW;
					badCfr.ny = 804;  badCfr.Ny = kUhdH; // BUGGY/NAIVE: full rasterH instead of active.h (1608)!
					badCfr.rasterW = kUhdW; badCfr.rasterH = kUhdH;
					smBadDenom.OnClassicalFrame(badCfr, lctx);
				}
				Assert::AreEqual(uint64_t(0), smBadDenom.Counters().confirmedByClassical,
					L"negative control: mismatched full-raster Ny=2160 fails to confirm letterboxed step-up");
				Assert::AreEqual(uint64_t(0), smBadDenom.Counters().immediateReleases);
			}
		}

		// 14. LOW-2: SourceResolutionMonitor::Monitor::SetSourceIdBlockClassicalAlone(true) blocks classical-alone
		//     consensus entry & AutoFactors engagement when chain is silent, while negative control
		//     SetSourceIdBlockClassicalAlone(false) on the EXACT SAME tiles enters cleanly (and restore resets cleanly).
		TEST_METHOD(Low2MonitorSetSourceIdBlockClassicalAloneBlocksUntilDisabled)
		{
			using namespace S2Fx;
			constexpr int kTile = 128;
			constexpr int kTiles = 8;
			constexpr int kW = 1920;
			constexpr int kH = 1080;

			// Deterministic 2x bilinear upscaled tile generator (independent synthetic noise at half resolution,
			// upscaled 2x across multi-shot brightness shifts so classical detector declares 0.5 with high confidence).
			auto makeUpscaledTiles = [kTiles, kTile](int frame) {
				std::vector<float> all(static_cast<size_t>(kTiles) * kTile * kTile);
				const int half = kTile / 2;
				const int shot = frame / 12;
				SplitMix64 rng(0xB10C4A10u ^ static_cast<uint64_t>(frame * 131 + shot * 977));
				for (int t = 0; t < kTiles; ++t)
				{
					std::vector<double> low(static_cast<size_t>(half) * half);
					for (double& v : low)
						v = 0.25 + 0.50 * rng.Uniform01();
					auto at = [&](int x, int y) {
						x = (std::min)((std::max)(x, 0), half - 1);
						y = (std::min)((std::max)(y, 0), half - 1);
						return low[static_cast<size_t>(y) * half + x];
					};
					const double level = (shot % 2 ? 0.12 : -0.08) + 0.0007 * (frame % 12);
					for (int y = 0; y < kTile; ++y)
					{
						for (int x = 0; x < kTile; ++x)
						{
							const int sx = x / 2, sy = y / 2;
							const int dx = (x % 2) ? 1 : -1;
							const int dy = (y % 2) ? 1 : -1;
							const double v = 0.5625 * at(sx, sy) + 0.1875 * at(sx + dx, sy)
								+ 0.1875 * at(sx, sy + dy) + 0.0625 * at(sx + dx, sy + dy);
							all[static_cast<size_t>(t) * kTile * kTile + static_cast<size_t>(y) * kTile + x] =
								static_cast<float>((std::min)(1.0, (std::max)(0.0, v + level)));
						}
					}
				}
				return all;
			};

			SourceResolutionMonitor::Monitor monOff; // In-test negative control: classical-alone allowed
			SourceResolutionMonitor::Monitor monOn;  // Under test: SetSourceIdBlockClassicalAlone(true)
			monOff.SetSourceIdConsensus(true);
			monOn.SetSourceIdConsensus(true);
			monOff.SetSourceIdBlockClassicalAlone(false);
			monOn.SetSourceIdBlockClassicalAlone(true);

			const std::vector<float> none;
			int engagedOff = 0;
			int engagedOn = 0;
			for (int f = 0; f < 120; ++f)
			{
				const double seconds = 0.5 * static_cast<double>(f);
				const uint64_t seq = static_cast<uint64_t>(f + 1);
				const std::vector<float> tiles = makeUpscaledTiles(f);

				monOff.AnalyseTilesForTest(tiles, kTiles, seconds, kW, kH, none, none, kTiles, kTile, 64.0 / 1023.0, seq);
				monOn.AnalyseTilesForTest(tiles, kTiles, seconds, kW, kH, none, none, kTiles, kTile, 64.0 / 1023.0, seq);

				std::string whyOff, whyOn;
				double fxOff = 1.0, fyOff = 1.0, fxOn = 1.0, fyOn = 1.0;
				monOff.AutoFactors(kW, kH, 0.8, whyOff, fxOff, fyOff, seconds + 0.1, seq);
				monOn.AutoFactors(kW, kH, 0.8, whyOn, fxOn, fyOn, seconds + 0.1, seq);
				if (fxOff < 0.75 && fyOff < 0.75) ++engagedOff;
				if (fxOn < 1.0 || fyOn < 1.0) ++engagedOn;
			}

			// Verify classical detector saw the exact same upscaled ~0.5 signal on BOTH monitors:
			const SourceResolutionMonitor::Snapshot snapOff = monOff.Read();
			const SourceResolutionMonitor::Snapshot snapOn = monOn.Read();
			Assert::IsTrue(snapOff.reading && snapOn.reading);
			Assert::AreEqual(snapOff.declaredRatio, snapOn.declaredRatio, 1e-6,
				L"both monitors see the exact same classical declared ratio");
			Assert::IsTrue(snapOn.declaredRatio < 0.6, L"fixture: classical detector declared ~0.5 upscale");

			// Negative control (SetSourceIdBlockClassicalAlone(false)): consensus latched ~0.5 and AutoFactors engaged!
			SourceIdentity::Record recOff, recOn;
			Assert::IsTrue(monOff.SourceIdRecord(recOff));
			Assert::IsTrue(monOn.SourceIdRecord(recOn));
			Assert::IsTrue(recOff.ratioH < 0.6f && recOff.ratioV < 0.6f,
				L"negative control: SetSourceIdBlockClassicalAlone(false) allows classical-alone consensus entry");
			Assert::IsTrue(engagedOff > 0, L"negative control: AutoFactors engages when classical-alone is not blocked");

			// Under test (SetSourceIdBlockClassicalAlone(true)): consensus stayed native 1.0x1.0 and AutoFactors NEVER engaged!
			Assert::AreEqual(1.0f, recOn.ratioH, 1e-6f, L"SetSourceIdBlockClassicalAlone(true) blocks classical-alone H entry");
			Assert::AreEqual(1.0f, recOn.ratioV, 1e-6f, L"SetSourceIdBlockClassicalAlone(true) blocks classical-alone V entry");
			Assert::AreEqual(0, engagedOn, L"SetSourceIdBlockClassicalAlone(true) prevents classical-alone AutoFactors engagement");

			// Clean state restore check (Rule 2): disabling the block + discontinuity cleanly restores classical-alone entry!
			monOn.SetSourceIdBlockClassicalAlone(false);
			monOn.Discontinuity("restore blockClassicalAlone");
			int engagedRestored = 0;
			for (int f = 0; f < 120; ++f)
			{
				const double seconds = 100.0 + 0.5 * static_cast<double>(f);
				const uint64_t seq = static_cast<uint64_t>(200 + f);
				const std::vector<float> tiles = makeUpscaledTiles(f);
				monOn.AnalyseTilesForTest(tiles, kTiles, seconds, kW, kH, none, none, kTiles, kTile, 64.0 / 1023.0, seq);
				std::string why;
				double fx = 1.0, fy = 1.0;
				monOn.AutoFactors(kW, kH, 0.8, why, fx, fy, seconds + 0.1, seq);
				if (fx < 0.75 && fy < 0.75) ++engagedRestored;
			}
			SourceIdentity::Record recRestored;
			Assert::IsTrue(monOn.SourceIdRecord(recRestored));
			Assert::IsTrue(recRestored.ratioH < 0.6f && recRestored.ratioV < 0.6f,
				L"restoring SetSourceIdBlockClassicalAlone(false) cleanly re-enables classical-alone entry");
			Assert::IsTrue(engagedRestored > 0);
		}

		// 15. B1 (BLOCKER): Blind-classical upscaled 1080p->4K signal (valid=true, ratio=1.0, trackerNative=true,
		//     perFrameDetailAbove=false) MUST allow Chain-Alone entry (1920x1080) and hold continuously > 60 s with
		//     0 releases / 0 flapping, carrying kReasonChainAloneEntry — with same-input negative controls proving
		//     sticky detailAbove=true blocks entry AND releases within ~3 s. Also proves per-candidate R1 Nyquist
		//     filtering via ClassicalFreshInput::detailAboveAtRatio rejects 960x540 while latching 1920x1080!
		TEST_METHOD(B1BlindClassicalNativeChainAloneHoldsOver60sAndPerCandidateNyquist)
		{
			using namespace S2Fx;
			namespace SR = SourceResolution;

			// Part A: Real SourceResolution::Tracker + FrameReading wired to ChainEstimator on blind-classical 1080p->4K.
			{
				// Construct a realistic blind-classical FrameReading on 1080p->4K: mirror coherence was lost (ratio == 1.0),
				// while the power spectrum has strong below-0.5 detail and quiet above-0.5 roll-off (no positive detail_above).
				auto makeBlindClassicalFrame = [](double belowPow, double abovePow) {
					SR::FrameReading fr;
					fr.informativeTiles = 8;
					fr.ratio = 1.0;
					fr.ratioH = 1.0;
					fr.ratioV = 1.0;
					fr.contrast = 0.25;
					fr.quantisationFloor = -7.1;
					fr.verticalPower.assign(129, belowPow);
					fr.horizontalPower.assign(129, belowPow);
					for (size_t k = 64; k < 129; ++k)
					{
						fr.verticalPower[k] = abovePow;
						fr.horizontalPower[k] = abovePow;
					}
					return fr;
				};

				// Run Tracker to confirm the monitor's sticky native state (tracker.Ratio() >= 1.0 && tracker.Candidate() >= 1.0)
				// is genuinely TRUE on this blind-classical upscaled stream while actual per-frame DetailAbove is strongly negative.
				SR::Tracker tracker;
				const SR::FrameReading blindFr = makeBlindClassicalFrame(1e-3, 1e-5);
				for (int i = 0; i < 6; ++i)
					tracker.Update(blindFr, 0.5 * static_cast<double>(i));
				const bool trackerNative = blindFr.informative() && tracker.Ratio() >= 1.0 && tracker.Candidate() >= 1.0;
				Assert::IsTrue(trackerNative, L"fixture: blind-classical tracker reads sticky native (Ratio >= 1 && Candidate >= 1)");
				const double daAtHalf = SR::DetailAbove(blindFr, 0.5);
				Assert::IsTrue(std::isfinite(daAtHalf) && daAtHalf < -1.5,
					L"fixture: actual per-frame DetailAbove(0.5) is strongly negative (no above-band 4K detail)");
				const bool perFrameDetailAbove = false;

				// Pre-generate 56 deterministic 1080p->4K snapshots over t = 0..82.5 s (1.5 s cadence, cuts every 12 s)
				// so protected run and both negative controls execute on the EXACT SAME frames!
				constexpr int kTotalEvals = 56;
				std::vector<C::LineSnapshot> stream;
				stream.reserve(kTotalEvals);
				for (int i = 0; i < kTotalEvals; ++i)
				{
					const int64_t tUs = static_cast<int64_t>(i) * 1500000;
					const bool cut = (i % 8) == 0;
					stream.push_back(MakeSynthSnapshot(kUhdW, kUhdH, 1920, 1080, kBilinear, 1,
						static_cast<uint64_t>(10 + i), tUs, 96001u + static_cast<uint64_t>(i), cut));
				}

				// (a1) Honest per-frame wiring (detailAbove = perFrameDetailAbove = false, ratio = 1.0, valid = true):
				//      MUST enter 1920x1080 via Chain-alone and hold continuously for > 60 s with 0 releases / 0 flapping!
				{
					ChainFx::ManualClock clk;
					C::EstimatorSettings cfg;
					cfg.safetyRules = true;
					cfg.jointLatch.enabled = true;
					cfg.conventionDrivenProposals = true;
					C::ChainEstimator est(cfg, clk.Fn());
					est.SetSynchronous(true);
					est.SetMode(C::EstimatorMode::On);

					int64_t firstLatchUs = -1;
					int64_t lastLatchUs = -1;
					int dropsAfterLatch = 0;
					G::Record latchedRec;

					for (int i = 0; i < kTotalEvals; ++i)
					{
						const int64_t tUs = stream[static_cast<size_t>(i)].timeUs;
						clk.Set(static_cast<double>(tUs) / 1000.0);
						est.SetClassicalFresh(MakeClassicalFresh(
							true, 1.0, 1.0, perFrameDetailAbove, stream[static_cast<size_t>(i)].seq, tUs, 1.5));
						Assert::IsTrue(est.SubmitLines(stream[static_cast<size_t>(i)]));

						const int nx = est.StatusInput().latchNx;
						const int ny = est.StatusInput().latchNy;
						if (nx == 1920 && ny == 1080)
						{
							if (firstLatchUs < 0)
							{
								firstLatchUs = tUs;
								Assert::IsTrue(est.Held().ReadLatest(0, latchedRec));
							}
							lastLatchUs = tUs;
						}
						else if (firstLatchUs >= 0)
						{
							++dropsAfterLatch;
						}
					}

					Assert::IsTrue(firstLatchUs >= 0, L"B1: Chain-alone must enter 1920x1080 when classical is blind native");
					const double holdSec = static_cast<double>(lastLatchUs - firstLatchUs) / 1e6;
					Assert::IsTrue(holdSec > 60.0, L"B1: Chain-alone latch must hold continuously for > 60 seconds");
					Assert::AreEqual(0, dropsAfterLatch, L"B1: zero drops / zero flapping during > 60 s hold");

					const C::EstimatorCounters cnt = est.GetCounters();
					Assert::AreEqual(uint64_t(1), cnt.jointLatch.entriesChainAlone, L"B1: entered cleanly once via Chain-alone");
					Assert::AreEqual(uint64_t(0), cnt.jointLatch.entriesJoint, L"B1: blind classical native does not enter via Joint");
					Assert::AreEqual(uint64_t(0), cnt.jointLatch.releases, L"B1: zero total releases during > 60 s hold");
					Assert::AreEqual(uint64_t(0), cnt.jointLatch.releasesDetailAbove, L"B1: zero false detailAbove releases");

					Assert::AreEqual(1920, latchedRec.applied[0].n);
					Assert::AreEqual(1080, latchedRec.applied[1].n);
					Assert::IsTrue((latchedRec.applied[0].reasonBits & C::kReasonChainAloneEntry) != 0u,
						L"L1/B1: published H AxisGeom carries kReasonChainAloneEntry");
					Assert::IsTrue((latchedRec.applied[1].reasonBits & C::kReasonChainAloneEntry) != 0u,
						L"L1/B1: published V AxisGeom carries kReasonChainAloneEntry");
					Assert::IsTrue(latchedRec.applied[0].conf > 0.0f && latchedRec.applied[0].conf <= 1.0f);
				}

				// (a2) Same-input Negative Control #1 (Rule 3): old broken sticky-native wiring (detailAbove = trackerNative = true)
				//      from t = 0 on the EXACT SAME 1080p->4K stream BLOCKS Chain-alone entry completely!
				{
					ChainFx::ManualClock clk;
					C::EstimatorSettings cfg;
					cfg.safetyRules = true;
					cfg.jointLatch.enabled = true;
					cfg.conventionDrivenProposals = true;
					C::ChainEstimator est(cfg, clk.Fn());
					est.SetSynchronous(true);
					est.SetMode(C::EstimatorMode::On);

					for (int i = 0; i < kTotalEvals; ++i)
					{
						const int64_t tUs = stream[static_cast<size_t>(i)].timeUs;
						clk.Set(static_cast<double>(tUs) / 1000.0);
						est.SetClassicalFresh(MakeClassicalFresh(
							true, 1.0, 1.0, trackerNative /* true = old sticky wiring */, stream[static_cast<size_t>(i)].seq, tUs, 1.5));
						Assert::IsTrue(est.SubmitLines(stream[static_cast<size_t>(i)]));
					}
					Assert::AreEqual(0, est.StatusInput().latchNx,
						L"negative control: sticky detailAbove=true blocks Chain-alone entry on the exact same stream");
					Assert::AreEqual(uint64_t(0), est.GetCounters().jointLatch.entriesChainAlone);
				}

				// (a3) Same-input Negative Control #2 (Rule 3): if already latched via Chain-alone and old sticky
				//      detailAbove=true arrives on the EXACT SAME stream, latch releases within ~3 s (detailAboveDebounceSec)!
				{
					ChainFx::ManualClock clk;
					C::EstimatorSettings cfg;
					cfg.safetyRules = true;
					cfg.jointLatch.enabled = true;
					cfg.conventionDrivenProposals = true;
					C::ChainEstimator est(cfg, clk.Fn());
					est.SetSynchronous(true);
					est.SetMode(C::EstimatorMode::On);

					int64_t stickyStartUs = -1;
					int64_t releaseUs = -1;
					for (int i = 0; i < kTotalEvals; ++i)
					{
						const int64_t tUs = stream[static_cast<size_t>(i)].timeUs;
						clk.Set(static_cast<double>(tUs) / 1000.0);
						const bool feedSticky = (stickyStartUs >= 0);
						est.SetClassicalFresh(MakeClassicalFresh(
							true, 1.0, 1.0, feedSticky ? trackerNative : false, stream[static_cast<size_t>(i)].seq, tUs, 1.5));
						Assert::IsTrue(est.SubmitLines(stream[static_cast<size_t>(i)]));

						if (stickyStartUs < 0 && est.StatusInput().latchNx == 1920)
							stickyStartUs = tUs;
						else if (stickyStartUs >= 0 && releaseUs < 0 && est.StatusInput().latchNx == 0)
						{
							releaseUs = tUs;
							break;
						}
					}
					Assert::IsTrue(stickyStartUs >= 0, L"fixture: latched before turning on sticky detailAbove");
					Assert::IsTrue(releaseUs >= 0, L"negative control: sticky detailAbove=true releases Chain-alone latch");
					const double releaseDelaySec = static_cast<double>(releaseUs - stickyStartUs) / 1e6;
					Assert::IsTrue(releaseDelaySec <= 6.0,
						L"negative control: sticky detailAbove=true releases within ~3 s debounce window");
					Assert::IsTrue(est.GetCounters().jointLatch.releasesDetailAbove >= 1);
				}
			}

			// Part B: Per-candidate R1 Nyquist check via ClassicalFreshInput::detailAboveAtRatio.
			//         Supply detailAboveAtRatio(rx, ry) returning true for 960x540 (0.25) and false for 1920x1080 (0.50):
			//         ApplyJointLatch MUST reject 960x540 and select/latch 1920x1080!
			//         Same-input negative control: uniform detailAbove = true blocks BOTH!
			{
				auto runPerCandidateR1 = [](bool usePerCandidateCb, int& outLatchNx, int& outLatchNy, int& outCbCalls) {
					ChainFx::ManualClock clk;
					C::EstimatorSettings cfg;
					cfg.safetyRules = true;
					cfg.jointLatch.enabled = true;
					cfg.conventionDrivenProposals = true;
					C::ChainEstimator est(cfg, clk.Fn());
					est.SetSynchronous(true);
					est.SetMode(C::EstimatorMode::On);

					outCbCalls = 0;
					uint64_t seq = 100;
					int s = 0;
					// 2 shots, each with 3 evals of 960x540 Lanczos3 (dips BOTH 960x540 and 2x harmonic 1920x1080)
					// followed by 2 evals of 1920x1080 bilinear (boosts 1920x1080 above any other candidate while keeping
					// 960x540 above jNeed). Hold entry until the final evaluation via uniform detailAbove = true.
					for (int shot = 0; shot < 2; ++shot)
					{
						for (int k = 0; k < 5; ++k, ++s, ++seq)
						{
							const int64_t tUs = static_cast<int64_t>(s) * 1000000;
							clk.Set(static_cast<double>(s) * 1500.0);
							const bool isFinal = (shot == 1 && k == 4);
							C::ClassicalFreshInput cf = MakeClassicalFresh(true, 1.0, 1.0, true, seq, tUs, 1.5);
							if (isFinal && usePerCandidateCb)
							{
								cf.detailAbove = false;
								cf.detailAboveAtRatio = [&outCbCalls](double rx, double ry) -> bool {
									++outCbCalls;
									// True above-band detail exists above 960x540 (0.25), NOT above 1920x1080 (0.50).
									return rx < 0.35 && ry < 0.35;
								};
							}
							est.SetClassicalFresh(cf);
							const int srcW = (k < 3) ? 960 : 1920;
							const int srcH = (k < 3) ? 540 : 1080;
							const int kern = (k < 3) ? kLanczos3 : kBilinear;
							const C::LineSnapshot snap = MakeSynthSnapshot(kUhdW, kUhdH, srcW, srcH, kern, 1,
								seq, tUs, 97001u + seq, k == 0);
							Assert::IsTrue(est.SubmitLines(snap));
						}
					}
					outLatchNx = est.StatusInput().latchNx;
					outLatchNy = est.StatusInput().latchNy;
				};

				int cbNx = -1, cbNy = -1, cbCalls = 0;
				runPerCandidateR1(true, cbNx, cbNy, cbCalls);
				Assert::IsTrue(cbCalls > 0, L"B1: detailAboveAtRatio callback must be queried per candidate in ApplyJointLatch");
				Assert::AreEqual(1920, cbNx, L"B1: per-candidate Nyquist rejects 960x540 and selects/latches 1920x1080");
				Assert::AreEqual(1080, cbNy);

				// Same-input negative control (Rule 3): uniform detailAbove = true blocks BOTH 960x540 and 1920x1080!
				int unifNx = -1, unifNy = -1, unifCalls = 0;
				runPerCandidateR1(false, unifNx, unifNy, unifCalls);
				Assert::AreEqual(0, unifNx, L"negative control: uniform detailAbove=true blocks both 960x540 and 1920x1080");
				Assert::AreEqual(0, unifNy);
			}
		}

		// 16. H1 + M1 (HIGH/MEDIUM):
		//     - Latched chain-alone 1080p->4K + steady blind-classical native (ratio=1.0, detailAbove=false) + 10 spurious
		//       ABR marks -> 0 false releases (with direct ChainStepMark positiveDetailAbove=true negative control);
		//     - Single classical snapshot (same frameSeq / single SetClassicalFresh call) MUST NOT double-count as
		//       2 consecutive classical frames (with 2 distinct frameSeq contrast control);
		//     - M1 staleness drop: ClassicalFreshInput with age > 2 * analysisIntervalSec increments
		//       classicalFreshStaleDrops and is ignored (with fresh contrast control).
		TEST_METHOD(H1M1BlindClassicalAbrSentinelStaleSnapshotAndStalenessDrop)
		{
			using namespace S2Fx;

			// Part A (H1): Latched chain-alone 1080p->4K + steady blind-classical native + 10 spurious ABR marks -> 0 releases!
			{
				ChainFx::ManualClock clk;
				C::EstimatorSettings cfg;
				cfg.safetyRules = true;
				cfg.jointLatch.enabled = true;
				cfg.stepMark.enabled = true;
				cfg.conventionDrivenProposals = true;
				C::ChainEstimator est(cfg, clk.Fn());
				est.SetSynchronous(true);
				est.SetMode(C::EstimatorMode::On);

				uint64_t seq = 10;
				uint64_t cfSeq = 1;
				int s = 0;
				// Acquire 1920x1080 via Chain-alone (2 shots, blind classical native throughout).
				for (int shot = 0; shot < 2; ++shot)
				{
					for (int k = 0; k < 5; ++k, ++s, ++seq)
					{
						const int64_t tUs = static_cast<int64_t>(s) * 1000000;
						clk.Set(static_cast<double>(s) * 1500.0);
						est.SetClassicalFresh(MakeClassicalFresh(true, 1.0, 1.0, false, ++cfSeq, tUs, 1.5));
						const C::LineSnapshot up = MakeSynthSnapshot(kUhdW, kUhdH, 1920, 1080, kBilinear, 1,
							seq, tUs, 98001u + seq, k == 0);
						Assert::IsTrue(est.SubmitLines(up));
					}
				}
				Assert::AreEqual(1920, est.StatusInput().latchNx, L"fixture: latched 1920x1080 via Chain-alone");
				Assert::AreEqual(1080, est.StatusInput().latchNy);

				// Fire 10 spurious ABR marks while steady 1080p->4K continues and blind classical keeps reporting native (detailAbove=false).
				for (int m = 0; m < 10; ++m)
				{
					const int64_t tMarkUs = static_cast<int64_t>(s) * 1000000;
					est.NoteAbrStepEvent(tMarkUs, +0.25, seq);
					for (int k = 0; k < 2; ++k, ++s, ++seq)
					{
						const int64_t tUs = static_cast<int64_t>(s) * 1000000;
						clk.Set(static_cast<double>(s) * 1500.0);
						est.SetClassicalFresh(MakeClassicalFresh(true, 1.0, 1.0, false, ++cfSeq, tUs, 1.5));
						const C::LineSnapshot up = MakeSynthSnapshot(kUhdW, kUhdH, 1920, 1080, kBilinear, 1,
							seq, tUs, 98200u + seq, false);
						Assert::IsTrue(est.SubmitLines(up));
					}
					Assert::AreEqual(1920, est.StatusInput().latchNx, L"H1: spurious ABR mark with blind-classical native must NOT release");
				}

				const C::EstimatorCounters cnt = est.GetCounters();
				Assert::AreEqual(uint64_t(10), cnt.stepMark.marksRaised);
				Assert::AreEqual(uint64_t(10), cnt.stepMark.unconfirmedTimeouts);
				Assert::AreEqual(uint64_t(0), cnt.stepMark.confirmedByClassical, L"H1: blind-classical native never confirms ABR mark");
				Assert::AreEqual(uint64_t(0), cnt.stepMark.immediateReleases, L"H1: 0 false immediate releases across 10 spurious marks");
				Assert::AreEqual(uint64_t(0), cnt.jointLatch.releasesAbr);
				Assert::AreEqual(uint64_t(0), cnt.jointLatch.releases);

				// Same-input negative control (a): direct ChainStepMark with positiveDetailAbove = true on 2 fresh frames DOES confirm!
				C::StepMarkSettings smCfg;
				smCfg.enabled = true;
				C::ChainStepMark smDirect(smCfg);
				C::StepMarkLatchContext lctx;
				lctx.latched = true;
				lctx.nx = 1920; lctx.Nx = kUhdW;
				lctx.ny = 1080; lctx.Ny = kUhdH;
				Assert::IsTrue(smDirect.OnAbrStep(10 * 1000000, +0.50).action == C::StepMarkAction::MarkPending);

				C::StepMarkClassicalFrame f1;
				f1.timeUs = 11 * 1000000;
				f1.informative = true;
				f1.nativeReading = true;
				f1.positiveDetailAbove = true;
				f1.rasterW = kUhdW; f1.rasterH = kUhdH;
				Assert::IsTrue(smDirect.OnClassicalFrame(f1, lctx).action == C::StepMarkAction::None);

				C::StepMarkClassicalFrame f2 = f1;
				f2.timeUs = 12 * 1000000;
				const auto oPos = smDirect.OnClassicalFrame(f2, lctx);
				Assert::IsTrue(oPos.action == C::StepMarkAction::ConfirmVoidAndRelease,
					L"negative control (a): genuine positiveDetailAbove=true on 2 fresh frames confirms and releases promptly");
				Assert::AreEqual(uint64_t(1), smDirect.Counters().confirmedByClassical);
				Assert::AreEqual(uint64_t(1), smDirect.Counters().immediateReleases);
			}

			// Part B (H1): Single classical snapshot (same frameSeq / single SetClassicalFresh call) across 2+ chain evals
			//              MUST NOT double-count as 2 consecutive frames (0 false confirmations); 2 distinct frameSeqs DO confirm!
			{
				ChainFx::ManualClock clk;
				C::EstimatorSettings cfg;
				cfg.safetyRules = true;
				cfg.jointLatch.enabled = true;
				cfg.stepMark.enabled = true;
				cfg.conventionDrivenProposals = true;
				C::ChainEstimator est(cfg, clk.Fn());
				est.SetSynchronous(true);
				est.SetMode(C::EstimatorMode::On);

				uint64_t seq = 10;
				uint64_t cfSeq = 100;
				int s = 0;
				// Latch 960x540 in 1 shot via Joint.
				for (; s <= 8 && est.StatusInput().latchNx == 0; ++s, ++seq)
				{
					const int64_t tUs = static_cast<int64_t>(s) * 1000000;
					clk.Set(static_cast<double>(s) * 1500.0);
					est.SetClassicalFresh(MakeClassicalFresh(true, 0.25, 0.25, false, ++cfSeq, tUs, 2.0));
					const C::LineSnapshot up540 = MakeSynthSnapshot(kUhdW, kUhdH, 960, 540, kBilinear, 1,
						seq, tUs, 98400u + seq, s == 0);
					Assert::IsTrue(est.SubmitLines(up540));
				}
				Assert::AreEqual(960, est.StatusInput().latchNx, L"fixture: latched 960x540");

				// Raise ABR mark and post ONLY ONE classical snapshot (frameSeq = 500) at 1920x1080 across 2 flat chain evals:
				const int64_t t0Us = static_cast<int64_t>(s) * 1000000;
				est.NoteAbrStepEvent(t0Us, +0.25, seq);
				est.SetClassicalFresh(MakeClassicalFresh(true, 0.5, 0.5, false, 500, t0Us + 500000, 3.0));
				for (int k = 0; k < 2; ++k, ++s, ++seq)
				{
					const int64_t tUs = static_cast<int64_t>(s) * 1000000;
					clk.Set(static_cast<double>(s) * 1500.0);
					const C::LineSnapshot flatPost = MakeFlatSnapshot(kUhdW, kUhdH, 512, seq, tUs, false);
					Assert::IsTrue(est.SubmitLines(flatPost));
				}
				Assert::AreEqual(960, est.StatusInput().latchNx,
					L"H1: ONE single classical snapshot must NOT double-count as 2 consecutive frames");
				Assert::AreEqual(uint64_t(0), est.GetCounters().stepMark.confirmedByClassical);
				Assert::AreEqual(uint64_t(0), est.GetCounters().stepMark.immediateReleases);
				Assert::AreEqual(uint64_t(1), est.GetCounters().stepMark.unconfirmedTimeouts);

				// Same-input contrast control (b): now feed 2 genuinely DISTINCT frameSeqs (601, 602) across 2 flat evals -> DOES confirm!
				const int64_t t1Us = static_cast<int64_t>(s) * 1000000;
				est.NoteAbrStepEvent(t1Us, +0.25, seq);
				for (int k = 0; k < 2; ++k, ++s, ++seq)
				{
					const int64_t tUs = static_cast<int64_t>(s) * 1000000;
					clk.Set(static_cast<double>(s) * 1500.0);
					est.SetClassicalFresh(MakeClassicalFresh(true, 0.5, 0.5, false, static_cast<uint64_t>(601 + k), tUs, 3.0));
					const C::LineSnapshot flatPost = MakeFlatSnapshot(kUhdW, kUhdH, 512, seq, tUs, false);
					Assert::IsTrue(est.SubmitLines(flatPost));
				}
				Assert::AreEqual(0, est.StatusInput().latchNx,
					L"contrast control (b): 2 distinct classical frameSeqs confirm and release immediately");
				Assert::AreEqual(uint64_t(1), est.GetCounters().stepMark.confirmedByClassical);
				Assert::AreEqual(uint64_t(1), est.GetCounters().stepMark.immediateReleases);
			}

			// Part C (M1): ClassicalFreshInput staleness check (age > 2 * analysisIntervalSec -> drop & count visibly).
			{
				auto runStaleness = [](int64_t classicalTimeUs, int& outLatchNx, C::EstimatorCounters& outCnt) {
					ChainFx::ManualClock clk;
					C::EstimatorSettings cfg;
					cfg.safetyRules = true;
					cfg.jointLatch.enabled = true;
					cfg.conventionDrivenProposals = true;
					C::ChainEstimator est(cfg, clk.Fn());
					est.SetSynchronous(true);
					est.SetMode(C::EstimatorMode::On);

					// 1-shot 1080p->4K from t = 8..11 s with classical silent (builds 1-shot credit >= jNeed, dwell = 3 s).
					est.SetClassicalFresh(false, 1.0, 1.0, false);
					for (int sec = 8; sec <= 11; ++sec)
					{
						clk.Set(static_cast<double>(sec) * 1500.0);
						const C::LineSnapshot up = MakeSynthSnapshot(kUhdW, kUhdH, 1920, 1080, kBilinear, 1,
							static_cast<uint64_t>(sec), static_cast<int64_t>(sec) * 1000000, 98600u + static_cast<uint64_t>(sec), sec == 8);
						Assert::IsTrue(est.SubmitLines(up));
					}
					Assert::AreEqual(0, est.StatusInput().latchNx, L"fixture: 1-shot chain alone has not latched yet");

					// At t = 12 s (dwell = 4.0 s >= 3.0 s), feed ClassicalFreshInput with analysisIntervalSec = 0.5 s:
					//   - Stale (timeUs = 10 s -> age = 2.0 s > 2 * 0.5 s = 1.0 s): dropped & ignored!
					//   - Fresh (timeUs = 11.7 s -> age = 0.3 s <= 1.0 s): triggers 1-shot Joint entry!
					clk.Set(12.0 * 1500.0);
					est.SetClassicalFresh(MakeClassicalFresh(true, 0.5, 0.5, false, 700, classicalTimeUs, 0.5));
					const C::LineSnapshot up12 = MakeSynthSnapshot(kUhdW, kUhdH, 1920, 1080, kBilinear, 1,
						12, 12 * 1000000, 98612u, false);
					Assert::IsTrue(est.SubmitLines(up12));
					outLatchNx = est.StatusInput().latchNx;
					outCnt = est.GetCounters();
				};

				// Stale classical witness (timeUs = 10 s at eval t = 12 s, age = 2.0 s > 2 * 0.5 s = 1.0 s):
				int staleNx = -1;
				C::EstimatorCounters staleCnt;
				runStaleness(10 * 1000000, staleNx, staleCnt);
				Assert::IsTrue(staleCnt.classicalFreshStaleDrops >= 1,
					L"M1: stale classical witness (age 2.0s > 2 * 0.5s) must increment classicalFreshStaleDrops");
				Assert::AreEqual(0, staleNx, L"M1: stale classical witness must be ignored (no 1-shot Joint entry)");
				Assert::AreEqual(uint64_t(0), staleCnt.jointLatch.entriesJoint);

				// Same-input contrast control (fresh timeUs = 11.7 s, age = 0.3 s <= 1.0 s): enters Joint cleanly!
				int freshNx = -1;
				C::EstimatorCounters freshCnt;
				runStaleness(11700000, freshNx, freshCnt);
				Assert::AreEqual(uint64_t(0), freshCnt.classicalFreshStaleDrops, L"contrast control: fresh witness is not dropped");
				Assert::AreEqual(1920, freshNx, L"contrast control: fresh classical witness triggers 1-shot Joint entry");
				Assert::AreEqual(uint64_t(1), freshCnt.jointLatch.entriesJoint);
			}
		}

		// 17. M4 (MEDIUM): ChainEvidence edgeStats MUST abstain visibly (edgeStatsNullMismatch == true,
		//     kReasonNullTableMismatch, !informative, increment == 0) when given NullTables::Placeholder() OR a
		//     calibrated WholeLineLogRatio table, and ChainEstimator with eightBitPromotion = true + PlaceholderOnly()
		//     MUST increment EstimatorCounters::edgeStatsNullTableAbstains > 0!
		TEST_METHOD(M4EdgeStatsRejectsUnmatchedOrPlaceholderNullTablesVisibly)
		{
			using namespace S2Fx;

			// Part A: Direct ChainEvidence stat-kind & placeholder guard with same-input positive control.
			{
				const double floor8Tpdf = C::TpdfDitherFloorCodes(4.0);
				C::VerifyResult vr;
				vr.candidates.push_back(MakeSyntheticCandidate(1920, 2.0, -0.10, 16, 15, 7.0, 1.85, 1.70, 1.65, 0.5, 3));
				vr.candidates.push_back(MakeSyntheticCandidate(1440, 2.0, 0.00, 16, 15, 7.0, 7.0, 1.70, 1.65, 0.375, 3));
				vr.candidates.push_back(MakeSyntheticCandidate(2400, 2.0, 0.01, 16, 15, 7.0, 7.0, 1.70, 1.65, 0.625, 3));
				vr.candidates.push_back(MakeSyntheticCandidate(3360, 2.0, -0.01, 16, 15, 7.0, 7.0, 1.70, 1.65, 0.875, 3));

				C::AxisObservation obs;
				obs.verify = &vr;
				obs.axisLen = kUhdW;
				obs.floorCodes = floor8Tpdf;
				obs.probeRan = true;
				obs.probeSeen = true;
				obs.shotId = 1;
				obs.timeUs = -1;
				obs.rasterW = kUhdW;
				obs.rasterH = kUhdH;

				C::EvidenceSettings onCfg;
				onCfg.edgeStats.enabled = true;

				// (1) NullTables::Placeholder() (placeholder=true, statKind=WholeLineLogRatio) -> MUST abstain visibly!
				{
					C::ChainEvidence evPh(onCfg, C::NullTables::Placeholder());
					C::EvidenceUpdate upPh;
					Assert::IsTrue(evPh.Submit(0, obs, upPh));
					Assert::IsFalse(upPh.cand[0].informative, L"M4: Placeholder null table must abstain under edgeStats");
					Assert::AreEqual(0.0, upPh.cand[0].increment, 1e-12, L"M4: abstained candidate earns 0.0 increment");
					Assert::IsTrue(upPh.cand[0].edgeStatsNullMismatch, L"M4: edgeStatsNullMismatch visibly set on Placeholder");
					Assert::IsTrue((upPh.cand[0].reason & C::kReasonNullTableMismatch) != 0u,
						L"M4: kReasonNullTableMismatch reason bit visibly set on Placeholder");
				}

				// (2) Calibrated WholeLineLogRatio table (placeholder=false, statKind=WholeLineLogRatio) -> STILL MUST abstain!
				{
					C::NullTables calWholeLine = C::NullTables::Placeholder();
					calWholeLine.placeholder = false;
					calWholeLine.statKind = C::NullStatKind::WholeLineLogRatio;
					C::ChainEvidence evWl(onCfg, calWholeLine);
					C::EvidenceUpdate upWl;
					Assert::IsTrue(evWl.Submit(0, obs, upWl));
					Assert::IsFalse(upWl.cand[0].informative, L"M4: WholeLineLogRatio table must abstain for SegmentExcessVariance stat");
					Assert::AreEqual(0.0, upWl.cand[0].increment, 1e-12);
					Assert::IsTrue(upWl.cand[0].edgeStatsNullMismatch, L"M4: edgeStatsNullMismatch visibly set on WholeLineLogRatio");
					Assert::IsTrue((upWl.cand[0].reason & C::kReasonNullTableMismatch) != 0u,
						L"M4: kReasonNullTableMismatch reason bit visibly set on WholeLineLogRatio");
				}

				// (3) Same-input positive control: calibrated SegmentExcessVariance table on the EXACT SAME VerifyResult succeeds!
				{
					C::ChainEvidence evCal(onCfg, MakeCalibratedEdgeStatsNulls());
					C::EvidenceUpdate upCal;
					Assert::IsTrue(evCal.Submit(0, obs, upCal));
					Assert::IsTrue(upCal.cand[0].informative, L"positive control: calibrated SegmentExcessVariance table is accepted");
					Assert::IsTrue(upCal.cand[0].increment > 0.0);
					Assert::IsFalse(upCal.cand[0].edgeStatsNullMismatch);
					Assert::IsTrue((upCal.cand[0].reason & C::kReasonNullTableMismatch) == 0u);
				}
			}

			// Part B: End-to-end ChainEstimator with eightBitPromotion = true and DepthNullTableSet::PlaceholderOnly()
			//         on a sparse 80%-flat 8-bit TPDF-dithered 1080p->4K frame -> edgeStatsNullTableAbstains > 0!
			{
				ChainFx::ManualClock clk;
				C::EstimatorSettings cfg;
				cfg.eightBitPromotion = true;
				cfg.depthNullTables = C::DepthNullTableSet::PlaceholderOnly();
				cfg.safetyRules = true;
				cfg.conventionDrivenProposals = true;
				C::ChainEstimator est(cfg, clk.Fn());
				est.SetSynchronous(true);
				est.SetMode(C::EstimatorMode::On);

				// 80%-flat 8-bit TPDF-dithered 1080p->4K snapshot (step = 4): whole-line baseline is diluted by 80% flat
				// dither so sparse edgeStats activates, hitting PlaceholderOnly() -> edgeStatsNullTableAbstains > 0!
				for (int i = 0; i < 3; ++i)
				{
					clk.Set(static_cast<double>(i + 1) * 1500.0);
					const C::LineSnapshot sparse8 = MakeSynthSnapshot(kUhdW, kUhdH, 1920, 1080, kBilinear, 4,
						static_cast<uint64_t>(10 + i), static_cast<int64_t>(i + 1) * 1000000,
						98800u + static_cast<uint64_t>(i), i == 0, 0.0, true, 0.80);
					Assert::IsTrue(est.SubmitLines(sparse8));
				}
				Assert::IsTrue(est.GetCounters().edgeStatsNullTableAbstains > 0,
					L"M4: eightBitPromotion + PlaceholderOnly must visibly increment edgeStatsNullTableAbstains");
			}
		}

		// 18. L1 + L2 (LOW):
		//     - L1: Joint entry sets kReasonJointEntry + derived conf (< 1.0f when LLR < c1, NOT hardcoded 1.0f),
		//       Chain-Alone entry sets kReasonChainAloneEntry, and ChainJointLatch exposes JointLatchOrigin cleanly;
		//     - L2: Pushing > eventCap ABR step events before evaluation visibly increments abrStepEventsEvicted!
		TEST_METHOD(L1JointOriginReasonBitsAndDerivedConfAndL2AbrEventEvictionCounter)
		{
			using namespace S2Fx;
			const double c1 = C::ComputeThresholds(C::EvidenceSettings()).c1;

			// L1 (a): Direct ChainJointLatch::Origin() & JointLatchOutput::origin contract.
			{
				C::JointLatchSettings js;
				js.enabled = true;
				C::ChainJointLatch latch(js);
				Assert::IsTrue(latch.Origin() == C::JointLatchOrigin::None, L"L1: unlatched origin is None");

				C::JointLatchInput in;
				in.timeUs = 5 * 1000000;
				in.c1 = c1;
				in.rasterW = kUhdW; in.rasterH = kUhdH;
				in.chainInformative = true;
				in.classicalValid = true;
				in.classicalNx = 1920; in.classicalNxDenom = kUhdW;
				in.classicalNy = 1080; in.classicalNyDenom = kUhdH;
				in.currentShotDwellSec = 4.0;
				in.currentShotCreditX = 0.6 * c1;
				in.currentShotCreditY = 0.6 * c1;
				const C::JointLatchOutput oJoint = latch.Step(in);
				Assert::IsTrue(oJoint.latched && oJoint.event == C::JointLatchEvent::EnteredJoint);
				Assert::IsTrue(oJoint.origin == C::JointLatchOrigin::Joint, L"L1: Joint output origin == Joint");
				Assert::IsTrue(latch.Origin() == C::JointLatchOrigin::Joint, L"L1: latch.Origin() == Joint");

				// Chain-Alone origin on fresh latch:
				C::ChainJointLatch latchCa(js);
				C::JointLatchInput inCa;
				inCa.timeUs = 10 * 1000000;
				inCa.c1 = c1;
				inCa.rasterW = kUhdW; inCa.rasterH = kUhdH;
				inCa.chainInformative = true;
				inCa.haveChainPair = true;
				inCa.chainNx = 1920; inCa.chainNxDenom = kUhdW;
				inCa.chainNy = 1080; inCa.chainNyDenom = kUhdH;
				inCa.chainLlrX = c1 + 2.0;
				inCa.chainLlrY = c1 + 2.0;
				inCa.chainShots = 2;
				const C::JointLatchOutput oCa = latchCa.Step(inCa);
				Assert::IsTrue(oCa.latched && oCa.event == C::JointLatchEvent::EnteredChainAlone);
				Assert::IsTrue(oCa.origin == C::JointLatchOrigin::ChainAlone, L"L1: ChainAlone output origin == ChainAlone");
				Assert::IsTrue(latchCa.Origin() == C::JointLatchOrigin::ChainAlone, L"L1: latch.Origin() == ChainAlone");
			}

			// L1 (b): End-to-end ChainEstimator 1-shot Joint entry -> kReasonJointEntry + derived conf (NOT hardcoded 1.0f when LLR < c1).
			{
				ChainFx::ManualClock clk;
				C::EstimatorSettings cfg;
				cfg.safetyRules = true;
				cfg.jointLatch.enabled = true;
				cfg.conventionDrivenProposals = true;
				C::ChainEstimator est(cfg, clk.Fn());
				est.SetSynchronous(true);
				est.SetMode(C::EstimatorMode::On);

				for (int s = 0; s <= 5; ++s)
				{
					const int64_t tUs = static_cast<int64_t>(s) * 1000000;
					clk.Set(static_cast<double>(s) * 1500.0);
					est.SetClassicalFresh(MakeClassicalFresh(true, 0.5, 0.5, false, static_cast<uint64_t>(10 + s), tUs, 1.5));
					const C::LineSnapshot up = MakeSynthSnapshot(kUhdW, kUhdH, 1920, 1080, kBilinear, 1,
						static_cast<uint64_t>(10 + s), tUs, 99001u + static_cast<uint64_t>(s), s == 0);
					Assert::IsTrue(est.SubmitLines(up));
					if (est.StatusInput().latchNx == 1920)
						break;
				}
				Assert::AreEqual(1920, est.StatusInput().latchNx, L"fixture: 1-shot Joint latched 1920x1080");
				Assert::AreEqual(uint64_t(1), est.GetCounters().jointLatch.entriesJoint);

				G::Record rec;
				Assert::IsTrue(est.Held().ReadLatest(0, rec));
				for (int a = 0; a < 2; ++a)
				{
					Assert::IsTrue((rec.applied[a].reasonBits & C::kReasonJointEntry) != 0u,
						L"L1: Joint entry published AxisGeom must set kReasonJointEntry");
					Assert::IsTrue((rec.applied[a].reasonBits & C::kReasonChainAloneEntry) == 0u,
						L"L1: Joint entry must not set kReasonChainAloneEntry");
					Assert::IsTrue(rec.applied[a].llr > 0.0f && rec.applied[a].llr < static_cast<float>(c1),
						L"fixture: 1-shot Joint LLR is strictly below c1");
					// Derived confidence (ConfFromLlr / ConfidenceOfLlr), NEVER hardcoded 1.0f when LLR < c1!
					Assert::IsTrue(rec.applied[a].conf > 0.0f && rec.applied[a].conf < 1.0f,
						L"L1: Joint entry conf must be derived (< 1.0f when LLR < c1), never hardcoded 1.0f");
					const float confSigmoid = static_cast<float>(C::ConfFromLlr(rec.applied[a].llr));
					const float confLinear = static_cast<float>(C::ConfidenceOfLlr(rec.applied[a].llr, c1));
					const bool matchesDerived =
						(std::fabs(rec.applied[a].conf - confSigmoid) < 1e-3f) ||
						(std::fabs(rec.applied[a].conf - confLinear) < 1e-3f);
					Assert::IsTrue(matchesDerived, L"L1: Joint entry conf matches derived ConfFromLlr / ConfidenceOfLlr");
				}
			}

			// L1 (c): End-to-end ChainEstimator 2-shot Chain-Alone entry -> kReasonChainAloneEntry (NOT kReasonJointEntry).
			{
				ChainFx::ManualClock clk;
				C::EstimatorSettings cfg;
				cfg.safetyRules = true;
				cfg.jointLatch.enabled = true;
				cfg.conventionDrivenProposals = true;
				C::ChainEstimator est(cfg, clk.Fn());
				est.SetSynchronous(true);
				est.SetMode(C::EstimatorMode::On);
				est.SetClassicalFresh(false, 1.0, 1.0, false);

				uint64_t seq = 10;
				int s = 0;
				for (int shot = 0; shot < 2; ++shot)
				{
					for (int k = 0; k < 5; ++k, ++s, ++seq)
					{
						clk.Set(static_cast<double>(s) * 1500.0);
						const C::LineSnapshot up = MakeSynthSnapshot(kUhdW, kUhdH, 1920, 1080, kBilinear, 1,
							seq, static_cast<int64_t>(s) * 1000000, 99100u + seq, k == 0);
						Assert::IsTrue(est.SubmitLines(up));
					}
				}
				Assert::AreEqual(1920, est.StatusInput().latchNx);
				G::Record rec;
				Assert::IsTrue(est.Held().ReadLatest(0, rec));
				for (int a = 0; a < 2; ++a)
				{
					Assert::IsTrue((rec.applied[a].reasonBits & C::kReasonChainAloneEntry) != 0u,
						L"L1: Chain-Alone entry must set kReasonChainAloneEntry");
					Assert::IsTrue((rec.applied[a].reasonBits & C::kReasonJointEntry) == 0u,
						L"L1: Chain-Alone entry must not set kReasonJointEntry");
				}
			}

			// L2: Pushing > eventCap ABR step events before evaluation visibly increments abrStepEventsEvicted!
			{
				ChainFx::ManualClock clk;
				C::EstimatorSettings cfg;
				cfg.stepMark.enabled = true;
				cfg.eventCap = 4;
				C::ChainEstimator est(cfg, clk.Fn());
				est.SetSynchronous(true);
				est.SetMode(C::EstimatorMode::On);

				// Within cap (4 events <= eventCap = 4): 0 evictions.
				for (int i = 0; i < 4; ++i)
					est.NoteAbrStepEvent(static_cast<int64_t>(i + 1) * 1000000, +0.25, static_cast<uint64_t>(i + 1));
				Assert::AreEqual(uint64_t(0), est.GetCounters().abrStepEventsEvicted,
					L"contrast control: <= eventCap ABR step events cause 0 evictions");

				// Push 6 more events (total 10 > eventCap = 4) before any evaluation -> 6 evicted visibly!
				for (int i = 4; i < 10; ++i)
					est.NoteAbrStepEvent(static_cast<int64_t>(i + 1) * 1000000, +0.25, static_cast<uint64_t>(i + 1));
				Assert::AreEqual(uint64_t(6), est.GetCounters().abrStepEventsEvicted,
					L"L2: pushing 10 ABR step events into eventCap=4 queue must visibly count 6 evictions");
			}
		}
	};
}
