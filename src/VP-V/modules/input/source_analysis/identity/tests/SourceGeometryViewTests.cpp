/*
 * Copyright(C) 2026 VP-V contributors
 * SPDX-License-Identifier: GPL-3.0-only
 */
// SourceGeometryView (SID train r2): Confident() and Provisional() are different types; an absent, stale or other-version record gives the
// delivered geometry plus an engaged FallbackCatalog decision product.source_geometry with a reason; a fresh record whose axes are UNSURE
// (not confident) is no fallback. Candidate and Unstable are reserved numerics of the parked latch design: never a claim. CPU only.
#include "pch.h"
#include "CppUnitTest.h"

#include "GeometryFixtures.h"

#include <VP-V/modules/input/source_analysis/identity/SourceGeometryView.h>

#include <string>
#include <type_traits>

using namespace Microsoft::VisualStudio::CppUnitTestFramework;

namespace Tests
{
	TEST_CLASS(SourceGeometryViewTests)
	{
	public:
		static GeoFx::G::Delivered DeliveredUhd()
		{
			GeoFx::G::Delivered d;
			d.width = 3840;
			d.height = 2160;
			return d;
		}

		TEST_METHOD(ConfidentAndProvisionalAreDifferentTypes)
		{
			using namespace GeoFx;
			static_assert(!std::is_same<G::ConfidentGeometry, G::ProvisionalGeometry>::value, "different types");
			static_assert(!std::is_convertible<G::ConfidentGeometry, G::ProvisionalGeometry>::value, "no implicit conversion");
			static_assert(!std::is_convertible<G::ProvisionalGeometry, G::ConfidentGeometry>::value, "no implicit conversion");
			static_assert(!std::is_same<G::ConfidentAxis, G::ProvisionalAxis>::value, "different axis types");
			Assert::IsTrue(true);
		}

		TEST_METHOD(AFreshRecordGivesTheAppliedValuesToConfidentAndTheMeasuredOnesToProvisional)
		{
			using namespace GeoFx;
			G::Record r = MakeRecord(5, 10000000, AppliedAxis(1920, 3840, 5), AppliedAxis(1080, 2160, 5));
			r.measured[0] = MeasuredAxis(G::LatchState::Provisional, 1280, 3840);   // the measured geometry has moved on
			r.measured[1] = MeasuredAxis(G::LatchState::Unknown, 0, 2160);
			r.appliedGen = 4;
			r.geometryGen = 9;
			const G::ConfidentGeometry l = G::Confident(&r, DeliveredUhd(), 10500000);
			Assert::IsTrue(l.fromRecord && !l.fallback.engaged && l.reason.empty());
			Assert::IsTrue(l.axis[0].confident && l.axis[0].n == 1920 && l.axis[0].N == 3840 && l.axis[0].ratio == 0.5);
			Assert::IsTrue(l.axis[0].kernel.Known() && l.axis[0].kernel.index == 5);
			Assert::IsTrue(l.appliedGen == 4 && l.AnyConfident());
			const G::ProvisionalGeometry p = G::Provisional(&r, DeliveredUhd(), 10500000);
			Assert::IsTrue(p.fromRecord && p.axis[0].known && p.axis[0].n == 1280, L"provisional = the measured hypothesis");
			Assert::IsTrue(!p.axis[1].known, L"an UNKNOWN measured axis makes no provisional claim");
			Assert::IsTrue(p.axis[1].n == 2160 && p.axis[1].N == 2160, L"and reads as the delivered size");
			Assert::IsTrue(p.geometryGen == 9 && p.appliedGen == 4);
		}

		TEST_METHOD(AnAbsentRecordGivesTheDeliveredGeometryAndAVisibleFallback)
		{
			using namespace GeoFx;
			const G::ConfidentGeometry l = G::Confident(nullptr, DeliveredUhd(), 1000);
			Assert::IsFalse(l.fromRecord);
			Assert::IsTrue(l.fallback.engaged && l.fallback.id == "product.source_geometry");
			Assert::IsTrue(l.fallback.reason.find("absent") != std::string::npos);
			Assert::IsTrue(l.reason == "absent" && l.cause == G::kCauseAbsent);
			Assert::IsTrue(l.axis[0].n == 3840 && l.axis[0].N == 3840 && l.axis[1].n == 2160 && l.axis[0].ratio == 1.0 && !l.AnyConfident());
			const G::ProvisionalGeometry p = G::Provisional(nullptr, DeliveredUhd(), 1000);
			Assert::IsTrue(!p.fromRecord && p.fallback.engaged && p.fallback.id == "product.source_geometry" && !p.AnyKnown());
			Assert::IsTrue(p.axis[1].n == 2160);
		}

		TEST_METHOD(AStaleRecordFallsBackAndRecoversOnTheNextFreshOne)
		{
			using namespace GeoFx;
			G::Record r = MakeRecord(5, 10000000, AppliedAxis(1920, 3840, 5), AppliedAxis(1080, 2160, 5));
			r.maxAgeUs = 4000000;
			G::ConfidentGeometry l = G::Confident(&r, DeliveredUhd(), 14000001);
			Assert::IsTrue(!l.fromRecord && l.fallback.engaged && l.reason == "stale");
			Assert::IsTrue(l.axis[0].n == 3840, L"the delivered geometry stands in");
			l = G::Confident(&r, DeliveredUhd(), 13000000);
			Assert::IsTrue(l.fromRecord && !l.fallback.engaged, L"a call within the age is fresh again");
			Assert::IsTrue(l.fallback.id == "product.source_geometry", L"released, with the same id");
			const G::ProvisionalGeometry p = G::Provisional(&r, DeliveredUhd(), 20000000);
			Assert::IsTrue(!p.fromRecord && p.reason == "stale");
		}

		TEST_METHOD(AnotherUnitVersionFallsBackWithItsReason)
		{
			using namespace GeoFx;
			G::Record r = MakeRecord(5, 10000000, AppliedAxis(1920, 3840, 5), AppliedAxis(1080, 2160, 5));
			r.unitVersion = G::kUnitVersion + 1;
			const G::ConfidentGeometry l = G::Confident(&r, DeliveredUhd(), 10000000);
			Assert::IsTrue(!l.fromRecord && l.reason == "unit version" && l.fallback.engaged);
			Assert::IsTrue(l.fallback.reason.find("unit version") != std::string::npos);
			Assert::IsTrue(!G::Provisional(&r, DeliveredUhd(), 10000000).fromRecord);
		}

		TEST_METHOD(AnUnsureAxisMakesNoClaimAndIsNoFallback)
		{
			using namespace GeoFx;
			G::Record r = MakeRecord(5, 10000000, MeasuredAxis(G::LatchState::Provisional, 1920, 3840), AppliedAxis(1080, 2160, -1));
			r.applied[0] = G::AxisGeom();   // UNSURE on H: nothing applied
			const G::ConfidentGeometry l = G::Confident(&r, DeliveredUhd(), 10000000);
			Assert::IsTrue(l.fromRecord && !l.fallback.engaged, L"a fresh record is no fallback");
			Assert::IsFalse(l.axis[0].confident);
			Assert::IsTrue(l.axis[0].n == 3840 && l.axis[0].ratio == 1.0, L"no claim: the delivered values");
			Assert::IsTrue(l.axis[1].confident && !l.axis[1].kernel.Known(), L"n known, kernel unsure is visible to the consumer");
			Assert::IsTrue(G::Provisional(&r, DeliveredUhd(), 10000000).axis[0].known, L"while the measured side has a hypothesis");
		}

		TEST_METHOD(ReservedStatesOnTheAppliedSideAreNeverAClaim)
		{
			using namespace GeoFx;
			G::Record r = MakeRecord(5, 10000000, AppliedAxis(2560, 3840, 5), AppliedAxis(1080, 2160, 5));
			r.applied[0].state = G::LatchState::Unstable;    // reserved by the parked latch design: never written, never trusted
			r.applied[1].state = G::LatchState::Candidate;
			const G::ConfidentGeometry l = G::Confident(&r, DeliveredUhd(), 10000000);
			Assert::IsTrue(l.fromRecord && !l.axis[0].confident && !l.axis[1].confident && !l.AnyConfident());
			Assert::IsTrue(l.axis[0].n == 3840 && l.axis[1].n == 2160, L"the delivered values stand in for an axis without a claim");
		}

		TEST_METHOD(TheStatusTextSaysWhatTheChainDecidedOrWhyTheClassicalVotersDecide)
		{
			using namespace GeoFx;
			G::Record r = MakeRecord(5, 10000000, AppliedAxis(1920, 3840, 5), AppliedAxis(1080, 2160, -1));
			std::string t = G::ChainStatusText(G::Confident(&r, DeliveredUhd(), 10000000), G::Provisional(&r, DeliveredUhd(), 10000000));
			Assert::IsTrue(t.find("chain: H 1920->3840 lanczos3") != std::string::npos, L"confident axis with its kernel");
			Assert::IsTrue(t.find("V 1080->2160 kernel unsure") != std::string::npos);
			r.applied[0] = G::AxisGeom();
			r.applied[1] = G::AxisGeom();
			r.measured[0] = MeasuredAxis(G::LatchState::Abstain, 0, 3840);
			r.measured[0].reasonBits = SourceIdentity::Chain::kReasonDark;
			t = G::ChainStatusText(G::Confident(&r, DeliveredUhd(), 10000000), G::Provisional(&r, DeliveredUhd(), 10000000));
			Assert::IsTrue(t.find("classical (chain: abstain)") == 0, L"nothing confident: the classical voters decide, visibly");
			t = G::ChainStatusText(G::Confident(nullptr, DeliveredUhd(), 0), G::Provisional(nullptr, DeliveredUhd(), 0));
			Assert::IsTrue(t == "classical (chain: absent)");
			r.applied[1] = AppliedAxis(1080, 2160, 5);
			t = G::ChainStatusText(G::Confident(&r, DeliveredUhd(), 10000000), G::Provisional(&r, DeliveredUhd(), 10000000));
			Assert::IsTrue(t.find("H classical (abstain: dark)") != std::string::npos, L"an UNSURE axis names its state and reason");
		}
		TEST_METHOD(ARecordWithoutAMeasuredIntervalIsNeverUsedAndSaysSo)
		{
			using namespace GeoFx;
			G::Record r = MakeRecord(5, 10000000, AppliedAxis(1920, 3840, 5), AppliedAxis(1080, 2160, 5));
			r.maxAgeUs = 0;
			const G::ConfidentGeometry l = G::Confident(&r, DeliveredUhd(), 10000000);
			Assert::IsTrue(!l.fromRecord && l.cause == G::kCauseNotMeasured && l.reason == "evaluation interval not measured yet");
			Assert::IsTrue(l.fallback.engaged && l.fallback.reason.find("evaluation interval not measured yet") != std::string::npos);
		}

		TEST_METHOD(AnUndecodableRecordIsNamedByItsReadCauseAndTheConsumerIsInTheWording)
		{
			using namespace GeoFx;
			G::ConfidentGeometry l = G::Confident(nullptr, DeliveredUhd(), 0, G::Consumer::AdaptiveScaler, ~0ull, G::kCauseUnitVersion);
			Assert::IsTrue(!l.fromRecord && l.cause == G::kCauseUnitVersion && l.reason == "unit version");
			Assert::IsTrue(l.fallback.engaged && l.fallback.reason.find("adaptive scaling") != std::string::npos, L"names the consumer");
			l = G::Confident(nullptr, DeliveredUhd(), 0, G::Consumer::Status, ~0ull, G::kCauseCorrupt);
			Assert::IsTrue(l.cause == G::kCauseCorrupt && l.reason == "corrupt record");
			const G::ProvisionalGeometry p = G::Provisional(nullptr, DeliveredUhd(), 0, G::Consumer::QualityPattern, ~0ull, G::kCauseCorrupt);
			Assert::IsTrue(!p.fromRecord && p.cause == G::kCauseCorrupt && p.fallback.engaged);
			Assert::IsTrue(p.fallback.reason.find("quality pattern") != std::string::npos);
			// A record that is not sealed yet for the reader's frame is "not sealed yet", through the same Read().
			G::Record r = MakeRecord(50, 1000000, AppliedAxis(1920, 3840, 5), AppliedAxis(1080, 2160, 5));
			l = G::Confident(&r, DeliveredUhd(), 1000000, G::Consumer::Status, 40);
			Assert::IsTrue(!l.fromRecord && l.cause == G::kCauseNotYet);
		}

		TEST_METHOD(ConfidentOfIsTheOneDefinitionOfAClaim)
		{
			using namespace GeoFx;
			G::Record r = MakeRecord(5, 10000000, AppliedAxis(1920, 3840, 5), AppliedAxis(1080, 2160, 5));
			r.applied[0].state = G::LatchState::Hold;   // an overlay state is not a decision (the field state lives in measured)
			r.applied[1].state = G::LatchState::Provisional;
			G::ConfidentGeometry l = G::ConfidentOf(r, DeliveredUhd());
			Assert::IsTrue(!l.axis[0].confident && l.axis[0].n == 3840);
			Assert::IsTrue(!l.axis[1].confident && l.axis[1].n == 2160);
			r.applied[1].state = G::LatchState::Latched;   // Latched is the reserved numeric of CONFIDENT
			l = G::ConfidentOf(r, DeliveredUhd());
			Assert::IsTrue(l.axis[1].confident && l.axis[1].n == 1080);
		}
	};
}
