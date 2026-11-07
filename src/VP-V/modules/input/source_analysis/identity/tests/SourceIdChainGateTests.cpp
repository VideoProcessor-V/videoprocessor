/*
 * Copyright(C) 2026 VP-V contributors
 * SPDX-License-Identifier: GPL-3.0-only
 */
// The Monitor with the chain in the consensus (SID train r2, doc 78 section 5 owner decision). N1: with source_id_consensus ON the
// CONSENSUS (chain + classical voters) feeds the ORIGINAL AutoFactor(s) engage / revert in place of the tracker's declared value -
// WHAT; WHEN is the original latch's (engage moments, dwell, crossfade). Off = the original byte for byte, Shadow = the original
// decides and the would-be is computed and published only. N2: confident chain evidence that contradicts native stops one
// native-looking frame from clearing the trackers and from raising the classical safety veto. The chain has no gate, no engage claim
// and no hold of its own any more. The record's fallback (product.source_geometry) is visible; the Original pathway forces Off.
// CPU only, synthetic tiles through AnalyseTilesForTest, no GPU, no files.
#include "pch.h"
#include "CppUnitTest.h"

#include "GeometryFixtures.h"

#include <VP-V/modules/input/source_analysis/SourceResolutionMonitor.h>

#include <cstdint>
#include <string>
#include <vector>

using namespace Microsoft::VisualStudio::CppUnitTestFramework;

namespace Tests
{
	namespace SidGate
	{
		typedef SourceResolutionMonitor::Monitor Mon;
		constexpr int T = 128, kTiles = 8;

		inline int64_t Us(double sec) { return static_cast<int64_t>(sec * 1e6); }

		// One analysed frame of textured tiles through the same path as a captured frame (the classical reading is incidental here:
		// the chain decides and the snapshot is replaced by a scripted one before the factors are asked for).
		inline void Analyse(Mon& m, double seconds, uint64_t seq)
		{
			std::vector<float> data(static_cast<size_t>(T) * T * kTiles);
			uint32_t x = 12345u + static_cast<uint32_t>(seq);
			for (size_t i = 0; i < data.size(); ++i)
			{
				x = x * 1664525u + 1013904223u;
				data[i] = static_cast<float>(0.3 + 0.4 * (static_cast<double>(x >> 8) / 16777216.0));
			}
			const std::vector<float> none;
			m.AnalyseTilesForTest(data, kTiles, seconds, 3840, 2160, none, none, kTiles, T, 64.0 / 1023.0, seq);
		}

		inline SourceResolutionMonitor::Snapshot CleanSnapshot(unsigned long long frames, bool blank)
		{
			SourceResolutionMonitor::Snapshot s;
			s.reading = true;
			s.nativeWidth = 3840;
			s.nativeHeight = 2160;
			s.axisRatioX = 1.0;
			s.axisRatioY = 1.0;
			s.axisConfidence = 0.0;
			s.frames = frames;
			s.frameBlank = blank;
			return s;
		}

		// A chain record of the evaluation `evalSeq`: applied 1920 -> 3840 and 1080 -> 2160 (KernelBank index `kernel`), or native.
		inline GeoFx::G::Record Upscaled(uint64_t evalSeq, double timeSec, int kernel = 5)
		{
			using namespace GeoFx;
			return MakeRecord(evalSeq, Us(timeSec), AppliedAxis(1920, 3840, kernel), AppliedAxis(1080, 2160, kernel));
		}

		inline GeoFx::G::Record NativeRecord(uint64_t evalSeq, double timeSec)
		{
			using namespace GeoFx;
			return MakeRecord(evalSeq, Us(timeSec), AppliedAxis(3840, 3840, -1), AppliedAxis(2160, 2160, -1));
		}

		// `frames` analysed frames at 0.5 s from `t0`, sequence numbers from `seq` on; with `record` set, one chain record per second
		// (distinct evaluations; the frame between reads the same record again). Returns the next sequence.
		inline uint64_t Feed(Mon& m, GeoFx::G::HeldState& geo, double t0, int frames, uint64_t seq, bool upscaled, bool publish = true)
		{
			for (int i = 0; i < frames; ++i)
			{
				const double t = t0 + 0.5 * i;
				if (publish && i % 2 == 0)
					geo.Publish(upscaled ? Upscaled(seq, t - 0.1) : NativeRecord(seq, t - 0.1));
				Analyse(m, t, seq);
				++seq;
			}
			return seq;
		}

		struct Outcome
		{
			double fx = 1.0, fy = 1.0;
			std::string why1, why2;
			uint64_t nextSeq = 0;
		};

		// 6 s of frames with a chain record per second (the consensus adopts the chain value after two evaluations and its 3 s dwell),
		// then the factors twice: the first call sees the wish and waits, the second is a low-detail (black) moment.
		inline Outcome Script(Mon& m, GeoFx::G::HeldState& geo, double t0)
		{
			Outcome o;
			uint64_t seq = Feed(m, geo, t0, 13, 2, true);
			m.InjectResultForTest(CleanSnapshot(1, false), seq - 1);
			std::string why;
			m.AutoFactors(3840, 2160, 0.8, why, o.fx, o.fy, t0 + 6.1, seq);
			o.why1 = why;
			why.clear();
			m.InjectResultForTest(CleanSnapshot(2, true), seq);
			m.AutoFactors(3840, 2160, 0.8, why, o.fx, o.fy, t0 + 6.6, seq + 1);
			o.why2 = why;
			o.nextSeq = seq + 2;
			return o;
		}
	}

	TEST_CLASS(SourceIdChainGateTests)
	{
	public:
		TEST_METHOD(OnEngagesThroughTheConsensusWhenOnlyTheChainSeesTheUpscale)
		{
			SidGate::Mon m;
			GeoFx::G::HeldState geo;
			m.SetGeometrySource(&geo);
			m.SetSourceIdMode(SidGate::Mon::SourceIdMode::On);
			const SidGate::Outcome o = SidGate::Script(m, geo, 10.0);
			SourceIdentity::Record p14;
			Assert::IsTrue(m.SourceIdRecord(p14) && p14.ratioH == 0.5f && p14.ratioV == 0.5f, L"fixture: the consensus adopted the chain value (not at once: two evaluations and the dwell)");
			Assert::IsTrue((p14.flags & SourceIdentity::kFlagChainVote) != 0);
			Assert::IsTrue(m.Read().declaredRatio == 1.0, L"fixture: the classical trackers declared nothing");
			Assert::IsTrue(o.why1.find("waiting") != std::string::npos, L"the consensus's wish waits for an engage moment (WHEN is unchanged)");
			Assert::AreEqual(0.5, o.fx, 1e-9, L"the consensus fed the original engage at the low-detail moment");
			Assert::AreEqual(0.5, o.fy, 1e-9);
			Assert::IsFalse(m.SourceIdGeometryFallback().engaged, L"a fresh record is no fallback");
			const std::string status = m.SourceIdStatusLine(16.7);
			Assert::IsTrue(status.find("chain: H 1920->3840 lanczos3") != std::string::npos, L"the status says what the chain reads");
		}

		TEST_METHOD(TheEngageOfTheConsensusValueWaitsForTheCrossfadeAfterTheMaxWait)
		{
			SidGate::Mon m;
			m.SetEngageTiming(4.0, 1.5, 0.25);   // a short max wait
			GeoFx::G::HeldState geo;
			m.SetGeometrySource(&geo);
			m.SetSourceIdMode(SidGate::Mon::SourceIdMode::On);
			double firstEngaged = -1.0;
			std::string whyAtEngage;
			double fx = 1.0, fy = 1.0;
			uint64_t seq = 2;
			for (int i = 0; i <= 40; ++i)   // frames at 10.0 .. 30.0 s, a chain record per second, never a cut, black or low-detail moment
			{
				const double t = 10.0 + 0.5 * i;
				if (i % 2 == 0)
					geo.Publish(SidGate::Upscaled(seq, t - 0.1));
				SidGate::Analyse(m, t, seq);
				m.InjectResultForTest(SidGate::CleanSnapshot(1, false), seq);
				std::string why;
				m.AutoFactors(3840, 2160, 0.8, why, fx, fy, t + 0.1, seq + 1);
				if (fx < 1.0 && firstEngaged < 0.0)
				{
					firstEngaged = t;
					whyAtEngage = why;
				}
				++seq;
			}
			// The consensus adopts at t = 14.0 s (second evaluation 11.0 s + 3 s dwell); the wish is first seen then; the 4 s max wait ends at 18 s.
			Assert::IsTrue(firstEngaged >= 18.0 - 1e-9 && firstEngaged <= 19.0 + 1e-9, L"engaged only by the crossfade, after the max wait since the wish");
			Assert::IsTrue(whyAtEngage.find("crossfade") != std::string::npos);
			Assert::AreEqual(0.5, fx, 1e-9);
		}

		TEST_METHOD(ARevertToNativeFollowsTheConsensusAndIsImmediateWhenTheTrackerSeesNative)
		{
			SidGate::Mon m;
			GeoFx::G::HeldState geo;
			m.SetGeometrySource(&geo);
			m.SetSourceIdMode(SidGate::Mon::SourceIdMode::On);
			const SidGate::Outcome o = SidGate::Script(m, geo, 10.0);
			Assert::AreEqual(0.5, o.fx, 1e-9);
			// The chain now reads native (a menu): it no longer contradicts native, so the classical safety veto applies again and the consensus
			// follows.
			uint64_t seq = SidGate::Feed(m, geo, 16.7, 32, o.nextSeq, false);
			SourceIdentity::Record p14;
			Assert::IsTrue(m.SourceIdRecord(p14) && p14.ratioH == 1.0f && p14.ratioV == 1.0f, L"fixture: the consensus adopted native");
			m.InjectResultForTest(SidGate::CleanSnapshot(3, false), seq - 1);
			std::string why;
			double fx = o.fx, fy = o.fy;
			m.AutoFactors(3840, 2160, 0.8, why, fx, fy, 33.0, seq);
			Assert::AreEqual(1.0, fx, 1e-9);
			Assert::AreEqual(1.0, fy, 1e-9);
			Assert::IsTrue(why.find("at once") != std::string::npos, L"the original revert path: the tracker reads native, so it is immediate");
		}

		TEST_METHOD(ShadowIsByteIdenticalToOffAndOnEngagesOnTheSameScript)
		{
			SidGate::Mon off, shadow, on;
			GeoFx::G::HeldState geoOff, geoShadow, geoOn;
			off.SetGeometrySource(&geoOff);
			shadow.SetGeometrySource(&geoShadow);
			on.SetGeometrySource(&geoOn);
			off.SetSourceIdMode(SidGate::Mon::SourceIdMode::Off);
			shadow.SetSourceIdMode(SidGate::Mon::SourceIdMode::Shadow);
			on.SetSourceIdMode(SidGate::Mon::SourceIdMode::On);
			const SidGate::Outcome a = SidGate::Script(off, geoOff, 10.0);
			const SidGate::Outcome b = SidGate::Script(shadow, geoShadow, 10.0);
			const SidGate::Outcome c = SidGate::Script(on, geoOn, 10.0);
			Assert::IsTrue(a.fx == b.fx && a.fy == b.fy && a.why1 == b.why1 && a.why2 == b.why2, L"Shadow == Off, factors and decision texts");
			Assert::AreEqual(1.0, a.fx, 1e-9);
			Assert::AreEqual(0.5, c.fx, 1e-9, L"the same script engages under On: the equality above is not vacuous");
			Assert::IsTrue(off.NativeFramesHeldByChain() == 0 && shadow.NativeFramesHeldByChain() == 0, L"N2 is not active in Off or Shadow");
			SourceIdentity::Record rec;
			Assert::IsTrue(shadow.SourceIdRecord(rec) && rec.ratioH == 0.5f && (rec.flags & SourceIdentity::kFlagShadow) != 0,
				L"the would-be is computed and published, flagged as shadow");
			Assert::IsFalse(off.SourceIdRecord(rec), L"off publishes nothing");
			Assert::IsFalse(shadow.SourceIdGeometryFallback().engaged || off.SourceIdGeometryFallback().engaged, L"the fallback is the On path's");
			Assert::AreEqual(std::string("Source identity: off"), off.SourceIdStatusLine(11.0));
			Assert::AreEqual(std::string("Source identity: shadow (classical decides)"), shadow.SourceIdStatusLine(11.0));
		}

		TEST_METHOD(AConfidentChainStopsNativeLookingFramesFromVetoingAndClearing)
		{
			SidGate::Mon on, off;
			GeoFx::G::HeldState geoOn, geoOff;
			on.SetGeometrySource(&geoOn);
			off.SetGeometrySource(&geoOff);
			on.SetSourceIdMode(SidGate::Mon::SourceIdMode::On);
			off.SetSourceIdMode(SidGate::Mon::SourceIdMode::Off);
			SidGate::Feed(on, geoOn, 10.0, 16, 2, true);
			SidGate::Feed(off, geoOff, 10.0, 16, 2, true);
			Assert::IsFalse(on.Read().frameBlank, L"fixture: the textured tiles are informative native-looking frames");
			SourceIdentity::Record p14;
			Assert::IsTrue(on.SourceIdRecord(p14) && p14.ratioH == 0.5f, L"the classical safety veto did not pin the consensus native: the chain value was adopted");
			Assert::IsTrue(on.NativeFramesHeldByChain() > 0, L"the trackers left the contradicted native-looking frames out (counted)");
			Assert::IsTrue(off.NativeFramesHeldByChain() == 0, L"Off: the original trackers");
		}

		TEST_METHOD(ANativeLookingFrameIsEvidenceAgainWhenTheChainIsUnsureOrNative)
		{
			SidGate::Mon m;
			GeoFx::G::HeldState geo;
			m.SetGeometrySource(&geo);
			m.SetSourceIdMode(SidGate::Mon::SourceIdMode::On);
			// The chain reads native (confident native contradicts nothing): the trackers see every informative frame.
			SidGate::Feed(m, geo, 10.0, 12, 2, false);
			Assert::IsTrue(m.NativeFramesHeldByChain() == 0, L"a native chain reading is no contradiction");
			// An UNSURE record (nothing applied) is no contradiction either.
			GeoFx::G::HeldState unsure;
			SidGate::Mon u;
			u.SetGeometrySource(&unsure);
			u.SetSourceIdMode(SidGate::Mon::SourceIdMode::On);
			uint64_t seq = 2;
			for (int i = 0; i < 12; ++i)
			{
				if (i % 2 == 0)
					unsure.Publish(GeoFx::MakeRecord(seq, SidGate::Us(10.0 + 0.5 * i - 0.1), GeoFx::G::AxisGeom(), GeoFx::G::AxisGeom()));
				SidGate::Analyse(u, 10.0 + 0.5 * i, seq++);
			}
			Assert::IsTrue(u.NativeFramesHeldByChain() == 0, L"UNSURE: the original rules");
		}

		TEST_METHOD(TheGeometryFallbackIsVisibleWhileConsensusIsOnAndTheRecordIsUnusable)
		{
			SidGate::Mon m;
			m.SetSourceIdMode(SidGate::Mon::SourceIdMode::On);
			std::string why;
			double fx = 1.0, fy = 1.0;
			m.AutoFactors(3840, 2160, 0.8, why, fx, fy, 10.0, 1);
			Assert::IsTrue(m.SourceIdGeometryFallback().engaged && m.SourceIdGeometryFallback().id == "product.source_geometry");
			Assert::IsTrue(m.SourceIdGeometryFallback().reason.find("absent") != std::string::npos, L"no estimator wired: a record that is not there");
			Assert::AreEqual(1.0, fx);
			Assert::IsTrue(m.SourceIdStatusLine(10.0).find("classical (chain:") == std::string::npos, L"no source set: the base line is unchanged");
			GeoFx::G::HeldState geo;
			GeoFx::G::Record g = SidGate::Upscaled(1, 0.0);
			g.maxAgeUs = 1000000;
			geo.Publish(g);
			m.SetGeometrySource(&geo);
			m.AutoFactors(3840, 2160, 0.8, why, fx, fy, 100.0, 2);
			Assert::IsTrue(m.SourceIdGeometryFallback().engaged && m.SourceIdGeometryFallback().reason.find("stale") != std::string::npos);
			Assert::IsTrue(m.SourceIdStatusLine(100.0).find("classical (chain: stale)") != std::string::npos, L"and the status says why the classical voters decide");
			m.SetSourceIdMode(SidGate::Mon::SourceIdMode::Off);
			m.AutoFactors(3840, 2160, 0.8, why, fx, fy, 101.0, 3);
			Assert::IsFalse(m.SourceIdGeometryFallback().engaged, L"off releases it");
		}

		TEST_METHOD(TheOriginalPathwayForcesSourceIdentityOffWhateverWasRequested)
		{
			SidGate::Mon m;
			GeoFx::G::HeldState geo;
			m.SetGeometrySource(&geo);
			m.SetOriginalPathway(true);
			m.SetSourceIdMode(SidGate::Mon::SourceIdMode::On);
			Assert::IsTrue(m.RequestedSourceIdMode() == SidGate::Mon::SourceIdMode::On, L"the request is kept");
			Assert::IsTrue(m.GetSourceIdMode() == SidGate::Mon::SourceIdMode::Off && !m.SourceIdConsensus() && !m.SourceIdRunning());
			Assert::AreEqual(std::string("Source identity: off (Original pathway)"), m.SourceIdStatusLine(10.0));
			const SidGate::Outcome o = SidGate::Script(m, geo, 10.0);
			Assert::AreEqual(1.0, o.fx, 1e-9);
			Assert::AreEqual(1.0, o.fy, 1e-9);
			SourceIdentity::Record rec;
			Assert::IsFalse(m.SourceIdRecord(rec), L"no pipeline ran");
			Assert::IsFalse(m.SourceIdGeometryFallback().engaged, L"no fallback");
			Assert::IsTrue(m.NativeFramesHeldByChain() == 0);
			m.SetOriginalPathway(false);
			Assert::IsTrue(m.SourceIdConsensus(), L"leaving the Original pathway the requested mode takes effect again");
		}
	};
}
