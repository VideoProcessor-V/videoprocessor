// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
// Built without the test project's precompiled header: keep windows.h from defining min/max.
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include "CppUnitTest.h"

// Skeleton S2 (src/VP-V/core/CardProfile.h): synthetic card profiles (laptop 3050 Ti, 3090, a
// small card, no optical-flow engine), the tier grant (T0 first, then T1/T2 by ms saved per MB,
// under free VRAM x headroom), system-RAM rules, the optical-flow load, the profile cache, the
// adaptive headroom and determinism. CPU only.

#include <VP-V/core/VramGovernor.h>

#include <string>
#include <vector>

using namespace Microsoft::VisualStudio::CppUnitTestFramework;

namespace CardProfileTestsNs
{
	using namespace VpSkeleton;

	constexpr uint64_t kMB = 1024ull * 1024ull;

	CardProfile Laptop()
	{
		CardProfile c;
		c.key.luid = 1;
		c.key.driverVersion = 100;
		c.vramBudgetBytes = 3500 * kMB;
		c.vramUsageBytes = 1000 * kMB;
		c.systemRamFreeBytes = 8000 * kMB;
		c.copyGBps = 150.0;
		c.uploadGBps = 12.0;
		c.computeScale = 1.0;
		c.ofaPresent = true;
		c.ofaMsPerPairRef = 11.0;
		c.measured = true;
		return c;
	}

	CardProfile Rtx3090()
	{
		CardProfile c = Laptop();
		c.key.luid = 2;
		c.vramBudgetBytes = 23000 * kMB;
		c.vramUsageBytes = 2000 * kMB;
		c.computeScale = 4.0;
		c.ofaMsPerPairRef = 8.0;
		return c;
	}

	// R4b form: the re-base decision moves to the lane, so the entrance output can be made ahead.
	Registry R4bProducts()
	{
		std::vector<ProductDecl> d = FirstProducts().All();
		d[kP0RebaseDecision].backend = Backend::Cpu;
		return Registry(std::move(d));
	}

	ConsumerDecl Consumer(uint16_t id, ProductId p)
	{
		ConsumerDecl c;
		c.id = id;
		AddConsume(c, p);
		return c;
	}

	Graph Graph4k(bool ofa = true, uint32_t w = 3840, uint32_t h = 2160)
	{
		GraphSettings gs;
		gs.dims.width = w;
		gs.dims.height = h;
		gs.ofaPresent = ofa;
		return BuildGraph(R4bProducts(),
			{ Consumer(1, kP6EntranceAhead), Consumer(2, kP4SourceStats), Consumer(3, kP5MonitorTiles) }, gs);
	}

	uint64_t LiveVram(const Registry& reg, const Graph& g)
	{
		uint64_t v = 0;
		for (const Node& n : g.nodes)
			if (ResidencyFor(reg.Get(n.id), n.bufferBytes) == Residency::Vram)
				v += n.liveBytes;
		return v;
	}

	TEST_CLASS(CardProfileTests)
	{
	public:
		TEST_METHOD(LaptopGrantsEveryLaneAt4k)
		{
			const Registry reg = R4bProducts();
			const Graph g = Graph4k();
			const TierPlan p = PlanTiers(reg, g, Laptop(), TierInput());
			Assert::AreEqual(g.nodes.size(), p.entries.size());
			for (const TierEntry& e : p.entries) {
				if (e.backend == Backend::RenderOnly)
					Assert::IsTrue(e.why == GrantWhy::RenderOnly);
				else
					Assert::IsTrue(e.grant == Grant::Lane, L"every lane product fits 2.5 GB free");
			}
			Assert::IsFalse(p.t0OverBudget);
			Assert::IsTrue(p.vramBytes <= p.budgetBytes);
			Assert::AreEqual(uint64_t(double(2500 * kMB) * kHeadroomStart), p.budgetBytes);
		}

		TEST_METHOD(EntranceAheadWaitsForALaneRebaseDecision)
		{
			// Today (R4a) the re-base decision is made in the render, so its same-frame reader
			// cannot be laned whatever the memory.
			const Registry r4a = FirstProducts();
			GraphSettings gs;
			gs.dims.width = 3840;
			gs.dims.height = 2160;
			const Graph g = BuildGraph(r4a, { Consumer(1, kP6EntranceAhead) }, gs);
			const TierPlan p = PlanTiers(r4a, g, Rtx3090(), TierInput());
			Assert::IsTrue(p.Find(kP0RebaseDecision)->why == GrantWhy::RenderOnly);
			Assert::IsTrue(p.Find(kP6EntranceAhead)->why == GrantWhy::InputInRender);
			Assert::IsTrue(p.Laned(kP3AnalysisField));
		}

		TEST_METHOD(T0IsGrantedFirstThenT1ByMsPerMb)
		{
			const TierPlan p = PlanTiers(R4bProducts(), Graph4k(), Laptop(), TierInput());
			size_t firstT1 = p.entries.size();
			for (size_t i = 0; i < p.entries.size(); ++i)
				if (p.entries[i].tier != Tier::T0) {
					firstT1 = i;
					break;
				}
			for (size_t i = firstT1; i < p.entries.size(); ++i)
				Assert::IsTrue(p.entries[i].tier != Tier::T0, L"no T0 after the first T1");
			// Monitor tiles (small, cheap per MB) rank above the 4K entrance output.
			Assert::AreEqual(ProductId(kP5MonitorTiles), p.entries[firstT1].id);
			Assert::AreEqual(ProductId(kP6EntranceAhead), p.entries[firstT1 + 1].id);
			Assert::IsTrue(p.entries[firstT1].msPerMB > p.entries[firstT1 + 1].msPerMB);
		}

		TEST_METHOD(SmallCardShedsTheEntranceOutputFirst)
		{
			const Registry reg = R4bProducts();
			const Graph g = Graph4k();
			CardProfile small = Laptop();
			small.vramBudgetBytes = 1000 * kMB;
			small.vramUsageBytes = 700 * kMB;   // 300 MB free, 240 MB budget
			const TierPlan p = PlanTiers(reg, g, small, TierInput());
			Assert::IsTrue(p.Laned(kP1HalfLuma));
			Assert::IsTrue(p.Laned(kP2VectorPair));
			Assert::IsTrue(p.Laned(kP3AnalysisField));
			Assert::IsTrue(p.Laned(kP5MonitorTiles));
			Assert::IsFalse(p.Laned(kP6EntranceAhead));
			Assert::IsTrue(p.Find(kP6EntranceAhead)->why == GrantWhy::OverBudget);
			Assert::IsFalse(p.t0OverBudget);
			Assert::AreNotEqual(std::string::npos, p.notice.find("entrance output ahead computed in the render"),
				L"the cutback is visible");
			// The 3090 keeps it.
			Assert::IsTrue(PlanTiers(reg, g, Rtx3090(), TierInput()).Laned(kP6EntranceAhead));
		}

		TEST_METHOD(T0OverBudgetIsVisibleAndItsReadersFollowIntoTheRender)
		{
			const Registry reg = R4bProducts();
			const Graph g = Graph4k();
			const Node* p1 = g.Find(kP1HalfLuma);
			const uint64_t extraP1 = p1->ringBytes;
			CardProfile card = Laptop();
			TierInput in;
			in.headroom = kHeadroomMin;   // budget = free / 2 exactly
			const uint64_t budget = LiveVram(reg, g) + extraP1 - 1;
			card.vramUsageBytes = 0;
			card.vramBudgetBytes = budget * 2;
			const TierPlan p = PlanTiers(reg, g, card, in);
			Assert::AreEqual(budget, p.budgetBytes);
			// One shed order: T1 first, then T0 readers before their inputs (P3, P2, then P1).
			Assert::IsTrue(p.Find(kP1HalfLuma)->why == GrantWhy::OverBudget);
			Assert::IsTrue(p.Find(kP2VectorPair)->why == GrantWhy::OverBudget);
			Assert::IsTrue(p.Find(kP3AnalysisField)->why == GrantWhy::OverBudget);
			Assert::IsTrue(p.Laned(kP0RebaseDecision), L"the smallest T0 input keeps its lane");
			Assert::IsTrue(p.t0OverBudget);
			Assert::IsFalse(p.notice.empty());
			Assert::IsTrue(p.vramBytes <= p.budgetBytes);
		}

		TEST_METHOD(LivePoolsOverBudgetAreVisible)
		{
			CardProfile tiny = Laptop();
			tiny.vramBudgetBytes = 600 * kMB;
			tiny.vramUsageBytes = 500 * kMB;
			const TierPlan p = PlanTiers(R4bProducts(), Graph4k(), tiny, TierInput());
			Assert::IsTrue(p.liveOverBudget);
			Assert::AreNotEqual(std::string::npos, p.notice.find("not enough free video memory"));
		}

		TEST_METHOD(OwnUsageDoesNotShrinkTheGrantBetweenRebuilds)
		{
			const Registry reg = FirstProducts();
			CardProfile small = Laptop();
			small.vramBudgetBytes = 1000 * kMB;
			small.vramUsageBytes = 700 * kMB;
			const TierPlan first = PlanTiers(R4bProducts(), Graph4k(), small, TierInput());
			// The granted memory is now allocated and shows up in CurrentUsage as VP's own.
			CardProfile running = small;
			running.vramUsageBytes += first.vramBytes;
			TierInput in;
			in.ownUsageBytes = first.vramBytes;
			const TierPlan second = PlanTiers(R4bProducts(), Graph4k(), running, in);
			Assert::AreEqual(first.budgetBytes, second.budgetBytes);
			for (const TierEntry& e : first.entries)
				Assert::IsTrue(e.grant == second.Find(e.id)->grant, L"no flapping");
			(void)reg;
		}

		TEST_METHOD(LaneGrantsReachTheSlots)
		{
			const Registry reg = R4bProducts();
			const Graph g = Graph4k();
			CardProfile small = Laptop();
			small.vramBudgetBytes = 1000 * kMB;
			small.vramUsageBytes = 700 * kMB;
			const TierPlan p = PlanTiers(reg, g, small, TierInput());
			SlotTable table(g);
			ApplyLaneGrants(p, table);
			Assert::IsFalse(table.Find(kP6EntranceAhead)->LaneGranted());
			Assert::IsTrue(table.Find(kP3AnalysisField)->LaneGranted());
		}

		TEST_METHOD(SystemRamRules)
		{
			const Registry reg = R4bProducts();
			// Small (< 1 MB) CPU stats: system RAM.
			Assert::IsTrue(ResidencyFor(reg.Get(kP4SourceStats), 8192) == Residency::SystemRam);
			// A full-resolution per-frame plane never goes to system RAM, even if preferred.
			ProductDecl p6 = reg.Get(kP6EntranceAhead);
			p6.residency = Residency::SystemRam;
			Assert::IsFalse(SystemRamAllowed(p6, 100));
			Assert::IsTrue(ResidencyFor(p6, 100) == Residency::Vram);
			// A 2 MB compute product: only when rarely touched.
			ProductDecl p3 = reg.Get(kP3AnalysisField);
			p3.residency = Residency::SystemRam;
			Assert::IsFalse(SystemRamAllowed(p3, 2 * kMB));
			p3.rarelyTouched = true;
			Assert::IsTrue(SystemRamAllowed(p3, 2 * kMB));
			// A CPU-made item may be larger (not full resolution).
			Assert::IsTrue(SystemRamAllowed(reg.Get(kP1HalfLuma), 5 * kMB));
			// System RAM items do not count against VRAM; their upload is charged.
			const TierPlan p = PlanTiers(reg, Graph4k(), Laptop(), TierInput());
			const TierEntry* p4 = p.Find(kP4SourceStats);
			Assert::IsTrue(p4->residency == Residency::SystemRam);
			Assert::IsTrue(p.systemRamBytes > 0);
			const double estimate = 0.4 * (3840.0 * 2160.0) / (1920.0 * 1080.0);
			Assert::IsTrue(p4->msSaved < estimate, L"upload charged");
		}

		TEST_METHOD(OpticalFlowLoadAndPresence)
		{
			const Registry reg = R4bProducts();
			TierInput in;
			in.framePeriodMs = 1000.0 / 24.0;
			// 4K on the laptop engine: 11 ms x 4 = 44 ms > 41.7 ms.
			const TierPlan slow = PlanTiers(reg, Graph4k(), Laptop(), in);
			Assert::IsTrue(slow.ofaUsed);
			Assert::IsFalse(slow.ofaFits);
			Assert::IsTrue(slow.ofaLoad > 1.0);
			Assert::AreNotEqual(std::string::npos, slow.notice.find("Vector engine"));
			// 1080p fits.
			const TierPlan hd = PlanTiers(reg, Graph4k(true, 1920, 1080), Laptop(), in);
			Assert::IsTrue(hd.ofaFits);
			Assert::IsTrue(hd.notice.empty());
			// No engine: the matcher pair is the motion product and is granted.
			CardProfile noEngine = Laptop();
			noEngine.ofaPresent = false;
			const TierPlan m = PlanTiers(reg, Graph4k(false), noEngine, in);
			Assert::IsFalse(m.ofaUsed);
			Assert::IsTrue(m.Laned(kP2mMatcherPair));
			// A graph built for an engine the card lacks is flagged.
			const TierPlan wrong = PlanTiers(reg, Graph4k(true, 1920, 1080), noEngine, in);
			Assert::IsFalse(wrong.ofaFits);
		}

		TEST_METHOD(MeasuredCostsReplaceEstimates)
		{
			const Registry reg = R4bProducts();
			TierInput in;
			MeasuredCost none;
			none.id = kP6EntranceAhead;
			none.renderMs = 0.0;
			in.measured.push_back(none);
			const TierPlan p = PlanTiers(reg, Graph4k(), Rtx3090(), in);
			Assert::IsTrue(p.Find(kP6EntranceAhead)->why == GrantWhy::NoGain, L"no saving, no lane memory");
			// The compute scale divides the estimate.
			const double laptop = RenderMsOf(reg.Get(kP3AnalysisField), Graph4k(), Laptop(), {});
			const double big = RenderMsOf(reg.Get(kP3AnalysisField), Graph4k(), Rtx3090(), {});
			Assert::AreEqual(laptop / 4.0, big, 1e-9);
		}

		TEST_METHOD(ProfileCacheIsKeyedByAdapterAndDriver)
		{
			CardProfileCache cache;
			cache.Store(Laptop());
			cache.Store(Rtx3090());
			Assert::AreEqual(size_t(2), cache.Size());
			AdapterKey k = Laptop().key;
			Assert::IsNotNull(cache.Find(k));
			k.driverVersion = 101;
			Assert::IsNull(cache.Find(k), L"a driver update measures again");
			CardProfile live;
			Assert::IsTrue(cache.WithLiveValues(Laptop().key, 4000 * kMB, 100 * kMB, 1, &live));
			Assert::AreEqual(3900 * kMB, live.FreeVram());
			Assert::AreEqual(11.0, live.ofaMsPerPairRef);
			CardProfile updated = Laptop();
			updated.ofaMsPerPairRef = 12.0;
			cache.Store(updated);
			Assert::AreEqual(size_t(2), cache.Size());
			Assert::AreEqual(12.0, cache.Find(Laptop().key)->ofaMsPerPairRef);
			Assert::AreNotEqual(Laptop().Identity(), Rtx3090().Identity());
		}

		TEST_METHOD(HeadroomShrinksFastAndGrowsAfterADwell)
		{
			Headroom h;
			Assert::AreEqual(kHeadroomStart, h.Value());
			h.OnPressure();
			Assert::AreEqual(kHeadroomStart * kHeadroomShrink, h.Value(), 1e-12);
			const double after = h.Value();
			for (int i = 0; i < kHeadroomDwellWindows - 1; ++i)
				h.OnCalmWindow();
			Assert::AreEqual(after, h.Value(), L"no growth before the dwell");
			h.OnCalmWindow();
			Assert::AreEqual(after + kHeadroomGrowStep, h.Value(), 1e-12);
			for (int i = 0; i < 20; ++i)
				h.OnPressure();
			Assert::AreEqual(kHeadroomMin, h.Value());
			for (int i = 0; i < 1000; ++i)
				h.OnCalmWindow();
			Assert::AreEqual(kHeadroomMax, h.Value(), 1e-12);
		}

		TEST_METHOD(SameInputsGiveTheSamePlan)
		{
			const Registry reg = R4bProducts();
			const Graph g = Graph4k();
			CardProfile small = Laptop();
			small.vramUsageBytes = 3200 * kMB;
			const TierPlan a = PlanTiers(reg, g, small, TierInput());
			const TierPlan b = PlanTiers(reg, g, small, TierInput());
			Assert::AreEqual(a.entries.size(), b.entries.size());
			for (size_t i = 0; i < a.entries.size(); ++i) {
				Assert::AreEqual(a.entries[i].id, b.entries[i].id);
				Assert::IsTrue(a.entries[i].grant == b.entries[i].grant);
				Assert::IsTrue(a.entries[i].why == b.entries[i].why);
			}
			Assert::AreEqual(a.vramBytes, b.vramBytes);
			Assert::AreEqual(a.notice, b.notice);
		}
	};
}
