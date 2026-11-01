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
#include <VP-V/modules/repair/grain_split/GrainSplitGpu.h>
#include <VP-V/modules/repair/grain_split/GpuPassUtil.h>
#include <VP-V/modules/repair/grain_split/shared/BandFromBase.h>
#include "BandFromBaseGolden.generated.h"

#include <cmath>
#include <cstdint>
#include <cstring>
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
			Assert::AreEqual(size_t(48008), text.size(), L"the reversed text has the accessor-free (K0) size");
			Assert::AreEqual(uint64_t(0xb77b206dff1cdaf2ull), h, L"the reversed text hashes to the accessor-free (K0) shader text");
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
			// Compute() uses Gauss three times: base = G(2.5) Y and d1 = Y - G(1) Y equal the frozen-Gauss composition.
			const GrainSplit::Plane y = TestPlane(48, 40, 99);
			const GrainSplit::Split a = GrainSplit::Compute(y, 1.0, 16);
			const GrainSplit::Plane g1 = Frozen::Gauss(y, 1.0), gb = Frozen::Gauss(y, 2.5);
			Assert::IsTrue(std::memcmp(a.base.v.data(), gb.v.data(), gb.v.size() * sizeof(double)) == 0, L"base bits");
			for (size_t i = 0; i < y.v.size(); ++i)
				Assert::IsTrue(BitEqual(a.d1.v[i], y.v[i] - g1.v[i]), L"d1 bits");
		}
	};
}
