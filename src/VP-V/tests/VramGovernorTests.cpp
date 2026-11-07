// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
// Built without the test project's precompiled header: keep windows.h from defining min/max.
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include "CppUnitTest.h"

// Skeleton S2 VRAM governor (src/VP-V/core/VramGovernor.h): one budget and one shed order for every
// VRAM user, the skeleton products included (PlanTiers). Synthetic 4 GB card at 4K with all
// features on (figures from bug hunt doc 38, high case), the transient 2x early-upload pool, a
// 3090, own usage re-planned, pressure/headroom over time and determinism. CPU only.

#include <VP-V/core/VramGovernor.h>

#include <string>
#include <vector>

using namespace Microsoft::VisualStudio::CppUnitTestFramework;

namespace VramGovernorTestsNs
{
	using namespace VpSkeleton;

	constexpr uint64_t kMB = 1024ull * 1024ull;

	VramUser User(uint16_t id, const char* name, Tier tier, uint8_t order, std::vector<uint64_t> mb,
		std::vector<const char*> text = {}, bool lossless = false)
	{
		VramUser u;
		u.id = id;
		u.name = name;
		u.tier = tier;
		u.shedOrder = order;
		u.lossless = lossless;
		for (uint64_t m : mb)
			u.levels.push_back(m * kMB);
		u.levelText = text;
		return u;
	}

	// The 4K feature set (every non-skeleton VRAM user), high case of doc 38.
	std::vector<VramUser> OtherUsers4k()
	{
		std::vector<VramUser> u;
		u.push_back(User(kUserLibplacebo, "libplacebo intermediates", Tier::T0, 0, { 660 }));
		u.push_back(User(kUserCapturePlanes, "capture planes", Tier::T0, 0, { 200 }));
		u.push_back(User(kUserNvofProducer, "vector engine", Tier::T0, 0, { 200, 120 },
			{ nullptr, "at quarter input" }));
		u.push_back(User(kUserRenderAhead, "render-ahead", Tier::T2, 0, { 200, 133, 67, 0 },
			{ nullptr, "depth 6 -> 4", "depth 6 -> 2", "off" }, true));
		u.push_back(User(kUserEarlyUpload, "early upload", Tier::T2, 1, { 800, 600, 400, 200 },
			{ nullptr, "slots 16 -> 12", "slots 16 -> 8", "slots 16 -> 4" }, true));
		u.push_back(User(kUserInterpolator, "interpolator", Tier::T2, 3, { 300, 0 }));
		u.push_back(User(kUserTemporalDetail, "temporal detail", Tier::T1, 0, { 400, 224, 0 },
			{ nullptr, "without future slots", "off" }));
		u.push_back(User(kUserCameraModel, "camera model", Tier::T1, 1, { 100, 50 }));
		u.push_back(User(kUserEnhancers, "texture restore / detail pulse", Tier::T1, 2, { 300, 150, 0 }));
		return u;
	}

	Registry R4b()
	{
		std::vector<ProductDecl> d = FirstProducts().All();
		d[kP0RebaseDecision].backend = Backend::Cpu;
		return Registry(std::move(d));
	}

	// Every skeleton product at 4K, render-ahead 2.
	Graph Products4k(const Registry& reg)
	{
		ConsumerDecl c;
		c.id = 1;
		AddConsume(c, kP6EntranceAhead);
		AddConsume(c, kP5MonitorTiles);
		AddConsume(c, kP4SourceStats);
		GraphSettings gs;
		gs.dims.width = 3840;
		gs.dims.height = 2160;
		gs.renderAhead = 2;
		return BuildGraph(reg, { c }, gs);
	}

	TierInput AllFeatures()
	{
		TierInput in;
		in.otherUsers = OtherUsers4k();
		return in;
	}

	CardProfile Card4GB()
	{
		CardProfile c;
		c.key.luid = 9;
		c.vramBudgetBytes = 3300 * kMB;   // WDDM budget on the 4 GB 3050 Ti
		c.vramUsageBytes = 450 * kMB;     // desktop and other apps; VP not started
		c.systemRamFreeBytes = 8000 * kMB;
		c.uploadGBps = 12.0;
		c.ofaPresent = true;
		c.ofaMsPerPairRef = 11.0;
		return c;
	}

	TEST_CLASS(VramGovernorTests)
	{
	public:
		TEST_METHOD(FourGigabyteCardAt4kAllFeaturesFitsByShedding)
		{
			const Registry reg = R4b();
			const Graph g = Products4k(reg);
			const TierPlan p = PlanTiers(reg, g, Card4GB(), AllFeatures());
			const GovernorPlan& gov = p.governor;
			uint64_t want = 0;
			for (const VramUser& u : OtherUsers4k())
				want += u.levels[0];
			Assert::IsTrue(want > gov.budgetBytes, L"the features alone exceed the budget at full need");
			Assert::IsTrue(gov.fits);
			Assert::IsTrue(gov.totalBytes <= gov.budgetBytes);
			// Fixed users are never cut; render-ahead goes first, then early upload.
			Assert::AreEqual(size_t(0), gov.Find(kUserLibplacebo)->level);
			Assert::AreEqual(size_t(0), gov.Find(kUserCapturePlanes)->level);
			Assert::IsTrue(gov.Find(kUserRenderAhead)->shed);
			Assert::IsTrue(gov.Find(kUserEarlyUpload)->reduced);
			// Within T1 the entrance output's identical in-render level goes before temporal detail
			// loses anything.
			const UserGrant* p6 = gov.Find(uint16_t(kUserProductBase + kP6EntranceAhead));
			Assert::IsNotNull(p6);
			if (gov.Find(kUserTemporalDetail)->reduced)
				Assert::AreEqual(size_t(1), p6->level);
			// Product grants follow the governor levels; system-RAM stats are not VRAM users.
			for (const TierEntry& e : p.entries) {
				const UserGrant* u = gov.Find(uint16_t(kUserProductBase + e.id));
				if (e.residency == Residency::SystemRam) {
					Assert::IsNull(u);
					continue;
				}
				Assert::IsNotNull(u);
				if (e.grant == Grant::Lane)
					Assert::AreEqual(size_t(0), u->level);
				else if (e.why == GrantWhy::OverBudget)
					Assert::AreEqual(size_t(1), u->level);
			}
			Assert::IsTrue(p.Laned(kP4SourceStats));
			// Every cutback is visible, one line each, and in the notice.
			size_t reduced = 0;
			for (const UserGrant& u : gov.grants)
				reduced += u.reduced ? 1u : 0u;
			Assert::AreEqual(reduced, gov.lines.size());
			for (const std::string& l : gov.lines)
				Assert::AreNotEqual(std::string::npos, p.notice.find(l));
			bool sawSlots = false;
			for (const std::string& l : gov.lines)
				sawSlots = sawSlots || l.find("early upload slots 16 -> ") != std::string::npos;
			Assert::IsTrue(sawSlots);
			Logger::WriteMessage(p.notice.c_str());
		}

		TEST_METHOD(ShedOrderIsTierThenLosslessThenOrder)
		{
			GovernorInput in;
			in.card = Card4GB();
			in.users = OtherUsers4k();
			std::vector<size_t> level;
			const GovernorPlan p = PlanVram(in, &level);
			for (size_t i = 0; i < in.users.size(); ++i) {
				if (level[i] == 0)
					continue;
				for (size_t j = 0; j < in.users.size(); ++j) {
					const VramUser& a = in.users[j];
					const VramUser& b = in.users[i];
					const bool first = uint8_t(a.tier) > uint8_t(b.tier)
						|| (a.tier == b.tier && a.lossless && !b.lossless)
						|| (a.tier == b.tier && a.lossless == b.lossless
							&& (a.shedOrder < b.shedOrder || (a.shedOrder == b.shedOrder && a.id < b.id)));
					if (first)
						Assert::AreEqual(a.levels.size() - 1, level[j], L"an earlier user is at its minimum");
				}
			}
			Assert::IsTrue(p.fits);
		}

		TEST_METHOD(ASingleStoreLaneDropRanksAsLossyNotLossless)
		{
			// S2: a single-store product has no live fill, so its lane-drop level is "not kept" (its readers lose the store and run their own
			// fallback): a reduced, NON-identical mode. It must rank as lossy: an ordinary product of the same tier, whose lane drop is an
			// identical in-render recompute, sheds first even though the store would otherwise come earlier in the reverse topological order.
			using namespace VpIndicator;
			constexpr ProductId kPlain = 0, kStore = 1, kReader = 2;
			const CounterMask content = Bit(kFull) | Bit(kContinuity);
			std::vector<ProductDecl> d;
			d.push_back(detail::Make(kPlain, "plain", "plain product", Backend::GpuCompute, 0, Tier::T0, content, 0));
			ProductDecl store = detail::Make(kStore, "store", "single store", Backend::GpuCompute, 0, Tier::T0, content, 0);
			store.singletonStore = true;
			d.push_back(store);
			ProductDecl reader = detail::Make(kReader, "reader", "store reader", Backend::RenderOnly, 0, Tier::T0, content, 0);
			detail::AddInput(reader, kStore, 1);
			d.push_back(reader);
			d[kPlain].size.fixedBytes = 64u * 1024u * 1024u;
			d[kStore].size.fixedBytes = 64u * 1024u * 1024u;
			d[kReader].size.fixedBytes = 64u;
			const Registry reg(d);
			ConsumerDecl c;
			c.id = 1;
			AddConsume(c, kPlain);
			AddConsume(c, kReader);
			GraphSettings gs;
			gs.dims.width = 64;
			gs.dims.height = 64;
			gs.renderAhead = 0;
			gs.laneLead = 1;
			gs.queueDepth = 4;
			const Graph g = BuildGraph(reg, { c }, gs);
			Assert::IsTrue(g.valid);
			Assert::AreEqual(2, int(g.Find(kStore)->ring), L"the store is a two-buffer ping-pong with no live pool");
			Assert::AreEqual(0, int(g.Find(kStore)->livePool));
			// Full need: plain 3 ring + 1 live buffers, store 2 buffers = 6 x 64 MiB. One step is enough at 300 MiB, whichever sheds first.
			CardProfile card = Card4GB();
			card.vramUsageBytes = 0;
			card.vramBudgetBytes = 375 * kMB;   // x the starting headroom 0.80 = 300 MiB
			TierInput in;
			const TierPlan p = PlanTiers(reg, g, card, in);
			const UserGrant* plain = p.governor.Find(uint16_t(kUserProductBase + kPlain));
			const UserGrant* st = p.governor.Find(uint16_t(kUserProductBase + kStore));
			Assert::IsNotNull(plain);
			Assert::IsNotNull(st);
			Assert::AreEqual(size_t(1), plain->level, L"the lossless in-render drop of the ordinary product goes first");
			Assert::AreEqual(size_t(0), st->level, L"the single store keeps its lane: dropping it changes the picture (lossy)");
			Assert::IsTrue(p.governor.fits);
			// With the budget cut further the store is shed too, after the ordinary product, and says so visibly.
			card.vramBudgetBytes = 100 * kMB;   // 80 MiB
			const TierPlan tight = PlanTiers(reg, g, card, in);
			Assert::AreEqual(size_t(1), tight.governor.Find(uint16_t(kUserProductBase + kPlain))->level);
			Assert::AreEqual(size_t(1), tight.governor.Find(uint16_t(kUserProductBase + kStore))->level);
			bool said = false;
			for (const std::string& l : tight.governor.lines)
				said = said || l.find("not kept (no live fill") != std::string::npos;
			Assert::IsTrue(said, L"the store's drop is on the status lines");
		}

		TEST_METHOD(TransientPoolAtASizeChangeShedsMoreButStaysVisible)
		{
			const Registry reg = R4b();
			const Graph g = Products4k(reg);
			TierInput in = AllFeatures();
			const TierPlan calm = PlanTiers(reg, g, Card4GB(), in);
			in.transientBytes = 800 * kMB;   // the old pool alongside the new one (doc 38 M3)
			const TierPlan tr = PlanTiers(reg, g, Card4GB(), in);
			Assert::IsTrue(tr.governor.steps > calm.governor.steps);
			Assert::IsTrue(tr.governor.lines.size() >= calm.governor.lines.size());
			if (!tr.governor.fits)
				Assert::AreEqual(std::string("Video memory: over budget even at minimum settings"),
					tr.governor.lines.back());
		}

		TEST_METHOD(OwnUsageIsReplannedNotCountedTwice)
		{
			GovernorInput in;
			in.card = Card4GB();
			in.users = OtherUsers4k();
			const GovernorPlan before = PlanVram(in);
			in.card.vramUsageBytes += 1500 * kMB;   // VP running: CurrentUsage includes its own memory
			in.ownUsageBytes = 1500 * kMB;
			const GovernorPlan running = PlanVram(in);
			Assert::AreEqual(before.budgetBytes, running.budgetBytes);
			Assert::AreEqual(before.steps, running.steps);
		}

		TEST_METHOD(BigCardShedsNothing)
		{
			const Registry reg = R4b();
			CardProfile big = Card4GB();
			big.vramBudgetBytes = 23000 * kMB;
			const TierPlan p = PlanTiers(reg, Products4k(reg), big, AllFeatures());
			Assert::IsTrue(p.governor.fits);
			Assert::AreEqual(uint32_t(0), p.governor.steps);
			Assert::IsTrue(p.notice.empty());
			Assert::IsTrue(p.Laned(kP6EntranceAhead));
		}

		TEST_METHOD(ImpossibleBudgetSaysSo)
		{
			GovernorInput in;
			in.card = Card4GB();
			in.card.vramBudgetBytes = 900 * kMB;   // fixed users alone exceed it
			in.users = OtherUsers4k();
			std::vector<size_t> level;
			const GovernorPlan p = PlanVram(in, &level);
			Assert::IsFalse(p.fits);
			Assert::AreEqual(std::string("Video memory: over budget even at minimum settings"), p.lines.back());
			for (size_t i = 0; i < in.users.size(); ++i)
				Assert::AreEqual(in.users[i].levels.size() - 1, level[i], L"every user at its minimum");
		}

		TEST_METHOD(GovernorShrinksOnPressureAndRecoversAfterADwell)
		{
			const Registry reg = R4b();
			const Graph g = Products4k(reg);
			VramGovernor gov;
			const uint32_t calmSteps = gov.Update(reg, g, Card4GB(), AllFeatures()).governor.steps;
			const double h0 = gov.Headroom();
			CardProfile pressed = Card4GB();
			pressed.vramUsageBytes = pressed.vramBudgetBytes + kMB;   // over the WDDM budget
			TierInput in = AllFeatures();
			in.ownUsageBytes = pressed.vramUsageBytes - 450 * kMB;
			gov.Update(reg, g, pressed, in);
			Assert::IsTrue(gov.Headroom() < h0);
			const uint32_t tight = gov.Update(reg, g, Card4GB(), AllFeatures()).governor.steps;
			Assert::IsTrue(tight >= calmSteps, L"the smaller headroom keeps more shed");
			for (int i = 0; i < 200; ++i)
				gov.Update(reg, g, Card4GB(), AllFeatures());
			Assert::IsTrue(gov.Headroom() >= h0);
		}

		TEST_METHOD(SameInputsGiveTheSameGovernorPlan)
		{
			const Registry reg = R4b();
			const Graph g = Products4k(reg);
			TierInput in = AllFeatures();
			in.transientBytes = 300 * kMB;
			const TierPlan a = PlanTiers(reg, g, Card4GB(), in);
			const TierPlan b = PlanTiers(reg, g, Card4GB(), in);
			Assert::AreEqual(a.governor.grants.size(), b.governor.grants.size());
			for (size_t i = 0; i < a.governor.grants.size(); ++i)
				Assert::AreEqual(a.governor.grants[i].level, b.governor.grants[i].level);
			Assert::AreEqual(a.notice, b.notice);
		}
	};
}
