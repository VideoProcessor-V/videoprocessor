/*
 * Copyright(C) 2026 VP-V contributors
 * SPDX-License-Identifier: GPL-3.0-only
 */
// The Monitor's chain gate (SID-C2a): with source_id_consensus ON, AutoFactors may ENGAGE (add) a re-base for an axis the chain has
// LATCHED with a verified kernel, at the unchanged engage moments (here a low-detail moment); "n known, kernel unsure" holds the
// re-base visibly; a chain-latched return to native is immediate; Shadow is byte-identical to Off (the would-be is computed and
// published only); Off with a geometry source is inert; the record's fallback (product.source_geometry) is visible. CPU only,
// synthetic tiles through AnalyseTilesForTest, no GPU, no files.
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

		struct Outcome
		{
			double fx = 1.0, fy = 1.0;
			std::string why1, why2;
		};

		// Two analysed frames, then the factors twice: the first call sees the wish and waits, the second is a low-detail moment.
		inline Outcome Script(Mon& m, double t0)
		{
			Analyse(m, t0, 2);
			Analyse(m, t0 + 0.5, 3);
			Outcome o;
			m.InjectResultForTest(CleanSnapshot(1, false), 3);
			std::string why;
			m.AutoFactors(3840, 2160, 0.8, why, o.fx, o.fy, t0 + 0.6, 4);
			o.why1 = why;
			why.clear();
			m.InjectResultForTest(CleanSnapshot(2, true), 4);
			m.AutoFactors(3840, 2160, 0.8, why, o.fx, o.fy, t0 + 1.1, 5);
			o.why2 = why;
			return o;
		}

		inline GeoFx::G::Record Latched(uint64_t seq, double timeSec, int kernel)
		{
			using namespace GeoFx;
			return MakeRecord(seq, Us(timeSec), AppliedAxis(1920, 3840, kernel), AppliedAxis(1080, 2160, kernel));
		}
	}

	TEST_CLASS(SourceIdChainGateTests)
	{
	public:
		TEST_METHOD(OnAddsAReBaseForAChainLatchedAxisAtAnEngageMoment)
		{
			SidGate::Mon m;
			GeoFx::G::HeldState geo;
			geo.Publish(SidGate::Latched(2, 9.5, 5));
			m.SetGeometrySource(&geo);
			m.SetSourceIdMode(SidGate::Mon::SourceIdMode::On);
			const SidGate::Outcome o = SidGate::Script(m, 10.0);
			SourceIdentity::Record p14;
			Assert::IsTrue(m.SourceIdRecord(p14) && p14.ratioH == 0.5f && p14.ratioV == 0.5f, L"fixture: the consensus adopted the chain value");
			Assert::IsTrue(m.Read().declaredRatio == 1.0, L"fixture: the classical trackers declared nothing");
			Assert::IsTrue(o.why1.find("waiting for the next cut or black") != std::string::npos, L"the wish waits for an engage moment");
			Assert::AreEqual(0.5, o.fx, 1e-9, L"the chain ADDED a re-base on H at the low-detail moment");
			Assert::AreEqual(0.5, o.fy, 1e-9);
			Assert::IsTrue(o.why2.find("low-detail moment") != std::string::npos);
			Assert::IsTrue(m.SourceIdChainHold().empty());
			Assert::IsFalse(m.SourceIdGeometryFallback().engaged, L"a fresh record is no fallback");
			const std::string status = m.SourceIdStatusLine(11.1);
			Assert::IsTrue(status.find("chain: H 1920->3840 lanczos3") != std::string::npos, L"the status says what the chain decided");
		}

		TEST_METHOD(AChainWithoutAnEngageMomentWaitsForTheCrossfadeAfterTheMaxWait)
		{
			SidGate::Mon m;
			m.SetEngageTiming(4.0, 1.5, 0.25);   // a short max wait: the classical veto cannot interfere inside this script
			GeoFx::G::HeldState geo;
			m.SetGeometrySource(&geo);
			m.SetSourceIdMode(SidGate::Mon::SourceIdMode::On);
			double firstEngaged = -1.0;
			std::string whyAtEngage;
			double fx = 1.0, fy = 1.0;
			uint64_t seq = 2;
			for (int i = 0; i <= 14; ++i)   // frames at 10.0 .. 17.0 s, each with a fresh chain record
			{
				const double t = 10.0 + 0.5 * i;
				geo.Publish(SidGate::Latched(seq, t - 0.4, 5));
				SidGate::Analyse(m, t, seq);
				m.InjectResultForTest(SidGate::CleanSnapshot(1, false), seq);   // never a cut, black or low-detail moment
				std::string why;
				m.AutoFactors(3840, 2160, 0.8, why, fx, fy, t + 0.1, seq + 1);
				if (fx < 1.0 && firstEngaged < 0.0)
				{
					firstEngaged = t;
					whyAtEngage = why;
				}
				++seq;
			}
			Assert::IsTrue(firstEngaged >= 14.0 - 1e-9 && firstEngaged <= 14.5 + 1e-9, L"engaged only after the 4 s max wait since the wish");
			Assert::IsTrue(whyAtEngage.find("crossfade") != std::string::npos);
			Assert::AreEqual(0.5, fx, 1e-9);
		}

		TEST_METHOD(NKnownKernelUnsureHoldsTheReBaseWithAVisibleReason)
		{
			SidGate::Mon m;
			GeoFx::G::HeldState geo;
			geo.Publish(SidGate::Latched(2, 9.5, -1));   // n known on both axes, the kernel is not verified
			m.SetGeometrySource(&geo);
			m.SetSourceIdMode(SidGate::Mon::SourceIdMode::On);
			const SidGate::Outcome o = SidGate::Script(m, 10.0);
			Assert::AreEqual(1.0, o.fx, 1e-9, L"no re-base without a verified kernel");
			Assert::AreEqual(1.0, o.fy, 1e-9);
			Assert::IsTrue(o.why1.find("kernel unsure") != std::string::npos, L"the decision text says why");
			Assert::IsTrue(m.SourceIdChainHold().find("kernel unsure") != std::string::npos);
			SourceIdentity::Record p14;
			Assert::IsTrue(m.SourceIdRecord(p14) && p14.ratioH == 0.5f, L"the consensus still knows n (TD and the pattern detectors may use it)");
			Assert::IsTrue(o.why2.find("kernel unsure") == std::string::npos, L"said once per reason, not every call");
		}

		TEST_METHOD(AChainLatchedReturnToNativeIsImmediate)
		{
			SidGate::Mon m;
			GeoFx::G::HeldState geo;
			geo.Publish(SidGate::Latched(2, 9.5, 5));
			m.SetGeometrySource(&geo);
			m.SetSourceIdMode(SidGate::Mon::SourceIdMode::On);
			const SidGate::Outcome o = SidGate::Script(m, 10.0);
			Assert::AreEqual(0.5, o.fx, 1e-9);
			// The chain latches native (a menu): its own latch has decided, so the return is not delayed to a cut.
			geo.Publish(GeoFx::MakeRecord(5, SidGate::Us(10.9), GeoFx::AppliedAxis(3840, 3840, -1), GeoFx::AppliedAxis(2160, 2160, -1)));
			SidGate::Analyse(m, 11.5, 6);
			SidGate::Analyse(m, 12.0, 7);
			SourceIdentity::Record p14;
			Assert::IsTrue(m.SourceIdRecord(p14) && p14.ratioH == 1.0f && p14.ratioV == 1.0f, L"fixture: the consensus adopted native");
			m.InjectResultForTest(SidGate::CleanSnapshot(3, false), 7);
			std::string why;
			double fx = o.fx, fy = o.fy;
			m.AutoFactors(3840, 2160, 0.8, why, fx, fy, 12.1, 8);
			Assert::AreEqual(1.0, fx, 1e-9);
			Assert::AreEqual(1.0, fy, 1e-9);
			Assert::IsTrue(why.find("at once") != std::string::npos);
		}

		TEST_METHOD(ShadowIsByteIdenticalToOffAndOnEngagesOnTheSameScript)
		{
			SidGate::Mon off, shadow, on;
			GeoFx::G::HeldState geoOff, geoShadow, geoOn;
			geoOff.Publish(SidGate::Latched(2, 9.5, 5));
			geoShadow.Publish(SidGate::Latched(2, 9.5, 5));
			geoOn.Publish(SidGate::Latched(2, 9.5, 5));
			off.SetGeometrySource(&geoOff);
			shadow.SetGeometrySource(&geoShadow);
			on.SetGeometrySource(&geoOn);
			off.SetSourceIdMode(SidGate::Mon::SourceIdMode::Off);
			shadow.SetSourceIdMode(SidGate::Mon::SourceIdMode::Shadow);
			on.SetSourceIdMode(SidGate::Mon::SourceIdMode::On);
			const SidGate::Outcome a = SidGate::Script(off, 10.0);
			const SidGate::Outcome b = SidGate::Script(shadow, 10.0);
			const SidGate::Outcome c = SidGate::Script(on, 10.0);
			Assert::IsTrue(a.fx == b.fx && a.fy == b.fy && a.why1 == b.why1 && a.why2 == b.why2, L"Shadow == Off, factors and decision texts");
			Assert::AreEqual(1.0, a.fx, 1e-9);
			Assert::AreEqual(0.5, c.fx, 1e-9, L"the same script engages under On: the equality above is not vacuous");
			SourceIdentity::Record rec;
			Assert::IsTrue(shadow.SourceIdRecord(rec) && rec.ratioH == 0.5f && (rec.flags & SourceIdentity::kFlagShadow) != 0,
				L"the would-be is computed and published, flagged as shadow");
			Assert::IsFalse(off.SourceIdRecord(rec), L"off publishes nothing");
			Assert::IsTrue(shadow.SourceIdChainHold().empty() && !shadow.SourceIdGeometryFallback().engaged);
			Assert::IsTrue(off.SourceIdChainHold().empty() && !off.SourceIdGeometryFallback().engaged, L"off with a geometry source is inert");
			Assert::AreEqual(std::string("Source identity: off"), off.SourceIdStatusLine(11.0));
			Assert::AreEqual(std::string("Source identity: shadow (classical decides)"), shadow.SourceIdStatusLine(11.0));
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
			GeoFx::G::Record g = SidGate::Latched(1, 0.0, 5);
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
		TEST_METHOD(AKernelWithNoClassicalInverseHoldsTheReBaseEvenWhenKnown)
		{
			SidGate::Mon m;
			GeoFx::G::HeldState geo;
			geo.Publish(SidGate::Latched(2, 9.5, 8));   // spline36: a chain kernel the re-base cannot invert
			m.SetGeometrySource(&geo);
			m.SetSourceIdMode(SidGate::Mon::SourceIdMode::On);
			const SidGate::Outcome o = SidGate::Script(m, 10.0);
			Assert::AreEqual(1.0, o.fx, 1e-9);
			Assert::AreEqual(1.0, o.fy, 1e-9);
			Assert::IsTrue(o.why1.find("no inverse") != std::string::npos, L"visible: known, but no inverse");
		}

		TEST_METHOD(ATwoToOneChainWithTheBilinearBicubicTieEngagesThroughItsInvertibleMember)
		{
			SidGate::Mon m;
			GeoFx::G::HeldState geo;
			geo.Publish(SidGate::Latched(2, 9.5, 0));   // the latch picked bilinear out of the equivalence class (it has an inverse)
			m.SetGeometrySource(&geo);
			m.SetSourceIdMode(SidGate::Mon::SourceIdMode::On);
			const SidGate::Outcome o = SidGate::Script(m, 10.0);
			Assert::AreEqual(0.5, o.fx, 1e-9);
			Assert::AreEqual(0.5, o.fy, 1e-9);
		}

		TEST_METHOD(TheOriginalPathwayForcesSourceIdentityOffWhateverWasRequested)
		{
			SidGate::Mon m;
			GeoFx::G::HeldState geo;
			geo.Publish(SidGate::Latched(2, 9.5, 5));
			m.SetGeometrySource(&geo);
			m.SetOriginalPathway(true);
			m.SetSourceIdMode(SidGate::Mon::SourceIdMode::On);
			Assert::IsTrue(m.RequestedSourceIdMode() == SidGate::Mon::SourceIdMode::On, L"the request is kept");
			Assert::IsTrue(m.GetSourceIdMode() == SidGate::Mon::SourceIdMode::Off && !m.SourceIdConsensus() && !m.SourceIdRunning());
			Assert::AreEqual(std::string("Source identity: off (Original pathway)"), m.SourceIdStatusLine(10.0));
			const SidGate::Outcome o = SidGate::Script(m, 10.0);
			Assert::AreEqual(1.0, o.fx, 1e-9);
			Assert::AreEqual(1.0, o.fy, 1e-9);
			SourceIdentity::Record rec;
			Assert::IsFalse(m.SourceIdRecord(rec), L"no pipeline ran");
			Assert::IsTrue(!m.SourceIdGeometryFallback().engaged && m.SourceIdChainHold().empty(), L"no fallback, no chain gate");
			m.SetOriginalPathway(false);
			Assert::IsTrue(m.SourceIdConsensus(), L"leaving the Original pathway the requested mode takes effect again");
		}
	};
}
