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
#include <VP-V/modules/repair/grain_split/BandMemo.h>
#include <VP-V/modules/repair/grain_split/BandMemoFill.h>
#include <VP-V/modules/repair/grain_split/GrainNode.h>
#include <VP-V/modules/repair/grain_split/GrainNodeGpu.h>
#include <VP-V/modules/repair/grain_split/GrainSplitGpu.h>
#include <VP-V/modules/repair/grain_split/GpuPassUtil.h>
#include <VP-V/modules/repair/grain_split/shared/BandFromBase.h>
#include "BandFromBaseGolden.generated.h"

#include <cmath>
#include <cstdint>
#include <cstring>
#include <limits>
#include <memory>
#include <string>
#include <thread>
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
			// texelFetch(gblocks, B, 0) in the SA2 shader text gives back, byte for byte, the accessor-free text (160987e5 plus
			// K0/SA0's own TemporalDetail changes: feat/camera-k0 at f62e9fae; pinned by size and FNV-1a 64). With the Full prelude (gs_fetch = texelFetch(split, ...)) the Full-mode shader
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
			Assert::AreEqual(size_t(49973), text.size(), L"the reversed text has the accessor-free (K0 + U7 v3 + TD-NONFINITE + 15b A4) size");
			Assert::AreEqual(uint64_t(0x3d63ca6088548f9full), h, L"the reversed text hashes to the accessor-free (K0 + U7 v3 + TD-NONFINITE + 15b A4) shader text");
		}

		// Finisher MUST 2: the node's declaration is built from the GRANTED storage (NodeLayout::decl = GrainDecl(choice.storage)),
		// so a Compact request downgraded to Full allocates as Full instead of failing every allocation. CPU: fake textures.
		TEST_METHOD(DeclarationFollowsTheGrantedStorage)
		{
			struct Be
			{
				using Handle = int;
				int n = 0;
				Handle Create(const VpSkeleton::TexShape&) { return ++n; }
				void Destroy(Handle& h) { h = 0; }
			};
			const VpSkeleton::Dims sizes[] = { { 1920, 1080 }, { 3840, 2160 }, { 7680, 4320 }, { 1366, 769 } };
			for (const VpSkeleton::Dims& d : sizes)
				for (int B : { 16, 32 })
				{
					GrainDesc desc;
					desc.w = static_cast<int>(d.width); desc.h = static_cast<int>(d.height); desc.B = B; desc.codeUnit = 1.0 / 1023.0; desc.component = 0;
					for (Storage granted : { Storage::Full, Storage::Compact })
					{
						const VpSkeleton::ProductTexLayout layout = GrainLayout(granted, desc);
						const VpSkeleton::ProductDecl decl = GrainDecl(granted);
						Assert::IsTrue(layout.MatchesDecl(decl, d), L"the layout matches the declaration of its own storage");
						const VpSkeleton::Registry reg = GrainRegistryFor(granted);
						Assert::IsTrue(reg.Validate(), L"the registry for the granted storage validates");
						Assert::AreEqual(VpSkeleton::BytesOf(decl.size, d), VpSkeleton::BytesOf(reg.Get(VpSkeleton::kP10GrainSplit).size, d), L"one declaration");
						VpSkeleton::Node node;
						node.id = VpSkeleton::kP10GrainSplit;
						node.ring = 0;
						node.livePool = 2;
						Be be;
						VpSkeleton::ProductTextures<Be> tex(be);
						Assert::IsTrue(tex.Allocate(node, decl, layout, d, VpSkeleton::TierPlan()), L"allocates");
						Assert::IsTrue(tex.Bytes() <= 2 * VpSkeleton::BytesOf(decl.size, d), L"within the declaration");
					}
					// Luma and chroma granted separately: P10 and P10c each follow their own grant.
					{
						const VpSkeleton::Registry mixed = GrainRegistryFor(Storage::Compact, Storage::Full);
						Assert::IsTrue(mixed.Validate(), L"mixed grants validate");
						Assert::AreEqual(VpSkeleton::BytesOf(GrainDecl(Storage::Compact).size, d), VpSkeleton::BytesOf(mixed.Get(VpSkeleton::kP10GrainSplit).size, d), L"luma compact");
						Assert::AreEqual(VpSkeleton::BytesOf(VpSkeleton::SkeletonProducts(false).Get(VpSkeleton::kP10cGrainSplitChroma).size, d),
							VpSkeleton::BytesOf(mixed.Get(VpSkeleton::kP10cGrainSplitChroma).size, d), L"chroma full");
					}
					// Requested Compact, granted Full (a float plane): the REQUESTED declaration would refuse the layout forever.
					Assert::IsFalse(GrainLayout(Storage::Full, desc).MatchesDecl(GrainDecl(Storage::Compact), d), L"requested declaration refuses the downgrade");
				}
		}

		// S2-H6: at <=720p (B = 8), GrainLayout emits a shift-3 block grid (ceil(w/8) x ceil(h/8)); GrainDecl(storage, B)
		// and ChooseNodeLayout must declare the same shift-3 block grid so ProductTexLayout::MatchesDecl and ProductTextures::Allocate
		// succeed for both Full and Compact storage (while >=1080p stays shift-4). Negative control: the un-parameterized legacy
		// GrainDecl(storage) (default blockPx = 16, shift = 4) fails MatchesDecl and allocation on the exact same <=720p layouts.
		TEST_METHOD(ChooseNodeLayoutAndGrainDeclMatchAt720pAndBelow)
		{
			struct Be
			{
				using Handle = int;
				int n = 0;
				Handle Create(const VpSkeleton::TexShape&) { return ++n; }
				void Destroy(Handle& h) { h = 0; }
			};
			auto declHasShift = [](const VpSkeleton::ProductDecl& decl, uint8_t shift) -> bool {
				const size_t n = decl.size.termCount < decl.size.terms.size() ? size_t(decl.size.termCount) : decl.size.terms.size();
				for (size_t i = 0; i < n; ++i)
					if (decl.size.terms[i].shift == shift)
						return true;
				return false;
			};

			pl_fmt_t unormFmt = {};
			unormFmt.type = PL_FMT_UNORM;
			pl_fmt_t floatFmt = {};
			floatFmt.type = PL_FMT_FLOAT;
			const double cu10 = 1.0 / 1023.0;

			struct Case
			{
				uint32_t w, h;
				int B;
				uint8_t expectedShift;
			};
			const Case cases[] = {
				{ 640, 480, 8, 3 },     // <=720p (SD, B = 8)
				{ 854, 480, 8, 3 },     // <=720p non-multiple of 8 width (ceil(854/8) = 107)
				{ 1280, 720, 8, 3 },    // <=720p boundary (B = 8)
				{ 1920, 1080, 16, 4 },  // >=1080p (B = 16)
				{ 3840, 2160, 32, 4 },  // 4K (B = 32, shift-4 upper bound)
			};

			VpSkeleton::Node node;
			node.id = VpSkeleton::kP10GrainSplit;
			node.ring = 0;
			node.livePool = 2;

			for (const Case& c : cases)
			{
				const VpSkeleton::Dims dims{ c.w, c.h };
				GrainDesc desc;
				desc.w = static_cast<int>(c.w);
				desc.h = static_cast<int>(c.h);
				desc.B = c.B;
				desc.codeUnit = cu10;
				desc.component = 0;

				const uint32_t expectedBw = (c.w + static_cast<uint32_t>(c.B) - 1u) / static_cast<uint32_t>(c.B);
				const uint32_t expectedBh = (c.h + static_cast<uint32_t>(c.B) - 1u) / static_cast<uint32_t>(c.B);

				for (Storage storage : { Storage::Full, Storage::Compact })
				{
					// Direct GrainLayout + blockPx-aware GrainDecl(storage, desc.B).
					const VpSkeleton::ProductTexLayout layout = GrainLayout(storage, desc);
					const VpSkeleton::ProductDecl decl = GrainDecl(storage, desc.B);
					Assert::IsFalse(layout.terms.empty(), L"layout has terms");
					const VpSkeleton::TexTerm& blockTerm = layout.terms.back();
					Assert::AreEqual(uint32_t(c.expectedShift), uint32_t(blockTerm.shift), L"GrainLayout block term shift");
					Assert::AreEqual(expectedBw, blockTerm.absW, L"GrainLayout block width");
					Assert::AreEqual(expectedBh, blockTerm.absH, L"GrainLayout block height");
					Assert::IsTrue(declHasShift(decl, c.expectedShift), L"GrainDecl declares the matching block shift");
					Assert::IsFalse(declHasShift(decl, c.expectedShift == 3u ? uint8_t(4) : uint8_t(3)),
						L"GrainDecl does not leave the mismatched block shift");
					Assert::IsTrue(layout.MatchesDecl(decl, dims), L"GrainLayout matches blockPx-aware GrainDecl");

					// ChooseNodeLayout (production entry point) with a Compact-capable UNORM plane.
					const GrainSplit::Gpu::NodeLayout nl = GrainSplit::Gpu::ChooseNodeLayout(storage, &unormFmt, cu10, desc);
					Assert::IsTrue(nl.choice.storage == storage, L"UNORM 10-bit plane grants the requested storage");
					Assert::IsFalse(nl.choice.downgraded, L"not downgraded on UNORM 10-bit plane");
					Assert::IsTrue(declHasShift(nl.decl, c.expectedShift), L"ChooseNodeLayout decl carries the matching block shift");
					Assert::IsTrue(nl.layout.MatchesDecl(nl.decl, dims), L"ChooseNodeLayout layout matches its own decl");

					Be be;
					VpSkeleton::ProductTextures<Be> tex(be);
					Assert::IsTrue(tex.Allocate(node, nl.decl, nl.layout, dims, VpSkeleton::TierPlan()),
						L"ProductTextures::Allocate succeeds with ChooseNodeLayout");
					Assert::AreEqual(uint64_t(2) * nl.layout.BytesPerBuffer(dims), tex.Bytes(),
						L"allocated bytes match 2 live buffers");
					Assert::IsTrue(tex.Bytes() <= uint64_t(2) * VpSkeleton::BytesOf(nl.decl.size, dims),
						L"allocated bytes fit within the declared SizeRule upper bound");

					if (c.B <= 8)
					{
						// Mandatory negative control (Rule 3): on the exact same <=720p (B = 8) GrainLayout, the
						// un-parameterized legacy GrainDecl(storage) (default blockPx = 16 -> shift = 4) MUST fail
						// MatchesDecl and ProductTextures::Allocate — proving the shift-4 vs shift-3 hazard is real.
						const VpSkeleton::ProductDecl legacyDecl = GrainDecl(storage);
						Assert::IsTrue(declHasShift(legacyDecl, 4u), L"legacy 1-arg GrainDecl emits shift 4");
						Assert::IsFalse(declHasShift(legacyDecl, 3u), L"legacy 1-arg GrainDecl does not emit shift 3");
						Assert::IsFalse(layout.MatchesDecl(legacyDecl, dims),
							L"negative control: <=720p B=8 layout MUST fail MatchesDecl against legacy shift-4 GrainDecl");
						Assert::IsFalse(nl.layout.MatchesDecl(legacyDecl, dims),
							L"negative control: ChooseNodeLayout <=720p layout MUST fail MatchesDecl against legacy shift-4 GrainDecl");

						Be negBe;
						VpSkeleton::ProductTextures<Be> negTex(negBe);
						Assert::IsFalse(negTex.Allocate(node, legacyDecl, layout, dims, VpSkeleton::TierPlan()),
							L"negative control: ProductTextures::Allocate MUST refuse <=720p layout under legacy shift-4 GrainDecl");
						Assert::AreEqual(1u, negTex.Refusals(), L"refusal is counted");
						Assert::IsFalse(negTex.LastRefusal().empty(), L"refusal reason is visible");
					}
					else
					{
						// Reverse mismatch control: a >=1080p (shift 4) layout must NOT match a shift-3 (B = 8) decl.
						const VpSkeleton::ProductDecl shift3Decl = GrainDecl(storage, 8);
						Assert::IsFalse(layout.MatchesDecl(shift3Decl, dims),
							L"mismatch control: >=1080p shift-4 layout must not match shift-3 GrainDecl");
					}
				}

				// Compact requested on a float plane at <=720p (and >=1080p): downgraded to Full, and still matches decl.
				const GrainSplit::Gpu::NodeLayout downgraded = GrainSplit::Gpu::ChooseNodeLayout(Storage::Compact, &floatFmt, cu10, desc);
				Assert::IsTrue(downgraded.choice.downgraded && downgraded.choice.storage == Storage::Full,
					L"float plane downgrades Compact to Full");
				Assert::IsTrue(declHasShift(downgraded.decl, c.expectedShift), L"downgraded decl keeps the matching block shift");
				Assert::IsTrue(downgraded.layout.MatchesDecl(downgraded.decl, dims),
					L"downgraded ChooseNodeLayout layout matches its own decl");
				Be downBe;
				VpSkeleton::ProductTextures<Be> downTex(downBe);
				Assert::IsTrue(downTex.Allocate(node, downgraded.decl, downgraded.layout, dims, VpSkeleton::TierPlan()),
					L"downgraded ChooseNodeLayout allocates cleanly");
			}
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
			Assert::AreEqual(size_t(5), modifiers, L"modifiers: f, N, e3, e1, e2");
			Assert::IsTrue(hasN && hasF && hasE3, L"the modifier fields the consumers derive from");
		}

		// S2-H5 / CT-M6 (BandMemo.h codec): canonical BandKey round-trips cleanly (including kMinSigmaPx = 0.5e-6);
		// non-canonical param0 >= 1000 aliases (which would split memo slots under distinct VariantKey hashes),
		// sub-quantum / non-positive / non-finite / overflow sigmas, half-zero sizes, and stray crop fields MUST
		// be rejected by Encodable/ToVariant/FromVariant.
		TEST_METHOD(BandMemoCodecRejectsNonCanonicalParam0AndSubQuantumSigma)
		{
			namespace BM = VpUnit::BandMemo;
			using VK = VpSkeleton::VariantKey;

			// 1. Canonical round-trips: standard sigmaPx = 1.5, micro-pixel remainder, kMinSigmaPx = 0.5e-6, and Law::Low.
			BM::BandKey k15;
			k15.sigmaPx = 1.5;
			Assert::IsTrue(BM::Encodable(k15), L"sigmaPx=1.5 is encodable");
			const VK v15 = BM::ToVariant(k15);
			Assert::AreEqual(uint32_t(1500), v15.sigmaMilli, L"1.5 px -> 1500 milli-px");
			Assert::AreEqual(uint32_t(0), uint32_t(v15.param0), L"1.5 px -> 0 micro-px remainder");
			BM::BandKey out;
			Assert::IsTrue(BM::FromVariant(v15, &out), L"canonical sigmaPx=1.5 decodes");
			Assert::IsTrue(std::fabs(out.sigmaPx - 1.5) < 1e-12, L"sigmaPx=1.5 round-trips");
			Assert::IsTrue(out.resolution == BM::Stage::Full && out.law == BM::Law::Taps
				&& out.edge == VpUnit::BandFromBase::EdgeMode::Replicate && out.minOne && out.factor == 3.0 && out.eps == 0.0,
				L"default BandKey fields round-trip");
			Assert::IsTrue(BM::ToVariant(out) == v15, L"re-encoding canonical BandKey is identical");

			BM::BandKey kRich;
			kRich.resolution = BM::Stage::Half;
			kRich.pipelineStage = 2;
			kRich.plane = 1;
			kRich.width = 960;
			kRich.height = 540;
			kRich.sigmaPx = 1.50025;
			kRich.law = BM::Law::Taps;
			kRich.edge = VpUnit::BandFromBase::EdgeMode::Reflect;
			kRich.factor = 2.5;
			kRich.minOne = false;
			kRich.eps = 1e-9;
			Assert::IsTrue(BM::Encodable(kRich), L"rich canonical BandKey is encodable");
			const VK vRich = BM::ToVariant(kRich);
			Assert::AreEqual(uint32_t(1500), vRich.sigmaMilli, L"rich sigmaMilli");
			Assert::AreEqual(uint32_t(250), uint32_t(vRich.param0), L"rich micro-px remainder");
			Assert::IsTrue(BM::FromVariant(vRich, &out), L"rich BandKey decodes");
			Assert::IsTrue(std::fabs(out.sigmaPx - 1.50025) < 1e-12 && out.resolution == BM::Stage::Half
				&& out.pipelineStage == 2 && out.plane == 1 && out.width == 960u && out.height == 540u
				&& out.edge == VpUnit::BandFromBase::EdgeMode::Reflect && out.factor == 2.5
				&& !out.minOne && std::fabs(out.eps - 1e-9) < 1e-18,
				L"every rich BandKey field survives ToVariant -> FromVariant");
			Assert::IsTrue(BM::ToVariant(out) == vRich, L"rich key re-encodes identically");

			BM::BandKey kMin;
			kMin.sigmaPx = BM::kMinSigmaPx;   // 0.5e-6 px: rounds to 1 micro-px quantum under std::llround
			Assert::IsTrue(std::fabs(BM::kMinSigmaPx - 0.5e-6) < 1e-18, L"kMinSigmaPx is 0.5e-6");
			Assert::IsTrue(BM::Encodable(kMin), L"kMinSigmaPx is encodable");
			const VK vMin = BM::ToVariant(kMin);
			Assert::IsFalse(vMin.IsDefault(), L"kMinSigmaPx does not collapse to the default zero key");
			Assert::AreEqual(uint32_t(0), vMin.sigmaMilli, L"kMinSigmaPx sigmaMilli == 0");
			Assert::AreEqual(uint32_t(1), uint32_t(vMin.param0), L"kMinSigmaPx rounds to 1 micro-px");
			Assert::IsTrue(BM::FromVariant(vMin, &out), L"kMinSigmaPx variant decodes cleanly");
			Assert::IsTrue(std::fabs(out.sigmaPx - 1.0e-6) < 1e-15, L"kMinSigmaPx decodes to 1 micro-px quantum");
			Assert::IsTrue(BM::ToVariant(out) == vMin, L"decoded kMinSigmaPx re-encodes identically");

			BM::BandKey kLow;
			kLow.sigmaPx = 2.0;
			kLow.law = BM::Law::Low;
			Assert::IsTrue(BM::Encodable(kLow), L"canonical Law::Low is encodable");
			const VK vLow = BM::ToVariant(kLow);
			Assert::IsTrue(BM::FromVariant(vLow, &out) && out.law == BM::Law::Low && std::fabs(out.sigmaPx - 2.0) < 1e-12,
				L"Law::Low round-trips cleanly");

			VK vParam999 = v15;
			vParam999.sigmaMilli = 1;
			vParam999.param0 = 999;
			Assert::IsTrue(BM::FromVariant(vParam999, &out) && std::fabs(out.sigmaPx - 1.999e-3) < 1e-12,
				L"upper canonical boundary param0=999 succeeds");

			// 2. Non-canonical param0 >= 1000 aliases MUST be rejected, with same-input positive & negative controls.
			BM::BandKey sentinel;
			sentinel.sigmaPx = 42.0;

			BM::BandKey k20;
			k20.sigmaPx = 2.0;
			const VK vCanon20 = BM::ToVariant(k20);   // sigmaMilli=2000, param0=0

			VK vAlias1000 = vCanon20;
			vAlias1000.sigmaMilli = 1999;
			vAlias1000.param0 = 1000;   // exact same mathematical sigma (1999*1000 + 1000)/1e6 == 2.0
			// Negative control (Rule 3): prove the slot-splitting hazard is real — VariantKey::Canonical() alone
			// does NOT collapse param0=1000 into sigmaMilli+1, so their memo keys and hashes diverge.
			Assert::IsTrue(vAlias1000 != vCanon20 && vAlias1000.Canonical() != vCanon20.Canonical(),
				L"negative control: VariantKey::Canonical alone leaves param0=1000 distinct from sigmaMilli+1");
			Assert::AreNotEqual(vCanon20.Hash(), vAlias1000.Hash(),
				L"negative control: non-canonical param0=1000 hashes to a different memo slot");
			out = sentinel;
			Assert::IsFalse(BM::FromVariant(vAlias1000, &out), L"non-canonical param0=1000 MUST be rejected");
			Assert::AreEqual(sentinel.sigmaPx, out.sigmaPx, L"failed FromVariant leaves out untouched");
			// Same-input positive control: shifting 1000 micro-px into sigmaMilli yields the canonical spelling.
			VK vCanonFrom1000 = vAlias1000;
			vCanonFrom1000.sigmaMilli += 1u;
			vCanonFrom1000.param0 = uint16_t(vCanonFrom1000.param0 - BM::kMicroPerMilli);
			Assert::IsTrue(vCanonFrom1000 == vCanon20, L"normalized alias equals canonical variant");
			Assert::IsTrue(BM::FromVariant(vCanonFrom1000, &out) && std::fabs(out.sigmaPx - 2.0) < 1e-12,
				L"positive control: canonical spelling (sigmaMilli+1, param0-1000) succeeds");

			VK vAlias1500 = vCanon20;
			vAlias1500.sigmaMilli = 1999;
			vAlias1500.param0 = 1500;   // exact same mathematical sigma 2.0005 as (sigmaMilli=2000, param0=500)
			out = sentinel;
			Assert::IsFalse(BM::FromVariant(vAlias1500, &out), L"non-canonical param0=1500 MUST be rejected");
			Assert::AreEqual(sentinel.sigmaPx, out.sigmaPx, L"failed FromVariant(param0=1500) leaves out untouched");
			VK vCanonFrom1500 = vAlias1500;
			vCanonFrom1500.sigmaMilli += 1u;
			vCanonFrom1500.param0 = uint16_t(vCanonFrom1500.param0 - BM::kMicroPerMilli);
			Assert::AreNotEqual(vAlias1500.Hash(), vCanonFrom1500.Hash(),
				L"negative control: param0=1500 has a distinct hash from its canonical spelling");
			Assert::IsTrue(BM::FromVariant(vCanonFrom1500, &out) && std::fabs(out.sigmaPx - 2.0005) < 1e-12,
				L"positive control: canonical spelling (sigmaMilli=2000, param0=500) succeeds");
			Assert::IsTrue(BM::ToVariant(out) == vCanonFrom1500, L"canonical 2.0005 round-trips identically");

			VK vZeroMilliAlias1000;
			vZeroMilliAlias1000.sigmaMilli = 0;
			vZeroMilliAlias1000.param0 = 1000;
			Assert::IsFalse(BM::FromVariant(vZeroMilliAlias1000, &out), L"sigmaMilli=0, param0=1000 MUST be rejected");
			VK vZeroMilliCanon;
			vZeroMilliCanon.sigmaMilli = 1;
			vZeroMilliCanon.param0 = 0;
			Assert::IsTrue(BM::FromVariant(vZeroMilliCanon, &out) && std::fabs(out.sigmaPx - 1.0e-3) < 1e-12,
				L"positive control: sigmaMilli=1, param0=0 succeeds");

			// 3. Sub-quantum, zero, negative, NaN, Inf, and over-max sigmas MUST be rejected.
			// Negative control (Rule 3): prove the sub-quantum rounding hazard on the exact input 0.25e-6 —
			// strictly > 0.0, yet std::llround(0.25e-6 * 1e6) == 0 collides with the invalid zero sentinel,
			// whereas kMinSigmaPx (0.5e-6) rounds to 1.
			Assert::IsTrue(0.25e-6 > 0.0, L"negative control: 0.25e-6 passes a naive sigmaPx > 0 check");
			Assert::AreEqual(0LL, std::llround(0.25e-6 * double(BM::kMicroPerPx)),
				L"negative control: 0.25e-6 rounds to 0 micro-px and would collide with zero key without kMinSigmaPx floor");
			Assert::AreEqual(1LL, std::llround(BM::kMinSigmaPx * double(BM::kMicroPerPx)),
				L"kMinSigmaPx rounds to 1 micro-px");

			const double badSigmas[] = {
				0.25e-6,
				0.49e-6,
				0.0,
				-1.0,
				std::numeric_limits<double>::quiet_NaN(),
				std::numeric_limits<double>::infinity(),
				-std::numeric_limits<double>::infinity(),
				BM::kMaxSigmaPx + 1.0,
			};
			const VK zeroKey;
			for (double badSigma : badSigmas)
			{
				BM::BandKey bad;
				bad.sigmaPx = badSigma;
				Assert::IsFalse(BM::Encodable(bad), L"invalid/sub-quantum sigmaPx must not be Encodable");
				const VK vBad = BM::ToVariant(bad);
				Assert::IsTrue(vBad == zeroKey && vBad.IsDefault(), L"ToVariant of unencodable sigma returns zero key");
				out = sentinel;
				Assert::IsFalse(BM::FromVariant(vBad, &out), L"FromVariant rejects zero key from unencodable sigma");
				Assert::AreEqual(sentinel.sigmaPx, out.sigmaPx, L"out untouched on rejected zero key");
			}
			Assert::IsFalse(BM::FromVariant(zeroKey, &out), L"default zero VariantKey is rejected");

			// 4. Half-zero size and stray crop fields MUST be rejected, with same-input positive controls.
			VK vHalfW = v15;
			vHalfW.width = 1920;
			vHalfW.height = 0;
			Assert::IsFalse(BM::FromVariant(vHalfW, &out), L"half-zero size (width!=0, height==0) MUST be rejected");

			VK vHalfH = v15;
			vHalfH.width = 0;
			vHalfH.height = 1080;
			Assert::IsFalse(BM::FromVariant(vHalfH, &out), L"half-zero size (width==0, height!=0) MUST be rejected");

			VK vFullSize = v15;
			vFullSize.width = 1920;
			vFullSize.height = 1080;
			Assert::IsTrue(BM::FromVariant(vFullSize, &out) && out.width == 1920u && out.height == 1080u,
				L"positive control: both non-zero width and height succeed");

			// Stray crop fields: HasCrop() is false when only cropX / cropY / cropW / cropH is set, proving
			// checking HasCrop() alone is insufficient (Rule 3 negative control).
			VK vCropX = v15;
			vCropX.cropX = 16;
			Assert::IsFalse(vCropX.HasCrop(), L"negative control: HasCrop() is false for stray cropX");
			Assert::IsFalse(BM::FromVariant(vCropX, &out), L"stray cropX MUST be rejected");

			VK vCropY = v15;
			vCropY.cropY = 8;
			Assert::IsFalse(vCropY.HasCrop(), L"negative control: HasCrop() is false for stray cropY");
			Assert::IsFalse(BM::FromVariant(vCropY, &out), L"stray cropY MUST be rejected");

			VK vCropW = v15;
			vCropW.cropW = 64;
			Assert::IsFalse(vCropW.HasCrop(), L"negative control: HasCrop() is false for stray cropW");
			Assert::IsFalse(BM::FromVariant(vCropW, &out), L"stray cropW MUST be rejected");

			VK vCropH = v15;
			vCropH.cropH = 64;
			Assert::IsFalse(vCropH.HasCrop(), L"negative control: HasCrop() is false for stray cropH");
			Assert::IsFalse(BM::FromVariant(vCropH, &out), L"stray cropH MUST be rejected");

			VK vCropBoth = v15;
			vCropBoth.cropW = 64;
			vCropBoth.cropH = 64;
			Assert::IsTrue(vCropBoth.HasCrop(), L"full crop has HasCrop()==true");
			Assert::IsFalse(BM::FromVariant(vCropBoth, &out), L"crop rectangle MUST be rejected");
		}

		// S2-H5 / CT-M6 (BandMemoFill.h):
		//   (a) BandMemoModule surfaces registration refusals via FallbackLines()/OnStatus(StatusKind::Fallbacks),
		//       resets tried_ on Forget (want == 0) so re-observing the exact same size succeeds and clears fallbacks;
		//   (b) MakeBandMemoOwnerFill rejects RGB SourcePlanes offers and mismatched kAxisSize BEFORE allocating GPU
		//       textures, records visible decline diagnostics, and surfaces them via FallbackLine/OnStatus(Fallbacks)
		//       while staying silent on non-Fallbacks StatusKinds.
		TEST_METHOD(BandMemoFillRejectsRgbAndSizeMismatchAndReportsFallbacks)
		{
			namespace BM = VpUnit::BandMemo;
			namespace GB = Vpv::GrainBand;
			namespace RE = RendererExtension;
			using Vpv::Capture::SourcePlanes;
			using Vpv::Capture::SourcePlaneSet;

			struct SourceHostGuard
			{
				const void* host = nullptr;
				explicit SourceHostGuard(const void* h) : host(h) {}
				~SourceHostGuard()
				{
					if (host != nullptr)
						SourcePlanes::Forget(host);
				}
				SourceHostGuard(const SourceHostGuard&) = delete;
				SourceHostGuard& operator=(const SourceHostGuard&) = delete;
			};

			struct FakeDemand : Vpv::Skeleton::IExternalDemand
			{
				VpSkeleton::SkeletonMemo* memo = nullptr;
				bool refuse = false;
				VpSkeleton::SkeletonMemo::Registration refuseCode = VpSkeleton::SkeletonMemo::Registration::NotInCatalog;
				int registerCalls = 0;
				int replaceCalls = 0;
				VpSkeleton::OwnerFill lastFill;

				void SetExternalDemand(const void*, const void*, const std::vector<VpSkeleton::ConsumerDecl>&,
					const std::vector<VpSkeleton::ProductId>&) override {}
				void ClearExternalDemand(const void*, const void*) override {}

				VpSkeleton::SkeletonMemo::Registration RegisterOwnerFill(VpSkeleton::ProductId id, VpSkeleton::AcquireForm form,
					const std::string& unit, VpSkeleton::OwnerFill fill) override
				{
					++registerCalls;
					lastFill = fill;
					if (refuse)
						return refuseCode;
					return memo != nullptr
						? memo->RegisterOwnerFill(id, form, unit, std::move(fill))
						: VpSkeleton::SkeletonMemo::Registration::Registered;
				}

				VpSkeleton::SkeletonMemo::Registration ReplaceOwnerFill(VpSkeleton::ProductId id, VpSkeleton::AcquireForm form,
					const std::string& unit, VpSkeleton::OwnerFill fill) override
				{
					++replaceCalls;
					lastFill = fill;
					if (refuse)
						return refuseCode;
					return memo != nullptr
						? memo->ReplaceOwnerFill(id, form, unit, std::move(fill))
						: VpSkeleton::SkeletonMemo::Registration::Replaced;
				}

				bool UnregisterOwnerFill(VpSkeleton::ProductId id, VpSkeleton::AcquireForm form, const std::string& unit) override
				{
					return memo != nullptr && memo->UnregisterOwnerFill(id, form, unit);
				}
			};

			// Opaque GPU device test double: non-null GPU/dispatch pointers, counts and refuses every texture allocation.
			struct RefusingDevice : VpSkeleton::IOwnerGpuDevice
			{
				int gpuTag = 0;
				int dispatchTag = 0;
				int asked = 0;
				void* OwnerGpu() const override { return const_cast<int*>(&gpuTag); }
				void* OwnerDispatch() const override { return const_cast<int*>(&dispatchTag); }
				bool CreateOwnerTexture(const VpSkeleton::TexShape&, VpSkeleton::GpuMemoHandle&, std::string& why) override
				{
					++asked;
					why = "test device makes no texture";
					return false;
				}
			};

			// ---- Part (a): registration failure visibility, tried_ latch, Forget reset, and same-size recovery ----
			{
				VpSkeleton::SkeletonMemo memo;
				FakeDemand demand;
				demand.memo = &memo;
				demand.refuse = true;
				demand.refuseCode = VpSkeleton::SkeletonMemo::Registration::NotInCatalog;

				GB::BandMemoModule module;
				int host = 0;
				const VpSkeleton::Dims d1080{ 1920, 1080 };

				Assert::IsFalse(module.Registered(), L"not registered initially");
				Assert::AreEqual(uint64_t(0), module.RegisteredReserveBytes(), L"no reserve initially");
				Assert::IsTrue(module.FallbackLines().empty(), L"no fallback line before any size is observed");
				Assert::AreEqual(uint64_t(0), module.Stats().fills.load(), L"no fills initially");
				Assert::AreEqual(uint64_t(0), module.Stats().declines.load(), L"no declines initially");

				// Registration initially fails -> visible fallback line naming the catalog refusal.
				Assert::IsFalse(module.Observe(&host, d1080, &demand), L"refused registration returns false");
				Assert::IsFalse(module.Registered(), L"module stays unregistered on refusal");
				Assert::AreEqual(1, demand.registerCalls, L"one registration attempt");
				const std::vector<std::string> fb = module.FallbackLines();
				Assert::AreEqual(size_t(1), fb.size(), L"refused registration must emit a visible FallbackLine (Rule 4)");
				Assert::IsTrue(Contains(fb[0], GB::kBandMemoFallbackId), L"fallback line carries product.band_memo_unavailable id");
				Assert::IsTrue(Contains(fb[0], "not in the catalog"), L"fallback line visibly names the registration refusal");

				// OnStatus emits the fallback line on VP-V pathway for StatusKind::Fallbacks, and stays silent on others.
				RE::Context ctx{};
				ctx.host = &host;
				std::vector<std::string> statusLines;
				module.OnStatus(ctx, RE::StatusKind::Fallbacks, statusLines, nullptr, nullptr);
				Assert::AreEqual(size_t(1), statusLines.size(), L"OnStatus(Fallbacks) emits the fallback line on VP-V pathway");
				Assert::AreEqual(fb[0], statusLines[0], L"OnStatus(Fallbacks) matches FallbackLines()");

				std::vector<std::string> nonFbLines;
				module.OnStatus(ctx, RE::StatusKind::Lines, nonFbLines, nullptr, nullptr);
				module.OnStatus(ctx, RE::StatusKind::WorkerFault, nonFbLines, nullptr, nullptr);
				module.OnStatus(ctx, RE::StatusKind::WorkerOk, nonFbLines, nullptr, nullptr);
				Assert::IsTrue(nonFbLines.empty(), L"OnStatus stays silent on non-Fallbacks status kinds");

				// Same-input negative control (Rule 3): while the host stays attached at the same size, tried_ == want
				// latches even after demand stops refusing — proving tried_ is actively suppressing same-size retries
				// until Forget resets it.
				demand.refuse = false;
				Assert::IsFalse(module.Observe(&host, d1080, &demand), L"negative control: without Forget, same size stays latched");
				Assert::AreEqual(1, demand.registerCalls, L"negative control: still 1 attempt before Forget");
				Assert::IsFalse(module.Registered(), L"negative control: still unregistered before Forget");
				Assert::AreEqual(size_t(1), module.FallbackLines().size(), L"fallback line persists while still refused/latched");

				// Forget(host) drops want to 0, resetting tried_ and clearing regProblem_.
				module.Forget(&host, &demand);
				Assert::IsTrue(module.FallbackLines().empty(), L"after Forget (want==0), nothing is owed and FallbackLines clears");

				// Re-observing the EXACT SAME size once demand accepts MUST retry and succeed (on pre-fix code,
				// tried_ was never reset on Forget so this remained permanently stuck at Registered()==false).
				Assert::IsTrue(module.Observe(&host, d1080, &demand), L"after Forget, re-observing exact same size succeeds");
				Assert::AreEqual(2, demand.registerCalls, L"registration retried once after Forget reset tried_");
				Assert::IsTrue(module.Registered(), L"module is now registered");
				Assert::AreEqual(GB::BandMemoReserveBytes(d1080), module.RegisteredReserveBytes(), L"reserve matches 1080p");
				Assert::IsTrue(memo.HasOwnerFill(VpSkeleton::kBandMemo, VpSkeleton::AcquireForm::Gpu), L"memo holds kBandMemo fill");
				Assert::IsTrue(module.FallbackLines().empty(), L"healthy registration clears FallbackLines");

				statusLines.clear();
				module.OnStatus(ctx, RE::StatusKind::Fallbacks, statusLines, nullptr, nullptr);
				Assert::IsTrue(statusLines.empty(), L"OnStatus(Fallbacks) is empty once registered cleanly");
			}

			// ---- Part (b): RGB rejection, kAxisSize mismatch before GPU allocation, and OnStatus/FallbackLine ----
			{
				int fillHost = 0;
				SourceHostGuard hostGuard(&fillHost);

				pl_tex_t lumaTex{};
				lumaTex.params.w = 128;
				lumaTex.params.h = 96;
				pl_tex_t halfTex{};
				halfTex.params.w = 64;
				halfTex.params.h = 48;

				RefusingDevice dev;
				VpSkeleton::SkeletonMemo fillMemo;
				fillMemo.SetHost(&fillHost);
				fillMemo.SetRenderThread(std::this_thread::get_id());
				fillMemo.SetGpuDispatcherPresent(true);
				fillMemo.SetGpuBudget(true, uint64_t(1) << 28);
				fillMemo.SetGpuDevice(&dev);

				Assert::IsTrue(fillMemo.RegisterOwnerFill(VpSkeleton::kP13SourceRing, VpSkeleton::AcquireForm::Gpu,
					SourcePlanes::kUnit, SourcePlanes::MakeOwnerFill()) == VpSkeleton::SkeletonMemo::Registration::Registered,
					L"P13 source-planes owner registered");

				// Stub GPU P1 half-luma view so Stage::Half can prove BandMemoFill's own RGB guard stops RGB BEFORE
				// acquiring P1 or allocating GPU textures (independent of P13's own RGB check).
				VpSkeleton::OwnerFill halfFill;
				halfFill.view = true;
				halfFill.frameScoped = true;
				halfFill.fill = [&halfTex](const VpSkeleton::OwnerFillRequest&, VpSkeleton::SkeletonMemo&, VpSkeleton::OwnerFillOutput& out) {
					out.gpu.resource = &halfTex;
					out.gpu.width = uint32_t(halfTex.params.w);
					out.gpu.height = uint32_t(halfTex.params.h);
					out.gpu.bytes = 0;
					return true;
				};
				Assert::IsTrue(fillMemo.RegisterOwnerFill(VpSkeleton::kP1HalfLuma, VpSkeleton::AcquireForm::Gpu,
					VpSkeleton::SkeletonMemo::OwnerUnitOf(VpSkeleton::kP1HalfLuma), std::move(halfFill))
					== VpSkeleton::SkeletonMemo::Registration::Registered, L"stub GPU P1 half-luma owner registered");

				const VpSkeleton::Dims lumaDims{ 128, 96 };
				const uint64_t reserve = GB::BandMemoReserveBytes(lumaDims);

				// (b1) Direct MakeBandMemoOwnerFill: RGB offer (facts->rgb == true) rejected with "RGB" in LastDecline & FallbackLine.
				const std::shared_ptr<GB::Counters> directCounters = std::make_shared<GB::Counters>();
				const VpSkeleton::OwnerFill directFill = GB::MakeBandMemoOwnerFill(reserve, directCounters);

				SourcePlaneSet rgbSet;
				rgbSet.seq = 10;
				rgbSet.planes = 1;
				rgbSet.tex[0] = &lumaTex;
				rgbSet.components[0] = 3;
				rgbSet.repr.sys = PL_COLOR_SYSTEM_RGB;
				rgbSet.rgb = true;
				rgbSet.origin = "band_memo test rgb";
				Assert::IsTrue(SourcePlanes::Offer(&fillHost, rgbSet), L"RGB plane set offered");

				BM::BandKey fullKey;
				fullKey.sigmaPx = 1.5;
				fullKey.resolution = BM::Stage::Full;
				VpSkeleton::OwnerGpuContext gpuCtx(&dev, reserve);
				VpSkeleton::OwnerFillRequest req;
				req.host = &fillHost;
				req.id = VpSkeleton::kBandMemo;
				req.seq = 10;
				req.form = VpSkeleton::AcquireForm::Gpu;
				req.variant = BM::ToVariant(fullKey);
				req.gpu = &gpuCtx;

				VpSkeleton::OwnerFillOutput out;
				Assert::IsFalse(directFill.fill(req, fillMemo, out), L"RGB Full-stage request MUST be declined");
				Assert::IsTrue(Contains(out.declineReason, "RGB"), L"RGB declineReason names RGB");
				Assert::AreEqual(0, dev.asked, L"RGB decline happens before any GPU texture allocation");
				Assert::AreEqual(uint64_t(1), directCounters->declines.load(), L"RGB decline is counted");
				Assert::IsTrue(directCounters->lastDeclined.load(), L"lastDeclined engaged on RGB decline");
				Assert::IsTrue(Contains(directCounters->LastDecline(), "RGB"), L"counters->LastDecline() records RGB");
				const std::string rgbFallback = GB::FallbackLine(std::string(), directCounters->declines.load(),
					directCounters->LastDecline(), directCounters->lastDeclined.load());
				Assert::IsFalse(rgbFallback.empty(), L"FallbackLine is non-empty after RGB decline");
				Assert::IsTrue(Contains(rgbFallback, GB::kBandMemoFallbackId), L"FallbackLine carries product.band_memo_unavailable");
				Assert::IsTrue(Contains(rgbFallback, "RGB"), L"FallbackLine visibly surfaces RGB reason");

				// Same RGB offer on Stage::Half (where P1 would otherwise return a valid 64x48 texture):
				// BandMemoFill's own RGB check rejects it BEFORE P1/Allocate (dev.asked remains 0).
				BM::BandKey halfKey;
				halfKey.sigmaPx = 1.5;
				halfKey.resolution = BM::Stage::Half;
				req.variant = BM::ToVariant(halfKey);
				out = VpSkeleton::OwnerFillOutput();
				Assert::IsFalse(directFill.fill(req, fillMemo, out), L"RGB Half-stage request MUST also be declined");
				Assert::IsTrue(Contains(out.declineReason, "RGB"), L"RGB Half-stage declineReason names RGB");
				Assert::AreEqual(0, dev.asked, L"RGB Half-stage decline happens before any GPU texture allocation");

				// Same-input positive control (Rule 3): flipping ONLY rgb=false on the exact same Half-stage path
				// passes the RGB guard and reaches GPU allocation (dev.asked increments from 0 to 1).
				SourcePlaneSet yuvHalfSet = rgbSet;
				yuvHalfSet.seq = 11;
				yuvHalfSet.repr.sys = PL_COLOR_SYSTEM_BT_709;
				yuvHalfSet.rgb = false;
				yuvHalfSet.components[0] = 1;
				Assert::IsTrue(SourcePlanes::Offer(&fillHost, yuvHalfSet), L"YUV plane set offered for positive control");
				req.seq = 11;
				out = VpSkeleton::OwnerFillOutput();
				Assert::IsFalse(directFill.fill(req, fillMemo, out), L"refusing fake device declines at allocation");
				Assert::AreEqual(1, dev.asked, L"positive control: YUV passes RGB guard and reaches GPU texture allocation");
				Assert::IsFalse(Contains(out.declineReason, "RGB"), L"positive control: YUV decline is not an RGB rejection");

				// (b2) Mismatched kAxisSize checked BEFORE GPU texture allocation, routed through BandMemoModule + SkeletonMemo.
				dev.asked = 0;
				FakeDemand fillDemand;
				fillDemand.memo = &fillMemo;
				GB::BandMemoModule fillModule;
				Assert::IsTrue(fillModule.Observe(&fillHost, lumaDims, &fillDemand), L"BandMemoModule registers on fillMemo");
				Assert::IsTrue(fillModule.Registered(), L"fillModule registered");
				Assert::IsTrue(fillModule.FallbackLines().empty(), L"no fallback line before any fill decline");

				SourcePlaneSet yuvSet;
				yuvSet.seq = 20;
				yuvSet.planes = 1;
				yuvSet.tex[0] = &lumaTex;   // 128x96
				yuvSet.components[0] = 1;
				yuvSet.repr.sys = PL_COLOR_SYSTEM_BT_709;
				yuvSet.rgb = false;
				yuvSet.origin = "band_memo test yuv";
				Assert::IsTrue(SourcePlanes::Offer(&fillHost, yuvSet), L"128x96 YUV source offered");

				BM::BandKey wrongSizeKey;
				wrongSizeKey.sigmaPx = 1.5;
				wrongSizeKey.width = 64;
				wrongSizeKey.height = 48;
				const VpSkeleton::AcquireResult mismatchRes = fillMemo.Acquire(
					VpSkeleton::kBandMemo, 20, VpSkeleton::AcquireForm::Gpu, BM::ToVariant(wrongSizeKey));
				Assert::IsFalse(mismatchRes.Ok(), L"mismatched kAxisSize (64x48 vs 128x96) MUST be declined");
				Assert::IsTrue(mismatchRes.cause == VpSkeleton::AcquireCause::OwnerDeclined, L"cause is OwnerDeclined");
				Assert::AreEqual(0, dev.asked, L"size mismatch MUST be rejected BEFORE allocating GPU textures");
				Assert::AreEqual(uint64_t(1), fillModule.Stats().declines.load(), L"size mismatch decline counted in module Stats()");
				const std::string sizeDecline = fillModule.Stats().LastDecline();
				Assert::IsTrue(Contains(sizeDecline, "64x48") && Contains(sizeDecline, "128x96") && Contains(sizeDecline, "does not match"),
					L"LastDecline visibly names requested 64x48 vs input 128x96 mismatch");

				const std::vector<std::string> modFb = fillModule.FallbackLines();
				Assert::AreEqual(size_t(1), modFb.size(), L"BandMemoModule FallbackLines surfaces the fill decline");
				Assert::IsTrue(Contains(modFb[0], GB::kBandMemoFallbackId) && Contains(modFb[0], "64x48") && Contains(modFb[0], "128x96"),
					L"module FallbackLine names product.band_memo_unavailable and the size mismatch");

				RE::Context fillCtx{};
				fillCtx.host = &fillHost;
				std::vector<std::string> fillStatus;
				fillModule.OnStatus(fillCtx, RE::StatusKind::Fallbacks, fillStatus, nullptr, nullptr);
				Assert::AreEqual(size_t(1), fillStatus.size(), L"OnStatus(Fallbacks) emits the fill decline on VP-V pathway");
				Assert::AreEqual(modFb[0], fillStatus[0], L"OnStatus(Fallbacks) matches module FallbackLines()");

				std::vector<std::string> fillNonFb;
				fillModule.OnStatus(fillCtx, RE::StatusKind::Lines, fillNonFb, nullptr, nullptr);
				fillModule.OnStatus(fillCtx, RE::StatusKind::WorkerFault, fillNonFb, nullptr, nullptr);
				fillModule.OnStatus(fillCtx, RE::StatusKind::WorkerOk, fillNonFb, nullptr, nullptr);
				Assert::IsTrue(fillNonFb.empty(), L"OnStatus stays silent on non-Fallbacks status kinds during active decline");

				// Same-input positive controls (Rule 3): on the exact same 128x96 source luma (seq 20), both an
				// exact matching kAxisSize (128x96) and an unspecified size (0x0) pass the size check and proceed
				// to GPU texture allocation (dev.asked increments to 1, then 2).
				BM::BandKey matchSizeKey;
				matchSizeKey.sigmaPx = 1.5;
				matchSizeKey.width = 128;
				matchSizeKey.height = 96;
				const VpSkeleton::AcquireResult matchRes = fillMemo.Acquire(
					VpSkeleton::kBandMemo, 20, VpSkeleton::AcquireForm::Gpu, BM::ToVariant(matchSizeKey));
				Assert::IsFalse(matchRes.Ok(), L"fake device refuses texture allocation");
				Assert::AreEqual(1, dev.asked, L"positive control: matching 128x96 kAxisSize passes size guard to GPU allocation");
				Assert::IsFalse(Contains(fillModule.Stats().LastDecline(), "does not match"),
					L"positive control: matching 128x96 does not fail size check");

				BM::BandKey unspecSizeKey;
				unspecSizeKey.sigmaPx = 1.5;
				const VpSkeleton::AcquireResult unspecRes = fillMemo.Acquire(
					VpSkeleton::kBandMemo, 20, VpSkeleton::AcquireForm::Gpu, BM::ToVariant(unspecSizeKey));
				Assert::IsFalse(unspecRes.Ok(), L"fake device refuses texture allocation");
				Assert::AreEqual(2, dev.asked, L"positive control: unspecified 0x0 size passes size guard to GPU allocation");
				Assert::IsFalse(Contains(fillModule.Stats().LastDecline(), "does not match"),
					L"positive control: unspecified 0x0 size does not fail size check");

				fillMemo.ReleaseGpuSpots();
			}
		}

		// S2-H5 Follow-Up (MEDIUM-3): a failed ReplaceOwnerFill leaves the previously registered fill active on the
		// skeleton; BandMemoModule must preserve Registered()==true and the prior RegisteredReserveBytes(), surface the
		// failed replace visibly via FallbackLines()/OnStatus(Fallbacks) (Rule 4), use ReplaceOwnerFill (not RegisterOwnerFill,
		// which the real skeleton refuses with AlreadyRegistered) on the next resize, and clear regProblem_ + reset tried_
		// when returning to the already-registered size so retrying the previously failed size later succeeds.
		TEST_METHOD(BandMemoModulePreservesRegistrationOnFailedReplaceAndRetriesOnNextResize)
		{
			namespace GB = Vpv::GrainBand;
			namespace RE = RendererExtension;

			struct ReplaceDemand : Vpv::Skeleton::IExternalDemand
			{
				VpSkeleton::SkeletonMemo* memo = nullptr;
				bool refuseReplace = false;
				VpSkeleton::SkeletonMemo::Registration refuseCode = VpSkeleton::SkeletonMemo::Registration::BadDefaults;
				int registerCalls = 0;
				int replaceCalls = 0;
				uint64_t lastReserveBytes = 0;

				void SetExternalDemand(const void*, const void*, const std::vector<VpSkeleton::ConsumerDecl>&,
					const std::vector<VpSkeleton::ProductId>&) override {}
				void ClearExternalDemand(const void*, const void*) override {}

				VpSkeleton::SkeletonMemo::Registration RegisterOwnerFill(VpSkeleton::ProductId id, VpSkeleton::AcquireForm form,
					const std::string& unit, VpSkeleton::OwnerFill fill) override
				{
					++registerCalls;
					lastReserveBytes = fill.reserveBytes;
					return memo->RegisterOwnerFill(id, form, unit, std::move(fill));
				}

				VpSkeleton::SkeletonMemo::Registration ReplaceOwnerFill(VpSkeleton::ProductId id, VpSkeleton::AcquireForm form,
					const std::string& unit, VpSkeleton::OwnerFill fill) override
				{
					++replaceCalls;
					lastReserveBytes = fill.reserveBytes;
					if (refuseReplace)
						return refuseCode;
					return memo->ReplaceOwnerFill(id, form, unit, std::move(fill));
				}

				bool UnregisterOwnerFill(VpSkeleton::ProductId id, VpSkeleton::AcquireForm form, const std::string& unit) override
				{
					return memo->UnregisterOwnerFill(id, form, unit);
				}
			};

			VpSkeleton::SkeletonMemo memo;
			ReplaceDemand demand;
			demand.memo = &memo;

			GB::BandMemoModule module;
			int host = 0;
			RE::Context ctx{};
			ctx.host = &host;

			const VpSkeleton::Dims d1080{ 1920, 1080 };
			const VpSkeleton::Dims d1440{ 2560, 1440 };
			const VpSkeleton::Dims d4k{ 3840, 2160 };
			const uint64_t r1080 = GB::BandMemoReserveBytes(d1080);
			const uint64_t r1440 = GB::BandMemoReserveBytes(d1440);
			const uint64_t r4k = GB::BandMemoReserveBytes(d4k);
			const std::string r4kText = std::to_string(r4k);

			// 1. Initial registration at 1920x1080.
			Assert::IsTrue(module.Observe(&host, d1080, &demand), L"initial 1080p registration succeeds");
			Assert::IsTrue(module.Registered(), L"module is registered at 1080p");
			Assert::AreEqual(r1080, module.RegisteredReserveBytes(), L"registered reserve matches 1080p");
			Assert::AreEqual(1, demand.registerCalls, L"initial registration uses RegisterOwnerFill once");
			Assert::AreEqual(0, demand.replaceCalls, L"initial registration does not call ReplaceOwnerFill");
			Assert::AreEqual(r1080, demand.lastReserveBytes, L"registered fill carries 1080p reserve");
			Assert::IsTrue(memo.HasOwnerFill(VpSkeleton::kBandMemo, VpSkeleton::AcquireForm::Gpu), L"skeleton holds kBandMemo fill");
			Assert::IsTrue(module.FallbackLines().empty(), L"healthy initial registration has no fallback line");

			// 2. Refuse ReplaceOwnerFill on resize to 3840x2160 while keeping the 1080p fill registered in the skeleton.
			demand.refuseReplace = true;
			Assert::IsTrue(module.Observe(&host, d4k, &demand), L"failed replace preserves active registration");
			Assert::AreEqual(1, demand.registerCalls, L"failed replace does not call RegisterOwnerFill");
			Assert::AreEqual(1, demand.replaceCalls, L"resize invokes ReplaceOwnerFill once");
			Assert::IsTrue(module.Registered(), L"Registered() stays true after failed ReplaceOwnerFill");
			Assert::AreEqual(r1080, module.RegisteredReserveBytes(), L"RegisteredReserveBytes() stays at prior 1080p reserve");
			Assert::IsTrue(memo.HasOwnerFill(VpSkeleton::kBandMemo, VpSkeleton::AcquireForm::Gpu), L"prior 1080p fill remains registered");

			const std::vector<std::string> fb1 = module.FallbackLines();
			Assert::AreEqual(size_t(1), fb1.size(), L"failed replace emits a visible FallbackLine (Rule 4)");
			Assert::IsTrue(Contains(fb1[0], GB::kBandMemoFallbackId), L"fallback line carries product.band_memo_unavailable id");
			Assert::IsTrue(Contains(fb1[0], "refused: bad defaults"), L"fallback line names the replace refusal");
			Assert::IsTrue(Contains(fb1[0], r4kText.c_str()), L"fallback line names the requested 4K reserve");

			std::vector<std::string> statusLines;
			module.OnStatus(ctx, RE::StatusKind::Fallbacks, statusLines, nullptr, nullptr);
			Assert::AreEqual(size_t(1), statusLines.size(), L"OnStatus(Fallbacks) surfaces the failed replace");
			Assert::AreEqual(fb1[0], statusLines[0], L"OnStatus(Fallbacks) matches FallbackLines()");

			// Same-input negative control (Rule 3): prove the skeleton contract hazard is real — on the exact same
			// SkeletonMemo where the 1080p fill stayed registered, calling RegisterOwnerFill (what pre-fix code did
			// after clobbering registered_ to false) fails with AlreadyRegistered.
			const VpSkeleton::SkeletonMemo::Registration negReg = memo.RegisterOwnerFill(
				VpSkeleton::kBandMemo, VpSkeleton::AcquireForm::Gpu, GB::kBandMemoUnit, GB::MakeBandMemoOwnerFill(r1440));
			Assert::IsTrue(negReg == VpSkeleton::SkeletonMemo::Registration::AlreadyRegistered,
				L"negative control: real skeleton refuses RegisterOwnerFill with AlreadyRegistered while prior fill is active");

			// 3. Accept ReplaceOwnerFill again (while real skeleton still refuses RegisterOwnerFill with AlreadyRegistered)
			//    and resize to 2560x1440: module must call ReplaceOwnerFill (not RegisterOwnerFill) and recover cleanly.
			demand.refuseReplace = false;
			Assert::IsTrue(module.Observe(&host, d1440, &demand), L"next resize retries via ReplaceOwnerFill and succeeds");
			Assert::AreEqual(1, demand.registerCalls, L"next resize must NOT call RegisterOwnerFill");
			Assert::AreEqual(2, demand.replaceCalls, L"next resize calls ReplaceOwnerFill");
			Assert::IsTrue(module.Registered(), L"module stays registered after replace recovery");
			Assert::AreEqual(r1440, module.RegisteredReserveBytes(), L"RegisteredReserveBytes() updates to 1440p");
			Assert::AreEqual(r1440, demand.lastReserveBytes, L"replaced fill carries 1440p reserve");
			Assert::IsTrue(module.FallbackLines().empty(), L"successful replace clears FallbackLines");

			statusLines.clear();
			module.OnStatus(ctx, RE::StatusKind::Fallbacks, statusLines, nullptr, nullptr);
			Assert::IsTrue(statusLines.empty(), L"OnStatus(Fallbacks) clears after successful replace");

			// 4. Returning to the already-registered size (2560x1440) after a failed replace (3840x2160) clears
			//    regProblem_ and resets tried_ so retrying the exact previously failed size (3840x2160) later succeeds.
			demand.refuseReplace = true;
			Assert::IsTrue(module.Observe(&host, d4k, &demand), L"second failed replace preserves 1440p registration");
			Assert::AreEqual(1, demand.registerCalls, L"still no extra RegisterOwnerFill");
			Assert::AreEqual(3, demand.replaceCalls, L"4K resize invokes ReplaceOwnerFill");
			Assert::IsTrue(module.Registered(), L"Registered() stays true on second failed replace");
			Assert::AreEqual(r1440, module.RegisteredReserveBytes(), L"prior 1440p reserve preserved on second failed replace");
			const std::vector<std::string> fb2 = module.FallbackLines();
			Assert::AreEqual(size_t(1), fb2.size(), L"second failed replace emits visible FallbackLine");
			Assert::IsTrue(Contains(fb2[0], "refused: bad defaults") && Contains(fb2[0], r4kText.c_str()),
				L"second failed replace names refusal and 4K reserve");

			// Same-input negative control (Rule 3): before returning to 1440p, re-observing 4K stays latched at tried_==r4k.
			demand.refuseReplace = false;
			Assert::IsTrue(module.Observe(&host, d4k, &demand), L"negative control: latched 4K keeps active 1440p registration");
			Assert::AreEqual(3, demand.replaceCalls, L"negative control: without returning to registered size, tried_ suppresses 4K retry");
			Assert::AreEqual(r1440, module.RegisteredReserveBytes(), L"negative control: reserve stays 1440p while 4K is latched");
			Assert::AreEqual(size_t(1), module.FallbackLines().size(), L"negative control: fallback persists while latched at failed size");

			// Returning to the already-registered size clears regProblem_ and resets tried_ back to reserve_.
			Assert::IsTrue(module.Observe(&host, d1440, &demand), L"returning to already-registered 1440p succeeds");
			Assert::AreEqual(3, demand.replaceCalls, L"returning to already-registered size needs no ReplaceOwnerFill");
			Assert::AreEqual(1, demand.registerCalls, L"returning to already-registered size needs no RegisterOwnerFill");
			Assert::IsTrue(module.Registered(), L"still registered at 1440p");
			Assert::AreEqual(r1440, module.RegisteredReserveBytes(), L"reserve stays 1440p");
			Assert::IsTrue(module.FallbackLines().empty(), L"returning to already-registered size clears regProblem_");

			// Retrying the exact previously failed size (3840x2160) now succeeds because tried_ was reset.
			Assert::IsTrue(module.Observe(&host, d4k, &demand), L"retrying previously failed 4K succeeds after returning to registered size");
			Assert::AreEqual(1, demand.registerCalls, L"4K retry uses ReplaceOwnerFill, not RegisterOwnerFill");
			Assert::AreEqual(4, demand.replaceCalls, L"returning to registered size reset tried_, so 4K retries ReplaceOwnerFill");
			Assert::IsTrue(module.Registered(), L"module is registered at 4K after retry");
			Assert::AreEqual(r4k, module.RegisteredReserveBytes(), L"RegisteredReserveBytes() updates to 4K");
			Assert::AreEqual(r4k, demand.lastReserveBytes, L"replaced fill carries 4K reserve");
			Assert::IsTrue(memo.HasOwnerFill(VpSkeleton::kBandMemo, VpSkeleton::AcquireForm::Gpu), L"skeleton holds updated kBandMemo fill");
			Assert::IsTrue(module.FallbackLines().empty(), L"FallbackLines stays clear after 4K retry succeeds");
		}
	};
}

namespace BandFromBaseTestsNs
{
	// Frozen copies of the private implementations the shared unit replaced (pre-migration, verbatim).
	namespace Frozen
	{
		inline int GaussRadius(double sigma) { return (std::max)(1, static_cast<int>(std::ceil(3.0 * sigma))); }
		inline double GaussWeight(int k, double sigma)
		{
			const int r = GaussRadius(sigma);
			double sum = 0.0;
			for (int i = -r; i <= r; ++i) sum += std::exp(-0.5 * i * i / (sigma * sigma));
			return std::exp(-0.5 * k * k / (sigma * sigma)) / sum;
		}
		inline GrainSplit::Plane Gauss(const GrainSplit::Plane& in, double sigma)
		{
			using GrainSplit::Plane;
			if (!(sigma > 0.0)) return in;
			const int r = (std::max)(1, static_cast<int>(std::ceil(3.0 * sigma)));
			std::vector<double> k(static_cast<size_t>(2 * r + 1));
			double sum = 0.0;
			for (int i = -r; i <= r; ++i) sum += k[static_cast<size_t>(i + r)] = std::exp(-0.5 * i * i / (sigma * sigma));
			for (double& x : k) x /= sum;
			Plane t(in.w, in.h), o(in.w, in.h);
			std::vector<double> pad(static_cast<size_t>(in.w + 2 * r));
			for (int y = 0; y < in.h; ++y)
			{
				for (int x = -r; x < in.w + r; ++x) pad[static_cast<size_t>(x + r)] = in.get(x, y);
				for (int x = 0; x < in.w; ++x)
				{
					double a = 0.0;
					for (int i = -r; i <= r; ++i) a += k[static_cast<size_t>(i + r)] * pad[static_cast<size_t>(x + r + i)];
					t.at(x, y) = a;
				}
			}
			for (int y = 0; y < in.h; ++y)
			{
				double* dst = &o.v[static_cast<size_t>(y) * in.w];
				for (int i = -r; i <= r; ++i)
				{
					const double* src = &t.v[static_cast<size_t>((std::max)(0, (std::min)(in.h - 1, y + i))) * in.w];
					const double kk = k[static_cast<size_t>(i + r)];
					for (int x = 0; x < in.w; ++x) dst[x] += kk * src[x];
				}
			}
			return o;
		}
		// The GLSL weights block as GrainSplitGpu.cpp's WeightsGlsl built it.
		inline std::string WeightsGlsl(const char* name, const char* radiusName, double sigma)
		{
			const int r = GaussRadius(sigma);
			return VpvGpu::DefineInt(radiusName, r) + "float " + name + "(int k)\n{\n    " +
				VpvGpu::ConstArray("w", 2 * r + 1, [&](int i) { return GaussWeight(i - r, sigma); }) + "    return w[k + " + radiusName + "];\n}\n";
		}
	}

	inline bool BitEqual(double a, double b) { return std::memcmp(&a, &b, sizeof(double)) == 0; }

	inline GrainSplit::Plane TestPlane(int w, int h, unsigned seed)
	{
		GrainSplit::Plane p(w, h);
		unsigned s = seed;
		for (double& v : p.v)
		{
			s = s * 1664525u + 1013904223u;
			v = static_cast<double>((s >> 8) & 1023u);
		}
		return p;
	}

	TEST_CLASS(BandFromBaseUnitTests)
	{
	public:
		TEST_METHOD(UnitVersionIsOne)
		{
			Assert::AreEqual(uint32_t(1), VpUnit::BandFromBase::kUnitVersion);
		}

		TEST_METHOD(RadiusWeightsHalfSigmaAndBlurMatchThePythonGolden)
		{
			using namespace BandFromBaseGolden;
			for (int i = 0; i < kSigmaCount; ++i)
			{
				const double s = kSigmas[i];
				const int r = VpUnit::BandFromBase::Radius(s, 3.0, true, 0.0);
				Assert::AreEqual(kRadius[i], r, L"radius");
				for (int k = -r; k <= r; ++k)
					Assert::IsTrue(std::fabs(VpUnit::BandFromBase::Weight(k, s, r) - kWeights[i][k + r]) < 1e-14, L"weight vs python");
				if (s > 0.5)
					Assert::IsTrue(std::fabs(VpUnit::BandFromBase::HalfSigma(s) - kHalfSigma[i]) < 1e-14, L"half sigma vs python");
			}
			const std::vector<double> plane(kPlane, kPlane + kPlaneW * kPlaneH);
			const std::vector<double> a = VpUnit::BandFromBase::Blur(plane, kPlaneW, kPlaneH, kBlurSigmaA);
			const std::vector<double> b = VpUnit::BandFromBase::Blur(plane, kPlaneW, kPlaneH, kBlurSigmaB);
			for (int i = 0; i < kPlaneW * kPlaneH; ++i)
			{
				Assert::IsTrue(std::fabs(a[static_cast<size_t>(i)] - kBlurA[i]) < 1e-12, L"blur A vs python");
				Assert::IsTrue(std::fabs(b[static_cast<size_t>(i)] - kBlurB[i]) < 1e-12, L"blur B vs python");
			}
		}

		TEST_METHOD(WeightsAreNormalisedAndSymmetric)
		{
			for (double s : { 0.4, 1.0, 2.5, 6.0, 15.0 })
			{
				const int r = VpUnit::BandFromBase::Radius(s, 3.0, true, 0.0);
				double sum = 0.0;
				for (int k = -r; k <= r; ++k)
				{
					const double w = VpUnit::BandFromBase::Weight(k, s, r);
					sum += w;
					Assert::IsTrue(BitEqual(w, VpUnit::BandFromBase::Weight(-k, s, r)), L"symmetric");
				}
				Assert::IsTrue(std::fabs(sum - 1.0) < 1e-12, L"normalised");
			}
		}

		TEST_METHOD(SigmaFromPictureHeightFractionAndGlslEntryNames)
		{
			VpUnit::BandFromBase::Params p;
			p.sigmaRel = 1.0 / 1080.0;
			p.activeHeight = 2160.0;
			Assert::IsTrue(std::fabs(p.SigmaPx() - 2.0) < 1e-12, L"sigma is a fraction of the active picture height");
			Assert::AreEqual(6, VpUnit::BandFromBase::Radius(p), L"radius from the params");
			const std::string g = VpUnit::BandFromBase::Glsl(p);
			Assert::IsTrue(g.find("vpu_band_from_base_w(int k)") != std::string::npos);
			Assert::IsTrue(g.find("vpu_band_from_base_across") != std::string::npos);
			Assert::IsTrue(g.find("vpu_band_from_base_down") != std::string::npos);
			Assert::IsTrue(g.find("VPU_BAND_FROM_BASE_VERSION 1") != std::string::npos);
			Assert::IsTrue(g.find("VPU_BAND_FROM_BASE_R 6") != std::string::npos);
			// The unit's weight literals are the shared formatter's: the same text a ConstArray of Weight() gives.
			const int r = VpUnit::BandFromBase::Radius(p);
			const std::string tab = VpvGpu::ConstArray("vpu_band_from_base_tab", 2 * r + 1,
				[&](int i) { return VpUnit::BandFromBase::Weight(i - r, p.SigmaPx(), r); });
			Assert::IsTrue(g.find(tab) != std::string::npos, L"table text identical to VpvGpu::ConstArray");
		}
	};

	// B1: GrainSplit's own Gauss kernel and GrainSplitGpu's GaussRadius/GaussWeight now call the unit; their numbers
	// and the GLSL they emit must be unchanged.
	TEST_CLASS(GrainSplitBandFromBaseIdentityTests)
	{
	public:
		TEST_METHOD(GaussRadiusAndWeightAreBitIdenticalToTheFrozenPrivateCopy)
		{
			for (double s = 0.1; s < 20.0; s += 0.137)
			{
				Assert::AreEqual(Frozen::GaussRadius(s), GrainSplit::Gpu::GaussRadius(s), L"radius");
				const int r = Frozen::GaussRadius(s);
				for (int k = -r; k <= r; ++k)
					Assert::IsTrue(BitEqual(Frozen::GaussWeight(k, s), GrainSplit::Gpu::GaussWeight(k, s)), L"weight bits");
			}
			for (double base : { 1.0, 2.5, 6.0 })
				for (double scale : { 1.0, 1.25, 1.5, 2.0, 2.5, 4.0 })
				{
					const double s = base * scale;
					Assert::AreEqual(Frozen::GaussRadius(s), GrainSplit::Gpu::GaussRadius(s));
					for (int k = -Frozen::GaussRadius(s); k <= Frozen::GaussRadius(s); ++k)
						Assert::IsTrue(BitEqual(Frozen::GaussWeight(k, s), GrainSplit::Gpu::GaussWeight(k, s)));
				}
		}

		TEST_METHOD(EmittedWeightsGlslIsByteIdentical)
		{
			for (double base : { 1.0, 2.5, 6.0 })
				for (double scale : { 1.0, 1.5, 2.0, 2.5 })
				{
					const double s = base * scale;
					const int r = GrainSplit::Gpu::GaussRadius(s);
					const std::string now = VpvGpu::DefineInt("GS_R", r) + "float gs_w(int k)\n{\n    " +
						VpvGpu::ConstArray("w", 2 * r + 1, [&](int i) { return GrainSplit::Gpu::GaussWeight(i - r, s); }) + "    return w[k + GS_R];\n}\n";
					Assert::AreEqual(Frozen::WeightsGlsl("gs_w", "GS_R", s), now, L"GLSL text");
				}
		}

		TEST_METHOD(CpuGaussIsBitIdenticalToTheFrozenPrivateCopy)
		{
			const int dims[][2] = { { 1, 1 }, { 2, 3 }, { 5, 3 }, { 33, 17 }, { 64, 48 }, { 7, 40 } };
			unsigned seed = 11;
			for (const auto& d : dims)
				for (double s : { 0.4, 1.0, 1.5, 2.5, 6.0, 7.5 })
				{
					const GrainSplit::Plane in = TestPlane(d[0], d[1], seed++);
					const GrainSplit::Plane a = Frozen::Gauss(in, s), b = GrainSplit::Gauss(in, s);
					Assert::AreEqual(a.v.size(), b.v.size());
					Assert::IsTrue(std::memcmp(a.v.data(), b.v.data(), a.v.size() * sizeof(double)) == 0, L"Gauss bits");
				}
			const GrainSplit::Plane in = TestPlane(8, 8, 3);
			Assert::IsTrue(std::memcmp(Frozen::Gauss(in, 0.0).v.data(), GrainSplit::Gauss(in, 0.0).v.data(), 64 * sizeof(double)) == 0, L"sigma 0 passes through");
		}

		TEST_METHOD(CpuSplitComputeIsUnchanged)
		{
			// ComputeV2() (the v2 law) uses Gauss three times: base = G(2.5) Y and d1 = Y - G(1) Y equal the frozen-Gauss composition.
			// d1 is the full-resolution fine band in v3 too. WAIVER-P10V3-01 (approved v3 law, refs/116): the v3 base is the
			// half-resolution blur read bilinearly, so "Compute().base == frozen G(2.5) Y" (old) is now asserted on ComputeV2()
			// (the retained v2 law, same arithmetic as before) and v3's d1 stays checked against the frozen G(1).
			const GrainSplit::Plane y = TestPlane(48, 40, 99);
			const GrainSplit::Split a = GrainSplit::Compute(y, 1.0, 16);
			const GrainSplit::Split a2 = GrainSplit::ComputeV2(y, 1.0, 16);
			const GrainSplit::Plane g1 = Frozen::Gauss(y, 1.0), gb = Frozen::Gauss(y, 2.5);
			Assert::IsTrue(std::memcmp(a2.base.v.data(), gb.v.data(), gb.v.size() * sizeof(double)) == 0, L"v2 base bits");
			for (size_t i = 0; i < y.v.size(); ++i)
				Assert::IsTrue(BitEqual(a.d1.v[i], y.v[i] - g1.v[i]), L"d1 bits");
		}
	};
}
