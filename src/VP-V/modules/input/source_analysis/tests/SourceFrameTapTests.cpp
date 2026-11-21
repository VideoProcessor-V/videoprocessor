// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only

// The source frame tap (SourceFrameTap.h + VpFrameAnalysis::Host::Owner / OnArrival / Process): with no
// sink nothing changes; a registered sink gets the decoded luma plane as a SourceFrameView (msbShift 6,
// keepAlive holding the pooled plane); formats read in place are decoded privately only for a sink; the
// Host registers its store under its owner key and removes it with itself. CPU only.

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include "CppUnitTest.h"

#include <VP-V/modules/input/source_analysis/SourceFrameTap.h>
#include <VP-V/modules/input/source_analysis/VpFrameAnalysis.h>
#include <VP-V/modules/input/source_analysis/VpFrameAnalysisHost.h>
#include <VP-V/tests/SyntheticAnalysisFrames.h>

#include <memory>
#include <string>
#include <vector>

using namespace Microsoft::VisualStudio::CppUnitTestFramework;

namespace SourceFrameTapTestsNs
{
	using namespace SyntheticAnalysisFrames;
	namespace Tap = VpFrameAnalysis::SourceFrameTap;
	using VpSkeleton::SourceFrameView;

	// The luma of every pixel equals Sample().luma, through the view's own geometry.
	void AssertViewMatches(const SourceFrameView& v, const AnalysisLumaSource& source)
	{
		Assert::AreEqual<uint32_t>(static_cast<uint32_t>(source.width), v.width);
		Assert::AreEqual<uint32_t>(static_cast<uint32_t>(source.height), v.height);
		Assert::AreEqual<uint32_t>(v.width, v.strideSamples);
		Assert::AreEqual<int>(6, v.msbShift, L"10-bit codes in the low bits of 16");
		Assert::IsTrue(v.Valid());
		for (int y = 0; y < source.height; ++y)
			for (int x = 0; x < source.width; ++x)
			{
				AnalysisLumaSample sample;
				Assert::IsTrue(source.Sample(x, y, sample));
				if (v.luma[static_cast<size_t>(y) * v.strideSamples + x] != sample.luma)
					Assert::Fail(L"the view's luma differs from Sample().luma");
			}
	}

	TEST_CLASS(SourceFrameTapTests)
	{
	public:
		TEST_METHOD(NoSinkChangesNothing)
		{
			VpFrameAnalysis::Host host;
			host.Owner(&host).Configure(true, false, false);
			host.Worker().Stop();
			Assert::IsFalse(Tap::AnyWants());
			Assert::IsFalse(Tap::Wants(&host));
			Frame frame = Make(Kind::Uyvy, 1);
			VpFrameAnalysis::Host::AttachIfEnabled(frame.source);
			Assert::IsTrue(frame.source.sharedLuma == nullptr, L"no sink and no reader: no plane");
			host.OnArrival(1, frame.source, FrameAnalysis::Rect{}, 0, false);
			host.Worker().Drain();
			Assert::AreEqual(uint64_t(0), host.Worker().Processed(), L"no job: nothing wants the frame");
			Assert::IsTrue(Tap::StoreFor(&host) == &host.Store(), L"Owner registers the store for the camera extension");
			host.Configure(false);
		}

		TEST_METHOD(AHostWithoutAnOwnerIsNeverTapped)
		{
			VpFrameAnalysis::Host host;   // Owner() never called
			std::vector<SourceFrameView> got;
			Tap::Register(&host, [&got](const SourceFrameView& v) { got.push_back(v); });
			host.Configure(true, false, false);
			host.Worker().Stop();
			Frame frame = Make(Kind::Uyvy, 1);
			host.OnArrival(1, frame.source, FrameAnalysis::Rect{}, 0, false);
			host.Worker().Drain();
			Assert::AreEqual<size_t>(0, got.size());
			Assert::AreEqual(uint64_t(0), host.Worker().Processed());
			Tap::Unregister(&host);
			host.Configure(false);
		}

		TEST_METHOD(ASinkGetsTheDecodedPlaneAndHoldsItAlive)
		{
			VpFrameAnalysis::Host host;
			std::vector<SourceFrameView> got;
			Tap::Register(&host, [&got](const SourceFrameView& v) { got.push_back(v); });
			Assert::IsTrue(Tap::AnyWants() && Tap::Wants(&host));
			host.Owner(&host).Configure(true, false, false);   // no other reader wants the plane
			host.Worker().Stop();
			Frame frame = Make(Kind::Uyvy, 1);
			VpFrameAnalysis::Host::AttachIfEnabled(frame.source);
			Assert::IsTrue(frame.source.sharedLuma != nullptr, L"a registered sink attaches the shared plane");
			host.OnArrival(7, frame.source, FrameAnalysis::Rect{}, 0, false);
			host.Worker().Drain();
			Assert::AreEqual<size_t>(1, got.size());
			Assert::AreEqual<uint64_t>(7, got[0].seq);
			Assert::IsTrue(got[0].keepAlive != nullptr);
			AssertViewMatches(got[0], frame.source);
			Assert::AreEqual<uint64_t>(1, Tap::Delivered(&host));

			// The view holds the pooled plane: it is not recycled for a later frame while the view lives.
			const void* held = got[0].keepAlive.get();
			frame.source.sharedLuma.reset();
			Frame next = Make(Kind::Uyvy, 2);
			VpFrameAnalysis::Host::AttachIfEnabled(next.source);
			Assert::IsTrue(next.source.sharedLuma != nullptr && next.source.sharedLuma.get() != held,
				L"a held plane is never handed out again");
			got.clear();   // the last holder lets go: the pool may recycle it now
			next.source.sharedLuma.reset();
			Frame third = Make(Kind::Uyvy, 3);
			VpFrameAnalysis::Host::AttachIfEnabled(third.source);
			Assert::IsTrue(third.source.sharedLuma != nullptr);

			Tap::Unregister(&host);
			Assert::IsFalse(Tap::AnyWants());
			host.Configure(false);
		}

		TEST_METHOD(PlaneFormatsAreDecodedPrivatelyForASink)
		{
			VpFrameAnalysis::Host host;
			std::vector<SourceFrameView> got;
			Tap::Register(&host, [&got](const SourceFrameView& v) { got.push_back(v); });
			host.Owner(&host).Configure(true, false, false);
			host.Worker().Stop();
			Frame frame = Make(Kind::P010, 1);
			VpFrameAnalysis::Host::AttachIfEnabled(frame.source);
			Assert::IsTrue(frame.source.sharedLuma == nullptr, L"a plane format is read in place: no shared plane");
			host.OnArrival(3, frame.source, FrameAnalysis::Rect{}, 0, false);
			host.Worker().Drain();
			Assert::AreEqual<size_t>(1, got.size());
			AssertViewMatches(got[0], frame.source);
			Tap::Unregister(&host);
			host.Configure(false);
		}

		TEST_METHOD(OnlyTheOwnersSinkIsDelivered)
		{
			VpFrameAnalysis::Host host;
			int other = 0;
			std::vector<SourceFrameView> gotOther;
			Tap::Register(&other, [&gotOther](const SourceFrameView& v) { gotOther.push_back(v); });
			host.Owner(&host).Configure(true, false, false);
			host.Worker().Stop();
			Assert::IsTrue(Tap::AnyWants());
			Assert::IsFalse(Tap::Wants(&host), L"another renderer's sink is not this one's");
			Frame frame = Make(Kind::Uyvy, 1);
			host.OnArrival(1, frame.source, FrameAnalysis::Rect{}, 0, false);
			host.Worker().Drain();
			Assert::AreEqual<size_t>(0, gotOther.size());
			Assert::AreEqual(uint64_t(0), host.Worker().Processed());
			Tap::Unregister(&other);
			host.Configure(false);
		}

		TEST_METHOD(AHostTakesItsStoreAndSinkWithIt)
		{
			const void* key = nullptr;
			{
				VpFrameAnalysis::Host host;
				key = &host;
				host.Owner(&host);
				Tap::Register(&host, [](const SourceFrameView&) {});
				Assert::IsTrue(Tap::StoreFor(key) != nullptr);
				Assert::IsTrue(Tap::Wants(key));
			}
			Assert::IsTrue(Tap::StoreFor(key) == nullptr, L"the store pointer never outlives the Host");
			Assert::IsFalse(Tap::Wants(key), L"nor its sink");
		}

		TEST_METHOD(ASinkMayRunWithoutTheRegistryLock)
		{
			// A sink that unregisters itself from inside the call must not deadlock.
			int key = 0;
			bool ran = false;
			Tap::Register(&key, [&](const SourceFrameView&) { ran = true; Tap::Unregister(&key); });
			SourceFrameView v;
			Assert::IsTrue(Tap::Deliver(&key, v));
			Assert::IsTrue(ran);
			Assert::IsFalse(Tap::Wants(&key));
			Assert::IsFalse(Tap::Deliver(&key, v));
		}

		// ---- SID-C2b: several sinks per owner ----
		TEST_METHOD(SeveralNamedSinksAreDeliveredInNameOrder)
		{
			int key = 0;
			std::vector<std::string> order;
			Tap::Register(&key, "b", [&order](const SourceFrameView&) { order.push_back("b"); });
			Tap::Register(&key, [&order](const SourceFrameView&) { order.push_back(""); });   // the old API is the name ""
			Tap::Register(&key, "a", [&order](const SourceFrameView&) { order.push_back("a"); });
			SourceFrameView v;
			Assert::IsTrue(Tap::Deliver(&key, v));
			Assert::AreEqual<size_t>(3, order.size());
			Assert::AreEqual(std::string(""), order[0]);
			Assert::AreEqual(std::string("a"), order[1]);
			Assert::AreEqual(std::string("b"), order[2]);
			Tap::Register(&key, "a", [&order](const SourceFrameView&) { order.push_back("a2"); });   // the same name replaces
			order.clear();
			Assert::IsTrue(Tap::Deliver(&key, v));
			Assert::AreEqual<size_t>(3, order.size());
			Assert::AreEqual(std::string("a2"), order[1]);
			Tap::UnregisterAll(&key);
			Assert::IsFalse(Tap::AnyWants());
			Assert::IsFalse(Tap::Deliver(&key, v));
		}

		TEST_METHOD(TheOldApiIsTheNameEmptyAndUnregisterKeepsTheOtherSinks)
		{
			int key = 0;
			Tap::Register(&key, [](const SourceFrameView&) {});
			Tap::Register(&key, "chain", [](const SourceFrameView&) {});
			Assert::IsTrue(Tap::Registered(&key, ""));
			Assert::IsTrue(Tap::Registered(&key, "chain"));
			Tap::Unregister(&key);   // the old call: the name "" only
			Assert::IsFalse(Tap::Registered(&key, ""));
			Assert::IsTrue(Tap::Registered(&key, "chain"), L"a module's teardown of its own sink leaves the others");
			Assert::IsTrue(Tap::Wants(&key));
			Tap::Unregister(&key, "chain");
			Assert::IsFalse(Tap::AnyWants());
			Assert::IsFalse(Tap::Wants(&key));
		}

		TEST_METHOD(AWantsPredicateGatesWantsAndDelivery)
		{
			int key = 0;
			bool want = false;
			int got = 0, always = 0;
			Tap::Register(&key, "chain", [&got](const SourceFrameView&) { ++got; }, [&want]() { return want; });
			Assert::IsTrue(Tap::AnyWants(), L"registered: the one-atomic fast path says yes");
			Assert::IsFalse(Tap::Wants(&key), L"but this owner's only sink does not want a frame now");
			SourceFrameView v;
			Assert::IsFalse(Tap::Deliver(&key, v));
			Assert::AreEqual(0, got);
			want = true;
			Assert::IsTrue(Tap::Wants(&key));
			Assert::IsTrue(Tap::Deliver(&key, v));
			Assert::AreEqual(1, got);
			Assert::AreEqual<uint64_t>(1, Tap::Delivered(&key, "chain"));
			Tap::Register(&key, [&always](const SourceFrameView&) { ++always; });   // an always-wanting sink next to it
			want = false;
			Assert::IsTrue(Tap::Wants(&key));
			Assert::IsTrue(Tap::Deliver(&key, v));
			Assert::AreEqual(1, got, L"the gated sink was not handed the frame");
			Assert::AreEqual(1, always);
			Tap::UnregisterAll(&key);
		}

		TEST_METHOD(ASkeletonSinkAndANamedSinkBothGetEveryPlaneAndTheOldOneIsUnchanged)
		{
			VpFrameAnalysis::Host host;
			std::vector<SourceFrameView> old, chain;
			Tap::Register(&host, [&old](const SourceFrameView& v) { old.push_back(v); });
			Tap::Register(&host, "chain", [&chain](const SourceFrameView& v) { chain.push_back(v); }, []() { return true; });
			host.Owner(&host).Configure(true, false, false);
			host.Worker().Stop();
			Frame frame = Make(Kind::Uyvy, 1);
			VpFrameAnalysis::Host::AttachIfEnabled(frame.source);
			host.OnArrival(7, frame.source, FrameAnalysis::Rect{}, 0, false);
			host.Worker().Drain();
			Assert::AreEqual<size_t>(1, old.size(), L"the skeleton's sink still receives every plane");
			Assert::AreEqual<size_t>(1, chain.size());
			Assert::AreEqual<uint64_t>(7, old[0].seq);
			AssertViewMatches(old[0], frame.source);
			AssertViewMatches(chain[0], frame.source);
			Assert::AreEqual<uint64_t>(1, Tap::Delivered(&host, ""));
			Assert::AreEqual<uint64_t>(1, Tap::Delivered(&host, "chain"));
			Assert::AreEqual<uint64_t>(2, Tap::Delivered(&host), L"the sum over the sinks");
			old.clear();
			chain.clear();
			Tap::UnregisterAll(&host);
			host.Configure(false);
		}

		TEST_METHOD(ASinkThatDoesNotWantAPlaneNowMeansNoDecodeAndNoJob)
		{
			VpFrameAnalysis::Host host;
			bool want = false;
			std::vector<SourceFrameView> got;
			Tap::Register(&host, "chain", [&got](const SourceFrameView& v) { got.push_back(v); }, [&want]() { return want; });
			host.Owner(&host).Configure(true, false, false);
			host.Worker().Stop();
			Frame frame = Make(Kind::Uyvy, 1);
			host.OnArrival(1, frame.source, FrameAnalysis::Rect{}, 0, false);
			host.Worker().Drain();
			Assert::AreEqual(uint64_t(0), host.Worker().Processed(), L"nothing wants the frame now: no decode, no job");
			Assert::AreEqual<size_t>(0, got.size());
			want = true;
			Frame next = Make(Kind::Uyvy, 2);
			VpFrameAnalysis::Host::AttachIfEnabled(next.source);
			host.OnArrival(2, next.source, FrameAnalysis::Rect{}, 0, false);
			host.Worker().Drain();
			Assert::AreEqual<size_t>(1, got.size());
			AssertViewMatches(got[0], next.source);
			got.clear();
			Tap::UnregisterAll(&host);
			host.Configure(false);
		}

		TEST_METHOD(StoreOwnersListsTheRenderersThatRegisteredAStore)
		{
			Assert::AreEqual<size_t>(0, Tap::StoreOwners().size());
			{
				VpFrameAnalysis::Host host;
				host.Owner(&host);
				const std::vector<const void*> owners = Tap::StoreOwners();
				Assert::AreEqual<size_t>(1, owners.size());
				Assert::IsTrue(owners[0] == &host);
			}
			Assert::AreEqual<size_t>(0, Tap::StoreOwners().size());
		}

		TEST_METHOD(AHostTakesEverySinkOfItsOwnerWithIt)
		{
			const void* key = nullptr;
			{
				VpFrameAnalysis::Host host;
				key = &host;
				host.Owner(&host);
				Tap::Register(&host, [](const SourceFrameView&) {});
				Tap::Register(&host, "chain", [](const SourceFrameView&) {});
				Assert::IsTrue(Tap::Registered(key, "") && Tap::Registered(key, "chain"));
			}
			Assert::IsFalse(Tap::Registered(key, ""), L"no sink of any name outlives its Host's store");
			Assert::IsFalse(Tap::Registered(key, "chain"));
			Assert::IsFalse(Tap::AnyWants());
		}

		TEST_METHOD(SourceViewOfIsTheTapsViewForEveryFormat)
		{
			// C9 commit A: the view the tap delivers, as a function (seq left 0 for the caller). Every format, a plane
			// decoded by the shared decode, and none while an attached plane is still pending.
			for (int k = 0; k < static_cast<int>(Kind::Count); ++k)
			{
				Frame frame = Make(static_cast<Kind>(k), 2);
				SourceFrameView view;
				Assert::IsTrue(VpFrameAnalysis::SourceViewOf(frame.source, view), KindName(static_cast<Kind>(k)));
				Assert::AreEqual<uint64_t>(0u, view.seq);
				Assert::IsTrue(view.keepAlive != nullptr);
				AssertViewMatches(view, frame.source);
			}
			Frame pending = Make(Kind::Uyvy, 2);
			VpFrameAnalysis::AttachSharedLuma(pending.source);
			Assert::IsTrue(pending.source.sharedLuma != nullptr);
			SourceFrameView none;
			Assert::IsFalse(VpFrameAnalysis::SourceViewOf(pending.source, none), L"a pending plane is not a view");
			Assert::IsTrue(VpFrameAnalysis::DecodeSharedLuma(pending.source));
			SourceFrameView shared;
			Assert::IsTrue(VpFrameAnalysis::SourceViewOf(pending.source, shared));
			Assert::IsTrue(shared.luma == pending.source.SharedLumaRow(0), L"the shared plane itself, no copy");
			AssertViewMatches(shared, pending.source);
		}
	};

	// 15c-c8 (coordinator ruling 2026-11-15, the demand-driven principle): the Host's store runs while configured OR demanded by a
	// registered consumer (SourceFrameTap::SetStoreDemand); the configured value is never changed and the demand is withdrawn with
	// the consumer. CPU only.
	TEST_CLASS(StoreConsumerDemandTests)
	{
	public:
		TEST_METHOD(ADemandTurnsAnOffStoreOnAndItsWithdrawalTurnsItOff)
		{
			VpFrameAnalysis::Host host;
			host.Owner(&host).Configure(false);
			Assert::IsFalse(host.Enabled(), L"configured off, no consumer: off");
			Assert::IsFalse(Tap::StoreDemanded(&host));
			Tap::SetStoreDemand(&host, true, "LED drive");
			Assert::IsTrue(host.Enabled(), L"a consumer's demand turns the store on");
			Assert::IsTrue(host.Store().Enabled());
			Assert::IsTrue(host.RaisedByDemand(), L"on only because of the demand");
			Assert::IsTrue(Tap::StoreRaisedByDemand(&host));
			std::string why;
			Assert::IsTrue(Tap::StoreDemanded(&host, &why));
			Assert::AreEqual(std::string("LED drive"), why, L"the consumer is named for the status");
			host.Owner(&host).Configure(false);   // a settings apply with the configuration still off keeps the demand
			Assert::IsTrue(host.Enabled());
			Tap::SetStoreDemand(&host, false, std::string());
			Assert::IsFalse(host.Enabled(), L"withdrawn: back to the configured off");
			Assert::IsFalse(host.RaisedByDemand());
			Assert::IsFalse(Tap::StoreRaisedByDemand(&host));
		}

		TEST_METHOD(AConfiguredStoreIsNotReportedAsRaisedAndStaysOnWhenTheDemandGoes)
		{
			VpFrameAnalysis::Host host;
			host.Owner(&host).Configure(true);
			Tap::SetStoreDemand(&host, true, "LED drive");
			Assert::IsTrue(host.Enabled());
			Assert::IsFalse(host.RaisedByDemand(), L"configured on: the demand raises nothing");
			Tap::SetStoreDemand(&host, false, std::string());
			Assert::IsTrue(host.Enabled(), L"the configuration keeps it on");
			host.Owner(&host).Configure(false);
			Assert::IsFalse(host.Enabled());
			host.Worker().Stop();
		}

		// GF-P2b S3 (2), test first (GV): with frame_analysis_store OFF by configuration and the store raised by a consumer's demand (the LED
		// drive, through the skeleton), the frames that arrive after the raise get frame-analysis records and reach the tap's sink. If this is
		// RED the follow-up is in VpFrameAnalysis::Host (the configured-off shared-plane flag), not here.
		// The registered tap sink makes Tap::AnyWants true, as in production (the skeleton registers it), so AttachIfEnabled attaches the
		// plane through the sink and the Host's SharedFlag (configured on AND full-luma readers) is NOT exercised here: the next test is
		// the case without a sink.
		TEST_METHOD(ARaisedStoreWithTheConfigurationOffProducesRecordsForTheFramesAfterTheRaise)
		{
			VpFrameAnalysis::Host host;
			std::vector<SourceFrameView> got;
			Tap::Register(&host, [&got](const SourceFrameView& v) { got.push_back(v); });
			host.Owner(&host).Configure(false);
			Tap::SetStoreDemand(&host, true, "LED drive");
			Assert::IsTrue(host.Enabled(), L"the demand turns the store on");
			Assert::IsTrue(host.RaisedByDemand());
			host.Worker().Stop();   // the raise started the worker; stopped so the arrival below is drained by the test
			Frame frame = Make(Kind::Uyvy, 1);
			VpFrameAnalysis::Host::AttachIfEnabled(frame.source);
			host.OnArrival(5, frame.source, FrameAnalysis::Rect{}, 0, false);
			host.Worker().Drain();
			Assert::IsTrue(host.Store().HasRecord(5), L"a record exists for the frame that arrived after the raise");
			Assert::AreEqual<size_t>(1, got.size(), L"and the tap's sink got the frame");
			Tap::SetStoreDemand(&host, false, std::string());
			Tap::Unregister(&host);
			host.Worker().Stop();
		}

		// The same without a tap sink and with a full-luma reader configured (Configure(false, true, true)): the shared plane is attached by
		// the Host's own flag. If this is RED (the flag is enabled AND readers, so a raised-only store attaches no plane), report it to
		// GF-P2b / A20-C5; no code is changed here.
		TEST_METHOD(ARaisedStoreWithTheConfigurationOffAttachesThePlaneForAConfiguredFullLumaReader)
		{
			VpFrameAnalysis::Host host;
			host.Owner(&host).Configure(false, true, true);
			Tap::SetStoreDemand(&host, true, "LED drive");
			Assert::IsTrue(host.Enabled(), L"the demand turns the store on");
			host.Worker().Stop();
			Frame frame = Make(Kind::Uyvy, 1);
			VpFrameAnalysis::Host::AttachIfEnabled(frame.source);
			Assert::IsTrue(frame.source.sharedLuma != nullptr, L"the shared plane is attached for the reader");
			host.OnArrival(5, frame.source, FrameAnalysis::Rect{}, 0, false);
			host.Worker().Drain();
			Assert::IsTrue(host.Store().HasRecord(5), L"a record exists for the frame that arrived after the raise");
			Tap::SetStoreDemand(&host, false, std::string());
			host.Worker().Stop();
		}

		// GF-P2b FIX-SF (R4 gate, T13): the shared-plane want is a per-Host share of a process-wide count. No tap sink is registered, so only
		// the Hosts' own wants attach the plane. One Host turning off never turns off another's; a destroyed Host releases its share; a
		// demand-raised store with full-luma readers shares the plane too.
		TEST_METHOD(TwoHostsShareThePlaneIndependentlyAndADestroyedHostReleasesItsShare)
		{
			Assert::IsFalse(Tap::AnyWants(), L"precondition: no tap sink registered");
			auto attached = []() {
				Frame f = Make(Kind::Uyvy, 1);
				VpFrameAnalysis::Host::AttachIfEnabled(f.source);
				return f.source.sharedLuma != nullptr;
			};
			Assert::IsFalse(attached(), L"precondition: nobody wants the plane");
			std::unique_ptr<VpFrameAnalysis::Host> a(new VpFrameAnalysis::Host());
			VpFrameAnalysis::Host b;
			a->Configure(true, true, false);
			b.Configure(true, true, false);
			Assert::IsTrue(attached(), L"(a) both want it");
			b.Configure(false, true, false);   // B turns off: A still wants the plane
			Assert::IsTrue(attached(), L"(a) A still wants it after B turned off");
			b.Configure(true, false, false);   // B on, no full-luma readers
			Assert::IsTrue(attached(), L"(b) still attached through A");
			a.reset();   // A destroyed while B wants no plane
			Assert::IsFalse(attached(), L"(c) a destroyed Host released its share");
			b.Configure(false);
			b.Worker().Stop();
			// (d) the demand path: a configured-off store raised by a demand, with a full-luma reader.
			VpFrameAnalysis::Host c;
			c.Configure(false, true, true);
			Assert::IsFalse(attached(), L"(d) configured off, no demand: no plane");
			c.ApplyDemand(true);
			Assert::IsTrue(attached(), L"(d) the demand-raised store shares the plane");
			c.ApplyDemand(false);
			Assert::IsFalse(attached(), L"(d) the demand withdrawn: no plane");
			c.Configure(false);
			c.Worker().Stop();
			Assert::IsFalse(attached(), L"every Host is off: the count is back to 0");
		}

		TEST_METHOD(ADemandMadeBeforeTheHostRegisteredIsAppliedAtRegistrationAndAGoneHostIsNeverCalled)
		{
			int key = 0;
			Tap::SetStoreDemand(&key, true, "LED drive");
			{
				VpFrameAnalysis::Host host;
				host.Owner(&key).Configure(false);
				Assert::IsTrue(host.Enabled(), L"the recorded demand reaches a Host that registers later");
				Assert::IsTrue(Tap::StoreRaisedByDemand(&key));
			}
			Assert::IsFalse(Tap::StoreRaisedByDemand(&key), L"the Host unregistered with itself");
			Tap::SetStoreDemand(&key, false, std::string());   // no Host: recorded only, nothing called
			Tap::SetStoreDemand(&key, true, "LED drive");
			Assert::IsTrue(Tap::StoreDemanded(&key));
			Tap::SetStoreDemand(&key, false, std::string());
			Assert::IsFalse(Tap::StoreDemanded(&key));
		}

		TEST_METHOD(NoDemandChangesNothing)
		{
			VpFrameAnalysis::Host host;
			host.Owner(&host).Configure(false);
			Tap::SetStoreDemand(&host, false, std::string());
			Assert::IsFalse(host.Enabled(), L"no consumer: nothing turns on");
			Assert::IsFalse(Tap::StoreDemanded(&host));
		}
	};
}
