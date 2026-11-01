// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
// Built without the test project's precompiled header: keep windows.h from defining min/max.
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include "CppUnitTest.h"

// repair.grain_split as a skeleton node (GrainNode.h over the core SlotSet): the fill rule (lane publishes before
// the seal, else the first consumer fills live once, later consumers reuse), the graceful fallbacks (Missing,
// Stale, WrongSize, Shed) and the visible reader fallback. CPU only: the compute is a fake.
//
// Fresh SlotSets only accept a lane claim for the first frames (seq <= slot index), so every test first
// discards frame seq - ring (the render dropped it): the slot then holds a released earlier frame, exactly the
// steady state the lane sees, and frames >= 10 are claimable.

#include "../../../../tests/SettingsTestSupport.h"
#include <VP-V/modules/repair/grain_split/GrainNode.h>

#include <cstdint>
#include <string>
#include <vector>

using namespace Microsoft::VisualStudio::CppUnitTestFramework;

namespace GrainNodeTestsNs
{
	using namespace GrainSplit;
	using VpSkeleton::ClaimResult;
	using VpSkeleton::PublishResult;
	using VpSkeleton::SealOutcome;
	using VpSkeleton::SealReason;
	using VpSkeleton::SlotState;

	constexpr uint16_t kRing = 4;
	constexpr uint16_t kLive = 4;

	struct FakeCompute
	{
		int calls = 0;
		bool fail = false;
		std::vector<GrainTarget> targets;
		std::vector<uint64_t> seqs;
		bool Fill(uint64_t seq, const GrainTarget& t)
		{
			++calls;
			targets.push_back(t);
			seqs.push_back(seq);
			return !fail;
		}
	};

	GrainDesc Desc()
	{
		GrainDesc d;
		d.w = 1920;
		d.h = 1080;
		d.B = 16;
		d.scale = 1.0;
		d.codeUnit = 1.0 / 1023.0;
		d.component = 0;
		return d;
	}

	// The render dropped frame seq - ring: the slot of `seq` holds a released earlier frame.
	void Warm(GrainNode& node, uint64_t seq)
	{
		node.Slots().Discard(seq - kRing);
	}

	const char* const kConsumers[3] = { "temporal_detail", "chroma_cfl", "region_evidence" };

	bool Contains(const std::string& s, const char* part) { return s.find(part) != std::string::npos; }

	TEST_CLASS(GrainNodeTests)
	{
	public:
		TEST_METHOD(PublishedBeforeSealIsReusedByEveryConsumer)
		{
			GrainNode node(VpSkeleton::kP10GrainSplit, kRing, kLive, Desc());
			node.SetLaneGranted(true);
			Warm(node, 10);
			FakeCompute lane, render;

			Assert::IsTrue(node.LanePrecompute(10, lane) == LaneOutcome::Published, L"lane publishes before the seal");
			Assert::AreEqual(1, lane.calls, L"one lane compute");
			Assert::IsTrue(!lane.targets[0].live, L"the lane writes the ring buffer, not a live buffer");

			const VpSkeleton::SealResult s = node.Seal(10);
			Assert::IsTrue(s.outcome == SealOutcome::Sealed && s.reason == SealReason::Accepted, L"the seal accepts the published result");

			int index = -1;
			for (int i = 0; i < 3; ++i)
			{
				const GrainRead r = node.Acquire(10, Desc(), kConsumers[i], render);
				Assert::IsTrue(r.status == GrainStatus::Ring, L"consumer reads the ring buffer");
				Assert::IsTrue(!r.filledHere, L"no consumer fills");
				if (i == 0)
					index = r.index;
				Assert::AreEqual(index, r.index, L"every consumer gets the same buffer");
			}
			Assert::AreEqual(int(10 % kRing), index, L"the ring slot of frame 10");
			Assert::AreEqual(0, render.calls, L"the render compute was never called");
			Assert::AreEqual(uint64_t(0), node.Counters().liveFills, L"no live fills");
			Assert::AreEqual(uint64_t(3), node.Counters().ringReads, L"three ring reads");

			VpSkeleton::LaneTicket t;
			Assert::IsTrue(node.LaneClaim(10, &t) != ClaimResult::Claimed, L"locked after the seal: no claim");
			Assert::IsTrue(node.LanePrecompute(10, lane) == LaneOutcome::Skipped, L"a precompute after the seal is skipped");
			Assert::AreEqual(1, lane.calls, L"no compute after the seal");
		}

		TEST_METHOD(LatePrecomputeIsDiscardedAndFirstConsumerFillsLive)
		{
			GrainNode node(VpSkeleton::kP10GrainSplit, kRing, kLive, Desc());
			node.SetLaneGranted(true);
			Warm(node, 10);
			FakeCompute lane, live;

			VpSkeleton::LaneTicket t;
			Assert::IsTrue(node.LaneClaim(10, &t) == ClaimResult::Claimed, L"lane claims");
			Assert::IsTrue(node.LaneWrite(t, lane), L"lane writes");
			Assert::AreEqual(1, lane.calls, L"lane computed once");

			const VpSkeleton::SealResult s = node.Seal(10);
			Assert::IsTrue(s.outcome == SealOutcome::NeedsLive && s.reason == SealReason::LaneLate, L"seal: needs live, lane late");

			const GrainRead a = node.Acquire(10, Desc(), "temporal_detail", live);
			Assert::IsTrue(a.status == GrainStatus::Live && a.filledHere, L"first consumer fills live");
			Assert::AreEqual(1, live.calls, L"one live compute");
			Assert::IsTrue(live.targets[0].live, L"live target");

			const GrainRead b = node.Acquire(10, Desc(), "chroma_cfl", live);
			Assert::IsTrue(b.status == GrainStatus::Live && !b.filledHere, L"second consumer reuses");
			Assert::AreEqual(a.index, b.index, L"same live buffer");
			Assert::AreEqual(1, live.calls, L"still one live compute");

			Assert::IsTrue(node.LanePublish(t) == PublishResult::Late, L"the late lane result is discarded");
			const GrainRead c = node.Acquire(10, Desc(), "region_evidence", live);
			Assert::IsTrue(c.status == GrainStatus::Live && c.index == a.index, L"the live buffer is unchanged by the late publish");
			Assert::AreEqual(1, live.calls, L"no second compute after the late publish");
			Assert::AreEqual(uint64_t(1), node.Slots().Counters().lateDiscards.load(), L"one late discard");
			Assert::IsTrue(node.LastFiller() == "temporal_detail", L"the first consumer was the filler");
		}

		TEST_METHOD(LiveFillByFirstConsumerThenReuse)
		{
			GrainNode node(VpSkeleton::kP10GrainSplit, kRing, kLive, Desc());
			node.SetLaneGranted(false);
			Warm(node, 10);
			FakeCompute live;

			const VpSkeleton::SealResult s = node.Seal(10);
			Assert::IsTrue(s.outcome == SealOutcome::NeedsLive, L"nothing published: needs live");
			Assert::IsTrue(s.reason == SealReason::InRenderOnly || s.reason == SealReason::LaneMissing, L"in render only / lane missing");

			const GrainRead a = node.Acquire(10, Desc(), kConsumers[0], live);
			Assert::IsTrue(a.status == GrainStatus::Live && a.filledHere, L"first consumer fills");
			Assert::AreEqual(1, live.calls, L"one fill");
			for (int i = 1; i < 3; ++i)
			{
				const GrainRead r = node.Acquire(10, Desc(), kConsumers[i], live);
				Assert::IsTrue(r.status == GrainStatus::Live && !r.filledHere && r.index == a.index, L"later consumers reuse");
			}
			Assert::AreEqual(1, live.calls, L"still one fill");
			Assert::AreEqual(uint64_t(2), node.Counters().liveReuses, L"two reuses");

			Assert::IsTrue(node.Release(10), L"frame 10 released");
			Warm(node, 11);
			const GrainRead n = node.Acquire(11, Desc(), kConsumers[0], live);
			Assert::IsTrue(n.status == GrainStatus::Live && n.filledHere, L"frame 11 fills independently");
			Assert::AreEqual(2, live.calls, L"two fills in total");
			Assert::IsTrue(live.seqs[1] == 11, L"the second fill is for frame 11");
		}

		TEST_METHOD(FailedLiveFillIsNotRetriedInTheFrame)
		{
			GrainNode node(VpSkeleton::kP10GrainSplit, kRing, kLive, Desc());
			node.SetLaneGranted(false);
			Warm(node, 10);
			FakeCompute live;
			live.fail = true;

			const GrainRead a = node.Acquire(10, Desc(), kConsumers[0], live);
			Assert::IsTrue(a.status == GrainStatus::Missing, L"failed fill: Missing");
			Assert::AreEqual(uint64_t(1), node.Counters().fillFailures, L"one fill failure");
			Assert::AreEqual(1, live.calls, L"one attempt");

			const GrainRead b = node.Acquire(10, Desc(), kConsumers[1], live);
			Assert::IsTrue(b.status == GrainStatus::Missing, L"second consumer: Missing");
			Assert::AreEqual(1, live.calls, L"no second producer in the frame");

			Assert::IsTrue(node.Release(10), L"frame released");
			Warm(node, 11);
			live.fail = false;
			const GrainRead n = node.Acquire(11, Desc(), kConsumers[0], live);
			Assert::IsTrue(n.status == GrainStatus::Live && n.filledHere, L"recovers on the next frame");
			Assert::AreEqual(2, live.calls, L"the next frame computes");
		}

		TEST_METHOD(WrongSizeStaleShedPerConsumer)
		{
			for (int i = 0; i < 3; ++i)
			{
				const char* name = kConsumers[i];
				GrainNode node(VpSkeleton::kP10GrainSplit, kRing, kLive, Desc());
				node.SetLaneGranted(false);
				Warm(node, 10);
				FakeCompute fake;

				// (a) WrongSize: each descriptor field differing.
				GrainDesc d[5] = { Desc(), Desc(), Desc(), Desc(), Desc() };
				d[0].w += 2;
				d[1].codeUnit = 1.0 / 255.0;
				d[2].component = 1;
				d[3].B = 8;
				d[4].scale = 2.0;
				for (int k = 0; k < 5; ++k)
				{
					const GrainRead r = node.Acquire(10, d[k], name, fake);
					Assert::IsTrue(r.status == GrainStatus::WrongSize, L"mismatched plane: WrongSize");
				}
				Assert::AreEqual(0, fake.calls, L"WrongSize computes nothing");
				Assert::AreEqual(uint64_t(5), node.Counters().wrongSize, L"five wrong-size reads");

				// (b) Shed and recovery.
				node.SetShed(true);
				const GrainRead sh = node.Acquire(10, Desc(), name, fake);
				Assert::IsTrue(sh.status == GrainStatus::Shed, L"shed: Shed");
				Assert::AreEqual(0, fake.calls, L"Shed computes nothing");
				VpSkeleton::LaneTicket t;
				Assert::IsTrue(node.LaneClaim(10, &t) != ClaimResult::Claimed, L"a shed product is not precomputed");
				node.SetShed(false);
				const GrainRead ok = node.Acquire(10, Desc(), name, fake);
				Assert::IsTrue(ok.status == GrainStatus::Live && ok.filledHere, L"after SetShed(false) the product is normal again");
				Assert::AreEqual(1, fake.calls, L"one fill after recovery");

				// (c) Stale. Frame 10 is live and NOT released, so slot 2 still holds frame 10.
				// Frame 14 maps to the same slot: Seal sees an older frame holding it -> Unavailable/Overrun,
				// which Acquire reports as Stale. Frame 6 is older than the slot's frame: OrderError, also Stale.
				// Neither may return Ring/Live, and frame 10's data stays intact.
				const GrainRead s14 = node.Acquire(14, Desc(), name, fake);
				Assert::IsTrue(s14.status == GrainStatus::Stale, L"frame 14 onto a held slot: Stale");
				const GrainRead s6 = node.Acquire(6, Desc(), name, fake);
				Assert::IsTrue(s6.status == GrainStatus::Stale, L"frame 6 older than the slot's frame: Stale");
				Assert::AreEqual(1, fake.calls, L"stale reads compute nothing");
				const GrainRead again = node.Acquire(10, Desc(), name, fake);
				Assert::IsTrue(again.status == GrainStatus::Live && again.index == ok.index && !again.filledHere, L"frame 10 still intact");
			}
		}

		TEST_METHOD(ReaderFallbackIsVisibleAndRecovers)
		{
			const GrainStatus bad[4] = { GrainStatus::Missing, GrainStatus::Stale, GrainStatus::WrongSize, GrainStatus::Shed };
			for (int i = 0; i < 3; ++i)
			{
				const std::string id = std::string("grain split (") + kConsumers[i] + ")";
				for (int k = 0; k < 4; ++k)
				{
					const FallbackCatalog::Decision d = ReaderFallback(kConsumers[i], bad[k]);
					Assert::IsTrue(d.engaged, L"a not-Ok status engages the fallback");
					Assert::IsTrue(d.id == id, L"stable id per consumer");
					Assert::IsTrue(!d.reason.empty() && Contains(d.reason, "estimating its own grain"), L"reason says what runs instead");
				}
				Assert::IsTrue(!ReaderFallback(kConsumers[i], GrainStatus::Ring).engaged, L"Ring releases");
				Assert::IsTrue(!ReaderFallback(kConsumers[i], GrainStatus::Live).engaged, L"Live releases");
				Assert::IsTrue(ReaderFallback(kConsumers[i], GrainStatus::Ring).id == id, L"released entry keeps the id");

				// Ring -> Missing -> Ring through a registry.
				FallbackStatus::Registry reg;
				FallbackCatalog::Sync(reg, ReaderFallback(kConsumers[i], GrainStatus::Ring));
				Assert::IsTrue(!reg.Engaged(id), L"not engaged while Ring");
				Assert::IsTrue(FallbackCatalog::Sync(reg, ReaderFallback(kConsumers[i], GrainStatus::Missing)), L"engaging changes the heading");
				Assert::IsTrue(reg.Engaged(id), L"engaged on Missing");
				FallbackCatalog::Sync(reg, ReaderFallback(kConsumers[i], GrainStatus::Ring));
				Assert::IsTrue(!reg.Engaged(id), L"released again on Ring (recovery)");
			}
		}

		TEST_METHOD(OneProducerPerSlotPerFrame)
		{
			{
				GrainNode node(VpSkeleton::kP10GrainSplit, kRing, kLive, Desc());
				node.SetLaneGranted(true);
				Warm(node, 10);
				FakeCompute lane, render;
				Assert::IsTrue(node.LanePrecompute(10, lane) == LaneOutcome::Published, L"published before the seal");
				node.Seal(10);
				for (int rep = 0; rep < 5; ++rep)
					for (int i = 0; i < 3; ++i)
						Assert::IsTrue(node.Acquire(10, Desc(), kConsumers[i], render).status == GrainStatus::Ring, L"ring every time");
				Assert::AreEqual(0, render.calls, L"the render never produces a published frame");
				Assert::AreEqual(uint64_t(1), node.Slots().Counters().sealed.load(), L"sealed==1");
				Assert::AreEqual(uint64_t(0), node.Slots().Counters().live.load(), L"live==0");
			}
			{
				GrainNode node(VpSkeleton::kP10GrainSplit, kRing, kLive, Desc());
				node.SetLaneGranted(true);
				Warm(node, 10);
				FakeCompute lane, live;
				VpSkeleton::LaneTicket t;
				Assert::IsTrue(node.LaneClaim(10, &t) == ClaimResult::Claimed, L"lane claims");
				Assert::IsTrue(node.LaneWrite(t, lane), L"lane writes");
				node.Seal(10);
				Assert::IsTrue(node.Acquire(10, Desc(), kConsumers[0], live).filledHere, L"live fill");
				Assert::IsTrue(node.LanePublish(t) == PublishResult::Late, L"lane publish after a live seal is Late");
				Assert::IsTrue(node.Slots().StateOf(10) == SlotState::Live, L"slot state Live");
				Assert::AreEqual(uint64_t(1), node.Slots().Counters().live.load(), L"live==1");
				Assert::AreEqual(uint64_t(0), node.Slots().Counters().sealed.load(), L"sealed==0");
				Assert::AreEqual(1, live.calls, L"exactly one live producer");
			}
		}

		TEST_METHOD(ProductAbsentLaneNotGranted)
		{
			GrainNode node(VpSkeleton::kP10GrainSplit, kRing, kLive, Desc());
			node.SetLaneGranted(false);
			Warm(node, 10);
			FakeCompute live;
			int index = -1;
			for (int i = 0; i < 3; ++i)
			{
				const GrainRead r = node.Acquire(10, Desc(), kConsumers[i], live);
				Assert::IsTrue(r.status == GrainStatus::Live, L"every consumer gets the frame");
				if (i == 0)
					index = r.index;
				Assert::AreEqual(index, r.index, L"same buffer");
			}
			Assert::AreEqual(1, live.calls, L"exactly one live fill");
			Assert::AreEqual(uint64_t(0), node.Slots().Counters().missing.load(), L"InRenderOnly is not a miss");
			Assert::AreEqual(uint64_t(0), node.Counters().missing, L"no node-level miss either");
		}

		TEST_METHOD(ShedIsADistinctLaneOutcome)
		{
			GrainNode node(VpSkeleton::kP10GrainSplit, kRing, kLive, Desc());
			node.SetLaneGranted(true);
			Warm(node, 10);
			FakeCompute lane;
			node.SetShed(true);
			Assert::IsTrue(node.LanePrecompute(10, lane) == LaneOutcome::Shed, L"a shed product reports Shed, not Skipped");
			Assert::AreEqual(0, lane.calls, L"nothing is computed for a shed product");
			Assert::AreEqual(uint64_t(1), node.Counters().shedClaims, L"the refused claim is counted");
			VpSkeleton::LaneTicket t;
			Assert::IsTrue(node.LaneClaim(10, &t) != ClaimResult::Claimed, L"no claim while shed");
			Assert::AreEqual(uint64_t(2), node.Counters().shedClaims, L"counted again");
			node.SetShed(false);
			Assert::IsTrue(node.LanePrecompute(10, lane) == LaneOutcome::Published, L"recovers when the governor returns the product");
		}

		TEST_METHOD(TwoFramesInFlightKeepTheirOwnSealResult)
		{
			// Frame 10 is sealed (published by the lane), frame 11 is sealed after it with NOTHING published (live fill).
			// Re-acquiring frame 10 afterwards still gets frame 10's ring buffer: the seal results are per ring slot.
			GrainNode node(VpSkeleton::kP10GrainSplit, kRing, kLive, Desc());
			node.SetLaneGranted(true);
			Warm(node, 10);
			Warm(node, 11);
			FakeCompute lane, render;
			Assert::IsTrue(node.LanePrecompute(10, lane) == LaneOutcome::Published, L"frame 10 published");
			node.Seal(10);
			const GrainRead a10 = node.Acquire(10, Desc(), "temporal_detail", render);
			node.Seal(11);   // no lane result for 11
			const GrainRead a11 = node.Acquire(11, Desc(), "temporal_detail", render);
			Assert::IsTrue(a10.status == GrainStatus::Ring && a11.status == GrainStatus::Live && a11.filledHere, L"10 ring, 11 live");
			Assert::AreEqual(1, render.calls, L"one live fill (frame 11)");
			const GrainRead again = node.Acquire(10, Desc(), "chroma_cfl", render);
			Assert::IsTrue(again.status == GrainStatus::Ring && again.index == a10.index && !again.filledHere,
				L"frame 10 still reads its own ring buffer after frame 11 was sealed");
			Assert::AreEqual(1, render.calls, L"no extra compute");
			const GrainRead again11 = node.Acquire(11, Desc(), "chroma_cfl", render);
			Assert::IsTrue(again11.status == GrainStatus::Live && again11.index == a11.index && !again11.filledHere, L"11 reused");
		}

		TEST_METHOD(AFailedFillStaysFailedPerFrame)
		{
			// Frame 10's live fill fails; frame 11 fails too: neither is retried (no second producer) and the failure of
			// 11 does not clear or replace the failure of 10.
			GrainNode node(VpSkeleton::kP10GrainSplit, kRing, kLive, Desc());
			Warm(node, 10);
			Warm(node, 11);
			FakeCompute render;
			render.fail = true;
			Assert::IsTrue(node.Acquire(10, Desc(), "temporal_detail", render).status == GrainStatus::Missing, L"10 fails");
			Assert::IsTrue(node.Acquire(11, Desc(), "temporal_detail", render).status == GrainStatus::Missing, L"11 fails");
			Assert::AreEqual(2, render.calls, L"one attempt per frame");
			render.fail = false;   // the compute works again, but inside the frame nobody retries
			Assert::IsTrue(node.Acquire(10, Desc(), "chroma_cfl", render).status == GrainStatus::Missing, L"10 stays failed");
			Assert::IsTrue(node.Acquire(11, Desc(), "chroma_cfl", render).status == GrainStatus::Missing, L"11 stays failed");
			Assert::AreEqual(2, render.calls, L"no second producer for either frame");
			node.Release(10);
			node.Release(11);
			Warm(node, 14);
			Assert::IsTrue(node.Acquire(14, Desc(), "temporal_detail", render).status == GrainStatus::Live, L"a later frame recovers");
		}

		TEST_METHOD(TemporalDetailShadersReverseToTheirPreAccessorText)
		{
			// The accessor switch is mechanical: replacing gs_fetch(P) by texelFetch(split, P, 0) and gs_blocks(B) by
			// texelFetch(gblocks, B, 0) in the SA2 shader text gives back, byte for byte, the text at 160987e5 (pinned
			// by size and FNV-1a 64). With the Full prelude (gs_fetch = texelFetch(split, ...)) the Full-mode shader
			// therefore computes exactly what it computed before.
			const std::string root = SettingsTestSupport::RepoRoot();
			std::string text = SettingsTestSupport::ReadFile(root + "src\\VP-V\\modules\\repair\\temporal_detail\\TemporalDetailShaders.h");
			Assert::IsTrue(!text.empty(), L"TemporalDetailShaders.h is readable");
			text.erase(std::remove(text.begin(), text.end(), '\r'), text.end());
			auto replaceCalls = [](const std::string& t, const std::string& name, const std::string& before, const std::string& after) {
				std::string out;
				size_t i = 0;
				for (;;)
				{
					const size_t j = t.find(name + "(", i);
					if (j == std::string::npos)
					{
						out += t.substr(i);
						break;
					}
					out += t.substr(i, j - i);
					const size_t k = j + name.size() + 1;
					size_t m = k;
					int depth = 1;
					while (depth > 0 && m < t.size())
					{
						depth += (t[m] == '(') - (t[m] == ')');
						++m;
					}
					out += before + t.substr(k, m - 1 - k) + after;
					i = m;
				}
				return out;
			};
			text = replaceCalls(text, "gs_fetch", "texelFetch(split, ", ", 0)");
			text = replaceCalls(text, "gs_blocks", "texelFetch(gblocks, ", ", 0)");
			uint64_t h = 14695981039346656037ull;
			for (unsigned char c : text)
			{
				h ^= c;
				h *= 1099511628211ull;
			}
			Assert::AreEqual(size_t(46944), text.size(), L"the reversed text has the pre-SA2 size");
			Assert::AreEqual(uint64_t(0xd4c796a262566634ull), h, L"the reversed text hashes to the pre-SA2 shader text");
		}

		TEST_METHOD(GrainLayoutNamesBaseAndModifiersWithTheirConsumers)
		{
			size_t n = 0;
			const FieldSpec* f = GrainFields(&n);
			size_t base = 0, modifiers = 0;
			bool hasN = false, hasF = false, hasE3 = false;
			for (size_t i = 0; i < n; ++i)
			{
				Assert::IsTrue(f[i].name && f[i].where && f[i].derivedBy && std::string(f[i].derivedBy).size() > 10, L"every field names where it is and who derives what");
				(f[i].kind == FieldKind::Base ? base : modifiers) += 1;
				hasN = hasN || std::string(f[i].name) == "N";
				hasF = hasF || std::string(f[i].name) == "f";
				hasE3 = hasE3 || std::string(f[i].name) == "e3";
			}
			Assert::AreEqual(size_t(3), base, L"base: base, s1, s2 (the main map)");
			Assert::AreEqual(size_t(3), modifiers, L"modifiers: f, N, e3");
			Assert::IsTrue(hasN && hasF && hasE3, L"the modifier fields the consumers derive from");
		}
	};
}
