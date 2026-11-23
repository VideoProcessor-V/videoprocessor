// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
//
// P10 D5 phase A (refs/118): the P13 source ring (sized from the queue window, keyed by (seq, epoch), graveyard release at
// Begin, demand gate), the non-mutating EarlyUploadHandoff::PeekAttached, and the lane tap-source decision (every cause
// visible). CPU only.
#include "pch.h"
#include "CppUnitTest.h"
#include <VP-V/core/QueueWindow.h>
#include <VP-V/core/RingContract.h>
#include <VP-V/core/SourceRingFeed.h>
#include <VP-V/modules/input/capture/SourceRing.h>
#include <VP-V/modules/input/capture/EarlyUploadHandoff.h>
#include <VP-V/modules/input/source_analysis/lane/shared/TapSource.h>
#include <VP-V/modules/input/source_analysis/SourceRingPin.h>

#include <chrono>
#include <future>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

using namespace Microsoft::VisualStudio::CppUnitTestFramework;

namespace VideoProcessorTest
{
	TEST_CLASS(SourceRingTests)
	{
	public:
		using Ring = Vpv::Capture::SourceRing;
		using Tap = Vpv::TapSource::Cause;

		// One queued frame as the walk hands it over: seq and an OPAQUE extension slot holding the capture payload (the ring decodes it).
		static VpSkeleton::AheadFrame Frame(uint64_t seq, std::shared_ptr<EarlyUploadHandoff> h,
			VideoFrameEncoding enc = VideoFrameEncoding::P010, uint32_t rowBytes = 3840 * 2, int height = 2160,
			bool dvTunnel = false)
		{
			auto payload = std::make_shared<Vpv::Capture::PayloadExtension>();
			payload->earlyHandoff = h;
			payload->rawEncoding = enc;
			payload->rawRowBytes = rowBytes;
			payload->earlyHeight = height;
			payload->dvTunnelRaw = dvTunnel;
			VpSkeleton::AheadFrame f;
			f.seq = seq;
			f.width = 3840;
			f.height = height;
			f.extension = std::static_pointer_cast<void>(payload);
			return f;
		}

		// Publishes the window the terms derive (R render-ahead, P present lag, H history, L lane lead, W look-ahead walk)
		// into the ring's board, as the skeleton does at every Begin. Returns the window.
		static VpSkeleton::QueueWindow Publish(Ring& r, uint16_t R, uint16_t P, uint16_t H, uint16_t L, uint16_t W, uint64_t epoch)
		{
			VpSkeleton::QueueWindowTerms t;
			t.renderAhead = R;
			t.presentLag = P;
			t.history = H;
			t.laneLead = L;
			t.lookAheadWalk = W;
			t.wired = VpSkeleton::QueueWindowTerms::kRenderAhead | VpSkeleton::QueueWindowTerms::kPresentLag |
				VpSkeleton::QueueWindowTerms::kHistory | VpSkeleton::QueueWindowTerms::kLaneLead |
				VpSkeleton::QueueWindowTerms::kLookAhead;
			const VpSkeleton::QueueWindow w = VpSkeleton::DeriveWindow(t, 100, epoch);
			r.Board().Publish(w);
			return w;
		}

		TEST_METHOD(AnUnwantedRingTakesNothingAndAnyWantedFollowsTheDemand)
		{
			const bool before = Ring::AnyWanted();
			Ring r;
			Publish(r, 1, 0, 1, 1, 1, 1);
			auto h = std::make_shared<EarlyUploadHandoff>();
			const VpSkeleton::AheadFrame s[1] = { Frame(1, h) };
			r.Update(s, 1);
			Assert::AreEqual<size_t>(0, r.GetStats().entries);
			r.SetWanted(true);
			Assert::IsTrue(Ring::AnyWanted());
			r.SetWanted(true);   // idempotent
			r.SetWanted(false);
			Assert::IsTrue(Ring::AnyWanted() == before, L"one wanted ring, counted once");
		}

		TEST_METHOD(ARingWithoutAWindowHoldsNothingAndCountsIt)
		{
			Ring r;
			r.SetWanted(true);
			auto h = std::make_shared<EarlyUploadHandoff>();
			const VpSkeleton::AheadFrame s[1] = { Frame(1, h) };
			r.Update(s, 1);
			Assert::AreEqual<size_t>(0, r.GetStats().entries);
			Assert::AreEqual<uint64_t>(1, r.GetStats().noWindow);
			Assert::AreEqual<size_t>(0, r.GetStats().capacity);
			r.SetWanted(false);
		}

		TEST_METHOD(EntriesAreKeyedBySeqAndEpochAndLeaveToTheGraveyardUntilBegin)
		{
			Ring r;
			r.SetWanted(true);
			Publish(r, 1, 0, 1, 2, 1, 1);
			auto a = std::make_shared<EarlyUploadHandoff>();
			auto b = std::make_shared<EarlyUploadHandoff>();
			const VpSkeleton::AheadFrame first[2] = { Frame(10, a), Frame(11, b) };
			r.Update(first, 2);
			Assert::AreEqual<size_t>(2, r.GetStats().entries);
			Ring::Entry got;
			Assert::IsTrue(r.Find(10, 1, got) && got.earlyHandoff == std::static_pointer_cast<void>(a));
			got = Ring::Entry();
			Assert::IsFalse(r.Find(10, 2, got), L"the same seq in another epoch is another frame");
			// frame 10 leaves the queue, frame 12 arrives
			auto c = std::make_shared<EarlyUploadHandoff>();
			const VpSkeleton::AheadFrame second[2] = { Frame(11, b), Frame(12, c) };
			r.Update(second, 2);
			Assert::IsFalse(r.Find(10, 1, got));
			Assert::AreEqual<size_t>(1, r.GetStats().graveyard);
			const long held = a.use_count();
			Assert::IsTrue(held >= 2, L"nothing is destroyed under the queue lock: the graveyard still holds frame 10");
			Assert::AreEqual<size_t>(1, r.ReleaseGraveyard());
			Assert::IsTrue(a.use_count() == held - 1, L"the reference drops at Begin");
			Assert::AreEqual<size_t>(0, r.GetStats().graveyard, L"one Begin empties the graveyard");
			Assert::AreEqual<size_t>(0, r.GetStats().graveyard);
			// an empty pass clears the ring (the queue ran dry)
			r.Update(nullptr, 0);
			Assert::AreEqual<size_t>(0, r.GetStats().entries);
			r.ReleaseGraveyard();
			r.SetWanted(false);
		}

		TEST_METHOD(AFullRingCountsWhatItRefuses)
		{
			Ring r;
			r.SetWanted(true);
			const VpSkeleton::QueueWindow w = Publish(r, 2, 0, 1, 3, 2, 1);   // back 1 + 2, ahead 3 + 2: the window the old literal capacity matched
			const size_t capacity = 9;
			Assert::AreEqual<size_t>(VpSkeleton::RingWindowFrames(w.back, w.ahead), capacity, L"the literal is the P13 formula's value");
			auto h = std::make_shared<EarlyUploadHandoff>();
			std::vector<VpSkeleton::AheadFrame> s;
			for (size_t i = 0; i < capacity + 2; ++i)
				s.push_back(Frame(i + 1, h));
			r.Update(s.data(), s.size());
			Assert::AreEqual<size_t>(capacity, r.GetStats().entries);
			Assert::AreEqual<uint64_t>(2, r.GetStats().overCapacity);
			r.SetWanted(false);
			Assert::AreEqual<size_t>(0, r.GetStats().entries);
			r.ReleaseGraveyard();
		}

		TEST_METHOD(TheCapacityFollowsTheBoardAcrossTheTermSweep)
		{
			const uint16_t Rs[] = { 0, 1, 3 };
			const uint16_t Ls[] = { 0, 2, 4 };
			const uint16_t Ws[] = { 0, 1, 2 };
			for (uint16_t R : Rs)
				for (uint16_t L : Ls)
					for (uint16_t W : Ws)
					{
						Ring r;
						r.SetWanted(true);
						const VpSkeleton::QueueWindow w = Publish(r, R, 0, 1, L, W, 1);
						const size_t expect = VpSkeleton::RingWindowFrames(w.back, w.ahead);
						Assert::AreEqual<size_t>(static_cast<size_t>(R) + 1 + 1 + L + W, expect, L"back + 1 + lead from the terms");
						auto h = std::make_shared<EarlyUploadHandoff>();
						std::vector<VpSkeleton::AheadFrame> s;
						for (size_t i = 0; i < expect + 3; ++i)
							s.push_back(Frame(i + 1, h));
						r.Update(s.data(), s.size());
						Assert::AreEqual<size_t>(expect, r.GetStats().capacity);
						Assert::AreEqual<size_t>(expect, r.GetStats().entries);
						Assert::AreEqual<uint64_t>(3, r.GetStats().overCapacity);
						r.SetWanted(false);
						r.ReleaseGraveyard();
					}
		}

		TEST_METHOD(AWindowChangeResizesTheNextPass)
		{
			Ring r;
			r.SetWanted(true);
			auto h = std::make_shared<EarlyUploadHandoff>();
			std::vector<VpSkeleton::AheadFrame> s;
			for (size_t i = 0; i < 12; ++i)
				s.push_back(Frame(i + 1, h));
			Publish(r, 0, 0, 0, 1, 0, 1);   // 0 + 1 + 1 = 2
			r.Update(s.data(), s.size());
			Assert::AreEqual<size_t>(2, r.GetStats().entries);
			Publish(r, 0, 0, 0, 4, 2, 1);   // 0 + 1 + 6 = 7, the same epoch
			r.Update(s.data(), s.size());
			Assert::AreEqual<size_t>(7, r.GetStats().entries);
			Assert::AreEqual<size_t>(7, r.GetStats().capacity);
			r.SetWanted(false);
			r.ReleaseGraveyard();
		}

		TEST_METHOD(EveryFactKindEmptiesTheRingAndTheGraveyardWaitsForBegin)
		{
			// Each queue fact kind (flush, discontinuity, format change) bumps the window epoch (QueueWindow.epoch); the ring
			// reads the larger of that epoch and the queue-fact epoch (OnQueueFact), so one epoch change per kind is the whole contract.
			const VpSourceRing::Invalidation kinds[] = { VpSourceRing::Invalidation::Flush, VpSourceRing::Invalidation::Discontinuity,
				VpSourceRing::Invalidation::FormatChange };
			Ring r;
			r.SetWanted(true);
			auto h = std::make_shared<EarlyUploadHandoff>();
			const VpSkeleton::AheadFrame s[3] = { Frame(1, h), Frame(2, h), Frame(3, h) };
			uint64_t epoch = 1;
			Publish(r, 1, 0, 1, 2, 1, epoch);
			r.Update(s, 3);
			Assert::AreEqual<size_t>(3, r.GetStats().entries);
			uint64_t flushes = r.GetStats().epochFlushes;
			for (size_t k = 0; k < sizeof(kinds) / sizeof(kinds[0]); ++k)
			{
				++epoch;
				Publish(r, 1, 0, 1, 2, 1, epoch);
				Ring::Entry got;
				r.Update(nullptr, 0);   // the pass after the fact: the old entries are gone, in the graveyard
				Assert::AreEqual<size_t>(0, r.GetStats().entries);
				Assert::IsFalse(r.Find(1, epoch - 1, got));
				Assert::IsTrue(r.GetStats().graveyard > 0, L"nothing is released before Begin");
				Assert::AreEqual<uint64_t>(++flushes, r.GetStats().epochFlushes);
				r.Update(s, 3);   // the same seqs again are NEW entries in the new epoch
				Assert::AreEqual<size_t>(3, r.GetStats().entries);
				Assert::IsFalse(r.Find(1, epoch - 1, got), L"the old epoch's key no longer finds");
				Assert::IsTrue(r.Find(1, epoch, got));
				Assert::IsTrue(r.ReleaseGraveyard() > 0);
				Assert::AreEqual<size_t>(0, r.GetStats().graveyard);
			}
			r.SetWanted(false);
			r.ReleaseGraveyard();
		}

		TEST_METHOD(AnEpochChangeWithFramesListedMovesThemAllToTheGraveyard)
		{
			Ring r;
			r.SetWanted(true);
			auto h = std::make_shared<EarlyUploadHandoff>();
			const VpSkeleton::AheadFrame s[2] = { Frame(1, h), Frame(2, h) };
			Publish(r, 1, 0, 1, 2, 1, 1);
			r.Update(s, 2);
			Publish(r, 1, 0, 1, 2, 1, 2);
			r.Update(s, 2);
			const Ring::Stats st = r.GetStats();
			Assert::AreEqual<uint64_t>(2, st.epochFlushes);   // 0 -> 1 on the first pass (empty ring), 1 -> 2 here
			Assert::AreEqual<size_t>(2, st.graveyard, L"the two old entries wait in the graveyard");
			Assert::AreEqual<size_t>(2, st.entries);
			Assert::AreEqual<uint64_t>(2, st.epoch);
			Assert::AreEqual<size_t>(2, r.ReleaseGraveyard());
			r.SetWanted(false);
			r.ReleaseGraveyard();
		}

		TEST_METHOD(AQueueFactBetweenTwoFeedsEmptiesTheRingOnTheNextFeed)
		{
			Ring r;
			r.SetWanted(true);
			auto h = std::make_shared<EarlyUploadHandoff>();
			const VpSkeleton::AheadFrame s[2] = { Frame(1, h), Frame(2, h) };
			Publish(r, 1, 0, 1, 2, 1, 5);
			r.Update(s, 2);
			Assert::AreEqual<size_t>(2, r.GetStats().entries);
			const uint64_t flushes = r.GetStats().epochFlushes;
			r.OnQueueFact(6);   // the fact (memo epoch 6), NO Publish: the board still says epoch 5
			r.OnQueueFact(4);   // a lower value never moves the epoch back
			r.Update(s, 2);
			const Ring::Stats st = r.GetStats();
			Assert::AreEqual<uint64_t>(flushes + 1, st.epochFlushes, L"one flush for the fact");
			Assert::AreEqual<uint64_t>(6, st.epoch);
			Assert::AreEqual<size_t>(2, st.graveyard, L"the pre-fact entries wait for Begin");
			Ring::Entry got;
			Assert::IsFalse(r.Find(1, 5, got), L"the old epoch no longer finds");
			Assert::IsTrue(r.Find(1, 6, got));
			Assert::AreEqual<size_t>(2, r.ReleaseGraveyard());
			Assert::AreEqual<size_t>(0, r.GetStats().graveyard, L"one Begin empties the graveyard");
			r.SetWanted(false);
			r.ReleaseGraveyard();
		}

		TEST_METHOD(ASeqListedAgainWithANewHandoffReplacesTheEntry)
		{
			Ring r;
			r.SetWanted(true);
			Publish(r, 1, 0, 1, 2, 1, 1);
			auto oldH = std::make_shared<EarlyUploadHandoff>();
			auto newH = std::make_shared<EarlyUploadHandoff>();
			const VpSkeleton::AheadFrame first[1] = { Frame(7, oldH) };
			r.Update(first, 1);
			const VpSkeleton::AheadFrame again[1] = { Frame(7, newH) };   // a restarted sequence re-used seq 7
			r.Update(again, 1);
			Ring::Entry got;
			Assert::IsTrue(r.Find(7, 1, got));
			Assert::IsTrue(got.earlyHandoff == std::static_pointer_cast<void>(newH), L"the entry is the new frame's");
			got = Ring::Entry();
			Assert::AreEqual<size_t>(1, r.GetStats().entries);
			Assert::AreEqual<size_t>(1, r.GetStats().graveyard);
			const long held = oldH.use_count();
			Assert::IsTrue(held >= 2, L"the old hand-off waits in the graveyard");
			Assert::AreEqual<size_t>(1, r.ReleaseGraveyard());
			Assert::IsTrue(oldH.use_count() == held - 1, L"only ReleaseGraveyard drops it");
			r.SetWanted(false);
			r.ReleaseGraveyard();
		}

		TEST_METHOD(TheLockOrderIsQueueThenRingAndTheReaderTakesOnlyTheRing)
		{
			// The producer holds a stand-in queue mutex and then updates the ring (queue -> ring); the reader takes only the
			// ring's mutex. Neither takes the other order, so both finish.
			Ring r;
			r.SetWanted(true);
			Publish(r, 1, 0, 1, 2, 1, 1);
			std::mutex queueMutex;
			auto h = std::make_shared<EarlyUploadHandoff>();
			const VpSkeleton::AheadFrame s[3] = { Frame(1, h), Frame(2, h), Frame(3, h) };
			auto producer = std::async(std::launch::async, [&]() {
				for (int i = 0; i < 2000; ++i)
				{
					std::lock_guard<std::mutex> q(queueMutex);
					r.Update(i % 2 ? s : nullptr, i % 2 ? 3 : 0);
				}
			});
			auto reader = std::async(std::launch::async, [&]() {
				Ring::Entry e;
				for (int i = 0; i < 2000; ++i)
				{
					r.Find(1, 1, e);
					r.GetStats();
					r.ReleaseGraveyard();   // the Begin: no queue lock
				}
			});
			Assert::IsTrue(producer.wait_for(std::chrono::seconds(30)) == std::future_status::ready, L"no deadlock");
			Assert::IsTrue(reader.wait_for(std::chrono::seconds(30)) == std::future_status::ready, L"no deadlock");
			r.SetWanted(false);
			r.ReleaseGraveyard();
		}

		TEST_METHOD(PeekAttachedNeverAbandonsOrConsumesTheRendersTake)
		{
			EarlyUploadHandoff pending;
			EarlyUploadHandoff::State st{};
			Assert::IsTrue(pending.PeekAttached(&st) == nullptr);
			Assert::IsTrue(st == EarlyUploadHandoff::State::Pending);
			Assert::IsTrue(pending.BeginUpload(), L"a peek left the upload runnable (no Abandon)");
			EarlyUploadHandoff ready;
			auto slot = std::make_shared<int>(5);
			Assert::IsTrue(ready.BeginUpload());
			ready.Finish(slot);
			Assert::IsTrue(ready.PeekAttached(&st) == slot && st == EarlyUploadHandoff::State::Attached);
			Assert::IsTrue(ready.PeekAttached() == slot);
			EarlyUploadHandoff::Outcome o{};
			bool first = false;
			Assert::IsTrue(ready.Take(&o, &first) == slot);
			Assert::IsTrue(first, L"the render's first Take is still the first (peeks set no taken flag)");
			Assert::IsTrue(o == EarlyUploadHandoff::Outcome::Early);
			// Abandon after peeks behaves as without them: the render's Take abandons a Pending hand-off, the peeks did not
			EarlyUploadHandoff late;
			late.PeekAttached();
			late.PeekAttached();
			EarlyUploadHandoff::Outcome lo{};
			Assert::IsTrue(late.Take(&lo) == nullptr);
			Assert::IsFalse(late.BeginUpload(), L"Take abandoned it; the peeks did not");
		}

		TEST_METHOD(TheTapDecisionNamesEveryAbsentCause)
		{
			Ring r;
			r.SetWanted(true);
			Publish(r, 1, 0, 1, 4, 4, 1);
			using Vpv::TapSource::Mode;
			using Vpv::TapSource::Pick;
			Pick p;
			Assert::IsTrue(Vpv::TapSource::Decide(Mode::Cpu, r, 1, 1, false, 3840, 2160, p) == Tap::SettingCpu);
			Assert::IsTrue(Vpv::TapSource::Decide(Mode::Auto, r, 1, 1, false, 3840, 2160, p) == Tap::NotInRing);

			const VpSkeleton::AheadFrame noRaw = Frame(2, nullptr);
			auto pendingH = std::make_shared<EarlyUploadHandoff>();
			auto okH = std::make_shared<EarlyUploadHandoff>();
			Assert::IsTrue(okH->BeginUpload());
			auto texture = std::make_shared<int>(1);
			okH->Finish(texture);
			auto abandoned = std::make_shared<EarlyUploadHandoff>();
			EarlyUploadHandoff::Outcome o{};
			abandoned->Take(&o);   // Pending -> Abandoned
			const VpSkeleton::AheadFrame dv = Frame(10, okH, VideoFrameEncoding::P010, 3840 * 2, 2160, true);
			const VpSkeleton::AheadFrame dvRgb = Frame(11, okH, VideoFrameEncoding::BGRA_8BIT, 3840 * 2, 2160, true);
			const VpSkeleton::AheadFrame s[11] = {
				noRaw,
				Frame(3, pendingH),
				Frame(4, okH),
				Frame(5, abandoned),
				Frame(6, okH, VideoFrameEncoding::BGRA_8BIT),
				Frame(7, okH, VideoFrameEncoding::NV12),
				Frame(8, okH, VideoFrameEncoding::P010, 64, 2160),
				dv,
				dvRgb,
				Frame(9, okH, VideoFrameEncoding::V210, 3840 * 16 / 6 + 16, 2160),
				Frame(12, okH) };
			r.Update(s, 11);
			Assert::IsTrue(Vpv::TapSource::Decide(Mode::Auto, r, 2, 1, false, 3840, 2160, p) == Tap::NoRawPayload);
			Assert::IsTrue(Vpv::TapSource::Decide(Mode::Auto, r, 3, 1, false, 3840, 2160, p) == Tap::UploadPending);
			Assert::IsTrue(Vpv::TapSource::Decide(Mode::Auto, r, 4, 1, false, 3840, 2160, p) == Tap::None);
			Assert::IsTrue(p.slot == texture);
			Assert::IsTrue(Vpv::TapSource::Decide(Mode::Auto, r, 4, 1, true, 3840, 2160, p) == Tap::FullRange);
			Assert::IsTrue(Vpv::TapSource::Decide(Mode::Auto, r, 5, 1, false, 3840, 2160, p) == Tap::NoEarlySlot);
			Assert::IsTrue(Vpv::TapSource::Decide(Mode::Auto, r, 6, 1, false, 3840, 2160, p) == Tap::FormatRgb);
			Assert::IsTrue(Vpv::TapSource::Decide(Mode::Auto, r, 7, 1, false, 3840, 2160, p) == Tap::FormatOther);
			Assert::IsTrue(Vpv::TapSource::Decide(Mode::Auto, r, 8, 1, false, 3840, 2160, p) == Tap::SizeMismatch);
			Assert::IsTrue(Vpv::TapSource::Decide(Mode::Auto, r, 4, 1, false, 3840, 4000, p) == Tap::SizeMismatch, L"a short slot");
			// the Dolby Vision tunnel is named before the format rule (its raw payload would read as rgb / another format)
			Assert::IsTrue(Vpv::TapSource::Decide(Mode::Auto, r, 10, 1, false, 3840, 2160, p) == Tap::FormatDolbyVision);
			Assert::IsTrue(Vpv::TapSource::Decide(Mode::Auto, r, 11, 1, false, 3840, 2160, p) == Tap::FormatDolbyVision,
				L"an rgb-looking tunnel payload is the tunnel, not rgb");
			Assert::IsTrue(Vpv::TapSource::Decide(Mode::Auto, r, 4, 2, false, 3840, 2160, p) == Tap::NotInRing, L"another epoch");
			// the decisions only peeked: the pending hand-off is still pending, no Take was consumed
			Assert::IsTrue(pendingH->Peek() == EarlyUploadHandoff::State::Pending);
			r.SetWanted(false);
			r.ReleaseGraveyard();
		}

		TEST_METHOD(TheFeedRegistryFindsTheRingPerHost)
		{
			int hostA = 0, hostB = 0;
			auto ring = std::make_shared<Ring>();
			VpSkeleton::SourceRingFeeds::Register(&hostA, ring);
			Assert::IsTrue(VpSkeleton::SourceRingFeeds::Find(&hostA) == ring);
			Assert::IsTrue(VpSkeleton::SourceRingFeeds::Find(&hostB) == nullptr, L"another host has no feed");
			VpSkeleton::SourceRingFeeds::Unregister(&hostA);
			Assert::IsTrue(VpSkeleton::SourceRingFeeds::Find(&hostA) == nullptr);
		}

		TEST_METHOD(TheStatusAndDumpShowTheOverCapacityRing)
		{
			Ring::Stats st;
			st.capacity = 9;
			st.overCapacity = 4;
			const std::string text = Vpv::TapSource::DescribeRing(st);
			Assert::IsTrue(text.find("over capacity 4") != std::string::npos);
			Assert::IsTrue(text.find("holds 9") != std::string::npos);
		}

		TEST_METHOD(TheCountersAndTheDumpTextShowEveryCause)
		{
			Vpv::TapSource::Counters c;
			Assert::AreEqual<uint64_t>(0, c.Fallbacks());
			c.Count(Tap::None);
			c.Count(Tap::UploadPending);
			c.Count(Tap::UploadPending);
			c.Count(Tap::FullRange);
			Assert::AreEqual<uint64_t>(3, c.Fallbacks());
			Assert::IsTrue(c.Last() == Tap::FullRange);
			const std::string text = Vpv::TapSource::Describe(c);
			Assert::IsTrue(text.find("gpu 1") != std::string::npos);
			Assert::IsTrue(text.find("upload pending 2") != std::string::npos);
			Assert::IsTrue(text.find("full range 1") != std::string::npos);
			Assert::IsTrue(text.find("last: full range") != std::string::npos);
		}
	};
}
