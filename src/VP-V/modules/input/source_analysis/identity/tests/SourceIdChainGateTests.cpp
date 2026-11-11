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
#include <VP-V/modules/input/source_analysis/identity/chain/ChainEstimator.h>

#include <cstdint>
#include <cstdio>
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

		// SID r2 N1: the chain latch released by its no-cut CEILING (no cut, black or fade within 60 s of its starvation bound): the native
		// switch the consensus then asks for is at once but shown through the engage CROSSFADE (1.5 s), not the 0.25 s release fade; the
		// cue is used once.
		TEST_METHOD(ANoCutCeilingReleaseOfTheLatchRevertsThroughTheCrossfade)
		{
			SidGate::Mon m;
			GeoFx::G::HeldState geo;
			m.SetGeometrySource(&geo);
			m.SetSourceIdMode(SidGate::Mon::SourceIdMode::On);
			m.ChainEstimatorRef().SetSafetyRules(true);   // N2: the cue is used only while the rules are on (it only exists then)
			const SidGate::Outcome o = SidGate::Script(m, geo, 10.0);
			Assert::AreEqual(0.5, o.fx, 1e-9);
			Assert::AreEqual<uint64_t>(0, m.ChainEstimatorRef().LatchCeilingReleases(), L"fixture: no ceiling release yet");
			m.ChainEstimatorRef().NoteLatchCeilingForTest();   // the latch's ReleasedCeiling, as FoldLatchCounters counts it
			uint64_t seq = SidGate::Feed(m, geo, 16.7, 32, o.nextSeq, false);
			SourceIdentity::Record p14;
			Assert::IsTrue(m.SourceIdRecord(p14) && p14.ratioH == 1.0f && p14.ratioV == 1.0f, L"fixture: the consensus adopted native");
			m.InjectResultForTest(SidGate::CleanSnapshot(3, false), seq - 1);
			std::string why;
			double fx = o.fx, fy = o.fy;
			m.AutoFactors(3840, 2160, 0.8, why, fx, fy, 33.0, seq);
			Assert::AreEqual(1.0, fx, 1e-9, L"native at once");
			Assert::AreEqual(1.0, fy, 1e-9);
			Assert::IsTrue(why.find("crossfade") != std::string::npos && why.find("ceiling") != std::string::npos, (L"named as the ceiling's crossfade: " + std::wstring(why.begin(), why.end())).c_str());
			const SidGate::Mon::Transition tr = m.AutoTransition(33.0);
			Assert::AreEqual(1.5, tr.duration, 1e-9, L"the engage crossfade (source_resolution_crossfade_s), not the release fade");
			Assert::AreEqual(0.5, tr.fromX, 1e-9, L"faded from the upscaled path");
			Assert::AreEqual(1.0, tr.toX, 1e-9);
			Assert::IsFalse(m.LatchCeilingCuePendingForTest(), L"the cue is used once");
		}

		// SID r2 N2: the ceiling's crossfade cue never lingers. It is decided by the FIRST fresh gate record after it: a record that still
		// reads the upscale drops it (a later unrelated native switch takes its own path, not the crossfade); and it is dropped at once
		// when no fresh gate is in use (Shadow) or the safety rules are off. Re-reading the record seen before the cue keeps it.
		TEST_METHOD(TheCeilingCueIsDroppedByTheFirstFreshRecordAfterItOrWithoutAFreshGate)
		{
			{
				SidGate::Mon m;
				GeoFx::G::HeldState geo;
				m.SetGeometrySource(&geo);
				m.SetSourceIdMode(SidGate::Mon::SourceIdMode::On);
				m.ChainEstimatorRef().SetSafetyRules(true);
				const SidGate::Outcome o = SidGate::Script(m, geo, 10.0);
				Assert::AreEqual(0.5, o.fx, 1e-9);
				m.ChainEstimatorRef().NoteLatchCeilingForTest();
				std::string why;
				double fx = o.fx, fy = o.fy;
				m.InjectResultForTest(SidGate::CleanSnapshot(3, false), o.nextSeq - 1);
				m.AutoFactors(3840, 2160, 0.8, why, fx, fy, 16.7, o.nextSeq);
				Assert::IsTrue(m.LatchCeilingCuePendingForTest(), L"the record seen before the cue does not decide it");
				Assert::AreEqual(0.5, fx, 1e-9);
				// New evaluations still reading the upscale: the first fresh record after the cue drops it.
				uint64_t seq = SidGate::Feed(m, geo, 16.8, 4, o.nextSeq, true);
				m.InjectResultForTest(SidGate::CleanSnapshot(4, false), seq - 1);
				why.clear();
				m.AutoFactors(3840, 2160, 0.8, why, fx, fy, 18.4, seq);
				Assert::IsFalse(m.LatchCeilingCuePendingForTest(), L"dropped by the first fresh record after it (still upscaled)");
				Assert::AreEqual(0.5, fx, 1e-9, L"nothing else changed");
				// A later, unrelated native switch: the original revert path, not the ceiling's crossfade.
				seq = SidGate::Feed(m, geo, 18.5, 32, seq, false);
				SourceIdentity::Record p14;
				Assert::IsTrue(m.SourceIdRecord(p14) && p14.ratioH == 1.0f && p14.ratioV == 1.0f, L"fixture: the consensus adopted native");
				m.InjectResultForTest(SidGate::CleanSnapshot(5, false), seq - 1);
				why.clear();
				m.AutoFactors(3840, 2160, 0.8, why, fx, fy, 35.0, seq);
				Assert::AreEqual(1.0, fx, 1e-9);
				Assert::IsTrue(why.find("ceiling") == std::string::npos && why.find("at once") != std::string::npos,
					(L"the ordinary revert, not the stale cue's crossfade: " + std::wstring(why.begin(), why.end())).c_str());
				Assert::IsTrue(m.AutoTransition(35.0).duration < 1.5, L"not the engage crossfade");
			}
			for (int variant = 0; variant < 2; ++variant)
			{
				// 0: Shadow (no fresh gate in use); 1: the safety rules turned off.
				SidGate::Mon m;
				GeoFx::G::HeldState geo;
				m.SetGeometrySource(&geo);
				m.SetSourceIdMode(SidGate::Mon::SourceIdMode::On);
				m.ChainEstimatorRef().SetSafetyRules(true);
				const SidGate::Outcome o = SidGate::Script(m, geo, 10.0);
				Assert::AreEqual(0.5, o.fx, 1e-9);
				m.ChainEstimatorRef().NoteLatchCeilingForTest();
				if (variant == 0)
					m.SetSourceIdMode(SidGate::Mon::SourceIdMode::Shadow);
				else
					m.ChainEstimatorRef().SetSafetyRules(false);
				std::string why;
				double fx = o.fx, fy = o.fy;
				m.InjectResultForTest(SidGate::CleanSnapshot(3, false), o.nextSeq - 1);
				m.AutoFactors(3840, 2160, 0.8, why, fx, fy, 16.7, o.nextSeq);
				Assert::IsFalse(m.LatchCeilingCuePendingForTest(), variant == 0 ? L"Shadow: dropped at once" : L"rules off: dropped at once");
				Assert::IsTrue(why.find("ceiling") == std::string::npos, L"and never used");
			}
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

		// S2 (round 1): the hold reads the latest SEALED record with evalSeq <= the frame being analysed (the P14g contract), never a newer one.
		TEST_METHOD(ARecordSealedAfterTheFrameNeverHoldsTheTrackers)
		{
			SidGate::Mon m;
			GeoFx::G::HeldState geo;
			m.SetGeometrySource(&geo);
			m.SetSourceIdMode(SidGate::Mon::SourceIdMode::On);
			geo.Publish(SidGate::Upscaled(1000, 15.9));   // sealed for frame 1000: frames 2..13 are of its past
			uint64_t seq = 2;
			for (int i = 0; i < 12; ++i)
				SidGate::Analyse(m, 10.0 + 0.5 * i, seq++);
			Assert::IsTrue(m.NativeFramesHeldByChain() == 0, L"a record from after these frames is no contradiction of them");
			for (int i = 0; i < 4; ++i)
				SidGate::Analyse(m, 16.0 + 0.5 * i, 1000 + static_cast<uint64_t>(i));
			Assert::IsTrue(m.NativeFramesHeldByChain() > 0, L"frames at or after its sequence do see it (fixture)");
		}

		// S3 (round 1): a confident axis holds only its own tracker (and the combined one).
		TEST_METHOD(AConfidentAxisHoldsOnlyItsOwnTracker)
		{
			auto held = [](bool h, bool v) {
				SidGate::Mon m;
				GeoFx::G::HeldState geo;
				m.SetGeometrySource(&geo);
				m.SetSourceIdMode(SidGate::Mon::SourceIdMode::On);
				uint64_t seq = 2;
				for (int i = 0; i < 12; ++i)
				{
					const double t = 10.0 + 0.5 * i;
					if (i % 2 == 0)
						geo.Publish(GeoFx::MakeRecord(seq, SidGate::Us(t - 0.1), h ? GeoFx::AppliedAxis(1920, 3840, 5) : GeoFx::G::AxisGeom(),
							v ? GeoFx::AppliedAxis(1080, 2160, 5) : GeoFx::G::AxisGeom()));
					SidGate::Analyse(m, t, seq++);
				}
				return m.NativeFramesHeldByChain();
			};
			const unsigned long long both = held(true, true), hOnly = held(true, false), vOnly = held(false, true), neither = held(false, false);
			Assert::IsTrue(neither == 0, L"no confident axis: nothing is held");
			Assert::IsTrue(hOnly > 0 && vOnly > 0, L"one confident axis holds its own tracker");
			Assert::IsTrue(hOnly < both && vOnly < both, L"and not the other axis's tracker (two of three trackers, not three)");
		}

		// S1 (a) (round 1): the hold lasts only while the record is fresh by ITS OWN maximum age.
		TEST_METHOD(AStaleChainRecordNoLongerHoldsNativeLookingFrames)
		{
			SidGate::Mon m;
			GeoFx::G::HeldState geo;
			m.SetGeometrySource(&geo);
			m.SetSourceIdMode(SidGate::Mon::SourceIdMode::On);
			GeoFx::G::Record r = SidGate::Upscaled(2, 9.9);
			r.maxAgeUs = 1000000;   // stale 1 s after its evaluation
			geo.Publish(r);
			uint64_t seq = 2;
			unsigned long long heldWhileFresh = 0;
			for (int i = 0; i < 12; ++i)
			{
				SidGate::Analyse(m, 10.0 + 0.5 * i, seq++);
				if (i == 1) heldWhileFresh = m.NativeFramesHeldByChain();   // t = 10.5 s: 0.6 s after the evaluation
			}
			Assert::IsTrue(heldWhileFresh > 0, L"fixture: fresh, the confident record holds the frames");
			Assert::IsTrue(m.NativeFramesHeldByChain() == heldWhileFresh, L"stale (past its own max age): the original rules, no frame is held any more");
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

		// SID r2 (L6 / missing test): a consensus reading of ONE axis below native is held (two axes are required unless the single-axis
		// switch is on), and the hold is visible in the status line while it lasts, not only in the debug log.
		TEST_METHOD(ASingleAxisRebaseIsHeldAndVisibleInTheStatus)
		{
			auto run = [](bool singleAxisSwitch, double& fx, double& fy) {
				SidGate::Mon m;
				GeoFx::G::HeldState geo;
				m.SetGeometrySource(&geo);
				m.SetSourceIdMode(SidGate::Mon::SourceIdMode::On);
				m.SetSingleAxisRebase(singleAxisSwitch);
				uint64_t seq = 2;
				for (int i = 0; i < 13; ++i)
				{
					const double t = 10.0 + 0.5 * i;
					if (i % 2 == 0)   // applied H 1920 -> 3840, V native
						geo.Publish(GeoFx::MakeRecord(seq, SidGate::Us(t - 0.1), GeoFx::AppliedAxis(1920, 3840, 5), GeoFx::AppliedAxis(2160, 2160, -1)));
					SidGate::Analyse(m, t, seq++);
				}
				std::string why;
				fx = fy = 1.0;
				m.InjectResultForTest(SidGate::CleanSnapshot(1, false), seq - 1);
				m.AutoFactors(3840, 2160, 0.8, why, fx, fy, 16.1, seq);
				m.InjectResultForTest(SidGate::CleanSnapshot(2, true), seq);
				m.AutoFactors(3840, 2160, 0.8, why, fx, fy, 16.6, seq + 1);
				return m.SourceIdStatusLine(16.7);
			};
			double fx = 0.0, fy = 0.0;
			const std::string held = run(false, fx, fy);
			Assert::AreEqual(1.0, fx, 1e-9, L"one axis below native: not applied (two axes are required)");
			Assert::AreEqual(1.0, fy, 1e-9);
			Assert::IsTrue(held.find("single-axis re-base held (H 0.5000") != std::string::npos,
				std::wstring(L"the hold is in the status line: ").append(held.begin(), held.end()).c_str());
			const std::string on = run(true, fx, fy);
			Assert::IsTrue(on.find("single-axis re-base held") == std::string::npos, L"the switch on: nothing is held, nothing shown");
			Assert::AreEqual(0.5, fx, 1e-9, L"the switch on: the single-axis re-base is applied");
			Assert::AreEqual(1.0, fy, 1e-9);
		}
	};

	// SID r2 (owner 2026-11-11): source_resolution auto re-bases ONLY a two-axis conventional pair (one listed, non-parked
	// Scaling::GeometrySet geometry on the delivered raster, tolerance 0.6 %), on the classical path (consensus off or no fresh record)
	// and through the consensus alike. A single-axis reading (unless the single-axis switch is on) or a size outside the list is held:
	// logged and shown (Snapshot::conventionNote -> Describe -> Ctrl+I and the Config status line), never applied. Listed two-axis
	// sizes, natives and the Original pathway: exactly as before (the same trace as with the restriction switched off).
	namespace SidConv
	{
		typedef SourceResolutionMonitor::Monitor Mon;
		typedef SourceResolutionMonitor::Snapshot Snap;
		constexpr int W = 1920, H = 1080;   // the delivered raster (test fixture)

		inline Snap Uniform(double r)
		{
			Snap s;
			s.reading = true;
			s.nativeWidth = W;
			s.nativeHeight = H;
			s.declaredRatio = s.liveRatio = s.frameRatio = r;
			s.confidence = r < 1.0 ? 0.9 : 0.0;
			s.frames = 1;
			return s;
		}

		inline Snap PerAxis(double rx, double ry)
		{
			Snap s = Uniform(1.0);
			s.axisRatioX = rx;
			s.axisRatioY = ry;
			s.axisConfidence = 0.9;
			return s;
		}

		struct Run
		{
			double fx = 1.0, fy = 1.0;
			std::string trace, note, shown, hidden;
		};

		// The classical path on the frame-bound source clock: one scripted reading, then 8 decisions 62.5 ms apart (a 50 ms wait:
		// a re-base that is applied engages on the second). The trace is every factor, fade and reason a viewer could see.
		inline Run Drive(Mon& m, const Snap& s, int rasterW = W, int rasterH = H)
		{
			m.SetEngageTiming(0.05, 0.125, 0.05);
			m.SetFrameBound(true, 0);
			m.InjectResultForTest(s, 1);
			Run r;
			for (uint64_t seq = 1; seq <= 8; ++seq)
			{
				std::string why;
				double fx = 1.0, fy = 1.0;
				const double t = 100.0 + 0.0625 * static_cast<double>(seq - 1);
				m.AutoFactors(rasterW, rasterH, 0.5, why, fx, fy, t, seq);
				const Mon::Transition tr = m.AutoTransition(t);
				char line[400];
				std::snprintf(line, sizeof(line), "%llu f=%.6f,%.6f tr=%.6f,%.6f>%.6f,%.6f w=%.6f why=%s\n", static_cast<unsigned long long>(seq),
					fx, fy, tr.fromX, tr.fromY, tr.toX, tr.toY, tr.weight, why.c_str());
				r.trace += line;
				r.fx = fx;
				r.fy = fy;
			}
			const Snap now = m.Read();
			r.note = now.conventionNote;
			r.shown = SourceResolutionMonitor::Describe(now, m.AutoRatio(), true);
			r.hidden = SourceResolutionMonitor::Describe(now, m.AutoRatio(), false);
			return r;
		}

		inline Run DriveFresh(const Snap& s, bool conventionOnly, bool singleAxis = false, bool original = false)
		{
			Mon m;
			m.SetRebaseConventionOnly(conventionOnly);
			m.SetSingleAxisRebase(singleAxis);
			m.SetOriginalPathway(original);
			return Drive(m, s);
		}

		inline bool Has(const std::string& s, const char* what) { return s.find(what) != std::string::npos; }
		inline std::wstring W16(const std::string& s) { return std::wstring(s.begin(), s.end()); }
	}

	TEST_CLASS(SourceResolutionConventionTests)
	{
	public:
		TEST_METHOD(AListedTwoAxisSizeIsReBasedExactlyAsBefore)
		{
			// 960x540 on 1080p (uniform 1/2) and 720x480 at 16:9 on 1080p (0.375 x 0.4444, the classical per-axis reading).
			const SidConv::Run u = SidConv::DriveFresh(SidConv::Uniform(0.5), true);
			const SidConv::Run u0 = SidConv::DriveFresh(SidConv::Uniform(0.5), false);
			Assert::AreEqual(0.5, u.fx, 1e-9, SidConv::W16(u.trace).c_str());
			Assert::AreEqual(0.5, u.fy, 1e-9);
			Assert::AreEqual(u0.trace, u.trace, L"byte-identical with the restriction switched off");
			Assert::IsTrue(u.note.empty() && !SidConv::Has(u.shown, "re-base held"), L"nothing is held, nothing shown");
			const SidConv::Run p = SidConv::DriveFresh(SidConv::PerAxis(720.0 / 1920.0, 480.0 / 1080.0), true);
			const SidConv::Run p0 = SidConv::DriveFresh(SidConv::PerAxis(720.0 / 1920.0, 480.0 / 1080.0), false);
			Assert::AreEqual(0.375, p.fx, 1e-9, SidConv::W16(p.trace).c_str());
			Assert::AreEqual(480.0 / 1080.0, p.fy, 1e-9);
			Assert::AreEqual(p0.trace, p.trace, L"byte-identical with the restriction switched off");
			Assert::IsTrue(p.note.empty());
		}

		TEST_METHOD(ANonStandardUniformSizeIsHeldAndShownNeverApplied)
		{
			// 0.7 on 1080p = 1344x756: no listed size (1280x720 is 2/3, outside the 0.6 % tolerance).
			const SidConv::Run r = SidConv::DriveFresh(SidConv::Uniform(0.7), true);
			Assert::AreEqual(1.0, r.fx, 1e-9, SidConv::W16(r.trace).c_str());
			Assert::AreEqual(1.0, r.fy, 1e-9);
			Assert::IsTrue(SidConv::Has(r.note, "re-base held (non-standard size H 0.7000 x V 0.7000"), SidConv::W16(r.note).c_str());
			Assert::IsTrue(SidConv::Has(r.shown, r.note.c_str()), L"shown on the auto status (Ctrl+I and the Config status line)");
			Assert::IsFalse(SidConv::Has(r.hidden, "re-base held"), L"not shown while auto is not selected");
			const SidConv::Run off = SidConv::DriveFresh(SidConv::Uniform(0.7), false);
			Assert::AreEqual(756.0 / 1080.0, off.fx, 1e-9, L"fixture: the restriction off applies it (the earlier behaviour)");
			Assert::IsTrue(off.note.empty());
		}

		TEST_METHOD(AClassicalSingleAxisReadingIsHeldUnlessTheSwitchIsOn)
		{
			const SidConv::Run r = SidConv::DriveFresh(SidConv::PerAxis(0.75, 1.0), true);
			Assert::AreEqual(1.0, r.fx, 1e-9, SidConv::W16(r.trace).c_str());
			Assert::AreEqual(1.0, r.fy, 1e-9);
			// Owner 2026-11-11: H 0.75 on a 1080p raster is the named parked HDV entry, held by its name (was the generic single-axis text).
			Assert::IsTrue(SidConv::Has(r.note, "held: anamorphic HDV 1440x1080 (single-axis re-base parked; H 0.7500"), SidConv::W16(r.note).c_str());
			Assert::IsTrue(SidConv::Has(r.shown, r.note.c_str()));
			const SidConv::Run v = SidConv::DriveFresh(SidConv::PerAxis(1.0, 0.75), true);
			Assert::AreEqual(1.0, v.fy, 1e-9);
			Assert::IsTrue(SidConv::Has(v.note, "single-axis re-base held (V 0.7500"), SidConv::W16(v.note).c_str());
			const SidConv::Run on = SidConv::DriveFresh(SidConv::PerAxis(0.75, 1.0), true, true);
			Assert::AreEqual(0.75, on.fx, 1e-9, SidConv::W16(on.trace).c_str());
			Assert::AreEqual(1.0, on.fy, 1e-9);
			Assert::IsTrue(on.note.empty(), L"the single-axis switch on: applied, nothing held");
		}

		TEST_METHOD(ANonStandardPerAxisPairIsHeld)
		{
			const SidConv::Run r = SidConv::DriveFresh(SidConv::PerAxis(0.7, 0.6), true);
			Assert::AreEqual(1.0, r.fx, 1e-9, SidConv::W16(r.trace).c_str());
			Assert::AreEqual(1.0, r.fy, 1e-9);
			Assert::IsTrue(SidConv::Has(r.note, "non-standard size H 0.7000 x V 0.6000"), SidConv::W16(r.note).c_str());
			const SidConv::Run off = SidConv::DriveFresh(SidConv::PerAxis(0.7, 0.6), false);
			Assert::IsTrue(off.fx < 1.0 && off.fy < 1.0, L"fixture: the restriction off applies it");
		}

		TEST_METHOD(TheOriginalPathwayAndNativesAreUntouched)
		{
			const SidConv::Run o = SidConv::DriveFresh(SidConv::Uniform(0.7), true, false, true);
			const SidConv::Run o0 = SidConv::DriveFresh(SidConv::Uniform(0.7), false, false, true);
			Assert::AreEqual(o0.trace, o.trace, L"the Original pathway: no convention check (byte-identical)");
			Assert::AreEqual(756.0 / 1080.0, o.fx, 1e-9, SidConv::W16(o.trace).c_str());
			Assert::IsTrue(o.note.empty());
			const SidConv::Run op = SidConv::DriveFresh(SidConv::PerAxis(0.75, 1.0), true, false, true);
			const SidConv::Run op0 = SidConv::DriveFresh(SidConv::PerAxis(0.75, 1.0), false, false, true);
			Assert::AreEqual(op0.trace, op.trace, L"the Original pathway: the single-axis reading as before");
			Assert::IsTrue(op.note.empty());
			const SidConv::Run n = SidConv::DriveFresh(SidConv::Uniform(1.0), true);
			const SidConv::Run n0 = SidConv::DriveFresh(SidConv::Uniform(1.0), false);
			Assert::AreEqual(n0.trace, n.trace, L"natives: byte-identical");
			Assert::AreEqual(1.0, n.fx, 1e-9);
			Assert::IsTrue(n.note.empty() && !SidConv::Has(n.shown, "re-base held"));
		}

		// MED SD (owner 2026-11-11): 720x576 / 720x480 shown at 16:9 on 1080p re-base ONLY through the per-axis reading matching the
		// listed pair (0.375 x lines/1080). The classical UNIFORM misread (lines/1080 on both axes: 1024x576 / 853x480, not listed) is
		// held as non-standard and never applied, also when a per-axis reading of another size accompanies it.
		TEST_METHOD(SdReBasesOnlyAsItsListedPerAxisPair)
		{
			for (int lines : { 576, 480 })
			{
				const double rv = lines / 1080.0;
				const SidConv::Run u = SidConv::DriveFresh(SidConv::Uniform(rv), true);
				Assert::AreEqual(1.0, u.fx, 1e-9, SidConv::W16(u.trace).c_str());
				Assert::AreEqual(1.0, u.fy, 1e-9);
				Assert::IsTrue(SidConv::Has(u.note, "re-base held (non-standard size"), SidConv::W16(u.note).c_str());
				Assert::IsTrue(SidConv::Has(u.trace, "f=1.000000,1.000000") && !SidConv::Has(u.trace, "tr=1.000000,1.000000>0."),
					L"the uniform misread is never applied, not even for one frame");
				SidConv::Snap both = SidConv::Uniform(rv);
				both.axisRatioX = 720.0 / 1920.0;
				both.axisRatioY = rv;
				both.axisConfidence = 0.9;
				const SidConv::Run p = SidConv::DriveFresh(both, true);
				Assert::AreEqual(0.375, p.fx, 1e-9, SidConv::W16(p.trace).c_str());
				Assert::AreEqual(rv, p.fy, 1e-9);
				Assert::IsTrue(p.note.empty(), SidConv::W16(p.note).c_str());
				SidConv::Snap other = both;
				other.axisRatioX = 0.45;   // 864 columns: not the SD pair
				const SidConv::Run o = SidConv::DriveFresh(other, true);
				Assert::AreEqual(1.0, o.fx, 1e-9, SidConv::W16(o.trace).c_str());
				Assert::AreEqual(1.0, o.fy, 1e-9);
				Assert::IsTrue(SidConv::Has(o.note, "re-base held (non-standard size"), SidConv::W16(o.note).c_str());
			}
		}

		// F3: the Original pathway shows no held note (also one left from before the switch), and its fallback is released.
		TEST_METHOD(TheOriginalPathwayShowsNoStaleNote)
		{
			SidConv::Mon m;
			const SidConv::Run r = SidConv::Drive(m, SidConv::Uniform(0.7));
			Assert::IsFalse(r.note.empty(), L"fixture: held while the convention applies");
			Assert::IsTrue(m.ConventionHoldFallback().engaged);
			m.SetOriginalPathway(true);
			Assert::IsTrue(m.Read().conventionNote.empty(), SidConv::W16(m.Read().conventionNote).c_str());
			Assert::IsFalse(m.ConventionHoldFallback().engaged);
		}

		// F3: the consensus's single-axis hold (L6) does not outlive its fresh record, also when no per-axis branch runs.
		TEST_METHOD(TheSingleAxisHoldEndsWhenTheRecordIsNotFresh)
		{
			SidGate::Mon m;
			GeoFx::G::HeldState geo;
			m.SetGeometrySource(&geo);
			m.SetSourceIdMode(SidGate::Mon::SourceIdMode::On);
			uint64_t seq = 2;
			for (int i = 0; i < 13; ++i)
			{
				const double t = 10.0 + 0.5 * i;
				if (i % 2 == 0)
					geo.Publish(GeoFx::MakeRecord(seq, SidGate::Us(t - 0.1), GeoFx::AppliedAxis(720, 3840, 5), GeoFx::AppliedAxis(2160, 2160, -1)));
				SidGate::Analyse(m, t, seq++);
			}
			std::string why;
			double fx = 1.0, fy = 1.0;
			m.InjectResultForTest(SidGate::CleanSnapshot(1, false), seq - 1);
			m.AutoFactors(3840, 2160, 0.8, why, fx, fy, 16.1, seq);
			const std::string before = m.Read().conventionNote;
			Logger::WriteMessage(("fresh: " + before + "\n").c_str());
			// Long after the last record (not fresh) and with no per-axis reading (neither branch runs).
			SidConv::Snap none = SidGate::CleanSnapshot(2, false);
			none.reading = false;
			m.InjectResultForTest(none, seq);
			m.AutoFactors(3840, 2160, 0.8, why, fx, fy, 200.0, seq + 1);
			const std::string after = m.Read().conventionNote;
			Assert::IsFalse(SidConv::Has(after, "single-axis"), SidConv::W16(before + " -> " + after).c_str());
			if (!SidConv::Has(before, "single-axis"))
				Logger::WriteMessage("the consensus did not hold a single axis here (the stale clear is exercised without a prior hold)\n");
		}

		// F4: a raster width <= 0 through AutoFactors holds on the uniform path as on the per-axis path (the list cannot be checked);
		// AutoFactor called alone keeps the earlier law.
		TEST_METHOD(AnUnknownRasterWidthHoldsOnBothPaths)
		{
			SidConv::Mon mu;
			const SidConv::Run u = SidConv::Drive(mu, SidConv::Uniform(0.5), 0);
			Assert::AreEqual(1.0, u.fx, 1e-9, SidConv::W16(u.trace).c_str());
			Assert::AreEqual(1.0, u.fy, 1e-9);
			Assert::IsTrue(SidConv::Has(u.note, "re-base held (non-standard size"), SidConv::W16(u.note).c_str());
			SidConv::Mon mp;
			const SidConv::Run p = SidConv::Drive(mp, SidConv::PerAxis(720.0 / 1920.0, 480.0 / 1080.0), 0);
			Assert::AreEqual(1.0, p.fx, 1e-9, SidConv::W16(p.trace).c_str());
			Assert::AreEqual(1.0, p.fy, 1e-9);
			Assert::IsTrue(SidConv::Has(p.note, "re-base held (non-standard size"), SidConv::W16(p.note).c_str());
			// AutoFactor alone (no raster width): the earlier law, the non-standard 0.7 applied.
			SidConv::Mon m;
			m.SetEngageTiming(0.05, 0.125, 0.05);
			m.SetFrameBound(true, 0);
			m.InjectResultForTest(SidConv::Uniform(0.7), 1);
			double f = 1.0;
			for (uint64_t seq = 1; seq <= 8; ++seq)
			{
				std::string why;
				f = m.AutoFactor(SidConv::H, 0.5, why, 100.0 + 0.0625 * static_cast<double>(seq - 1), seq);
			}
			Assert::AreEqual(756.0 / 1080.0, f, 1e-9, L"AutoFactor alone keeps the earlier law");
		}

		// LOW: the held state has a one-time notice (FallbackCatalog "source resolution held"), released when nothing is held.
		TEST_METHOD(TheHeldStateHasAOneTimeNotice)
		{
			SidConv::Mon m;
			SidConv::Drive(m, SidConv::PerAxis(0.75, 1.0));
			const FallbackCatalog::Decision d = m.ConventionHoldFallback();
			Assert::AreEqual(std::string("source resolution held"), d.id);
			Assert::IsTrue(d.engaged);
			Assert::IsTrue(SidConv::Has(d.reason, "held: anamorphic HDV 1440x1080 (single-axis re-base parked; H 0.7500"), SidConv::W16(d.reason).c_str());   // named (owner 2026-11-11)
			SidConv::Mon n;
			SidConv::Drive(n, SidConv::Uniform(0.5));
			Assert::IsFalse(n.ConventionHoldFallback().engaged, L"a listed size: nothing held, no notice");
		}

		// The consensus On with a fresh record: the same restriction on its latched pair (the uniform wish and the per-axis path).
		TEST_METHOD(TheConsensusPathHasTheSameRestriction)
		{
			struct Out { SidGate::Outcome o; std::string note; bool latched = false; };
			auto run = [](int nx, int ny, bool conventionOnly) {
				Out out;
				SidGate::Mon m;
				GeoFx::G::HeldState geo;
				m.SetGeometrySource(&geo);
				m.SetSourceIdMode(SidGate::Mon::SourceIdMode::On);
				m.SetRebaseConventionOnly(conventionOnly);
				uint64_t seq = 2;
				for (int i = 0; i < 13; ++i)
				{
					const double t = 10.0 + 0.5 * i;
					if (i % 2 == 0)
						geo.Publish(GeoFx::MakeRecord(seq, SidGate::Us(t - 0.1), GeoFx::AppliedAxis(nx, 3840, 5), GeoFx::AppliedAxis(ny, 2160, 5)));
					SidGate::Analyse(m, t, seq++);
				}
				std::string why;
				m.InjectResultForTest(SidGate::CleanSnapshot(1, false), seq - 1);
				m.AutoFactors(3840, 2160, 0.8, why, out.o.fx, out.o.fy, 16.1, seq);
				out.o.why1 = why;
				why.clear();
				m.InjectResultForTest(SidGate::CleanSnapshot(2, true), seq);
				m.AutoFactors(3840, 2160, 0.8, why, out.o.fx, out.o.fy, 16.6, seq + 1);
				out.o.why2 = why;
				out.note = m.Read().conventionNote;
				SourceIdentity::Record rec;
				out.latched = m.SourceIdRecord(rec) && rec.ratioH < 1.0f && rec.ratioV < 1.0f;
				return out;
			};
			// 2560x1440 (2/3) is listed: engaged exactly as with the restriction off.
			const Out c = run(2560, 1440, true), c0 = run(2560, 1440, false);
			Assert::IsTrue(c.latched, L"fixture: the consensus latched the listed pair");
			Assert::AreEqual(1440.0 / 2160.0, c.o.fx, 1e-9, SidConv::W16(c.o.why1 + " | " + c.o.why2).c_str());
			Assert::AreEqual(1440.0 / 2160.0, c.o.fy, 1e-9);
			Assert::AreEqual(c0.o.fx, c.o.fx, 0.0);
			Assert::AreEqual(c0.o.why2, c.o.why2, L"the same reasons as with the restriction off");
			Assert::IsTrue(c.note.empty());
			// 2700x1512 (0.703 x 0.700) is not listed: held, shown, never applied, whether the consensus latched it or refused it upstream.
			const Out n = run(2700, 1512, true), n0 = run(2700, 1512, false);
			Assert::AreEqual(1.0, n.o.fx, 1e-9, SidConv::W16(n.o.why1 + " | " + n.o.why2).c_str());
			Assert::AreEqual(1.0, n.o.fy, 1e-9);
			if (n.latched)
			{
				Logger::WriteMessage("the consensus latched the non-listed pair; the Monitor held it\n");
				Assert::IsTrue(SidConv::Has(n.note, "re-base held (non-standard size"), SidConv::W16(n.note).c_str());
				Assert::IsTrue(n0.o.fx < 1.0, L"fixture: the restriction off applies the latched pair");
			}
			else
				Logger::WriteMessage("the consensus did not latch the non-listed pair (refused upstream); nothing applied\n");
		}
	};

	// SID r2 (owner 2026-11-11 ~10:20 / ~10:25): named anamorphic HD entries, parked (detect on, apply off), and the detection setting
	// source_geometry_detect. Recognition names and counts a hold; it never changes what is applied.
	namespace SidNamed
	{
		namespace G = Scaling::GeometrySet;

		inline SidConv::Snap PerAxisOn(double rx, double ry, int w, int h)
		{
			SidConv::Snap s = SidConv::PerAxis(rx, ry);
			s.nativeWidth = w;
			s.nativeHeight = h;
			return s;
		}

		// The factor part of a trace (the reasons may carry the note text): what a viewer gets applied.
		inline std::string Factors(const std::string& trace)
		{
			std::string out, line;
			for (char ch : trace)
			{
				if (ch != '\n') { line += ch; continue; }
				const std::size_t w = line.find(" why=");
				out += line.substr(0, w) + "\n";
				line.clear();
			}
			return out;
		}

		inline bool AllNative(const std::string& trace)
		{
			std::size_t pos = 0, lines = 0;
			while ((pos = trace.find(" f=", pos)) != std::string::npos)
			{
				if (trace.compare(pos, 21, " f=1.000000,1.000000 ") != 0)
					return false;
				++pos;
				++lines;
			}
			return lines == 8;
		}

		inline const G::Geometry* Entry(const char* id)
		{
			std::size_t count = 0;
			const G::Geometry* g = G::Geometries(count);
			for (std::size_t i = 0; i < count; ++i)
				if (std::string(id) == g[i].id)
					return &g[i];
			return nullptr;
		}

		inline SidConv::Run DriveDetect(const SidConv::Snap& s, int w, int h, const char* detect, SidConv::Mon& m)
		{
			m.SetRebaseConventionOnly(true);
			if (detect != nullptr)
				m.SetGeometryDetect(detect);
			return SidConv::Drive(m, s, w, h);
		}
	}

	TEST_CLASS(SourceGeometryNamedHoldTests)
	{
	public:
		TEST_METHOD(EachNamedAnamorphicEntryIsRecognisedAndHeldByItsName)
		{
			const char* ids[] = { "hdv_1440x1080", "dvcprohd_1280x1080", "dvcprohd_960x720" };
			const int rasters[][2] = { { 1920, 1080 }, { 3840, 2160 } };
			for (const char* id : ids)
				for (const auto& ra : rasters)
				{
					const Scaling::GeometrySet::Geometry* e = SidNamed::Entry(id);
					Assert::IsNotNull(e, L"the entry is in the table");
					Assert::IsTrue(e->detect && !e->apply, L"parked: detect on, apply off");
					double rx = 0.0, ry = 0.0;
					Assert::IsTrue(Scaling::GeometrySet::RatiosFor(*e, ra[0], ra[1], rx, ry), L"placed on the delivered raster");
					SidConv::Mon m;
					const SidConv::Run r = SidNamed::DriveDetect(SidNamed::PerAxisOn(rx, ry, ra[0], ra[1]), ra[0], ra[1], nullptr, m);
					char msg[400];
					std::snprintf(msg, sizeof(msg), "%s on %dx%d (H %.4f x V %.4f): note '%s'\n", id, ra[0], ra[1], rx, ry, r.note.c_str());
					Logger::WriteMessage(msg);
					const std::wstring w = SidConv::W16(msg);
					Assert::IsTrue(SidNamed::AllNative(r.trace), (w + SidConv::W16(r.trace)).c_str());
					Assert::IsTrue(SidConv::Has(r.note, (std::string("held: ") + e->name + " (").c_str()) && SidConv::Has(r.note, "re-base parked"), w.c_str());
					Assert::IsTrue(SidConv::Has(r.shown, r.note.c_str()), L"shown on the auto status");
					Assert::AreEqual(1u, m.NamedHoldCount(id), (L"one episode counted once: " + w).c_str());
					Assert::IsTrue(m.ConventionHoldFallback().engaged && SidConv::Has(m.ConventionHoldFallback().reason, e->name), w.c_str());
					// The same reading with recognition narrowed to the conventional entries: the same factors, a generic hold, no count.
					SidConv::Mon n;
					const SidConv::Run g = SidNamed::DriveDetect(SidNamed::PerAxisOn(rx, ry, ra[0], ra[1]), ra[0], ra[1], "conventional", n);
					Assert::AreEqual(SidNamed::Factors(r.trace), SidNamed::Factors(g.trace), (L"apply byte-identical: " + w).c_str());
					Assert::IsFalse(SidConv::Has(g.note, "held: "), SidConv::W16(g.note).c_str());
					Assert::IsTrue(SidConv::Has(g.note, "re-base held"), SidConv::W16(g.note).c_str());
					Assert::AreEqual(0u, n.NamedHoldCount(id));
				}
		}

		TEST_METHOD(ANearMissStaysAGenericHold)
		{
			SidConv::Mon m;
			const SidConv::Run r = SidNamed::DriveDetect(SidNamed::PerAxisOn(0.72, 1.0, 1920, 1080), 1920, 1080, "all", m);
			Assert::IsTrue(SidNamed::AllNative(r.trace), SidConv::W16(r.trace).c_str());
			Assert::IsTrue(SidConv::Has(r.note, "single-axis re-base held (H 0.7200"), SidConv::W16(r.note).c_str());
			Assert::IsFalse(SidConv::Has(r.note, "held: "), SidConv::W16(r.note).c_str());
			for (const char* id : { "hdv_1440x1080", "dvcprohd_1280x1080", "dvcprohd_960x720" })
				Assert::AreEqual(0u, m.NamedHoldCount(id));
		}

		TEST_METHOD(NativesAndListedSizesAreUnaffected)
		{
			for (const char* detect : { "all", "conventional", "hdv_1440x1080 dvcprohd_1280x1080 dvcprohd_960x720" })
			{
				SidConv::Mon m, m0;
				const SidConv::Run n = SidNamed::DriveDetect(SidConv::Uniform(1.0), 1920, 1080, detect, m);
				Assert::IsTrue(SidNamed::AllNative(n.trace) && n.note.empty(), SidConv::W16(n.trace).c_str());
				const SidConv::Run p = SidNamed::DriveDetect(SidConv::PerAxis(1.0, 1.0), 1920, 1080, detect, m0);
				Assert::IsTrue(SidNamed::AllNative(p.trace) && p.note.empty(), SidConv::W16(p.trace).c_str());
				SidConv::Mon l, l0;
				const SidConv::Run u = SidNamed::DriveDetect(SidConv::Uniform(0.5), 1920, 1080, detect, l);
				const SidConv::Run u0 = SidConv::DriveFresh(SidConv::Uniform(0.5), true);
				Assert::AreEqual(u0.trace, u.trace, L"a listed size: byte-identical whatever the detection set");
				Assert::IsTrue(u.note.empty());
				for (const char* id : { "hdv_1440x1080", "dvcprohd_1280x1080", "dvcprohd_960x720" })
					Assert::AreEqual(0u, m.NamedHoldCount(id) + m0.NamedHoldCount(id) + l.NamedHoldCount(id));
			}
		}

		TEST_METHOD(TheOriginalPathwayIsUntouched)
		{
			const SidConv::Snap s = SidNamed::PerAxisOn(0.75, 1.0, 1920, 1080);
			SidConv::Mon a, b;
			a.SetOriginalPathway(true);
			b.SetOriginalPathway(true);
			const SidConv::Run ra = SidNamed::DriveDetect(s, 1920, 1080, "all", a);
			const SidConv::Run rb = SidNamed::DriveDetect(s, 1920, 1080, "conventional", b);
			const SidConv::Run r0 = SidConv::DriveFresh(s, true, false, true);
			Assert::AreEqual(r0.trace, ra.trace, L"the Original pathway: the detection setting changes nothing");
			Assert::AreEqual(r0.trace, rb.trace);
			Assert::IsTrue(ra.note.empty() && rb.note.empty(), SidConv::W16(ra.note).c_str());
			Assert::AreEqual(0u, a.NamedHoldCount("hdv_1440x1080"), L"nothing counted on the Original pathway");
		}

		TEST_METHOD(NarrowingTheSettingRemovesTheEntryFromRecognition)
		{
			const SidConv::Snap hdv = SidNamed::PerAxisOn(0.75, 1.0, 1920, 1080);
			SidConv::Mon only960;
			const SidConv::Run r = SidNamed::DriveDetect(hdv, 1920, 1080, "dvcprohd_960x720", only960);
			Assert::IsFalse(SidConv::Has(r.note, "held: "), SidConv::W16(r.note).c_str());
			Assert::IsTrue(SidConv::Has(r.note, "single-axis re-base held (H 0.7500"), SidConv::W16(r.note).c_str());
			Assert::AreEqual(0u, only960.NamedHoldCount("hdv_1440x1080"));
			SidConv::Mon listed;
			const SidConv::Run l = SidNamed::DriveDetect(hdv, 1920, 1080, "HDV_1440x1080", listed);
			Assert::IsTrue(SidConv::Has(l.note, "held: anamorphic HDV 1440x1080"), L"ids are case-insensitive");
			Assert::AreEqual(SidNamed::Factors(r.trace), SidNamed::Factors(l.trace), L"apply byte-identical");
		}

		TEST_METHOD(UnknownDetectNamesAreVisibleNeverSilent)
		{
			namespace G = Scaling::GeometrySet;
			G::DetectMask mask = 0u;
			std::string unknown;
			Assert::IsTrue(G::ParseDetect("", mask, unknown) && mask == G::DefaultDetectMask());
			Assert::IsTrue(G::ParseDetect(" ALL ", mask, unknown) && mask == G::DefaultDetectMask());
			Assert::IsTrue(G::ParseDetect("conventional", mask, unknown));
			std::size_t count = 0;
			const G::Geometry* g = G::Geometries(count);
			for (std::size_t i = 0; i < count; ++i)
				Assert::AreEqual(g[i].apply, (mask & (1u << i)) != 0u, L"conventional = the apply entries");
			Assert::IsFalse(G::ParseDetect("hdv_1440x1080, bogus;dvcprohd_960x720", mask, unknown));
			Assert::AreEqual(std::string("bogus"), unknown);
			SidConv::Mon m;
			const SidConv::Run r = SidNamed::DriveDetect(SidNamed::PerAxisOn(0.75, 1.0, 1920, 1080), 1920, 1080, "hdv_1440x1080, bogus", m);
			const FallbackCatalog::Decision d = m.GeometryDetectFallback();
			Assert::AreEqual(std::string("source geometry detect"), d.id);
			Assert::IsTrue(d.engaged && SidConv::Has(d.reason, "bogus"), SidConv::W16(d.reason).c_str());
			Assert::IsTrue(SidConv::Has(r.note, "held: anamorphic HDV 1440x1080"), L"the known ids still detect");
			m.SetGeometryDetect("all");
			Assert::IsFalse(m.GeometryDetectFallback().engaged, L"released when every id is known");
		}

		// Doc 91 E: widening DETECT never latches a native. A native chain stream, every entry detect on (default, and listed by id).
		TEST_METHOD(ANativeChainStreamWithEveryEntryDetectingNeverLatches)
		{
			for (const char* detect : { "all", "2560x1440 1920x1080 1280x720 960x540 dci_2048x1080 dci_1998x1080 hdv_1440x1080 sd_720x576 sd_720x480 dvcprohd_1280x1080 dvcprohd_960x720" })
			{
				SidGate::Mon m;
				GeoFx::G::HeldState geo;
				m.SetGeometrySource(&geo);
				m.SetSourceIdMode(SidGate::Mon::SourceIdMode::On);
				m.SetRebaseConventionOnly(true);
				m.SetGeometryDetect(detect);
				Assert::IsFalse(m.GeometryDetectFallback().engaged, L"every id is known");
				uint64_t seq = 2;
				for (int i = 0; i < 13; ++i)
				{
					const double t = 10.0 + 0.5 * i;
					if (i % 2 == 0)
						geo.Publish(GeoFx::MakeRecord(seq, SidGate::Us(t - 0.1), GeoFx::AppliedAxis(3840, 3840, 5), GeoFx::AppliedAxis(2160, 2160, 5)));
					SidGate::Analyse(m, t, seq++);
				}
				std::string why;
				double fx = 1.0, fy = 1.0;
				m.InjectResultForTest(SidGate::CleanSnapshot(1, false), seq - 1);
				m.AutoFactors(3840, 2160, 0.8, why, fx, fy, 16.1, seq);
				m.InjectResultForTest(SidGate::CleanSnapshot(2, true), seq);
				m.AutoFactors(3840, 2160, 0.8, why, fx, fy, 16.6, seq + 1);
				SourceIdentity::Record rec;
				const bool latched = m.SourceIdRecord(rec) && (rec.ratioH < 1.0f || rec.ratioV < 1.0f);
				Assert::IsFalse(latched, L"0 latches on a native stream");
				Assert::AreEqual(1.0, fx, 1e-9, SidConv::W16(why).c_str());
				Assert::AreEqual(1.0, fy, 1e-9);
				Assert::IsTrue(m.Read().conventionNote.empty(), SidConv::W16(m.Read().conventionNote).c_str());
				for (const char* id : { "hdv_1440x1080", "dvcprohd_1280x1080", "dvcprohd_960x720" })
					Assert::AreEqual(0u, m.NamedHoldCount(id));
			}
		}
	};

	// Crop-aware convention (coordinator design call 2026-11-11, Scaling::GeometrySet CroppedRatiosFor): a raster WIDER than an entry's
	// display aspect (a scope / flat film coded as its active area) is that entry cropped to the raster's aspect, width-fitted. Uniform
	// two-axis pairs engage; non-uniform crops stay held; 16:9 rasters, natives and the Original pathway are unchanged.
	namespace SidCrop
	{
		inline SidConv::Snap UniformOn(double r, int w, int h)
		{
			SidConv::Snap s = SidConv::Uniform(r);
			s.nativeWidth = w;
			s.nativeHeight = h;
			return s;
		}

		inline SidConv::Run DriveOn(const SidConv::Snap& s, int w, int h, bool conventionOnly, bool original = false)
		{
			SidConv::Mon m;
			m.SetRebaseConventionOnly(conventionOnly);
			m.SetOriginalPathway(original);
			return SidConv::Drive(m, s, w, h);
		}
	}

	TEST_CLASS(SourceGeometryCropAwareTests)
	{
	public:
		TEST_METHOD(ScopeAndFlatCodedActiveAreasEngage)
		{
			struct Case { int w, h; double rH, rV; const wchar_t* what; };
			const Case cases[] = {
				{ 1920, 800, 0.5, 0.5, L"scope 2.40 1920x800 from 960x400 (960x540 cropped)" },
				{ 1920, 804, 0.5, 0.5, L"scope 2.39 1920x804 from 960x402" },
				{ 1920, 1038, 0.5, 0.5, L"flat 1.85 1920x1038 from 960x519" },
				{ 1920, 800, 1280.0 / 1920.0, 536.0 / 800.0, L"scope 1280x536 at 2/3 on 1920x800 (1280x720 cropped)" },
			};
			for (const Case& c : cases)
			{
				const bool uniform = c.rH == c.rV;
				const SidConv::Snap s = uniform ? SidCrop::UniformOn(c.rH, c.w, c.h) : SidNamed::PerAxisOn(c.rH, c.rV, c.w, c.h);
				const SidConv::Run r = SidCrop::DriveOn(s, c.w, c.h, true);
				const SidConv::Run r0 = SidCrop::DriveOn(s, c.w, c.h, false);
				// Engaged at the reading (the monitor applies whole source lines: within two lines of the raster).
				Assert::AreEqual(c.rH, r.fx, 2.0 / c.h, (std::wstring(c.what) + L": " + SidConv::W16(r.trace)).c_str());
				Assert::AreEqual(c.rV, r.fy, 2.0 / c.h, c.what);
				Assert::IsTrue(r.fx < 1.0 && r.fy < 1.0, c.what);
				Assert::AreEqual(r0.trace, r.trace, (std::wstring(c.what) + L": byte-identical with the restriction switched off").c_str());
				Assert::IsTrue(r.note.empty() && !SidConv::Has(r.shown, "re-base held"), c.what);
			}
			// The uniform 2/3 reading on the same scope raster (the classical tracker's form) engages too.
			const SidConv::Run u = SidCrop::DriveOn(SidCrop::UniformOn(2.0 / 3.0, 1920, 800), 1920, 800, true);
			Assert::AreEqual(2.0 / 3.0, u.fx, 1e-9, SidConv::W16(u.trace).c_str());
			Assert::IsTrue(u.note.empty());
		}

		TEST_METHOD(ANonUniformCropStaysHeld)
		{
			const SidConv::Run r = SidCrop::DriveOn(SidNamed::PerAxisOn(0.5, 0.6, 1920, 800), 1920, 800, true);
			Assert::IsTrue(SidNamed::AllNative(r.trace), SidConv::W16(r.trace).c_str());
			Assert::IsTrue(SidConv::Has(r.note, "non-standard size H 0.5000 x V 0.6000"), SidConv::W16(r.note).c_str());
			const SidConv::Run parked = SidCrop::DriveOn(SidNamed::PerAxisOn(0.5, 2.0 / 3.0, 1920, 800), 1920, 800, true);
			Assert::IsTrue(SidNamed::AllNative(parked.trace), L"a parked entry in the crop form is never applied");
			Assert::IsTrue(SidConv::Has(parked.note, "held: anamorphic DVCPRO HD 960x720"), SidConv::W16(parked.note).c_str());
			const SidConv::Run single = SidCrop::DriveOn(SidNamed::PerAxisOn(0.5, 1.0, 1920, 800), 1920, 800, true);
			Assert::IsTrue(SidNamed::AllNative(single.trace), L"single-axis stays held on a cropped raster");
			Assert::IsFalse(single.note.empty());
			const SidConv::Run odd = SidCrop::DriveOn(SidCrop::UniformOn(0.7, 1920, 800), 1920, 800, true);
			Assert::IsTrue(SidNamed::AllNative(odd.trace), L"a uniform size outside the list stays held");
		}

		TEST_METHOD(NativesAndTheOriginalPathwayAreUnaffected)
		{
			for (int h : { 800, 804, 1038 })
			{
				const SidConv::Run n = SidCrop::DriveOn(SidCrop::UniformOn(1.0, 1920, h), 1920, h, true);
				Assert::IsTrue(SidNamed::AllNative(n.trace) && n.note.empty(), SidConv::W16(n.trace).c_str());
				const SidConv::Run p = SidCrop::DriveOn(SidNamed::PerAxisOn(1.0, 1.0, 1920, h), 1920, h, true);
				Assert::IsTrue(SidNamed::AllNative(p.trace) && p.note.empty(), SidConv::W16(p.trace).c_str());
			}
			const SidConv::Snap s = SidCrop::UniformOn(0.7, 1920, 800);
			const SidConv::Run o = SidCrop::DriveOn(s, 1920, 800, true, true);
			const SidConv::Run o0 = SidCrop::DriveOn(s, 1920, 800, false, true);
			Assert::AreEqual(o0.trace, o.trace, L"the Original pathway: no convention check (byte-identical)");
			Assert::IsTrue(o.note.empty());
		}

		TEST_METHOD(TheCropFormOnlyAddsRastersWiderThanTheEntry)
		{
			namespace G = Scaling::GeometrySet;
			std::size_t count = 0;
			const G::Geometry* g = G::Geometries(count);
			double rx = 0.0, ry = 0.0;
			for (std::size_t i = 0; i < count; ++i)
			{
				Assert::IsFalse(G::CroppedRatiosFor(g[i], 1920, 1080, rx, ry), L"16:9 raster: the fit only (unchanged)");
				Assert::IsFalse(G::CroppedRatiosFor(g[i], 3840, 2160, rx, ry), L"2160p raster: the fit only (unchanged)");
			}
			Assert::AreEqual(std::string("960x540"), std::string(G::ConventionalPair(960, 1920, 400, 800, 1920, 800)));
			Assert::AreEqual(std::string("960x540"), std::string(G::ConventionalPair(960, 1920, 519, 1038, 1920, 1038)));
			Assert::AreEqual(std::string("1280x720"), std::string(G::ConventionalPair(1280, 1920, 536, 800, 1920, 800)));
			Assert::IsTrue(G::AgreeingPair(960, 1920, 402, 804, 1920, 804));
			Assert::IsNull(G::ConventionalPair(960, 1920, 480, 800, 1920, 800), L"non-uniform: no entry");
			bool dvParked = false;
			Assert::IsNotNull(G::ConventionalPair(960, 1920, 533, 800, 1920, 800, &dvParked), L"DVCPRO HD 960x720 cropped: recognised");
			Assert::IsTrue(dvParked, L"... and parked");
			Assert::IsFalse(G::AgreeingPair(960, 1920, 533, 800, 1920, 800), L"... never a latch");
			bool parked = false;
			const char* hdv = G::ConventionalPair(1440, 1920, 600, 800, 1920, 800, &parked);
			Assert::IsTrue(hdv == nullptr || parked, L"anamorphic entries stay parked in the crop form too");
			Assert::IsFalse(G::AgreeingPair(1440, 1920, 600, 800, 1920, 800));
		}
	};
}
